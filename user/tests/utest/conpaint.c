/* utest: the console's cells as drawn (user/services/console/cellpaint.c,
 * linked in, with libfun's smooth text):
 *
 * conpaint_metrics  JetBrains Mono at TERM_PX: 9x21 cells, baseline 16;
 *                   every glyph the console can put in a cell is one
 *                   cell wide; the box drawing's lines meet the next
 *                   cell's (a full row and a full column of ink at the
 *                   edges), and it has its own glyphs (not .notdef), as
 *                   Latin Extended-A has; the bitmap's look is 8x16
 * conpaint_cells    a cell drawn writes its own pixels only, the
 *                   background where there is no ink; a space is all
 *                   background, the cursor (inverted) all foreground;
 *                   each glyph is font_draw's at the cell's origin and
 *                   baseline, clipped to the cell
 * conpaint_bold     a bold cell is JetBrains Mono Bold's pixels, not
 *                   Regular's
 * conpaint_blocks   the block elements fill exact fractions of the
 *                   cell: halves that meet, eighths, quadrants, shades
 * conpaint_bitmap   terminal.font = bitmap: the 8x16 font's bits, the
 *                   bitmap's half blocks, '?' for the rest of the box
 *                   drawing, bold ignored */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <os.h>
#include "cells.h"
#include "utest.h"

#define BW 40           /* the test buffer: room for a cell and a border */
#define BH 40
#define CX 11           /* where the cell goes */
#define CY 7
#define SENTINEL 0x123456u

static uint32_t buf[BH * BW];

static void clear_buf(void)
{
    for (int i = 0; i < BW * BH; i++)
        buf[i] = SENTINEL;
}

static struct cell cell_of(uint16_t ch, uint8_t attr, uint8_t style)
{
    return (struct cell){ ch, attr, style };
}

/* Every pixel outside the cell at (CX, CY) in look l is untouched. */
static bool outside_untouched(const struct cell_look *l)
{
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++) {
            bool in = x >= CX && x < CX + l->w && y >= CY && y < CY + l->h;
            if (!in && buf[y * BW + x] != SENTINEL)
                FAIL("pixel (%d, %d) outside the cell written", x, y);
        }
    return true;
}

static bool look_open(struct cell_look *l)
{
    if (!cell_look_open(l))
        FAIL("cell_look_open: no smooth font");
    return true;
}

