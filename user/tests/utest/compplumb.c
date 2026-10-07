/* utest: the desktop's plumbing (track D2b) on the compositor as init
 * wires it, headless with the seat's harness (compseat.h): /svc/notify's
 * shared channel (SR_USER + 5), and fake ends of the mixer's desktop
 * channel (SR_USER + 6) and netstack's read-only one (SR_USER + 7).
 *
 * t_comp_notify: compctl's NOTIFY level: a channel from /svc/notify's
 * svc.connect may notify, notify_wait and withdraw, and nothing else (every
 * ADMIN method and connect_input refused); an INPUT channel may not
 * notify; bad text, tints, icons and buttons refused; three cards with
 * buttons a channel; withdraw only one's own; a button pressed is
 * answered to the poster's notify_wait (kept until it asks when it isn't
 * waiting); without the desktop ERR_NOT_SUPPORTED.
 * t_comp_notify_splash: while the compositor waits for the boot splash a
 * notice gets its id but no card: its poster may withdraw it (dropped
 * unseen), and a click where its button would be answers nothing; when
 * the wait ends with no splash (5 s) the held card shows and its press
 * answers the notify_wait sent during the splash.
 * t_comp_launch: the search box's rows reach init: an app as
 * initctl.launch with its command name ("jamjar"), what is typed as
 * initctl.terminal's command; the cursor is busy from Enter until init
 * refuses, or until the window init's answer names maps (the app's title,
 * "Terminal 3"), as the image shows around the pointer.
 * t_comp_volume_net: the popovers' data: the compositor asks the mixer's
 * desktop channel (audioctl.desk) and netstack's (netctl.summary) at its
 * start and when a popover opens, the network's again every second; the
 * volume slider sends audioctl.set_master in the popover's dB scale. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <idl/audioctl.h>
#include <idl/compctl.h>
#include <idl/initctl.h>
#include <idl/input.h>
#include <idl/netctl.h>
#include <idl/svc.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <os.h>
#include "compseat.h"
#include "look.h"
#include "utest.h"

#define U_A     0x04
#define U_J     0x0d
#define U_L     0x0f
#define U_M     0x10
#define U_S     0x16
#define U_ENTER 0x28
#define SHORT   (300 * NS_PER_MS)        /* a card's slide in, a busy ring's frame */
#define WAIT_TX 0x77a17001u
#define PX      12                       /* where the pointer waits: far from every card */
#define PY      452

/* ---- small helpers ------------------------------------------------------------------ */

/* A notice's fields, NUL-padded. */
struct note {
    uint8_t title[64], body[96], buttons[72];
};

static void note_make(struct note *n, const char *title, const char *body, const char *b0,
                      const char *b1)
{
    memset(n, 0, sizeof(*n));
    snprintf((char *)n->title, sizeof(n->title), "%s", title);
    snprintf((char *)n->body, sizeof(n->body), "%s", body);
    if (b0)
        snprintf((char *)n->buttons, 24, "%s", b0);
    if (b1)
        snprintf((char *)n->buttons + 24, 24, "%s", b1);
}

static status_t post(handle_t ch, const struct note *n, uint8_t icon, uint8_t tint,
                     uint32_t *id)
{
    return compctl_notify_within(ch, CT_WAIT, n->title, n->body, icon, tint, n->buttons, id);
}

/* A click of the left button at (x, y), the pointer left there. */
static bool click(struct cs *t, int32_t x, int32_t y)
{
    CHECK(cs_pointer_to(t, x, y));
    CHECK(cs_button(t, true));
    CHECK(cs_button(t, false));
    return true;
}

/* The newest card's first button (one without a body), once it slid in. */
static bool press_first_button(struct cs *t)
{
    jam_nanosleep(now() + SHORT);
    int32_t x1 = OUT_W - LOOK_NOTE_RIGHT - LOOK_NOTE_W;
    int32_t bx = x1 + LOOK_NOTE_PAD_X + LOOK_NOTE_TILE + 10, by = LOOK_NOTE_TOP + LOOK_NOTE_PAD_Y +
                                                                    18 + 8;
    CHECK(click(t, bx + 8, by + LOOK_NOTE_BTN_H / 2));
    return true;
}

