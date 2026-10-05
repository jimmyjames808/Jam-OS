/* utest's compositor painting tests (comp_paint.c, comp_look.c): what they
 * share (comp_ref.c). bin/compositor runs its test scene headless
 * (testscene.c: windows of known pixels with no client) into an image VMO
 * of ours, each paint reported on a channel of ours; a reference painter
 * here paints the same scene from its description, with our own blends,
 * coverages and shadow, never the compositor's code, so every pixel can
 * be compared exactly. Its model of the look is look.h's description:
 * the wallpaper's formula, the corners' supersampled circles, the
 * shadow's blur of three boxes (counted here as the ways three numbers add
 * up), the outline and borders. The title's text and circles it doesn't
 * paint: those pixels are marked unknown and checked by the tests. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "paint.h"
#include "testscene.h"

#define CP_W       320
#define CP_H       200
#define CP_REPORTS 16

/* One run of the test scene: its image and its reports. */
struct cp_run {
    uint32_t *image;           /* CP_W * CP_H, 0x00RRGGBB, ours (mapped read-only) */
    uint64_t image_size;
    struct testscene_report rep[CP_REPORTS];
    unsigned nrep;
};

/* bin/compositor headless at CP_W x CP_H running the scene's commands; it
 * must exit 0, leaving its job empty. */
bool cp_run(const char *const *cmds, unsigned n, struct cp_run *r);
void cp_done(struct cp_run *r);

/* One window as a `win=` command says it. look: 0 none, 't' floating, 'g'
 * tiled (testscene.c's flags). */
struct cp_win {
    int32_t x, y, w, h;
    uint32_t argb;             /* AARRGGBB */
    bool solid, opaque_region;
    char look;
    bool focused;
};

/* The `win=` command for w, into buf. */
void cp_win_cmd(char *buf, size_t n, const struct cp_win *w);
/* The windows over the wallpaper, bottom to top, into img; unknown[i] set
 * where the reference doesn't know the pixel (title text and circles). */
void ref_paint(uint32_t *img, uint8_t *unknown, const struct cp_win *v, unsigned n);
/* The arrow (cursors.c): its hot spot in its picture, the picture's box
 * with the pointer at (x, y), and a pixel of it that is all fill, from the
 * pointer (the SVG's unit (6, 10): well inside, clear of the outline). */
#define CP_ARROW_HX   (5 + CURSOR_PAD)
#define CP_ARROW_HY   (2 + CURSOR_PAD)
#define CP_ARROW_FILL 0xf6f3f8u
#define CP_FILL_DX    1
#define CP_FILL_DY    8
static inline struct comp_box cp_arrow_at(int32_t x, int32_t y)
{
    return (struct comp_box){ x - CP_ARROW_HX, y - CP_ARROW_HY, x - CP_ARROW_HX + CURSOR_IMG,
                              y - CP_ARROW_HY + CURSOR_IMG };
}
/* The wallpaper's pixel (x, y) on a CP_W x CP_H output (look.h's formula). */
uint32_t ref_wallpaper(int32_t x, int32_t y);
/* A floating window's frame, and its extent (with the shadow), on the output. */
struct comp_box ref_frame(const struct cp_win *w);
struct comp_box ref_extent(const struct cp_win *w);
/* The pixels of a run's image as want has them, but inside skip and where
 * unknown (NULL: none) is set. */
bool cp_same_image(const struct cp_run *r, const uint32_t *want, const uint8_t *unknown,
                   struct comp_box skip);
/* Run windows v plus the commands after them, paint, and check every
 * pixel the reference knows (but inside skip). */
bool cp_run_windows(const struct cp_win *v, unsigned n, const char *const *more, unsigned nmore,
                    struct comp_box skip, struct cp_run *r);
/* Pixels of colour c in box b of a run's image. */
unsigned cp_count_colour(const struct cp_run *r, struct comp_box b, uint32_t c);
