/* The window manager (wm.h, comp.h): which toplevels there are, what each
 * is asked to be, where each goes, and the focus's side of it (raising,
 * the activated state, Alt+Tab's order). Policy only: the scene (scene.c)
 * keeps and damages, xdg-shell (xdgtop.c) speaks to the client, the seat
 * (focus.c) owns the keyboard focus and tells us when it moves.
 *
 * Each toplevel is on a virtual screen (screens.c), whose arrangement,
 * floating or tiling, is its own; the room a screen has for windows is the
 * output less the strip along the top while the desktop is on, and no
 * window goes into the strip or the LOOK_STRIP_GAP below it, whatever the
 * arrangement (its floor). Placement, for the states the shown buffer was
 * drawn for:
 *   - full screen: centred on the output (a smaller buffer shows the
 *     background around it, never stretched), on a screen of its own (a
 *     boot overlay's on none: over every screen, screens.c);
 *   - maximised: centred in the room below the floor, under its title bar
 *     (tiling: inside its border instead);
 *   - tiling: centred in its tile (a resizable window was asked to fill
 *     it; one that can't resize keeps its own size, wholly in the room
 *     where it fits);
 *   - floating: at its floating place. The first time it is centred in the
 *     room, moved down and right by a title bar's height while another
 *     window's top-left corner is already there (the cascade); its title
 *     bar never goes above the floor.
 *
 * The focus is the seat's (focus.c: a click, Alt+Tab, a client's first
 * window, the next one down when one goes); it tells us when it moves
 * (wm_focus_changed), and we raise the window and keep the toplevels'
 * activated state. Alt+Tab's order is ours: the order the toplevels
 * opened (stable, unlike the stacking order a raise changes), then
 * windows that are no toplevel (testwin's) from the bottom up. */
#include "desk.h"

#define CASCADE_STEP  (COMP_TITLE_H + DECO_BORDER)   /* one cascade step, both ways */
#define CASCADE_MAX   16u                            /* steps before it starts over */
#define REACH_MIN     48   /* a floating title bar keeps this much on the output */
#define CYCLE_MAX     (COMP_CLIENTS_MAX * COMP_SURFACES_MAX)   /* windows there can be */

static struct {
    struct wm_window *first, *last;   /* every toplevel, oldest first */
    struct wm_window *focused;        /* the toplevel with the seat's focus, or NULL */
    uint64_t focus_count;             /* focus changes: each window's focused_at */
} wm;

void wm_init(enum comp_layout layout)
{
    wm_grab_cancel();
    anim_finish();
    wm.first = wm.last = wm.focused = NULL;
    wm.focus_count = 0;
    screens_init(layout);
}

struct wm_window *wm_first(void)
{
    return wm.first;
}

struct wm_window *wm_focused(void)
{
    return wm.focused;
}

enum comp_layout wm_layout_of(const struct wm_window *ww)
{
    const struct desk_screen *s = ww && ww->screen ? ww->screen : screens_cur();
    return s->kind == SCREEN_NORMAL ? s->layout : COMP_FLOATING;
}

/* The room ww's windows have (its screen's, or the current one's). */
static struct comp_box room_of(const struct wm_window *ww)
{
    return screens_room(ww->screen ? ww->screen : screens_cur());
}

/* The top a window's frame may reach: the floor under the strip. */
static int32_t floor_of(const struct wm_window *ww)
{
    struct comp_box r = room_of(ww);
    return r.y1 > 0 ? r.y1 + LOOK_STRIP_GAP : r.y1;
}

/* ---- making and losing toplevels ---------------------------------------------------- */

struct wm_window *wm_create(struct comp_surface *s, const struct wm_ops *ops, void *ctx)
{
    struct wm_window *ww = calloc(1, sizeof(*ww));
    if (!ww)
        return NULL;
    ww->surface = s;
    ww->ops = ops;
    ww->ctx = ctx;
    ww->prev = wm.last;
    if (wm.last)
        wm.last->next = ww;
    else
        wm.first = ww;
    wm.last = ww;
    return ww;
}

void wm_destroy(struct wm_window *ww)
{
    wm_unmap(ww);
    if (ww->prev)
        ww->prev->next = ww->next;
    else
        wm.first = ww->next;
    if (ww->next)
        ww->next->prev = ww->prev;
    else
        wm.last = ww->prev;
    free(ww);
}

