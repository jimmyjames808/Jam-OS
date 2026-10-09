/* usb-storage: the USB mass-storage class driver (Bulk-Only Transport,
 * SCSI transparent command set: interface class 08, subclass 06, protocol
 * 50). One process per mass-storage interface, started by devmgr; drv/
 * usb-storage in bootfs.
 *
 * Handles (roles from <jam/driver.h>):
 *   DR_USB    the `usb` channel of this one interface (abi/idl/usb.idl);
 *             usb-bus closes it when the stick is unplugged
 *   DR_SERVE  the channel it serves `storage` on (abi/idl/storage.idl);
 *             whoever started it holds the other end (devmgr). Without
 *             one there is nobody to serve: exit 0 before the device is
 *             touched. It also answers the svc protocol's connect
 *             (abi/idl/svc.idl) with a `storage` channel of the caller's
 *             own (MAX_CONNS at once): devmgr asks for one for volumes,
 *             which is the disk's one real client, so nothing volumes
 *             reads is ever queued on a channel devmgr holds too.
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
 * Then it serves: `storage` on DR_SERVE and on each connect's channel
 * (the disk, its partitions, and open_partition, which makes a `block`
 * channel limited to one partition), and every open block channel
 * (block.c). Everything arrives on one port;
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
 * closed: we are being stopped, exit 0 (a connect's channel closed ends
 * that channel alone: volumes was started again). Either way the process's end
 * closes every block channel, which is how clients learn of it
 * (ERR_PEER_CLOSED). Exit 1: no DR_USB, or the port failed; 2: not a
 * Bulk-Only SCSI interface; 3: the device couldn't be brought up, or
 * stopped answering (devmgr restarts the driver, with backoff). */
#include <idl/storage.h>
#include <idl/svc.h>
#include <idl/usb.h>
#include "storage.h"

#define USB_TIMEOUT (5 * NS_PER_S)   /* a control request or a descriptor */
#define MAX_CONNS   4                /* connects' channels at once (one per volumes) */

#define DESC_CONFIG    2
#define DESC_INTERFACE 4
#define DESC_ENDPOINT  5

static struct disk disk;
static handle_t port, serve;
static bool serve_pending, serve_closed, serve_held;
struct idl_held storage_held;

/* A `storage` channel made by svc.connect on DR_SERVE. */
static struct conn {
    handle_t ch;        /* our end; HANDLE_INVALID: a free slot */
    bool     pending;   /* may have requests queued */
    bool     held;      /* ... and left there: its client is behind (storage_held) */
    uint16_t gen;       /* bumped at every close: in its port key */
} conns[MAX_CONNS];

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
    uint32_t n = 0;
    st = usb_get_descriptor_until(k->usb, soon(), DESC_CONFIG, 0, 0, sizeof(cfg), 0, cfg, &n);
    if (n > sizeof(cfg))
        n = sizeof(cfg);   /* whatever usb-bus says, the buffer ends here */
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

/* s (n space-padded bytes of INQUIRY's) as a string: ASCII, anything
 * else '?', trailing spaces and NULs trimmed. Returns its length. */
static uint32_t text(char *out, const uint8_t *s, unsigned n)
{
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0))
        n--;
    for (unsigned i = 0; i < n; i++)
        out[i] = s[i] >= 0x20 && s[i] < 0x7f ? (char)s[i] : '?';
    out[n] = 0;
    return n;
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

/* The disk. Only an MBR is read: no GPT disk GUID, ever (storage.idl). */
static status_t s_info(void *ctx, char *vendor, uint32_t *vendor_len, char *product,
                       uint32_t *product_len, uint32_t *block_size, uint64_t *blocks,
                       uint8_t *scheme, uint32_t *partitions, uint32_t *mbr_id,
                       uint8_t disk_guid[16])
{
    const struct disk *k = ctx;
    char v[9], p[17];
    *vendor_len = text(v, k->vendor, 8);
    *product_len = text(p, k->product, 16);
    __builtin_memcpy(vendor, v, *vendor_len);
    __builtin_memcpy(product, p, *product_len);
    *block_size = k->block_size;
    *blocks = k->blocks;
    *scheme = k->scheme;
    *partitions = k->nparts;
    *mbr_id = k->mbr_id;
    __builtin_memset(disk_guid, 0, 16);
    return OK;
}

