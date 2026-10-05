/* The pointer on decorations, and the compositor's grabs (wm.h, comp.h):
 * moving a window by its title bar, resizing it by an edge, its three
 * circles (close, minimise, full screen), a double-click on the title bar
 * that makes it full screen (the owner's look; leaving full screen gives
 * back what it was). The seat (pointer.c) owns the pointer:
 * it offers us every first press (wm_press) before a client sees it, and
 * we take the ones on a window's decorations; a client's own
 * xdg_toplevel.move or resize reaches us once the seat has checked its
 * serial (xdgtop.c). Either way the pointer is then ours through the
 * seat's grab (seat_grab_begin): motion comes to grab_motion, the last
 * release to grab_end, and no client sees any of it.
 *
 * Only a floating window in its normal state moves, and only one that can
 * resize (wm_resizable) resizes: in tiling the layout decides, and a
 * maximised or full-screen window has no place of its own to move.
 *
 * A move changes the floating place at once (it is ours). A resize only
 * asks: each motion sends a configure with the size the pointer says
 * (within the client's limits, and at least WM_MIN_SIDE), marked resizing,
 * and the window takes the size when the client commits it. Dragging the
 * left or top edge keeps the opposite side still (the window's anchor),
 * however late the client's commits come.
 *
 * A circle acts when the press that began on it is released on it too
 * (pressed and dragged away: nothing), as buttons do.
 *
 * The desktop (desk.c) sees every first press before the window manager:
 * the strip and its cards are over every window.
 *
 * A double-click is two presses on one title bar within
 * WM_DOUBLE_CLICK_NS, the first released without the pointer moving
 * further than WM_CLICK_SLOP (a drag is not a click). */
#include "wm.h"

#define WM_CLICK_SLOP 4       /* pixels a click may wander and still be one */
#define WM_BTN_LEFT   0x110u  /* evdev's left button (input-event-codes.h): wm_press's button */

enum grab_kind {
    GRAB_NONE,
    GRAB_TITLE,                    /* a title bar pressed where it can't move: watch for a click */
    GRAB_MOVE,
    GRAB_RESIZE,
    GRAB_BUTTON,                   /* pressed on a circle: acts if released on it */
};

static struct {
    enum grab_kind kind;
    struct wm_window *ww;          /* the window it is on; NULL once that went */
    uint32_t edges;                /* GRAB_RESIZE: WM_EDGE_* */
    enum title_button button;      /* GRAB_BUTTON: which circle */
    int32_t px, py;                /* where the pointer was at the start */
    struct comp_box start;         /* the surface's box at the start */
} grab;

static struct {
    const struct wm_window *ww;    /* the last click on a title bar, for a double-click */
    uint64_t t;
} last_title;

static void grab_motion(void *data, int32_t x, int32_t y);
static void grab_end(void *data);
static const struct comp_grab_ops grab_ops = { grab_motion, grab_end };

/* ---- starting and ending ------------------------------------------------------------ */

static bool can_move(const struct wm_window *ww)
{
    return ww->win && wm_layout_of(ww) == COMP_FLOATING && ww->want == WM_NORMAL &&
           !(ww->shown & (WM_ST_MAXIMIZED | WM_ST_FULLSCREEN));
}

/* The seat's grab, for kind on ww from (x, y). */
static status_t start(enum grab_kind kind, struct wm_window *ww, int32_t x, int32_t y)
{
    if (grab.kind != GRAB_NONE)
        return ERR_BAD_STATE;
    status_t st = seat_grab_begin(&grab_ops, NULL);
    if (st != OK)
        return st;
    grab.kind = kind;
    grab.ww = ww;
    grab.edges = 0;
    grab.px = x;
    grab.py = y;
    grab.start = window_surface_box(ww->win);
    return OK;
}

/* Our side of a grab over: a resize's last configure says it is done. */
static void finish(void)
{
    struct wm_window *ww = grab.ww;
    enum grab_kind kind = grab.kind;
    grab.kind = GRAB_NONE;
    grab.ww = NULL;
    if (kind == GRAB_RESIZE && ww) {
        ww->resizing = false;
        wm_reconfigure(ww);
    }
}

void wm_grab_cancel(void)
{
    if (grab.kind != GRAB_NONE)
        seat_grab_cancel();
    finish();
    last_title.ww = NULL;
}

void wm_grab_forget(const struct wm_window *ww)
{
    if (grab.kind != GRAB_NONE && grab.ww == ww) {
        seat_grab_cancel();   /* the window goes mid-grab: the seat drops it */
        if (grab.kind == GRAB_RESIZE)
            grab.ww->resizing = false;
        grab.kind = GRAB_NONE;
        grab.ww = NULL;
    }
    if (last_title.ww == ww)
        last_title.ww = NULL;
}

status_t wm_begin_move(struct comp_window *w, int32_t x, int32_t y)
{
    struct wm_window *ww = w ? w->wm : NULL;
    if (!ww || !can_move(ww))
        return ERR_BAD_STATE;
    status_t st = start(GRAB_MOVE, ww, x, y);
    if (st == OK)
        ww->anchor = 0;
    return st;
}

static bool edges_ok(uint32_t edges)
{
    return edges && !(edges & ~(WM_EDGE_TOP | WM_EDGE_BOTTOM | WM_EDGE_LEFT | WM_EDGE_RIGHT)) &&
           (edges & (WM_EDGE_TOP | WM_EDGE_BOTTOM)) != (WM_EDGE_TOP | WM_EDGE_BOTTOM) &&
           (edges & (WM_EDGE_LEFT | WM_EDGE_RIGHT)) != (WM_EDGE_LEFT | WM_EDGE_RIGHT);
}

