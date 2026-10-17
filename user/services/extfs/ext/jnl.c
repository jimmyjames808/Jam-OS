/* ext's journal: the mount that recovers, and the life of the log while
 * the volume is mounted: where the head and the tail are, the checkpoint
 * after each commit, the tail's moves, the clean close. The transaction
 * itself (what a commit writes) is jnl_commit.c; the blocks and checksums
 * jnl_disk.c; the replay jnl_replay.c. ext_jnl.h has the model.
 *
 * The log's invariants, kept by everything here:
 *   - On the disk, s_start is 0 (empty) or the first block of the oldest
 *     transaction that may not be home, s_sequence its id. Every
 *     transaction from there to the head is whole or is the one being
 *     written, so a replay from s_start is right at any moment.
 *   - `clean` is where the head was after the last checkpoint that wrote
 *     everything home: the tail may move to it, never past it.
 *   - Ids only grow. After a replay the next id skips the one the scan
 *     stopped at, so a torn transaction left in the log can never be read
 *     as the start of a new one with the same id.
 *   - needs_recovery is set in the superblock on the disk for as long as
 *     the journal may hold anything (from the mount to the clean close). */
#include "jnl_int.h"

/* The live set's slots for a log of `blocks`: a power of two, twice it. */
static uint32_t live_slots(uint32_t blocks)
{
    uint32_t n = 16;
    while (n < 2 * blocks && n < (1u << 30))
        n <<= 1;
    return n;
}

size_t ext_jnl_mem_size(uint32_t block_size, uint32_t journal_blocks, uint32_t revokes)
{
    size_t stage = (size_t)EXT_JNL_STAGE_MAX * block_size;
    size_t rest = (size_t)live_slots(journal_blocks) * sizeof(uint64_t) +
                  (size_t)revokes * sizeof(struct ext_jnl_revoke);
    return 64 + 8 + (rest > stage ? stage + rest : 2 * stage);
}

status_t ext_jnl_init(struct ext_jnl *j, void *mem, size_t mem_size, uint32_t flags)
{
    if (!j || !mem || (flags & ~(uint32_t)EXT_JNL_ONE_WRITE))
        return ERR_INVALID_ARGS;
    __builtin_memset(j, 0, sizeof(*j));
    j->mem = mem;
    j->mem_size = mem_size;
    j->flags = flags;
    j->tail_every = EXT_JNL_TAIL_EVERY;
    j->tail_blocks = EXT_JNL_TAIL_BLOCKS;
    return OK;
}

/* Carve j's region for the volume's block size: up to half of it (and at
 * most EXT_JNL_STAGE_MAX blocks) stages log blocks, the rest holds revoke
 * records (until carve_live takes the live set from it). */
static status_t carve(struct ext_fs *fs, struct ext_jnl *j)
{
    uintptr_t p = ((uintptr_t)j->mem + 63) & ~(uintptr_t)63;
    size_t skip = p - (uintptr_t)j->mem;
    if (j->mem_size < skip + 8)
        return ERR_NO_MEMORY;
    size_t avail = j->mem_size - skip - 8;
    size_t n = avail / 2 / fs->block_size;
    if (n > EXT_JNL_STAGE_MAX)
        n = EXT_JNL_STAGE_MAX;
    size_t recs = (avail - n * fs->block_size) / sizeof(struct ext_jnl_revoke);
    if (n < EXT_JNL_STAGE_MIN || recs < 16)
        return ERR_NO_MEMORY;
    j->stage = (uint8_t *)p;
    j->nstage = (uint32_t)n;
    j->revokes = (struct ext_jnl_revoke *)(p + n * fs->block_size);
    j->cap_revokes = recs > UINT32_MAX ? UINT32_MAX : (uint32_t)recs;
    return OK;
}

/* The live set, from the front of the revoke area, for the log j has now. */
static status_t carve_live(struct ext_jnl *j)
{
    uint32_t slots = live_slots(j->maxlen);
    uint32_t take = (uint32_t)(((size_t)slots * sizeof(uint64_t) + sizeof(j->revokes[0]) - 1) /
                               sizeof(j->revokes[0]));
    if (j->cap_revokes < take + 16)
        return ERR_NO_MEMORY;
    j->live = (uint64_t *)(void *)j->revokes;
    j->cap_live = slots;
    j->revokes += take;
    j->cap_revokes -= take;
    jnl_live_clear(j);
    return OK;
}

void jnl_live_clear(struct ext_jnl *j)
{
    __builtin_memset(j->live, 0xFF, (size_t)j->cap_live * sizeof(uint64_t));
    j->nlive = 0;
}

static uint32_t live_slot(const struct ext_jnl *j, uint64_t blk)
{
    return (uint32_t)((blk * 0x9E3779B97F4A7C15ull) >> 32) & (j->cap_live - 1);
}

void jnl_live_add(struct ext_jnl *j, uint64_t blk)
{
    for (uint32_t s = live_slot(j, blk);; s = (s + 1) & (j->cap_live - 1)) {
        if (j->live[s] == blk)
            return;
        if (j->live[s] == EXT_NO_BLOCK) {
            j->live[s] = blk;
            j->nlive++;   /* at most the log's length: the set is twice that */
            return;
        }
    }
}

