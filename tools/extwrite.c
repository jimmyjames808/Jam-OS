/* extcheck's write tests, part one: the device and helpers every write
 * test uses (tools/extwrite.h), and the tests of blocks and inodes taken
 * and given back. tools/extfile.c has the extent trees and file writes.
 *
 * A test that fails without the behaviour it checks, for each:
 *   - a group never written (BLOCK_UNINIT) gets a bitmap with its
 *     metadata marked: the block allocated there is past its superblock
 *     copy (else the copy is handed out), and the volume checks whole;
 *   - the counts: after every allocation and free the bitmap, the
 *     descriptor and the superblock agree (extverify.c), and a volume
 *     filled to the last block and emptied again is where it started;
 *   - superblock counts that disagree with the groups' are mended;
 *   - freeing a group's metadata or a free block is damage (ERR_IO);
 *   - inodes: every free inode taken and given back, uninitialised inode
 *     bitmaps made, itable_unused lowered (a reader would not see the
 *     inode otherwise), directories counted, a directory put in the flex
 *     group with room, the fields written as asked and read back after a
 *     remount, an inode with blocks refused, a reserved one refused;
 *   - a volume that may not be written refuses every writer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extwrite.h"

const char *save_dir;

/* ---- the device ------------------------------------------------------------------------ */

static status_t wd_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    struct wfs *w = ctx;
    if (!len || off % 1024 || len % 1024 || off > w->size || len > w->size - off)
        fail("%s: a read of %u at %llu", w->name, len, (unsigned long long)off);
    memcpy(buf, w->bytes + off, len);
    return OK;
}

static status_t wd_write(void *ctx, uint64_t off, uint32_t len, const void *buf)
{
    struct wfs *w = ctx;
    if (!len || off % w->fs.block_size || len % w->fs.block_size || off > w->size ||
        len > w->size - off)
        fail("%s: a write of %u at %llu: not whole blocks inside the volume", w->name, len,
             (unsigned long long)off);
    memcpy(w->bytes + off, buf, len);
    return OK;
}

static void mount_bytes(struct wfs *w)
{
    struct ext_dev dev = { .ctx = w, .bytes = w->size, .read = wd_read, .write = wd_write };
    status_t st = ext_mount(&w->fs, &dev, w->cache, CACHE_BYTES, 0);
    if (st != OK || w->fs.ro)
        fail("%s: mount for writing: %d, ro %#x (%s)", w->name, st, w->fs.ro,
             w->fs.why ? w->fs.why : "");
}

void wmount(struct wfs *w, const struct img *m)
{
    memset(w, 0, sizeof(*w));
    w->name = m->name;
    w->size = m->size;
    w->bytes = malloc(m->size);
    w->cache = malloc(CACHE_BYTES);
    if (!w->bytes || !w->cache)
        fail("out of memory");
    memcpy(w->bytes, m->bytes, m->size);
    mount_bytes(w);
    w->free0 = ext_free_blocks(&w->fs);
    w->ifree0 = w->fs.sb.free_inodes_count;
}

void wsync(struct wfs *w)
{
    check_pins(&w->fs);
    status_t st = ext_sync(&w->fs);
    if (st != OK)
        fail("%s: ext_sync: %d", w->name, st);
}

void wremount(struct wfs *w)
{
    wsync(w);
    mount_bytes(w);
}

void wdone(struct wfs *w)
{
    free(w->bytes);
    free(w->cache);
    w->bytes = w->cache = NULL;
}

void wcounts_back(struct wfs *w)
{
    if (ext_free_blocks(&w->fs) != w->free0 || w->fs.sb.free_inodes_count != w->ifree0)
        fail("%s: %llu blocks and %u inodes free, not the %llu and %u it started with", w->name,
             (unsigned long long)ext_free_blocks(&w->fs), w->fs.sb.free_inodes_count,
             (unsigned long long)w->free0, w->ifree0);
}

/* ---- files ----------------------------------------------------------------------------- */

void new_file(struct wfs *w, uint32_t gen, struct ext_inode *out)
{
    struct ext_inode_spec spec = { .parent = EXT_ROOT_INO, .mode = EXT_S_IFREG | 0644,
                                   .links = 1, .uid = 1000, .gid = 1000, .generation = gen,
                                   .time = { .sec = 1760000000, .nsec = 5 } };
    status_t st = ext_inode_new(&w->fs, &spec, out);
    if (st != OK)
        fail("%s: ext_inode_new: %d (%s)", w->name, st, w->fs.why ? w->fs.why : "");
}

