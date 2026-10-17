/* ext's journal: a transaction. It collects in the block cache (every
 * dirty buffer) and in the journal's revoke list; a commit writes it to
 * the log at the head:
 *
 *     revoke blocks (the revoked blocks, sorted), then for each group of
 *     dirty buffers a descriptor (their tags) and their copies, then the
 *     commit block
 *
 * through the staging blocks, one device write per run of staged blocks
 * (a run ends where the staging blocks are full or the log wraps). By
 * default the commit block goes in a write of its own after every other
 * block is on the disk; with EXT_JNL_ONE_WRITE it goes with them. Then
 * the checkpoint writes the dirty buffers home (ext_sync).
 *
 * A copy whose first word is jbd2's magic is written with 0 there and its
 * tag says so (escaped), or a replay would read it as a log block of its
 * own. Every tag's checksum is computed from the buffer as the copy will
 * lie in the log, so the descriptor is made before its copies are staged.
 *
 * The cache is walked in its own order (dirty buffers by index), twice per
 * descriptor: once for the tags, once for the copies; nothing changes the
 * cache in between. */
#include "jnl_int.h"

/* Staged log blocks on their way to the device: `n` blocks for log
 * positions [at, at + n); the transaction's digest of every block staged
 * before `last` (the one being filled). */
struct out {
    uint32_t at;
    uint32_t n;
    uint64_t digest;
    const uint8_t *last;
};

/* ---- room ----------------------------------------------------------------------------------- */

static uint32_t div_up(uint32_t a, uint32_t b)
{
    return (a + b - 1) / b;
}

/* Log blocks a transaction of `meta` dirty blocks and `revokes` revoked
 * ones takes. */
static uint32_t log_blocks(const struct ext_fs *fs, const struct ext_jnl *j, uint32_t meta,
                           uint32_t revokes)
{
    return div_up(revokes, jnl_recs_per_revoke(fs, j)) +
           div_up(meta, jnl_tags_per_desc(fs, j)) + meta + 1;
}

/* Dirty blocks a transaction with `revokes` revoked ones may hold at most:
 * the most M with M + its descriptors within a quarter of the log, and
 * EXT_CACHE_MIN buffers of the cache kept clean for reading. */
static uint32_t capacity(const struct ext_fs *fs, const struct ext_jnl *j, uint32_t revokes)
{
    uint32_t over = 1 + div_up(revokes, jnl_recs_per_revoke(fs, j));
    if (j->max_tx <= over)
        return 0;
    uint32_t avail = j->max_tx - over;
    uint32_t m = avail - div_up(avail, jnl_tags_per_desc(fs, j) + 1);
    uint32_t nbufs = fs->cache.nbufs;
    uint32_t cache = nbufs > EXT_CACHE_MIN ? nbufs - EXT_CACHE_MIN : 0;
    return m < cache ? m : cache;
}

uint32_t ext_jnl_room(const struct ext_fs *fs)
{
    const struct ext_jnl *j = fs->jnl;
    if (!j)
        return 0;
    uint32_t cap = capacity(fs, j, j->nrevokes);
    return cap > fs->cache.ndirty ? cap - fs->cache.ndirty : 0;
}

status_t ext_jnl_begin(struct ext_fs *fs, uint32_t *out_tid)
{
    struct ext_jnl *j = fs->jnl;
    if (!j)
        return ERR_BAD_STATE;
    if (fs->cache.ndirty || j->nrevokes)
        return ERR_BAD_STATE;
    j->sb_before = fs->sb;
    if (out_tid)
        *out_tid = j->tid;
    return OK;
}

void ext_jnl_abort(struct ext_fs *fs)
{
    struct ext_jnl *j = fs->jnl;
    ext_discard(fs);
    if (!j)
        return;
    j->nrevokes = 0;
    fs->sb = j->sb_before;
}

status_t ext_jnl_step(struct ext_fs *fs, uint32_t need)
{
    struct ext_jnl *j = fs->jnl;
    if (!j)
        return ERR_BAD_STATE;
    if (need > capacity(fs, j, 0))
        return ERR_NO_RESOURCES;
    if (ext_jnl_room(fs) >= need)
        return OK;
    return ext_jnl_commit(fs);
}

