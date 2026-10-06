/* utest: the console's window mode, its pure parts
 * (user/services/console/wlinput.c and view.c, linked in): the terminal's
 * grid on outputs of several sizes in both fonts' cells, a window's size
 * as a grid inside its padding, the view (text from the top in a window,
 * scrolling once full, a clear, resizes; the full screen's bottom row),
 * terminal.font's values, every key the
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

/* The two looks' cell sizes (conpaint.c checks the smooth one's numbers
 * come from JetBrains Mono at TERM_PX). */
static const struct cell_look bitmap_look = { GW, GH, 0, NULL, NULL };
static const struct cell_look smooth_look = { 9, 21, 16, NULL, NULL };

bool t_conwin_grid(void)
{
    /* Three quarters of the output, less the padding, in cells; capped
     * at WIN_COLS x WIN_ROWS, at least 80x24 when the output has room. */
    static const struct { bool smooth; int32_t w, h; uint32_t cols, rows; } want[] = {
        { true, 2560, 1440, 160, 50 },   /* the PC: a 1460x1070 window, centred */
        { true, 1920, 1080, 157, 37 },
        { true, 1280, 800, 104, 27 },    /* QEMU: a 956x587 window */
        { true, 1024, 768, 83, 26 },
        { true, 800, 600, 80, 24 },      /* never under 80x24 if they fit */
        { true, 600, 300, 64, 13 },      /* a small output: all of it */
        { true, 0, 0, WIN_COLS, WIN_ROWS },   /* not told yet */
        { false, 2560, 1440, 160, 50 },  /* terminal.font = bitmap */
        { false, 1920, 1080, 160, 49 },
        { false, 1280, 800, 117, 36 },
        { false, 1024, 768, 93, 34 },
        { false, 800, 600, 80, 26 },
        { false, 600, 300, 72, 17 },
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const struct cell_look *l = want[i].smooth ? &smooth_look : &bitmap_look;
        uint32_t c = 0, r = 0;
        term_grid(l, want[i].w, want[i].h, &c, &r);
        if (c != want[i].cols || r != want[i].rows)
            FAIL("%s %dx%d: %ux%u cells, want %ux%u", want[i].smooth ? "smooth" : "bitmap",
                 want[i].w, want[i].h, c, r, want[i].cols, want[i].rows);
        int32_t ww, wh;
        window_size(l, c, r, &ww, &wh);
        CHECK(ww == (int32_t)c * l->w + 2 * WIN_PAD && wh == (int32_t)r * l->h + 2 * WIN_PAD);
        CHECK(want[i].w == 0 || ww <= want[i].w);
        CHECK(want[i].h == 0 || wh <= want[i].h);
        /* The window it asks for holds that grid exactly. */
        uint32_t c2, r2;
        grid_of_size(l, ww, wh, &c2, &r2);
        CHECK(c2 == c && r2 == r);
    }
    return true;
}

bool t_conwin_padding(void)
{
    uint32_t c, r;
    /* The padding is not cells: 2 x WIN_PAD less each way. */
    grid_of_size(&smooth_look, 104 * 9 + 2 * WIN_PAD, 27 * 21 + 2 * WIN_PAD, &c, &r);
    CHECK(c == 104 && r == 27);
    grid_of_size(&smooth_look, 104 * 9 + 2 * WIN_PAD - 1, 27 * 21 + 2 * WIN_PAD - 1, &c, &r);
    CHECK(c == 103 && r == 26);
    grid_of_size(&smooth_look, 104 * 9 + 2 * WIN_PAD + 8, 27 * 21 + 2 * WIN_PAD + 20, &c, &r);
    CHECK(c == 104 && r == 27);   /* what is past the last whole cell is margin */
    grid_of_size(&bitmap_look, 160 * GW + 2 * WIN_PAD + 3, 50 * GH + 2 * WIN_PAD + 5, &c, &r);
    CHECK(c == 160 && r == 50);
    /* The tiling's and maximised windows: any size the compositor gives. */
    grid_of_size(&smooth_look, 1280, 726, &c, &r);
    CHECK(c == (1280 - 20) / 9 && r == (726 - 20) / 21);
    grid_of_size(&smooth_look, 2560, 1412, &c, &r);   /* a 2560x1440 output maximised */
    CHECK(c == 282 && r == 66 && c <= MAX_COLS && r <= MAX_ROWS);
    grid_of_size(&smooth_look, 3, 5, &c, &r);
    CHECK(c == 1 && r == 1);
    grid_of_size(&smooth_look, 2 * WIN_PAD, 2 * WIN_PAD, &c, &r);
    CHECK(c == 1 && r == 1);
    grid_of_size(&smooth_look, 100000, 100000, &c, &r);
    CHECK(c == MAX_COLS && r == MAX_ROWS);
    grid_of_size(&bitmap_look, -5, 0, &c, &r);
    CHECK(c == 1 && r == 1);
    /* The compositor's rounded corners (LOOK_RADIUS 10, inside a 1-pixel
     * outline) cut only pixels within 9 of the surface's corner: the
     * padding, never a cell. */
    CHECK(WIN_PAD >= 10 - 1);
    return true;
}