/* ---- what each window is asked to be ------------------------------------------------- */

/* v within the client's limits (0: none), and at least 1. */
static int32_t limit(int32_t v, int32_t lo, int32_t hi)
{
    if (hi > 0 && v > hi)
        v = hi;
    if (lo > 0 && v < lo)
        v = lo;
    return v < 1 ? 1 : v;
}

bool wm_resizable(const struct wm_window *ww)
{
    bool fixed_w = ww->max_w > 0 && ww->min_w == ww->max_w;
    bool fixed_h = ww->max_h > 0 && ww->min_h == ww->max_h;
    return !(fixed_w && fixed_h);
}

static struct comp_box output_box(void)
{
    return (struct comp_box){ 0, 0, scene.width, scene.height };
}

/* A maximised window's frame: the room below the floor. */
static struct comp_box max_box(const struct wm_window *ww)
{
    struct comp_box r = room_of(ww);
    r.y1 = floor_of(ww);
    return r;
}

void wm_wanted(const struct wm_window *ww, struct wm_config *out)
{
    *out = (struct wm_config){ 0, 0, ww == wm.focused ? WM_ST_ACTIVATED : 0 };
    enum comp_layout layout = wm_layout_of(ww);
    struct comp_box b;
    switch (ww->want) {
    case WM_FULLSCREEN:
        out->states |= WM_ST_FULLSCREEN;
        b = deco_inner(output_box(), WM_ST_FULLSCREEN, layout);
        break;
    case WM_MAXIMIZED:
        out->states |= WM_ST_MAXIMIZED;
        b = deco_inner(max_box(ww), WM_ST_MAXIMIZED, layout);
        break;
    default:
        if (layout == COMP_TILING) {
            if (!wm_resizable(ww))
                return;   /* 0 by 0: it keeps its own size */
            b = deco_inner(wm_tile(ww), 0, layout);
            out->width = limit(b.x2 - b.x1, ww->min_w, ww->max_w);
            out->height = limit(b.y2 - b.y1, ww->min_h, ww->max_h);
            return;
        }
        out->width = ww->float_w;
        out->height = ww->float_h;
        if (ww->resizing)
            out->states |= WM_ST_RESIZING;
        return;
    }
    out->width = b.x2 - b.x1 > 0 ? b.x2 - b.x1 : 1;
    out->height = b.y2 - b.y1 > 0 ? b.y2 - b.y1 : 1;
}

void wm_reconfigure(struct wm_window *ww)
{
    struct wm_config c;
    wm_wanted(ww, &c);
    if (ww->ops && ww->ops->configure)
        ww->ops->configure(ww->ctx, &c);
}

void wm_relayout(void)
{
    for (struct wm_window *ww = wm.first; ww; ww = ww->next) {
        wm_reconfigure(ww);
        if (ww->win)
            wm_place(ww);
    }
}

/* ---- placing ------------------------------------------------------------------------ */

/* One axis: the surface's start for a side of `side` pixels centred in
 * [lo, hi), then kept on [min, max) with its decorations (before, after)
 * where the frame fits, else at min. */
static int32_t centre(int32_t lo, int32_t hi, int32_t side, int32_t before, int32_t after,
                      int32_t min, int32_t max)
{
    int32_t v = lo + (hi - lo - side) / 2;
    if (side + before + after > max - min)
        return min + before;
    if (v - before < min)
        v = min + before;
    if (v + side + after > max)
        v = max - side - after;
    return v;
}

/* w centred in b, its frame kept on the output below floor. */
static void centre_in(struct comp_window *w, struct comp_box b, int32_t floor)
{
    const struct comp_surface *s = w->surface;
    window_move(w, centre(b.x1, b.x2, s->width, w->deco_left, w->deco_right, 0, scene.width),
                centre(b.y1, b.y2, s->height, w->deco_top, w->deco_bottom, floor, scene.height));
}

/* Is another mapped window's surface at (x, y) already? */
static bool corner_taken(const struct wm_window *self, int32_t x, int32_t y)
{
    for (const struct wm_window *ww = wm.first; ww; ww = ww->next)
        if (ww != self && ww->win && ww->win->x == x && ww->win->y == y)
            return true;
    return false;
}

