/* testscene.h: what the compositor's test scene (testscene.c) and the
 * tests that run it (utest's comp_paint.c) agree on: the pixels of a test
 * window, and the report of each paint.
 *
 * A test window's pixel (x, y) of its buffer, for the colour RRGGBB a
 * `win=` command names: the colour with (x * 7 + y * 13) & 0x3f in each
 * channel flipped (so a picture moved by one pixel is a different
 * picture), or the plain colour for a solid one. An opaque window's buffer
 * is xrgb8888 with 0xa5 in its unused top byte (the compositor must ignore
 * it); a translucent one's is argb8888, each channel premultiplied by the
 * alpha as testscene_pm says. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TESTSCENE_GARBAGE 0xa5000000u   /* an xrgb pixel's top byte, which means nothing */

/* The colour of pixel (x, y), 0x00RRGGBB, before any alpha. */
static inline uint32_t testscene_rgb(uint32_t rgb, int32_t x, int32_t y, bool solid)
{
    rgb &= 0xffffff;
    return solid ? rgb : rgb ^ (uint32_t)((x * 7 + y * 13) & 0x3f) * 0x010101u;
}

/* rgb at alpha a (0..255), premultiplied: each channel (c * a + 127) / 255. */
static inline uint32_t testscene_pm(uint32_t rgb, uint32_t a)
{
    uint32_t out = a << 24;
    for (int sh = 0; sh < 24; sh += 8)
        out |= ((rgb >> sh & 0xff) * a + 127) / 255 << sh;
    return out;
}

/* The report channel's startup role: SR_USER + this (SR_USER + 2 and 3
 * are compctl's and init's, ctl.c). */
#define TESTSCENE_REPORT_ROLE 4u

/* One paint, sent on the report channel as one message. */
struct testscene_report {
    uint32_t paint;       /* 1 for the first */
    uint32_t tiles;       /* tiles composed */
    uint32_t direct;      /* of them, copied straight from a full-screen window */
    uint32_t reserved;    /* 0 */
    uint64_t px;          /* output pixels written */
    uint64_t layer_px;    /* pixels drawn from windows' buffers, overdraw included */
    uint64_t ns;          /* how long it took */
};
