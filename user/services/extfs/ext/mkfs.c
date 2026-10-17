/* ext: making a volume (ext_mkfs.h says what it makes).
 *
 * The shape comes first, all of it computed before a block is written:
 * the groups (a last group too small for its own metadata is left off the
 * volume), the inodes per group (a multiple of what a block and a bitmap
 * byte hold), the flex groups (halved until each one's bitmaps and tables
 * fit in its first group), then where the journal, the root directory and
 * lost+found go. The shape is kept in a struct ext_fs filled as ext_mount
 * would fill it, so the library's own checksum and group functions (and
 * nothing else) serve both.
 *
 * Choices, each the same as Linux's mke2fs makes with the same options
 * (tools/extcheck.c compares the two on the jam4k fixture's shape):
 *   - a flex group's block bitmaps, then its inode bitmaps, then its
 *     inode tables, from the first block after its first group's
 *     superblock copy and descriptors;
 *   - the journal in the middle group when that group holds no metadata,
 *     else the nearest such group, else after a group's metadata; its
 *     superblock (jbd2's, big-endian, version 2) clean (s_start 0),
 *     sequence 1, no features (the journal's code sets the checksum
 *     feature when it first writes); the inode's block list copied into
 *     the superblock (s_jnl_blocks) as Linux's tools expect;
 *   - the root directory's block and lost+found's 16 KiB right after
 *     group 0's metadata (after the journal if it went there);
 *   - the reserved inodes (1 and 3 to 10) empty but with valid checksums
 *     (e2fsck checks every inode of the used part of a table).
 * Jam OS's own: errors=remount-ro (s_errors 2), no default mount options,
 * no reserved blocks, the root and lost+found owned by the one user. */
#include "ext_int.h"
#include "ext_mkfs.h"

#define INODE_SIZE   256u
#define LPF_INO      11u
#define LPF_BYTES    16384u
#define JNL_MAGIC    0xC03B3998u
#define JNL_SB_V2    4u
#define JNL_MIN      1024u                 /* jbd2's smallest journal, in blocks */
#define JNL_BIG      (64u << 20)           /* the journal of a volume over 8 GiB */
#define JNL_ERRORS_RO 2u                    /* s_errors: remount read-only */

struct mk {
    struct ext_fs fs;            /* the volume's shape, as a mount would read it */
    const struct ext_mkfs *p;
    const struct ext_dev *dev;
    uint8_t *gdt;                /* one block: descriptors being filled */
    uint8_t *work;               /* one block: anything else */
    uint8_t *zeros;              /* the rest of mem, zeros */
    uint32_t zero_blocks;
    uint32_t flex;               /* groups per flex group */
    uint32_t jgroup;
    uint64_t jstart;             /* the journal's first block */
    uint32_t jblocks;
    uint64_t root_blk, lpf_blk;  /* the root's block; lost+found's first */
    uint32_t lpf_blocks;
    uint64_t free_blocks;        /* counted as the bitmaps are written */
    uint32_t free_inodes;
};

/* ---- the shape --------------------------------------------------------------------------- */

static uint32_t sb_blocks(const struct mk *m, uint32_t g)
{
    return ext_group_has_super(&m->fs, g) ? 1 + m->fs.gdt_blocks : 0;
}

static bool leader(const struct mk *m, uint32_t g)
{
    return g % m->flex == 0;
}

/* Groups in flex group with leader L. */
static uint32_t flex_n(const struct mk *m, uint32_t L)
{
    return m->fs.groups - L < m->flex ? m->fs.groups - L : m->flex;
}

static uint64_t meta_base(const struct mk *m, uint32_t L)
{
    return ext_group_first(&m->fs, L) + sb_blocks(m, L);
}

/* The first block of group g past its superblock copy and the flex
 * metadata it holds. */
static uint64_t meta_end(const struct mk *m, uint32_t g)
{
    if (!leader(m, g))
        return ext_group_first(&m->fs, g) + sb_blocks(m, g);
    return meta_base(m, g) + (uint64_t)flex_n(m, g) * (2 + m->fs.itable_blocks);
}