/* ---- revokes ------------------------------------------------------------------------------ */

status_t ext_jnl_revoke(struct ext_fs *fs, uint64_t blk)
{
    struct ext_jnl *j = fs->jnl;
    if (!j)
        return ERR_BAD_STATE;
    if (blk >= fs->blocks)
        return ERR_OUT_OF_RANGE;
    if (j->nrevokes == j->cap_revokes)
        return ERR_NO_RESOURCES;
    /* Its bytes in the cache belong to a block that is free now: they must
     * not be written home or journaled. */
    for (uint32_t i = 0; i < fs->cache.nbufs; i++) {
        struct ext_buf *b = &fs->cache.bufs[i];
        if (b->blk != blk)
            continue;
        if (b->dirty) {
            b->dirty = false;
            fs->cache.ndirty--;
        }
        b->kind = EXT_KIND_NONE;
        if (!b->pins)
            ext_binval(fs, blk, 1);
        break;
    }
    j->revokes[j->nrevokes++] = (struct ext_jnl_revoke){ .blk = blk };
    return OK;
}

status_t ext_jnl_freed(struct ext_fs *fs, uint64_t blk, uint64_t count)
{
    struct ext_jnl *j = fs->jnl;
    if (!j)
        return OK;
    ext_bforget(fs, blk, count);
    for (uint64_t i = 0; i < count && j->nlive; i++) {
        if (!jnl_live_has(j, blk + i))
            continue;
        status_t st = ext_jnl_revoke(fs, blk + i);
        if (st != OK)
            return st;
    }
    return OK;
}

/* Sort the revoke list (a heap sort: no recursion, no allocation). */
static void sift(struct ext_jnl_revoke *r, uint32_t at, uint32_t n)
{
    for (uint32_t c; (c = 2 * at + 1) < n; at = c) {
        if (c + 1 < n && r[c + 1].blk > r[c].blk)
            c++;
        if (r[at].blk >= r[c].blk)
            return;
        struct ext_jnl_revoke t = r[at];
        r[at] = r[c];
        r[c] = t;
    }
}

static void sort_revokes(struct ext_jnl_revoke *r, uint32_t n)
{
    for (uint32_t i = n / 2; i-- > 0;)
        sift(r, i, n);
    for (uint32_t i = n; i-- > 1;) {
        struct ext_jnl_revoke t = r[0];
        r[0] = r[i];
        r[i] = t;
        sift(r, 0, i);
    }
}

/* The index of blk in the sorted list r of n, or n if it isn't there. */
static uint32_t find(const struct ext_jnl_revoke *r, uint32_t n, uint64_t blk)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (r[mid].blk == blk)
            return mid;
        if (r[mid].blk < blk)
            lo = mid + 1;
        else
            hi = mid;
    }
    return n;
}

/* The list sorted, each block once, without the blocks dirty again (made
 * metadata again in this transaction: the copy replays after the older
 * ones the revoke was for, so it is enough). */
static void settle_revokes(struct ext_fs *fs, struct ext_jnl *j)
{
    struct ext_jnl_revoke *r = j->revokes;
    sort_revokes(r, j->nrevokes);
    uint32_t n = 0;
    for (uint32_t i = 0; i < j->nrevokes; i++)
        if (!n || r[n - 1].blk != r[i].blk)
            r[n++] = r[i];
    /* Marked (`used`, free while a transaction runs) and dropped after, so
     * the list stays sorted for every search. */
    for (uint32_t i = 0; i < fs->cache.nbufs; i++) {
        struct ext_buf *b = &fs->cache.bufs[i];
        uint32_t at = b->dirty ? find(r, n, b->blk) : n;
        if (at < n)
            r[at].used = 1;
    }
    j->nrevokes = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!r[i].used)
            r[j->nrevokes++] = r[i];
}

/* ---- staging and writing ------------------------------------------------------------------ */

