/* utest: listing a directory through fat (user/services/fat/dirs.c) over a
 * RAM disk. A listing of n entries, index 0, 1, 2, ..., costs fat about n
 * entry reads and a sector read per 16 entries (its fsctl.stats counts
 * both), not the n * n / 2 of a walk from the start for every index; and
 * the answers are still what such a walk gives while entries are made,
 * removed and renamed in between, more directories are listed at once
 * than fat keeps cursors for, or a listing starts again. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/fsctl.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define BIG_N   2000u   /* entries in the big directory */
#define SMALL_N 40u     /* entries in each small one */
#define DIRS    10u     /* small directories listed at once: more than fat's cursors */

static struct ramdisk disk;

struct counts {
    uint64_t entries;   /* directory entries fat read */
    uint64_t sectors;   /* sector reads fat made (from its cache or past it) */
};

static bool counts(const struct fatrun *r, struct counts *c)
{
    uint64_t entries = 0, hits = 0, fills = 0, bypassed = 0, updated = 0;
    CHECK_ST(fsctl_stats_until(r->ctl, now() + FAT_CALL_NS, &entries, &hits, &fills, &bypassed,
                               &updated), OK);
    c->entries = entries;
    c->sectors = hits + fills + bypassed;
    return true;
}

/* Make `dir` with `n` empty files named f0000.txt, f0001.txt, ... */
static bool make_dir(const struct fatrun *r, const char *dir, unsigned n)
{
    char path[64];
    CHECK_ST(t_mkdir(r, dir), OK);
    for (unsigned i = 0; i < n; i++) {
        struct tfile f;
        snprintf(path, sizeof(path), "%s/f%04u.txt", dir, i);
        CHECK_ST(t_open(r, path, FS_READ | FS_WRITE | FS_CREATE, &f), OK);
        t_close(&f);
    }
    return true;
}

/* The whole listing of `dir` into names[] (cap of them, FS_PATH_MAX
 * bytes each), from index 0 with every request a fresh walk's answer. */
static bool list_all(const struct fatrun *r, const char *dir, char (*names)[FS_PATH_MAX],
                     unsigned cap, unsigned *n)
{
    status_t st = OK;
    unsigned i = 0;
    while (i < cap && (st = t_readdir(r, dir, i, names[i], NULL)) == OK)
        i++;
    CHECK(i < cap);
    CHECK_ST(st, ERR_NOT_FOUND);
    *n = i;
    return true;
}

bool t_fat_dir_linear(void)
{
    static char names[BIG_N + 8][FS_PATH_MAX];
    struct fatrun r;
    struct counts before, after;
    unsigned n = 0;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false) ||
        !make_dir(&r, "/big", BIG_N) || !counts(&r, &before) ||
        !list_all(&r, "/big", names, BIG_N + 8, &n) || !counts(&r, &after))
        return false;
    CHECK_EQ(n, BIG_N);
    for (unsigned i = 0; i < BIG_N; i++) {
        char want[16];
        snprintf(want, sizeof(want), "f%04u.txt", i);
        if (strcmp(names[i], want))
            FAIL("entry %u is \"%s\", not %s", i, names[i], want);
    }
    uint64_t entries = after.entries - before.entries, sectors = after.sectors - before.sectors;
    printf("utest: fat_dir_linear: %u entries listed: %lu entry reads, %lu sector reads\n",
           BIG_N, (unsigned long)entries, (unsigned long)sectors);
    /* One read per entry, plus the end; a walk from the start for each
     * index reads BIG_N * BIG_N / 2 entries, and a sector per 16 of them
     * (BIG_N * BIG_N / 32 sectors). The sectors' bound leaves room for
     * the FAT's sectors at each cluster's end. */
    CHECK(entries <= BIG_N + 2);
    CHECK(sectors <= BIG_N / 4);
    return fat_stop(&r) && ramdisk_destroy(&disk);
}

/* Entry `index` of `dir` must be what a fresh walk finds there now. */
static bool entry_is_fresh(const struct fatrun *r, const char *dir, uint32_t index)
{
    static char now_names[SMALL_N + 8][FS_PATH_MAX];
    char got[FS_PATH_MAX];
    unsigned n = 0;
    status_t st = t_readdir(r, dir, index, got, NULL);
    /* Listing it all from 0 opens fat's cursor again from the start. */
    if (!list_all(r, dir, now_names, SMALL_N + 8, &n))
        return false;
    if (index >= n) {
        CHECK_ST(st, ERR_NOT_FOUND);
        return true;
    }
    CHECK_ST(st, OK);
    if (strcmp(got, now_names[index]))
        FAIL("%s entry %u: \"%s\" from the cursor, \"%s\" from the start", dir, index, got,
             now_names[index]);
    return true;
}