void pattern(uint32_t seed, uint64_t off, uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint64_t v = ((off + i) ^ (uint64_t)seed << 44) * 0x9E3779B97F4A7C15ull;
        v ^= v >> 29;
        buf[i] = (uint8_t)(1 + v % 255);   /* never 0: a hole can't pass for it */
    }
}

void write_pat(struct wfs *w, struct ext_inode *in, uint64_t off, size_t n)
{
    uint8_t *buf = malloc(n ? n : 1);
    if (!buf)
        fail("out of memory");
    pattern(in->ino, off, buf, n);
    size_t got = 0;
    status_t st = ext_write(&w->fs, in, off, buf, n, &got);
    free(buf);
    if (st != OK || got != n)
        fail("%s: inode %u: write %zu at %llu: %d, %zu (%s)", w->name, in->ino, n,
             (unsigned long long)off, st, got, w->fs.why ? w->fs.why : "");
    check_pins(&w->fs);
}

static void expect(struct wfs *w, struct ext_inode *in, uint64_t off, size_t n, bool zero)
{
    uint8_t *want = malloc(n ? n : 1), *got = malloc(n ? n : 1);
    if (!want || !got)
        fail("out of memory");
    if (zero)
        memset(want, 0, n);
    else
        pattern(in->ino, off, want, n);
    size_t m;
    status_t st = ext_read(&w->fs, in, off, got, n, &m);
    if (st != OK || m != n)
        fail("%s: inode %u: read %zu at %llu: %d, %zu", w->name, in->ino, n,
             (unsigned long long)off, st, m);
    for (size_t i = 0; i < n; i++)
        if (want[i] != got[i])
            fail("%s: inode %u: byte %llu is %#x, not %#x", w->name, in->ino,
                 (unsigned long long)(off + i), got[i], want[i]);
    free(want);
    free(got);
    check_pins(&w->fs);
}

void expect_pat(struct wfs *w, struct ext_inode *in, uint64_t off, size_t n)
{
    expect(w, in, off, n, false);
}

void expect_zero(struct wfs *w, struct ext_inode *in, uint64_t off, size_t n)
{
    expect(w, in, off, n, true);
}

unsigned truncate_all(struct wfs *w, struct ext_inode *in, uint64_t size)
{
    unsigned steps = 0;
    for (bool done = false; !done; steps++) {
        status_t st = ext_truncate(&w->fs, in, size, &done);
        if (st != OK)
            fail("%s: inode %u: truncate to %llu: %d (%s)", w->name, in->ino,
                 (unsigned long long)size, st, w->fs.why ? w->fs.why : "");
        if (ext_isize(in) != size)
            fail("%s: inode %u: size %llu after a step, not %llu", w->name, in->ino,
                 (unsigned long long)ext_isize(in), (unsigned long long)size);
        wsync(w);
    }
    return steps;
}

uint32_t tree_depth(const struct ext_inode *in)
{
    return ((const struct ext_extent_header *)in->d.block)->depth;
}

uint32_t tree_extents(struct wfs *w, struct ext_inode *in)
{
    uint32_t n = 0;
    for (uint64_t lblk = 0; lblk < (uint64_t)1 << 32;) {
        struct ext_run r;
        if (ext_map(&w->fs, in, (uint32_t)lblk, &r) != OK || !r.len)
            fail("%s: inode %u: no run at %llu", w->name, in->ino, (unsigned long long)lblk);
        n += r.kind != EXT_RUN_HOLE;
        lblk += r.len;
    }
    return n;
}

/* A name for ino in directory dir (a plain one: no index), in the first
 * block with room for it, or a new block. */
