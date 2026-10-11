/* ext: every structure's checksum, computed the way ext4's format says,
 * for the readers (which compare) and the writers (which set).
 *
 * With metadata_csum each structure's CRC32C starts from the volume's seed
 * (fs->csum_seed: crc32c(~0, the UUID), or the superblock's own
 * checksum_seed with csum_seed) and covers the structure with its own
 * checksum field counted as zeros. What an inode owns (its extent nodes,
 * its directory blocks) starts from the inode's seed instead: the volume's
 * seed run on through the inode's number and generation, so a block moved
 * to another inode, or left from an inode since freed and made again,
 * doesn't pass. Where a field holds only 16 bits, the low 16 of the CRC
 * are stored. The superblock alone starts from ~0.
 *
 * Every function here takes bytes as the disk has them and reads only
 * inside the structure's size (the header's max and limit fields are
 * checked against the block before they say where the tail is), so the
 * fuzzed images of tools/extcheck.c can be checked and re-sealed with
 * them whatever they hold. */
#include "ext.h"

static uint32_t le32_crc(uint32_t crc, uint32_t v)
{
    return ext_crc32c(crc, &v, 4);   /* the value as its 4 bytes on the disk (little-endian) */
}

uint32_t ext_super_csum(const struct ext_super *sb)
{
    return ext_crc32c(~0u, sb, __builtin_offsetof(struct ext_super, checksum));
}

/* metadata_csum: CRC32C of the group's number and the descriptor, its
 * checksum field as zeros. uninit_bg: CRC16 of the UUID, the group's
 * number and the descriptor, the checksum field left out. */
uint16_t ext_group_csum(const struct ext_fs *fs, uint32_t g, const void *desc)
{
    const uint8_t *d = desc;
    uint32_t at = __builtin_offsetof(struct ext_group_desc, checksum);
    uint32_t after = at + 2;
    if (fs->csum) {
        uint16_t zero = 0;
        uint32_t c = le32_crc(fs->csum_seed, g);
        c = ext_crc32c(c, d, at);
        c = ext_crc32c(c, &zero, 2);
        c = ext_crc32c(c, d + after, fs->desc_size - after);
        return (uint16_t)c;
    }
    if (fs->gdt_csum) {
        uint16_t c = ext_crc16(0xffff, fs->sb.uuid, sizeof(fs->sb.uuid));
        c = ext_crc16(c, &g, 4);
        c = ext_crc16(c, d, at);
        if (fs->desc_size > after)
            c = ext_crc16(c, d + after, fs->desc_size - after);
        return c;
    }
    return 0;
}

uint32_t ext_inode_seed(const struct ext_fs *fs, uint32_t ino, uint32_t generation)
{
    if (!fs->csum)
        return 0;
    return le32_crc(le32_crc(fs->csum_seed, ino), generation);
}

/* An inode's CRC32C: its inode_size bytes, checksum_lo as zeros, and
 * checksum_hi as zeros when i_extra_isize covers it. The caller has
 * checked extra_isize fits inode_size. */
static uint32_t inode_crc(const struct ext_fs *fs, uint32_t seed, const uint8_t *raw)
{
    const struct ext_disk_inode *d = (const struct ext_disk_inode *)raw;
    uint32_t lo = __builtin_offsetof(struct ext_disk_inode, checksum_lo);
    uint32_t hi = __builtin_offsetof(struct ext_disk_inode, checksum_hi);
    uint16_t zero = 0;
    uint32_t c = ext_crc32c(seed, raw, lo);
    c = ext_crc32c(c, &zero, 2);
    c = ext_crc32c(c, raw + lo + 2, EXT_GOOD_OLD_INODE - (lo + 2));
    if (fs->inode_size == EXT_GOOD_OLD_INODE)
        return c;
    c = ext_crc32c(c, raw + EXT_GOOD_OLD_INODE, hi - EXT_GOOD_OLD_INODE);
    uint32_t rest = hi;
    if (EXT_GOOD_OLD_INODE + (uint32_t)d->extra_isize >= hi + 2) {
        c = ext_crc32c(c, &zero, 2);
        rest = hi + 2;
    }
    return ext_crc32c(c, raw + rest, fs->inode_size - rest);
}

/* Is the checksum_hi half there (large inodes whose extra part covers it)? */
static bool inode_has_hi(const struct ext_fs *fs, const struct ext_disk_inode *d)
{
    return fs->inode_size > EXT_GOOD_OLD_INODE &&
           EXT_GOOD_OLD_INODE + (uint32_t)d->extra_isize >=
               __builtin_offsetof(struct ext_disk_inode, checksum_hi) + 2;
}

bool ext_inode_csum_ok(const struct ext_fs *fs, uint32_t seed, const void *raw)
{
    const struct ext_disk_inode *d = raw;
    uint32_t c = inode_crc(fs, seed, raw);
    if (inode_has_hi(fs, d))
        return c == ((uint32_t)d->checksum_hi << 16 | d->checksum_lo);
    return (uint16_t)c == d->checksum_lo;
}

void ext_inode_csum_set(const struct ext_fs *fs, uint32_t seed, void *raw)
{
    struct ext_disk_inode *d = raw;
    uint32_t c = inode_crc(fs, seed, raw);
    d->checksum_lo = (uint16_t)c;
    if (inode_has_hi(fs, d))
        d->checksum_hi = (uint16_t)(c >> 16);
}

