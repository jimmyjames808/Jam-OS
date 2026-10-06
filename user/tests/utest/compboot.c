/* utest: the compositor as init wires it on a plain boot, headless with
 * the seat's harness (compseat.h):
 *
 * t_comp_layout_wait: compctl's set_layout and layout_wait (how init
 * learns of a switch and saves it): answered at once for another layout,
 * kept until Super+T switches it, one waiting per channel, set_layout
 * answering it too; ADMIN only, the values checked.
 * t_comp_super_enter: Super+Enter (and keypad Enter) asks init for a
 * terminal on its control channel, one ask at a time, whichever window has
 * the keys, and the window never sees Enter.
 * t_comp_early_keys: keys typed while no window has the keys (before the
 * first, or after the focused one went) reach the next that takes them,
 * with their modifiers, unless they are more than 5 s old by then.
 * t_comp_screen_on_top: a full-screen window that takes no keys (a boot
 * overlay) stays over a window that maps after it and takes the keys, with
 * the desktop off and on: a click anywhere (where the strip is too) is
 * its, the keys stay the other window's, and still are once it has gone.
 * t_comp_no_keyboard: a window whose client has no wl_keyboard (the boot
 * splash's) never takes the keys: not when it maps over the focused
 * window, not when it is clicked. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/compctl.h>
#include <idl/initctl.h>
#include <idl/input.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <os.h>
#include "compseat.h"
#include "utest.h"

#define FLOATING   0u
#define TILING     1u
#define WAIT_TXID  0x1a7e5701u
#define U_ENTER    0x28
#define U_KP_ENTER 0x58
#define U_LSHIFT   0xe1
#define QUIET      (200 * NS_PER_MS)   /* how long "nothing comes" is watched */
#define STALE      (5300 * NS_PER_MS)  /* past keyboard.c's EARLY_KEEP (5 s) */

/* The answer to our layout_wait (WAIT_TXID) on ch, within CT_WAIT: its status and layout. */
static status_t wait_answer(handle_t ch, uint8_t *layout)
{
    signals_t seen;
    status_t st = jam_object_wait_one(ch, SIG_READABLE, now() + CT_WAIT, &seen);
    uint8_t rep[COMPCTL_REP_MAX];
    struct idl_msg m;
    if (st == OK)
        st = idl_reply_read(ch, rep, sizeof(rep), &m);
    if (st == OK && m.txid != WAIT_TXID)
        st = ERR_INTERNAL;
    return st == OK ? compctl_layout_wait_result(rep, &m, layout) : st;
}

/* Nothing is queued on ch for a while. */
static bool quiet(handle_t ch)
{
    signals_t seen;
    CHECK_ST(jam_object_wait_one(ch, SIG_READABLE, now() + QUIET, &seen), ERR_TIMED_OUT);
    return true;
}

static bool layout_refusals(struct cs *t)
{
    handle_t in;
    uint8_t l = 0;
    CHECK_ST(compctl_new_client_within(t->ctl, CT_WAIT, 1, &in), OK);
    CHECK_ST(compctl_set_layout_within(in, CT_WAIT, TILING), ERR_ACCESS_DENIED);
    CHECK_ST(compctl_layout_wait_within(in, CT_WAIT, TILING, &l), ERR_ACCESS_DENIED);
    jam_handle_close(in);
    CHECK_ST(compctl_set_layout_within(t->ctl, CT_WAIT, 2), ERR_INVALID_ARGS);
    return true;
}

bool t_comp_layout_wait(void)
{
    struct cs t;
    uint8_t l = 9;
    CHECK(cs_start(&t));
    CHECK(layout_refusals(&t));
    /* it starts floating (no layout= argument): a wait for tiling is answered at once */
    CHECK_ST(compctl_layout_wait_within(t.ctl, CT_WAIT, TILING, &l), OK);
    CHECK_EQ(l, FLOATING);
    /* a wait for floating stays until the user switches; a second one is refused */
    CHECK_ST(compctl_layout_wait_send(t.ctl, WAIT_TXID, FLOATING), OK);
    CHECK(quiet(t.ctl));
    CHECK_ST(compctl_layout_wait_within(t.ctl, CT_WAIT, FLOATING, &l), ERR_BAD_STATE);
    CHECK(cs_tap(&t, U_T, INPUT_MOD_LGUI));   /* Super+T */
    CHECK_ST(wait_answer(t.ctl, &l), OK);
    CHECK_EQ(l, TILING);
    /* set_layout (init, once /data is there) answers a waiting one too */
    CHECK_ST(compctl_layout_wait_send(t.ctl, WAIT_TXID, TILING), OK);
    CHECK(quiet(t.ctl));
    CHECK_ST(compctl_set_layout_within(t.ctl, CT_WAIT, TILING), OK);   /* no change: no answer */
    CHECK(quiet(t.ctl));
    CHECK_ST(compctl_set_layout_within(t.ctl, CT_WAIT, FLOATING), OK);
    CHECK_ST(wait_answer(t.ctl, &l), OK);
    CHECK_EQ(l, FLOATING);
    /* a wait left waiting when its channel closes goes with it */
    CHECK_ST(compctl_layout_wait_send(t.ctl, WAIT_TXID, FLOATING), OK);
    CHECK(cs_stop(&t));
    return true;
}

