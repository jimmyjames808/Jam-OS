/* cursorgen: the compositor's cursor set, drawn on the Mac at build time
 * (the Makefile runs it) into a C table the compositor links
 * (build/gen/cursorset.c: user/services/compositor/cursors.c reads it),
 * and a preview PNG to look at.
 *
 *     cursorgen <cursorset.c> [preview.png]
 *
 * The set is the owner's "style C" (docs/G1-PLAN.md "The look"), whose
 * exact source is docs/design/cursors.svg: the arrow, the four resize
 * arrows, move, the text bar, the hand, and busy in CURSOR_BUSY_FRAMES
 * frames of its turn. Drawing them takes far too long on QEMU's emulated
 * CPU to do at every start (seconds), and nothing about them changes at
 * run time, so they are made here, once.
 *
 * Each shape is the SVG's paths, flattened into points (arcs and curves
 * cut into short lines) in its 24-unit square, a unit a pixel at 1x, and
 * rasterised by supersampling: each pixel is SAMPLES by SAMPLES points,
 * each coloured as the SVG paints it there:
 *   - a filled shape: the outline colour within half the outline's width
 *     (0.55) of an edge (a centred stroke with round joins), else the fill
 *     inside the shape (even-odd), else nothing;
 *   - the text bar: its centre lines, the fill within 1.2 of them, the
 *     outline colour within 2.3 (a 2.4 white stroke over a 4.6 dark one,
 *     round caps);
 *   - busy: a ring of radius 8 so stroked, with a raspberry arc 2.4 wide
 *     and 14 units long (round caps) over it, turning once a second.
 * A pixel is the mean of its points, premultiplied (points that hit
 * nothing count as transparent). Under it, the shadow: the shape's
 * coverage taken 1.5 units lower, blurred (a Gaussian of sigma 0.75: CSS's
 * drop-shadow blur of 1.5), at 55% black.
 *
 * Every picture is IMG square: the 24 units with PAD pixels round them for
 * the outline and the shadow (paint.h's CURSOR_IMG and CURSOR_PAD, which
 * the table's header checks). Hot spots, from the SVG's comment: the arrow
 * (5, 2.5), the hand (10.8, 2), the others the centre; rounded down to the
 * pixel they fall in. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fontpreview.h"

#define IMG          28
#define PAD          2
#define BUSY_FRAMES  30
#define SAMPLES      5
#define FILL         0xf6f3f8u   /* the SVG's fill */
#define INK          0x262a35u   /* its outline */
#define ARC          0xd4537eu   /* busy's arc: raspberry */
#define HALF_OUTLINE 0.55f
#define SHADOW_DY    1.5f
#define SHADOW_A     140         /* 55% */
#define PTS_MAX      128

/* The shapes, in the compositor's order (comp.h's enum cursor_shape). */
enum shape { ARROW, RESIZE_EW, RESIZE_NS, RESIZE_NWSE, RESIZE_NESW, MOVE, TEXT, HAND, BUSY,
             SHAPES };
static const char *const names[SHAPES] = { "arrow", "resize-ew", "resize-ns", "resize-nwse",
                                            "resize-nesw", "move", "text", "hand", "busy" };

struct pts {
    float xy[2 * PTS_MAX];
    int n;
};

static const float pi = 3.14159265f;

/* ---- paths -------------------------------------------------------------------------------- */

static void pt(struct pts *p, float x, float y)
{
    if (p->n < PTS_MAX) {
        p->xy[2 * p->n] = x;
        p->xy[2 * p->n + 1] = y;
        p->n++;
    }
}

static void pts(struct pts *p, const float *xy, int n)
{
    for (int i = 0; i < n; i++)
        pt(p, xy[2 * i], xy[2 * i + 1]);
}

/* An arc about (cx, cy) of radius r from angle a0 to a1 (radians, y down:
 * increasing is clockwise on the screen, as SVG's sweep flag 1). */
static void arc(struct pts *p, float cx, float cy, float r, float a0, float a1)
{
    const int steps = 10;
    for (int i = 1; i <= steps; i++) {
        float a = a0 + (a1 - a0) * (float)i / steps;
        pt(p, cx + r * cosf(a), cy + r * sinf(a));
    }
}

/* A cubic Bezier from the last point. */
static void curve(struct pts *p, float x1, float y1, float x2, float y2, float x3, float y3)
{
    float x0 = p->xy[2 * p->n - 2], y0 = p->xy[2 * p->n - 1];
    const int steps = 12;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps, u = 1 - t;
        pt(p, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
           u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
    }
}

