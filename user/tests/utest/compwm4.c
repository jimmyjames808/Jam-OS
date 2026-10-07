/* utest: tiles and their windows' minimums (user/services/compositor
 * wmtile.c's needs, wm.c's clip), on compwm.c's harness (compwm.h). The
 * desktop is off (the room is the output, 1280x800, less a gap of 6 all
 * round), a border of 2 inside each tile.
 *
 * t_wm_tile_mins: a gap dragged towards a window with a minimum stops
 * where its tile would get less than the minimum and its border (the
 * owner's PC: Jamjar's 944x568 beside a terminal, the terminal's tile grew
 * over Jamjar), and towards a plain window at the tiler's 200 minimum,
 * both ways; Super+Alt+direction stops at the same places and says
 * nothing moved; nested splits along the same axis add their halves'
 * needs (and the inner split gives the room to the half that needs it),
 * across the other axis the larger; a minimum that comes after the
 * window is tiled moves the gap at once; with less room than the tiles
 * need a window bigger than its tile is clipped to it, from its top left,
 * never over its neighbour. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include "compwm.h"
#include "utest.h"

#define U_H     0x0b
#define U_L     0x0f
#define SUPER_ALT (INPUT_MOD_LGUI | INPUT_MOD_LALT)
#define BORDER  2   /* a tile's border, each side (DECO_BORDER) */

static bool tile_x(unsigned i, int32_t x1, int32_t x2)
{
    CHECK(fks[i].ww->leaf);
    if (win(i)->tile.x1 != x1 || win(i)->tile.x2 != x2)
        FAIL("window %u's tile is x %d..%d, want %d..%d", i, win(i)->tile.x1, win(i)->tile.x2,
             x1, x2);
    return true;
}

/* 0 (its minimum 500 by 300) left and 1 right, both drawn, 0 focused. */
static bool min_pair(void)
{
    CHECK(fk_open_min(&fks[0], 300, 200, 500, 300));
    seat_focus(win(0));
    CHECK(fk_open(&fks[1], 300, 200, false));
    CHECK(fk_draw(&fks[0]) && fk_draw(&fks[1]));
    CHECK(tile_x(0, 6, 637) && tile_x(1, 643, 1274));
    return true;
}

static bool drag_steps(void)
{
    /* towards 0: its tile stops at its minimum and border (504), the gap
     * with it; its configure is its minimum */
    CHECK(press_btn(640, 400, BTN));
    move_to(300, 400);
    CHECK(tile_x(0, 6, 6 + 500 + 2 * BORDER) && tile_x(1, 516, 1274));
    CHECK_EQ(fks[0].cfg.width, 500);
    move_to(-400, 400);
    CHECK(tile_x(0, 6, 510));
    /* and back the other way at once: it follows the pointer again */
    move_to(700, 400);
    CHECK(tile_x(0, 6, 697));
    /* towards 1 (no minimum of its own): the tiler's 200 */
    move_to(5000, 400);
    CHECK(tile_x(1, 1274 - WM_TILE_MIN_W, 1274));
    release_at(5000, 400);
    CHECK(fk_draw(&fks[0]) && fk_draw(&fks[1]));
    /* a press on the gap's new place drags it back as far as 0's minimum */
    CHECK(drag(1274 - WM_TILE_MIN_W - WM_GAP / 2, 400, -2000, 0));
    CHECK(tile_x(0, 6, 510));
    return true;
}

/* Super+Alt+direction on the focused tile; its tile's right edge after. */
static bool push_to(uint16_t usage, unsigned i, int32_t x2)
{
    CHECK(wm_test_key(usage, SUPER_ALT));
    anim_finish();
    CHECK_EQ(win(i)->tile.x2, x2);
    return true;
}

static bool push_steps(void)
{
    /* 0 at its minimum: pushing its right edge left moves nothing */
    seat_focus(win(0));
    CHECK(!tiles_push(fks[0].ww, -1, 0));
    CHECK(push_to(U_H, 0, 510));
    /* right, 48 a push, as far as 1's 200 */
    CHECK(push_to(U_L, 0, 558));
    for (int i = 0; i < 30; i++) {
        (void)tiles_push(fks[0].ww, 1, 0);
        wm_relayout();
    }
    CHECK(tile_x(1, 1274 - WM_TILE_MIN_W, 1274));
    CHECK(!tiles_push(fks[0].ww, 1, 0));
    /* from 1's side: its left edge left, as far as 0's minimum */
    seat_focus(win(1));
    for (int i = 0; i < 30; i++) {
        (void)tiles_push(fks[1].ww, -1, 0);
        wm_relayout();
    }
    CHECK(tile_x(0, 6, 510));
    CHECK(!tiles_push(fks[1].ww, -1, 0));
    return true;
}

/* A minimum told after the window is tiled (xdg's set_min_size on a later
 * commit): the gap makes room at once. */
static bool late_min_steps(void)
{
    CHECK(drag(513, 400, 3000, 0));   /* 1 down to the tiler's 200 */
    CHECK(tile_x(1, 1274 - WM_TILE_MIN_W, 1274));
    wm_set_limits(fks[1].ww, 700, 0, 0, 0);
    CHECK(tile_x(1, 1274 - 700 - 2 * BORDER, 1274));
    CHECK_EQ(fks[1].cfg.width, 700);
    /* and dragged towards it again: held there */
    CHECK(drag(win(1)->tile.x1 - WM_GAP / 2, 400, 3000, 0));
    CHECK(tile_x(1, 1274 - 700 - 2 * BORDER, 1274));
    return true;
}

