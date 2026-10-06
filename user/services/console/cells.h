/* console: a cell of the terminal and how one is drawn (cellpaint.c),
 * apart from the rest of console.h so that cellpaint.c and its utest can
 * include libfun's <fun.h> beside it (console.h's names, line() and the
 * like, would clash with libfun's). console.h includes it.
 *
 * Two fonts draw the cells:
 *   - the 8x16 bitmap (Spleen, <font.h>): the full-screen console's
 *     always (`nocomp`), and a window's with `terminal.font = bitmap`;
 *   - the smooth one (the default in a window): JetBrains Mono through
 *     libfun's smooth text (<fun.h>), Regular and Bold, at TERM_PX pixels
 *     to the em. Its cells are the face's advance rounded to whole pixels
 *     wide (9 at 15: JetBrains Mono's 0.6 em is exactly 9, so every glyph
 *     sits where the font means it) and 1.4 times the size high (21), with
 *     the face's ascent and descent centred in that height; each glyph is
 *     drawn at its cell's origin, clipped to its cell (a cell is drawn
 *     alone: a glyph reaching into its neighbour would be left behind when
 *     only one of them is drawn again). The box drawing comes from the
 *     font (its lines run past the em box, so clipped to the cell they meet
 *     the next cell's); the block elements, U+2580..U+259F, are filled as
 *     exact fractions of the cell (the font's are made for its own 1.32 em
 *     line and would leave gaps at 1.4). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <font.h>

#define GW 8                    /* the bitmap font's cell */
#define GH 16
#define TERM_PX 15              /* the smooth font's size, pixels to the em */
#define TERM_LINE_X5 7          /* ... its line height: 7/5 (1.4) of the size */
#define WIN_PAD 10              /* a window's padding on every side, the background's */

/* Palette indices (cell_palette has the colours). */
enum { C_BLACK, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_GREY,
       C_DARK, C_BRED, C_BGREEN, C_BYELLOW, C_BBLUE, C_BMAGENTA, C_BCYAN, C_WHITE };
#define ATTR(fg, bg) ((uint8_t)((fg) | (bg) << 4))
/* The 16 colours as 0xRRGGBB. */
extern const uint32_t cell_palette[16];

struct cell {
    uint16_t ch;    /* the glyph: ASCII, G_LATIN + n or G_BOX + n */
    uint8_t  attr;  /* ATTR(fg, bg) */
    uint8_t  style; /* S_*: how the smooth font draws it (the bitmap ignores it) */
};
#define S_BOLD 1u   /* ESC [ 1 m: JetBrains Mono Bold */
/* U+00A0 + n (n < FONT_LATIN_N, <font.h>) is glyph G_LATIN + n; U+2500 + n
 * (n < G_BOX_N: the box drawing and block elements) is G_BOX + n. */
#define G_LATIN 128u
#define G_BOX   (G_LATIN + FONT_LATIN_N)
#define G_BOX_N 160u

static inline bool cell_same(struct cell a, struct cell b)
{
    return a.ch == b.ch && a.attr == b.attr && a.style == b.style;
}

struct font;
/* How the cells are drawn: their size, and the font. */
struct cell_look {
    int32_t w, h;                    /* a cell, pixels */
    int32_t base;                    /* the smooth font's baseline, pixels below a cell's top */
    const struct font *reg, *bold;   /* the smooth font (NULL: the 8x16 bitmap) */
};

/* The 8x16 bitmap's look. */
void cell_look_bitmap(struct cell_look *l);
/* The smooth look of fonts reg and bold (bold may be NULL: reg draws it). */
void cell_look_smooth(const struct font *reg, const struct font *bold, struct cell_look *l);
/* The smooth look at TERM_PX, its fonts opened (baked: ~75 KiB each); false
 * if they can't be (no memory): *l is then the bitmap's. */
bool cell_look_open(struct cell_look *l);
/* Its fonts closed (none: nothing), and *l the bitmap's. */
void cell_look_close(struct cell_look *l);

/* The code point cell c shows. */
uint32_t cell_cp(struct cell c);
/* The 8x16 bits of cell c's glyph, one byte a row, the leftmost pixel
 * the top bit: the font's, or block (filled) for the block elements the
 * full-screen programs draw with (half blocks, the full block, the three
 * shades); '?' for the rest of the box drawing, which Spleen's table here
 * doesn't have. */
const uint8_t *cell_bits(struct cell c, uint8_t block[GH]);
/* How a cell is marked on the screen (grid_walk's marks). */
#define CELL_CURSOR   1u   /* the cursor: fg and bg swapped */
#define CELL_SELECTED 2u   /* in the mouse's selection: the background tinted blackcurrant */
#define SEL_TINT      0x7f77ddu   /* blackcurrant (the jam colours), ... */
#define SEL_TINT_A    115u        /* ... at 45% (of 255) over the cell's background */
/* Cell c with marks (CELL_*) in look l with its top left at (x, y) of a
 * buffer of 0xRRGGBB pixels, w x h of them, stride a row; only the cell's
 * own pixels (and of those, only the buffer's) are written. The cursor
 * wins over the selection's tint. */
void cell_paint(uint32_t *px, int32_t stride, int32_t w, int32_t h, const struct cell_look *l,
                int32_t x, int32_t y, struct cell c, uint8_t marks);
/* The background a selected cell of background bg shows. */
uint32_t cell_selected_bg(uint32_t bg);
