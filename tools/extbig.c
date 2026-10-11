/* extbig: the names under load. 10,000 entries in one directory: made,
 * found through the hash (one build, then a probe each: a block read at
 * most), listed, half removed, a readdir resumed across removals and adds
 * (nothing skipped or repeated), the gaps filled again without the
 * directory growing. And a storm of random operations (create, mkdir,
 * symlink, unlink, rmdir, link, rename with each flag) against a model of
 * the tree, every answer the model's, one transaction in ten given up
 * (the model's step with it), checked against the model and extinv.c's
 * invariants every 50 steps and remounted every 400. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extvol.h"

/* ---- 10,000 entries in one directory ----------------------------------------------------- */

#define BIG 10000u

static struct ext_name big_name(char *buf, const char *tag, unsigned i)
{
    snprintf(buf, 32, "%s-%05u-entry", tag, i);
    return nm(buf);
}

/* tag and number of a name big_name made; false for another name. */
static bool big_parse(const char *name, char *tag, unsigned *i)
{
    return sscanf(name, "%3[a-z]-%5u-entry", tag, i) == 2 && *i < BIG;
}

static void big_create(struct vol *v, struct ext_inode *dir, const char *tag, unsigned from,
                       unsigned to, unsigned step, uint32_t *inos)
{
    char buf[32];
    for (unsigned i = from; i < to; i += step) {
        struct ext_name n = big_name(buf, tag, i);
        struct ext_attrs a = attrs(EXT_S_IFREG | 0644);
        struct ext_inode in;
        expect(ext_create(&v->fs, dir, &n, &a, &in), OK, buf);
        if (inos)
            inos[i] = in.ino;
        if (i % 100 == 99)
            vol_sync(v);
    }
    vol_sync(v);
}

/* Every big name found through the hash: one build, then probes. */
static void big_lookups(struct vol *v, const uint32_t *inos, unsigned step)
{
    struct ext_inode dir;
    walk(v, "big", &dir);
    struct ext_names *h = v->fs.names;
    uint64_t builds = h->builds, hits = h->hits, misses = h->misses, reads = v->fs.cache.reads;
    char buf[32];
    for (unsigned i = 0; i < BIG; i++) {
        struct ext_name n = big_name(buf, "old", i);
        struct ext_dentry d;
        status_t st = ext_lookup(&v->fs, &dir, &n, &d);
        expect(st, i % step ? ERR_NOT_FOUND : OK, buf);
        if (st == OK && d.ino != inos[i])
            fail("%s: inode %u, not %u", buf, d.ino, inos[i]);
    }
    uint64_t blocks = ext_isize(&dir) / v->fs.block_size;
    if (h->builds != builds + 1 || h->hits - hits != BIG / step ||
        h->misses - misses != BIG - BIG / step || h->linear)
        fail("the hash: %llu builds, %llu hits, %llu misses, %llu walks",
             (unsigned long long)(h->builds - builds), (unsigned long long)(h->hits - hits),
             (unsigned long long)(h->misses - misses), (unsigned long long)h->linear);
    if (v->fs.cache.reads - reads > blocks + 2)
        fail("%llu reads for lookups in a directory of %llu blocks",
             (unsigned long long)(v->fs.cache.reads - reads), (unsigned long long)blocks);
}

/* A readdir resumed across removals and adds: nothing seen twice, every
 * name there the whole time seen once. */
static void big_resume(struct vol *v)
{
    static uint8_t seen[2][BIG], gone[BIG];
    memset(seen, 0, sizeof(seen));
    memset(gone, 0, sizeof(gone));
    struct ext_inode dir;
    walk(v, "big", &dir);
    struct ext_dentry d;
    char tag[4], buf[32];
    unsigned i, n = 0;
    uint64_t pos = 0;
    for (; n < 1000; n++) {
        expect(ext_dir_next(&v->fs, &dir, &pos, &d), OK, "readdir");
        if (big_parse(d.name, tag, &i))
            seen[0][i]++;
    }
    for (i = 0; i < BIG; i += 10) {   /* every fifth even one, seen or not */
        char path[48];
        big_name(buf, "old", i);
        snprintf(path, sizeof(path), "big/%s", buf);
        expect(rm(v, path, 0, NULL), OK, "unlink while reading");
        gone[i] = 1;
    }
    walk(v, "big", &dir);
    big_create(v, &dir, "add", 1, BIG, 20, NULL);
    for (;;) {
        expect(ext_dir_next(&v->fs, &dir, &pos, &d), OK, "readdir");
        if (!d.ino)
            break;
        if (big_parse(d.name, tag, &i) && !strcmp(tag, "old"))
            seen[0][i]++;
        if (big_parse(d.name, tag, &i) && !strcmp(tag, "add"))
            seen[1][i]++;
    }
    for (i = 0; i < BIG; i++) {
        if (seen[0][i] > 1 || seen[1][i] > 1)
            fail("readdir gave %s-%05u twice", seen[0][i] > 1 ? "old" : "add", i);
        if (i % 2 == 0 && !gone[i] && seen[0][i] != 1)
            fail("readdir missed old-%05u", i);
    }
}

