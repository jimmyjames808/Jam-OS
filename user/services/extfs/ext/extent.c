/* ext: extent trees. A file's blocks are runs (extents: a first logical
 * block, a length, a first physical block) in a tree whose root is the
 * inode's i_block: a 12-byte header and room for 4 entries. A leaf
 * (depth 0) holds extents, a node above it index entries, each the
 * first logical block its child covers and the child's block; a child is
 * a whole block, a header, (block_size - 12) / 12 entries' room and, with
 * metadata_csum, a 4-byte checksum after the room (the inode's seed). At
 * most EXT_EXTENT_MAX_DEPTH levels below the root.
 *
 * Every node is checked when the walk reaches it (and remembered as
 * checked on its buffer, kind EXT_KIND_EXTENT with its depth): its
 * checksum, its magic, its entries within its room, its depth one less
 * than its parent's (so a walk ends: a node can't lead back up), its
 * entries in strictly increasing order of logical block, every extent at
 * least one block long, inside the volume, ending by 2^32 and before the
 * next one starts, every child inside the volume.
 *
 * A walk follows, at each level, the last entry whose first block is at
 * or before the one asked for (the first entry if there is none, so the
 * path always reaches a leaf, as a writer inserting before the first
 * extent needs). */
#include "ext_int.h"

/* Entries a node has room for: in the inode, or in a block. */
static uint32_t room(const struct ext_fs *fs, bool root)
{
    if (root)
        return (EXT_N_BLOCKS * 4 - sizeof(struct ext_extent_header)) / sizeof(struct ext_extent);
    return (fs->block_size - sizeof(struct ext_extent_header)) / sizeof(struct ext_extent);
}

static status_t check_leaf(struct ext_fs *fs, struct ext_inode *in, struct ext_extent_header *h,
                           uint64_t blk)
{
    struct ext_extent *e = ext_extents(h);
    for (uint32_t i = 0; i < h->entries; i++) {
        uint32_t len = ext_extent_len(&e[i]);
        if (!len || (uint64_t)e[i].block + len > (1ull << 32))
            return ext_damaged(fs, "extent: length", in->ino, blk);
        if (!ext_run_ok(fs, ext_extent_start(&e[i]), len))
            return ext_damaged(fs, "extent: blocks outside the volume", in->ino, blk);
        if (i && (uint64_t)e[i - 1].block + ext_extent_len(&e[i - 1]) > e[i].block)
            return ext_damaged(fs, "extent: out of order", in->ino, blk);
    }
    return OK;
}

static status_t check_index(struct ext_fs *fs, struct ext_inode *in, struct ext_extent_header *h,
                            uint64_t blk)
{
    struct ext_extent_idx *ix = ext_idxs(h);
    if (!h->entries)
        return ext_damaged(fs, "extent: empty index", in->ino, blk);
    for (uint32_t i = 0; i < h->entries; i++) {
        if (!ext_blk_ok(fs, ext_idx_leaf(&ix[i])))
            return ext_damaged(fs, "extent: index outside the volume", in->ino, blk);
        if (i && ix[i - 1].block >= ix[i].block)
            return ext_damaged(fs, "extent: index out of order", in->ino, blk);
    }
    return OK;
}

/* Check node h (blk: its block, 0 for the root) at `depth`. */
static status_t check_node(struct ext_fs *fs, struct ext_inode *in, struct ext_extent_header *h,
                           uint32_t depth, uint64_t blk)
{
    if (h->magic != EXT_EXTENT_MAGIC)
        return ext_damaged(fs, "extent: magic", in->ino, blk);
    if (h->max > room(fs, blk == 0) || h->entries > h->max)
        return ext_damaged(fs, "extent: entries", in->ino, blk);
    if (h->depth != depth || depth > EXT_EXTENT_MAX_DEPTH)
        return ext_damaged(fs, "extent: depth", in->ino, blk);
    return depth ? check_index(fs, in, h, blk) : check_leaf(fs, in, h, blk);
}

/* The last entry whose first block is <= lblk, or -1. Entries are
 * 12 bytes, each starting with its first block, extents and indexes
 * alike. */
static int32_t search(struct ext_extent_header *h, uint32_t lblk)
{
    const struct ext_extent *e = ext_extents(h);
    int32_t lo = 0, hi = (int32_t)h->entries - 1, at = -1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (e[mid].block <= lblk) {
            at = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return at;
}

/* Read child block blk of `in` at `depth`, checked. */
static status_t read_node(struct ext_fs *fs, struct ext_inode *in, uint64_t blk, uint32_t depth,
                          struct ext_buf **out)
{
    struct ext_buf *b;
    status_t st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    if (!ext_bchecked(b, EXT_KIND_EXTENT, (uint8_t)depth, in->seed)) {
        if (fs->csum && !ext_extent_csum_ok(fs, in->seed, b->data))
            st = ext_damaged(fs, "extent block checksum", in->ino, blk);
        if (st == OK)
            st = check_node(fs, in, (struct ext_extent_header *)b->data, depth, blk);
        if (st != OK) {
            ext_brelse(fs, b);
            return st;
        }
        ext_bmark(b, EXT_KIND_EXTENT, (uint8_t)depth, in->seed);
    }
    *out = b;
    return OK;
}

status_t ext_extent_find(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, struct ext_path *p)
{
    __builtin_memset(p, 0, sizeof(*p));
    if (!(in->d.flags & EXT_EXTENTS_FL))
        return ERR_INVALID_ARGS;
    struct ext_extent_header *h = (struct ext_extent_header *)in->d.block;
    status_t st = check_node(fs, in, h, h->depth, 0);
    if (st != OK)
        return st;
    p->depth = h->depth;
    for (uint32_t lv = 0;; lv++) {
        int32_t at = search(h, lblk);
        p->lv[lv].h = h;
        if (lv == p->depth) {
            p->lv[lv].at = at;
            return OK;
        }
        uint32_t follow = at < 0 ? 0 : (uint32_t)at;
        struct ext_extent_idx *ix = ext_idxs(h);
        p->lv[lv].at = (int32_t)follow;
        if (follow + 1 < h->entries && (!p->bound || ix[follow + 1].block < p->bound))
            p->bound = ix[follow + 1].block;
        struct ext_buf *b;
        st = read_node(fs, in, ext_idx_leaf(&ix[follow]), p->depth - lv - 1, &b);
        if (st != OK)
            return st;
        p->lv[lv + 1].buf = b;
        h = (struct ext_extent_header *)b->data;
    }
}

void ext_path_release(struct ext_fs *fs, struct ext_path *p)
{
    for (uint32_t lv = 0; lv <= EXT_EXTENT_MAX_DEPTH; lv++) {
        if (p->lv[lv].buf)
            ext_brelse(fs, p->lv[lv].buf);
        p->lv[lv].buf = NULL;
    }
}