bool t_conpaint_metrics(void)
{
    struct cell_look l;
    if (!look_open(&l))
        return false;
    /* JetBrains Mono: 1000 units to the em, every advance 600 (0.6 em:
     * 9 pixels at 15), ascender 1020, descender -300 (hhea): 15.3 and 4.5
     * pixels, rounded up 16 and 5; the line 1.4 x 15 = 21. */
    CHECK_EQ(l.w, 9);
    CHECK_EQ(l.h, 21);
    CHECK_EQ(l.base, 16);
    CHECK(l.reg && l.bold && l.reg != l.bold);
    CHECK(TERM_PX * 135 <= l.h * 100 && l.h * 100 <= TERM_PX * 145);   /* 1.35 to 1.45 */
    /* Every glyph a cell can hold is one cell wide, both weights. */
    char u[4];
    for (uint32_t cp = 0x20; cp < 0x25a0; cp++) {
        if (cp == 0x7f)
            cp = 0xa0;
        if (cp == 0x180)
            cp = 0x2500;
        if (cp < 0x80) {
            u[0] = (char)cp, u[1] = 0;
        } else if (cp < 0x800) {
            u[0] = (char)(0xc0 | cp >> 6), u[1] = (char)(0x80 | (cp & 0x3f)), u[2] = 0;
        } else {
            u[0] = (char)(0xe0 | cp >> 12), u[1] = (char)(0x80 | (cp >> 6 & 0x3f));
            u[2] = (char)(0x80 | (cp & 0x3f)), u[3] = 0;
        }
        if (font_width(l.reg, u) != 9 || font_width(l.bold, u) != 9)
            FAIL("U+%04x is %d / %d pixels wide", cp, font_width(l.reg, u), font_width(l.bold, u));
    }
    /* The box drawing's lines reach the cell's edges, so neighbours meet:
     * '─' (U+2500) has a row of ink from the first column to the last, '│'
     * (U+2502) a column of it from the top row to the bottom. */
    clear_buf();
    cell_paint(buf, BW, BW, BH, &l, CX, CY, cell_of(G_BOX + 0x00, ATTR(C_WHITE, C_BLACK), 0), false);
    bool row_full = false;
    for (int y = CY; y < CY + l.h && !row_full; y++) {
        int n = 0;
        for (int x = CX; x < CX + l.w; x++)
            n += buf[y * BW + x] == cell_palette[C_WHITE];
        row_full = n == l.w;
    }
    CHECK(row_full);
    clear_buf();
    cell_paint(buf, BW, BW, BH, &l, CX, CY, cell_of(G_BOX + 0x02, ATTR(C_WHITE, C_BLACK), 0), false);
    bool col_full = false;
    for (int x = CX; x < CX + l.w && !col_full; x++) {
        int n = 0;
        for (int y = CY; y < CY + l.h; y++)
            n += buf[y * BW + x] == cell_palette[C_WHITE];
        col_full = n == l.h;
    }
    CHECK(col_full);
    if (!outside_untouched(&l))
        return false;
    /* They are glyphs of their own, not .notdef's box: '─', '┼' and 'ł'
     * (U+0142) each draw differently from U+0378 (no glyph anywhere). */
    uint32_t ref[BW * BH];
    clear_buf();
    struct surf s = { buf, BW, BH, BW };
    font_draw(&s, NULL, l.reg, CX, CY + l.base, 0xffffff, "\xcd\xb8");
    memcpy(ref, buf, sizeof(ref));
    static const char *own[] = { "\xe2\x94\x80", "\xe2\x94\xbc", "\xc5\x82" };
    for (unsigned i = 0; i < 3; i++) {
        clear_buf();
        font_draw(&s, NULL, l.reg, CX, CY + l.base, 0xffffff, own[i]);
        CHECK(memcmp(ref, buf, sizeof(ref)) != 0);
    }
    cell_look_close(&l);
    CHECK(!l.reg && l.w == GW && l.h == GH);
    struct cell_look b;
    cell_look_bitmap(&b);
    CHECK(b.w == 8 && b.h == 16 && !b.reg && !b.bold);
    return true;
}

bool t_conpaint_cells(void)
{
    struct cell_look l;
    if (!look_open(&l))
        return false;
    uint8_t attr = ATTR(C_BYELLOW, C_BLUE);
    uint32_t fg = cell_palette[C_BYELLOW], bg = cell_palette[C_BLUE];
    /* A space: all background; the cursor on it: all foreground. */
    for (int inv = 0; inv < 2; inv++) {
        clear_buf();
        cell_paint(buf, BW, BW, BH, &l, CX, CY, cell_of(' ', attr, 0), inv);
        for (int y = CY; y < CY + l.h; y++)
            for (int x = CX; x < CX + l.w; x++)
                CHECK_EQ(buf[y * BW + x], inv ? fg : bg);
        if (!outside_untouched(&l))
            return false;
    }
    /* A letter: the background filled, then font_draw's glyph at the
     * cell's origin on its baseline, clipped to the cell. */
    static const uint16_t chars[] = { 'g', 'M', '_', '|', '@', G_LATIN + 0x49 /* é */,
                                      G_LATIN + 0xa2 /* ł */, G_BOX + 0x0c /* ┌ */,
                                      G_BOX + 0x6d /* ╭ */, G_BOX + 0x3c /* ┼ */ };
    for (unsigned k = 0; k < sizeof(chars) / sizeof(chars[0]); k++) {
        struct cell c = cell_of(chars[k], attr, 0);
        uint32_t want[BW * BH];
        for (int i = 0; i < BW * BH; i++)
            want[i] = SENTINEL;
        for (int y = CY; y < CY + l.h; y++)
            for (int x = CX; x < CX + l.w; x++)
                want[y * BW + x] = bg;
        uint32_t cp = cell_cp(c);
        char u[4] = { 0 };
        if (cp < 0x80)
            u[0] = (char)cp;
        else if (cp < 0x800)
            u[0] = (char)(0xc0 | cp >> 6), u[1] = (char)(0x80 | (cp & 0x3f));
        else
            u[0] = (char)(0xe0 | cp >> 12), u[1] = (char)(0x80 | (cp >> 6 & 0x3f)),
            u[2] = (char)(0x80 | (cp & 0x3f));
        struct surf s = { want, BW, BH, BW };
        struct rect r = { CX, CY, l.w, l.h };
        font_draw(&s, &r, l.reg, CX, CY + l.base, fg, u);
        clear_buf();
        cell_paint(buf, BW, BW, BH, &l, CX, CY, c, false);
        if (memcmp(want, buf, sizeof(buf)))
            FAIL("cell U+%04x isn't font_draw's glyph in its cell", cp);
        int ink = 0;
        for (int i = 0; i < BW * BH; i++)
            ink += buf[i] != bg && buf[i] != SENTINEL;
        CHECK(ink > 4);
    }
    /* A cell at a buffer's edge: only the buffer's pixels are written. */
    clear_buf();
    cell_paint(buf, BW, BW, BH, &l, BW - 4, BH - 5, cell_of('W', attr, 0), false);
    cell_paint(buf, BW, BW, BH, &l, -5, -10, cell_of(G_BOX + 0x88, attr, 0), false);
    CHECK_EQ(buf[(BH - 1) * BW + BW - 1], bg);
    CHECK_EQ(buf[0], fg);
    CHECK_EQ(buf[BW / 2], SENTINEL);
    cell_look_close(&l);
    return true;
}

