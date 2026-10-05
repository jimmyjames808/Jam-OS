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
 * The output is 1280x800 in every test; decorations are COMP_TITLE_H (28)
 * above and DECO_OUTLINE (1) around (tiling: DECO_BORDER, 2, all round),
 * so the numbers below can be checked by hand. The desktop is off here
 * (no strip: the whole output is the windows'); compdesk.c tests it on.
 * The harness (compwm.h) is shared with compdesk.c and compdesk2.c. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <settings.h>
#include "compwm.h"
#include "fattest.h"
#include "utest.h"

#define WF    "/w/etc/settings"

/* ---- the seat, played here --------------------------------------------------------- */

struct comp_cursor cursor;
struct comp comp = { .period_ns = NS_PER_S / 60 };
static struct jwl_conn fake_conn;   /* a live connection: status OK */
struct comp_client fake_client = { .conn = &fake_conn };
struct fake_seat seat;
struct fake_desk fdesk;

/* The painting the desktop's logic calls, played here: snapshots are
 * counted (no pixels), the cursor's damage dropped. */
status_t anim_snapshot(const struct comp_window *w, struct anim_snap *out)
{
    struct comp_box f = window_frame(w);
    fdesk.snaps++;
    *out = (struct anim_snap){ (uint32_t *)(uintptr_t)1, f.x2 - f.x1, f.y2 - f.y1, 0 };
    return OK;
}

void anim_snapshot_free(struct anim_snap *s)
{
    if (s->px)
        fdesk.snaps_freed++;
    *s = (struct anim_snap){ 0 };
}

void cursor_moved(int32_t old_x, int32_t old_y)
{
    (void)old_x;
    (void)old_y;
}

/* The plumbing's hooks, played here. */
void ctl_launch(const char *app)
{
    snprintf(fdesk.launched, sizeof(fdesk.launched), "%s", app);
    fdesk.nlaunch++;
}

void ctl_run_in_terminal(const char *cmd)
{
    snprintf(fdesk.ran, sizeof(fdesk.ran), "%s", cmd);
    fdesk.nrun++;
}

void ctl_notify_answered(uint32_t id, uint32_t button)
{
    fdesk.answered_id = id;
    fdesk.answered_button = button;
    fdesk.nanswered++;
}

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
    if (w == seat.focused) {   /* the real seat moves it on; here to none */
        w->flags &= ~COMP_WIN_FOCUSED;
        seat.focused = NULL;
        wm_focus_changed(NULL);
    }
}

/* Painting's (paint.c), which scene.c asks: nothing is ever covered here. */
bool window_covered(const struct comp_window *w)
{
    (void)w;
    return false;
}

void ctl_layout_changed(enum comp_layout layout)
{
    seat.layout = layout;
    seat.nlayout++;
    if (seat.save)
        (void)settings_set(WF, WM_LAYOUT_SETTING, wm_layout_name(layout));   /* read back */
}

bool press_btn(int32_t x, int32_t y, uint32_t button)
{
    cursor.x = x;
    cursor.y = y;
    seat.held = true;
    return wm_press(x, y, button);
}

void move_to(int32_t x, int32_t y)
{
    cursor.x = x;
    cursor.y = y;
    if (seat.ops)
        seat.ops->motion(seat.data, x, y);
}

void release_at(int32_t x, int32_t y)
{
    move_to(x, y);
    seat.held = false;
    const struct comp_grab_ops *ops = seat.ops;
    seat.ops = NULL;
    if (ops)
        ops->end(seat.data);
}

bool drag(int32_t x, int32_t y, int32_t dx, int32_t dy)
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

struct fk fks[FK_MAX];

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

void wm_test_start(enum comp_layout layout)
{
    desk_init(false, false);
    scene_init(OUT_W, OUT_H, 0);
    wm_init(layout);
    memset(fks, 0, sizeof(fks));
    memset(&seat, 0, sizeof(seat));
    memset(&fdesk, 0, sizeof(fdesk));
}

bool fk_draw(struct fk *f)
{
    f->s.width = f->cfg.width && !f->fixed ? f->cfg.width : f->own_w;
    f->s.height = f->cfg.height && !f->fixed ? f->cfg.height : f->own_h;
    CHECK_ST(wm_commit(f->ww, f->cfg.states), OK);
    return true;
}

