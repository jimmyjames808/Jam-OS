/* ext: the mount. The superblock is read, checked and turned into the
 * volume's shape (struct ext_fs); the feature table below decides what
 * may be done with it; the cache is made; every group descriptor is
 * checked (group.c); the root and the journal inode are read. Nothing is
 * written.
 *
 * The feature table is docs/M13.6-PLAN.md's subset. Each known bit is
 * RW (read and written here), RO (read here, never written: the volume
 * mounts read-only) or REFUSE (not mounted: reading it here would be
 * wrong). Bits not in the table: an unknown incompatible bit refuses, as
 * the format says; an unknown compatible or read-only-compatible bit
 * makes the volume read-only, since a writer must never change what it
 * doesn't know. The journal's "needs recovery" bit is RW but marks the
 * volume EXT_RO_RECOVER until a replay (the journal's code) clears it.
 *
 * Every superblock field the library computes with is checked here, so
 * the rest of the library can trust struct ext_fs: sizes powers of two in
 * the format's ranges, the groups' count and the inodes' count agreeing,
 * the volume inside the device, the descriptors inside group 0. */
#include "ext_int.h"

enum verdict { RW, RO, REFUSE };

struct feature {
    uint8_t set;              /* enum ext_fset */
    uint8_t verdict;          /* enum verdict */
    uint32_t bit;
    const char *name;         /* as mke2fs and tune2fs name it */
};

