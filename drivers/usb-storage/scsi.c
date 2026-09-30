/* usb-storage: the SCSI commands a USB stick needs (SPC-4, SBC-3), over
 * the transport in bot.c: INQUIRY, TEST UNIT READY, REQUEST SENSE, READ
 * CAPACITY(10), READ(10), WRITE(10), SYNCHRONIZE CACHE(10).
 *
 * run() is one command to its end: a transport failure (already reset by
 * bot.c) is tried once more; a command the device failed is followed by
 * REQUEST SENSE, and a UNIT ATTENTION (the device's one-time "something
 * changed": power on, a reset, a new medium) is tried once more too.
 * Anything else is an error, mapped from the sense key.
 *
 * Timeouts: 10 s for a write or a cache flush, 5 s otherwise. The small
 * commands keep their data in the bulk buffer's tail, so READ and WRITE
 * data at its start is not disturbed by a REQUEST SENSE in between. */
#include "storage.h"

#define OP_TEST_UNIT_READY 0x00
#define OP_REQUEST_SENSE   0x03
#define OP_INQUIRY         0x12
#define OP_READ_CAPACITY   0x25
#define OP_READ_10         0x28
#define OP_WRITE_10        0x2a
#define OP_SYNC_CACHE      0x35

#define KEY_NOT_READY       0x2
#define KEY_ILLEGAL_REQUEST 0x5
#define KEY_UNIT_ATTENTION  0x6
#define KEY_DATA_PROTECT    0x7
#define ASC_LBA_RANGE       0x21   /* logical block address out of range */
#define ASC_NO_MEDIUM       0x3a

#define READ_MS   5000u
#define WRITE_MS  10000u
#define READY_NS  (10 * NS_PER_S)   /* how long a medium may take to become ready */

/* Where the small commands' data goes: after the CBW and CSW. */
static uint32_t aux_off(const struct disk *k)
{
    return k->data_max + 1024;
}

/* REQUEST SENSE into k->key / asc / ascq (key 0xff: it couldn't be read). */
static status_t request_sense(struct disk *k)
{
    struct bot_cmd c = { .cdb = { OP_REQUEST_SENSE, 0, 0, 0, 18 }, .cdb_len = 6, .in = true,
                         .off = aux_off(k), .len = 18, .timeout_ms = READ_MS };
    uint32_t moved = 0;
    bool failed = false;
    k->key = 0xff;
    k->asc = k->ascq = 0;
    status_t st = bot_run(k, &c, &moved, &failed);
    if (st != OK)
        return st;
    const uint8_t *s = k->xbuf + c.off;
    if (!failed && moved >= 14 && (s[0] & 0x7e) == 0x70) {   /* fixed-format sense */
        k->key = s[2] & 0xf;
        k->asc = s[12];
        k->ascq = s[13];
    }
    return OK;
}

static status_t sense_status(const struct disk *k)
{
    switch (k->key) {
    case KEY_DATA_PROTECT: return ERR_ACCESS_DENIED;
    case KEY_ILLEGAL_REQUEST: return k->asc == ASC_LBA_RANGE ? ERR_OUT_OF_RANGE
                                                             : ERR_NOT_SUPPORTED;
    default: return ERR_IO;
    }
}

/* c to its end; *moved: its data bytes. quiet: a failure is expected (the
 * caller probes), so it isn't logged. */
static status_t run(struct disk *k, const struct bot_cmd *c, uint32_t *moved, bool quiet)
{
    status_t st = ERR_IO;
    k->key = 0xff;
    k->asc = k->ascq = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        bool failed = false;
        st = bot_run(k, c, moved, &failed);
        if (st == ERR_IO)
            continue;   /* the transport was reset: once more */
        if (st != OK || !failed)
            return st;
        st = request_sense(k);
        if (st != OK)
            return st;
        if (k->key == KEY_UNIT_ATTENTION)
            continue;
        break;
    }
    if (st == OK)
        st = sense_status(k);   /* the device failed it */
    if (!quiet && ++k->failures <= 8)
        drv_log("usb-storage %04x:%04x: command %02x: %s (sense %x/%02x/%02x)", k->vid, k->pid,
                c->cdb[0], status_str(st), k->key, k->asc, k->ascq);
    return st;
}

