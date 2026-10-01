/* The 8x16 console font (Spleen), shared by the programs that draw text
 * themselves: the console and the fun apps (user/apps/fun). user/lib/font_8x16.c,
 * written by `make font` from the same source as the kernel's copy, and
 * user/lib/font_latin.c, the same font's Latin letters (`make font` too).
 * Each table is an object of its own in libos.a: only the programs that
 * use it link it in. */
#pragma once

#include <stdint.h>

/* 128 glyphs of 16 rows, one byte a row, bit 7 the leftmost pixel. */
extern const uint8_t font_8x16[128][16];

/* U+00A0 .. U+017F (Latin-1's letters and Latin Extended-A: accents,
 * "e" with an acute, "Y" with a diaeresis), laid out as font_8x16: entry
 * i is code point FONT_LATIN_FIRST + i. */
#define FONT_LATIN_FIRST 0xa0u
#define FONT_LATIN_N     224u
extern const uint8_t font_latin[FONT_LATIN_N][16];
