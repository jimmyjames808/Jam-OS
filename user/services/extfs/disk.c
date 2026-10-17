/* extfs: the disk under the ext library. Its device (struct ext_dev: read
 * and write by byte offset) becomes `block` calls on the one partition
 * channel extfs was given, through the channel's shared buffer (mapped
 * once), a buffer's worth per call, each waited for: the journal's order
 * (data, then the log, then its commit block, then the blocks home) is
 * the order the calls are made in.
 *
 * An answered write is on the medium only where the disk says so: drv/nvme
 * writes with FUA (M12.7), and volumes starts extfs on an NVMe drive with
 * EXTFS_ARG_FUA. Anywhere else (a USB stick: usb-storage's writes land in
 * the stick's cache, in any order) the device has a flush, block.sync
 * (SCSI SYNCHRONIZE CACHE), which the journal calls where its order needs
 * it and request.c before an answer; it is sent only when something was
 * written since the last one, so it costs nothing on a quiet volume.
 *
 * The library reads in multiples of 1 KiB at multiples of 1 KiB and
 * writes whole blocks. A channel whose blocks are bigger than that (a 4 KiB
 * NVMe namespace) gets each read rounded out to its blocks and copied
 * from the buffer; a write that isn't whole blocks of the channel's (a
 * volume of 1 KiB blocks on 4 KiB sectors, which Linux's mkfs never makes)
 * is refused.
 *
 * Every call has a time limit (BLOCK_WAIT), so a disk that stops answering
 * fails the request with ERR_IO instead of hanging extfs; a call that finds
 * the channel closed sets vol.disk_gone, and main.c winds extfs down.
 *
 * A call that times out is tried again (call()). usb-storage gives a SCSI
 * command 10 s; a stick that takes longer (a big SYNCHRONIZE CACHE, or
 * QEMU's stick behind a busy host: docs/CR-PLAN.md's "The disk can fail a
 * shard") is reset and the call answered ERR_TIMED_OUT, the device ready
 * for the next command; our own wait running out leaves the request queued
 * at the driver. Either way the next try may well go through, where the
 * journal used to turn the volume read-only for the rest of the boot.
 *   - A flush is tried again on any channel: it moves no data, and a
 *     second one covers what the first did.
 *   - A read or a write only on a channel that answers in order (queue
 *     depth 1: usb-storage): the try moves the same blocks through the
 *     same buffer, untouched in between, so what landed of the first is
 *     written over with the same bytes, and the retry's answer means every
 *     earlier try is done: the buffer is free when call() returns. drv/nvme
 *     answers in completion order, so there a retry's answer says nothing
 *     of the first command, which could still read the buffer after the
 *     next write has filled it: its timeouts fail the call as before (it
 *     writes with FUA and is never flushed).
 * Nothing else is sent meanwhile (one call at a time), so the journal's
 * order holds: what must follow a flush waits for a try that went through.
 * Bounds: RETRY_TRIES tries within RETRY_BUDGET of the first one's start,
 * with pauses that double. usb-storage gives up on a device after three
 * commands in a row went unanswered (MAX_SILENT: its channels close, the
 * disk is gone here, devmgr starts a fresh driver), and three of its
 * timeouts with their pauses take 31.5 s: the budget covers them. A client
 * waits 60 s for an answer (FS_CALL_TIMEOUT, the pager's page requests
 * too), so an episode leaves the rest of its request 20 s. The count
 * bounds a driver that answers ERR_TIMED_OUT at once (7.5 s of pauses).
 * Spent, the call fails as before (a write or a flush: the journal turns
 * the volume read-only), so a dead disk never holds writers longer. One
 * line per episode, when it ends.
 *
 * In place of the channel, the disk may be a VMO (EXTFS_SR_RAM, init's
 * /tmp in memory): mapped whole, each read and write a copy. */
#include <idl/block.h>
#include "extfs.h"

#define BLOCK_WAIT   (30 * NS_PER_S)
#define BUF_WANT     (256u << 10)     /* the buffer asked for: a journal's staged run in one call */
#define RETRY_TRIES  5                /* tries of a call that times out, the first one too */
#define RETRY_BUDGET (40 * NS_PER_S)  /* ... all of them, from the first one's start */
#define RETRY_PAUSE  (500 * NS_PER_MS)   /* before the second try, doubled before each after */

static uint64_t writes;

enum call { CALL_READ, CALL_WRITE, CALL_SYNC };

static const char *const call_names[] = { "read", "write", "flush" };

/* A call's tries, for its log line. */
struct tries {
    unsigned n;
    uint64_t ns;   /* from the first one's start to the last one's end */
};

static status_t call_once(enum call c, uint64_t deadline, uint64_t lba, uint32_t count)
{
    if (c == CALL_READ)
        return block_read_until(vol.block, deadline, lba, count, 0);
    if (c == CALL_WRITE)
        return block_write_until(vol.block, deadline, lba, count, 0);
    return block_sync_until(vol.block, deadline);
}

