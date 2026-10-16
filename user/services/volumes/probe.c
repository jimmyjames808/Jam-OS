/* volumes: what a partition holds, for the ones that may hold ext: its
 * first sectors read for the ext superblock (docs/M13.6-PLAN.md's U7a),
 * and the zeros a format of a Jam OS drive's data partition starts with
 * (U7b).
 *
 * A partition whose type can hold ext (MBR 0x83, GPT's Linux filesystem
 * type, Jam OS's data type) and the boot disk's data partition get a
 * service by what is on them, not by their type: bin/extfs on a partition
 * with an ext superblock, bin/fat on the others (fsvc.c decides; a blank
 * one, its first 4 KiB all zeros, is extfs's to format where a format is
 * allowed). The look is a read-only `block` channel of its own, closed
 * again before the service is started, so it can't write whatever the
 * partition holds, and the service gets a channel opened the way its mount
 * is (read-only for someone else's volume).
 *
 * It runs in the start's task and never waits for a disk inside the loop:
 * each call to the disk's driver is written without waiting and its answer
 * looked for every POLL (a stick that stalls costs PROBE_WAIT for this
 * partition, and volumes goes on serving meanwhile). The channel is
 * volumes' alone, so nothing else reads it; there is no packet to wait for,
 * hence the polling. Only the superblock's magic is looked at: whether
 * extfs can serve the volume (its features, its state) is extfs's to say,
 * and a volume it refuses is left alone like a FAT one fat refuses.
 *
 * The wipe (part_wipe, prepare.c's format of a data partition, after the
 * owner's confirmation) is the same kind of channel opened writable: the
 * partition's first WIPE_BYTES written with zeros, so the volume that was
 * there is gone and the partition is blank for the format that follows.
 * The driver decides whether it may write there at all (a Jam OS drive's
 * partitions only). */
#include <idl/block.h>
#include <idl/storage.h>
#include <jam/parttable.h>
#include "disk.h"

#define PROBE_WAIT (5 * NS_PER_S)    /* each call: the disk answered storage.info already */
#define POLL       (2 * NS_PER_MS)
#define PROBE_BUF  8192u             /* the shared buffer asked for: HEAD_BYTES fit */
#define HEAD_BYTES 4096u             /* read from the start: the superblock, and the blank rule */
#define SB_OFFSET  1024u             /* the superblock's place in the volume */
#define SB_MAGIC_AT 56u              /* s_magic: 0xEF53, little-endian */
#define WIPE_BUF   (64u << 10)       /* the wipe's shared buffer: zeros, written over and over */
#define WIPE_BYTES PT_BLANK_SPOT_BYTES   /* what a wipe zeroes: as an erase does where a
                                          * partition starts (drivers/nvme/table.c) */

/* In the task: the reply to txid on ch into rep and *m, or why not. */
static status_t reply(handle_t ch, uint32_t txid, void *rep, uint32_t cap, struct idl_msg *m)
{
    uint64_t deadline = now() + PROBE_WAIT;
    for (;;) {
        status_t st = idl_reply_read(ch, rep, cap, m);
        if (st == OK) {
            if (m->txid == txid)
                return OK;
            idl_msg_drop(m);   /* not the one we wait for */
            continue;
        }
        if (st == ERR_INTERNAL)
            continue;   /* too big for any answer of block's: dropped */
        if (st != ERR_SHOULD_WAIT)
            return st;   /* ERR_PEER_CLOSED: the disk went */
        uint64_t t = now();
        if (t >= deadline)
            return ERR_TIMED_OUT;
        task_wait(tasks, t + POLL < deadline ? t + POLL : deadline);
    }
}

/* Is the volume's primary superblock in sb? */
static bool has_magic(const uint8_t *sb)
{
    return sb[SB_MAGIC_AT] == 0x53 && sb[SB_MAGIC_AT + 1] == 0xef;
}

/* blk's block size and blocks, its shared buffer asked for (`want` bytes)
 * and mapped (perms) at *addr, *size bytes of it. */
static status_t open_buffer(handle_t blk, uint32_t want, uint32_t perms, uint32_t *bs,
                            uint64_t *blocks, uint32_t *maxb, uint64_t *addr, uint32_t *size)
{
    _Alignas(8) uint8_t rep[CALL_REP_MAX];
    struct idl_msg m;
    handle_t vmo = HANDLE_INVALID;
    uint32_t txid = call_txid();
    status_t st = block_info_send(blk, txid);
    if (st == OK)
        st = reply(blk, txid, rep, sizeof(rep), &m);
    if (st == OK)
        st = block_info_result(rep, &m, bs, blocks, NULL, maxb, NULL, NULL);
    if (st != OK)
        return st;
    if (*bs < 512 || *bs > 4096 || (*bs & (*bs - 1)))
        return ERR_NOT_SUPPORTED;   /* a size no ext volume has (extfs reads 512 to 4096) */
    txid = call_txid();
    st = block_map_buffer_send(blk, txid, want);
    if (st == OK)
        st = reply(blk, txid, rep, sizeof(rep), &m);
    if (st == OK)
        st = block_map_buffer_result(rep, &m, &vmo, size);
    if (st != OK)
        return st;
    *size -= *size % PAGE_SIZE;
    st = *size >= HEAD_BYTES ? jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, *size, perms, addr)
                             : ERR_INVALID_ARGS;
    jam_handle_close(vmo);
    return st;
}