static void link_into(struct ext_fs *fs, struct ext_inode *dir, const char *name, uint32_t ino)
{
    uint32_t need = ext_dirent_size((uint32_t)strlen(name)), bs = fs->block_size;
    uint32_t end = bs - (fs->csum ? EXT_DIR_TAIL_SIZE : 0);
    uint32_t nblk = (uint32_t)(ext_isize(dir) >> fs->block_log);
    struct ext_dirent *d = NULL;
    struct ext_buf *b = NULL;
    for (uint32_t lblk = 0; lblk < nblk && !d; lblk++) {
        enum ext_dirblk kind;
        if (ext_dir_block(fs, dir, lblk, &b, &kind) != OK || kind != EXT_DIRBLK_LEAF)
            fail("link: directory block %u", lblk);
        for (uint32_t off = 0, rl; off < end && !d; off += rl) {
            struct ext_dirent *e = (struct ext_dirent *)(b->data + off);
            rl = ext_rec_len(fs, e->rec_len);
            uint32_t used = e->inode ? ext_dirent_size(e->name_len) : 0;
            if (rl - used < need)
                continue;
            if (used)
                e->rec_len = (uint16_t)used;
            d = (struct ext_dirent *)(b->data + off + used);
            d->rec_len = (uint16_t)(rl - used);
        }
        if (!d)
            ext_brelse(fs, b);
    }
    if (!d) {   /* a new block at the directory's end */
        struct ext_run r;
        if (ext_map_alloc(fs, dir, nblk, 1, &r) != OK || ext_bnew(fs, r.pblk, &b) != OK)
            fail("link: a new directory block");
        d = (struct ext_dirent *)b->data;
        d->rec_len = (uint16_t)end;
        uint64_t size = ext_isize(dir) + bs;
        dir->d.size_lo = (uint32_t)size;
        dir->d.size_high = (uint32_t)(size >> 32);
        if (ext_iput(fs, dir) != OK)
            fail("link: the directory's inode");
    }
    d->inode = ino;
    d->name_len = (uint8_t)strlen(name);
    d->file_type = EXT_FT_REG;
    memcpy(d->name, name, d->name_len);
    if (fs->csum)
        ext_dirleaf_csum_set(fs, dir->seed, b->data);
    ext_bdirty(fs, b);
    ext_brelse(fs, b);
}

void save_linked(struct wfs *w, const char *what, const uint32_t *files, const char *const *names,
                 unsigned n)
{
    if (!save_dir)
        return;
    wsync(w);
    struct wfs c = *w;
    c.bytes = malloc(w->size);
    c.cache = malloc(CACHE_BYTES);
    if (!c.bytes || !c.cache)
        fail("out of memory");
    memcpy(c.bytes, w->bytes, w->size);
    mount_bytes(&c);
    struct ext_inode dir;
    lookup(&c.fs, "emptydir", &dir);
    for (unsigned i = 0; i < n; i++)
        link_into(&c.fs, &dir, names[i], files[i]);
    wsync(&c);
    verify_volume(&c.fs);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s-%s.img", save_dir, w->name, what);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(c.bytes, 1, c.size, f) != c.size || fclose(f))
        fail("%s: can't write it", path);
    wdone(&c);
}

/* ---- blocks ---------------------------------------------------------------------------- */

static uint64_t balloc1(struct wfs *w, uint64_t goal)
{
    uint64_t blk;
    uint32_t n;
    status_t st = ext_balloc(&w->fs, goal, 1, &blk, &n);
    if (st != OK || n != 1)
        fail("%s: balloc at %llu: %d", w->name, (unsigned long long)goal, st);
    return blk;
}

/* An uninitialised group's first allocation skips its metadata. */
static void check_uninit_group(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    for (uint32_t g = 1; g < w.fs.groups; g++) {
        struct ext_group gr;
        if (ext_group_get(&w.fs, g, &gr) != OK || !(gr.flags & EXT_BG_BLOCK_UNINIT))
            continue;
        uint64_t first = ext_group_first(&w.fs, g), blk = balloc1(&w, first);
        uint64_t meta = ext_group_has_super(&w.fs, g) ? 1 + w.fs.gdt_blocks : 0;
        if (w.fs.sb.feature_compat & EXT_COMPAT_RESIZE_INODE && meta)
            meta += w.fs.sb.reserved_gdt_blocks;
        if (blk < first + meta || blk >= first + w.fs.blocks_per_group)
            fail("%s: group %u's first block for a file is %llu", w.name, g,
                 (unsigned long long)blk);
        if (ext_group_get(&w.fs, g, &gr) != OK || gr.flags & EXT_BG_BLOCK_UNINIT)
            fail("%s: group %u still uninitialised", w.name, g);
        if (ext_bfree(&w.fs, blk, 1) != OK)
            fail("%s: bfree", w.name);
        wsync(&w);
        verify_volume(&w.fs);
    }
    wcounts_back(&w);
    wdone(&w);
}

