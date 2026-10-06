/* utest: the compositor's clipboard (user/services/compositor/data.c),
 * headless, with the seat's harness (compseat.h): test clients speaking
 * Wayland's wl_data_device_manager over real channels.
 *
 * t_comp_data_selection: the selection only to and from the client with
 * the keyboard focus: set_selection refused (its source cancelled) with a
 * serial that isn't one of the client's input events, or without the
 * focus; only text types offered on; the focused client told the selection
 * before wl_keyboard.enter, an unfocused one never; a receive passed on to
 * the source with a channel end the source can write and not read, its
 * data reaching the reader; receives refused (the reader's end closed, the
 * source told nothing) for a type not offered, a handle that isn't a
 * channel, a reader without the focus, past its budget (32 between
 * focus enters, whatever offer it uses: a new device's new offer
 * too); the owner going clears the selection.
 * t_comp_data_rules: drag and drop refused (start_drag cancels its source
 * at once; a used source used again, a drag source as the selection,
 * finish and set_actions on a selection's offer are protocol errors); a
 * source that never writes holds up nobody (the compositor answers and
 * routes keys meanwhile; the reader's end closes when the source closes
 * its own); a selection with no text type is told as none; a client that
 * never destroys its offers stops getting new ones (the selection said to
 * be empty) at the cap. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/input.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <os.h>
#include "compseat.h"
#include "utest.h"

#define TEXT     "text/plain;charset=utf-8"
#define DATA_V   3u
#define RECEIVES 32u   /* the compositor's DATA_RECEIVES_MAX (comp.h) */

/* A client with a seat, its data device and the device manager. */
struct dc {
    struct sc s;
    uint32_t mgr, dev;
};

static bool data_client(struct cs *t, struct dc *c)
{
    CHECK(cs_client(t, &c->s));
    struct ct_client *k = &c->s.k;
    CHECK((c->mgr = ct_new(k, &jwl_wl_data_device_manager_interface, DATA_V)) != 0);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 7, "wl_data_device_manager", DATA_V,
                                  c->mgr), OK);
    CHECK((c->dev = ct_new(k, &jwl_wl_data_device_interface, DATA_V)) != 0);
    CHECK_ST(jwl_wl_data_device_manager_get_data_device(k->c, c->mgr, c->dev, c->s.seat), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    CHECK(!k->errored);
    if (k->kept != HANDLE_INVALID)   /* the keymap's: the handles we look at come later */
        jam_handle_close(k->kept);
    k->kept = HANDLE_INVALID;
    return true;
}

/* A source of c's offering the n types. */
static uint32_t source(struct dc *c, const char *const *types, unsigned n)
{
    struct ct_client *k = &c->s.k;
    uint32_t id = ct_new(k, &jwl_wl_data_source_interface, DATA_V);
    if (!id || jwl_wl_data_device_manager_create_data_source(k->c, c->mgr, id) != OK)
        return 0;
    for (unsigned i = 0; i < n; i++)
        if (jwl_wl_data_source_offer(k->c, id, types[i]) != OK)
            return 0;
    return id;
}

/* Where event (iface, op) is in k's log since ct_clear: its index, or -1. */
static int ev_index(const struct ct_client *k, const struct jwl_interface *iface, uint16_t op)
{
    for (unsigned i = 0; i < k->nev; i++)
        if (k->ev[i].iface == iface && k->ev[i].op == op)
            return (int)i;
    return -1;
}

/* How many events (iface, op) k got since ct_clear. */
static unsigned ev_count(const struct ct_client *k, const struct jwl_interface *iface, uint16_t op)
{
    unsigned n = 0;
    for (unsigned i = 0; i < k->nev; i++)
        n += k->ev[i].iface == iface && k->ev[i].op == op;
    return n;
}

/* The serial of c's last input event in its log: a wl_keyboard.enter or a
 * key pressed. */
