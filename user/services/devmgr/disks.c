/* devmgr: the disks its drivers serve, told to volumes on the DISKS
 * channel (abi/idl/volumes.idl's `disks` events): a stick's (usb-storage,
 * on a USB interface: bus BUS_USB, id usb-bus's device id) and an internal
 * drive's (drv/nvme, on a PCI function: bus BUS_NVME, id 0x40000000 |
 * bus << 8 | device << 3 | function, a range of its own that never has
 * the top bit test disks use). devmgr keeps the drivers and nothing of
 * storage: no disk is devmgr's to look at, no filesystem, no mount.
 *
 * For each running disk's driver devmgr asks, without waiting, for a
 * `storage` channel of volumes' own (svc.connect on the driver's DR_SERVE
 * channel: drivers/usb-storage answers it), and hands it to volumes
 * (disks.attach, with the disk's id) when the answer comes. That is
 * the one thing devmgr reads on a disk driver's channel; nobody else ever
 * gets it (GET_SERVICE refuses a disk's driver). A test holding the
 * control channel may ask for a `storage` channel of its own the same way
 * (devmgrctl.disk_storage: no more than set_volumes gives it), one at a
 * time per disk, answered when the driver answers. A PCI disk is never
 * "gone": its function stays. When the device is gone
 * for good (the binding is retired) volumes is told (disks.gone); a driver
 * that merely ended is no news (volumes sees its `storage` channel close,
 * and the next start is announced again with the same id).
 *
 * The DISKS channel's client end comes from init (devmgrctl.set_volumes):
 * once both devmgr and volumes run, and again whenever either starts
 * again. A new one is told every disk again; until there is one, a disk
 * waits (nothing is asked of its driver). */
#include <idl/svc.h>
#include <idl/volumes.h>
#include "internal.h"

#define PCI_DISK_IDS 0x40000000u   /* a PCI disk's id: this | its bus, device, function */

static handle_t vol;          /* our end of the DISKS channel (0: none) */
/* The svc.connect txids: bit 31 clear, as <jam/abi.h> CHANNEL_TXID_KERNEL
 * asks of a writer that picks its own. */
static uint32_t connects = 0x515c0000u;

/* The DISKS channel is dead: dropped until init sends a new one. */
static void drop_vol(void)
{
    if (vol)
        jam_handle_close(vol);
    vol = HANDLE_INVALID;
}

/* What sending an event to volumes did. */
static void told(status_t st)
{
    if (st == ERR_PEER_CLOSED)
        drop_vol();   /* volumes ended: init sends the next one's */
    else if (st != OK)
        say(false, "devmgr: can't tell volumes of a disk (%s)", status_str(st));
}

bool disks_pci_driver(const char *path)
{
    return path && !strcmp(path, "drv/nvme");
}

/* The disk's id and bus, for volumes. */
static uint32_t disk_id(const struct binding *b)
{
    if (b->kind == BIND_USB)
        return b->usb_id;
    return PCI_DISK_IDS | (uint32_t)b->info.bus << 8 | (uint32_t)b->info.dev << 3 | b->info.fn;
}

static uint8_t disk_bus(const struct binding *b)
{
    return b->kind == BIND_USB ? DISKS_BUS_USB : DISKS_BUS_NVME;
}

/* The next svc.connect txid. */
static uint32_t next_txid(void)
{
    connects = (connects + 1) & ~CHANNEL_TXID_KERNEL;
    if (connects == 0)
        connects++;
    return connects;
}

void disks_started(struct binding *b)
{
    b->disk_txid = 0;
    if (!b->disk || !b->client || !vol)
        return;
    uint32_t txid = next_txid();
    status_t st = svc_connect_send(b->client, txid);
    if (st != OK) {
        say(false, "devmgr: %s %s: can't ask it for a channel for volumes (%s)", bdf(b), b->path,
            status_str(st));
        return;
    }
    b->disk_txid = txid;
}

/* The disk_storage asked for b is answered (st, and the channel if OK:
 * consumed either way). */
