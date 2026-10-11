/* ext: blocks taken from and given back to the groups' block bitmaps.
 *
 * Each group's block bitmap has a bit per block of the group (1: in use);
 * the bits past the group's blocks (the last group may be short, and a
 * bitmap block holds more bits than a group has blocks) are set, as the
 * format wants. A bitmap's checksum lives in its descriptor. Three counts
 * say how many blocks are free: the bitmap's clear bits, the group's
 * descriptor and, summed over the groups, the superblock: every change
 * here keeps all three exact (Linux's e2fsck compares them).
 *
 * A group that was never written since mke2fs (BLOCK_UNINIT, with group
 * checksums) has no bitmap on the disk: what is in use there is its own
 * metadata only, the copy of the superblock and descriptors (and
 * resize_inode's reserve) if the group holds one, and any group's bitmaps
 * and inode table that lie in it (with flex_bg, a flex group's metadata
 * lies together in its first group). ext_group_bitmap makes that bitmap
 * the first time a group is used and checks it against the descriptor's
 * free count. Inode bitmaps (ialloc.c) are made the same way.
 *
 * Allocation: the first free block at or after a goal (the caller's:
 * after the file's previous block, so a file grows in runs), then as many
 * free blocks after it as asked for, inside the group. A group with no
 * free block is skipped on its descriptor's count, without reading its
 * bitmap. */
#include "ext_alloc_int.h"

/* ---- bits ------------------------------------------------------------------------------ */

void ext_bits_set(uint8_t *map, uint32_t from, uint32_t n)
{
    for (; n && (from & 7); from++, n--)
        map[from >> 3] |= (uint8_t)(1u << (from & 7));
    for (; n >= 8; from += 8, n -= 8)
        map[from >> 3] = 0xff;
    for (; n; from++, n--)
        map[from >> 3] |= (uint8_t)(1u << (from & 7));
}

void ext_bits_clear(uint8_t *map, uint32_t from, uint32_t n)
{
    for (; n && (from & 7); from++, n--)
        map[from >> 3] &= (uint8_t)~(1u << (from & 7));
    for (; n >= 8; from += 8, n -= 8)
        map[from >> 3] = 0;
    for (; n; from++, n--)
        map[from >> 3] &= (uint8_t)~(1u << (from & 7));
}

uint32_t ext_bit_find_clear(const uint8_t *map, uint32_t from, uint32_t to)
{
    while (from < to) {
        if (!(from & 7) && map[from >> 3] == 0xff) {   /* a whole byte in use */
            from += 8;
            continue;
        }
        if (!ext_bit(map, from))
            return from;
        from++;
    }
    return to;
}

uint32_t ext_bits_count(const uint8_t *map, uint32_t from, uint32_t to)
{
    uint32_t n = 0;
    while (from < to) {
        if (!(from & 7) && to - from >= 8) {
            n += (uint32_t)__builtin_popcount(map[from >> 3]);
            from += 8;
            continue;
        }
        n += ext_bit(map, from);
        from++;
    }
    return n;
}

/* Clear bits from `from` on, before `to`, at most max. */
static uint32_t run_clear(const uint8_t *map, uint32_t from, uint32_t to, uint32_t max)
{
    uint32_t n = 0;
    while (from + n < to && n < max) {
        uint32_t i = from + n;
        if (!(i & 7) && to - i >= 8 && max - n >= 8 && map[i >> 3] == 0) {
            n += 8;
            continue;
        }
        if (ext_bit(map, i))
            break;
        n++;
    }
    return n;
}

/* ---- the free counts ------------------------------------------------------------------- */

uint64_t ext_free_blocks(const struct ext_fs *fs)
{
    uint64_t n = fs->sb.free_blocks_count_lo;
    if (fs->is64)
        n |= (uint64_t)fs->sb.free_blocks_count_hi << 32;
    return n;
}

