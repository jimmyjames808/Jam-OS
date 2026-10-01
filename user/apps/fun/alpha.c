/* libfun: alpha blending with premultiplied alpha (fun.h "alpha").
 *
 * A premultiplied pixel is 0xAARRGGBB whose red, green and blue are
 * already multiplied by its alpha (a/255): drawing it over a destination
 * pixel d is, per channel, out = src + d * (255 - a) / 255. One multiply
 * per channel, no division by alpha, and a fully transparent pixel is
 * exactly 0. The surfaces stay opaque 0xRRGGBB: what is drawn over them is
 * what carries alpha.
 *
 * /255 is done exactly with integers: for 0 <= x <= 255 * 255,
 * x / 255 rounded is (x + 128 + ((x + 128) >> 8)) >> 8.
 *
 * fill_pm is the hot path (a fade over the whole screen): two pixels at a
 * time as eight 16-bit lanes in GCC vector types, which -march=x86-64
 * compiles to SSE2 (pmullw, psrlw, packuswb); the rest are per pixel.
 * The anti-aliased shapes use coverage as alpha: each pixel's share of
 * the shape, from the distance of its centre to the shape's edge (a one
 * pixel wide ramp), times the colour's own alpha. */
#include "internal.h"

typedef uint16_t v8u16 __attribute__((vector_size(16)));
typedef uint8_t  v8u8  __attribute__((vector_size(8)));

/* x / 255, rounded, for 0 <= x <= 255 * 255. */
static inline uint32_t div255(uint32_t x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

uint32_t argb_pm(uint32_t rgb, uint32_t a)
{
    a = a > 255 ? 255 : a;
    return a << 24 | div255((rgb >> 16 & 0xff) * a) << 16 | div255((rgb >> 8 & 0xff) * a) << 8 |
           div255((rgb & 0xff) * a);
}

uint32_t px_over(uint32_t dst, uint32_t src)
{
    uint32_t inv = 255 - (src >> 24);
    uint32_t r = (src >> 16 & 0xff) + div255((dst >> 16 & 0xff) * inv);
    uint32_t g = (src >> 8 & 0xff) + div255((dst >> 8 & 0xff) * inv);
    uint32_t b = (src & 0xff) + div255((dst & 0xff) * inv);
    /* A valid premultiplied source never exceeds 255 here; clamp anyway
     * (a source whose colour is more than its alpha). */
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

/* src with no colour channel above its alpha (what premultiplying gives;
 * a caller's hand-made value may not be). */
static uint32_t pm_valid(uint32_t src)
{
    uint32_t a = src >> 24, out = a << 24;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t c = src >> sh & 0xff;
        out |= (c > a ? a : c) << sh;
    }
    return out;
}

/* n pixels of p, src over each (src valid premultiplied, the top byte of
 * the result 0 as the surfaces keep it). */
static void row_over(uint32_t *p, int n, uint32_t src)
{
    uint16_t inv = (uint16_t)(255 - (src >> 24));
    v8u16 vinv = { inv, inv, inv, 0, inv, inv, inv, 0 };
    v8u16 vsrc = { (uint16_t)(src & 0xff), (uint16_t)(src >> 8 & 0xff),
                   (uint16_t)(src >> 16 & 0xff), 0, (uint16_t)(src & 0xff),
                   (uint16_t)(src >> 8 & 0xff), (uint16_t)(src >> 16 & 0xff), 0 };
    int i = 0;
    for (; i + 2 <= n; i += 2) {
        v8u8 d8;
        __builtin_memcpy(&d8, p + i, 8);
        v8u16 x = __builtin_convertvector(d8, v8u16) * vinv + 128;
        x = ((x + (x >> 8)) >> 8) + vsrc;   /* at most 255: src is valid (pm_valid) */
        v8u8 o = __builtin_convertvector(x, v8u8);
        __builtin_memcpy(p + i, &o, 8);
    }
    if (i < n)
        p[i] = px_over(p[i], src);
}

static bool clip_rect(const struct surf *s, int *x, int *y, int *w, int *h)
{
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*w > s->w - *x)
        *w = s->w - *x;
    if (*h > s->h - *y)
        *h = s->h - *y;
    return *w > 0 && *h > 0;
}

void fill_pm(const struct surf *s, int x, int y, int w, int h, uint32_t src)
{
    if (!clip_rect(s, &x, &y, &w, &h) || !(src >> 24 || src & 0xffffff))
        return;   /* nothing to draw: fully transparent */
    src = pm_valid(src);
    for (int j = y; j < y + h; j++)
        row_over(s->px + (uint64_t)j * s->stride + x, w, src);
}

void blit_pm(const struct surf *dst, int x, int y, const struct surf *src)
{
    int sx = 0, sy = 0, w = src->w, h = src->h, x0 = x, y0 = y;
    if (!clip_rect(dst, &x, &y, &w, &h))
        return;
    sx = x - x0;
    sy = y - y0;
    for (int j = 0; j < h; j++) {
        const uint32_t *sp = src->px + (uint64_t)(sy + j) * src->stride + sx;
        uint32_t *dp = dst->px + (uint64_t)(y + j) * dst->stride + x;
        for (int i = 0; i < w; i++) {
            uint32_t a = sp[i] >> 24;
            if (a == 255)
                dp[i] = sp[i] & 0xffffff;
            else if (sp[i])
                dp[i] = px_over(dp[i], sp[i]);
        }
    }
}