static const struct feature features[] = {
    { EXT_FSET_COMPAT, RO, EXT_COMPAT_DIR_PREALLOC, "dir_prealloc" },
    { EXT_FSET_COMPAT, RO, EXT_COMPAT_IMAGIC_INODES, "imagic_inodes" },
    { EXT_FSET_COMPAT, RW, EXT_COMPAT_HAS_JOURNAL, "has_journal" },
    { EXT_FSET_COMPAT, RW, EXT_COMPAT_EXT_ATTR, "ext_attr" },
    { EXT_FSET_COMPAT, RW, EXT_COMPAT_RESIZE_INODE, "resize_inode" },
    { EXT_FSET_COMPAT, RW, EXT_COMPAT_DIR_INDEX, "dir_index" },
    { EXT_FSET_COMPAT, RO, EXT_COMPAT_LAZY_BG, "lazy_bg" },
    { EXT_FSET_COMPAT, RO, EXT_COMPAT_EXCLUDE_INODE, "exclude_inode" },
    { EXT_FSET_COMPAT, RO, EXT_COMPAT_EXCLUDE_BITMAP, "exclude_bitmap" },
    { EXT_FSET_COMPAT, RO, EXT_COMPAT_SPARSE_SUPER2, "sparse_super2" },
    { EXT_FSET_COMPAT, RO, EXT_COMPAT_FAST_COMMIT, "fast_commit" },
    { EXT_FSET_COMPAT, RO, EXT_COMPAT_STABLE_INODES, "stable_inodes" },
    { EXT_FSET_COMPAT, REFUSE, EXT_COMPAT_ORPHAN_FILE, "orphan_file" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_COMPRESSION, "compression" },
    { EXT_FSET_INCOMPAT, RW, EXT_INCOMPAT_FILETYPE, "filetype" },
    { EXT_FSET_INCOMPAT, RW, EXT_INCOMPAT_RECOVER, "needs_recovery" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_JOURNAL_DEV, "journal_dev" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_META_BG, "meta_bg" },
    { EXT_FSET_INCOMPAT, RW, EXT_INCOMPAT_EXTENTS, "extent" },
    { EXT_FSET_INCOMPAT, RW, EXT_INCOMPAT_64BIT, "64bit" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_MMP, "mmp" },
    { EXT_FSET_INCOMPAT, RW, EXT_INCOMPAT_FLEX_BG, "flex_bg" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_EA_INODE, "ea_inode" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_DIRDATA, "dirdata" },
    { EXT_FSET_INCOMPAT, RW, EXT_INCOMPAT_CSUM_SEED, "metadata_csum_seed" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_LARGEDIR, "large_dir" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_INLINE_DATA, "inline_data" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_ENCRYPT, "encrypt" },
    { EXT_FSET_INCOMPAT, REFUSE, EXT_INCOMPAT_CASEFOLD, "casefold" },
    { EXT_FSET_RO_COMPAT, RW, EXT_RO_COMPAT_SPARSE_SUPER, "sparse_super" },
    { EXT_FSET_RO_COMPAT, RW, EXT_RO_COMPAT_LARGE_FILE, "large_file" },
    { EXT_FSET_RO_COMPAT, RO, EXT_RO_COMPAT_BTREE_DIR, "btree_dir" },
    { EXT_FSET_RO_COMPAT, RW, EXT_RO_COMPAT_HUGE_FILE, "huge_file" },
    { EXT_FSET_RO_COMPAT, RO, EXT_RO_COMPAT_GDT_CSUM, "uninit_bg" },
    { EXT_FSET_RO_COMPAT, RW, EXT_RO_COMPAT_DIR_NLINK, "dir_nlink" },
    { EXT_FSET_RO_COMPAT, RW, EXT_RO_COMPAT_EXTRA_ISIZE, "extra_isize" },
    { EXT_FSET_RO_COMPAT, RO, EXT_RO_COMPAT_HAS_SNAPSHOT, "snapshot" },
    { EXT_FSET_RO_COMPAT, RO, EXT_RO_COMPAT_QUOTA, "quota" },
    { EXT_FSET_RO_COMPAT, REFUSE, EXT_RO_COMPAT_BIGALLOC, "bigalloc" },
    { EXT_FSET_RO_COMPAT, RW, EXT_RO_COMPAT_METADATA_CSUM, "metadata_csum" },
    { EXT_FSET_RO_COMPAT, RO, EXT_RO_COMPAT_REPLICA, "replica" },
    { EXT_FSET_RO_COMPAT, RO, EXT_RO_COMPAT_READONLY, "read-only" },
    { EXT_FSET_RO_COMPAT, RO, EXT_RO_COMPAT_PROJECT, "project" },
    { EXT_FSET_RO_COMPAT, RO, EXT_RO_COMPAT_SHARED_BLOCKS, "shared_blocks" },
    { EXT_FSET_RO_COMPAT, RO, EXT_RO_COMPAT_VERITY, "verity" },
    { EXT_FSET_RO_COMPAT, REFUSE, EXT_RO_COMPAT_ORPHAN_PRESENT, "orphan_present" },
};

static const struct feature *feature(enum ext_fset set, uint32_t bit)
{
    for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); i++)
        if (features[i].set == set && features[i].bit == bit)
            return &features[i];
    return NULL;
}

const char *ext_feature_name(enum ext_fset set, uint32_t bit)
{
    const struct feature *f = feature(set, bit);
    return f ? f->name : NULL;
}

status_t ext_damaged(struct ext_fs *fs, const char *why, uint32_t ino, uint64_t blk)
{
    fs->why = why;
    fs->why_ino = ino;
    fs->why_blk = blk;
    fs->ro |= EXT_RO_DAMAGED;
    return ERR_IO;
}

/* Refuse the mount: not ext, or not readable here. */
static status_t refuse(struct ext_fs *fs, const char *why)
{
    fs->why = why;
    return ERR_NOT_SUPPORTED;
}

/* Sort each set bit of the three feature words into fs->ro_features and
 * fs->refused. */