status_t ext_count_free(struct ext_fs *fs)
{
    status_t st = ext_writable(fs);
    uint64_t blocks = 0, inodes = 0;
    for (uint32_t g = 0; st == OK && g < fs->groups; g++) {
        struct ext_group gr;
        st = ext_group_get(fs, g, &gr);
        if (st == OK) {
            blocks += gr.free_blocks;
            inodes += gr.free_inodes;
        }
    }
    if (st != OK || (blocks == ext_free_blocks(fs) && inodes == fs->sb.free_inodes_count))
        return st;
    fs->sb.free_blocks_count_lo = (uint32_t)blocks;
    if (fs->is64)
        fs->sb.free_blocks_count_hi = (uint32_t)(blocks >> 32);
    fs->sb.free_inodes_count = (uint32_t)inodes;   /* at most groups * inodes_per_group */
    return ext_super_write(fs);
}

/* The superblock's count, moved by delta, written back. */
static status_t sb_free_add(struct ext_fs *fs, int64_t delta)
{
    uint64_t n = ext_free_blocks(fs) + (uint64_t)delta;
    if (n > fs->blocks)
        return ext_damaged(fs, "superblock: free block count", 0, 0);
    fs->sb.free_blocks_count_lo = (uint32_t)n;
    if (fs->is64)
        fs->sb.free_blocks_count_hi = (uint32_t)(n >> 32);
    return ext_super_write(fs);
}

/* ---- groups' metadata ------------------------------------------------------------------ */

/* Blocks at a group's start that its copy of the superblock and
 * descriptors take (0 if it holds none). */
static uint32_t super_blocks(const struct ext_fs *fs, uint32_t g)
{
    if (!ext_group_has_super(fs, g))
        return 0;
    uint32_t r = fs->sb.feature_compat & EXT_COMPAT_RESIZE_INODE ? fs->sb.reserved_gdt_blocks : 0;
    return 1 + fs->gdt_blocks + r;
}

/* Mark in map (group g's bits) the part of [blk, blk + n) inside g. */
static void mark_inside(const struct ext_fs *fs, uint32_t g, uint8_t *map, uint64_t blk,
                        uint64_t n)
{
    uint64_t first = ext_group_first(fs, g), end = first + ext_group_blocks(fs, g);
    uint64_t lo = blk > first ? blk : first, hi = blk + n < end ? blk + n : end;
    if (lo < hi)
        ext_bits_set(map, (uint32_t)(lo - first), (uint32_t)(hi - lo));
}

/* The groups whose bitmaps and tables may lie in group g: with flex_bg,
 * those of g's flex group (mke2fs puts them in its first group); else g. */
static void meta_groups(const struct ext_fs *fs, uint32_t g, uint32_t *from, uint32_t *to)
{
    uint64_t size = fs->flex_bg ? (uint64_t)1 << fs->sb.log_groups_per_flex : 1;
    uint64_t first = g / size * size;
    *from = (uint32_t)first;
    *to = (uint32_t)(first + size < fs->groups ? first + size : fs->groups);
}

/* Mark group g's metadata in map: its superblock copy and every group's
 * bitmaps and inode table that lie in it (all groups are looked at: a
 * resized volume may put them anywhere). */
static status_t mark_meta(struct ext_fs *fs, uint32_t g, uint8_t *map)
{
    mark_inside(fs, g, map, ext_group_first(fs, g), super_blocks(fs, g));
    for (uint32_t h = 0; h < fs->groups; h++) {
        struct ext_group gr;
        status_t st = ext_group_get(fs, h, &gr);
        if (st != OK)
            return st;
        mark_inside(fs, g, map, gr.block_bitmap, 1);
        mark_inside(fs, g, map, gr.inode_bitmap, 1);
        mark_inside(fs, g, map, gr.inode_table, fs->itable_blocks);
    }
    return OK;
}

/* Does [blk, blk + n), inside group g, cover any group's metadata? */
static status_t is_meta(struct ext_fs *fs, uint32_t g, uint64_t blk, uint64_t n, bool *out)
{
    uint64_t first = ext_group_first(fs, g);
    bool hit = blk - first < super_blocks(fs, g);
    uint32_t from, to;
    meta_groups(fs, g, &from, &to);
    for (uint32_t h = from; h < to && !hit; h++) {
        struct ext_group gr;
        status_t st = ext_group_get(fs, h, &gr);
        if (st != OK)
            return st;
        hit = (gr.block_bitmap >= blk && gr.block_bitmap - blk < n) ||
              (gr.inode_bitmap >= blk && gr.inode_bitmap - blk < n) ||
              (gr.inode_table < blk + n && blk < gr.inode_table + fs->itable_blocks);
    }
    *out = hit;
    return OK;
}