/* The answer to our notify_wait (WAIT_TX) on ch. */
static status_t press_answer(handle_t ch, uint32_t *id, uint8_t *button)
{
    signals_t seen;
    status_t st = jam_object_wait_one(ch, SIG_READABLE, now() + CT_WAIT, &seen);
    uint8_t rep[COMPCTL_REP_MAX];
    struct idl_msg m;
    if (st == OK)
        st = idl_reply_read(ch, rep, sizeof(rep), &m);
    if (st == OK && m.txid != WAIT_TX)
        st = ERR_INTERNAL;
    return st == OK ? compctl_notify_wait_result(rep, &m, id, button) : st;
}

/* ---- t_comp_notify_splash ------------------------------------------------------------ */

bool t_comp_notify_splash(void)
{
    struct cs t;
    handle_t n;
    struct note x;
    uint32_t id = 0, held = 0, got = 0;
    uint8_t b = 9;
    signals_t seen;
    CHECK(cs_start_splash(&t));
    uint64_t t0 = now();   /* the wait began before the compositor took input */
    CHECK_ST(svc_connect_within(t.note, CT_WAIT, &n), OK);
    /* held: an id at once; withdrawn, it is gone unseen */
    note_make(&x, "Held", "", NULL, NULL);
    CHECK_ST(post(n, &x, 0, 0, &id), OK);
    CHECK(id != 0);
    CHECK_ST(compctl_withdraw_within(n, CT_WAIT, id), OK);
    CHECK_ST(compctl_withdraw_within(n, CT_WAIT, id), ERR_NOT_FOUND);
    /* one with buttons, waited for: no card to press yet */
    note_make(&x, "Update written", "", "Reboot", "Later");
    CHECK_ST(post(n, &x, 'U', 2, &held), OK);
    CHECK_ST(compctl_notify_wait_send(n, WAIT_TX), OK);
    CHECK(press_first_button(&t));
    CHECK_ST(jam_object_wait_one(n, SIG_READABLE, now() + SHORT, &seen), ERR_TIMED_OUT);
    /* no splash in 5 s: the desktop, the held card, its press answered */
    jam_nanosleep(t0 + 5 * NS_PER_S + SHORT);
    CHECK(press_first_button(&t));
    CHECK_ST(press_answer(n, &got, &b), OK);
    CHECK(got == held && b == 0);
    jam_handle_close(n);
    CHECK(cs_stop(&t));
    return true;
}

/* ---- t_comp_notify ------------------------------------------------------------------- */

static bool notify_refusals(struct cs *t, handle_t n)
{
    handle_t h, in;
    uint64_t u[11];
    uint32_t v[4], id = 0;
    uint8_t l = 0;
    struct note x;
    CHECK_ST(compctl_blank_within(n, CT_WAIT, 1), ERR_ACCESS_DENIED);
    CHECK_ST(compctl_new_client_within(n, CT_WAIT, 2, &h), ERR_ACCESS_DENIED);
    CHECK_ST(compctl_new_client_within(n, CT_WAIT, 1, &h), ERR_ACCESS_DENIED);
    CHECK_ST(compctl_stats_within(n, CT_WAIT, &u[0], &u[1], &u[2], &u[3], &v[0], &v[1], &v[2],
                                  &u[4], &u[5], &u[6], &u[7], &u[8], &v[3], &u[9], &u[10]),
             ERR_ACCESS_DENIED);
    CHECK_ST(compctl_set_layout_within(n, CT_WAIT, 1), ERR_ACCESS_DENIED);
    CHECK_ST(compctl_layout_wait_within(n, CT_WAIT, 1, &l), ERR_ACCESS_DENIED);
    CHECK_ST(compctl_connect_input_within(n, CT_WAIT, &h), ERR_ACCESS_DENIED);
    /* an INPUT channel can't post */
    CHECK_ST(compctl_new_client_within(t->ctl, CT_WAIT, 1, &in), OK);
    note_make(&x, "Hello", "", NULL, NULL);
    CHECK_ST(post(in, &x, 0, 0, &id), ERR_ACCESS_DENIED);
    jam_handle_close(in);
    /* bad text, tint, icon, a button label with no end */
    note_make(&x, "", "body", NULL, NULL);
    CHECK_ST(post(n, &x, 0, 0, &id), ERR_INVALID_ARGS);
    note_make(&x, "Hi", "a\nb", NULL, NULL);
    CHECK_ST(post(n, &x, 0, 0, &id), ERR_INVALID_ARGS);
    note_make(&x, "Hi", "", NULL, NULL);
    CHECK_ST(post(n, &x, 0, 3, &id), ERR_INVALID_ARGS);
    CHECK_ST(post(n, &x, '!', 0, &id), ERR_INVALID_ARGS);
    memset(x.buttons, 'x', 24);
    CHECK_ST(post(n, &x, 0, 0, &id), ERR_INVALID_ARGS);
    return true;
}

