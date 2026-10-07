/* utest: the compositor's memory across its restarts (user/services/
 * compositor wmsave.c, wmtile.c's placeholders), on compwm.c's harness
 * (compwm.h). A "restart" here is what a new compositor does: the window
 * manager started afresh (wm_test_start) and handed the description the
 * old one left (wm_save_describe, then wm_save_load); the windows then
 * come back one by one, each presenting the key it was given
 * (fk_open_keyed: jam_window_memory_v1.identify before its initial
 * commit), in another order than they opened.
 *
 * t_wm_save_restore: two screens, the first tiling (0 | [1 / 2], the
 * root's gap moved; 0 and 1 with one title), the second floating (3
 * moved by its title bar, 4 minimised), the first shown and 1 focused;
 * back in the order 4, 2, 1, 3, 0: each window's first configure is its
 * old tile (its place kept while the others are missing), the same tree
 * and ratio, 3 at its floating place on the second screen, 4 minimised
 * there, the first screen still shown, the focus on 1 once it is back;
 * the wait over when the last is back; a key isn't taken twice.
 * t_wm_save_late: a window that doesn't come back keeps its place for
 * WM_SAVE_HOLD_NS, then its place goes as a closed window's does; one that
 * comes later, or presents no key, is a new window with a new key.
 * t_wm_save_full: a full-screen window comes back full screen, its full
 * screen shown if it was, else the screen that was.
 * t_wm_save_bad: a description with a bad magic, another version, a bad
 * checksum, any count, index, key, ratio, box or tree wrong, is ignored
 * whole (nothing of it used); one taken while windows are there refused;
 * random damage (with the checksum made to fit) never crashes, and what
 * loads is a sane arrangement. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include "compwm.h"
#include "utest.h"

static struct wm_save d, copy;
static uint64_t key[FK_MAX];

/* d: the arrangement now, as a dying compositor leaves it. */
static bool describe(void)
{
    wm_save_describe(&d);
    d.sum = wm_save_sum(&d);
    return true;
}

/* The restart: every window gone with its old compositor, a fresh one
 * (layout as init passes it) handed d. */
static bool restart(enum comp_layout layout, uint64_t t)
{
    memset(fks, 0, sizeof(fks));   /* the old compositor's windows died with it */
    wm_test_start(layout);
    CHECK_ST(wm_save_load(&d, t), OK);
    return true;
}

static bool back(unsigned i)
{
    uint64_t got;
    CHECK(fk_open_keyed(&fks[i], 300, 200, key[i], &got));
    CHECK_EQ(got, key[i]);
    return true;
}

/* ---- the whole arrangement ---------------------------------------------------------------- */

static struct comp_box tile[3];
static int32_t fx, fy, fw, fh, ratio;

static bool arrange(void)
{
    CHECK(fk_open_keyed(&fks[0], 300, 200, 0, &key[0]));
    seat_focus(win(0));
    CHECK(fk_open_keyed(&fks[1], 300, 200, 0, &key[1]));
    seat_focus(win(1));
    CHECK(fk_open_keyed(&fks[2], 300, 200, 0, &key[2]));
    CHECK(key[0] != key[1] && key[1] != key[2]);
    wm_set_title(fks[0].ww, "Terminal");
    wm_set_title(fks[1].ww, "Terminal");
    struct tile_node *root = screens_cur()->tree;
    tiles_gap_move(root, 500);
    wm_relayout();
    for (unsigned i = 0; i < 3; i++) {
        CHECK(fk_draw(&fks[i]));
        tile[i] = win(i)->tile;
    }
    ratio = root->ratio;
    /* the second screen floats: 3 moved by its title bar, 4 minimised */
    screens_step(1);
    wm_set_layout(COMP_FLOATING);
    CHECK(fk_open_keyed(&fks[3], 300, 200, 0, &key[3]));
    CHECK(fk_open_keyed(&fks[4], 300, 200, 0, &key[4]));
    struct comp_box f3 = window_frame(win(3));
    CHECK(drag((f3.x1 + f3.x2) / 2, f3.y1 + COMP_TITLE_H / 2, -150, -80));
    fx = win(3)->x;
    fy = win(3)->y;
    fw = fks[3].ww->float_w;
    fh = fks[3].ww->float_h;
    screens_minimise(fks[4].ww);
    screens_go(0);
    seat_focus(win(1));
    CHECK(describe());
    CHECK_EQ(d.nscreens, 2);
    CHECK_EQ(d.nwins, 5);
    CHECK_EQ(d.cur, 0);
    CHECK_EQ(d.focused, key[1]);
    CHECK_EQ(d.screens[0].n, 5);   /* two splits, three tiles */
    return true;
}

