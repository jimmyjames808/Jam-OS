/* usb-storage: the USB mass-storage class driver (Bulk-Only Transport,
 * SCSI transparent command set: interface class 08, subclass 06, protocol
 * 50). One process per mass-storage interface, started by devmgr; drv/
 * usb-storage in bootfs.
 *
 * Handles (roles from <jam/driver.h>):
 *   DR_USB    the `usb` channel of this one interface (abi/idl/usb.idl);
 *             usb-bus closes it when the stick is unplugged
 *   DR_SERVE  the channel it serves `storage` on (abi/idl/storage.idl);
 *             whoever started it holds the other end. Without one there
 *             is nobody to serve: exit 0 before the device is touched.
 *
 * Start: usb.info (class 08/06/50, or exit 2); the configuration
 * descriptor (this interface's bulk IN and OUT endpoints); GET MAX LUN
 * (only unit 0 is used); usb.open_bulk, which gives the 64 KiB buffer all
 * data moves through; then INQUIRY, TEST UNIT READY until the medium is
 * ready, READ CAPACITY and the partition table (scsi.c, block.c). A
 * device left in the middle of a command by whoever drove it before
 * (a driver that crashed) answers the first command with a STALL or not
 * at all; the transport's reset recovery (bot.c) gets it back.
 *
 * Then it serves: `storage` on DR_SERVE (the disk, its partitions, and
 * open_partition, which makes a `block` channel limited to one partition),
 * and every open block channel (block.c). Everything arrives on one port;
 * a packet only marks its channel pending, and the loop serves one
 * request at a time, so one SCSI command runs at a time.
 *
 * Authority: it holds one interface's `usb` channel and the shared bulk
 * buffer, never a dma_cap. It serves no channel to the whole disk.
 *
 * Every wait is bounded (bot.c), so a stick that stops answering fails
 * its requests (ERR_TIMED_OUT, ERR_IO), never hangs a client. After
 * MAX_SILENT commands in a row without any answer the driver gives up
 * (exit 3): its clients see their channels close instead of waiting out
 * every further request's timeouts, and devmgr starts a fresh driver.
 *
 * Ending: DR_USB closed, or usb-bus answering ERR_PEER_CLOSED: the stick
 * is gone, exit 0 (devmgr binds a new driver when it comes back). DR_SERVE
 * closed: we are being stopped, exit 0. Either way the process's end
 * closes every block channel, which is how clients learn of it
 * (ERR_PEER_CLOSED). Exit 1: no DR_USB, or the port failed; 2: not a
 * Bulk-Only SCSI interface; 3: the device couldn't be brought up, or
 * stopped answering (devmgr restarts the driver, with backoff). */
#include <idl/storage.h>
#include <idl/usb.h>
#include "storage.h"

#define USB_TIMEOUT (5 * NS_PER_S)   /* a control request or a descriptor */

#define DESC_CONFIG    2
#define DESC_INTERFACE 4
#define DESC_ENDPOINT  5

static struct disk disk;
static handle_t port, serve;
static bool serve_pending, serve_closed;

static uint64_t soon(void)
{
    return drv_clock_ns() + USB_TIMEOUT;
}

/* ---- finding the interface and its endpoints ---------------------------------------- */

/* The interface's bulk endpoints, from the configuration descriptor (cfg,
 * n bytes): the first IN and the first OUT of its active setting. */
static bool find_endpoints(struct disk *k, uint8_t alt, const uint8_t *cfg, uint32_t n)
{
    bool mine = false;
    k->ep_in = k->ep_out = 0;
    for (uint32_t i = 0; i + 2 <= n && cfg[i] >= 2 && i + cfg[i] <= n; i += cfg[i]) {
        const uint8_t *p = cfg + i;
        if (p[1] == DESC_INTERFACE && p[0] >= 9) {
            mine = p[2] == k->ifnum && p[3] == alt;
        } else if (mine && p[1] == DESC_ENDPOINT && p[0] >= 7 && (p[3] & 3) == 2) {
            if ((p[2] & 0x80) && !k->ep_in)
                k->ep_in = p[2];
            else if (!(p[2] & 0x80) && !k->ep_out)
                k->ep_out = p[2];
        }
    }
    return k->ep_in && k->ep_out;
}

