/* utest: input routing in the compositor (compseat.h has the harness).
 *
 * t_comp_seat_focus: a client's first window takes the keyboard (its
 * later ones don't); keys go to the focused window only; a click moves the
 * focus; Alt+Tab moves it with the keys held listed in the enter, and Tab
 * itself goes nowhere; the focused window's client going gives the focus
 * to the next window.
 * t_comp_seat_grab: the pointer's enter, motion and leave in surface
 * coordinates; the implicit grab (a drag that leaves the window keeps
 * going to it until the button is up; one that starts on the background
 * reaches no window); motion coalesced for a client that doesn't read; the
 * wheel.
 * t_comp_seat_reserved: Ctrl+C reaches the window; Super+F, Super+T and
 * Ctrl+Alt+Del never do; Ctrl+Alt+Del asks init to reboot, keys are
 * dropped until init says it failed; the counts in compctl.stats.
 * t_comp_seat_text: a terminal's bytes typed as the key presses a keyboard
 * would send (Shift, Ctrl, arrows, Enter, Caps Lock on); two keyboards
 * holding one key; a keyboard that goes away lets go of its keys.
 * t_comp_seat_move: xdg_toplevel.move honoured only with the serial of a
 * press still held (only once the compositor offers xdg_wm_base). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/compctl.h>
#include <idl/initctl.h>
#include <idl/input.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <keymap.h>
#include <os.h>
#include "compseat.h"
#include "utest.h"

/* A's window and then B's: each takes the keys as it appears. */
static bool two_windows(struct cs *t, struct sc *a, struct sc *b, uint32_t *sa, uint32_t *sb)
{
    CHECK(cs_client(t, a));
    CHECK(cs_client(t, b));
    CHECK((*sa = cs_window(a, 50, 50, 100, 80)) != 0);
    const struct ct_event *e = ct_await(&a->k, &jwl_wl_keyboard_interface,
                                        JWL_WL_KEYBOARD_EV_ENTER, a->kb, CT_WAIT);
    CHECK(e && e->u[1] == *sa);
    CHECK((*sb = cs_window(b, 300, 50, 100, 80)) != 0);
    e = ct_await(&b->k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, b->kb, CT_WAIT);
    CHECK(e && e->u[1] == *sb);
    e = ct_await(&a->k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_LEAVE, a->kb, CT_WAIT);
    CHECK(e && e->u[1] == *sa);
    ct_clear(&a->k);
    ct_clear(&b->k);
    return true;
}