static status_t flush(struct ext_fs *fs, struct ext_jnl *j, struct out *o)
{
    if (!o->n)
        return OK;
    status_t st = jnl_write(fs, j, o->at, o->n, j->stage);
    for (uint32_t i = 0; i < o->n; i++)
        o->at = jnl_next(j, o->at);
    o->n = 0;
    return st;
}

/* The next staging block, zeroed (flushing first when the staging blocks
 * are full or the next log block wraps to the log's start). The block
 * staged before it is whole now: it joins the digest. */
static status_t stage(struct ext_fs *fs, struct ext_jnl *j, struct out *o, uint8_t **out)
{
    if (o->last)
        o->digest = jnl_digest(o->digest, o->last, fs->block_size);
    if (o->n == j->nstage || (o->n && o->at + o->n >= j->maxlen)) {
        status_t st = flush(fs, j, o);
        if (st != OK)
            return st;
    }
    *out = j->stage + ((size_t)o->n++ << fs->block_log);
    __builtin_memset(*out, 0, fs->block_size);
    o->last = *out;
    return OK;
}

static status_t write_revokes(struct ext_fs *fs, struct ext_jnl *j, struct out *o)
{
    uint32_t per = jnl_recs_per_revoke(fs, j);
    for (uint32_t i = 0; i < j->nrevokes; i += per) {
        uint8_t *b;
        status_t st = stage(fs, j, o, &b);
        if (st != OK)
            return st;
        jnl_header_set(b, JBD2_REVOKE, j->tid);
        uint32_t n = j->nrevokes - i < per ? j->nrevokes - i : per;
        for (uint32_t k = 0; k < n; k++) {
            uint64_t blk = j->revokes[i + k].blk;
            if (j->rec_bytes == 8) {
                uint64_t v = jbd2_be64(blk);
                __builtin_memcpy(b + JBD2_REVOKE_FIRST + 8 * k, &v, 8);
            } else {
                uint32_t v = jbd2_be32((uint32_t)blk);
                __builtin_memcpy(b + JBD2_REVOKE_FIRST + 4 * k, &v, 4);
            }
        }
        ((struct jbd2_revoke_header *)(void *)b)->count = jbd2_be32(JBD2_REVOKE_FIRST +
                                                                    n * j->rec_bytes);
        jnl_tail_set(fs, j, b);
    }
    return OK;
}

/* A descriptor for the dirty buffers from index *from on (as many as it
 * holds), then their copies; *from moves past them. */
static status_t write_group(struct ext_fs *fs, struct ext_jnl *j, struct out *o, uint32_t *from)
{
    uint8_t *d;
    status_t st = stage(fs, j, o, &d);
    if (st != OK)
        return st;
    jnl_header_set(d, JBD2_DESCRIPTOR, j->tid);
    uint32_t per = jnl_tags_per_desc(fs, j), n = 0, at = sizeof(struct jbd2_header), i;
    struct jbd2_tag3 *last = NULL;
    for (i = *from; i < fs->cache.nbufs && n < per; i++) {
        struct ext_buf *b = &fs->cache.bufs[i];
        if (!b->dirty)
            continue;
        bool esc = jnl_magic_at(b->data);
        struct jbd2_tag3 *t = (void *)(d + at);
        t->blocknr = jbd2_be32((uint32_t)b->blk);
        t->blocknr_high = jbd2_be32((uint32_t)(b->blk >> 32));
        t->flags = jbd2_be32((esc ? JBD2_FLAG_ESCAPE : 0) | (n ? JBD2_FLAG_SAME_UUID : 0));
        t->checksum = jbd2_be32(jnl_tag_csum(fs, j, j->tid, b->data, esc));
        at += j->tag_bytes;
        if (!n++) {
            __builtin_memcpy(d + at, j->sb.uuid, JBD2_UUID_SIZE);
            at += JBD2_UUID_SIZE;
        }
        last = t;
    }
    last->flags |= jbd2_be32(JBD2_FLAG_LAST_TAG);
    jnl_tail_set(fs, j, d);
    for (uint32_t k = *from; k < i; k++) {
        struct ext_buf *b = &fs->cache.bufs[k];
        if (!b->dirty)
            continue;
        uint8_t *c;
        st = stage(fs, j, o, &c);
        if (st != OK)
            return st;
        __builtin_memcpy(c, b->data, fs->block_size);
        if (jnl_magic_at(c))
            __builtin_memset(c, 0, 4);
        jnl_live_add(j, b->blk);
    }
    *from = i;
    return OK;
}

