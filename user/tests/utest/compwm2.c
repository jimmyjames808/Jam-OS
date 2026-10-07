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
 * t_wm_tile_small: a focused tile too small to halve (the halves would be
 * under WM_TILE_MIN_W wide, or WM_TILE_MIN_H high) isn't split: the new
 * window goes to a new screen at the end, all its room (its first
 * configure too), the view following; the first screen unchanged; closed,
 * the view back to the first and the emptied screen gone. Super+T
 * on a floating screen with more windows than fit: the rest go to a new
 * screen, the view staying; a window moved onto a full screen goes to a
 * new one, followed. The windows' own minimums: the new window's, the old
 * one's, a fixed size, each against its half. A narrow tall tile still
 * splits down (t_wm_tile_gaps' stacked gap).
 * t_wm_tile_gaps: a gap takes presses 12 pixels across centred on it (not
 * under a maximised window), shows the resize arrows and its bar lit while
 * hovered (not before a mouse first moves the pointer, which starts on the
 * first two tiles' gap) or dragged; dragging it resizes both sides live,
 * with no glide, clamped; stacked gaps too.
 * t_wm_tile_float: tiling to floating keeps the arrangement (owner,
 * 2026-10-07): each tiled window's floating box comes from its tile (its
 * frame 4 pixels inside it), so the boxes keep their places and never
 * overlap, whatever floating places they had before; a min bigger than
 * the tile is kept, from the tile's top left, on the output; again after
 * tiling and back. A window maximised before its first buffer, then not,
 * still gets 60% of the room, centred.
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
    /* the root's gap moved to x 800: the left half 791 wide */
    struct tile_node *root = screens_cur()->tree;
    CHECK(root && root->across);
    tiles_gap_move(root, 800);
    wm_relayout();
    CHECK_EQ(win(0)->tile.x2, 797);
    CHECK_EQ(win(1)->tile.x1, 803);
    /* to x 1000: stopped where the right half's two tiles side by side
     * keep the tiler's 200 each (406 with the gap) */
    tiles_gap_move(root, 1000);
    wm_relayout();
    CHECK_EQ(win(1)->tile.x1, 1274 - 2 * WM_TILE_MIN_W - WM_GAP);
    /* stopped where either side would be under the tiler's minimum (200
     * wide a tile: tighter than 85% and 15% of this room) */
    tiles_gap_move(root, 5000);
    wm_relayout();
    CHECK_EQ(win(0)->tile.x2, 1274 - 2 * WM_TILE_MIN_W - 2 * WM_GAP);
    tiles_gap_move(root, -100);
    wm_relayout();
    CHECK_EQ(win(0)->tile.x2, 6 + WM_TILE_MIN_W);
    /* with a smaller minimum: clamped at 85% and 15% */
    tiles_set_min(1, 1);
    wm_relayout();
    tiles_gap_move(root, 5000);
    wm_relayout();
    CHECK_EQ(root->ratio, TILE_MAX);
    CHECK_EQ(win(0)->tile.x2, 6 + part(1268, TILE_MAX));
    tiles_gap_move(root, -100);
    wm_relayout();
    CHECK_EQ(root->ratio, TILE_MIN);
    CHECK_EQ(win(0)->tile.x2, 6 + part(1268, TILE_MIN));
    tiles_set_min(WM_TILE_MIN_W, WM_TILE_MIN_H);
    wm_relayout();
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

/* ---- small tiles ------------------------------------------------------------------------ */

static bool tile_small_steps(void)
{
    /* each new window splits the focused (newest) tile: a spiral */
    for (unsigned i = 0; i < 5; i++) {
        if (i)
            seat_focus(win(i - 1));
        CHECK(fk_open(&fks[i], 300, 200, false));
    }
    CHECK(tile_is(0, LEFT) && tile_is(1, RTOP));
    int32_t across = part(631, TILE_ONE / 2);   /* 313: the bottom right split across */
    struct comp_box t2 = { 643, 403, 643 + across, 794 };
    int32_t down = part(391, TILE_ONE / 2);     /* 193: then its right part down */
    struct comp_box t3 = { 643 + across + WM_GAP, 403, 1274, 403 + down };
    struct comp_box t4 = { t3.x1, t3.y2 + WM_GAP, 1274, 794 };
    CHECK(tile_is(2, t2) && tile_is(3, t3) && tile_is(4, t4));
    /* the newest, 312x192, halved across would leave 153 wide: under the
     * minimum, so the sixth goes to a new screen at the end, all its room
     * (its first configure says so), and the view goes there; the first
     * screen's tiles stay as they were */
    CHECK((t4.x2 - t4.x1 - WM_GAP) / 2 < WM_TILE_MIN_W);
    seat_focus(win(4));
    struct desk_screen *first = screens_cur();
    CHECK(fk_open(&fks[5], 300, 200, false));
    CHECK_EQ(screens_count(), 2);
    CHECK(fks[5].ww->screen == screens_nth(1) && screens_cur() == screens_nth(1));
    CHECK_EQ(screens_nth(1)->layout, COMP_TILING);
    CHECK(tile_is(5, ALL));
    CHECK(fills_box(5, tile_inner(ALL)));
    CHECK(tile_is(0, LEFT) && tile_is(1, RTOP) && tile_is(2, t2) && tile_is(3, t3) &&
          tile_is(4, t4));
    for (unsigned i = 0; i < 5; i++)
        CHECK(fks[i].ww->screen == first && !(win(i)->flags & COMP_WIN_MAPPED));
    CHECK(win(5)->flags & COMP_WIN_MAPPED);
    /* its window closed: the view goes back to the first screen, and the
     * emptied spill goes; opened again, it spills again */
    wm_destroy(fks[5].ww);
    fks[5].ww = NULL;
    CHECK(screens_cur() == first && screens_count() == 1);
    CHECK(win(0)->flags & COMP_WIN_MAPPED);
    seat_focus(win(4));
    CHECK(fk_open(&fks[5], 300, 200, false));
    CHECK(screens_count() == 2 && screens_cur() == fks[5].ww->screen);
    return true;
}

/* Super+T building a tree from a floating screen's windows: those that
 * don't fit go to one new screen, in order, the view staying; a window
 * moved onto a full screen goes to a new one instead, the view following. */
static bool tile_small_switch_steps(void)
{
    struct desk_screen *first = screens_nth(0);
    const struct comp_box t4 = { 962, 602, 1274, 794 };
    screens_go(0);
    wm_toggle_layout();   /* the first screen floats: no tree */
    CHECK(!first->tree);
    screens_move_to(fks[5].ww, 0);   /* the sixth joins it (floating: no tile) */
    CHECK(fks[5].ww->screen == first && screens_cur() == first);
    wm_toggle_layout();   /* tiling again: made in the windows' order, each splitting the last */
    CHECK(screens_cur() == first);
    CHECK(tile_is(0, LEFT) && tile_is(4, t4));
    struct desk_screen *spill = fks[5].ww->screen;
    CHECK(spill != first && screens_index(spill) == (int)screens_count() - 1);
    CHECK(tile_is(5, ALL) && !(win(5)->flags & COMP_WIN_MAPPED));
    CHECK(win(0)->flags & COMP_WIN_MAPPED);
    /* moved onto the full first screen (its last tile too small): to a new
     * screen at the end instead, which the view follows */
    screens_go((unsigned)screens_index(spill));
    CHECK(screens_cur() == spill);
    screens_move_to(fks[5].ww, 0);
    CHECK(fks[5].ww->screen != first && screens_cur() == fks[5].ww->screen);
    CHECK(tile_is(5, ALL) && (win(5)->flags & COMP_WIN_MAPPED));
    CHECK(tile_is(4, t4));
    return true;
}

/* The windows' own minimums: a new window whose minimum doesn't fit the
 * half it would get goes to a new screen, one whose minimum fits splits;
 * the old window's minimum counts too; a fixed size bigger than its half
 * goes to a new screen, a smaller one splits. */
static bool tile_small_min_steps(void)
{
    struct desk_screen *first = screens_cur();
    CHECK(fk_open(&fks[0], 300, 200, false));
    seat_focus(win(0));
    /* 700 wide: the right half (631, its border off) is too narrow */
    CHECK(fk_open_min(&fks[1], 300, 200, 700, 100));
    CHECK(fks[1].ww->screen != first && screens_cur() == fks[1].ww->screen);
    CHECK(tile_is(1, ALL) && tile_is(0, ALL));
    screens_go(0);
    seat_focus(win(0));
    /* 500 by 300 fits the right half: an ordinary split */
    CHECK(fk_open_min(&fks[2], 300, 200, 500, 300));
    CHECK(fks[2].ww->screen == first && tile_is(0, LEFT) && tile_is(2, RIGHT));
    /* the old window's minimum: 2's 500 high no longer fits its top half */
    wm_set_limits(fks[2].ww, 500, 500, 0, 0);
    seat_focus(win(2));
    CHECK(fk_open(&fks[3], 300, 200, false));
    CHECK(fks[3].ww->screen != first && tile_is(3, ALL));
    CHECK(tile_is(0, LEFT) && tile_is(2, RIGHT));
    screens_go(0);
    /* a fixed 600x450 window: bigger than the left half's bottom (391 high) */
    seat_focus(win(0));
    CHECK(fk_open(&fks[4], 600, 450, true));
    CHECK(fks[4].ww->screen != first && screens_cur() == fks[4].ww->screen);
    screens_go(0);
    /* a fixed 300x200 one fits it: the left half splits, it is centred there */
    seat_focus(win(0));
    CHECK(fk_open(&fks[5], 300, 200, true));
    CHECK(fks[5].ww->screen == first);
    CHECK(tile_is(0, ((struct comp_box){ 6, 6, 637, 397 })));
    CHECK(tile_is(5, ((struct comp_box){ 6, 403, 637, 794 })));
    return true;
}

bool t_wm_tile_small(void)
{
    wm_test_start(COMP_TILING);
    bool ok = tile_small_steps() && tile_small_switch_steps();
    fk_close_all();
    wm_test_start(COMP_TILING);
    ok = ok && tile_small_min_steps();
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
    cursor.moved = false;   /* where it starts (the output's middle): no mouse has moved it */
    damage_clear(&scene.damage);
    wm_marks_update();
    CHECK(box_empty(wm_marks.bar));
    CHECK_EQ(scene.damage.n, 0);
    cursor.moved = true;
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
    /* past the right tile's minimum (200 wide, before 85%): held there */
    move_to(5000, 400);
    CHECK_EQ(win(0)->tile.x2, 1274 - WM_TILE_MIN_W - WM_GAP);
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
    /* no further than the right tile's minimum (200 wide, before 85%) */
    for (int i = 0; i < 20; i++)
        (void)wm_test_key(U_RIGHT, U_RIGHT_ALT);
    anim_finish();
    CHECK_EQ(win(0)->tile.x2, 1274 - WM_TILE_MIN_W - WM_GAP);
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

/* Window i's frame inside box b. */
static bool frame_in(unsigned i, struct comp_box b)
{
    struct comp_box f = window_frame(win(i));
    if (f.x1 < b.x1 || f.y1 < b.y1 || f.x2 > b.x2 || f.y2 > b.y2)
        FAIL("window %u's frame %d,%d..%d,%d is outside %d,%d..%d,%d", i, f.x1, f.y1, f.x2, f.y2,
             b.x1, b.y1, b.x2, b.y2);
    return true;
}

/* The surface size a tile's floating box gives (wm.c's float_from_tile). */
static struct comp_box float_inner(struct comp_box tile)
{
    struct comp_box f = { tile.x1 + 4, tile.y1 + 4, tile.x2 - 4, tile.y2 - 4 };
    return deco_inner(f, 0, COMP_FLOATING);
}

static bool tile_float_steps(void)
{
    /* three windows floating first (centred and cascaded: places the
     * switch below must not use) */
    for (unsigned i = 0; i < 3; i++)
        CHECK(fk_open(&fks[i], 320, 200, false));
    int32_t old_x = win(0)->x, old_y = win(0)->y;
    /* tiling: 0 left, 1 top right, 2 bottom right; then 3 splits 2's tile,
     * and is given a min width of 400 its 312 can't hold */
    wm_toggle_layout();
    seat_focus(win(2));
    CHECK(fk_open(&fks[3], 320, 200, false));
    wm_set_limits(fks[3].ww, 400, 0, 0, 0);
    anim_finish();
    struct comp_box tile[4];
    for (unsigned i = 0; i < 4; i++) {
        CHECK(fk_draw(&fks[i]));
        tile[i] = win(i)->tile;
    }
    CHECK(box_eq(tile[0], LEFT) && box_eq(tile[1], RTOP));
    /* floating: each window's box from its tile, its frame inside it, the
     * arrangement kept, no two overlapping; the old places unused */
    wm_toggle_layout();
    for (unsigned i = 0; i < 3; i++) {
        struct comp_box in = float_inner(tile[i]);
        CHECK_EQ(fks[i].cfg.width, in.x2 - in.x1);
        CHECK_EQ(fks[i].cfg.height, in.y2 - in.y1);
        CHECK(fk_draw(&fks[i]));
        AT(i, in.x1, in.y1);
        CHECK(frame_in(i, tile[i]));
    }
    struct comp_box f0 = window_frame(win(0)), f1 = window_frame(win(1)),
                    f2 = window_frame(win(2));
    CHECK(f0.x2 < f1.x1 && f0.x2 < f2.x1 && f1.y2 < f2.y1);   /* left, top right, bottom */
    CHECK(box_empty(box_intersect(f0, f1)) && box_empty(box_intersect(f0, f2)) &&
          box_empty(box_intersect(f1, f2)));
    CHECK(win(0)->x != old_x || win(0)->y != old_y);
    /* its min: 400 wide, from its tile's top left, kept on the output */
    CHECK_EQ(fks[3].cfg.width, 400);
    CHECK(fk_draw(&fks[3]));
    CHECK(on_output(3, 0));
    struct comp_box f3 = window_frame(win(3));
    CHECK(f3.y1 >= tile[3].y1 && f3.x2 <= OUT_W);
    /* tiling and back: from the tiles again (the floating places don't stay) */
    wm_toggle_layout();
    anim_finish();
    for (unsigned i = 0; i < 4; i++)
        CHECK(fk_draw(&fks[i]));
    wm_toggle_layout();
    struct comp_box in = float_inner(win(0)->tile);
    CHECK_EQ(fks[0].cfg.width, in.x2 - in.x1);
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

/* With the desktop on: its tile's box, under the strip. */
static bool strip_float_steps(void)
{
    desk_test_start(COMP_TILING);
    CHECK(fk_open(&fks[0], 320, 200, false));
    anim_finish();
    CHECK(fk_draw(&fks[0]));
    int32_t floor = wm_floor(fks[0].ww);
    CHECK(floor > 0);
    struct comp_box tile = win(0)->tile, in = float_inner(tile);
    CHECK(tile.y1 >= floor);
    wm_toggle_layout();   /* its tile's box: under the strip */
    CHECK_EQ(fks[0].cfg.height, in.y2 - in.y1);
    CHECK(fk_draw(&fks[0]));
    CHECK(on_output(0, floor) && frame_in(0, tile));
    return true;
}

bool t_wm_tile_float(void)
{
    wm_test_start(COMP_FLOATING);
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