/* The first floating place: centred in the room, then down the cascade. */
static void first_place(struct wm_window *ww)
{
    struct comp_window *w = ww->win;
    const struct comp_surface *s = ww->surface;
    int32_t fl = floor_of(ww);
    int32_t x0 = centre(0, scene.width, s->width, w->deco_left, w->deco_right, 0, scene.width);
    int32_t y0 = centre(fl, scene.height, s->height, w->deco_top, w->deco_bottom, fl,
                        scene.height);
    int32_t x = x0, y = y0;
    for (unsigned i = 1; i < CASCADE_MAX && corner_taken(ww, x, y); i++) {
        x = x0 + (int32_t)i * CASCADE_STEP;
        y = y0 + (int32_t)i * CASCADE_STEP;
        if (x + s->width + w->deco_right > scene.width ||
            y + s->height + w->deco_bottom > scene.height) {
            x = x0;
            y = y0;
            break;   /* off the output: back at the centre, on top of it */
        }
    }
    ww->float_x = x;
    ww->float_y = y;
    ww->float_placed = true;
}

/* A floating place the user can always get back to: the title bar's top
 * on the output below the floor and at least REACH_MIN of it across. */
static void keep_reachable(const struct comp_window *w, int32_t floor, int32_t *x, int32_t *y)
{
    int32_t width = w->surface->width;
    if (*y - w->deco_top < floor)
        *y = floor + w->deco_top;
    if (*y > scene.height - 1)
        *y = scene.height - 1;
    if (*x + width < REACH_MIN)
        *x = REACH_MIN - width;
    if (*x > scene.width - REACH_MIN)
        *x = scene.width - REACH_MIN;
}

static void place_floating(struct wm_window *ww)
{
    struct comp_window *w = ww->win;
    const struct comp_surface *s = ww->surface;
    if (!ww->float_placed)
        first_place(ww);
    if (ww->anchor & WM_EDGE_LEFT)
        ww->float_x = ww->anchor_x2 - s->width;
    if (ww->anchor & WM_EDGE_TOP)
        ww->float_y = ww->anchor_y2 - s->height;
    keep_reachable(w, floor_of(ww), &ww->float_x, &ww->float_y);
    window_move(w, ww->float_x, ww->float_y);
}

void wm_place(struct wm_window *ww)
{
    struct comp_window *w = ww->win;
    enum comp_layout layout = wm_layout_of(ww);
    deco_set(w, ww->shown);
    w->flags &= ~(COMP_WIN_MAXIMIZED | COMP_WIN_FULLSCREEN);
    if (ww->shown & WM_ST_FULLSCREEN) {
        w->flags |= COMP_WIN_FULLSCREEN;
        centre_in(w, deco_inner(output_box(), WM_ST_FULLSCREEN, layout), 0);
    } else if (ww->shown & WM_ST_MAXIMIZED) {
        w->flags |= COMP_WIN_MAXIMIZED;
        struct comp_box b = max_box(ww);
        centre_in(w, deco_inner(b, WM_ST_MAXIMIZED, layout), b.y1);
    } else if (layout == COMP_TILING) {
        w->tile = wm_tile(ww);
        centre_in(w, deco_inner(w->tile, 0, layout), room_of(ww).y1);
    } else {
        place_floating(ww);
    }
}

/* ---- commits ------------------------------------------------------------------------ */

/* The size the client drew while floating normally becomes the size to ask
 * for again (after a maximise, a full screen, tiling), except while a
 * resize drives it; and a resize's anchor is done once the client has
 * caught up with it. */
static void note_floating_size(struct wm_window *ww)
{
    const struct comp_surface *s = ww->surface;
    if (wm_layout_of(ww) != COMP_FLOATING || ww->want != WM_NORMAL ||
        (ww->shown & (WM_ST_MAXIMIZED | WM_ST_FULLSCREEN)) || ww->resizing)
        return;
    if (ww->anchor && (s->width != ww->float_w || s->height != ww->float_h))
        return;   /* the resize's last size isn't drawn yet */
    ww->float_w = s->width;
    ww->float_h = s->height;
    if (ww->anchor) {
        wm_place(ww);   /* at the anchor, for the last time */
        ww->anchor = 0;
    }
}