/* A click on a's window: the keyboard follows it. */
static bool click_focuses(struct cs *t, struct sc *a, struct sc *b, uint32_t sa, uint32_t sb)
{
    CHECK(cs_pointer_to(t, 100, 90));
    CHECK(cs_button(t, true));
    CHECK(cs_button(t, false));
    CHECK(cs_sync(a, b));
    const struct ct_event *e = find_ev(a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER);
    CHECK(e && e->u[1] == sa);   /* where it came in: its right edge */
    CHECK(motion_at(a, AT(50), AT(40)));
    e = find_ev(a, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER);
    CHECK(e && e->u[1] == sa);
    e = find_ev(b, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_LEAVE);
    CHECK(e && e->u[1] == sb);
    e = find_ev(a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON);
    CHECK(e && e->u[2] == BTN_LEFT && e->u[3] == JWL_WL_POINTER_BUTTON_STATE_PRESSED);
    CHECK(!find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    ct_clear(&a->k);
    ct_clear(&b->k);
    return true;
}

/* x held, then Alt+Tab: the focus to b, its enter listing x and Alt; Tab
 * goes to nobody. */
static bool alt_tab(struct cs *t, struct sc *a, struct sc *b, uint32_t sb)
{
    CHECK(cs_key(t, U_X, INPUT_KEY_DOWN, 0));
    CHECK(cs_key(t, U_LALT, INPUT_KEY_DOWN, INPUT_MOD_LALT));
    CHECK(cs_tap(t, U_TAB, INPUT_MOD_LALT));
    CHECK(cs_key(t, U_LALT, INPUT_KEY_UP, 0));
    CHECK(cs_key(t, U_X, INPUT_KEY_UP, 0));
    CHECK(cs_sync(a, b));
    const uint32_t a_keys[] = { DOWN(KEY_X), DOWN(KEY_LEFTALT) };
    CHECK(keys_are(&a->k, a_keys, 2));
    CHECK(find_ev(a, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_LEAVE));
    const struct ct_event *e = find_ev(b, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER);
    CHECK(e && e->u[1] == sb);
    CHECK(e->na == 2 && e->a[0] == KEY_X && e->a[1] == KEY_LEFTALT);
    e = find_ev(b, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_MODIFIERS);
    CHECK(e && e->u[1] == KEYMAP_MOD_ALT && e->u[3] == KEYMAP_MOD_NUM);
    const uint32_t b_keys[] = { UP(KEY_LEFTALT), UP(KEY_X) };
    CHECK(keys_are(&b->k, b_keys, 2));
    return true;
}

bool t_comp_seat_focus(void)
{
    static struct sc a, b;
    struct cs t;
    uint32_t sa, sb;
    CHECK(cs_start(&t));
    CHECK(two_windows(&t, &a, &b, &sa, &sb));
    /* keys to the focused window (b's) only */
    CHECK(cs_tap(&t, U_A, 0));
    CHECK(cs_sync(&a, &b));
    const uint32_t ab[] = { DOWN(KEY_A), UP(KEY_A) };
    CHECK(keys_are(&b.k, ab, 2));
    CHECK(no_keys(&a.k));
    ct_clear(&b.k);
    /* a click on a's window */
    CHECK(click_focuses(&t, &a, &b, sa, sb));
    CHECK(cs_tap(&t, U_B, 0));
    CHECK(cs_sync(&a, &b));
    const uint32_t bb[] = { DOWN(KEY_B), UP(KEY_B) };
    CHECK(keys_are(&a.k, bb, 2));
    CHECK(no_keys(&b.k));
    ct_clear(&a.k);
    CHECK(alt_tab(&t, &a, &b, sb));
    /* a second window of a's doesn't take the focus from b; b going gives
     * it to the window below b's (none), else the top one: a's second */
    ct_clear(&a.k);
    ct_clear(&b.k);
    uint32_t sa2 = cs_window(&a, 400, 300, 50, 50);
    CHECK(sa2);
    CHECK(cs_sync(&a, &b));
    CHECK(!find_ev(&a, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER));
    CHECK(!find_ev(&b, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_LEAVE));
    ct_close(&b.k);
    const struct ct_event *e = ct_await(&a.k, &jwl_wl_keyboard_interface,
                                        JWL_WL_KEYBOARD_EV_ENTER, a.kb, CT_WAIT);
    CHECK(e && e->u[1] == sa2);
    ct_close(&a.k);
    CHECK(cs_stop(&t));
    return true;
}

/* How many wl_pointer.motion events c got. */
static unsigned motions(const struct sc *c)
{
    unsigned n = 0;
    for (unsigned i = 0; i < c->k.nev; i++)
        n += c->k.ev[i].iface == &jwl_wl_pointer_interface &&
             c->k.ev[i].op == JWL_WL_POINTER_EV_MOTION;
    return n;
}

/* A press in a, a drag into b (42 reports): a keeps every event until the
 * release. a reads nothing meanwhile, so past the compositor's window of
 * unread batches its motion is coalesced: fewer motions than reports, the
 * newest position sent once it has read (after the first round trip's
 * answer, so a second one brings it). */
static bool drag_out(struct cs *t, struct sc *a, struct sc *b, uint32_t sb)
{
    CHECK(cs_button(t, true));
    CHECK(cs_pointer_to(t, 350, 90));
    CHECK(cs_sync(a, b));
    CHECK(cs_sync(a, b));
    CHECK(!find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER));
    CHECK(!find_ev(a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_LEAVE));
    CHECK(motion_at(a, AT(300), AT(40)));   /* outside a */
    if (motions(a) >= 42)
        FAIL("%u motions for 42 reports: not coalesced", motions(a));
    ct_clear(&a->k);
    CHECK(cs_button(t, false));
    CHECK(cs_sync(a, b));
    const struct ct_event *e = find_ev(a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON);
    CHECK(e && e->u[3] == JWL_WL_POINTER_BUTTON_STATE_RELEASED);
    CHECK(find_ev(a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_LEAVE));
    e = find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER);
    CHECK(e && e->u[1] == sb && e->u[2] == AT(50) && e->u[3] == AT(40));
    CHECK(!find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    return true;
}