static bool notify_caps(handle_t n, handle_t other)
{
    struct note x;
    uint32_t id = 0, ids[3];
    note_make(&x, "Plain", "it fades", NULL, NULL);
    CHECK_ST(post(n, &x, 'P', 2, &id), OK);
    CHECK(id != 0);
    CHECK_ST(compctl_withdraw_within(other, CT_WAIT, id), ERR_NOT_FOUND);   /* not its own */
    CHECK_ST(compctl_withdraw_within(n, CT_WAIT, id), OK);
    CHECK_ST(compctl_withdraw_within(n, CT_WAIT, id), ERR_NOT_FOUND);       /* leaving */
    note_make(&x, "Asks", "", "Yes", "No");
    for (unsigned i = 0; i < 3; i++)
        CHECK_ST(post(n, &x, 0, 1, &ids[i]), OK);
    CHECK_ST(post(n, &x, 0, 1, &id), ERR_NO_RESOURCES);   /* three with buttons a channel */
    CHECK_ST(post(other, &x, 0, 1, &id), OK);              /* another channel's own three */
    for (unsigned i = 0; i < 3; i++)
        CHECK_ST(compctl_withdraw_within(n, CT_WAIT, ids[i]), OK);
    CHECK_ST(compctl_withdraw_within(other, CT_WAIT, id), OK);
    jam_nanosleep(now() + SHORT);   /* every card gone */
    return true;
}

static bool notify_presses(struct cs *t, handle_t n)
{
    struct note x;
    uint32_t id = 0, got = 0;
    uint8_t b = 9;
    /* waiting: the press answers it */
    note_make(&x, "Update written", "", "Reboot", "Later");
    CHECK_ST(post(n, &x, 'U', 2, &id), OK);
    CHECK_ST(compctl_notify_wait_send(n, WAIT_TX), OK);
    CHECK_ST(compctl_notify_wait_within(n, CT_WAIT, &got, &b), ERR_BAD_STATE);   /* one waits */
    CHECK(press_first_button(t));
    CHECK_ST(press_answer(n, &got, &b), OK);
    CHECK(got == id && b == 0);
    /* not waiting: kept for the next notify_wait, answered at once */
    jam_nanosleep(now() + SHORT);   /* the card has gone */
    CHECK_ST(post(n, &x, 'U', 2, &id), OK);
    CHECK(press_first_button(t));
    jam_nanosleep(now() + SHORT);
    CHECK_ST(compctl_notify_wait_within(n, CT_WAIT, &got, &b), OK);
    CHECK(got == id && b == 0);
    return true;
}

bool t_comp_notify(void)
{
    struct cs t;
    handle_t n, other;
    CHECK(cs_start_desk(&t));
    CHECK_ST(svc_connect_within(t.note, CT_WAIT, &n), OK);
    CHECK_ST(svc_connect_within(t.note, CT_WAIT, &other), OK);
    CHECK(notify_refusals(&t, n));
    CHECK(notify_caps(n, other));
    CHECK(notify_presses(&t, n));
    /* ADMIN may post too (init's own notices) */
    struct note x;
    uint32_t id = 0;
    note_make(&x, "From init", "", NULL, NULL);
    CHECK_ST(post(t.ctl, &x, 0, 0, &id), OK);
    jam_handle_close(other);
    jam_handle_close(n);
    CHECK(cs_stop(&t));
    /* no desktop: no cards */
    CHECK(cs_start(&t));
    CHECK_ST(svc_connect_within(t.note, CT_WAIT, &n), OK);
    CHECK_ST(post(n, &x, 0, 0, &id), ERR_NOT_SUPPORTED);
    jam_handle_close(n);
    CHECK(cs_stop(&t));
    return true;
}