/* Both minimums together more than the room: shared in proportion to the
 * needs, each window that is bigger than its tile clipped to it, from the
 * tile's top left, its frame the tile's. */
static bool clip_steps(void)
{
    wm_set_limits(fks[1].ww, 1000, 0, 0, 0);   /* 504 + 6 + 1004 > 1262 */
    CHECK(fk_draw(&fks[0]) && fk_draw(&fks[1]));
    for (unsigned i = 0; i < 2; i++) {
        struct comp_box t = win(i)->tile, in = tile_inner(t);
        struct comp_box s = window_surface_box(win(i));
        CHECK(fks[i].s.width > in.x2 - in.x1);   /* it drew its minimum */
        CHECK(box_eq(s, ((struct comp_box){ in.x1, in.y1, in.x2, s.y2 })));
        CHECK(box_eq(window_frame(win(i)), ((struct comp_box){ t.x1, t.y1, t.x2,
                                                               window_frame(win(i)).y2 })));
    }
    CHECK(win(0)->tile.x2 + WM_GAP == win(1)->tile.x1);
    CHECK(box_empty(box_intersect(window_frame(win(0)), window_frame(win(1)))));
    /* the room's share: 1262 * 504 / 1508 */
    CHECK(tile_x(0, 6, 6 + 1262 * 504 / 1508));
    /* the minimum gone: the gap where the ratio left it, nothing clipped */
    wm_set_limits(fks[1].ww, 0, 0, 0, 0);
    CHECK(fk_draw(&fks[0]) && fk_draw(&fks[1]));
    CHECK_EQ(win(1)->view_w, fks[1].s.width);
    return true;
}

/* [0 | [1 / [2 | 3]]]: 3 has a minimum 300 wide, so the right column needs
 * 200 + 6 + 304 = 510 across; the root's gap stops there. 0's minimum
 * (400) stops it the other way. */
static bool nested_steps(void)
{
    CHECK(fk_open_min(&fks[0], 300, 200, 400, 0));
    seat_focus(win(0));
    CHECK(fk_open(&fks[1], 300, 200, false));    /* right half: 631 x 788, splits down */
    seat_focus(win(1));
    CHECK(fk_open(&fks[2], 300, 200, false));    /* bottom right: 631 x 391, across */
    seat_focus(win(2));
    CHECK(fk_open_min(&fks[3], 300, 200, 300, 0));
    for (unsigned i = 0; i < 4; i++)
        CHECK(fk_draw(&fks[i]));
    struct tile_node *root = screens_cur()->tree;
    CHECK(root && root->across);
    CHECK_EQ(root->b->need_w, WM_TILE_MIN_W + WM_GAP + 300 + 2 * BORDER);
    CHECK_EQ(root->a->need_w, 400 + 2 * BORDER);
    /* the root's gap dragged far right: the right column keeps 510; inside
     * it 2 keeps its 200 and 3 its 304 */
    CHECK(drag(640, 200, 3000, 0));
    CHECK(tile_x(0, 6, 1274 - 510 - WM_GAP));
    CHECK(tile_x(1, 1274 - 510, 1274));
    CHECK(tile_x(2, 1274 - 510, 1274 - 510 + WM_TILE_MIN_W));
    CHECK(tile_x(3, 1274 - 304, 1274));
    /* and far left: 0's minimum */
    CHECK(drag(1274 - 510 - WM_GAP / 2, 200, -3000, 0));
    CHECK(tile_x(0, 6, 6 + 404));
    /* the inner across split's own gap: 3's minimum one way, 2's 200 the other */
    struct comp_box t3 = win(3)->tile;
    CHECK(drag(t3.x1 - WM_GAP / 2, 600, 3000, 0));
    CHECK(tile_x(3, 1274 - 304, 1274));
    CHECK(drag(win(3)->tile.x1 - WM_GAP / 2, 600, -3000, 0));
    CHECK_EQ(win(2)->tile.x2 - win(2)->tile.x1, WM_TILE_MIN_W);
    /* down the column (the other axis): 1 over [2 | 3], each 120 high at least */
    struct comp_box t1 = win(1)->tile;
    CHECK(drag((t1.x1 + t1.x2) / 2, t1.y2 + WM_GAP / 2, 0, 3000));
    CHECK_EQ(win(3)->tile.y2 - win(3)->tile.y1, WM_TILE_MIN_H);
    CHECK(drag((t1.x1 + t1.x2) / 2, win(1)->tile.y2 + WM_GAP / 2, 0, -3000));
    CHECK_EQ(win(1)->tile.y2 - win(1)->tile.y1, WM_TILE_MIN_H);
    return true;
}

bool t_wm_tile_mins(void)
{
    wm_test_start(COMP_TILING);
    bool ok = min_pair() && drag_steps() && push_steps() && late_min_steps() && clip_steps();
    fk_close_all();
    wm_test_start(COMP_TILING);
    ok = ok && nested_steps();
    fk_close_all();
    return ok;
}