static void judge_features(struct ext_fs *fs)
{
    const uint32_t words[3] = { fs->sb.feature_compat, fs->sb.feature_incompat,
                                fs->sb.feature_ro_compat };
    for (uint32_t set = 0; set < 3; set++)
        for (uint32_t i = 0; i < 32; i++) {
            uint32_t bit = 1u << i;
            if (!(words[set] & bit))
                continue;
            const struct feature *f = feature((enum ext_fset)set, bit);
            uint8_t v = f ? f->verdict : set == EXT_FSET_INCOMPAT ? REFUSE : RO;
            if (v == REFUSE)
                fs->refused[set] |= bit;
            else if (v == RO)
                fs->ro_features[set] |= bit;
        }
}

/* The block size and the groups' sizes. */
static status_t check_sizes(struct ext_fs *fs)
{
    const struct ext_super *sb = &fs->sb;
    if (sb->log_block_size > EXT_MAX_BLOCK_LOG - EXT_MIN_BLOCK_LOG)
        return ext_damaged(fs, "superblock: block size", 0, 0);
    fs->block_log = EXT_MIN_BLOCK_LOG + sb->log_block_size;
    fs->block_size = 1u << fs->block_log;
    uint32_t bs = fs->block_size;
    if (sb->log_cluster_size != sb->log_block_size ||
        sb->clusters_per_group != sb->blocks_per_group)
        return ext_damaged(fs, "superblock: cluster size", 0, 0);
    if (sb->first_data_block != (bs == 1024 ? 1u : 0u))
        return ext_damaged(fs, "superblock: first data block", 0, 0);
    fs->first_data_block = sb->first_data_block;
    if (sb->blocks_per_group < 8 || sb->blocks_per_group > 8 * bs || sb->blocks_per_group % 8)
        return ext_damaged(fs, "superblock: blocks per group", 0, 0);
    fs->blocks_per_group = sb->blocks_per_group;
    uint32_t isz = sb->inode_size;
    if (isz < EXT_GOOD_OLD_INODE || isz > bs || (isz & (isz - 1)))
        return ext_damaged(fs, "superblock: inode size", 0, 0);
    fs->inode_size = isz;
    fs->inodes_per_block = bs / isz;
    uint32_t ipg = sb->inodes_per_group;
    if (ipg < fs->inodes_per_block || ipg > 8 * bs || ipg % 8 || ipg % fs->inodes_per_block)
        return ext_damaged(fs, "superblock: inodes per group", 0, 0);
    fs->inodes_per_group = ipg;
    fs->itable_blocks = ipg / fs->inodes_per_block;
    if (fs->is64) {
        uint32_t ds = sb->desc_size;
        if (ds < EXT_DESC_MIN_64 || ds > EXT_DESC_MAX || (ds & (ds - 1)))
            return ext_damaged(fs, "superblock: descriptor size", 0, 0);
        fs->desc_size = ds;
    } else {
        fs->desc_size = EXT_DESC_MIN;
    }
    fs->descs_per_block = bs / fs->desc_size;
    fs->addr_per_block = bs / 4;
    return OK;
}

