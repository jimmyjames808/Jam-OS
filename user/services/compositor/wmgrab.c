/* The pointer on decorations and tiles, and the compositor's grabs (wm.h,
 * comp.h): moving a window by its title bar, resizing it by an edge, its
 * three circles (close, minimise, full screen), a double-click on the
 * title bar that makes it full screen (the owner's look; leaving full
 * screen gives back what it was), dragging the gap between two tiles,
 * and with Super held (the owner's keys): a floating window moved from
 * anywhere in it (Super+drag) or resized from its nearest corner
 * (Super+right-drag), a tiled one picked up and dropped on another to
 * swap them. The seat (pointer.c) owns the pointer: it offers us every
 * first press (wm_press) before a client sees it, and we take the ones on
 * a window's decorations, on a gap, and every one with Super held on a
 * window; a client's own xdg_toplevel.move or resize reaches us once the
 * seat has checked its serial (xdgtop.c). Either way the pointer is then
 * ours through the seat's grab (seat_grab_begin): motion comes to
 * grab_motion, the last release to grab_end, and no client sees any of
 * it.
 *
 * Only a floating window in its normal state moves, and only one that can
 * resize (wm_resizable) resizes: in tiling the layout decides, and a
 * maximised or full-screen window has no place of its own to move.
 *
 * A move changes the floating place at once (it is ours). A resize only
 * asks: each motion sends a configure with the size the pointer says
 * (within the client's limits, at least WM_MIN_SIDE, and with its title
 * bar below the strip), marked resizing, and the window takes the size
 * when the client commits it. Dragging the left or top edge keeps the
 * opposite side still (the window's anchor), however late the client's
 * commits come.
 *
 * A gap (wmtile.c: WM_GAP_HIT pixels across, centred on it) moves with
 * the pointer, every tile on the screen placed again at each motion with
 * no glide (live); it is lit while the pointer is over it or drags it
 * (wm_marks.bar). A picked-up tile is drawn as its picture (an animation's
 * snapshot, see-through) following the pointer, with the tile under the
 * pointer marked (wm_marks: wmdraw.c draws them); letting go over one
 * swaps the two, and the tiles glide to their new places.
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
#include "desk.h"

#define WM_CLICK_SLOP 4       /* pixels a click may wander and still be one */
#define WM_BTN_LEFT   0x110u  /* evdev's buttons (input-event-codes.h): wm_press's button */
#define WM_BTN_RIGHT  0x111u
#define MOD_SUPER     (INPUT_MOD_LGUI | INPUT_MOD_RGUI)

enum grab_kind {
    GRAB_NONE,
    GRAB_TITLE,                    /* a title bar pressed where it can't move: watch for a click */
    GRAB_MOVE,
    GRAB_RESIZE,
    GRAB_BUTTON,                   /* pressed on a circle: acts if released on it */
    GRAB_GAP,                      /* a gap between tiles dragged */
    GRAB_LIFT,                     /* a tile picked up with Super: dropped on another, they swap */
};

static struct {
    enum grab_kind kind;
    struct wm_window *ww;          /* the window it is on; NULL once that went (and for a gap) */
    uint32_t edges;                /* GRAB_RESIZE: WM_EDGE_* */
    enum title_button button;      /* GRAB_BUTTON: which circle */
    int32_t px, py;                /* where the pointer was at the start */
    struct comp_box start;         /* the surface's box at the start */
    struct tile_node *split;       /* GRAB_GAP: its split, while the tree is generation gen */
    uint64_t gen;
    int32_t off;                   /* GRAB_GAP: the gap's middle less the pointer, along it */
    struct comp_box frame;         /* GRAB_LIFT: the lifted window's frame at the start */
    struct wm_window *target;      /* GRAB_LIFT: the tile it would swap with, or NULL */
} grab;

static struct {
    const struct wm_window *ww;    /* the last click on a title bar, for a double-click */
    uint64_t t;
} last_title;

struct wm_marks wm_marks;

static void grab_motion(void *data, int32_t x, int32_t y);
static void grab_end(void *data);
static const struct comp_grab_ops grab_ops = { grab_motion, grab_end };

