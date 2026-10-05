/* The cursor set (paint.h; the owner's "style C", docs/G1-PLAN.md "The
 * look", whose exact source is docs/design/cursors.svg): the arrow, the
 * four resize arrows, move, the text bar, the hand, and busy turning in
 * CURSOR_BUSY_FRAMES frames a second.
 *
 * The pictures are drawn on the Mac at build time (tools/cursorgen.c: the
 * SVG's paths supersampled, the outline, the shadow; drawing them here
 * would take seconds of QEMU's emulated CPU at every start) into
 * build/gen/cursorset.c's cursor_pictures, premultiplied, CURSOR_IMG
 * square, the shape's 24 units CURSOR_PAD pixels in from the edges. This
 * file only says where each one's hot spot is (the SVG's comment: the
 * arrow (5, 2.5), the hand (10.8, 2), the others the centre, rounded down
 * to the pixel they fall in) and which busy frame shows when. */
#include "paint.h"

/* tools/cursorgen.c's table: the shapes but busy, then busy's frames. */
extern const uint32_t cursor_pictures[CURSOR_SHAPES - 1 + CURSOR_BUSY_FRAMES]
                                     [CURSOR_IMG * CURSOR_IMG];

static struct cursor_image set[CURSOR_SHAPES - 1 + CURSOR_BUSY_FRAMES];

void cursors_init(void)
{
    for (unsigned k = 0; k < CURSOR_SHAPES - 1 + CURSOR_BUSY_FRAMES; k++) {
        int32_t hx = k == CURSOR_ARROW ? 5 : k == CURSOR_HAND ? 10 : 12;
        int32_t hy = k == CURSOR_ARROW ? 2 : k == CURSOR_HAND ? 2 : 12;
        set[k] = (struct cursor_image){ CURSOR_IMG, CURSOR_IMG, hx + CURSOR_PAD, hy + CURSOR_PAD,
                                        cursor_pictures[k] };
    }
}

const struct cursor_image *cursors_get(enum cursor_shape s, uint64_t t)
{
    if (s == CURSOR_BUSY)   /* a turn a second */
        return &set[CURSOR_BUSY + t / (NS_PER_S / CURSOR_BUSY_FRAMES) % CURSOR_BUSY_FRAMES];
    return &set[s < CURSOR_BUSY ? s : CURSOR_ARROW];
}