/* ---- t_comp_launch -------------------------------------------------------------------- */

/* The image around (PX, PY): the cursor and what is under it. */
static uint64_t around(const struct cs *t)
{
    const volatile uint32_t *img = t->p.image;
    uint64_t h = 1469598103934665603ull;
    for (int32_t y = PY - 20; y < OUT_H; y++)
        for (int32_t x = 0; x < PX + 36; x++)
            h = (h ^ img[y * OUT_W + x]) * 1099511628211ull;
    return h;
}

/* The image there becomes the same as `was` (same) or not, within CT_WAIT. */
static bool around_is(const struct cs *t, uint64_t was, bool same)
{
    uint64_t until = now() + CT_WAIT;
    while ((around(t) == was) != same && now() < until)
        jam_nanosleep(now() + 5 * NS_PER_MS);
    if ((around(t) == was) != same)
        FAIL("the cursor is %s", same ? "still busy" : "not busy");
    return true;
}

/* Super tapped alone, letters typed, Enter: the search box's top row runs. */
static bool search_run(struct cs *t, const uint16_t *keys, unsigned n)
{
    CHECK(cs_key(t, U_LGUI, INPUT_KEY_DOWN, INPUT_MOD_LGUI));
    CHECK(cs_key(t, U_LGUI, INPUT_KEY_UP, 0));
    for (unsigned i = 0; i < n; i++)
        CHECK(cs_tap(t, keys[i], 0));
    CHECK(cs_tap(t, U_ENTER, 0));
    return true;
}

/* The next request on init's end, of `want` bytes and ordinal: into q. */
static bool init_asked(struct cs *t, void *q, uint32_t want, uint32_t ordinal)
{
    signals_t seen;
    CHECK_ST(jam_object_wait_one(t->init, SIG_READABLE, now() + CT_WAIT, &seen), OK);
    uint32_t n = 0;
    struct channel_read_args r = { .h = t->init, .bytes_cap = want,
                                   .bytes = (uint64_t)(uintptr_t)q,
                                   .actual_bytes = (uint64_t)(uintptr_t)&n };
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK_EQ(n, want);
    CHECK_EQ(((const struct idl_req_hdr *)q)->ordinal, ordinal);
    return true;
}

/* A floating xdg toplevel titled title, 32x32, mapped. */
struct xw {
    struct ct_client k;
    uint32_t wm, pool;
    handle_t vmo;
};

static bool xdg_window(struct cs *t, struct xw *w, const char *title)
{
    CHECK(ct_open(&t->p, &w->k));
    CHECK(ct_bind_all(&w->k));
    struct jwl_conn *c = w->k.c;
    CHECK((w->wm = ct_new(&w->k, &jwl_xdg_wm_base_interface, 1)) != 0);
    CHECK_ST(jwl_wl_registry_bind(c, w->k.registry, 5, "xdg_wm_base", 1, w->wm), OK);
    CHECK((w->pool = ct_pool(&w->k, 32 * 32 * 4, &w->vmo)) != 0);
    uint32_t s = ct_new(&w->k, &jwl_wl_surface_interface, 4);
    uint32_t xs = ct_new(&w->k, &jwl_xdg_surface_interface, 1);
    uint32_t top = ct_new(&w->k, &jwl_xdg_toplevel_interface, 1);
    CHECK(s && xs && top);   /* new ids in the order they are made: the buffer's later */
    CHECK_ST(jwl_wl_compositor_create_surface(c, w->k.compositor, s), OK);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(c, w->wm, xs, s), OK);
    CHECK_ST(jwl_xdg_surface_get_toplevel(c, xs, top), OK);
    CHECK_ST(jwl_xdg_toplevel_set_title(c, top, title), OK);
    CHECK_ST(jwl_wl_surface_commit(c, s), OK);
    CHECK_ST(ct_roundtrip(&w->k), OK);
    uint32_t serial = 0;
    for (unsigned i = w->k.nev; i-- > 0;)
        if (w->k.ev[i].iface == &jwl_xdg_surface_interface &&
            w->k.ev[i].op == JWL_XDG_SURFACE_EV_CONFIGURE) {
            serial = w->k.ev[i].u[0];
            break;
        }
    CHECK_ST(jwl_xdg_surface_ack_configure(c, xs, serial), OK);
    uint32_t buf = ct_new(&w->k, &jwl_wl_buffer_interface, 1);
    CHECK(buf);
    CHECK_ST(jwl_wl_shm_pool_create_buffer(c, w->pool, buf, 0, 32, 32, 32 * 4,
                                           JWL_WL_SHM_FORMAT_XRGB8888), OK);
    CHECK_ST(jwl_wl_surface_attach(c, s, buf, 0, 0), OK);
    CHECK_ST(jwl_wl_surface_commit(c, s), OK);
    CHECK_ST(ct_roundtrip(&w->k), OK);
    CHECK(!w->k.errored);
    return true;
}

