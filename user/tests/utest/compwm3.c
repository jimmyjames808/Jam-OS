/* utest: the window keys and Super+drag (user/services/compositor wmkeys.c,
 * wmgrab.c, anim.c's glide), on compwm.c's harness (compwm.h), the
 * desktop off unless a test says (1280x800; three tiles: 0 the left half,
 * 1 the right half's top, 2 its bottom).
 *
 * t_wm_focus_dir: Super+direction, H/J/K/L and the arrows alike, focuses
 * the nearest window that way (none that way: it stays), tiled or
 * floating; with nothing focused, the screen's last focused window.
 * t_wm_swap: Super+Shift+direction swaps the focused tile with its
 * neighbour (floating: nothing); Super+drag lifts a tile (drawn as its
 * picture, following the pointer), marks the tile under the pointer, and
 * swaps the two on release (nothing over the background or itself);
 * floating, Super+drag moves a window from anywhere in it and
 * Super+right-drag resizes it from the nearest corner, within its limits
 * and below the strip.
 * t_wm_reflow: with animations on, tiles glide to their new places on a
 * window opening and going, a swap and a key's resize: part way they are
 * between, each frame damaging only the boxes the gliding windows cover;
 * they end exactly at their places; another key jumps to the end.
 * t_wm_keys: the owner's whole table: Q, M, F, T, Enter and keypad Enter,
 * 1..9 and Shift+1..9, Ctrl with Left/Right/H/L and Shift; Super+Left and
 * Super+Right never switch screens; keys without Super, and Super with
 * keys not in the table, reach the client. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include "compwm.h"
#include "utest.h"

#define U_F        0x09
#define U_H        0x0b
#define U_J        0x0d
#define U_K        0x0e
#define U_L        0x0f
#define U_M        0x10
#define U_Q        0x14
#define U_T        0x17
#define U_1        0x1e
#define U_2        0x1f
#define U_9        0x26
#define U_ENTER    0x28
#define U_RIGHT    0x4f
#define U_LEFT     0x50
#define U_DOWN     0x51
#define U_UP       0x52
#define U_KP_ENTER 0x58
#define SUPER      INPUT_MOD_LGUI
#define SHIFT      INPUT_MOD_LSHIFT
#define CTRL       INPUT_MOD_LCTRL
#define ALT        INPUT_MOD_LALT
#define BTN_RIGHT  (BTN + 1)

static const struct comp_box LEFT = { 6, 6, 637, 794 };
static const struct comp_box RTOP = { 643, 6, 1274, 397 };
static const struct comp_box RBOT = { 643, 403, 1274, 794 };

/* Three tiles, drawn: 0 left, 1 right top, 2 right bottom; 0 focused. */
static bool three_tiles(bool fixed)
{
    CHECK(fk_open(&fks[0], 200, 100, fixed));
    seat_focus(win(0));
    CHECK(fk_open(&fks[1], 200, 100, fixed));
    seat_focus(win(1));
    CHECK(fk_open(&fks[2], 200, 100, fixed));
    anim_finish();
    for (unsigned i = 0; i < 3; i++)
        CHECK(fk_draw(&fks[i]));
    CHECK(box_eq(win(0)->tile, LEFT) && box_eq(win(1)->tile, RTOP) && box_eq(win(2)->tile, RBOT));
    seat_focus(win(0));
    return true;
}

/* Super+usage (with more held) taken, the focus then on window i. */
static bool focus_after(uint16_t usage, uint8_t mods, unsigned i)
{
    CHECK(wm_test_key(usage, SUPER | mods));
    CHECK_EQ(seat.focused, win(i));
    return true;
}

/* ---- focus by direction ----------------------------------------------------------------- */

static bool focus_tiled_steps(void)
{
    CHECK(three_tiles(false));
    /* from the left: right is either of the two (the same score: the first) */
    CHECK(focus_after(U_L, 0, 1) && focus_after(U_J, 0, 2) && focus_after(U_H, 0, 0));
    CHECK(focus_after(U_RIGHT, 0, 1) && focus_after(U_DOWN, 0, 2) && focus_after(U_UP, 0, 1));
    CHECK(focus_after(U_K, 0, 1));          /* nothing above it: it stays */
    CHECK(focus_after(U_LEFT, 0, 0));
    CHECK(focus_after(U_H, 0, 0));          /* nothing left of it */
    CHECK(focus_after(U_K, 0, 1));          /* up: the right top's middle is higher */
    /* nothing focused: the screen's last focused window */
    seat_focus(NULL);
    CHECK(focus_after(U_L, 0, 1));
    return true;
}

