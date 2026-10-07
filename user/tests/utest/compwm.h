/* utest's harness for the compositor's window manager and desktop linked
 * in (compwm.c defines it; compdesk.c and compdesk2.c use it too): the
 * seat played by the test, fake toplevels that play their clients, and
 * the presses a user makes. The output is OUT_W x OUT_H; each test starts
 * with wm_test_start (the desktop off) or desk_test_start (on, with no
 * animations, the clock fixed), and ends with fk_close_all. */
#pragma once

#include "desk.h"

#define OUT_W 1280
#define OUT_H 800
#define FK_MAX 6
#define BTN   0x110u   /* evdev's left button */

/* The seat, as the window manager sees it. */
struct fake_seat {
    const struct comp_grab_ops *ops;   /* the grab the window manager began */
    void *data;
    bool held;                         /* a button is down */
    struct comp_window *focused;
    unsigned nmapped, ngone, nlayout;
    struct comp_window *gone;          /* the last window gone */
    enum comp_layout layout;           /* the last switch reported */
    bool save;                         /* reported switches go to WF, as init would write them */
    uint8_t mods;                      /* the modifiers held at a press (INPUT_MOD_*) */
};
extern struct fake_seat seat;
extern struct comp_client fake_client;
extern struct comp_client fake_keyless;   /* a client with no wl_keyboard (the splash's) */

/* A fake toplevel: its surface, the last configure it got, closes asked. */
struct fk {
    struct comp_surface s;
    struct wm_window *ww;
    struct wm_config cfg;
    unsigned nconf, closes;
    int32_t own_w, own_h;          /* what it draws when the configure says 0 */
    bool fixed;                    /* min = max = its own size */
};
extern struct fk fks[FK_MAX];

void wm_test_start(enum comp_layout layout);
/* compdesk.c: the same with the desktop on, no animations, the clock at
 * Monday 5 October 2026 14:32. */
void desk_test_start(enum comp_layout layout);
/* The client answers its last configure: draws, commits. */
bool fk_draw(struct fk *f);
/* A toplevel w by h of its own: the initial configure, then its first buffer. */
bool fk_open(struct fk *f, int32_t w, int32_t h, bool fixed);
/* A full-screen toplevel of client cl's (fake_keyless: a boot overlay), asked
 * for before its first buffer. */
bool fk_open_full(struct fk *f, struct comp_client *cl);
/* A toplevel w by h of its own, maximised before its first buffer (as
 * libfun asks on a screen up to 1920x1200). */
bool fk_open_max(struct fk *f, int32_t w, int32_t h);
/* fk_open, resizable, with a declared minimum (xdg_toplevel.set_min_size)
 * before its initial commit. */
bool fk_open_min(struct fk *f, int32_t w, int32_t h, int32_t min_w, int32_t min_h);
void fk_close_all(void);
struct comp_window *win(unsigned i);
/* A press at (x, y) with button (and seat.mods held): did the window
 * manager (or the desktop) take it? */
bool press_btn(int32_t x, int32_t y, uint32_t button);
/* The same with Super held. */
bool press_super(int32_t x, int32_t y, uint32_t button);
/* A key press as the seat offers it (focus.c): the desktop's, then the
 * window manager's. Taken? */
bool wm_test_key(uint16_t usage, uint8_t mods);
/* Window i's surface at (wx, wy); fake i's last configure (in a test body). */
#define AT(i, wx, wy)                                                   \
    do {                                                                \
        CHECK(win(i));                                                  \
        CHECK_EQ(win(i)->x, (wx));                                      \
        CHECK_EQ(win(i)->y, (wy));                                      \
    } while (0)
#define CFG(i, cw, ch, st)                                              \
    do {                                                                \
        CHECK_EQ(fks[i].cfg.width, (cw));                               \
        CHECK_EQ(fks[i].cfg.height, (ch));                              \
        CHECK_EQ(fks[i].cfg.states, (st));                              \
    } while (0)

static inline bool box_eq(struct comp_box a, struct comp_box b)
{
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}
/* tile's surface box for a resizable tiled window (its border inside it). */
struct comp_box tile_inner(struct comp_box tile);
/* f was asked to fill box b (a surface box), draws, and sits in it. */
bool fills_box(unsigned f, struct comp_box b);
void move_to(int32_t x, int32_t y);
/* The pointer to (x, y), and the button up: a grab ends. */
void release_at(int32_t x, int32_t y);
/* A whole drag: press at (x, y), move by (dx, dy), release there. */
bool drag(int32_t x, int32_t y, int32_t dx, int32_t dy);

/* The fakes' counts for the desktop's tests: snapshots made and freed,
 * apps launched, commands run, notification answers. */
struct fake_desk {
    unsigned snaps, snaps_freed;
    char launched[32], ran[80];
    unsigned nlaunch, nrun, nterminal;
    uint32_t answered_id, answered_button;
    unsigned nanswered;
};
extern struct fake_desk fdesk;