status_t wm_commit(struct wm_window *ww, uint32_t states)
{
    bool first = !ww->win;
    if (first) {
        status_t st = window_create(ww->surface, 0, 0, &ww->win);
        if (st != OK)
            return st;
        ww->win->wm = ww;
        ww->win->title = ww->title;   /* ours: it outlives the window */
        screens_window_new(ww);
    }
    ww->shown = states;
    wm_place(ww);
    note_floating_size(ww);
    if (!first)
        return OK;
    if (wm_layout_of(ww) == COMP_TILING)
        wm_relayout();   /* the others make room */
    window_map(ww->win, screens_shown(ww));   /* the seat hears (a client's first window
                                               * takes the keys) */
    if (ww->want == WM_FULLSCREEN)
        screens_fullscreen(ww, true);   /* asked before its first buffer */
    else
        anim_open(ww->win);
    desk_window_mapped(ww);
    strip_dirty();
    return OK;
}

void wm_unmap(struct wm_window *ww)
{
    if (!ww->win)
        return;
    wm_grab_forget(ww);
    if (wm.focused == ww)
        wm.focused = NULL;
    if (ww->surface->buffer && ww->overlay)
        anim_fade(ww->win);    /* the splash fades out to what is under it */
    else if (ww->surface->buffer && screens_shown(ww))
        anim_close(ww->win);   /* its picture, while its pixels are still there */
    bool tiled = wm_layout_of(ww) == COMP_TILING;
    window_destroy(ww->win);   /* the seat hears first, while it is in the order */
    ww->win = NULL;
    screens_window_gone(ww);
    strip_dirty();
    /* back to what get_toplevel made (xdg-shell's "unmapping") */
    ww->want = ww->before_fs = WM_NORMAL;
    ww->shown = 0;
    ww->float_placed = false;
    ww->float_w = ww->float_h = 0;
    ww->anchor = 0;
    ww->not_responding = false;
    if (tiled)
        wm_relayout();   /* the others take its room */
}

/* ---- what the client says ---------------------------------------------------------- */

static void copy_text(char *to, const char *from)
{
    snprintf(to, WM_TEXT_MAX, "%s", from ? from : "");
}

void wm_set_title(struct wm_window *ww, const char *title)
{
    copy_text(ww->title, title);
    if (ww->win)
        window_damage(ww->win);   /* its title bar */
    strip_dirty();                /* its chip */
}

void wm_set_app_id(struct wm_window *ww, const char *app_id)
{
    copy_text(ww->app_id, app_id);
}

void wm_set_limits(struct wm_window *ww, int32_t min_w, int32_t min_h, int32_t max_w,
                   int32_t max_h)
{
    if (ww->min_w == min_w && ww->min_h == min_h && ww->max_w == max_w && ww->max_h == max_h)
        return;
    ww->min_w = min_w;
    ww->min_h = min_h;
    ww->max_w = max_w;
    ww->max_h = max_h;
    wm_reconfigure(ww);   /* a tile's size depends on them */
}

/* ww is asked to be mode now: full screen takes it to a screen of its
 * own, and back. */
static void request(struct wm_window *ww, enum wm_mode mode)
{
    if (ww->want == mode)
        return;
    bool was_full = ww->want == WM_FULLSCREEN;
    wm_grab_forget(ww);
    ww->anchor = 0;
    ww->want = mode;
    if (ww->win && mode != WM_NORMAL)
        window_raise(ww->win);
    wm_reconfigure(ww);
    if (was_full != (mode == WM_FULLSCREEN))
        screens_fullscreen(ww, mode == WM_FULLSCREEN);
}

void wm_request_maximized(struct wm_window *ww, bool on)
{
    if (ww->want == WM_FULLSCREEN)
        ww->before_fs = on ? WM_MAXIMIZED : WM_NORMAL;   /* where full screen comes back to */
    else
        request(ww, on ? WM_MAXIMIZED : WM_NORMAL);
}

void wm_request_fullscreen(struct wm_window *ww, bool on)
{
    if (on && ww->want != WM_FULLSCREEN)
        ww->before_fs = ww->want;
    if (on)
        request(ww, WM_FULLSCREEN);
    else if (ww->want == WM_FULLSCREEN)
        request(ww, ww->before_fs);
}

void wm_set_not_responding(struct wm_window *ww, bool on)
{
    if (ww->not_responding == on)
        return;
    ww->not_responding = on;
    if (!ww->win)
        return;
    ww->win->flags = on ? ww->win->flags | COMP_WIN_UNRESPONSIVE
                        : ww->win->flags & ~COMP_WIN_UNRESPONSIVE;
    window_damage(ww->win);   /* its title bar says so, or not */
}

/* ---- the user's side ---------------------------------------------------------------- */

