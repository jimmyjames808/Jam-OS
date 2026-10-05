/* The cursor (comp.h): the pointer's picture, drawn by the compositor last
 * in every tile it touches (paint.c), never into a client's pixels.
 *
 * It is one of: libfun's arrow (the same the apps drew themselves, at
 * twice the size above 1100 lines, as libfun's UI scale), nothing (a
 * client's set_cursor with no surface), or a client's cursor surface, up to
 * 64x64 of its buffer with the client's hot spot. Nothing at all is shown
 * until the seat says a pointer has stirred (cursor_show): a machine with
 * no mouse shows no arrow.
 *
 * Every change damages the cursor's box as it was and as it is, so a move
 * repaints two small boxes (on the PC two 32x32 squares take about 2 us:
 * fbbench). The arrow is kept as premultiplied pixels, drawn with the
 * same blend as windows; a client's surface is drawn as its window would
 * be (copied if opaque, blended if argb). */
#include <fun.h>
#include <jwl/wayland.h>
#include "paint.h"

#define SURFACE_MAX 64   /* a cursor surface's pixels drawn, across and down */
#define SCALE_MAX   2

static struct {
    bool shown;                     /* a pointer has stirred: something may be drawn */
    enum comp_cursor kind;
    struct comp_surface *surface;   /* COMP_CURSOR_SURFACE's */
    int32_t hx, hy;                 /* its hot spot, in it */
    int32_t x, y;                   /* the hot spot on the output */
    int32_t aw, ah;                 /* the arrow's size at our scale */
    struct comp_box box;            /* its box when last damaged (cursor_damage) */
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
    cur.x = output.width / 2;
    cur.y = output.height / 2;
}

struct comp_box cursor_box(void)
{
    if (!cur.shown || cur.kind == COMP_CURSOR_HIDDEN)
        return (struct comp_box){ 0, 0, 0, 0 };
    if (cur.kind == COMP_CURSOR_ARROW)
        return box_make(cur.x, cur.y, cur.aw, cur.ah);
    const struct comp_surface *s = cur.surface;
    if (!s || !s->buffer)
        return (struct comp_box){ 0, 0, 0, 0 };
    return box_make(cur.x - cur.hx, cur.y - cur.hy, s->width < SURFACE_MAX ? s->width : SURFACE_MAX,
                     s->height < SURFACE_MAX ? s->height : SURFACE_MAX);
}

/* Where it was when last damaged and where it is now: a cursor surface's
 * commit may have changed its size before we hear of it, so the old box
 * is remembered, not worked out again. */
void cursor_damage(void)
{
    scene_damage(cur.box);
    cur.box = cursor_box();
    scene_damage(cur.box);
}

void cursor_show(bool on)
{
    cur.shown = on;
    cursor_damage();
}

void cursor_move(int32_t x, int32_t y)
{
    if (x == cur.x && y == cur.y)
        return;
    cur.x = x;
    cur.y = y;
    cursor_damage();
}

void cursor_set(enum comp_cursor kind, struct comp_surface *s, int32_t hx, int32_t hy)
{
    cur.kind = kind == COMP_CURSOR_SURFACE && !s ? COMP_CURSOR_HIDDEN : kind;
    cur.surface = kind == COMP_CURSOR_SURFACE ? s : NULL;
    cur.hx = hx;
    cur.hy = hy;
    cursor_damage();
}

void cursor_draw(const struct tile_buf *t)
{
    struct comp_box c = cursor_box(), in = box_intersect(c, t->b);
    if (box_empty(in))
        return;
    int n = in.x2 - in.x1;
    const struct comp_buffer *b = cur.kind == COMP_CURSOR_SURFACE ? cur.surface->buffer : NULL;
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
