/* extcheck's fuzz pass: mutated copies of a fixture, each mounted and
 * walked end to end by the library, which must neither crash, read
 * outside the image, read without end, nor leave a buffer pinned.
 *
 * The mutations go where the parser looks. First the clean fixture is
 * walked once to list its sites: the superblock, each group descriptor,
 * each bitmap, each inode in use, each extent node, each directory block,
 * and every other block the walk read (block-map blocks, data). A run
 * makes one to eight edits, each at a site: a field set to an edge value
 * (0, 1, all ones, the sign bit, one more or less than it was, a random
 * value; for the superblock and inodes mostly the fields the library
 * computes with). Then, seven times in eight, the site's checksum is set
 * right again with the library's own _set functions, so the checks behind
 * the checksum are reached; a raw block's edits are left as they are.
 * Some runs also shrink the device, fail one of its reads, or give the
 * cache the fewest buffers it takes. Every edit is undone after its run;
 * the image is checked unchanged every 256 runs. All of it from extcheck's
 * seed, so a run repeats exactly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extcheck.h"

#define MAX_SITES    8192
#define MAX_DIRS     512       /* directories one walk goes into */
#define MAX_ENTRIES  20000     /* entries one walk reads */
#define READ_BYTES   (64u << 10)

enum site_kind { S_SUPER, S_GDESC, S_BITMAP, S_INODE, S_EXTENT, S_DIR, S_XATTR, S_RAW };

struct site {
    uint8_t  kind;             /* enum site_kind */
    uint8_t  aux;              /* S_DIR: enum ext_dirblk; S_BITMAP: enum ext_bitmap */
    uint32_t g;                /* S_GDESC, S_BITMAP: the group */
    uint32_t ino, seed;        /* the owner (S_INODE: itself) */
    uint64_t off, len;         /* the bytes */
};

static struct site sites[MAX_SITES];
static unsigned nsites;
static uint64_t *touched;      /* blocks the clean walk read */
static unsigned ntouched;

static void add_site(struct site s)
{
    for (unsigned i = 0; i < nsites; i++)
        if (sites[i].off == s.off && sites[i].kind == s.kind)
            return;
    if (nsites < MAX_SITES)
        sites[nsites++] = s;
}

/* ---- the walk -------------------------------------------------------------------------- */

struct walk {
    uint32_t dirs[MAX_DIRS];
    unsigned ndirs, entries;
    bool sites;                /* list the sites as it goes (the clean walk) */
};

static void file_sites(struct ext_fs *fs, struct ext_inode *in)
{
    uint64_t bs = fs->block_size;
    for (uint64_t lblk = 0, n = 0; lblk <= UINT32_MAX && n < 512; n++) {
        struct ext_path p;
        status_t st = ext_extent_find(fs, in, (uint32_t)lblk, &p);
        for (uint32_t lv = 1; st == OK && lv <= p.depth; lv++)
            add_site((struct site){ .kind = S_EXTENT, .ino = in->ino, .seed = in->seed,
                                    .off = p.lv[lv].buf->blk * bs, .len = bs });
        ext_path_release(fs, &p);
        struct ext_run r;
        if (st != OK || ext_map(fs, in, (uint32_t)lblk, &r) != OK)
            return;
        lblk += r.len;
    }
}

static void dir_sites(struct ext_fs *fs, struct ext_inode *in)
{
    for (uint64_t lblk = 0; lblk < ext_isize(in) >> fs->block_log && lblk < 256; lblk++) {
        struct ext_buf *b;
        enum ext_dirblk kind;
        if (ext_dir_block(fs, in, (uint32_t)lblk, &b, &kind) != OK)
            return;
        add_site((struct site){ .kind = S_DIR, .aux = (uint8_t)kind, .ino = in->ino,
                                .seed = in->seed, .off = b->blk * fs->block_size,
                                .len = fs->block_size });
        ext_brelse(fs, b);
    }
}

/* Read what an inode holds: some of a file, a link's target, a directory's
 * entries (queued). */
