/* volumes: what a partition holds, for the ones that may hold ext: its
 * first sectors read for the ext superblock (docs/M13.6-PLAN.md's U7a).
 *
 * A partition whose type can hold ext (MBR 0x83, GPT's Linux filesystem
 * type, Jam OS's data type) and the boot disk's data partition get a
 * service by what is on them, not by their type: bin/extfs on a partition
 * with an ext superblock, bin/fat on the others (fsvc.c decides). The look
 * is a read-only `block` channel of its own, closed again before the
 * service is started, so it can't write whatever the partition holds, and
 * the service gets a channel opened the way its mount is (read-only for
 * someone else's volume).
 *
 * It runs in the start's task and never waits for a disk inside the loop:
 * each call to the disk's driver is written without waiting and its answer
 * looked for every POLL (a stick that stalls costs PROBE_WAIT for this
 * partition, and volumes goes on serving meanwhile). The channel is
 * volumes' alone, so nothing else reads it; there is no packet to wait for,
 * hence the polling. Only the superblock's magic is looked at: whether
 * extfs can serve the volume (its features, its state) is extfs's to say,
 * and a volume it refuses is left alone like a FAT one fat refuses. */
#include <idl/block.h>
#include "disk.h"

#define PROBE_WAIT (5 * NS_PER_S)    /* each call: the disk answered storage.info already */
#define POLL       (2 * NS_PER_MS)
#define PROBE_BUF  8192u             /* the shared buffer asked for: a superblock fits */
#define SB_OFFSET  1024u             /* the superblock's place in the volume */
#define SB_SIZE    1024u
#define SB_MAGIC_AT 56u              /* s_magic: 0xEF53, little-endian */

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

/* Is the volume's primary superblock in sb (SB_SIZE bytes)? */
static bool has_magic(const uint8_t *sb)
{
    return sb[SB_MAGIC_AT] == 0x53 && sb[SB_MAGIC_AT + 1] == 0xef;
}

/* The `block` channel blk's partition read for the superblock: *is_ext. */
static status_t look(handle_t blk, bool *is_ext)
{
    _Alignas(8) uint8_t rep[CALL_REP_MAX];
    struct idl_msg m;
    uint32_t bs = 0, size = 0;
    uint64_t blocks = 0;
    handle_t vmo = HANDLE_INVALID;
    uint32_t txid = call_txid();
    status_t st = block_info_send(blk, txid);
    if (st == OK)
        st = reply(blk, txid, rep, sizeof(rep), &m);
    if (st == OK)
        st = block_info_result(rep, &m, &bs, &blocks, NULL, NULL, NULL, NULL);
    if (st != OK)
        return st;
    if (bs < 512 || bs > 4096 || (bs & (bs - 1)) || blocks * bs < SB_OFFSET + SB_SIZE) {
        *is_ext = false;   /* a size no ext volume has (extfs reads 512 to 4096) */
        return OK;
    }
    txid = call_txid();
    st = block_map_buffer_send(blk, txid, PROBE_BUF);
    if (st == OK)
        st = reply(blk, txid, rep, sizeof(rep), &m);
    if (st == OK)
        st = block_map_buffer_result(rep, &m, &vmo, &size);
    if (st != OK)
        return st;
    size -= size % PAGE_SIZE;
    uint64_t addr = 0;
    st = size >= SB_OFFSET + SB_SIZE + bs
             ? jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size, VMAR_READ, &addr)
             : ERR_INVALID_ARGS;
    jam_handle_close(vmo);
    if (st != OK)
        return st;
    uint32_t skip = SB_OFFSET % bs, first = SB_OFFSET / bs;
    uint32_t count = (skip + SB_SIZE + bs - 1) / bs;
    txid = call_txid();
    st = block_read_send(blk, txid, first, count, 0);
    if (st == OK)
        st = reply(blk, txid, rep, sizeof(rep), &m);
    if (st == OK)
        st = block_read_result(rep, &m);
    if (st == OK)
        *is_ext = has_magic((const uint8_t *)(uintptr_t)addr + skip);
    jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, size);
    return st;
}

status_t fs_probe_ext(struct vol *b, bool *is_ext)
{
    handle_t blk = HANDLE_INVALID;
    uint32_t serial = b->serial;
    *is_ext = false;
    status_t st = fs_open_block(b, true, &blk);   /* read-only, whatever the mount will be */
    if (st != OK)
        return st;
    st = look(blk, is_ext);
    jam_handle_close(blk);
    if (b->serial != serial || stopping)
        return ERR_PEER_CLOSED;   /* b went meanwhile */
    return st;
}
