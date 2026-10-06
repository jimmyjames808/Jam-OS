/* utest: the compositor's desktop (user/services/compositor: screens.c,
 * anim.c, strip.c, desk.c with the window manager), linked in and driven
 * directly on compwm.c's harness (compwm.h: the seat played by the test,
 * fake toplevels). The desktop is on, its animations off unless a test
 * turns them on, its clock fixed at Monday 5 October 2026 14:32; the
 * output 1280x800. No pixels here: comp_look's and comp-test.sh's pictures
 * have those.
 *
 * t_desk_screens: one screen at the start; Super+Right past the last makes
 * one, an empty screen left behind goes; Super+1..9 and the dots go to one
 * that exists; each screen's own arrangement (Super+T acts on the current
 * one, and new screens take it); a window moved with Super+Shift+Right
 * makes a screen and goes there, focused, and its old screen goes when
 * empty; windows of other screens are unmapped.
 * t_desk_fullscreen: full screen moves the window to a screen of its own
 * right of its home (no strip there, all of the output) and back again
 * (its screen gone); a full-screen window closing takes the user back home;
 * a new window opened while a full-screen screen is current goes to the
 * normal screen left of it.
 * t_desk_minimise: the circle hides the window (its chip dimmed, the focus
 * moved on, its tile given to the others), its screen stays while it is
 * there; its chip, or Alt+Tab, brings it back focused, from another screen
 * too; a chip click focuses, minimises the focused one, restores.
 * t_desk_room: windows never go under the strip: floating (first places,
 * the cascade, a drag up, a client's maximise), tiling (every tile, a
 * maximise), a screen with no strip (full screen) has all of it; and with
 * the desktop off, all of the output is the windows'.
 * t_desk_strip: the islands and items: "Jam OS", a dot per screen (the
 * current one the pill, a full-screen one a square), "+", the chips (the
 * focused one lit, minimised ones marked), the icons and the clock's text;
 * a click on each does what it says.
 * t_desk_anim: with animations on: opening, minimising, restoring and
 * closing each draw a snapshot and end exactly where the change ends (the
 * window mapped or not, drawn as itself, the snapshot freed); a slide moves
 * both screens' windows and ends with every offset 0 and the old screen's
 * windows unmapped; a key or click mid-way jumps to the end; each frame
 * damages only the boxes it touches.
 * t_desk_cursors: the cursor set's pictures at known pixels (all fill well
 * inside, nothing far outside, the outline along an edge), their hot
 * spots, busy's arc turning a turn a second; the resize arrows on each edge
 * and corner of a resizable floating window, the hand on its circles, the
 * strip's islands and the search box's rows, the arrow elsewhere; busy from
 * a launch until that app's first window, or DESK_BUSY_NS. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <keymap.h>
#include "compwm.h"
#include "utest.h"

#define U_T     0x17
#define U_1     0x1e
#define U_2     0x1f
#define U_RIGHT 0x4f
#define U_LEFT  0x50
#define SUPER   INPUT_MOD_LGUI
#define SHIFT   INPUT_MOD_LSHIFT
#define FLOOR   (LOOK_STRIP_H + LOOK_STRIP_GAP)   /* the highest a frame reaches */

void desk_test_start(enum comp_layout layout)
{
    wm_test_start(layout);
    struct civil c = { .year = 2026, .month = 10, .day = 5, .hour = 14, .minute = 32,
                       .wday = 1 };
    desk_time_fix(&c);
    desk_init(true, false);
    strip_update();
}

static bool key(uint16_t usage, uint8_t mods)
{
    return desk_key(usage, mods, keymap_mods_of_hid(mods));
}

/* Screen i's windows: those of fks[] on it, as a bit mask. */
static unsigned on_screen(unsigned i)
{
    unsigned m = 0;
    for (unsigned k = 0; k < FK_MAX; k++)
        if (fks[k].ww && fks[k].ww->win && fks[k].ww->screen == screens_nth(i))
            m |= 1u << k;
    return m;
}

static bool mapped(unsigned k)
{
    return fks[k].ww && fks[k].ww->win && (fks[k].ww->win->flags & COMP_WIN_MAPPED);
}

/* ---- screens ------------------------------------------------------------------------------ */