/* One block call (through the buffer's start), tried again while it times
 * out, as the header says. */
static status_t call(enum call c, uint64_t lba, uint32_t count, struct tries *t)
{
    uint64_t start = now(), end = start + RETRY_BUDGET;
    uint64_t pause = test_quick_retry() ? NS_PER_MS : RETRY_PAUSE;
    bool again = c == CALL_SYNC || vol.in_order;
    status_t st;
    t->n = 0;
    for (;;) {
        uint64_t at = now();
        st = call_once(c, at + BLOCK_WAIT < end ? at + BLOCK_WAIT : end, lba, count);
        t->n++;
        if (st != ERR_TIMED_OUT || !again || t->n >= RETRY_TRIES || now() + pause >= end)
            break;
        jam_sleep_until(now() + pause);
        pause *= 2;
    }
    t->ns = now() - start;
    if (st == OK && t->n > 1 && c == CALL_SYNC)
        printf("extfs %s: a flush of the disk's cache timed out; try %u went through, %lu ms "
               "after the first began\n", vol.name, t->n, (unsigned long)(t->ns / NS_PER_MS));
    else if (st == OK && t->n > 1)
        printf("extfs %s: %s of %u blocks at %lu timed out; try %u went through, %lu ms after "
               "the first began\n", vol.name, call_names[c], count, (unsigned long)lba, t->n,
               (unsigned long)(t->ns / NS_PER_MS));
    return st;
}

/* " (5 tries in 7512 ms)" for a call that was tried again, else "". */
static const char *tried(const struct tries *t, char *buf, size_t n)
{
    if (t->n < 2)
        return "";
    snprintf(buf, n, " (%u tries in %lu ms)", t->n, (unsigned long)(t->ns / NS_PER_MS));
    return buf;
}

static status_t failed(enum call c, uint64_t lba, uint32_t count, status_t st,
                       const struct tries *t)
{
    char buf[48];
    if (st == ERR_PEER_CLOSED) {
        vol.disk_gone = true;
        return st;
    }
    printf("extfs %s: %s of %u blocks at %lu failed: %s%s\n", vol.name, call_names[c], count,
           (unsigned long)lba, status_str(st), tried(t, buf, sizeof(buf)));
    return ERR_IO;
}

static status_t dev_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    (void)ctx;
    uint8_t *out = buf;
    uint32_t sec = vol.sector;
    uint32_t per = vol.bbuf_size / sec;
    if (per > vol.max_sectors)
        per = vol.max_sectors;
    while (len) {
        uint64_t lba = off / sec;
        uint32_t skip = (uint32_t)(off % sec);
        uint64_t want = ((uint64_t)skip + len + sec - 1) / sec;
        uint32_t count = want > per ? per : (uint32_t)want;
        if (lba >= vol.sectors || count > vol.sectors - lba)
            return ERR_OUT_OF_RANGE;
        struct tries t;
        status_t st = call(CALL_READ, lba, count, &t);
        if (st != OK)
            return failed(CALL_READ, lba, count, st, &t);
        uint32_t n = count * sec - skip;
        if (n > len)
            n = len;
        memcpy(out, vol.bbuf + skip, n);
        out += n;
        off += n;
        len -= n;
    }
    return OK;
}

static status_t dev_write(void *ctx, uint64_t off, uint32_t len, const void *buf)
{
    (void)ctx;
    const uint8_t *in = buf;
    uint32_t sec = vol.sector;
    if (vol.read_only)
        return ERR_ACCESS_DENIED;
    if (off % sec || len % sec)
        return ERR_NOT_SUPPORTED;
    uint32_t per = vol.bbuf_size / sec;
    if (per > vol.max_sectors)
        per = vol.max_sectors;
    while (len) {
        uint64_t lba = off / sec;
        uint32_t count = len / sec > per ? per : len / sec;
        if (lba >= vol.sectors || count > vol.sectors - lba)
            return ERR_OUT_OF_RANGE;
        memcpy(vol.bbuf, in, (size_t)count * sec);
        vol.unflushed = true;   /* a write that fails may still have landed some */
        struct tries t;
        status_t st = call(CALL_WRITE, lba, count, &t);
        if (st != OK)
            return failed(CALL_WRITE, lba, count, st, &t);
        writes++;
        test_die(XF_DIE_WRITE);
        in += (size_t)count * sec;
        off += (uint64_t)count * sec;
        len -= count * sec;
    }
    return OK;
}

uint64_t disk_writes(void)
{
    return writes;
}