static void visit(struct ext_fs *fs, struct ext_inode *in, struct walk *w)
{
    static uint8_t buf[READ_BYTES];
    char target[8192];
    size_t n;
    enum ext_ftype t = ext_itype(in);
    if (t == EXT_FT_REG || t == EXT_FT_SYMLINK) {
        (void)ext_read(fs, in, 0, buf, below(2) ? READ_BYTES : 1 + below(READ_BYTES), &n);
        check_pins(fs);
        (void)ext_read(fs, in, rnd() % (ext_isize(in) + 1), buf, 1 + below(9000), &n);
        check_pins(fs);
        struct ext_run r;
        (void)ext_map(fs, in, (uint32_t)rnd(), &r);
        check_pins(fs);
    }
    if (t == EXT_FT_SYMLINK)
        (void)ext_readlink(fs, in, target, sizeof(target), &n);
    struct ext_buf *b;
    if (ext_xattr_block(fs, in, &b) == OK) {
        if (w->sites)
            add_site((struct site){ .kind = S_XATTR, .off = b->blk * fs->block_size,
                                    .len = fs->block_size });
        ext_brelse(fs, b);
    }
    if (w->sites && in->d.flags & EXT_EXTENTS_FL)
        file_sites(fs, in);
    if (w->sites && t == EXT_FT_DIR)
        dir_sites(fs, in);
    check_pins(fs);
    if (t != EXT_FT_DIR || w->ndirs == MAX_DIRS)
        return;
    for (unsigned i = 0; i < w->ndirs; i++)
        if (w->dirs[i] == in->ino)
            return;
    w->dirs[w->ndirs++] = in->ino;
}

static void walk_dir(struct ext_fs *fs, uint32_t ino, struct walk *w)
{
    struct ext_inode dir, in;
    if (ext_iget(fs, ino, 0, &dir) != OK)
        return;
    struct ext_dentry d;
    for (uint64_t pos = 0; w->entries < MAX_ENTRIES; w->entries++) {
        status_t st = ext_dir_next(fs, &dir, &pos, &d);
        check_pins(fs);
        if (st != OK || !d.ino)
            break;
        if (d.name_len != strlen(d.name) || strchr(d.name, '/'))
            fail("a name with a NUL or '/' came out of a directory");
        if (ext_iget(fs, d.ino, 0, &in) == OK)
            visit(fs, &in, w);
        check_pins(fs);
    }
    if (ext_dir_lookup(fs, &dir, "small", 5, &d) == OK && !d.ino)
        fail("lookup found inode 0");
    check_pins(fs);
}

/* Everything the library reads, from one mount. */
static void walk_all(struct ext_fs *fs, struct walk *w)
{
    struct ext_inode in;
    if (ext_iget(fs, EXT_ROOT_INO, 0, &in) == OK)
        visit(fs, &in, w);
    for (unsigned i = 0; i < w->ndirs; i++)
        walk_dir(fs, w->dirs[i], w);
    uint32_t list[64], n;
    (void)ext_orphans(fs, list, 64, &n);
    for (uint32_t g = 0; g < fs->groups && g < 64; g++) {
        struct ext_group gr;
        (void)ext_group_get(fs, g, &gr);
        for (int k = 0; k < 2; k++) {
            struct ext_buf *b;
            if (ext_bitmap_read(fs, g, (enum ext_bitmap)k, &b) == OK)
                ext_brelse(fs, b);
        }
    }
    for (int i = 0; i < 16; i++)
        (void)ext_iget(fs, (uint32_t)below(fs->inodes + 2), (uint32_t)below(2), &in);
    check_pins(fs);
}

/* ---- the sites ------------------------------------------------------------------------- */

static status_t record_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    struct img *m = ctx;
    if (off > m->size || len > m->size - off)
        fail("the clean walk read outside the image");
    memcpy(buf, m->bytes + off, len);
    for (uint32_t k = 0; k < len; k += 1024)
        touched[ntouched++ % 65536] = off + k;
    return OK;
}

static void list_sites(struct img *m, struct ext_fs *fs)
{
    nsites = 0;
    ntouched = 0;
    struct ext_dev dev = { .ctx = m, .bytes = m->size, .read = record_read };
    if (ext_mount(fs, &dev, m->cache, CACHE_BYTES, EXT_MOUNT_READ_ONLY) != OK)
        fail("mount");
    add_site((struct site){ .kind = S_SUPER, .off = EXT_SUPER_OFFSET, .len = EXT_SUPER_SIZE });
    uint64_t gdt = (fs->first_data_block + 1u) * (uint64_t)fs->block_size;
    for (uint32_t g = 0; g < fs->groups; g++) {
        add_site((struct site){ .kind = S_GDESC, .g = g, .off = gdt + g * fs->desc_size,
                                .len = fs->desc_size });
        struct ext_group gr;
        if (ext_group_get(fs, g, &gr) != OK)
            fail("group %u", g);
        for (uint8_t k = 0; k < 2; k++)
            if (!(gr.flags & (k ? EXT_BG_INODE_UNINIT : EXT_BG_BLOCK_UNINIT)))
                add_site((struct site){ .kind = S_BITMAP, .aux = k, .g = g,
                                        .off = (k ? gr.inode_bitmap : gr.block_bitmap) *
                                               fs->block_size, .len = fs->block_size });
    }
    for (uint32_t ino = 1; ino <= fs->inodes; ino++) {
        struct ext_inode in;
        uint64_t blk;
        uint32_t off;
        if (ext_iget(fs, ino, EXT_IGET_UNLINKED, &in) != OK || ext_iloc(fs, ino, &blk, &off))
            continue;
        add_site((struct site){ .kind = S_INODE, .ino = ino,
                                .off = blk * fs->block_size + off, .len = fs->inode_size });
    }
    struct walk w = { .sites = true };
    walk_all(fs, &w);
    if (fs->ro & EXT_RO_DAMAGED)
        fail("the clean walk found damage: %s (inode %u, block %llu)", fs->why, fs->why_ino,
             (unsigned long long)fs->why_blk);
    for (unsigned i = 0; i < ntouched && i < 65536; i++)
        add_site((struct site){ .kind = S_RAW, .off = touched[i] / fs->block_size *
                                fs->block_size, .len = fs->block_size });
}