/* The next request on init's end: a terminal request, its txid in *txid. */
static bool terminal_asked(struct cs *t, uint32_t *txid)
{
    signals_t seen;
    CHECK_ST(jam_object_wait_one(t->init, SIG_READABLE, now() + CT_WAIT, &seen), OK);
    struct initctl_terminal_req q;
    uint32_t n = 0;
    struct channel_read_args r = { .h = t->init, .bytes_cap = sizeof(q),
                                   .bytes = (uint64_t)(uintptr_t)&q,
                                   .actual_bytes = (uint64_t)(uintptr_t)&n };
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK(n == sizeof(q) && q.ordinal == INITCTL_TERMINAL);
    *txid = q.txid;
    return true;
}

static bool terminal_answer(struct cs *t, uint32_t txid, status_t st)
{
    struct initctl_terminal_rep rep = { .txid = txid, .status = st, .number = 2 };
    CHECK_ST(jam_channel_write(t->init, &rep, st == OK ? sizeof(rep) : 8, NULL, 0), OK);
    return true;
}

bool t_comp_super_enter(void)
{
    static struct sc a;
    struct cs t;
    uint32_t txid;
    CHECK(cs_start(&t));
    CHECK(cs_client(&t, &a));
    CHECK(cs_window(&a, 10, 10, 64, 64));
    CHECK(ct_await(&a.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, a.kb, CT_WAIT));
    ct_clear(&a.k);
    CHECK(cs_key(&t, U_LGUI, INPUT_KEY_DOWN, INPUT_MOD_LGUI));
    CHECK(cs_tap(&t, U_ENTER, INPUT_MOD_LGUI));
    CHECK(terminal_asked(&t, &txid));
    CHECK(cs_tap(&t, U_ENTER, INPUT_MOD_LGUI));   /* before the answer: the same ask */
    CHECK(quiet(t.init));
    CHECK(terminal_answer(&t, txid, OK));
    CHECK(cs_tap(&t, U_KP_ENTER, INPUT_MOD_LGUI));
    CHECK(terminal_asked(&t, &txid));
    CHECK(terminal_answer(&t, txid, ERR_NO_RESOURCES));   /* said in the log, nothing more */
    CHECK(cs_key(&t, U_LGUI, INPUT_KEY_UP, 0));
    CHECK(cs_tap(&t, U_ENTER, 0));   /* without Super: the window's */
    CHECK(cs_sync(&a, NULL));
    const uint32_t want[] = { DOWN(KEY_LEFTMETA), UP(KEY_LEFTMETA), DOWN(KEY_ENTER),
                              UP(KEY_ENTER) };
    CHECK(keys_are(&a.k, want, 4));
    CHECK(quiet(t.init));
    ct_close(&a.k);
    CHECK(cs_stop(&t));
    return true;
}

/* Keys typed while no window has the keys (the shell's prompt comes
 * before its terminal's window: at boot, after a restart) reach the next
 * window that takes them, after its enter, with their modifiers; not
 * those more than 5 s old by then. */
