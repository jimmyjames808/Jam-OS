/* The desktop's drawing (desk.h): rounded boxes, frosted glass cards,
 * dividers, letter tiles, text and the strip's icons, each drawn into one
 * tile of a paint (paint.h) and clipped to it, by any painting worker at
 * once: nothing here writes anything but the tile's pixels.
 *
 * Rounded corners use a coverage table per radius (1 to UI_RADIUS_MAX),
 * made once by supersampling as mask.c does the windows' corners: a pixel's
 * coverage is the share of a 16 by 16 grid of points in it inside the
 * circle. A radius larger than half the box is cut to that. The icons are
 * the prototype's (16-unit pictures, 1.5 units wide strokes with round
 * ends), drawn with libfun's anti-aliased lines and circles, scaled to the
 * box they are given. */
#include <fun.h>
#include "desk.h"

#define SAMPLES 16

static uint8_t corner[UI_RADIUS_MAX + 1][UI_RADIUS_MAX * UI_RADIUS_MAX];

/* ---- corners -------------------------------------------------------------------------------- */

/* Pixel (i, j)'s coverage by the circle of radius r centred at (r, r). */
static uint8_t cover(int32_t i, int32_t j, int32_t r)
{
    const int64_t u = 2 * SAMPLES, rr = (int64_t)r * u * ((int64_t)r * u);
    uint32_t n = 0;
    for (int32_t t = 0; t < SAMPLES; t++)
        for (int32_t s = 0; s < SAMPLES; s++) {
            int64_t dx = (int64_t)i * u + 2 * s + 1 - (int64_t)r * u;
            int64_t dy = (int64_t)j * u + 2 * t + 1 - (int64_t)r * u;
            n += dx * dx + dy * dy <= rr;
        }
    return (uint8_t)((n * 255 + SAMPLES * SAMPLES / 2) / (SAMPLES * SAMPLES));
}

void ui_init(void)
{
    for (int32_t r = 1; r <= UI_RADIUS_MAX; r++)
        for (int32_t j = 0; j < r; j++)
            for (int32_t i = 0; i < r; i++)
                corner[r][j * r + i] = cover(i, j, r);
}

const uint8_t *ui_corner(int32_t r)
{
    return r >= 1 && r <= UI_RADIUS_MAX ? corner[r] : NULL;
}

/* The radius b can have (at most r, half its shorter side, UI_RADIUS_MAX). */
static int32_t radius(struct comp_box b, int32_t r)
{
    int32_t w = b.x2 - b.x1, h = b.y2 - b.y1, half = (w < h ? w : h) / 2;
    r = r < half ? r : half;
    return r > UI_RADIUS_MAX ? UI_RADIUS_MAX : r < 0 ? 0 : r;
}

/* Pixel (x, y)'s coverage (0..255) by box b with corners of radius r. */
static uint32_t round_cov(struct comp_box b, int32_t r, int32_t x, int32_t y)
{
    if (!box_contains(b, x, y))
        return 0;
    if (r <= 0)
        return 255;
    int32_t i = x < b.x1 + r ? x - b.x1 : x >= b.x2 - r ? b.x2 - 1 - x : -1;
    int32_t j = y < b.y1 + r ? y - b.y1 : y >= b.y2 - r ? b.y2 - 1 - y : -1;
    if (i < 0 || j < 0)
        return 255;
    return corner[r][j * r + i];
}

void ui_round(const struct tile_buf *t, struct comp_box b, int32_t r, uint32_t rgb, uint32_t a)
{
    struct comp_box in = box_intersect(b, t->b);
    if (box_empty(in) || !a)
        return;
    r = radius(b, r);
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint32_t *row = tile_row(t, y) - t->b.x1;
        bool edge = y < b.y1 + r || y >= b.y2 - r;
        for (int32_t x = in.x1; x < in.x2; x++) {
            uint32_t c = edge ? round_cov(b, r, x, y) * a / 255 : a;
            if (c)
                row[x] = paint_mix(row[x], rgb, c);
        }
    }
}

