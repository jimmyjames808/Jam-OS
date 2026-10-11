/* ext: a stand-in allocator, ext_alloc.h's contracts for what the name
 * code needs, until the real allocator replaces this file. It is simple
 * on purpose:
 *   - inodes: the first free bit, from the parent's group on; a group
 *     whose inode bitmap was never written (INODE_UNINIT) gets one, all
 *     free, and loses the flag; the group's count of never-used inodes
 *     shrinks past the one taken;
 *   - blocks: the first free bit at or after a goal, group by group,
 *     skipping groups whose block bitmap was never written (BLOCK_UNINIT:
 *     making one needs the group's metadata map);
 *   - a file's blocks: extent trees whose root (in the inode) is a leaf,
 *     a new block extending the last extent when it lands next to it, or
 *     a new extent while the root has room;
 *   - truncation: to nothing only, of a tree of any depth.
 * Anything past that is ERR_NOT_SUPPORTED. */
#include "ext_alloc.h"
#include "ext_int.h"

static uint64_t sb_free_blocks(const struct ext_fs *fs)
{
    uint64_t n = fs->sb.free_blocks_count_lo;
    if (fs->is64)
        n |= (uint64_t)fs->sb.free_blocks_count_hi << 32;
    return n;
}

static void sb_set_free_blocks(struct ext_fs *fs, uint64_t n)
{
    fs->sb.free_blocks_count_lo = (uint32_t)n;
    if (fs->is64)
        fs->sb.free_blocks_count_hi = (uint32_t)(n >> 32);
}

static void set_bit(uint8_t *map, uint32_t i)
{
    map[i / 8] |= (uint8_t)(1u << (i % 8));
}

static void clear_bit(uint8_t *map, uint32_t i)
{
    map[i / 8] &= (uint8_t)~(1u << (i % 8));
}

static bool test_bit(const uint8_t *map, uint32_t i)
{
    return map[i / 8] >> (i % 8) & 1;
}

/* The first clear bit in [from, n), or n. */
static uint32_t find_clear(const uint8_t *map, uint32_t from, uint32_t n)
{
    for (uint32_t i = from; i < n; i++) {
        if (i % 8 == 0 && map[i / 8] == 0xff && i + 8 <= n) {
            i += 7;
            continue;
        }
        if (!test_bit(map, i))
            return i;
    }
    return n;
}

/* A group's inode bitmap made for the first time: all free, the padding
 * after the group's inodes set (the format wants it). */
static status_t inode_bitmap_new(struct ext_fs *fs, struct ext_group *gr, struct ext_buf **out)
{
    struct ext_buf *b;
    status_t st = ext_bnew(fs, gr->inode_bitmap, &b);
    if (st != OK)
        return st;
    for (uint32_t i = fs->inodes_per_group; i < fs->block_size * 8; i++)
        set_bit(b->data, i);
    gr->flags &= (uint16_t)~EXT_BG_INODE_UNINIT;
    *out = b;
    return OK;
}

/* Take a free inode of group g, if it has one: *out_ino 0 if not. */
static status_t ialloc_group(struct ext_fs *fs, uint32_t g, bool dir, uint32_t *out_ino)
{
    struct ext_group gr;
    status_t st = ext_group_get(fs, g, &gr);
    if (st != OK || !gr.free_inodes) {
        *out_ino = 0;
        return st;
    }
    struct ext_buf *b;
    st = ext_bitmap_read(fs, g, EXT_BITMAP_INODE, &b);
    if (st == ERR_BAD_STATE)
        st = inode_bitmap_new(fs, &gr, &b);
    if (st != OK)
        return st;
    uint32_t i = find_clear(b->data, 0, fs->inodes_per_group);
    uint32_t ino = g * fs->inodes_per_group + i + 1;
    if (i == fs->inodes_per_group || ino < fs->first_ino) {
        ext_brelse(fs, b);
        return ext_damaged(fs, "inode bitmap: free count", 0, gr.inode_bitmap);
    }
    set_bit(b->data, i);
    gr.inode_bitmap_csum = ext_bitmap_csum(fs, EXT_BITMAP_INODE, b->data);
    ext_bdirty(fs, b);
    ext_brelse(fs, b);
    gr.free_inodes--;
    if (dir)
        gr.used_dirs++;
    if (i >= fs->inodes_per_group - gr.itable_unused)
        gr.itable_unused = fs->inodes_per_group - i - 1;
    st = ext_group_put(fs, g, &gr);
    if (st != OK)
        return st;
    fs->sb.free_inodes_count--;
    *out_ino = ino;
    return ext_super_write(fs);
}

