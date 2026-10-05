/* The cursor's picture (comp.h, paint.h): drawn by the compositor last in
 * every tile it touches (paint.c), never into a client's pixels.
 *
 * Where it is and what it shows are the seat's (struct comp_cursor,
 * pointer.c): the default arrow (libfun's, the same the apps drew
 * themselves, at twice the size above 1100 lines, as libfun's UI scale),
 * nothing (a client's set_cursor with no surface), or the client's cursor
 * surface, drawn with its hot spot at the pointer's tip, at most 64x64 of
 * it. Nothing at all is shown until the pointer has moved once (or the
 * test scene shows it): a machine with no mouse shows no arrow.
 *
 * The seat calls cursor_moved after every change, of place or of picture.
 * It damages the cursor's box as it was last damaged and as it is now, so
 * a move repaints two small boxes (on the PC two 32x32 squares take about
 * 2 us: fbbench) and a cursor surface whose commit changed its size, or
 * that went, leaves nothing behind. The arrow is kept as premultiplied
 * pixels, drawn with the same blend as windows; a client's surface is
 * drawn as its window would be (copied if opaque, blended if argb). */
#include <fun.h>
#include <jwl/wayland.h>
#include "paint.h"

#define SURFACE_MAX 64   /* a cursor surface's pixels drawn, across and down */
#define SCALE_MAX   2

static struct {
    bool shown;                     /* the pointer has moved once: something may be drawn */
    int32_t aw, ah;                 /* the arrow's size at our scale */
    struct comp_box box;            /* its box when last damaged (cursor_moved) */
    uint32_t arrow[POINTER_ARROW_W * SCALE_MAX * POINTER_ARROW_H * SCALE_MAX];   /* premultiplied */
} cur;

void cursor_init(void)
{
    int k = output.height > 1100 ? 2 : 1;   /* libfun's ui scale */
    cur.aw = POINTER_ARROW_W * k;
    cur.ah = POINTER_ARROW_H * k;
    for (int32_t j = 0; j < cur.ah; j++)
        for (int32_t i = 0; i < cur.aw; i++) {
            char c = pointer_arrow[j / k][i / k];
            cur.arrow[j * cur.aw + i] = c == '#' ? 0xff000000u : c == 'o' ? 0xffffffffu : 0;
        }
}

struct comp_box cursor_box(void)
{
    const struct comp_surface *s = cursor.surface;
    if (!cur.shown || cursor.hidden)
        return (struct comp_box){ 0, 0, 0, 0 };
    if (!s)
        return box_make(cursor.x, cursor.y, cur.aw, cur.ah);
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
    scene_damage(cur.box);
    cur.box = cursor_box();
    scene_damage(cur.box);
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
            src = cur.arrow + (y - c.y1) * cur.aw + (in.x1 - c.x1);
        if (blend) {
            px_over_row(dst, src, n);
        } else {
            for (int i = 0; i < n; i++)
                dst[i] = src[i] & 0xffffff;
        }
    }
}