static bool screens_steps(void)
{
    CHECK_EQ(screens_count(), 1);
    CHECK(fk_open(&fks[0], 320, 200, false));
    /* past the last: a new, empty screen, current; the first keeps its window */
    CHECK(key(U_RIGHT, SUPER));
    CHECK_EQ(screens_count(), 2);
    CHECK_EQ(screens_cur_index(), 1);
    CHECK(!mapped(0));
    CHECK_EQ(seat.focused, NULL);
    /* again: the empty one left behind goes, so there are still two */
    CHECK(key(U_RIGHT, SUPER));
    CHECK_EQ(screens_count(), 2);
    CHECK_EQ(screens_cur_index(), 1);
    CHECK(fk_open(&fks[1], 300, 200, false));   /* opens here */
    CHECK_EQ(on_screen(1), 2u);
    /* Super+1, Super+2: there; Super+9: no such screen, nothing */
    CHECK(key(U_1, SUPER));
    CHECK_EQ(screens_cur_index(), 0);
    CHECK(mapped(0) && !mapped(1));
    CHECK_EQ(seat.focused, win(0));
    CHECK(key(U_1 + 8, SUPER));
    CHECK_EQ(screens_cur_index(), 0);
    CHECK(!key(U_RIGHT, 0));   /* without Super: a client's */
    /* Super+Left from the first: nowhere */
    CHECK(key(U_LEFT, SUPER));
    CHECK_EQ(screens_cur_index(), 0);
    return true;
}

static bool layout_steps(void)
{
    /* each screen its own arrangement: Super+T on the second only */
    screens_go(1);
    wm_toggle_layout();
    CHECK_EQ(screens_nth(1)->layout, COMP_TILING);
    CHECK_EQ(screens_nth(0)->layout, COMP_FLOATING);
    CHECK_EQ(win(1)->deco_top, DECO_BORDER);   /* tiled: a border, no title bar */
    CHECK_EQ(win(0)->deco_top, COMP_TITLE_H);
    CHECK_EQ(seat.nlayout, 1);
    /* a new screen takes the last choice */
    screens_add();
    CHECK_EQ(screens_count(), 3);
    CHECK_EQ(screens_cur()->layout, COMP_TILING);
    screens_go(1);
    CHECK_EQ(screens_count(), 2);              /* the empty third went */
    /* the window moved right (Super+Shift+Right): a new screen past the
     * last, current, the window focused there; the screen it left, empty, goes */
    seat_focus(win(1));
    CHECK(key(U_RIGHT, SUPER | SHIFT));
    CHECK_EQ(screens_count(), 2);
    CHECK_EQ(screens_cur_index(), 1);
    CHECK_EQ(on_screen(1), 2u);
    CHECK_EQ(seat.focused, win(1));
    CHECK(mapped(1) && !mapped(0));
    /* and left again: onto the first screen with window 0; its own goes */
    CHECK(key(U_LEFT, SUPER | SHIFT));
    CHECK_EQ(screens_count(), 1);
    CHECK_EQ(on_screen(0), 3u);
    CHECK_EQ(seat.focused, win(1));
    CHECK(mapped(0) && mapped(1));
    return true;
}

bool t_desk_screens(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = screens_steps() && layout_steps();
    fk_close_all();
    return ok;
}

/* ---- full screen -------------------------------------------------------------------------- */

static bool fullscreen_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK(fk_open(&fks[1], 300, 200, false));
    CHECK(strip.shown);
    /* full screen: a screen of its own right of its home, current, no strip */
    wm_toggle_fullscreen(win(1));
    CHECK_EQ(screens_count(), 2);
    CHECK_EQ(screens_cur_index(), 1);
    CHECK_EQ(screens_cur()->kind, SCREEN_FULL);
    CHECK(fks[1].ww->screen == screens_nth(1) && fks[1].ww->home == screens_nth(0));
    CHECK(fk_draw(&fks[1]));
    CHECK_EQ(fks[1].s.width, OUT_W);
    CHECK_EQ(fks[1].s.height, OUT_H);
    CHECK(win(1)->x == 0 && win(1)->y == 0);
    CHECK(mapped(1) && !mapped(0));
    strip_update();
    CHECK(!strip.shown && box_empty(strip_box()));
    CHECK_EQ(seat.focused, win(1));
    /* and back: home, the full-screen screen gone, the strip back */
    wm_toggle_fullscreen(win(1));
    CHECK_EQ(screens_count(), 1);
    CHECK_EQ(screens_cur_index(), 0);
    CHECK(fk_draw(&fks[1]));
    CHECK(mapped(0) && mapped(1));
    strip_update();
    CHECK(strip.shown);
    /* closed while full screen: home again, its screen gone */
    wm_toggle_fullscreen(win(1));
    CHECK(fk_draw(&fks[1]));
    wm_destroy(fks[1].ww);
    fks[1].ww = NULL;
    CHECK_EQ(screens_count(), 1);
    CHECK_EQ(screens_cur()->kind, SCREEN_NORMAL);
    CHECK(mapped(0));
    CHECK_EQ(seat.focused, win(0));
    /* a window opened while a full-screen screen is current: on the normal
     * screen left of it, which becomes current */
    wm_toggle_fullscreen(win(0));
    CHECK_EQ(screens_cur()->kind, SCREEN_FULL);
    CHECK(fk_open(&fks[2], 200, 100, false));
    CHECK_EQ(screens_cur()->kind, SCREEN_NORMAL);
    CHECK(fks[2].ww->screen == screens_cur() && mapped(2) && !mapped(0));
    return true;
}