/* ---- the edits ------------------------------------------------------------------------- */

/* The superblock's and an inode's fields the library computes with. */
static const uint16_t super_fields[] = {
    0x00, 0x04, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x38, 0x3A, 0x4C, 0x54, 0x58, 0x5C, 0x60,
    0x64, 0xCE, 0xE0, 0xE4, 0xE8, 0xFE, 0x150, 0x15C, 0x15E, 0x174, 0x175, 0x270,
};
static const uint16_t inode_fields[] = {
    0x00, 0x04, 0x14, 0x1A, 0x1C, 0x20, 0x28, 0x2A, 0x2C, 0x2E, 0x30, 0x34, 0x38, 0x3C, 0x40,
    0x44, 0x48, 0x4C, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x6C, 0x74, 0x80,
};

static uint64_t edge(unsigned width, uint64_t old)
{
    uint64_t ones = width == 8 ? UINT64_MAX : (1ull << 8 * width) - 1;
    switch (below(8)) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return ones;
    case 3:  return 1ull << (8 * width - 1);
    case 4:  return old + 1;
    case 5:  return old - 1;
    case 6:  return old ^ 1ull << below(8 * width);
    default: return rnd();
    }
}

/* The byte offset (in the site) of the field an edit changes. */
static uint64_t pick_field(const struct site *s, unsigned width)
{
    if (s->kind == S_SUPER && below(10) < 7)
        return super_fields[below(sizeof(super_fields) / sizeof(super_fields[0]))];
    if (s->kind == S_INODE && below(10) < 7)
        return inode_fields[below(sizeof(inode_fields) / sizeof(inode_fields[0]))];
    uint64_t span = s->kind == S_INODE && s->len > 160 ? 160 : s->len;
    if ((s->kind == S_DIR || s->kind == S_EXTENT) && below(2))
        span = 64;   /* the header and the first entries, where the walk starts */
    return below(span / width) * width;
}

/* Set the checksum of the block at off (the site's) right again. */
static void seal(struct img *m, struct ext_fs *fs, const struct site *s)
{
    uint32_t bs = fs->block_size;
    uint64_t blk_off = s->off / bs * bs;
    uint8_t *copy = malloc(bs);
    if (!copy)
        fail("out of memory");
    memcpy(copy, m->bytes + blk_off, bs);
    uint8_t *at = copy + (s->off - blk_off);
    if (s->kind == S_SUPER) {
        struct ext_super *sb = (struct ext_super *)at;
        sb->checksum = ext_super_csum(sb);
    } else if (s->kind == S_GDESC) {
        ((struct ext_group_desc *)at)->checksum = ext_group_csum(fs, s->g, at);
    } else if (s->kind == S_INODE) {
        struct ext_disk_inode *d = (struct ext_disk_inode *)at;
        ext_inode_csum_set(fs, ext_inode_seed(fs, s->ino, d->generation), at);
    } else if (s->kind == S_EXTENT) {
        (void)ext_extent_csum_set(fs, s->seed, copy);
    } else if (s->kind == S_DIR && s->aux == EXT_DIRBLK_LEAF) {
        ext_dirleaf_csum_set(fs, s->seed, copy);
    } else if (s->kind == S_XATTR) {
        ((struct ext_xattr_header *)copy)->checksum = ext_xattr_csum(fs, blk_off / bs, copy);
    } else if (s->kind == S_DIR) {
        uint32_t count_off = s->aux == EXT_DIRBLK_DX_ROOT ? EXT_DX_ROOT_COUNT : EXT_DX_NODE_COUNT;
        (void)ext_dx_csum_set(fs, s->seed, copy, count_off);
    }
    poke(m, blk_off, copy, bs);
    free(copy);
}

