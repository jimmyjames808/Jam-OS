/* utest's tests of the compositor's seat (compseat.c, compinput.c): a
 * compositor run headless with the test power `testwin` (a client's
 * surface becomes a window where the test puts it), a fake input source on
 * its compctl channel, and test clients with a seat, a keyboard and a
 * pointer. What the two files share. */
#pragma once

#include <jwl.h>
#include "comptest.h"

#define OUT_W 640
#define OUT_H 480

/* evdev codes (Linux's input-event-codes.h, the numbers Wayland sends) */
#define KEY_ENTER     28
#define KEY_LEFTCTRL  29
#define KEY_A         30
#define KEY_LEFTSHIFT 42
#define KEY_X         45
#define KEY_C         46
#define KEY_B         48
#define KEY_LEFTALT   56
#define KEY_UP        103
#define KEY_DELETE    111
#define KEY_LEFTMETA  125
#define KEY_TAB       15
#define KEY_F         33
#define KEY_T         20
#define BTN_LEFT      0x110

/* HID usages (the input protocol's) */
#define U_A      0x04
#define U_B      0x05
#define U_C      0x06
#define U_F      0x09
#define U_T      0x17
#define U_X      0x1b
#define U_TAB    0x2b
#define U_CAPS   0x39
#define U_DELETE 0x4c
#define U_LCTRL  0xe0
#define U_LALT   0xe2
#define U_LGUI   0xe3

/* A key event as keys_got reports it: the evdev code, pressed or not. */
#define DOWN(code) ((uint32_t)(code) << 1 | 1)
#define UP(code)   ((uint32_t)(code) << 1)
/* A pixel's position in a surface as wl_fixed: the pointer sits in the
 * middle of its pixel. */
#define AT(px) ((uint32_t)(px) * 256 + 128)

/* A compositor with its compctl channel, init's end and one input source. */
struct cs {
    struct ct_comp p;
    handle_t ctl;          /* compctl, ADMIN */
    handle_t init;         /* our end of the compositor's initctl channel */
    handle_t src;          /* an input source (keyboard and mouse) */
    int32_t x, y;          /* where the pointer is */
    uint8_t buttons;       /* held */
};

/* A client with a seat, a keyboard and a pointer. */
struct sc {
    struct ct_client k;
    uint32_t seat, kb, ptr;
};

/* The compositor (headless, 640x480, testwin, nodesk) with compctl, init's end and
 * a source that said it is a keyboard and a mouse; cs_stop: all of it gone
 * and the compositor's job empty (ct_stop). */
bool cs_start(struct cs *t);
bool cs_stop(struct cs *t);
/* A client, every global bound, a seat 5 with a keyboard and a pointer. */
bool cs_client(struct cs *t, struct sc *c);
/* A w x h window at (x, y) (testwin): its surface's id, 0 on failure. */
uint32_t cs_window(struct sc *c, int32_t x, int32_t y, int32_t w, int32_t h);
/* The fake source: a key (usage, INPUT_KEY_*, modifier byte), a key
 * pressed and released, a mouse report (the buttons held: t->buttons),
 * the left button, the pointer to (x, y) in 1:1 steps. */
bool cs_key(struct cs *t, uint16_t usage, uint8_t state, uint8_t mods);
bool cs_tap(struct cs *t, uint16_t usage, uint8_t mods);
bool cs_mouse(struct cs *t, int16_t dx, int16_t dy, int8_t wheel);
bool cs_button(struct cs *t, bool down);
bool cs_pointer_to(struct cs *t, int32_t x, int32_t y);
/* A round trip on a (and b, if not NULL): every event so far is in. */
bool cs_sync(struct sc *a, struct sc *b);
/* The key events k got since ct_clear (DOWN/UP words), at most max. */
unsigned keys_got(const struct ct_client *k, uint32_t *out, unsigned max);
bool no_keys(const struct ct_client *k);
bool keys_are(const struct ct_client *k, const uint32_t *want, unsigned n);
/* The first event (iface, op) c got since ct_clear, or NULL. */
const struct ct_event *find_ev(const struct sc *c, const struct jwl_interface *iface, uint16_t op);
/* c's last wl_pointer.motion was to (x, y), wl_fixed. */
bool motion_at(const struct sc *c, uint32_t x, uint32_t y);