static uint32_t enter_serial(const struct dc *c)
{
    const struct ct_event *e = NULL;
    for (unsigned i = 0; i < c->s.k.nev; i++) {
        const struct ct_event *x = &c->s.k.ev[i];
        if (x->iface == &jwl_wl_keyboard_interface && (x->op == JWL_WL_KEYBOARD_EV_ENTER ||
                                                       (x->op == JWL_WL_KEYBOARD_EV_KEY &&
                                                        x->u[3] == 1)))
            e = x;
    }
    return e ? e->u[0] : 0;
}

/* What c's device was told the selection is last (an offer, or 0 for
 * none): its id, or UINT32_MAX if no selection event came. */
static uint32_t told(const struct dc *c)
{
    uint32_t last = UINT32_MAX;
    for (unsigned i = 0; i < c->s.k.nev; i++) {
        const struct ct_event *e = &c->s.k.ev[i];
        if (e->iface == &jwl_wl_data_device_interface && e->id == c->dev &&
            e->op == JWL_WL_DATA_DEVICE_EV_SELECTION)
            last = e->u[0];
    }
    return last;
}

/* A window for c at x, which takes the focus: told the selection first. */
static bool window_told_first(struct dc *c, int32_t x, uint32_t want_offer)
{
    ct_clear(&c->s.k);
    CHECK(cs_window(&c->s, x, 50, 100, 80));
    CHECK(ct_await(&c->s.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, c->s.kb,
                   CT_WAIT));
    int sel = ev_index(&c->s.k, &jwl_wl_data_device_interface, JWL_WL_DATA_DEVICE_EV_SELECTION);
    int enter = ev_index(&c->s.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER);
    CHECK(sel >= 0 && sel < enter);
    if (want_offer)
        CHECK(told(c) != 0);
    else
        CHECK_EQ(told(c), 0);
    return true;
}

/* c receives offer in type over a new channel: our end in *mine. */
static bool receive(struct dc *c, uint32_t offer, const char *type, handle_t *mine)
{
    handle_t theirs;
    CHECK_ST(jam_channel_create(mine, &theirs), OK);
    CHECK_ST(jwl_wl_data_offer_receive(c->s.k.c, offer, type, theirs), OK);
    CHECK_ST(ct_roundtrip(&c->s.k), OK);
    return true;
}

/* Our end of a receive the compositor refused: closed, nothing in it. */
static bool refused(handle_t mine)
{
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(mine, SIG_PEER_CLOSED, now() + CT_WAIT, &seen), OK);
    CHECK(!(seen & SIG_READABLE));
    jam_handle_close(mine);
    return true;
}

/* src's client (a) got wl_data_source.send for src in TEXT with a channel
 * end it can write but not read; it writes msg and closes; the reader's
 * end (mine) gets msg, then the end. */
static bool sent_through(struct dc *a, uint32_t src, handle_t mine, const char *msg)
{
    const struct ct_event *e = ct_await(&a->s.k, &jwl_wl_data_source_interface,
                                        JWL_WL_DATA_SOURCE_EV_SEND, src, CT_WAIT);
    CHECK(e && !strcmp(e->s, TEXT));
    handle_t w = a->s.k.kept;
    a->s.k.kept = HANDLE_INVALID;
    CHECK(w != HANDLE_INVALID);
    uint8_t buf[64];
    uint32_t got = 0;
    struct channel_read_args r = { .h = w, .bytes_cap = sizeof(buf),
                                   .bytes = (uint64_t)(uintptr_t)buf,
                                   .actual_bytes = (uint64_t)(uintptr_t)&got };
    CHECK_ST(jam_channel_read(&r), ERR_ACCESS_DENIED);   /* write only */
    CHECK_ST(jam_channel_write(w, msg, (uint32_t)strlen(msg), NULL, 0), OK);
    jam_handle_close(w);
    r.h = mine;
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK(got == strlen(msg) && !memcmp(buf, msg, got));
    CHECK_ST(jam_channel_read(&r), ERR_PEER_CLOSED);
    jam_handle_close(mine);
    ct_clear(&a->s.k);
    return true;
}

/* a's window has the focus: set_selection refused with a made-up serial,
 * then honoured with its enter's; only the text type offered on. */