static bool restore_steps(void)
{
    uint64_t t = now();
    CHECK(restart(COMP_TILING, t));
    CHECK(wm_save_pending());
    CHECK_EQ(wm_save_deadline(), t + WM_SAVE_HOLD_NS);
    CHECK_EQ(screens_count(), 2);
    CHECK_EQ(screens_cur_index(), 0);
    CHECK_EQ(screens_nth(0)->layout, COMP_TILING);
    CHECK_EQ(screens_nth(1)->layout, COMP_FLOATING);
    struct comp_box b;
    CHECK(tiles_held_box(key[2], &b) && box_eq(b, tile[2]));   /* places kept, empty */
    /* 4 first: minimised on the second screen, not shown */
    CHECK(back(4));
    CHECK(fks[4].ww->minimised && fks[4].ww->screen == screens_nth(1));
    CHECK(!(win(4)->flags & COMP_WIN_MAPPED));
    /* 2: its old tile, from its first configure, with 0 and 1 missing */
    CHECK(back(2));
    CHECK(box_eq(win(2)->tile, tile[2]));
    CHECK(fills_box(2, tile_inner(tile[2])));
    /* 1 before 0 (one title): each its own tile */
    CHECK(back(1));
    CHECK(box_eq(win(1)->tile, tile[1]));
    CHECK(wm_focused() == fks[1].ww);   /* it had the keys */
    CHECK(back(3));
    CHECK(fks[3].ww->screen == screens_nth(1) && !fks[3].ww->minimised);
    CFG(3, fw, fh, 0);
    AT(3, fx, fy);
    CHECK(!(win(3)->flags & COMP_WIN_MAPPED));   /* the first screen is still the one shown */
    CHECK(wm_save_pending());
    CHECK(back(0));
    CHECK(box_eq(win(0)->tile, tile[0]));
    CHECK(!wm_save_pending());   /* the last one back ends the wait */
    CHECK_EQ(screens_cur_index(), 0);
    CHECK(wm_focused() == fks[1].ww);
    CHECK_EQ(screens_cur()->tree->ratio, ratio);
    for (unsigned i = 0; i < 3; i++)
        CHECK(fills_box(i, tile_inner(tile[i])));
    /* and the same again: the description of the restored arrangement is
     * the one it came from (the windows' order aside) */
    static struct wm_save again;
    wm_save_describe(&again);
    CHECK_EQ(again.nwins, d.nwins);
    CHECK_EQ(again.nnodes, d.nnodes);
    CHECK_EQ(again.focused, d.focused);
    for (unsigned i = 0; i < again.nnodes; i++)
        CHECK_EQ(again.nodes[i].ratio, d.nodes[i].ratio);
    /* a key is taken once: another window presenting 0's is a new one */
    uint64_t got;
    CHECK(fk_open_keyed(&fks[5], 300, 200, key[0], &got));
    CHECK(got != key[0]);
    return true;
}

bool t_wm_save_restore(void)
{
    wm_test_start(COMP_TILING);
    bool ok = arrange() && restore_steps();
    fk_close_all();
    return ok;
}

/* ---- late, missing, not remembered -------------------------------------------------------- */

static bool three_tiles(void)
{
    CHECK(fk_open_keyed(&fks[0], 300, 200, 0, &key[0]));
    seat_focus(win(0));
    CHECK(fk_open_keyed(&fks[1], 300, 200, 0, &key[1]));
    seat_focus(win(1));
    CHECK(fk_open_keyed(&fks[2], 300, 200, 0, &key[2]));
    for (unsigned i = 0; i < 3; i++) {
        CHECK(fk_draw(&fks[i]));
        tile[i] = win(i)->tile;
    }
    return describe();
}

static bool late_steps(void)
{
    uint64_t t = now();
    CHECK(restart(COMP_TILING, t));
    CHECK(back(0) && back(2));
    CHECK(box_eq(win(0)->tile, tile[0]) && box_eq(win(2)->tile, tile[2]));
    wm_save_tick(t + WM_SAVE_HOLD_NS - 1);
    CHECK(wm_save_pending());
    CHECK(box_eq(win(2)->tile, tile[2]));   /* 1's place still kept */
    /* the wait over: 1's place goes, 2 takes their parent's room */
    wm_save_tick(t + WM_SAVE_HOLD_NS);
    CHECK(!wm_save_pending());
    CHECK(!tiles_held_box(key[1], &(struct comp_box){ 0 }));
    CHECK(box_eq(win(2)->tile, ((struct comp_box){ tile[1].x1, tile[1].y1, tile[2].x2,
                                                    tile[2].y2 })));
    CHECK(box_eq(win(0)->tile, tile[0]));
    /* 1 late: a new key, a new window (it splits a tile) */
    uint64_t got;
    seat_focus(win(2));
    CHECK(fk_open_keyed(&fks[1], 300, 200, key[1], &got));
    CHECK(got != key[1]);
    CHECK(fks[1].ww->leaf);
    CHECK(box_eq(win(1)->tile, tile[2]));   /* 2's half, below it */
    return true;
}

