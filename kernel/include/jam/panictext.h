/* The panic screen's words and the glyphs baked for them (the screen is
 * kernel/debug/panicscreen.c; the owner's design "B1", docs/G1-PLAN.md).
 *
 * A crashed kernel runs no font code: the few glyphs the screen needs are
 * baked on the Mac at build time by tools/panicglyphs.c (part of
 * build/host/fontpreview, which builds libfun's own font code from the
 * same Inter file the desktop uses) into build/gen/panicglyphs.c, which
 * the kernel links. The tool reads this header for the words, so a word
 * changed here is baked again by the next `make`.
 *
 * Two sizes of Inter Regular, at 1x (look.h: every output is 1x):
 *   - the title, PANIC_TITLE_PX: the characters of the PANIC_TITLE_*
 *     lines;
 *   - the small line, PANIC_SMALL_PX: those of the PANIC_SMALL_* lines,
 *     the code's (PANIC_CODE_CHARS) and the digits.
 * An ASCII apostrophe is baked as Inter's U+2019 (a typographic one).
 * Each glyph is baked as libfun's font.c bakes it: its coverage at four
 * positions a quarter pixel apart, its advance in 1/256 pixels, and the
 * kerning between the baked characters; the kernel lays a line out as
 * libfun's fontdraw.c does, so a line is drawn to the same pixels. Only
 * this header and the generated table are shared with the host tool. */
#pragma once

#include <stdint.h>

#define PANIC_TITLE_PX 17   /* pixels to the em */
#define PANIC_SMALL_PX 12

/* The colours (0xRRGGBB): the dark background, the title, the small lines. */
#define PANIC_BG        0x11141bu
#define PANIC_INK_TITLE 0xe6e9ecu
#define PANIC_INK_SMALL 0x8b98a6u

/* Case 1: the stored kernel will start (it restarts by itself). */
#define PANIC_TITLE_RESTARTING "Jam OS hit a problem and is restarting"
/* Case 2: it can't recover; then the firmware reset after PANIC_RESET_S. */
#define PANIC_TITLE_STUCK      "Jam OS hit a problem it can't recover from"
#define PANIC_SMALL_COUNTDOWN  "Restarting the PC in %u s"   /* digits baked too */
/* ... and if the firmware reset did not happen. */
#define PANIC_TITLE_NORESET    "Jam OS couldn't restart the PC"
#define PANIC_SMALL_POWER      "Hold the power button to turn it off, then on again"
/* The code: JAM-<kind>-<4 hex digits> (<jam/panicscreen.h>). */
#define PANIC_CODE_CHARS       "JAM-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"

/* The characters each size has. */
#define PANIC_TITLE_CHARS PANIC_TITLE_RESTARTING PANIC_TITLE_STUCK PANIC_TITLE_NORESET
#define PANIC_SMALL_CHARS PANIC_SMALL_COUNTDOWN PANIC_SMALL_POWER PANIC_CODE_CHARS

#define PANIC_PHASES 4   /* positions each glyph is baked at, 1/4 pixel apart */

/* One glyph at one position: its ink's box and coverage. */
struct panic_glyph {
    int8_t   x, y;    /* the box's top-left corner from the pen on the baseline, pixels */
    uint8_t  w, h;    /* the box's size (0 x 0: no ink, a space) */
    uint32_t off;     /* its coverage, w * h bytes (0..255) row after row, at cov + off */
};

/* The pen moves by d (1/256 pixels) more after slot l when slot r follows. */
struct panic_kern {
    uint8_t l, r;
    int16_t d;
};

/* One size: the characters baked (by slot), and how to find them. */
struct panic_font {
    int16_t px;                                    /* pixels to the em */
    int16_t ascent, descent;                       /* above and below the baseline, pixels */
    int16_t cap_h;                                 /* a capital's height, pixels */
    uint8_t nslots;                                /* characters baked */
    const int8_t *slot_of;                         /* [128]: an ASCII character's slot, -1 none */
    const int32_t *adv;                            /* nslots advances, 1/256 pixels */
    const struct panic_glyph (*g)[PANIC_PHASES];   /* each slot at each position */
    uint16_t nkern;                                /* pairs in kern[] */
    const struct panic_kern *kern;
    const uint8_t *cov;                            /* every glyph's coverage */
};

extern const struct panic_font panic_font_title, panic_font_small;

/* Lines the host tool drew with libfun itself (font_draw, from the same
 * file), for ktest panicscreen_text_matches_libfun: `text` in rgb over
 * bg on a w x h picture, its pen starting at x on baseline y; hash is the
 * FNV-1a of the picture's 0xRRGGBB words, row after row. */
struct panic_text_ref {
    const char *text;
    int title;          /* 1: panic_font_title, 0: panic_font_small */
    uint32_t rgb, bg;
    int16_t w, h, x, y;
    uint32_t hash;
};

extern const struct panic_text_ref panic_text_refs[];
extern const unsigned panic_text_nrefs;
