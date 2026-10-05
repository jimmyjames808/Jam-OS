/* utest: the console's window mode, its pure parts
 * (user/services/console/wlinput.c, linked in): the terminal's grid on
 * outputs of several sizes, a window's size as a grid, every key the
 * layout has from wl_keyboard back to the HID usage and modifiers the
 * console's focus stack takes, Super+Enter, and the pointer as mouse
 * reports (relative pixels, buttons held, the wheel's direction). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <keymap.h>
#include <os.h>
#include "console.h"
#include "utest.h"

bool t_conwin_grid(void)
{
    static const struct { int32_t w, h; uint32_t cols, rows; } want[] = {
        { 2560, 1440, 160, 50 },   /* the PC: 1280x800 pixels, centred */
        { 1920, 1080, 160, 50 },
        { 1280, 800, 120, 37 },    /* QEMU: three quarters */
        { 1024, 768, 96, 36 },
        { 800, 600, 80, 28 },      /* never under 80 columns if they fit */
        { 600, 300, 75, 18 },      /* a small output: all of it */
        { 0, 0, WIN_COLS, WIN_ROWS },   /* not told yet */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        uint32_t c = 0, r = 0;
        term_grid(want[i].w, want[i].h, &c, &r);
        if (c != want[i].cols || r != want[i].rows)
            FAIL("%dx%d: %ux%u cells, want %ux%u", want[i].w, want[i].h, c, r, want[i].cols,
                 want[i].rows);
        CHECK(want[i].w == 0 || c * GW <= (uint32_t)want[i].w);
        CHECK(want[i].h == 0 || r * GH <= (uint32_t)want[i].h);
    }
    uint32_t c, r;
    grid_of_size(1283, 805, &c, &r);   /* the margins are not cells */
    CHECK(c == 160 && r == 50);
    grid_of_size(3, 5, &c, &r);
    CHECK(c == 1 && r == 1);
    grid_of_size(100000, 100000, &c, &r);
    CHECK(c == MAX_COLS && r == MAX_ROWS);
    grid_of_size(-5, 0, &c, &r);
    CHECK(c == 1 && r == 1);
    return true;
}

bool t_conwin_keys(void)
{
    unsigned keys = 0;
    for (uint32_t usage = 1; usage < KEYMAP_HID_USAGES; usage++) {
        uint32_t code = keymap_evdev_of_hid(usage);
        if (!code || keymap_hid_of_evdev(code) != usage)
            continue;   /* no code, or a second usage for one code */
        struct input_key_event ev;
        if (!key_of_wayland(code, JWL_KEY_PRESSED, 0, 'x', &ev))
            FAIL("usage %#x (evdev %u) refused", usage, code);
        CHECK(ev.usage == usage && ev.state == INPUT_KEY_DOWN && ev.mods == 0);
        CHECK(ev.codepoint == 'x');
        keys++;
    }
    CHECK(keys > 100);
    struct input_key_event ev;
    uint32_t c = keymap_evdev_of_hid(0x06);   /* c */
    CHECK(key_of_wayland(c, JWL_KEY_RELEASED, KEYMAP_MOD_CTRL | KEYMAP_MOD_SHIFT, 0, &ev));
    CHECK(ev.state == INPUT_KEY_UP && ev.mods == (INPUT_MOD_LCTRL | INPUT_MOD_LSHIFT));
    CHECK(key_of_wayland(c, JWL_KEY_REPEATED, KEYMAP_MOD_ALT | KEYMAP_MOD_SUPER | KEYMAP_MOD_CAPS |
                                                  KEYMAP_MOD_NUM, 'c', &ev));
    CHECK(ev.state == INPUT_KEY_REPEAT && ev.mods == (INPUT_MOD_LALT | INPUT_MOD_LGUI));
    CHECK(!key_of_wayland(c, 7, 0, 0, &ev));        /* no such state */
    CHECK(!key_of_wayland(0, JWL_KEY_PRESSED, 0, 0, &ev));
    CHECK(!key_of_wayland(100000, JWL_KEY_PRESSED, 0, 0, &ev));
    /* Super+Enter asks for a terminal; Enter alone, or released, doesn't. */
    uint32_t enter = keymap_evdev_of_hid(0x28);
    CHECK(key_of_wayland(enter, JWL_KEY_PRESSED, KEYMAP_MOD_SUPER, '\n', &ev) && asks_terminal(&ev));
    CHECK(key_of_wayland(enter, JWL_KEY_PRESSED, 0, '\n', &ev) && !asks_terminal(&ev));
    CHECK(key_of_wayland(enter, JWL_KEY_RELEASED, KEYMAP_MOD_SUPER, 0, &ev) && !asks_terminal(&ev));
    return true;
}

