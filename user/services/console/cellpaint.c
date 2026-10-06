/* console: one cell drawn, in the 8x16 bitmap or the smooth font, and
 * the cells' size in each (cells.h says how they look).
 *
 * No state of the console's and no system calls but font_open's: the
 * full-screen console (screen.c, cell_bits) and the window (winpaint.c)
 * draw with it, and utest checks it (user/tests/utest/conpaint.c). */
#include <fun.h>
#include "cells.h"

/* The palette (C_* in cells.h). */
const uint32_t cell_palette[16] = {
    0x101018, 0xcc4444, 0x44aa44, 0xccaa33, 0x4466cc, 0xaa44aa, 0x44aaaa, 0xb0b0b0,
    0x707070, 0xff6666, 0x66dd66, 0xffdd55, 0x6699ff, 0xdd77dd, 0x66dddd, 0xf0f0f0,
};

/* ---- the looks ------------------------------------------------------------------- */

void cell_look_bitmap(struct cell_look *l)
{
    *l = (struct cell_look){ GW, GH, 0, NULL, NULL };
}

void cell_look_smooth(const struct font *reg, const struct font *bold, struct cell_look *l)
{
    const struct font_metrics *m = font_metrics(reg);
    int32_t ink = m->ascent + m->descent;
    int32_t h = (m->px * TERM_LINE_X5 + 2) / 5;   /* 1.4 x the size, rounded */
    *l = (struct cell_look){
        .w = font_width(reg, "M"),   /* the advance, rounded to whole pixels */
        .h = h > ink ? h : ink,
        .reg = reg,
        .bold = bold ? bold : reg,
    };
    l->base = (l->h - ink) / 2 + m->ascent;
}

bool cell_look_open(struct cell_look *l)
{
    struct font *reg = NULL, *bold = NULL;
    if (font_open(FONT_MONO, TERM_PX, &reg) != OK || font_open(FONT_MONO_BOLD, TERM_PX, &bold) != OK) {
        font_close(reg);
        cell_look_bitmap(l);
        return false;
    }
    cell_look_smooth(reg, bold, l);
    return true;
}

void cell_look_close(struct cell_look *l)
{
    if (l->bold != l->reg)
        font_close((struct font *)l->bold);
    font_close((struct font *)l->reg);
    cell_look_bitmap(l);
}

/* ---- what a cell shows ------------------------------------------------------------- */

uint32_t cell_cp(struct cell c)
{
    if (c.ch >= G_BOX && c.ch < G_BOX + G_BOX_N)
        return 0x2500 + (c.ch - G_BOX);
    if (c.ch >= G_LATIN && c.ch < G_LATIN + FONT_LATIN_N)
        return FONT_LATIN_FIRST + (c.ch - G_LATIN);
    return c.ch < 0x80 ? c.ch : '?';
}

/* The block elements the bitmap draws itself, as G_BOX + n. */
#define B_UPPER  (G_BOX + 0x80)
#define B_LOWER  (G_BOX + 0x84)
#define B_FULL   (G_BOX + 0x88)
#define B_LIGHT  (G_BOX + 0x91)
#define B_MEDIUM (G_BOX + 0x92)
#define B_DARK   (G_BOX + 0x93)

const uint8_t *cell_bits(struct cell c, uint8_t block[GH])
{
    static const uint8_t shade[3][2] = { { 0x88, 0x22 }, { 0xaa, 0x55 }, { 0x77, 0xdd } };
    switch (c.ch) {
    case B_UPPER:
    case B_LOWER:
    case B_FULL:
    case B_LIGHT:
    case B_MEDIUM:
    case B_DARK:
        for (int i = 0; i < GH; i++)
            block[i] = c.ch == B_UPPER ? (i < GH / 2 ? 0xff : 0)
                     : c.ch == B_LOWER ? (i >= GH / 2 ? 0xff : 0)
                     : c.ch == B_FULL  ? 0xff
                                       : shade[c.ch - B_LIGHT][i & 1];
        return block;
    }
    if (c.ch >= G_LATIN && c.ch < G_LATIN + FONT_LATIN_N)
        return font_latin[c.ch - G_LATIN];
    return font_8x16[c.ch < 0x80 ? c.ch : '?'];
}

/* ---- drawing ------------------------------------------------------------------------- */

/* A buffer to draw a cell into, and the cell's box in it. */
struct dst {
    uint32_t *px;
    int32_t   stride, w, h;
    int32_t   x, y, cw, ch;   /* the cell */
};

/* [x0, x1) x [y0, y1) of the cell (pixels from its top left) in rgb,
 * inside the buffer. */
static void part_fill(const struct dst *d, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t rgb)
{
    x0 += d->x, x1 += d->x, y0 += d->y, y1 += d->y;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > d->w ? d->w : x1;
    y1 = y1 > d->h ? d->h : y1;
    for (int32_t y = y0; y < y1; y++) {
        uint32_t *p = d->px + (int64_t)y * d->stride;
        for (int32_t x = x0; x < x1; x++)
            p[x] = rgb;
    }
}

/* The block elements U+2580..U+259F as parts of the cell in eighths
 * (x0, y0, x1, y1), the quadrants as bits (1 upper left, 2 upper right, 4
 * lower left, 8 lower right), the shades as fg's share over bg (of 255). */