bool t_conpaint_bold(void)
{
    struct cell_look l;
    if (!look_open(&l))
        return false;
    uint8_t attr = ATTR(C_WHITE, C_BLACK);
    uint32_t reg[BW * BH], bold[BW * BH];
    for (const char *p = "MHabg0#"; *p; p++) {
        clear_buf();
        cell_paint(buf, BW, BW, BH, &l, CX, CY, cell_of((uint16_t)*p, attr, 0), false);
        memcpy(reg, buf, sizeof(buf));
        clear_buf();
        cell_paint(buf, BW, BW, BH, &l, CX, CY, cell_of((uint16_t)*p, attr, S_BOLD), false);
        memcpy(bold, buf, sizeof(buf));
        CHECK(memcmp(reg, bold, sizeof(reg)) != 0);
        /* Bold's pixels are font_draw's with the Bold face. */
        clear_buf();
        for (int y = CY; y < CY + l.h; y++)
            for (int x = CX; x < CX + l.w; x++)
                buf[y * BW + x] = cell_palette[C_BLACK];
        char u[2] = { *p, 0 };
        struct surf s = { buf, BW, BH, BW };
        struct rect r = { CX, CY, l.w, l.h };
        font_draw(&s, &r, l.bold, CX, CY + l.base, cell_palette[C_WHITE], u);
        CHECK(memcmp(buf, bold, sizeof(buf)) == 0);
        /* More ink than Regular. */
        int nr = 0, nb = 0;
        for (int i = 0; i < BW * BH; i++) {
            nr += reg[i] != cell_palette[C_BLACK] && reg[i] != SENTINEL;
            nb += bold[i] != cell_palette[C_BLACK] && bold[i] != SENTINEL;
        }
        CHECK(nb >= nr);
    }
    cell_look_close(&l);
    return true;
}

/* Cell pixel (x, y) (from the cell's top left) after cell_paint. */
static uint32_t at(int x, int y)
{
    return buf[(CY + y) * BW + CX + x];
}

