/* Title bars, their circles, outlines and borders: drawing them (comp.h,
 * paint.h; look.h has every colour and size).
 *
 * A window's decorations are the strips of its frame around its surface,
 * as wide as the window manager made them (struct comp_window's deco_*):
 *   - the top strip, if it is COMP_TITLE_H or more, is the title bar: the
 *     three circles on its left (close, minimise, full screen; grey on an
 *     unfocused window; each with its symbol while the pointer is over
 *     them), and the title centred, "(not responding)" after it when the
 *     window didn't answer a ping, cut short with "..." where it would
 *     reach the circles' side or the same distance from the right end;
 *   - the other strips, and a thinner top one (tiling mode's windows have
 *     no title bar, a border all round), are the outline or the border;
 *     a floating window's outline also runs along its title bar's top
 *     and sides.
 * Every pixel of a strip is drawn, and opaquely, so paint.c may treat
 * decorations as hiding what is below (but for the rounded corners,
 * shape.c's, which it asks about).
 *
 * Drawing happens tile by tile on the painting workers: each call draws
 * only what falls in the tile (libfun's fill and text clip to the tile's
 * buffer, which stands for its box of the output). A title can't draw
 * outside its bar: the text is cut before the circles' side and clipped
 * to the bar's rows. libfun's glyph tables and the masks are made before
 * the workers start (paint_init), so the workers only read them. */
#include <fun.h>
#include "paint.h"

#define TITLE_MAX 300   /* bytes of title drawn: the window manager keeps at most 256 */

const struct comp_window *title_hovered;

static const uint32_t btn_colour[TITLE_BUTTONS] = { LOOK_CLOSE, LOOK_MINIMISE, LOOK_FULLSCREEN };
static const uint32_t btn_ink[TITLE_BUTTONS] = { LOOK_CLOSE_INK, LOOK_MINIMISE_INK,
                                                 LOOK_FULLSCREEN_INK };

/* ---- where things are --------------------------------------------------------------- */

struct comp_box title_bar_box(const struct comp_window *w)
{
    struct comp_box f = window_frame(w), s = window_surface_box(w);
    if (w->deco_top < COMP_TITLE_H || box_empty(s))
        return (struct comp_box){ 0, 0, 0, 0 };   /* none, or a top border (tiling's) */
    return (struct comp_box){ f.x1, f.y1, f.x2, s.y1 };
}

struct comp_box title_button_box(const struct comp_window *w, enum title_button b)
{
    struct comp_box bar = title_bar_box(w);
    if (box_empty(bar) || b >= TITLE_BUTTONS)
        return (struct comp_box){ 0, 0, 0, 0 };
    int32_t x = bar.x1 + LOOK_BTN_LEFT + (int32_t)b * (LOOK_BTN_D + LOOK_BTN_GAP);
    int32_t y = bar.y1 + LOOK_BTN_TOP;
    if (x + LOOK_BTN_D > bar.x2 - LOOK_BTN_LEFT)
        return (struct comp_box){ 0, 0, 0, 0 };   /* a bar too short for it */
    return (struct comp_box){ x, y, x + LOOK_BTN_D, y + LOOK_BTN_D };
}

struct comp_box title_button_hit(const struct comp_window *w, enum title_button b)
{
    struct comp_box c = title_button_box(w, b);
    if (box_empty(c))
        return c;
    return (struct comp_box){ c.x1 - LOOK_BTN_HIT, c.y1 - LOOK_BTN_HIT, c.x2 + LOOK_BTN_HIT,
                              c.y2 + LOOK_BTN_HIT };
}

enum title_button title_button_at(const struct comp_window *w, int32_t x, int32_t y)
{
    for (int b = 0; b < TITLE_BUTTONS; b++)
        if (box_contains(title_button_hit(w, (enum title_button)b), x, y))
            return (enum title_button)b;
    return TITLE_NONE;
}

struct comp_box title_close_box(const struct comp_window *w)
{
    return title_button_box(w, TITLE_CLOSE);
}

struct comp_box title_buttons_hit(const struct comp_window *w)
{
    struct comp_box all = { 0, 0, 0, 0 };
    for (int b = 0; b < TITLE_BUTTONS; b++)
        all = box_bounds(all, title_button_hit(w, (enum title_button)b));
    return all;
}

uint32_t title_edge_colour(const struct comp_window *w)
{
    bool focused = w->flags & COMP_WIN_FOCUSED;
    if (look_of(w) == LOOK_TILED)
        return focused ? LOOK_TILE_FOCUSED : LOOK_TILE;
    return focused ? LOOK_OUTLINE_FOCUSED : LOOK_OUTLINE;
}

/* ---- drawing ------------------------------------------------------------------------ */

/* b on the output as a rectangle of t's buffer (struct surf's coordinates). */
static struct rect in_tile(const struct tile_buf *t, struct comp_box b)
{
    return (struct rect){ b.x1 - t->b.x1, b.y1 - t->b.y1, b.x2 - b.x1, b.y2 - b.y1 };
}

/* Box b filled with colour c where it meets t. */
static void fill_box(const struct tile_buf *t, const struct surf *s, struct comp_box b, uint32_t c)
{
    if (box_empty(box_intersect(b, t->b)))
        return;
    struct rect r = in_tile(t, b);
    fill_rect(s, &r, c);
}

