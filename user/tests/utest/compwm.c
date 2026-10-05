/* utest: the compositor's window manager (user/services/compositor wm.c,
 * wmtile.c, wmgrab.c, deco.c, on scene.c and region.c), linked in and
 * driven directly. Fake toplevels play the client (they draw whatever
 * size each configure asks, or their own when it says 0 or they can't
 * resize), and this file plays the seat: it defines the seat's calls the
 * window manager makes (seat_focus, seat_grab_begin, ...) and makes the
 * seat's calls into it (wm_press, the grab's motion and end, wm_cycle,
 * the keys' toggles), as pointer.c and focus.c do. The protocol side
 * (xdg-shell over real channels) is compxdg.c's; the real seat's, the
 * comp_seat_* tests'.
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
#define BTN   0x110u   /* evdev's left button */
#define WF    "/w/etc/settings"

/* ---- the seat, played here --------------------------------------------------------- */

struct comp_cursor cursor;
static struct jwl_conn fake_conn;   /* a live connection: status OK */
static struct comp_client fake_client = { .conn = &fake_conn };

static struct {
    const struct comp_grab_ops *ops;   /* the grab the window manager began */
    void *data;
    bool held;                         /* a button is down */
    struct comp_window *focused;
    unsigned nmapped, ngone, nlayout;
    struct comp_window *gone;          /* the last window gone */
    enum comp_layout layout;           /* the last switch reported */
    bool save;                         /* reported switches go to WF, as init would write them */
} seat;

status_t seat_grab_begin(const struct comp_grab_ops *ops, void *data)
{
    if (!seat.held || seat.ops)
        return ERR_BAD_STATE;
    seat.ops = ops;
    seat.data = data;
    return OK;
}

void seat_grab_cancel(void)
{
    seat.ops = NULL;
    seat.data = NULL;
}

void seat_focus(struct comp_window *w)
{
    if (w == seat.focused)
        return;
    if (seat.focused)
        seat.focused->flags &= ~COMP_WIN_FOCUSED;
    seat.focused = w;
    if (w)
        w->flags |= COMP_WIN_FOCUSED;
    wm_focus_changed(w);
}

void seat_window_mapped(struct comp_window *w)
{
    (void)w;
    seat.nmapped++;
}

void seat_window_gone(struct comp_window *w)
{
    seat.ngone++;
    seat.gone = w;
    if (w == seat.focused) {
        w->flags &= ~COMP_WIN_FOCUSED;
        seat.focused = NULL;
    }
}

void ctl_layout_changed(enum comp_layout layout)
{
    seat.layout = layout;
    seat.nlayout++;
    if (seat.save)
        (void)settings_set(WF, WM_LAYOUT_SETTING, wm_layout_name(layout));   /* read back */
}

/* A press at (x, y) with button: did the window manager take it? */
static bool press_btn(int32_t x, int32_t y, uint32_t button)
{
    cursor.x = x;
    cursor.y = y;
    seat.held = true;
    return wm_press(x, y, button);
}

static void move_to(int32_t x, int32_t y)
{
    cursor.x = x;
    cursor.y = y;
    if (seat.ops)
        seat.ops->motion(seat.data, x, y);
}

/* The pointer to (x, y), and the button up: a grab ends. */
static void release_at(int32_t x, int32_t y)
{
    move_to(x, y);
    seat.held = false;
    const struct comp_grab_ops *ops = seat.ops;
    seat.ops = NULL;
    if (ops)
        ops->end(seat.data);
}

/* A whole drag: press at (x, y), move by (dx, dy), release there. */
static bool drag(int32_t x, int32_t y, int32_t dx, int32_t dy)
{
    CHECK(press_btn(x, y, BTN));
    CHECK(seat.ops);
    move_to(x + dx, y + dy);
    release_at(x + dx, y + dy);
    CHECK(!seat.ops);
    return true;
}

