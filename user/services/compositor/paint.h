/* paint.h: what the painting files of the compositor share among
 * themselves (output.c, paint.c, title.c, shape.c, mask.c, wallpaper.c,
 * cursor.c, clock.c, testscene.c); what other files need is in comp.h,
 * and the look's colours and sizes in look.h.
 *
 * The model (paint.c has the whole of it): the scene's damage is cut into
 * tiles, at most TILE_W by TILE_H pixels; the workers of libfun's pool
 * each compose a tile into a buffer of their own (cached memory), bottom
 * to top, then write it to the output with wide stores. The output is
 * write-combining framebuffer memory, which is never read. A tile's
 * drawing steps (decorations, a window's buffer, the cursor) each take
 * the tile's box on the output and its buffer, whose rows are the box's
 * width apart. */
#pragma once

#include "comp.h"
#include "look.h"

/* A tile: at most this many pixels across and down. The PC's numbers
 * (fbbench, 2026-10-05) show the framebuffer takes 10.8 GB/s whatever the
 * tile's shape, from two threads up; 16 rows is libfun's band, and 512
 * pixels keep a tile (32 KiB) in a core's first-level cache. */
#define TILE_W 512
#define TILE_H 16

/* The pixels of a tile: box b of the output, row y of it at
 * px + (y - b.y1) * (b.x2 - b.x1). */
struct tile_buf {
    struct comp_box b;
    uint32_t *px;
};

static inline uint32_t *tile_row(const struct tile_buf *t, int32_t y)
{
    return t->px + (uint64_t)(y - t->b.y1) * (uint32_t)(t->b.x2 - t->b.x1);
}

/* ---- output.c ---------------------------------------------------------------------- */

/* Row y of the output from x, n pixels of 0x00RRGGBB (the top byte is
 * ignored), in the output's own format, with 16-byte stores. */
void output_put(int32_t x, int32_t y, const uint32_t *src, int n);

/* ---- paint.c ----------------------------------------------------------------------- */

/* What the last paint did, for the clock's log line and the test scene. */
struct paint_stats {
    uint64_t px;          /* output pixels written */
    uint64_t layer_px;    /* pixels drawn from windows' buffers, overdraw included */
    uint32_t tiles;       /* tiles composed */
    uint32_t direct;      /* tiles copied straight from a full-screen window */
    uint64_t ns;          /* how long it took */
};
extern struct paint_stats paint_last;

/* p's pixels in box `in` (inside its surface's box on the output) hide
 * whatever is below: an xrgb buffer, or inside one box of its opaque
 * region (a hint the compositor takes: those pixels are then copied, alpha
 * ignored, so nothing below is ever needed). */
bool paint_opaque_over(const struct comp_window *w, struct comp_box in);

/* a over b by coverage c (0: all a, 255: all b), per channel, rounded as
 * px_over rounds; top byte 0. */
static inline uint32_t paint_mix(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t x = (a >> sh & 0xff) * (255 - c) + (b >> sh & 0xff) * c + 128;
        out |= ((x + (x >> 8)) >> 8) << sh;
    }
    return out;
}

/* ---- title.c ----------------------------------------------------------------------- */

/* The titles' fonts, baked (once, before the workers start). ERR_NO_MEMORY:
 * none, and the titles are in the 8x16 text. */
status_t title_init(void);
/* w's decorations (title bar and its circles, outline or borders) where
 * they meet t. */
void title_draw(const struct comp_window *w, const struct tile_buf *t);
/* The window whose circles the pointer is over, which show their symbols
 * (paint.c sets it before each paint's tiles), or NULL. */
extern const struct comp_window *title_hovered;
/* The box round the hit boxes of w's circles (empty: none): what changes
 * when the pointer comes onto them or leaves. */
struct comp_box title_buttons_hit(const struct comp_window *w);
/* The colour of w's outline (floating) or border (tiled), by its focus. */
uint32_t title_edge_colour(const struct comp_window *w);

/* ---- mask.c ------------------------------------------------------------------------ */

/* A rounded corner: the top-left r by r square (the others are its mirror
 * images), each pixel's coverage by the window's shape, and by the band of
 * its outline or border along the curve (ring <= cov). */
#define MASK_CORNER_MAX LOOK_RADIUS
struct corner_mask {
    int32_t r;                                         /* the radius */
    uint8_t cov[MASK_CORNER_MAX * MASK_CORNER_MAX];    /* r * r of them, row by row */
    uint8_t ring[MASK_CORNER_MAX * MASK_CORNER_MAX];
};
extern struct corner_mask mask_float, mask_tile;     /* LOOK_RADIUS, LOOK_TILE_RADIUS */
extern uint8_t mask_disc[LOOK_BTN_D * LOOK_BTN_D];   /* a title bar circle */
extern uint8_t mask_symbol[TITLE_BUTTONS][LOOK_BTN_D * LOOK_BTN_D];   /* x, -, full screen */
/* All of them made (once, before the workers paint). */
void mask_init(void);