/* The volume's length, the groups, the inodes, the descriptors' blocks. */
static status_t check_counts(struct ext_fs *fs)
{
    const struct ext_super *sb = &fs->sb;
    uint64_t blocks = sb->blocks_count_lo;
    if (fs->is64)
        blocks |= (uint64_t)sb->blocks_count_hi << 32;
    if (blocks <= fs->first_data_block + 1u)
        return ext_damaged(fs, "superblock: block count", 0, 0);
    if (blocks > fs->dev.bytes >> fs->block_log)
        return ext_damaged(fs, "the volume is larger than its device", 0, 0);
    fs->blocks = blocks;
    uint64_t groups = (blocks - fs->first_data_block + fs->blocks_per_group - 1) /
                      fs->blocks_per_group;
    if (groups > UINT32_MAX || groups * fs->inodes_per_group != sb->inodes_count)
        return ext_damaged(fs, "superblock: inode count", 0, 0);
    fs->groups = (uint32_t)groups;
    fs->inodes = sb->inodes_count;
    uint64_t gdt = (groups * fs->desc_size + fs->block_size - 1) >> fs->block_log;
    uint64_t reserved = sb->feature_compat & EXT_COMPAT_RESIZE_INODE ? sb->reserved_gdt_blocks : 0;
    if (1 + gdt + reserved > fs->blocks_per_group || fs->first_data_block + 1 + gdt >= blocks)
        return ext_damaged(fs, "superblock: descriptors don't fit group 0", 0, 0);
    fs->gdt_blocks = (uint32_t)gdt;
    if (sb->first_ino < EXT_FIRST_INO_MIN || sb->first_ino >= fs->inodes)
        return ext_damaged(fs, "superblock: first inode", 0, 0);
    fs->first_ino = sb->first_ino;
    uint32_t extra = fs->inode_size - EXT_GOOD_OLD_INODE;
    if ((sb->feature_ro_compat & EXT_RO_COMPAT_EXTRA_ISIZE) &&
        (sb->min_extra_isize > extra || sb->want_extra_isize > extra))
        return ext_damaged(fs, "superblock: extra inode size", 0, 0);
    if ((sb->feature_incompat & EXT_INCOMPAT_FLEX_BG) && sb->log_groups_per_flex > 31)
        return ext_damaged(fs, "superblock: groups per flex group", 0, 0);
    return OK;
}

/* The superblock's own checksum, and the seed of every other one. */
static status_t check_csum(struct ext_fs *fs)
{
    const struct ext_super *sb = &fs->sb;
    if (fs->csum) {
        if (sb->checksum_type != EXT_CRC32C)
            return ext_damaged(fs, "superblock: checksum type", 0, 0);
        if (ext_super_csum(sb) != sb->checksum)
            return ext_damaged(fs, "superblock checksum", 0, 0);
        if (sb->feature_incompat & EXT_INCOMPAT_CSUM_SEED)
            fs->csum_seed = sb->checksum_seed;
        else
            fs->csum_seed = ext_crc32c(~0u, sb->uuid, sizeof(sb->uuid));
    }
    return OK;
}

/* The journal: in an inode of this volume (an external one is refused). */
static status_t check_journal(struct ext_fs *fs)
{
    const struct ext_super *sb = &fs->sb;
    if (!fs->has_journal)
        return OK;
    if (sb->journal_dev || !sb->journal_inum)
        return refuse(fs, "external journal");
    if (sb->journal_inum > fs->inodes)
        return ext_damaged(fs, "superblock: journal inode", 0, 0);
    return OK;
}

/* Read and check the superblock into fs. */
static status_t read_super(struct ext_fs *fs)
{
    struct ext_super *sb = &fs->sb;
    if (fs->dev.bytes < EXT_SUPER_OFFSET + EXT_SUPER_SIZE)
        return refuse(fs, "too small for ext");
    if (fs->dev.read(fs->dev.ctx, EXT_SUPER_OFFSET, EXT_SUPER_SIZE, sb) != OK) {
        fs->why = "superblock: read failed";
        return ERR_IO;
    }
    if (sb->magic != EXT_MAGIC)
        return refuse(fs, "not ext (no superblock)");
    if (sb->rev_level < EXT_DYNAMIC_REV)
        return refuse(fs, "ext revision 0");
    judge_features(fs);
    if (fs->refused[0] | fs->refused[1] | fs->refused[2])
        return refuse(fs, "a feature not read here");
    fs->csum = sb->feature_ro_compat & EXT_RO_COMPAT_METADATA_CSUM;
    fs->gdt_csum = !fs->csum && (sb->feature_ro_compat & EXT_RO_COMPAT_GDT_CSUM);
    fs->is64 = sb->feature_incompat & EXT_INCOMPAT_64BIT;
    fs->filetype = sb->feature_incompat & EXT_INCOMPAT_FILETYPE;
    fs->dir_index = sb->feature_compat & EXT_COMPAT_DIR_INDEX;
    fs->huge_file = sb->feature_ro_compat & EXT_RO_COMPAT_HUGE_FILE;
    fs->flex_bg = sb->feature_incompat & EXT_INCOMPAT_FLEX_BG;
    fs->sparse_super = sb->feature_ro_compat & EXT_RO_COMPAT_SPARSE_SUPER;
    fs->has_journal = sb->feature_compat & EXT_COMPAT_HAS_JOURNAL;
    fs->needs_recovery = sb->feature_incompat & EXT_INCOMPAT_RECOVER;
    status_t st = check_csum(fs);
    if (st == OK)
        st = check_sizes(fs);
    if (st == OK)
        st = check_counts(fs);
    if (st == OK)
        st = check_journal(fs);
    return st;
}