/* p turned by a (radians) about (12, 12), as the SVG's rotate(). */
static void turn(struct pts *p, float a)
{
    float c = cosf(a), s = sinf(a);
    for (int i = 0; i < p->n; i++) {
        float x = p->xy[2 * i] - 12, y = p->xy[2 * i + 1] - 12;
        p->xy[2 * i] = 12 + x * c - y * s;
        p->xy[2 * i + 1] = 12 + x * s + y * c;
    }
}

static const float arrow_xy[] = { 5, 2.5f, 5, 18.7f, 9.1f, 15, 11.8f, 21.2f, 14.5f, 20,
                                  11.8f, 13.9f, 17.4f, 13.9f };
static const float ew_xy[] = { 1.8f, 12, 7.2f, 7.2f, 7.2f, 10.25f, 16.8f, 10.25f, 16.8f, 7.2f,
                               22.2f, 12, 16.8f, 16.8f, 16.8f, 13.75f, 7.2f, 13.75f, 7.2f, 16.8f };
static const float move_xy[] = {
    12, 1.6f, 16, 5.8f, 13.75f, 5.8f, 13.75f, 10.25f, 18.2f, 10.25f, 18.2f, 8, 22.4f, 12,
    18.2f, 16, 18.2f, 13.75f, 13.75f, 13.75f, 13.75f, 18.2f, 16, 18.2f, 12, 22.4f, 8, 18.2f,
    10.25f, 18.2f, 10.25f, 13.75f, 5.8f, 13.75f, 5.8f, 16, 1.6f, 12, 5.8f, 8, 5.8f, 10.25f,
    10.25f, 10.25f, 10.25f, 5.8f, 8, 5.8f,
};

/* The hand: its path, the SVG's arcs drawn about their centres (the last
 * one's, from (4.4, 13.8) to (6.8, 11.7) with radius 1.6, worked out by
 * SVG's endpoint-to-centre rule: (5.687, 12.850), from 2.505 to 5.481). */
static void hand(struct pts *p)
{
    pt(p, 9.2f, 3.2f);
    arc(p, 10.8f, 3.2f, 1.6f, pi, 2 * pi);
    pt(p, 12.4f, 9.8f);
    pt(p, 12.9f, 9.8f);
    pt(p, 12.9f, 8.6f);
    arc(p, 14.5f, 8.6f, 1.6f, pi, 2 * pi);
    pt(p, 16.1f, 10.1f);
    pt(p, 16.5f, 10.1f);
    pt(p, 16.5f, 9.5f);
    arc(p, 18.1f, 9.5f, 1.6f, pi, 2 * pi);
    pt(p, 19.7f, 15.1f);
    curve(p, 19.7f, 18.7f, 17.3f, 21.3f, 13.9f, 21.3f);
    pt(p, 12.3f, 21.3f);
    curve(p, 10.3f, 21.3f, 8.9f, 20.4f, 7.8f, 18.8f);
    pt(p, 4.4f, 13.8f);
    arc(p, 5.687f, 12.850f, 1.6f, 2.505f, 5.481f);
    pt(p, 9.2f, 14.0f);
}

/* The text bar's centre lines: five strokes (start index, count) in one list. */
static void text_bar(struct pts *p, int *starts, int *counts)
{
    static const float ends[5][2] = { { 8.4f, 4.4f }, { 15.6f, 4.4f }, { 12, 7.2f },
                                      { 12, 16.8f }, { 12, 16.8f } };
    for (int k = 0; k < 5; k++) {
        starts[k] = p->n;
        pt(p, ends[k][0], ends[k][1]);
        if (k == 0)
            curve(p, 10.4f, 4.4f, 12, 5.2f, 12, 7.2f);
        else if (k == 1)
            curve(p, 13.6f, 4.4f, 12, 5.2f, 12, 7.2f);
        else if (k == 2)
            pt(p, 12, 16.8f);
        else if (k == 3)
            curve(p, 12, 18.8f, 10.4f, 19.6f, 8.4f, 19.6f);
        else
            curve(p, 12, 18.8f, 13.6f, 19.6f, 15.6f, 19.6f);
        counts[k] = p->n - starts[k];
    }
}

/* ---- what a point is ----------------------------------------------------------------------- */

static bool inside(const struct pts *p, float x, float y)
{
    bool in = false;
    for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
        float xi = p->xy[2 * i], yi = p->xy[2 * i + 1], xj = p->xy[2 * j], yj = p->xy[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            in = !in;
    }
    return in;
}

static float seg_d2(float x, float y, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, px = x - ax, py = y - ay, l2 = vx * vx + vy * vy;
    float k = l2 > 0 ? (px * vx + py * vy) / l2 : 0;
    k = k < 0 ? 0 : k > 1 ? 1 : k;
    float dx = px - k * vx, dy = py - k * vy;
    return dx * dx + dy * dy;
}