static bool double_click_title(int32_t x, int32_t y)
{
    CHECK(press_btn(x, y, BTN));
    release_at(x, y);
    CHECK(press_btn(x, y, BTN));
    release_at(x, y);
    return true;
}

/* ---- fake toplevels --------------------------------------------------------------------- */

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

static void start(enum comp_layout layout)
{
    scene_init(OUT_W, OUT_H, 0);
    wm_init(layout);
    memset(fks, 0, sizeof(fks));
    memset(&seat, 0, sizeof(seat));
}

/* The client answers its last configure: draws, commits. */
static bool fk_draw(struct fk *f)
{
    f->s.width = f->cfg.width && !f->fixed ? f->cfg.width : f->own_w;
    f->s.height = f->cfg.height && !f->fixed ? f->cfg.height : f->own_h;
    CHECK_ST(wm_commit(f->ww, f->cfg.states), OK);
    return true;
}

/* A toplevel w by h of its own: the initial configure, then its first
 * buffer. */
static bool fk_open(struct fk *f, int32_t w, int32_t h, bool fixed)
{
    memset(f, 0, sizeof(*f));
    f->s.client = &fake_client;
    f->s.input_all = true;
    f->own_w = w;
    f->own_h = h;
    f->fixed = fixed;
    CHECK((f->ww = wm_create(&f->s, &fk_ops, f)) != NULL);
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

/* ---- floating ---------------------------------------------------------------------- */

static bool floating_place_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    CFG(0, 0, 0, 0);                     /* the initial configure: its own size */
    AT(0, 480, 300);                     /* centred */
    CHECK(fk_open(&fks[1], 320, 200, false));
    AT(1, 508, 328);                     /* one cascade step: 24 + 4 */
    CHECK(fk_open(&fks[2], 320, 200, false));
    AT(2, 536, 356);
    CHECK_EQ(seat.nmapped, 3);           /* the scene told the seat of each */
    CHECK_EQ(scene.top, win(2));
    /* the first one's corner is free again */
    struct comp_window *w0 = win(0);
    wm_destroy(fks[0].ww);
    fks[0].ww = NULL;
    CHECK_EQ(seat.gone, w0);
    CHECK(fk_open(&fks[3], 320, 200, false));
    AT(3, 480, 300);
    /* bigger than the output: its frame's corner at the output's */
    CHECK(fk_open(&fks[4], 1400, 900, false));
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

static bool close_box_steps(void)
{
    /* the close box: closes on a release on it, not elsewhere */
    struct comp_box c = deco_close_box(win(0));
    CHECK(!box_empty(c) && c.x2 == 580 + 320 && c.y2 == DECO_TITLE_H);
    int32_t cx = (c.x1 + c.x2) / 2, cy = (c.y1 + c.y2) / 2;
    CHECK(press_btn(cx, cy, BTN));
    release_at(cx, cy);
    CHECK_EQ(fks[0].closes, 1);
    CHECK(press_btn(cx, cy, BTN));
    release_at(cx - 100, cy + 100);
    CHECK_EQ(fks[0].closes, 1);
    AT(0, 580, DECO_TITLE_H);            /* a close box press never moves it */
    return true;
}

static bool floating_move_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    AT(0, 480, 300);
    /* the title bar is y 276..299: drag it */
    CHECK(drag(600, 290, 100, 50));
    AT(0, 580, 350);
    CHECK_EQ(seat.focused, win(0));      /* a press on decorations focuses */
    /* never off the top: the title bar stays reachable */
    CHECK(drag(700, 340, 0, -1000));
    AT(0, 580, DECO_TITLE_H);
    /* the surface is the client's, the background nobody's */
    CHECK(!press_btn(700, 100, BTN));
    release_at(700, 100);
    CHECK(!press_btn(10, 790, BTN));
    release_at(10, 790);
    /* another button on the title bar: taken, does nothing */
    CHECK(press_btn(700, 10, BTN + 1));
    CHECK(!seat.ops);
    release_at(800, 300);
    AT(0, 580, DECO_TITLE_H);
    return close_box_steps();
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
    CHECK(press_btn(801, 400, BTN));
    CFG(0, 320, 200, WM_ST_ACTIVATED | WM_ST_RESIZING);
    move_to(901, 400);
    CFG(0, 420, 200, WM_ST_ACTIVATED | WM_ST_RESIZING);
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    release_at(901, 400);
    CFG(0, 420, 200, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    /* the left border (x 476..479): the right side (900) stays put */
    CHECK(drag(478, 400, -100, 0));
    CFG(0, 520, 200, WM_ST_ACTIVATED);
    AT(0, 480, 300);                     /* not drawn yet: where it was */
    CHECK(fk_draw(&fks[0]));
    AT(0, 380, 300);
    CHECK_EQ(win(0)->x + fks[0].s.width, 900);
    return true;
}

static bool resize_limits(void)
{
    /* a corner: bottom right takes both */
    struct comp_box s = window_surface_box(win(0));
    CHECK(drag(s.x2 + 1, s.y2 + 1, 10, 20));
    CFG(0, 530, 220, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    /* the client's limits, and never under WM_MIN_SIDE */
    wm_set_limits(fks[0].ww, 0, 0, 600, 0);
    s = window_surface_box(win(0));
    CHECK(drag(s.x2 + 1, (s.y1 + s.y2) / 2, 500, 0));
    CFG(0, 600, 220, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    s = window_surface_box(win(0));
    CHECK(drag((s.x1 + s.x2) / 2, s.y2 + 1, 0, -1000));
    CFG(0, 600, WM_MIN_SIDE, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    return true;
}

static bool floating_resize_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK(resize_right_and_left() && resize_limits());
    /* a window that can't resize: its border is taken and does nothing */
    CHECK(fk_open(&fks[1], 200, 100, true));
    struct comp_box s = window_surface_box(win(1));
    unsigned n = fks[1].nconf;
    CHECK(press_btn(s.x2 + 1, (s.y1 + s.y2) / 2, BTN));
    CHECK(!seat.ops);
    release_at(s.x2 + 51, (s.y1 + s.y2) / 2);
    CHECK_EQ(fks[1].nconf, n + 1);       /* only its focus: activated */
    CHECK_ST(wm_begin_resize(win(1), WM_EDGE_RIGHT, 0, 0), ERR_BAD_STATE);
    CHECK_ST(wm_begin_resize(win(0), WM_EDGE_LEFT | WM_EDGE_RIGHT, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(wm_begin_resize(win(0), 0, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(wm_begin_resize(win(0), 16, 0, 0), ERR_INVALID_ARGS);
    /* a client's own resize (xdg_toplevel.resize, after the seat's check):
     * only while a button is held, one grab at a time */
    CHECK_ST(wm_begin_resize(win(0), WM_EDGE_BOTTOM, 0, 0), ERR_BAD_STATE);
    seat.held = true;
    CHECK_ST(wm_begin_resize(win(0), WM_EDGE_BOTTOM, 0, 0), OK);
    CHECK_ST(wm_begin_move(win(0), 0, 0), ERR_BAD_STATE);
    /* the window goes mid-grab: the seat's grab is dropped */
    wm_destroy(fks[0].ww);
    fks[0].ww = NULL;
    CHECK(!seat.ops);
    seat.held = false;
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

static bool maximise_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK(double_click_title(600, 290));
    CFG(0, OUT_W, OUT_H - DECO_TITLE_H, WM_ST_ACTIVATED | WM_ST_MAXIMIZED);
    AT(0, 480, 300);                     /* until it draws for it */
    CHECK(fk_draw(&fks[0]));
    AT(0, 0, DECO_TITLE_H);
    CHECK(win(0)->flags & COMP_WIN_MAXIMIZED);
    CHECK_EQ(win(0)->deco_left, 0);
    seat.held = true;
    CHECK_ST(wm_begin_move(win(0), 0, 0), ERR_BAD_STATE);
    seat.held = false;
    CHECK(double_click_title(600, 10));
    CFG(0, 320, 200, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    CHECK(!(win(0)->flags & COMP_WIN_MAXIMIZED));
    /* a drag between two presses: no double-click */
    CHECK(drag(600, 290, 0, 30));
    CHECK(drag(600, 320, 0, -30));
    CFG(0, 320, 200, WM_ST_ACTIVATED);
    return true;
}

static bool fullscreen_steps(void)
{
    /* the full-screen key, on and off */
    wm_toggle_fullscreen(win(0));
    CFG(0, OUT_W, OUT_H, WM_ST_ACTIVATED | WM_ST_FULLSCREEN);
    CHECK(fk_draw(&fks[0]));
    AT(0, 0, 0);
    CHECK(box_empty(deco_title_bar(win(0))));
    CHECK(win(0)->flags & COMP_WIN_FULLSCREEN);
    wm_toggle_fullscreen(win(0));
    CFG(0, 320, 200, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    /* the client's requests: full screen returns to maximised */
    wm_request_maximized(fks[0].ww, true);
    CHECK(fk_draw(&fks[0]));
    wm_request_fullscreen(fks[0].ww, true);
    CFG(0, OUT_W, OUT_H, WM_ST_ACTIVATED | WM_ST_FULLSCREEN);
    wm_request_fullscreen(fks[0].ww, false);
    CFG(0, OUT_W, OUT_H - DECO_TITLE_H, WM_ST_ACTIVATED | WM_ST_MAXIMIZED);
    wm_request_maximized(fks[0].ww, false);
    CFG(0, 320, 200, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    /* a window that can't resize, maximised: centred below the title bar */
    CHECK(fk_open(&fks[1], 320, 200, true));
    wm_request_maximized(fks[1].ww, true);
    CHECK(fk_draw(&fks[1]));
    AT(1, 480, DECO_TITLE_H + (OUT_H - DECO_TITLE_H - 200) / 2);
    return true;
}

bool t_wm_states(void)
{
    start(COMP_FLOATING);
    bool ok = maximise_steps() && fullscreen_steps();
    fk_close_all();
    return ok;
}

/* ---- tiling ------------------------------------------------------------------------ */

/* The surface box tile i of n gives a resizable window. */
static struct comp_box inner(unsigned n, unsigned i)
{
    return deco_inner(wm_tile_box((struct comp_box){ 0, 0, OUT_W, OUT_H }, n, i), 0);
}

/* f was asked to fill tile i of n, draws, and sits in it. */
static bool fills(unsigned f, unsigned n, unsigned i)
{
    struct comp_box b = inner(n, i);
    CFG(f, b.x2 - b.x1, b.y2 - b.y1, fks[f].cfg.states & WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[f]));
    AT(f, b.x1, b.y1);
    return true;
}

/* Every tile of n inside the output and apart. */
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

static bool tiling_join_steps(void)
{
    for (unsigned n = 1; n <= 8; n++)
        CHECK(tiles_sane(n));
    /* one window: all of the output, less the gap and its decorations */
    CHECK(fk_open(&fks[0], 320, 200, false));
    CFG(0, 1264, 764, 0);
    CHECK(fk_draw(&fks[0]));
    AT(0, 8, 28);
    /* a fixed one joins: the master shrinks to the left half, the new one
     * sits centred in the right half at its own size */
    CHECK(fk_open(&fks[1], 320, 200, true));
    CFG(1, 0, 0, 0);
    CHECK(fills(0, 2, 0));
    CHECK_EQ(inner(2, 0).x2, 634);
    AT(1, 646 + (626 - 320) / 2, 28 + (764 - 200) / 2);
    /* a third: the stack splits */
    CHECK(fk_open(&fks[2], 320, 200, false));
    CHECK(fills(2, 3, 2));
    struct comp_box s1 = inner(3, 1);
    AT(1, s1.x1 + (s1.x2 - s1.x1 - 320) / 2, s1.y1 + (s1.y2 - s1.y1 - 200) / 2);
    return true;
}

static bool tiling_rules_steps(void)
{
    /* no moving or resizing in tiling; a double-click still maximises */
    struct comp_box t = deco_title_bar(win(2));
    CHECK(drag(t.x1 + 50, t.y2 - 5, -300, 100));
    CHECK(fills(2, 3, 2));
    seat.held = true;
    CHECK_ST(wm_begin_move(win(2), 0, 0), ERR_BAD_STATE);
    CHECK_ST(wm_begin_resize(win(2), WM_EDGE_RIGHT, 0, 0), ERR_BAD_STATE);
    seat.held = false;
    CHECK(double_click_title(t.x1 + 50, t.y2 - 5));
    CFG(2, OUT_W, OUT_H - DECO_TITLE_H, WM_ST_ACTIVATED | WM_ST_MAXIMIZED);
    wm_request_maximized(fks[2].ww, false);
    CHECK(fills(2, 3, 2));
    /* the master leaves: the fixed one is the master now */
    wm_destroy(fks[0].ww);
    fks[0].ww = NULL;
    struct comp_box m = inner(2, 0);
    AT(1, m.x1 + (m.x2 - m.x1 - 320) / 2, m.y1 + (m.y2 - m.y1 - 200) / 2);
    CHECK(fills(2, 2, 1));
    /* a fixed window bigger than its tile: centred on it, wholly on the screen */
    CHECK(fk_open(&fks[3], 700, 500, true));
    struct comp_box f = window_frame(win(3));
    CHECK(f.x1 >= 0 && f.y1 >= 0 && f.x2 <= OUT_W && f.y2 <= OUT_H);
    return true;
}

bool t_wm_tiling(void)
{
    start(COMP_TILING);
    bool ok = tiling_join_steps() && tiling_rules_steps();
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
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK(fk_open(&fks[1], 200, 150, true));
    CHECK(fk_open(&fks[2], 400, 300, false));
    seat_focus(win(0));   /* raised: its title bar is under the third's otherwise */
    CHECK(drag(win(0)->x + 50, win(0)->y - 10, -300, -200));
    struct comp_box s = window_surface_box(win(2));
    CHECK(drag(s.x2 + 1, s.y2 + 1, 100, 50));
    CHECK(fk_draw(&fks[2]));
    struct place p[3] = { place_of(0), place_of(1), place_of(2) };
    CHECK_EQ(p[2].w, 500);
    /* the key: everything tiles, and init is told to keep it */
    wm_toggle_layout();
    CHECK_EQ(scene.layout, COMP_TILING);
    CHECK_EQ(seat.nlayout, 1);
    CHECK_EQ(seat.layout, COMP_TILING);
    CHECK(fills(0, 3, 0));
    CHECK(fills(2, 3, 2));
    CFG(1, 0, 0, 0);
    CHECK(fk_draw(&fks[1]));
    struct comp_box i1 = inner(3, 1);
    AT(1, i1.x1 + (i1.x2 - i1.x1 - 200) / 2, i1.y1 + (i1.y2 - i1.y1 - 150) / 2);
    /* and back: every window where and as big as it was */
    wm_toggle_layout();
    CHECK_EQ(seat.layout, COMP_FLOATING);
    for (unsigned i = 0; i < 3; i++)
        CHECK(same_place(i, p[i]));
    /* a window first opened while tiling gets a floating place, on the screen */
    wm_toggle_layout();
    CHECK(fk_open(&fks[3], 300, 300, false));
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

/* A window that is no toplevel (testwin's): the seat's alone. */
static struct comp_surface plain;

static bool cycle_steps(struct comp_window *tw)
{
    /* Alt+Tab: the order toplevels opened, then the others, both ways, round */
    CHECK_EQ(wm_cycle(NULL, false), win(0));
    CHECK_EQ(wm_cycle(win(0), false), win(1));
    CHECK_EQ(wm_cycle(win(2), false), tw);
    CHECK_EQ(wm_cycle(tw, false), win(0));
    CHECK_EQ(wm_cycle(win(0), true), tw);
    CHECK_EQ(wm_cycle(NULL, true), tw);
    /* the plain window: focused and raised; no press, full screen or click
     * of the window manager's touches it */
    seat_focus(tw);
    CHECK_EQ(scene.top, tw);
    CFG(1, 320, 200, 0);
    CHECK(!press_btn(tw->x + 5, tw->y + 5, BTN));
    release_at(tw->x + 5, tw->y + 5);
    wm_toggle_fullscreen(tw);
    wm_clicked(tw);
    CHECK(box_empty(deco_title_bar(tw)) && !strcmp(window_title(tw), ""));
    /* an unmapped toplevel is skipped */
    wm_unmap(fks[1].ww);
    CHECK_EQ(wm_cycle(win(0), false), win(2));
    return true;
}

static bool focus_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK(fk_open(&fks[1], 320, 200, false));
    CHECK(fk_open(&fks[2], 320, 200, false));
    /* focus raises and activates; the one before is told it isn't */
    seat_focus(win(0));
    CHECK_EQ(scene.top, win(0));
    CFG(0, 320, 200, WM_ST_ACTIVATED);
    seat_focus(win(1));
    CHECK_EQ(scene.top, win(1));
    CFG(0, 320, 200, 0);
    CFG(1, 320, 200, WM_ST_ACTIVATED);
    struct comp_window *tw;
    plain = (struct comp_surface){ .client = &fake_client, .width = 50, .height = 50,
                                   .input_all = true };
    CHECK_ST(window_create(&plain, 10, 10, &tw), OK);
    window_map(tw, true);
    bool ok = cycle_steps(tw);
    window_destroy(tw);
    CHECK(ok);
    /* the focused toplevel goes: none is activated, the next one can be */
    seat_focus(win(2));
    wm_destroy(fks[2].ww);
    fks[2].ww = NULL;
    seat_focus(win(0));
    CFG(0, 320, 200, WM_ST_ACTIVATED);
    /* alone: nothing to cycle to */
    CHECK_EQ(wm_cycle(win(0), false), NULL);
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
    bool on;
    CHECK(fk_open(&fks[0], 320, 200, false));   /* at 480, 300 */
    CHECK(fk_open(&fks[1], 320, 200, false));   /* at 508, 328: over it */
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

/* As init and the compositor do it across a reboot: the switch saved,
 * then a fresh window manager started with what was saved. */
static bool layout_setting_steps(void)
{
    enum comp_layout l = COMP_TILING;
    CHECK(wm_layout_parse("floating", &l) && l == COMP_FLOATING);
    CHECK(wm_layout_parse("tiling", &l) && l == COMP_TILING);
    CHECK(!wm_layout_parse("tile", &l) && !wm_layout_parse("", &l));
    CHECK(!strcmp(wm_layout_name(COMP_FLOATING), "floating"));
    seat.save = true;
    wm_toggle_layout();
    char v[SETTINGS_VALUE_MAX];
    CHECK_ST(settings_get(WF, WM_LAYOUT_SETTING, v, sizeof(v)), OK);
    CHECK(!strcmp(v, "tiling"));
    start(COMP_FLOATING);   /* the next boot */
    CHECK_ST(settings_get(WF, WM_LAYOUT_SETTING, v, sizeof(v)), OK);
    CHECK(wm_layout_parse(v, &l));
    wm_set_layout(l);
    CHECK_EQ(seat.nlayout, 0);   /* init's own choice isn't reported back */
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK(fills(0, 1, 0));
    seat.save = true;
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