/* Coverage 0..1 of a pixel whose centre is `inside` pixels inside an edge
 * (negative: outside), as an alpha 0..a. */
static inline uint32_t cover(float inside, uint32_t a)
{
    float c = inside + 0.5f;
    if (c <= 0.0f)
        return 0;
    if (c >= 1.0f)
        return a;
    return (uint32_t)(c * (float)a + 0.5f);
}

/* rgb at alpha a over pixel (x, y), which is on the surface. */
static inline void plot(const struct surf *s, int x, int y, uint32_t rgb, uint32_t a)
{
    if (!a)
        return;
    uint32_t *p = s->px + (uint64_t)y * s->stride + x;
    *p = a >= 255 ? rgb : px_over(*p, argb_pm(rgb, a));
}

void disc_aa(const struct surf *s, float cx, float cy, float r, uint32_t rgb, uint32_t a)
{
    a = a > 255 ? 255 : a;
    int x0 = (int)floord(cx - r - 1), x1 = (int)(cx + r + 1);
    int y0 = (int)floord(cy - r - 1), y1 = (int)(cy + r + 1);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= s->w ? s->w - 1 : x1;
    y1 = y1 >= s->h ? s->h - 1 : y1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            plot(s, x, y, rgb, cover(r - sqrtf_(dx * dx + dy * dy), a));
        }
    }
}

/* The distance from (px, py) to the segment (x0, y0)-(x1, y1). */
static float seg_dist(float px, float py, float x0, float y0, float x1, float y1)
{
    float vx = x1 - x0, vy = y1 - y0, wx = px - x0, wy = py - y0;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf_(dx * dx + dy * dy);
}

void line_aa(const struct surf *s, float x0, float y0, float x1, float y1, float width,
             uint32_t rgb, uint32_t a)
{
    a = a > 255 ? 255 : a;
    float half = width / 2.0f;
    float lx = (x0 < x1 ? x0 : x1) - half - 1, hx = (x0 > x1 ? x0 : x1) + half + 1;
    float ly = (y0 < y1 ? y0 : y1) - half - 1, hy = (y0 > y1 ? y0 : y1) + half + 1;
    int bx0 = lx < 0 ? 0 : (int)lx, by0 = ly < 0 ? 0 : (int)ly;
    int bx1 = hx >= (float)s->w ? s->w - 1 : (int)hx;
    int by1 = hy >= (float)s->h ? s->h - 1 : (int)hy;
    for (int y = by0; y <= by1; y++)
        for (int x = bx0; x <= bx1; x++)
            plot(s, x, y, rgb,
                 cover(half - seg_dist((float)x + 0.5f, (float)y + 0.5f, x0, y0, x1, y1), a));
}

void ring_aa(const struct surf *s, float cx, float cy, float r, float width, uint32_t rgb,
             uint32_t a)
{
    a = a > 255 ? 255 : a;
    float half = width / 2.0f, out = r + half + 1.0f;
    int x0 = (int)floord(cx - out), x1 = (int)(cx + out);
    int y0 = (int)floord(cy - out), y1 = (int)(cy + out);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= s->w ? s->w - 1 : x1;
    y1 = y1 >= s->h ? s->h - 1 : y1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, d = sqrtf_(dx * dx + dy * dy) - r;
            plot(s, x, y, rgb, cover(half - (d < 0 ? -d : d), a));
        }
    }
}

#define POLY_MAX 16

void poly_aa(const struct surf *s, const float *xy, int n, uint32_t rgb, uint32_t a)
{
    if (n < 3 || n > POLY_MAX)
        return;
    a = a > 255 ? 255 : a;
    /* Each edge's inward unit normal (nx, ny) and offset: inside where
     * nx * x + ny * y - off > 0. The winding decides which side is in. */
    float area = 0.0f, nx[POLY_MAX], ny[POLY_MAX], off[POLY_MAX];
    float lx = xy[0], hx = xy[0], ly = xy[1], hy = xy[1];
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
        lx = xy[2 * i] < lx ? xy[2 * i] : lx;
        hx = xy[2 * i] > hx ? xy[2 * i] : hx;
        ly = xy[2 * i + 1] < ly ? xy[2 * i + 1] : ly;
        hy = xy[2 * i + 1] > hy ? xy[2 * i + 1] : hy;
    }
    float sign = area > 0.0f ? 1.0f : -1.0f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float ex = xy[2 * j] - xy[2 * i], ey = xy[2 * j + 1] - xy[2 * i + 1];
        float len = sqrtf_(ex * ex + ey * ey);
        len = len > 0.0f ? len : 1.0f;
        nx[i] = -ey / len * sign;
        ny[i] = ex / len * sign;
        off[i] = nx[i] * xy[2 * i] + ny[i] * xy[2 * i + 1];
    }
    int x0 = (int)floord(lx - 1), x1 = (int)(hx + 1), y0 = (int)floord(ly - 1), y1 = (int)(hy + 1);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= s->w ? s->w - 1 : x1;
    y1 = y1 >= s->h ? s->h - 1 : y1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f, d = 1e9f;
            for (int i = 0; i < n; i++) {
                float e = nx[i] * px + ny[i] * py - off[i];
                d = e < d ? e : d;
            }
            plot(s, x, y, rgb, cover(d, a));
        }
}
