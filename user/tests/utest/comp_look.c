/* utest: the compositor's look (look.h: user/services/compositor title.c,
 * shape.c, mask.c, wallpaper.c), through bin/compositor's test scene run
 * headless and the reference painter (comppaint.h, comp_ref.c): every
 * pixel the reference paints is compared exactly, and the title's text and
 * circles, which it doesn't paint, are checked here.
 *
 * t_comp_look_title: floating windows' title bars, outlines and circles,
 * focused and not; the title's text centred in its bar.
 * t_comp_look_corners: a floating window's rounded corners cut the
 * client's own pixels too, and the window below (and the shadow on it)
 * shows through them; a translucent floating window over another.
 * t_comp_look_shadow: the shadow under a focused and an unfocused window,
 * exact; a move paints exactly the old and new extents (frame and
 * shadow), and leaves nothing behind.
 * t_comp_look_buttons: with the pointer over an unfocused window's
 * circles they show their colours and symbols, and go back to grey when
 * it leaves; a focused window's show their colours without symbols.
 * t_comp_look_tiled: tiled windows' borders, focused and not, rounded with
 * the border following the curve; no shadow.
 * t_comp_look_wallpaper: the wallpaper is look.h's formula, pixel for
 * pixel, the same on every run, and smooth (no step between neighbours). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <os.h>
#include "comppaint.h"
#include "utest.h"

static const struct comp_box no_skip = { 0, 0, 0, 0 };
static const char *const paint1[] = { "paint" };

/* The pixel (i, j) of the circle box at (bx, by) of a run's image. */
static uint32_t btn_px(const struct cp_run *r, int32_t bx, int32_t by, int32_t i, int32_t j)
{
    return r->image[(by + j) * CP_W + bx + i];
}

/* The box of circle b of a floating window at surface (x, y). */
static struct comp_box btn_box(int32_t x, int32_t y, int b)
{
    int32_t bx = x - DECO_OUTLINE + LOOK_BTN_LEFT + b * (LOOK_BTN_D + LOOK_BTN_GAP);
    int32_t by = y - COMP_TITLE_H + LOOK_BTN_TOP;
    return (struct comp_box){ bx, by, bx + LOOK_BTN_D, by + LOOK_BTN_D };
}

/* The pixels in box b of the image nearer the text's colour ink than the
 * bar's (the text is anti-aliased: its green channel past the middle):
 * how many, and the leftmost and rightmost columns. */
static unsigned ink_span(const struct cp_run *r, struct comp_box b, uint32_t ink, uint32_t bar,
                         int32_t *x1, int32_t *x2)
{
    unsigned n = 0, mid = ((ink >> 8 & 0xff) + (bar >> 8 & 0xff)) / 2;
    *x1 = CP_W;
    *x2 = -1;
    for (int32_t y = b.y1; y < b.y2; y++)
        for (int32_t x = b.x1; x < b.x2; x++) {
            if ((r->image[y * CP_W + x] >> 8 & 0xff) <= mid)
                continue;
            n++;
            *x1 = x < *x1 ? x : *x1;
            *x2 = x > *x2 ? x : *x2;
        }
    return n;
}

/* ---- title bars ------------------------------------------------------------------------- */

static bool title_checks(const struct cp_run *r)
{
    /* Window 1, focused: its circles in their colours (each disc's middle
     * pixels are all disc), no symbols. */
    static const uint32_t lit[3] = { LOOK_CLOSE, LOOK_MINIMISE, LOOK_FULLSCREEN };
    for (int b = 0; b < 3; b++) {
        struct comp_box c = btn_box(40, 44, b);
        CHECK_EQ(btn_px(r, c.x1, c.y1, 5, 5), lit[b]);
        CHECK_EQ(btn_px(r, c.x1, c.y1, 6, 6), lit[b]);
        struct comp_box d = btn_box(100, 128, b);   /* window 2: grey */
        CHECK_EQ(btn_px(r, d.x1, d.y1, 5, 5), LOOK_BTN_IDLE);
    }
    /* The titles' text, centred in each bar (frames 39..241 and 99..301). */
    int32_t x1, x2;
    struct comp_box text1 = { 39 + LOOK_BTNS_W, 17, 241 - LOOK_BTNS_W, 43 };
    CHECK(ink_span(r, text1, LOOK_TITLE_FOCUSED, LOOK_BAR_FOCUSED, &x1, &x2) > 20);
    CHECK((x1 + x2 + 1) / 2 >= 140 - 3 && (x1 + x2 + 1) / 2 <= 140 + 3);
    struct comp_box text2 = { 99 + LOOK_BTNS_W, 101, 301 - LOOK_BTNS_W, 127 };
    CHECK(ink_span(r, text2, LOOK_TITLE, LOOK_BAR, &x1, &x2) > 20);
    CHECK((x1 + x2 + 1) / 2 >= 200 - 3 && (x1 + x2 + 1) / 2 <= 200 + 3);
    CHECK(x1 >= 99 + LOOK_BTNS_W + LOOK_TEXT_PAD);
    return true;
}

