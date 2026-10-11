/* ext: writing a file's bytes, filling its holes, and cutting it short.
 *
 * A write moves through the file a run at a time (ext_map): over the
 * file's own blocks it writes in place; in a hole it first takes blocks
 * (ext_balloc, near the file's block before, so a file written in order
 * is one extent) and writes them, and only then maps them (an extent
 * inserted in the cache: ordered mode, data before the metadata that
 * points at it); in an unwritten extent it writes the blocks and then
 * marks them written. Whole blocks go from the caller's buffer to the
 * device in one write per run, and any cached copy is dropped; a part of
 * a block is put together in a cache buffer (the old bytes, or zeros in
 * a new block) and written whole, the buffer left clean, the same as
 * the disk.
 *
 * Space for the tree: a hole is filled only while the volume keeps a
 * block per level of the tree and two more, so the insert that maps the
 * new blocks can always split its nodes.
 *
 * Truncation shortens a file from its end (ext_truncate): the size first,
 * the last block's bytes past the new size zeroed on the disk (a later
 * write that grows the file must find zeros there), then the extents
 * past it removed, EXT_TRUNCATE_STEP at a time. */
#include "ext_alloc_int.h"

#define DIRECT_MAX (1u << 30)   /* bytes one device write takes at most */

status_t ext_iblocks_add(struct ext_fs *fs, struct ext_inode *in, int64_t n)
{
    uint64_t cur = ext_iblocks512(fs, in);
    int64_t d = n * (int64_t)(fs->block_size >> 9);
    if (d < 0 && (uint64_t)-d > cur)
        return ext_damaged(fs, "inode: block count", in->ino, 0);
    uint64_t v = cur + (uint64_t)d;
    if (v >> 32 && (!fs->huge_file || v >> 48))
        return ERR_NO_SPACE;
    in->d.blocks_lo = (uint32_t)v;
    in->d.blocks_high = (uint16_t)(v >> 32);
    in->d.flags &= ~(uint32_t)EXT_HUGE_FILE_FL;   /* counted in sectors from now on */
    return OK;
}

/* Where lblk's new blocks should go: after the extent before it, or
 * before the one after it, or at the start of the inode's group. */
static status_t goal_for(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, uint64_t *out)
{
    struct ext_path p;
    status_t st = ext_extent_find(fs, in, lblk, &p);
    if (st != OK) {
        ext_path_release(fs, &p);
        return st;
    }
    struct ext_extent_header *h = p.lv[p.depth].h;
    struct ext_extent *e = ext_extents(h);
    int32_t at = p.lv[p.depth].at;
    uint64_t goal = ext_group_first(fs, (in->ino - 1) / fs->inodes_per_group);
    if (at >= 0) {
        goal = ext_extent_start(&e[at]) + (lblk - e[at].block);
        if (!ext_blk_ok(fs, goal))   /* far past it: right after it, then */
            goal = ext_extent_start(&e[at]) + ext_extent_len(&e[at]);
    } else if (h->entries && ext_extent_start(&e[0]) > e[0].block - lblk)
        goal = ext_extent_start(&e[0]) - (e[0].block - lblk);
    ext_path_release(fs, &p);
    *out = goal;
    return OK;
}

/* Take up to want blocks for lblk, keeping the tree's reserve. */
static status_t take(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, uint32_t want,
                     uint64_t *out_blk, uint32_t *out_n)
{
    const struct ext_extent_header *root = (const struct ext_extent_header *)in->d.block;
    uint64_t reserve = (uint64_t)root->depth + 3, free_now = ext_free_blocks(fs);
    if (free_now <= reserve)
        return ERR_NO_SPACE;
    if (want > free_now - reserve)
        want = (uint32_t)(free_now - reserve);
    uint64_t goal;
    status_t st = goal_for(fs, in, lblk, &goal);
    if (st == OK)
        st = ext_balloc(fs, goal, want, out_blk, out_n);
    return st;
}

