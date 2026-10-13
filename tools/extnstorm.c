/* The journal's names storm (tools/extjnl.c has the model): on a volume
 * U2b's mkfs made, requests of the library's real namespace changes, each
 * one transaction: directories made, files made with data, hard links,
 * symbolic links, unlinks (the orphan dropped in the same transaction),
 * renames that move, replace and exchange, data appended, files cut.
 * Each request's change is chosen from its own seed against the volume as
 * it is, so every run of the storm makes the same changes up to its cut;
 * a change the volume refuses (a name taken, a name missing) is given up
 * (ext_jnl_abort) and the request commits nothing.
 *
 * Its state is the whole listing (every name, inode, mode, link count,
 * size, time and the files' bytes) as one hash, and the superblock's
 * write time. A request frees blocks (an unlink, a replace, a cut) or
 * writes new data, never both (ext_alloc.h's rule). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ext_alloc.h>
#include <ext_mkfs.h>
#include <ext_names.h>
#include "extjnl.h"

#define NDIRS  3
#define NFILES 6
#define VOL_BYTES (16u << 20)

static uint64_t nrng;

static uint32_t pick(uint32_t n)
{
    nrng ^= nrng << 13;
    nrng ^= nrng >> 7;
    nrng ^= nrng << 17;
    return (uint32_t)(nrng % n);
}

static struct ext_name name_of(const char *s)
{
    return (struct ext_name){ .s = s, .len = (uint32_t)strlen(s) };
}

/* A change's times: request `at`'s, so every run stamps the same. */
static struct ext_time when(uint32_t at)
{
    return (struct ext_time){ .sec = 1760000000 + at, .nsec = at * 1000 };
}

static struct ext_attrs attrs_at(uint16_t mode, uint32_t at)
{
    return (struct ext_attrs){ .mode = mode, .uid = 1000, .gid = 1000,
                               .generation = 0x51000 + at, .now = when(at) };
}

/* Directory d's inode (d0, d1, d2 in the root). */
static status_t dir_of(struct ext_fs *fs, uint32_t d, struct ext_inode *out)
{
    char n[8];
    struct ext_inode root;
    struct ext_dentry e;
    snprintf(n, sizeof(n), "d%u", d);
    struct ext_name nm = name_of(n);
    status_t st = ext_iget(fs, EXT_ROOT_INO, 0, &root);
    if (st == OK)
        st = ext_lookup(fs, &root, &nm, &e);
    return st == OK ? ext_iget(fs, e.ino, 0, out) : st;
}

/* A random place: a directory and a file name in it. */
struct place {
    struct ext_inode dir;
    char name[8];
};

static status_t place(struct ext_fs *fs, struct place *p)
{
    snprintf(p->name, sizeof(p->name), "f%u", pick(NFILES));
    return dir_of(fs, pick(NDIRS), &p->dir);
}

/* The file at p (ERR_NOT_FOUND, ERR_IS_DIR: no file there; with
 * `links`, a symbolic link will do). */
static status_t file_at(struct ext_fs *fs, struct place *p, struct ext_inode *out, bool links)
{
    struct ext_dentry e;
    struct ext_name nm = name_of(p->name);
    status_t st = ext_lookup(fs, &p->dir, &nm, &e);
    if (st == OK)
        st = ext_iget(fs, e.ino, 0, out);
    if (st == OK && ext_itype(out) != EXT_FT_REG &&
        !(links && ext_itype(out) == EXT_FT_SYMLINK))
        st = ERR_IS_DIR;
    return st;
}

/* A place that holds a name (`reg`: a regular file), scanned from a
 * random one; ERR_NOT_FOUND when none does. */
static status_t taken(struct ext_fs *fs, struct place *p, bool reg)
{
    uint32_t start = pick(NDIRS * NFILES);
    for (uint32_t i = 0; i < NDIRS * NFILES; i++) {
        uint32_t k = (start + i) % (NDIRS * NFILES);
        struct ext_inode in;
        snprintf(p->name, sizeof(p->name), "f%u", k % NFILES);
        status_t st = dir_of(fs, k / NFILES, &p->dir);
        if (st == OK)
            st = file_at(fs, p, &in, !reg);
        if (st != ERR_NOT_FOUND && st != ERR_IS_DIR)
            return st;
    }
    return ERR_NOT_FOUND;
}

