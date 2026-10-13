/* Decorations' geometry (wm.h, comp.h): how big each side is, and what a
 * point on or near a window's frame is. Drawing them is title.c's, and so
 * are the title bar's and its circles' boxes (title_bar_box,
 * title_button_hit): a click lands exactly where the drawing is.
 *
 * The frame of a normal floating window, around its surface S (its window
 * geometry, comp.h's window_geometry: the whole surface unless the client
 * set one):
 *
 *     +--------------------------------------------+  <- top edge: the title
 *     | (x)(-)(f)          title                    |     bar's first DECO_BORDER rows
 *     +--------------------------------------------+
 *     |                                            |
 *     |                     S                      |  <- the outline: DECO_OUTLINE
 *     |                                            |
 *     +--------------------------------------------+
 *
 * (look.h rounds its corners and gives it a shadow; neither changes what a
 * press is on.) The circles close, minimise and make full screen. On a
 * window that can be resized (floating, normal, wm_resizable) the outline,
 * and DECO_GRAB pixels around the frame, resize it: within DECO_CORNER of
 * a corner, both ways. The title bar moves it. Maximised windows have the
 * title bar only and full-screen ones nothing, so neither has edges.
 *
 * Tiling (the owner's look): no title bar at all, a DECO_BORDER border on
 * all four sides, whose colour shows the focus (title.c draws a top strip
 * thinner than a title bar as border); Super+Q closes the focused window,
 * there being no close circle. A press on a tiled window's border focuses
 * it and does nothing more.
 *
 * A client that draws its own decorations (WM_ST_CSD: one that never
 * bound xdg-decoration's manager, xdgdeco.c) gets none of these but
 * tiling's border: floating, only the rounded corners and the shadow
 * (COMP_WIN_CSD, look.h's LOOK_CSD), so its frame is its window geometry,
 * and it moves and resizes itself (xdg_toplevel.move and resize;
 * Super+drag still works). One whose window geometry is smaller than its
 * surface (it draws its own shadow round its window: window_inset) gets
 * neither our corners nor our shadow, only its own (LOOK_PLAIN). A press
 * on its surface outside the frame (on its own shadow, where a client
 * keeps its resize margins) is the client's, by its input region. */
#include "wm.h"

#define DECO_CORNER 16   /* this close to a corner, an edge is a corner */

struct deco_sizes deco_sizes(uint32_t states, enum comp_layout layout)
{
    if (states & WM_ST_FULLSCREEN)
        return (struct deco_sizes){ 0, 0, 0, 0 };
    if (layout == COMP_TILING)   /* no title bar: a border all round */
        return (struct deco_sizes){ DECO_BORDER, DECO_BORDER, DECO_BORDER, DECO_BORDER };
    if (states & WM_ST_CSD)   /* the client's own, inside its surface */
        return (struct deco_sizes){ 0, 0, 0, 0 };
    if (states & WM_ST_MAXIMIZED)
        return (struct deco_sizes){ COMP_TITLE_H, 0, 0, 0 };
    return (struct deco_sizes){ COMP_TITLE_H, DECO_OUTLINE, DECO_OUTLINE, DECO_OUTLINE };
}

struct comp_box deco_inner(struct comp_box frame, uint32_t states, enum comp_layout layout)
{
    struct deco_sizes d = deco_sizes(states, layout);
    return (struct comp_box){ frame.x1 + d.left, frame.y1 + d.top, frame.x2 - d.right,
                              frame.y2 - d.bottom };
}

void deco_set(struct comp_window *w, uint32_t states)
{
    enum comp_layout layout = wm_layout_of(w->wm);
    struct deco_sizes d = deco_sizes(states, layout);
    bool csd = (states & WM_ST_CSD) && layout == COMP_FLOATING &&
               !(states & (WM_ST_MAXIMIZED | WM_ST_FULLSCREEN)) &&
               !window_inset(w);   /* its own shadow round it: none of ours (LOOK_PLAIN) */
    if (w->deco_top == d.top && w->deco_left == d.left && w->deco_right == d.right &&
        w->deco_bottom == d.bottom && csd == !!(w->flags & COMP_WIN_CSD))
        return;
    window_damage(w);
    w->deco_top = d.top;
    w->deco_left = d.left;
    w->deco_right = d.right;
    w->deco_bottom = d.bottom;
    w->flags = csd ? w->flags | COMP_WIN_CSD : w->flags & ~COMP_WIN_CSD;
    window_damage(w);
}