static bool focus_floating_steps(void)
{
    /* three floating windows placed by hand: 0 top left, 1 top right, 2 below 0 */
    static const int32_t at[3][2] = { { 100, 100 }, { 800, 120 }, { 120, 500 } };
    for (unsigned i = 0; i < 3; i++) {
        CHECK(fk_open(&fks[i], 200, 100, false));
        fks[i].ww->float_x = at[i][0];
        fks[i].ww->float_y = at[i][1];
        wm_place(fks[i].ww);
    }
    seat_focus(win(0));
    CHECK(focus_after(U_L, 0, 1) && focus_after(U_LEFT, 0, 0) && focus_after(U_J, 0, 2));
    CHECK(focus_after(U_UP, 0, 0) && focus_after(U_RIGHT, 0, 1) && focus_after(U_DOWN, 0, 2));
    CHECK(focus_after(U_K, 0, 0) && focus_after(U_K, 0, 0));
    return true;
}

bool t_wm_focus_dir(void)
{
    wm_test_start(COMP_TILING);
    bool ok = focus_tiled_steps();
    fk_close_all();
    wm_test_start(COMP_FLOATING);
    ok = ok && focus_floating_steps();
    fk_close_all();
    return ok;
}

/* ---- swapping ---------------------------------------------------------------------------- */

static bool swap_keys_steps(void)
{
    CHECK(three_tiles(false));
    /* Super+Shift+L: the focused left tile and the right top trade places,
     * the focus staying with the window */
    CHECK(wm_test_key(U_L, SUPER | SHIFT));
    CHECK(box_eq(win(0)->tile, RTOP) && box_eq(win(1)->tile, LEFT));
    CHECK_EQ(seat.focused, win(0));
    CHECK(wm_test_key(U_DOWN, SUPER | SHIFT));
    CHECK(box_eq(win(0)->tile, RBOT) && box_eq(win(2)->tile, RTOP));
    CHECK(wm_test_key(U_H, SUPER | SHIFT));
    CHECK(box_eq(win(0)->tile, LEFT) && box_eq(win(1)->tile, RBOT));
    CHECK(wm_test_key(U_LEFT, SUPER | SHIFT));   /* nothing left of it */
    CHECK(box_eq(win(0)->tile, LEFT));
    return true;
}

/* Super+drag window 0 (from (x, y)) by (dx, dy): the picture follows, the
 * tile under the pointer is marked (target: its index, or -1). */
static bool lift_to(int32_t x, int32_t y, int32_t dx, int32_t dy, int target)
{
    struct comp_box f = window_frame(win(0));
    unsigned snaps = fdesk.snaps;
    CHECK(press_super(x, y, BTN));
    CHECK(seat.ops);
    CHECK_EQ(seat.focused, win(0));
    CHECK_EQ(fdesk.snaps, snaps + 1);
    CHECK(win(0)->flags & COMP_WIN_ANIMATED);   /* drawn as its picture */
    CHECK(box_eq(wm_marks.ghost, f));
    move_to(x + dx, y + dy);
    CHECK(box_eq(wm_marks.ghost, box_translate(f, dx, dy)));
    if (target < 0)
        CHECK(box_empty(wm_marks.target));
    else
        CHECK(box_eq(wm_marks.target, window_frame(win((unsigned)target))));
    release_at(x + dx, y + dy);
    CHECK(!seat.ops && !(win(0)->flags & COMP_WIN_ANIMATED));
    CHECK(box_empty(wm_marks.ghost) && box_empty(wm_marks.target) && !wm_marks.lift.px);
    CHECK_EQ(fdesk.snaps, fdesk.snaps_freed);
    return true;
}

