/* splash: a decoded frame into the screen's back buffer (splash_int.h).
 *
 * MPEG-1 frames are YCbCr 4:2:0: a Cb and a Cr sample for each 2x2 block of
 * Y, sited at the block's centre. The colour is studio-range BT.601
 * (ycc). Three ways onto the screen (video.c chooses):
 *   DRAW_UP        1:1 or an integer scale k: the chroma is brought up to
 *                  full size by interpolation (each pixel 9/16 of its own
 *                  block's sample, 3/16 of each of the two nearest beside
 *                  and above or below, 1/16 of the diagonal one, the usual
 *                  filter for centre-sited 4:2:0), so the red drupelets'
 *                  edges on the dark background are smooth, not 2x2 steps
 *                  of colour; then each pixel repeated k times both ways;
 *   DRAW_BOX       down by an integer n: Y, Cb and Cr each averaged over
 *                  the n x n block (the conversion is linear but for its
 *                  clamp, so this is the average colour), then converted;
 *   DRAW_BILINEAR  down by any other ratio: Y and the chroma each
 *                  interpolated between their four nearest samples at the
 *                  output pixel's centre (sharp enough down to half size;
 *                  smaller than that it would alias, but such screens
 *                  get DRAW_BOX).
 * All three in bands of BAND rows on the thread pool, fixed point only. */
#include "splash_int.h"

#define BAND   16     /* rows a pool item converts */
#define MAXCW  2048   /* chroma samples a row at most (a 4096-wide frame) */

struct job {
    const plm_frame_t         *f;
    const struct video_layout *l;
};

static inline uint32_t clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
}

/* Studio-range BT.601 YCbCr (what MPEG-1 holds) to RGB, in 16.16 fixed
 * point: R = 1.164 (Y - 16) + 1.596 Cr', G = ... - 0.392 Cb' - 0.813 Cr',
 * B = ... + 2.017 Cb' (Cb' = Cb - 128, Cr' = Cr - 128). y, cb and cr are
 * in 1/16 of a level here, so interpolated values keep their fraction. */
static inline uint32_t ycc16(int y, int cb, int cr)
{
    int l = (y - 16 * 16) * 76309;
    cb -= 128 * 16;
    cr -= 128 * 16;
    return clamp255((l + cr * 104597 + (1 << 19)) >> 20) << 16 |
           clamp255((l - cb * 25675 - cr * 53279 + (1 << 19)) >> 20) << 8 |
           clamp255((l + cb * 132201 + (1 << 19)) >> 20);
}

