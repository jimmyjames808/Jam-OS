/* jamjar: the self-test (`run jamjar --selftest`): names, UTF-8, the
 * library and the search on a fixture laid out like the owner's (UTF-8
 * names, '_' for spaces, years before albums, files at odd depths), the
 * jar labels, the jam, the roulette, the layout for screens QEMU doesn't
 * have, and whole frames drawn at each of them. It needs no handle and no
 * screen: it draws into memory of its own. */
#include "jamjar.h"

#define R "/m/OnTheSpot"

static const char *const fixture[] = {
    R "/JAŸ-Z/2017_4-44/Kill_Jay_Z.mp3",
    R "/JAŸ-Z/2017_4-44/The_Story_of_O.J..mp3",
    R "/JAŸ-Z/1996 Reasonable_Doubt/Can't_Knock_the_Hustle.mp3",
    R "/¥$/2024_Vultures_1/Carnival.mp3",
    R "/¥$/2024_Vultures_1/Fünf ~ Ÿ.MP3",
    R "/Kanye_West/2010 My Beautiful Dark Twisted Fantasy/01 - Dark_Fantasy.mp3",
    R "/Kanye_West/2010 My Beautiful Dark Twisted Fantasy/~9. Runaway.mp3",
    R "/Kanye_West/loose.wav",
    R "/top.wav",
    R "/a/b/c/Deep.mp3",
    R "/kanye_west/Lower/x.mp3",
};
#define NFIX (sizeof(fixture) / sizeof(fixture[0]))

static struct library lib;
static struct view view;

static bool eq(const char *a, const char *b)
{
    return !strcmp(a, b);
}

static void test_names(void)
{
    char a[NAME_MAX], y[5];
    name_tidy("  Kill__Jay_Z ", 14, a, sizeof(a));
    fun_check(eq(a, "Kill Jay Z"), "names: '_' as spaces, runs of spaces as one, ends trimmed");
    name_track("01 - Dark_Fantasy.mp3", 21, a, sizeof(a));
    bool ok = eq(a, "Dark Fantasy");
    name_track("99_Problems.mp3", 15, a, sizeof(a));
    ok &= eq(a, "99 Problems");
    name_track("The_Story_of_O.J..mp3", 21, a, sizeof(a));
    ok &= eq(a, "The Story of O.J.");
    fun_check(ok, "  ... a title: no ending, no track number ('99 Problems' keeps its 99)");
    name_album("2017_4-44", 9, a, sizeof(a), y);
    ok = eq(a, "4-44") && eq(y, "2017");
    name_album("1996 Reasonable_Doubt", 21, a, sizeof(a), y);
    ok &= eq(a, "Reasonable Doubt") && eq(y, "1996");
    name_album("4-44", 4, a, sizeof(a), y);
    ok &= eq(a, "4-44") && !y[0];
    fun_check(ok, "  ... an album: its leading year apart, '2017_4-44' and '1996 Name'");
    char k[KEY_MAX];
    name_fold("JAŸ-Z", k, sizeof(k));
    ok = eq(k, "jay z");
    name_fold("Fünf ~ Ÿ", k, sizeof(k));
    ok &= eq(k, "funf y");
    name_fold("¥$", k, sizeof(k));
    ok &= eq(k, "y$");
    name_fold("Can't_Knock", k, sizeof(k));
    ok &= eq(k, "can t knock") && name_has(k, "knock") && name_has(k, "") && !name_has(k, "jay");
    fun_check(ok, "  ... search keys: 'JAY-Z' with a diaeresis is 'jay z', '\302\245$' is 'y$'");
    struct track_names n;
    names_of_path(R "/JAŸ-Z/2017_4-44/Kill_Jay_Z.mp3", &n);
    fun_check(eq(n.artist, "JAŸ-Z") && eq(n.album, "4-44") && eq(n.year, "2017") &&
                  eq(n.title, "Kill Jay Z"),
              "  ... from a path: artist, album, year and title");
    fun_check(album_hash("/x/A/B", 6) == album_hash("/y/z/A/B", 8) &&
                  album_hash("/x/A/B", 6) != album_hash("/x/A/C", 6),
              "  ... an album's art seed is its last two folders");
}