bool t_comp_look_title(void)
{
    static const struct cp_win v[] = {
        { 40, 44, 200, 96, 0xff305070, false, false, 't', true },
        { 100, 128, 200, 56, 0xff705030, false, false, 't', false },
    };
    struct cp_run r;
    bool ok = cp_run_windows(v, 2, paint1, 1, no_skip, &r) && title_checks(&r);
    if (ok) {   /* and nothing past the shadow's reach */
        CHECK_EQ(r.image[0], ref_wallpaper(0, 0));
        CHECK_EQ(r.image[100 * CP_W + 39 - LOOK_SHADOW_F_REACH - 1],
                 ref_wallpaper(39 - LOOK_SHADOW_F_REACH - 1, 100));
    }
    cp_done(&r);
    return ok;
}

/* ---- corners and shadows ----------------------------------------------------------------- */

bool t_comp_look_corners(void)
{
    static const struct cp_win v[] = {
        { 0, 0, CP_W, CP_H, 0xff4080c0, false, false, 0, false },   /* a picture below */
        { 80, 60, 160, 90, 0xff30a050, false, false, 't', true },
        { 200, 20, 90, 60, 0xc0e03060, false, false, 't', false },  /* translucent, over it */
    };
    struct cp_run r;
    bool ok = cp_run_windows(v, 3, paint1, 1, no_skip, &r);
    if (ok) {
        /* The surface's own bottom-left pixel is cut: the picture below
         * (darkened by the shadow) shows there, not the client's. */
        uint32_t p = r.image[149 * CP_W + 80];
        CHECK(p != testscene_rgb(0xff30a050, 0, 89, false));
        CHECK(p != LOOK_OUTLINE_FOCUSED);
        /* The frame's very corner is all below: no outline, no client. */
        uint32_t below = testscene_rgb(0xff4080c0, 79, 150, false);
        CHECK(r.image[150 * CP_W + 79] != below);   /* the shadow is on it */
        /* A pixel inside the corner's curve is the client's. */
        CHECK_EQ(r.image[140 * CP_W + 90], testscene_rgb(0xff30a050, 10, 80, false));
    }
    cp_done(&r);
    return ok;
}

/* The pixels of a and b together. */
static uint64_t union_px(struct comp_box a, struct comp_box b)
{
    struct comp_box out = { 0, 0, CP_W, CP_H };
    a = box_intersect(a, out);
    b = box_intersect(b, out);
    struct comp_box both = box_intersect(a, b);
    uint64_t pa = box_empty(a) ? 0 : (uint64_t)(a.x2 - a.x1) * (uint64_t)(a.y2 - a.y1);
    uint64_t pb = box_empty(b) ? 0 : (uint64_t)(b.x2 - b.x1) * (uint64_t)(b.y2 - b.y1);
    uint64_t pc = box_empty(both) ? 0
                                  : (uint64_t)(both.x2 - both.x1) * (uint64_t)(both.y2 - both.y1);
    return pa + pb - pc;
}