void check_big_dir(void)
{
    where = "10,000 entries in one directory";
    struct ext_mkfs p = { .block_size = 4096, .inodes = 16384, .uid = 1000, .gid = 1000,
                          .now = clock_now };
    struct vol v;
    vol_new(&v, 64u << 20);
    expect(vol_mkfs(&v, &p), OK, "mkfs");
    vol_mount(&v);
    expect(make(&v, "big", EXT_S_IFDIR | 0755, NULL), OK, "mkdir big");
    struct ext_inode dir;
    walk(&v, "big", &dir);
    static uint32_t inos[BIG];
    big_create(&v, &dir, "old", 0, BIG, 1, inos);
    struct inv inv;
    vol_remount(&v, &inv);
    if (inv.names != BIG + 2)
        fail("%u names, not %u", inv.names, BIG + 2);
    big_lookups(&v, inos, 1);
    walk(&v, "big", &dir);
    uint64_t size = ext_isize(&dir);
    for (unsigned i = 1; i < BIG; i += 2) {
        char buf[32];
        big_name(buf, "old", i);
        uint32_t o;
        struct ext_name n = nm(buf);
        struct ext_time t = now();
        expect(ext_unlink(&v.fs, &dir, &n, 0, &t, &o), OK, buf);
        if (o != inos[i])
            fail("%s: orphan %u, not %u", buf, o, inos[i]);
        expect(ext_orphan_drop(&v.fs, o, &t), OK, "drop");
        if (i % 100 == 99)
            vol_sync(&v);
    }
    vol_remount(&v, &inv);
    big_lookups(&v, inos, 2);
    walk(&v, "big", &dir);
    big_create(&v, &dir, "new", 1, BIG, 2, NULL);   /* names as long: the gaps hold them */
    walk(&v, "big", &dir);
    if (ext_isize(&dir) != size)
        fail("the directory grew from %llu to %llu bytes", (unsigned long long)size,
             (unsigned long long)ext_isize(&dir));
    big_resume(&v);
    vol_remount(&v, &inv);
    vol_free(&v);
}

/* ---- a storm against a model ------------------------------------------------------------- */

#define S_NODES 48
#define S_NAMES 6

/* The model: inodes (node 0 the root) and each directory's names. */
struct model {
    struct snode {
        bool used, dir;
        uint32_t ino;
        uint32_t parent;             /* a directory's: its parent node */
        uint32_t name;               /* and its name there */
    } node[S_NODES];
    uint8_t tab[S_NODES][S_NAMES];   /* node + 1 named by (directory, name), 0: none */
};

static uint32_t s_names(const struct model *md, uint32_t node)
{
    uint32_t n = 0;
    for (uint32_t d = 0; d < S_NODES; d++)
        for (uint32_t i = 0; i < S_NAMES; i++)
            n += md->tab[d][i] == node + 1;
    return n;
}

static bool s_empty(const struct model *md, uint32_t dir)
{
    for (uint32_t i = 0; i < S_NAMES; i++)
        if (md->tab[dir][i])
            return false;
    return true;
}

/* Is x the node `top` or below it? */
static bool s_below(const struct model *md, uint32_t x, uint32_t top)
{
    for (uint32_t n = 0; n < S_NODES; n++) {
        if (x == top)
            return true;
        if (x == 0)
            return false;
        x = md->node[x].parent;
    }
    fail("the model has a loop");
}

static void s_path(const struct model *md, uint32_t dir, uint32_t name, char *out, size_t cap)
{
    char tmp[512] = "";
    if (dir)
        s_path(md, md->node[dir].parent, md->node[dir].name, tmp, sizeof(tmp));
    snprintf(out, cap, "%s%sn%u", tmp, tmp[0] ? "/" : "", name);
}