status_t wm_begin_resize(struct comp_window *w, uint32_t edges, int32_t x, int32_t y)
{
    struct wm_window *ww = w ? w->wm : NULL;
    if (!edges_ok(edges))
        return ERR_INVALID_ARGS;
    if (!ww || !can_move(ww) || !wm_resizable(ww))
        return ERR_BAD_STATE;
    status_t st = start(GRAB_RESIZE, ww, x, y);
    if (st != OK)
        return st;
    grab.edges = edges;
    ww->resizing = true;
    ww->anchor = edges & (WM_EDGE_LEFT | WM_EDGE_TOP);
    ww->anchor_x2 = grab.start.x2;
    ww->anchor_y2 = grab.start.y2;
    ww->float_w = grab.start.x2 - grab.start.x1;
    ww->float_h = grab.start.y2 - grab.start.y1;
    wm_reconfigure(ww);   /* resizing on */
    return OK;
}

/* ---- presses ------------------------------------------------------------------------ */

/* A press on a title bar: the second of a double-click makes the window
 * full screen; otherwise a move, or a grab that only watches for a click. */
static void title_press(struct wm_window *ww, int32_t x, int32_t y)
{
    uint64_t t = now();
    if (last_title.ww == ww && t - last_title.t <= WM_DOUBLE_CLICK_NS) {
        last_title.ww = NULL;
        wm_toggle_fullscreen(ww->win);
        return;
    }
    last_title.ww = ww;
    last_title.t = t;
    (void)start(can_move(ww) ? GRAB_MOVE : GRAB_TITLE, ww, x, y);   /* refused: a click still */
}

bool wm_press(int32_t x, int32_t y, uint32_t button)
{
    if (desk_press(x, y, button))
        return true;   /* the strip, a card, or a click that closed a menu on the strip */
    bool on_surface;
    struct comp_window *w = wm_window_at(x, y, &on_surface);
    struct wm_window *ww = w ? w->wm : NULL;
    if (!ww || on_surface)
        return false;   /* a client's, or nobody's */
    seat_focus(w);
    if (button != WM_BTN_LEFT)
        return true;   /* taken: no client sees it, and it does nothing */
    uint32_t edges;
    enum deco_part part = deco_hit(w, x, y, &edges);
    if (part == DECO_CLOSE || part == DECO_MINIMISE || part == DECO_FULLSCREEN) {
        if (start(GRAB_BUTTON, ww, x, y) == OK)
            grab.button = title_button_at(w, x, y);
    } else if (part == DECO_TITLE)
        title_press(ww, x, y);
    else if (part == DECO_EDGE && can_move(ww) && wm_resizable(ww))
        (void)wm_begin_resize(w, edges, x, y);
    return true;
}

/* ---- the grab's motion and end --------------------------------------------------------- */

/* One side's new length: the start's, grown by how far the pointer went
 * outwards (d), within lo and hi (0: none), at least WM_MIN_SIDE. */
static int32_t resized(int32_t len, int32_t d, int32_t lo, int32_t hi)
{
    int64_t v = (int64_t)len + d;
    if (hi > 0 && v > hi)
        v = hi;
    if (v < lo)
        v = lo;
    if (v < WM_MIN_SIDE)
        v = WM_MIN_SIDE;
    return v > COMP_BUFFER_SIDE_MAX ? COMP_BUFFER_SIDE_MAX : (int32_t)v;
}

static void resize_to(int32_t x, int32_t y)
{
    struct wm_window *ww = grab.ww;
    int32_t dx = x - grab.px, dy = y - grab.py;
    int32_t w = grab.start.x2 - grab.start.x1, h = grab.start.y2 - grab.start.y1;
    if (grab.edges & (WM_EDGE_LEFT | WM_EDGE_RIGHT))
        w = resized(w, grab.edges & WM_EDGE_LEFT ? -dx : dx, ww->min_w, ww->max_w);
    if (grab.edges & (WM_EDGE_TOP | WM_EDGE_BOTTOM))
        h = resized(h, grab.edges & WM_EDGE_TOP ? -dy : dy, ww->min_h, ww->max_h);
    if (w == ww->float_w && h == ww->float_h)
        return;
    ww->float_w = w;
    ww->float_h = h;
    wm_reconfigure(ww);
}

static void grab_motion(void *data, int32_t x, int32_t y)
{
    struct wm_window *ww = grab.ww;
    (void)data;
    int32_t dx = x - grab.px, dy = y - grab.py;
    if (dx > WM_CLICK_SLOP || dx < -WM_CLICK_SLOP || dy > WM_CLICK_SLOP || dy < -WM_CLICK_SLOP)
        last_title.ww = NULL;   /* a drag, not a click */
    if (!ww)
        return;
    if (grab.kind == GRAB_MOVE) {
        ww->float_x = grab.start.x1 + dx;
        ww->float_y = grab.start.y1 + dy;
        wm_place(ww);
    } else if (grab.kind == GRAB_RESIZE) {
        resize_to(x, y);
    }
}

/* A circle's press released on it: what it does. */
static void button_up(struct wm_window *ww, enum title_button b)
{
    if (b == TITLE_CLOSE && ww->ops && ww->ops->close)
        ww->ops->close(ww->ctx);
    else if (b == TITLE_MINIMISE)
        wm_minimise(ww->win);
    else if (b == TITLE_FULLSCREEN)
        wm_toggle_fullscreen(ww->win);
}

static void grab_end(void *data)
{
    struct wm_window *ww = grab.ww;
    enum title_button b = grab.button;
    (void)data;
    bool on = grab.kind == GRAB_BUTTON && ww && ww->win &&
              title_button_at(ww->win, cursor.x, cursor.y) == b;
    finish();
    if (on)
        button_up(ww, b);
}