static uint64_t group_end(const struct mk *m, uint32_t g)
{
    return ext_group_first(&m->fs, g) + ext_group_blocks(&m->fs, g);
}

static uint64_t block_bitmap(const struct mk *m, uint32_t g)
{
    uint32_t L = g - g % m->flex;
    return meta_base(m, L) + (g - L);
}

static uint64_t inode_bitmap(const struct mk *m, uint32_t g)
{
    uint32_t L = g - g % m->flex;
    return meta_base(m, L) + flex_n(m, L) + (g - L);
}

static uint64_t inode_table(const struct mk *m, uint32_t g)
{
    uint32_t L = g - g % m->flex;
    return meta_base(m, L) + 2ull * flex_n(m, L) + (uint64_t)(g - L) * m->fs.itable_blocks;
}

/* Does every flex group's metadata fit in its first group (with room
 * left: at least 16 blocks)? */
static bool flex_fits(const struct mk *m)
{
    for (uint32_t L = 0; L < m->fs.groups; L += m->flex)
        if (meta_end(m, L) + 16 > group_end(m, L))
            return false;
    return true;
}

/* The block size, the groups' size, the inodes. */
static status_t shape_sizes(struct mk *m)
{
    const struct ext_mkfs *p = m->p;
    struct ext_fs *fs = &m->fs;
    uint32_t bs = p->block_size ? p->block_size : 4096;
    if (bs != 1024 && bs != 2048 && bs != 4096)
        return ERR_INVALID_ARGS;
    fs->block_size = bs;
    fs->block_log = (uint32_t)__builtin_ctz(bs);
    fs->first_data_block = bs == 1024;
    uint32_t bpg = p->blocks_per_group ? p->blocks_per_group : 8 * bs;
    if (bpg % 8 || bpg < 256 || bpg > 8 * bs)
        return ERR_INVALID_ARGS;
    fs->blocks_per_group = bpg;
    uint64_t blocks = m->dev->bytes >> fs->block_log;
    if (blocks > (1ull << 48))
        blocks = 1ull << 48;
    if (blocks < 2048)
        return ERR_NO_SPACE;
    fs->blocks = blocks;
    uint64_t groups = (blocks - fs->first_data_block + bpg - 1) / bpg;
    fs->groups = (uint32_t)groups;
    fs->inode_size = INODE_SIZE;
    fs->inodes_per_block = bs / INODE_SIZE;
    fs->desc_size = EXT_DESC_MIN_64;
    fs->descs_per_block = bs / EXT_DESC_MIN_64;
    uint64_t ipg = p->inodes ? (p->inodes + groups - 1) / groups
                             : (uint64_t)bpg * bs / (p->inode_ratio ? p->inode_ratio : 16384);
    uint32_t unit = fs->inodes_per_block > 8 ? fs->inodes_per_block : 8;
    ipg = (ipg + unit - 1) / unit * unit;
    ipg = ipg < 16 ? 16 : ipg > 8ull * bs ? 8ull * bs : ipg;
    if (groups > UINT32_MAX / ipg)
        return ERR_INVALID_ARGS;
    fs->inodes_per_group = (uint32_t)ipg;
    fs->itable_blocks = fs->inodes_per_group / fs->inodes_per_block;
    return OK;
}

