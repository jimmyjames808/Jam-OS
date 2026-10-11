/* extcheck's checks of the library against damage it must see and of
 * the paths that write: every structure's checksum broken alone, the
 * feature table, and every structure written back unchanged
 * (tools/extcheck.c has the model). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extcheck.h"

/* ---- checksums broken one at a time ---------------------------------------------------- */

/* Flip one byte at off; then `probe` must fail with ERR_IO and `why`; then
 * `seal` (the library's _set function for that structure) makes it whole
 * again and probe must pass. */
struct breaker {
    const char *what;             /* the structure */
    const char *why;              /* the reason the library must give */
    uint64_t off;                 /* the byte to flip */
    void (*seal)(struct img *m, struct ext_fs *clean, const struct breaker *br);
    status_t (*probe)(struct img *m, const struct breaker *br, struct ext_fs *fs);
    uint64_t at;                  /* the structure's start (seal's) */
    uint32_t ino, seed, g;        /* its owner and group (ino: 1 for the inode bitmap) */
    uint64_t blk;                 /* its block */
};

static status_t probe_mount(struct img *m, const struct breaker *br, struct ext_fs *fs)
{
    (void)br;
    return mount_img(m, fs, CACHE_BYTES, 0);
}

static status_t probe_inode(struct img *m, const struct breaker *br, struct ext_fs *fs)
{
    status_t st = mount_img(m, fs, CACHE_BYTES, 0);
    struct ext_inode in;
    if (st == OK)
        st = ext_iget(fs, br->ino, 0, &in);
    return st;
}

static status_t probe_dir(struct img *m, const struct breaker *br, struct ext_fs *fs)
{
    status_t st = mount_img(m, fs, CACHE_BYTES, 0);
    struct ext_inode in;
    struct ext_dentry d = { .ino = 1 };
    if (st == OK)
        st = ext_iget(fs, br->ino, 0, &in);
    for (uint64_t pos = 0; st == OK && d.ino;)
        st = ext_dir_next(fs, &in, &pos, &d);
    return st;
}

static status_t probe_map(struct img *m, const struct breaker *br, struct ext_fs *fs)
{
    status_t st = mount_img(m, fs, CACHE_BYTES, 0);
    struct ext_inode in;
    struct ext_run r;
    if (st == OK)
        st = ext_iget(fs, br->ino, 0, &in);
    for (uint32_t lblk = 0; st == OK && lblk < 4096; lblk += 7)
        st = ext_map(fs, &in, lblk, &r);
    return st;
}

static status_t probe_xattr(struct img *m, const struct breaker *br, struct ext_fs *fs)
{
    status_t st = mount_img(m, fs, CACHE_BYTES, 0);
    struct ext_inode in;
    struct ext_buf *b;
    if (st == OK)
        st = ext_iget(fs, br->ino, 0, &in);
    if (st == OK)
        st = ext_xattr_block(fs, &in, &b);
    if (st == OK)
        ext_brelse(fs, b);
    return st;
}

static status_t probe_bitmap(struct img *m, const struct breaker *br, struct ext_fs *fs)
{
    status_t st = mount_img(m, fs, CACHE_BYTES, 0);
    struct ext_buf *b;
    if (st == OK)
        st = ext_bitmap_read(fs, br->g, br->ino ? EXT_BITMAP_INODE : EXT_BITMAP_BLOCK, &b);
    if (st == OK)
        ext_brelse(fs, b);
    return st;
}

static void seal_super(struct img *m, struct ext_fs *clean, const struct breaker *br)
{
    (void)clean;
    struct ext_super sb;
    memcpy(&sb, m->bytes + br->at, sizeof(sb));
    uint32_t c = ext_super_csum(&sb);
    poke(m, br->at + offsetof(struct ext_super, checksum), &c, 4);
}

static void seal_group(struct img *m, struct ext_fs *clean, const struct breaker *br)
{
    uint16_t c = ext_group_csum(clean, br->g, m->bytes + br->at);
    poke(m, br->at + offsetof(struct ext_group_desc, checksum), &c, 2);
}