/* The row the current line is on, -1 if off the screen. */
static int64_t cursor_row(const struct view *v, uint32_t back)
{
    int64_t row = (int64_t)v->committed - (view_base(v) - back);
    return row >= 0 && row < v->rows ? row : -1;
}

bool t_conwin_view(void)
{
    /* A window: the text from the top, filling down; scrolling only once
     * the current line reaches the bottom row. */
    struct view v = { 0, 0, 10, true };
    CHECK(view_base(&v) == 0 && cursor_row(&v, 0) == 0 && view_back_max(&v) == 0);
    for (uint64_t n = 0; n < 10; n++) {
        v.committed = n;
        CHECK(view_base(&v) == 0 && cursor_row(&v, 0) == (int64_t)n);
        CHECK(view_back_max(&v) == 0);   /* nothing above the screen yet */
    }
    v.committed = 10;   /* the first line scrolls off */
    CHECK(view_base(&v) == 1 && cursor_row(&v, 0) == 9 && view_back_max(&v) == 1);
    v.committed = 30;
    CHECK(view_base(&v) == 21 && cursor_row(&v, 0) == 9 && view_back_max(&v) == 21);
    CHECK(cursor_row(&v, 1) == -1 && cursor_row(&v, 21) == -1);
    /* A clear: the screen starts again at the current line, on the top
     * row; the lines before it are still there to scroll back to. */
    v.top = 30;
    CHECK(view_base(&v) == 30 && cursor_row(&v, 0) == 0 && view_back_max(&v) == 30);
    v.committed = 35;
    CHECK(cursor_row(&v, 0) == 5 && cursor_row(&v, 3) == 8 && cursor_row(&v, 4) == 9);
    CHECK(cursor_row(&v, 5) == -1);   /* scrolled back: the current line goes down, then off */
    /* Resizes keep the current line on the screen. */
    for (uint32_t rows = 1; rows < 60; rows++) {
        v.rows = rows;
        CHECK(cursor_row(&v, 0) >= 0);
        CHECK(cursor_row(&v, 0) == (rows > 5 ? 5 : (int64_t)rows - 1));
    }
    v = (struct view){ 30, 0, 40, true };   /* bigger than the text: all of it, from the top */
    CHECK(view_base(&v) == 0 && cursor_row(&v, 0) == 30);
    /* The scrollback keeps SCROLLBACK lines: no further back than those. */
    v = (struct view){ 10000, 0, 10, true };
    CHECK(view_back_max(&v) == (uint32_t)(10000 - 9 - (10000 - SCROLLBACK)));
    /* The full-screen console (nocomp): the current line on the bottom row,
     * as it always was, the rows above it blank at the start. */
    v = (struct view){ 0, 0, 10, false };
    CHECK(view_base(&v) == -9 && cursor_row(&v, 0) == 9 && view_back_max(&v) == 0);
    v.committed = 30;
    CHECK(view_base(&v) == 21 && cursor_row(&v, 0) == 9 && view_back_max(&v) == 21);
    v.top = 30;   /* top means nothing there */
    CHECK(view_base(&v) == 21);
    return true;
}

bool t_conwin_font_setting(void)
{
    CHECK(term_font_parse("smooth") == 0);
    CHECK(term_font_parse("bitmap") == 1);
    CHECK(term_font_parse("Bitmap") == -1 && term_font_parse("") == -1);
    CHECK(term_font_parse("smooth ") == -1 && term_font_parse("8x16") == -1);
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
