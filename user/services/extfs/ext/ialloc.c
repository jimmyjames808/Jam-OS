/* ext: inodes made and freed.
 *
 * Each group's inode bitmap has a bit per inode of the group's table (1:
 * in use), the bits past them set. Three counts move with it, as for
 * blocks (balloc.c): the descriptor's free inodes and directories, and
 * the superblock's free inodes. With group checksums a descriptor also
 * keeps itable_unused, the inodes at the table's end never used: a
 * reader takes them as not there (inode.c), so taking one lowers it.
 *
 * Where an inode goes: a file near its directory (the parent's group,
 * then the groups after it); a directory spread over the volume, into
 * the first flex group from the parent's on that has at least the
 * average free inodes and free blocks (else the one with the most free
 * inodes), and in it the group with the most free inodes (Linux's Orlov
 * allocator, simplified), so that the files a directory gets later have
 * room near it.
 *
 * The lowest free inode of a group is taken, so the part of a table
 * below itable_unused's line holds only inodes that were in use once: a
 * table Linux never zeroed (lazy_itable_init) shows a reader none of its
 * garbage. A new inode's bytes in the table are zeroed whole (the attributes'
 * room after the fields included, which may hold a freed inode's), then
 * filled: i_extra_isize 32 (all of struct ext_disk_inode) in a large
 * inode. A freed inode keeps its mode and gets a deletion time, as the
 * format wants of an inode that is not in use. */
#include "ext_alloc_int.h"

/* The superblock's free inodes moved by delta, written back. */
static status_t sb_inodes_add(struct ext_fs *fs, int32_t delta)
{
    uint64_t n = (uint64_t)fs->sb.free_inodes_count + (uint64_t)(int64_t)delta;
    if (n > fs->inodes)
        return ext_damaged(fs, "superblock: free inode count", 0, 0);
    fs->sb.free_inodes_count = (uint32_t)n;
    return ext_super_write(fs);
}

static uint32_t flex_size(const struct ext_fs *fs)
{
    if (!fs->flex_bg || fs->sb.log_groups_per_flex >= 31)
        return 1;
    uint32_t n = 1u << fs->sb.log_groups_per_flex;
    return n < fs->groups ? n : fs->groups;
}

/* Flex group f's free inodes and blocks, and its group with the most free
 * inodes. */
static status_t flex_counts(struct ext_fs *fs, uint32_t f, uint64_t *inodes, uint64_t *blocks,
                            uint32_t *best)
{
    uint32_t size = flex_size(fs), most = 0;
    *inodes = *blocks = 0;
    *best = f * size;
    for (uint32_t g = f * size; g < fs->groups && g < (f + 1) * size; g++) {
        struct ext_group gr;
        status_t st = ext_group_get(fs, g, &gr);
        if (st != OK)
            return st;
        *inodes += gr.free_inodes;
        *blocks += gr.free_blocks;
        if (gr.free_inodes > most) {
            most = gr.free_inodes;
            *best = g;
        }
    }
    return OK;
}

/* A group for a directory whose parent is in group pg: in the first flex
 * group from pg's on with at least the average free inodes and blocks,
 * else in the one with the most free inodes. */
static status_t dir_group(struct ext_fs *fs, uint32_t pg, uint32_t *out)
{
    uint32_t size = flex_size(fs), nflex = (fs->groups + size - 1) / size, fallback = pg;
    uint64_t avg_i = fs->sb.free_inodes_count / nflex, avg_b = ext_free_blocks(fs) / nflex;
    uint64_t most = 0;
    for (uint32_t i = 0; i < nflex; i++) {
        uint32_t f = (pg / size + i) % nflex, best;
        uint64_t inodes, blocks;
        status_t st = flex_counts(fs, f, &inodes, &blocks, &best);
        if (st != OK)
            return st;
        if (inodes && inodes >= avg_i && blocks >= avg_b) {
            *out = best;
            return OK;
        }
        if (inodes > most) {
            most = inodes;
            fallback = best;
        }
    }
    *out = fallback;
    return OK;
}