/* The rest seal a copy of the block, then poke the whole block back. */
static void seal_block(struct img *m, struct ext_fs *clean, const struct breaker *br)
{
    uint32_t bs = clean->block_size;
    uint8_t *copy = malloc(bs);
    if (!copy)
        fail("out of memory");
    memcpy(copy, m->bytes + br->blk * bs, bs);
    uint8_t *at = copy + (br->at - br->blk * bs);
    if (!strcmp(br->what, "inode"))
        ext_inode_csum_set(clean, br->seed, at);
    else if (!strcmp(br->what, "extent node"))
        ext_extent_csum_set(clean, br->seed, copy);
    else if (!strcmp(br->what, "directory leaf"))
        ext_dirleaf_csum_set(clean, br->seed, copy);
    else if (!strcmp(br->what, "index root"))
        ext_dx_csum_set(clean, br->seed, copy, EXT_DX_ROOT_COUNT);
    else if (!strcmp(br->what, "attribute block"))
        ((struct ext_xattr_header *)copy)->checksum = ext_xattr_csum(clean, br->blk, copy);
    poke(m, br->blk * bs, copy, bs);
    free(copy);
}

static void seal_bitmap(struct img *m, struct ext_fs *clean, const struct breaker *br)
{
    uint32_t bs = clean->block_size;
    enum ext_bitmap w = br->ino ? EXT_BITMAP_INODE : EXT_BITMAP_BLOCK;
    uint32_t c = ext_bitmap_csum(clean, w, m->bytes + br->blk * bs);
    uint64_t gd = (clean->first_data_block + 1u) * (uint64_t)bs + br->g * clean->desc_size;
    size_t lo = w ? offsetof(struct ext_group_desc, inode_bitmap_csum_lo)
                  : offsetof(struct ext_group_desc, block_bitmap_csum_lo);
    size_t hi = w ? offsetof(struct ext_group_desc, inode_bitmap_csum_hi)
                  : offsetof(struct ext_group_desc, block_bitmap_csum_hi);
    poke(m, gd + lo, &c, 2);
    if (clean->desc_size >= EXT_DESC_MIN_64) {
        uint16_t h = (uint16_t)(c >> 16);
        poke(m, gd + hi, &h, 2);
    }
    struct breaker g = { .at = gd, .g = br->g };
    seal_group(m, clean, &g);
}

static void run_breaker(struct img *m, struct ext_fs *clean, const struct breaker *br)
{
    char label[128];
    snprintf(label, sizeof(label), "%s: %s broken", m->name, br->what);
    where = label;
    uint8_t v = m->bytes[br->off] ^ 0x5a;
    poke(m, br->off, &v, 1);
    struct ext_fs fs;
    status_t st = br->probe(m, br, &fs);
    if (st != ERR_IO || !fs.why || strcmp(fs.why, br->why))
        fail("got %d (%s), not ERR_IO (%s)", st, fs.why ? fs.why : "no reason", br->why);
    br->seal(m, clean, br);
    st = br->probe(m, br, &fs);
    if (st != OK)
        fail("sealed again by the library: %d (%s)", st, fs.why ? fs.why : "");
    check_pins(&fs);
    undo(m);
}

/* Block lblk of /name (its inode into *in). */
static uint64_t block_of(struct ext_fs *fs, const char *name, uint32_t lblk, struct ext_inode *in)
{
    lookup(fs, name, in);
    struct ext_run r;
    if (ext_map(fs, in, lblk, &r) != OK || r.kind != EXT_RUN_DATA)
        fail("/%s has no block %u", name, lblk);
    return r.pblk;
}

/* The inode, the extent node, the directory blocks and the bitmaps:
 * metadata_csum's structures (on a volume that has it). */
