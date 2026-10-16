/* extdetect: the checker (ext_check, user/services/extfs/ext/check.c) finds
 * what is wrong. Each case makes a good volume, spoils it one way through
 * the library's own calls (so every checksum is right and only the meaning
 * is broken) and wants the checker to say so in words it can be matched by;
 * a volume with nothing spoilt reports nothing. The limit on problems and
 * the cancel are tried too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ext_check.h>
#include "extvol.h"
#include "extwrite.h"

#define MSGS 4096

struct run {
    char     text[MSGS];
    unsigned problems;
    unsigned cancel_after;           /* 0: never; else true from this many asks on */
    unsigned asks;
};

static void *run_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return calloc(1, n);
}

static void run_release(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static void run_report(void *ctx, const char *fmt, va_list ap)
{
    struct run *r = ctx;
    size_t used = strlen(r->text);
    r->problems++;
    if (used + 2 < MSGS) {
        vsnprintf(r->text + used, MSGS - used - 1, fmt, ap);
        strcat(r->text, "\n");
    }
}

static bool run_cancel(void *ctx)
{
    struct run *r = ctx;
    return r->cancel_after && ++r->asks >= r->cancel_after;
}

static status_t run_check(struct vol *v, struct run *r, struct ext_check_result *res)
{
    struct ext_check_io io = { .ctx = r, .alloc = run_alloc, .release = run_release,
                               .report = run_report, .cancel = r->cancel_after ? run_cancel : NULL };
    status_t st = ext_check(&v->fs, &io, res);
    check_pins(&v->fs);
    return st;
}

/* The checker finds a problem, and one of them has `needle` in it. */
static void found(struct vol *v, const char *what, const char *needle)
{
    struct run r = { .text = "" };
    struct ext_check_result res;
    vol_sync(v);
    expect(run_check(v, &r, &res), OK, what);
    if (!r.problems || res.problems != r.problems)
        fail("detect %s: %u problems reported (%u counted)", what, r.problems, res.problems);
    if (!strstr(r.text, needle))
        fail("detect %s: wanted \"%s\" in:\n%s", what, needle, r.text);
}

static void clean(struct vol *v, const char *what)
{
    struct run r = { .text = "" };
    struct ext_check_result res;
    expect(run_check(v, &r, &res), OK, what);
    if (r.problems)
        fail("detect %s: a good volume reports:\n%s", what, r.text);
}

/* A volume with /d/a (two blocks of data) and /d/b. */
static void good_vol(struct vol *v, struct ext_inode *a, struct ext_inode *b)
{
    small_vol(v, 4096, 16u << 20);
    expect(make(v, "d", EXT_S_IFDIR | 0755, NULL), OK, "mkdir d");
    expect(make(v, "d/a", EXT_S_IFREG | 0644, a), OK, "create a");
    expect(make(v, "d/b", EXT_S_IFREG | 0644, b), OK, "create b");
    uint8_t buf[8192];
    memset(buf, 0x41, sizeof(buf));
    size_t n = 0;
    expect(ext_write(&v->fs, a, 0, buf, sizeof(buf), &n), OK, "write a");
    expect(ext_write(&v->fs, b, 0, buf, 4096, &n), OK, "write b");
    vol_sync(v);
    clean(v, "good volume");
}

void check_detects(void)
{
    struct vol v;
    struct ext_inode a, b;

    /* a link count that is wrong */
    good_vol(&v, &a, &b);
    a.d.links_count = 5;
    expect(ext_iput(&v.fs, &a), OK, "iput");
    found(&v, "links", "5 links, 1 counted");
    vol_free(&v);

    /* a block in use but free in its bitmap */
    good_vol(&v, &a, &b);
    struct ext_run run;
    expect(ext_map(&v.fs, &a, 0, &run), OK, "map");
    expect(ext_bfree(&v.fs, run.pblk, 1), OK, "bfree");
    found(&v, "used but free", "is used but free in its bitmap");
    vol_free(&v);

    /* an inode in use with no name, not an orphan */
    good_vol(&v, &a, &b);
    struct ext_inode_spec spec = { .parent = EXT_ROOT_INO, .mode = EXT_S_IFREG | 0644, .links = 1,
                                   .uid = 1000, .gid = 1000, .generation = 77, .time = now() };
    struct ext_inode lost;
    expect(ext_inode_new(&v.fs, &spec, &lost), OK, "inode_new");
    found(&v, "nameless", "with no name and not an orphan");
    vol_free(&v);

    /* the superblock's free count is not the groups' */
    good_vol(&v, &a, &b);
    v.fs.sb.free_blocks_count_lo += 7;
    expect(ext_super_write(&v.fs), OK, "super_write");
    found(&v, "superblock count", "superblock:");
    vol_free(&v);

    /* two files owning one block */
    good_vol(&v, &a, &b);
    struct ext_run first;
    expect(ext_map(&v.fs, &a, 0, &first), OK, "map a");
    struct ext_run one = { .pblk = first.pblk, .len = 1, .kind = EXT_RUN_DATA };
    expect(ext_extent_insert(&v.fs, &b, 1, &one), OK, "insert");
    expect(ext_iput(&v.fs, &b), OK, "iput b");
    found(&v, "owned twice", "owned twice");
    vol_free(&v);

    /* a size that is not whole blocks in a directory */
    good_vol(&v, &a, &b);
    struct ext_inode d;
    iget(&v, ino_of(&v, "d"), &d);
    d.d.size_lo += 512;
    expect(ext_iput(&v.fs, &d), OK, "iput d");
    found(&v, "directory size", "is not whole blocks");
    vol_free(&v);

    /* many problems: the check stops at its limit and says so */
    small_vol(&v, 4096, 16u << 20);
    for (unsigned i = 0; i < 80; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%u", i);
        struct ext_inode f;
        expect(make(&v, name, EXT_S_IFREG | 0644, &f), OK, "create");
        f.d.links_count = 9;
        expect(ext_iput(&v.fs, &f), OK, "iput");
    }
    vol_sync(&v);
    struct run r = { .text = "" };
    struct ext_check_result res;
    expect(run_check(&v, &r, &res), OK, "many");
    if (res.problems != EXT_CHECK_MAX_PROBLEMS || !res.stopped)
        fail("detect many: %u problems, stopped %d", res.problems, res.stopped);

    /* a cancel is honoured */
    struct run c = { .text = "", .cancel_after = 2 };
    expect(run_check(&v, &c, &res), ERR_CANCELED, "cancel");
    vol_free(&v);

    printf("extcheck: the checker finds a wrong link count, a block used but free, a nameless "
           "inode, a wrong superblock count, a block owned twice, a bad directory size; stops "
           "at %d problems; cancels: ok\n", EXT_CHECK_MAX_PROBLEMS);
}