bool t_desk_fullscreen(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = fullscreen_steps();
    fk_close_all();
    return ok;
}

/* ---- minimising --------------------------------------------------------------------------- */

static const struct strip_item *chip_of(unsigned k)
{
    strip_update();
    for (unsigned i = 0; i < strip.n; i++)
        if (strip.items[i].part == STRIP_CHIP && strip.items[i].ww == fks[k].ww)
            return &strip.items[i];
    return NULL;
}

static bool minimise_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK(fk_open(&fks[1], 300, 200, false));
    seat_focus(win(1));
    const struct strip_item *c = chip_of(1);
    CHECK(c && c->on && !c->minimised);
    /* its circle: hidden, its chip dimmed, the focus on to the other */
    wm_minimise(win(1));
    CHECK(fks[1].ww->minimised && !mapped(1));
    CHECK_EQ(seat.focused, win(0));
    c = chip_of(1);
    CHECK(c && !c->on && c->minimised);
    CHECK(chip_of(0)->on);
    /* its screen stays while it is there: switching away leaves both screens */
    screens_step(1);
    wm_destroy(fks[0].ww);   /* the other window goes */
    fks[0].ww = NULL;
    screens_step(1);         /* away again: screen 2 (empty) goes, screen 1 stays for it */
    CHECK_EQ(screens_count(), 2);
    CHECK_EQ(on_screen(0), 2u);
    /* Alt+Tab's way back: to its screen, restored, focused */
    screens_activate(fks[1].ww);
    CHECK_EQ(screens_cur_index(), 0);
    CHECK(!fks[1].ww->minimised && mapped(1));
    CHECK_EQ(seat.focused, win(1));
    /* a chip click: on the focused window minimises it, again restores it */
    strip_click(chip_of(1));
    CHECK(fks[1].ww->minimised && !mapped(1));
    strip_click(chip_of(1));
    CHECK(!fks[1].ww->minimised && mapped(1) && seat.focused == win(1));
    return true;
}

static bool minimise_tiling_steps(void)
{
    wm_toggle_layout();
    CHECK(fk_open(&fks[2], 300, 200, false));
    CHECK(fk_draw(&fks[1]) && fk_draw(&fks[2]));
    struct comp_box half = window_frame(win(2));
    CHECK(half.x2 - half.x1 < OUT_W / 2);
    /* minimised: the other takes the whole room */
    wm_minimise(win(1));
    CHECK(fk_draw(&fks[2]));
    struct comp_box all = window_frame(win(2));
    CHECK_EQ(all.x1, 6);
    CHECK_EQ(all.x2, OUT_W - 6);
    CHECK_EQ(all.y1, FLOOR);
    /* a chip click on another, unfocused window: the focus to it */
    screens_restore(fks[1].ww);
    CHECK(fk_draw(&fks[1]) && fk_draw(&fks[2]));
    seat_focus(win(1));
    strip_click(chip_of(2));
    CHECK_EQ(seat.focused, win(2));
    return true;
}

bool t_desk_minimise(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = minimise_steps() && minimise_tiling_steps();
    fk_close_all();
    return ok;
}

/* ---- the room under the strip ------------------------------------------------------------- */

static bool below_strip(unsigned k)
{
    struct comp_box f = window_frame(win(k));
    CHECK(f.y1 >= FLOOR);
    return true;
}