/* Groups, descriptors, flex groups; a last group too small is left off. */
static status_t shape_groups(struct mk *m)
{
    struct ext_fs *fs = &m->fs;
    m->flex = m->p->flex ? m->p->flex : 16;
    if (m->flex & (m->flex - 1) || m->flex > (1u << 16))
        return ERR_INVALID_ARGS;
    for (;;) {
        fs->gdt_blocks = (fs->groups + fs->descs_per_block - 1) / fs->descs_per_block;
        fs->inodes = fs->groups * fs->inodes_per_group;
        uint32_t last = fs->groups - 1;
        uint64_t need = ext_group_first(fs, last) + sb_blocks(m, last) +
                        (leader(m, last) ? 2 + fs->itable_blocks : 0) + 16;
        if (need <= fs->blocks || fs->groups == 1)
            break;
        fs->blocks = ext_group_first(fs, last);
        fs->groups--;
    }
    if (1 + fs->gdt_blocks >= fs->blocks_per_group)
        return ERR_NO_SPACE;
    while (m->flex > 1 && !flex_fits(m))
        m->flex /= 2;
    return flex_fits(m) ? OK : ERR_NO_SPACE;
}

/* The journal's size, by the plan's rule. */
static status_t journal_size(struct mk *m)
{
    struct ext_fs *fs = &m->fs;
    uint64_t j = m->p->journal_blocks;
    if (!j && (fs->blocks << fs->block_log) > (8ull << 30))
        j = JNL_BIG >> fs->block_log;
    else if (!j)
        j = fs->blocks / 128;
    if (!m->p->journal_blocks && j < JNL_MIN)
        j = JNL_MIN;
    if (j < JNL_MIN || j > EXT_INIT_MAX_LEN)
        return ERR_INVALID_ARGS;
    if (j > fs->blocks / 4)
        return ERR_NO_SPACE;
    m->jblocks = (uint32_t)j;
    return OK;
}

/* Does a run of n blocks from `start` (the end of group g's metadata)
 * stay clear of every later group's metadata, and inside the volume? */
static bool run_clear(const struct mk *m, uint32_t g, uint64_t start, uint64_t n)
{
    if (start + n > m->fs.blocks)
        return false;
    for (uint32_t h = g + 1; h < m->fs.groups && ext_group_first(&m->fs, h) < start + n; h++)
        if (sb_blocks(m, h) || leader(m, h))
            return false;
    return true;
}

/* Where the journal goes: from a group with no metadata nearest the
 * middle, else from the nearest one's metadata end; in one run, which
 * may go on into the next groups while they hold no metadata. Starting
 * in group 0, the root's and lost+found's blocks follow it. */
static status_t place_journal(struct mk *m)
{
    uint32_t G = m->fs.groups, mid = G / 2;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t d = 0; d <= G; d++)
            for (int side = 0; side < 2; side++) {
                uint64_t g = side ? (uint64_t)mid + d : (uint64_t)mid - d;
                if ((side && !d) || g >= G || (!side && d > mid))
                    continue;
                bool clear = !sb_blocks(m, (uint32_t)g) && !leader(m, (uint32_t)g);
                uint64_t start = meta_end(m, (uint32_t)g);
                uint64_t after = g == 0 ? 1 + LPF_BYTES / m->fs.block_size : 0;
                if ((pass == 0 && !clear) || !run_clear(m, (uint32_t)g, start, m->jblocks + after))
                    continue;
                m->jgroup = (uint32_t)g;
                m->jstart = start;
                return OK;
            }
    return ERR_NO_SPACE;
}

static status_t shape(struct mk *m)
{
    struct ext_fs *fs = &m->fs;
    fs->first_ino = EXT_FIRST_INO_MIN;
    fs->csum = fs->is64 = fs->filetype = fs->flex_bg = fs->sparse_super = true;
    fs->huge_file = fs->has_journal = true;
    fs->csum_seed = ext_crc32c(~0u, m->p->uuid, 16);
    __builtin_memcpy(fs->sb.uuid, m->p->uuid, 16);
    status_t st = shape_sizes(m);
    if (st == OK)
        st = shape_groups(m);
    if (st == OK)
        st = journal_size(m);
    if (st == OK)
        st = place_journal(m);
    if (st != OK)
        return st;
    m->lpf_blocks = LPF_BYTES / fs->block_size;
    m->root_blk = meta_end(m, 0) + (m->jgroup == 0 ? m->jblocks : 0);
    m->lpf_blk = m->root_blk + 1;
    return run_clear(m, 0, m->root_blk, 1 + m->lpf_blocks) ? OK : ERR_NO_SPACE;
}