static const struct {
    uint8_t x0, y0, x1, y1, quads, shade;
} blocks[32] = {
    { 0, 0, 8, 4, 0, 0 },   /* 2580 upper half */
    { 0, 7, 8, 8, 0, 0 },   /* 2581 lower one eighth ... */
    { 0, 6, 8, 8, 0, 0 },
    { 0, 5, 8, 8, 0, 0 },
    { 0, 4, 8, 8, 0, 0 },   /* 2584 lower half */
    { 0, 3, 8, 8, 0, 0 },
    { 0, 2, 8, 8, 0, 0 },
    { 0, 1, 8, 8, 0, 0 },   /* 2587 ... lower seven eighths */
    { 0, 0, 8, 8, 0, 0 },   /* 2588 full block */
    { 0, 0, 7, 8, 0, 0 },   /* 2589 left seven eighths ... */
    { 0, 0, 6, 8, 0, 0 },
    { 0, 0, 5, 8, 0, 0 },
    { 0, 0, 4, 8, 0, 0 },   /* 258C left half */
    { 0, 0, 3, 8, 0, 0 },
    { 0, 0, 2, 8, 0, 0 },
    { 0, 0, 1, 8, 0, 0 },   /* 258F ... left one eighth */
    { 4, 0, 8, 8, 0, 0 },   /* 2590 right half */
    { 0, 0, 8, 8, 0, 64 },  /* 2591 light shade */
    { 0, 0, 8, 8, 0, 128 }, /* 2592 medium shade */
    { 0, 0, 8, 8, 0, 192 }, /* 2593 dark shade */
    { 0, 0, 8, 1, 0, 0 },   /* 2594 upper one eighth */
    { 7, 0, 8, 8, 0, 0 },   /* 2595 right one eighth */
    { 0, 0, 0, 0, 4, 0 },   /* 2596 quadrant lower left */
    { 0, 0, 0, 0, 8, 0 },   /* 2597 lower right */
    { 0, 0, 0, 0, 1, 0 },   /* 2598 upper left */
    { 0, 0, 0, 0, 13, 0 },  /* 2599 upper left, lower left and lower right */
    { 0, 0, 0, 0, 9, 0 },   /* 259A upper left and lower right */
    { 0, 0, 0, 0, 7, 0 },   /* 259B upper left, upper right and lower left */
    { 0, 0, 0, 0, 11, 0 },  /* 259C upper left, upper right and lower right */
    { 0, 0, 0, 0, 2, 0 },   /* 259D upper right */
    { 0, 0, 0, 0, 6, 0 },   /* 259E upper right and lower left */
    { 0, 0, 0, 0, 14, 0 },  /* 259F upper right, lower left and lower right */
};

/* e eighths of n pixels, rounded. */
static int32_t eighths(int32_t n, int32_t e)
{
    return (n * e + 4) / 8;
}

/* Block element cp (U+2580..U+259F) in fg over a cell filled with bg. */
static void block_paint(const struct dst *d, uint32_t cp, uint32_t fg, uint32_t bg)
{
    unsigned i = cp - 0x2580;
    int32_t w = d->cw, h = d->ch;
    if (blocks[i].shade) {
        part_fill(d, 0, 0, w, h, px_over(bg, argb_pm(fg, blocks[i].shade)));
        return;
    }
    if (blocks[i].quads) {
        int32_t mx = eighths(w, 4), my = eighths(h, 4);
        for (unsigned q = 0; q < 4; q++)
            if (blocks[i].quads & (1u << q))
                part_fill(d, q & 1 ? mx : 0, q & 2 ? my : 0, q & 1 ? w : mx, q & 2 ? h : my, fg);
        return;
    }
    part_fill(d, eighths(w, blocks[i].x0), eighths(h, blocks[i].y0), eighths(w, blocks[i].x1),
         eighths(h, blocks[i].y1), fg);
}

/* cp as UTF-8 into u (5 bytes), terminated. */
static void utf8_put(uint32_t cp, char u[5])
{
    if (cp < 0x80) {
        u[0] = (char)cp, u[1] = 0;
    } else if (cp < 0x800) {
        u[0] = (char)(0xc0 | cp >> 6), u[1] = (char)(0x80 | (cp & 0x3f)), u[2] = 0;
    } else {
        u[0] = (char)(0xe0 | cp >> 12), u[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        u[2] = (char)(0x80 | (cp & 0x3f)), u[3] = 0;
    }
}

void cell_paint(uint32_t *px, int32_t stride, int32_t w, int32_t h, const struct cell_look *l,
                int32_t x, int32_t y, struct cell c, bool inverse)
{
    uint32_t fg = cell_palette[c.attr & 15], bg = cell_palette[c.attr >> 4];
    if (inverse) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    struct dst d = { px, stride, w, h, x, y, l->w, l->h };
    if (!l->reg) {
        uint8_t block[GH];
        const uint8_t *g = cell_bits(c, block);
        for (int32_t i = 0; i < GH; i++) {
            if (y + i < 0 || y + i >= h)
                continue;
            uint32_t *row = px + (int64_t)(y + i) * stride;
            for (int32_t j = 0; j < GW; j++)
                if (x + j >= 0 && x + j < w)
                    row[x + j] = (g[i] & (0x80 >> j)) ? fg : bg;
        }
        return;
    }
    uint32_t cp = cell_cp(c);
    if (cp >= 0x2580 && cp <= 0x259f) {
        part_fill(&d, 0, 0, l->w, l->h, bg);
        block_paint(&d, cp, fg, bg);
        return;
    }
    part_fill(&d, 0, 0, l->w, l->h, bg);
    if (cp == ' ')
        return;
    char u[5];
    utf8_put(cp, u);
    struct surf s = { px, w, h, stride };
    struct rect r = { x, y, l->w, l->h };
    font_draw(&s, &r, (c.style & S_BOLD) ? l->bold : l->reg, x, y + l->base, fg, u);
}
