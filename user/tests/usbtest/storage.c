/* usbtest: the mass-storage checks: usb-bus's bulk transfers, and
 * drv/usb-storage through the `storage` and `block` protocols.
 *
 * usbtest drives a disk itself: it opens the interface (usbbus.
 * open_interface), talks Bulk-Only Transport by hand on that channel, then
 * starts a usb-storage of its own on the SAME channel, holding the
 * driver's DR_SERVE. devmgr's own usb-storage has the disk first: usbtest
 * asks devmgr to let go of it for the checks (take_disk below) and to take
 * the boot stick back afterwards, and waits (give_back) until volumes has
 * its mounts back, so nothing started after usbtest misses /data. (If a
 * usb-storage still has the interface's bulk pair, usb.open_bulk says
 * ERR_BAD_STATE and the checks are skipped.)
 *
 * On the first mass-storage interface that isn't the second disk (in QEMU
 * and on the PC: the boot stick):
 *   storage_bulk   usb.open_bulk gives a 64 KiB buffer, a second channel
 *                  is refused, bad transfers are refused, and after reset
 *                  recovery (what the driver before may have left the
 *                  device needing: a real stick failed its new owner's first
 *                  CBW) an INQUIRY by hand (CBW out, data in, CSW in) works
 *   storage_handover  the device is left in the middle of a READ, the pair
 *                  is opened again on the same channel (a restarted
 *                  driver's): QEMU's stick refuses the next CBW, and after
 *                  reset recovery the first command works
 *   storage_stall  an unknown SCSI command ends "failed" (QEMU: with
 *                  sense ILLEGAL REQUEST), whether the device STALLs its
 *                  data phase (real sticks) or pads it (QEMU); a CBW with a bad
 *                  signature is refused (QEMU: a STALL on the OUT pipe),
 *                  and after reset recovery the next command works
 *   storage_bind   the device is left in the middle of a READ, as a driver
 *                  that crashed would leave it; usb-storage, started on
 *                  it, recovers and reports the disk and its partitions
 *   storage_esp    partition 1 (type EF) opened read-only: block 0 has
 *                  the 55 AA signature and "FAT32"; a write is refused
 *                  (ERR_ACCESS_DENIED) and changes nothing
 *   storage_range  requests outside the partition or the buffer are
 *                  refused (ERR_OUT_OF_RANGE), also ones that would wrap
 *   storage_write  the FAT32 data partition opened read-write: on a QEMU
 *                  disk its last 64 KiB are overwritten with a pattern,
 *                  read back and restored; on a real stick one block is
 *                  written back unchanged (nothing on it is altered)
 *   block_in_flight  two READs sent without waiting, each at its own
 *                  offset of a buffer asked bigger than buffer_max (so
 *                  capped there): both answered right, by txid; a second
 *                  map_buffer is refused
 *   storage_fence  a client queues WRITEs and leaves (its channel closes)
 *                  while usb-storage is busy with another channel's READs:
 *                  the WRITEs are dropped, never performed (QEMU disks
 *                  only: a failure would write a pattern, put back after)
 *   storage_stop   DR_SERVE closed: the driver exits 0 and its block
 *                  channels close
 * and with tools/storage-test.sh's two more disks, behind the hub (so at
 * full speed), found by their serials:
 *   storage_disk2  "jamos-disk2": the same read and write checks
 *   storage_apart  while a READ waits on the slow disk below, the second
 *                  disk reads 64 KiB at its own pace: usb-bus runs each
 *                  device's bulk transfers apart from the others'
 *   storage_kill  a usb-storage killed with a READ in flight (the disk's
 *                  throttle keeps it going for a moment), a new one started
 *                  on the same disk: it takes the device over and works
 *   storage_unplug device_del in the middle of reads: the read fails
 *                  (it doesn't hang), the block channel closes, the
 *                  driver exits 0
 *   storage_timeout "jamos-slow", too slow for a READ's 5 s: the READ
 *                  fails ERR_TIMED_OUT (it doesn't hang); then a later
 *                  one works, or after three in a row the driver gives
 *                  up (exit 3)
 *
 * A block request's deadline here is BLK_WAIT: a command's own timeouts
 * (5 s, 10 s for a write) plus its reset recovery, twice. */
#include <idl/block.h>
#include <idl/devmgr.h>
#include <idl/storage.h>
#include <idl/usb.h>
#include <idl/usbbus.h>
#include <idl/volumes.h>
#include "usbtest.h"

#define BLK_WAIT   (60 * NS_PER_S)
#define BUF_SIZE   65536u      /* a block channel's buffer (block.idl) */
#define BULK_BUF   (68u << 10) /* usb-bus's bulk buffer (usb.open_bulk): 64 KiB and a page */
#define STEP_MS    5000u       /* a bulk transfer by hand */
#define RAW_CBW    0xf000u     /* where the hand-made CBW and CSW sit in the bulk buffer */
#define RAW_CSW    0xf200u
#define CBW_SIG    0x43425355u
#define DISK2      "jamos-disk2"   /* the second disk's serial */
#define SLOW       "jamos-slow"    /* the slow disk's */

/* A mass-storage interface driven by hand. */
struct raw {
    handle_t ch;               /* its `usb` channel (usbbus.open_interface) */
    uint32_t dev_id;           /* usb-bus's device id */
    uint8_t ifnum;             /* interface number */
    uint8_t ep_in, ep_out;     /* bulk endpoint addresses */
    uint8_t *buf;              /* the bulk buffer, mapped; NULL: not open */
    bool qemu;                 /* INQUIRY vendor "QEMU" */
    uint32_t tag;              /* the last CBW's tag */
};

/* A usb-storage usbtest started. */
struct drive {
    handle_t proc;             /* the driver's process */
    handle_t storage;          /* the client end of its DR_SERVE */
    bool qemu;                 /* INQUIRY vendor "QEMU": a disk image we may overwrite */
    uint32_t bs;               /* block size */
    uint64_t blocks;           /* blocks on the disk */
    uint8_t nparts;            /* entries in parts[] */
    struct {
        uint8_t type;          /* MBR type */
        uint64_t start, blocks;
    } parts[4];
};

/* An open block channel with its buffer mapped. */
struct pch {
    handle_t ch;               /* the block channel */
    uint8_t *buf;              /* its 64 KiB buffer */
    uint64_t blocks;           /* blocks in the partition */
};

static struct raw boot_raw;
static struct drive boot_drive, disk2, slow;
static uint8_t save[BUF_SIZE];   /* what a write check overwrote */

/* A bulk transfer's own deadline: STEP_MS from now, or ms. */
static uint64_t step_end(void)
{
    return in((uint64_t)STEP_MS * NS_PER_MS);
}

static uint64_t usb_wait(void)
{
    return in(20 * NS_PER_S);   /* a transfer's timeout plus usb-bus stopping the endpoint */
}

/* The `usb` channel to drive disk r through. devmgr binds a usb-storage of
 * its own to every disk (and mounts the boot stick's partitions through
 * it), and usb-bus gives an interface's bulk endpoints to one channel
 * only: devmgr's. So volumes is asked to stop the disk's mounts in order
 * (volumes.release_disk: its files closed, the volume clean), then devmgr
 * to let go of the disk (devmgrctl.release_usb: its driver stops) and hand out a
 * duplicate of that channel. Where devmgr has no driver on the disk, a new
 * channel from usb-bus does. */
static status_t take_disk(const struct raw *r, handle_t *out)
{
    handle_t vm = svc_get(SVC_VOLUMES);
    if (!vm)   /* usbtest's JAM_WANTS asks for it: without it the disk is let go of under its mounts */
        printf("usbtest: storage: no volumes service: the disk's mounts can't be stopped first\n");
    status_t st = vm ? volumes_release_disk_until(vm, in(60 * NS_PER_S), r->dev_id)
                     : ERR_NOT_FOUND;
    if (st != OK && st != ERR_NOT_FOUND)   /* not found: volumes doesn't have it */
        printf("usbtest: storage: volumes didn't release disk %u (%s)\n", r->dev_id,
               status_str(st));
    if (dm && devmgrctl_release_usb_until(dm, in(60 * NS_PER_S), r->dev_id, r->ifnum, out) == OK)
        return OK;
    return usbbus_open_interface_until(bus, soon(), r->dev_id, r->ifnum, out);
}