static bool swap_drag_steps(void)
{
    /* window 0 dropped on 2: they swap; on the background or on itself: nothing */
    CHECK(lift_to(300, 400, 600, 200, 2));
    CHECK(box_eq(win(0)->tile, RBOT) && box_eq(win(2)->tile, LEFT));
    CHECK(lift_to(900, 600, 0, 30, -1));
    CHECK(box_eq(win(0)->tile, RBOT));
    CHECK(lift_to(900, 600, 380, 200, -1));   /* off every window */
    CHECK(box_eq(win(0)->tile, RBOT));
    /* the window under the pointer goes mid-drag: nothing to swap with */
    CHECK(press_super(900, 600, BTN));
    move_to(300, 400);
    CHECK(!box_empty(wm_marks.target));
    wm_destroy(fks[2].ww);
    fks[2].ww = NULL;
    CHECK(box_empty(wm_marks.target));
    release_at(300, 400);
    CHECK(!(win(0)->flags & COMP_WIN_ANIMATED));
    /* without Super the press is the client's */
    CHECK(!press_btn(900, 600, BTN));
    release_at(900, 600);
    return true;
}

static bool floating_super_steps(void)
{
    desk_test_start(COMP_FLOATING);   /* the strip: a resize from the top stops below it */
    CHECK(fk_open(&fks[0], 320, 200, false));
    struct comp_box s = window_surface_box(win(0));
    /* Super+drag from the middle of its surface: moved */
    CHECK(press_super(s.x1 + 160, s.y1 + 100, BTN));
    move_to(s.x1 + 60, s.y1 + 150);
    release_at(s.x1 + 60, s.y1 + 150);
    AT(0, s.x1 - 100, s.y1 + 50);
    /* Super+right-drag near its bottom right: that corner follows */
    s = window_surface_box(win(0));
    CHECK(press_super(s.x2 - 10, s.y2 - 10, BTN_RIGHT));
    move_to(s.x2 + 40, s.y2 + 20);
    CFG(0, 370, 230, WM_ST_ACTIVATED | WM_ST_RESIZING);
    release_at(s.x2 + 40, s.y2 + 20);
    CHECK(fk_draw(&fks[0]));
    /* near its top left, dragged far up and left: the top stops below the
     * strip (its title bar's top on the floor), the bottom right stays */
    s = window_surface_box(win(0));
    CHECK(press_super(s.x1 + 5, s.y1 + 5, BTN_RIGHT));
    release_at(s.x1 - 2000, s.y1 - 2000);
    CHECK_EQ(fks[0].cfg.height, s.y2 - COMP_TITLE_H - (LOOK_STRIP_H + LOOK_STRIP_GAP));
    CHECK(fk_draw(&fks[0]));
    CHECK_EQ(window_frame(win(0)).y1, LOOK_STRIP_H + LOOK_STRIP_GAP);
    CHECK_EQ(window_surface_box(win(0)).y2, s.y2);
    /* its limits: inwards no smaller than its minimum */
    wm_set_limits(fks[0].ww, 250, 150, 0, 0);
    s = window_surface_box(win(0));
    CHECK(press_super(s.x2 - 3, s.y2 - 3, BTN_RIGHT));
    release_at(s.x2 - 3000, s.y2 - 3000);
    CFG(0, 250, 150, WM_ST_ACTIVATED);
    return true;
}

bool t_wm_swap(void)
{
    wm_test_start(COMP_TILING);
    bool ok = swap_keys_steps();
    fk_close_all();
    wm_test_start(COMP_TILING);
    ok = ok && three_tiles(false) && swap_drag_steps();
    fk_close_all();
    ok = ok && floating_super_steps();
    fk_close_all();
    return ok;
}

/* ---- the glide ---------------------------------------------------------------------------- */

/* Every damage box inside one of the n boxes. */
static bool damage_within(const struct comp_box *b, unsigned n)
{
    CHECK(scene.damage.n > 0);
    for (uint32_t i = 0; i < scene.damage.n; i++) {
        struct comp_box d = scene.damage.b[i];
        bool in = false;
        for (unsigned k = 0; k < n && !in; k++)
            in = d.x1 >= b[k].x1 && d.y1 >= b[k].y1 && d.x2 <= b[k].x2 && d.y2 <= b[k].y2;
        if (!in)
            FAIL("damage %d,%d..%d,%d outside the glide", d.x1, d.y1, d.x2, d.y2);
    }
    return true;
}