status_t disk_flush(void)
{
    if (vol.fua || vol.ram || !vol.unflushed)
        return OK;
    struct tries t;
    char buf[48];
    status_t st = call(CALL_SYNC, 0, 0, &t);
    if (st == ERR_PEER_CLOSED)
        vol.disk_gone = true;
    if (st == ERR_PEER_CLOSED)
        return st;
    if (st != OK) {
        printf("extfs %s: a flush of the disk's cache failed: %s%s\n", vol.name, status_str(st),
               tried(&t, buf, sizeof(buf)));
        return ERR_IO;
    }
    vol.unflushed = false;
    return OK;
}

static status_t dev_flush(void *ctx)
{
    (void)ctx;
    return disk_flush();
}

status_t disk_open(handle_t block, struct ext_dev *dev)
{
    uint32_t bs = 0, size = 0, maxb = 0;
    uint16_t depth = 0;
    uint8_t ro = 0;
    handle_t vmo = HANDLE_INVALID;
    status_t st = block_info_within(block, BLOCK_WAIT, &bs, &vol.sectors, &ro, &maxb, &depth,
                                   NULL);
    if (st == OK && (bs < 512 || bs > 4096 || (bs & (bs - 1)))) {
        printf("extfs %s: %u-byte blocks are not supported\n", vol.name, bs);
        return ERR_NOT_SUPPORTED;
    }
    if (st == OK)
        st = block_map_buffer_within(block, BLOCK_WAIT, BUF_WANT, &vmo, &size);
    if (st == ERR_PEER_CLOSED)
        vol.disk_gone = true;
    if (st != OK)
        return st;
    size -= size % PAGE_SIZE;
    uint64_t addr = 0;
    uint32_t perms = ro ? VMAR_READ : VMAR_READ | VMAR_WRITE;
    st = size >= bs ? jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size, perms, &addr)
                    : ERR_INVALID_ARGS;
    jam_handle_close(vmo);
    if (st != OK)
        return st;
    vol.block = block;
    vol.bbuf = (uint8_t *)(uintptr_t)addr;
    vol.bbuf_size = size;
    vol.sector = bs;
    vol.max_sectors = maxb ? maxb : size / bs;
    vol.read_only = ro != 0;
    vol.in_order = depth == 1;
    *dev = (struct ext_dev){
        .ctx = NULL, .bytes = vol.sectors * bs, .read = dev_read,
        .write = ro ? NULL : dev_write, .flush = ro || vol.fua ? NULL : dev_flush,
    };
    return OK;
}

/* ---- a VMO as the disk (EXTFS_SR_RAM) ------------------------------------------------- */

#define RAM_SECTOR 512u   /* what vol.sector says of it: the library's 1 KiB reads fit */
#define RAM_MIN    (1ull << 20)

static uint8_t *ram;      /* the VMO, mapped whole; NULL: the disk is a block channel */

static status_t ram_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    (void)ctx;
    uint64_t size = vol.sectors * RAM_SECTOR;
    if (off > size || len > size - off)
        return ERR_OUT_OF_RANGE;
    memcpy(buf, ram + off, len);
    return OK;
}

static status_t ram_write(void *ctx, uint64_t off, uint32_t len, const void *buf)
{
    (void)ctx;
    uint64_t size = vol.sectors * RAM_SECTOR;
    if (off > size || len > size - off)
        return ERR_OUT_OF_RANGE;
    memcpy(ram + off, buf, len);
    writes++;
    test_die(XF_DIE_WRITE);
    return OK;
}

status_t disk_open_ram(handle_t vmo, struct ext_dev *dev)
{
    uint64_t size = 0, addr = 0;
    status_t st = jam_vmo_get_size(vmo, &size);
    size -= size % PAGE_SIZE;
    if (st == OK && size < RAM_MIN)
        st = ERR_INVALID_ARGS;
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size, VMAR_READ | VMAR_WRITE,
                          &addr);
    if (st != OK) {
        printf("extfs %s: the memory to serve can't be mapped: %s\n", vol.name, status_str(st));
        return st;
    }
    ram = (uint8_t *)(uintptr_t)addr;
    vol.ram = true;
    vol.block = HANDLE_INVALID;
    vol.sector = RAM_SECTOR;
    vol.sectors = size / RAM_SECTOR;
    vol.max_sectors = (uint32_t)(size / RAM_SECTOR > UINT32_MAX ? UINT32_MAX : size / RAM_SECTOR);
    vol.read_only = false;
    *dev = (struct ext_dev){
        .ctx = NULL, .bytes = size, .read = ram_read, .write = ram_write,
    };
    return OK;
}

status_t disk_is_blank(bool *out)
{
    static uint8_t first[4096];
    uint64_t n = vol.sectors * vol.sector < sizeof(first) ? vol.sectors * vol.sector
                                                          : sizeof(first);
    status_t st = ram ? ram_read(NULL, 0, (uint32_t)n, first) : dev_read(NULL, 0, (uint32_t)n, first);
    if (st != OK)
        return st;
    *out = true;
    for (uint64_t i = 0; i < n && *out; i++)
        *out = first[i] == 0;
    return OK;
}