void ui_ring(const struct tile_buf *t, struct comp_box b, int32_t r, int32_t w, uint32_t rgb,
             uint32_t a)
{
    struct comp_box in = box_intersect(b, t->b);
    struct comp_box hole = { b.x1 + w, b.y1 + w, b.x2 - w, b.y2 - w };
    if (box_empty(in) || !a)
        return;
    r = radius(b, r);
    int32_t hr = radius(hole, r - w);
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint32_t *row = tile_row(t, y) - t->b.x1;
        for (int32_t x = in.x1; x < in.x2; x++) {
            uint32_t outer = round_cov(b, r, x, y), inner = round_cov(hole, hr, x, y);
            uint32_t c = outer > inner ? (outer - inner) * a / 255 : 0;
            if (c)
                row[x] = paint_mix(row[x], rgb, c);
        }
    }
}

/* ---- glass ---------------------------------------------------------------------------------- */

void ui_glass(const struct tile_buf *t, struct comp_box b, enum frost_slot slot, uint32_t a)
{
    shadow_box(t, b, LOOK_GLASS_R, a);
    struct comp_box in = box_intersect(b, t->b);
    if (box_empty(in) || !a)
        return;
    int32_t r = radius(b, LOOK_GLASS_R);
    struct comp_box inner = { b.x1 + 1, b.y1 + 1, b.x2 - 1, b.y2 - 1 };
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint32_t *row = tile_row(t, y) - t->b.x1;
        const uint32_t *back = frost_row(slot, in.x1, y);
        for (int32_t x = in.x1; x < in.x2; x++) {
            uint32_t cov = round_cov(b, r, x, y);
            if (!cov)
                continue;
            uint32_t base = back ? back[x - in.x1] : row[x];
            uint32_t glass = paint_mix(base, LOOK_GLASS_TINT, LOOK_GLASS_TINT_A);
            uint32_t ring = cov - round_cov(inner, r - 1, x, y) * cov / 255;
            if (ring)
                glass = paint_mix(glass, 0xffffff, ring * LOOK_GLASS_LINE_A / 255);
            row[x] = paint_mix(row[x], glass, cov * a / 255);
        }
    }
}

void ui_divider(const struct tile_buf *t, int32_t x1, int32_t x2, int32_t y)
{
    ui_round(t, (struct comp_box){ x1, y, x2, y + 1 }, 0, 0xffffff, LOOK_GLASS_LINE_A);
}

/* ---- text ----------------------------------------------------------------------------------- */

/* t as a libfun surface, and box b in its coordinates. */
static struct surf surf_of(const struct tile_buf *t)
{
    int32_t w = t->b.x2 - t->b.x1;
    return (struct surf){ t->px, w, t->b.y2 - t->b.y1, w };
}

static struct rect rect_in(const struct tile_buf *t, struct comp_box b)
{
    return (struct rect){ b.x1 - t->b.x1, b.y1 - t->b.y1, b.x2 - b.x1, b.y2 - b.y1 };
}

void ui_text(const struct tile_buf *t, struct comp_box b, const struct font *f, uint32_t rgb,
             bool centre, const char *str)
{
    if (!f || box_empty(box_intersect(b, t->b)))
        return;
    struct surf s = surf_of(t);
    struct rect r = rect_in(t, b);
    (void)font_draw_in(&s, &r, f, rgb, centre ? FONT_CENTRE : FONT_LEFT, str);
}

void ui_tile(const struct tile_buf *t, struct comp_box b, int32_t r, uint32_t col, char letter,
             const struct font *f)
{
    if (box_empty(box_intersect(b, t->b)))
        return;
    ui_round(t, b, r, col, 255);
    if (!letter) {
        int32_t d = (b.x2 - b.x1) * 9 / 16, x = b.x1 + (b.x2 - b.x1 - d) / 2;
        int32_t y = b.y1 + (b.y2 - b.y1 - d) / 2;
        ui_icon(t, (struct comp_box){ x, y, x + d, y + d }, UI_INFO, 0xffffff);
        return;
    }
    char s[2] = { letter, '\0' };
    ui_text(t, b, f, 0xffffff, true, s);
}

/* ---- icons ---------------------------------------------------------------------------------- */

