/* The cursor's picture (comp.h, paint.h): drawn by the compositor last in
 * every tile it touches (paint.c), never into a client's pixels.
 *
 * Where it is and what it shows are the seat's (struct comp_cursor,
 * pointer.c): one of the compositor's set (cursors.c: the arrow, the
 * resize arrows, move, the text bar, the hand, busy), nothing (a client's
 * set_cursor with no surface), or the client's cursor surface, drawn with
 * its hot spot at the pointer's tip, at most 64x64 of it. Nothing at all
 * is shown until the pointer has moved once (or the test scene shows it):
 * a machine with no mouse shows no arrow.
 *
 * The seat calls cursor_moved after every change, of place or of picture
 * (and the desktop each tick while busy turns). It damages the cursor's box
 * as it was last damaged and as it is now, so a move repaints two small
 * boxes (on the PC two 32x32 squares take about 2 us: fbbench) and a
 * cursor surface whose commit changed its size, or that went, leaves
 * nothing behind. The set's picture is chosen there, on the loop's thread
 * (busy's frame by the time), and only read by the painting workers. The
 * set is premultiplied and drawn with the same blend as windows; a client's
 * surface is drawn as its window would be (copied if opaque, blended if
 * argb). */
#include <fun.h>
#include <jwl/wayland.h>
#include "paint.h"

#define SURFACE_MAX 64   /* a cursor surface's pixels drawn, across and down */

static struct {
    bool shown;                     /* the pointer has moved once: something may be drawn */
    struct comp_box box;            /* its box when last damaged (cursor_moved) */
    const struct cursor_image *img; /* the set's picture then (NULL: a surface, or none) */
} cur;

void cursor_init(void)
{
    cursors_init();
    cur.img = cursors_get(CURSOR_ARROW, 0);
}

/* The box of the set's picture img with its hot spot at the pointer. */
static struct comp_box image_box(const struct cursor_image *img)
{
    return box_make(cursor.x - img->hot_x, cursor.y - img->hot_y, img->w, img->h);
}

struct comp_box cursor_box(void)
{
    const struct comp_surface *s = cursor.surface;
    if (!cur.shown || cursor.hidden)
        return (struct comp_box){ 0, 0, 0, 0 };
    if (!s)
        return cur.img ? image_box(cur.img) : (struct comp_box){ 0, 0, 0, 0 };
    if (!s->buffer)
        return (struct comp_box){ 0, 0, 0, 0 };
    return box_make(cursor.x - cursor.hot_x, cursor.y - cursor.hot_y,
                    s->width < SURFACE_MAX ? s->width : SURFACE_MAX,
                    s->height < SURFACE_MAX ? s->height : SURFACE_MAX);
}

void cursor_moved(int32_t old_x, int32_t old_y)
{
    if (old_x != cursor.x || old_y != cursor.y)
        cur.shown = true;   /* a pointer exists */
    cur.img = cursor.surface ? NULL : cursors_get(cursor.shape, now());
    scene_damage_over(cur.box);
    cur.box = cursor_box();
    scene_damage_over(cur.box);
}

void cursor_show(bool on)
{
    cur.shown = on;
    cursor_moved(cursor.x, cursor.y);
}

void cursor_draw(const struct tile_buf *t)
{
    struct comp_box c = cursor_box(), in = box_intersect(c, t->b);
    if (box_empty(in))
        return;
    int n = in.x2 - in.x1;
    const struct comp_buffer *b = cursor.surface ? cursor.surface->buffer : NULL;
    bool blend = !b || b->format == JWL_WL_SHM_FORMAT_ARGB8888;
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint32_t *dst = tile_row(t, y) + (in.x1 - t->b.x1);
        const uint32_t *src;
        if (b)
            src = (const uint32_t *)(const void *)(comp_buffer_data(b) +
                                                   (uint64_t)(y - c.y1) * b->stride) + (in.x1 - c.x1);
        else
            src = cur.img->px + (y - c.y1) * cur.img->w + (in.x1 - c.x1);
        if (blend) {
            px_over_row(dst, src, n);
        } else {
            for (int i = 0; i < n; i++)
                dst[i] = src[i] & 0xffffff;
        }
    }
}
