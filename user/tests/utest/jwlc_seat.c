/* utest: libjwl's client side, the seat, against the fake compositor
 * (jwlcfake.h):
 *   jwlc_keyboard  the keymap VMO named by its first line (US), keys
 *                  decoded to characters with the modifiers last sent,
 *                  the focus, key repeat made by the library from
 *                  repeat_info (and stopped by the release), a keymap
 *                  naming no layout of ours (US instead);
 *   jwlc_pointer   enter, motion (two queued become one), buttons, the
 *                  wheel with its notches folded in, and a move asked
 *                  with the press's serial. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl_client.h>
#include <keymap.h>
#include <os.h>
#include "jwlcfake.h"
#include "utest.h"

#define KEY_A 30u   /* evdev */
#define KEY_1 2u
#define BTN_LEFT 0x110u

/* The next event, which must be of type. */
static bool expect(struct jwl_client *c, uint32_t type, struct jwl_event *ev)
{
    CHECK_ST(jwl_client_next_event(c, ev), OK);
    if (ev->type != type)
        FAIL("event %u, want %u", ev->type, type);
    return true;
}

static bool expect_key(struct jwl_client *c, uint32_t state, uint32_t cp, uint32_t mods)
{
    struct jwl_event ev;
    CHECK(expect(c, JWL_EV_KEY, &ev));
    CHECK(ev.key.code == KEY_A && ev.key.state == state);
    CHECK_EQ(ev.key.cp, cp);
    CHECK_EQ(ev.key.mods, mods);
    return true;
}

bool t_jwlc_keyboard(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    f.repeat_rate = 1000;   /* a repeat every ms after 20 ms */
    f.repeat_delay = 20;
    struct jwl_client *c = fake_ready(&f);
    CHECK(c);
    CHECK(jwl_client_info(c)->keymap == &keymap_us);
    struct jwl_window_config wc = { .width = 20, .height = 20 };
    struct jwl_window *w;
    struct fake_surface *s;
    CHECK(fake_window(&f, c, &wc, 0, &w, &s));

    CHECK_ST(fake_kb_enter(&f, s), OK);
    CHECK_ST(fake_key(&f, KEY_A, true), OK);
    CHECK_ST(fake_key(&f, KEY_A, false), OK);
    CHECK_ST(fake_mods(&f, KEYMAP_MOD_SHIFT, 0), OK);
    CHECK_ST(fake_key(&f, KEY_A, true), OK);
    uint64_t pressed = now();
    CHECK(fake_pump(&f, c));
    struct jwl_event ev;
    CHECK(expect(c, JWL_EV_KEYBOARD_ENTER, &ev) && ev.win == w);
    CHECK(expect_key(c, JWL_KEY_PRESSED, 'a', 0));
    CHECK(expect_key(c, JWL_KEY_RELEASED, 'a', 0));
    CHECK(expect(c, JWL_EV_MODIFIERS, &ev) && ev.key.mods == KEYMAP_MOD_SHIFT);
    CHECK(expect_key(c, JWL_KEY_PRESSED, 'A', KEYMAP_MOD_SHIFT));
    CHECK_EQ(ev.win, w);

    /* held: the library repeats it once the delay has passed */
    uint64_t d = jwl_client_deadline(c);
    CHECK(d != DEADLINE_NEVER && d >= pressed);
    (void)jam_nanosleep(d + 3 * NS_PER_MS);
    CHECK_ST(jwl_client_dispatch(c), OK);
    CHECK(expect_key(c, JWL_KEY_REPEATED, 'A', KEYMAP_MOD_SHIFT));
    CHECK_ST(jwl_client_next_event(c, &ev), ERR_SHOULD_WAIT);   /* one a dispatch, no burst */
    CHECK_ST(fake_key(&f, KEY_A, false), OK);
    CHECK(fake_pump(&f, c));
    bool released = false;   /* repeats may come first: the pump takes its time */
    while (!released && fake_next_of(c, JWL_EV_KEY, &ev))
        released = ev.key.state == JWL_KEY_RELEASED;
    CHECK(released);
    CHECK_EQ(jwl_client_deadline(c), DEADLINE_NEVER);
    (void)jam_nanosleep(now() + 30 * NS_PER_MS);
    CHECK_ST(jwl_client_dispatch(c), OK);
    CHECK_ST(jwl_client_next_event(c, &ev), ERR_SHOULD_WAIT);
    CHECK_ST(fake_key(&f, KEY_1, true), OK);   /* Shift is still down */
    CHECK(fake_pump(&f, c));
    CHECK(fake_next_of(c, JWL_EV_KEY, &ev) && ev.key.cp == '!');
    CHECK(fake_all_gone(&f, c, h0, b0));

    /* a keymap that names no layout of ours: decoded as US */
    fake_init(&f);
    f.keymap_text = "xkb_keymap { };\n";
    c = fake_ready(&f);
    CHECK(c && jwl_client_info(c)->keymap == &keymap_us);
    return fake_all_gone(&f, c, h0, b0);
}

bool t_jwlc_pointer(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct jwl_client *c = fake_ready(&f);
    CHECK(c);
    struct jwl_window_config wc = { .width = 40, .height = 30 };
    struct jwl_window *w;
    struct fake_surface *s;
    CHECK(fake_window(&f, c, &wc, 0, &w, &s));
    CHECK_ST(jwl_window_move(w), ERR_BAD_STATE);   /* no press yet */

    CHECK_ST(fake_ptr_enter(&f, s, 10, 20), OK);
    CHECK_ST(fake_motion(&f, 11, 21), OK);
    CHECK_ST(fake_motion(&f, 12, 22), OK);
    uint32_t press = f.serial;
    CHECK_ST(fake_button(&f, BTN_LEFT, true), OK);
    CHECK_ST(fake_button(&f, BTN_LEFT, false), OK);
    CHECK_ST(fake_wheel(&f, -1), OK);
    CHECK(fake_pump(&f, c));
    struct jwl_event ev;
    CHECK(expect(c, JWL_EV_POINTER_ENTER, &ev) && ev.win == w);
    CHECK(ev.pointer.x == 10 * 256 && ev.pointer.y == 20 * 256);
    CHECK(expect(c, JWL_EV_POINTER_MOTION, &ev) && ev.pointer.x == 12 * 256);
    CHECK(expect(c, JWL_EV_POINTER_BUTTON, &ev) && ev.button.pressed);
    CHECK(ev.button.button == BTN_LEFT && ev.win == w);
    CHECK(expect(c, JWL_EV_POINTER_BUTTON, &ev) && !ev.button.pressed);
    CHECK(expect(c, JWL_EV_POINTER_AXIS, &ev) && ev.axis.axis == 0);
    CHECK(ev.axis.discrete == -1 && ev.axis.value == -10 * 256);
    CHECK_ST(jwl_client_next_event(c, &ev), ERR_SHOULD_WAIT);

    CHECK_ST(jwl_window_move(w), OK);
    CHECK(fake_pump(&f, c));
    CHECK_EQ(s->move_serial, press);
    return fake_all_gone(&f, c, h0, b0);
}