bool t_comp_early_keys(void)
{
    static struct sc a, b, c;
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(cs_client(&t, &a));
    CHECK(cs_tap(&t, U_A, 0));
    CHECK(cs_key(&t, U_LSHIFT, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT));
    CHECK(cs_tap(&t, U_B, INPUT_MOD_LSHIFT));
    CHECK(cs_key(&t, U_LSHIFT, INPUT_KEY_UP, 0));
    CHECK(cs_window(&a, 10, 10, 64, 64));
    CHECK(ct_await(&a.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, a.kb, CT_WAIT));
    CHECK(cs_sync(&a, NULL));
    const uint32_t want[] = { DOWN(KEY_A), UP(KEY_A), DOWN(KEY_LEFTSHIFT), DOWN(KEY_B), UP(KEY_B),
                              UP(KEY_LEFTSHIFT) };
    CHECK(keys_are(&a.k, want, 6));
    const struct ct_event *e = find_ev(&a, &jwl_wl_keyboard_interface,
                                       JWL_WL_KEYBOARD_EV_MODIFIERS);
    CHECK(e);
    bool shifted = false;   /* B came with Shift down */
    for (unsigned i = 0; i < a.k.nev; i++)
        shifted |= a.k.ev[i].iface == &jwl_wl_keyboard_interface &&
                   a.k.ev[i].op == JWL_WL_KEYBOARD_EV_MODIFIERS && a.k.ev[i].u[1] != 0;
    CHECK(shifted);
    /* a's window gone (a terminal restarting): a key typed now reaches
     * the next window, b's */
    ct_close(&a.k);
    CHECK(cs_client(&t, &b));
    jam_nanosleep(now() + QUIET);   /* a's windows taken away */
    CHECK(cs_tap(&t, U_X, 0));
    CHECK(cs_window(&b, 10, 10, 64, 64));
    CHECK(ct_await(&b.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, b.kb, CT_WAIT));
    CHECK(cs_sync(&b, NULL));
    const uint32_t kx[] = { DOWN(KEY_X), UP(KEY_X) };
    CHECK(keys_are(&b.k, kx, 2));
    /* but not one typed more than 5 s before a window takes the keys */
    ct_close(&b.k);
    CHECK(cs_client(&t, &c));
    jam_nanosleep(now() + QUIET);
    CHECK(cs_tap(&t, U_A, 0));
    jam_nanosleep(now() + STALE);
    CHECK(cs_window(&c, 10, 10, 64, 64));
    CHECK(ct_await(&c.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, c.kb, CT_WAIT));
    CHECK(cs_sync(&c, NULL));
    CHECK(no_keys(&c.k));
    ct_close(&c.k);
    CHECK(cs_stop(&t));
    return true;
}

/* A client with a seat and a pointer, no keyboard (libjwl's no_keyboard). */
static bool keyless_client(struct cs *t, struct sc *c)
{
    CHECK(ct_open(&t->p, &c->k));
    CHECK(ct_bind_all(&c->k));
    struct jwl_conn *k = c->k.c;
    CHECK((c->seat = ct_new(&c->k, &jwl_wl_seat_interface, 5)) != 0);
    CHECK_ST(jwl_wl_registry_bind(k, c->k.registry, 4, "wl_seat", 5, c->seat), OK);
    CHECK((c->ptr = ct_new(&c->k, &jwl_wl_pointer_interface, 5)) != 0);
    CHECK_ST(jwl_wl_seat_get_pointer(k, c->seat, c->ptr), OK);
    c->kb = 0;
    CHECK_ST(ct_roundtrip(&c->k), OK);
    return true;
}

/* c's full-screen xdg toplevel (the splash's kind of window), mapped with
 * an OUT_W x OUT_H buffer. */
static bool fullscreen_window(struct sc *c)
{
    struct ct_client *k = &c->k;
    struct jwl_conn *n = k->c;
    uint32_t wm = ct_new(k, &jwl_xdg_wm_base_interface, 1);
    CHECK(wm && jwl_wl_registry_bind(n, k->registry, 5, "xdg_wm_base", 1, wm) == OK);
    uint32_t size = OUT_W * OUT_H * 4;
    handle_t vmo = HANDLE_INVALID;
    uint32_t pool = ct_pool(k, size, &vmo);
    if (vmo != HANDLE_INVALID)
        jam_handle_close(vmo);
    uint32_t buf = pool ? ct_new(k, &jwl_wl_buffer_interface, 1) : 0;
    CHECK(buf && jwl_wl_shm_pool_create_buffer(n, pool, buf, 0, OUT_W, OUT_H, OUT_W * 4,
                                               JWL_WL_SHM_FORMAT_XRGB8888) == OK);
    uint32_t s = ct_new(k, &jwl_wl_surface_interface, 4);
    uint32_t xs = ct_new(k, &jwl_xdg_surface_interface, 1);
    uint32_t top = ct_new(k, &jwl_xdg_toplevel_interface, 1);
    CHECK(s && xs && top);
    CHECK_ST(jwl_wl_compositor_create_surface(n, k->compositor, s), OK);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(n, wm, xs, s), OK);
    CHECK_ST(jwl_xdg_surface_get_toplevel(n, xs, top), OK);
    CHECK_ST(jwl_xdg_toplevel_set_fullscreen(n, top, 0), OK);
    CHECK_ST(jwl_wl_surface_commit(n, s), OK);   /* the initial commit: a configure comes */
    const struct ct_event *e = ct_await(k, &jwl_xdg_surface_interface,
                                        JWL_XDG_SURFACE_EV_CONFIGURE, xs, CT_WAIT);
    CHECK(e);
    CHECK_ST(jwl_xdg_surface_ack_configure(n, xs, e->u[0]), OK);
    CHECK_ST(jwl_wl_surface_attach(n, s, buf, 0, 0), OK);
    CHECK_ST(jwl_wl_surface_commit(n, s), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    return true;
}