static void break_csum_structures(struct img *m, struct ext_fs *clean)
{
    uint32_t bs = clean->block_size;
    struct ext_inode in;
    lookup(clean, "small", &in);
    uint64_t blk;
    uint32_t off;
    if (ext_iloc(clean, in.ino, &blk, &off) != OK)
        fail("iloc");
    struct breaker ib = { "inode", "inode checksum", blk * bs + off + 2, seal_block, probe_inode,
                          .at = blk * bs + off, .ino = in.ino, .seed = in.seed, .blk = blk };
    run_breaker(m, clean, &ib);

    lookup(clean, bs == 4096 ? "holes" : "deep", &in);
    struct ext_path p;
    if (ext_extent_find(clean, &in, 0, &p) != OK || !p.depth)
        fail("no extent node");
    blk = p.lv[1].buf->blk;
    ext_path_release(clean, &p);
    struct breaker eb = { "extent node", "extent block checksum", blk * bs + 8, seal_block,
                          probe_map, .at = blk * bs, .ino = in.ino, .seed = in.seed, .blk = blk };
    run_breaker(m, clean, &eb);

    blk = block_of(clean, "many", 1, &in);
    struct breaker db = { "directory leaf", "directory block checksum", blk * bs + 20, seal_block,
                          probe_dir, .at = blk * bs, .ino = in.ino, .seed = in.seed, .blk = blk };
    run_breaker(m, clean, &db);
    if (in.d.flags & EXT_INDEX_FL) {
        blk = block_of(clean, "many", 0, &in);
        struct breaker xb = { "index root", "directory index checksum", blk * bs + 40,
                              seal_block, probe_dir, .at = blk * bs, .ino = in.ino,
                              .seed = in.seed, .blk = blk };
        run_breaker(m, clean, &xb);
    }

    lookup(clean, "pattern", &in);
    struct ext_buf *xb;
    if (ext_xattr_block(clean, &in, &xb) == OK) {
        blk = xb->blk;
        ext_brelse(clean, xb);
        struct breaker ab = { "attribute block", "attribute block checksum", blk * bs + 40,
                              seal_block, probe_xattr, .at = blk * bs, .ino = in.ino,
                              .blk = blk };
        run_breaker(m, clean, &ab);
    }

    struct ext_group g0;
    if (ext_group_get(clean, 0, &g0) != OK)
        fail("group 0");
    for (uint32_t w = 0; w < 2; w++) {
        blk = w ? g0.inode_bitmap : g0.block_bitmap;
        struct breaker bb = { w ? "inode bitmap" : "block bitmap",
                              w ? "inode bitmap checksum" : "block bitmap checksum",
                              blk * bs + 3, seal_bitmap, probe_bitmap, .at = blk * bs,
                              .ino = w, .g = 0, .blk = blk };
        run_breaker(m, clean, &bb);
    }
}

void check_breaks(struct img *m)
{
    struct ext_fs clean;
    where = m->name;
    if (mount_img(m, &clean, CACHE_BYTES, 0) != OK)
        fail("mount");
    uint64_t gd1 = (clean.first_data_block + 1u) * (uint64_t)clean.block_size + clean.desc_size;
    struct breaker gd = { "group descriptor", "group descriptor checksum", gd1 + 0x0C, seal_group,
                          probe_mount, .at = gd1, .g = 1 };
    run_breaker(m, &clean, &gd);
    if (!clean.csum)
        return;
    struct breaker sb = { "superblock", "superblock checksum", EXT_SUPER_OFFSET + 0x78,
                          seal_super, probe_mount, .at = EXT_SUPER_OFFSET };
    run_breaker(m, &clean, &sb);
    break_csum_structures(m, &clean);
}

/* ---- the feature table ----------------------------------------------------------------- */

enum want { W_RW, W_RO, W_REFUSE };

/* docs/M13.6-PLAN.md's table, written out again: what setting one bit
 * must do to a volume Jam OS writes. Bits not listed: an incompatible one
 * refuses, any other makes it read-only. */
static enum want plan_verdict(int set, uint32_t bit)
{
    static const uint32_t rw[3] = {
        EXT_COMPAT_HAS_JOURNAL | EXT_COMPAT_EXT_ATTR | EXT_COMPAT_RESIZE_INODE |
            EXT_COMPAT_DIR_INDEX,
        EXT_INCOMPAT_FILETYPE | EXT_INCOMPAT_EXTENTS | EXT_INCOMPAT_64BIT |
            EXT_INCOMPAT_FLEX_BG | EXT_INCOMPAT_CSUM_SEED | EXT_INCOMPAT_RECOVER,
        EXT_RO_COMPAT_SPARSE_SUPER | EXT_RO_COMPAT_LARGE_FILE | EXT_RO_COMPAT_HUGE_FILE |
            EXT_RO_COMPAT_DIR_NLINK | EXT_RO_COMPAT_EXTRA_ISIZE | EXT_RO_COMPAT_METADATA_CSUM,
    };
    static const uint32_t refuse[3] = {
        EXT_COMPAT_ORPHAN_FILE,
        ~(uint32_t)0,
        EXT_RO_COMPAT_BIGALLOC | EXT_RO_COMPAT_ORPHAN_PRESENT,
    };
    if (rw[set] & bit)
        return W_RW;
    return refuse[set] & bit ? W_REFUSE : W_RO;
}