uint32_t video_pixel(const plm_frame_t *f, int x, int y)
{
    int c = (y / 2) * (int)f->cb.width + x / 2;
    return ycc16(16 * f->y.data[y * (int)f->y.width + x], 16 * f->cb.data[c],
                 16 * f->cr.data[c]);
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- DRAW_UP --------------------------------------------------------------------- */

/* Source row y at full chroma resolution into dst (its first copy on the
 * screen), each pixel k times. vb, vr: scratch for a row of chroma. */
static void up_row(const plm_frame_t *f, int y, int k, uint32_t *dst, int *vb, int *vr)
{
    int cw = (int)f->cb.width, ch = (int)f->cb.height, w = (int)f->width;
    int cy = y / 2, cy2 = clampi(y & 1 ? cy + 1 : cy - 1, 0, ch - 1);
    const uint8_t *b0 = f->cb.data + cy * cw, *b1 = f->cb.data + cy2 * cw;
    const uint8_t *r0 = f->cr.data + cy * cw, *r1 = f->cr.data + cy2 * cw;
    for (int i = 0; i < cw; i++) {   /* vertically: 3/4 its own row, 1/4 the nearer other */
        vb[i] = 3 * b0[i] + b1[i];
        vr[i] = 3 * r0[i] + r1[i];
    }
    const uint8_t *py = f->y.data + (uint64_t)y * f->y.width;
    for (int x = 0; x < w; x++) {
        int cx = x / 2, cx2 = clampi(x & 1 ? cx + 1 : cx - 1, 0, cw - 1);
        /* horizontally the same: (3 a + b) / 4, so 16ths of a level in all */
        uint32_t c = ycc16(16 * py[x], 3 * vb[cx] + vb[cx2], 3 * vr[cx] + vr[cx2]);
        for (int j = 0; j < k; j++)
            dst[x * k + j] = c;
    }
}

static void up_band(uint32_t item, uint32_t worker, void *arg)
{
    static int scratch[FUN_MAX_THREADS][2][MAXCW];
    const struct job *jb = arg;
    const struct video_layout *l = jb->l;
    for (int y = (int)item * BAND; y < (int)(item + 1) * BAND && y < l->h; y++) {
        uint32_t *first = scr.s.px + (uint64_t)(l->y + y * l->n) * scr.s.stride + l->x;
        up_row(jb->f, y, l->n, first, scratch[worker][0], scratch[worker][1]);
        for (int j = 1; j < l->n; j++)
            memcpy(first + (uint64_t)j * scr.s.stride, first, (size_t)l->ow * 4);
    }
}

/* ---- DRAW_BOX -------------------------------------------------------------------- */

/* The average of plane p's samples [x0, x1) x [y0, y1), in 1/16 of a level. */
static int box_avg(const plm_plane_t *p, int x0, int x1, int y0, int y1)
{
    int sum = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            sum += p->data[y * (int)p->width + x];
    int n = (x1 - x0) * (y1 - y0);
    return (sum * 16 + n / 2) / n;
}

static void box_band(uint32_t item, uint32_t worker, void *arg)
{
    (void)worker;
    const struct job *jb = arg;
    const struct video_layout *l = jb->l;
    const plm_frame_t *f = jb->f;
    int n = l->n, cw = (int)f->cb.width, ch = (int)f->cb.height;
    for (int oy = (int)item * BAND; oy < (int)(item + 1) * BAND && oy < l->oh; oy++) {
        uint32_t *dst = scr.s.px + (uint64_t)(l->y + oy) * scr.s.stride + l->x;
        int y0 = oy * n, cy0 = y0 / 2, cy1 = clampi((y0 + n + 1) / 2, cy0 + 1, ch);
        for (int ox = 0; ox < l->ow; ox++) {
            int x0 = ox * n, cx0 = x0 / 2, cx1 = clampi((x0 + n + 1) / 2, cx0 + 1, cw);
            dst[ox] = ycc16(box_avg(&f->y, x0, x0 + n, y0, y0 + n),
                            box_avg(&f->cb, cx0, cx1, cy0, cy1),
                            box_avg(&f->cr, cx0, cx1, cy0, cy1));
        }
    }
}

/* ---- DRAW_BILINEAR --------------------------------------------------------------- */

/* Plane p at (x, y) in 1/256 of a sample (clamped to its edges), in 1/16
 * of a level. */
static int bilin(const plm_plane_t *p, int x, int y)
{
    int w = (int)p->width, h = (int)p->height;
    x = clampi(x, 0, (w - 1) * 256);
    y = clampi(y, 0, (h - 1) * 256);
    int x0 = x >> 8, y0 = y >> 8, fx = x & 255, fy = y & 255;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    const uint8_t *r0 = p->data + y0 * w, *r1 = p->data + y1 * w;
    int top = r0[x0] * (256 - fx) + r0[x1] * fx, bottom = r1[x0] * (256 - fx) + r1[x1] * fx;
    return ((top * (256 - fy) + bottom * fy) * 16 + (1 << 15)) >> 16;
}

static void bilinear_band(uint32_t item, uint32_t worker, void *arg)
{
    (void)worker;
    const struct job *jb = arg;
    const struct video_layout *l = jb->l;
    const plm_frame_t *f = jb->f;
    /* Output pixel o's centre in source pixels: (o + 0.5) * w / ow - 0.5;
     * in chroma samples, half of that plus 0.5, less 0.5. All in 1/256. */
    for (int oy = (int)item * BAND; oy < (int)(item + 1) * BAND && oy < l->oh; oy++) {
        uint32_t *dst = scr.s.px + (uint64_t)(l->y + oy) * scr.s.stride + l->x;
        int sy = (int)(((2 * (int64_t)oy + 1) * l->h * 128) / l->oh) - 128;
        int cy = (sy + 128) / 2 - 128;
        for (int ox = 0; ox < l->ow; ox++) {
            int sx = (int)(((2 * (int64_t)ox + 1) * l->w * 128) / l->ow) - 128;
            int cx = (sx + 128) / 2 - 128;
            dst[ox] = ycc16(bilin(&f->y, sx, sy), bilin(&f->cb, cx, cy), bilin(&f->cr, cx, cy));
        }
    }
}

void draw_frame(const plm_frame_t *f, const struct video_layout *l)
{
    struct job jb = { f, l };
    if ((int)f->cb.width > MAXCW || (int)f->width != l->w || (int)f->height != l->h)
        return;   /* video_open refused such sizes; a frame of another size: skipped */
    if (l->mode == DRAW_UP)
        pool_run(up_band, &jb, (uint32_t)(l->h + BAND - 1) / BAND);
    else if (l->mode == DRAW_BOX)
        pool_run(box_band, &jb, (uint32_t)(l->oh + BAND - 1) / BAND);
    else
        pool_run(bilinear_band, &jb, (uint32_t)(l->oh + BAND - 1) / BAND);
}