/* Where an extent node's tail lies (after `max` entries), or 0 if it
 * doesn't fit the block. */
static uint32_t extent_tail(const struct ext_fs *fs, const void *block)
{
    const struct ext_extent_header *h = block;
    uint32_t at = sizeof(*h) + (uint32_t)h->max * sizeof(struct ext_extent);
    if (at + sizeof(struct ext_extent_tail) > fs->block_size)
        return 0;
    return at;
}

bool ext_extent_csum_ok(const struct ext_fs *fs, uint32_t seed, const void *block)
{
    uint32_t at = extent_tail(fs, block);
    if (!at)
        return false;
    const struct ext_extent_tail *t = (const void *)((const uint8_t *)block + at);
    return ext_crc32c(seed, block, at) == t->checksum;
}

bool ext_extent_csum_set(const struct ext_fs *fs, uint32_t seed, void *block)
{
    uint32_t at = extent_tail(fs, block);
    if (!at)
        return false;
    struct ext_extent_tail *t = (void *)((uint8_t *)block + at);
    t->checksum = ext_crc32c(seed, block, at);
    return true;
}

bool ext_dirleaf_csum_ok(const struct ext_fs *fs, uint32_t seed, const void *block)
{
    uint32_t at = fs->block_size - EXT_DIR_TAIL_SIZE;
    const struct ext_dir_tail *t = (const void *)((const uint8_t *)block + at);
    if (t->zero1 || t->rec_len != EXT_DIR_TAIL_SIZE || t->zero2 || t->file_type != EXT_DIR_TAIL_FT)
        return false;
    return ext_crc32c(seed, block, at) == t->checksum;
}

void ext_dirleaf_csum_set(const struct ext_fs *fs, uint32_t seed, void *block)
{
    uint32_t at = fs->block_size - EXT_DIR_TAIL_SIZE;
    struct ext_dir_tail *t = (void *)((uint8_t *)block + at);
    t->zero1 = 0;
    t->rec_len = EXT_DIR_TAIL_SIZE;
    t->zero2 = 0;
    t->file_type = EXT_DIR_TAIL_FT;
    t->checksum = ext_crc32c(seed, block, at);
}

/* An index block's checksum covers its header and entries up to `count`
 * (from the block's start), then the tail's reserved word; the tail lies
 * after `limit` entries. 0: no room for it, or count past limit. */
static uint32_t dx_tail(const struct ext_fs *fs, const void *block, uint32_t count_off,
                        uint32_t *out_covered)
{
    if (count_off + sizeof(struct ext_dx_countlimit) > fs->block_size)
        return 0;
    const struct ext_dx_countlimit *cl = (const void *)((const uint8_t *)block + count_off);
    uint32_t at = count_off + (uint32_t)cl->limit * sizeof(struct ext_dx_entry);
    if (cl->count > cl->limit || at + sizeof(struct ext_dx_tail) > fs->block_size)
        return 0;
    *out_covered = count_off + (uint32_t)cl->count * sizeof(struct ext_dx_entry);
    return at;
}

static uint32_t dx_crc(uint32_t seed, const void *block, uint32_t covered, uint32_t at)
{
    uint32_t zero = 0;
    uint32_t c = ext_crc32c(seed, block, covered);
    c = ext_crc32c(c, (const uint8_t *)block + at,
                   __builtin_offsetof(struct ext_dx_tail, checksum));
    return ext_crc32c(c, &zero, sizeof(zero));   /* the checksum field, as zeros */
}

bool ext_dx_csum_ok(const struct ext_fs *fs, uint32_t seed, const void *block, uint32_t count_off)
{
    uint32_t covered;
    uint32_t at = dx_tail(fs, block, count_off, &covered);
    if (!at)
        return false;
    const struct ext_dx_tail *t = (const void *)((const uint8_t *)block + at);
    return dx_crc(seed, block, covered, at) == t->checksum;
}

bool ext_dx_csum_set(const struct ext_fs *fs, uint32_t seed, void *block, uint32_t count_off)
{
    uint32_t covered;
    uint32_t at = dx_tail(fs, block, count_off, &covered);
    if (!at)
        return false;
    struct ext_dx_tail *t = (void *)((uint8_t *)block + at);
    t->checksum = dx_crc(seed, block, covered, at);
    return true;
}

uint32_t ext_xattr_csum(const struct ext_fs *fs, uint64_t blk, const void *block)
{
    uint32_t at = __builtin_offsetof(struct ext_xattr_header, checksum), zero = 0;
    uint32_t c = ext_crc32c(fs->csum_seed, &blk, 8);
    c = ext_crc32c(c, block, at);
    c = ext_crc32c(c, &zero, 4);
    return ext_crc32c(c, (const uint8_t *)block + at + 4, fs->block_size - (at + 4));
}

uint32_t ext_bitmap_csum(const struct ext_fs *fs, enum ext_bitmap which, const void *bitmap)
{
    if (!fs->csum)
        return 0;
    uint32_t bytes = (which == EXT_BITMAP_BLOCK ? fs->blocks_per_group : fs->inodes_per_group) / 8;
    return ext_crc32c(fs->csum_seed, bitmap, bytes);
}
