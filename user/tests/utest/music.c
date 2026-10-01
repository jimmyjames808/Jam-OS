/* utest: the music player's folder walk (user/services/music/tracks.c,
 * compiled in here), on a tree in bin/ramfs. The walk is read a few
 * entries at a time so that the player answers its channel while a big
 * folder is read (docs/history/AUDIO-REVIEW.md, item 2): read one entry
 * per step it must take many steps and find exactly what one big step
 * finds, in the same order: the .mp3 and .wav files (any case), not
 * names starting with '.', nothing more than MAX_DEPTH folders down. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#include "../../services/music/tracks.c"

#define M "/m"

static bool tree(void)
{
    static const char *const dirs[] = {
        M "/lib", M "/lib/A", M "/lib/A/Album", M "/lib/.hidden", M "/lib/Empty",
        M "/lib/Empty/Nested",
    };
    static const char *const files[] = {
        M "/lib/A/Album/1. One.mp3", M "/lib/A/Album/2. Two.WAV", M "/lib/A/Album/notes.txt",
        M "/lib/.hidden/x.mp3", M "/lib/Loose.Mp3", M "/lib/._Loose.Mp3", M "/lib/a.mp",
    };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        CHECK_ST(fs_mkdir(dirs[i]), OK);
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        CHECK_ST(ns_put(files[i], "x"), OK);
    /* MAX_DEPTH (16) folders below lib: a file in the 16th is found, one
     * in a 17th is not. */
    char p[FS_PATH_MAX] = M "/lib";
    size_t n = strlen(p);
    for (unsigned d = 1; d <= MAX_DEPTH + 1; d++) {
        memcpy(p + n, "/d", 3);
        n += 2;
        CHECK_ST(fs_mkdir(p), OK);
        if (d >= MAX_DEPTH) {
            char f[FS_PATH_MAX];
            snprintf(f, sizeof(f), "%s/deep%u.wav", p, d);
            CHECK_ST(ns_put(f, "x"), OK);
        }
    }
    return true;
}

/* folder read `entries` at a time: the paths into *out (owned), and the
 * steps it took. */
static bool scan(const char *folder, unsigned entries, struct tracks *out, unsigned *steps)
{
    *out = (struct tracks){ .count = 0 };
    *steps = 0;
    CHECK_ST(tracks_scan_begin(out, folder), OK);
    status_t st;
    while ((st = tracks_scan_step(out, entries)) == ERR_SHOULD_WAIT)
        ++*steps;
    ++*steps;
    CHECK_ST(st, OK);
    CHECK(!out->scan);
    return true;
}

static bool has(const struct tracks *t, const char *path)
{
    for (uint32_t i = 0; i < t->count; i++)
        if (!strcmp(t->path[i], path))
            return true;
    return false;
}

bool t_music_scan(void)
{
    struct ram r;
    bool dir = false;
    if (fs_stat("/boot", NULL, &dir, NULL) != OK || !dir) {
        printf("utest: %s: no /boot in our namespace: skipped\n", utest_cur);
        return true;
    }
    if (!ram_start(M, &r))
        return false;
    struct tracks one = { .count = 0 }, all = { .count = 0 };
    unsigned s1 = 0, s2 = 0;
    bool ok = tree() && scan(M "/lib/", 1, &one, &s1) && scan(M "/lib", 100000, &all, &s2);
    if (ok) {
        CHECK_EQ(one.count, 4);
        CHECK_EQ(all.count, 4);
        CHECK(s1 > 20);   /* one entry a step: lib's 9, A, Album's 4, the 17 deep ones, ... */
        CHECK_EQ(s2, 1);
        for (uint32_t i = 0; i < 4; i++)
            CHECK(!strcmp(one.path[i], all.path[i]));   /* the same order */
        CHECK(has(&all, M "/lib/A/Album/1. One.mp3"));
        CHECK(has(&all, M "/lib/A/Album/2. Two.WAV"));
        CHECK(has(&all, M "/lib/Loose.Mp3"));
        CHECK(has(&all, M "/lib/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/d/deep16.wav"));
        CHECK(all.bad && all.order && all.order[3] == 3);
    }
    tracks_free(&one);
    tracks_free(&all);
    /* Not a folder; not there; a folder of nothing (found 0, OK). */
    struct tracks t = { .count = 0 };
    unsigned s = 0;
    if (ok) {
        CHECK_ST(tracks_scan_begin(&t, M "/lib/Loose.Mp3"), ERR_WRONG_TYPE);
        CHECK_ST(tracks_scan_begin(&t, M "/lib/nothere"), ERR_NOT_FOUND);
        CHECK(scan(M "/lib/Empty", 1, &t, &s));
        CHECK_EQ(t.count, 0);
        tracks_free(&t);
        /* Freed half way (`music stop` while it reads): nothing kept. */
        CHECK_ST(tracks_scan_begin(&t, M "/lib"), OK);
        CHECK_ST(tracks_scan_step(&t, 1), ERR_SHOULD_WAIT);
        tracks_free(&t);
        CHECK(!t.scan && !t.path);
    }
    return ram_stop(M, &r) && ok;
}
