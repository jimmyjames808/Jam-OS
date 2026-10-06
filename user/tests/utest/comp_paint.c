/* utest: the compositor's painting (user/services/compositor paint.c,
 * cursor.c, wallpaper.c), through bin/compositor's test scene run
 * headless and the reference painter (comppaint.h, comp_ref.c). The
 * windows here have no decorations; comp_look.c has those.
 *
 * t_comp_paint_overlap: opaque and translucent windows overlapping each
 * other, the wallpaper and the output's edges: every pixel exact (the
 * blend's rounding included).
 * t_comp_paint_cull: windows hidden by opaque ones (xrgb, and argb with an
 * opaque region, whose pixels are then copied, alpha ignored) are skipped:
 * the pixels drawn from windows are exactly the visible layers', and the
 * image is still exact.
 * t_comp_paint_damage: only the damage is painted, once per pixel however
 * the boxes overlap: a window moved paints its old and new place, the
 * cursor appearing paints its box, nothing to paint paints nothing.
 * t_comp_paint_fullscreen: a full-screen opaque window is copied straight
 * from its buffer (every tile direct), except under the cursor; a window
 * over it turns that off.
 * t_comp_paint_cursor: a client's cursor surface at its hot spot, blended;
 * when it shrinks (a commit of a smaller buffer) its old box is painted
 * again, so nothing of it is left behind; a move paints both boxes.
 * t_comp_paint_blank: blank shows the splash's background only, no window,
 * no cursor.
 * t_comp_paint_overlay: a boot overlay (a full-screen window whose client
 * takes no keys, the splash's) on the desktop covers all of the output, the
 * strip included, copied straight from its buffer; when it goes it fades
 * (a frame between shows both) to exactly the picture without it, each
 * frame painting its box only. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <os.h>
#include "comppaint.h"
#include "utest.h"

static const struct comp_box no_skip = { 0, 0, 0, 0 };

/* ---- the tests ----------------------------------------------------------------------------- */

bool t_comp_paint_overlap(void)
{
    static const struct cp_win v[] = {
        { 20, 30, 160, 100, 0xff3060a0, false, false, 0, false },    /* opaque */
        { 100, 70, 150, 90, 0xff70a030, false, false, 0, false },    /* opaque, over it */
        { 140, 20, 120, 120, 0x80602040, false, false, 0, false },   /* half alpha over all */
        { -30, 150, 100, 80, 0x40404040, false, false, 0, false },   /* off left and bottom */
        { 280, -20, 80, 60, 0xff808080, true, false, 0, false },     /* solid, off top, right */
        { 60, 90, 40, 40, 0x01010101, false, false, 0, false },      /* nearly transparent */
    };
    static const char *const paint[] = { "paint" };
    struct cp_run r;
    bool ok = cp_run_windows(v, 6, paint, 1, no_skip, &r);
    if (ok) {
        CHECK_EQ(r.nrep, 1);
        CHECK_EQ(r.rep[0].px, CP_W * CP_H);   /* the first paint: everything, once */
        CHECK_EQ(r.rep[0].direct, 0);
    }
    cp_done(&r);
    return ok;
}

/* Pixels drawn from windows, row by row: from the topmost window hiding
 * the whole row (full-width opaque ones, in these scenes) up. */
static uint64_t visible_layers(const struct cp_win *v, unsigned n)
{
    uint64_t sum = 0;
    for (int32_t y = 0; y < CP_H; y++) {
        unsigned from = 0;
        for (unsigned k = 0; k < n; k++)
            if (v[k].x <= 0 && v[k].x + v[k].w >= CP_W && y >= v[k].y && y < v[k].y + v[k].h &&
                ((v[k].argb >> 24) == 0xff || v[k].opaque_region))
                from = k;
        for (unsigned k = from; k < n; k++) {
            if (y < v[k].y || y >= v[k].y + v[k].h)
                continue;
            int32_t x1 = v[k].x < 0 ? 0 : v[k].x, x2 = v[k].x + v[k].w;
            sum += (uint64_t)((x2 < CP_W ? x2 : CP_W) - x1);
        }
    }
    return sum;
}

bool t_comp_paint_cull(void)
{
    /* Edges on multiples of 16 rows, so hiding a whole row hides whole tiles. */
    static const struct cp_win v[] = {
        { 0, 0, CP_W, CP_H, 0xff102030, false, false, 0, false },   /* everything, opaque */
        { 40, 16, 100, 64, 0xff405060, false, false, 0, false },    /* opaque, then hidden */
        { 0, 48, CP_W, 64, 0xff506070, false, false, 0, false },    /* hides rows 48..112 */
        { 0, 128, CP_W, 48, 0xc0a0b0c0, false, true, 0, false },    /* opaque region: 128..176 */
        { 100, 8, 50, 140, 0x80ff4000, false, false, 0, false },    /* translucent over all */
    };
    static const char *const paint[] = { "paint" };
    struct cp_run r;
    bool ok = cp_run_windows(v, 5, paint, 1, no_skip, &r);
    if (ok) {
        uint64_t all = 0, want = visible_layers(v, 5);
        for (unsigned k = 0; k < 5; k++)
            all += (uint64_t)v[k].w * (uint64_t)v[k].h;
        CHECK_EQ(r.nrep, 1);
        CHECK(want < all);
        CHECK_EQ(r.rep[0].layer_px, want);
    }
    cp_done(&r);
    return ok;
}

