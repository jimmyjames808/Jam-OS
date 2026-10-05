/* The desktop in a paint (desk.h, paint.c): its layers over the windows,
 * in this order from the bottom:
 *   low:  an opening or closing window's picture (anim.c), the strip, a
 *         minimising or restoring window's picture (it goes into the strip,
 *         so over it);
 *   high: the notifications, a popover, the search box or Alt+Tab's list.
 * Then paint.c draws the cursor over all of it. The cards' backdrops are
 * what is under them without the high layer (frost.c), so a card never
 * blurs another card.
 *
 * The strip is opaque, so a tile wholly inside it skips the windows and
 * the wallpaper (desk_hides); and while anything of the desktop is over
 * the windows, a full-screen window's damage is not copied straight to the
 * output (desk_over_windows). */
#include "desk.h"

void desk_paint_prepare(void)
{
    strip_update();
    if (desk_on())
        frost_prepare();
}

void desk_paint_backdrops(void)
{
    if (desk_on())
        frost_build();
}

static bool inside(struct comp_box a, struct comp_box b)
{
    return a.x1 >= b.x1 && a.y1 >= b.y1 && a.x2 <= b.x2 && a.y2 <= b.y2;
}

bool desk_hides(struct comp_box b)
{
    return strip.shown && inside(b, strip_box());
}

bool desk_over_windows(void)
{
    struct anim_draw d;
    if (anim_now(&d))
        return true;
    return desk_on() && (strip.shown || search.open || alttab.shown || pop.kind != POP_NONE ||
                         notes.n);
}

void desk_draw_low(const struct tile_buf *t)
{
    anim_draw(t, false);
    strip_draw(t);
    anim_draw(t, true);
}

void desk_draw_high(const struct tile_buf *t)
{
    if (!desk_on())
        return;
    notify_draw(t);
    pop_draw(t);
    search_draw(t);
    alttab_draw(t);
}