/* A bitmap's checksum lives in its group's descriptor. */
static void seal_bitmap(struct img *m, struct ext_fs *fs, const struct site *s)
{
    uint32_t c = ext_bitmap_csum(fs, (enum ext_bitmap)s->aux, m->bytes + s->off);
    uint64_t gd = (fs->first_data_block + 1u) * (uint64_t)fs->block_size + s->g * fs->desc_size;
    uint64_t lo = gd + (s->aux ? offsetof(struct ext_group_desc, inode_bitmap_csum_lo)
                               : offsetof(struct ext_group_desc, block_bitmap_csum_lo));
    poke(m, lo, &c, 2);
    if (fs->desc_size >= EXT_DESC_MIN_64) {
        uint16_t h = (uint16_t)(c >> 16);
        poke(m, gd + (s->aux ? offsetof(struct ext_group_desc, inode_bitmap_csum_hi)
                             : offsetof(struct ext_group_desc, block_bitmap_csum_hi)), &h, 2);
    }
    struct site d = { .kind = S_GDESC, .g = s->g, .off = gd, .len = fs->desc_size };
    seal(m, fs, &d);
}

static void edit(struct img *m, struct ext_fs *fs)
{
    const struct site *s = &sites[below(nsites)];
    static const unsigned widths[4] = { 1, 2, 4, 8 };
    unsigned width = widths[below(4)];
    while (width > s->len)
        width /= 2;
    uint64_t at = s->off + pick_field(s, width), old = 0;
    if (at + width > m->size)
        return;
    memcpy(&old, m->bytes + at, width);
    uint64_t v = edge(width, old);
    poke(m, at, &v, width);
    if (s->kind == S_RAW || !below(8))
        return;
    if (s->kind == S_BITMAP)
        seal_bitmap(m, fs, s);
    else
        seal(m, fs, s);
}

/* ---- the pass -------------------------------------------------------------------------- */

/* What the runs came to: mounted, refused (ERR_NOT_SUPPORTED), not
 * mounted for damage or a failed read, and mounted with damage found by
 * the walk. */
struct tally {
    unsigned mounted, refused, unmounted, damaged;
};

static void one_run(struct img *m, struct ext_fs *clean, struct tally *t)
{
    unsigned edits = below(10) < 6 ? 1 : 2 + (unsigned)below(7);
    for (unsigned i = 0; i < edits; i++)
        edit(m, clean);
    if (!below(20))
        m->dev_bytes = below(m->size + 1);
    m->fail_at = below(20) ? 0 : 1 + below(300);
    size_t cache = below(5) ? CACHE_BYTES : ext_mem_size(EXT_CACHE_MIN, clean->block_size);
    struct ext_fs fs;
    status_t st = mount_img(m, &fs, cache, below(2) ? EXT_MOUNT_READ_ONLY : 0);
    if (st == OK) {
        struct walk w = { .sites = false };
        walk_all(&fs, &w);
        if (fs.cache.ndirty || fs.cache.writes)
            fail("a read changed something");
        t->mounted++;
        t->damaged += (fs.ro & EXT_RO_DAMAGED) != 0;
    } else {
        *(st == ERR_NOT_SUPPORTED ? &t->refused : &t->unmounted) += 1;
    }
    m->fail_at = 0;
    undo(m);
}

void fuzz(struct img *m, unsigned n)
{
    char label[96];
    snprintf(label, sizeof(label), "%s: listing its sites", m->name);
    where = label;
    static uint64_t touched_blocks[65536];
    touched = touched_blocks;
    struct ext_fs clean;
    list_sites(m, &clean);
    uint32_t pristine = ext_crc32c(~0u, m->bytes, m->size);
    uint64_t seed = rng;
    struct tally t = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < n; i++) {
        snprintf(label, sizeof(label), "%s: fuzz run %u of %u (seed %#llx at the start)", m->name,
                 i, n, (unsigned long long)seed);
        one_run(m, &clean, &t);
        if ((i % 256 == 255 || i == n - 1) && ext_crc32c(~0u, m->bytes, m->size) != pristine)
            fail("the image changed under the fuzz pass");
    }
    printf("extcheck: %s: %u sites, %u fuzzed images: %u mounted (damage found in %u), "
           "%u refused, %u not mounted\n", m->name, nsites, n, t.mounted, t.damaged, t.refused,
           t.unmounted);
}