/* Fill the volume to its last block, then give every block back. */
static void check_fill(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    static uint64_t runs[1 << 16][2];
    unsigned n = 0;
    for (;;) {
        uint64_t blk;
        uint32_t got;
        status_t st = ext_balloc(&w.fs, rnd() % w.fs.blocks, 1 + (uint32_t)below(700), &blk, &got);
        if (st == ERR_NO_SPACE)
            break;
        if (st != OK || n == 1 << 16)
            fail("%s: filling: %d", w.name, st);
        runs[n][0] = blk;
        runs[n++][1] = got;
    }
    if (ext_free_blocks(&w.fs))
        fail("%s: full, yet %llu free", w.name, (unsigned long long)ext_free_blocks(&w.fs));
    for (uint32_t g = 0; g < w.fs.groups; g++) {
        struct ext_group gr;
        if (ext_group_get(&w.fs, g, &gr) != OK || gr.free_blocks)
            fail("%s: full, yet group %u has free blocks", w.name, g);
    }
    wsync(&w);
    for (unsigned i = 0; i < n; i++)   /* in a shuffled order */
        if (ext_bfree(&w.fs, runs[(i * 7919u) % n][0], runs[(i * 7919u) % n][1]) != OK)
            fail("%s: emptying", w.name);
    wremount(&w);
    wcounts_back(&w);
    verify_volume(&w.fs);
    wdone(&w);
}

/* Superblock counts that disagree with the groups' are set from them. */
static void check_count_free(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    w.fs.sb.free_blocks_count_lo += 7;
    w.fs.sb.free_inodes_count -= 3;
    if (ext_count_free(&w.fs) != OK)
        fail("%s: count_free", w.name);
    wcounts_back(&w);
    wremount(&w);
    wcounts_back(&w);
    verify_volume(&w.fs);
    wdone(&w);
}

/* Freeing what isn't a file's is damage. */
static void check_bad_frees(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    struct ext_group gr;
    if (ext_group_get(&w.fs, 0, &gr) != OK)
        fail("%s: group 0", w.name);
    status_t st = ext_bfree(&w.fs, gr.inode_table + 1, 1);
    if (st != ERR_IO || strcmp(w.fs.why, "freeing a group's metadata"))
        fail("%s: freeing an inode table block: %d", w.name, st);
    wdone(&w);
    wmount(&w, m);
    uint64_t blk = balloc1(&w, 0);
    if (ext_bfree(&w.fs, blk, 1) != OK)
        fail("%s: bfree", w.name);
    st = ext_bfree(&w.fs, blk, 1);
    if (st != ERR_IO || strcmp(w.fs.why, "freeing a free block"))
        fail("%s: freeing a free block: %d", w.name, st);
    wdone(&w);
}

/* ---- inodes ---------------------------------------------------------------------------- */

/* Free in, then fill its slot's bytes past the fields with garbage on the
 * disk (an old inode's attributes), and mount again. */
static void must_free_and_dirty(struct wfs *w, struct ext_inode *in)
{
    uint32_t ino = in->ino;
    if (ext_inode_free(&w->fs, in, 1) != OK)
        fail("%s: free", w->name);
    wsync(w);
    uint64_t blk;
    uint32_t off;
    if (ext_iloc(&w->fs, ino, &blk, &off) != OK)
        fail("%s: iloc", w->name);
    memset(w->bytes + (blk << w->fs.block_log) + off + sizeof(struct ext_disk_inode), 0x5A,
           w->fs.inode_size - sizeof(struct ext_disk_inode));
    wremount(w);
}

