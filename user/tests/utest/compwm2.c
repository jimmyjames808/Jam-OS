/* utest: tiling's dwindle tree and its gaps (user/services/compositor
 * wmtile.c, wmgrab.c, wmkeys.c), on compwm.c's harness (compwm.h). The
 * desktop is off (the room is the output, 1280x800), so every tile can be
 * worked out by hand: a gap of 6 round the room and between halves, a
 * split's halves a's ratio of its length less the gap (half: 1262 / 2 =
 * 631 across the room, 782 / 2 = 391 down it), a border of 2 inside each
 * tile.
 *
 * t_wm_tile_tree: a new window splits the focused tile in half along its
 * longer side (the old window left or above); one that goes leaves its
 * sibling their parent's room; a split's ratio set by its gap, clamped to
 * 15-85%, kept per screen; two windows swap tiles; a screen switched from
 * floating builds its tree in the windows' order, each splitting the last.
 * t_wm_tile_gaps: a gap takes presses 12 pixels across centred on it (not
 * under a maximised window), shows the resize arrows and its bar lit while
 * hovered or dragged; dragging it resizes both sides live, with no glide,
 * clamped; stacked gaps too.
 * t_wm_tile_float: windows that have only ever tiled (or been maximised),
 * switched to floating, are asked for 60% of the room under the strip
 * each way, as their limits allow and never more than the room, and
 * centred and cascaded for that size; they keep it across another switch.
 * t_wm_tile_push: Super+Alt+direction (H/J/K/L and the arrows alike)
 * pushes the focused tile's edge 48 pixels that way, or with no edge
 * there its other edge, gliding; a floating window grows or shrinks by its
 * right or bottom edge, within its limits. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include "compwm.h"
#include "utest.h"

#define U_H     0x0b
#define U_J     0x0d
#define U_K     0x0e
#define U_L     0x0f
#define U_RIGHT 0x4f
#define U_LEFT  0x50
#define U_DOWN  0x51
#define U_UP    0x52
#define U_RIGHT_ALT (INPUT_MOD_LGUI | INPUT_MOD_LALT)

static const struct comp_box ALL = { 6, 6, 1274, 794 };
static const struct comp_box LEFT = { 6, 6, 637, 794 };
static const struct comp_box RIGHT = { 643, 6, 1274, 794 };
static const struct comp_box RTOP = { 643, 6, 1274, 397 };
static const struct comp_box RBOT = { 643, 403, 1274, 794 };

/* a's length of a room len long split at ratio (wmtile.c's rounding). */
static int32_t part(int32_t len, int32_t ratio)
{
    return (int32_t)(((int64_t)(len - WM_GAP) * ratio + TILE_ONE / 2) / TILE_ONE);
}

static bool tile_is(unsigned i, struct comp_box b)
{
    CHECK(fks[i].ww->leaf);
    if (!box_eq(win(i)->tile, b))
        FAIL("window %u's tile is %d,%d..%d,%d, want %d,%d..%d,%d", i, win(i)->tile.x1,
             win(i)->tile.y1, win(i)->tile.x2, win(i)->tile.y2, b.x1, b.y1, b.x2, b.y2);
    return true;
}

/* ---- the tree ------------------------------------------------------------------------- */

static bool tree_insert_steps(void)
{
    CHECK(fk_open(&fks[0], 300, 200, false));
    CHECK(tile_is(0, ALL));
    seat_focus(win(0));
    CHECK(fk_open(&fks[1], 300, 200, false));   /* the room is wider: across */
    CHECK(tile_is(0, LEFT) && tile_is(1, RIGHT));
    seat_focus(win(1));
    CHECK(fk_open(&fks[2], 300, 200, false));   /* the right half is taller: down */
    CHECK(tile_is(1, RTOP) && tile_is(2, RBOT) && tile_is(0, LEFT));
    /* the focus decides which tile splits: the left half, top and bottom */
    seat_focus(win(0));
    CHECK(fk_open(&fks[3], 300, 200, false));
    CHECK(tile_is(0, ((struct comp_box){ 6, 6, 637, 397 })));
    CHECK(tile_is(3, ((struct comp_box){ 6, 403, 637, 794 })));
    /* with the focus on no tile here: the last tile (the newest corner) */
    seat_focus(NULL);
    CHECK(fk_open(&fks[4], 300, 200, false));
    int32_t half = part(631, TILE_ONE / 2);   /* the bottom right is wider: across, 313 */
    CHECK(tile_is(2, ((struct comp_box){ 643, 403, 643 + half, 794 })));
    CHECK(tile_is(4, ((struct comp_box){ 643 + half + WM_GAP, 403, 1274, 794 })));
    /* every configure follows its tile; the client draws it */
    for (unsigned i = 0; i < 5; i++)
        CHECK(fills_box(i, tile_inner(win(i)->tile)));
    return true;
}

