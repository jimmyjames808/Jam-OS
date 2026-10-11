/* utest: the window geometry (xdg_surface.set_window_geometry) in the
 * compositor's window manager (user/services/compositor wm.c, deco.c,
 * scene.c's window_geom, window_geometry, window_frame and window_extent),
 * on compwm.c's harness (compwm.h): the output is 1280x800, floating, the
 * desktop off.
 *
 * t_wm_geometry: a client that draws its own decorations and its own
 * shadow (8 left, 24 right, 4 above, 28 below a 64x64 window: a 96x96
 * surface) is placed by its geometry: the window centred, its surface that
 * much up and left of it; its frame is the geometry; it gets neither our
 * corners nor our shadow (LOOK_PLAIN), and what it paints reaches its
 * whole surface (window_extent); a press on its own shadow is its own; it
 * is asked for its geometry's size, maximised to fill the room with its
 * window (its shadow past the output), and back at its place; a geometry
 * set without a commit changes nothing until the commit, then the window
 * stays where it is while its shadow grows. A window whose compositor
 * draws its title bar and has a geometry inset the same way: its frame
 * and title bar go round the geometry, the cascade counts geometries, its
 * right edge resizes past the geometry where its surface still is. A
 * client-side window with no geometry keeps our corners and shadow
 * (LOOK_CSD). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include "compwm.h"
#include "look.h"
#include "utest.h"

#define SIDE 64   /* the window (its geometry), floating */
#define SH_L 8    /* the client's shadow round it */
#define SH_R 24
#define SH_T 4
#define SH_B 28

static void gk_configure(void *ctx, const struct wm_config *c)
{
    struct fk *f = ctx;
    f->cfg = *c;
    f->nconf++;
}

static void gk_close(void *ctx)
{
    ((struct fk *)ctx)->closes++;
}

static const struct wm_ops gk_ops = { gk_configure, gk_close, NULL };

/* f draws its last configure's size (its own, SIDE, at 0) as its window,
 * its shadow (l, t, r, b) round it, sets that window geometry and commits. */
static bool gk_draw(struct fk *f, int32_t l, int32_t t, int32_t r, int32_t b)
{
    int32_t w = f->cfg.width ? f->cfg.width : f->own_w;
    int32_t h = f->cfg.height ? f->cfg.height : f->own_h;
    f->s.width = l + w + r;
    f->s.height = t + h + b;
    if (l || t || r || b)
        wm_set_geometry(f->ww, box_make(l, t, w, h));
    CHECK_ST(wm_commit(f->ww, f->cfg.states), OK);
    return true;
}

/* A toplevel of SIDE x SIDE, csd: its client draws its own decorations
 * (it never bound the decoration manager). Up to its initial commit. */
static bool gk_open(struct fk *f, bool csd)
{
    memset(f, 0, sizeof(*f));
    f->s.client = &fake_client;
    f->s.input_all = true;
    f->own_w = f->own_h = SIDE;
    CHECK((f->ww = wm_create(&f->s, &gk_ops, f)) != NULL);
    f->ww->csd = csd;
    wm_reconfigure(f->ww);   /* the initial commit */
    return true;
}

static bool box_is(struct comp_box b, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    if (b.x1 != x1 || b.y1 != y1 || b.x2 != x2 || b.y2 != y2)
        FAIL("box %d,%d..%d,%d, want %d,%d..%d,%d", b.x1, b.y1, b.x2, b.y2, x1, y1, x2, y2);
    return true;
}

