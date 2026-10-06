/* The tiler's marks over the windows (desk.h's wm_marks, kept by wmgrab.c),
 * the prototype's look (look.h): a gap the pointer is over or drags shows
 * its bar in apricot; a tile picked up with Super follows the pointer as
 * its picture, see-through, with a shadow; the tile it would swap with is
 * tinted apricot inside an apricot ring. Drawn over the windows and under
 * the desktop's strip and cards (paint.c's paint_under), by the painting
 * workers, which only read the marks. */
#include <fun.h>
#include "desk.h"

void wm_marks_draw(const struct tile_buf *t)
{
    const struct wm_marks *m = &wm_marks;
    if (!box_empty(m->target)) {
        ui_round(t, m->target, LOOK_TILE_RADIUS, LOOK_DROP, LOOK_DROP_TINT_A);
        ui_ring(t, m->target, LOOK_TILE_RADIUS, LOOK_DROP_RING, LOOK_DROP, 255);
    }
    if (!box_empty(m->bar))
        ui_round(t, m->bar, LOOK_GAP_BAR_R, LOOK_GAP_LIT, LOOK_GAP_LIT_A);
    if (m->lift.px && !box_empty(m->ghost)) {
        shadow_box(t, m->ghost, LOOK_TILE_RADIUS, 255);
        anim_draw_snap(t, &m->lift, m->ghost, LOOK_LIFT_A);
    }
}