bool fk_open(struct fk *f, int32_t w, int32_t h, bool fixed)
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

void fk_close_all(void)
{
    for (unsigned i = 0; i < FK_MAX; i++)
        if (fks[i].ww)
            wm_destroy(fks[i].ww);
    memset(fks, 0, sizeof(fks));
}

struct comp_window *win(unsigned i)
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
    AT(1, 510, 330);                     /* one cascade step: 28 + 2 */
    CHECK(fk_open(&fks[2], 320, 200, false));
    AT(2, 540, 360);
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
    AT(4, DECO_OUTLINE, COMP_TITLE_H);
    /* the size it drew is what it is asked for from now on */
    wm_reconfigure(fks[2].ww);
    CFG(2, 320, 200, 0);
    return true;
}

bool t_wm_floating_place(void)
{
    wm_test_start(COMP_FLOATING);
    bool ok = floating_place_steps();
    fk_close_all();
    return ok;
}

/* A click (press and release) at the middle of circle b of win(0). */
static bool click_button(enum title_button b)
{
    struct comp_box c = title_button_box(win(0), b);
    CHECK(!box_empty(c));
    int32_t cx = (c.x1 + c.x2) / 2, cy = (c.y1 + c.y2) / 2;
    CHECK(press_btn(cx, cy, BTN));
    release_at(cx, cy);
    return true;
}

static bool close_box_steps(void)
{
    /* the close circle, first on the left: closes on a release on it, not
     * elsewhere */
    struct comp_box c = title_close_box(win(0));
    CHECK(!box_empty(c) && c.x1 == 580 - DECO_OUTLINE + LOOK_BTN_LEFT && c.y1 == LOOK_BTN_TOP);
    CHECK_EQ(c.x2 - c.x1, LOOK_BTN_D);
    CHECK(click_button(TITLE_CLOSE));
    CHECK_EQ(fks[0].closes, 1);
    int32_t cx = (c.x1 + c.x2) / 2, cy = (c.y1 + c.y2) / 2;
    CHECK(press_btn(cx, cy, BTN));
    release_at(cx - 100, cy + 100);
    CHECK_EQ(fks[0].closes, 1);
    AT(0, 580, COMP_TITLE_H);            /* a circle's press never moves it */
    /* minimise: hidden (its place kept), the focus gone with it; brought
     * back focused where it was */
    unsigned n = fks[0].nconf;
    CHECK(click_button(TITLE_MINIMISE));
    CHECK(fks[0].ww->minimised && !(win(0)->flags & COMP_WIN_MAPPED));
    CHECK_EQ(seat.focused, NULL);
    CFG(0, 320, 200, 0);                 /* activated no more */
    CHECK_EQ(fks[0].nconf, n + 1);
    CHECK_EQ(fks[0].closes, 1);
    screens_restore(fks[0].ww);
    CHECK(!fks[0].ww->minimised && (win(0)->flags & COMP_WIN_MAPPED));
    CHECK_EQ(seat.focused, win(0));
    AT(0, 580, COMP_TITLE_H);
    /* full screen, and back with Super+F */
    CHECK(click_button(TITLE_FULLSCREEN));
    CFG(0, OUT_W, OUT_H, WM_ST_ACTIVATED | WM_ST_FULLSCREEN);
    wm_toggle_fullscreen(win(0));
    CFG(0, 320, 200, WM_ST_ACTIVATED);
    return true;
}

static bool floating_move_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    AT(0, 480, 300);
    /* the title bar is y 272..299: drag it (right of its circles) */
    CHECK(drag(600, 290, 100, 50));
    AT(0, 580, 350);
    CHECK_EQ(seat.focused, win(0));      /* a press on decorations focuses */
    /* never off the top: the title bar stays reachable */
    CHECK(drag(700, 340, 0, -1000));
    AT(0, 580, COMP_TITLE_H);
    /* the surface is the client's, the background nobody's */
    CHECK(!press_btn(700, 100, BTN));
    release_at(700, 100);
    CHECK(!press_btn(10, 790, BTN));
    release_at(10, 790);
    /* another button on the title bar: taken, does nothing */
    CHECK(press_btn(700, 10, BTN + 1));
    CHECK(!seat.ops);
    release_at(800, 300);
    AT(0, 580, COMP_TITLE_H);
    return close_box_steps();
}

