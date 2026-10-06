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
void fk_close_all(void);
struct comp_window *win(unsigned i);
/* A press at (x, y) with button: did the window manager (or the desktop) take it? */
bool press_btn(int32_t x, int32_t y, uint32_t button);
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
    unsigned nlaunch, nrun;
    uint32_t answered_id, answered_button;
    unsigned nanswered;
};
extern struct fake_desk fdesk;