status_t ext_ialloc(struct ext_fs *fs, uint32_t parent, bool dir, uint32_t *out_ino)
{
    uint32_t start = parent && parent <= fs->inodes ? (parent - 1) / fs->inodes_per_group : 0;
    for (uint32_t k = 0; k < fs->groups; k++) {
        uint32_t ino = 0;
        status_t st = ialloc_group(fs, (start + k) % fs->groups, dir, &ino);
        if (st != OK)
            return st;
        if (ino) {
            *out_ino = ino;
            return OK;
        }
    }
    return ERR_NO_SPACE;
}

status_t ext_ifree(struct ext_fs *fs, uint32_t ino, bool dir)
{
    if (ino < fs->first_ino || ino > fs->inodes)
        return ERR_OUT_OF_RANGE;
    uint32_t g = (ino - 1) / fs->inodes_per_group, i = (ino - 1) % fs->inodes_per_group;
    struct ext_group gr;
    status_t st = ext_group_get(fs, g, &gr);
    struct ext_buf *b;
    if (st == OK)
        st = ext_bitmap_read(fs, g, EXT_BITMAP_INODE, &b);
    if (st != OK)
        return st;
    if (!test_bit(b->data, i)) {
        ext_brelse(fs, b);
        return ext_damaged(fs, "inode bitmap: freeing a free inode", ino, gr.inode_bitmap);
    }
    clear_bit(b->data, i);
    gr.inode_bitmap_csum = ext_bitmap_csum(fs, EXT_BITMAP_INODE, b->data);
    ext_bdirty(fs, b);
    ext_brelse(fs, b);
    gr.free_inodes++;
    if (dir && gr.used_dirs)
        gr.used_dirs--;
    st = ext_group_put(fs, g, &gr);
    if (st != OK)
        return st;
    fs->sb.free_inodes_count++;
    return ext_super_write(fs);
}

/* Take the first free block of group g at or after bit `from`; *out 0 if none. */
static status_t balloc_group(struct ext_fs *fs, uint32_t g, uint32_t from, uint64_t *out)
{
    struct ext_group gr;
    *out = 0;
    status_t st = ext_group_get(fs, g, &gr);
    if (st != OK || !gr.free_blocks)
        return st;
    struct ext_buf *b;
    st = ext_bitmap_read(fs, g, EXT_BITMAP_BLOCK, &b);
    if (st == ERR_BAD_STATE)
        return OK;
    if (st != OK)
        return st;
    uint32_t n = ext_group_blocks(fs, g);
    uint32_t i = find_clear(b->data, from < n ? from : n, n);
    if (i == n) {
        ext_brelse(fs, b);
        return OK;
    }
    set_bit(b->data, i);
    gr.block_bitmap_csum = ext_bitmap_csum(fs, EXT_BITMAP_BLOCK, b->data);
    ext_bdirty(fs, b);
    ext_brelse(fs, b);
    gr.free_blocks--;
    st = ext_group_put(fs, g, &gr);
    if (st != OK)
        return st;
    sb_set_free_blocks(fs, sb_free_blocks(fs) - 1);
    *out = ext_group_first(fs, g) + i;
    return ext_super_write(fs);
}