bool t_wm_floating_move(void)
{
    wm_test_start(COMP_FLOATING);
    bool ok = floating_move_steps();
    fk_close_all();
    return ok;
}

static bool resize_right_and_left(void)
{
    /* the right outline is x 800, and DECO_GRAB more outside it */
    CHECK(press_btn(801, 400, BTN));
    CFG(0, 320, 200, WM_ST_ACTIVATED | WM_ST_RESIZING);
    move_to(901, 400);
    CFG(0, 420, 200, WM_ST_ACTIVATED | WM_ST_RESIZING);
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    release_at(901, 400);
    CFG(0, 420, 200, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    /* the left outline (x 479, and outside it): the right side (900) stays put */
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
    /* a window that can't resize: its outline is taken and does nothing */
    CHECK(fk_open(&fks[1], 200, 100, true));
    struct comp_box s = window_surface_box(win(1));
    unsigned n = fks[1].nconf;
    CHECK(press_btn(s.x2, (s.y1 + s.y2) / 2, BTN));
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
    wm_test_start(COMP_FLOATING);
    bool ok = floating_resize_steps();
    fk_close_all();
    return ok;
}

/* ---- maximised and full screen ------------------------------------------------------ */

static bool maximise_steps(void)
{
    /* a double-click on the title bar: full screen (the owner's look) */
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK(double_click_title(600, 290));
    CFG(0, OUT_W, OUT_H, WM_ST_ACTIVATED | WM_ST_FULLSCREEN);
    AT(0, 480, 300);                     /* until it draws for it */
    CHECK(fk_draw(&fks[0]));
    AT(0, 0, 0);
    CHECK(win(0)->flags & COMP_WIN_FULLSCREEN);
    wm_toggle_fullscreen(win(0));
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    /* maximised (the client asks): the title bar only, which doesn't move */
    wm_request_maximized(fks[0].ww, true);
    CFG(0, OUT_W, OUT_H - COMP_TITLE_H, WM_ST_ACTIVATED | WM_ST_MAXIMIZED);
    CHECK(fk_draw(&fks[0]));
    AT(0, 0, COMP_TITLE_H);
    CHECK(win(0)->flags & COMP_WIN_MAXIMIZED);
    CHECK_EQ(win(0)->deco_left, 0);
    seat.held = true;
    CHECK_ST(wm_begin_move(win(0), 0, 0), ERR_BAD_STATE);
    seat.held = false;
    /* a double-click there: full screen, which gives back maximised */
    CHECK(double_click_title(600, 10));
    CFG(0, OUT_W, OUT_H, WM_ST_ACTIVATED | WM_ST_FULLSCREEN);
    CHECK(fk_draw(&fks[0]));
    wm_toggle_fullscreen(win(0));
    CFG(0, OUT_W, OUT_H - COMP_TITLE_H, WM_ST_ACTIVATED | WM_ST_MAXIMIZED);
    CHECK(fk_draw(&fks[0]));
    wm_request_maximized(fks[0].ww, false);
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
    CHECK(box_empty(title_bar_box(win(0))));
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
    CFG(0, OUT_W, OUT_H - COMP_TITLE_H, WM_ST_ACTIVATED | WM_ST_MAXIMIZED);
    wm_request_maximized(fks[0].ww, false);
    CFG(0, 320, 200, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    /* a window that can't resize, maximised: centred below the title bar */
    CHECK(fk_open(&fks[1], 320, 200, true));
    wm_request_maximized(fks[1].ww, true);
    CHECK(fk_draw(&fks[1]));
    AT(1, 480, COMP_TITLE_H + (OUT_H - COMP_TITLE_H - 200) / 2);
    return true;
}

bool t_wm_states(void)
{
    wm_test_start(COMP_FLOATING);
    bool ok = maximise_steps() && fullscreen_steps();
    fk_close_all();
    return ok;
}

/* ---- tiling ------------------------------------------------------------------------ */

/* The surface box tile i of n gives a resizable window. */
static struct comp_box inner(unsigned n, unsigned i)
{
    return deco_inner(wm_tile_box((struct comp_box){ 0, 0, OUT_W, OUT_H }, n, i), 0, COMP_TILING);
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
        CHECK(b.x1 >= 6 && b.y1 >= 6 && b.x2 <= OUT_W - 6 && b.y2 <= OUT_H - 6);
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
    /* one window: all of the output, less the gap (6) and its border (2) */
    CHECK(fk_open(&fks[0], 320, 200, false));
    CFG(0, 1264, 784, 0);
    CHECK(fk_draw(&fks[0]));
    AT(0, 8, 8);
    /* no title bar when tiling: a border all round */
    CHECK_EQ(win(0)->deco_top, DECO_BORDER);
    CHECK(box_empty(title_bar_box(win(0))) && box_empty(title_close_box(win(0))));
    /* a fixed one joins: the master shrinks to the left half, the new one
     * sits centred in the right half at its own size */
    CHECK(fk_open(&fks[1], 320, 200, true));
    CFG(1, 0, 0, 0);
    CHECK(fills(0, 2, 0));
    CHECK_EQ(inner(2, 0).x2, 635);
    AT(1, 645 + (627 - 320) / 2, 8 + (784 - 200) / 2);
    /* a third: the stack splits */
    CHECK(fk_open(&fks[2], 320, 200, false));
    CHECK(fills(2, 3, 2));
    struct comp_box s1 = inner(3, 1);
    AT(1, s1.x1 + (s1.x2 - s1.x1 - 320) / 2, s1.y1 + (s1.y2 - s1.y1 - 200) / 2);
    return true;
}

static bool tiling_rules_steps(void)
{
    /* a press on a tiled window's border focuses it, nothing more: no
     * moving, no resizing, no double-click (no title bar) */
    struct comp_box f2 = window_frame(win(2));
    CHECK(press_btn(f2.x1 + 50, f2.y1, BTN));
    CHECK(!seat.ops);
    CHECK_EQ(seat.focused, win(2));
    release_at(f2.x1 - 300, f2.y1 + 100);
    CHECK(fills(2, 3, 2));
    seat.held = true;
    CHECK_ST(wm_begin_move(win(2), 0, 0), ERR_BAD_STATE);
    CHECK_ST(wm_begin_resize(win(2), WM_EDGE_RIGHT, 0, 0), ERR_BAD_STATE);
    seat.held = false;
    /* the client may still maximise it: the whole output, a border round */
    wm_request_maximized(fks[2].ww, true);
    CFG(2, OUT_W - 2 * DECO_BORDER, OUT_H - 2 * DECO_BORDER,
        WM_ST_ACTIVATED | WM_ST_MAXIMIZED);
    wm_request_maximized(fks[2].ww, false);
    CHECK(fills(2, 3, 2));
    /* Super+Q: the close box tiling has none of */
    wm_close(win(2));
    CHECK_EQ(fks[2].closes, 1);
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
    wm_test_start(COMP_TILING);
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
    wm_test_start(COMP_FLOATING);
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
    wm_close(tw);
    CHECK(box_empty(title_bar_box(tw)) && !tw->title);
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
    wm_test_start(COMP_FLOATING);
    bool ok = focus_steps();
    fk_close_all();
    return ok;
}

/* ---- what is under the pointer --------------------------------------------------------- */

/* The circles of win(1) (surface at 510, 330: frame from 509, bar y
 * 302..329): each one's hit box, LOOK_BTN_HIT round the circle, the boxes
 * touching. */
static bool circle_hits(void)
{
    uint32_t e;
    int32_t y = 302 + LOOK_BTN_TOP + LOOK_BTN_D / 2;
    CHECK_EQ(deco_hit(win(1), 524, y, &e), DECO_CLOSE);        /* the middles */
    CHECK_EQ(deco_hit(win(1), 542, y, &e), DECO_MINIMISE);
    CHECK_EQ(deco_hit(win(1), 560, y, &e), DECO_FULLSCREEN);
    CHECK_EQ(deco_hit(win(1), 515, y, &e), DECO_CLOSE);        /* close: x 515..532 */
    CHECK_EQ(deco_hit(win(1), 514, y, &e), DECO_TITLE);
    CHECK_EQ(deco_hit(win(1), 532, y, &e), DECO_CLOSE);
    CHECK_EQ(deco_hit(win(1), 533, y, &e), DECO_MINIMISE);     /* minimise: 533..550 */
    CHECK_EQ(deco_hit(win(1), 551, y, &e), DECO_FULLSCREEN);   /* full screen: 551..568 */
    CHECK_EQ(deco_hit(win(1), 568, y, &e), DECO_FULLSCREEN);
    CHECK_EQ(deco_hit(win(1), 569, y, &e), DECO_TITLE);
    CHECK_EQ(deco_hit(win(1), 524, 307, &e), DECO_CLOSE);      /* y 307..324 */
    CHECK_EQ(deco_hit(win(1), 524, 306, &e), DECO_TITLE);
    CHECK_EQ(deco_hit(win(1), 524, 324, &e), DECO_CLOSE);
    CHECK_EQ(deco_hit(win(1), 524, 325, &e), DECO_TITLE);
    CHECK_EQ(title_button_at(win(1), 542, y), TITLE_MINIMISE);
    CHECK(box_contains(title_close_box(win(1)), 524, y));
    return true;
}

static bool window_at_steps(void)
{
    bool on;
    CHECK(fk_open(&fks[0], 320, 200, false));   /* at 480, 300 */
    CHECK(fk_open(&fks[1], 320, 200, false));   /* at 510, 330: over it */
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
    CHECK_EQ(deco_hit(win(1), 600, 303, &e), DECO_EDGE);   /* the title bar's top rows */
    CHECK_EQ(e, WM_EDGE_TOP);
    CHECK_EQ(deco_hit(win(1), 505, 315, &e), DECO_EDGE);   /* near the top-left corner */
    CHECK_EQ(e, WM_EDGE_LEFT | WM_EDGE_TOP);
    CHECK_EQ(deco_hit(win(1), 830, 400, &e), DECO_EDGE);   /* the right outline */
    CHECK_EQ(e, WM_EDGE_RIGHT);
    CHECK(circle_hits());
    CHECK_EQ(deco_hit(win(1), 100, 100, &e), DECO_NONE);
    /* a few pixels outside a resizable window's frame still resize it */
    CHECK_EQ(deco_hit(win(1), 836, 400, &e), DECO_EDGE);
    CHECK_EQ(e, WM_EDGE_RIGHT);
    CHECK_EQ(wm_window_at(836, 400, &on), win(1));
    CHECK(!on);
    CHECK_EQ(wm_window_at(837, 400, &on), NULL);
    CHECK(!strcmp(win(1)->title, ""));
    wm_set_title(fks[1].ww, "a window");
    CHECK(!strcmp(win(1)->title, "a window"));
    /* a title set before the first buffer (as libfun does) is there at the map */
    struct fk *f = &fks[2];
    *f = (struct fk){ .s = { .client = &fake_client, .input_all = true }, .own_w = 64,
                      .own_h = 64 };
    CHECK((f->ww = wm_create(&f->s, &fk_ops, f)) != NULL);
    wm_set_title(f->ww, "early");
    wm_reconfigure(f->ww);
    CHECK(fk_draw(f));
    CHECK(win(2)->title && !strcmp(win(2)->title, "early"));
    /* not responding: the flag the title bar reads */
    wm_set_not_responding(fks[1].ww, true);
    CHECK(win(1)->flags & COMP_WIN_UNRESPONSIVE);
    wm_set_not_responding(fks[1].ww, false);
    CHECK(!(win(1)->flags & COMP_WIN_UNRESPONSIVE));
    return true;
}

bool t_wm_window_at(void)
{
    wm_test_start(COMP_FLOATING);
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
    wm_test_start(COMP_FLOATING);   /* the next boot */
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
    wm_test_start(COMP_FLOATING);
    bool ok = layout_setting_steps();
    fk_close_all();
    CHECK_ST(ns_unmount("/w"), OK);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}