/* Drop the inode a removal left with no name, in this transaction. */
static status_t drop(struct ext_fs *fs, status_t st, uint32_t orphan, uint32_t at)
{
    struct ext_time t = when(at);
    return st == OK && orphan ? ext_orphan_drop(fs, orphan, &t) : st;
}

static status_t write_end(struct ext_fs *fs, struct ext_inode *in, uint32_t at)
{
    static uint8_t buf[2 * 4096];
    size_t len = 1 + pick(2 * fs->block_size), n;
    storm_pattern(buf, (uint32_t)sizeof(buf), 0xD0000 + at);
    return ext_write(fs, in, ext_isize(in), buf, len, &n);
}

static status_t make_file(struct ext_fs *fs, uint32_t at)
{
    struct place p;
    struct ext_inode in;
    status_t st = place(fs, &p);
    struct ext_name nm = name_of(p.name);
    struct ext_attrs a = attrs_at(EXT_S_IFREG | 0644, at);
    if (st == OK)
        st = ext_create(fs, &p.dir, &nm, &a, &in);
    return st == OK ? write_end(fs, &in, at) : st;
}

static status_t rename_op(struct ext_fs *fs, uint32_t at, uint32_t flags)
{
    struct place a, b;
    status_t st = taken(fs, &a, false);
    if (st == OK)
        st = flags & EXT_RENAME_EXCHANGE ? taken(fs, &b, false) : place(fs, &b);
    if (st != OK)
        return st;
    struct ext_rename r = { .from_dir = &a.dir, .from = name_of(a.name), .to_dir = &b.dir,
                            .to = name_of(b.name), .flags = flags, .now = when(at) };
    uint32_t orphan = 0;
    return drop(fs, ext_rename(fs, &r, &orphan), orphan, at);
}

static status_t link_op(struct ext_fs *fs, uint32_t at)
{
    struct place a, b;
    struct ext_inode in;
    struct ext_time t = when(at);
    status_t st = taken(fs, &a, false);
    if (st == OK)
        st = file_at(fs, &a, &in, true);
    if (st == OK)
        st = place(fs, &b);
    struct ext_name nm = name_of(b.name);
    return st == OK ? ext_link(fs, &b.dir, &nm, &in, &t) : st;
}

static status_t unlink_op(struct ext_fs *fs, uint32_t at)
{
    struct place p;
    struct ext_time t = when(at);
    uint32_t orphan = 0;
    status_t st = taken(fs, &p, false);
    struct ext_name nm = name_of(p.name);
    if (st == OK)
        st = ext_unlink(fs, &p.dir, &nm, 0, &t, &orphan);
    return drop(fs, st, orphan, at);
}

static status_t file_op(struct ext_fs *fs, uint32_t at, bool append)
{
    struct place p;
    struct ext_inode in;
    status_t st = taken(fs, &p, true);
    if (st == OK)
        st = file_at(fs, &p, &in, false);
    if (st != OK || append)
        return st == OK ? write_end(fs, &in, at) : st;
    uint64_t to = (ext_isize(&in) / 2) & ~(uint64_t)(fs->block_size - 1);
    for (bool done = false; st == OK && !done;)
        st = ext_truncate(fs, &in, to, &done);
    return st;
}

static status_t symlink_op(struct ext_fs *fs, uint32_t at)
{
    struct place p;
    struct ext_inode in;
    status_t st = place(fs, &p);
    struct ext_name nm = name_of(p.name), target = name_of(at % 2 ? "../d0/f1" : "f2");
    struct ext_attrs a = attrs_at(EXT_S_IFLNK | 0777, at);
    return st == OK ? ext_symlink(fs, &p.dir, &nm, &target, &a, &in) : st;
}

