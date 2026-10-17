/* utest: the RAM-disk `block` server (ramdisk.h). It follows block.idl as
 * usb-storage must: requests outside the disk are ERR_OUT_OF_RANGE, a
 * read-only session refuses writes with ERR_ACCESS_DENIED, the buffer is
 * handed out once. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/block.h>
#include <os.h>
#include "ramdisk.h"
#include "utest.h"

#define POLL_NS (5 * NS_PER_MS)   /* how often the server looks at `stop` */

static status_t op_info(void *ctx, uint32_t *out_block_size, uint64_t *out_blocks,
                        uint8_t *out_read_only, uint32_t *out_max_blocks,
                        uint16_t *out_queue_depth, uint32_t *out_buffer_max)
{
    struct ramdisk *rd = ctx;
    *out_block_size = RAMDISK_SECTOR;
    *out_blocks = rd->blocks;
    *out_read_only = rd->read_only;
    *out_max_blocks = RAMDISK_BUF / RAMDISK_SECTOR;
    uint16_t depth = __atomic_load_n(&rd->depth, __ATOMIC_RELAXED);
    *out_queue_depth = depth ? depth : 1;
    *out_buffer_max = RAMDISK_BUF;
    return OK;
}

/* One size of buffer, whatever is asked: RAMDISK_BUF (its buffer_max, and
 * what fat's 0 leaves to the server). */
static status_t op_map_buffer(void *ctx, uint32_t size, handle_t *out_buffer, uint32_t *out_size)
{
    struct ramdisk *rd = ctx;
    (void)size;
    if (rd->buf_given)
        return ERR_BAD_STATE;
    status_t st = jam_handle_duplicate(rd->buf_vmo,
                                       RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER,
                                       out_buffer);
    if (st != OK)
        return st;
    rd->buf_given = true;
    *out_size = RAMDISK_BUF;
    return OK;
}

/* The request's range on the disk and in the buffer. */
static status_t range(const struct ramdisk *rd, uint64_t lba, uint32_t count, uint32_t offset)
{
    if (lba > rd->blocks || count > rd->blocks - lba)
        return ERR_OUT_OF_RANGE;
    if (offset > RAMDISK_BUF || count > (RAMDISK_BUF - offset) / RAMDISK_SECTOR)
        return ERR_INVALID_ARGS;
    return OK;
}

/* Is this request of `kind` one ramdisk_time_out asked to time out? It is
 * counted then. */