bool t_comp_look_shadow(void)
{
    static const struct cp_win v[] = {
        { 0, 0, CP_W, CP_H, 0xff4080c0, false, false, 0, false },
        { 200, 120, 80, 40, 0xff806040, false, false, 't', false },   /* not focused */
        { 40, 60, 120, 70, 0xff30a050, false, false, 't', true },
    };
    static const char *const more[] = { "paint", "move=3,90,80", "paint" };
    struct cp_run r;
    bool ok = cp_run_windows(v, 3, more, 1, no_skip, &r);   /* the first paint, checked */
    cp_done(&r);
    if (!ok)
        return false;
    /* moved: exactly the old and new extents painted, the image exact */
    static char cmd[3][64];
    static uint32_t want[CP_W * CP_H];
    static uint8_t unknown[CP_W * CP_H];
    const char *cmds[6] = { cmd[0], cmd[1], cmd[2], more[0], more[1], more[2] };
    for (unsigned i = 0; i < 3; i++)
        cp_win_cmd(cmd[i], sizeof(cmd[i]), &v[i]);
    struct cp_win moved[3] = { v[0], v[1], v[2] };
    moved[2].x = 90;
    moved[2].y = 80;
    ok = cp_run(cmds, 6, &r);
    if (ok) {
        CHECK_EQ(r.nrep, 2);
        CHECK_EQ(r.rep[1].px, union_px(ref_extent(&v[2]), ref_extent(&moved[2])));
        ref_paint(want, unknown, moved, 3);
        ok = cp_same_image(&r, want, unknown, no_skip);
        /* the shadow is where it should be: below the window, darker */
        struct comp_box f = ref_frame(&moved[2]);
        CHECK(r.image[(f.y2 + 4) * CP_W + 150] != testscene_rgb(0xff4080c0, 150, f.y2 + 4, false));
    }
    cp_done(&r);
    return ok;
}

/* ---- the circles under the pointer -------------------------------------------------------- */

/* a over b by coverage c, rounded as the compositor's blends are. */
static uint32_t mix(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t x = (a >> sh & 0xff) * (255 - c) + (b >> sh & 0xff) * c + 128;
        out |= ((x + (x >> 8)) >> 8) << sh;
    }
    return out;
}

/* Window 2's circles (surface at 100, 128) with the pointer over them. */
static bool hovered_checks(const struct cp_run *r)
{
    struct comp_box c = btn_box(100, 128, 0), m = btn_box(100, 128, 1), f = btn_box(100, 128, 2);
    /* their colours, where no symbol is */
    CHECK_EQ(btn_px(r, c.x1, c.y1, 6, 2), LOOK_CLOSE);
    CHECK_EQ(btn_px(r, m.x1, m.y1, 2, 6), LOOK_MINIMISE);
    CHECK_EQ(btn_px(r, f.x1, f.y1, 9, 2), LOOK_FULLSCREEN);
    /* the symbols: the x's crossing, the bar (y 5.25 to 6.75: 3/4 of row
     * 5), an arrowhead's inside */
    CHECK_EQ(btn_px(r, c.x1, c.y1, 5, 5), LOOK_CLOSE_INK);
    CHECK_EQ(btn_px(r, c.x1, c.y1, 6, 6), LOOK_CLOSE_INK);
    CHECK_EQ(btn_px(r, m.x1, m.y1, 6, 5), mix(LOOK_MINIMISE, LOOK_MINIMISE_INK, 191));
    CHECK_EQ(btn_px(r, f.x1, f.y1, 3, 3), LOOK_FULLSCREEN_INK);
    CHECK_EQ(btn_px(r, f.x1, f.y1, 8, 8), LOOK_FULLSCREEN_INK);
    return true;
}