/* A full-screen window that takes no keys (the splash) mapped first, then
 * a's (the first terminal's, later than the splash's): a takes the keys
 * but comes up under the full-screen one, which a click reaches. */
static bool screen_on_top(bool desk)
{
    static struct sc a, s;
    struct cs t;
    memset(&a, 0, sizeof(a));
    memset(&s, 0, sizeof(s));
    CHECK(desk ? cs_start_desk(&t) : cs_start(&t));
    CHECK(keyless_client(&t, &s));
    CHECK(fullscreen_window(&s));
    CHECK(cs_client(&t, &a));
    CHECK(cs_window(&a, 10, 10, 64, 64));
    CHECK(ct_await(&a.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, a.kb, CT_WAIT));
    ct_clear(&a.k);
    ct_clear(&s.k);
    CHECK(cs_pointer_to(&t, 20, 20));
    CHECK(cs_button(&t, true));
    CHECK(cs_button(&t, false));
    CHECK(cs_tap(&t, U_A, 0));
    CHECK(cs_sync(&a, &s));
    CHECK(find_ev(&s, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    CHECK(!find_ev(&a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    const uint32_t ka[] = { DOWN(KEY_A), UP(KEY_A) };
    CHECK(keys_are(&a.k, ka, 2));   /* the keys are still a's */
    /* a click where the strip is: the overlay's, not the desktop's */
    ct_clear(&s.k);
    CHECK(cs_pointer_to(&t, 20, 4));
    CHECK(cs_button(&t, true));
    CHECK(cs_button(&t, false));
    CHECK(cs_sync(&a, &s));
    CHECK(find_ev(&s, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    /* it goes: a still has the keys */
    ct_close(&s.k);
    ct_clear(&a.k);
    CHECK(cs_tap(&t, U_B, 0));
    CHECK(cs_sync(&a, NULL));
    const uint32_t kb[] = { DOWN(KEY_B), UP(KEY_B) };
    CHECK(keys_are(&a.k, kb, 2));
    CHECK(!find_ev(&a, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_LEAVE));
    ct_close(&a.k);
    CHECK(cs_stop(&t));
    return true;
}

bool t_comp_screen_on_top(void)
{
    return screen_on_top(false) && screen_on_top(true);
}

bool t_comp_no_keyboard(void)
{
    static struct sc a, s;
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(cs_client(&t, &a));
    CHECK(cs_window(&a, 100, 100, 100, 80));
    CHECK(ct_await(&a.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, a.kb, CT_WAIT));
    /* the keyless client's window maps over the whole output: a keeps the keys */
    CHECK(keyless_client(&t, &s));
    CHECK(cs_window(&s, 0, 0, OUT_W, OUT_H));
    ct_clear(&a.k);
    CHECK(cs_tap(&t, U_A, 0));
    CHECK(cs_sync(&a, &s));
    const uint32_t ka[] = { DOWN(KEY_A), UP(KEY_A) };
    CHECK(keys_are(&a.k, ka, 2));
    CHECK(!find_ev(&a, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_LEAVE));
    /* a click on it: its pointer gets the press, the keys stay a's */
    ct_clear(&a.k);
    ct_clear(&s.k);
    CHECK(cs_pointer_to(&t, 20, 20));
    CHECK(cs_button(&t, true));
    CHECK(cs_button(&t, false));
    CHECK(cs_tap(&t, U_B, 0));
    CHECK(cs_sync(&a, &s));
    CHECK(find_ev(&s, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    const uint32_t kb[] = { DOWN(KEY_B), UP(KEY_B) };
    CHECK(keys_are(&a.k, kb, 2));
    CHECK(!find_ev(&a, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_LEAVE));
    ct_close(&s.k);
    ct_close(&a.k);
    CHECK(cs_stop(&t));
    return true;
}