/* Request at's change. The first make the directories. */
static status_t change(struct ext_fs *fs, uint32_t at)
{
    if (at < NDIRS) {
        char n[8];
        struct ext_inode root, in;
        snprintf(n, sizeof(n), "d%u", at);
        struct ext_name nm = name_of(n);
        struct ext_attrs a = attrs_at(EXT_S_IFDIR | 0755, at);
        status_t st = ext_iget(fs, EXT_ROOT_INO, 0, &root);
        return st == OK ? ext_create(fs, &root, &nm, &a, &in) : st;
    }
    switch (pick(12)) {
    case 0:
    case 1:
    case 2:
        return make_file(fs, at);
    case 3:
    case 4:
        return link_op(fs, at);
    case 5:
        return unlink_op(fs, at);
    case 6:
        return rename_op(fs, at, 0);
    case 7:
    case 8:
        return rename_op(fs, at, EXT_RENAME_EXCHANGE);
    case 9:
        return file_op(fs, at, true);
    case 10:
        return file_op(fs, at, false);
    default:
        return symlink_op(fs, at);
    }
}

/* A refusal the volume gives a change that doesn't fit it. */
static bool refusal(status_t st)
{
    return st == ERR_NOT_FOUND || st == ERR_ALREADY_EXISTS || st == ERR_IS_DIR ||
           st == ERR_NOT_DIR || st == ERR_NOT_EMPTY || st == ERR_INVALID_ARGS;
}

void names_run(struct storm *s, struct run *r, bool snap)
{
    struct ext_fs *fs = &r->fs;
    for (uint32_t at = 0; at < s->nreq; at++) {
        nrng = (s->cfg->seed ^ (at + 1) * 0x9E3779B97F4A7C15ull) | 1;
        status_t st = ext_jnl_begin(fs, NULL);
        if (st == OK) {
            fs->sb.wtime = 1750000000 + at;
            st = ext_super_write(fs);
        }
        if (st == OK)
            st = change(fs, at);
        check_pins(fs);
        if (refusal(st)) {
            ext_jnl_abort(fs);
            continue;
        }
        if (st == OK)
            st = ext_jnl_commit(fs);
        if (st != OK) {
            r->why_stop = st;
            return;
        }
        if (snap)
            take_snaps(s, r);
    }
    r->why_stop = OK;
}

uint64_t listing_hash(struct ext_fs *fs)
{
    static struct lines l;
    list_volume(fs, &l);
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < l.n; i++)
        h = (h ^ block_hash((const uint8_t *)l.v[i], (uint32_t)strlen(l.v[i]))) *
            0x100000001b3ull;
    free_lines(&l);
    return h;
}

void names_make(struct storm *s, const struct storm_cfg *cfg)
{
    struct img blank = { .name = cfg->name, .size = VOL_BYTES };
    blank.bytes = calloc(1, VOL_BYTES);
    uint8_t *mem = malloc(1u << 20);
    if (!blank.bytes || !mem)
        fail("out of memory");
    cut_init(&s->cd, &blank);
    free(blank.bytes);
    struct ext_mkfs p = { .block_size = cfg->mkfs_bs, .journal_blocks = 1024, .uid = 1000,
                          .gid = 1000, .now = 1760000000, .label = "jnl",
                          .flags = EXT_MKFS_ZEROED };
    memset(p.uuid, 0x4a, sizeof(p.uuid));
    struct ext_dev dev = cut_dev(&s->cd);
    status_t st = ext_mkfs(&dev, &p, mem, 1u << 20);
    free(mem);
    if (st != OK)
        fail("mkfs: %d", st);
    cut_rebase(&s->cd);
    s->cd.block_size = cfg->mkfs_bs;
    s->o_root = s->o_block = s->o_many = s->o_swap = s->o_file = UINT32_MAX;
    s->o_listing = s->nobj;
    s->objs[s->nobj++] = (struct obj){ .kind = OBJ_LISTING };
    s->o_wtime = s->nobj;
    s->objs[s->nobj++] = (struct obj){ .kind = OBJ_WTIME };
    s->nreq = cfg->nreq;
}