/* The mounts volumes has (a watch on it, volumes.watch): usbtest notes them
 * before it takes the stick and waits for them to come back when it gives
 * the stick back. The remount happens after devmgr's rebind has answered,
 * some milliseconds later, and a program started in that gap (init's next
 * one, the shell's) gets a namespace without /data for good: so usbtest
 * does not end before its stick's mounts are back. The events reach init's
 * watch before they reach this one (init's was made first, and volumes
 * tells the watches in the order they were made), so when usbtest has seen
 * them, init has them waiting. */
#define MOUNT_MAX  8
#define BACK_WAIT  (10 * NS_PER_S)

struct mountset {
    handle_t watch;                    /* 0: none (no volumes) */
    bool     settled;                  /* `settled` came since it was cleared */
    unsigned n;
    char     path[MOUNT_MAX][16];      /* the mounts there are, test disks' not counted */
};

static struct mountset before;         /* the mounts before the stick was let go of */
static struct mountset now_have;       /* the mounts as the watch last said */

static int ms_find(const struct mountset *s, const char *path, uint32_t len)
{
    for (unsigned i = 0; i < s->n; i++)
        if (strlen(s->path[i]) == len && !memcmp(s->path[i], path, len))
            return (int)i;
    return -1;
}

static void ms_mounted(void *ctx, const char *p, uint32_t len, uint32_t generation, uint8_t flags,
                       handle_t fs)
{
    struct mountset *s = ctx;
    (void)generation;
    jam_handle_close(fs);
    if ((flags & MOUNTS_TEST) || len >= sizeof(s->path[0]) || s->n >= MOUNT_MAX ||
        ms_find(s, p, len) >= 0)
        return;
    memcpy(s->path[s->n], p, len);
    s->path[s->n++][len] = '\0';
}

static void ms_unmounted(void *ctx, const char *p, uint32_t len, uint32_t generation)
{
    struct mountset *s = ctx;
    (void)generation;
    int i = ms_find(s, p, len);
    if (i < 0)
        return;
    memmove(s->path[i], s->path[i + 1], (size_t)(s->n - (unsigned)i - 1) * sizeof(s->path[0]));
    s->n--;
}

static void ms_settled(void *ctx, uint32_t generation)
{
    struct mountset *s = ctx;
    (void)generation;
    s->settled = true;
}

static const struct mounts_events ms_events = {
    .mounted = ms_mounted, .unmounted = ms_unmounted, .settled = ms_settled,
};

static void ms_close(struct mountset *s)
{
    if (s->watch)
        jam_handle_close(s->watch);
    *s = (struct mountset){ 0 };
}

/* Note the mounts there are now (a new watch lists them all, then `settled`). */
static void mounts_note(void)
{
    handle_t vm = svc_get(SVC_VOLUMES);
    ms_close(&before);
    ms_close(&now_have);
    if (!vm || volumes_watch_until(vm, in(10 * NS_PER_S), &now_have.watch) != OK) {
        now_have.watch = HANDLE_INVALID;
        return;
    }
    uint64_t deadline = in(10 * NS_PER_S);
    status_t st = OK;
    while (st == OK && !now_have.settled) {
        st = mounts_read_event(now_have.watch, &ms_events, &now_have);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen = 0;
            st = jam_object_wait_one(now_have.watch, SIG_READABLE | SIG_PEER_CLOSED, deadline,
                                     &seen);
        }
        if (st == ERR_INTERNAL)
            st = OK;
    }
    if (st != OK) {
        ms_close(&now_have);
        return;
    }
    before = now_have;
    before.watch = HANDLE_INVALID;   /* the watch is now_have's */
}

/* Are all the mounts noted there? */
static bool mounts_all_back(void)
{
    for (unsigned i = 0; i < before.n; i++)
        if (ms_find(&now_have, before.path[i], (uint32_t)strlen(before.path[i])) < 0)
            return false;
    return true;
}

/* Wait until every mount noted is there again (BACK_WAIT at most). What the
 * watch has queued since is read first: the stick's release is in it (its
 * mounts left), so the set read before it is not the one to compare. */
static void mounts_wait_back(void)
{
    if (!now_have.watch)
        return;
    uint64_t t0 = now(), deadline = t0 + BACK_WAIT;
    bool back = false;
    for (;;) {
        status_t st = OK;
        while (st == OK || st == ERR_INTERNAL)
            st = mounts_read_event(now_have.watch, &ms_events, &now_have);
        back = mounts_all_back();
        if (back || st != ERR_SHOULD_WAIT)
            break;
        signals_t seen = 0;
        if (jam_object_wait_one(now_have.watch, SIG_READABLE | SIG_PEER_CLOSED, deadline,
                                &seen) != OK)
            break;
    }
    if (back)
        printf("usbtest: storage: the stick's mounts are back after %lu ms\n",
               (unsigned long)((now() - t0) / NS_PER_MS));
    else
        printf("usbtest: storage: the stick's mounts did not come back in %lu s\n",
               (unsigned long)(BACK_WAIT / NS_PER_S));
    ms_close(&now_have);
    ms_close(&before);
}

/* devmgr takes disk r back: a new driver of its own, and for the boot
 * stick its mounts again, which are waited for. */
static void give_back(const struct raw *r)
{
    status_t st = dm ? devmgrctl_rebind_until(dm, in(60 * NS_PER_S), DEVMGR_KIND_USB_IFACE,
                                              r->dev_id, r->ifnum, 0)
                     : ERR_NOT_FOUND;
    if (st != OK && st != ERR_NOT_FOUND)
        printf("usbtest: storage: devmgr did not take the disk back (%s)\n", status_str(st));
    if (st == OK)
        mounts_wait_back();
    else
        ms_close(&now_have);
}

/* ---- finding the disks ------------------------------------------------------------ */

/* The first Bulk-Only SCSI interface of the disk we want: the one with
 * that serial, or (NULL) one that isn't a test scenario's extra disk. */
static bool find_disk(const char *serial, struct raw *r)
{
    for (unsigned i = 0; i < ndevs; i++) {
        const struct dev *d = &devs[i];
        bool extra = !strcmp(d->serial, DISK2) || !strcmp(d->serial, SLOW);
        if (!d->config || d->hub_ports || (serial ? strcmp(d->serial, serial) != 0 : extra))
            continue;
        for (uint8_t k = 0; k < d->nifs; k++) {
            uint8_t num, alt, nalts, cls, sub, proto, eps[8];
            uint32_t nep;
            if (usbbus_interface_until(bus, soon(), d->id, k, &num, &alt, &nalts, &cls, &sub,
                                       &proto, eps, &nep) != OK)
                continue;
            if (cls != 0x08 || sub != 0x06 || proto != 0x50)
                continue;
            *r = (struct raw){ .dev_id = d->id, .ifnum = num };
            for (uint32_t e = 0; e < nep && e < 8; e++) {
                if ((eps[e] & 0x80) && !r->ep_in)
                    r->ep_in = eps[e];
                else if (!(eps[e] & 0x80) && !r->ep_out)
                    r->ep_out = eps[e];
            }
            printf("usbtest: mass storage %04x:%04x at %s (speed %u), if%u, bulk in %02x out "
                   "%02x\n", d->vid, d->pid, d->path, d->speed, num, r->ep_in, r->ep_out);
            return true;
        }
    }
    return false;
}

static void raw_unmap(struct raw *r)
{
    if (r->buf)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)r->buf, BULK_BUF);
    r->buf = NULL;
}

static void raw_close(struct raw *r)
{
    raw_unmap(r);
    if (r->ch)
        jam_handle_close(r->ch);
    r->ch = HANDLE_INVALID;
}

/* usb.open_bulk on r's channel, and the buffer it gives mapped at r->buf.
 * ERR_BAD_STATE: another driver has the pair. */
static status_t raw_open_bulk(struct raw *r)
{
    handle_t vmo = HANDLE_INVALID;
    uint32_t size = 0;
    status_t st = usb_open_bulk_until(r->ch, soon(), r->ep_in, r->ep_out, &vmo, &size);
    if (st != OK)
        return st;
    uint64_t va = 0;
    st = size == BULK_BUF ? jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size,
                                         VMAR_READ | VMAR_WRITE, &va)
                          : ERR_INTERNAL;
    jam_handle_close(vmo);
    if (st == OK)
        r->buf = (uint8_t *)(uintptr_t)va;
    return st;
}

