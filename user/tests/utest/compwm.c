/* utest: the compositor's window manager (user/services/compositor wm.c,
 * wmtile.c, wmgrab.c, deco.c, on scene.c and region.c), linked in and
 * driven directly: fake toplevels play the client (they draw whatever
 * size each configure asks, or their own when it says 0 or they can't
 * resize), and the pointer and the keys are calls, as the seat makes them.
 * The protocol side (xdg-shell over real channels) is compxdg.c's.
 *
 * The output is 1280x800 in every test; decorations are DECO_TITLE_H (24)
 * above and DECO_BORDER (4) around, so the numbers below can be checked
 * by hand. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <settings.h>
#include "fattest.h"
#include "utest.h"
#include "wm.h"

#define OUT_W 1280
#define OUT_H 800
#define FK_MAX 6

/* A fake toplevel: its surface, the last configure it got, closes asked. */
struct fk {
    struct comp_surface s;
    struct wm_window *ww;
    struct wm_config cfg;
    unsigned nconf, closes;
    int32_t own_w, own_h;          /* what it draws when the configure says 0 */
    bool fixed;                    /* min = max = its own size */
};

static struct fk fks[FK_MAX];

/* What the seat heard from the window manager. */
static struct {
    struct comp_window *mapped[FK_MAX * 2];
    bool take_focus[FK_MAX * 2];
    unsigned nmapped, nunmapping, nfocus, nlayout;
    struct comp_window *unmapping, *focus;
    enum comp_layout layout;
} heard;

static void fk_configure(void *ctx, const struct wm_config *c)
{
    struct fk *f = ctx;
    f->cfg = *c;
    f->nconf++;
}

static void fk_close(void *ctx)
{
    ((struct fk *)ctx)->closes++;
}

static const struct wm_ops fk_ops = { fk_configure, fk_close };

static void on_mapped(struct comp_window *w, bool take_focus)
{
    if (heard.nmapped < FK_MAX * 2) {
        heard.mapped[heard.nmapped] = w;
        heard.take_focus[heard.nmapped] = take_focus;
    }
    heard.nmapped++;
}

static void on_unmapping(struct comp_window *w)
{
    heard.unmapping = w;
    heard.nunmapping++;
}

static void on_focus(struct comp_window *w)
{
    heard.focus = w;
    heard.nfocus++;
}

static void on_layout(enum comp_layout l)
{
    heard.layout = l;
    heard.nlayout++;
}

static void start(enum comp_layout layout)
{
    scene_init(OUT_W, OUT_H, 0);
    wm_init(layout);
    memset(fks, 0, sizeof(fks));
    memset(&heard, 0, sizeof(heard));
    comp_wm_hooks = (struct comp_wm_hooks){ .mapped = on_mapped, .unmapping = on_unmapping,
                                            .focus = on_focus, .layout_changed = on_layout };
}

/* The client answers its last configure: draws, commits. */
static bool fk_draw(struct fk *f)
{
    f->s.width = f->cfg.width && !f->fixed ? f->cfg.width : f->own_w;
    f->s.height = f->cfg.height && !f->fixed ? f->cfg.height : f->own_h;
    CHECK_ST(wm_commit(f->ww, f->cfg.states), OK);
    return true;
}

/* A toplevel of the client counting maps in *maps, w by h of its own:
 * the initial configure, then its first buffer. */
static bool fk_open(struct fk *f, uint32_t *maps, int32_t w, int32_t h, bool fixed)
{
    memset(f, 0, sizeof(*f));
    f->s.input_all = true;
    f->own_w = w;
    f->own_h = h;
    f->fixed = fixed;
    CHECK((f->ww = wm_create(&f->s, &fk_ops, f, maps)) != NULL);
    if (fixed)
        wm_set_limits(f->ww, w, h, w, h);
    wm_reconfigure(f->ww);   /* the initial commit */
    return fk_draw(f);
}

static void fk_close_all(void)
{
    for (unsigned i = 0; i < FK_MAX; i++)
        if (fks[i].ww)
            wm_destroy(fks[i].ww);
    memset(fks, 0, sizeof(fks));
    comp_wm_hooks = (struct comp_wm_hooks){ 0 };
}

static struct comp_window *win(unsigned i)
{
    return fks[i].ww->win;
}

