/* The wallpaper (paint.h, look.h): what the screen shows where no window
 * is. Made once at start at the output's size, in memory of its own (a
 * pixel a pixel: 14.1 MiB at 2560x1440), so a background fill is a copy
 * of rows, as cheap as a flat colour; no file is read.
 *
 * The picture: LOOK_WALL_BASE with look_glows added, each fading as
 * (1 - d^2/r^2)^2 from its centre, all in integers:
 *   - glow g's weight at pixel (x, y), 16-bit fixed point, where
 *     d2 = (x - cx)^2 + (y - cy)^2 < r^2:
 *         q = 65536 - (d2 * floor(2^48 / r^2)) >> 32,   f = q * q >> 16
 *     (cx, cy and r from look.h's thousandths, rounded down);
 *   - each channel, in 1/256ths: base * 256 + the sum over the glows of
 *     (f * (glow - base)) >> 8 (an arithmetic shift), kept within 0 and
 *     255 * 256;
 *   - dithered: plus the 8x8 Bayer matrix's entry at (x & 7, y & 7) times
 *     4, plus 2, then >> 8, at most 255.
 * The same output size always gives the same picture, pixel for pixel
 * (tools/comp-check.py paints it the same way to check QEMU's screen).
 * The rows are made on the painting workers, a row an item. */
#include <fun.h>
#include "paint.h"

static struct {
    uint32_t *px;                  /* w * h pixels, or NULL: the flat base colour */
    int32_t w, h;
    /* Each glow in pixels, for this output. */
    int64_t cx[LOOK_GLOWS], cy[LOOK_GLOWS], r2[LOOK_GLOWS], inv[LOOK_GLOWS];
} wall;

/* The 8x8 ordered-dither (Bayer) matrix: 0..63, each once. */
static const uint8_t bayer[8][8] = {
    { 0, 32, 8, 40, 2, 34, 10, 42 },  { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44, 4, 36, 14, 46, 6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    { 3, 35, 11, 43, 1, 33, 9, 41 },  { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47, 7, 39, 13, 45, 5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

/* Pixel (x, y)'s colour before dithering: each channel in 1/256ths. */
static void colour_at(int32_t x, int32_t y, int32_t acc[3])
{
    for (int c = 0; c < 3; c++)
        acc[c] = (int32_t)(LOOK_WALL_BASE >> (16 - 8 * c) & 0xff) * 256;
    for (int g = 0; g < LOOK_GLOWS; g++) {
        int64_t dx = x - wall.cx[g], dy = y - wall.cy[g], d2 = dx * dx + dy * dy;
        if (d2 >= wall.r2[g])
            continue;
        int64_t q = 65536 - ((d2 * wall.inv[g]) >> 32), f = (q * q) >> 16;
        for (int c = 0; c < 3; c++) {
            int32_t sh = 16 - 8 * c;
            int64_t delta = (int64_t)(look_glows[g].rgb >> sh & 0xff) -
                            (int64_t)(LOOK_WALL_BASE >> sh & 0xff);
            acc[c] += (int32_t)((f * delta) >> 8);
        }
    }
}

static void make_row(uint32_t y, uint32_t worker, void *arg)
{
    (void)worker;
    (void)arg;
    uint32_t *row = wall.px + (uint64_t)y * (uint32_t)wall.w;
    for (int32_t x = 0; x < wall.w; x++) {
        int32_t acc[3];
        colour_at(x, (int32_t)y, acc);
        uint32_t px = 0, th = bayer[y & 7][x & 7] * 4u + 2;
        for (int c = 0; c < 3; c++) {
            int32_t v = acc[c] < 0 ? 0 : acc[c] > 255 * 256 ? 255 * 256 : acc[c];
            uint32_t o = ((uint32_t)v + th) >> 8;
            px |= (o > 255 ? 255 : o) << (16 - 8 * c);
        }
        row[x] = px;
    }
}

status_t wallpaper_init(int32_t w, int32_t h)
{
    int64_t side = w > h ? w : h;
    wall.w = w;
    wall.h = h;
    for (int g = 0; g < LOOK_GLOWS; g++) {
        int64_t r = side * look_glows[g].r / 1000;
        wall.cx[g] = (int64_t)w * look_glows[g].cx / 1000;
        wall.cy[g] = (int64_t)h * look_glows[g].cy / 1000;
        wall.r2[g] = r * r > 0 ? r * r : 1;
        wall.inv[g] = (1ll << 48) / wall.r2[g];
    }
    wall.px = big_alloc((uint64_t)w * (uint64_t)h * 4);
    if (!wall.px)
        return ERR_NO_MEMORY;
    pool_run(make_row, NULL, (uint32_t)h);
    pool_rest();
    return OK;
}

void wallpaper_fill(const struct tile_buf *t)
{
    int32_t n = t->b.x2 - t->b.x1;
    for (int32_t y = t->b.y1; y < t->b.y2; y++) {
        uint32_t *dst = tile_row(t, y);
        if (wall.px) {
            memcpy(dst, wall.px + (uint64_t)y * (uint32_t)wall.w + (uint32_t)t->b.x1,
                   (size_t)n * 4);
            continue;
        }
        for (int32_t i = 0; i < n; i++)
            dst[i] = LOOK_WALL_BASE;
    }
}
