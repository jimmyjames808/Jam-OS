/* console: the window mode's pure parts (console.h): how big a terminal
 * window is on an output, and Wayland's keys and pointer as the console's
 * own input events (<jam/abi.h>, "input"), so a key typed into a window
 * reaches the focus stack exactly as one from a keyboard driver does.
 *
 * No state of the console's and no system calls: utest checks them
 * (user/tests/utest/conwin.c). */
#include <keymap.h>
#include "console.h"

void term_grid(int32_t ow, int32_t oh, uint32_t *out_cols, uint32_t *out_rows)
{
    if (ow <= 0 || oh <= 0) {   /* not told yet */
        *out_cols = WIN_COLS;
        *out_rows = WIN_ROWS;
        return;
    }
    /* Three quarters of the output at most, so the desktop shows around
     * it; never under the classic 80x24 unless the output is smaller. */
    uint32_t c = (uint32_t)ow / 4 * 3 / GW, r = (uint32_t)oh / 4 * 3 / GH;
    uint32_t maxc = (uint32_t)ow / GW, maxr = (uint32_t)oh / GH;
    c = c > WIN_COLS ? WIN_COLS : c < WIN_MIN_COLS ? WIN_MIN_COLS : c;
    r = r > WIN_ROWS ? WIN_ROWS : r < WIN_MIN_ROWS ? WIN_MIN_ROWS : r;
    c = c > maxc ? maxc : c;
    r = r > maxr ? maxr : r;
    *out_cols = c ? c : 1;
    *out_rows = r ? r : 1;
}

void grid_of_size(int32_t w, int32_t h, uint32_t *out_cols, uint32_t *out_rows)
{
    uint32_t c = w > 0 ? (uint32_t)w / GW : 0, r = h > 0 ? (uint32_t)h / GH : 0;
    *out_cols = c < 1 ? 1 : c > MAX_COLS ? MAX_COLS : c;
    *out_rows = r < 1 ? 1 : r > MAX_ROWS ? MAX_ROWS : r;
}

/* KEYMAP_MOD_* (XKB's modifiers, as libjwl keeps them) as the HID
 * modifier byte the console's keys carry: the left keys, since Wayland
 * doesn't say which side. The locks have no bit there. */
static uint8_t hid_mods(uint32_t mods)
{
    uint8_t m = 0;
    if (mods & KEYMAP_MOD_SHIFT)
        m |= INPUT_MOD_LSHIFT;
    if (mods & KEYMAP_MOD_CTRL)
        m |= INPUT_MOD_LCTRL;
    if (mods & KEYMAP_MOD_ALT)
        m |= INPUT_MOD_LALT;
    if (mods & KEYMAP_MOD_SUPER)
        m |= INPUT_MOD_LGUI;
    return m;
}

bool key_of_wayland(uint32_t code, uint32_t state, uint32_t mods, uint32_t cp,
                    struct input_key_event *out)
{
    uint32_t usage = keymap_hid_of_evdev(code);
    if (!usage || usage > 0xffff)
        return false;
    uint8_t s = state == JWL_KEY_PRESSED    ? INPUT_KEY_DOWN
              : state == JWL_KEY_REPEATED   ? INPUT_KEY_REPEAT
              : state == JWL_KEY_RELEASED   ? INPUT_KEY_UP
                                            : 0xff;
    if (s == 0xff)
        return false;
    *out = (struct input_key_event){ (uint16_t)usage, s, hid_mods(mods), cp };
    return true;
}

bool asks_terminal(const struct input_key_event *ev)
{
    return ev->state == INPUT_KEY_DOWN && (ev->mods & (INPUT_MOD_LGUI | INPUT_MOD_RGUI)) &&
           (ev->usage == 0x28 || ev->usage == 0x58);   /* Enter, keypad Enter */
}

/* An evdev button (BTN_LEFT 0x110 ...) as the boot report's button bit. */
static uint8_t button_bit(uint32_t button)
{
    return button == 0x110 ? INPUT_BTN_LEFT : button == 0x111 ? INPUT_BTN_RIGHT
         : button == 0x112 ? INPUT_BTN_MIDDLE : 0;
}

static int16_t clamp16(int32_t v)
{
    return (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
}

bool mouse_of_wayland(struct pointer_track *p, const struct jwl_event *ev,
                      struct input_mouse_event *out)
{
    *out = (struct input_mouse_event){ .kind = INPUT_EVENT_MOUSE };
    switch (ev->type) {
    case JWL_EV_POINTER_ENTER:
        p->x = ev->pointer.x;   /* where it came in: no movement of its own */
        p->y = ev->pointer.y;
        return false;
    case JWL_EV_POINTER_LEAVE:
        p->buttons = 0;   /* a press held out of the window is no longer ours to report */
        return false;
    case JWL_EV_POINTER_MOTION: {
        /* Whole pixels (fixed 24.8 to 0), each report's own share. */
        int32_t dx = (ev->pointer.x >> 8) - (p->x >> 8), dy = (ev->pointer.y >> 8) - (p->y >> 8);
        p->x = ev->pointer.x;
        p->y = ev->pointer.y;
        if (!dx && !dy)
            return false;
        out->dx = clamp16(dx);
        out->dy = clamp16(dy);
        break;
    }
    case JWL_EV_POINTER_BUTTON: {
        uint8_t b = button_bit(ev->button.button);
        if (!b)
            return false;
        p->buttons = ev->button.pressed ? (uint8_t)(p->buttons | b) : (uint8_t)(p->buttons & ~b);
        break;
    }
    case JWL_EV_POINTER_AXIS: {
        if (ev->axis.axis != JWL_WL_POINTER_AXIS_VERTICAL_SCROLL)
            return false;
        /* Wayland's + scrolls down (towards the user); the boot report's
         * wheel + turns away from the user. Notches if they came, else a
         * notch per 10 units, as compositors send for a wheel. */
        if (!ev->axis.discrete && !ev->axis.value)
            return false;
        int32_t n = ev->axis.discrete ? ev->axis.discrete : (ev->axis.value >> 8) / 10;
        if (!n)
            n = ev->axis.value > 0 ? 1 : -1;
        out->wheel = (int8_t)(n > 0 ? (n > 127 ? -127 : -n) : (n < -127 ? 127 : -n));
        break;
    }
    default:
        return false;
    }
    out->buttons = p->buttons;
    return true;
}