/* A window that presents no key while the others are awaited: a new
 * window with a key of its own; the remembered ones still get theirs. */
static bool unknown_steps(void)
{
    uint64_t t = now(), got;
    CHECK(restart(COMP_TILING, t));
    CHECK(back(0));
    CHECK(fk_open_keyed(&fks[3], 300, 200, 0, &got));
    CHECK(got && got != key[0] && got != key[1] && got != key[2]);
    CHECK(fks[3].ww->leaf);
    CHECK(back(1) && back(2));
    CHECK(!wm_save_pending());
    for (unsigned i = 0; i < 4; i++)
        CHECK(fks[i].ww->leaf);
    return true;
}

bool t_wm_save_late(void)
{
    wm_test_start(COMP_TILING);
    bool ok = three_tiles() && late_steps();
    fk_close_all();
    ok = ok && unknown_steps();
    fk_close_all();
    return ok;
}

/* ---- full screen --------------------------------------------------------------------------- */

static bool full_steps(bool shown)
{
    wm_test_start(COMP_TILING);
    CHECK(fk_open_keyed(&fks[0], 300, 200, 0, &key[0]));
    seat_focus(win(0));
    CHECK(fk_open_keyed(&fks[1], 300, 200, 0, &key[1]));
    wm_request_fullscreen(fks[0].ww, true);
    CHECK(fk_draw(&fks[0]));
    seat_focus(win(0));
    CHECK_EQ(screens_cur()->kind, SCREEN_FULL);
    if (!shown)
        screens_go(0);
    CHECK(describe());
    CHECK_EQ(d.cur_full, shown ? key[0] : 0);
    CHECK_EQ(d.cur, 0);
    CHECK(restart(COMP_TILING, now()));
    CHECK(back(1));
    CHECK_EQ(screens_cur()->kind, SCREEN_NORMAL);
    CHECK(back(0));
    CHECK(fks[0].cfg.states & WM_ST_FULLSCREEN);
    CHECK(fk_draw(&fks[0]));
    CHECK_EQ(fks[0].ww->screen->kind, SCREEN_FULL);
    CHECK_EQ(screens_count(), 2);
    if (shown) {
        CHECK(screens_cur() == fks[0].ww->screen);
        CHECK(wm_focused() == fks[0].ww);
    } else {
        CHECK_EQ(screens_cur_index(), 0);
    }
    fk_close_all();
    return true;
}

bool t_wm_save_full(void)
{
    return full_steps(true) && full_steps(false);
}

/* ---- descriptions that can't be used -------------------------------------------------------- */

/* copy, damaged by `how` (the checksum made to fit unless sum is false), is
 * ignored whole: nothing of it used. */
static bool refused(const char *how, bool sum)
{
    if (sum)
        copy.sum = wm_save_sum(&copy);
    memset(fks, 0, sizeof(fks));
    wm_test_start(COMP_TILING);
    status_t st = wm_save_load(&copy, now());
    if (st != ERR_INVALID_ARGS)
        FAIL("%s: taken (%s)", how, status_str(st));
    CHECK(screens_count() == 1 && !screens_cur()->tree && !wm_save_pending());
    copy = d;
    return true;
}

/* A sane arrangement: every screen's tree's leaves are windows or
 * placeholders of remembered keys, and every count in bounds. */
static bool sane(void)
{
    CHECK(screens_count() >= 1 && screens_count() <= DESK_SCREENS_MAX);
    CHECK(screens_cur_index() < screens_count());
    return true;
}