static void s_dir_path(const struct model *md, uint32_t dir, char *out, size_t cap)
{
    if (!dir)
        snprintf(out, cap, "%s", "");
    else
        s_path(md, md->node[dir].parent, md->node[dir].name, out, cap);
}

static uint32_t s_pick_dir(const struct model *md)
{
    for (;;) {
        uint32_t n = (uint32_t)below(S_NODES);
        if (md->node[n].used && md->node[n].dir)
            return n;
    }
}

/* Mostly a name that is there (*d, *i): changes need something to change. */
static void s_pick_name(const struct model *md, uint32_t *d, uint32_t *i)
{
    for (unsigned tries = 0; tries < 16 && below(4); tries++) {
        *d = s_pick_dir(md);
        *i = (uint32_t)below(S_NAMES);
        if (md->tab[*d][*i])
            return;
    }
}

static int s_free_node(const struct model *md)
{
    for (uint32_t n = 1; n < S_NODES; n++)
        if (!md->node[n].used)
            return (int)n;
    return -1;
}

/* Make a file, directory or link at (d, i) in both. */
static void s_make(struct vol *v, struct model *md, uint32_t d, uint32_t i, int kind)
{
    char path[512];
    s_path(md, d, i, path, sizeof(path));
    int n = s_free_node(md);
    if (n < 0)
        return;
    struct ext_inode in;
    status_t st = kind == 2 ? symlink_to(v, path, "target", 6, &in)
                            : make(v, path, kind ? EXT_S_IFDIR | 0755 : EXT_S_IFREG | 0644, &in);
    expect(st, md->tab[d][i] ? ERR_ALREADY_EXISTS : OK, path);
    if (st != OK)
        return;
    md->node[n] = (struct snode){ .used = true, .dir = kind == 1, .ino = in.ino, .parent = d,
                                  .name = i };
    md->tab[d][i] = (uint8_t)(n + 1);
}

static void s_unlink(struct vol *v, struct model *md, uint32_t d, uint32_t i, bool rmdir)
{
    char path[512];
    s_path(md, d, i, path, sizeof(path));
    uint32_t n = md->tab[d][i];
    status_t want = !n ? ERR_NOT_FOUND
                  : md->node[n - 1].dir != rmdir ? (rmdir ? ERR_NOT_DIR : ERR_IS_DIR)
                  : rmdir && !s_empty(md, n - 1) ? ERR_NOT_EMPTY : OK;
    expect(rm(v, path, rmdir ? EXT_UNLINK_DIR : 0, NULL), want, path);
    if (want != OK)
        return;
    md->tab[d][i] = 0;
    if (!s_names(md, n - 1))
        md->node[n - 1].used = false;
}

static void s_link(struct vol *v, struct model *md, uint32_t d, uint32_t i)
{
    uint32_t fd = s_pick_dir(md), fi = (uint32_t)below(S_NAMES);
    s_pick_name(md, &fd, &fi);
    uint32_t n = md->tab[fd][fi];
    if (!n || md->node[n - 1].dir)
        return;
    char from[512], to[512];
    s_path(md, fd, fi, from, sizeof(from));
    s_path(md, d, i, to, sizeof(to));
    expect(link_to(v, from, to), md->tab[d][i] ? ERR_ALREADY_EXISTS : OK, to);
    if (!md->tab[d][i])
        md->tab[d][i] = (uint8_t)n;
}

/* What ext_rename must answer (in the order it checks). */
static status_t s_rename_want(const struct model *md, uint32_t d1, uint32_t i1, uint32_t d2,
                              uint32_t i2, uint32_t flags)
{
    uint32_t a = md->tab[d1][i1], b = md->tab[d2][i2];
    if (!a || (flags & EXT_RENAME_EXCHANGE && !b))
        return ERR_NOT_FOUND;
    if (flags & EXT_RENAME_NOREPLACE && b)
        return ERR_ALREADY_EXISTS;
    if (a == b)
        return OK;
    bool adir = md->node[a - 1].dir, bdir = b && md->node[b - 1].dir;
    if (!(flags & EXT_RENAME_EXCHANGE) && b) {
        if (adir != bdir)
            return adir ? ERR_NOT_DIR : ERR_IS_DIR;
        if (bdir && !s_empty(md, b - 1))
            return ERR_NOT_EMPTY;
    }
    if (d1 != d2 && adir && s_below(md, d2, a - 1))
        return ERR_INVALID_ARGS;
    if (d1 != d2 && flags & EXT_RENAME_EXCHANGE && bdir && s_below(md, d1, b - 1))
        return ERR_INVALID_ARGS;
    return OK;
}