/* An MBR partition: its type byte, extent; no GUIDs, attributes or name. */
static status_t s_partition(void *ctx, uint32_t index, uint8_t *mbr_type, uint8_t type_guid[16],
                            uint8_t unique_guid[16], uint64_t *start, uint64_t *blocks,
                            uint64_t *attributes, char *name, uint32_t *name_len)
{
    const struct disk *k = ctx;
    (void)name;
    if (index >= k->nparts)
        return ERR_OUT_OF_RANGE;
    *mbr_type = k->parts[index].type;
    __builtin_memset(type_guid, 0, 16);
    __builtin_memset(unique_guid, 0, 16);
    *start = k->parts[index].start;
    *blocks = k->parts[index].blocks;
    *attributes = 0;
    *name_len = 0;
    return OK;
}

static status_t s_open_partition(void *ctx, uint32_t index, uint8_t read_only, handle_t *block)
{
    if (read_only > 1)
        return ERR_INVALID_ARGS;
    if (index >= MAX_PARTS)
        return ERR_OUT_OF_RANGE;
    return blk_open(ctx, port, (uint8_t)index, read_only, block);
}

static const struct storage_ops storage_ops = {
    .info = s_info,
    .partition = s_partition,
    .open_partition = s_open_partition,
};

/* ---- svc.connect: a `storage` channel of the caller's own ------------------------------ */

static uint64_t conn_key(const struct conn *c)
{
    return KEY_CONN | ((uint64_t)c->gen << 8) | (uint64_t)(c - conns);
}

static void conn_close(struct conn *c)
{
    if (c->ch == HANDLE_INVALID)
        return;
    (void)drv_port_unbind(port, c->ch, conn_key(c));
    drv_handle_close(c->ch);
    c->ch = HANDLE_INVALID;
    c->pending = false;
    c->gen++;
}

