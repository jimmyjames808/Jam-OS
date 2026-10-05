/* Composing without a framebuffer (comp.h): the scene's damage composed
 * into scene.pixels, an image in memory, so the protocol works and its
 * result can be looked at (tests, screenshots) when there is no screen.
 *
 * The plain painter's order, one damage box at a time: the background,
 * then every mapped window that touches the box from the bottom up, its
 * buffer copied (xrgb8888) or blended over what is below (argb8888,
 * premultiplied, with libfun's px_over: the same rounding as the apps').
 * Decorations and the cursor are the painting track's (deco.c, cursor.c);
 * the framebuffer's tiled, multi-threaded painting is paint.c's.
 *
 * Pixels are read only through our VMAR_KEPT_ONLY mapping of the client's
 * pool, inside the bounds the buffer was checked against when it was made:
 * a client can change what its pixels are, never where we read. */
#include <fun.h>
#include <jwl/wayland.h>
#include "comp.h"

/* Row y of the output from x1 to x2: the background. */
static void fill_row(uint32_t *row, int32_t x1, int32_t x2)
{
    for (int32_t x = x1; x < x2; x++)
        row[x] = scene.background;
}

/* Box b (output coordinates, inside w's surface box) of window w's buffer. */
static void compose_window(const struct comp_window *w, struct comp_box b)
{
    const struct comp_buffer *buf = w->surface->buffer;
    const uint8_t *data = comp_buffer_data(buf);
    bool blend = buf->format == JWL_WL_SHM_FORMAT_ARGB8888;
    for (int32_t y = b.y1; y < b.y2; y++) {
        const uint32_t *src = (const uint32_t *)(const void *)
            (data + (uint64_t)(y - w->y) * buf->stride);
        uint32_t *dst = scene.pixels + (uint64_t)y * scene.stride;
        for (int32_t x = b.x1; x < b.x2; x++) {
            uint32_t p = src[x - w->x];
            dst[x] = blend ? px_over(dst[x], p) : p & 0xffffffu;
        }
    }
}

static void compose_box(struct comp_box b)
{
    for (int32_t y = b.y1; y < b.y2; y++)
        fill_row(scene.pixels + (uint64_t)y * scene.stride, b.x1, b.x2);
    for (const struct comp_window *w = scene.bottom; w; w = w->above) {
        if (!(w->flags & COMP_WIN_MAPPED) || !w->surface->buffer)
            continue;
        struct comp_box in = box_intersect(b, window_surface_box(w));
        if (!box_empty(in))
            compose_window(w, in);
    }
    comp.stats.painted_px += (uint64_t)(b.x2 - b.x1) * (uint64_t)(b.y2 - b.y1);
}

void headless_compose(void)
{
    if (scene.pixels)
        for (uint32_t i = 0; i < scene.damage.n; i++)
            compose_box(scene.damage.b[i]);
    damage_clear(&scene.damage);
}
