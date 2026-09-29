/* The 8x16 console font (Spleen), shared by the programs that draw text
 * themselves: the console and the fun apps (user/apps/fun). user/lib/font_8x16.c,
 * written by `make font` from the same source as the kernel's copy. */
#pragma once

#include <stdint.h>

/* 128 glyphs of 16 rows, one byte a row, bit 7 the leftmost pixel. */
extern const uint8_t font_8x16[128][16];