/* A free block at or after goal (or anywhere after it, wrapping). */
static status_t balloc(struct ext_fs *fs, uint64_t goal, uint64_t *out)
{
    if (!ext_blk_ok(fs, goal))
        goal = fs->first_data_block + 1;
    uint32_t g0 = (uint32_t)((goal - fs->first_data_block) / fs->blocks_per_group);
    for (uint32_t k = 0; k <= fs->groups; k++) {
        uint32_t g = (g0 + k) % fs->groups;
        uint32_t from = k == 0 ? (uint32_t)(goal - ext_group_first(fs, g)) : 0;
        status_t st = balloc_group(fs, g, from, out);
        if (st != OK || *out)
            return st;
    }
    return ERR_NO_SPACE;
}

status_t ext_bfree(struct ext_fs *fs, uint64_t blk, uint64_t count)
{
    if (!ext_run_ok(fs, blk, count))
        return ERR_OUT_OF_RANGE;
    for (uint64_t k = 0; k < count; k++) {
        uint64_t at = blk + k;
        uint32_t g = (uint32_t)((at - fs->first_data_block) / fs->blocks_per_group);
        uint32_t i = (uint32_t)(at - ext_group_first(fs, g));
        struct ext_group gr;
        struct ext_buf *b;
        status_t st = ext_group_get(fs, g, &gr);
        if (st == OK)
            st = ext_bitmap_read(fs, g, EXT_BITMAP_BLOCK, &b);
        if (st != OK)
            return st;
        if (!test_bit(b->data, i)) {
            ext_brelse(fs, b);
            return ext_damaged(fs, "block bitmap: freeing a free block", 0, at);
        }
        clear_bit(b->data, i);
        gr.block_bitmap_csum = ext_bitmap_csum(fs, EXT_BITMAP_BLOCK, b->data);
        ext_bdirty(fs, b);
        ext_brelse(fs, b);
        gr.free_blocks++;
        st = ext_group_put(fs, g, &gr);
        if (st != OK)
            return st;
        sb_set_free_blocks(fs, sb_free_blocks(fs) + 1);
    }
    return ext_super_write(fs);
}

/* in's i_blocks, in 512-byte sectors, by delta blocks. */
static void add_blocks(const struct ext_fs *fs, struct ext_inode *in, int64_t delta)
{
    uint64_t n = ext_iblocks512(fs, in);
    n = (uint64_t)((int64_t)n + delta * (int64_t)(fs->block_size / 512));
    in->d.blocks_lo = (uint32_t)n;
    in->d.blocks_high = fs->huge_file ? (uint16_t)(n >> 32) : 0;
    in->d.flags &= ~EXT_HUGE_FILE_FL;
}

status_t ext_alloc_lblk(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk,
                        uint64_t *out_pblk)
{
    struct ext_extent_header *h = (struct ext_extent_header *)in->d.block;
    if (!(in->d.flags & EXT_EXTENTS_FL) || h->magic != EXT_EXTENT_MAGIC || h->depth)
        return ERR_NOT_SUPPORTED;
    struct ext_extent *e = ext_extents(h);
    int32_t at = -1;
    for (int32_t i = 0; i < h->entries && e[i].block <= lblk; i++)
        at = i;
    if (at >= 0 && (uint64_t)e[at].block + ext_extent_len(&e[at]) > lblk)
        return ERR_BAD_STATE;
    bool grow = at >= 0 && e[at].block + ext_extent_len(&e[at]) == lblk &&
                !ext_extent_unwritten(&e[at]) && e[at].len < EXT_INIT_MAX_LEN;
    if (!grow && h->entries >= h->max)
        return ERR_NOT_SUPPORTED;
    uint64_t goal = at >= 0 ? ext_extent_start(&e[at]) + (lblk - e[at].block)
                            : ext_group_first(fs, (in->ino - 1) / fs->inodes_per_group);
    uint64_t blk;
    status_t st = balloc(fs, goal, &blk);
    if (st != OK)
        return st;
    if (grow && blk == goal) {
        e[at].len++;
    } else if (h->entries < h->max) {
        for (int32_t i = h->entries; i > at + 1; i--)
            e[i] = e[i - 1];
        e[at + 1] = (struct ext_extent){ .block = lblk, .len = 1,
                                         .start_hi = (uint16_t)(blk >> 32),
                                         .start_lo = (uint32_t)blk };
        h->entries++;
    } else {
        (void)ext_bfree(fs, blk, 1);   /* the transaction is given up anyway */
        return ERR_NOT_SUPPORTED;
    }
    add_blocks(fs, in, 1);
    *out_pblk = blk;
    return OK;
}