static void xw_close(struct xw *w)
{
    if (w->vmo)
        jam_handle_close(w->vmo);
    ct_close(&w->k);
}

static bool launch_app(struct cs *t, uint64_t idle)
{
    static const uint16_t jamj[] = { U_J, U_A, U_M, U_J };
    struct initctl_launch_req q;
    /* refused: busy, then not */
    CHECK(search_run(t, jamj, 4));
    CHECK(init_asked(t, &q, sizeof(q), INITCTL_LAUNCH));
    CHECK(!strcmp((const char *)q.app, "jamjar"));
    CHECK(around_is(t, idle, false));
    struct initctl_launch_rep r = { .txid = q.txid, .status = ERR_NOT_FOUND };
    CHECK_ST(jam_channel_write(t->init, &r, 8, NULL, 0), OK);
    CHECK(around_is(t, idle, true));
    /* started: busy until a window called Jamjar maps */
    CHECK(search_run(t, jamj, 4));
    CHECK(init_asked(t, &q, sizeof(q), INITCTL_LAUNCH));
    r = (struct initctl_launch_rep){ .txid = q.txid, .status = OK, .koid = 77 };
    CHECK_ST(jam_channel_write(t->init, &r, sizeof(r), NULL, 0), OK);
    CHECK(around_is(t, idle, false));
    jam_nanosleep(now() + SHORT);
    CHECK(around(t) != idle);   /* still busy: no window yet */
    static struct xw w;
    CHECK(xdg_window(t, &w, "Jamjar"));
    CHECK(around_is(t, idle, true));
    xw_close(&w);
    return true;
}

static bool run_in_terminal(struct cs *t, uint64_t idle)
{
    static const uint16_t ls[] = { U_L, U_S };
    struct initctl_terminal_req q;
    CHECK(search_run(t, ls, 2));
    CHECK(init_asked(t, &q, sizeof(q), INITCTL_TERMINAL));
    CHECK(!strcmp((const char *)q.command, "ls"));
    struct initctl_terminal_rep r = { .txid = q.txid, .status = OK, .number = 3 };
    CHECK_ST(jam_channel_write(t->init, &r, sizeof(r), NULL, 0), OK);
    CHECK(around_is(t, idle, false));
    static struct xw w;
    CHECK(xdg_window(t, &w, "Terminal 2"));   /* not its window */
    jam_nanosleep(now() + SHORT);
    CHECK(around(t) != idle);
    static struct xw w3;
    CHECK(xdg_window(t, &w3, "Terminal 3"));
    CHECK(around_is(t, idle, true));
    xw_close(&w3);
    xw_close(&w);
    return true;
}

bool t_comp_launch(void)
{
    struct cs t;
    CHECK(cs_start_desk(&t));
    CHECK(cs_pointer_to(&t, PX, PY));
    jam_nanosleep(now() + SHORT);
    uint64_t idle = around(&t);
    jam_nanosleep(now() + SHORT);
    CHECK(around(&t) == idle);   /* nothing moves there: the arrow on the wallpaper */
    CHECK(launch_app(&t, idle));
    CHECK(run_in_terminal(&t, idle));
    CHECK(cs_stop(&t));
    return true;
}

/* ---- t_comp_volume_net ------------------------------------------------------------------ */

/* The next request on our end ch (the fake service's), within wait: its
 * ordinal, txid and bytes into q (cap). */