/* ---- Bulk-Only Transport by hand ---------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Send a CBW (signature sig) for cdb with a data phase of len bytes. */
static status_t raw_cbw(struct raw *r, uint32_t sig, const uint8_t *cdb, uint8_t n, bool in_dir,
                        uint32_t len)
{
    uint8_t *w = r->buf + RAW_CBW;
    memset(w, 0, 31);
    put32(w, sig);
    put32(w + 4, ++r->tag);
    put32(w + 8, len);
    w[12] = in_dir ? 0x80 : 0;
    w[14] = n;
    memcpy(w + 15, cdb, n);
    uint32_t actual = 0;
    status_t st = usb_bulk_out_until(r->ch, usb_wait(), RAW_CBW, 31, step_end(), &actual);
    return st == OK && actual != 31 ? ERR_IO : st;
}

/* Read the CSW (a STALL: clear it, once more); *status: its status byte. */
static status_t raw_csw(struct raw *r, uint8_t *status)
{
    uint32_t actual = 0;
    status_t st = usb_bulk_in_until(r->ch, usb_wait(), RAW_CSW, 13, step_end(), &actual);
    if (st == ERR_IO) {
        st = usb_clear_halt_until(r->ch, usb_wait(), r->ep_in);
        if (st == OK)
            st = usb_bulk_in_until(r->ch, usb_wait(), RAW_CSW, 13, step_end(), &actual);
    }
    if (st != OK)
        return st;
    const uint8_t *s = r->buf + RAW_CSW;
    if (actual != 13 || get32(s) != 0x53425355u || get32(s + 4) != r->tag)
        return ERR_INTERNAL;
    *status = s[12];
    return OK;
}

/* A whole command with its data (in, to the buffer's start). *stalled:
 * the data phase ended with a STALL, which was cleared. */
static status_t raw_cmd(struct raw *r, const uint8_t *cdb, uint8_t n, uint32_t len,
                        uint32_t *moved, uint8_t *status, bool *stalled)
{
    *moved = 0;
    *stalled = false;
    status_t st = raw_cbw(r, CBW_SIG, cdb, n, true, len);
    if (st != OK)
        return st;
    if (len) {
        st = usb_bulk_in_until(r->ch, usb_wait(), 0, len, step_end(), moved);
        if (st == ERR_IO) {
            *stalled = true;
            st = usb_clear_halt_until(r->ch, usb_wait(), r->ep_in);
        }
        if (st != OK)
            return st;
    }
    return raw_csw(r, status);
}

/* Reset recovery (Bulk-Only Transport 5.3.4), as drv/usb-storage does it: the class request
 * Bulk-Only Mass Storage Reset, then CLEAR_FEATURE(ENDPOINT_HALT) on bulk IN and bulk OUT
 * (usb-bus also puts its own side of each endpoint back). The device is ready for a CBW
 * after it, whatever the driver before it left (a command half done, a halt, a toggle out
 * of step). The first step that failed, or OK. */
static status_t raw_recover(struct raw *r)
{
    status_t reset = usb_control_out_until(r->ch, soon(), 0x21, 0xff, 0, r->ifnum, NULL, 0);
    status_t in_ep = usb_clear_halt_until(r->ch, usb_wait(), r->ep_in);
    status_t out_ep = usb_clear_halt_until(r->ch, usb_wait(), r->ep_out);
    if (reset != OK || in_ep != OK || out_ep != OK)
        printf("usbtest: reset recovery: reset %s, clear halt in %s, out %s\n", status_str(reset),
               status_str(in_ep), status_str(out_ep));
    return reset != OK ? reset : in_ep != OK ? in_ep : out_ep;
}

/* ---- storage_bulk, storage_stall ----------------------------------------------------- */

static bool t_storage_bulk(void)
{
    struct raw *r = &boot_raw;
    CHECK_ST(take_disk(r, &r->ch), OK);
    handle_t vmo = HANDLE_INVALID;
    uint32_t size = 0;
    status_t st = raw_open_bulk(r);
    if (st == ERR_BAD_STATE) {
        printf("usbtest: storage: another driver has the disk's bulk endpoints: skipped\n");
        skipped++;
        raw_close(r);
        return true;
    }
    CHECK_ST(st, OK);
    /* the pair belongs to this channel: another one can't open or use it */
    handle_t other;
    CHECK_ST(usbbus_open_interface_until(bus, soon(), r->dev_id, r->ifnum, &other), OK);
    uint32_t n = 0;
    status_t a = usb_open_bulk_until(other, soon(), r->ep_in, r->ep_out, &vmo, &size);
    status_t b = usb_bulk_in_until(other, soon(), 0, 13, in(100 * NS_PER_MS), &n);
    jam_handle_close(other);
    CHECK_ST(a, ERR_BAD_STATE);
    CHECK_ST(b, ERR_BAD_STATE);
    /* transfers outside the buffer, of nothing, or with a bad deadline: one
     * already past is over before it begins (ERR_TIMED_OUT, nothing sent),
     * one more than 60 s away, or none, is refused */
    uint64_t soon_end = in(100 * NS_PER_MS);
    CHECK_ST(usb_bulk_in_until(r->ch, soon(), BULK_BUF - 12, 13, soon_end, &n), ERR_OUT_OF_RANGE);
    CHECK_ST(usb_bulk_in_until(r->ch, soon(), 0xffffffffu, 13, soon_end, &n), ERR_OUT_OF_RANGE);
    CHECK_ST(usb_bulk_out_until(r->ch, soon(), 0, BULK_BUF + 1, soon_end, &n), ERR_OUT_OF_RANGE);
    CHECK_ST(usb_bulk_in_until(r->ch, soon(), 0, 0, soon_end, &n), ERR_INVALID_ARGS);
    CHECK_ST(usb_bulk_in_until(r->ch, soon(), 0, 13, 0, &n), ERR_TIMED_OUT);
    CHECK_ST(usb_bulk_in_until(r->ch, soon(), 0, 13, now() - 1, &n), ERR_TIMED_OUT);
    CHECK_ST(usb_bulk_in_until(r->ch, soon(), 0, 13, in(61 * NS_PER_S), &n), ERR_INVALID_ARGS);
    CHECK_ST(usb_bulk_out_until(r->ch, soon(), 0, 13, DEADLINE_NEVER, &n), ERR_INVALID_ARGS);
    CHECK_ST(usb_clear_halt_until(r->ch, soon(), 0x0f), ERR_INVALID_ARGS);
    CHECK_ST(usb_open_bulk_until(r->ch, soon(), r->ep_out, r->ep_in, &vmo, &size),
             ERR_INVALID_ARGS);
    /* INQUIRY, the first command of a new owner: the driver before may have left the device
     * in the middle of a command or out of sequence (the PC's SanDisk answered its new
     * owner's first CBW with a USB Transaction Error, and every one after it), so reset
     * recovery comes first, as in usb-storage. A first command that still fails gets one more
     * recovery and one more try, as scsi.c's does, and the line says so. */
    const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36 };
    uint32_t moved = 0;
    uint8_t status = 0xff;
    bool stalled = false;
    unsigned recoveries = 1;
    CHECK_ST(raw_recover(r), OK);
    st = raw_cmd(r, inquiry, 6, 36, &moved, &status, &stalled);
    if (st != OK) {
        printf("usbtest: the first command after reset recovery failed (%s): once more\n",
               status_str(st));
        recoveries++;
        CHECK_ST(raw_recover(r), OK);
        st = raw_cmd(r, inquiry, 6, 36, &moved, &status, &stalled);
    }
    CHECK_ST(st, OK);
    CHECK(moved == 36 && status == 0 && !stalled);
    printf("usbtest: the first command after the hand-over worked after %u reset recover%s\n",
           recoveries, recoveries == 1 ? "y" : "ies");
    r->qemu = !memcmp(r->buf + 8, "QEMU    ", 8);
    printf("usbtest: INQUIRY by hand: \"%.8s\" \"%.16s\"\n", (const char *)r->buf + 8,
           (const char *)r->buf + 16);
    return true;
}

