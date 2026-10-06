/* utest: libjwl's clipboard (<jwl_client.h>, user/lib/jwl_data.c) against
 * the real bin/compositor, headless (the seat's harness, compseat.h).
 *
 * t_jwlc_clip: a client copies text and pastes its own selection; copies
 * 300 KiB and another client, whose window then takes the focus, pastes
 * it (more than one message of the transfer); a raw client's selection
 * replaces it (the first client hears its copy was cancelled); a fourth
 * client pastes from that raw owner, which never writes (the paste times
 * out after JWL_CLIP_WAIT_NS and nothing else waits), writes one byte over
 * JWL_CLIP_MAX (refused), or sends a handle (refused). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/svc.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl_client.h>
#include <os.h>
#include "compseat.h"
#include "utest.h"

#define BIG (300u << 10)
#define TEXT_TYPE "text/plain;charset=utf-8"

struct cl {
    struct jwl_client *c;
    struct jwl_window *w;
};

static struct cl *all[3];   /* the libjwl clients the pump dispatches */

static status_t connect_to(void *ctx, handle_t *out)
{
    struct cs *t = ctx;
    return svc_connect_within(t->p.svc, CT_WAIT, out);
}

/* Every libjwl client's messages read and its paste looked at, once. */
static void pump_all(void)
{
    for (unsigned i = 0; i < 3; i++)
        if (all[i] && all[i]->c)
            (void)jwl_client_dispatch(all[i]->c);
}

/* The next event of type for k (others of k's dropped), pumping every
 * client, within timeout. */
static bool next_of(struct cl *k, uint32_t type, uint64_t timeout, struct jwl_event *out)
{
    uint64_t deadline = now() + timeout;
    while (now() < deadline) {
        pump_all();
        while (jwl_client_next_event(k->c, out) == OK)
            if (out->type == type)
                return true;
        jam_nanosleep(now() + NS_PER_MS);
    }
    FAIL("no event %u within %lu ms", type, (unsigned long)(timeout / NS_PER_MS));
}

/* Pump until k may paste (its SELECTION event may have come already). */
static bool until_available(struct cl *k)
{
    uint64_t deadline = now() + CT_WAIT;
    while (!jwl_clip_available(k->c) && now() < deadline) {
        pump_all();
        jam_nanosleep(now() + NS_PER_MS);
    }
    CHECK(jwl_clip_available(k->c));
    return true;
}

/* A client in slot with a window of its own, which takes the focus. */
static bool client_window(struct cs *t, struct cl *k, unsigned slot)
{
    struct jwl_client_config cfg = { .connect = connect_to, .connect_ctx = t,
                                     .name = "utest-clip", .quiet = true, .no_reconnect = true };
    CHECK_ST(jwl_client_connect(&cfg, now() + CT_WAIT, &k->c), OK);
    CHECK_EQ(jwl_client_info(k->c)->data_version, 3);
    all[slot] = k;
    struct jwl_window_config wc = { .width = 64, .height = 48, .title = "clip" };
    CHECK_ST(jwl_window_create(k->c, &wc, &k->w), OK);
    struct jwl_event ev;
    CHECK(next_of(k, JWL_EV_CONFIGURE, CT_WAIT, &ev));
    struct jwl_frame f;
    CHECK_ST(jwl_window_begin(k->w, &f), OK);
    memset(f.px, 0x40, (size_t)f.stride * (size_t)f.height);
    CHECK_ST(jwl_window_present(k->w, NULL, 0, false), OK);
    CHECK(next_of(k, JWL_EV_KEYBOARD_ENTER, CT_WAIT, &ev));
    return true;
}

/* k pastes: the result's status, and the text equal to want (n bytes) if OK. */
static bool paste_is(struct cl *k, status_t want_st, const char *want, size_t n)
{
    CHECK_ST(jwl_clip_paste(k->c), OK);
    CHECK_ST(jwl_clip_paste(k->c), ERR_BAD_STATE);   /* one at a time */
    struct jwl_event ev;
    CHECK(next_of(k, JWL_EV_PASTE, JWL_CLIP_WAIT_NS + CT_WAIT, &ev));
    CHECK_ST(ev.clip.status, want_st);
    if (want_st != OK)
        return true;
    size_t got = 0;
    const char *text = jwl_clip_pasted(k->c, &got);
    CHECK(text && got == n && ev.clip.size == n && !memcmp(text, want, n) && !text[n]);
    return true;
}

/* A copies and pastes its own text; then 300 KiB, which B pastes. */
static bool copy_and_paste(struct cs *t, struct cl *a, struct cl *b, char *big)
{
    static const char small[] = "hello\nclip";
    CHECK(client_window(t, a, 0));
    CHECK_ST(jwl_clip_copy(a->c, small, sizeof(small) - 1), OK);
    struct jwl_event ev;
    do
        CHECK(next_of(a, JWL_EV_SELECTION, CT_WAIT, &ev));
    while (!ev.clip.available);
    CHECK(paste_is(a, OK, small, sizeof(small) - 1));
    for (uint32_t i = 0; i < BIG; i++)
        big[i] = (char)('a' + i % 26);
    CHECK_ST(jwl_clip_copy(a->c, big, BIG), OK);
    CHECK_ST(jwl_clip_copy(a->c, big, JWL_CLIP_MAX + 1), ERR_OUT_OF_RANGE);
    CHECK(client_window(t, b, 1));
    CHECK(until_available(b));
    return paste_is(b, OK, big, BIG);
}