bool t_comp_paint_damage(void)
{
    static const struct cp_win v[] = {
        { 0, 0, CP_W, CP_H, 0xff203040, false, false, 0, false },
        { 50, 50, 100, 60, 0xff506070, false, false, 0, false },
    };
    static const char *const more[] = {
        "paint",
        "damage=10,10,40,30", "damage=30,20,40,30", "damage=12,12,5,5", "paint",
        "move=2,60,70", "paint",
        "cursor=200,100", "paint",
        "paint",
    };
    static const char *const cmds_v[2] = { "win=0,0,320,200,ff203040,", "win=50,50,100,60,ff506070," };
    const char *cmds[2 + sizeof(more) / sizeof(more[0])];
    for (unsigned i = 0; i < 2; i++)
        cmds[i] = cmds_v[i];
    for (unsigned i = 0; i < sizeof(more) / sizeof(more[0]); i++)
        cmds[2 + i] = more[i];
    struct cp_run r;
    static uint32_t want[CP_W * CP_H];
    static uint8_t unknown[CP_W * CP_H];
    bool ok = cp_run(cmds, sizeof(cmds) / sizeof(cmds[0]), &r);
    if (ok) {
        struct cp_win moved[2] = { v[0], v[1] };
        moved[1].x = 60;
        moved[1].y = 70;
        ref_paint(want, unknown, moved, 2);
        ok = cp_same_image(&r, want, NULL, cp_arrow_at(200, 100));
    }
    if (ok) {
        CHECK_EQ(r.nrep, 5);
        CHECK_EQ(r.rep[0].px, CP_W * CP_H);
        CHECK_EQ(r.rep[1].px, 40 * 30 + 40 * 30 - 20 * 20);   /* the union, the third inside */
        CHECK_EQ(r.rep[2].px, 100 * 60 * 2 - 90 * 40);        /* the old place and the new */
        CHECK_EQ(r.rep[3].px, CURSOR_IMG * CURSOR_IMG);
        CHECK_EQ(r.rep[4].px, 0);
        /* the arrow, its hot spot at the pointer: all fill well inside it */
        CHECK_EQ(r.image[(100 + CP_FILL_DY) * CP_W + 200 + CP_FILL_DX], CP_ARROW_FILL);
    }
    cp_done(&r);
    return ok;
}

bool t_comp_paint_fullscreen(void)
{
    static const char *const cmds[] = {
        "win=0,0,320,200,ff336699,", "paint",
        "cursor=100,100", "paint",
        "damage=0,0,320,200", "paint",
        "win=10,10,50,50,80ffffff,", "paint",
    };
    struct cp_run r;
    bool ok = cp_run(cmds, sizeof(cmds) / sizeof(cmds[0]), &r);
    if (ok) {
        CHECK_EQ(r.nrep, 4);
        CHECK(r.rep[0].tiles > 0);
        CHECK_EQ(r.rep[0].direct, r.rep[0].tiles);   /* all of it straight from the buffer */
        CHECK_EQ(r.rep[1].direct, 0);                /* the cursor's box: composed */
        CHECK(r.rep[2].direct > 0 && r.rep[2].direct < r.rep[2].tiles);
        CHECK_EQ(r.rep[2].px, CP_W * CP_H);
        CHECK_EQ(r.rep[3].direct, 0);                /* a window over it: no longer full screen */
        static const struct cp_win v[] = {
            { 0, 0, CP_W, CP_H, 0xff336699, false, false, 0, false },
            { 10, 10, 50, 50, 0x80ffffff, false, false, 0, false },
        };
        static uint32_t want[CP_W * CP_H];
        static uint8_t unknown[CP_W * CP_H];
        ref_paint(want, unknown, v, 2);
        ok = cp_same_image(&r, want, NULL, cp_arrow_at(100, 100));
        CHECK_EQ(r.image[(100 + CP_FILL_DY) * CP_W + 100 + CP_FILL_DX], CP_ARROW_FILL);
    }
    cp_done(&r);
    return ok;
}