static bool t_storage_stall(void)
{
    struct raw *r = &boot_raw;
    CHECK(r->buf != NULL);
    uint32_t moved = 0;
    uint8_t status = 0xff;
    bool stalled = false;
    /* a vendor-specific opcode nobody implements, asking for 512 bytes */
    const uint8_t unknown[6] = { 0xf7 };
    CHECK_ST(raw_cmd(r, unknown, 6, 512, &moved, &status, &stalled), OK);
    CHECK(status == 1);
    bool data_stalled = stalled;
    const uint8_t sense[6] = { 0x03, 0, 0, 0, 18 };
    CHECK_ST(raw_cmd(r, sense, 6, 18, &moved, &status, &stalled), OK);
    CHECK(status == 0 && moved >= 14);
    uint8_t key = r->buf[2] & 0xf, asc = r->buf[12];
    printf("usbtest: unknown command: %s, CSW failed, sense %x/%02x\n",
           data_stalled ? "data phase STALLed, halt cleared" : "data phase padded", key, asc);
    if (r->qemu)
        CHECK(key == 5 && asc == 0x20);   /* ILLEGAL REQUEST, invalid command opcode */
    /* a CBW that isn't one: the device refuses it and wants reset recovery */
    status_t bad = raw_cbw(r, 0x12345678, sense, 6, true, 18);
    CHECK(bad == ERR_IO || bad == OK);
    CHECK_ST(raw_recover(r), OK);
    const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36 };
    CHECK_ST(raw_cmd(r, inquiry, 6, 36, &moved, &status, &stalled), OK);
    CHECK(moved == 36 && status == 0);
    printf("usbtest: bad CBW: %s; reset recovery, then INQUIRY works\n",
           bad == ERR_IO ? "STALL" : "taken");
    return true;
}

/* A new owner on a pair the last one left in the middle of a command. A READ(10) is sent
 * and half of its data left unread (as a driver that crashed leaves it), then usb.open_bulk is
 * asked again on the same channel: what devmgr's restart of a class driver does, and usb-bus
 * gives the new owner a fresh pair (the controller's side started afresh, the device's
 * halts and toggles cleared). The device's command state is not usb-bus's to touch. QEMU's
 * stick, still in its data phase, refuses the next CBW with a STALL, the failure the PC's
 * SanDisk showed (there as a USB Transaction Error, from its out-of-step sequence numbers,
 * which QEMU does not model); a real stick may take it or not. Whichever, reset recovery
 * before the first command gets the device back. */
static bool t_storage_handover(void)
{
    struct raw *r = &boot_raw;
    CHECK(r->buf != NULL);
    const uint8_t read16[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 16, 0 };
    uint32_t moved = 0;
    CHECK_ST(raw_cbw(r, CBW_SIG, read16, 10, true, 16 * 512), OK);
    CHECK_ST(usb_bulk_in_until(r->ch, usb_wait(), 0, 4096, step_end(), &moved), OK);
    CHECK(moved == 4096);
    raw_unmap(r);
    CHECK_ST(raw_open_bulk(r), OK);
    const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36 };
    uint8_t status = 0xff;
    bool stalled = false;
    status_t first = raw_cmd(r, inquiry, 6, 36, &moved, &status, &stalled);
    if (r->qemu)
        CHECK(first != OK);   /* the device wants reset recovery: no recovery, no command */
    CHECK_ST(raw_recover(r), OK);
    CHECK_ST(raw_cmd(r, inquiry, 6, 36, &moved, &status, &stalled), OK);
    CHECK(moved == 36 && status == 0 && !stalled);
    printf("usbtest: a pair opened again on a device left mid-READ: the first command without "
           "recovery %s; after reset recovery INQUIRY works\n",
           first == OK ? "was taken" : status_str(first));
    return true;
}

/* ---- a usb-storage of our own ---------------------------------------------------------- */

static void drive_stop(struct drive *v)
{
    if (v->storage)
        jam_handle_close(v->storage);
    v->storage = HANDLE_INVALID;
    if (v->proc) {
        if (spawn_wait(v->proc, 10 * NS_PER_S, NULL) != OK)
            printf("usbtest: %s: a usb-storage didn't exit in 10 s\n", cur);
        jam_handle_close(v->proc);
    }
    v->proc = HANDLE_INVALID;
}

/* Start drv/usb-storage on usb (moved), and read what it found. */
static bool drive_start(struct drive *v, handle_t usb, const char *name)
{
    handle_t ours, theirs;
    *v = (struct drive){ 0 };
    status_t st = jam_channel_create(&ours, &theirs);
    if (st != OK) {
        jam_handle_close(usb);
        FAIL("channel_create: %s", status_str(st));
    }
    const struct spawn_handle x[2] = { { SR_DRIVER(DR_USB), usb },
                                       { SR_DRIVER(DR_SERVE), theirs } };
    const char *argv[] = { name };
    struct spawn_args a = {
        .path = "drv/usb-storage", .name = name, .argc = 1, .argv = argv,
        .job = startup_handle(SR_JOB), .extra = x, .nextra = 2,
    };
    st = spawn(&a, &v->proc);
    v->storage = ours;
    CHECK_ST(st, OK);
    char vendor[9], product[17];
    uint32_t vn = 0, pn = 0, nparts = 0, mbr_id = 0;
    uint8_t scheme = 0xff, guid[16], zero[16] = { 0 };
    CHECK_ST(storage_info_until(ours, in(BLK_WAIT), vendor, &vn, product, &pn, &v->bs, &v->blocks,
                                &scheme, &nparts, &mbr_id, guid), OK);
    v->qemu = !strcmp(vendor, "QEMU");
    v->nparts = (uint8_t)nparts;
    CHECK(v->bs == 512 && v->blocks > 0 && nparts <= 4);
    /* INQUIRY's text trimmed: no trailing space; MBR only, no GPT GUID */
    CHECK(vn == strlen(vendor) && (!vn || vendor[vn - 1] != ' '));
    CHECK(pn == strlen(product) && (!pn || product[pn - 1] != ' '));
    CHECK(scheme == STORAGE_SCHEME_MBR || scheme == STORAGE_SCHEME_NONE);
    CHECK(!memcmp(guid, zero, 16));
    printf("usbtest: %s: \"%s\" \"%s\" %lu x %u, scheme %u, disk id %08x, %u partition(s)\n",
           name, vendor, product, (unsigned long)v->blocks, v->bs, scheme, mbr_id, v->nparts);
    for (uint8_t i = 0; i < v->nparts; i++) {
        uint8_t tg[16], ug[16];
        uint64_t attrs = 1;
        char pname[109];
        uint32_t pname_len = 1;
        CHECK_ST(storage_partition_until(ours, soon(), i, &v->parts[i].type, tg, ug,
                                         &v->parts[i].start, &v->parts[i].blocks, &attrs, pname,
                                         &pname_len), OK);
        printf("usbtest: %s: partition %u: type %02x, %lu + %lu\n", name, i + 1,
               v->parts[i].type, (unsigned long)v->parts[i].start,
               (unsigned long)v->parts[i].blocks);
        CHECK(v->parts[i].start > 0 && v->parts[i].blocks > 0);
        CHECK(v->parts[i].start + v->parts[i].blocks <= v->blocks);
        CHECK(!memcmp(tg, zero, 16) && !memcmp(ug, zero, 16) && !attrs && !pname_len);
    }
    return true;
}

static void part_close(struct pch *p)
{
    if (p->buf)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)p->buf, BUF_SIZE);
    if (p->ch)
        jam_handle_close(p->ch);
    *p = (struct pch){ 0 };
}