static void check_fields(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    struct ext_inode_spec spec = { .parent = EXT_ROOT_INO, .mode = EXT_S_IFREG | 04755,
                                   .links = 3, .uid = 70000, .gid = 70001,
                                   .generation = 0x12345678,
                                   .time = { .sec = (int64_t)1 << 33, .nsec = 123456789 } };
    struct ext_inode in, back;
    if (ext_inode_new(&w.fs, &spec, &in) != OK)
        fail("%s: new inode", w.name);
    uint32_t ino = in.ino;
    wremount(&w);
    if (ext_iget(&w.fs, ino, 0, &back) != OK)
        fail("%s: the new inode %u isn't there after a remount", w.name, ino);
    struct ext_time t;
    static const enum ext_time_which all[] = { EXT_ATIME, EXT_CTIME, EXT_MTIME, EXT_CRTIME };
    for (unsigned i = 0; i < 4; i++) {
        ext_itime(&back, all[i], &t);
        if (t.sec != spec.time.sec || t.nsec != spec.time.nsec)
            fail("%s: time %u read back as %lld.%u", w.name, i, (long long)t.sec, t.nsec);
    }
    if (back.d.mode != spec.mode || ext_iuid(&back) != 70000 || ext_igid(&back) != 70001 ||
        back.d.links_count != 3 || back.d.generation != 0x12345678 || back.d.extra_isize != 32 ||
        !(back.d.flags & EXT_EXTENTS_FL) || ext_isize(&back) || ext_iblocks512(&w.fs, &back))
        fail("%s: the new inode's fields read back wrong", w.name);
    /* A slot that held something: every byte of the new inode is zero
     * but what is filled (the attributes' room included). */
    must_free_and_dirty(&w, &back);
    struct ext_inode again;
    if (ext_inode_new(&w.fs, &spec, &again) != OK || again.ino != ino)
        fail("%s: the freed slot %u wasn't taken again", w.name, ino);
    wsync(&w);
    uint64_t blk;
    uint32_t off;
    if (ext_iloc(&w.fs, ino, &blk, &off) != OK)
        fail("%s: iloc", w.name);
    const uint8_t *raw = w.bytes + (blk << w.fs.block_log) + off;
    for (uint32_t i = sizeof(struct ext_disk_inode); i < w.fs.inode_size; i++)
        if (raw[i])
            fail("%s: byte %u of a new inode is %#x", w.name, i, raw[i]);
    spec.mode = EXT_S_IFCHR | 0600;
    if (ext_inode_new(&w.fs, &spec, &in) != OK || in.d.flags & EXT_EXTENTS_FL)
        fail("%s: a device inode with an extent tree", w.name);
    spec.mode = 0777;   /* no type */
    if (ext_inode_new(&w.fs, &spec, &in) != ERR_INVALID_ARGS)
        fail("%s: an inode of no type", w.name);
    if (ext_iget(&w.fs, EXT_ROOT_INO, 0, &in) != OK ||
        ext_inode_free(&w.fs, &in, 1) != ERR_INVALID_ARGS)
        fail("%s: the root freed", w.name);
    new_file(&w, 1, &in);
    write_pat(&w, &in, 0, 10);
    if (ext_inode_free(&w.fs, &in, 1) != ERR_BAD_STATE)
        fail("%s: an inode with blocks freed", w.name);
    wdone(&w);
}

/* Every free inode taken (files and directories), then given back. */
static void check_all_inodes(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    uint32_t n = 0, cap = w.fs.sb.free_inodes_count;
    struct ext_inode *ins = calloc(cap + 1, sizeof(*ins));
    for (;; n++) {
        struct ext_inode_spec spec = { .parent = n ? ins[n / 2].ino : EXT_ROOT_INO,
                                       .mode = (n % 5 ? EXT_S_IFREG : EXT_S_IFDIR) | 0755,
                                       .links = 1, .uid = 1000, .gid = 1000, .generation = n,
                                       .time = { .sec = 1760000000, .nsec = 0 } };
        status_t st = n <= cap ? ext_inode_new(&w.fs, &spec, &ins[n]) : ERR_INTERNAL;
        if (st == ERR_NO_SPACE)
            break;
        if (st != OK)
            fail("%s: inode %u of %u: %d (%s)", w.name, n, cap, st, w.fs.why ? w.fs.why : "");
        if (n % 37 == 0)
            wsync(&w);
    }
    if (n != cap || w.fs.sb.free_inodes_count)
        fail("%s: %u inodes taken of %u free", w.name, n, cap);
    wsync(&w);
    verify_volume(&w.fs);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (i * 101u) % n;   /* every one, shuffled */
        if (ext_inode_free(&w.fs, &ins[k], 1760000001) != OK)
            fail("%s: freeing inode %u", w.name, ins[k].ino);
    }
    struct ext_inode again;
    if (ext_inode_free(&w.fs, &ins[0], 1) != ERR_IO)
        fail("%s: an inode freed twice", w.name);
    free(ins);
    wdone(&w);
    wmount(&w, m);   /* the double free marked it damaged: count on a clean mount */
    struct ext_inode a, b;
    new_file(&w, 7, &a);
    new_file(&w, 8, &b);
    if (ext_inode_free(&w.fs, &a, 5) != OK || ext_inode_free(&w.fs, &b, 5) != OK)
        fail("%s: free", w.name);
    if (ext_iget(&w.fs, a.ino, 0, &again) != ERR_NOT_FOUND)
        fail("%s: a freed inode is still there", w.name);
    wremount(&w);
    wcounts_back(&w);
    verify_volume(&w.fs);
    wdone(&w);
}

