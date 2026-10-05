/* utest: libjwl's client side (<jwl_client.h>) against the real
 * bin/compositor, run headless as comp.c runs it (comptest.h).
 *
 * t_jwlc_real_compositor: the client binds what the compositor offers
 * (wl_compositor 4, wl_shm 1 with both formats, wl_output 3 with the
 * headless size); a kept pool the compositor maps and two buffers in it;
 * a surface of the test's own with a buffer attached and a frame callback
 * answered (jwl_client_frame), the buffer a newer commit replaced
 * released. Then the compositor is killed: the connection goes, the
 * client's connect function starts another (as init restarts the real
 * one), and libjwl binds again and makes the pool and buffers again,
 * which the next surface uses at once. The last compositor ends cleanly
 * when its /svc/wayland closes, its job empty. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/svc.h>
#include <jwl_client.h>
#include <os.h>
#include "comptest.h"
#include "jwlcfake.h"
#include "utest.h"

#define SIDE 32

struct real {
    struct ct_comp p;
    unsigned starts;
};

/* The compositor, started again if the last one died, and a connection. */
static status_t real_connect(void *ctx, handle_t *out)
{
    struct real *r = ctx;
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
        if (!ct_start(&r->p, 320, 200))
            return ERR_INTERNAL;
        r->starts++;
    }
    return svc_connect_within(r->p.svc, CT_WAIT, out);
}

/* Dispatch until *flag (or CT_WAIT). */
static bool until_flag(struct jwl_client *c, const bool *flag)
{
    uint64_t deadline = now() + CT_WAIT;
    while (!*flag && now() < deadline) {
        signals_t seen;
        handle_t ch = jwl_client_channel(c);
        if (ch != HANDLE_INVALID)
            (void)jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED,
                                      now() + 5 * NS_PER_MS, &seen);
        else
            (void)jam_nanosleep(now() + NS_PER_MS);
        CHECK_ST(jwl_client_dispatch(c), OK);
    }
    return *flag;
}

/* A surface of our own: b[0] with a frame callback, then b[1]; b[0]
 * released. */
static bool surface_round(struct jwl_client *c, struct jwl_buffer *const *b)
{
    struct jwl_conn *k = jwl_client_conn(c);
    uint32_t sid;
    CHECK(k);
    CHECK_ST(jwl_conn_make(k, &jwl_wl_surface_interface, 4, NULL, &sid), OK);
    CHECK_ST(jwl_wl_compositor_create_surface(k, jwl_client_global(c,
                                                  &jwl_wl_compositor_interface), sid), OK);
    struct jwl_callback cb = { 0 };
    CHECK_ST(jwl_wl_surface_attach(k, sid, jwl_buffer_id(b[0]), 0, 0), OK);
    jwl_buffer_attached(b[0]);
    CHECK_ST(jwl_client_frame(c, sid, &cb), OK);   /* a version 4 wl_callback */
    CHECK_ST(jwl_wl_surface_commit(k, sid), OK);
    CHECK_ST(jwl_client_flush(c), OK);
    CHECK(until_flag(c, &cb.done));
    CHECK_ST(jwl_wl_surface_attach(k, sid, jwl_buffer_id(b[1]), 0, 0), OK);
    jwl_buffer_attached(b[1]);
    CHECK_ST(jwl_wl_surface_commit(k, sid), OK);
    CHECK_ST(jwl_client_roundtrip(c, now() + CT_WAIT), OK);
    CHECK(!jwl_buffer_busy(b[0]) && jwl_buffer_busy(b[1]));
    CHECK_ST(jwl_wl_surface_destroy(k, sid), OK);
    CHECK_ST(jwl_client_roundtrip(c, now() + CT_WAIT), OK);
    CHECK(!jwl_buffer_busy(b[1]));   /* its surface went */
    return true;
}

bool t_jwlc_real_compositor(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct real r = { .starts = 1 };
    CHECK(ct_start(&r.p, 320, 200));
    struct jwl_client_config cfg = { .connect = real_connect, .connect_ctx = &r,
                                     .name = "utest-jwl", .quiet = true };
    struct jwl_client *c;
    CHECK_ST(jwl_client_connect(&cfg, now() + CT_WAIT, &c), OK);
    const struct jwl_client_info *in = jwl_client_info(c);
    CHECK(in->compositor_version == 4 && in->shm_version == 1 && in->output_version == 3);
    CHECK(in->output_width == 320 && in->output_height == 200 && (in->shm_formats & 3) == 3);
    struct jwl_pool *pool;
    struct jwl_buffer *b[2];
    CHECK_ST(jwl_pool_create(c, 2 * SIDE * SIDE * 4, &pool), OK);
    for (unsigned i = 0; i < 2; i++)
        CHECK_ST(jwl_buffer_create(pool, i * SIDE * SIDE * 4, SIDE, SIDE, SIDE * 4,
                                   JWL_WL_SHM_FORMAT_XRGB8888, &b[i]), OK);
    CHECK(surface_round(c, b));

    CHECK_ST(jam_process_kill(r.p.proc), OK);
    uint64_t deadline = now() + CT_WAIT;
    while ((in->generation < 2 || jwl_client_status(c) != OK) && now() < deadline) {
        handle_t ch = jwl_client_channel(c);
        signals_t seen;
        if (ch != HANDLE_INVALID)
            (void)jam_object_wait_one(ch, SIG_PEER_CLOSED, now() + 5 * NS_PER_MS, &seen);
        else
            (void)jam_nanosleep(jwl_client_deadline(c));
        CHECK_ST(jwl_client_dispatch(c), OK);
    }
    CHECK(in->generation == 2 && r.starts == 2);   /* and ready again */
    CHECK(jwl_buffer_id(b[0]) && jwl_buffer_id(b[1]));   /* made again */
    CHECK(surface_round(c, b));
    struct jwl_event ev;
    CHECK(fake_next_of(c, JWL_EV_DISCONNECTED, &ev) && ev.conn.why == ERR_PEER_CLOSED);
    CHECK(fake_next_of(c, JWL_EV_RECONNECTED, &ev));

    for (unsigned i = 0; i < 2; i++)
        jwl_buffer_destroy(b[i]);
    jwl_pool_destroy(pool);
    jwl_client_destroy(c);
    CHECK(ct_stop(&r.p));
    uint64_t h1, b1;
    fake_held(&h1, &b1);
    CHECK_EQ(h1, h0);
    CHECK_EQ(b1, b0);
    return true;
}
