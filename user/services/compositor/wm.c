/* The window manager (wm.h, comp.h): which toplevels there are, what each
 * is asked to be, where each goes, and the focus's side of it (raising,
 * the activated state, Alt+Tab's order). Policy only: the scene (scene.c)
 * keeps and damages, xdg-shell (xdgtop.c) speaks to the client, the seat
 * (focus.c) owns the keyboard focus and tells us when it moves.
 *
 * Placement, for the states the shown buffer was drawn for:
 *   - full screen: centred on the output (a smaller buffer shows the
 *     background around it, never stretched);
 *   - maximised: centred in the output below its title bar;
 *   - tiling: centred in its tile (a resizable window was asked to fill
 *     it; one that can't resize keeps its own size, wholly on the screen
 *     where it fits);
 *   - floating: at its floating place. The first time it is centred on the
 *     output, moved down and right by a title bar's height while another
 *     window's top-left corner is already there (the cascade).
 *
 * The first-window rule (G1-PLAN, "Focus"): a client's first window takes
 * the keys when it appears; its later ones don't, so a program can't keep
 * stealing them. The count is the client's (client_maps), so a client
 * that closed its first window doesn't get the rule again. */
#include "wm.h"

#define CASCADE_STEP  (DECO_TITLE_H + DECO_BORDER)   /* one cascade step, both ways */
#define CASCADE_MAX   16u                            /* steps before it starts over */
#define REACH_MIN     48   /* a floating title bar keeps this much on the output */

struct comp_wm_hooks comp_wm_hooks;

static struct {
    struct wm_window *first, *last;   /* every toplevel, oldest first */
    struct wm_window *focused;        /* the seat's focus, as wm_focus_changed said */
} wm;

void wm_init(enum comp_layout layout)
{
    wm_grab_cancel();
    wm.first = wm.last = wm.focused = NULL;
    scene.layout = layout;
}

struct wm_window *wm_first(void)
{
    return wm.first;
}

/* ---- making and losing toplevels ---------------------------------------------------- */