static status_t write_commit(struct ext_fs *fs, struct ext_jnl *j, struct out *o)
{
    uint8_t *c;
    status_t st = stage(fs, j, o, &c);
    if (st != OK)
        return st;
    jnl_header_set(c, JBD2_COMMIT, j->tid);
    struct jbd2_commit *h = (void *)c;
    h->chksum[1] = jbd2_be32((uint32_t)(o->digest >> 32));
    h->chksum[2] = jbd2_be32((uint32_t)o->digest);
    h->chksum[3] = jbd2_be32(JNL_DIGEST_MARK);
    h->commit_sec = jbd2_be64(j->commit_sec);
    h->commit_nsec = jbd2_be32(j->commit_nsec);
    jnl_commit_set(fs, j, c);
    return flush(fs, j, o);
}

/* Write the transaction to the log from the head; *out_head: past it. */
static status_t write_tx(struct ext_fs *fs, struct ext_jnl *j, uint32_t *out_head)
{
    struct out o = { .at = j->head, .n = 0, .digest = jnl_digest_start(j, j->tid) };
    status_t st = write_revokes(fs, j, &o);
    for (uint32_t from = 0, left = fs->cache.ndirty; st == OK && left;) {
        uint32_t before = from;
        st = write_group(fs, j, &o, &from);
        for (uint32_t k = before; k < from; k++)
            left -= fs->cache.bufs[k].dirty;
    }
    if (st == OK && !(j->flags & EXT_JNL_ONE_WRITE))
        st = flush(fs, j, &o);   /* every copy on the disk before the commit block */
    if (st == OK && !(j->flags & EXT_JNL_ONE_WRITE))
        st = ext_flush(&fs->dev);   /* ... on the medium, with the file data before them */
    if (st == OK)
        st = write_commit(fs, j, &o);
    *out_head = o.at;
    return st;
}

/* ---- the commit -------------------------------------------------------------------------- */

status_t ext_jnl_commit(struct ext_fs *fs)
{
    struct ext_jnl *j = fs->jnl;
    if (!j)
        return ERR_BAD_STATE;
    if (fs->ro)
        return ERR_ACCESS_DENIED;
    settle_revokes(fs, j);
    uint32_t meta = fs->cache.ndirty;
    if (!meta && !j->nrevokes)
        return OK;
    uint32_t blocks = log_blocks(fs, j, meta, j->nrevokes);
    if (blocks > j->max_tx)
        return ERR_NO_RESOURCES;
    status_t st = jnl_make_room(fs, j, blocks);
    if (st == ERR_NO_RESOURCES)
        return st;
    if (j->on_commit && st == OK)
        j->on_commit(j->ctx, j->tid);
    /* On a device with a cache: the file data on the medium before a log
     * whose commit points at it (in one write with its copies, only the
     * digest keeps the commit block from counting alone) ... */
    if (st == OK && (j->flags & EXT_JNL_ONE_WRITE))
        st = ext_flush(&fs->dev);
    uint32_t head;
    if (st == OK)
        st = write_tx(fs, j, &head);
    /* ... and the commit block before anything goes home or is answered */
    if (st == OK)
        st = ext_flush(&fs->dev);
    if (st != OK)
        return ext_damaged(fs, "journal: commit write failed", fs->sb.journal_inum, 0);
    jnl_committed(fs, j, head);
    if (ext_sync(fs) != OK)
        return ext_damaged(fs, "journal: checkpoint write failed", 0, 0);
    if (jnl_checkpointed(fs, j) != OK)
        return ext_damaged(fs, "journal: tail write failed", fs->sb.journal_inum, 0);
    return OK;
}