/* A press on the background, a drag onto a's window: nobody gets the
 * pointer until the release, then a does. */
static bool drag_in(struct cs *t, struct sc *a, struct sc *b, uint32_t sa)
{
    CHECK(cs_pointer_to(t, 200, 300));
    CHECK(cs_sync(a, b));
    ct_clear(&a->k);
    ct_clear(&b->k);
    CHECK(cs_button(t, true));
    CHECK(cs_pointer_to(t, 100, 90));
    CHECK(cs_sync(a, b));
    CHECK(!find_ev(a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER));
    CHECK(cs_button(t, false));
    CHECK(cs_sync(a, b));
    const struct ct_event *e = find_ev(a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER);
    CHECK(e && e->u[1] == sa && e->u[2] == AT(50) && e->u[3] == AT(40));
    CHECK(!find_ev(a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    CHECK(!find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    return true;
}

bool t_comp_seat_grab(void)
{
    static struct sc a, b;
    struct cs t;
    uint32_t sa, sb;
    CHECK(cs_start(&t));
    CHECK(two_windows(&t, &a, &b, &sa, &sb));
    CHECK(cs_pointer_to(&t, 100, 90));
    CHECK(cs_sync(&a, &b));
    const struct ct_event *e = find_ev(&a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER);
    CHECK(e && e->u[1] == sa);
    CHECK(find_ev(&a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_FRAME));
    ct_clear(&a.k);
    CHECK(drag_out(&t, &a, &b, sb));
    /* off every window: b's leave; nobody learns where it is */
    ct_clear(&a.k);
    ct_clear(&b.k);
    CHECK(cs_pointer_to(&t, 350, 300));
    CHECK(cs_sync(&a, &b));
    CHECK(find_ev(&b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_LEAVE));
    CHECK(!find_ev(&a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_MOTION));
    ct_clear(&b.k);
    /* the wheel, one notch away from the user: up, -10 */
    CHECK(cs_pointer_to(&t, 350, 90));
    CHECK(cs_sync(&a, &b));
    ct_clear(&b.k);
    CHECK(cs_mouse(&t, 0, 0, 1));
    CHECK(cs_sync(&a, &b));
    e = find_ev(&b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_AXIS_SOURCE);
    CHECK(e && e->u[0] == JWL_WL_POINTER_AXIS_SOURCE_WHEEL);
    e = find_ev(&b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_AXIS_DISCRETE);
    CHECK(e && e->u[0] == JWL_WL_POINTER_AXIS_VERTICAL_SCROLL && (int32_t)e->u[1] == -1);
    e = find_ev(&b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_AXIS);
    CHECK(e && e->u[1] == JWL_WL_POINTER_AXIS_VERTICAL_SCROLL && (int32_t)e->u[2] == -10 * 256);
    CHECK(!find_ev(&a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_AXIS));
    CHECK(drag_in(&t, &a, &b, sa));
    ct_close(&a.k);
    ct_close(&b.k);
    CHECK(cs_stop(&t));
    return true;
}

/* Ctrl+Alt+Del: init is asked; keys are dropped until it answers that it
 * failed, then they flow again. Delete itself never reaches a. */
static bool ctrl_alt_del(struct cs *t, struct sc *a)
{
    CHECK(cs_key(t, U_LCTRL, INPUT_KEY_DOWN, INPUT_MOD_LCTRL));
    CHECK(cs_key(t, U_LALT, INPUT_KEY_DOWN, INPUT_MOD_LCTRL | INPUT_MOD_LALT));
    CHECK(cs_key(t, U_DELETE, INPUT_KEY_DOWN, INPUT_MOD_LCTRL | INPUT_MOD_LALT));
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(t->init, SIG_READABLE, now() + CT_WAIT, &seen), OK);
    struct initctl_reboot_req q;
    uint32_t n = 0;
    struct channel_read_args r = { .h = t->init, .bytes_cap = sizeof(q),
                                   .bytes = (uint64_t)(uintptr_t)&q,
                                   .actual_bytes = (uint64_t)(uintptr_t)&n };
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK(n == sizeof(q) && q.ordinal == INITCTL_REBOOT);
    CHECK(cs_tap(t, U_A, 0));   /* dropped: the machine is going */
    struct initctl_reboot_rep rep = { .txid = q.txid, .status = ERR_IO };
    CHECK_ST(jam_channel_write(t->init, &rep, sizeof(rep), NULL, 0), OK);
    CHECK(cs_key(t, U_DELETE, INPUT_KEY_UP, INPUT_MOD_LCTRL | INPUT_MOD_LALT));
    CHECK(cs_key(t, U_LALT, INPUT_KEY_UP, INPUT_MOD_LCTRL));
    CHECK(cs_key(t, U_LCTRL, INPUT_KEY_UP, 0));
    CHECK(cs_tap(t, U_B, 0));
    CHECK(cs_sync(a, NULL));
    const uint32_t want[] = { DOWN(KEY_LEFTCTRL), DOWN(KEY_LEFTALT), UP(KEY_LEFTALT),
                              UP(KEY_LEFTCTRL), DOWN(KEY_B), UP(KEY_B) };
    CHECK(keys_are(&a->k, want, 6));
    return true;
}

bool t_comp_seat_reserved(void)
{
    static struct sc a;
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(cs_client(&t, &a));
    CHECK(cs_window(&a, 10, 10, 64, 64));
    CHECK(ct_await(&a.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, a.kb, CT_WAIT));
    ct_clear(&a.k);
    /* Ctrl+C: an ordinary key, with Control in the modifiers */
    CHECK(cs_key(&t, U_LCTRL, INPUT_KEY_DOWN, INPUT_MOD_LCTRL));
    CHECK(cs_tap(&t, U_C, INPUT_MOD_LCTRL));
    CHECK(cs_key(&t, U_LCTRL, INPUT_KEY_UP, 0));
    /* Super+F and Super+T: the window keys, not the client's */
    CHECK(cs_key(&t, U_LGUI, INPUT_KEY_DOWN, INPUT_MOD_LGUI));
    CHECK(cs_tap(&t, U_F, INPUT_MOD_LGUI));
    CHECK(cs_tap(&t, U_T, INPUT_MOD_LGUI));
    CHECK(cs_key(&t, U_LGUI, INPUT_KEY_UP, 0));
    CHECK(cs_sync(&a, NULL));
    const uint32_t want[] = { DOWN(KEY_LEFTCTRL), DOWN(KEY_C), UP(KEY_C), UP(KEY_LEFTCTRL),
                              DOWN(KEY_LEFTMETA), UP(KEY_LEFTMETA) };
    CHECK(keys_are(&a.k, want, 6));
    const struct ct_event *e = find_ev(&a, &jwl_wl_keyboard_interface,
                                       JWL_WL_KEYBOARD_EV_MODIFIERS);
    CHECK(e && e->u[1] == KEYMAP_MOD_CTRL);
    ct_clear(&a.k);
    CHECK(ctrl_alt_del(&t, &a));
    uint64_t paints, px, last, worst, conn, refused, gc, gp, gs, keys, reserved;
    uint32_t clients, surfaces, windows, sources;
    CHECK_ST(compctl_stats_within(t.ctl, CT_WAIT, &paints, &px, &last, &worst, &clients,
                                  &surfaces, &windows, &conn, &refused, &gc, &gp, &gs, &sources,
                                  &keys, &reserved), OK);
    CHECK_EQ(reserved, 3);   /* F, T, Delete: Alt+Tab isn't in this test */
    CHECK_EQ(clients, 1);
    CHECK_EQ(windows, 1);
    CHECK_EQ(sources, 1);
    CHECK_EQ(keys, 9);       /* every press but the one dropped while rebooting */
    ct_close(&a.k);
    CHECK(cs_stop(&t));
    return true;
}

/* A second keyboard: a key both hold is pressed and released once; a key
 * held by a keyboard that goes away is released. */
static bool two_keyboards(struct cs *t, struct sc *a)
{
    handle_t kb2;
    CHECK_ST(compctl_connect_input_within(t->ctl, CT_WAIT, &kb2), OK);
    ct_clear(&a->k);
    CHECK(cs_key(t, U_A, INPUT_KEY_DOWN, 0));
    CHECK_ST(input_key_within(kb2, CT_WAIT, U_A, INPUT_KEY_DOWN, 0, 0), OK);
    CHECK(cs_key(t, U_A, INPUT_KEY_UP, 0));
    CHECK_ST(input_key_within(kb2, CT_WAIT, U_A, INPUT_KEY_UP, 0, 0), OK);
    CHECK_ST(input_key_within(kb2, CT_WAIT, U_X, INPUT_KEY_DOWN, 0, 0), OK);
    jam_handle_close(kb2);
    CHECK(cs_sync(a, NULL));
    uint32_t want[] = { DOWN(KEY_A), UP(KEY_A), DOWN(KEY_X), UP(KEY_X) };
    uint64_t until = now() + CT_WAIT;   /* the release comes when the compositor sees it go */
    uint32_t got[8];
    while (keys_got(&a->k, got, 8) < 4 && now() < until) {
        jam_nanosleep(now() + NS_PER_MS);
        CHECK(cs_sync(a, NULL));
    }
    CHECK(keys_are(&a->k, want, 4));
    return true;
}

bool t_comp_seat_text(void)
{
    static struct sc a;
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(cs_client(&t, &a));
    CHECK(cs_window(&a, 10, 10, 64, 64));
    CHECK(ct_await(&a.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, a.kb, CT_WAIT));
    ct_clear(&a.k);
    static const char text[] = "aB\x03\x1b[A\r";
    uint8_t bytes[64] = { 0 };
    memcpy(bytes, text, sizeof(text) - 1);
    CHECK_ST(input_text_within(t.src, CT_WAIT, sizeof(text) - 1, bytes), OK);
    CHECK(cs_sync(&a, NULL));
    const uint32_t want[] = {
        DOWN(KEY_A), UP(KEY_A),
        DOWN(KEY_LEFTSHIFT), DOWN(KEY_B), UP(KEY_B), UP(KEY_LEFTSHIFT),
        DOWN(KEY_LEFTCTRL), DOWN(KEY_C), UP(KEY_C), UP(KEY_LEFTCTRL),
        DOWN(KEY_UP), UP(KEY_UP), DOWN(KEY_ENTER), UP(KEY_ENTER),
    };
    CHECK(keys_are(&a.k, want, sizeof(want) / sizeof(want[0])));
    const struct ct_event *e = find_ev(&a, &jwl_wl_keyboard_interface,
                                       JWL_WL_KEYBOARD_EV_MODIFIERS);
    CHECK(e && e->u[1] == KEYMAP_MOD_SHIFT);
    /* Caps Lock on: "a" is typed with Shift (Shift and Caps Lock: lower case) */
    CHECK(cs_tap(&t, U_CAPS, 0));
    CHECK(cs_sync(&a, NULL));
    ct_clear(&a.k);
    bytes[0] = 'a';
    CHECK_ST(input_text_within(t.src, CT_WAIT, 1, bytes), OK);
    CHECK(cs_sync(&a, NULL));
    const uint32_t caps[] = { DOWN(KEY_LEFTSHIFT), DOWN(KEY_A), UP(KEY_A), UP(KEY_LEFTSHIFT) };
    CHECK(keys_are(&a.k, caps, 4));
    e = find_ev(&a, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_MODIFIERS);
    CHECK(e && e->u[3] == (KEYMAP_MOD_CAPS | KEYMAP_MOD_NUM));
    CHECK(two_keyboards(&t, &a));
    ct_close(&a.k);
    CHECK(cs_stop(&t));
    return true;
}

/* compctl's levels: an INPUT channel connects sources and nothing else. */
static bool toplevel_under_pointer(struct cs *t, struct sc *a, uint32_t *top)
{
    struct ct_client *k = &a->k;
    uint32_t wm = ct_new(k, &jwl_xdg_wm_base_interface, 1);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 5, "xdg_wm_base", 1, wm), OK);
    uint32_t s = ct_new(k, &jwl_wl_surface_interface, 4), xs = ct_new(k, &jwl_xdg_surface_interface, 1);
    *top = ct_new(k, &jwl_xdg_toplevel_interface, 1);
    CHECK_ST(jwl_wl_compositor_create_surface(k->c, k->compositor, s), OK);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(k->c, wm, xs, s), OK);
    CHECK_ST(jwl_xdg_surface_get_toplevel(k->c, xs, *top), OK);
    CHECK_ST(jwl_wl_surface_commit(k->c, s), OK);
    const struct ct_event *e = ct_await(k, &jwl_xdg_surface_interface,
                                        JWL_XDG_SURFACE_EV_CONFIGURE, xs, CT_WAIT);
    CHECK(e);
    CHECK_ST(jwl_xdg_surface_ack_configure(k->c, xs, e->u[0]), OK);
    handle_t vmo = HANDLE_INVALID;
    uint32_t pool = ct_pool(k, 100 * 80 * 4, &vmo), buf = ct_new(k, &jwl_wl_buffer_interface, 1);
    jam_handle_close(vmo);
    CHECK_ST(jwl_wl_shm_pool_create_buffer(k->c, pool, buf, 0, 100, 80, 400,
                                           JWL_WL_SHM_FORMAT_XRGB8888), OK);
    CHECK_ST(jwl_wl_surface_attach(k->c, s, buf, 0, 0), OK);
    CHECK_ST(jwl_wl_surface_commit(k->c, s), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    /* A lone window is centred: the output's middle is on its surface. */
    CHECK(cs_pointer_to(t, OUT_W / 2 + 1, OUT_H / 2 + 1));
    CHECK(ct_await(k, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER, a->ptr, CT_WAIT));
    return true;
}