static bool room_floating_steps(void)
{
    /* first places: centred in the room, cascaded, all below the floor */
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK_EQ(win(0)->y, FLOOR + (OUT_H - FLOOR - 200) / 2);
    CHECK(fk_open(&fks[1], 320, 200, false));
    CHECK(below_strip(0) && below_strip(1));
    /* a big one: its title bar at the floor */
    CHECK(fk_open(&fks[2], 1000, 760, false));
    CHECK_EQ(window_frame(win(2)).y1, FLOOR);
    /* dragged up as far as it goes: the floor */
    seat_focus(win(0));
    struct comp_box f = window_frame(win(0));
    CHECK(drag(f.x1 + 100, f.y1 + 10, 0, -1000));
    CHECK_EQ(window_frame(win(0)).y1, FLOOR);
    /* maximised: the room below the floor, under its title bar */
    wm_request_maximized(fks[0].ww, true);
    CHECK_EQ(fks[0].cfg.height, OUT_H - FLOOR - COMP_TITLE_H);
    CHECK(fk_draw(&fks[0]));
    CHECK_EQ(window_frame(win(0)).y1, FLOOR);
    wm_request_maximized(fks[0].ww, false);
    CHECK(fk_draw(&fks[0]));
    return true;
}

static bool room_tiling_steps(void)
{
    wm_toggle_layout();
    for (unsigned k = 0; k < 3; k++) {
        CHECK(fk_draw(&fks[k]));
        CHECK(below_strip(k));
    }
    CHECK_EQ(window_frame(win(0)).y1, FLOOR);   /* the master: the room less the gap */
    wm_request_maximized(fks[1].ww, true);
    CHECK(fk_draw(&fks[1]));
    CHECK_EQ(window_frame(win(1)).y1, FLOOR);   /* tiling's maximise: below the floor too */
    wm_request_maximized(fks[1].ww, false);
    /* full screen: a screen with no strip, all of the output */
    wm_toggle_fullscreen(win(2));
    CHECK(fk_draw(&fks[2]));
    CHECK(win(2)->x == 0 && win(2)->y == 0 && fks[2].s.height == OUT_H);
    return true;
}

bool t_desk_room(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = room_floating_steps() && room_tiling_steps();
    fk_close_all();
    /* the desktop off: the whole output */
    wm_test_start(COMP_FLOATING);
    ok = ok && fk_open(&fks[0], 1000, 760, false) && window_frame(win(0)).y1 == 0;
    fk_close_all();
    return ok;
}

/* ---- the strip ---------------------------------------------------------------------------- */

static unsigned count(enum strip_part p)
{
    unsigned n = 0;
    for (unsigned i = 0; i < strip.n; i++)
        n += strip.items[i].part == p;
    return n;
}

static bool strip_layout_steps(void)
{
    strip_update();
    CHECK(strip.shown);
    CHECK_EQ(count(STRIP_JAM), 1);
    CHECK_EQ(count(STRIP_DOT), 1);
    CHECK_EQ(count(STRIP_NOCHIP), 1);   /* "No windows" */
    CHECK(!strcmp(strip_find(STRIP_CLOCK)->text, "Mon 5 Oct  14:32"));
    /* the islands: left at the edge, right at the other, centre centred */
    CHECK_EQ(strip.islands[0].x1, LOOK_ISLAND_EDGE);
    CHECK_EQ(strip.islands[2].x2, OUT_W - LOOK_ISLAND_EDGE);
    int32_t off = strip.islands[1].x1 + strip.islands[1].x2 - OUT_W;
    CHECK(off >= -1 && off <= 1);
    for (unsigned i = 0; i < 3; i++)
        CHECK(strip.islands[i].y1 == LOOK_ISLAND_TOP && strip.islands[i].y2 == 33);
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK(fk_open(&fks[1], 320, 200, false));
    wm_set_title(fks[0].ww, "Terminal");
    wm_set_title(fks[1].ww, "Mines");
    seat_focus(win(1));
    strip_update();
    CHECK_EQ(count(STRIP_CHIP), 2);
    CHECK_EQ(count(STRIP_NOCHIP), 0);
    CHECK(!chip_of(0)->on && chip_of(1)->on);
    CHECK(!strcmp(chip_of(0)->text, "Terminal"));
    CHECK(chip_of(0)->box.x2 < chip_of(1)->box.x1);   /* in the order they opened */
    /* every item inside its island, under the strip */
    for (unsigned i = 0; i < strip.n; i++) {
        struct comp_box b = strip.items[i].box;
        bool in = false;
        for (unsigned k = 0; k < 3; k++)
            in |= b.x1 >= strip.islands[k].x1 && b.x2 <= strip.islands[k].x2;
        CHECK(in && b.y1 >= LOOK_ISLAND_TOP && b.y2 <= LOOK_ISLAND_TOP + LOOK_ISLAND_H);
    }
    return true;
}

