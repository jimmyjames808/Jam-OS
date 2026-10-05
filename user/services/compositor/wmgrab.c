/* The pointer on decorations, and the compositor's grabs (wm.h, comp.h):
 * moving a window by its title bar, resizing it by an edge, its close box,
 * a double-click that maximises. The seat (pointer.c) owns the pointer and
 * hands us a press that lands on a window's decorations, or a client's own
 * xdg_toplevel.move or resize once it has checked the press's serial; from
 * then until the release, the pointer is ours and no client sees it.
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
 * however late the client's commits come. */
#include "wm.h"

enum grab_kind {
    GRAB_NONE,
    GRAB_INERT,                    /* a press we took that does nothing: wait for the release */
    GRAB_MOVE,
    GRAB_RESIZE,
    GRAB_CLOSE,                    /* pressed on a close box: closes if released on it */
};

static struct {
    enum grab_kind kind;
    struct wm_window *ww;          /* the window it is on (NULL for an inert one on a gone window) */
    uint32_t edges;                /* GRAB_RESIZE: WM_EDGE_* */
    int32_t px, py;                /* where the pointer was at the start */
    struct comp_box start;         /* the surface's box at the start */
} grab;

static struct {
    const struct wm_window *ww;    /* the last press on a title bar, for a double-click */
    uint64_t t;
} last_title;

bool wm_grabbing(void)
{
    return grab.kind != GRAB_NONE;
}

void wm_grab_forget(const struct wm_window *ww)
{
    if (grab.ww == ww) {
        if (grab.kind == GRAB_RESIZE)
            grab.ww->resizing = false;
        grab.ww = NULL;
        if (grab.kind != GRAB_NONE)
            grab.kind = GRAB_INERT;   /* the button is still down: still ours */
    }
    if (last_title.ww == ww)
        last_title.ww = NULL;
}

static bool can_move(const struct wm_window *ww)
{
    return ww->win && scene.layout == COMP_FLOATING && ww->want == WM_NORMAL &&
           !(ww->shown & (WM_ST_MAXIMIZED | WM_ST_FULLSCREEN));
}

static void start(enum grab_kind kind, struct wm_window *ww, int32_t x, int32_t y)
{
    grab.kind = kind;
    grab.ww = ww;
    grab.edges = 0;
    grab.px = x;
    grab.py = y;
    grab.start = ww ? window_surface_box(ww->win) : (struct comp_box){ 0, 0, 0, 0 };
}

status_t wm_begin_move(struct comp_window *w, int32_t x, int32_t y)
{
    struct wm_window *ww = w ? w->wm : NULL;
    if (!ww || grab.kind != GRAB_NONE || !can_move(ww))
        return ERR_BAD_STATE;
    ww->anchor = 0;
    start(GRAB_MOVE, ww, x, y);
    return OK;
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
    if (!ww || grab.kind != GRAB_NONE || !can_move(ww) || !wm_resizable(ww))
        return ERR_BAD_STATE;
    start(GRAB_RESIZE, ww, x, y);
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

/* A press on a title bar: the second of a double-click maximises (or
 * restores), the first starts a move. */
static void title_press(struct wm_window *ww, int32_t x, int32_t y, uint64_t t)
{
    if (last_title.ww == ww && t - last_title.t <= WM_DOUBLE_CLICK_NS) {
        last_title.ww = NULL;
        wm_request_maximized(ww, ww->want != WM_MAXIMIZED);
        return;
    }
    last_title.ww = ww;
    last_title.t = t;
    if (can_move(ww))
        start(GRAB_MOVE, ww, x, y);
}

bool wm_pointer_press(int32_t x, int32_t y, uint32_t button, uint64_t t)
{
    if (grab.kind != GRAB_NONE)
        return true;   /* another button during our grab: ours too */
    bool on_surface;
    struct comp_window *w = wm_window_at(x, y, &on_surface);
    if (!w || on_surface)
        return false;
    struct wm_window *ww = w->wm;
    if (comp_wm_hooks.focus)
        comp_wm_hooks.focus(w);
    start(GRAB_INERT, ww, x, y);
    uint32_t edges;
    enum deco_part part = deco_hit(w, x, y, &edges);
    if (button != WM_BTN_LEFT || !ww)
        return true;
    if (part == DECO_CLOSE)
        grab.kind = GRAB_CLOSE;
    else if (part == DECO_TITLE)
        title_press(ww, x, y, t);
    else if (part == DECO_EDGE && can_move(ww) && wm_resizable(ww)) {
        grab.kind = GRAB_NONE;   /* wm_begin_resize starts from none */
        if (wm_begin_resize(w, edges, x, y) != OK)
            start(GRAB_INERT, ww, x, y);
    }
    return true;
}

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

void wm_pointer_motion(int32_t x, int32_t y)
{
    struct wm_window *ww = grab.ww;
    if (!ww)
        return;
    if (grab.kind == GRAB_MOVE) {
        ww->float_x = grab.start.x1 + (x - grab.px);
        ww->float_y = grab.start.y1 + (y - grab.py);
        wm_place(ww);
    } else if (grab.kind == GRAB_RESIZE) {
        resize_to(x, y);
    }
}

/* The grab ends: a resize's last configure says it is over. */
static void end(void)
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

void wm_pointer_release(int32_t x, int32_t y)
{
    struct wm_window *ww = grab.ww;
    uint32_t edges;
    bool close = grab.kind == GRAB_CLOSE && ww && ww->win &&
                 deco_hit(ww->win, x, y, &edges) == DECO_CLOSE;
    if (grab.kind == GRAB_MOVE || grab.kind == GRAB_RESIZE)
        wm_pointer_motion(x, y);
    end();
    if (close && ww->ops && ww->ops->close)
        ww->ops->close(ww->ctx);
}

void wm_grab_cancel(void)
{
    end();
    last_title.ww = NULL;
}