/* Every window's frame now (empty: none), before a change. */
static void frames(struct comp_box *f)
{
    for (unsigned i = 0; i < FK_MAX; i++)
        f[i] = fks[i].ww && fks[i].ww->win ? window_frame(win(i)) : (struct comp_box){ 0 };
}

/* Is window i there, with a frame before the change? */
static bool was(const struct comp_box *from, unsigned i)
{
    return !box_empty(from[i]) && fks[i].ww && fks[i].ww->win;
}

/* After a change: the windows glide from their frames before it (from) to
 * their new places, starting where they were; half way some are between,
 * every damage box inside one window's way (or `also`: a picture
 * animation's window); at the end each is exactly in its place. */
static bool glides(const struct comp_box *from, struct comp_box also)
{
    struct comp_box to[FK_MAX], span[FK_MAX + 1];
    unsigned n = 0;
    CHECK(anim_gliding());
    for (unsigned i = 0; i < FK_MAX; i++) {
        if (!was(from, i))
            continue;
        to[i] = box_translate(window_frame(win(i)), -win(i)->slide_x, -win(i)->slide_y);
        CHECK(box_eq(window_frame(win(i)), from[i]));
        span[n++] = box_bounds(from[i], to[i]);
    }
    span[n++] = also;
    damage_clear(&scene.damage);
    anim_tick(now() + LOOK_ANIM_GLIDE_MS * NS_PER_MS / 2);
    bool between = false;
    for (unsigned i = 0; i < FK_MAX; i++)
        between |= was(from, i) && !box_eq(window_frame(win(i)), from[i]) &&
                   !box_eq(window_frame(win(i)), to[i]);
    CHECK(between);
    CHECK(damage_within(span, n));
    anim_tick(now() + 2 * LOOK_ANIM_GLIDE_MS * NS_PER_MS);
    CHECK(!anim_gliding());
    for (unsigned i = 0; i < FK_MAX; i++)
        if (was(from, i))
            CHECK(box_eq(window_frame(win(i)), to[i]) && !win(i)->slide_x && !win(i)->slide_y);
    return true;
}

static bool reflow_steps(void)
{
    struct comp_box f[FK_MAX];
    anim_init(true);
    /* fixed-size windows, centred in their tiles, so a tile's change moves them */
    CHECK(fk_open(&fks[0], 200, 100, true));
    seat_focus(win(0));
    frames(f);
    CHECK(fk_open(&fks[1], 200, 100, true));   /* opening: the first glides to its half */
    CHECK(glides(f, window_extent(win(1))));
    /* a swap (by keys) */
    frames(f);
    CHECK(wm_test_key(U_L, SUPER | SHIFT));
    CHECK(glides(f, (struct comp_box){ 0 }));
    /* a key's resize: the focused one (now on the right) pushes its left edge */
    frames(f);
    CHECK(wm_test_key(U_H, SUPER | ALT));
    CHECK(glides(f, (struct comp_box){ 0 }));
    /* a window going: its sibling glides into the room (its closing picture beside) */
    seat_focus(win(1));
    CHECK(fk_open(&fks[2], 200, 100, true));
    anim_finish();
    frames(f);
    struct comp_box f2 = f[2];
    f[2] = (struct comp_box){ 0 };
    wm_destroy(fks[2].ww);
    fks[2].ww = NULL;
    CHECK(glides(f, f2));
    /* interrupted: another key jumps it to its end */
    CHECK(wm_test_key(U_L, SUPER | SHIFT));
    CHECK(anim_gliding() && (win(0)->slide_x || win(1)->slide_x));
    CHECK(wm_test_key(U_H, SUPER));   /* the focus moves; the glide ends first */
    CHECK(!anim_gliding() && !win(0)->slide_x && !win(1)->slide_x);
    return true;
}

bool t_wm_reflow(void)
{
    wm_test_start(COMP_TILING);
    bool ok = reflow_steps();
    anim_init(false);
    fk_close_all();
    return ok;
}

/* ---- the table ---------------------------------------------------------------------------- */

