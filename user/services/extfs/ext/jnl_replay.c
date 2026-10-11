/* ext's journal: recovery. The log is walked three times from the
 * journal superblock's s_start, each walk following the transactions'
 * ids from s_sequence:
 *
 *   1. the scan finds where the log ends: the first transaction that is
 *      not whole. Every block is checked before anything is believed: a
 *      header with the id expected, the descriptor's and revoke block's
 *      checksums, every tag's target inside the volume and outside the
 *      journal, every copy's checksum against its tag, the commit block's
 *      checksum. The first block that fails ends the log before its
 *      transaction, so a transaction is replayed whole or not at all
 *      (stricter than Linux, which checks a copy only as it replays it;
 *      the same answer for every log this library writes in its default
 *      mode: ext_jnl.h);
 *   2. the revoke records of the whole transactions go into a hash table
 *      in the journal's region, each block with the latest id that
 *      revoked it;
 *   3. each whole transaction's copies are written home in log order,
 *      except a copy of a block revoked by the same or a later
 *      transaction.
 *
 * Every walk is bounded by the log's length: a log that comes round to
 * where it began ends there. The replay writes with the device, not the
 * cache; ext_jnl_mount mounts the volume again over what it wrote. */
#include "jnl_int.h"

enum pass { SCAN, REVOKES, REPLAY };

/* A walk's place: the log block it is at, the id it expects, the blocks
 * it has read (bounded by the log's length), and where the scan ended. */
struct walk {
    enum pass pass;
    uint32_t pos;
    uint32_t tid;
    uint32_t budget;
    uint32_t end_tid;     /* REVOKES and REPLAY stop at this id (the scan's answer) */
    bool stop;            /* the log ends here */
    uint64_t digest;      /* the scan: the transaction's blocks so far (jnl_int.h) */
};

/* ---- the revoke table ------------------------------------------------------------------ */

static uint32_t slot_of(const struct ext_jnl *j, uint64_t blk)
{
    return (uint32_t)((blk * 0x9E3779B97F4A7C15ull) >> 32) % j->cap_revokes;
}

/* Record that blk is revoked by transaction tid (the latest id wins). */
static status_t revoke_add(struct ext_fs *fs, struct ext_jnl *j, uint64_t blk, uint32_t tid)
{
    uint32_t s = slot_of(j, blk);
    for (uint32_t n = 0; n < j->cap_revokes; n++, s = s + 1 == j->cap_revokes ? 0 : s + 1) {
        struct ext_jnl_revoke *r = &j->revokes[s];
        if (r->used && r->blk != blk)
            continue;
        if (!r->used || ext_jnl_after(tid, r->tid))
            *r = (struct ext_jnl_revoke){ .blk = blk, .tid = tid, .used = 1 };
        return OK;
    }
    fs->why = "journal: more revoke records than its memory holds";
    return ERR_NO_RESOURCES;
}

/* Is a copy of blk in transaction tid revoked (by tid or a later one)? */
static bool revoked(const struct ext_jnl *j, uint64_t blk, uint32_t tid)
{
    uint32_t s = slot_of(j, blk);
    for (uint32_t n = 0; n < j->cap_revokes; n++, s = s + 1 == j->cap_revokes ? 0 : s + 1) {
        const struct ext_jnl_revoke *r = &j->revokes[s];
        if (!r->used)
            return false;
        if (r->blk == blk)
            return !ext_jnl_after(tid, r->tid);
    }
    return false;
}

/* ---- the blocks of a transaction -------------------------------------------------------- */

/* Read the walk's next log block into buf. */
static status_t step_read(struct ext_fs *fs, struct ext_jnl *j, struct walk *w, uint8_t *buf)
{
    if (!w->budget) {
        w->stop = true;   /* round the whole log: it ends here */
        return OK;
    }
    w->budget--;
    status_t st = jnl_read(fs, j, w->pos, buf);
    w->pos = jnl_next(j, w->pos);
    return st;
}

