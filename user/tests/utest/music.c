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

#include "../../services/music/spectrum.c"
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

/* ---- the play order: `play`'s order 1 and its first file ----------------------------- */

/* The next n tracks handed out, as letters (the paths' last character). */
static void deal(struct tracks *t, unsigned n, char *out)
{
    bool pass = false;
    for (unsigned i = 0; i < n; i++) {
        int64_t k = tracks_next(t, &pass);
        out[i] = k < 0 ? '-' : t->path[k][strlen(t->path[k]) - 1];
    }
    out[n] = '\0';
}

bool t_music_order(void)
{
    static char *paths[] = { "/m/x/b", "/m/x/d", "/m/a", "/m/x/c" };
    uint32_t order[4];
    uint8_t bad[4] = { 0 };
    struct tracks t = { .path = paths, .bad = bad, .order = order, .count = 4, .last = -1 };
    char got[16];
    tracks_sort(&t);
    deal(&t, 6, got);
    CHECK(!strcmp(got, "abcdab"));   /* by path, over and over */
    CHECK_EQ(tracks_find(&t, "/m/x/c"), 3);
    CHECK_EQ(tracks_find(&t, "/m/x"), -1);
    tracks_sort(&t);
    tracks_first(&t, 1);             /* "/m/x/d" first: on from it */
    deal(&t, 5, got);
    CHECK(!strcmp(got, "dabcd"));
    for (uint64_t seed = 1; seed < 40; seed++) {
        tracks_shuffle(&t, seed);
        tracks_first(&t, 3);         /* "/m/x/c" first, the rest shuffled */
        deal(&t, 4, got);
        CHECK_EQ(got[0], 'c');
        CHECK(strchr(got, 'a') && strchr(got, 'b') && strchr(got, 'd'));
    }
    bad[0] = 1;                      /* a bad one is passed over in order too */
    t.nbad = 1;
    tracks_sort(&t);
    deal(&t, 4, got);
    CHECK(!strcmp(got, "acda"));
    return true;
}

/* ---- the bands (`levels`) ----------------------------------------------------------- */