/* Part way through a listing: an entry removed before the cursor, one
 * made, one renamed; each next answer as from the start. */
static bool changes_mid_listing(const struct fatrun *r)
{
    char name[FS_PATH_MAX];
    struct tfile f;
    for (uint32_t i = 0; i < 10; i++)
        CHECK_ST(t_readdir(r, "/d0", i, name, NULL), OK);
    CHECK_ST(t_unlink(r, "/d0/f0003.txt"), OK);
    if (!entry_is_fresh(r, "/d0", 10))
        return false;
    for (uint32_t i = 11; i < 20; i++)
        CHECK_ST(t_readdir(r, "/d0", i, name, NULL), OK);
    CHECK_ST(t_open(r, "/d0/new.txt", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    t_close(&f);
    if (!entry_is_fresh(r, "/d0", 20) || !entry_is_fresh(r, "/d0", SMALL_N))
        return false;
    CHECK_ST(t_readdir(r, "/d0", 25, name, NULL), OK);
    CHECK_ST(t_rename(r, "/d0/f0001.txt", "/d0/a much longer name than before.txt"), OK);
    return entry_is_fresh(r, "/d0", 26) && entry_is_fresh(r, "/d0", 0);
}

/* DIRS directories listed a step each in turn (more than fat's cursors),
 * then a listing started again: every answer the one at that index. */
static bool interleaved(const struct fatrun *r)
{
    char name[FS_PATH_MAX], want[16], dir[16];
    for (uint32_t i = 0; i < SMALL_N; i++)
        for (unsigned d = 1; d < DIRS; d++) {
            snprintf(dir, sizeof(dir), "/d%u", d);
            snprintf(want, sizeof(want), "f%04u.txt", i);
            CHECK_ST(t_readdir(r, dir, i, name, NULL), OK);
            if (strcmp(name, want))
                FAIL("%s entry %u: \"%s\", not %s", dir, i, name, want);
        }
    CHECK_ST(t_readdir(r, "/d1", SMALL_N, name, NULL), ERR_NOT_FOUND);
    CHECK_ST(t_readdir(r, "/d1", 0, name, NULL), OK);
    CHECK(!strcmp(name, "f0000.txt"));
    CHECK_ST(t_readdir(r, "/d1", 7, name, NULL), OK);
    CHECK(!strcmp(name, "f0007.txt"));
    CHECK_ST(t_readdir(r, "/d1/f0002.txt", 0, name, NULL), ERR_WRONG_TYPE);
    CHECK_ST(t_readdir(r, "/nothing", 0, name, NULL), ERR_NOT_FOUND);
    return true;
}

/* A directory a cursor holds open can still be renamed and removed. */
static bool held_dirs_change(const struct fatrun *r)
{
    char name[FS_PATH_MAX];
    CHECK_ST(t_mkdir(r, "/empty"), OK);
    CHECK_ST(t_readdir(r, "/empty", 0, name, NULL), ERR_NOT_FOUND);
    CHECK_ST(t_readdir(r, "/d2", 3, name, NULL), OK);
    CHECK_ST(t_rename(r, "/d2", "/moved"), OK);
    CHECK_ST(t_readdir(r, "/moved", 4, name, NULL), OK);
    CHECK(!strcmp(name, "f0004.txt"));
    CHECK_ST(t_readdir(r, "/d2", 0, name, NULL), ERR_NOT_FOUND);
    CHECK_ST(t_unlink(r, "/empty"), OK);
    return true;
}

bool t_fat_dir_cursors(void)
{
    struct fatrun r;
    char dir[16];
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    for (unsigned d = 0; d < DIRS; d++) {
        snprintf(dir, sizeof(dir), "/d%u", d);
        if (!make_dir(&r, dir, SMALL_N))
            return false;
    }
    if (!changes_mid_listing(&r) || !interleaved(&r) || !held_dirs_change(&r))
        return false;
    return fat_stop(&r) && ramdisk_destroy(&disk);
}