static bool a_copies(struct dc *a, uint32_t *src)
{
    static const char *const types[] = { "image/png", TEXT };
    uint32_t serial = enter_serial(a);
    CHECK(serial);
    uint32_t bad = source(a, types, 2);
    CHECK(bad);
    ct_clear(&a->s.k);
    CHECK_ST(jwl_wl_data_device_set_selection(a->s.k.c, a->dev, bad, serial + 1000), OK);
    CHECK(ct_await(&a->s.k, &jwl_wl_data_source_interface, JWL_WL_DATA_SOURCE_EV_CANCELLED, bad,
                   CT_WAIT));
    CHECK(!ct_find(&a->s.k, &jwl_wl_data_device_interface, JWL_WL_DATA_DEVICE_EV_DATA_OFFER, 0));
    CHECK((*src = source(a, types, 2)) != 0);
    CHECK_ST(jwl_wl_data_device_set_selection(a->s.k.c, a->dev, *src, serial), OK);
    CHECK_ST(ct_roundtrip(&a->s.k), OK);
    CHECK(told(a) != 0 && told(a) != UINT32_MAX);
    CHECK_EQ(ev_count(&a->s.k, &jwl_wl_data_offer_interface, JWL_WL_DATA_OFFER_EV_OFFER), 1);
    CHECK(!strcmp(ct_find(&a->s.k, &jwl_wl_data_offer_interface, JWL_WL_DATA_OFFER_EV_OFFER,
                          0)->s, TEXT));
    CHECK(!ct_find(&a->s.k, &jwl_wl_data_source_interface, JWL_WL_DATA_SOURCE_EV_CANCELLED, *src));
    return true;
}

/* b, unfocused, hears nothing and may not set the selection. */
static bool b_unfocused(struct dc *a, struct dc *b)
{
    CHECK(cs_sync(&a->s, &b->s));
    CHECK_EQ(told(b), UINT32_MAX);
    static const char *const types[] = { TEXT };
    uint32_t s = source(b, types, 1);
    CHECK(s);
    CHECK_ST(jwl_wl_data_device_set_selection(b->s.k.c, b->dev, s, enter_serial(a)), OK);
    CHECK(ct_await(&b->s.k, &jwl_wl_data_source_interface, JWL_WL_DATA_SOURCE_EV_CANCELLED, s,
                   CT_WAIT));
    CHECK_ST(jwl_wl_data_source_destroy(b->s.k.c, s), OK);
    return true;
}

/* b, its receive budget spent, makes another data device: it is told the
 * selection with a new offer, which reads no more than the old one. */
static bool fresh_device_refused(struct dc *b)
{
    struct ct_client *k = &b->s.k;
    uint32_t dev = ct_new(k, &jwl_wl_data_device_interface, DATA_V);
    CHECK(dev);
    ct_clear(k);
    CHECK_ST(jwl_wl_data_device_manager_get_data_device(k->c, b->mgr, dev, b->s.seat), OK);
    CHECK(ct_await(k, &jwl_wl_data_device_interface, JWL_WL_DATA_DEVICE_EV_SELECTION, dev,
                   CT_WAIT));
    uint32_t offer = ct_find(k, &jwl_wl_data_device_interface, JWL_WL_DATA_DEVICE_EV_SELECTION,
                             dev)->u[0];
    CHECK(offer);
    handle_t mine;
    CHECK(receive(b, offer, TEXT, &mine));
    return refused(mine);
}