struct wm_window *wm_create(struct comp_surface *s, const struct wm_ops *ops, void *ctx,
                            uint32_t *client_maps)
{
    struct wm_window *ww = calloc(1, sizeof(*ww));
    if (!ww)
        return NULL;
    ww->surface = s;
    ww->ops = ops;
    ww->ctx = ctx;
    ww->client_maps = client_maps;
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

void wm_wanted(const struct wm_window *ww, struct wm_config *out)
{
    *out = (struct wm_config){ 0, 0, ww == wm.focused ? WM_ST_ACTIVATED : 0 };
    struct comp_box b;
    switch (ww->want) {
    case WM_FULLSCREEN:
        out->states |= WM_ST_FULLSCREEN;
        b = deco_inner(output_box(), WM_ST_FULLSCREEN);
        break;
    case WM_MAXIMIZED:
        out->states |= WM_ST_MAXIMIZED;
        b = deco_inner(output_box(), WM_ST_MAXIMIZED);
        break;
    default:
        if (scene.layout == COMP_TILING) {
            if (!wm_resizable(ww))
                return;   /* 0 by 0: it keeps its own size */
            b = deco_inner(wm_tile(ww), 0);
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
 * [lo, hi), then kept on [0, out) with its decorations (before, after)
 * where the frame fits, else at the start. */
static int32_t centre(int32_t lo, int32_t hi, int32_t side, int32_t before, int32_t after,
                      int32_t out)
{
    int32_t v = lo + (hi - lo - side) / 2;
    if (side + before + after > out)
        return before;
    if (v - before < 0)
        v = before;
    if (v + side + after > out)
        v = out - side - after;
    return v;
}

static void centre_in(struct comp_window *w, struct comp_box b)
{
    const struct comp_surface *s = w->surface;
    window_move(w, centre(b.x1, b.x2, s->width, w->deco_left, w->deco_right, scene.width),
                centre(b.y1, b.y2, s->height, w->deco_top, w->deco_bottom, scene.height));
}

/* Is another mapped window's surface at (x, y) already? */
static bool corner_taken(const struct wm_window *self, int32_t x, int32_t y)
{
    for (const struct wm_window *ww = wm.first; ww; ww = ww->next)
        if (ww != self && ww->win && ww->win->x == x && ww->win->y == y)
            return true;
    return false;
}

/* The first floating place: centred, then down the cascade. */
static void first_place(struct wm_window *ww)
{
    struct comp_window *w = ww->win;
    const struct comp_surface *s = ww->surface;
    int32_t x0 = centre(0, scene.width, s->width, w->deco_left, w->deco_right, scene.width);
    int32_t y0 = centre(0, scene.height, s->height, w->deco_top, w->deco_bottom, scene.height);
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
 * on the output and at least REACH_MIN of it across. */
static void keep_reachable(const struct comp_window *w, int32_t *x, int32_t *y)
{
    int32_t width = w->surface->width;
    if (*y - w->deco_top < 0)
        *y = w->deco_top;
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
    keep_reachable(w, &ww->float_x, &ww->float_y);
    window_move(w, ww->float_x, ww->float_y);
}

void wm_place(struct wm_window *ww)
{
    struct comp_window *w = ww->win;
    deco_set(w, ww->shown);
    w->flags &= ~(COMP_WIN_MAXIMIZED | COMP_WIN_FULLSCREEN);
    if (ww->shown & WM_ST_FULLSCREEN) {
        w->flags |= COMP_WIN_FULLSCREEN;
        centre_in(w, deco_inner(output_box(), WM_ST_FULLSCREEN));
    } else if (ww->shown & WM_ST_MAXIMIZED) {
        w->flags |= COMP_WIN_MAXIMIZED;
        centre_in(w, deco_inner(output_box(), WM_ST_MAXIMIZED));
    } else if (scene.layout == COMP_TILING) {
        w->tile = wm_tile(ww);
        centre_in(w, deco_inner(w->tile, 0));
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
    if (scene.layout != COMP_FLOATING || ww->want != WM_NORMAL ||
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
    }
    ww->shown = states;
    wm_place(ww);
    note_floating_size(ww);
    if (!first)
        return OK;
    window_map(ww->win, true);
    bool take_focus = (*ww->client_maps)++ == 0;
    if (scene.layout == COMP_TILING)
        wm_relayout();   /* the others make room */
    if (comp_wm_hooks.mapped)
        comp_wm_hooks.mapped(ww->win, take_focus);
    return OK;
}

void wm_unmap(struct wm_window *ww)
{
    if (!ww->win)
        return;
    if (comp_wm_hooks.unmapping)
        comp_wm_hooks.unmapping(ww->win);
    wm_grab_forget(ww);
    if (wm.focused == ww)
        wm.focused = NULL;
    window_destroy(ww->win);
    ww->win = NULL;
    /* back to what get_toplevel made (xdg-shell's "unmapping") */
    ww->want = ww->before_fs = WM_NORMAL;
    ww->shown = 0;
    ww->float_placed = false;
    ww->float_w = ww->float_h = 0;
    ww->anchor = 0;
    ww->not_responding = false;
    if (scene.layout == COMP_TILING)
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
        scene_damage(deco_title_bar(ww->win));
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

/* ww is asked to be mode now. */
static void request(struct wm_window *ww, enum wm_mode mode)
{
    if (ww->want == mode)
        return;
    wm_grab_forget(ww);
    ww->anchor = 0;
    ww->want = mode;
    if (ww->win && mode != WM_NORMAL)
        window_raise(ww->win);
    wm_reconfigure(ww);
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
    if (ww->win)
        scene_damage(deco_title_bar(ww->win));
}

/* ---- the user's side ---------------------------------------------------------------- */

void wm_toggle_fullscreen(struct comp_window *w)
{
    struct wm_window *ww = w ? w->wm : NULL;
    if (ww)
        wm_request_fullscreen(ww, ww->want != WM_FULLSCREEN);
}

void wm_set_layout(enum comp_layout layout)
{
    if (scene.layout == layout)
        return;
    wm_grab_cancel();
    scene.layout = layout;
    for (struct wm_window *ww = wm.first; ww; ww = ww->next)
        ww->anchor = 0;
    wm_relayout();
}

void wm_toggle_layout(void)
{
    enum comp_layout l = scene.layout == COMP_TILING ? COMP_FLOATING : COMP_TILING;
    wm_set_layout(l);
    if (comp_wm_hooks.layout_changed)
        comp_wm_hooks.layout_changed(l);
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
    if (now == old)
        return;
    wm.focused = now;
    if (old && old->win) {
        scene_damage(deco_title_bar(old->win));   /* its title bar dims */
        wm_reconfigure(old);
    }
    if (now && now->win) {
        window_raise(now->win);
        wm_reconfigure(now);
    }
}

struct comp_window *wm_cycle(const struct comp_window *from, bool backwards)
{
    const struct wm_window *start = from ? from->wm : NULL;
    const struct wm_window *ww = start;
    for (unsigned n = 0; n <= COMP_CLIENTS_MAX * COMP_SURFACES_MAX; n++) {
        if (backwards)
            ww = ww && ww->prev ? ww->prev : wm.last;
        else
            ww = ww && ww->next ? ww->next : wm.first;
        if (!ww || (ww == start && n))
            return NULL;   /* round once: nothing else is mapped */
        if (ww->win && ww != start)
            return ww->win;
    }
    return NULL;
}

struct comp_window *wm_focus_successor(const struct comp_window *w)
{
    for (struct comp_window *b = w ? w->below : NULL; b; b = b->below)
        if (b->flags & COMP_WIN_MAPPED)
            return b;
    for (struct comp_window *t = scene.top; t; t = t->below)
        if (t != w && (t->flags & COMP_WIN_MAPPED))
            return t;
    return NULL;
}