/* With two groups to a flex group, a directory goes to the flex group
 * with more than the average free inodes and blocks. */
static void check_dir_spread(const struct img *m)
{
    struct wfs w;
    wmount(&w, m);
    if (w.fs.groups < 4 || !w.fs.flex_bg)
        fail("%s: needs 4 groups with flex_bg", w.name);
    w.fs.sb.log_groups_per_flex = 1;   /* in memory: this mount is thrown away */
    uint64_t inodes[2] = { 0, 0 };
    for (uint32_t g = 0; g < 4; g++) {
        struct ext_group gr;
        if (ext_group_get(&w.fs, g, &gr) != OK)
            fail("group %u", g);
        inodes[g / 2] += gr.free_inodes;
    }
    uint32_t want = inodes[1] > inodes[0] ? 1 : 0;
    struct ext_inode_spec spec = { .parent = EXT_ROOT_INO, .mode = EXT_S_IFDIR | 0755,
                                   .links = 2, .uid = 1000, .gid = 1000, .generation = 1,
                                   .time = { .sec = 1760000000, .nsec = 0 } };
    struct ext_inode in;
    if (ext_inode_new(&w.fs, &spec, &in) != OK)
        fail("%s: a directory", w.name);
    uint32_t g = (in.ino - 1) / w.fs.inodes_per_group;
    if (g / 2 != want)
        fail("%s: a directory in group %u, not flex group %u (%llu, %llu free inodes)", w.name, g,
             want, (unsigned long long)inodes[0], (unsigned long long)inodes[1]);
    wdone(&w);
}

/* ---- the entry point ------------------------------------------------------------------- */

/* A volume that may not be written: every writer refuses. */
static void check_refused(const struct img *m)
{
    uint8_t *cache = malloc(CACHE_BYTES);
    struct wfs w = { .name = m->name, .bytes = m->bytes, .size = m->size, .cache = cache };
    struct ext_dev dev = { .ctx = &w, .bytes = w.size, .read = wd_read, .write = wd_write };
    if (ext_mount(&w.fs, &dev, cache, CACHE_BYTES, 0) != OK || !w.fs.ro)
        fail("%s: it should mount read-only", m->name);
    uint64_t blk;
    uint32_t n;
    struct ext_inode in;
    struct ext_inode_spec spec = { .mode = EXT_S_IFREG | 0644, .links = 1 };
    size_t got;
    if (ext_balloc(&w.fs, 0, 1, &blk, &n) != ERR_ACCESS_DENIED ||
        ext_inode_new(&w.fs, &spec, &in) != ERR_ACCESS_DENIED)
        fail("%s: a read-only volume allocated", m->name);
    lookup(&w.fs, "small", &in);
    if (ext_write(&w.fs, &in, 0, "x", 1, &got) != ERR_ACCESS_DENIED)
        fail("%s: a read-only volume written", m->name);
    if (w.fs.cache.ndirty)
        fail("%s: a read-only volume has dirty blocks", m->name);
    free(cache);
}

void check_writes(const struct img *m, bool writable)
{
    where = m->name;
    if (!writable) {
        check_refused(m);
        return;
    }
    check_uninit_group(m);
    check_fill(m);
    check_count_free(m);
    check_bad_frees(m);
    check_fields(m);
    check_all_inodes(m);
    check_dir_spread(m);
    check_file_writes(m);
}