static bool tree_ratio_steps(void)
{
    /* the root's gap moved to x 1000: the left half 991 wide */
    struct tile_node *root = screens_cur()->tree;
    CHECK(root && root->across);
    tiles_gap_move(root, 1000);
    wm_relayout();
    CHECK_EQ(win(0)->tile.x2, 997);
    CHECK_EQ(win(1)->tile.x1, 1003);
    /* clamped at 85% and 15% */
    tiles_gap_move(root, 5000);
    wm_relayout();
    CHECK_EQ(root->ratio, TILE_MAX);
    CHECK_EQ(win(0)->tile.x2, 6 + part(1268, TILE_MAX));
    tiles_gap_move(root, -100);
    wm_relayout();
    CHECK_EQ(root->ratio, TILE_MIN);
    CHECK_EQ(win(0)->tile.x2, 6 + part(1268, TILE_MIN));
    /* a window that goes: its sibling takes their parent's room (the left
     * half's split's), the other splits keep their ratios */
    int32_t left_x2 = win(0)->tile.x2;
    wm_destroy(fks[0].ww);
    fks[0].ww = NULL;
    CHECK(tile_is(3, ((struct comp_box){ 6, 6, left_x2, 794 })));
    CHECK_EQ(screens_cur()->tree->ratio, TILE_MIN);
    /* a swap: the two trade tiles, nothing else moves */
    struct comp_box t1 = win(1)->tile, t3 = win(3)->tile, t2 = win(2)->tile;
    CHECK(tiles_swap(fks[1].ww, fks[3].ww));
    wm_relayout();
    CHECK(tile_is(1, t3) && tile_is(3, t1) && tile_is(2, t2));
    CHECK(!tiles_swap(fks[1].ww, fks[1].ww));
    return true;
}

static bool tree_screens_steps(void)
{
    /* another screen keeps its own tree and ratios */
    screens_step(1);
    CHECK_EQ(screens_cur()->layout, COMP_TILING);
    CHECK(fk_open(&fks[0], 300, 200, false));
    seat_focus(win(0));
    CHECK(fk_open(&fks[5], 300, 200, false));
    CHECK(tile_is(0, LEFT) && tile_is(5, RIGHT));
    screens_go(0);
    CHECK_EQ(screens_cur()->tree->ratio, TILE_MIN);
    /* floating and back: made again in the windows' order (1, 2, 3, 4),
     * each splitting the last; the ratios start at half again */
    wm_toggle_layout();
    CHECK(!screens_cur()->tree && !fks[1].ww->leaf);
    wm_toggle_layout();
    CHECK(tile_is(1, LEFT) && tile_is(2, RTOP));
    struct comp_box b = RBOT;
    int32_t top = part(631, TILE_ONE / 2);   /* the bottom right split across: 313 */
    CHECK(tile_is(3, ((struct comp_box){ b.x1, b.y1, b.x1 + top, b.y2 })));
    CHECK(tile_is(4, ((struct comp_box){ b.x1 + top + WM_GAP, b.y1, b.x2, b.y2 })));
    return true;
}

bool t_wm_tile_tree(void)
{
    wm_test_start(COMP_TILING);
    bool ok = tree_insert_steps() && tree_ratio_steps() && tree_screens_steps();
    fk_close_all();
    return ok;
}

/* ---- gaps ----------------------------------------------------------------------------- */

