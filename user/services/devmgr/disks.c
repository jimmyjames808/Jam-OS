/* devmgr: the disks its usb-storage drivers serve, told to volumes
 * (<volumes.h>, the DISKS channel). Since M12's storage split devmgr keeps
 * the drivers and nothing of storage: no disk is devmgr's to look at, no
 * filesystem, no mount.
 *
 * For each running disk's driver devmgr asks, without waiting, for a
 * `storage` channel of volumes' own (svc.connect on the driver's DR_SERVE
 * channel: drivers/usb-storage answers it), and hands it to volumes
 * (DISKS_ATTACH, with usb-bus's device id) when the answer comes. That is
 * the one thing devmgr reads on a disk driver's channel; nobody else ever
 * gets it (GET_SERVICE refuses a disk's driver). When the device is gone
 * for good (the binding is retired) volumes is told (DISKS_GONE); a driver
 * that merely ended is no news (volumes sees its `storage` channel close,
 * and the next start is announced again with the same id).
 *
 * The DISKS channel's client end comes from init (DEVMGR_SET_VOLUMES):
 * once both devmgr and volumes run, and again whenever either starts
 * again. A new one is told every disk again; until there is one, a disk
 * waits (nothing is asked of its driver). */
#include <idl/svc.h>
#include <volumes.h>
#include "internal.h"

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

/* m to volumes, with h (HANDLE_INVALID: none), which goes either way. */
static void tell(const struct disks_msg *m, handle_t h)
{
    if (!vol) {
        if (h)
            jam_handle_close(h);
        return;
    }
    status_t st = jam_channel_write(vol, m, sizeof(*m), h ? &h : NULL, h ? 1 : 0);
    if (st == ERR_PEER_CLOSED)
        drop_vol();   /* volumes ended: init sends the next one's */
    else if (st != OK)
        say(false, "devmgr: can't tell volumes of a disk (%s)", status_str(st));
}

void disks_started(struct binding *b)
{
    b->disk_txid = 0;
    if (!b->disk || !b->client || !vol)
        return;
    connects = (connects + 1) & ~CHANNEL_TXID_KERNEL;
    if (connects == 0)
        connects++;
    status_t st = svc_connect_send(b->client, connects);
    if (st != OK) {
        say(false, "devmgr: %s %s: can't ask it for a channel for volumes (%s)", bdf(b), b->path,
            status_str(st));
        return;
    }
    b->disk_txid = connects;
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
        bool ours = b->disk_txid && n >= sizeof(struct idl_rep_hdr) && r->txid == b->disk_txid;
        if (ours && n == sizeof(*r) && r->status == OK && nh == 1) {
            b->disk_txid = 0;
            struct disks_msg m = { .ordinal = DISKS_ATTACH, .id = b->usb_id,
                                   .bus = DISKS_BUS_USB };
            tell(&m, hs[0]);
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
}

void disks_gone(struct binding *b)
{
    if (!b->disk)
        return;
    b->disk_txid = 0;
    struct disks_msg m = { .ordinal = DISKS_GONE, .id = b->usb_id };
    tell(&m, HANDLE_INVALID);
}

void disks_sync(void)
{
    if (!vol)
        return;
    struct disks_msg m = { .ordinal = DISKS_SYNC };
    struct volumes_rep r;
    uint32_t n = 0;
    struct channel_call_args a = {
        .h = vol, .wr_len = sizeof(m), .wr_bytes = (uint64_t)(uintptr_t)&m, .rd_cap = sizeof(r),
        .rd_bytes = (uint64_t)(uintptr_t)&r, .rd_actual = (uint64_t)(uintptr_t)&n,
        .deadline_ns = now() + DISKS_SYNC_WAIT,
    };
    status_t st = jam_channel_call(&a);
    if (st == OK && n >= VOLUMES_REP_HDR)
        st = r.status;
    if (st == ERR_PEER_CLOSED)
        drop_vol();
    else if (st != OK)
        say(false, "devmgr: volumes didn't sync its mounts (%s)", status_str(st));
}

void disks_set_volumes(handle_t ch)
{
    drop_vol();
    vol = ch;
    unsigned n = 0;
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (b->kind == BIND_USB && b->disk && b->proc && b->state == DEVMGR_SUP_RUNNING) {
            disks_started(b);
            n++;
        }
    }
    say(false, "devmgr: a disks channel to volumes; %u disk(s) to tell it of", n);
}