/* Is the journal's run [pblk] one of its own blocks? */
static bool in_journal(const struct ext_jnl *j, uint64_t blk)
{
    for (uint32_t i = 0; i < j->nruns; i++)
        if (blk - j->runs[i].pblk < j->runs[i].len)
            return true;
    return false;
}

/* One tag of a descriptor: its copy read (the scan checks it, the replay
 * writes it home). */
static status_t do_tag(struct ext_fs *fs, struct ext_jnl *j, struct walk *w,
                       const struct jbd2_tag3 *t)
{
    uint8_t *copy = j->stage + fs->block_size;
    uint32_t flags = jbd2_be32(t->flags);
    uint64_t blk = jbd2_be32(t->blocknr);
    if (j->rec_bytes == 8)
        blk |= (uint64_t)jbd2_be32(t->blocknr_high) << 32;
    if (w->pass == REVOKES) {   /* nothing to read: skip the copy */
        w->pos = jnl_next(j, w->pos);
        return OK;
    }
    status_t st = step_read(fs, j, w, copy);
    if (st != OK || w->stop)
        return st;
    if (w->pass == SCAN) {
        w->digest = jnl_digest(w->digest, copy, fs->block_size);
        w->stop = blk >= fs->blocks || in_journal(j, blk) ||
                  jbd2_be32(t->checksum) != jnl_tag_csum(fs, j, w->tid, copy, false);
        return OK;
    }
    if (revoked(j, blk, w->tid)) {
        j->revoked_blocks++;
        return OK;
    }
    if (flags & JBD2_FLAG_ESCAPE) {
        uint32_t magic = jbd2_be32(JBD2_MAGIC);
        __builtin_memcpy(copy, &magic, 4);
    }
    if (fs->dev.write(fs->dev.ctx, blk << fs->block_log, fs->block_size, copy) != OK)
        return ext_damaged(fs, "journal: replay write failed", 0, blk);
    j->replayed_blocks++;
    return OK;
}

/* A descriptor block (in the first staging block): its tags, each with its
 * copy. The scan stops at a tag that runs past the block's room. */
static status_t do_descriptor(struct ext_fs *fs, struct ext_jnl *j, struct walk *w)
{
    const uint8_t *d = j->stage;
    if (w->pass == SCAN && !jnl_tail_ok(fs, j, d)) {
        w->stop = true;
        return OK;
    }
    uint32_t at = sizeof(struct jbd2_header);
    uint32_t end = fs->block_size - (uint32_t)sizeof(struct jbd2_tail);
    for (;;) {
        if (at + j->tag_bytes > end) {
            w->stop |= w->pass == SCAN;   /* no last tag: not a descriptor this library reads */
            return OK;
        }
        struct jbd2_tag3 t;
        __builtin_memcpy(&t, d + at, sizeof(t));
        uint32_t flags = jbd2_be32(t.flags);
        at += j->tag_bytes + (flags & JBD2_FLAG_SAME_UUID ? 0 : JBD2_UUID_SIZE);
        if (at > end) {
            w->stop |= w->pass == SCAN;
            return OK;
        }
        status_t st = do_tag(fs, j, w, &t);
        if (st != OK || w->stop || (flags & JBD2_FLAG_LAST_TAG))
            return st;
    }
}

/* A revoke block (in the first staging block): checked by the scan, its
 * records taken by the revoke walk. */