/* The squared distance to points [from, from + n) as a line (closed: back to the first). */
static float line_d2(const struct pts *p, int from, int n, bool closed, float x, float y)
{
    float best = 1e9f;
    for (int i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
        int a = from + i, b = from + (i + 1) % n;
        float d = seg_d2(x, y, p->xy[2 * a], p->xy[2 * a + 1], p->xy[2 * b], p->xy[2 * b + 1]);
        best = d < best ? d : best;
    }
    return best;
}

struct painter {
    enum shape shape;
    struct pts p;
    int starts[5], counts[5];      /* the text bar's strokes */
    float arc_a0;                  /* busy: where the arc starts (radians) */
};

static bool busy_at(const struct painter *pa, float x, float y, uint32_t *rgb)
{
    float dx = x - 12, dy = y - 12, d = sqrtf(dx * dx + dy * dy), off = fabsf(d - 8);
    if (off > 2.3f)
        return false;
    *rgb = off <= 1.2f ? FILL : INK;
    float len = 14.0f / 8.0f;   /* the dash, in radians of the ring */
    for (int k = 0; k <= 32; k++) {   /* within 1.2 of the arc (its ends round) */
        float a = pa->arc_a0 + len * (float)k / 32;
        float ax = 12 + 8 * cosf(a), ay = 12 + 8 * sinf(a);
        if ((x - ax) * (x - ax) + (y - ay) * (y - ay) <= 1.44f) {
            *rgb = ARC;
            break;
        }
    }
    return true;
}

/* What the shape paints at (x, y) in its units: a colour, or nothing (false). */
static bool paint_at(const struct painter *pa, float x, float y, uint32_t *rgb)
{
    if (pa->shape == BUSY)
        return busy_at(pa, x, y, rgb);
    if (pa->shape == TEXT) {
        float best = 1e9f;
        for (int k = 0; k < 5; k++) {
            float d = line_d2(&pa->p, pa->starts[k], pa->counts[k], false, x, y);
            best = d < best ? d : best;
        }
        if (best > 2.3f * 2.3f)
            return false;
        *rgb = best <= 1.2f * 1.2f ? FILL : INK;
        return true;
    }
    if (line_d2(&pa->p, 0, pa->p.n, true, x, y) <= HALF_OUTLINE * HALF_OUTLINE) {
        *rgb = INK;
        return true;
    }
    if (!inside(&pa->p, x, y))
        return false;
    *rgb = FILL;
    return true;
}

/* ---- the pictures ------------------------------------------------------------------------- */

/* Pixel (i, j)'s premultiplied colour, and its coverage taken dy lower. */
static uint32_t pixel(const struct painter *pa, int i, int j, float dy, uint32_t *cov_dy)
{
    uint32_t sum[3] = { 0, 0, 0 }, hit = 0, low = 0, rgb;
    for (int t = 0; t < SAMPLES; t++)
        for (int s = 0; s < SAMPLES; s++) {
            float x = (float)(i - PAD) + (s + 0.5f) / SAMPLES;
            float y = (float)(j - PAD) + (t + 0.5f) / SAMPLES;
            if (paint_at(pa, x, y, &rgb)) {
                hit++;
                for (int c = 0; c < 3; c++)
                    sum[c] += rgb >> (16 - 8 * c) & 0xff;
            }
            low += paint_at(pa, x, y - dy, &rgb);
        }
    const uint32_t n = SAMPLES * SAMPLES;
    *cov_dy = (low * 255 + n / 2) / n;
    uint32_t out = ((hit * 255 + n / 2) / n) << 24;
    for (int c = 0; c < 3; c++)
        out |= ((sum[c] + n / 2) / n) << (16 - 8 * c);
    return out;
}

/* The shadow (cov, blurred, 55% black) under the picture px. */
static void shadow_under(uint32_t *px, const uint8_t *cov)
{
    static const uint32_t k[5] = { 29, 411, 1000, 411, 29 };   /* sigma 0.75; sum 1880 */
    static uint32_t row[IMG * IMG];
    for (int j = 0; j < IMG; j++)
        for (int i = 0; i < IMG; i++) {
            uint32_t s = 0;
            for (int d = -2; d <= 2; d++)
                if (i + d >= 0 && i + d < IMG)
                    s += cov[j * IMG + i + d] * k[d + 2];
            row[j * IMG + i] = s;
        }
    const uint64_t one = 1880ull * 1880 * 255;
    for (int j = 0; j < IMG; j++)
        for (int i = 0; i < IMG; i++) {
            uint64_t s = 0;
            for (int d = -2; d <= 2; d++)
                if (j + d >= 0 && j + d < IMG)
                    s += (uint64_t)row[(j + d) * IMG + i] * k[d + 2];
            uint32_t sa = (uint32_t)((s * SHADOW_A + one / 2) / one);
            uint32_t *p = &px[j * IMG + i], a = *p >> 24;
            uint32_t add = (sa * (255 - a) + 127) / 255;   /* black under it: alpha only */
            *p = (*p & 0xffffff) | (a + add) << 24;
        }
}