static status_t s_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    for (unsigned i = 0; i < MAX_CONNS; i++) {
        struct conn *c = &conns[i];
        if (c->ch != HANDLE_INVALID)
            continue;
        handle_t theirs;
        status_t st = drv_channel_create(&c->ch, &theirs);
        if (st != OK) {
            c->ch = HANDLE_INVALID;
            return st;
        }
        st = drv_port_bind(port, c->ch, conn_key(c), SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
        if (st != OK) {
            drv_handle_close(c->ch);
            drv_handle_close(theirs);
            c->ch = HANDLE_INVALID;
            return st;
        }
        c->pending = true;   /* look once: the port says only what comes later */
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

static const struct svc_ops svc_ops = { .connect = s_connect };
static const struct svc_ops no_svc_ops = { 0 };   /* a connect's channel makes no more */

/* serve_one's: nothing read, ch's client has the unread bound of answers
 * unread (storage_unread_full): the channel is held, not pending. */
#define SERVE_HELD ERR_BUSY

/* Take one message off ch and answer it, as storage_serve_one does, with
 * the svc protocol's connect next to `storage` (on DR_SERVE alone:
 * `connect`). OK once a message was handled; SERVE_HELD; else
 * drv_channel_read's status (ERR_SHOULD_WAIT: nothing queued;
 * ERR_PEER_CLOSED: the client is gone for good). */
static status_t serve_one(handle_t ch, struct disk *k, bool connect)
{
    _Static_assert(SVC_REQ_MAX <= STORAGE_REQ_MAX && SVC_REP_MAX <= STORAGE_REP_MAX,
                   "a connect fits where a storage request does");
    _Alignas(8) uint8_t q[STORAGE_REQ_MAX], r[STORAGE_REP_MAX];
    handle_t hs[IDL_READ_HANDLES], rhs[IDL_REP_HANDLES];
    uint32_t n = 0, nh = 0, rhn = 0, rn;
    if (storage_unread_full(ch)) {   /* the unread bound: its client reads first */
        idl_hold(&storage_held);
        return SERVE_HELD;
    }
    status_t st = drv_channel_read(ch, q, sizeof(q), &n, hs, IDL_READ_HANDLES, &nh);
    if (st == ERR_BUFFER_TOO_SMALL)
        return idl_drain(ch, n, nh);
    if (st != OK)
        return st;
    if (nh) {
        idl_close_all(hs, nh);
        idl_reply_status(ch, q, n, ERR_INVALID_ARGS);
        return OK;
    }
    const struct idl_req_hdr *h = (const struct idl_req_hdr *)q;
    if (n >= sizeof(*h) && h->ordinal >> 16 == SVC_PROTOCOL_ID)
        rn = svc_dispatch(connect ? &svc_ops : &no_svc_ops, k, q, n, r, rhs, &rhn);
    else
        rn = storage_dispatch(&storage_ops, k, q, n, r, rhs, &rhn);
    if (rn)
        (void)drv_channel_write(ch, r, rn, rhs, rhn);   /* its handles go either way */
    else
        idl_close_all(rhs, rhn);
    return OK;
}

/* ---- the loop --------------------------------------------------------------------------- */

static void packet(struct disk *k, const struct port_packet *p)
{
    if (p->key == KEY_SERVE)
        serve_pending = true;
    else if (p->key == KEY_USB)
        k->gone = true;
    else if ((p->key & ~0xffffffull) == KEY_BLK)
        blk_packet(p);
    else if ((p->key & ~0xffffffull) == KEY_CONN && (p->key & 0xff) < MAX_CONNS &&
             conn_key(&conns[p->key & 0xff]) == p->key)
        conns[p->key & 0xff].pending = true;
}

/* What is queued on DR_SERVE; true if there was anything. */
static bool serve_storage(struct disk *k)
{
    if (!serve_pending)
        return false;
    serve_pending = false;
    status_t st = OK;
    for (int guard = 0; guard < 16 && st == OK; guard++)
        st = serve_one(serve, k, true);
    if (st == OK)
        serve_pending = true;   /* maybe more: the port won't say so again */
    else if (st == SERVE_HELD)
        serve_held = true;
    else if (st != ERR_SHOULD_WAIT)
        serve_closed = true;
    return true;
}

/* What is queued on each connect's channel; true if there was anything.
 * One whose client has gone is closed: that client's volumes ended. */
static bool serve_conns(struct disk *k)
{
    bool did = false;
    for (unsigned i = 0; i < MAX_CONNS; i++) {
        struct conn *c = &conns[i];
        if (c->ch == HANDLE_INVALID || !c->pending)
            continue;
        did = true;
        c->pending = false;
        status_t st = OK;
        for (int guard = 0; guard < 16 && st == OK; guard++)
            st = serve_one(c->ch, k, false);
        if (st == OK)
            c->pending = true;
        else if (st == SERVE_HELD)
            c->held = true;
        else if (st != ERR_SHOULD_WAIT)
            conn_close(c);
    }
    return did;
}

/* The channels held back (the unread bound) pending again: serving them
 * looks again. */
static void unheld(void)
{
    serve_pending |= serve_held;
    serve_held = false;
    for (unsigned i = 0; i < MAX_CONNS; i++) {
        conns[i].pending |= conns[i].held && conns[i].ch != HANDLE_INVALID;
        conns[i].held = false;
    }
    blk_unheld();
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
        if (idl_held_due(&storage_held))
            unheld();
        bool did = serve_storage(k);
        if (serve_conns(k))
            did = true;
        if (blk_serve())
            did = true;
        if (did)
            continue;
        struct port_packet p;
        status_t st = drv_port_wait(port, idl_held_deadline(&storage_held, DEADLINE_NEVER), &p);
        if (st == ERR_TIMED_OUT)
            continue;
        if (st != OK) {
            drv_log("usb-storage %04x:%04x: port wait: %s", k->vid, k->pid, status_str(st));
            return 1;
        }
        packet(k, &p);
    }
    drv_log("usb-storage %04x:%04x: %s; %u reset recover%s, %u failed command(s), %u request(s) "
            "of gone clients dropped", k->vid, k->pid,
            k->gone ? "the device is gone" : "stopped", k->resets, k->resets == 1 ? "y" : "ies",
            k->failures, k->dropped);
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
    for (unsigned i = 0; i < MAX_CONNS; i++)
        conn_close(&conns[i]);
    blk_close_all();
    return r;
}