static status_t do_revoke(struct ext_fs *fs, struct ext_jnl *j, struct walk *w)
{
    const uint8_t *b = j->stage;
    const struct jbd2_revoke_header *h = (const void *)b;
    uint32_t count = jbd2_be32(h->count);
    if (w->pass == SCAN) {
        w->stop = !jnl_tail_ok(fs, j, b) || count < JBD2_REVOKE_FIRST ||
                  count > fs->block_size - sizeof(struct jbd2_tail) ||
                  (count - JBD2_REVOKE_FIRST) % j->rec_bytes;
        return OK;
    }
    if (w->pass != REVOKES)
        return OK;
    for (uint32_t at = JBD2_REVOKE_FIRST; at + j->rec_bytes <= count; at += j->rec_bytes) {
        uint64_t blk;
        if (j->rec_bytes == 8) {
            __builtin_memcpy(&blk, b + at, 8);
            blk = jbd2_be64(blk);
        } else {
            uint32_t v;
            __builtin_memcpy(&v, b + at, 4);
            blk = jbd2_be32(v);
        }
        status_t st = revoke_add(fs, j, blk, w->tid);
        if (st != OK)
            return st;
    }
    return OK;
}

/* A commit block (in the first staging block): its checksum, and its
 * digest of the transaction when it carries one (this library's always
 * do; Linux's never). */
static bool commit_ok(struct ext_fs *fs, struct ext_jnl *j, const struct walk *w)
{
    const struct jbd2_commit *c = (const void *)j->stage;
    if (!jnl_commit_ok(fs, j, c))
        return false;
    if (jbd2_be32(c->chksum[3]) != JNL_DIGEST_MARK)
        return true;
    uint64_t d = (uint64_t)jbd2_be32(c->chksum[1]) << 32 | jbd2_be32(c->chksum[2]);
    return d == w->digest;
}

/* One block of the walk, by what its header says. */
static status_t do_block(struct ext_fs *fs, struct ext_jnl *j, struct walk *w)
{
    status_t st = step_read(fs, j, w, j->stage);
    if (st != OK || w->stop)
        return st;
    const struct jbd2_header *h = (const void *)j->stage;
    if (jbd2_be32(h->magic) != JBD2_MAGIC || jbd2_be32(h->sequence) != w->tid) {
        w->stop = true;
        return OK;
    }
    uint32_t type = jbd2_be32(h->blocktype);
    if (w->pass == SCAN && type != JBD2_COMMIT)
        w->digest = jnl_digest(w->digest, j->stage, fs->block_size);
    switch (type) {
    case JBD2_DESCRIPTOR:
        return do_descriptor(fs, j, w);
    case JBD2_REVOKE:
        return do_revoke(fs, j, w);
    case JBD2_COMMIT:
        if (w->pass == SCAN && !commit_ok(fs, j, w)) {
            w->stop = true;
            return OK;
        }
        w->tid++;
        w->digest = jnl_digest_start(j, w->tid);
        if (w->pass == SCAN)
            j->replayed++;
        return OK;
    default:
        w->stop = true;
        return OK;
    }
}

/* Walk the log from s_start: the scan until it stops, the other passes
 * until the scan's end. */
static status_t walk(struct ext_fs *fs, struct ext_jnl *j, enum pass pass, uint32_t end_tid,
                     uint32_t *out_tid)
{
    struct walk w = {
        .pass = pass,
        .pos = jbd2_be32(j->sb.start),
        .tid = jbd2_be32(j->sb.sequence),
        .budget = j->maxlen - j->first,
        .end_tid = end_tid,
        .digest = jnl_digest_start(j, jbd2_be32(j->sb.sequence)),
    };
    status_t st = OK;
    while (st == OK && !w.stop && (pass == SCAN || w.tid != w.end_tid))
        st = do_block(fs, j, &w);
    *out_tid = w.tid;
    return st;
}

status_t jnl_replay(struct ext_fs *fs, struct ext_jnl *j)
{
    uint32_t end, unused;
    j->replayed = j->replayed_blocks = j->revoked_blocks = 0;
    status_t st = walk(fs, j, SCAN, 0, &end);
    if (st != OK)
        return st;
    j->mount_end = end;
    if (end == jbd2_be32(j->sb.sequence))
        return OK;
    __builtin_memset(j->revokes, 0, (size_t)j->cap_revokes * sizeof(j->revokes[0]));
    st = walk(fs, j, REVOKES, end, &unused);
    if (st == OK)
        st = walk(fs, j, REPLAY, end, &unused);
    return st;
}