/* 0: client-side, its own shadow. */
static bool csd_steps(void)
{
    struct fk *f = &fks[0];
    bool on;
    uint32_t e;
    CHECK(gk_open(f, true));
    CFG(0, 0, 0, WM_ST_CSD);
    CHECK(gk_draw(f, SH_L, SH_T, SH_R, SH_B));
    /* the window centred: (1280 - 64) / 2, (800 - 64) / 2 */
    AT(0, 608 - SH_L, 368 - SH_T);
    CHECK(box_is(window_frame(win(0)), 608, 368, 672, 432));
    CHECK(box_is(window_geometry(win(0)), 608, 368, 672, 432));
    CHECK(window_inset(win(0)));
    CHECK(!(win(0)->flags & COMP_WIN_CSD));   /* no corners or shadow of ours */
    CHECK_EQ(look_of(win(0)), LOOK_PLAIN);
    CHECK(box_is(window_extent(win(0)), 600, 364, 696, 460));   /* its whole surface */
    /* a press on its own shadow, left of and below the window: its own */
    CHECK_EQ(deco_hit(win(0), 603, 400, &e), DECO_SURFACE);
    CHECK_EQ(deco_hit(win(0), 640, 450, &e), DECO_SURFACE);
    CHECK(wm_window_at(603, 400, &on) == win(0) && on);
    CHECK(wm_window_at(640, 450, &on) == win(0) && on);
    CHECK(wm_window_at(599, 400, &on) == NULL);   /* past its surface: nobody's */
    /* asked for its window's size, not its surface's */
    CHECK_EQ(f->ww->float_w, SIDE);
    CHECK_EQ(f->ww->float_h, SIDE);
    wm_reconfigure(f->ww);
    CFG(0, SIDE, SIDE, WM_ST_CSD);
    /* maximised: its window fills the room, its shadow past the output */
    wm_request_maximized(f->ww, true);
    CFG(0, OUT_W, OUT_H, WM_ST_MAXIMIZED | WM_ST_CSD);
    CHECK(gk_draw(f, SH_L, SH_T, SH_R, SH_B));
    AT(0, -SH_L, -SH_T);
    CHECK(box_is(window_frame(win(0)), 0, 0, OUT_W, OUT_H));
    /* back: where it was */
    wm_request_maximized(f->ww, false);
    CFG(0, SIDE, SIDE, WM_ST_CSD);
    CHECK(gk_draw(f, SH_L, SH_T, SH_R, SH_B));
    AT(0, 608 - SH_L, 368 - SH_T);
    /* a geometry is the commit's: set alone, nothing moves */
    wm_set_geometry(f->ww, box_make(16, 16, SIDE, SIDE));
    AT(0, 608 - SH_L, 368 - SH_T);
    CHECK(box_is(window_frame(win(0)), 608, 368, 672, 432));
    /* its shadow grows with the commit: the window stays, the surface moves */
    CHECK(gk_draw(f, 16, 16, 16, 16));
    AT(0, 608 - 16, 368 - 16);
    CHECK(box_is(window_frame(win(0)), 608, 368, 672, 432));
    CHECK(box_is(window_extent(win(0)), 592, 352, 688, 448));
    return true;
}

/* 1: the compositor's decorations round a geometry inset the same way. */
static bool ssd_steps(void)
{
    struct fk *f = &fks[1];
    uint32_t e;
    CHECK(gk_open(f, false));
    CHECK(gk_draw(f, SH_L, SH_T, SH_R, SH_B));
    /* centred where 0's window is: one cascade step (28 + 2) on, by the
     * geometries' corners */
    AT(1, 638 - SH_L, 398 - SH_T);
    CHECK(box_is(window_geometry(win(1)), 638, 398, 702, 462));
    /* the frame: a title bar above the geometry, an outline round it */
    CHECK(box_is(window_frame(win(1)), 637, 398 - COMP_TITLE_H, 703, 463));
    CHECK(box_is(title_bar_box(win(1)), 637, 398 - COMP_TITLE_H, 703, 398));
    CHECK_EQ(look_of(win(1)), LOOK_FLOATING);
    CHECK_EQ(deco_hit(win(1), 700, 395, &e), DECO_TITLE);   /* (below the circles) */
    /* its right edge, past the geometry (its surface still there): ours */
    CHECK_EQ(deco_hit(win(1), 704, 430, &e), DECO_EDGE);
    CHECK_EQ(e, WM_EDGE_RIGHT);
    CHECK_EQ(deco_hit(win(1), 670, 430, &e), DECO_SURFACE);
    return true;
}

/* 2: client-side with no geometry: our corners and shadow, as before. */
static bool plain_csd_steps(void)
{
    struct fk *f = &fks[2];
    CHECK(gk_open(f, true));
    f->own_w = 100;
    f->own_h = 80;
    CHECK(gk_draw(f, 0, 0, 0, 0));
    AT(2, 590, 360);
    CHECK(!window_inset(win(2)));
    CHECK(win(2)->flags & COMP_WIN_CSD);
    CHECK_EQ(look_of(win(2)), LOOK_CSD);
    CHECK(box_is(window_frame(win(2)), 590, 360, 690, 440));
    return true;
}

bool t_wm_geometry(void)
{
    wm_test_start(COMP_FLOATING);
    bool ok = csd_steps() && ssd_steps() && plain_csd_steps();
    fk_close_all();
    return ok;
}