/* A mask the size of a circle (LOOK_BTN_D square) at box c, in colour col,
 * over what is in t. */
static void draw_mask(const struct tile_buf *t, struct comp_box c, const uint8_t *mask,
                      uint32_t col)
{
    struct comp_box in = box_intersect(c, t->b);
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint32_t *row = tile_row(t, y) - t->b.x1;
        const uint8_t *m = mask + (y - c.y1) * LOOK_BTN_D - c.x1;
        for (int32_t x = in.x1; x < in.x2; x++)
            if (m[x])
                row[x] = paint_mix(row[x], col, m[x]);
    }
}

/* The three circles where they meet t. */
static void draw_buttons(const struct comp_window *w, const struct tile_buf *t)
{
    bool hover = title_hovered == w, lit = hover || (w->flags & COMP_WIN_FOCUSED);
    for (int b = 0; b < TITLE_BUTTONS; b++) {
        struct comp_box c = title_button_box(w, (enum title_button)b);
        if (box_empty(box_intersect(c, t->b)))
            continue;
        draw_mask(t, c, mask_disc, lit ? btn_colour[b] : LOOK_BTN_IDLE);
        if (hover)
            draw_mask(t, c, mask_symbol[b], btn_ink[b]);
    }
}

/* The title's text at r (r's top left, r->w wide at most, in s's
 * coordinates): the one place the font is chosen. */
static void title_text(const struct surf *s, const struct rect *r, bool focused, const char *str)
{
    text_clip(s, r, 1, focused ? LOOK_TITLE_FOCUSED : LOOK_TITLE, str);
}

/* The title, centred in the bar between the circles' side and as much on
 * the right, where it meets t. */
static void draw_title(const struct comp_window *w, const struct tile_buf *t,
                       const struct surf *s, struct comp_box bar)
{
    char title[TITLE_MAX];
    snprintf(title, sizeof(title), "%s%s", w->title ? w->title : "",
             w->flags & COMP_WIN_UNRESPONSIVE ? " (not responding)" : "");
    int32_t side = LOOK_BTNS_W + LOOK_TEXT_PAD, room = bar.x2 - bar.x1 - 2 * side;
    int32_t width = text_width(1, title);
    if (room <= 0 || !title[0])
        return;
    int32_t x = width < room ? bar.x1 + (bar.x2 - bar.x1 - width) / 2 : bar.x1 + side;
    struct comp_box text = { x, bar.y1 + (bar.y2 - bar.y1 - TEXT_H(1)) / 2, x + room, 0 };
    text.y2 = text.y1 + TEXT_H(1);
    if (box_empty(box_intersect(text, t->b)))
        return;
    /* Clipped to the bar's rows too: a bar shorter than the font. */
    struct comp_box rows = box_intersect(t->b, bar);
    struct surf clip = { tile_row(t, rows.y1), s->w, rows.y2 - rows.y1, s->stride };
    struct rect r = { text.x1 - t->b.x1, text.y1 - rows.y1, width < room ? width : room,
                      TEXT_H(1) };
    title_text(&clip, &r, w->flags & COMP_WIN_FOCUSED, title);
}

/* The title bar where it meets t: its colour, a floating window's outline
 * along its top and sides, the title, the circles. */
static void draw_bar(const struct comp_window *w, const struct tile_buf *t, const struct surf *s)
{
    struct comp_box bar = title_bar_box(w);
    if (box_empty(box_intersect(bar, t->b)))
        return;
    bool focused = w->flags & COMP_WIN_FOCUSED;
    fill_box(t, s, bar, focused ? LOOK_BAR_FOCUSED : LOOK_BAR);
    if (look_of(w) == LOOK_FLOATING) {
        uint32_t e = title_edge_colour(w);
        fill_box(t, s, (struct comp_box){ bar.x1, bar.y1, bar.x2, bar.y1 + DECO_OUTLINE }, e);
        fill_box(t, s, (struct comp_box){ bar.x1, bar.y1, bar.x1 + DECO_OUTLINE, bar.y2 }, e);
        fill_box(t, s, (struct comp_box){ bar.x2 - DECO_OUTLINE, bar.y1, bar.x2, bar.y2 }, e);
    }
    draw_title(w, t, s, bar);
    draw_buttons(w, t);
}

void title_draw(const struct comp_window *w, const struct tile_buf *t)
{
    if (look_of(w) == LOOK_PLAIN)
        return;
    struct comp_box f = window_frame(w), sb = window_surface_box(w);
    int32_t tw = t->b.x2 - t->b.x1;
    struct surf s = { t->px, tw, t->b.y2 - t->b.y1, tw };
    uint32_t edge = title_edge_colour(w);
    draw_bar(w, t, &s);
    if (box_empty(title_bar_box(w)))   /* a top strip thinner than a title bar: a border */
        fill_box(t, &s, (struct comp_box){ f.x1, f.y1, f.x2, sb.y1 }, edge);
    fill_box(t, &s, (struct comp_box){ f.x1, sb.y1, sb.x1, sb.y2 }, edge);   /* left */
    fill_box(t, &s, (struct comp_box){ sb.x2, sb.y1, f.x2, sb.y2 }, edge);   /* right */
    fill_box(t, &s, (struct comp_box){ f.x1, sb.y2, f.x2, f.y2 }, edge);     /* bottom */
}