/* Two tiles side by side (0 left, 1 right), drawn, 0 focused; animations on. */
static bool two_tiles(void)
{
    anim_init(true);
    CHECK(fk_open(&fks[0], 300, 200, false));
    seat_focus(win(0));
    CHECK(fk_open(&fks[1], 300, 200, false));
    anim_finish();
    CHECK(fk_draw(&fks[0]) && fk_draw(&fks[1]));
    CHECK(tile_is(0, LEFT) && tile_is(1, RIGHT));
    return true;
}

static bool gap_hover_steps(void)
{
    /* the gap is x 637..643, its middle 640: presses from 634 to 645 */
    CHECK(wm_gap_covers(640, 400) && wm_gap_covers(634, 400) && wm_gap_covers(645, 400));
    CHECK(!wm_gap_covers(633, 400) && !wm_gap_covers(646, 400) && !wm_gap_covers(640, 3));
    CHECK_EQ(wm_cursor_at(640, 400), CURSOR_RESIZE_EW);
    CHECK_EQ(wm_cursor_at(600, 400), CURSOR_ARROW);   /* the left surface */
    /* lit while the pointer is over it: a 3-pixel bar, 15% of its length
     * short at either end (788 * 0.15 = 118) */
    cursor.x = 640;
    cursor.y = 400;
    damage_clear(&scene.damage);
    wm_marks_update();
    CHECK(box_eq(wm_marks.bar, ((struct comp_box){ 639, 124, 642, 676 })));
    CHECK(scene.damage.n > 0);
    cursor.x = 600;
    wm_marks_update();
    CHECK(box_empty(wm_marks.bar));
    return true;
}

static bool gap_drag_steps(void)
{
    /* dragged 100 right: both sides resized at once (live), no glide */
    CHECK(press_btn(640, 400, BTN));
    CHECK(seat.ops);
    move_to(740, 400);
    CHECK(tile_is(0, ((struct comp_box){ 6, 6, 737, 794 })));
    CHECK(tile_is(1, ((struct comp_box){ 743, 6, 1274, 794 })));
    CFG(0, 727, 784, WM_ST_ACTIVATED);
    CFG(1, 527, 784, 0);
    CHECK(!anim_gliding() && win(1)->slide_x == 0);
    wm_marks_update();   /* the bar lit where the gap is now, while dragged */
    CHECK_EQ(wm_marks.bar.x1, 739);
    /* past 85%: held there */
    move_to(5000, 400);
    CHECK_EQ(win(0)->tile.x2, 6 + part(1268, TILE_MAX));
    release_at(5000, 400);
    CHECK(!seat.ops);
    wm_marks_update();
    CHECK(box_empty(wm_marks.bar));   /* the pointer is off it */
    CHECK(fk_draw(&fks[0]) && fk_draw(&fks[1]));
    /* a stacked gap: the up-down arrows */
    seat_focus(win(1));
    CHECK(fk_open(&fks[2], 300, 200, false));
    anim_finish();
    struct comp_box t2 = win(2)->tile;
    int32_t mid = t2.y1 - WM_GAP / 2;
    CHECK_EQ(wm_cursor_at((t2.x1 + t2.x2) / 2, mid), CURSOR_RESIZE_NS);
    CHECK(drag((t2.x1 + t2.x2) / 2, mid, 0, -100));
    CHECK_EQ(win(2)->tile.y1, t2.y1 - 100);
    /* a maximised window over the gaps: no gap under it */
    wm_request_maximized(fks[0].ww, true);
    CHECK(fk_draw(&fks[0]));
    CHECK(!wm_gap_covers(win(0)->tile.x2 + WM_GAP / 2, 400));
    return true;
}

bool t_wm_tile_gaps(void)
{
    wm_test_start(COMP_TILING);
    bool ok = two_tiles() && gap_hover_steps() && gap_drag_steps();
    anim_init(false);
    fk_close_all();
    return ok;
}

/* ---- Super+Alt+direction ---------------------------------------------------------------- */

