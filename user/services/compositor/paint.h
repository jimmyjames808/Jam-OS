/* paint.h: what the painting files of the compositor share among
 * themselves (output.c, paint.c, title.c, cursor.c, clock.c, testscene.c);
 * what other files need is in comp.h.
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

/* ---- title.c ----------------------------------------------------------------------- */

/* w's decorations (title bar, close box, borders) where they meet t. */
void title_draw(const struct comp_window *w, const struct tile_buf *t);
/* The colours, for tests: the title bar focused and not, the borders. */
#define TITLE_BAR_FOCUSED   0x4a3d66u
#define TITLE_BAR           0x2e2833u
#define TITLE_TEXT_FOCUSED  0xf2eaf6u
#define TITLE_TEXT          0x9d93a3u
#define BORDER_FOCUSED      0x7a66a8u
#define BORDER              0x3d3542u

/* ---- cursor.c ---------------------------------------------------------------------- */

/* The arrow at the output's scale (call after output_open). */
void cursor_init(void);
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