/* An icon's drawing: its 16-unit picture mapped onto a box of the tile. */
struct pen {
    struct surf s;
    float x0, y0, k;               /* the picture's (0, 0) in the surface, pixels a unit */
    uint32_t rgb;
};

static void seg(const struct pen *p, float x1, float y1, float x2, float y2)
{
    line_aa(&p->s, p->x0 + x1 * p->k, p->y0 + y1 * p->k, p->x0 + x2 * p->k, p->y0 + y2 * p->k,
            1.5f * p->k, p->rgb, 255);
}

/* A rectangle's outline, x, y to x + w, y + h. */
static void outline(const struct pen *p, float x, float y, float w, float h)
{
    seg(p, x, y, x + w, y);
    seg(p, x + w, y, x + w, y + h);
    seg(p, x + w, y + h, x, y + h);
    seg(p, x, y + h, x, y);
}

/* An arc of radius r about (cx, cy) from angle a0 to a1 (radians, y down). */
static void arc(const struct pen *p, float cx, float cy, float r, float a0, float a1)
{
    const int steps = 8;
    float px = cx + r * (float)cosd(a0), py = cy + r * (float)sind(a0);
    for (int i = 1; i <= steps; i++) {
        float a = a0 + (a1 - a0) * (float)i / steps;
        float x = cx + r * (float)cosd(a), y = cy + r * (float)sind(a);
        seg(p, px, py, x, y);
        px = x;
        py = y;
    }
}

static void draw_icon(const struct pen *p, enum ui_icon icon)
{
    switch (icon) {
    case UI_FLOATING:
        outline(p, 1.5f, 3, 9, 7);
        outline(p, 5.5f, 6.5f, 9, 7);
        break;
    case UI_TILING:
        outline(p, 1.5f, 2.5f, 13, 11);
        seg(p, 8, 2.5f, 8, 13.5f);
        seg(p, 8, 8, 14.5f, 8);
        break;
    case UI_NETWORK:
        seg(p, 3, 13, 3, 11);
        seg(p, 6.5f, 13, 6.5f, 8.5f);
        seg(p, 10, 13, 10, 6);
        seg(p, 13.5f, 13, 13.5f, 3);
        break;
    case UI_VOLUME: {
        static const float xy[] = { 2.5f, 6, 4.9f, 6, 8.5f, 3, 8.5f, 13, 4.9f, 10, 2.5f, 10 };
        for (int i = 0; i < 6; i++)
            seg(p, xy[2 * i], xy[2 * i + 1], xy[(2 * i + 2) % 12], xy[(2 * i + 3) % 12]);
        arc(p, 8.59f, 8, 3.4f, -0.785f, 0.785f);
        arc(p, 8.52f, 8, 6, -0.775f, 0.775f);
        break;
    }
    case UI_SEARCH:
        ring_aa(&p->s, p->x0 + 7 * p->k, p->y0 + 7 * p->k, 4.6f * p->k, 1.6f * p->k, p->rgb, 255);
        seg(p, 10.5f, 10.5f, 14, 14);
        break;
    case UI_TERMINAL:
        seg(p, 3, 4.5f, 6.5f, 8);
        seg(p, 6.5f, 8, 3, 11.5f);
        seg(p, 8.5f, 12, 13, 12);
        break;
    case UI_INFO:
        ring_aa(&p->s, p->x0 + 8 * p->k, p->y0 + 8 * p->k, 6 * p->k, 1.5f * p->k, p->rgb, 255);
        seg(p, 8, 7.2f, 8, 11);
        disc_aa(&p->s, p->x0 + 8 * p->k, p->y0 + 5 * p->k, 0.9f * p->k, p->rgb, 255);
        break;
    case UI_PLUS:
        seg(p, 2, 8, 14, 8);
        seg(p, 8, 2, 8, 14);
        break;
    }
}

void ui_icon(const struct tile_buf *t, struct comp_box b, enum ui_icon icon, uint32_t rgb)
{
    if (box_empty(box_intersect(b, t->b)))
        return;
    struct pen p = { surf_of(t), (float)(b.x1 - t->b.x1), (float)(b.y1 - t->b.y1),
                     (float)(b.x2 - b.x1) / 16.0f, rgb };
    draw_icon(&p, icon);
}
