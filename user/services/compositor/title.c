/* Title bars, close boxes and borders: drawing them (comp.h, paint.h).
 *
 * A window's decorations are the strips of its frame around its surface,
 * as wide as the window manager made them (struct comp_window's deco_*):
 *   - the top strip, if it is COMP_TITLE_H or more, is the title bar: the
 *     window's title on the left, in libfun's 8x16 font, cut short with
 *     "..." before the close box; the close box, a square as tall as the
 *     bar, at its right end, with an X; "(not responding)" after the title
 *     when the window didn't answer a ping;
 *   - the other strips, and a thinner top one (tiling mode's windows have
 *     no title bar, a border all round), are a plain border.
 * The focused window's are brighter. Every pixel of a strip is drawn, and
 * opaquely, so paint.c may treat decorations as hiding what is below.
 *
 * Drawing happens tile by tile on the painting workers: each call draws
 * only what falls in the tile (libfun's fill and text clip to the tile's
 * buffer, which stands for its box of the output). A title can't draw
 * outside its bar: the text is cut at the close box and clipped to the
 * bar's rows. libfun's glyph tables are made before the workers start
 * (paint_init), so the workers only read them. */
#include <fun.h>
#include "paint.h"

#define TEXT_PAD    8      /* pixels before the title, and between it and the close box */
#define CLOSE_INSET 7      /* the X's distance from the close box's edges */
#define TITLE_MAX   300    /* bytes of title drawn: the window manager keeps at most 256 */

struct comp_box title_bar_box(const struct comp_window *w)
{
    struct comp_box f = window_frame(w), s = window_surface_box(w);
    if (w->deco_top < COMP_TITLE_H || box_empty(s))
        return (struct comp_box){ 0, 0, 0, 0 };   /* none, or a top border (tiling's) */
    return (struct comp_box){ f.x1, f.y1, f.x2, s.y1 };
}

struct comp_box title_close_box(const struct comp_window *w)
{
    struct comp_box bar = title_bar_box(w);
    int32_t side = bar.y2 - bar.y1;
    if (box_empty(bar) || side > bar.x2 - bar.x1)
        return (struct comp_box){ 0, 0, 0, 0 };
    return (struct comp_box){ bar.x2 - side, bar.y1, bar.x2, bar.y2 };
}

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

/* The close box's X: two diagonals two pixels thick, inset from its edges. */
static void draw_x(const struct tile_buf *t, struct comp_box box, uint32_t c)
{
    struct comp_box x = { box.x1 + CLOSE_INSET, box.y1 + CLOSE_INSET, box.x2 - CLOSE_INSET,
                          box.y2 - CLOSE_INSET };
    struct comp_box in = box_intersect(x, t->b);
    int32_t side = x.x2 - x.x1;
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint32_t *row = tile_row(t, y) - t->b.x1;
        for (int32_t px = in.x1; px < in.x2; px++) {
            int32_t i = px - x.x1, j = y - x.y1, d = i - j, e = i + j - (side - 1);
            if ((d >= -1 && d <= 0) || (e >= -1 && e <= 0))
                row[px] = c;
        }
    }
}

/* The title bar where it meets t. */
static void draw_bar(const struct comp_window *w, const struct tile_buf *t, const struct surf *s)
{
    struct comp_box bar = title_bar_box(w);
    if (box_empty(box_intersect(bar, t->b)))
        return;
    bool focused = w->flags & COMP_WIN_FOCUSED;
    uint32_t ink = focused ? TITLE_TEXT_FOCUSED : TITLE_TEXT;
    fill_box(t, s, bar, focused ? TITLE_BAR_FOCUSED : TITLE_BAR);
    struct comp_box close = title_close_box(w);
    int32_t text_end = box_empty(close) ? bar.x2 : close.x1;
    char title[TITLE_MAX];
    snprintf(title, sizeof(title), "%s%s", w->title ? w->title : "",
             w->flags & COMP_WIN_UNRESPONSIVE ? " (not responding)" : "");
    struct comp_box text = { bar.x1 + TEXT_PAD, bar.y1 + (bar.y2 - bar.y1 - TEXT_H(1)) / 2,
                             text_end - TEXT_PAD, 0 };
    text.y2 = text.y1 + TEXT_H(1);
    if (text.x2 > text.x1 && !box_empty(box_intersect(text, t->b))) {
        /* Clipped to the bar's rows too: a bar shorter than the font. */
        struct comp_box rows = box_intersect(t->b, bar);
        struct surf clip = { tile_row(t, rows.y1), s->w, rows.y2 - rows.y1, s->stride };
        struct rect r = { text.x1 - t->b.x1, text.y1 - rows.y1, text.x2 - text.x1, TEXT_H(1) };
        text_clip(&clip, &r, 1, ink, title);
    }
    if (!box_empty(close))
        draw_x(t, close, ink);
}

void title_draw(const struct comp_window *w, const struct tile_buf *t)
{
    if (w->deco_top <= 0 && w->deco_left <= 0 && w->deco_right <= 0 && w->deco_bottom <= 0)
        return;
    struct comp_box f = window_frame(w), sb = window_surface_box(w);
    int32_t tw = t->b.x2 - t->b.x1;
    struct surf s = { t->px, tw, t->b.y2 - t->b.y1, tw };
    uint32_t border = w->flags & COMP_WIN_FOCUSED ? BORDER_FOCUSED : BORDER;
    draw_bar(w, t, &s);
    if (box_empty(title_bar_box(w)))   /* a top strip thinner than a title bar: a border */
        fill_box(t, &s, (struct comp_box){ f.x1, f.y1, f.x2, sb.y1 }, border);
    fill_box(t, &s, (struct comp_box){ f.x1, sb.y1, sb.x1, sb.y2 }, border);   /* left */
    fill_box(t, &s, (struct comp_box){ sb.x2, sb.y1, f.x2, sb.y2 }, border);   /* right */
    fill_box(t, &s, (struct comp_box){ f.x1, sb.y2, f.x2, f.y2 }, border);     /* bottom */
}