/* Super+Alt+usage taken, and the left tile's right edge then at x2. */
static bool push(uint16_t usage, int32_t x2)
{
    CHECK(wm_test_key(usage, U_RIGHT_ALT));
    anim_finish();
    CHECK_EQ(win(0)->tile.x2, x2);
    return true;
}

static bool push_tiled_steps(void)
{
    /* the left tile: its right edge right by L and Right; by H (no left
     * edge) its other edge, the right, comes left; J and K: no edge either
     * way down or up: nothing */
    CHECK(wm_test_key(U_L, U_RIGHT_ALT));
    CHECK(anim_gliding());   /* the right tile glides to its new place */
    CHECK(win(1)->slide_x != 0);
    anim_finish();
    CHECK_EQ(win(0)->tile.x2, 637 + 48);
    CHECK(win(1)->slide_x == 0 && !anim_gliding());
    CHECK(push(U_RIGHT, 637 + 96) && push(U_H, 637 + 48) && push(U_LEFT, 637));
    CHECK(push(U_J, 637) && push(U_K, 637) && push(U_DOWN, 637) && push(U_UP, 637));
    /* the right tile: its left edge left by H; by L (no right edge) its
     * left edge comes right */
    seat_focus(win(1));
    CHECK(push(U_H, 637 - 48) && push(U_L, 637) && push(U_RIGHT, 637 + 48));
    /* no further than 85% */
    for (int i = 0; i < 20; i++)
        CHECK(wm_test_key(U_RIGHT, U_RIGHT_ALT));
    CHECK_EQ(win(0)->tile.x2, 6 + part(1268, TILE_MAX));
    return true;
}