static bool strip_click_steps(void)
{
    /* "+": a new screen, current: two dots, the pill on the second */
    strip_click(strip_find(STRIP_PLUS));
    strip_update();
    CHECK_EQ(screens_count(), 2);
    CHECK_EQ(count(STRIP_DOT), 2);
    CHECK(strip.items[2].part == STRIP_DOT && strip.items[2].on);
    CHECK_EQ(strip.items[2].box.x2 - strip.items[2].box.x1, LOOK_DOT_CUR_W);
    /* the first dot: back */
    strip_click(strip_find(STRIP_DOT));
    CHECK_EQ(screens_cur_index(), 0);
    /* the layout icon: tiling here */
    strip_click(strip_find(STRIP_MODE));
    CHECK_EQ(screens_cur()->layout, COMP_TILING);
    /* "Jam OS": the search box, lit; again: closed */
    strip_click(strip_find(STRIP_JAM));
    CHECK(search.open && strip_find(STRIP_JAM)->on);
    strip_click(strip_find(STRIP_JAM));
    CHECK(!search.open);
    /* a full-screen screen's dot is a square: marked full */
    wm_toggle_fullscreen(win(1));
    screens_go(0);
    strip_update();
    CHECK(strip.items[2].part == STRIP_DOT && strip.items[2].full && !strip.items[2].on);
    return true;
}

bool t_desk_strip(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = strip_layout_steps() && strip_click_steps();
    fk_close_all();
    return ok;
}

/* ---- animations ---------------------------------------------------------------------------- */

/* Every damage box inside b (the boxes an animation's frame touched). */
static bool damage_inside(struct comp_box b)
{
    for (uint32_t i = 0; i < scene.damage.n; i++) {
        struct comp_box d = scene.damage.b[i];
        CHECK(d.x1 >= b.x1 && d.y1 >= b.y1 && d.x2 <= b.x2 && d.y2 <= b.y2);
    }
    return true;
}

static bool anim_picture_steps(void)
{
    /* opening: drawn by its snapshot, from 92%, until the end */
    CHECK(fk_open(&fks[0], 320, 200, false));
    CHECK_EQ(anim_running(), ANIM_OPEN);
    CHECK(win(0)->flags & COMP_WIN_ANIMATED);
    struct anim_draw d;
    CHECK(anim_now(&d) && d.alpha == 0 && !d.above_strip);
    struct comp_box f = window_frame(win(0));
    CHECK_EQ(d.at.x2 - d.at.x1, (f.x2 - f.x1) * LOOK_ANIM_OPEN_FROM / 1000);
    damage_clear(&scene.damage);
    anim_tick(now() + 75 * NS_PER_MS);   /* half way: only its boxes damaged */
    CHECK(anim_now(&d) && d.alpha > 0 && d.alpha < 255);
    CHECK(damage_inside(f));
    anim_tick(now() + 200 * NS_PER_MS);  /* past the end: the window itself */
    CHECK_EQ(anim_running(), ANIM_NONE);
    CHECK(!(win(0)->flags & COMP_WIN_ANIMATED) && mapped(0));
    CHECK_EQ(fdesk.snaps, 1);
    CHECK_EQ(fdesk.snaps_freed, 1);
    /* minimising: into its chip, above the strip; the end: unmapped */
    struct comp_box chip = strip_chip_box(fks[0].ww);
    wm_minimise(win(0));
    CHECK_EQ(anim_running(), ANIM_MINIMISE);
    CHECK(anim_now(&d) && d.above_strip && !mapped(0));
    anim_tick(now() + 300 * NS_PER_MS);
    CHECK_EQ(anim_running(), ANIM_NONE);
    CHECK(!mapped(0) && fks[0].ww->minimised);
    /* restoring, interrupted by a key: at its end at once */
    screens_restore(fks[0].ww);
    CHECK_EQ(anim_running(), ANIM_RESTORE);
    CHECK(anim_now(&d));
    CHECK(d.at.x1 >= chip.x1 - 20 && d.at.x2 <= chip.x2 + 20);   /* about the chip */
    CHECK(mapped(0) && (win(0)->flags & COMP_WIN_ANIMATED));
    (void)desk_key(0x04, 0, 0);   /* 'a': a client's key, but it interrupts */
    CHECK_EQ(anim_running(), ANIM_NONE);
    CHECK(mapped(0) && !(win(0)->flags & COMP_WIN_ANIMATED));
    /* closing: its picture fades after it went (a buffer to take it from) */
    static struct comp_buffer shown;
    fks[0].s.buffer = &shown;
    wm_destroy(fks[0].ww);
    fks[0].s.buffer = NULL;
    fks[0].ww = NULL;
    CHECK_EQ(anim_running(), ANIM_CLOSE);
    anim_finish();
    CHECK_EQ(fdesk.snaps, fdesk.snaps_freed);
    return true;
}