/* b (focused) reads a's selection; the refusals. */
static bool b_pastes(struct cs *t, struct dc *a, struct dc *b, uint32_t src, uint32_t a_offer)
{
    uint32_t offer = told(b);
    handle_t mine;
    CHECK(receive(b, offer, TEXT, &mine));
    CHECK(sent_through(a, src, mine, "copied text"));
    CHECK(receive(b, offer, "image/png", &mine));   /* not offered on */
    CHECK(refused(mine));
    handle_t ev;
    CHECK_ST(jam_event_create(&ev), OK);   /* not a channel */
    CHECK_ST(jwl_wl_data_offer_receive(b->s.k.c, offer, TEXT, ev), OK);
    CHECK_ST(ct_roundtrip(&b->s.k), OK);
    (void)t;
    CHECK(receive(a, a_offer, TEXT, &mine));   /* a hasn't the focus */
    CHECK(refused(mine));
    CHECK(cs_sync(&a->s, NULL));
    CHECK(!ct_find(&a->s.k, &jwl_wl_data_source_interface, JWL_WL_DATA_SOURCE_EV_SEND, 0));
    /* the budget: one passed on already, then up to RECEIVES, then
     * refused, whatever offer asks: a new device's own new offer too */
    for (unsigned i = 1; i < RECEIVES; i++) {
        CHECK(receive(b, offer, TEXT, &mine));
        CHECK(sent_through(a, src, mine, "again"));
    }
    CHECK(receive(b, offer, TEXT, &mine));
    CHECK(refused(mine));
    return fresh_device_refused(b);
}

bool t_comp_data_selection(void)
{
    static struct dc a, b;
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(data_client(&t, &a));
    CHECK(data_client(&t, &b));
    CHECK(window_told_first(&a, 50, 0));   /* nothing copied yet */
    uint32_t src;
    CHECK(a_copies(&a, &src));
    uint32_t a_offer = told(&a);
    CHECK(b_unfocused(&a, &b));
    CHECK(window_told_first(&b, 300, 1));
    CHECK(b_pastes(&t, &a, &b, src, a_offer));
    /* the owner goes: b is told there is no selection */
    ct_clear(&b.s.k);
    ct_close(&a.s.k);
    CHECK(ct_await(&b.s.k, &jwl_wl_data_device_interface, JWL_WL_DATA_DEVICE_EV_SELECTION, b.dev,
                   CT_WAIT));
    CHECK_EQ(told(&b), 0);
    ct_close(&b.s.k);
    CHECK(cs_stop(&t));
    return true;
}

/* ---- the rules around it ---------------------------------------------------------------- */

/* A fresh client breaking protocol rule `rule` (0: a source used again
 * after a drag, 1: a drag's source as the selection, 2: finish on a
 * selection's offer) gets the protocol error for it. */
static bool broken(struct cs *t, unsigned rule)
{
    static struct dc c;
    CHECK(data_client(t, &c));
    uint32_t surface = cs_window(&c.s, 50, 300, 40, 40);
    CHECK(surface);
    CHECK(ct_await(&c.s.k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER, c.s.kb, CT_WAIT));
    static const char *const types[] = { TEXT };
    uint32_t s = source(&c, types, 1), serial = enter_serial(&c);
    struct jwl_conn *k = c.s.k.c;
    uint32_t object = c.dev, code = JWL_WL_DATA_DEVICE_ERROR_USED_SOURCE;
    if (rule == 0) {   /* a drag: cancelled at once; the source can't be used again */
        CHECK_ST(jwl_wl_data_device_start_drag(k, c.dev, s, surface, 0, serial), OK);
        CHECK(ct_await(&c.s.k, &jwl_wl_data_source_interface, JWL_WL_DATA_SOURCE_EV_CANCELLED, s,
                       CT_WAIT));
        CHECK_ST(jwl_wl_data_device_set_selection(k, c.dev, s, serial), OK);
    } else if (rule == 1) {   /* a drag's source as the selection */
        CHECK_ST(jwl_wl_data_source_set_actions(k, s, 1), OK);
        CHECK_ST(jwl_wl_data_device_set_selection(k, c.dev, s, serial), OK);
        object = s;
        code = JWL_WL_DATA_SOURCE_ERROR_INVALID_SOURCE;
    } else {   /* finish on a selection's offer */
        CHECK_ST(jwl_wl_data_device_set_selection(k, c.dev, s, serial), OK);
        CHECK_ST(ct_roundtrip(&c.s.k), OK);
        object = told(&c);
        CHECK(object && object != UINT32_MAX);
        CHECK_ST(jwl_wl_data_offer_finish(k, object), OK);
        code = JWL_WL_DATA_OFFER_ERROR_INVALID_FINISH;
    }
    CHECK(ct_expect_error(&c.s.k, object, code));
    ct_close(&c.s.k);
    return true;
}