/* ---- writing ----------------------------------------------------------------------------- */

static status_t put(struct mk *m, uint64_t blk, const void *data)
{
    struct ext_fs *fs = &m->fs;
    return m->dev->write(m->dev->ctx, blk << fs->block_log, fs->block_size, data) == OK ? OK
                                                                                     : ERR_IO;
}

static status_t zero_run(struct mk *m, uint64_t blk, uint64_t count)
{
    struct ext_fs *fs = &m->fs;
    if (m->p->flags & EXT_MKFS_ZEROED)
        return OK;
    while (count) {
        uint64_t n = m->zero_blocks ? (count < m->zero_blocks ? count : m->zero_blocks) : 1;
        const void *src = m->zero_blocks ? m->zeros : m->work;
        if (!m->zero_blocks)
            __builtin_memset(m->work, 0, fs->block_size);
        if (m->dev->write(m->dev->ctx, blk << fs->block_log, (uint32_t)(n << fs->block_log),
                          src) != OK)
            return ERR_IO;
        blk += n;
        count -= n;
    }
    return OK;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* jbd2's superblock: magic, type, sequence 0; block size, length, first
 * log block 1, sequence 1, start 0 (clean); the volume's UUID; one user. */
static status_t write_journal(struct mk *m)
{
    status_t st = zero_run(m, m->jstart, m->jblocks);
    if (st != OK)
        return st;
    uint8_t *b = m->work;
    __builtin_memset(b, 0, m->fs.block_size);
    be32(b + 0x00, JNL_MAGIC);
    be32(b + 0x04, JNL_SB_V2);
    be32(b + 0x0C, m->fs.block_size);
    be32(b + 0x10, m->jblocks);
    be32(b + 0x14, 1);
    be32(b + 0x18, 1);
    __builtin_memcpy(b + 0x30, m->p->uuid, 16);
    be32(b + 0x40, 1);
    return put(m, m->jstart, b);
}

/* An inode's extent root holding one extent. */
static void one_extent(struct ext_inode *in, uint32_t len, uint64_t start)
{
    struct ext_extent_header *h = (struct ext_extent_header *)in->d.block;
    *h = (struct ext_extent_header){ .magic = EXT_EXTENT_MAGIC, .entries = 1, .max = 4 };
    ext_extents(h)[0] = (struct ext_extent){ .block = 0, .len = (uint16_t)len,
                                             .start_hi = (uint16_t)(start >> 32),
                                             .start_lo = (uint32_t)start };
    in->d.flags = EXT_EXTENTS_FL;
}

/* One of the inodes mkfs makes, with `blocks` blocks from `start`. */
static void make_inode(struct mk *m, struct ext_inode *in, uint16_t mode, uint32_t blocks,
                       uint64_t start)
{
    struct ext_time t = { .sec = m->p->now, .nsec = 0 };
    in->d.mode = mode;
    in->d.extra_isize = 32;
    in->d.links_count = 1;
    uint64_t size = (uint64_t)blocks << m->fs.block_log;
    in->d.size_lo = (uint32_t)size;
    in->d.size_high = (uint32_t)(size >> 32);
    in->d.blocks_lo = (uint32_t)(size >> 9);
    one_extent(in, blocks, start);
    static const enum ext_time_which all[] = { EXT_ATIME, EXT_CTIME, EXT_MTIME, EXT_CRTIME };
    for (uint32_t i = 0; i < 4; i++)
        (void)ext_itime_set(in, all[i], &t);   /* mkfs's own time, in range (checked) */
    if (mode != (EXT_S_IFREG | 0600)) {
        in->d.uid = (uint16_t)m->p->uid;
        in->d.uid_high = (uint16_t)(m->p->uid >> 16);
        in->d.gid = (uint16_t)m->p->gid;
        in->d.gid_high = (uint16_t)(m->p->gid >> 16);
    }
}

/* Inode ino's bytes (INODE_SIZE of them, zeroed) as mkfs writes it. */
static void fill_inode(struct mk *m, uint32_t ino, uint8_t *raw)
{
    struct ext_inode in;
    __builtin_memset(&in, 0, sizeof(in));
    in.ino = ino;
    if (ino == EXT_ROOT_INO) {
        make_inode(m, &in, EXT_S_IFDIR | 0755, 1, m->root_blk);
        in.d.links_count = 3;
    } else if (ino == EXT_JOURNAL_INO) {
        make_inode(m, &in, EXT_S_IFREG | 0600, m->jblocks, m->jstart);
    } else if (ino == LPF_INO) {
        make_inode(m, &in, EXT_S_IFDIR | 0700, m->lpf_blocks, m->lpf_blk);
        in.d.links_count = 2;
    }
    __builtin_memcpy(raw, &in.d, sizeof(in.d));
    ext_inode_csum_set(&m->fs, ext_inode_seed(&m->fs, ino, 0), raw);
}

/* Group 0's inode table: zeros, and inodes 1 to 11. */
static status_t write_itable(struct mk *m)
{
    struct ext_fs *fs = &m->fs;
    uint64_t it = inode_table(m, 0);
    status_t st = zero_run(m, it, fs->itable_blocks);
    uint32_t used = (LPF_INO + fs->inodes_per_block - 1) / fs->inodes_per_block;
    for (uint32_t k = 0; k < used && st == OK; k++) {
        __builtin_memset(m->work, 0, fs->block_size);
        for (uint32_t i = 0; i < fs->inodes_per_block; i++) {
            uint32_t ino = k * fs->inodes_per_block + i + 1;
            if (ino <= LPF_INO)
                fill_inode(m, ino, m->work + i * INODE_SIZE);
        }
        st = put(m, it + k, m->work);
    }
    return st;
}

/* A directory entry at off of m->work. */
static void dirent(struct mk *m, uint32_t off, uint32_t ino, uint32_t rec_len, const char *name)
{
    struct ext_dirent *d = (struct ext_dirent *)(m->work + off);
    uint32_t n = (uint32_t)__builtin_strlen(name);
    d->inode = ino;
    d->rec_len = (uint16_t)rec_len;
    d->name_len = (uint8_t)n;
    d->file_type = ino ? EXT_FT_DIR : 0;
    __builtin_memcpy(d->name, name, n);
}

/* The root's block and lost+found's blocks. */
static status_t write_dirs(struct mk *m)
{
    struct ext_fs *fs = &m->fs;
    uint32_t limit = fs->block_size - EXT_DIR_TAIL_SIZE;
    __builtin_memset(m->work, 0, fs->block_size);
    dirent(m, 0, EXT_ROOT_INO, 12, ".");
    dirent(m, 12, EXT_ROOT_INO, 12, "..");
    dirent(m, 24, LPF_INO, limit - 24, "lost+found");
    ext_dirleaf_csum_set(fs, ext_inode_seed(fs, EXT_ROOT_INO, 0), m->work);
    status_t st = put(m, m->root_blk, m->work);
    for (uint32_t k = 0; k < m->lpf_blocks && st == OK; k++) {
        __builtin_memset(m->work, 0, fs->block_size);
        if (k == 0) {
            dirent(m, 0, LPF_INO, 12, ".");
            dirent(m, 12, EXT_ROOT_INO, limit - 12, "..");
        } else {
            dirent(m, 0, 0, limit, "");
        }
        ext_dirleaf_csum_set(fs, ext_inode_seed(fs, LPF_INO, 0), m->work);
        st = put(m, m->lpf_blk + k, m->work);
    }
    return st;
}

/* ---- bitmaps, descriptors, superblocks --------------------------------------------------- */

static void set_bits(uint8_t *map, uint64_t from, uint64_t to)
{
    for (uint64_t i = from; i < to; i++)
        map[i / 8] |= (uint8_t)(1u << (i % 8));
}

/* Mark [blk, blk + n) in group g's block bitmap, the part inside the group. */
static void mark_run(const struct mk *m, uint8_t *map, uint32_t g, uint64_t blk, uint64_t n)
{
    uint64_t first = ext_group_first(&m->fs, g), end = group_end(m, g);
    uint64_t a = blk > first ? blk : first, b = blk + n < end ? blk + n : end;
    if (a < b)
        set_bits(map, a - first, b - first);
}

/* Group g's block bitmap into m->work; its free blocks. */
static uint32_t block_map(struct mk *m, uint32_t g)
{
    struct ext_fs *fs = &m->fs;
    uint8_t *map = m->work;
    uint64_t first = ext_group_first(fs, g);
    __builtin_memset(map, 0, fs->block_size);
    mark_run(m, map, g, first, sb_blocks(m, g));
    if (leader(m, g))
        mark_run(m, map, g, meta_base(m, g), meta_end(m, g) - meta_base(m, g));
    mark_run(m, map, g, m->jstart, m->jblocks);
    mark_run(m, map, g, m->root_blk, 1 + m->lpf_blocks);
    uint32_t used = 0, n = ext_group_blocks(fs, g);
    for (uint32_t i = 0; i < n; i++)
        used += map[i / 8] >> (i % 8) & 1;
    set_bits(map, n, fs->block_size * 8ull);
    return n - used;
}

/* Group g's inode bitmap into m->work: group 0's first 11 in use. */
static void inode_map(struct mk *m, uint32_t g)
{
    __builtin_memset(m->work, 0, m->fs.block_size);
    if (g == 0)
        set_bits(m->work, 0, LPF_INO);
    set_bits(m->work, m->fs.inodes_per_group, m->fs.block_size * 8ull);
}

/* Group g's descriptor, its bitmaps' checksums given. */
static void describe(struct mk *m, uint32_t g, uint32_t free_b, uint32_t bcs, uint32_t ics)
{
    struct ext_fs *fs = &m->fs;
    struct ext_group_desc d;
    __builtin_memset(&d, 0, sizeof(d));
    uint32_t free_i = fs->inodes_per_group - (g == 0 ? LPF_INO : 0);
    uint64_t bb = block_bitmap(m, g), ib = inode_bitmap(m, g), it = inode_table(m, g);
    d.block_bitmap_lo = (uint32_t)bb;
    d.block_bitmap_hi = (uint32_t)(bb >> 32);
    d.inode_bitmap_lo = (uint32_t)ib;
    d.inode_bitmap_hi = (uint32_t)(ib >> 32);
    d.inode_table_lo = (uint32_t)it;
    d.inode_table_hi = (uint32_t)(it >> 32);
    d.free_blocks_count_lo = (uint16_t)free_b;
    d.free_blocks_count_hi = (uint16_t)(free_b >> 16);
    d.free_inodes_count_lo = (uint16_t)free_i;
    d.free_inodes_count_hi = (uint16_t)(free_i >> 16);
    d.used_dirs_count_lo = g == 0 ? 2 : 0;
    d.itable_unused_lo = (uint16_t)free_i;
    d.itable_unused_hi = (uint16_t)(free_i >> 16);
    d.flags = g == 0 || (m->p->flags & EXT_MKFS_ZEROED) ? EXT_BG_INODE_ZEROED : 0;
    if (g)
        d.flags |= EXT_BG_INODE_UNINIT;
    d.block_bitmap_csum_lo = (uint16_t)bcs;
    d.block_bitmap_csum_hi = (uint16_t)(bcs >> 16);
    d.inode_bitmap_csum_lo = (uint16_t)ics;
    d.inode_bitmap_csum_hi = (uint16_t)(ics >> 16);
    uint8_t *raw = m->gdt + (g % fs->descs_per_block) * fs->desc_size;
    __builtin_memcpy(raw, &d, sizeof(d));
    ((struct ext_group_desc *)raw)->checksum = ext_group_csum(fs, g, raw);
    m->free_blocks += free_b;
    m->free_inodes += free_i;
}

/* Write group g's bitmaps and fill its descriptor into m->gdt. */
static status_t group(struct mk *m, uint32_t g)
{
    uint32_t free_b = block_map(m, g);
    uint32_t bcs = ext_bitmap_csum(&m->fs, EXT_BITMAP_BLOCK, m->work);
    status_t st = put(m, block_bitmap(m, g), m->work);
    inode_map(m, g);
    uint32_t ics = ext_bitmap_csum(&m->fs, EXT_BITMAP_INODE, m->work);
    if (st == OK)
        st = put(m, inode_bitmap(m, g), m->work);
    describe(m, g, free_b, bcs, ics);
    return st;
}

/* Every group's bitmaps, and each descriptor block where the primary
 * and every backup keep it. */
static status_t write_groups(struct mk *m)
{
    struct ext_fs *fs = &m->fs;
    for (uint32_t k = 0; k < fs->gdt_blocks; k++) {
        __builtin_memset(m->gdt, 0, fs->block_size);
        status_t st = OK;
        for (uint32_t g = k * fs->descs_per_block;
             g < fs->groups && g < (k + 1) * fs->descs_per_block && st == OK; g++)
            st = group(m, g);
        for (uint32_t g = 0; g < fs->groups && st == OK; g++)
            if (sb_blocks(m, g))
                st = put(m, ext_group_first(fs, g) + 1 + k, m->gdt);
        if (st != OK)
            return st;
    }
    return OK;
}

/* The superblock's shape and features. */
static void fill_shape(const struct mk *m, struct ext_super *sb)
{
    const struct ext_fs *fs = &m->fs;
    sb->inodes_count = fs->inodes;
    sb->blocks_count_lo = (uint32_t)fs->blocks;
    sb->blocks_count_hi = (uint32_t)(fs->blocks >> 32);
    sb->free_blocks_count_lo = (uint32_t)m->free_blocks;
    sb->free_blocks_count_hi = (uint32_t)(m->free_blocks >> 32);
    sb->free_inodes_count = m->free_inodes;
    sb->first_data_block = fs->first_data_block;
    sb->log_block_size = sb->log_cluster_size = fs->block_log - EXT_MIN_BLOCK_LOG;
    sb->blocks_per_group = sb->clusters_per_group = fs->blocks_per_group;
    sb->inodes_per_group = fs->inodes_per_group;
    sb->rev_level = EXT_DYNAMIC_REV;
    sb->first_ino = fs->first_ino;
    sb->inode_size = INODE_SIZE;
    sb->feature_compat = EXT_COMPAT_HAS_JOURNAL;
    sb->feature_incompat = EXT_INCOMPAT_FILETYPE | EXT_INCOMPAT_EXTENTS | EXT_INCOMPAT_64BIT |
                           EXT_INCOMPAT_FLEX_BG;
    sb->feature_ro_compat = EXT_RO_COMPAT_SPARSE_SUPER | EXT_RO_COMPAT_LARGE_FILE |
                            EXT_RO_COMPAT_HUGE_FILE | EXT_RO_COMPAT_DIR_NLINK |
                            EXT_RO_COMPAT_EXTRA_ISIZE | EXT_RO_COMPAT_METADATA_CSUM;
    sb->desc_size = EXT_DESC_MIN_64;
    sb->min_extra_isize = sb->want_extra_isize = 32;
    sb->log_groups_per_flex = (uint8_t)__builtin_ctz(m->flex);
}

/* The superblock (group 0's copy). */
static void fill_super(const struct mk *m, struct ext_super *sb)
{
    const struct ext_mkfs *p = m->p;
    __builtin_memset(sb, 0, sizeof(*sb));
    fill_shape(m, sb);
    sb->wtime = sb->lastcheck = sb->mkfs_time = (uint32_t)p->now;
    sb->max_mnt_count = -1;
    sb->magic = EXT_MAGIC;
    sb->state = EXT_STATE_VALID;
    sb->errors = JNL_ERRORS_RO;
    __builtin_memcpy(sb->uuid, p->uuid, 16);
    __builtin_memcpy(sb->volume_name, p->label, 16);
    sb->journal_inum = EXT_JOURNAL_INO;
    for (uint32_t i = 0; i < 4; i++)
        sb->hash_seed[i] = ext_crc32c(~i, p->uuid, 16);
    sb->def_hash_version = 1;       /* half MD4, for an index e2fsck -D may add */
    sb->flags = 1;                  /* hashes of signed chars, as on x86 Linux */
    sb->jnl_backup_type = 1;        /* s_jnl_blocks holds the journal inode's blocks */
    sb->checksum_type = EXT_CRC32C;
    struct ext_inode j;
    __builtin_memset(&j, 0, sizeof(j));
    one_extent(&j, m->jblocks, m->jstart);
    __builtin_memcpy(sb->jnl_blocks, j.d.block, sizeof(j.d.block));
    sb->jnl_blocks[16] = m->jblocks << m->fs.block_log;
}

/* The superblock's copy in group g: block_group_nr g, its checksum. */
static status_t write_super(struct mk *m, const struct ext_super *sb, uint32_t g)
{
    struct ext_fs *fs = &m->fs;
    __builtin_memset(m->work, 0, fs->block_size);
    uint32_t off = g == 0 && fs->block_size > EXT_SUPER_OFFSET ? EXT_SUPER_OFFSET : 0;
    struct ext_super *copy = (struct ext_super *)(m->work + off);
    *copy = *sb;
    copy->block_group_nr = (uint16_t)g;
    copy->checksum = ext_super_csum(copy);
    return put(m, ext_group_first(fs, g), m->work);
}

/* Carve mem: the descriptor block, the work block, zeros. */
static status_t carve(struct mk *m, void *mem, size_t mem_size)
{
    size_t bs = m->fs.block_size;
    if (mem_size < 2 * bs)
        return ERR_NO_MEMORY;
    size_t z = (mem_size - 2 * bs) / bs, cap = (1u << 30) >> m->fs.block_log;   /* a write's most */
    m->gdt = mem;
    m->work = (uint8_t *)mem + bs;
    m->zeros = (uint8_t *)mem + 2 * bs;
    m->zero_blocks = (uint32_t)(z < cap ? z : cap);
    __builtin_memset(m->zeros, 0, (size_t)m->zero_blocks * bs);
    return OK;
}

status_t ext_mkfs(const struct ext_dev *dev, const struct ext_mkfs *p, void *mem,
                  size_t mem_size)
{
    struct mk m;
    __builtin_memset(&m, 0, sizeof(m));
    if (!dev || !dev->write || !dev->read || !p || !mem ||
        (p->flags & ~(uint32_t)EXT_MKFS_ZEROED) || p->now < 0 || p->now > UINT32_MAX)
        return ERR_INVALID_ARGS;
    m.p = p;
    m.dev = dev;
    status_t st = shape(&m);
    if (st == OK)
        st = carve(&m, mem, mem_size);
    if (st == OK)
        st = write_journal(&m);
    if (st == OK)
        st = write_itable(&m);
    if (st == OK)
        st = write_dirs(&m);
    if (st == OK)
        st = write_groups(&m);
    if (st != OK)
        return st;
    struct ext_super sb;
    fill_super(&m, &sb);
    for (uint32_t g = 1; g < m.fs.groups && st == OK; g++)
        if (sb_blocks(&m, g))
            st = write_super(&m, &sb, g);
    /* The first 4 KiB last, on a device with a cache too: a format cut
     * short must leave them blank. */
    if (st == OK)
        st = ext_flush(dev);
    if (st == OK)
        st = write_super(&m, &sb, 0);
    return st == OK ? ext_flush(dev) : st;
}