static status_t pick_group(struct ext_fs *fs, const struct ext_inode_spec *spec, bool dir,
                           uint32_t *out)
{
    uint32_t pg = spec->parent && spec->parent <= fs->inodes
                      ? (spec->parent - 1) / fs->inodes_per_group : 0;
    if (!fs->sb.free_inodes_count)
        return ERR_NO_SPACE;
    if (dir)
        return dir_group(fs, pg, out);
    for (uint32_t i = 0; i < fs->groups; i++) {
        uint32_t g = (uint32_t)(((uint64_t)pg + i) % fs->groups);
        struct ext_group gr;
        status_t st = ext_group_get(fs, g, &gr);
        if (st != OK)
            return st;
        if (gr.free_inodes) {
            *out = g;
            return OK;
        }
    }
    return ext_damaged(fs, "superblock: free inodes counted, none found", 0, 0);
}

/* Take a free inode of group g: its number into *out. */
static status_t take(struct ext_fs *fs, uint32_t g, bool dir, uint32_t *out)
{
    struct ext_group gr;
    struct ext_buf *b;
    status_t st = ext_group_bitmap(fs, g, EXT_BITMAP_INODE, &gr, &b);
    if (st != OK)
        return st;
    uint32_t ipg = fs->inodes_per_group;
    uint32_t from = g == 0 ? fs->first_ino - 1 : 0;   /* below first_ino: reserved */
    uint32_t at = from < ipg ? ext_bit_find_clear(b->data, from, ipg) : ipg;
    if (at == ipg || !gr.free_inodes) {
        ext_brelse(fs, b);
        return ext_damaged(fs, "group descriptor: free inodes", 0, b->blk);
    }
    ext_bits_set(b->data, at, 1);
    gr.free_inodes--;
    gr.used_dirs += dir;
    if (at >= ipg - gr.itable_unused)
        gr.itable_unused = ipg - at - 1;
    st = ext_group_bitmap_seal(fs, g, EXT_BITMAP_INODE, &gr, b);
    ext_brelse(fs, b);
    if (st == OK)
        st = sb_inodes_add(fs, -1);
    if (st == OK)
        *out = g * ipg + at + 1;
    return st;
}

/* Fill a new inode from spec (its table bytes already zero). */
static status_t fill(const struct ext_fs *fs, const struct ext_inode_spec *spec,
                     struct ext_inode *in)
{
    __builtin_memset(in, 0, sizeof(*in));
    struct ext_disk_inode *d = &in->d;
    d->mode = spec->mode;
    d->uid = (uint16_t)spec->uid;
    d->uid_high = (uint16_t)(spec->uid >> 16);
    d->gid = (uint16_t)spec->gid;
    d->gid_high = (uint16_t)(spec->gid >> 16);
    d->links_count = spec->links;
    d->generation = spec->generation;
    if (fs->inode_size > EXT_GOOD_OLD_INODE) {
        uint32_t room = fs->inode_size - EXT_GOOD_OLD_INODE;
        uint32_t want = sizeof(struct ext_disk_inode) - EXT_GOOD_OLD_INODE;
        d->extra_isize = (uint16_t)(room < want ? room : want);
    }
    enum ext_ftype t = ext_mode_type(spec->mode);
    if (t == EXT_FT_REG || t == EXT_FT_DIR)
        ext_extent_init(in);
    static const enum ext_time_which all[] = { EXT_ATIME, EXT_CTIME, EXT_MTIME, EXT_CRTIME };
    for (uint32_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (all[i] == EXT_CRTIME && !ext_ifits(in, sizeof(struct ext_disk_inode)))
            continue;
        status_t st = ext_itime_set(in, all[i], &spec->time);
        if (st != OK)
            return st;
    }
    return OK;
}

status_t ext_inode_new(struct ext_fs *fs, const struct ext_inode_spec *spec,
                       struct ext_inode *out)
{
    status_t st = ext_writable(fs);
    if (st != OK)
        return st;
    if (ext_mode_type(spec->mode) == EXT_FT_UNKNOWN || spec->time.nsec > 999999999)
        return ERR_INVALID_ARGS;
    bool dir = ext_mode_type(spec->mode) == EXT_FT_DIR;
    struct ext_inode in;
    st = fill(fs, spec, &in);   /* first, so a time out of range takes no inode */
    uint32_t g = 0, ino = 0;
    if (st == OK)
        st = pick_group(fs, spec, dir, &g);
    if (st == OK)
        st = take(fs, g, dir, &ino);
    uint64_t blk;
    uint32_t off;
    if (st == OK)
        st = ext_iloc(fs, ino, &blk, &off);
    struct ext_buf *b;
    if (st == OK)
        st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    __builtin_memset(b->data + off, 0, fs->inode_size);   /* pinned until ext_iput wrote it */
    in.ino = ino;
    in.seed = ext_inode_seed(fs, ino, spec->generation);
    st = ext_iput(fs, &in);
    ext_brelse(fs, b);
    if (st == OK)
        *out = in;
    return st;
}