/* Map new blocks [pblk, pblk + n) at lblk, counted in i_blocks. */
static status_t map_new(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, uint64_t pblk,
                        uint32_t n)
{
    struct ext_run r = { .pblk = pblk, .len = n, .kind = EXT_RUN_DATA };
    status_t st = ext_iblocks_add(fs, in, n);
    if (st == OK) {
        st = ext_extent_insert(fs, in, lblk, &r);   /* writes the inode back */
        if (st != OK) {
            (void)ext_iblocks_add(fs, in, -(int64_t)n);   /* the add just made: can't fail */
            (void)ext_iput(fs, in);   /* the insert's error is the one to report */
        }
    }
    if (st != OK)
        (void)ext_bfree(fs, pblk, n);   /* taken a moment ago, never mapped */
    return st;
}

/* Up to want blocks of the hole at lblk, mapped (or the run already there). */
status_t ext_map_alloc(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, uint32_t want,
                       struct ext_run *out)
{
    struct ext_run r;
    status_t st = ext_map(fs, in, lblk, &r);
    if (st != OK || r.kind != EXT_RUN_HOLE) {
        if (st == OK)
            *out = r;
        return st;
    }
    st = ext_writable(fs);
    if (st != OK)
        return st;
    if (!(in->d.flags & EXT_EXTENTS_FL) || !want)
        return in->d.flags & EXT_EXTENTS_FL ? ERR_INVALID_ARGS : ERR_NOT_SUPPORTED;
    if (lblk >= UINT32_MAX)
        return ERR_OUT_OF_RANGE;   /* past the largest size (ext_max_size) */
    uint32_t n = want < r.len ? want : r.len;
    if (n > EXT_INIT_MAX_LEN)
        n = EXT_INIT_MAX_LEN;
    if (n > UINT32_MAX - lblk)
        n = UINT32_MAX - lblk;
    uint64_t pblk;
    st = take(fs, in, lblk, n, &pblk, &n);
    if (st == OK)
        st = map_new(fs, in, lblk, pblk, n);
    if (st == OK)
        *out = (struct ext_run){ .pblk = pblk, .len = n, .kind = EXT_RUN_DATA };
    return st;
}

/* ---- writing blocks -------------------------------------------------------------------- */

/* Put src's n bytes at `within` of block blk, through a cache buffer, and
 * write the block whole: its old bytes kept, or zeros (`fresh`). */
static status_t write_part(struct ext_fs *fs, uint64_t blk, bool fresh, uint32_t within,
                           const uint8_t *src, uint32_t n)
{
    struct ext_buf *b;
    status_t st = fresh ? ext_bnew(fs, blk, &b) : ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    if (src)
        __builtin_memcpy(b->data + within, src, n);
    else
        __builtin_memset(b->data + within, 0, n);
    fs->cache.writes++;
    st = fs->dev.write(fs->dev.ctx, blk << fs->block_log, fs->block_size, b->data);
    ext_brelse(fs, b);
    if (st != OK) {
        ext_bforget(fs, blk, 1);   /* the cached bytes aren't the disk's */
        return ERR_IO;
    }
    return OK;
}

/* Write n bytes of src at byte `within` of blocks [blk, ...): parts of a
 * block through write_part, whole blocks straight to the device. */
static status_t write_span(struct ext_fs *fs, uint64_t blk, bool fresh, uint32_t within,
                           const uint8_t *src, size_t n)
{
    uint32_t bs = fs->block_size;
    while (n) {
        status_t st;
        size_t take;
        if (within || n < bs) {
            take = n < bs - within ? n : bs - within;
            st = write_part(fs, blk, fresh, within, src, (uint32_t)take);
            blk++;
        } else {
            take = n > DIRECT_MAX ? DIRECT_MAX : n & ~(size_t)(bs - 1);
            uint64_t count = take >> fs->block_log;
            fs->cache.writes++;
            st = fs->dev.write(fs->dev.ctx, blk << fs->block_log, (uint32_t)take, src) == OK
                     ? OK : ERR_IO;
            ext_binval(fs, blk, count);
            blk += count;
        }
        if (st != OK)
            return st;
        src += take;
        n -= take;
        within = 0;
    }
    return OK;
}