static void test_utf8(void)
{
    const char *s = "JAŸ-Z";
    uint32_t cps[8];
    int n = 0;
    while (*s && n < 8)
        cps[n++] = utf8_next(&s);
    bool ok = n == 5 && cps[2] == 0x178;
    const char *bad = "a\xff\xc3(\xe2\x82";
    n = 0;
    while (*bad && n < 8)
        cps[n++] = utf8_next(&bad);
    ok &= n == 6 && cps[1] == UTF8_BAD && cps[2] == UTF8_BAD && cps[3] == '(' &&
          cps[4] == UTF8_BAD && cps[5] == UTF8_BAD;
    fun_check(ok, "utf-8: code points; a malformed byte is one bad character each");
    fun_check(text_width(1, "Ÿ") == text_width(1, "Y") && text_width(1, "\xe4\xb8\xad") > 0 &&
                  text_width(1, "\xe4\xb8\xad") < 2 * text_width(1, "?"),
              "  ... drawn one glyph a character: 'Y' with a diaeresis, one box for CJK");
}

static void test_library(void)
{
    status_t st = lib_build(&lib, R "/", fixture, NFIX);
    fun_check(st == OK && lib.ready && lib.ntracks == NFIX && lib.nartists == 6 &&
                  lib.nalbums == 8,
              "library: 11 files, 6 artists, 8 albums");
    bool ok = st == OK;
    const char *want[] = { "(no artist)", "b", "JAŸ-Z", "Kanye West", "kanye west", "¥$" };
    for (uint32_t i = 0; ok && i < 6; i++)
        ok &= eq(lib.artist[i].name, want[i]);
    fun_check(ok, "  ... artists by folder name, case apart; a root file has none");
    if (st != OK)
        return;
    const struct lib_artist *jay = &lib.artist[2];
    const struct lib_album *b0 = &lib.album[jay->first], *b1 = &lib.album[jay->first + 1];
    fun_check(jay->n == 2 && eq(b0->name, "Reasonable Doubt") && eq(b0->year, "1996") &&
                  eq(b1->name, "4-44") && b1->n == 2 && jay->tracks == 3 &&
                  eq(b1->dir, R "/JAŸ-Z/2017_4-44"),
              "  ... albums by folder: 1996 before 2017; an album's folder is what plays");
    const struct lib_artist *kw = &lib.artist[3];
    fun_check(kw->n == 2 && eq(lib.album[kw->first].name, "(loose tracks)") &&
                  eq(lib.album[kw->first].dir, R "/Kanye_West") &&
                  eq(lib.track[lib.album[kw->first + 1].first].name, "Dark Fantasy"),
              "  ... a file in an artist's folder is in its loose tracks");
    int64_t t = lib_find(&lib, R "/¥$/2024_Vultures_1/Fünf ~ Ÿ.MP3");
    fun_check(t >= 0 && eq(lib.track[t].name, "Fünf ~ Ÿ") && lib_find(&lib, R "/nope") < 0,
              "  ... find a track by its path");
}

static void test_view(void)
{
    if (!view_init(&view, &lib)) {
        fun_check(false, "view: out of memory");
        return;
    }
    fun_check(view.nrows[COL_ARTIST] == 7 && view.row[COL_ARTIST][0] == ROW_ALL &&
                  view.nrows[COL_ALBUM] == 8 && view.nrows[COL_TRACK] >= 1,
              "view: All and the 6 artists; All shows every album");
    view_query(&view, &lib, "jay");
    bool ok = view.nrows[COL_ARTIST] == 2 && view.row[COL_ARTIST][1] == 2;
    view_select(&view, &lib, COL_ARTIST, 1);
    ok &= view.nrows[COL_ALBUM] == 2 && view.nrows[COL_TRACK] >= 1;
    fun_check(ok, "  ... 'jay' finds JAY-Z (folded) and all of its albums");
    view_query(&view, &lib, "CARNIVAL");
    view_select(&view, &lib, COL_ARTIST, 0);
    ok = view.nrows[COL_ARTIST] == 2 && view.nrows[COL_ALBUM] == 1 &&
         view.nrows[COL_TRACK] == 1 && eq(lib.track[view.row[COL_TRACK][0]].name, "Carnival");
    fun_check(ok, "  ... 'CARNIVAL' keeps its artist, its album and that track only");
    view_query(&view, &lib, "zzz");
    fun_check(view.nrows[COL_ARTIST] == 1 && view.nrows[COL_ALBUM] == 0 &&
                  view.nrows[COL_TRACK] == 0 && view_item(&view, COL_TRACK) == -1,
              "  ... nothing matches: All alone, empty columns");
    view_query(&view, &lib, "");
    int64_t t = lib_find(&lib, R "/JAŸ-Z/2017_4-44/The_Story_of_O.J..mp3");
    ok = t >= 0 && view_locate(&view, &lib, (uint32_t)t) && view_item(&view, COL_TRACK) == t &&
         view_item(&view, COL_ARTIST) == 2;
    view_scroll(&view, 1);
    ok &= view.top[COL_TRACK] == view.sel[COL_TRACK];
    fun_check(ok, "  ... locate the track playing; the selection scrolls into view");
}