/* a's source never writes: b's paste waits, nobody else does. */
static bool slow_source(struct cs *t, struct dc *a, struct dc *b)
{
    static const char *const types[] = { TEXT };
    uint32_t s = source(a, types, 1);
    CHECK_ST(jwl_wl_data_device_set_selection(a->s.k.c, a->dev, s, enter_serial(a)), OK);
    CHECK_ST(ct_roundtrip(&a->s.k), OK);
    CHECK(window_told_first(b, 300, 1));
    handle_t mine;
    CHECK(receive(b, told(b), TEXT, &mine));
    CHECK(ct_await(&a->s.k, &jwl_wl_data_source_interface, JWL_WL_DATA_SOURCE_EV_SEND, s,
                   CT_WAIT));
    /* a holds its end and writes nothing: the compositor goes on */
    ct_clear(&b->s.k);
    CHECK(cs_tap(t, U_B, 0));
    CHECK(cs_sync(&b->s, NULL));
    const uint32_t keys[] = { DOWN(KEY_B), UP(KEY_B) };
    CHECK(keys_are(&b->s.k, keys, 2));
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(mine, SIG_READABLE | SIG_PEER_CLOSED, now() + 50 * NS_PER_MS,
                                 &seen), ERR_TIMED_OUT);
    jam_handle_close(a->s.k.kept);   /* the source gives up: the reader sees the end */
    a->s.k.kept = HANDLE_INVALID;
    return refused(mine);
}

/* A selection of no text type: the focused client is told "none". */
static bool no_text(struct dc *b, uint32_t serial)
{
    static const char *const types[] = { "image/png" };
    uint32_t s = source(b, types, 1);
    CHECK(s);
    ct_clear(&b->s.k);
    CHECK_ST(jwl_wl_data_device_set_selection(b->s.k.c, b->dev, s, serial), OK);
    CHECK_ST(ct_roundtrip(&b->s.k), OK);
    CHECK_EQ(told(b), 0);
    CHECK(!ct_find(&b->s.k, &jwl_wl_data_source_interface, JWL_WL_DATA_SOURCE_EV_CANCELLED, s));
    CHECK_ST(jwl_wl_data_source_destroy(b->s.k.c, s), OK);
    CHECK_ST(ct_roundtrip(&b->s.k), OK);
    return true;
}

/* A client that keeps every offer: past the cap it is told "none". */
static bool offers_capped(struct dc *b, uint32_t serial)
{
    static const char *const types[] = { TEXT };
    uint32_t last = 0;
    for (unsigned i = 0; i < 20; i++) {
        uint32_t s = source(b, types, 1);
        CHECK(s);
        ct_clear(&b->s.k);
        CHECK_ST(jwl_wl_data_device_set_selection(b->s.k.c, b->dev, s, serial), OK);
        CHECK_ST(ct_roundtrip(&b->s.k), OK);
        CHECK(!b->s.k.errored);
        last = told(b);
        if (last) {   /* sources stay few; its "none" read before the next round */
            CHECK_ST(jwl_wl_data_source_destroy(b->s.k.c, s), OK);
            CHECK_ST(ct_roundtrip(&b->s.k), OK);
        }
    }
    CHECK_EQ(last, 0);   /* DATA_OBJS_MAX offers held: no more made */
    return true;
}

bool t_comp_data_rules(void)
{
    static struct dc a, b;
    struct cs t;
    CHECK(cs_start(&t));
    for (unsigned rule = 0; rule < 3; rule++)
        CHECK(broken(&t, rule));
    CHECK(data_client(&t, &a));
    CHECK(data_client(&t, &b));
    CHECK(window_told_first(&a, 50, 0));
    CHECK(slow_source(&t, &a, &b));
    uint32_t serial = enter_serial(&b);   /* its last key press */
    CHECK(serial);
    CHECK(no_text(&b, serial));
    CHECK(offers_capped(&b, serial));
    ct_close(&a.s.k);
    ct_close(&b.s.k);
    CHECK(cs_stop(&t));
    return true;
}
