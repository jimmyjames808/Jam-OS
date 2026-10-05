/* Decorations' geometry (wm.h, comp.h): how big each side is, where the
 * title bar and its close box are, and what a point on a window's frame
 * is. Drawing them is painting's (paint.c); this file only says where.
 *
 * The frame of a normal window, around its surface S:
 *
 *     +--------------------------------------+---+   <- top edge: the title
 *     |  title bar (DECO_TITLE_H)            | x |      bar's first DECO_BORDER rows
 *     +-+----------------------------------+-+---+
 *     | |                                  | |
 *     | |                S                 | |      borders: DECO_BORDER
 *     | |                                  | |
 *     +-+----------------------------------+-+
 *
 * The close box is the title bar's last DECO_TITLE_H pixels above the
 * surface. A border (and a corner: DECO_CORNER pixels from one along
 * either side) resizes; the title bar moves. Maximised windows have the
 * title bar only and full-screen ones nothing, so neither has edges. */
#include "wm.h"

#define DECO_CORNER 16   /* this close to a corner, an edge is a corner */

struct deco_sizes deco_sizes(uint32_t states)
{
    if (states & WM_ST_FULLSCREEN)
        return (struct deco_sizes){ 0, 0, 0, 0 };
    if (states & WM_ST_MAXIMIZED)
        return (struct deco_sizes){ DECO_TITLE_H, 0, 0, 0 };
    return (struct deco_sizes){ DECO_TITLE_H, DECO_BORDER, DECO_BORDER, DECO_BORDER };
}

struct comp_box deco_inner(struct comp_box frame, uint32_t states)
{
    struct deco_sizes d = deco_sizes(states);
    return (struct comp_box){ frame.x1 + d.left, frame.y1 + d.top, frame.x2 - d.right,
                              frame.y2 - d.bottom };
}

void deco_set(struct comp_window *w, uint32_t states)
{
    struct deco_sizes d = deco_sizes(states);
    if (w->deco_top == d.top && w->deco_left == d.left && w->deco_right == d.right &&
        w->deco_bottom == d.bottom)
        return;
    window_damage(w);
    w->deco_top = d.top;
    w->deco_left = d.left;
    w->deco_right = d.right;
    w->deco_bottom = d.bottom;
    window_damage(w);
}

struct comp_box deco_title_bar(const struct comp_window *w)
{
    struct comp_box f = window_frame(w);
    if (w->deco_top <= 0 || box_empty(f))
        return (struct comp_box){ 0, 0, 0, 0 };
    return (struct comp_box){ f.x1, f.y1, f.x2, w->y };
}

struct comp_box deco_close_box(const struct comp_window *w)
{
    struct comp_box t = deco_title_bar(w);
    if (box_empty(t))
        return t;
    int32_t right = w->x + w->surface->width;
    int32_t left = right - w->deco_top;
    return (struct comp_box){ left > t.x1 ? left : t.x1, t.y1, right, t.y2 };
}

const char *window_title(const struct comp_window *w)
{
    const struct wm_window *ww = w->wm;
    return ww ? ww->title : "";
}

bool window_not_responding(const struct comp_window *w)
{
    const struct wm_window *ww = w->wm;
    return ww && ww->not_responding;
}

/* Which edges (x, y) on w's border is: the side it is on, and a corner's
 * second side near the ends. 0 when it isn't on one. */
static uint32_t edges_at(const struct comp_window *w, struct comp_box f, struct comp_box s,
                         int32_t x, int32_t y)
{
    if (w->deco_left <= 0)
        return 0;   /* only a normal window has borders */
    uint32_t e = 0;
    if (x < s.x1)
        e |= WM_EDGE_LEFT;
    else if (x >= s.x2)
        e |= WM_EDGE_RIGHT;
    if (y >= s.y2)
        e |= WM_EDGE_BOTTOM;
    else if (y < f.y1 + DECO_BORDER)
        e |= WM_EDGE_TOP;
    if (!e)
        return 0;
    if ((e & (WM_EDGE_LEFT | WM_EDGE_RIGHT)) && !(e & (WM_EDGE_TOP | WM_EDGE_BOTTOM)))
        e |= y < f.y1 + DECO_CORNER ? WM_EDGE_TOP : y >= f.y2 - DECO_CORNER ? WM_EDGE_BOTTOM : 0;
    if ((e & (WM_EDGE_TOP | WM_EDGE_BOTTOM)) && !(e & (WM_EDGE_LEFT | WM_EDGE_RIGHT)))
        e |= x < f.x1 + DECO_CORNER ? WM_EDGE_LEFT : x >= f.x2 - DECO_CORNER ? WM_EDGE_RIGHT : 0;
    return e;
}

enum deco_part deco_hit(const struct comp_window *w, int32_t x, int32_t y, uint32_t *edges)
{
    struct comp_box f = window_frame(w), s = window_surface_box(w);
    *edges = 0;
    if (!box_contains(f, x, y))
        return DECO_NONE;
    if (box_contains(s, x, y))
        return DECO_SURFACE;
    if ((*edges = edges_at(w, f, s, x, y)))
        return DECO_EDGE;
    if (box_contains(deco_close_box(w), x, y))
        return DECO_CLOSE;
    return box_contains(deco_title_bar(w), x, y) ? DECO_TITLE : DECO_NONE;
}

/* Does w's surface take input at output (x, y) (its input region)? */
static bool takes_input(const struct comp_window *w, int32_t x, int32_t y)
{
    const struct comp_surface *s = w->surface;
    return s->input_all || region_contains(&s->input, x - w->x, y - w->y);
}

struct comp_window *wm_window_at(int32_t x, int32_t y, bool *on_surface)
{
    for (struct comp_window *w = scene.top; w; w = w->below) {
        if (!(w->flags & COMP_WIN_MAPPED) || !box_contains(window_frame(w), x, y))
            continue;
        if (!box_contains(window_surface_box(w), x, y)) {
            *on_surface = false;
            return w;   /* its decorations */
        }
        if (takes_input(w, x, y)) {
            *on_surface = true;
            return w;
        }
    }
    *on_surface = false;
    return NULL;
}