/* ---- the marks: a lit gap, a lifted tile, its target ------------------------------------ */

static bool same_box(struct comp_box a, struct comp_box b)
{
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

/* *at becomes b, both damaged if it changed (for drawing over the windows:
 * what the desktop's cards blur, so plain damage). */
static void mark_set(struct comp_box *at, struct comp_box b)
{
    if (same_box(*at, b))
        return;
    scene_damage(*at);
    scene_damage(b);
    *at = b;
}

/* The lifted picture drawn at b (with the shadow it casts round it). */
static void ghost_set(struct comp_box b)
{
    struct comp_box *g = &wm_marks.ghost;
    if (same_box(*g, b))
        return;
    scene_damage((struct comp_box){ g->x1 - LOOK_SHADOW_SIDE, g->y1 - LOOK_SHADOW_ABOVE,
                                    g->x2 + LOOK_SHADOW_SIDE, g->y2 + LOOK_SHADOW_BELOW });
    *g = b;
    scene_damage((struct comp_box){ b.x1 - LOOK_SHADOW_SIDE, b.y1 - LOOK_SHADOW_ABOVE,
                                    b.x2 + LOOK_SHADOW_SIDE, b.y2 + LOOK_SHADOW_BELOW });
}

/* The gap under (x, y) a press would drag: on the current screen's tree,
 * with nothing over it but the tiles beside it (no maximised window, no
 * strip or card). */
static bool gap_under(int32_t x, int32_t y, struct tile_gap *g)
{
    bool on;
    if (desk_covers(x, y) || !tiles_gap_at(screens_cur(), x, y, g))
        return false;
    struct comp_window *w = wm_window_at(x, y, &on);
    const struct wm_window *ww = w ? w->wm : NULL;
    return !w || (ww && ww->leaf && !(ww->shown & (WM_ST_MAXIMIZED | WM_ST_FULLSCREEN)));
}

bool wm_gap_covers(int32_t x, int32_t y)
{
    struct tile_gap g;
    return gap_under(x, y, &g);
}

/* Is the gap grab's split still in a tree as it was? */
static bool gap_live(void)
{
    return grab.kind == GRAB_GAP && grab.split && tiles_generation() == grab.gen;
}

void wm_marks_update(void)
{
    struct comp_box bar = { 0, 0, 0, 0 };
    struct tile_gap g;
    if (gap_live())
        bar = tiles_gap(grab.split).bar;
    else if (grab.kind == GRAB_NONE && !comp.blanked && cursor.moved &&
             gap_under(cursor.x, cursor.y, &g))
        bar = g.bar;   /* hovered: not before the mouse first moves (no cursor is drawn) */
    mark_set(&wm_marks.bar, bar);
    if (grab.kind == GRAB_LIFT)   /* the target's frame may have moved under the pointer */
        mark_set(&wm_marks.target, grab.target && grab.target->win
                                       ? window_frame(grab.target->win)
                                       : (struct comp_box){ 0, 0, 0, 0 });
}

/* ---- starting and ending ------------------------------------------------------------ */

/* The seat's grab, for kind on ww (NULL: a gap) from (x, y). */
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
    grab.start = ww ? window_surface_box(ww->win) : (struct comp_box){ 0, 0, 0, 0 };
    grab.split = NULL;
    grab.target = NULL;
    return OK;
}

/* A lifted tile put down: drawn as itself again, its picture and marks gone. */
static void lift_end(void)
{
    struct wm_window *ww = grab.ww;
    if (ww && ww->win && wm_marks.lift.px) {
        ww->win->flags &= ~COMP_WIN_ANIMATED;
        window_damage(ww->win);
    }
    anim_snapshot_free(&wm_marks.lift);
    ghost_set((struct comp_box){ 0, 0, 0, 0 });
    mark_set(&wm_marks.target, (struct comp_box){ 0, 0, 0, 0 });
}

/* Our side of a grab over: a resize's last configure says it is done; a
 * lifted tile is put down. */
