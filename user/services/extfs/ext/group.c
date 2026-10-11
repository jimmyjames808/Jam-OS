/* ext: block groups. The volume is cut into groups of blocks_per_group
 * blocks from first_data_block on; each has a descriptor (in the blocks
 * after the primary superblock, without meta_bg, one desc_size slot per
 * group) naming its block bitmap, its inode bitmap and its inode table,
 * and counting its free blocks and inodes. With flex_bg the bitmaps and
 * tables of several groups lie together in the first of them, so they
 * may be anywhere in the volume; without it they lie in their own group.
 *
 * A descriptor is checked each time it is read (its checksum, a few
 * dozen bytes, costs less than remembering it): every block it names
 * inside the volume (and inside its group without flex_bg), none over the
 * primary superblock and descriptors, every count no more than a group
 * holds. ext_mount reads every one, so a volume with a bad descriptor is
 * not mounted. The flags and the never-used inodes count mean something
 * only with group checksums (metadata_csum or uninit_bg); without them
 * they read as 0. */
#include "ext_int.h"

/* Group g's descriptor's block and its offset in it. */
static void desc_loc(const struct ext_fs *fs, uint32_t g, uint64_t *blk, uint32_t *off)
{
    *blk = fs->first_data_block + 1u + g / fs->descs_per_block;
    *off = g % fs->descs_per_block * fs->desc_size;
}

static bool group_csums(const struct ext_fs *fs)
{
    return fs->csum || fs->gdt_csum;
}

static void decode(const struct ext_fs *fs, const uint8_t *raw, struct ext_group *out)
{
    struct ext_group_desc d;
    __builtin_memset(&d, 0, sizeof(d));
    __builtin_memcpy(&d, raw, fs->desc_size < sizeof(d) ? fs->desc_size : sizeof(d));
    out->block_bitmap = (uint64_t)d.block_bitmap_hi << 32 | d.block_bitmap_lo;
    out->inode_bitmap = (uint64_t)d.inode_bitmap_hi << 32 | d.inode_bitmap_lo;
    out->inode_table = (uint64_t)d.inode_table_hi << 32 | d.inode_table_lo;
    out->free_blocks = (uint32_t)d.free_blocks_count_hi << 16 | d.free_blocks_count_lo;
    out->free_inodes = (uint32_t)d.free_inodes_count_hi << 16 | d.free_inodes_count_lo;
    out->used_dirs = (uint32_t)d.used_dirs_count_hi << 16 | d.used_dirs_count_lo;
    out->itable_unused = (uint32_t)d.itable_unused_hi << 16 | d.itable_unused_lo;
    out->block_bitmap_csum = (uint32_t)d.block_bitmap_csum_hi << 16 | d.block_bitmap_csum_lo;
    out->inode_bitmap_csum = (uint32_t)d.inode_bitmap_csum_hi << 16 | d.inode_bitmap_csum_lo;
    out->flags = d.flags;
    if (!group_csums(fs)) {
        out->flags = 0;
        out->itable_unused = 0;
    }
}

uint64_t ext_group_first(const struct ext_fs *fs, uint32_t g)
{
    return fs->first_data_block + (uint64_t)g * fs->blocks_per_group;
}

uint32_t ext_group_blocks(const struct ext_fs *fs, uint32_t g)
{
    uint64_t left = fs->blocks - ext_group_first(fs, g);
    return left < fs->blocks_per_group ? (uint32_t)left : fs->blocks_per_group;
}

/* Is n a power of p (1 counts)? */
static bool power_of(uint32_t n, uint32_t p)
{
    while (n > 1 && n % p == 0)
        n /= p;
    return n == 1;
}

bool ext_group_has_super(const struct ext_fs *fs, uint32_t g)
{
    if (g == 0)
        return true;
    if (fs->sb.feature_compat & EXT_COMPAT_SPARSE_SUPER2)
        return g == fs->sb.backup_bgs[0] || g == fs->sb.backup_bgs[1];
    if (!fs->sparse_super)
        return true;
    return g == 1 || power_of(g, 3) || power_of(g, 5) || power_of(g, 7);
}