static bool times_out(struct ramdisk *rd, uint32_t kind)
{
    if (!(__atomic_load_n(&rd->to_kinds, __ATOMIC_RELAXED) & kind))
        return false;
    uint32_t left = __atomic_load_n(&rd->to_left, __ATOMIC_RELAXED);
    while (left && !__atomic_compare_exchange_n(&rd->to_left, &left, left - 1, false,
                                                __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
    if (!left)
        return false;
    __atomic_add_fetch(&rd->timeouts, 1, __ATOMIC_RELAXED);
    return true;
}

/* A read ramdisk_hold_read asked for: unanswered until the client sends
 * more or leaves; then its blocks land in the buffer late. */
static void hold(struct ramdisk *rd, uint8_t how, uint32_t offset, uint32_t count)
{
    signals_t want = how == 2 ? SIG_PEER_CLOSED : SIG_READABLE | SIG_PEER_CLOSED;
    (void)jam_object_wait_one(rd->ch, want, now() + RAMDISK_HOLD_MAX, NULL);
    memset(rd->buf + offset, RAMDISK_LATE, (size_t)count * RAMDISK_SECTOR);
    __atomic_add_fetch(&rd->late, 1, __ATOMIC_RELAXED);
}

static status_t op_read(void *ctx, struct idl_txn txn, uint64_t lba, uint32_t count,
                        uint32_t offset)
{
    (void)txn;   /* answered now */
    struct ramdisk *rd = ctx;
    status_t st = range(rd, lba, count, offset);
    if (st != OK)
        return st;
    uint8_t how = __atomic_exchange_n(&rd->hold, 0, __ATOMIC_RELAXED);
    if (how) {
        hold(rd, how, offset, count);
        return OK;
    }
    if (times_out(rd, RAMDISK_READ)) {
        memcpy(rd->buf + offset, rd->mem + lba * RAMDISK_SECTOR,
               (size_t)(count / 2) * RAMDISK_SECTOR);
        return ERR_TIMED_OUT;
    }
    memcpy(rd->buf + offset, rd->mem + lba * RAMDISK_SECTOR, (size_t)count * RAMDISK_SECTOR);
    __atomic_add_fetch(&rd->reads, 1, __ATOMIC_RELAXED);
    return OK;
}

static status_t op_write(void *ctx, struct idl_txn txn, uint64_t lba, uint32_t count,
                         uint32_t offset)
{
    (void)txn;
    struct ramdisk *rd = ctx;
    if (rd->read_only)
        return ERR_ACCESS_DENIED;
    status_t st = range(rd, lba, count, offset);
    if (st != OK)
        return st;
    uint32_t limit = __atomic_load_n(&rd->fail_after, __ATOMIC_RELAXED);
    if (__atomic_load_n(&rd->fail_writes, __ATOMIC_RELAXED) ||
        (limit && __atomic_load_n(&rd->writes, __ATOMIC_RELAXED) >= limit))
        return ERR_IO;
    if (times_out(rd, RAMDISK_WRITE)) {   /* landed in part: a retry must write it all */
        memcpy(rd->mem + lba * RAMDISK_SECTOR, rd->buf + offset,
               (size_t)(count / 2) * RAMDISK_SECTOR);
        return ERR_TIMED_OUT;
    }
    memcpy(rd->mem + lba * RAMDISK_SECTOR, rd->buf + offset, (size_t)count * RAMDISK_SECTOR);
    __atomic_add_fetch(&rd->writes, 1, __ATOMIC_RELAXED);
    void (*fn)(struct ramdisk *, uint64_t, uint32_t) =
        __atomic_load_n(&rd->on_write, __ATOMIC_ACQUIRE);   /* ramdisk_on_write's release */
    if (fn)
        fn(rd, lba, count);
    return OK;
}

static status_t op_sync(void *ctx, struct idl_txn txn)
{
    (void)txn;
    struct ramdisk *rd = ctx;
    if (times_out(rd, RAMDISK_SYNC))
        return ERR_TIMED_OUT;
    __atomic_add_fetch(&rd->syncs, 1, __ATOMIC_RELAXED);
    return OK;
}

static const struct block_ops ops = {
    .info = op_info, .map_buffer = op_map_buffer, .read = op_read, .write = op_write,
    .sync = op_sync,
};

/* The serving thread: until the client is gone, `stop`, or pull_after
 * requests answered. */
static void serve(void *arg)
{
    struct ramdisk *rd = arg;
    uint32_t pull = __atomic_load_n(&rd->pull_after, __ATOMIC_RELAXED);
    for (;;) {
        status_t st = block_serve_one(rd->ch, &ops, rd);
        if (st == OK && pull && ++rd->served >= pull)
            break;   /* pulled right after that answer */
        if (st == OK)
            continue;
        if (st != ERR_SHOULD_WAIT || __atomic_load_n(&rd->stop, __ATOMIC_RELAXED))
            break;
        /* A request, the client's close or the poll's end: the loop looks again. */
        (void)jam_object_wait_one(rd->ch, SIG_READABLE | SIG_PEER_CLOSED, now() + POLL_NS, NULL);
    }
    jam_handle_close(rd->ch);
}

bool ramdisk_create(struct ramdisk *rd, uint64_t blocks)
{
    memset(rd, 0, offsetof(struct ramdisk, stack));
    uint64_t addr = 0;
    CHECK_ST(jam_vmo_create(blocks * RAMDISK_SECTOR, 0, &rd->mem_vmo), OK);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), rd->mem_vmo, 0, blocks * RAMDISK_SECTOR,
                          VMAR_READ | VMAR_WRITE, &addr), OK);
    rd->mem = (uint8_t *)(uintptr_t)addr;
    rd->blocks = blocks;
    return true;
}