/* Mount with one more feature bit, and judge what the library made of it. */
static void try_feature(struct img *m, const struct ext_super *sb, int set, uint32_t bit)
{
    struct ext_super s = *sb;
    uint32_t *word = set == 0 ? &s.feature_compat : set == 1 ? &s.feature_incompat
                                                             : &s.feature_ro_compat;
    *word |= bit;
    if (set == EXT_FSET_INCOMPAT && bit == EXT_INCOMPAT_CSUM_SEED)
        s.checksum_seed = ext_crc32c(~0u, s.uuid, sizeof(s.uuid));
    s.checksum = ext_super_csum(&s);
    poke(m, EXT_SUPER_OFFSET, &s, sizeof(s));
    struct ext_fs fs;
    status_t st = mount_img(m, &fs, CACHE_BYTES, 0);
    enum want w = plan_verdict(set, bit);
    bool ok;
    if (w == W_REFUSE)
        ok = st == ERR_NOT_SUPPORTED && (fs.refused[set] & bit);
    else if (st != OK)
        ok = false;
    else if (w == W_RO)
        ok = (fs.ro & EXT_RO_FEATURES) && (fs.ro_features[set] & bit);
    else if (set == EXT_FSET_INCOMPAT && bit == EXT_INCOMPAT_RECOVER)
        ok = fs.ro == EXT_RO_RECOVER;
    else
        ok = fs.ro == 0;
    if (!ok)
        fail("mount %d, ro %#x (%s): not %s", st, fs.ro, fs.why ? fs.why : "",
             w == W_REFUSE ? "refused" : w == W_RO ? "read-only" : "written");
    undo(m);
}

void check_features(struct img *m)
{
    struct ext_super sb;
    memcpy(&sb, m->bytes + EXT_SUPER_OFFSET, sizeof(sb));
    const uint32_t words[3] = { sb.feature_compat, sb.feature_incompat, sb.feature_ro_compat };
    for (int set = 0; set < 3; set++)
        for (int i = 0; i < 32; i++) {
            uint32_t bit = 1u << i;
            if (words[set] & bit)
                continue;
            char label[96];
            const char *name = ext_feature_name((enum ext_fset)set, bit);
            snprintf(label, sizeof(label), "%s: feature %d:%#x (%s) set", m->name, set, bit,
                     name ? name : "unknown");
            where = label;
            try_feature(m, &sb, set, bit);
        }
}

/* ---- written back unchanged ------------------------------------------------------------ */

/* Every inode in use, its times set to what they were, written back. */
static void put_inodes(struct ext_fs *fs)
{
    for (uint32_t ino = 1; ino <= fs->inodes; ino++) {
        struct ext_inode in;
        status_t st = ext_iget(fs, ino, EXT_IGET_UNLINKED, &in);
        if (st == ERR_NOT_FOUND)
            continue;
        if (st != OK)
            fail("inode %u: %d (%s)", ino, st, fs->why ? fs->why : "");
        for (int w = EXT_ATIME; w <= EXT_CRTIME; w++) {
            struct ext_time t;
            ext_itime(&in, (enum ext_time_which)w, &t);
            st = ext_itime_set(&in, (enum ext_time_which)w, &t);
            if (st != OK && !(w == EXT_CRTIME && !ext_ifits(&in, 0x98)))
                fail("inode %u: time %d can't be set to what it was", ino, w);
        }
        if (ext_iput(fs, &in) != OK)
            fail("inode %u: put", ino);
        if (fs->cache.ndirty > fs->cache.nbufs / 2 && !fs->ro && ext_sync(fs) != OK)
            fail("sync");
    }
}

void check_roundtrip(struct img *m)
{
    char label[96];
    snprintf(label, sizeof(label), "%s: written back unchanged", m->name);
    where = label;
    struct ext_fs fs;
    if (mount_img(m, &fs, CACHE_BYTES, 0) != OK)
        fail("mount");
    m->same_writes = true;
    put_inodes(&fs);
    for (uint32_t g = 0; g < fs.groups; g++) {
        struct ext_group gr;
        if (ext_group_get(&fs, g, &gr) != OK || ext_group_put(&fs, g, &gr) != OK)
            fail("group %u", g);
    }
    if (ext_super_write(&fs) != OK)
        fail("superblock");
    status_t st = ext_sync(&fs);
    m->same_writes = false;
    check_pins(&fs);
    if (fs.ro == 0 ? st != OK || fs.cache.ndirty : st != ERR_ACCESS_DENIED)
        fail("sync: %d with ro %#x", st, fs.ro);
    if (fs.ro)
        ext_discard(&fs);
    if (fs.cache.ndirty)
        fail("dirty buffers after a discard");
    undo(m);
}