static bool anim_slide_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    anim_finish();
    screens_step(1);
    CHECK(fk_open(&fks[1], 320, 200, false));
    anim_finish();
    screens_go(0);
    CHECK_EQ(anim_running(), ANIM_SLIDE);
    CHECK(mapped(0) && mapped(1));   /* both shown while it slides */
    anim_tick(now() + 130 * NS_PER_MS);
    CHECK(win(0)->slide_x < 0 && win(0)->slide_x > -OUT_W);   /* coming in from the left... */
    CHECK(win(1)->slide_x > 0);                               /* ... the other going right */
    CHECK(press_btn(5, 790, BTN) == false);   /* a click ends it */
    release_at(5, 790);
    CHECK_EQ(anim_running(), ANIM_NONE);
    CHECK(win(0)->slide_x == 0 && win(1)->slide_x == 0);
    CHECK(mapped(0) && !mapped(1));
    CHECK_EQ(screens_count(), 2);   /* screen 2 has a window: it stays */
    return true;
}

/* ---- cursors ------------------------------------------------------------------------------- */

#define CFILL 0xfff6f3f8u   /* the set's fill, opaque, premultiplied */

static uint32_t px_of(enum cursor_shape s, int32_t i, int32_t j)
{
    const struct cursor_image *c = cursors_get(s, 0);
    return c->px[j * c->w + i];
}

/* The pictures, at known pixels: all fill well inside a shape, nothing far
 * outside it, the outline's dark ink along an edge; the hot spots; busy's
 * arc turning. */
static bool cursor_picture_steps(void)
{
    cursors_init();
    const struct cursor_image *a = cursors_get(CURSOR_ARROW, 0);
    CHECK(a->w == CURSOR_IMG && a->h == CURSOR_IMG);
    CHECK(a->hot_x == 5 + CURSOR_PAD && a->hot_y == 2 + CURSOR_PAD);
    CHECK_EQ(px_of(CURSOR_ARROW, 8, 12), CFILL);          /* the SVG's (6, 10) */
    CHECK_EQ(px_of(CURSOR_ARROW, 0, 0), 0);               /* nothing, no shadow */
    CHECK_EQ(px_of(CURSOR_ARROW, 26, 2), 0);
    uint32_t edge = px_of(CURSOR_ARROW, 6, 14);           /* across the left edge (x 5) */
    CHECK((edge >> 24) > 0x80 && (edge >> 16 & 0xff) < 0x50);   /* mostly the dark outline */
    CHECK(cursors_get(CURSOR_HAND, 0)->hot_x == 10 + CURSOR_PAD);
    for (int s = CURSOR_RESIZE_EW; s < CURSOR_SHAPES; s++) {
        const struct cursor_image *c = cursors_get((enum cursor_shape)s, 0);
        if (s != CURSOR_HAND)
            CHECK(c->hot_x == 12 + CURSOR_PAD && c->hot_y == 12 + CURSOR_PAD);
    }
    /* the resize arrows, move and the text bar: all fill at the hot spot's
     * pixel (the SVG's centre is its top-left corner: the shaft going up
     * to the right passes the pixel left of it) */
    for (int s = CURSOR_RESIZE_EW; s <= CURSOR_TEXT; s++)
        CHECK_EQ(px_of((enum cursor_shape)s, s == CURSOR_RESIZE_NESW ? 13 : 14, 14), CFILL);
    CHECK_EQ(px_of(CURSOR_HAND, 16, 16), CFILL);          /* the palm */
    /* busy: the ring, no fill at its middle, the arc somewhere and moving */
    CHECK_EQ(px_of(CURSOR_BUSY, 14, 14), 0);
    const struct cursor_image *b0 = cursors_get(CURSOR_BUSY, 0);
    const struct cursor_image *b1 = cursors_get(CURSOR_BUSY, NS_PER_S / 2);
    CHECK(b0 != b1 && b0 == cursors_get(CURSOR_BUSY, NS_PER_S));   /* a turn a second */
    unsigned raspberry = 0, moved = 0;
    for (int i = 0; i < CURSOR_IMG * CURSOR_IMG; i++) {
        raspberry += b0->px[i] == 0xffd4537eu;
        moved += b0->px[i] != b1->px[i];
    }
    CHECK(raspberry > 5 && moved > 10);
    return true;
}

