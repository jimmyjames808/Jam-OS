/* libfun: scaling an image of premultiplied 0xAARRGGBB pixels (fun.h
 * "alpha"): area averaging to make it smaller (each output pixel the
 * average of the source area it covers, partial pixels weighted by their
 * share), bilinear to make it bigger. Premultiplied, so a transparent
 * pixel's colour never bleeds into its neighbours. jamjar draws its album
 * covers with it, and bin/jamcover makes them. */
#include "internal.h"

/* How much of source cell [i, i + 1) the span [a, b) covers. */
static float cover(int i, float a, float b)
{
    return ((float)(i + 1) < b ? (float)(i + 1) : b) - ((float)i > a ? (float)i : a);
}

/* One output pixel of box(): the area [x0, x1) x [y0, y1) of s averaged,
 * partial pixels weighted by how much of them it covers. */
static uint32_t box_pixel(const struct picture *s, float x0, float x1, float y0, float y1)
{
    float acc[4] = { 0, 0, 0, 0 }, wsum = 0;
    for (int sy = (int)y0; sy < s->h && (float)sy < y1; sy++) {
        float wy = cover(sy, y0, y1);
        for (int sx = (int)x0; sx < s->w && (float)sx < x1; sx++) {
            float w = wy * cover(sx, x0, x1);
            uint32_t p = s->px[(size_t)sy * s->stride + sx];
            for (int c = 0; c < 4; c++)
                acc[c] += w * (float)(p >> (8 * c) & 0xff);
            wsum += w;
        }
    }
    uint32_t o = 0;
    for (int c = 0; c < 4; c++)
        o |= (uint32_t)(acc[c] / (wsum > 0 ? wsum : 1) + 0.5f) << (8 * c);
    return o;
}

/* src to dst (dw x dh): each output pixel the average of the source area
 * it covers. For making smaller; scale_pm picks. */
static void box(const struct picture *s, uint32_t *dst, int dw, int dh)
{
    float fx = (float)s->w / (float)dw, fy = (float)s->h / (float)dh;
    for (int y = 0; y < dh; y++) {
        float y0 = (float)y * fy, y1 = y0 + fy;
        for (int x = 0; x < dw; x++) {
            float x0 = (float)x * fx;
            dst[(size_t)y * dw + x] = box_pixel(s, x0, x0 + fx, y0, y1);
        }
    }
}

/* Bilinear, for making bigger (pixel centres line up). */
static void bilinear(const struct picture *s, uint32_t *dst, int dw, int dh)
{
    const uint32_t *src = s->px;
    int sw = s->w, sh = s->h, stride = s->stride;
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        fy = fy < 0 ? 0 : fy;
        int y0 = (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        float ty = fy - (float)y0;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            fx = fx < 0 ? 0 : fx;
            int x0 = (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float tx = fx - (float)x0;
            uint32_t a = src[(size_t)y0 * stride + x0], b = src[(size_t)y0 * stride + x1];
            uint32_t c = src[(size_t)y1 * stride + x0], d = src[(size_t)y1 * stride + x1], o = 0;
            for (int k = 0; k < 32; k += 8) {
                float top = (float)(a >> k & 0xff) * (1 - tx) + (float)(b >> k & 0xff) * tx;
                float bot = (float)(c >> k & 0xff) * (1 - tx) + (float)(d >> k & 0xff) * tx;
                o |= (uint32_t)(top * (1 - ty) + bot * ty + 0.5f) << k;
            }
            dst[(size_t)y * dw + x] = o;
        }
    }
}

void scale_pm(const struct picture *src, uint32_t *dst, int dw, int dh)
{
    if (dw <= src->w && dh <= src->h)
        box(src, dst, dw, dh);
    else
        bilinear(src, dst, dw, dh);
}