static bool window_keys_steps(void)
{
    CHECK(three_tiles(false));
    /* Super+Q asks the focused window to close; Super+M minimises it */
    CHECK(wm_test_key(U_Q, SUPER));
    CHECK_EQ(fks[0].closes, 1);
    CHECK(wm_test_key(U_M, SUPER));
    CHECK(fks[0].ww->minimised && !fks[0].ww->leaf);
    screens_restore(fks[0].ww);
    /* Super+F: full screen and back */
    CHECK(wm_test_key(U_F, SUPER));
    CHECK(fks[0].cfg.states & WM_ST_FULLSCREEN);
    CHECK(wm_test_key(U_F, SUPER));
    CHECK(!(fks[0].cfg.states & WM_ST_FULLSCREEN));
    /* Super+T: floating and back */
    CHECK(wm_test_key(U_T, SUPER));
    CHECK_EQ(screens_cur()->layout, COMP_FLOATING);
    CHECK(wm_test_key(U_T, SUPER));
    CHECK_EQ(screens_cur()->layout, COMP_TILING);
    /* Super+Enter and keypad Enter: a terminal each */
    CHECK(wm_test_key(U_ENTER, SUPER) && wm_test_key(U_KP_ENTER, SUPER));
    CHECK_EQ(fdesk.nterminal, 2);
    return true;
}

static bool screen_keys_steps(void)
{
    /* Super+Left and Super+Right: the focus, never a screen */
    CHECK(wm_test_key(U_RIGHT, SUPER) && wm_test_key(U_LEFT, SUPER));
    CHECK_EQ(screens_count(), 1);
    /* Super+Ctrl+Right, then L: a new screen each past the last (the empty one goes) */
    CHECK(wm_test_key(U_RIGHT, SUPER | CTRL));
    CHECK_EQ(screens_cur_index(), 1);
    CHECK(wm_test_key(U_LEFT, SUPER | CTRL));
    CHECK(wm_test_key(U_L, SUPER | CTRL));
    CHECK_EQ(screens_cur_index(), 1);
    CHECK(wm_test_key(U_H, SUPER | CTRL));
    CHECK_EQ(screens_cur_index(), 0);
    /* Super+Ctrl+Shift+Right: the focused window to a new screen, with it */
    seat_focus(win(1));
    CHECK(wm_test_key(U_RIGHT, SUPER | CTRL | SHIFT));
    CHECK_EQ(screens_count(), 2);
    CHECK(fks[1].ww->screen == screens_cur() && screens_cur_index() == 1);
    CHECK(wm_test_key(U_H, SUPER | CTRL | SHIFT));
    CHECK(fks[1].ww->screen == screens_nth(0));
    /* Super+Shift+2, Super+2, Super+1, Super+9 (none) */
    CHECK(wm_test_key(U_2, SUPER | SHIFT));
    CHECK(fks[1].ww->screen == screens_nth(1) && screens_cur_index() == 1);
    CHECK(wm_test_key(U_1, SUPER));
    CHECK_EQ(screens_cur_index(), 0);
    CHECK(wm_test_key(U_2, SUPER));
    CHECK_EQ(screens_cur_index(), 1);
    CHECK(wm_test_key(U_9, SUPER));
    CHECK_EQ(screens_cur_index(), 1);
    return true;
}

static bool not_ours_steps(void)
{
    /* without Super, and Super with what isn't in the table: the client's */
    static const struct { uint16_t usage; uint8_t mods; } keys[] = {
        { U_L, 0 }, { U_L, ALT }, { U_L, CTRL }, { U_RIGHT, CTRL | ALT }, { U_Q, CTRL },
        { U_J, SUPER | CTRL }, { U_UP, SUPER | CTRL }, { U_L, SUPER | CTRL | ALT },
        { 0x04, SUPER }, { U_Q, SUPER | ALT }, { U_1, SUPER | CTRL },
    };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (wm_test_key(keys[i].usage, keys[i].mods))
            FAIL("key %u taken (usage %#x, mods %#x)", i, keys[i].usage, keys[i].mods);
    return true;
}

bool t_wm_keys(void)
{
    wm_test_start(COMP_TILING);
    bool ok = window_keys_steps() && screen_keys_steps() && not_ours_steps();
    fk_close_all();
    return ok;
}