/* Open partition `index` of v with its buffer mapped. */
static bool part_open(const struct drive *v, uint8_t index, bool ro, struct pch *p)
{
    *p = (struct pch){ 0 };
    CHECK(v->storage != HANDLE_INVALID);
    CHECK_ST(storage_open_partition_until(v->storage, soon(), index, ro, &p->ch), OK);
    uint32_t bs = 0, size = 0, max_blocks = 0, buffer_max = 0;
    uint16_t depth = 0;
    uint8_t is_ro = 2;
    handle_t vmo;
    CHECK_ST(block_info_until(p->ch, soon(), &bs, &p->blocks, &is_ro, &max_blocks, &depth,
                              &buffer_max), OK);
    CHECK(bs == v->bs && p->blocks == v->parts[index].blocks && is_ro == ro);
    /* usb-storage: one request at a time, buffers up to 256 KiB */
    CHECK(depth == 1 && buffer_max >= BUF_SIZE && max_blocks == buffer_max / bs);
    /* no buffer yet: nothing to read into */
    CHECK_ST(block_read_until(p->ch, soon(), 0, 1, 0), ERR_BAD_STATE);
    CHECK_ST(block_map_buffer_until(p->ch, soon(), 0, &vmo, &size), OK);   /* its choice */
    uint64_t va = 0;
    /* usb-storage maps the buffer itself, so it can't be shrunk or
     * decommitted from here; a read-only channel's can't be written
     * either. */
    if (ro)
        CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, BUF_SIZE,
                              VMAR_READ | VMAR_WRITE, &va), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_set_size(vmo, 4096), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_decommit(vmo, 0, 4096), ERR_ACCESS_DENIED);
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, BUF_SIZE,
                               ro ? VMAR_READ : VMAR_READ | VMAR_WRITE, &va);
    jam_handle_close(vmo);
    CHECK(size == BUF_SIZE);
    CHECK_ST(st, OK);
    p->buf = (uint8_t *)(uintptr_t)va;
    CHECK_ST(block_map_buffer_until(p->ch, soon(), 0, &vmo, &size), ERR_BAD_STATE);
    return true;
}

static status_t rd(const struct pch *p, uint64_t lba, uint32_t count, uint32_t offset)
{
    return block_read_until(p->ch, in(BLK_WAIT), lba, count, offset);
}

static status_t wr(const struct pch *p, uint64_t lba, uint32_t count, uint32_t offset)
{
    return block_write_until(p->ch, in(BLK_WAIT), lba, count, offset);
}

static int part_of_type(const struct drive *v, uint8_t type, uint8_t type2)
{
    for (uint8_t i = 0; i < v->nparts; i++)
        if (v->parts[i].type == type || v->parts[i].type == type2)
            return i;
    return -1;
}

/* ---- the checks on a drive --------------------------------------------------------------- */

/* The EFI System Partition, read-only: its boot sector is FAT32's, and
 * nothing can be written. */
static bool check_esp(const struct drive *v, struct pch *p, bool formatted)
{
    int e = part_of_type(v, 0xef, 0xef);
    if (e != 0) {
        printf("usbtest: %s: partition 1 is not an EFI System Partition: skipped\n", cur);
        skipped++;
        return true;
    }
    if (!part_open(v, 0, true, p))
        return false;
    CHECK_ST(rd(p, 0, 1, 0), OK);
    CHECK(p->buf[510] == 0x55 && p->buf[511] == 0xaa);
    if (formatted) {
        CHECK(!memcmp(p->buf + 82, "FAT32   ", 8));
        printf("usbtest: %s: the ESP's boot sector: FAT32, label \"%.11s\"\n", cur,
               (const char *)p->buf + 71);
    }
    memcpy(save, p->buf, 512);
    /* 64 KiB in one request (more than one SCSI command), the same block first */
    CHECK_ST(rd(p, 0, BUF_SIZE / v->bs, 0), OK);
    CHECK(!memcmp(save, p->buf, 512));
    /* read-only: the write is refused and nothing changed (the buffer
     * can't even be written here: it is mapped read-only) */
    CHECK_ST(wr(p, 0, 1, 0), ERR_ACCESS_DENIED);
    CHECK_ST(wr(p, p->blocks, 1, 0), ERR_ACCESS_DENIED);
    CHECK_ST(rd(p, 0, 1, 512), OK);
    CHECK(!memcmp(save, p->buf + 512, 512));
    CHECK_ST(block_sync_until(p->ch, in(BLK_WAIT)), OK);
    return true;
}