/* Can the user resize w by its edges now? */
static bool has_edges(const struct comp_window *w)
{
    const struct wm_window *ww = w->wm;
    return ww && w->deco_left > 0 && wm_layout_of(ww) == COMP_FLOATING &&
           ww->want == WM_NORMAL && wm_resizable(ww);
}

/* The box presses on w's decorations land in: its frame, and the grab
 * margin around it when its edges resize. */
static struct comp_box press_box(const struct comp_window *w)
{
    struct comp_box f = window_frame(w);
    if (!has_edges(w) || box_empty(f))
        return f;
    return (struct comp_box){ f.x1 - DECO_GRAB, f.y1 - DECO_GRAB, f.x2 + DECO_GRAB,
                              f.y2 + DECO_GRAB };
}

/* Which edges (x, y) is on: the side, and a corner's second side near the
 * frame's ends. 0 when it is on none. */
static uint32_t edges_at(const struct comp_window *w, int32_t x, int32_t y)
{
    struct comp_box f = window_frame(w), s = window_geometry(w);
    if (!has_edges(w))
        return 0;
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
    *edges = 0;
    if (box_contains(window_geometry(w), x, y))
        return DECO_SURFACE;
    if (!box_contains(press_box(w), x, y))   /* past the frame: its own shadow, or nothing */
        return box_contains(window_surface_box(w), x, y) ? DECO_SURFACE : DECO_NONE;
    if ((*edges = edges_at(w, x, y)))
        return DECO_EDGE;
    static const enum deco_part parts[TITLE_BUTTONS] = { DECO_CLOSE, DECO_MINIMISE,
                                                        DECO_FULLSCREEN };
    enum title_button b = title_button_at(w, x, y);
    if (b != TITLE_NONE)
        return parts[b];
    return box_contains(title_bar_box(w), x, y) ? DECO_TITLE : DECO_NONE;
}

enum cursor_shape wm_cursor_at(int32_t x, int32_t y)
{
    bool on_surface;
    uint32_t e;
    enum cursor_shape gap = wm_gap_cursor(x, y);
    if (gap != CURSOR_SHAPES)
        return gap;   /* a gap between tiles: its resize arrows */
    struct comp_window *w = wm_window_at(x, y, &on_surface);
    if (!w || on_surface)
        return CURSOR_ARROW;
    switch (deco_hit(w, x, y, &e)) {
    case DECO_CLOSE:
    case DECO_MINIMISE:
    case DECO_FULLSCREEN:
        return CURSOR_HAND;
    case DECO_EDGE:
        if (!(e & (WM_EDGE_TOP | WM_EDGE_BOTTOM)))
            return CURSOR_RESIZE_EW;
        if (!(e & (WM_EDGE_LEFT | WM_EDGE_RIGHT)))
            return CURSOR_RESIZE_NS;
        if (((e & WM_EDGE_TOP) && (e & WM_EDGE_LEFT)) ||
            ((e & WM_EDGE_BOTTOM) && (e & WM_EDGE_RIGHT)))
            return CURSOR_RESIZE_NWSE;
        return CURSOR_RESIZE_NESW;
    default:
        return CURSOR_ARROW;
    }
}

/* Does w's surface take input at output (x, y) (its input region)? */
static bool takes_input(const struct comp_window *w, int32_t x, int32_t y)
{
    const struct comp_surface *s = w->surface;
    return s->input_all ||
           region_contains(&s->input, x - window_shown_x(w), y - window_shown_y(w));
}

struct comp_window *wm_window_at(int32_t x, int32_t y, bool *on_surface)
{
    for (struct comp_window *w = scene.top; w; w = w->below) {
        if (!(w->flags & COMP_WIN_MAPPED))
            continue;
        /* the frame round the window geometry is ours; the surface past
         * the frame (a client's own shadow) is the client's, as inside */
        bool on_deco = box_contains(press_box(w), x, y) &&
                       !box_contains(window_geometry(w), x, y);
        if (on_deco) {
            *on_surface = false;
            return w;   /* its decorations */
        }
        if (box_contains(window_surface_box(w), x, y) && takes_input(w, x, y)) {
            *on_surface = true;
            return w;
        }
    }
    *on_surface = false;
    return NULL;
}