bool t_conpaint_blocks(void)
{
    struct cell_look l;
    if (!look_open(&l))
        return false;
    uint8_t attr = ATTR(C_BGREEN, C_BLACK);
    uint32_t fg = cell_palette[C_BGREEN], bg = cell_palette[C_BLACK];
    int w = l.w, h = l.h, mh = (h * 4 + 4) / 8, mw = (w * 4 + 4) / 8;
    struct { uint32_t cp; int x0, y0, x1, y1; } rects[] = {
        { 0x2588, 0, 0, w, h },                  /* full */
        { 0x2580, 0, 0, w, mh },                 /* upper half */
        { 0x2584, 0, mh, w, h },                 /* lower half: meets the upper */
        { 0x258c, 0, 0, mw, h },                 /* left half */
        { 0x2590, mw, 0, w, h },                 /* right half */
        { 0x2581, 0, h - (h + 4) / 8, w, h },    /* lower one eighth */
        { 0x2594, 0, 0, w, (h + 4) / 8 },        /* upper one eighth */
        { 0x258f, 0, 0, (w + 4) / 8, h },        /* left one eighth */
        { 0x2598, 0, 0, mw, mh },                /* quadrant upper left */
        { 0x2597, mw, mh, w, h },                /* quadrant lower right */
    };
    for (unsigned k = 0; k < sizeof(rects) / sizeof(rects[0]); k++) {
        clear_buf();
        cell_paint(buf, BW, BW, BH, &l, CX, CY, cell_of((uint16_t)(G_BOX + rects[k].cp - 0x2500),
                                                      attr, 0), false);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                bool in = x >= rects[k].x0 && x < rects[k].x1 && y >= rects[k].y0 &&
                          y < rects[k].y1;
                if (at(x, y) != (in ? fg : bg))
                    FAIL("U+%04x: pixel (%d, %d) is %06x", rects[k].cp, x, y, at(x, y));
            }
        if (!outside_untouched(&l))
            return false;
    }
    /* The shades: the whole cell, fg over bg at a quarter, a half, three
     * quarters. */
    static const uint32_t share[3] = { 64, 128, 192 };
    for (unsigned k = 0; k < 3; k++) {
        clear_buf();
        cell_paint(buf, BW, BW, BH, &l, CX, CY, cell_of((uint16_t)(G_BOX + 0x91 + k), attr, 0),
                   false);
        uint32_t want = px_over(bg, argb_pm(fg, share[k]));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                CHECK_EQ(at(x, y), want);
    }
    /* Two quadrants make the half they cover, with no gap and no overlap:
     * 2599 (upper left, lower left, lower right) and 259D (upper right)
     * together are the full block. */
    clear_buf();
    cell_paint(buf, BW, BW, BH, &l, CX, CY, cell_of(G_BOX + 0x99, attr, 0), false);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            CHECK_EQ(at(x, y), x >= mw && y < mh ? bg : fg);
    cell_look_close(&l);
    return true;
}

bool t_conpaint_bitmap(void)
{
    struct cell_look l;
    cell_look_bitmap(&l);
    uint8_t attr = ATTR(C_WHITE, C_BLACK);
    uint32_t fg = cell_palette[C_WHITE], bg = cell_palette[C_BLACK];
    static const struct { uint16_t ch; const uint8_t *bits; } want[] = {
        { 'A', font_8x16['A'] },
        { G_LATIN + 0x49, font_latin[0x49] },    /* é */
        { G_LATIN + 0xa2, font_latin[0xa2] },    /* ł */
        { G_BOX + 0x00, font_8x16['?'] },        /* ─: not in the bitmap's table */
    };
    for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++)
        for (uint8_t style = 0; style <= S_BOLD; style++) {   /* bold changes nothing */
            clear_buf();
            cell_paint(buf, BW, BW, BH, &l, CX, CY, cell_of(want[k].ch, attr, style), false);
            for (int y = 0; y < GH; y++)
                for (int x = 0; x < GW; x++)
                    CHECK_EQ(at(x, y), (want[k].bits[y] & (0x80 >> x)) ? fg : bg);
            if (!outside_untouched(&l))
                return false;
        }
    /* The bitmap's own block elements, as the full-screen console draws. */
    uint8_t block[GH];
    const uint8_t *b = cell_bits(cell_of(G_BOX + 0x80, attr, 0), block);
    for (int y = 0; y < GH; y++)
        CHECK_EQ(b[y], y < GH / 2 ? 0xff : 0);
    b = cell_bits(cell_of(G_BOX + 0x88, attr, 0), block);
    for (int y = 0; y < GH; y++)
        CHECK_EQ(b[y], 0xff);
    b = cell_bits(cell_of(G_BOX + 0x92, attr, 0), block);
    CHECK(b[0] == 0xaa && b[1] == 0x55);
    CHECK(cell_bits(cell_of(G_BOX + 0x9f, attr, 0), block) == font_8x16['?']);
    CHECK(cell_cp(cell_of(G_BOX + 0x9f, attr, 0)) == 0x259f);
    CHECK(cell_cp(cell_of(G_LATIN, attr, 0)) == 0xa0);
    CHECK(cell_cp(cell_of('x', attr, 0)) == 'x');
    return true;
}