bool t_comp_look_buttons(void)
{
    static const struct cp_win v[] = {
        { 40, 44, 200, 96, 0xff305070, false, false, 't', true },
        { 100, 128, 200, 56, 0xff705030, false, false, 't', false },
    };
    /* the pointer onto window 2's circles (its tip at the full-screen
     * circle's hit box's far corner: the arrow covers none of them) */
    struct comp_box hit = btn_box(100, 128, 2);
    char at[32];
    snprintf(at, sizeof(at), "cursor=%d,%d", hit.x2 + LOOK_BTN_HIT - 1, hit.y2 + LOOK_BTN_HIT - 1);
    const char *more[] = { "paint", at, "paint" };
    struct cp_run r;
    struct comp_box arrow = { hit.x2 + LOOK_BTN_HIT - 1, hit.y2 + LOOK_BTN_HIT - 1, CP_W, CP_H };
    bool ok = cp_run_windows(v, 2, more, 3, arrow, &r) && hovered_checks(&r);
    if (ok) {
        CHECK_EQ(r.nrep, 2);
        struct comp_box c = btn_box(40, 44, 0);   /* the focused one: colours, no symbol */
        CHECK_EQ(btn_px(&r, c.x1, c.y1, 5, 5), LOOK_CLOSE);
        /* the circles were painted again for it */
        CHECK(r.rep[1].px >= (uint64_t)(3 * LOOK_BTN_D + 2 * LOOK_BTN_GAP) * LOOK_BTN_D);
    }
    cp_done(&r);
    if (!ok)
        return false;
    /* ... and the pointer gone from them: grey again, no symbols */
    const char *away[] = { "paint", at, "paint", "cursor=300,10", "paint" };
    ok = cp_run_windows(v, 2, away, 5, (struct comp_box){ 300, 10, CP_W, 40 }, &r);
    if (ok) {
        struct comp_box c = btn_box(100, 128, 0);
        CHECK_EQ(btn_px(&r, c.x1, c.y1, 5, 5), LOOK_BTN_IDLE);
        CHECK_EQ(btn_px(&r, c.x1, c.y1, 6, 2), LOOK_BTN_IDLE);
    }
    cp_done(&r);
    return ok;
}

/* ---- tiling ------------------------------------------------------------------------------- */

bool t_comp_look_tiled(void)
{
    static const struct cp_win v[] = {
        { 8, 8, 147, 184, 0xff305070, false, false, 'g', true },
        { 167, 8, 145, 184, 0xff705030, false, false, 'g', false },
    };
    struct cp_run r;
    bool ok = cp_run_windows(v, 2, paint1, 1, no_skip, &r);
    if (ok) {
        CHECK_EQ(r.image[100 * CP_W + 6], LOOK_TILE_FOCUSED);     /* the borders */
        CHECK_EQ(r.image[100 * CP_W + 156], LOOK_TILE_FOCUSED);
        CHECK_EQ(r.image[100 * CP_W + 165], LOOK_TILE);
        CHECK_EQ(r.image[6 * CP_W + 6], ref_wallpaper(6, 6));     /* the corner: cut */
        CHECK_EQ(r.image[4 * CP_W + 80], ref_wallpaper(80, 4));   /* the gap: no shadow */
        CHECK_EQ(r.image[100 * CP_W + 160], ref_wallpaper(160, 100));
    }
    cp_done(&r);
    return ok;
}

/* ---- the wallpaper ------------------------------------------------------------------------- */

static bool smooth(const struct cp_run *r)
{
    for (int32_t y = 0; y < CP_H; y++)
        for (int32_t x = 1; x < CP_W; x++) {
            uint32_t a = r->image[y * CP_W + x - 1], b = r->image[y * CP_W + x];
            for (int sh = 0; sh < 24; sh += 8) {
                int32_t d = (int32_t)(a >> sh & 0xff) - (int32_t)(b >> sh & 0xff);
                if (d > 2 || d < -2)
                    FAIL("a step of %d at (%d, %d)", d, x, y);
            }
        }
    return true;
}

bool t_comp_look_wallpaper(void)
{
    static uint32_t first[CP_W * CP_H];
    struct cp_run r;
    bool ok = cp_run_windows(NULL, 0, paint1, 1, no_skip, &r) && smooth(&r);
    if (ok) {
        memcpy(first, r.image, sizeof(first));
        /* a glow is there: the blackcurrant one's middle is far from the base */
        uint32_t g = r.image[36 * CP_W + 48];
        CHECK((g >> 16 & 0xff) > (LOOK_WALL_BASE >> 16 & 0xff) + 30);
    }
    cp_done(&r);
    if (!ok)
        return false;
    ok = cp_run(paint1, 1, &r);
    if (ok && memcmp(first, r.image, sizeof(first)))
        ok = false;
    cp_done(&r);
    CHECK(ok);
    return true;
}