static void finish(void)
{
    struct wm_window *ww = grab.ww;
    enum grab_kind kind = grab.kind;
    if (kind == GRAB_LIFT)
        lift_end();
    grab.kind = GRAB_NONE;
    grab.ww = NULL;
    grab.split = NULL;
    grab.target = NULL;
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
    if (grab.kind == GRAB_LIFT && grab.target == ww) {
        grab.target = NULL;   /* the tile under the pointer went: nothing to swap with */
        mark_set(&wm_marks.target, (struct comp_box){ 0, 0, 0, 0 });
    }
    if (grab.kind != GRAB_NONE && grab.ww == ww) {
        seat_grab_cancel();   /* the window goes mid-grab: the seat drops it */
        if (grab.kind == GRAB_RESIZE)
            grab.ww->resizing = false;
        if (grab.kind == GRAB_LIFT)
            lift_end();
        grab.kind = GRAB_NONE;
        grab.ww = NULL;
    }
    if (last_title.ww == ww)
        last_title.ww = NULL;
}

status_t wm_begin_move(struct comp_window *w, int32_t x, int32_t y)
{
    struct wm_window *ww = w ? w->wm : NULL;
    if (!ww || !wm_floating_normal(ww))
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
    if (!ww || !wm_floating_normal(ww) || !wm_resizable(ww))
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
    (void)start(wm_floating_normal(ww) ? GRAB_MOVE : GRAB_TITLE, ww, x, y);   /* refused: a
                                                                               * click still */
}

/* A press on a gap: it follows the pointer from here. */
static bool gap_press(int32_t x, int32_t y)
{
    struct tile_gap g;
    if (!gap_under(x, y, &g) || start(GRAB_GAP, NULL, x, y) != OK)
        return false;
    grab.split = g.split;
    grab.gen = tiles_generation();
    bool across = g.split->across;
    grab.off = across ? g.gap.x1 + WM_GAP / 2 - x : g.gap.y1 + WM_GAP / 2 - y;
    mark_set(&wm_marks.bar, g.bar);
    return true;
}

/* A tile picked up: its picture follows the pointer from here. */
static void lift(struct wm_window *ww, int32_t x, int32_t y)
{
    if (start(GRAB_LIFT, ww, x, y) != OK)
        return;
    struct comp_window *w = ww->win;
    grab.frame = window_frame(w);
    if (anim_snapshot(w, &wm_marks.lift) == OK) {   /* none: it stays drawn where it is */
        w->flags |= COMP_WIN_ANIMATED;
        window_damage(w);
        ghost_set(grab.frame);
    }
}

/* The corner of w's frame nearest (x, y), as resize edges. */
static uint32_t nearest_corner(const struct comp_window *w, int32_t x, int32_t y)
{
    struct comp_box f = window_frame(w);
    return (2 * x < f.x1 + f.x2 ? WM_EDGE_LEFT : WM_EDGE_RIGHT) |
           (2 * y < f.y1 + f.y2 ? WM_EDGE_TOP : WM_EDGE_BOTTOM);
}

/* A press with Super held: on a toplevel, ours wherever it lands. */
static bool super_press(int32_t x, int32_t y, uint32_t button)
{
    bool on_surface;
    struct comp_window *w = wm_window_at(x, y, &on_surface);
    struct wm_window *ww = w ? w->wm : NULL;
    if (!ww)
        return false;   /* the background, or a window that is no toplevel: its client's */
    seat_focus(w);
    if (button == WM_BTN_LEFT && ww->leaf && !(ww->shown & (WM_ST_MAXIMIZED | WM_ST_FULLSCREEN)))
        lift(ww, x, y);
    else if (button == WM_BTN_LEFT && wm_floating_normal(ww))
        (void)wm_begin_move(w, x, y);
    else if (button == WM_BTN_RIGHT && wm_floating_normal(ww) && wm_resizable(ww))
        (void)wm_begin_resize(w, nearest_corner(w, x, y), x, y);
    return true;   /* taken: no client sees it, whatever it did */
}

