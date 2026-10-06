/* The top bar (desk.h): a frosted strip along the top of a normal screen
 * with three islands on it (docs/G1-PLAN.md "The look", the owner's "3 on
 * a strip"; sizes in look.h):
 *   left    "Jam OS" (the search box), a dot for each screen (the current
 *           one a wider apricot pill, a full-screen one a small square),
 *           and "+" (a new screen);
 *   centre  the current screen's windows as chips, in the order they
 *           opened: the focused one tinted raspberry, minimised ones dimmed
 *           with an apricot dot ("No windows" when there are none). A click
 *           focuses a window, minimises it if it is focused, brings it back
 *           if it is minimised;
 *   right   the floating/tiling icon (a click switches), the network and
 *           volume icons and the clock ("Mon 5 Oct  14:32"), which open
 *           their popovers.
 * Not shown on a full-screen screen, under a boot overlay (the splash,
 * screens.c: it covers all of the output), nor with the desktop off.
 *
 * The layout is made again (strip_update, before each paint, and before a
 * chip's box is asked for) only when something it shows changed
 * (strip_dirty: screens, the focus, a title, minimising, the clock's
 * minute, a popover or the search box opening): the whole strip is then
 * damaged, a full-width band 40 pixels high, which costs little. What is
 * drawn (stripdraw.c) and what a click hits are the same boxes. */
#include "desk.h"

struct strip_layout strip;
static bool dirty = true;