#define AT(i, wx, wy)                                                   \
    do {                                                                \
        CHECK(win(i));                                                  \
        CHECK_EQ(win(i)->x, (wx));                                      \
        CHECK_EQ(win(i)->y, (wy));                                      \
    } while (0)
#define CFG(i, cw, ch, st)                                              \
    do {                                                                \
        CHECK_EQ(fks[i].cfg.width, (cw));                               \
        CHECK_EQ(fks[i].cfg.height, (ch));                              \
        CHECK_EQ(fks[i].cfg.states, (st));                              \
    } while (0)

/* A whole drag: press at (x, y), move by (dx, dy), release there. */
static bool drag(int32_t x, int32_t y, int32_t dx, int32_t dy, uint64_t t)
{
    CHECK(wm_pointer_press(x, y, WM_BTN_LEFT, t));
    CHECK(wm_grabbing());
    wm_pointer_motion(x + dx, y + dy);
    wm_pointer_release(x + dx, y + dy);
    CHECK(!wm_grabbing());
    return true;
}

/* ---- floating ---------------------------------------------------------------------- */

static bool floating_place_steps(void)
{
    uint32_t a = 0, b = 0;
    CHECK(fk_open(&fks[0], &a, 320, 200, false));
    CFG(0, 0, 0, 0);                     /* the initial configure: its own size */
    AT(0, 480, 300);                     /* centred */
    CHECK(fk_open(&fks[1], &a, 320, 200, false));
    AT(1, 508, 328);                     /* one cascade step: 24 + 4 */
    CHECK(fk_open(&fks[2], &b, 320, 200, false));
    AT(2, 536, 356);
    CHECK_EQ(heard.nmapped, 3);
    CHECK(heard.take_focus[0] && !heard.take_focus[1] && heard.take_focus[2]);
    CHECK_EQ(scene.top, win(2));
    /* the first one's corner is free again; its client's next window
     * doesn't take the focus (the rule is per client, for its life) */
    wm_destroy(fks[0].ww);
    fks[0].ww = NULL;
    CHECK_EQ(heard.unmapping == NULL, false);
    CHECK(fk_open(&fks[3], &a, 320, 200, false));
    AT(3, 480, 300);
    CHECK(!heard.take_focus[3]);
    /* bigger than the output: its frame's corner at the output's */
    CHECK(fk_open(&fks[4], &b, 1400, 900, false));
    AT(4, DECO_BORDER, DECO_TITLE_H);
    /* the size it drew is what it is asked for from now on */
    wm_reconfigure(fks[2].ww);
    CFG(2, 320, 200, 0);
    return true;
}

bool t_wm_floating_place(void)
{
    start(COMP_FLOATING);
    bool ok = floating_place_steps();
    fk_close_all();
    return ok;
}

static bool floating_move_steps(void)
{
    uint32_t a = 0;
    CHECK(fk_open(&fks[0], &a, 320, 200, false));
    AT(0, 480, 300);
    /* the title bar is y 276..299: drag it */
    CHECK(drag(600, 290, 100, 50, 1 * NS_PER_S));
    AT(0, 580, 350);
    CHECK_EQ(heard.focus, win(0));       /* a press on decorations asks for the focus */
    /* never off the top: the title bar stays reachable */
    CHECK(drag(700, 340, 0, -1000, 3 * NS_PER_S));
    AT(0, 580, DECO_TITLE_H);
    /* the surface is the client's, the background nobody's */
    CHECK(!wm_pointer_press(700, 100, WM_BTN_LEFT, 5 * NS_PER_S));
    CHECK(!wm_pointer_press(10, 790, WM_BTN_LEFT, 5 * NS_PER_S));
    CHECK(!wm_grabbing());
    /* another button on the title bar: taken, does nothing */
    CHECK(wm_pointer_press(700, 10, 0x111, 7 * NS_PER_S));
    wm_pointer_motion(800, 300);
    wm_pointer_release(800, 300);
    AT(0, 580, DECO_TITLE_H);
    /* the close box: closes on a release on it, not elsewhere */
    struct comp_box c = deco_close_box(win(0));
    CHECK(!box_empty(c) && c.x2 == 580 + 320 && c.y2 == DECO_TITLE_H);
    int32_t cx = (c.x1 + c.x2) / 2, cy = (c.y1 + c.y2) / 2;
    CHECK(wm_pointer_press(cx, cy, WM_BTN_LEFT, 9 * NS_PER_S));
    wm_pointer_release(cx, cy);
    CHECK_EQ(fks[0].closes, 1);
    CHECK(wm_pointer_press(cx, cy, WM_BTN_LEFT, 11 * NS_PER_S));
    wm_pointer_release(cx - 100, cy + 100);
    CHECK_EQ(fks[0].closes, 1);
    AT(0, 580, DECO_TITLE_H);            /* a close box press never moves it */
    return true;
}

