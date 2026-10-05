/* The look's anti-aliased shapes (paint.h, look.h), made once at start by
 * supersampling: each pixel's coverage, 0 (none of it) to 255 (all of it),
 * is the share of a grid of MASK_SAMPLES by MASK_SAMPLES points in it that
 * fall inside the shape. Drawing then only looks them up.
 *
 *   - a rounded corner: the top-left r by r square of a window with corner
 *     radius r (the other three corners are its mirror images), with the
 *     circle's centre at (r, r): `cov` how much of each pixel is inside
 *     the window's shape, `ring` how much of it is in the band of the given
 *     width along the edge (the outline, or tiling's border), which follows
 *     the curve;
 *   - the title bar's circle, LOOK_BTN_D across;
 *   - the three symbols the circles show under the pointer: an x, a bar,
 *     and two arrowheads pointing out to opposite corners (full screen).
 *
 * The disc and corner tests are exact integer arithmetic (a point at
 * (2i + 1) / (2 * MASK_SAMPLES) of a pixel, distances squared), so the
 * masks are the same on every machine, and tools/comp-check.py makes the
 * same corners to check QEMU's screen with. */
#include "paint.h"

#define MASK_SAMPLES 16   /* each way: 256 points a pixel */

struct corner_mask mask_float, mask_tile;
uint8_t mask_disc[LOOK_BTN_D * LOOK_BTN_D];
uint8_t mask_symbol[TITLE_BUTTONS][LOOK_BTN_D * LOOK_BTN_D];

/* n points of MASK_SAMPLES^2 as a coverage, rounded. */
static uint8_t coverage(uint32_t n)
{
    return (uint8_t)((n * 255 + MASK_SAMPLES * MASK_SAMPLES / 2) / (MASK_SAMPLES * MASK_SAMPLES));
}

/* How many points of pixel (i, j) are within radius r of (cx, cy), all in
 * pixels (the points in units of 1 / (2 * MASK_SAMPLES) pixel). */
static uint32_t points_within(int32_t i, int32_t j, int32_t cx, int32_t cy, int32_t r)
{
    const int64_t u = 2 * MASK_SAMPLES, rr = (int64_t)r * u * ((int64_t)r * u);
    uint32_t n = 0;
    for (int32_t t = 0; t < MASK_SAMPLES; t++)
        for (int32_t s = 0; s < MASK_SAMPLES; s++) {
            int64_t dx = (int64_t)i * u + 2 * s + 1 - (int64_t)cx * u;
            int64_t dy = (int64_t)j * u + 2 * t + 1 - (int64_t)cy * u;
            n += dx * dx + dy * dy <= rr;
        }
    return n;
}

static void make_corner(struct corner_mask *m, int32_t r, int32_t ring)
{
    m->r = r;
    for (int32_t j = 0; j < r; j++)
        for (int32_t i = 0; i < r; i++) {
            uint8_t cov = coverage(points_within(i, j, r, r, r));
            uint8_t inner = coverage(points_within(i, j, r, r, r - ring));
            m->cov[j * r + i] = cov;
            m->ring[j * r + i] = (uint8_t)(cov - inner);
        }
}

/* ---- the symbols: shapes in pixel coordinates of the circle's box ------------------- */

struct pt {
    float x, y;
};

/* Is p within w of the segment a-b? */
static bool near_segment(struct pt p, struct pt a, struct pt b, float w)
{
    float vx = b.x - a.x, vy = b.y - a.y, px = p.x - a.x, py = p.y - a.y;
    float k = (px * vx + py * vy) / (vx * vx + vy * vy);
    k = k < 0 ? 0 : k > 1 ? 1 : k;
    float dx = px - k * vx, dy = py - k * vy;
    return dx * dx + dy * dy <= w * w;
}

/* Is p inside triangle a, b, c (either winding)? */
static bool in_triangle(struct pt p, struct pt a, struct pt b, struct pt c)
{
    float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
    float d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
    float d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static bool in_symbol(enum title_button b, struct pt p)
{
    const float lo = 3.75f, hi = LOOK_BTN_D - 3.75f, mid = LOOK_BTN_D / 2.0f;
    switch (b) {
    case TITLE_CLOSE:
        return near_segment(p, (struct pt){ lo, lo }, (struct pt){ hi, hi }, 0.7f) ||
               near_segment(p, (struct pt){ hi, lo }, (struct pt){ lo, hi }, 0.7f);
    case TITLE_MINIMISE:
        return p.x >= 3.0f && p.x <= LOOK_BTN_D - 3.0f && p.y >= mid - 0.75f && p.y <= mid + 0.75f;
    default:   /* full screen: one arrowhead to the top left, one to the bottom right */
        return in_triangle(p, (struct pt){ 3.2f, 3.2f }, (struct pt){ 7.6f, 3.2f },
                           (struct pt){ 3.2f, 7.6f }) ||
               in_triangle(p, (struct pt){ 8.8f, 8.8f }, (struct pt){ 4.4f, 8.8f },
                           (struct pt){ 8.8f, 4.4f });
    }
}

/* How many points of pixel (i, j) are inside symbol b. */
static uint32_t symbol_points(enum title_button b, int32_t i, int32_t j)
{
    uint32_t n = 0;
    for (int32_t t = 0; t < MASK_SAMPLES; t++)
        for (int32_t s = 0; s < MASK_SAMPLES; s++) {
            struct pt p = { i + (2 * s + 1) / (2.0f * MASK_SAMPLES),
                            j + (2 * t + 1) / (2.0f * MASK_SAMPLES) };
            n += in_symbol(b, p);
        }
    return n;
}

static void make_symbol(enum title_button b)
{
    for (int32_t j = 0; j < LOOK_BTN_D; j++)
        for (int32_t i = 0; i < LOOK_BTN_D; i++)
            mask_symbol[b][j * LOOK_BTN_D + i] = coverage(symbol_points(b, i, j));
}

void mask_init(void)
{
    make_corner(&mask_float, LOOK_RADIUS, DECO_OUTLINE);
    make_corner(&mask_tile, LOOK_TILE_RADIUS, DECO_BORDER);
    for (int32_t j = 0; j < LOOK_BTN_D; j++)
        for (int32_t i = 0; i < LOOK_BTN_D; i++)
            mask_disc[j * LOOK_BTN_D + i] =
                coverage(points_within(i, j, LOOK_BTN_D / 2, LOOK_BTN_D / 2, LOOK_BTN_D / 2));
    for (int b = 0; b < TITLE_BUTTONS; b++)
        make_symbol((enum title_button)b);
}