/* sin x by its series, after x is brought into [-pi, pi]. */
static double sine(double x)
{
    const double pi = 3.14159265358979323846;
    x -= (double)(int64_t)(x / (2 * pi)) * 2 * pi;
    x = x > pi ? x - 2 * pi : x;
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 12; n++) {
        term *= -x2 / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

/* A second of a sine of hz_l at amp_l of full scale on the left and one of
 * hz_r at amp_r on the right (0: silence), rate Hz, `channels` 2 (or 1:
 * the left alone, as a mono file), fed as the player does: 1024-frame
 * chunks, the stream frame of each chunk's start counted at 48 kHz from 0. */
static void feed_lr(struct spectrum *s, uint32_t rate, double hz_l, double amp_l, double hz_r,
                    double amp_r, unsigned channels)
{
    static int16_t pcm[2 * 1024];
    const double tau = 2 * 3.14159265358979323846;
    int64_t done = 0;
    for (uint32_t chunk = 0; chunk < rate / 1024; chunk++) {
        for (uint32_t k = 0; k < 1024; k++) {
            double t = (double)(done + k) / rate;
            int16_t l = (int16_t)(sine(tau * hz_l * t) * amp_l * 32767.0);
            int16_t r = (int16_t)(sine(tau * hz_r * t) * amp_r * 32767.0);
            if (channels == 1) {
                pcm[k] = l;
                continue;
            }
            pcm[2 * k] = l;
            pcm[2 * k + 1] = r;
        }
        spec_feed(s, pcm, 1024, channels, rate, done * 48000 / (int64_t)rate);
        done += 1024;
    }
}

/* The same sine on both channels. */
static void feed_tone(struct spectrum *s, uint32_t rate, double hz, double amp)
{
    feed_lr(s, rate, hz, amp, hz, amp, 2);
}

static int loudest_of(const uint8_t *band)
{
    int best = 0;
    for (int b = 1; b < (int)SPEC_BANDS; b++)
        best = band[b] > band[best] ? b : best;
    return best;
}

static int loudest(const struct spec_entry *e)
{
    return loudest_of(e->band);
}

/* Each tone in its band (64 of a sixth of an octave from 40 Hz): 300 Hz
 * in band 21 (285-313 Hz, narrower than a bin: read between bins), 1 kHz
 * in 34, 5 kHz in 51; bands far from it much lower; the level of a
 * -12 dBFS sine. */
static bool tone_lands(struct spectrum *s, uint32_t rate, double hz, int band)
{
    spec_reset(s);
    feed_tone(s, rate, hz, 0.25);
    struct spec_entry e;
    CHECK(spec_at(s, 24000, &e));   /* half a second in */
    /* Below about 300 Hz a band is narrower than an FFT bin (21.5 Hz at
     * 44.1 kHz): read between two bins, a tone may show in the band next
     * to its own. Above, exactly its own. */
    int got = loudest(&e), off = got > band ? got - band : band - got;
    CHECK(off <= (band < 22 ? 1 : 0));
    band = got;
    CHECK(e.band[band] > 150);
    for (int b = 0; b < (int)SPEC_BANDS; b++)
        if (b < band - 6 || b > band + 6)
            CHECK(e.band[b] + 60 < e.band[band]);
    CHECK(e.level > 180 && e.level < 200);   /* -15 dBFS RMS: 191 */
    return true;
}

bool t_music_spectrum(void)
{
    struct spectrum *s = malloc(sizeof(*s));
    CHECK(s);
    spec_init(s);
    bool ok = tone_lands(s, 44100, 300, 21) && tone_lands(s, 48000, 1000, 34) &&
              tone_lands(s, 22050, 5000, 51) && tone_lands(s, 96000, 1000, 34) &&
              tone_lands(s, 44100, 60, 4);
    struct spec_entry e;
    if (ok) {
        /* Silence: zeros; nothing yet (or nothing near): no entry. */
        spec_reset(s);
        CHECK(!spec_at(s, 24000, &e));
        feed_tone(s, 48000, 1000, 0.0);
        CHECK(spec_at(s, 24000, &e));
        for (unsigned b = 0; b < SPEC_BANDS; b++)
            CHECK_EQ(e.band[b], 0);
        CHECK_EQ(e.level, 0);
        CHECK(!spec_at(s, 48000 * 5, &e));   /* 4 s past the last: stale */
        CHECK(!spec_at(s, -1, &e));           /* before the first */
        /* A skip at 0.5 s: what was written after is gone, the rest stays. */
        spec_cut(s, 24000);
        CHECK(spec_at(s, 23000, &e));
        CHECK(!spec_at(s, 48000 * 3 / 4, &e));
    }
    free(s);
    return ok;
}

/* The channels apart (`stereo`): 1 kHz on the left only lands in the left
 * bands' 34 and leaves the right at zero; 4 kHz on the right only, in the
 * right's 49; both at once, each in its own; a mono file gives both
 * channels the same bands, and the mono mix of identical channels is
 * either channel. */
bool t_music_stereo(void)
{
    struct spectrum *s = malloc(sizeof(*s));
    CHECK(s);
    spec_init(s);
    struct spec_entry e;
    bool ok = true;
    spec_reset(s);
    feed_lr(s, 44100, 1000, 0.25, 4000, 0.0, 2);
    CHECK(spec_at(s, 24000, &e));
    ok &= loudest_of(e.left) == 34 && e.left[34] > 150;
    for (unsigned b = 0; b < SPEC_BANDS; b++)
        ok &= e.right[b] == 0;
    spec_reset(s);
    feed_lr(s, 48000, 1000, 0.0, 4000, 0.25, 2);
    CHECK(spec_at(s, 24000, &e));
    ok &= loudest_of(e.right) == 49 && e.right[49] > 150;
    for (unsigned b = 0; b < SPEC_BANDS; b++)
        ok &= e.left[b] == 0;
    CHECK(ok);
    spec_reset(s);
    feed_lr(s, 44100, 1000, 0.25, 4000, 0.25, 2);
    CHECK(spec_at(s, 24000, &e));
    CHECK_EQ(loudest_of(e.left), 34);
    CHECK_EQ(loudest_of(e.right), 49);
    CHECK(e.left[49] + 60 < e.right[49] && e.right[34] + 60 < e.left[34]);
    spec_reset(s);
    feed_lr(s, 44100, 300, 0.25, 0, 0.0, 1);
    CHECK(spec_at(s, 24000, &e));
    for (unsigned b = 0; b < SPEC_BANDS; b++) {
        CHECK_EQ(e.left[b], e.right[b]);
        CHECK(e.band[b] >= e.left[b] - 1 && e.band[b] <= e.left[b] + 1);
    }
    CHECK(e.left[loudest_of(e.left)] > 150);
    free(s);
    return true;
}