bool t_wm_floating_move(void)
{
    start(COMP_FLOATING);
    bool ok = floating_move_steps();
    fk_close_all();
    return ok;
}

static bool resize_right_and_left(void)
{
    /* the right border is x 800..803 */
    CHECK(wm_pointer_press(801, 400, WM_BTN_LEFT, 1 * NS_PER_S));
    CFG(0, 320, 200, WM_ST_RESIZING);
    wm_pointer_motion(901, 400);
    CFG(0, 420, 200, WM_ST_RESIZING);
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    wm_pointer_release(901, 400);
    CFG(0, 420, 200, 0);
    CHECK(fk_draw(&fks[0]));
    /* the left border (x 476..479): the right side (900) stays put */
    CHECK(drag(478, 400, -100, 0, 3 * NS_PER_S));
    CFG(0, 520, 200, 0);
    AT(0, 480, 300);                     /* not drawn yet: where it was */
    CHECK(fk_draw(&fks[0]));
    AT(0, 380, 300);
    CHECK_EQ(win(0)->x + fks[0].s.width, 900);
    return true;
}

static bool floating_resize_steps(void)
{
    uint32_t a = 0, b = 0;
    CHECK(fk_open(&fks[0], &a, 320, 200, false));
    CHECK(resize_right_and_left());
    /* a corner: bottom right takes both */
    struct comp_box s = window_surface_box(win(0));
    CHECK(drag(s.x2 + 1, s.y2 + 1, 10, 20, 5 * NS_PER_S));
    CFG(0, 530, 220, 0);
    CHECK(fk_draw(&fks[0]));
    /* the client's limits, and never under WM_MIN_SIDE */
    wm_set_limits(fks[0].ww, 0, 0, 600, 0);
    s = window_surface_box(win(0));
    CHECK(drag(s.x2 + 1, (s.y1 + s.y2) / 2, 500, 0, 7 * NS_PER_S));
    CFG(0, 600, 220, 0);
    CHECK(fk_draw(&fks[0]));
    s = window_surface_box(win(0));
    CHECK(drag((s.x1 + s.x2) / 2, s.y2 + 1, 0, -1000, 9 * NS_PER_S));
    CFG(0, 600, WM_MIN_SIDE, 0);
    CHECK(fk_draw(&fks[0]));
    /* a window that can't resize: its border does nothing */
    CHECK(fk_open(&fks[1], &b, 200, 100, true));
    s = window_surface_box(win(1));
    unsigned n = fks[1].nconf;
    CHECK(drag(s.x2 + 1, (s.y1 + s.y2) / 2, 50, 0, 11 * NS_PER_S));
    CHECK_EQ(fks[1].nconf, n);
    CHECK_ST(wm_begin_resize(win(1), WM_EDGE_RIGHT, 0, 0), ERR_BAD_STATE);
    CHECK_ST(wm_begin_resize(win(0), WM_EDGE_LEFT | WM_EDGE_RIGHT, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(wm_begin_resize(win(0), 0, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(wm_begin_resize(win(0), 16, 0, 0), ERR_INVALID_ARGS);
    /* a client's own resize (xdg_toplevel.resize, after the seat's check) */
    CHECK_ST(wm_begin_resize(win(0), WM_EDGE_BOTTOM, 0, 0), OK);
    CHECK_ST(wm_begin_move(win(0), 0, 0), ERR_BAD_STATE);   /* one grab at a time */
    wm_grab_cancel();
    CHECK(!wm_grabbing());
    return true;
}

bool t_wm_floating_resize(void)
{
    start(COMP_FLOATING);
    bool ok = floating_resize_steps();
    fk_close_all();
    return ok;
}

/* ---- maximised and full screen ------------------------------------------------------ */

static bool double_click_title(int32_t x, int32_t y, uint64_t t)
{
    CHECK(wm_pointer_press(x, y, WM_BTN_LEFT, t));
    wm_pointer_release(x, y);
    CHECK(wm_pointer_press(x, y, WM_BTN_LEFT, t + WM_DOUBLE_CLICK_NS / 2));
    wm_pointer_release(x, y);
    return true;
}

static bool states_steps(void)
{
    uint32_t a = 0;
    CHECK(fk_open(&fks[0], &a, 320, 200, false));
    CHECK(double_click_title(600, 290, 10 * NS_PER_S));
    CFG(0, OUT_W, OUT_H - DECO_TITLE_H, WM_ST_MAXIMIZED);
    AT(0, 480, 300);                     /* until it draws for it */
    CHECK(fk_draw(&fks[0]));
    AT(0, 0, DECO_TITLE_H);
    CHECK(win(0)->flags & COMP_WIN_MAXIMIZED);
    CHECK_EQ(win(0)->deco_left, 0);
    CHECK_ST(wm_begin_move(win(0), 0, 0), ERR_BAD_STATE);
    CHECK(double_click_title(600, 10, 20 * NS_PER_S));
    CFG(0, 320, 200, 0);
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    CHECK(!(win(0)->flags & COMP_WIN_MAXIMIZED));
    /* the full-screen key, on and off */
    wm_toggle_fullscreen(win(0));
    CFG(0, OUT_W, OUT_H, WM_ST_FULLSCREEN);
    CHECK(fk_draw(&fks[0]));
    AT(0, 0, 0);
    CHECK(box_empty(deco_title_bar(win(0))));
    CHECK(win(0)->flags & COMP_WIN_FULLSCREEN);
    wm_toggle_fullscreen(win(0));
    CFG(0, 320, 200, 0);
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    /* the client's requests: full screen returns to maximised */
    wm_request_maximized(fks[0].ww, true);
    CHECK(fk_draw(&fks[0]));
    wm_request_fullscreen(fks[0].ww, true);
    CFG(0, OUT_W, OUT_H, WM_ST_FULLSCREEN);
    wm_request_fullscreen(fks[0].ww, false);
    CFG(0, OUT_W, OUT_H - DECO_TITLE_H, WM_ST_MAXIMIZED);
    wm_request_maximized(fks[0].ww, false);
    CFG(0, 320, 200, 0);
    return true;
}

static bool fixed_maximised_steps(void)
{
    uint32_t b = 0;
    /* a window that can't resize, maximised: centred below the title bar */
    CHECK(fk_open(&fks[1], &b, 320, 200, true));
    wm_request_maximized(fks[1].ww, true);
    CHECK(fk_draw(&fks[1]));
    AT(1, 480, DECO_TITLE_H + (OUT_H - DECO_TITLE_H - 200) / 2);
    return true;
}

bool t_wm_states(void)
{
    start(COMP_FLOATING);
    bool ok = states_steps() && fixed_maximised_steps();
    fk_close_all();
    return ok;
}

/* ---- tiling ------------------------------------------------------------------------ */

/* The surface box tile i of n gives a resizable window. */
static struct comp_box inner(unsigned n, unsigned i)
{
    return deco_inner(wm_tile_box((struct comp_box){ 0, 0, OUT_W, OUT_H }, n, i), 0);
}

static bool fills(unsigned f, unsigned n, unsigned i)
{
    struct comp_box b = inner(n, i);
    CFG(f, b.x2 - b.x1, b.y2 - b.y1, fks[f].cfg.states & WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[f]));
    AT(f, b.x1, b.y1);
    return true;
}

/* Every tile of n inside the output, apart, and at least the gap apart. */
static bool tiles_sane(unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        struct comp_box b = wm_tile_box((struct comp_box){ 0, 0, OUT_W, OUT_H }, n, i);
        CHECK(b.x1 >= 4 && b.y1 >= 4 && b.x2 <= OUT_W - 4 && b.y2 <= OUT_H - 4);
        CHECK(!box_empty(b));
        for (unsigned j = 0; j < i; j++) {
            struct comp_box c = wm_tile_box((struct comp_box){ 0, 0, OUT_W, OUT_H }, n, j);
            CHECK(box_empty(box_intersect(b, c)));
        }
    }
    return true;
}

static bool tiling_steps(void)
{
    uint32_t a = 0, b = 0, c = 0;
    for (unsigned n = 1; n <= 8; n++)
        CHECK(tiles_sane(n));
    /* one window: all of the output, less the gap and its decorations */
    CHECK(fk_open(&fks[0], &a, 320, 200, false));
    CFG(0, 1264, 764, 0);
    CHECK(fk_draw(&fks[0]));
    AT(0, 8, 28);
    /* a fixed one joins: the master shrinks to the left half, the new one
     * sits centred in the right half at its own size */
    CHECK(fk_open(&fks[1], &b, 320, 200, true));
    CFG(1, 0, 0, 0);
    CHECK(fills(0, 2, 0));
    CHECK_EQ(inner(2, 0).x2, 634);
    AT(1, 646 + (626 - 320) / 2, 28 + (764 - 200) / 2);
    /* a third: the stack splits */
    CHECK(fk_open(&fks[2], &c, 320, 200, false));
    CHECK(fills(2, 3, 2));
    struct comp_box s1 = inner(3, 1);
    AT(1, s1.x1 + (s1.x2 - s1.x1 - 320) / 2, s1.y1 + (s1.y2 - s1.y1 - 200) / 2);
    /* no moving or resizing in tiling; a double-click still maximises */
    struct comp_box t = deco_title_bar(win(2));
    CHECK(wm_pointer_press(t.x1 + 50, t.y2 - 5, WM_BTN_LEFT, 1 * NS_PER_S));
    wm_pointer_motion(t.x1 - 300, t.y2 + 100);
    wm_pointer_release(t.x1 - 300, t.y2 + 100);
    CHECK(fills(2, 3, 2));
    CHECK_ST(wm_begin_move(win(2), 0, 0), ERR_BAD_STATE);
    CHECK_ST(wm_begin_resize(win(2), WM_EDGE_RIGHT, 0, 0), ERR_BAD_STATE);
    CHECK(double_click_title(t.x1 + 50, t.y2 - 5, 3 * NS_PER_S));
    CFG(2, OUT_W, OUT_H - DECO_TITLE_H, WM_ST_MAXIMIZED);
    wm_request_maximized(fks[2].ww, false);
    CHECK(fills(2, 3, 2));
    /* the master leaves: the fixed one is the master now */
    wm_destroy(fks[0].ww);
    fks[0].ww = NULL;
    struct comp_box m = inner(2, 0);
    AT(1, m.x1 + (m.x2 - m.x1 - 320) / 2, m.y1 + (m.y2 - m.y1 - 200) / 2);
    CHECK(fills(2, 2, 1));
    return true;
}

static bool tiling_big_fixed(void)
{
    uint32_t d = 0;
    /* a fixed window bigger than its tile: centred on it, wholly on the screen */
    CHECK(fk_open(&fks[3], &d, 700, 500, true));
    struct comp_box f = window_frame(win(3));
    CHECK(f.x1 >= 0 && f.y1 >= 0 && f.x2 <= OUT_W && f.y2 <= OUT_H);
    return true;
}

bool t_wm_tiling(void)
{
    start(COMP_TILING);
    bool ok = tiling_steps() && tiling_big_fixed();
    fk_close_all();
    return ok;
}

/* ---- the switch ------------------------------------------------------------------- */

struct place {
    int32_t x, y, w, h;
};

static struct place place_of(unsigned i)
{
    return (struct place){ win(i)->x, win(i)->y, fks[i].s.width, fks[i].s.height };
}

static bool same_place(unsigned i, struct place p)
{
    CHECK(fk_draw(&fks[i]));
    CHECK_EQ(win(i)->x, p.x);
    CHECK_EQ(win(i)->y, p.y);
    CHECK_EQ(fks[i].s.width, p.w);
    CHECK_EQ(fks[i].s.height, p.h);
    return true;
}

static bool switch_steps(void)
{
    uint32_t a = 0, b = 0, c = 0;
    CHECK(fk_open(&fks[0], &a, 320, 200, false));
    CHECK(fk_open(&fks[1], &b, 200, 150, true));
    CHECK(fk_open(&fks[2], &c, 400, 300, false));
    wm_focus_changed(win(0));   /* raised: its title bar is under the third's otherwise */
    CHECK(drag(win(0)->x + 50, win(0)->y - 10, -300, -200, 1 * NS_PER_S));
    struct comp_box s = window_surface_box(win(2));
    CHECK(drag(s.x2 + 1, s.y2 + 1, 100, 50, 3 * NS_PER_S));
    CHECK(fk_draw(&fks[2]));
    struct place p[3] = { place_of(0), place_of(1), place_of(2) };
    CHECK_EQ(p[2].w, 500);
    /* the key: everything tiles, and init is told to keep it */
    wm_toggle_layout();
    CHECK_EQ(scene.layout, COMP_TILING);
    CHECK_EQ(heard.nlayout, 1);
    CHECK_EQ(heard.layout, COMP_TILING);
    CHECK(fills(0, 3, 0));
    CHECK(fills(2, 3, 2));
    CFG(1, 0, 0, 0);
    CHECK(fk_draw(&fks[1]));
    struct comp_box i1 = inner(3, 1);
    AT(1, i1.x1 + (i1.x2 - i1.x1 - 200) / 2, i1.y1 + (i1.y2 - i1.y1 - 150) / 2);
    /* and back: every window where and as big as it was */
    wm_toggle_layout();
    CHECK_EQ(heard.layout, COMP_FLOATING);
    for (unsigned i = 0; i < 3; i++)
        CHECK(same_place(i, p[i]));
    /* a window first opened while tiling gets a floating place, on the screen */
    wm_toggle_layout();
    uint32_t d = 0;
    CHECK(fk_open(&fks[3], &d, 300, 300, false));
    wm_toggle_layout();
    CHECK(fk_draw(&fks[3]));
    struct comp_box f = window_frame(win(3));
    CHECK(f.x1 >= 0 && f.y1 >= 0 && f.x2 <= OUT_W && f.y2 <= OUT_H);
    return true;
}

bool t_wm_switch(void)
{
    start(COMP_FLOATING);
    bool ok = switch_steps();
    fk_close_all();
    return ok;
}

/* ---- focus --------------------------------------------------------------------------- */

static bool focus_steps(void)
{
    uint32_t a = 0, b = 0;
    CHECK(fk_open(&fks[0], &a, 320, 200, false));
    CHECK(fk_open(&fks[1], &a, 320, 200, false));
    CHECK(fk_open(&fks[2], &b, 320, 200, false));
    /* focus raises and activates; the one before is told it isn't */
    wm_focus_changed(win(0));
    CHECK_EQ(scene.top, win(0));
    CFG(0, 320, 200, WM_ST_ACTIVATED);
    wm_focus_changed(win(1));
    CHECK_EQ(scene.top, win(1));
    CFG(0, 320, 200, 0);
    CFG(1, 320, 200, WM_ST_ACTIVATED);
    /* Alt+Tab: the order they opened, both ways, round */
    CHECK_EQ(wm_cycle(NULL, false), win(0));
    CHECK_EQ(wm_cycle(win(0), false), win(1));
    CHECK_EQ(wm_cycle(win(2), false), win(0));
    CHECK_EQ(wm_cycle(win(0), true), win(2));
    CHECK_EQ(wm_cycle(NULL, true), win(2));
    /* when the top one goes, the next one down gets the keys */
    wm_focus_changed(win(2));
    CHECK_EQ(wm_focus_successor(win(2)), win(1));
    /* an unmapped window is skipped, and the seat was told first */
    struct comp_window *w1 = win(1);
    wm_unmap(fks[1].ww);
    CHECK_EQ(heard.unmapping, w1);
    CHECK_EQ(wm_cycle(win(0), false), win(2));
    CHECK_EQ(wm_focus_successor(win(2)), win(0));
    /* alone: nothing to cycle to */
    wm_unmap(fks[0].ww);
    CHECK_EQ(wm_cycle(win(2), false), NULL);
    CHECK_EQ(wm_focus_successor(win(2)), NULL);
    return true;
}

bool t_wm_focus(void)
{
    start(COMP_FLOATING);
    bool ok = focus_steps();
    fk_close_all();
    return ok;
}

/* ---- what is under the pointer --------------------------------------------------------- */

static bool window_at_steps(void)
{
    uint32_t a = 0;
    bool on;
    CHECK(fk_open(&fks[0], &a, 320, 200, false));   /* at 480, 300 */
    CHECK(fk_open(&fks[1], &a, 320, 200, false));   /* at 508, 328: over it */
    CHECK_EQ(wm_window_at(600, 400, &on), win(1));
    CHECK(on);
    /* the top window's title bar hides the surface under it */
    CHECK_EQ(wm_window_at(600, 310, &on), win(1));
    CHECK(!on);
    CHECK_EQ(wm_window_at(490, 310, &on), win(0));   /* left of it: the lower one */
    CHECK(on);
    CHECK_EQ(wm_window_at(5, 5, &on), NULL);
    /* an empty input region lets the pointer through to the one below */
    fks[1].s.input_all = false;
    CHECK_EQ(wm_window_at(600, 400, &on), win(0));
    CHECK(on);
    fks[1].s.input_all = true;
    /* what each part of a frame is */
    uint32_t e;
    CHECK_EQ(deco_hit(win(1), 600, 310, &e), DECO_TITLE);
    CHECK_EQ(deco_hit(win(1), 600, 400, &e), DECO_SURFACE);
    CHECK_EQ(deco_hit(win(1), 600, 305, &e), DECO_EDGE);   /* the title bar's top rows */
    CHECK_EQ(e, WM_EDGE_TOP);
    CHECK_EQ(deco_hit(win(1), 506, 315, &e), DECO_EDGE);   /* near the top-left corner */
    CHECK_EQ(e, WM_EDGE_LEFT | WM_EDGE_TOP);
    CHECK_EQ(deco_hit(win(1), 829, 400, &e), DECO_EDGE);
    CHECK_EQ(e, WM_EDGE_RIGHT);
    CHECK_EQ(deco_hit(win(1), 820, 315, &e), DECO_CLOSE);
    CHECK_EQ(deco_hit(win(1), 100, 100, &e), DECO_NONE);
    return true;
}

bool t_wm_window_at(void)
{
    start(COMP_FLOATING);
    bool ok = window_at_steps();
    fk_close_all();
    return ok;
}

/* ---- the layout's setting -------------------------------------------------------------- */

#define WF "/w/etc/settings"

static void save_layout(enum comp_layout l)
{
    on_layout(l);
    (void)settings_set(WF, WM_LAYOUT_SETTING, wm_layout_name(l));   /* read back below */
}

/* As init and the compositor do it across a reboot: the switch saved,
 * then a fresh window manager started with what was saved. */
static bool layout_setting_steps(void)
{
    enum comp_layout l = COMP_TILING;
    CHECK(wm_layout_parse("floating", &l) && l == COMP_FLOATING);
    CHECK(wm_layout_parse("tiling", &l) && l == COMP_TILING);
    CHECK(!wm_layout_parse("tile", &l) && !wm_layout_parse("", &l));
    CHECK(!strcmp(wm_layout_name(COMP_FLOATING), "floating"));
    comp_wm_hooks.layout_changed = save_layout;
    wm_toggle_layout();
    char v[SETTINGS_VALUE_MAX];
    CHECK_ST(settings_get(WF, WM_LAYOUT_SETTING, v, sizeof(v)), OK);
    CHECK(!strcmp(v, "tiling"));
    start(COMP_FLOATING);   /* the next boot */
    CHECK_ST(settings_get(WF, WM_LAYOUT_SETTING, v, sizeof(v)), OK);
    CHECK(wm_layout_parse(v, &l));
    wm_set_layout(l);
    uint32_t a = 0;
    CHECK(fk_open(&fks[0], &a, 320, 200, false));
    CHECK(fills(0, 1, 0));
    comp_wm_hooks.layout_changed = save_layout;
    wm_toggle_layout();
    CHECK_ST(settings_get(WF, WM_LAYOUT_SETTING, v, sizeof(v)), OK);
    CHECK(!strcmp(v, "floating"));
    return true;
}

bool t_wm_layout_setting(void)
{
    static struct ramdisk disk;
    struct fatrun r;
    handle_t fs;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK_ST(jam_handle_duplicate(r.fs, RIGHT_SAME, &fs), OK);
    CHECK_ST(ns_mount("/w", fs), OK);
    start(COMP_FLOATING);
    bool ok = layout_setting_steps();
    fk_close_all();
    CHECK_ST(ns_unmount("/w"), OK);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}