/* usb.info and the endpoints. 0, or the exit code. */
static int identify(struct disk *k)
{
    uint8_t speed, cls, sub, proto, nep, alt, address;
    status_t st = usb_info_until(k->usb, soon(), &k->vid, &k->pid, &speed, &k->ifnum, &cls, &sub,
                                 &proto, &nep, &alt, &address);
    if (st != OK) {
        drv_log("usb-storage: usb.info: %s", status_str(st));
        return st == ERR_PEER_CLOSED ? 0 : 3;
    }
    if (cls != 0x08 || sub != 0x06 || proto != 0x50) {
        drv_log("usb-storage %04x:%04x if %u: class %02x/%02x/%02x is not Bulk-Only SCSI storage",
                k->vid, k->pid, k->ifnum, cls, sub, proto);
        return 2;
    }
    static uint8_t cfg[1024];
    uint16_t n = 0;
    st = usb_get_descriptor_until(k->usb, soon(), DESC_CONFIG, 0, 0, sizeof(cfg), 0, &n, cfg);
    if (st != OK || !find_endpoints(k, alt, cfg, n)) {
        drv_log("usb-storage %04x:%04x if %u: no bulk IN and OUT endpoints (configuration "
                "descriptor: %s, %u bytes)", k->vid, k->pid, k->ifnum, status_str(st), n);
        return st == ERR_PEER_CLOSED ? 0 : 3;
    }
    return 0;
}

/* The bulk pair and its buffer, mapped. 0, or the exit code. */
static int open_pipes(struct disk *k)
{
    handle_t vmo;
    status_t st = usb_open_bulk_until(k->usb, soon(), k->ep_in, k->ep_out, &vmo, &k->xsize);
    if (st != OK) {
        drv_log("usb-storage %04x:%04x: usb.open_bulk(%02x, %02x): %s", k->vid, k->pid, k->ep_in,
                k->ep_out, status_str(st));
        return st == ERR_PEER_CLOSED ? 0 : 3;
    }
    void *p = NULL;
    if (k->xsize < 2 * XFER_TAIL)
        st = ERR_OUT_OF_RANGE;
    else
        st = drv_vmo_map(vmo, 0, k->xsize, VMAR_READ | VMAR_WRITE, &p);
    drv_handle_close(vmo);   /* the mapping keeps the buffer */
    if (st != OK) {
        drv_log("usb-storage %04x:%04x: the %u-byte bulk buffer: %s", k->vid, k->pid, k->xsize,
                status_str(st));
        return 3;
    }
    k->xbuf = p;
    k->data_max = k->xsize - XFER_TAIL;
    return 0;
}

/* ---- the RESULTS line ----------------------------------------------------------------- */

/* s (n space-padded bytes) as a string: printable ASCII, trimmed. */
static void text(char *out, const uint8_t *s, unsigned n)
{
    unsigned len = 0;
    for (unsigned i = 0; i < n; i++) {
        out[i] = s[i] >= 0x20 && s[i] < 0x7f ? (char)s[i] : ' ';
        if (out[i] != ' ')
            len = i + 1;
    }
    out[len] = 0;
}

/* One RESULTS line for the disk (the box cuts long lines, so sizes are in
 * MiB there), and each partition's blocks in the log. */
static void report(const struct disk *k, unsigned luns)
{
    char vendor[9], product[17], parts[64];
    text(vendor, k->vendor, 8);
    text(product, k->product, 16);
    /* blocks -> MiB; without a medium there is no block size */
    unsigned shift = k->block_size ? 20 - (unsigned)__builtin_ctz(k->block_size) : 0;
    int o = 0;
    parts[0] = 0;
    for (int i = 0; i < k->nparts && o < (int)sizeof(parts) - 1; i++) {
        o += drv_snprintf(parts + o, sizeof(parts) - (size_t)o, "%s%02x %lu MiB", i ? ", " : " (",
                          k->parts[i].type, (unsigned long)(k->parts[i].blocks >> shift));
        drv_log("usb-storage %04x:%04x: partition %d: type %02x, blocks %lu + %lu", k->vid,
                k->pid, i + 1, k->parts[i].type, (unsigned long)k->parts[i].start,
                (unsigned long)k->parts[i].blocks);
    }
    drv_report("usb-storage %04x:%04x: %s %s, %lu MiB, %u partition(s)%s%s", k->vid, k->pid,
               vendor, product, (unsigned long)(k->blocks >> shift), k->nparts, parts,
               k->nparts ? ")" : "");
    drv_log("usb-storage %04x:%04x: %lu blocks of %u bytes, %u logical unit(s)%s", k->vid, k->pid,
            (unsigned long)k->blocks, k->block_size, luns, luns > 1 ? ": only unit 0 is used" : "");
}

/* ---- the storage protocol (DR_SERVE) --------------------------------------------------- */

static status_t s_info(void *ctx, uint8_t vendor[8], uint8_t product[16], uint32_t *block_size,
                       uint64_t *blocks, uint8_t *partitions)
{
    const struct disk *k = ctx;
    __builtin_memcpy(vendor, k->vendor, 8);
    __builtin_memcpy(product, k->product, 16);
    *block_size = k->block_size;
    *blocks = k->blocks;
    *partitions = k->nparts;
    return OK;
}