/* Does in still have blocks in a block map (ext2's and ext3's)? */
static bool has_block_map(const struct ext_inode *in)
{
    enum ext_ftype t = ext_itype(in);
    if (in->d.flags & EXT_EXTENTS_FL)
        return false;
    if (t == EXT_FT_SYMLINK && ext_isize(in) < sizeof(in->d.block))
        return false;   /* a fast link: its target, not blocks */
    if (t != EXT_FT_REG && t != EXT_FT_DIR && t != EXT_FT_SYMLINK)
        return false;
    for (uint32_t i = 0; i < EXT_N_BLOCKS; i++)
        if (in->d.block[i])
            return true;
    return false;
}

/* Let go of in's attribute block: freed if no other inode shares it. */
static status_t release_xattr(struct ext_fs *fs, struct ext_inode *in)
{
    struct ext_buf *b;
    status_t st = ext_xattr_block(fs, in, &b);
    if (st == ERR_NOT_FOUND)
        return OK;
    if (st != OK)
        return st;
    uint64_t blk = b->blk;
    struct ext_xattr_header *h = (struct ext_xattr_header *)b->data;
    if (h->refcount > 1) {
        h->refcount--;
        if (fs->csum)
            h->checksum = ext_xattr_csum(fs, blk, b->data);
        ext_bdirty(fs, b);
        ext_brelse(fs, b);
    } else {
        ext_brelse(fs, b);
        st = ext_bfree(fs, blk, 1);
    }
    if (st == OK)
        st = ext_iblocks_add(fs, in, -1);
    if (st == OK) {
        in->d.file_acl_lo = 0;
        if (fs->is64)
            in->d.file_acl_high = 0;
    }
    return st;
}

/* Clear ino's bit in its group's inode bitmap; the counts raised. */
static status_t give(struct ext_fs *fs, uint32_t ino, bool dir)
{
    uint32_t g = (ino - 1) / fs->inodes_per_group, at = (ino - 1) % fs->inodes_per_group;
    struct ext_group gr;
    struct ext_buf *b;
    status_t st = ext_group_bitmap(fs, g, EXT_BITMAP_INODE, &gr, &b);
    if (st != OK)
        return st;
    if (!ext_bit(b->data, at) || gr.free_inodes >= fs->inodes_per_group || (dir && !gr.used_dirs)) {
        ext_brelse(fs, b);
        return ext_damaged(fs, "freeing an inode not in use", ino, b->blk);
    }
    ext_bits_clear(b->data, at, 1);
    gr.free_inodes++;
    gr.used_dirs -= dir;
    st = ext_group_bitmap_seal(fs, g, EXT_BITMAP_INODE, &gr, b);
    ext_brelse(fs, b);
    if (st == OK)
        st = sb_inodes_add(fs, 1);
    return st;
}

status_t ext_inode_free(struct ext_fs *fs, struct ext_inode *in, uint32_t dtime)
{
    status_t st = ext_writable(fs);
    if (st != OK)
        return st;
    if (in->ino < fs->first_ino || in->ino > fs->inodes)
        return ERR_INVALID_ARGS;   /* the reserved inodes are never freed */
    if (in->d.flags & EXT_EXTENTS_FL) {
        const struct ext_extent_header *h = (const struct ext_extent_header *)in->d.block;
        if (h->entries)
            return ERR_BAD_STATE;
    } else if (has_block_map(in)) {
        return ERR_NOT_SUPPORTED;
    }
    st = release_xattr(fs, in);
    if (st == OK)
        st = give(fs, in->ino, ext_itype(in) == EXT_FT_DIR);
    if (st != OK)
        return st;
    in->d.links_count = 0;
    /* A deletion time no larger than the inode count reads to a checker
     * (Linux's e2fsck) as the next link of an orphan list, and 0 as an
     * inode still in use: either is damage on a volume Linux checks. */
    if (dtime <= fs->inodes)
        dtime = fs->inodes < UINT32_MAX ? fs->inodes + 1 : UINT32_MAX;
    in->d.dtime = dtime;
    return ext_iput(fs, in);
}