bool jnl_live_has(const struct ext_jnl *j, uint64_t blk)
{
    for (uint32_t s = live_slot(j, blk);; s = (s + 1) & (j->cap_live - 1)) {
        if (j->live[s] == blk)
            return true;
        if (j->live[s] == EXT_NO_BLOCK)
            return false;
    }
}

/* The journal can't be used: the volume is mounted read-only, saying why. */
static status_t refuse(struct ext_fs *fs, status_t st)
{
    if (!fs->why)
        fs->why = "journal: unusable";
    fs->ro |= st == ERR_NOT_SUPPORTED ? EXT_RO_FEATURES : EXT_RO_DAMAGED;
    return OK;
}

/* After a replay: the journal marked empty, the next id past the one the
 * scan stopped at. Written only once every copy is home (flushed first:
 * on a device with a cache the log must not be forgotten before what it
 * wrote home is on the medium). */
static status_t mark_empty(struct ext_fs *fs, struct ext_jnl *j, uint32_t next)
{
    status_t st = ext_flush(&fs->dev);
    if (st != OK)
        return st;
    j->sb.start = 0;
    j->sb.sequence = jbd2_be32(next);
    st = jnl_sb_write(fs, j);
    j->tail = 0;
    if (j->live)
        jnl_live_clear(j);
    return st;
}

/* The journal superblock as this library writes it: checksum v3, 64-bit
 * block numbers on a 64bit volume, revoke records, asynchronous commit
 * with EXT_JNL_ONE_WRITE. Changed only while the journal is empty. */
static status_t set_features(struct ext_fs *fs, struct ext_jnl *j)
{
    struct jbd2_super *sb = &j->sb;
    uint32_t incompat = JBD2_INCOMPAT_REVOKE | JBD2_INCOMPAT_CSUM_V3;
    if (fs->is64)
        incompat |= JBD2_INCOMPAT_64BIT;
    if (j->flags & EXT_JNL_ONE_WRITE)
        incompat |= JBD2_INCOMPAT_ASYNC_COMMIT;
    j->rec_bytes = incompat & JBD2_INCOMPAT_64BIT ? 8 : 4;
    if (jbd2_be32(sb->feature_incompat) == incompat && !sb->feature_compat &&
        sb->checksum_type == JBD2_CRC32C)
        return OK;
    sb->feature_compat = 0;
    sb->feature_incompat = jbd2_be32(incompat);
    sb->checksum_type = JBD2_CRC32C;
    return jnl_sb_write(fs, j);
}

/* Ready to write: the features, needs_recovery on the disk, the log's
 * place, fs->jnl. */
static status_t ready(struct ext_fs *fs, struct ext_jnl *j)
{
    status_t st = set_features(fs, j);
    if (st != OK)
        return st;
    if (!(fs->sb.feature_incompat & EXT_INCOMPAT_RECOVER)) {
        fs->sb.feature_incompat |= EXT_INCOMPAT_RECOVER;
        st = ext_super_write(fs);
        if (st == OK)
            st = ext_sync(fs);
        if (st != OK)
            return st;
    }
    j->head = j->clean = j->first;
    j->tid = j->clean_tid = j->tail_tid = j->mount_end + 1;
    j->tail = 0;
    j->since_tail = 0;
    j->nrevokes = 0;
    j->sb_before = fs->sb;
    fs->jnl = j;
    return OK;
}

/* Replay what the log holds; the journal marked empty after it. */
static status_t replay(struct ext_fs *fs, struct ext_jnl *j)
{
    if (!(jbd2_be32(j->sb.feature_incompat) & JBD2_INCOMPAT_CSUM_V3)) {
        fs->why = "journal: replay needs checksum v3";
        return ERR_NOT_SUPPORTED;
    }
    status_t st = jnl_replay(fs, j);
    return st == OK ? mark_empty(fs, j, j->mount_end + 1) : st;
}

/* Mount the volume again over what the replay wrote, keeping what j
 * learned. */
static status_t remount(struct ext_fs *fs, struct ext_jnl *j, const struct ext_dev *dev,
                        void *mem, size_t mem_size, uint32_t flags)
{
    struct jbd2_super sb = j->sb;
    uint32_t end = j->mount_end, n = j->replayed, blocks = j->replayed_blocks,
             revoked = j->revoked_blocks;
    status_t st = ext_mount(fs, dev, mem, mem_size, flags);
    j->sb = sb;
    j->mount_end = end;
    j->replayed = n;
    j->replayed_blocks = blocks;
    j->revoked_blocks = revoked;
    return st;
}