static void test_answer(struct binding *b, status_t st, handle_t ch)
{
    if (!b->test_txid) {
        if (ch)
            jam_handle_close(ch);
        return;
    }
    (void)devmgrctl_reply_disk_storage(b->test_txn, st, st == OK ? ch : HANDLE_INVALID);
    if (st != OK && ch)
        jam_handle_close(ch);
    jam_handle_close(b->test_txn.ch);   /* our duplicate */
    b->test_txid = 0;
    b->test_txn = (struct idl_txn){ HANDLE_INVALID, 0 };
}

status_t disks_test_storage(struct binding *b, struct idl_txn txn)
{
    if (!b->disk)
        return ERR_NOT_SUPPORTED;
    if (!b->proc || !b->client || b->state != DEVMGR_SUP_RUNNING || b->test_txid)
        return ERR_BAD_STATE;
    /* The request's channel may close before the answer: a duplicate of
     * it stays ours, so the answer never goes to a handle reused since. */
    handle_t dup;
    status_t st = jam_handle_duplicate(txn.ch, RIGHT_SAME, &dup);
    if (st != OK)
        return st;
    uint32_t txid = next_txid();
    if ((st = svc_connect_send(b->client, txid)) != OK) {
        jam_handle_close(dup);
        return st;
    }
    b->test_txid = txid;
    b->test_txn = (struct idl_txn){ dup, txn.txid };
    return IDL_LATER;
}

void disks_driver_ended(struct binding *b)
{
    test_answer(b, ERR_PEER_CLOSED, HANDLE_INVALID);
}

void disks_events(struct binding *b)
{
    for (int guard = 0; guard < 64 && b->client; guard++) {
        _Alignas(8) uint8_t buf[64];
        handle_t hs[4];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = b->client, .rd_cap = sizeof(buf), .rd_bytes = (uint64_t)(uintptr_t)buf,
            .rd_actual = (uint64_t)(uintptr_t)&n, .rd_handles = (uint64_t)(uintptr_t)hs,
            .rd_hcap = 4, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(b->client, n, nh);   /* not ours */
            continue;
        }
        if (st != OK)
            return;
        const struct svc_connect_rep *r = (const struct svc_connect_rep *)buf;
        bool whole = n >= sizeof(struct idl_rep_hdr);
        if (b->test_txid && whole && r->txid == b->test_txid) {
            bool ok = n == sizeof(*r) && r->status == OK && nh == 1;
            for (uint32_t i = ok ? 1 : 0; i < nh; i++)
                jam_handle_close(hs[i]);
            test_answer(b, ok ? OK : r->status < 0 ? r->status : ERR_INTERNAL,
                        ok ? hs[0] : HANDLE_INVALID);
            continue;
        }
        bool ours = b->disk_txid && whole && r->txid == b->disk_txid;
        if (ours && n == sizeof(*r) && r->status == OK && nh == 1) {
            b->disk_txid = 0;
            if (vol)
                told(disks_attach(vol, disk_id(b), disk_bus(b), DISKS_UNIT_NONE, hs[0]));   /* hs[0] goes */
            else
                jam_handle_close(hs[0]);
            continue;
        }
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
        if (ours) {
            b->disk_txid = 0;
            say(false, "devmgr: %s %s: no channel for volumes (%s)", bdf(b), b->path,
                status_str(n >= sizeof(struct idl_rep_hdr) ? r->status : ERR_INTERNAL));
        }
    }
    /* 64 read and maybe more queued (a driver that writes as fast as we
     * read): the binding fires when the channel turns readable, not while
     * it stays so, and without another turn the answer volumes' channel
     * comes in would never be read. */
    if (b->client && b->client_key &&
        port_again(port, b->client, b->client_key, SIG_READABLE) != OK) {
        say(false, "devmgr: %s: can't watch its channel for events", bdf(b));
        b->client_key = 0;
    }
}

void disks_device_gone(struct binding *b)
{
    if (!b->disk)
        return;
    b->disk_txid = 0;
    if (vol)
        told(disks_gone(vol, disk_id(b)));
}

void disks_set_volumes(handle_t ch)
{
    drop_vol();
    vol = ch;
    unsigned n = 0;
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (b->disk && b->proc && b->state == DEVMGR_SUP_RUNNING) {
            disks_started(b);
            n++;
        }
    }
    say(false, "devmgr: a disks channel to volumes; %u disk(s) to tell it of", n);
}