/* What may be done with it: fs->ro. */
static void judge_ro(struct ext_fs *fs, uint32_t flags)
{
    const struct ext_super *sb = &fs->sb;
    if ((flags & EXT_MOUNT_READ_ONLY) || !fs->dev.write)
        fs->ro |= EXT_RO_ASKED;
    if (fs->ro_features[0] | fs->ro_features[1] | fs->ro_features[2])
        fs->ro |= EXT_RO_FEATURES;
    if ((sb->feature_compat & EXT_RW_NEEDS_COMPAT) != EXT_RW_NEEDS_COMPAT ||
        (sb->feature_incompat & EXT_RW_NEEDS_INCOMPAT) != EXT_RW_NEEDS_INCOMPAT ||
        (sb->feature_ro_compat & EXT_RW_NEEDS_RO_COMPAT) != EXT_RW_NEEDS_RO_COMPAT)
        fs->ro |= EXT_RO_LACKS;
    if (sb->state & EXT_STATE_ERROR)
        fs->ro |= EXT_RO_ERRORS;
    if (fs->needs_recovery)
        fs->ro |= EXT_RO_RECOVER;
}

/* The root must be a directory; the journal inode a file. */
static status_t check_inodes(struct ext_fs *fs)
{
    struct ext_inode in;
    status_t st = ext_iget(fs, EXT_ROOT_INO, 0, &in);
    if (st == ERR_NOT_FOUND || (st == OK && ext_itype(&in) != EXT_FT_DIR))
        return ext_damaged(fs, "the root is not a directory", EXT_ROOT_INO, 0);
    if (st != OK || !fs->has_journal)
        return st;
    st = ext_iget(fs, fs->sb.journal_inum, 0, &in);
    if (st == ERR_NOT_FOUND || (st == OK && ext_itype(&in) != EXT_FT_REG))
        return ext_damaged(fs, "the journal is not a file", fs->sb.journal_inum, 0);
    return st;
}

status_t ext_mount(struct ext_fs *fs, const struct ext_dev *dev, void *mem, size_t mem_size,
                   uint32_t flags)
{
    __builtin_memset(fs, 0, sizeof(*fs));
    if (!dev || !dev->read || !mem || (flags & ~(uint32_t)EXT_MOUNT_READ_ONLY))
        return ERR_INVALID_ARGS;
    fs->dev = *dev;
    status_t st = read_super(fs);
    if (st == OK)
        st = ext_cache_init(fs, mem, mem_size);
    if (st == OK)
        st = ext_groups_check(fs);
    if (st == OK)
        st = check_inodes(fs);
    if (st != OK)
        return st;
    judge_ro(fs, flags);
    return OK;
}

status_t ext_super_write(struct ext_fs *fs)
{
    uint64_t blk = fs->block_size == 1024 ? 1 : 0;
    uint32_t off = fs->block_size == 1024 ? 0 : EXT_SUPER_OFFSET;
    struct ext_buf *b;
    status_t st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    if (fs->csum)
        fs->sb.checksum = ext_super_csum(&fs->sb);
    __builtin_memcpy(b->data + off, &fs->sb, sizeof(fs->sb));
    ext_bdirty(fs, b);
    ext_brelse(fs, b);
    return OK;
}
