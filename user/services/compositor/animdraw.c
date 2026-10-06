/* The animations' pictures (desk.h, anim.c).
 *
 * A snapshot is a window's frame drawn once, as paint.c draws it but with
 * no shadow, into memory of its own (big_alloc, freed when the animation
 * ends) over transparent black: so its rounded corners come out
 * premultiplied by their coverage, and every other pixel opaque (a
 * translucent client's pixels are taken as they look over black: for the
 * length of a fade nobody sees the difference).
 *
 * Drawing it: each pixel of the box the animation has reached is mapped
 * back into the snapshot (16.16 fixed point, the box's size to the
 * snapshot's), sampled bilinearly from its four neighbours, faded by the
 * animation's alpha and laid over the tile (premultiplied: src +
 * dst * (255 - a) / 255). On the painting workers, reading only. */
#include <fun.h>
#include "desk.h"

status_t anim_snapshot(const struct comp_window *w, struct anim_snap *out)
{
    static uint32_t save[SHAPE_SAVE_PX];   /* the loop's thread only */
    struct comp_box f = window_frame(w);
    int32_t fw = f.x2 - f.x1, fh = f.y2 - f.y1;
    if (fw <= 0 || fh <= 0)
        return ERR_INVALID_ARGS;
    uint64_t bytes = (uint64_t)fw * (uint64_t)fh * 4;
    uint32_t *px = big_alloc(bytes);
    if (!px)
        return ERR_NO_MEMORY;
    struct tile_buf t = { f, px };
    paint_window(w, &t, save);
    const struct corner_mask *m = shape_corners(w);
    for (int32_t y = 0; y < fh; y++)
        for (int32_t x = 0; x < fw; x++) {
            uint32_t a = 255;
            if (m) {   /* inside a corner square: its coverage */
                int32_t i = x < m->r ? x : x >= fw - m->r ? fw - 1 - x : -1;
                int32_t j = y < m->r ? y : y >= fh - m->r ? fh - 1 - y : -1;
                if (i >= 0 && j >= 0)
                    a = m->cov[j * m->r + i];
            }
            uint32_t *p = &px[(uint64_t)y * (uint32_t)fw + (uint32_t)x];
            *p = (*p & 0xffffff) | a << 24;
        }
    *out = (struct anim_snap){ px, fw, fh, bytes };
    return OK;
}

void anim_snapshot_free(struct anim_snap *s)
{
    big_free(s->px, s->bytes);
    *s = (struct anim_snap){ 0 };
}

/* Channel c (shift sh) of four pixels, weighted by fx, fy (0..256). */
static uint32_t mix4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t fx, uint32_t fy,
                     int sh)
{
    uint32_t top = (a >> sh & 0xff) * (256 - fx) + (b >> sh & 0xff) * fx;
    uint32_t bot = (c >> sh & 0xff) * (256 - fx) + (d >> sh & 0xff) * fx;
    return (top * (256 - fy) + bot * fy + 32768) >> 16;
}

/* The snapshot at fixed-point (u, v) (16.16, pixel centres at .5), its
 * edges repeated: premultiplied 0xAARRGGBB. */
static uint32_t sample(const struct anim_snap *s, int64_t u, int64_t v)
{
    int64_t x0 = u >> 16, y0 = v >> 16;
    uint32_t fx = (uint32_t)(u >> 8 & 0xff), fy = (uint32_t)(v >> 8 & 0xff);
    int32_t xa = x0 < 0 ? 0 : x0 >= s->w ? s->w - 1 : (int32_t)x0;
    int32_t xb = x0 + 1 < 0 ? 0 : x0 + 1 >= s->w ? s->w - 1 : (int32_t)x0 + 1;
    int32_t ya = y0 < 0 ? 0 : y0 >= s->h ? s->h - 1 : (int32_t)y0;
    int32_t yb = y0 + 1 < 0 ? 0 : y0 + 1 >= s->h ? s->h - 1 : (int32_t)y0 + 1;
    const uint32_t *ra = s->px + (uint64_t)ya * (uint32_t)s->w;
    const uint32_t *rb = s->px + (uint64_t)yb * (uint32_t)s->w;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8)
        out |= mix4(ra[xa], ra[xb], rb[xa], rb[xb], fx, fy, sh) << sh;
    return out;
}

/* Premultiplied p faded by alpha (0..255). */
static uint32_t faded(uint32_t p, uint32_t alpha)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8)
        out |= (((p >> sh & 0xff) * alpha + 127) / 255) << sh;
    return out;
}

void anim_draw_snap(const struct tile_buf *t, const struct anim_snap *s, struct comp_box at,
                    uint32_t alpha)
{
    struct comp_box in = box_intersect(at, t->b);
    if (box_empty(in) || !s->px || !alpha)
        return;
    int64_t aw = at.x2 - at.x1, ah = at.y2 - at.y1;
    int64_t sx = ((int64_t)s->w << 16) / aw, sy = ((int64_t)s->h << 16) / ah;
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint32_t *row = tile_row(t, y) - t->b.x1;
        int64_t v = (y - at.y1) * sy + sy / 2 - 0x8000;
        for (int32_t x = in.x1; x < in.x2; x++) {
            int64_t u = (x - at.x1) * sx + sx / 2 - 0x8000;
            uint32_t p = sample(s, u, v);
            if (alpha < 255)
                p = faded(p, alpha);
            row[x] = px_over(row[x], p);
        }
    }
}

void anim_draw(const struct tile_buf *t, bool above_strip)
{
    struct anim_draw d;
    if (anim_now(&d) && d.above_strip == above_strip)
        anim_draw_snap(t, d.snap, d.at, d.alpha);
}