/* One run's worth of a write at off: *out_n bytes written. */
static status_t write_run(struct ext_fs *fs, struct ext_inode *in, uint64_t off,
                          const uint8_t *src, size_t n, size_t *out_n)
{
    uint32_t lblk = (uint32_t)(off >> fs->block_log);
    uint32_t within = (uint32_t)(off & (fs->block_size - 1));
    struct ext_run r;
    status_t st = ext_map(fs, in, lblk, &r);
    if (st != OK)
        return st;
    uint64_t blocks = (within + (uint64_t)n + fs->block_size - 1) >> fs->block_log;
    uint32_t k = (uint32_t)(blocks < r.len ? blocks : r.len);
    if (r.kind == EXT_RUN_HOLE) {
        st = take(fs, in, lblk, k < EXT_INIT_MAX_LEN ? k : EXT_INIT_MAX_LEN, &r.pblk, &k);
        if (st != OK)
            return st;
    }
    size_t span = ((size_t)k << fs->block_log) - within;
    size_t bytes = n < span ? n : span;
    st = write_span(fs, r.pblk, r.kind != EXT_RUN_DATA, within, src, bytes);
    if (st == OK && r.kind == EXT_RUN_HOLE)
        st = map_new(fs, in, lblk, r.pblk, k);
    else if (st != OK && r.kind == EXT_RUN_HOLE)
        (void)ext_bfree(fs, r.pblk, k);   /* never mapped */
    else if (st == OK && r.kind == EXT_RUN_UNWRITTEN)
        st = ext_extent_written(fs, in, lblk, k);
    if (st == OK)
        *out_n = bytes;
    return st;
}

status_t ext_write(struct ext_fs *fs, struct ext_inode *in, uint64_t off, const void *buf,
                   size_t len, size_t *out_n)
{
    status_t st = ext_writable(fs);
    if (st != OK)
        return st;
    if (!(in->d.flags & EXT_EXTENTS_FL))
        return ERR_NOT_SUPPORTED;
    uint64_t max = ext_max_size(fs);
    if (off >= max && len)
        return ERR_OUT_OF_RANGE;
    if (len > max - off)
        len = (size_t)(max - off);
    size_t done = 0;
    while (done < len) {
        size_t n;
        st = write_run(fs, in, off + done, (const uint8_t *)buf + done, len - done, &n);
        if (st != OK)
            break;
        done += n;
    }
    if (st == ERR_NO_SPACE && done)
        st = OK;   /* a short write: what was written stays */
    if (done && off + done > ext_isize(in)) {
        in->d.size_lo = (uint32_t)(off + done);
        in->d.size_high = (uint32_t)((off + done) >> 32);
    }
    status_t put = ext_iput(fs, in);
    if (st == OK)
        st = put;
    if (st == OK)
        *out_n = done;
    return st;
}

/* ---- truncation ------------------------------------------------------------------------ */

/* Zero the bytes of the block holding `size` past it, if it has data. */
static status_t zero_tail(struct ext_fs *fs, struct ext_inode *in, uint64_t size)
{
    uint32_t within = (uint32_t)(size & (fs->block_size - 1));
    if (!within)
        return OK;
    struct ext_run r;
    status_t st = ext_map(fs, in, (uint32_t)(size >> fs->block_log), &r);
    if (st != OK || r.kind != EXT_RUN_DATA)
        return st;
    return write_part(fs, r.pblk, false, within, NULL, fs->block_size - within);
}

status_t ext_truncate(struct ext_fs *fs, struct ext_inode *in, uint64_t size, bool *out_done)
{
    status_t st = ext_writable(fs);
    if (st != OK)
        return st;
    if (!(in->d.flags & EXT_EXTENTS_FL))
        return ERR_NOT_SUPPORTED;
    if (size > ext_max_size(fs))
        return ERR_OUT_OF_RANGE;
    if (size != ext_isize(in)) {
        if (size < ext_isize(in))
            st = zero_tail(fs, in, size);
        if (st != OK)
            return st;
        in->d.size_lo = (uint32_t)size;
        in->d.size_high = (uint32_t)(size >> 32);
    }
    uint64_t first = (size + fs->block_size - 1) >> fs->block_log;
    bool done;
    st = ext_extent_punch(fs, in, (uint32_t)first, (uint64_t)1 << 32, EXT_TRUNCATE_STEP, &done);
    if (st == OK)
        *out_done = done;
    return st;
}