/* ---- bitmaps --------------------------------------------------------------------------- */

/* Make group g's bitmap, which the group never wrote: fill, check, seal. */
static status_t make_bitmap(struct ext_fs *fs, uint32_t g, enum ext_bitmap which,
                            struct ext_buf **out)
{
    struct ext_group gr;
    status_t st = ext_group_get(fs, g, &gr);
    if (st != OK)
        return st;
    bool blocks = which == EXT_BITMAP_BLOCK;
    struct ext_buf *b;
    st = ext_bnew(fs, blocks ? gr.block_bitmap : gr.inode_bitmap, &b);
    if (st != OK)
        return st;
    uint32_t bits = fs->block_size * 8;
    uint32_t used = blocks ? ext_group_blocks(fs, g) : fs->inodes_per_group;
    ext_bits_set(b->data, used, bits - used);
    if (blocks)
        st = mark_meta(fs, g, b->data);
    uint32_t free_now = used - ext_bits_count(b->data, 0, used);
    if (st == OK && free_now != (blocks ? gr.free_blocks : gr.free_inodes))
        st = ext_damaged(fs, blocks ? "uninitialised group: free blocks"
                                    : "uninitialised group: free inodes", 0, b->blk);
    if (st == OK) {
        gr.flags &= (uint16_t)~(blocks ? EXT_BG_BLOCK_UNINIT : EXT_BG_INODE_UNINIT);
        st = ext_group_bitmap_seal(fs, g, which, &gr, b);
    }
    if (st != OK) {
        uint64_t blk = b->blk;
        ext_brelse(fs, b);
        ext_bforget(fs, blk, 1);   /* the half-made bitmap must not be read as the real one */
        return st;
    }
    *out = b;
    return OK;
}

status_t ext_group_bitmap(struct ext_fs *fs, uint32_t g, enum ext_bitmap which,
                          struct ext_group *gr, struct ext_buf **out)
{
    struct ext_buf *b;
    status_t st = ext_bitmap_read(fs, g, which, &b);
    if (st == ERR_BAD_STATE)
        st = make_bitmap(fs, g, which, &b);
    if (st != OK)
        return st;
    st = ext_group_get(fs, g, gr);
    if (st != OK) {
        ext_brelse(fs, b);
        return st;
    }
    *out = b;
    return OK;
}

status_t ext_group_bitmap_seal(struct ext_fs *fs, uint32_t g, enum ext_bitmap which,
                               struct ext_group *gr, struct ext_buf *b)
{
    uint32_t c = ext_bitmap_csum(fs, which, b->data);
    if (which == EXT_BITMAP_BLOCK)
        gr->block_bitmap_csum = c;
    else
        gr->inode_bitmap_csum = c;
    if (fs->desc_size < EXT_DESC_MIN_64) {   /* a 32-byte descriptor keeps the low half */
        gr->block_bitmap_csum &= 0xffff;
        gr->inode_bitmap_csum &= 0xffff;
    }
    status_t st = ext_group_put(fs, g, gr);
    if (st == OK)
        ext_bdirty(fs, b);
    return st;
}

/* ---- taking and giving back ------------------------------------------------------------ */

/* Take the first free block in [from, to) of group g and the free ones
 * after it (to the group's end, at most want). ERR_NOT_FOUND: none. */