bool ramdisk_serve(struct ramdisk *rd, bool read_only, handle_t *out_client)
{
    uint64_t addr = 0;
    rd->read_only = read_only;
    rd->buf_given = false;
    rd->served = 0;
    __atomic_store_n(&rd->stop, false, __ATOMIC_RELAXED);
    CHECK_ST(jam_vmo_create(RAMDISK_BUF, 0, &rd->buf_vmo), OK);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), rd->buf_vmo, 0, RAMDISK_BUF,
                          VMAR_READ | VMAR_WRITE, &addr), OK);
    rd->buf = (uint8_t *)(uintptr_t)addr;
    CHECK_ST(jam_channel_create(&rd->ch, out_client), OK);
    CHECK_ST(thread_spawn("ramdisk", serve, rd, rd->stack, sizeof(rd->stack), &rd->thread), OK);
    return true;
}

bool ramdisk_join(struct ramdisk *rd)
{
    if (!wait_threads(&rd->thread, 1))
        return false;
    CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)rd->buf,
                            RAMDISK_BUF), OK);
    CHECK_ST(jam_handle_close(rd->buf_vmo), OK);
    rd->buf = NULL;
    return true;
}

bool ramdisk_unplug(struct ramdisk *rd)
{
    __atomic_store_n(&rd->stop, true, __ATOMIC_RELAXED);
    return ramdisk_join(rd);
}

bool ramdisk_destroy(struct ramdisk *rd)
{
    CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)rd->mem,
                            rd->blocks * RAMDISK_SECTOR), OK);
    CHECK_ST(jam_handle_close(rd->mem_vmo), OK);
    rd->mem = NULL;
    return true;
}

uint32_t ramdisk_reads(const struct ramdisk *rd)
{
    return __atomic_load_n(&rd->reads, __ATOMIC_RELAXED);
}

uint32_t ramdisk_writes(const struct ramdisk *rd)
{
    return __atomic_load_n(&rd->writes, __ATOMIC_RELAXED);
}

uint32_t ramdisk_syncs(const struct ramdisk *rd)
{
    return __atomic_load_n(&rd->syncs, __ATOMIC_RELAXED);
}

void ramdisk_fail_writes(struct ramdisk *rd, bool on)
{
    __atomic_store_n(&rd->fail_writes, on, __ATOMIC_RELAXED);
}

void ramdisk_fail_after(struct ramdisk *rd, uint32_t writes)
{
    __atomic_store_n(&rd->fail_after, writes, __ATOMIC_RELAXED);
}

void ramdisk_on_write(struct ramdisk *rd,
                      void (*fn)(struct ramdisk *rd, uint64_t lba, uint32_t count))
{
    __atomic_store_n(&rd->on_write, fn, __ATOMIC_RELEASE);   /* op_write acquires it */
}

void ramdisk_pull_after(struct ramdisk *rd, uint32_t requests)
{
    __atomic_store_n(&rd->pull_after, requests, __ATOMIC_RELAXED);
}

void ramdisk_time_out(struct ramdisk *rd, uint32_t kinds, uint32_t n)
{
    __atomic_store_n(&rd->to_left, 0, __ATOMIC_RELAXED);   /* no request takes half of it */
    __atomic_store_n(&rd->to_kinds, kinds, __ATOMIC_RELAXED);
    __atomic_store_n(&rd->to_left, n, __ATOMIC_RELAXED);
}

uint32_t ramdisk_timeouts(const struct ramdisk *rd)
{
    return __atomic_load_n(&rd->timeouts, __ATOMIC_RELAXED);
}

void ramdisk_depth(struct ramdisk *rd, uint16_t depth)
{
    __atomic_store_n(&rd->depth, depth, __ATOMIC_RELAXED);
}

void ramdisk_hold_read(struct ramdisk *rd, bool until_gone)
{
    __atomic_store_n(&rd->hold, until_gone ? 2 : 1, __ATOMIC_RELAXED);
}

uint32_t ramdisk_late(const struct ramdisk *rd)
{
    return __atomic_load_n(&rd->late, __ATOMIC_RELAXED);
}

bool ramdisk_has_late(const struct ramdisk *rd)
{
    for (uint64_t s = 0; s < rd->blocks; s++) {
        const uint8_t *p = rd->mem + s * RAMDISK_SECTOR;
        uint32_t i = 0;
        while (i < RAMDISK_SECTOR && p[i] == RAMDISK_LATE)
            i++;
        if (i == RAMDISK_SECTOR)
            return true;
    }
    return false;
}