static status_t asked(handle_t ch, uint64_t wait, void *q, uint32_t cap, uint32_t *n)
{
    signals_t seen;
    status_t st = jam_object_wait_one(ch, SIG_READABLE, now() + wait, &seen);
    if (st != OK)
        return st;
    struct channel_read_args r = { .h = ch, .bytes_cap = cap, .bytes = (uint64_t)(uintptr_t)q,
                                   .actual_bytes = (uint64_t)(uintptr_t)n };
    return jam_channel_read(&r);
}

static bool answer_desk(struct cs *t, uint32_t txid)
{
    struct audioctl_desk_rep r = { .txid = txid, .status = OK, .master = -180, .playing = 1 };
    snprintf((char *)r.title, sizeof(r.title), "Artist - Song");
    snprintf((char *)r.output, sizeof(r.output), "QEMU line-out");
    CHECK_ST(jam_channel_write(t->mix, &r, sizeof(r), NULL, 0), OK);
    return true;
}

static bool answer_summary(struct cs *t, uint32_t txid, uint64_t rx)
{
    struct netctl_summary_rep r = { .txid = txid, .status = OK, .link = 1, .address = 0x0a00020f,
                                    .mask = 0xffffff00, .speed = 1000, .rx_bytes = rx,
                                    .tx_bytes = rx / 2 };
    snprintf((char *)r.chip, sizeof(r.chip), "e1000e");
    CHECK_ST(jam_channel_write(t->net, &r, sizeof(r), NULL, 0), OK);
    return true;
}

/* At its start the compositor asks both once: answered. */
static bool first_asks(struct cs *t)
{
    union { struct idl_req_hdr h; uint8_t b[64]; } q;
    uint32_t n = 0;
    CHECK_ST(asked(t->mix, CT_WAIT, &q, sizeof(q), &n), OK);
    CHECK_EQ(q.h.ordinal, AUDIOCTL_DESK);
    CHECK(answer_desk(t, q.h.txid));
    CHECK_ST(asked(t->net, CT_WAIT, &q, sizeof(q), &n), OK);
    CHECK_EQ(q.h.ordinal, NETCTL_SUMMARY);
    CHECK(answer_summary(t, q.h.txid, 1000));
    return true;
}

/* The strip's layout icon, by clicking along the strip from its middle to
 * the right until the layout switches (init's layout_wait answered): its
 * left edge, to within 2 pixels; the layout switched back. */
static bool find_mode_icon(struct cs *t, int32_t *x1)
{
    CHECK_ST(compctl_layout_wait_send(t->ctl, WAIT_TX, 0), OK);   /* floating: kept waiting */
    for (int32_t x = OUT_W / 2; x < OUT_W - 8; x += 2) {
        CHECK(click(t, x, LOOK_STRIP_H / 2));
        signals_t seen;
        if (jam_object_wait_one(t->ctl, SIG_READABLE, now() + 40 * NS_PER_MS, &seen) != OK)
            continue;
        uint8_t rep[COMPCTL_REP_MAX], l = 0;
        struct idl_msg m;
        CHECK_ST(idl_reply_read(t->ctl, rep, sizeof(rep), &m), OK);
        CHECK_EQ(m.txid, WAIT_TX);
        CHECK_ST(compctl_layout_wait_result(rep, &m, &l), OK);
        CHECK_EQ(l, 1);
        CHECK_ST(compctl_set_layout_within(t->ctl, CT_WAIT, 0), OK);
        *x1 = x;
        return true;
    }
    FAIL("no layout icon on the strip");
}

/* Drop what came on ch (the compositor's asks while the icon was found). */
static bool drain_asks(struct cs *t, handle_t ch, bool mix)
{
    union { struct idl_req_hdr h; uint8_t b[64]; } q;
    uint32_t n = 0;
    while (asked(ch, 50 * NS_PER_MS, &q, sizeof(q), &n) == OK)
        CHECK(mix ? answer_desk(t, q.h.txid) : answer_summary(t, q.h.txid, 1000));
    return true;
}