/* Which cursor the window manager and the desktop want where. */
static bool cursor_choice_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));
    struct comp_box s = window_surface_box(win(0)), f = window_frame(win(0));
    CHECK_EQ(wm_cursor_at(s.x2 + 2, (s.y1 + s.y2) / 2), CURSOR_RESIZE_EW);
    CHECK_EQ(wm_cursor_at(s.x1 - 3, (s.y1 + s.y2) / 2), CURSOR_RESIZE_EW);
    CHECK_EQ(wm_cursor_at((s.x1 + s.x2) / 2, s.y2 + 2), CURSOR_RESIZE_NS);
    CHECK_EQ(wm_cursor_at((s.x1 + s.x2) / 2, f.y1 - 2), CURSOR_RESIZE_NS);
    CHECK_EQ(wm_cursor_at(s.x2 + 2, s.y2 + 2), CURSOR_RESIZE_NWSE);
    CHECK_EQ(wm_cursor_at(f.x1 - 2, f.y1 - 2), CURSOR_RESIZE_NWSE);
    CHECK_EQ(wm_cursor_at(s.x2 + 2, f.y1 - 2), CURSOR_RESIZE_NESW);
    CHECK_EQ(wm_cursor_at(f.x1 - 2, s.y2 + 2), CURSOR_RESIZE_NESW);
    struct comp_box c = title_button_box(win(0), TITLE_MINIMISE);
    CHECK_EQ(wm_cursor_at(c.x1 + 5, c.y1 + 5), CURSOR_HAND);
    CHECK_EQ(wm_cursor_at((f.x1 + f.x2) / 2, f.y1 + 14), CURSOR_ARROW);   /* the title bar */
    CHECK_EQ(wm_cursor_at((s.x1 + s.x2) / 2, (s.y1 + s.y2) / 2), CURSOR_ARROW);
    CHECK_EQ(wm_cursor_at(5, 790), CURSOR_ARROW);
    /* the desktop: the hand on the islands and the search box's rows */
    strip_update();
    const struct strip_item *clock = strip_find(STRIP_CLOCK);
    CHECK_EQ(desk_cursor_at(clock->box.x1 + 3, clock->box.y1 + 3), CURSOR_HAND);
    CHECK_EQ(desk_cursor_at(OUT_W / 2 - 300, 20), CURSOR_ARROW);   /* between islands */
    CHECK_EQ(desk_cursor_at(600, 600), CURSOR_SHAPES);            /* not the desktop's */
    search_toggle();
    struct comp_box r = search_row_box(0);
    CHECK_EQ(desk_cursor_at(r.x1 + 5, r.y1 + 5), CURSOR_HAND);
    search_close();
    /* busy from launching until the app's first window, at most DESK_BUSY_NS */
    desk_launch("jamjar");
    CHECK(desk_busy());
    CHECK(fk_open(&fks[1], 200, 100, false));
    CHECK(desk_busy());   /* not jamjar */
    wm_set_title(fks[1].ww, "Jamjar");
    desk_window_mapped(fks[1].ww);
    CHECK(!desk_busy());
    desk_launch("terminal");
    desk_tick(now() + DESK_BUSY_NS + NS_PER_MS);
    CHECK(!desk_busy());
    return true;
}

bool t_desk_cursors(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = cursor_picture_steps() && cursor_choice_steps();
    fk_close_all();
    return ok;
}

bool t_desk_anim(void)
{
    desk_test_start(COMP_FLOATING);
    anim_init(true);
    bool ok = anim_picture_steps() && anim_slide_steps();
    anim_init(false);
    fk_close_all();
    return ok;
}