static struct jwl_event pointer_ev(uint32_t type, int32_t x, int32_t y)
{
    struct jwl_event e = { .type = type };
    e.pointer.x = x * 256;
    e.pointer.y = y * 256;
    return e;
}

bool t_conwin_pointer(void)
{
    struct pointer_track p = { 0 };
    struct input_mouse_event m;
    struct jwl_event e = pointer_ev(JWL_EV_POINTER_ENTER, 100, 50);
    CHECK(!mouse_of_wayland(&p, &e, &m));   /* coming in is no movement */
    e = pointer_ev(JWL_EV_POINTER_MOTION, 103, 46);
    CHECK(mouse_of_wayland(&p, &e, &m));
    CHECK(m.kind == INPUT_EVENT_MOUSE && m.dx == 3 && m.dy == -4 && !m.buttons && !m.wheel);
    CHECK(!m.reserved);
    e = pointer_ev(JWL_EV_POINTER_MOTION, 103, 46);
    CHECK(!mouse_of_wayland(&p, &e, &m));   /* no movement, no report */
    e = (struct jwl_event){ .type = JWL_EV_POINTER_BUTTON };
    e.button.button = 0x110;
    e.button.pressed = true;
    CHECK(mouse_of_wayland(&p, &e, &m) && m.buttons == INPUT_BTN_LEFT && !m.dx);
    e.button.button = 0x111;
    CHECK(mouse_of_wayland(&p, &e, &m) && m.buttons == (INPUT_BTN_LEFT | INPUT_BTN_RIGHT));
    e.button.button = 0x110;
    e.button.pressed = false;
    CHECK(mouse_of_wayland(&p, &e, &m) && m.buttons == INPUT_BTN_RIGHT);
    e.button.button = 0x113;   /* a side button: none of the boot report's */
    CHECK(!mouse_of_wayland(&p, &e, &m));
    e = (struct jwl_event){ .type = JWL_EV_POINTER_AXIS };
    e.axis.axis = JWL_WL_POINTER_AXIS_VERTICAL_SCROLL;
    e.axis.value = 10 * 256;
    e.axis.discrete = 1;   /* a notch towards the user */
    CHECK(mouse_of_wayland(&p, &e, &m) && m.wheel == -1 && m.buttons == INPUT_BTN_RIGHT);
    e.axis.value = -30 * 256;
    e.axis.discrete = 0;   /* no notches told: 10 units a notch */
    CHECK(mouse_of_wayland(&p, &e, &m) && m.wheel == 3);
    e.axis.value = 0;
    CHECK(!mouse_of_wayland(&p, &e, &m));
    e.axis.axis = JWL_WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    e.axis.value = 256;
    CHECK(!mouse_of_wayland(&p, &e, &m));
    e = pointer_ev(JWL_EV_POINTER_LEAVE, 0, 0);
    CHECK(!mouse_of_wayland(&p, &e, &m) && p.buttons == 0);
    e = pointer_ev(JWL_EV_POINTER_MOTION, 40000, -40000);   /* clamped, not wrapped */
    p.x = p.y = 0;
    CHECK(mouse_of_wayland(&p, &e, &m) && m.dx == INT16_MAX && m.dy == INT16_MIN);
    return true;
}