/* The `block` channel blk's partition read for the superblock and the
 * blank rule: *what. */
static status_t look(handle_t blk, enum part_holds *what)
{
    _Alignas(8) uint8_t rep[CALL_REP_MAX];
    struct idl_msg m;
    uint32_t bs = 0, size = 0, maxb = 0;
    uint64_t blocks = 0, addr = 0;
    *what = PART_HOLDS_OTHER;
    status_t st = open_buffer(blk, PROBE_BUF, VMAR_READ, &bs, &blocks, &maxb, &addr, &size);
    if (st == ERR_NOT_SUPPORTED)
        return OK;   /* no ext volume, and no format of one */
    if (st != OK)
        return st;
    if (blocks * bs >= HEAD_BYTES) {
        uint32_t txid = call_txid();
        st = block_read_send(blk, txid, 0, HEAD_BYTES / bs, 0);
        if (st == OK)
            st = reply(blk, txid, rep, sizeof(rep), &m);
        if (st == OK)
            st = block_read_result(rep, &m);
    }
    const uint8_t *head = (const uint8_t *)(uintptr_t)addr;
    if (st == OK && blocks * bs >= HEAD_BYTES) {
        bool zeros = true;
        for (uint32_t i = 0; i < HEAD_BYTES && zeros; i++)
            zeros = head[i] == 0;
        *what = has_magic(head + SB_OFFSET) ? PART_HOLDS_EXT
                : zeros                     ? PART_HOLDS_BLANK
                                            : PART_HOLDS_OTHER;
    }
    jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, size);
    return st;
}

status_t fs_probe(struct vol *b, enum part_holds *what)
{
    handle_t blk = HANDLE_INVALID;
    uint32_t serial = b->serial;
    *what = PART_HOLDS_OTHER;
    status_t st = fs_open_block(b, true, &blk);   /* read-only, whatever the mount will be */
    if (st != OK)
        return st;
    st = look(blk, what);
    jam_handle_close(blk);
    if (b->serial != serial || stopping)
        return ERR_PEER_CLOSED;   /* b went meanwhile */
    return st;
}

/* blk's first WIPE_BYTES (as much as the partition has) written with zeros
 * from the shared buffer, as many blocks a call as the buffer and the
 * driver take. */
static status_t wipe(handle_t blk)
{
    _Alignas(8) uint8_t rep[CALL_REP_MAX];
    struct idl_msg m;
    uint32_t bs = 0, size = 0, maxb = 0;
    uint64_t blocks = 0, addr = 0;
    status_t st = open_buffer(blk, WIPE_BUF, VMAR_READ | VMAR_WRITE, &bs, &blocks, &maxb, &addr,
                              &size);
    if (st != OK)
        return st;
    memset((void *)(uintptr_t)addr, 0, size);
    uint64_t total = WIPE_BYTES / bs < blocks ? WIPE_BYTES / bs : blocks;
    uint32_t per = size / bs;
    if (maxb && per > maxb)
        per = maxb;
    for (uint64_t lba = 0; lba < total && st == OK;) {
        uint32_t count = total - lba < per ? (uint32_t)(total - lba) : per;
        uint32_t txid = call_txid();
        st = block_write_send(blk, txid, lba, count, 0);
        if (st == OK)
            st = reply(blk, txid, rep, sizeof(rep), &m);
        if (st == OK)
            st = block_write_result(rep, &m);
        lba += count;
    }
    jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, size);
    return st;
}

status_t part_wipe(struct disk *d, unsigned part)
{
    if (!d->ch || d->state == DISK_FREE || d->state == DISK_DOWN)
        return ERR_PEER_CLOSED;
    uint16_t gen = d->gen;
    handle_t blk = HANDLE_INVALID;
    struct call c;
    call_begin(&c, d->ch);
    status_t st = storage_open_partition_send(d->ch, c.txid, part, 0);   /* writable */
    if (st != OK) {
        call_end(&c);
        return st;
    }
    st = call_wait(&c, now() + CALL_WAIT);
    if (st == OK)
        st = storage_open_partition_result(c.rep, &c.m, &blk);
    if (st != OK)
        return st;
    st = d->gen == gen && !stopping ? wipe(blk) : ERR_PEER_CLOSED;
    jam_handle_close(blk);
    return st;
}
