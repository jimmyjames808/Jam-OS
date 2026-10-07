/* utest: windows back in their places after the compositor's restart,
 * end to end: libjwl's clients (<jwl_client.h>) against the real
 * bin/compositor, headless and tiling, started as init starts it with a
 * state VMO the test keeps (SR_STATE, <compstate.h>).
 *
 * t_jwlc_restore: three programs, a window each, opened in turn (a, b,
 * c): each new window splits the focused one, the newest, so a has the
 * left half, b the top right and c the bottom right. Each window's key
 * (jam_window_memory_v1) comes, three different ones. The compositor is
 * killed and started again with the same state; the programs reconnect
 * in the other order (c, then b, then a: each one's window configured on
 * the new connection before the next program even connects). Each
 * window's first configure is its old size, so its old tile (in that
 * order a new compositor would have given c the whole room, then halves):
 * the keys are kept. The last compositor ends cleanly, its job empty. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <compstate.h>
#include <idl/svc.h>
#include <jwl_client.h>
#include <os.h>
#include <svcstate.h>
#include "comptest.h"
#include "utest.h"

#define W 640
#define H 400

struct restore {
    struct ct_comp p;
    handle_t state;                /* ours, kept across the restart (init's) */
    unsigned starts;
};

/* The compositor, started again with the state if the last one died, and
 * a connection. */
static status_t restore_connect(void *ctx, handle_t *out)
{
    struct restore *r = ctx;
    signals_t seen = 0;
    if (jam_object_wait_one(r->p.svc, SIG_PEER_CLOSED, 0, &seen) == OK &&
        (seen & SIG_PEER_CLOSED)) {
        struct process_info info;
        (void)spawn_wait(r->p.proc, CT_WAIT, &info);   /* killed: it has ended */
        jam_handle_close(r->p.proc);
        jam_handle_close(r->p.job);
        jam_handle_close(r->p.svc);
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)r->p.image,
                             r->p.image_size);
        if (!ct_start_state(&r->p, W, H, "layout=tiling", r->state))
            return ERR_INTERNAL;
        r->starts++;
    }
    return svc_connect_within(r->p.svc, CT_WAIT, out);
}

/* Dispatch c until done(w) or CT_WAIT. */
static bool until(struct jwl_client *c, struct jwl_window *w,
                  bool (*done)(struct jwl_client *, struct jwl_window *))
{
    uint64_t deadline = now() + CT_WAIT;
    while (!done(c, w) && now() < deadline) {
        handle_t ch = jwl_client_channel(c);
        signals_t seen;
        if (ch != HANDLE_INVALID)
            (void)jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED,
                                      now() + 5 * NS_PER_MS, &seen);
        else
            (void)jam_nanosleep(now() + NS_PER_MS);
        CHECK_ST(jwl_client_dispatch(c), OK);
        struct jwl_event ev;
        while (jwl_client_next_event(c, &ev) == OK) {
        }
    }
    return done(c, w);
}

static bool configured(struct jwl_client *c, struct jwl_window *w)
{
    (void)c;
    return jwl_window_configured(w);
}

static bool configured_again(struct jwl_client *c, struct jwl_window *w)
{
    return jwl_client_info(c)->generation == 2 && jwl_client_status(c) == OK &&
           jwl_window_configured(w);
}

static bool keyed(struct jwl_client *c, struct jwl_window *w)
{
    (void)c;
    return jwl_window_key(w) != 0;
}

/* A window drawn at its size and shown. */
static bool draw(struct jwl_client *c, struct jwl_window *w)
{
    struct jwl_frame f;
    CHECK_ST(jwl_window_begin(w, &f), OK);
    for (int32_t y = 0; y < f.height; y++)
        for (int32_t x = 0; x < f.width; x++)
            f.px[y * f.stride / 4 + x] = 0x203040;
    CHECK_ST(jwl_window_present(w, NULL, 0, false), OK);
    CHECK_ST(jwl_client_roundtrip(c, now() + CT_WAIT), OK);
    return true;
}

bool t_jwlc_restore(void)
{
    struct restore r = { .starts = 1 };
    CHECK_ST(svcstate_create(COMP_STATE_SIZE, &r.state), OK);
    CHECK(ct_start_state(&r.p, W, H, "layout=tiling", r.state));
    struct jwl_client *c[3];
    struct jwl_window *w[3];
    int32_t ww[3], wh[3];
    uint64_t key[3];
    static const char *const names[3] = { "utest-restore-a", "utest-restore-b", "utest-restore-c" };
    for (unsigned i = 0; i < 3; i++) {
        struct jwl_client_config cfg = { .connect = restore_connect, .connect_ctx = &r,
                                         .name = names[i], .quiet = true };
        CHECK_ST(jwl_client_connect(&cfg, now() + CT_WAIT, &c[i]), OK);
        CHECK_EQ(jwl_client_info(c[i])->memory_version, 1);
        struct jwl_window_config wc = { .width = 100, .height = 100, .title = "Same",
                                        .resizable = true };
        CHECK_ST(jwl_window_create(c[i], &wc, &w[i]), OK);
        CHECK(until(c[i], w[i], configured) && until(c[i], w[i], keyed));
        CHECK(draw(c[i], w[i]));
        key[i] = jwl_window_key(w[i]);
    }
    for (unsigned i = 0; i < 3; i++) {   /* each window's latest configure (a and b were split) */
        CHECK_ST(jwl_client_roundtrip(c[i], now() + CT_WAIT), OK);
        jwl_window_size(w[i], &ww[i], &wh[i]);
    }
    CHECK(key[0] != key[1] && key[1] != key[2] && key[0] != key[2]);
    /* a | [b / c]: a full height, b and c a quarter each */
    CHECK(wh[0] > wh[1] + 100 && ww[1] == ww[2] && wh[1] < H / 2);
    (void)jam_nanosleep(now() + 50 * NS_PER_MS);   /* a loop turn writes the state */

    CHECK_ST(jam_process_kill(r.p.proc), OK);
    for (unsigned i = 3; i-- > 0;) {   /* c, b, a: each back before the next connects */
        CHECK(until(c[i], w[i], configured_again));
        int32_t nw, nh;
        jwl_window_size(w[i], &nw, &nh);
        if (nw != ww[i] || nh != wh[i])
            FAIL("window %u came back %dx%d, was %dx%d", i, nw, nh, ww[i], wh[i]);
        CHECK_EQ(jwl_window_key(w[i]), key[i]);
    }
    CHECK_EQ(r.starts, 2);

    for (unsigned i = 0; i < 3; i++) {
        jwl_window_destroy(w[i]);
        jwl_client_destroy(c[i]);
    }
    CHECK(ct_stop(&r.p));
    CHECK_ST(jam_handle_close(r.state), OK);
    return true;
}