static bool bad_steps(void)
{
    wm_test_start(COMP_TILING);
    CHECK(three_tiles());
    seat_focus(win(1));
    screens_step(1);
    wm_set_layout(COMP_FLOATING);
    CHECK(fk_open_keyed(&fks[3], 300, 200, 0, &key[3]));
    screens_go(0);
    CHECK(describe());
    fk_close_all();
    copy = d;
    /* the control: it is taken */
    copy.sum = wm_save_sum(&copy);
    wm_test_start(COMP_TILING);
    CHECK_ST(wm_save_load(&copy, now()), OK);
    CHECK_EQ(screens_count(), 2);
    copy = d;
    copy.magic ^= 1;
    CHECK(refused("a bad magic", true));
    copy.version = WM_SAVE_VERSION + 1;
    CHECK(refused("another version", true));
    copy.nodes[0].ratio ^= 1;
    CHECK(refused("a bad checksum", false));
    copy.bytes--;
    CHECK(refused("another size", true));
    copy.nwins = WM_SAVE_WINDOWS + 1;
    CHECK(refused("too many windows", true));
    copy.nscreens = 0;
    CHECK(refused("no screens", true));
    copy.nscreens = WM_SAVE_SCREENS + 1;
    CHECK(refused("too many screens", true));
    copy.nnodes = WM_SAVE_NODES + 1;
    CHECK(refused("too many nodes", true));
    copy.cur = copy.nscreens;
    CHECK(refused("no such screen shown", true));
    copy.deflt = 2;
    CHECK(refused("no such layout", true));
    copy.screens[1].layout = 7;
    CHECK(refused("a screen's layout", true));
    copy.wins[3].screen = 5;
    CHECK(refused("a window on no screen", true));
    copy.wins[0].key = 0;
    CHECK(refused("a window with no key", true));
    copy.wins[1].key = copy.wins[0].key;
    CHECK(refused("one key twice", true));
    copy.wins[2].want = 9;
    CHECK(refused("no such mode", true));
    copy.wins[3].fw = 100000;
    CHECK(refused("a floating size too big", true));
    copy.wins[3].fx = -1000000;
    CHECK(refused("a floating place far off", true));
    copy.wins[0].flags = 0xff;
    CHECK(refused("unknown flags", true));
    copy.wins[0].flags = WM_SAVE_MINIMISED;
    CHECK(refused("a minimised window in a tile", true));
    copy.nodes[1].win = (uint16_t)copy.nwins;
    CHECK(refused("a tile of no window", true));
    CHECK(copy.nodes[2].win == WM_SAVE_SPLIT && copy.nodes[3].win < WM_SAVE_GONE);
    copy.nodes[3].win = copy.nodes[1].win;   /* (0 | [1 / 2]): 0 in 1's tile too */
    CHECK(refused("a window in two tiles", true));
    copy.nodes[0].ratio = TILE_MAX + 1;
    CHECK(refused("a ratio past 85%", true));
    copy.nodes[0].ratio = 0;
    CHECK(refused("a ratio of nothing", true));
    copy.screens[0].n--;
    copy.nnodes--;
    CHECK(refused("a tree cut short", true));
    copy.nodes[copy.screens[0].n - 1].win = WM_SAVE_SPLIT;
    CHECK(refused("a tree that never ends", true));
    copy.screens[0].first = 1;
    CHECK(refused("a tree not where it says", true));
    copy.screens[1].n = 1;
    copy.screens[1].first = (uint16_t)copy.nnodes;
    copy.nnodes++;
    CHECK(refused("a floating screen with a tree", true));
    copy.focused = 12345;
    CHECK(refused("the focus on no window", true));
    copy.cur_full = copy.wins[0].key;
    CHECK(refused("a full screen shown of a window that isn't", true));
    /* taken only by a fresh compositor */
    wm_test_start(COMP_TILING);
    CHECK(fk_open(&fks[0], 300, 200, false));
    copy.sum = wm_save_sum(&copy);
    CHECK_ST(wm_save_load(&copy, now()), ERR_BAD_STATE);
    fk_close_all();
    /* random damage, the checksum made to fit: refused or sane, never a crash */
    for (unsigned n = 0; n < 400; n++) {
        copy = d;
        uint8_t *p = (uint8_t *)&copy;
        for (unsigned k = 1 + n % 4; k; k--) {
            uint32_t r = os_random_u32();
            p[r % offsetof(struct wm_save, sum)] ^= (uint8_t)(1u << (r >> 24) % 8);
        }
        copy.sum = wm_save_sum(&copy);
        memset(fks, 0, sizeof(fks));
        wm_test_start(COMP_TILING);
        if (wm_save_load(&copy, now()) == OK) {
            CHECK(sane());
            for (unsigned i = 0; i < 4 && i < FK_MAX; i++) {   /* the windows come back */
                uint64_t got;
                CHECK(fk_open_keyed(&fks[i], 300, 200, d.wins[i].key, &got));
            }
            wm_save_tick(now() + WM_SAVE_HOLD_NS);
            CHECK(sane() && !wm_save_pending());
            fk_close_all();
        }
    }
    return true;
}

bool t_wm_save_bad(void)
{
    bool ok = bad_steps();
    memset(fks, 0, sizeof(fks));
    wm_test_start(COMP_TILING);
    return ok;
}