bool wm_press(int32_t x, int32_t y, uint32_t button, uint8_t mods)
{
    if (desk_press(x, y, button))
        return true;   /* the strip, a card, or a click that closed a menu on the strip */
    if (mods & MOD_SUPER)
        return super_press(x, y, button);
    if (button == WM_BTN_LEFT && gap_press(x, y))
        return true;
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
    else if (part == DECO_EDGE && wm_floating_normal(ww) && wm_resizable(ww))
        (void)wm_begin_resize(w, edges, x, y);
    return true;
}

enum cursor_shape wm_gap_cursor(int32_t x, int32_t y)
{
    struct tile_gap g;
    if (!gap_under(x, y, &g))
        return CURSOR_SHAPES;
    return g.split->across ? CURSOR_RESIZE_EW : CURSOR_RESIZE_NS;
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
    if (grab.edges & WM_EDGE_TOP) {   /* its title bar stays below the strip */
        int32_t room = grab.start.y2 - ww->win->deco_top - wm_floor(ww);
        int32_t hi = ww->max_h > 0 && ww->max_h < room ? ww->max_h : room < 1 ? 1 : room;
        h = resized(h, -dy, ww->min_h, hi);
    } else if (grab.edges & WM_EDGE_BOTTOM) {
        h = resized(h, dy, ww->min_h, ww->max_h);
    }
    if (w == ww->float_w && h == ww->float_h)
        return;
    ww->float_w = w;
    ww->float_h = h;
    wm_reconfigure(ww);
}

/* The tile a lifted one would swap with at (x, y): the topmost window
 * whose frame holds it, if that is a tile of the same screen. */
static struct wm_window *drop_target(int32_t x, int32_t y)
{
    const struct wm_window *from = grab.ww;
    for (struct comp_window *w = scene.top; w; w = w->below) {
        if (!(w->flags & COMP_WIN_MAPPED) || w == from->win || !box_contains(window_frame(w), x, y))
            continue;
        struct wm_window *ww = w->wm;
        return ww && ww->leaf && ww->tiled_on == from->tiled_on ? ww : NULL;
    }
    return NULL;
}

static void lift_motion(int32_t x, int32_t y)
{
    if (wm_marks.lift.px)
        ghost_set(box_translate(grab.frame, x - grab.px, y - grab.py));
    grab.target = drop_target(x, y);
    mark_set(&wm_marks.target, grab.target ? window_frame(grab.target->win)
                                           : (struct comp_box){ 0, 0, 0, 0 });
}

static void gap_motion(int32_t x, int32_t y)
{
    if (!gap_live())
        return;   /* its tree changed under it (a window went): the drag is over */
    tiles_gap_move(grab.split, (grab.split->across ? x : y) + grab.off);
    wm_relayout();   /* live: placed at once, no glide */
}

static void grab_motion(void *data, int32_t x, int32_t y)
{
    struct wm_window *ww = grab.ww;
    (void)data;
    int32_t dx = x - grab.px, dy = y - grab.py;
    if (dx > WM_CLICK_SLOP || dx < -WM_CLICK_SLOP || dy > WM_CLICK_SLOP || dy < -WM_CLICK_SLOP)
        last_title.ww = NULL;   /* a drag, not a click */
    if (grab.kind == GRAB_GAP)
        gap_motion(x, y);
    if (!ww)
        return;
    if (grab.kind == GRAB_MOVE) {
        ww->float_x = grab.start.x1 + dx;
        ww->float_y = grab.start.y1 + dy;
        wm_place(ww);
    } else if (grab.kind == GRAB_RESIZE) {
        resize_to(x, y);
    } else if (grab.kind == GRAB_LIFT) {
        lift_motion(x, y);
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
    struct wm_window *ww = grab.ww, *target = grab.target;
    enum title_button b = grab.button;
    enum grab_kind kind = grab.kind;
    (void)data;
    bool on = kind == GRAB_BUTTON && ww && ww->win &&
              title_button_at(ww->win, cursor.x, cursor.y) == b;
    finish();
    if (on)
        button_up(ww, b);
    if (kind == GRAB_LIFT && ww && target && tiles_swap(ww, target))
        wm_reflow();   /* the two glide to each other's tiles */
}