static void draw(const struct painter *pa, uint32_t *px)
{
    static uint8_t cov[IMG * IMG];
    for (int j = 0; j < IMG; j++)
        for (int i = 0; i < IMG; i++) {
            uint32_t c;
            px[j * IMG + i] = pixel(pa, i, j, SHADOW_DY, &c);
            cov[j * IMG + i] = (uint8_t)c;
        }
    shadow_under(px, cov);
}

static void painter_for(struct painter *pa, enum shape s)
{
    memset(pa, 0, sizeof(*pa));
    pa->shape = s;
    switch (s) {
    case ARROW:
        pts(&pa->p, arrow_xy, 7);
        break;
    case RESIZE_EW:
    case RESIZE_NS:
    case RESIZE_NWSE:
    case RESIZE_NESW:
        pts(&pa->p, ew_xy, 10);
        turn(&pa->p, s == RESIZE_NS ? pi / 2 : s == RESIZE_NWSE ? pi / 4
                     : s == RESIZE_NESW ? -pi / 4 : 0);
        break;
    case MOVE:
        pts(&pa->p, move_xy, 24);
        break;
    case TEXT:
        text_bar(&pa->p, pa->starts, pa->counts);
        break;
    case HAND:
        hand(&pa->p);
        break;
    default:
        break;
    }
}

/* ---- out ---------------------------------------------------------------------------------- */

#define PICTURES (SHAPES - 1 + BUSY_FRAMES)
static uint32_t pics[PICTURES][IMG * IMG];

static void make(void)
{
    struct painter pa;
    for (int s = 0; s < BUSY; s++) {
        painter_for(&pa, (enum shape)s);
        draw(&pa, pics[s]);
    }
    painter_for(&pa, BUSY);
    for (int f = 0; f < BUSY_FRAMES; f++) {
        pa.arc_a0 = 2 * pi * (float)f / BUSY_FRAMES;
        draw(&pa, pics[BUSY + f]);
    }
}

static int write_c(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        return 1;
    }
    fprintf(f, "/* The compositor's cursor set (user/services/compositor/cursors.c reads\n"
               " * it): generated by tools/cursorgen.c from docs/design/cursors.svg's shapes.\n"
               " * Do not edit; `make` makes it again. */\n"
               "#include \"paint.h\"\n\n"
               "_Static_assert(CURSOR_IMG == %d && CURSOR_PAD == %d && CURSOR_BUSY_FRAMES == %d &&\n"
               "               CURSOR_SHAPES == %d, \"tools/cursorgen.c draws what paint.h says\");\n\n"
               "const uint32_t cursor_pictures[CURSOR_SHAPES - 1 + CURSOR_BUSY_FRAMES]"
               "[CURSOR_IMG * CURSOR_IMG] = {\n", IMG, PAD, BUSY_FRAMES, SHAPES);
    for (int k = 0; k < PICTURES; k++) {
        if (k < BUSY)
            fprintf(f, "    {   /* %s */\n", names[k]);
        else
            fprintf(f, "    {   /* busy, frame %d */\n", k - BUSY);
        for (int i = 0; i < IMG * IMG; i++)
            fprintf(f, "%s0x%08x,%s", i % 8 ? " " : "        ", pics[k][i],
                    i % 8 == 7 ? "\n" : "");
        fprintf(f, "    },\n");
    }
    fprintf(f, "};\n");
    return fclose(f) ? 1 : 0;
}

/* Every picture 4 times as big, over grey and over white. */
static int write_png(const char *path)
{
    const int k = 4, cols = PICTURES, w = cols * IMG * k, h = 2 * IMG * k;
    static uint32_t out[PICTURES * IMG * 4 * 2 * IMG * 4];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int pic = x / (IMG * k), i = x % (IMG * k) / k, j = y % (IMG * k) / k;
            uint32_t bg = y < IMG * k ? 0x6a6f78 : 0xffffff, p = pics[pic][j * IMG + i];
            uint32_t a = p >> 24, o = 0;
            for (int c = 0; c < 24; c += 8)
                o |= (((p >> c & 0xff) * 255 + (bg >> c & 0xff) * (255 - a) + 127) / 255) << c;
            out[y * w + x] = o;
        }
    return png_write(path, out, w, h, w) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: cursorgen <cursorset.c> [preview.png]\n");
        return 2;
    }
    make();
    int st = write_c(argv[1]);
    if (!st && argc == 3)
        st = write_png(argv[2]);
    return st;
}