static bool push_floating_steps(void)
{
    wm_test_start(COMP_FLOATING);
    CHECK(fk_open(&fks[0], 320, 200, false));
    seat_focus(win(0));
    /* its right edge out by L and in by H, its bottom by J and K; the top left stays */
    CHECK(wm_test_key(U_L, U_RIGHT_ALT));
    CFG(0, 368, 200, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    CHECK(wm_test_key(U_RIGHT, U_RIGHT_ALT));
    CHECK(wm_test_key(U_J, U_RIGHT_ALT));
    CFG(0, 416, 248, WM_ST_ACTIVATED);
    CHECK(wm_test_key(U_H, U_RIGHT_ALT));
    CHECK(wm_test_key(U_UP, U_RIGHT_ALT));
    CHECK(wm_test_key(U_K, U_RIGHT_ALT));
    CFG(0, 368, 152, WM_ST_ACTIVATED);
    CHECK(fk_draw(&fks[0]));
    AT(0, 480, 300);
    /* its limits hold */
    wm_set_limits(fks[0].ww, 340, 0, 0, 0);
    CHECK(wm_test_key(U_LEFT, U_RIGHT_ALT));
    CFG(0, 340, 152, WM_ST_ACTIVATED);
    /* a window that can't resize: taken, nothing changes */
    CHECK(fk_open(&fks[1], 200, 100, true));
    seat_focus(win(1));
    unsigned n = fks[1].nconf;
    CHECK(wm_test_key(U_L, U_RIGHT_ALT));
    CHECK_EQ(fks[1].nconf, n);
    return true;
}

/* ---- a tile's floating size ------------------------------------------------------------- */

/* Window i's frame wholly on the output, its top at floor or below. */
static bool on_output(unsigned i, int32_t floor)
{
    struct comp_box f = window_frame(win(i));
    if (f.x1 < 0 || f.y1 < floor || f.x2 > OUT_W || f.y2 > OUT_H)
        FAIL("window %u's frame is %d,%d..%d,%d", i, f.x1, f.y1, f.x2, f.y2);
    return true;
}

static bool tile_float_steps(void)
{
    /* five tiles: two of no limits, one of one size, one of max width 500
     * and min height 600, one of min width 2000 */
    for (unsigned i = 0; i < 5; i++) {
        CHECK(fk_open(&fks[i], 320, 200, i == 2));
        seat_focus(win(i));
    }
    wm_set_limits(fks[3].ww, 0, 600, 500, 0);
    wm_set_limits(fks[4].ww, 2000, 0, 0, 0);
    anim_finish();
    for (unsigned i = 0; i < 5; i++)
        CHECK(fk_draw(&fks[i]));
    /* floating: none has a floating size of its own, so each is asked for
     * 60% of the room (1278x771 inside a frame: 766x462), as its limits
     * allow and never more than the room, and centred for it (two of one
     * size cascade), before it draws */
    wm_toggle_layout();
    CHECK_EQ(fks[0].cfg.width, 766);
    CHECK_EQ(fks[0].cfg.height, 462);
    AT(0, (OUT_W - 766) / 2, (OUT_H - 462) / 2);
    CHECK_EQ(fks[1].cfg.width, 766);
    CHECK_EQ(fks[1].cfg.height, 462);
    AT(1, (OUT_W - 766) / 2 + COMP_TITLE_H + DECO_BORDER, (OUT_H - 462) / 2 + COMP_TITLE_H + DECO_BORDER);
    CHECK_EQ(fks[2].cfg.width, 320);      /* its one size */
    CHECK_EQ(fks[2].cfg.height, 200);
    CHECK_EQ(fks[3].cfg.width, 500);
    CHECK_EQ(fks[3].cfg.height, 600);
    AT(3, (OUT_W - 500) / 2, (OUT_H - 600) / 2);
    CHECK_EQ(fks[4].cfg.width, OUT_W - 2 * DECO_OUTLINE);   /* the room, not 2000 */
    CHECK_EQ(fks[4].cfg.height, 462);
    for (unsigned i = 0; i < 5; i++) {
        CHECK(fk_draw(&fks[i]));
        CHECK(on_output(i, 0));
    }
    AT(0, (OUT_W - 766) / 2, (OUT_H - 462) / 2);   /* where it was put for that size */
    /* tiling and back: each where and as big as it floated */
    struct comp_box was = window_surface_box(win(0));
    wm_toggle_layout();
    anim_finish();
    wm_toggle_layout();
    CFG(0, 766, 462, 0);
    CHECK(fk_draw(&fks[0]));
    CHECK(box_eq(window_surface_box(win(0)), was));
    return true;
}

/* A window maximised before its first buffer, on a floating screen, then
 * not: 60% of the room, centred (not the maximised size it drew). */
static bool maximised_float_steps(void)
{
    wm_test_start(COMP_FLOATING);
    CHECK(fk_open_max(&fks[0], 320, 200));
    CHECK(win(0)->flags & COMP_WIN_MAXIMIZED);
    CHECK_EQ(fks[0].s.height, OUT_H - COMP_TITLE_H);
    wm_request_maximized(fks[0].ww, false);
    CHECK_EQ(fks[0].cfg.width, 766);
    CHECK_EQ(fks[0].cfg.height, 462);
    CHECK(fk_draw(&fks[0]));
    AT(0, (OUT_W - 766) / 2, (OUT_H - 462) / 2);
    CHECK(on_output(0, 0));
    return true;
}

/* With the desktop on: 60% of the room under the strip, the frame below it. */
static bool strip_float_steps(void)
{
    desk_test_start(COMP_TILING);
    CHECK(fk_open(&fks[0], 320, 200, false));
    anim_finish();
    CHECK(fk_draw(&fks[0]));
    int32_t floor = wm_floor(fks[0].ww);
    CHECK(floor > 0);
    wm_toggle_layout();
    CHECK_EQ(fks[0].cfg.height, (OUT_H - floor - COMP_TITLE_H - DECO_OUTLINE) * 60 / 100);
    CHECK(fk_draw(&fks[0]));
    CHECK(on_output(0, floor));
    return true;
}

bool t_wm_tile_float(void)
{
    wm_test_start(COMP_TILING);
    bool ok = tile_float_steps();
    fk_close_all();
    ok = ok && maximised_float_steps();
    fk_close_all();
    ok = ok && strip_float_steps();
    fk_close_all();
    return ok;
}

bool t_wm_tile_push(void)
{
    wm_test_start(COMP_TILING);
    bool ok = two_tiles() && push_tiled_steps();
    anim_init(false);
    fk_close_all();
    ok = ok && push_floating_steps();
    fk_close_all();
    return ok;
}