static status_t inquiry(struct disk *k)
{
    struct bot_cmd c = { .cdb = { OP_INQUIRY, 0, 0, 0, 36 }, .cdb_len = 6, .in = true,
                         .off = aux_off(k), .len = 36, .timeout_ms = READ_MS };
    uint32_t moved = 0;
    status_t st = run(k, &c, &moved, false);
    if (st != OK)
        return st;
    const uint8_t *d = k->xbuf + c.off;
    if (moved < 36)
        return ERR_IO;
    if ((d[0] & 0x1f) != 0) {   /* peripheral device type 0: a direct-access block device */
        drv_log("usb-storage %04x:%04x: device type %u is not a disk", k->vid, k->pid,
                d[0] & 0x1f);
        return ERR_NOT_SUPPORTED;
    }
    __builtin_memcpy(k->vendor, d + 8, 8);
    __builtin_memcpy(k->product, d + 16, 16);
    return OK;
}

/* TEST UNIT READY until it passes. OK; ERR_NOT_FOUND: no medium; else
 * the last error once READY_NS have passed. */
static status_t wait_ready(struct disk *k)
{
    struct bot_cmd c = { .cdb = { OP_TEST_UNIT_READY }, .cdb_len = 6, .timeout_ms = READ_MS };
    uint64_t end = drv_clock_ns() + READY_NS;
    for (;;) {
        uint32_t moved = 0;
        status_t st = run(k, &c, &moved, true);
        if (st == OK || st == ERR_PEER_CLOSED)
            return st;
        if (k->key == KEY_NOT_READY && k->asc == ASC_NO_MEDIUM)
            return ERR_NOT_FOUND;
        if (drv_clock_ns() >= end) {
            drv_log("usb-storage %04x:%04x: not ready after 10 s: %s (sense %x/%02x/%02x)",
                    k->vid, k->pid, status_str(st), k->key, k->asc, k->ascq);
            return st;
        }
        drv_sleep_until(drv_clock_ns() + 100 * NS_PER_MS);
    }
}

static status_t read_capacity(struct disk *k)
{
    struct bot_cmd c = { .cdb = { OP_READ_CAPACITY }, .cdb_len = 10, .in = true,
                         .off = aux_off(k), .len = 8, .timeout_ms = READ_MS };
    uint32_t moved = 0;
    status_t st = run(k, &c, &moved, false);
    if (st != OK)
        return st;
    if (moved < 8)
        return ERR_IO;
    const uint8_t *d = k->xbuf + c.off;
    uint32_t last = be32(d), size = be32(d + 4);
    if (size != 512 && size != 1024 && size != 2048 && size != 4096) {
        drv_log("usb-storage %04x:%04x: block size %u is not supported", k->vid, k->pid, size);
        return ERR_NOT_SUPPORTED;
    }
    /* 0xffffffff means "more than READ CAPACITY(10) can say" (over 2 TiB
     * at 512 bytes): the part READ(10) reaches is used. */
    k->blocks = (uint64_t)last + 1;
    k->block_size = size;
    return OK;
}

status_t scsi_bring_up(struct disk *k)
{
    k->block_size = 0;
    k->blocks = 0;
    status_t st = inquiry(k);
    if (st != OK)
        return st;
    st = wait_ready(k);
    if (st == ERR_NOT_FOUND) {
        drv_log("usb-storage %04x:%04x: no medium", k->vid, k->pid);
        return OK;
    }
    if (st != OK)
        return st;
    return read_capacity(k);
}

status_t scsi_rw(struct disk *k, bool write, uint64_t lba, uint32_t count)
{
    if (!k->block_size || !count || count > k->data_max / k->block_size || lba > 0xffffffffu)
        return ERR_OUT_OF_RANGE;
    struct bot_cmd c = { .cdb_len = 10, .in = !write, .off = 0, .len = count * k->block_size,
                         .timeout_ms = write ? WRITE_MS : READ_MS };
    c.cdb[0] = write ? OP_WRITE_10 : OP_READ_10;
    c.cdb[2] = (uint8_t)(lba >> 24);
    c.cdb[3] = (uint8_t)(lba >> 16);
    c.cdb[4] = (uint8_t)(lba >> 8);
    c.cdb[5] = (uint8_t)lba;
    c.cdb[7] = (uint8_t)(count >> 8);
    c.cdb[8] = (uint8_t)count;
    uint32_t moved = 0;
    status_t st = run(k, &c, &moved, false);
    if (st == OK && moved != c.len)
        st = ERR_IO;   /* passed, but not all of it moved */
    return st;
}

status_t scsi_sync(struct disk *k)
{
    if (k->no_sync || !k->block_size)
        return k->gone ? ERR_PEER_CLOSED : OK;
    struct bot_cmd c = { .cdb = { OP_SYNC_CACHE }, .cdb_len = 10, .timeout_ms = WRITE_MS };
    uint32_t moved = 0;
    status_t st = run(k, &c, &moved, true);
    if (st == ERR_NOT_SUPPORTED) {
        /* No such command: a stick without a write cache. Everything
         * written is on the medium already. */
        k->no_sync = true;
        return OK;
    }
    return st;
}