static void test_art(void)
{
    char a[64], b[64];
    uint32_t c0, c1, d0, d1;
    art_flavour(lib.album[0].hash, &c0, &c1, a, sizeof(a));
    art_flavour(lib.album[0].hash, &d0, &d1, b, sizeof(b));
    bool ok = eq(a, b) && c0 == d0 && c1 == d1 && c0 != c1 && strstr(a, " & ");
    unsigned differ = 0;
    for (uint32_t i = 1; i < lib.nalbums; i++) {
        art_flavour(lib.album[i].hash, &d0, &d1, b, sizeof(b));
        differ += !eq(a, b);
    }
    fun_check(ok && differ >= lib.nalbums - 2, "art: a flavour per album, stable, two fruits");
    static uint32_t px[2][96 * 96];
    struct surf s0 = { px[0], 96, 96, 96 }, s1 = { px[1], 96, 96, 96 };
    art_draw(&s0, 0, 0, 96, lib.album[0].hash, C_PANEL);
    art_draw(&s1, 0, 0, 96, lib.album[1].hash, C_PANEL);
    unsigned diff = 0;
    for (int i = 0; i < 96 * 96; i++)
        diff += px[0][i] != px[1][i];
    art_draw(&s1, 0, 0, 96, lib.album[0].hash, C_PANEL);   /* from the cache now */
    fun_check(diff > 96 * 96 / 8 && !memcmp(px[0], px[1], sizeof(px[0])),
              "  ... two albums' labels differ; the same one again is the same picture");
}

static void test_simmer(void)
{
    static struct simmer s;
    uint8_t loud[16], quiet[16] = { 0 };
    for (int i = 0; i < 16; i++)
        loud[i] = 230;
    simmer_init(&s, 7);
    for (int i = 0; i < 60; i++)
        simmer_step(&s, quiet, 0, 1.0f / 30);
    float low = s.rest[SIM_COLS / 2];
    int bubbles = 0;
    for (int i = 0; i < 60; i++) {
        uint8_t *b = (i / 6) % 2 ? loud : quiet;   /* beats */
        simmer_step(&s, b, (uint8_t)(b[0] / 2), 1.0f / 30);
        for (int k = 0; k < SIM_BUBBLES; k++)
            bubbles += s.bub[k].live;
    }
    fun_check(s.rest[SIM_COLS / 2] > low + 0.2f && bubbles > 0,
              "simmer: the music lifts the jam and its beats make bubbles");
    simmer_splash(&s, 0.5f, 1.0f);
    bool ok = simmer_busy(&s);
    for (int i = 0; i < 600; i++)
        simmer_step(&s, quiet, 0, 1.0f / 30);
    fun_check(ok && !simmer_busy(&s), "  ... a splash moves it; quiet, it comes to rest");
}

static void test_roulette(void)
{
    static struct roulette r;
    uint64_t t0 = 1000 * NS_PER_S;
    bool ok = roulette_start(&r, &lib, 5, t0);
    float last = -1;
    int64_t got = -1;
    for (uint64_t t = t0; ok && got < 0 && t < t0 + 10 * NS_PER_S; t += 20 * NS_PER_MS) {
        got = roulette_step(&r, t);
        ok &= r.pos >= last;
        last = r.pos;
    }
    fun_check(ok && got >= 0 && got < lib.nalbums && !r.on &&
                  (uint32_t)got == r.tile[r.target],
              "roulette: slows to the target label and picks its album");
}

static bool apart(const struct rect *p, const struct rect *q)
{
    return p->x + p->w <= q->x || q->x + q->w <= p->x || p->y + p->h <= q->y ||
           q->y + q->h <= p->y;
}