status_t ext_jnl_mount(struct ext_fs *fs, struct ext_jnl *j, const struct ext_dev *dev,
                       void *mem, size_t mem_size, uint32_t flags)
{
    if (!j || !j->mem)
        return ERR_INVALID_ARGS;
    status_t st = ext_mount(fs, dev, mem, mem_size, flags);
    j->replayed = j->replayed_blocks = j->revoked_blocks = 0;
    if (st != OK || !fs->has_journal || (fs->ro & ~(uint32_t)EXT_RO_RECOVER))
        return st;
    st = carve(fs, j);
    if (st == OK)
        st = jnl_load(fs, j);
    if (st == OK)
        st = carve_live(j);
    if (st == ERR_NO_MEMORY)
        return st;
    if (st != OK)
        return refuse(fs, st);
    j->mount_end = jbd2_be32(j->sb.sequence);
    if (j->sb.start) {
        st = replay(fs, j);
        if (st != OK) {
            fs->ro |= EXT_RO_RECOVER;   /* still needs it */
            return refuse(fs, st);
        }
        st = remount(fs, j, dev, mem, mem_size, flags);
        if (st != OK)
            return st;   /* what was replayed is damaged: as ext_mount says */
    }
    /* The journal is empty now: nothing is left to recover. */
    fs->needs_recovery = false;
    fs->ro &= ~(uint32_t)EXT_RO_RECOVER;
    if (fs->ro)
        return OK;
    st = ready(fs, j);
    return st == OK ? OK : refuse(fs, st);
}

bool ext_jnl_committed(const struct ext_fs *fs, uint32_t tid)
{
    const struct ext_jnl *j = fs->jnl;
    if (!j)
        return false;
    if (ext_jnl_after(j->mount_end, tid))
        return true;   /* before the mount's end: replayed, or home before it */
    return ext_jnl_after(tid, j->mount_end) && ext_jnl_after(j->tid, tid);
}

/* ---- the tail ----------------------------------------------------------------------------- */

/* Write the tail: s_start = pos (0: the journal is empty), s_sequence = tid. */
static status_t set_tail(struct ext_fs *fs, struct ext_jnl *j, uint32_t pos, uint32_t tid)
{
    j->sb.start = jbd2_be32(pos);
    j->sb.sequence = jbd2_be32(tid);
    status_t st = jnl_sb_write(fs, j);
    if (st != OK)
        return st;
    j->tail = pos;
    j->tail_tid = tid;
    j->since_tail = 0;
    if (pos == j->head)
        jnl_live_clear(j);   /* the log holds nothing before the head now */
    return OK;
}

/* Move the tail to `clean` (every transaction before it is home). On a
 * device with a cache, what the checkpoints wrote home is flushed first:
 * a tail that reached the medium before them would leave a cut nothing to
 * replay them from. */
static status_t move_tail(struct ext_fs *fs, struct ext_jnl *j)
{
    if (!j->tail || (j->tail == j->clean && j->tail_tid == j->clean_tid))
        return OK;
    status_t st = ext_flush(&fs->dev);
    return st == OK ? set_tail(fs, j, j->clean, j->clean_tid) : st;
}

/* Log blocks free for the next transaction (one is kept between the head
 * and the tail, so a full log never looks empty). */
static uint32_t log_free(const struct ext_jnl *j)
{
    uint32_t len = j->maxlen - j->first;
    uint32_t used = j->tail ? jnl_dist(j, j->tail, j->head) : 0;
    return len - 1 - used;
}

status_t jnl_make_room(struct ext_fs *fs, struct ext_jnl *j, uint32_t blocks)
{
    if (blocks > log_free(j)) {
        status_t st = move_tail(fs, j);
        if (st != OK)
            return st;
    }
    if (blocks > log_free(j))
        return ERR_NO_RESOURCES;
    /* Empty on the disk: point s_start at this transaction before it is
     * written, so its commit block is all it needs to count. */
    return j->tail ? OK : set_tail(fs, j, j->head, j->tid);
}

void jnl_committed(struct ext_fs *fs, struct ext_jnl *j, uint32_t head)
{
    (void)fs;
    j->head = head;
    j->tid++;
    j->since_tail++;
    j->nrevokes = 0;
}

status_t jnl_checkpointed(struct ext_fs *fs, struct ext_jnl *j)
{
    j->clean = j->head;
    j->clean_tid = j->tid;
    j->sb_before = fs->sb;
    if (j->since_tail >= j->tail_every ||
        (j->tail && jnl_dist(j, j->tail, j->head) >= j->tail_blocks))
        return move_tail(fs, j);
    return OK;
}

status_t ext_jnl_checkpoint(struct ext_fs *fs)
{
    struct ext_jnl *j = fs->jnl;
    if (!j)
        return ERR_BAD_STATE;
    status_t st = move_tail(fs, j);
    return st == OK ? OK : ext_damaged(fs, "journal: tail write failed", 0, 0);
}

status_t ext_jnl_close(struct ext_fs *fs)
{
    struct ext_jnl *j = fs->jnl;
    if (!j)
        return ERR_BAD_STATE;
    if (fs->cache.ndirty || j->nrevokes)
        return ERR_BAD_STATE;
    fs->jnl = NULL;
    if (fs->ro)
        return OK;
    status_t st = mark_empty(fs, j, j->tid);
    if (st != OK)
        return st;
    fs->sb.feature_incompat &= ~(uint32_t)EXT_INCOMPAT_RECOVER;
    st = ext_super_write(fs);
    if (st == OK)
        st = ext_sync(fs);
    return st == OK ? ext_flush(&fs->dev) : st;
}