static status_t take(struct ext_fs *fs, uint32_t g, uint32_t from, uint32_t to, uint32_t want,
                     uint64_t *out_blk, uint32_t *out_n)
{
    struct ext_group gr;
    status_t st = ext_group_get(fs, g, &gr);
    if (st != OK)
        return st;
    if (!gr.free_blocks)
        return ERR_NOT_FOUND;
    struct ext_buf *b;
    st = ext_group_bitmap(fs, g, EXT_BITMAP_BLOCK, &gr, &b);
    if (st != OK)
        return st;
    uint32_t at = ext_bit_find_clear(b->data, from, to);
    if (at == to) {
        ext_brelse(fs, b);
        return ERR_NOT_FOUND;
    }
    uint32_t n = run_clear(b->data, at, ext_group_blocks(fs, g), want);
    if (n > gr.free_blocks) {
        ext_brelse(fs, b);
        return ext_damaged(fs, "group descriptor: free blocks", 0, b->blk);
    }
    ext_bits_set(b->data, at, n);
    gr.free_blocks -= n;
    st = ext_group_bitmap_seal(fs, g, EXT_BITMAP_BLOCK, &gr, b);
    ext_brelse(fs, b);
    if (st == OK)
        st = sb_free_add(fs, -(int64_t)n);
    if (st == OK) {
        *out_blk = ext_group_first(fs, g) + at;
        *out_n = n;
    }
    return st;
}

status_t ext_balloc(struct ext_fs *fs, uint64_t goal, uint32_t want, uint64_t *out_blk,
                    uint32_t *out_n)
{
    status_t st = ext_writable(fs);
    if (st != OK)
        return st;
    if (!want)
        return ERR_INVALID_ARGS;
    if (!ext_free_blocks(fs))
        return ERR_NO_SPACE;
    if (!ext_blk_ok(fs, goal))
        goal = fs->first_data_block;
    uint32_t g0 = (uint32_t)((goal - fs->first_data_block) / fs->blocks_per_group);
    uint32_t at = (uint32_t)(goal - ext_group_first(fs, g0));
    /* g0 from the goal on, every other group, then g0 before the goal. */
    for (uint32_t i = 0; i <= fs->groups; i++) {
        uint32_t g = (uint32_t)(((uint64_t)g0 + i) % fs->groups);
        uint32_t from = i == 0 ? at : 0;
        uint32_t to = i == fs->groups ? at : ext_group_blocks(fs, g);
        st = take(fs, g, from, to, want, out_blk, out_n);
        if (st != ERR_NOT_FOUND)
            return st;
    }
    return ext_damaged(fs, "superblock: free blocks counted, none found", 0, 0);
}

/* Give back [blk, blk + n), all inside group g. */
static status_t give(struct ext_fs *fs, uint32_t g, uint64_t blk, uint32_t n)
{
    bool meta;
    status_t st = is_meta(fs, g, blk, n, &meta);
    if (st == OK && meta)
        st = ext_damaged(fs, "freeing a group's metadata", 0, blk);
    struct ext_group gr;
    struct ext_buf *b;
    if (st == OK)
        st = ext_group_bitmap(fs, g, EXT_BITMAP_BLOCK, &gr, &b);
    if (st != OK)
        return st;
    uint32_t at = (uint32_t)(blk - ext_group_first(fs, g));
    if (ext_bits_count(b->data, at, at + n) != n || gr.free_blocks + n > fs->blocks_per_group) {
        ext_brelse(fs, b);
        return ext_damaged(fs, "freeing a free block", 0, blk);
    }
    ext_bits_clear(b->data, at, n);
    gr.free_blocks += n;
    st = ext_group_bitmap_seal(fs, g, EXT_BITMAP_BLOCK, &gr, b);
    ext_brelse(fs, b);
    if (st == OK)
        st = sb_free_add(fs, n);
    if (st == OK)
        ext_bforget(fs, blk, n);
    return st;
}

status_t ext_bfree(struct ext_fs *fs, uint64_t blk, uint64_t count)
{
    status_t st = ext_writable(fs);
    if (st != OK)
        return st;
    if (!ext_run_ok(fs, blk, count))
        return ext_damaged(fs, "freeing blocks outside the volume", 0, blk);
    while (count && st == OK) {
        uint32_t g = (uint32_t)((blk - fs->first_data_block) / fs->blocks_per_group);
        uint64_t left = ext_group_first(fs, g) + ext_group_blocks(fs, g) - blk;
        uint32_t n = (uint32_t)(count < left ? count : left);
        st = give(fs, g, blk, n);
        blk += n;
        count -= n;
    }
    return st;
}