void wm_close(struct comp_window *w)
{
    struct wm_window *ww = w ? w->wm : NULL;
    if (ww && ww->ops && ww->ops->close)
        ww->ops->close(ww->ctx);   /* the client decides; a ping asks if it is alive */
}

void wm_toggle_fullscreen(struct comp_window *w)
{
    struct wm_window *ww = w ? w->wm : NULL;
    if (ww)
        wm_request_fullscreen(ww, ww->want != WM_FULLSCREEN);
}

void wm_set_layout(enum comp_layout layout)
{
    struct desk_screen *s = screens_cur();
    bool same = s->kind != SCREEN_NORMAL || s->layout == layout;
    screens_set_default(layout);
    if (same)
        return;
    wm_grab_cancel();
    for (struct wm_window *ww = wm.first; ww; ww = ww->next)
        ww->anchor = 0;
    wm_relayout();
    strip_dirty();   /* its icon */
}

/* Super+T: the current screen's arrangement, which new screens take too. */
void wm_toggle_layout(void)
{
    struct desk_screen *s = screens_cur();
    if (s->kind != SCREEN_NORMAL)
        return;   /* a full-screen screen has none */
    enum comp_layout l = s->layout == COMP_TILING ? COMP_FLOATING : COMP_TILING;
    wm_set_layout(l);
    ctl_layout_changed(l);
}

void wm_minimise(struct comp_window *w)
{
    struct wm_window *ww = w ? w->wm : NULL;
    if (ww)
        screens_minimise(ww);
}

/* ctl.c's tells init (comp.h); a build without it (a test's) tells nobody. */
__attribute__((weak)) void ctl_layout_changed(enum comp_layout layout)
{
    (void)layout;
}

const char *wm_layout_name(enum comp_layout layout)
{
    return layout == COMP_TILING ? "tiling" : "floating";
}

bool wm_layout_parse(const char *s, enum comp_layout *out)
{
    if (!strcmp(s, "floating"))
        *out = COMP_FLOATING;
    else if (!strcmp(s, "tiling"))
        *out = COMP_TILING;
    else
        return false;
    return true;
}

/* ---- focus -------------------------------------------------------------------------- */

void wm_focus_changed(struct comp_window *w)
{
    struct wm_window *now = w ? w->wm : NULL, *old = wm.focused;
    if (w)
        window_raise(w);   /* Alt+Tab and a first window come up too, not only a click */
    if (now)
        now->focused_at = ++wm.focus_count;
    if (now == old)
        return;
    wm.focused = now;
    strip_dirty();   /* the focused chip */
    if (old && old->win)
        wm_reconfigure(old);   /* activated no more (the seat damaged its title bar) */
    if (now && now->win)
        wm_reconfigure(now);
}

void wm_clicked(struct comp_window *w)
{
    window_raise(w);   /* wm_focus_changed did, if the focus moved; a click raises anyway */
}

/* Can the keys go to w: mapped, its client still there, no boot overlay. */
static bool cyclable(const struct comp_window *w)
{
    const struct jwl_conn *c = w->surface->client->conn;
    return (w->flags & COMP_WIN_MAPPED) && !(w->flags & COMP_WIN_OVERLAY) && c &&
           c->status == OK;
}

/* Alt+Tab's order into order[]: the toplevels as they opened, then the
 * other windows bottom up. Its length. */
static unsigned cycle_order(struct comp_window **order)
{
    unsigned n = 0;
    for (struct wm_window *ww = wm.first; ww && n < CYCLE_MAX; ww = ww->next)
        if (ww->win && cyclable(ww->win))
            order[n++] = ww->win;
    for (struct comp_window *w = scene.bottom; w && n < CYCLE_MAX; w = w->above)
        if (!w->wm && cyclable(w))
            order[n++] = w;
    return n;
}

struct comp_window *wm_cycle(struct comp_window *from, bool backward)
{
    static struct comp_window *order[CYCLE_MAX];
    unsigned n = cycle_order(order), at = n;
    for (unsigned i = 0; i < n; i++)
        if (order[i] == from)
            at = i;
    if (at == n)   /* from isn't in it (none focused): the first, or the last */
        return n ? order[backward ? n - 1 : 0] : NULL;
    if (n < 2)
        return NULL;   /* nothing else to go to */
    return order[backward ? (at + n - 1) % n : (at + 1) % n];
}