static void s_rename(struct vol *v, struct model *md, uint32_t d1, uint32_t i1)
{
    uint32_t d2 = s_pick_dir(md), i2 = (uint32_t)below(S_NAMES), flags = (uint32_t)below(3);
    char from[512], to[512];
    s_path(md, d1, i1, from, sizeof(from));
    s_path(md, d2, i2, to, sizeof(to));
    status_t want = s_rename_want(md, d1, i1, d2, i2, flags);
    expect(mv(v, from, to, flags, NULL), want, to);
    uint32_t a = md->tab[d1][i1], b = md->tab[d2][i2];
    if (want != OK || a == b)
        return;
    if (flags & EXT_RENAME_EXCHANGE) {
        md->tab[d1][i1] = (uint8_t)b;
        md->tab[d2][i2] = (uint8_t)a;
        if (md->node[b - 1].dir)
            md->node[b - 1].parent = d1, md->node[b - 1].name = i1;
    } else {
        md->tab[d1][i1] = 0;
        md->tab[d2][i2] = (uint8_t)a;
        if (b && !s_names(md, b - 1))
            md->node[b - 1].used = false;
    }
    if (md->node[a - 1].dir)
        md->node[a - 1].parent = d2, md->node[a - 1].name = i2;
}

/* Every name in the model found with its inode; each directory's count. */
static void s_verify(struct vol *v, const struct model *md)
{
    char path[512];
    for (uint32_t d = 0; d < S_NODES; d++) {
        if (!md->node[d].used || !md->node[d].dir)
            continue;
        uint32_t names = 0;
        for (uint32_t i = 0; i < S_NAMES; i++) {
            uint32_t n = md->tab[d][i];
            s_path(md, d, i, path, sizeof(path));
            uint32_t ino = walk(v, path, NULL);
            if (ino != (n ? md->node[n - 1].ino : 0))
                fail("%s: inode %u, the model's %u", path, ino, n ? md->node[n - 1].ino : 0);
            names += n != 0;
        }
        struct ext_inode dir;
        struct ext_dentry de;
        s_dir_path(md, d, path, sizeof(path));
        walk(v, path, &dir);
        uint32_t seen = 0;
        for (uint64_t pos = 0;; seen++) {
            expect(ext_dir_next(&v->fs, &dir, &pos, &de), OK, "readdir");
            if (!de.ino)
                break;
        }
        if (seen != names + 2 + (d == 0))
            fail("%s: %u entries, the model's %u", path, seen, names + 2);
    }
    vol_check(v, NULL);
}

void check_storm(unsigned steps)
{
    where = "a storm against a model";
    struct vol v;
    small_vol(&v, 1024, 16u << 20);
    static struct model md, saved;
    memset(&md, 0, sizeof(md));
    md.node[0] = (struct snode){ .used = true, .dir = true, .ino = EXT_ROOT_INO };
    for (unsigned step = 0; step < steps; step++) {
        saved = md;
        struct ext_super sb = v.fs.sb;
        uint32_t d = s_pick_dir(&md), i = (uint32_t)below(S_NAMES), kind = (uint32_t)below(8);
        if (kind >= 3 && kind != 5)
            s_pick_name(&md, &d, &i);
        switch (kind) {
        case 0: s_make(&v, &md, d, i, 0); break;
        case 1: s_make(&v, &md, d, i, 1); break;
        case 2: s_make(&v, &md, d, i, 2); break;
        case 3: s_unlink(&v, &md, d, i, false); break;
        case 4: s_unlink(&v, &md, d, i, true); break;
        case 5: s_link(&v, &md, d, i); break;
        default: s_rename(&v, &md, d, i); break;
        }
        if (below(10) == 0) {   /* the transaction given up: the model's too */
            ext_discard(&v.fs);
            v.fs.sb = sb;
            md = saved;
        } else {
            vol_sync(&v);
        }
        if (step % 50 == 49)
            s_verify(&v, &md);
        if (step % 400 == 399)
            vol_remount(&v, NULL);
    }
    s_verify(&v, &md);
    vol_remount(&v, NULL);
    vol_free(&v);
}