/* A raw client takes the selection (A's copy cancelled). */
static bool raw_owner(struct cs *t, struct sc *r, struct cl *a, uint32_t *src)
{
    CHECK(cs_client(t, r));
    struct ct_client *k = &r->k;
    if (k->kept != HANDLE_INVALID)   /* the keymap's */
        jam_handle_close(k->kept);
    k->kept = HANDLE_INVALID;
    uint32_t mgr = ct_new(k, &jwl_wl_data_device_manager_interface, 3);
    uint32_t dev = ct_new(k, &jwl_wl_data_device_interface, 3);
    CHECK(mgr && dev);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 7, "wl_data_device_manager", 3, mgr), OK);
    CHECK_ST(jwl_wl_data_device_manager_get_data_device(k->c, mgr, dev, r->seat), OK);
    CHECK(cs_window(r, 10, 10, 20, 20));
    const struct ct_event *e = ct_await(k, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER,
                                        r->kb, CT_WAIT);
    CHECK(e);
    uint32_t serial = e->u[0];
    CHECK((*src = ct_new(k, &jwl_wl_data_source_interface, 3)) != 0);
    CHECK_ST(jwl_wl_data_device_manager_create_data_source(k->c, mgr, *src), OK);
    CHECK_ST(jwl_wl_data_source_offer(k->c, *src, TEXT_TYPE), OK);
    CHECK_ST(jwl_wl_data_device_set_selection(k->c, dev, *src, serial), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    struct jwl_event ev;
    return next_of(a, JWL_EV_COPY_CANCELLED, CT_WAIT, &ev);
}

/* The raw owner's end of the next transfer, once e's paste asked. */
static handle_t owner_end(struct sc *r, uint32_t src)
{
    ct_clear(&r->k);
    if (!ct_await(&r->k, &jwl_wl_data_source_interface, JWL_WL_DATA_SOURCE_EV_SEND, src, CT_WAIT))
        return HANDLE_INVALID;
    handle_t w = r->k.kept;
    r->k.kept = HANDLE_INVALID;
    return w;
}

/* E pastes from the raw owner: too slow, too much, a handle. */
static bool hostile_owner(struct sc *r, uint32_t src, struct cl *e, char *big)
{
    struct jwl_event ev;
    CHECK(until_available(e));
    uint64_t t0 = now();
    CHECK_ST(jwl_clip_paste(e->c), OK);
    handle_t w = owner_end(r, src);
    CHECK(w != HANDLE_INVALID);
    CHECK(next_of(e, JWL_EV_PASTE, JWL_CLIP_WAIT_NS + CT_WAIT, &ev));
    CHECK_ST(ev.clip.status, ERR_TIMED_OUT);
    CHECK(now() - t0 >= JWL_CLIP_WAIT_NS);
    jam_handle_close(w);
    /* one byte over the cap */
    CHECK_ST(jwl_clip_paste(e->c), OK);
    CHECK((w = owner_end(r, src)) != HANDLE_INVALID);
    for (uint32_t at = 0; at <= JWL_CLIP_MAX; at += JWL_CLIP_CHUNK) {
        uint32_t n = JWL_CLIP_MAX + 1 - at < JWL_CLIP_CHUNK ? JWL_CLIP_MAX + 1 - at : JWL_CLIP_CHUNK;
        CHECK_ST(jam_channel_write(w, big, n, NULL, 0), OK);   /* big has JWL_CLIP_CHUNK */
    }
    jam_handle_close(w);
    CHECK(next_of(e, JWL_EV_PASTE, CT_WAIT, &ev));
    CHECK_ST(ev.clip.status, ERR_OUT_OF_RANGE);
    /* a handle in the transfer */
    CHECK_ST(jwl_clip_paste(e->c), OK);
    CHECK((w = owner_end(r, src)) != HANDLE_INVALID);
    handle_t extra;
    CHECK_ST(jam_event_create(&extra), OK);
    CHECK_ST(jam_channel_write(w, "x", 1, &extra, 1), OK);
    jam_handle_close(w);
    CHECK(next_of(e, JWL_EV_PASTE, CT_WAIT, &ev));
    CHECK_ST(ev.clip.status, ERR_INVALID_ARGS);
    CHECK(!jwl_clip_pasted(e->c, &(size_t){ 0 }));
    return true;
}

bool t_jwlc_clip(void)
{
    static struct cl a, b, e;
    static struct sc r;
    static char big[BIG];
    struct cs t;
    memset(all, 0, sizeof(all));
    CHECK(cs_start(&t));
    CHECK(copy_and_paste(&t, &a, &b, big));
    uint32_t src;
    CHECK(raw_owner(&t, &r, &a, &src));
    CHECK(client_window(&t, &e, 2));
    CHECK(hostile_owner(&r, src, &e, big));
    for (unsigned i = 0; i < 3; i++)
        jwl_client_destroy(all[i]->c);
    memset(all, 0, sizeof(all));
    ct_close(&r.k);
    CHECK(cs_stop(&t));
    return true;
}