static status_t s_partition(void *ctx, uint8_t index, uint8_t *type, uint64_t *start,
                            uint64_t *blocks)
{
    const struct disk *k = ctx;
    if (index >= k->nparts)
        return ERR_OUT_OF_RANGE;
    *type = k->parts[index].type;
    *start = k->parts[index].start;
    *blocks = k->parts[index].blocks;
    return OK;
}

static status_t s_open_partition(void *ctx, uint8_t index, uint8_t read_only, handle_t *block)
{
    if (read_only > 1)
        return ERR_INVALID_ARGS;
    return blk_open(ctx, port, index, read_only, block);
}

static const struct storage_ops storage_ops = {
    .info = s_info,
    .partition = s_partition,
    .open_partition = s_open_partition,
};

/* ---- the loop --------------------------------------------------------------------------- */

static void packet(struct disk *k, const struct port_packet *p)
{
    if (p->key == KEY_SERVE)
        serve_pending = true;
    else if (p->key == KEY_USB)
        k->gone = true;
    else if ((p->key & ~0xffffffull) == KEY_BLK)
        blk_packet(p);
}

/* What is queued on DR_SERVE; true if there was anything. */
static bool serve_storage(struct disk *k)
{
    if (!serve_pending)
        return false;
    serve_pending = false;
    status_t st = OK;
    for (int guard = 0; guard < 16 && st == OK; guard++)
        st = storage_serve_one(serve, &storage_ops, k);
    if (st == OK)
        serve_pending = true;   /* maybe more: the port won't say so again */
    else if (st != ERR_SHOULD_WAIT)
        serve_closed = true;
    return true;
}

/* Serve until the device goes or DR_SERVE closes. 0, or the exit code. */
static int run(struct disk *k)
{
    while (!k->gone && !serve_closed) {
        if (k->silent >= MAX_SILENT) {
            drv_report("usb-storage %04x:%04x: FAILED: no answer to %u commands in a row; "
                       "giving up on it", k->vid, k->pid, k->silent);
            return 3;
        }
        bool did = serve_storage(k);
        if (blk_serve())
            did = true;
        if (did)
            continue;
        struct port_packet p;
        status_t st = drv_port_wait(port, DEADLINE_NEVER, &p);
        if (st != OK) {
            drv_log("usb-storage %04x:%04x: port wait: %s", k->vid, k->pid, status_str(st));
            return 1;
        }
        packet(k, &p);
    }
    drv_log("usb-storage %04x:%04x: %s; %u reset recover%s, %u failed command(s)", k->vid,
            k->pid, k->gone ? "the device is gone" : "stopped", k->resets,
            k->resets == 1 ? "y" : "ies", k->failures);
    return 0;
}

/* The port, watching DR_SERVE and DR_USB. 0, or the exit code. */
static int watch(struct disk *k)
{
    status_t st = drv_port_create(&port);
    if (st == OK)
        st = drv_port_bind(port, serve, KEY_SERVE, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK)
        st = drv_port_bind(port, k->usb, KEY_USB, SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        drv_log("usb-storage: no port: %s", status_str(st));
        return 1;
    }
    serve_pending = true;   /* look once: requests may be there already */
    return 0;
}

/* The device, from its descriptors to its partition table. 0, or the
 * exit code. */
static int bring_up(struct disk *k)
{
    int r = identify(k);
    if (r == 0)
        r = open_pipes(k);
    if (r != 0)
        return r;
    unsigned luns = bot_luns(k);
    status_t st = scsi_bring_up(k);
    if (st == OK)
        st = parts_read(k);
    if (k->gone)
        return 0;
    if (st != OK) {
        drv_report("usb-storage %04x:%04x: FAILED to bring the disk up: %s", k->vid, k->pid,
                   status_str(st));
        return 3;
    }
    report(k, luns);
    return 0;
}

int driver_main(const struct driver_start *s)
{
    struct disk *k = &disk;
    k->usb = drv_handle(s, DR_USB);
    serve = drv_handle(s, DR_SERVE);
    if (k->usb == HANDLE_INVALID) {
        drv_log("usb-storage: no DR_USB channel: nothing to drive");
        return 1;
    }
    /* devmgr gives every USB class driver an input source of the console;
     * a disk has no keys to send, and an open source holds one of the
     * console's slots. */
    if (drv_handle(s, DR_INPUT) != HANDLE_INVALID)
        drv_handle_close(drv_handle(s, DR_INPUT));
    if (serve == HANDLE_INVALID) {
        drv_log("usb-storage: no DR_SERVE channel: nobody to serve the disk to");
        return 0;
    }
    int r = watch(k);
    if (r == 0)
        r = bring_up(k);
    if (r == 0 && !k->gone)
        r = run(k);
    blk_close_all();
    return r;
}