bool t_comp_paint_cursor(void)
{
    static const char *const cmds[] = {
        "win=0,0,320,200,ff203040,", "cursor=100,80", "cursorsurf=40,30,80c03060,10,5", "paint",
        "cursorsize=20,10", "paint", "cursor=200,150", "paint",
    };
    struct cp_run r;
    bool ok = cp_run(cmds, sizeof(cmds) / sizeof(cmds[0]), &r);
    if (ok) {
        CHECK_EQ(r.nrep, 3);
        CHECK_EQ(r.rep[1].px, 40 * 30);   /* shrunk: where it was, all of it */
        CHECK_EQ(r.rep[2].px, 2 * 20 * 10);
        /* The client's cursor, blended at its hot spot; nothing left behind. */
        static const struct cp_win v[] = {
            { 0, 0, CP_W, CP_H, 0xff203040, false, false, 0, false },
            { 190, 145, 20, 10, 0x80c03060, false, false, 0, false },
        };
        static uint32_t want[CP_W * CP_H];
        static uint8_t unknown[CP_W * CP_H];
        ref_paint(want, unknown, v, 2);
        ok = cp_same_image(&r, want, NULL, no_skip);
    }
    cp_done(&r);
    return ok;
}

bool t_comp_paint_blank(void)
{
    static const char *const cmds[] = {
        "win=10,10,100,100,ff123456,t", "cursor=50,50", "blank", "paint",
    };
    struct cp_run r;
    bool ok = cp_run(cmds, 4, &r);
    if (ok) {
        CHECK_EQ(cp_count_colour(&r, (struct comp_box){ 0, 0, CP_W, CP_H }, LOOK_BLANK),
                 CP_W * CP_H);
        CHECK_EQ(r.rep[0].layer_px, 0);
    }
    cp_done(&r);
    return ok;
}

/* The desktop with a floating window; then a boot overlay (the test
 * scene's client takes no keys) over it. */
#define OV_DESK  "nocursor", "desktop", "time=2026,10,5,14,32", "top=120,60,ff3060a0,Term,f", \
                 "paint"
#define OV_SPLASH "top=320,200,ff70a030,Splash,F", "paint"
#define OV_FADE  "animate", "close=2", "tick=50", "paint"

/* Pixels of run r equal to run o's. */
static unsigned same_px(const struct cp_run *r, const struct cp_run *o)
{
    unsigned n = 0;
    for (unsigned i = 0; i < CP_W * CP_H; i++)
        n += r->image[i] == o->image[i];
    return n;
}

static bool overlay_runs(struct cp_run *desk, struct cp_run *over, struct cp_run *mid,
                         struct cp_run *end)
{
    static const char *const d[] = { OV_DESK };
    static const char *const o[] = { OV_DESK, OV_SPLASH };
    static const char *const m[] = { OV_DESK, OV_SPLASH, OV_FADE };
    static const char *const e[] = { OV_DESK, OV_SPLASH, OV_FADE, "tick=250", "paint" };
    CHECK(cp_run(d, sizeof(d) / sizeof(d[0]), desk));
    CHECK(cp_run(o, sizeof(o) / sizeof(o[0]), over));
    CHECK(cp_run(m, sizeof(m) / sizeof(m[0]), mid));
    CHECK(cp_run(e, sizeof(e) / sizeof(e[0]), end));
    /* the overlay: its own pixels everywhere (over the strip too), direct */
    for (int32_t y = 0; y < CP_H; y++)
        for (int32_t x = 0; x < CP_W; x++)
            CHECK_EQ(over->image[y * CP_W + x], testscene_rgb(0x70a030, x, y, false));
    CHECK_EQ(over->nrep, 2);
    CHECK(over->rep[1].tiles > 0);
    CHECK_EQ(over->rep[1].direct, over->rep[1].tiles);
    /* the strip is there without it */
    CHECK(same_px(desk, over) < CP_W * CP_H / 100);
    /* half-way: neither (a blend of both), its box painted, composed */
    CHECK_EQ(mid->nrep, 3);
    CHECK(same_px(mid, desk) < CP_W * CP_H / 2);
    CHECK(same_px(mid, over) < CP_W * CP_H / 2);
    CHECK_EQ(mid->rep[2].px, CP_W * CP_H);
    CHECK_EQ(mid->rep[2].direct, 0);
    /* the end: exactly the desktop without it */
    CHECK_EQ(end->nrep, 4);
    CHECK_EQ(end->rep[3].px, CP_W * CP_H);
    CHECK_EQ(same_px(end, desk), CP_W * CP_H);
    return true;
}

bool t_comp_paint_overlay(void)
{
    static struct cp_run r[4];
    memset(r, 0, sizeof(r));
    bool ok = overlay_runs(&r[0], &r[1], &r[2], &r[3]);
    for (unsigned i = 0; i < 4; i++)
        cp_done(&r[i]);
    return ok;
}
