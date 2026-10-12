/* ext's journal: the declarations its files (jnl.c, jnl_disk.c,
 * jnl_replay.c) share with each other and with no one else. */
#pragma once

#include "ext_int.h"
#include "ext_jnl.h"

/* ---- jnl_disk.c: the journal's blocks ------------------------------------------------- */

/* Map the journal's inode into j->runs and read and check its superblock
 * into j->sb, filling j's shape (maxlen, first, seed, tag and record
 * sizes). ERR_IO with fs->why for a journal that can't be used (the
 * caller marks the volume); ERR_NOT_SUPPORTED for one this library
 * neither replays nor writes (its features). */
status_t jnl_load(struct ext_fs *fs, struct ext_jnl *j);

/* Write j->sb to log block 0, its checksum set (with checksum v2 or v3). */
status_t jnl_sb_write(struct ext_fs *fs, struct ext_jnl *j);

/* Read log block `pos` into buf (block_size bytes). */
status_t jnl_read(struct ext_fs *fs, struct ext_jnl *j, uint32_t pos, void *buf);
/* Write n log blocks from pos on (none past the log's end: the caller
 * splits at the wrap), from buf. */
status_t jnl_write(struct ext_fs *fs, struct ext_jnl *j, uint32_t pos, uint32_t n,
                   const void *buf);

/* The log block after pos, wrapping from the last to the first. */
static inline uint32_t jnl_next(const struct ext_jnl *j, uint32_t pos)
{
    return pos + 1 >= j->maxlen ? j->first : pos + 1;
}
/* Log blocks from a to b going forward (0 when they are the same). */
static inline uint32_t jnl_dist(const struct ext_jnl *j, uint32_t a, uint32_t b)
{
    return b >= a ? b - a : (j->maxlen - a) + (b - j->first);
}

/* Checksum v3: a descriptor's or revoke block's tail, set and checked. */
void jnl_tail_set(const struct ext_fs *fs, const struct ext_jnl *j, void *block);
bool jnl_tail_ok(const struct ext_fs *fs, const struct ext_jnl *j, const void *block);
/* The commit block's checksum, set and checked. */
void jnl_commit_set(const struct ext_fs *fs, const struct ext_jnl *j, void *block);
bool jnl_commit_ok(const struct ext_fs *fs, const struct ext_jnl *j, const void *block);
/* A tag's checksum of `copy` (the block as it is in the log) in
 * transaction tid; `escaped`: its first word is written as 0 although
 * copy holds the magic there. */
uint32_t jnl_tag_csum(const struct ext_fs *fs, const struct ext_jnl *j, uint32_t tid,
                      const uint8_t *copy, bool escaped);

/* The transaction digest (ext_jnl.h): a commit block whose h_chksum[3] is
 * JNL_DIGEST_MARK carries in h_chksum[1] and [2] (high and low halves) a
 * digest of every block of its transaction before it, in log order, as in
 * the log. Not CRC32C: CRC32C is blind to a block that ends in its own
 * CRC32C being swapped for another version of itself (ext_jnl.h). */
#define JNL_DIGEST_MARK 0x4A414D44u   /* "JAMD" */
static inline uint64_t jnl_digest_start(const struct ext_jnl *j, uint32_t tid)
{
    return ((uint64_t)j->seed << 32 | tid) ^ 0x9E3779B97F4A7C15ull;
}
/* The digest h carried over one more block. */
uint64_t jnl_digest(uint64_t h, const void *block, uint32_t size);

/* Does a block's first word read as jbd2's magic (so its copy is escaped)? */
static inline bool jnl_magic_at(const uint8_t *block)
{
    uint32_t w;
    __builtin_memcpy(&w, block, 4);
    return jbd2_be32(w) == JBD2_MAGIC;
}

/* A header's fields (big-endian on the disk). */
static inline void jnl_header_set(void *block, uint32_t type, uint32_t tid)
{
    struct jbd2_header *h = block;
    h->magic = jbd2_be32(JBD2_MAGIC);
    h->blocktype = jbd2_be32(type);
    h->sequence = jbd2_be32(tid);
}

/* Tags a descriptor block holds: the first followed by the UUID, the
 * rest not; all before the tail. */
static inline uint32_t jnl_tags_per_desc(const struct ext_fs *fs, const struct ext_jnl *j)
{
    uint32_t room = fs->block_size - (uint32_t)sizeof(struct jbd2_header) -
                    (uint32_t)sizeof(struct jbd2_tail) - JBD2_UUID_SIZE;
    return room / j->tag_bytes;
}
/* Records a revoke block holds. */
static inline uint32_t jnl_recs_per_revoke(const struct ext_fs *fs, const struct ext_jnl *j)
{
    return (fs->block_size - JBD2_REVOKE_FIRST - (uint32_t)sizeof(struct jbd2_tail)) /
           j->rec_bytes;
}

/* ---- jnl_replay.c: recovery ----------------------------------------------------------- */

/* Replay the log from j->sb's s_start (non-zero): every whole transaction,
 * revoked copies skipped, each copy written home with the device (the
 * cache is not used: the caller mounts the volume again after). Sets
 * j->mount_end (the first id not committed) and the counts. ERR_IO with
 * fs->why if the device fails; ERR_NO_RESOURCES if the revoke records
 * don't fit j's region. */
status_t jnl_replay(struct ext_fs *fs, struct ext_jnl *j);

/* ---- jnl.c: the live set ---------------------------------------------------------------- */

/* The blocks with a copy in the log since the tail (cleared when the tail
 * reaches the head: nothing older is left to replay). */
void jnl_live_clear(struct ext_jnl *j);
void jnl_live_add(struct ext_jnl *j, uint64_t blk);
bool jnl_live_has(const struct ext_jnl *j, uint64_t blk);

/* ---- jnl.c: the log, for jnl_commit.c ------------------------------------------------- */

/* Room in the log for a transaction of `blocks` log blocks: the tail moved
 * if it must be, and s_start pointed at the head if the journal is empty
 * on the disk. ERR_NO_RESOURCES: not even then; ERR_IO. */
status_t jnl_make_room(struct ext_fs *fs, struct ext_jnl *j, uint32_t blocks);
/* The running transaction is on the disk up to `head`: the next id, its
 * revokes done. */
void jnl_committed(struct ext_fs *fs, struct ext_jnl *j, uint32_t head);
/* ... and home: the tail may move to the head; moved if it is due. */
status_t jnl_checkpointed(struct ext_fs *fs, struct ext_jnl *j);