/* The blocks the primary superblock and descriptors (and resize_inode's
 * reserve) take at the volume's start: no group's metadata may be there. */
static uint64_t primary_end(const struct ext_fs *fs)
{
    uint64_t r = fs->sb.feature_compat & EXT_COMPAT_RESIZE_INODE ? fs->sb.reserved_gdt_blocks : 0;
    return fs->first_data_block + 1u + fs->gdt_blocks + r;
}

/* Is [blk, blk + n) a place for group g's metadata? */
static bool meta_ok(const struct ext_fs *fs, uint32_t g, uint64_t blk, uint64_t n)
{
    if (!ext_run_ok(fs, blk, n) || blk < primary_end(fs))
        return false;
    if (fs->flex_bg)
        return true;
    uint64_t first = ext_group_first(fs, g);
    uint64_t in = ext_group_blocks(fs, g);
    return blk >= first && n <= in && blk - first <= in - n;
}

static status_t check(struct ext_fs *fs, uint32_t g, const struct ext_group *gr, uint64_t blk)
{
    if (!meta_ok(fs, g, gr->block_bitmap, 1))
        return ext_damaged(fs, "group descriptor: block bitmap", 0, blk);
    if (!meta_ok(fs, g, gr->inode_bitmap, 1))
        return ext_damaged(fs, "group descriptor: inode bitmap", 0, blk);
    if (!meta_ok(fs, g, gr->inode_table, fs->itable_blocks))
        return ext_damaged(fs, "group descriptor: inode table", 0, blk);
    if (gr->free_blocks > fs->blocks_per_group || gr->free_inodes > fs->inodes_per_group ||
        gr->used_dirs > fs->inodes_per_group || gr->itable_unused > fs->inodes_per_group)
        return ext_damaged(fs, "group descriptor: counts", 0, blk);
    return OK;
}

status_t ext_group_get(struct ext_fs *fs, uint32_t g, struct ext_group *out)
{
    if (g >= fs->groups)
        return ERR_OUT_OF_RANGE;
    uint64_t blk;
    uint32_t off;
    desc_loc(fs, g, &blk, &off);
    struct ext_buf *b;
    status_t st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    const uint8_t *raw = b->data + off;
    const struct ext_group_desc *d = (const struct ext_group_desc *)raw;
    if (group_csums(fs) && ext_group_csum(fs, g, raw) != d->checksum) {
        ext_brelse(fs, b);
        return ext_damaged(fs, "group descriptor checksum", 0, blk);
    }
    struct ext_group gr;
    decode(fs, raw, &gr);
    ext_brelse(fs, b);
    st = check(fs, g, &gr, blk);
    if (st == OK)
        *out = gr;
    return st;
}

/* in's fields into d (desc_size bytes), keeping what in doesn't say. */
static status_t encode(const struct ext_fs *fs, const struct ext_group *in,
                       struct ext_group_desc *d)
{
    bool wide = fs->desc_size >= EXT_DESC_MIN_64;
    if (!wide && (in->block_bitmap >> 32 || in->inode_bitmap >> 32 || in->inode_table >> 32 ||
                  in->free_blocks >> 16 || in->free_inodes >> 16 || in->used_dirs >> 16 ||
                  in->itable_unused >> 16))
        return ERR_OUT_OF_RANGE;
    d->block_bitmap_lo = (uint32_t)in->block_bitmap;
    d->block_bitmap_hi = (uint32_t)(in->block_bitmap >> 32);
    d->inode_bitmap_lo = (uint32_t)in->inode_bitmap;
    d->inode_bitmap_hi = (uint32_t)(in->inode_bitmap >> 32);
    d->inode_table_lo = (uint32_t)in->inode_table;
    d->inode_table_hi = (uint32_t)(in->inode_table >> 32);
    d->free_blocks_count_lo = (uint16_t)in->free_blocks;
    d->free_blocks_count_hi = (uint16_t)(in->free_blocks >> 16);
    d->free_inodes_count_lo = (uint16_t)in->free_inodes;
    d->free_inodes_count_hi = (uint16_t)(in->free_inodes >> 16);
    d->used_dirs_count_lo = (uint16_t)in->used_dirs;
    d->used_dirs_count_hi = (uint16_t)(in->used_dirs >> 16);
    d->block_bitmap_csum_lo = (uint16_t)in->block_bitmap_csum;
    d->inode_bitmap_csum_lo = (uint16_t)in->inode_bitmap_csum;
    if (wide) {
        d->block_bitmap_csum_hi = (uint16_t)(in->block_bitmap_csum >> 16);
        d->inode_bitmap_csum_hi = (uint16_t)(in->inode_bitmap_csum >> 16);
    }
    if (group_csums(fs)) {
        d->flags = in->flags;
        d->itable_unused_lo = (uint16_t)in->itable_unused;
        d->itable_unused_hi = (uint16_t)(in->itable_unused >> 16);
    }
    return OK;
}