static bool inside(const struct rect *in, const struct rect *out)
{
    return in->x >= out->x && in->y >= out->y && in->x + in->w <= out->x + out->w &&
           in->y + in->h <= out->y + out->h && in->w > 0 && in->h > 0;
}

static struct app app;
static uint32_t *frame_px;   /* the back buffer for the frames: the biggest screen tested */

/* A whole frame on a w x h screen, as the app draws it, playing a fixture
 * track with loud bands: the jam at the bottom is jam-coloured, the mark
 * at the top left is drawn, and how long it took. */
static void test_frame(int w, int h)
{
    if (!frame_px && !(frame_px = big_alloc(2560ull * 1440 * 4))) {
        fun_check(false, "frame: no memory to draw into");
        return;
    }
    scr.s = (struct surf){ frame_px, w, h, w };
    scr.w = w;
    scr.h = h;
    scr.ui = h > 1100 ? 2 : 1;
    struct app *a = &app;
    a->lib = lib;
    a->view = view;
    a->view_ok = true;
    layout_make(&a->lo, w, h, scr.ui);
    view_scroll(&a->view, a->lo.rows);
    a->snap = (struct snap){ .link = true, .answered = true, .playing = 1, .serial = 1,
                             .elapsed_ms = 83000, .length_ms = 245000, .volume = -120 };
    snprintf(a->snap.path, sizeof(a->snap.path), "%s", fixture[1]);
    names_of_path(a->snap.path, &a->now);
    a->now_track = lib_find(&lib, a->snap.path);
    a->mx = a->my = -1;
    simmer_init(&a->sim, 3);
    uint8_t bands[16];
    for (int i = 0; i < 16; i++)
        bands[i] = (uint8_t)(220 - 9 * i);
    for (int i = 0; i < 40; i++)
        simmer_step(&a->sim, bands, 200, 1.0f / 30);
    uint64_t t0 = now();
    draw_frame(a, t0);
    uint64_t us = (now() - t0) / 1000;
    uint32_t bottom = frame_px[(uint64_t)(h - 2) * w + w / 2];
    const struct rect *m = &a->lo.mark;
    uint32_t centre = frame_px[(uint64_t)(m->y + m->h / 2) * w + m->x + m->w / 2];
    char what[96];
    snprintf(what, sizeof(what), "  ... a whole frame at %dx%d in %lu us", w, h,
             (unsigned long)us);
    fun_check(bottom != C_BG && bottom != C_BG2 && centre == C_BERRY0, what);
}

/* One screen: the layout, then a whole frame drawn on it. */
static void test_screen(int w, int h)
{
    static struct layout l;
    int ui = h > 1100 ? 2 : 1;
    layout_make(&l, w, h, ui);
    struct rect screen = { 0, 0, w, h };
    const struct rect *big[] = { &l.top, &l.lib, &l.now, &l.jam };
    bool ok = l.rows >= 5;
    for (int i = 0; i < 4; i++) {
        ok &= inside(big[i], &screen);
        for (int j = 0; j < i; j++)
            ok &= apart(big[i], big[j]);
    }
    const struct rect *in_now[] = { &l.art, &l.title, &l.progress, &l.btn[0], &l.btn[1],
                                    &l.btn[2], &l.btn[3], &l.vol, &l.mode };
    for (unsigned i = 0; i < sizeof(in_now) / sizeof(in_now[0]); i++)
        ok &= inside(in_now[i], &l.now);
    for (int c = 0; c < NCOLS; c++)
        ok &= inside(&l.list[c], &l.lib) && (c == 0 || apart(&l.list[c], &l.list[c - 1]));
    ok &= apart(&l.vol, &l.mode) && inside(&l.search, &l.top);
    ok &= !l.info.h || (inside(&l.info, &l.now) && apart(&l.info, &l.mode));
    char what[96];
    snprintf(what, sizeof(what), "layout %dx%d: fits, %d rows a column", w, h, l.rows);
    fun_check(ok, what);
    if ((uint64_t)w * h <= 2560ull * 1440)
        test_frame(w, h);
}

int jamjar_selftest(void)
{
    fun_selftest_begin("jamjar", 74);
    test_names();
    test_utf8();
    test_library();
    test_view();
    test_art();
    test_simmer();
    test_roulette();
    test_screen(1280, 720);
    test_screen(1280, 800);
    test_screen(1024, 600);
    test_screen(1920, 1080);
    test_screen(2560, 1440);
    test_screen(3840, 2160);
    return fun_selftest_end();
}