static const char *const day_abbr[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const month_abbr[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void strip_dirty(void)
{
    dirty = true;
}

struct comp_box strip_box(void)
{
    if (!strip.shown)
        return (struct comp_box){ 0, 0, 0, 0 };
    return (struct comp_box){ 0, 0, scene.width, LOOK_STRIP_H };
}

/* ---- laying out -------------------------------------------------------------------------- */

#define BTN_Y1 (LOOK_ISLAND_TOP + (LOOK_ISLAND_H - LOOK_BTN_H) / 2)
#define BTN_Y2 (BTN_Y1 + LOOK_BTN_H)

static struct strip_item *add(enum strip_part part, int32_t x1, int32_t x2)
{
    if (strip.n == STRIP_ITEMS_MAX)
        return NULL;
    struct strip_item *it = &strip.items[strip.n++];
    *it = (struct strip_item){ .part = part, .box = { x1, BTN_Y1, x2, BTN_Y2 } };
    return it;
}

static struct comp_box island(int32_t x1, int32_t x2)
{
    return (struct comp_box){ x1, LOOK_ISLAND_TOP, x2, LOOK_ISLAND_TOP + LOOK_ISLAND_H };
}

/* The left island: "Jam OS", the dots, "+". Its right end. */
static int32_t lay_left(void)
{
    int32_t x = LOOK_ISLAND_EDGE + LOOK_ISLAND_PAD;
    struct strip_item *it = add(STRIP_JAM, x, x + desk_text_w(desk_font.m12, "Jam OS") +
                                                   2 * LOOK_BTN_PAD);
    if (it) {
        it->on = search.open;
        x = it->box.x2 + LOOK_ISLAND_GAP + 2;
    }
    for (unsigned i = 0; i < screens_count(); i++) {
        bool cur = i == screens_cur_index();
        int32_t w = cur ? LOOK_DOT_CUR_W : LOOK_DOT_D;
        if (!(it = add(STRIP_DOT, x, x + w)))
            break;
        it->index = i;
        it->on = cur;
        it->full = screens_nth(i)->kind == SCREEN_FULL;
        x += w + LOOK_DOT_GAP;
    }
    x += 2 - LOOK_DOT_GAP + LOOK_ISLAND_GAP;
    if ((it = add(STRIP_PLUS, x, x + LOOK_PLUS + 6)))
        x = it->box.x2;
    strip.islands[0] = island(LOOK_ISLAND_EDGE, x + LOOK_ISLAND_PAD);
    return strip.islands[0].x2;
}

/* The right island, from the right: the clock, volume, network, layout.
 * Its left end. */
static int32_t lay_right(void)
{
    struct civil c;
    int32_t x = scene.width - LOOK_ISLAND_EDGE - LOOK_ISLAND_PAD;
    char text[32];
    (void)desk_time(&c);
    snprintf(text, sizeof(text), "%s %u %s  %02u:%02u", day_abbr[c.wday % 7], c.day,
             month_abbr[(c.month + 11) % 12], c.hour, c.minute);
    int32_t w = desk_text_w(desk_font.r12, text) + 2 * LOOK_BTN_PAD;
    struct strip_item *it = add(STRIP_CLOCK, x - w, x);
    if (it) {
        snprintf(it->text, sizeof(it->text), "%s", text);
        it->on = pop.kind == POP_CLOCK;
    }
    static const enum strip_part icons[3] = { STRIP_VOL, STRIP_NET, STRIP_MODE };
    x -= w + LOOK_ISLAND_GAP;
    for (unsigned i = 0; i < 3; i++) {
        if ((it = add(icons[i], x - LOOK_ICON_W, x)))
            it->on = (icons[i] == STRIP_VOL && pop.kind == POP_VOLUME) ||
                     (icons[i] == STRIP_NET && pop.kind == POP_NETWORK);
        x -= LOOK_ICON_W + (i < 2 ? LOOK_ISLAND_GAP : 0);
    }
    strip.islands[2] = island(x - LOOK_ISLAND_PAD, scene.width - LOOK_ISLAND_EDGE);
    return strip.islands[2].x1;
}

/* A chip's width for its title, as wide as it wants (at most LOOK_CHIP_MAX). */
static int32_t chip_want(const struct wm_window *ww)
{
    int32_t w = desk_text_w(desk_font.r12, ww->title) + 2 * LOOK_CHIP_PAD;
    if (ww->minimised)
        w += LOOK_CHIP_MIN_DOT + LOOK_DOT_GAP;
    return w < LOOK_CHIP_MAX ? w : LOOK_CHIP_MAX;
}

/* The centre island between left and right: the current screen's chips,
 * narrowed (to LOOK_CHIP_MIN) to fit, those past that hidden. */
static void lay_centre(int32_t left, int32_t right)
{
    const struct desk_screen *s = screens_cur();
    int32_t room = right - left - 2 * LOOK_ISLAND_GAP;
    if (room > scene.width - LOOK_ISLAND_MIDDLE)
        room = scene.width - LOOK_ISLAND_MIDDLE;
    room -= 2 * LOOK_ISLAND_PAD;
    unsigned n = 0;
    int32_t total = 0;
    for (const struct wm_window *ww = wm_first(); ww; ww = ww->next)
        if (ww->win && ww->screen == s) {
            n++;
            total += chip_want(ww) + LOOK_ISLAND_GAP;
        }
    int32_t cap = LOOK_CHIP_MAX;
    if (n && total - LOOK_ISLAND_GAP > room) {
        cap = (room - (int32_t)(n - 1) * LOOK_ISLAND_GAP) / (int32_t)n;
        cap = cap < LOOK_CHIP_MIN ? LOOK_CHIP_MIN : cap;
    }
    unsigned first = strip.n;
    int32_t x = 0;
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next) {
        if (!ww->win || ww->screen != s)
            continue;
        int32_t w = chip_want(ww) < cap ? chip_want(ww) : cap;
        if (x + w > room)
            break;   /* no room: the rest hidden */
        struct strip_item *it = add(STRIP_CHIP, x, x + w);
        if (!it)
            break;
        it->ww = ww;
        it->minimised = ww->minimised;
        it->on = !ww->minimised && ww == wm_focused();
        snprintf(it->text, sizeof(it->text), "%s", ww->title);
        x += w + LOOK_ISLAND_GAP;
    }
    if (strip.n == first) {
        int32_t w = desk_text_w(desk_font.r12, "No windows") + 2 * LOOK_BTN_PAD;
        if (add(STRIP_NOCHIP, 0, w))
            x = w + LOOK_ISLAND_GAP;
    }
    int32_t width = x - LOOK_ISLAND_GAP + 2 * LOOK_ISLAND_PAD;
    int32_t x1 = (scene.width - width) / 2;
    for (unsigned i = first; i < strip.n; i++)
        strip.items[i].box = box_translate(strip.items[i].box, x1 + LOOK_ISLAND_PAD, 0);
    strip.islands[1] = island(x1, x1 + width);
}

static void lay_out(void)
{
    strip.n = 0;
    memset(strip.islands, 0, sizeof(strip.islands));
    strip.shown = desk_on() && screens_cur()->kind == SCREEN_NORMAL && !screens_overlay() &&
                  scene.height > LOOK_STRIP_H && scene.width > 2 * LOOK_ISLAND_EDGE;
    if (!strip.shown)
        return;
    int32_t left = lay_left();
    int32_t right = lay_right();
    lay_centre(left, right);
}

void strip_update(void)
{
    if (!dirty)
        return;
    dirty = false;
    struct comp_box was = strip_box();
    lay_out();
    scene_damage_over(was);   /* the strip casts no shadow: its own box */
    scene_damage_over(strip_box());
}

/* ---- what is where ------------------------------------------------------------------------ */

const struct strip_item *strip_at(int32_t x, int32_t y)
{
    strip_update();
    for (unsigned i = 0; i < strip.n; i++)
        if (box_contains(strip.items[i].box, x, y) && strip.items[i].part != STRIP_NOCHIP)
            return &strip.items[i];
    return NULL;
}

const struct strip_item *strip_find(enum strip_part part)
{
    strip_update();
    for (unsigned i = 0; i < strip.n; i++)
        if (strip.items[i].part == part)
            return &strip.items[i];
    return NULL;
}

struct comp_box strip_chip_box(const struct wm_window *ww)
{
    strip_update();
    for (unsigned i = 0; i < strip.n; i++)
        if (strip.items[i].part == STRIP_CHIP && strip.items[i].ww == ww)
            return strip.items[i].box;
    return (struct comp_box){ 0, 0, 0, 0 };
}

/* ---- clicks ------------------------------------------------------------------------------- */

static void chip_click(struct wm_window *ww)
{
    if (ww->minimised)
        screens_restore(ww);
    else if (ww == wm_focused())
        screens_minimise(ww);
    else
        seat_focus(ww->win);
}

void strip_click(const struct strip_item *at)
{
    struct strip_item it = *at;   /* the layout may change under what it does */
    if (it.part != STRIP_JAM)
        search_close();
    if (it.part != STRIP_VOL && it.part != STRIP_NET && it.part != STRIP_CLOCK)
        pop_close();
    switch (it.part) {
    case STRIP_JAM:
        search_toggle();
        break;
    case STRIP_DOT:
        screens_go(it.index);
        break;
    case STRIP_PLUS:
        screens_add();
        break;
    case STRIP_CHIP:
        chip_click(it.ww);
        break;
    case STRIP_MODE:
        wm_toggle_layout();
        break;
    case STRIP_VOL:
        pop_toggle(POP_VOLUME, it.box);
        break;
    case STRIP_NET:
        pop_toggle(POP_NETWORK, it.box);
        break;
    case STRIP_CLOCK:
        pop_toggle(POP_CLOCK, it.box);
        break;
    default:
        break;
    }
    strip_dirty();
}