status_t ext_group_put(struct ext_fs *fs, uint32_t g, const struct ext_group *in)
{
    if (g >= fs->groups)
        return ERR_OUT_OF_RANGE;
    uint64_t blk;
    uint32_t off;
    desc_loc(fs, g, &blk, &off);
    struct ext_buf *b;
    status_t st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    uint8_t *raw = b->data + off;
    uint32_t n = fs->desc_size < sizeof(struct ext_group_desc) ? fs->desc_size
                                                               : sizeof(struct ext_group_desc);
    struct ext_group_desc d;
    __builtin_memset(&d, 0, sizeof(d));
    __builtin_memcpy(&d, raw, n);
    st = encode(fs, in, &d);
    if (st == OK) {
        __builtin_memcpy(raw, &d, n);
        if (group_csums(fs))
            ((struct ext_group_desc *)raw)->checksum = ext_group_csum(fs, g, raw);
        ext_bdirty(fs, b);
    }
    ext_brelse(fs, b);
    return st;
}

status_t ext_groups_check(struct ext_fs *fs)
{
    for (uint32_t g = 0; g < fs->groups; g++) {
        struct ext_group gr;
        status_t st = ext_group_get(fs, g, &gr);
        if (st != OK)
            return st;
    }
    return OK;
}

/* Does the bitmap's checksum match the one its descriptor keeps? */
static bool bitmap_ok(const struct ext_fs *fs, enum ext_bitmap which, const struct ext_group *gr,
                      const uint8_t *bitmap)
{
    uint32_t want = which == EXT_BITMAP_BLOCK ? gr->block_bitmap_csum : gr->inode_bitmap_csum;
    uint32_t got = ext_bitmap_csum(fs, which, bitmap);
    if (fs->desc_size < EXT_DESC_MIN_64)
        return (uint16_t)got == (uint16_t)want;
    return got == want;
}

status_t ext_bitmap_read(struct ext_fs *fs, uint32_t g, enum ext_bitmap which,
                         struct ext_buf **out)
{
    struct ext_group gr;
    status_t st = ext_group_get(fs, g, &gr);
    if (st != OK)
        return st;
    uint16_t uninit = which == EXT_BITMAP_BLOCK ? EXT_BG_BLOCK_UNINIT : EXT_BG_INODE_UNINIT;
    if (gr.flags & uninit)
        return ERR_BAD_STATE;
    uint64_t blk = which == EXT_BITMAP_BLOCK ? gr.block_bitmap : gr.inode_bitmap;
    struct ext_buf *b;
    st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    if (fs->csum && !ext_bchecked(b, EXT_KIND_BITMAP, (uint8_t)which, g)) {
        if (!bitmap_ok(fs, which, &gr, b->data)) {
            ext_brelse(fs, b);
            return ext_damaged(fs, which == EXT_BITMAP_BLOCK ? "block bitmap checksum"
                                                            : "inode bitmap checksum", 0, blk);
        }
        ext_bmark(b, EXT_KIND_BITMAP, (uint8_t)which, g);
    }
    *out = b;
    return OK;
}