static bool volume_slider(struct cs *t, int32_t mode_x1)
{
    /* the volume icon: two icons right of the layout one (strip.c) */
    int32_t vol_x2 = mode_x1 + 3 * LOOK_ICON_W + 2 * LOOK_ISLAND_GAP;
    jam_nanosleep(now() + 600 * NS_PER_MS);   /* the last answer is stale: asked again */
    CHECK(click(t, vol_x2 - LOOK_ICON_W / 2 + 1, LOOK_STRIP_H / 2));
    union { struct idl_req_hdr h; struct audioctl_set_master_req s; uint8_t b[64]; } q;
    uint32_t n = 0;
    CHECK_ST(asked(t->mix, CT_WAIT, &q, sizeof(q), &n), OK);
    CHECK_EQ(q.h.ordinal, AUDIOCTL_DESK);
    CHECK(answer_desk(t, q.h.txid));
    /* the slider (popover.c): three quarters along it; it ends short of the
     * percentage's slot (as wide as "100%" in the strip's font) by the gap
     * and the knob */
    struct font *f;
    CHECK_ST(font_open(FONT_REGULAR, LOOK_STRIP_PX, &f), OK);
    int32_t pct = font_width(f, "100%");
    font_close(f);
    int32_t sx1 = vol_x2 - LOOK_POP_W + LOOK_POP_PAD + 16 + 9;
    int32_t sx2 = vol_x2 - LOOK_POP_PAD - pct - LOOK_POP_PCT_GAP - LOOK_POP_KNOB;
    int32_t sy = LOOK_STRIP_H + LOOK_POP_GAP + LOOK_POP_PAD + LOOK_POP_LINE / 2;
    CHECK(click(t, sx1 + (sx2 - sx1) * 3 / 4, sy));
    CHECK_ST(asked(t->mix, CT_WAIT, &q, sizeof(q), &n), OK);
    CHECK_EQ(q.h.ordinal, AUDIOCTL_SET_MASTER);
    /* 75% is -25 * 0.6 dB = -150 cb; the icon found to 2 pixels */
    CHECK(q.s.centibels <= -120 && q.s.centibels >= -180);
    struct audioctl_set_master_rep r = { .txid = q.h.txid, .status = OK,
                                         .centibels = q.s.centibels };
    CHECK_ST(jam_channel_write(t->mix, &r, sizeof(r), NULL, 0), OK);
    /* at the slider's left end: silence */
    CHECK(click(t, sx1, sy));
    CHECK_ST(asked(t->mix, CT_WAIT, &q, sizeof(q), &n), OK);
    CHECK_EQ(q.h.ordinal, AUDIOCTL_SET_MASTER);
    CHECK_EQ(q.s.centibels, -960);
    return true;
}

static bool network_popover(struct cs *t, int32_t mode_x1)
{
    int32_t net_x = mode_x1 + LOOK_ICON_W + LOOK_ISLAND_GAP + LOOK_ICON_W / 2;
    jam_nanosleep(now() + 600 * NS_PER_MS);
    CHECK(click(t, net_x, LOOK_STRIP_H / 2));
    union { struct idl_req_hdr h; uint8_t b[64]; } q;
    uint32_t n = 0;
    CHECK_ST(asked(t->net, CT_WAIT, &q, sizeof(q), &n), OK);
    CHECK_EQ(q.h.ordinal, NETCTL_SUMMARY);
    CHECK(answer_summary(t, q.h.txid, 5000));
    /* open: asked again each second, for the rates */
    CHECK_ST(asked(t->net, 3 * NS_PER_S, &q, sizeof(q), &n), OK);
    CHECK_EQ(q.h.ordinal, NETCTL_SUMMARY);
    CHECK(answer_summary(t, q.h.txid, 9000));
    return true;
}

bool t_comp_volume_net(void)
{
    struct cs t;
    int32_t mode_x1 = 0;
    CHECK(cs_start_desk(&t));
    CHECK(first_asks(&t));
    CHECK(find_mode_icon(&t, &mode_x1));
    CHECK(drain_asks(&t, t.mix, true) && drain_asks(&t, t.net, false));
    CHECK(click(&t, PX, PY));   /* any popover closed */
    CHECK(volume_slider(&t, mode_x1));
    CHECK(click(&t, PX, PY));
    CHECK(drain_asks(&t, t.mix, true));
    CHECK(network_popover(&t, mode_x1));
    CHECK(cs_stop(&t));
    return true;
}