/* ---- shape.c ----------------------------------------------------------------------- */

/* The shadow's edge profiles, made once (after mask_init). */
void shape_init(void);
/* w's corner mask, or NULL: its corners are square (no decorations,
 * maximised, or a frame too small for the radius). */
const struct corner_mask *shape_corners(const struct comp_window *w);
/* Does b meet one of w's rounded corner squares (where w doesn't hide what
 * is below)? */
bool shape_corner_meets(const struct comp_window *w, struct comp_box b);
/* w's shadow where it meets t (floating windows only; outside the
 * window's shape only: under the window it would never show). */
void shadow_draw(const struct comp_window *w, const struct tile_buf *t);
/* A card's: the focused window's shadow of box b (corners of radius r),
 * its darkness scaled by a (of 255), where it meets t. What it reaches
 * past b: LOOK_SHADOW_SIDE, _ABOVE and _BELOW. */
void shadow_box(const struct tile_buf *t, struct comp_box b, int32_t r, uint32_t a);
/* Around drawing w into t: the pixels below its corners kept first
 * (save: room for 4 * MASK_CORNER_MAX^2), then w's corners cut round, its
 * outline or border along the curve. */
#define SHAPE_SAVE_PX (4 * MASK_CORNER_MAX * MASK_CORNER_MAX)
void shape_save(const struct comp_window *w, const struct tile_buf *t, uint32_t *save);
void shape_clip(const struct comp_window *w, const struct tile_buf *t, const uint32_t *save);

/* ---- wallpaper.c ------------------------------------------------------------------- */

/* The wallpaper made at the output's size (look.h), on the pool's workers.
 * ERR_NO_MEMORY: none (the background is then LOOK_WALL_BASE, flat). */
status_t wallpaper_init(int32_t w, int32_t h);
/* t all wallpaper. */
void wallpaper_fill(const struct tile_buf *t);
/* The wallpaper's row y, or NULL (none made, or no such row). */
const uint32_t *wallpaper_row(int32_t y);

/* What paint.c lends the desktop's drawing (desk.h): */
/* Everything under the desktop's cards where it meets t (the wallpaper,
 * the windows, the strip): a card's backdrop. worker: the pool's. */
void paint_under(const struct tile_buf *t, uint32_t worker);
/* w's frame (decorations, its buffer, corners cut round over what t has
 * there), with no shadow: an animation's picture. save: SHAPE_SAVE_PX. */
void paint_window(const struct comp_window *w, const struct tile_buf *t, uint32_t *save);

/* ---- cursors.c: the cursor set's pictures -------------------------------------------- */

#define CURSOR_IMG          28   /* each picture's side: 24 units and CURSOR_PAD round */
#define CURSOR_PAD          2    /* for the outline and the shadow */
#define CURSOR_BUSY_FRAMES  30   /* busy's turn, a frame each 1/30 s */

/* A picture: premultiplied 0xAARRGGBB, w x h, drawn with (hot_x, hot_y)
 * at the pointer. */
struct cursor_image {
    int32_t w, h, hot_x, hot_y;
    const uint32_t *px;
};
/* Every picture drawn (once, at start: they are only read after). */
void cursors_init(void);
/* Shape s's picture (busy's frame for time t). */
const struct cursor_image *cursors_get(enum cursor_shape s, uint64_t t);

/* ---- cursor.c ---------------------------------------------------------------------- */

/* The cursor's pictures made (call after output_open). */
void cursor_init(void);
/* Shown or not whether the pointer has moved yet (the test scene's). */
void cursor_show(bool on);
/* The cursor where it meets t: drawn last, over everything. */
void cursor_draw(const struct tile_buf *t);
/* Where the cursor is on the output (empty: not shown). */
struct comp_box cursor_box(void);

/* ---- clock.c ----------------------------------------------------------------------- */

/* The paint clock at hz paints a second (default 60). */
void clock_init(uint32_t hz);

/* ---- testscene.c ------------------------------------------------------------------- */

/* The `testscene` argument's commands (argv[first] on), with no clients:
 * the exit code. */
int testscene_run(int argc, char **argv, int first);
/* testdesk.c: a desktop command (1 done, 0 refused, -1 not one of its
 * own), and the desktop's layout to the log after a paint. */
int  testdesk_command(const char *c);
void testdesk_report(void);
/* One kind of frame timed (testscene.c's `bench`): damage() before each
 * of a few paints, the median and worst in a `compositor: bench:` line. */
void testscene_bench(const char *what, void (*damage)(void));