static bool check_range(const struct drive *v, struct pch *p)
{
    CHECK(v->nparts >= 1);
    if (!part_open(v, 0, true, p))
        return false;
    uint64_t n = p->blocks;
    uint32_t per = BUF_SIZE / v->bs;
    CHECK_ST(rd(p, n, 1, 0), ERR_OUT_OF_RANGE);               /* the block after the last */
    CHECK_ST(rd(p, n - 1, 2, 0), ERR_OUT_OF_RANGE);           /* runs over the end */
    CHECK_ST(rd(p, UINT64_MAX, 2, 0), ERR_OUT_OF_RANGE);      /* lba + count wraps */
    CHECK_ST(rd(p, UINT64_MAX - n + 1, 1, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(rd(p, 0, per + 1, 0), ERR_OUT_OF_RANGE);         /* more than the buffer */
    CHECK_ST(rd(p, 0, 0xffffffffu, 0), ERR_OUT_OF_RANGE);     /* count * block size wraps */
    CHECK_ST(rd(p, 0, 0x800000u, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(rd(p, 0, 1, BUF_SIZE - v->bs + 1), ERR_OUT_OF_RANGE);   /* past the buffer's end */
    CHECK_ST(rd(p, 0, 1, 0xffffffffu), ERR_OUT_OF_RANGE);
    CHECK_ST(rd(p, 0, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(rd(p, n - 1, 1, BUF_SIZE - v->bs), OK);          /* the last block, the last slot */
    uint8_t type;
    uint64_t start, blocks;
    handle_t h;
    CHECK_ST(storage_partition_until(v->storage, soon(), v->nparts, &type, NULL, NULL, &start,
                                     &blocks, NULL, NULL, NULL), ERR_OUT_OF_RANGE);
    CHECK_ST(storage_partition_until(v->storage, soon(), 0xffffffffu, &type, NULL, NULL, &start,
                                     &blocks, NULL, NULL, NULL), ERR_OUT_OF_RANGE);
    CHECK_ST(storage_open_partition_until(v->storage, soon(), v->nparts, 0, &h),
             ERR_OUT_OF_RANGE);
    CHECK_ST(storage_open_partition_until(v->storage, soon(), 0x100, 0, &h), ERR_OUT_OF_RANGE);
    CHECK_ST(storage_open_partition_until(v->storage, soon(), 0, 2, &h), ERR_INVALID_ARGS);
    return true;
}

/* The pattern a write check writes: different in every block. */
static void pattern(uint8_t *b, uint32_t seed)
{
    for (uint32_t i = 0; i < BUF_SIZE; i++)
        b[i] = (uint8_t)(i * 7 + (i >> 9) * 13 + seed);
}

/* The data partition, read-write. A QEMU image: its last 64 KiB are
 * overwritten, read back and restored. A real stick: one block is written
 * back as it was. */
static bool check_write(const struct drive *v, struct pch *p, struct pch *ro)
{
    int e = part_of_type(v, 0x0c, 0x0b);
    if (e < 0 || v->parts[e].blocks < BUF_SIZE / v->bs) {
        printf("usbtest: %s: no FAT32 data partition: skipped\n", cur);
        skipped++;
        return true;
    }
    if (!part_open(v, (uint8_t)e, false, p) || !part_open(v, (uint8_t)e, true, ro))
        return false;
    uint32_t per = BUF_SIZE / v->bs;
    uint64_t lba = p->blocks - per;
    CHECK_ST(wr(p, p->blocks, 1, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(wr(p, p->blocks - 1, 2, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(wr(ro, lba, 1, 0), ERR_ACCESS_DENIED);   /* the same partition, opened read-only */
    if (!v->qemu) {
        CHECK_ST(rd(p, p->blocks - 1, 1, 0), OK);
        memcpy(save, p->buf, v->bs);
        CHECK_ST(wr(p, p->blocks - 1, 1, 0), OK);
        CHECK_ST(rd(p, p->blocks - 1, 1, v->bs), OK);
        CHECK(!memcmp(save, p->buf + v->bs, v->bs));
        CHECK_ST(block_sync_until(p->ch, in(BLK_WAIT)), OK);
        printf("usbtest: %s: a real stick: block %lu written back unchanged\n", cur,
               (unsigned long)(p->blocks - 1));
        return true;
    }
    CHECK_ST(rd(p, lba, per, 0), OK);
    memcpy(save, p->buf, BUF_SIZE);
    pattern(p->buf, 0x5a);
    CHECK_ST(wr(p, lba, per, 0), OK);
    CHECK_ST(block_sync_until(p->ch, in(BLK_WAIT)), OK);
    memset(p->buf, 0, BUF_SIZE);
    CHECK_ST(rd(ro, lba, per, 0), OK);   /* through the other channel: it is on the disk */
    pattern(p->buf, 0x5a);
    bool same = !memcmp(p->buf, ro->buf, BUF_SIZE);
    memcpy(p->buf, save, BUF_SIZE);
    CHECK_ST(wr(p, lba, per, 0), OK);    /* put back what was there */
    CHECK_ST(rd(ro, lba, per, 0), OK);
    CHECK(same);
    CHECK(!memcmp(save, ro->buf, BUF_SIZE));
    printf("usbtest: %s: 64 KiB at block %lu of partition %d written, read back, restored\n",
           cur, (unsigned long)lba, e + 1);
    return true;
}

/* ---- the boot disk -------------------------------------------------------------------------- */

/* Leave the device in the middle of a READ(10), half its data unread, and
 * start usb-storage on it. */
static bool t_storage_bind(void)
{
    struct raw *r = &boot_raw;
    CHECK(r->buf != NULL);
    const uint8_t read16[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 16, 0 };
    uint32_t moved = 0;
    CHECK_ST(raw_cbw(r, CBW_SIG, read16, 10, true, 16 * 512), OK);
    CHECK_ST(usb_bulk_in_until(r->ch, usb_wait(), 0, 4096, step_end(), &moved), OK);
    CHECK(moved == 4096);
    handle_t usb = r->ch;
    r->ch = HANDLE_INVALID;
    raw_close(r);
    uint64_t t0 = now();
    if (!drive_start(&boot_drive, usb, "usb-storage-test"))
        return false;
    printf("usbtest: usb-storage took over a disk left mid-READ in %lu ms\n",
           (unsigned long)((now() - t0) / NS_PER_MS));
    return true;
}

static bool t_storage_esp(void)
{
    struct pch p = { 0 };
    bool ok = check_esp(&boot_drive, &p, true);
    part_close(&p);
    return ok;
}

static bool t_storage_range(void)
{
    struct pch p = { 0 };
    bool ok = check_range(&boot_drive, &p);
    part_close(&p);
    return ok;
}

static bool t_storage_write(void)
{
    struct pch p = { 0 }, ro = { 0 };
    bool ok = check_write(&boot_drive, &p, &ro);
    part_close(&p);
    part_close(&ro);
    return ok;
}

/* ---- block_in_flight ------------------------------------------------------------------------ */

/* The answer to a request sent with txid one of want[0..1] on ch: its
 * status into got[i]. */
static status_t take_answer(handle_t ch, const uint32_t want[2], status_t got[2], bool seen[2])
{
    _Alignas(8) uint8_t buf[BLOCK_REP_MAX];
    struct idl_msg m;
    signals_t sig = 0;
    status_t st = jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, in(BLK_WAIT), &sig);
    if (st == OK)
        st = idl_reply_read(ch, buf, sizeof(buf), &m);
    if (st != OK)
        return st;
    for (unsigned i = 0; i < 2; i++)
        if (m.txid == want[i] && !seen[i]) {
            seen[i] = true;
            got[i] = block_read_result(buf, &m);
            return OK;
        }
    idl_msg_drop(&m);
    return ERR_INTERNAL;   /* a txid we never sent */
}

/* Two READs in flight on one channel, each at its own offset in a buffer
 * bigger than the default: both answered (in either order) with the right
 * blocks. A buffer asked for past buffer_max is capped there; a second
 * map_buffer is refused. */
static bool t_block_in_flight(void)
{
    struct drive *v = &boot_drive;
    handle_t ch, vmo;
    uint32_t bs = 0, max_blocks = 0, buffer_max = 0, size = 0;
    uint16_t depth = 0;
    uint64_t blocks = 0, va = 0;
    uint8_t ro = 0;
    CHECK(v->storage != HANDLE_INVALID && v->nparts >= 1);
    CHECK_ST(storage_open_partition_until(v->storage, soon(), 0, 1, &ch), OK);
    CHECK_ST(block_info_until(ch, soon(), &bs, &blocks, &ro, &max_blocks, &depth, &buffer_max), OK);
    CHECK(buffer_max >= 2 * BUF_SIZE && blocks >= 2 * BUF_SIZE / bs);
    CHECK_ST(block_map_buffer_until(ch, soon(), 1u << 20, &vmo, &size), OK);
    CHECK_EQ(size, buffer_max);   /* 1 MiB asked: capped */
    handle_t again;
    CHECK_ST(block_map_buffer_until(ch, soon(), 0, &again, &size), ERR_BAD_STATE);
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, buffer_max, VMAR_READ, &va);
    jam_handle_close(vmo);
    CHECK_ST(st, OK);
    const uint8_t *buf = (const uint8_t *)(uintptr_t)va;
    uint32_t per = BUF_SIZE / bs;
    bool ok = true;
    /* one at a time first: blocks [0, per) at 0, [per, 2 per) at BUF_SIZE */
    if (block_read_until(ch, in(BLK_WAIT), 0, per, 0) != OK ||
        block_read_until(ch, in(BLK_WAIT), per, per, BUF_SIZE) != OK)
        ok = false;
    static uint8_t first[2 * BUF_SIZE];
    memcpy(first, buf, sizeof(first));
    /* then both in flight, swapped: [per, 2 per) at 2 BUF_SIZE, [0, per) at 3 BUF_SIZE */
    const uint32_t txid[2] = { 0x1f11, 0x1f12 };
    status_t got[2] = { ERR_INTERNAL, ERR_INTERNAL };
    bool seen[2] = { false, false };
    if (ok && (block_read_send(ch, txid[0], per, per, 2 * BUF_SIZE) != OK ||
               block_read_send(ch, txid[1], 0, per, 3 * BUF_SIZE) != OK))
        ok = false;
    for (unsigned i = 0; ok && i < 2; i++)
        ok = take_answer(ch, txid, got, seen) == OK;
    bool same = ok && !memcmp(buf + 2 * BUF_SIZE, first + BUF_SIZE, BUF_SIZE) &&
                !memcmp(buf + 3 * BUF_SIZE, first, BUF_SIZE);
    jam_vmar_unmap(startup_handle(SR_SELF_VMAR), va, buffer_max);
    jam_handle_close(ch);
    CHECK(ok);
    CHECK_ST(got[0], OK);
    CHECK_ST(got[1], OK);
    CHECK(same);
    printf("usbtest: %s: two READs of 64 KiB in flight at offsets 128 and 192 KiB of a %u KiB "
           "buffer (queue depth %u): both right\n", cur, buffer_max >> 10, depth);
    return true;
}

/* ---- storage_fence ------------------------------------------------------------------------- */

#define FENCE_READS  32   /* READs of 64 KiB that keep usb-storage busy */
#define FENCE_WRITES 4    /* WRITEs the client that leaves has queued */
#define FENCE_TRIES  5    /* tries before "never kept busy long enough" fails the check */

/* A READ or WRITE of count blocks at lba, from `offset` in p's buffer,
 * sent without waiting for its answer. */
static status_t rw_send(const struct pch *p, uint32_t ordinal, uint32_t txid, uint64_t lba,
                        uint32_t count, uint32_t offset)
{
    struct block_write_req q = { .txid = txid, .ordinal = ordinal, .lba = lba, .count = count,
                                 .offset = offset };
    return jam_channel_write(p->ch, &q, sizeof(q), NULL, 0);
}

static status_t read_answer(const struct pch *p, uint64_t deadline);

/* The race itself. usb-storage's loop serves a channel's queued requests
 * one after another and looks at its port (where another channel's new
 * requests are announced) only when every channel it knows to be pending
 * is empty. So once busy's first READ is answered, it reads nothing of
 * dead's until all of busy's READs are done: dead's WRITEs are queued and
 * dead's end closed (what a client's death does to its channels) inside
 * that time. *valid: busy still had answers to come after the close, so
 * the WRITEs were still queued, unread, when the client left. */
static bool fence_race(const struct drive *v, const struct pch *busy, struct pch *dead,
                       uint64_t lba, bool *valid)
{
    uint32_t per = BUF_SIZE / v->bs, n = per / FENCE_WRITES;
    pattern(dead->buf, 0xa5);
    for (uint32_t i = 0; i < FENCE_READS; i++)
        CHECK_ST(rw_send(busy, BLOCK_READ, i + 1, 0, per, 0), OK);
    CHECK_ST(read_answer(busy, in(BLK_WAIT)), OK);
    for (uint32_t i = 0; i < FENCE_WRITES; i++)
        CHECK_ST(rw_send(dead, BLOCK_WRITE, i + 1, lba + i * n, n, i * n * v->bs), OK);
    jam_handle_close(dead->ch);
    dead->ch = HANDLE_INVALID;
    unsigned got = 1;
    while (got < FENCE_READS && read_answer(busy, 0) == OK)
        got++;
    *valid = got < FENCE_READS;
    unsigned at_close = got;
    for (; got < FENCE_READS; got++)
        CHECK_ST(read_answer(busy, in(BLK_WAIT)), OK);
    printf("usbtest: %s: %u WRITEs queued and their client gone with %u of %u READs still to "
           "answer\n", cur, FENCE_WRITES, FENCE_READS - at_close, FENCE_READS);
    return true;
}

/* One try, on fresh channels: busy reads the ESP, dead is the data
 * partition read-write. */
static bool fence_try(const struct drive *v, int e, uint64_t lba, bool *valid)
{
    struct pch busy = { 0 }, dead = { 0 };
    bool ok = part_open(v, 0, true, &busy) && part_open(v, (uint8_t)e, false, &dead) &&
              fence_race(v, &busy, &dead, lba, valid);
    part_close(&busy);
    part_close(&dead);
    return ok;
}

/* The tries, and what is on the disk after each, read through rw: the
 * same 64 KiB as before, or (the WRITEs landed) put back. */
static bool fence(const struct drive *v, int e, const struct pch *rw)
{
    uint32_t per = BUF_SIZE / v->bs;
    uint64_t lba = rw->blocks - per;
    CHECK_ST(rd(rw, lba, per, 0), OK);
    memcpy(save, rw->buf, BUF_SIZE);
    bool valid = false, landed = false;
    unsigned tries = 0;
    while (!valid && tries < FENCE_TRIES) {
        tries++;
        if (!fence_try(v, e, lba, &valid))
            return false;
        CHECK_ST(rd(rw, lba, per, 0), OK);
        landed = memcmp(save, rw->buf, BUF_SIZE) != 0;
        if (landed) {
            memcpy(rw->buf, save, BUF_SIZE);
            CHECK_ST(wr(rw, lba, per, 0), OK);
        }
    }
    if (valid)
        printf("usbtest: %s: the WRITEs of a client that left %s (try %u)\n", cur,
               landed ? "LANDED on the disk" : "never landed", tries);
    if (!valid)
        FAIL("usb-storage was never kept busy until the client had left (%u tries)", tries);
    CHECK(!landed);
    return true;
}

/* A client queues WRITEs and leaves before usb-storage has read them: they
 * must never be performed, or a dying fat's last write could land after
 * its successor's writes to the same blocks. Needs a disk image (QEMU):
 * if the check fails, its pattern is written to the data partition (and
 * put back after). */
static bool t_storage_fence(void)
{
    const struct drive *v = &boot_drive;
    int e = part_of_type(v, 0x0c, 0x0b);
    if (!v->qemu || e < 0 || v->parts[e].blocks < BUF_SIZE / v->bs) {
        printf("usbtest: %s: no QEMU disk with a FAT32 data partition: skipped\n", cur);
        skipped++;
        return true;
    }
    struct pch rw = { 0 };
    bool ok = part_open(v, (uint8_t)e, false, &rw) && fence(v, e, &rw);
    part_close(&rw);
    return ok;
}

static bool peer_closed(handle_t h, uint64_t deadline)
{
    signals_t seen = 0;
    return jam_object_wait_one(h, SIG_PEER_CLOSED, deadline, &seen) == OK;
}

/* DR_SERVE closed: the driver exits 0, and its block channels close. */
static bool t_storage_stop(void)
{
    struct drive *v = &boot_drive;
    CHECK(v->storage != HANDLE_INVALID && v->nparts >= 1);
    handle_t blk;
    CHECK_ST(storage_open_partition_until(v->storage, soon(), 0, 1, &blk), OK);
    jam_handle_close(v->storage);
    v->storage = HANDLE_INVALID;
    struct process_info info = { 0 };
    status_t st = spawn_wait(v->proc, 10 * NS_PER_S, &info);
    bool closed = peer_closed(blk, in(5 * NS_PER_S));
    jam_handle_close(blk);
    CHECK_ST(st, OK);
    jam_handle_close(v->proc);
    v->proc = HANDLE_INVALID;
    CHECK(info.exit_code == 0 && !info.killed);
    CHECK(closed);
    return true;
}

/* ---- the second disk ----------------------------------------------------------- */

static bool t_storage_disk2(void)
{
    struct raw r;
    CHECK(find_disk(DISK2, &r));
    handle_t usb;
    CHECK_ST(take_disk(&r, &usb), OK);
    if (!drive_start(&disk2, usb, "usb-storage-disk2"))
        return false;
    CHECK(disk2.qemu && disk2.nparts == 2);
    struct pch p = { 0 }, ro = { 0 };
    bool ok = check_esp(&disk2, &p, true);
    part_close(&p);
    ok = ok && check_write(&disk2, &p, &ro);
    part_close(&p);
    part_close(&ro);
    return ok;
}

/* The disk is unplugged while it is being read: the read in flight fails
 * in bounded time, the channel closes, the driver exits 0. */
static bool t_storage_unplug(void)
{
    struct drive *v = &disk2;
    struct pch p = { 0 };
    int e = part_of_type(v, 0x0c, 0x0b);
    CHECK(e >= 0);
    if (!part_open(v, (uint8_t)e, true, &p))
        return false;
    uint32_t per = BUF_SIZE / v->bs;
    printf("usbtest: unplug the second disk now\n");
    uint64_t end = in(30 * NS_PER_S), lba = 0, took = 0;
    unsigned reads = 0;
    status_t st = OK;
    while (st == OK && now() < end) {
        uint64_t t0 = now();
        st = rd(&p, lba, per, 0);
        took = now() - t0;
        reads++;
        lba = lba + 2 * per <= p.blocks ? lba + per : 0;
    }
    bool closed = st != OK && peer_closed(p.ch, in(15 * NS_PER_S));
    part_close(&p);
    struct process_info info = { 0 };
    status_t ended = spawn_wait(v->proc, 15 * NS_PER_S, &info);
    printf("usbtest: after %u reads of 64 KiB: %s, %lu ms after it was asked for\n", reads,
           status_str(st), (unsigned long)(took / NS_PER_MS));
    CHECK(st == ERR_PEER_CLOSED || st == ERR_IO || st == ERR_TIMED_OUT);
    CHECK(took < 20 * NS_PER_S);
    CHECK(closed);
    CHECK_ST(ended, OK);
    CHECK(info.exit_code == 0 && !info.killed);
    return true;
}

/* ---- the slow disk ------------------------------------------------------------- */

/* The slow disk's usb-storage, started by the first check that needs it. */
static bool slow_start(void)
{
    if (slow.proc)
        return true;
    struct raw r;
    CHECK(find_disk(SLOW, &r));
    handle_t usb;
    CHECK_ST(take_disk(&r, &usb), OK);
    return drive_start(&slow, usb, "usb-storage-slow");
}

/* A READ of count blocks at lba into p's buffer, sent without waiting for
 * its answer. */
static status_t read_send(const struct pch *p, uint64_t lba, uint32_t count)
{
    struct block_read_req q = { .txid = 1, .ordinal = BLOCK_READ, .lba = lba, .count = count };
    return jam_channel_write(p->ch, &q, sizeof(q), NULL, 0);
}

/* That READ's answer, if it comes by deadline (ERR_SHOULD_WAIT: not yet). */
static status_t read_answer(const struct pch *p, uint64_t deadline)
{
    signals_t seen = 0;
    status_t st = jam_object_wait_one(p->ch, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
    if (st != OK)
        return st == ERR_TIMED_OUT ? ERR_SHOULD_WAIT : st;
    struct block_read_rep r;
    uint32_t n = 0;
    struct channel_read_args a = { .h = p->ch, .rd_cap = sizeof(r),
                                   .rd_bytes = (uint64_t)(uintptr_t)&r,
                                   .rd_actual = (uint64_t)(uintptr_t)&n };
    st = jam_channel_read(&a);
    return st == OK ? idl_rep_status(&r, n, sizeof(r)) : st;
}

/* usb-storage killed with a READ in flight (the disk is read at 256 KiB/s, so the 64 KiB
 * READ is still going a few tens of ms in: the CBW is taken, the data phase half done), and a
 * new one started on the same disk, the way devmgr restarts a driver that crashed. The new
 * one must take the device over and serve (drive_start reads its partitions through the
 * `storage` protocol), and read. */
static bool t_storage_kill(void)
{
    struct drive *v = &disk2;
    struct pch p = { 0 };
    int e = part_of_type(v, 0x0c, 0x0b);
    CHECK(e >= 0 && v->proc != HANDLE_INVALID);
    CHECK(part_open(v, (uint8_t)e, true, &p));
    CHECK_ST(read_send(&p, 0, BUF_SIZE / v->bs), OK);
    jam_sleep_until(in(60 * NS_PER_MS));
    CHECK_ST(jam_process_kill(v->proc), OK);
    part_close(&p);
    drive_stop(v);   /* waits for it to be gone */
    jam_sleep_until(in(200 * NS_PER_MS));   /* usb-bus sees the channel close */
    struct raw r;
    CHECK(find_disk(DISK2, &r));
    handle_t usb;
    CHECK_ST(take_disk(&r, &usb), OK);
    uint64_t t0 = now();
    if (!drive_start(v, usb, "usb-storage-disk2"))
        return false;
    printf("usbtest: a usb-storage killed with a READ in flight: the next one took over in %lu "
           "ms\n", (unsigned long)((now() - t0) / NS_PER_MS));
    CHECK(v->qemu && v->nparts == 2);
    bool ok = part_open(v, (uint8_t)e, true, &p);
    ok = ok && rd(&p, 0, BUF_SIZE / v->bs, 0) == OK;
    part_close(&p);
    CHECK(ok);
    return true;
}

/* While a READ waits on the slow disk (its data phase can take the whole
 * 5 s), the second disk reads 64 KiB: about a quarter of a second at its
 * 256 KiB/s, since usb-bus runs each device's bulk transfers apart (one
 * transfer at a time for the whole bus would hold it until the slow one
 * ends). A READ the slow disk answers at once (QEMU's throttling lets a
 * burst through) doesn't count: the next one is tried. */
static bool apart(const struct pch *sp, const struct pch *fast)
{
    uint64_t took = 0;
    status_t st = OK, slow_st = OK;
    unsigned tries = 0;
    bool tried = false;
    for (; tries < 8 && !tried && slow_st != ERR_PEER_CLOSED; tries++) {
        CHECK_ST(read_send(sp, 48 * (uint64_t)tries, 48), OK);
        slow_st = read_answer(sp, in(300 * NS_PER_MS));
        if (slow_st != ERR_SHOULD_WAIT)
            continue;
        uint64_t t0 = now();
        st = rd(fast, 0, BUF_SIZE / disk2.bs, 0);
        took = now() - t0;
        tried = true;
        slow_st = read_answer(sp, in(BLK_WAIT));
    }
    printf("usbtest: 64 KiB from the second disk in %lu ms while a READ waited on the slow one "
           "(try %u; the slow READ: %s)\n", (unsigned long)(took / NS_PER_MS), tries,
           status_str(slow_st));
    CHECK(tried);
    CHECK_ST(st, OK);
    CHECK(took < 2 * NS_PER_S);
    return true;
}

static bool t_storage_apart(void)
{
    CHECK(disk2.storage != HANDLE_INVALID);
    int e = part_of_type(&disk2, 0x0c, 0x0b);
    CHECK(e >= 0);
    if (!slow_start())
        return false;
    struct pch sp = { 0 }, fast = { 0 };
    bool ok = part_open(&slow, 0, true, &sp) && part_open(&disk2, (uint8_t)e, true, &fast) &&
              apart(&sp, &fast);
    part_close(&sp);
    part_close(&fast);
    return ok;
}

/* A disk too slow for a READ's 5 s (QEMU reads this one at 4 KiB/s, in
 * bursts: a READ of 24 KiB may pass at once or wait 6 s). What must hold
 * whatever the bursts do: a READ that gets no data in time fails
 * ERR_TIMED_OUT in bounded time (it doesn't hang), and then either a later
 * READ works (reset recovery left the disk usable), or, after three
 * commands in a row without an answer, the driver gives up: exit 3, its
 * channels closed. */
static bool t_storage_timeout(void)
{
    if (!slow_start())
        return false;
    struct pch p = { 0 };
    if (!part_open(&slow, 0, true, &p))
        return false;
    unsigned timeouts = 0, worked_after = 0, other = 0, reads = 0;
    uint64_t worst = 0;
    status_t st = OK;
    for (; reads < 8 && st != ERR_PEER_CLOSED && !(timeouts && worked_after); reads++) {
        uint64_t t0 = now();
        st = rd(&p, 48 * (uint64_t)reads, 48, 0);
        if (now() - t0 > worst)
            worst = now() - t0;
        if (st == ERR_TIMED_OUT)
            timeouts++;
        else if (st == OK)
            worked_after += timeouts != 0;
        else if (st != ERR_PEER_CLOSED)
            other++;
    }
    bool gave_up = st == ERR_PEER_CLOSED;
    bool closed = gave_up && peer_closed(p.ch, in(15 * NS_PER_S));
    part_close(&p);
    struct process_info info = { 0 };
    status_t ended = gave_up ? spawn_wait(slow.proc, 15 * NS_PER_S, &info) : OK;
    printf("usbtest: %u READs of 24 KiB from a 4 KiB/s disk: %u timed out (the longest call "
           "%lu ms), then %s\n", reads, timeouts, (unsigned long)(worst / NS_PER_MS),
           gave_up ? "the driver gave up" : "one worked");
    CHECK(timeouts >= 1 && !other);
    CHECK(worst < 40 * NS_PER_S);
    CHECK(worked_after || gave_up);
    if (gave_up) {
        CHECK(closed);
        CHECK_ST(ended, OK);
        CHECK(info.exit_code == 3 && !info.killed);
    }
    return true;
}

/* ---- all of them --------------------------------------------------------------- */

void storage_tests(void)
{
    if (load() != OK || !find_disk(NULL, &boot_raw)) {
        printf("usbtest: storage: no mass-storage device: skipped\n");
        skipped += 10;
    } else {
        mounts_note();   /* give_back waits for these to be back */
        run("storage_bulk", t_storage_bulk);
        if (!boot_raw.buf) {
            skipped += 9;   /* it said why */
        } else {
            run("storage_handover", t_storage_handover);
            run("storage_stall", t_storage_stall);
            run("storage_bind", t_storage_bind);
            run("storage_esp", t_storage_esp);
            run("storage_range", t_storage_range);
            run("storage_write", t_storage_write);
            run("block_in_flight", t_block_in_flight);
            run("storage_fence", t_storage_fence);
            run("storage_stop", t_storage_stop);
        }
        raw_close(&boot_raw);
        drive_stop(&boot_drive);
        give_back(&boot_raw);
    }
    /* These two are not given back: one is unplugged, the other is left
     * timing out. */
    struct raw r;
    if (!find_disk(DISK2, &r) || !find_disk(SLOW, &r)) {
        printf("usbtest: storage_disk2, storage_kill, storage_apart, storage_unplug, "
               "storage_timeout: no disks with serials " DISK2 " and " SLOW " (the "
               "tools/storage-test.sh scenario): skipped\n");
        skipped += 5;
        return;
    }
    run("storage_disk2", t_storage_disk2);
    run("storage_kill", t_storage_kill);
    run("storage_apart", t_storage_apart);
    run("storage_unplug", t_storage_unplug);
    drive_stop(&disk2);
    run("storage_timeout", t_storage_timeout);
    drive_stop(&slow);
}
