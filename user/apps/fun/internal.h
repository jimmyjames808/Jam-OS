/* libfun: what its own files share and the apps don't see (fun.h is the
 * library's public side). */
#pragma once

#include <font.h>
#include "fun.h"

/* A present goes by bands of PBAND rows, each row in pieces of PSEG
 * pixels: a piece that differs from what is shown is written. */
#define PBAND 16
#define PSEG  64

/* gfx.c: present rows y0 .. y1 - 1 only (what changed in them). */
void gfx_present_rows(int y0, int y1);
/* gfx.c: n pixels from s to d (a plain loop of wide stores, never rep
 * movsb: write-combining memory likes it best, and QEMU too). */
void px_copy(uint32_t *restrict d, const uint32_t *restrict s, int n);
/* Whether n pixels are the same. */
static inline bool px_same(const uint32_t *a, const uint32_t *b, int n)
{
    const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)b;
    uint64_t diff = 0;
    for (int i = 0; i < n / 2; i++)
        diff |= x[i] ^ y[i];
    if (n & 1)
        diff |= a[n - 1] ^ b[n - 1];
    return !diff;
}

/* What keys.c reads: from the console's key channel, or from the window
 * (wl.c). */
enum msg_kind {
    MSG_KEY,      /* a key event */
    MSG_MOVE,     /* the mouse moved (mouse.c has it) */
    MSG_BUTTON,   /* a button or the wheel changed (mouse.c has it) */
    MSG_RESIZE,   /* the window took a new size: scr is new (gfx_resizable) */
};

/* mouse.c: one mouse report from the console into the pointer and the
 * buttons. True if it is one the app must see by itself (a button or the
 * wheel changed), false for plain movement, which may be merged. */
bool mouse_report(const struct input_mouse_event *ev);
/* mouse.c: the same for a pointer whose place is known (a window's: x, y
 * in scr's pixels, clamped here), the buttons held (MOUSE_*) and the
 * wheel's notches since the last call. */
bool mouse_at(int x, int y, uint8_t buttons, int wheel);
/* mouse.c: the app asked for the mouse (gfx_mouse_open). */
bool mouse_wanted(void);
/* mouse.c: draw the arrow into the back buffer for a present, and take it
 * out again after (the app's frame never keeps it). */
void pointer_paint(void);
void pointer_unpaint(void);
/* mouse.c: gfx_close's part: no mouse, no arrow. */
void mouse_close(void);

/* wl.c: the window. wl_open: open one at the size the app asked for
 * (full: full screen; keys: the keys are the app's) and fill scr, on bg.
 * ERR_NOT_FOUND: no /svc/wayland in our namespace; ERR_NOT_SUPPORTED: the
 * compositor offers no windows; ERR_TIMED_OUT: it didn't answer; a
 * connection's or ERR_NO_MEMORY. Anything but OK leaves no window, no
 * connection and scr untouched. */
status_t wl_open(uint32_t bg, bool keys, bool full);
void     wl_close(void);
/* wl.c: the next message for keys.c before deadline (0: only what has
 * come): a key, the pointer, a new size. ERR_TIMED_OUT: none came;
 * ERR_BAD_HANDLE: the keys aren't ours (gfx_open_screen);
 * ERR_PEER_CLOSED: the compositor is gone for good. */
status_t wl_next(uint64_t deadline, struct input_key_event *ev, enum msg_kind *kind);
/* wl.c, for wlpaint.c: read the compositor's news until the frame
 * callback of the last commit came (bounded: a hidden window's come once
 * a second); and until a buffer is free or `until`. */
void     wl_wait_frame(void);
void     wl_wait_until(uint64_t until);
/* wl.c: the window's size now, and where scr's picture sits in it (its
 * top-left corner; negative when the window is the smaller). */
void     wl_place(int32_t *w, int32_t *h, int *ox, int *oy);
/* wl.c: the window and whether it is gone for good, for wlpaint.c. */
struct jwl_window *wl_window(void);
bool     wl_dead(void);
/* wl.c: the frame callback of the last commit is still to come. */
void     wl_frame_asked(void);

/* wlpaint.c: present into the window: what changed, or all of it. */
void wl_present(bool all);
/* wlpaint.c: forget what the window's buffers hold (a new size, a new
 * connection): the next present writes all of each and damages it all. */
void wl_paint_reset(void);