bool t_comp_seat_move(void)
{
    static struct sc a;
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(cs_client(&t, &a));
    bool xdg = false;
    for (unsigned i = 0; i < a.k.nev; i++)
        xdg |= a.k.ev[i].iface == &jwl_wl_registry_interface && !strcmp(a.k.ev[i].s, "xdg_wm_base");
    if (!xdg) {
        printf("utest: comp_seat_move: skipped: the compositor offers no xdg_wm_base yet\n");
        ct_close(&a.k);
        return cs_stop(&t);
    }
    uint32_t top;
    CHECK(toplevel_under_pointer(&t, &a, &top));
    /* a press, released: its serial is stale, and move is ignored */
    ct_clear(&a.k);
    CHECK(cs_button(&t, true));
    const struct ct_event *e = ct_await(&a.k, &jwl_wl_pointer_interface,
                                        JWL_WL_POINTER_EV_BUTTON, a.ptr, CT_WAIT);
    CHECK(e);
    uint32_t stale = e->u[0];
    CHECK(cs_button(&t, false));
    CHECK_ST(jwl_xdg_toplevel_move(a.k.c, top, a.seat, stale), OK);
    CHECK_ST(ct_roundtrip(&a.k), OK);
    CHECK(!find_ev(&a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_LEAVE));
    /* a press still held: move takes the pointer (leave) until the release */
    ct_clear(&a.k);
    CHECK(cs_button(&t, true));
    e = ct_await(&a.k, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON, a.ptr, CT_WAIT);
    CHECK(e);
    CHECK_ST(jwl_xdg_toplevel_move(a.k.c, top, a.seat, e->u[0]), OK);
    CHECK(ct_await(&a.k, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_LEAVE, a.ptr, CT_WAIT));
    CHECK(cs_button(&t, false));
    CHECK(!a.k.errored);
    ct_close(&a.k);
    CHECK(cs_stop(&t));
    return true;
}