/* Free the extents of a leaf. */
static status_t free_leaf(struct ext_fs *fs, struct ext_extent_header *h)
{
    struct ext_extent *e = ext_extents(h);
    for (uint32_t i = 0; i < h->entries; i++) {
        status_t st = ext_bfree(fs, ext_extent_start(&e[i]), ext_extent_len(&e[i]));
        if (st != OK)
            return st;
    }
    return OK;
}

/* Free every block of an extent tree, leaf by leaf (each found by its
 * first block), and each node block once the walk has left it. */
static status_t free_tree(struct ext_fs *fs, struct ext_inode *in)
{
    uint64_t node[EXT_EXTENT_MAX_DEPTH + 1] = { 0 };
    uint64_t lblk = 0;
    for (uint32_t n = 0;; n++) {
        struct ext_path p;
        status_t st = ext_extent_find(fs, in, (uint32_t)lblk, &p);
        if (st == OK)
            st = free_leaf(fs, p.lv[p.depth].h);
        for (uint32_t lv = 1; st == OK && lv <= p.depth; lv++) {
            uint64_t blk = p.lv[lv].buf->blk;
            if (node[lv] && node[lv] != blk)
                st = ext_bfree(fs, node[lv], 1);
            node[lv] = blk;
        }
        uint64_t next = p.bound;
        ext_path_release(fs, &p);
        if (st != OK)
            return st;
        if (!next)
            break;
        if (next <= lblk || n > fs->blocks)
            return ext_damaged(fs, "extent: index out of order", in->ino, 0);
        lblk = next;
    }
    for (uint32_t lv = 1; lv <= EXT_EXTENT_MAX_DEPTH; lv++)
        if (node[lv]) {
            status_t st = ext_bfree(fs, node[lv], 1);
            if (st != OK)
                return st;
        }
    return OK;
}

/* Truncation to `size` > 0 of a tree whose root is a leaf with nothing
 * past it: only the size changes. */
static status_t truncate_nothing(struct ext_fs *fs, struct ext_inode *in, uint64_t size)
{
    struct ext_extent_header *h = (struct ext_extent_header *)in->d.block;
    uint64_t end = (size + fs->block_size - 1) >> fs->block_log;
    if (!(in->d.flags & EXT_EXTENTS_FL) || h->depth)
        return ERR_NOT_SUPPORTED;
    for (uint32_t i = 0; i < h->entries; i++)
        if (ext_extents(h)[i].block + (uint64_t)ext_extent_len(&ext_extents(h)[i]) > end)
            return ERR_NOT_SUPPORTED;
    in->d.size_lo = (uint32_t)size;
    in->d.size_high = (uint32_t)(size >> 32);
    return OK;
}

status_t ext_truncate(struct ext_fs *fs, struct ext_inode *in, uint64_t size)
{
    if (size)
        return truncate_nothing(fs, in, size);
    struct ext_extent_header *h = (struct ext_extent_header *)in->d.block;
    bool xattr = in->d.file_acl_lo || (fs->is64 && in->d.file_acl_high);
    if (in->d.flags & EXT_EXTENTS_FL) {
        status_t st = free_tree(fs, in);
        if (st != OK)
            return st;
        *h = (struct ext_extent_header){ .magic = EXT_EXTENT_MAGIC, .max = 4 };
        __builtin_memset(ext_extents(h), 0, sizeof(in->d.block) - sizeof(*h));
    } else if (ext_iblocks512(fs, in) > (xattr ? fs->block_size / 512 : 0)) {
        return ERR_NOT_SUPPORTED;   /* a block map with blocks */
    }
    in->d.blocks_lo = 0;
    in->d.blocks_high = 0;
    in->d.flags &= ~EXT_HUGE_FILE_FL;
    if (xattr)
        add_blocks(fs, in, 1);
    in->d.size_lo = 0;
    in->d.size_high = 0;
    return OK;
}
