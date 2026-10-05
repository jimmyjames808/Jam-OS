/* utest: libjwl's transport (<jwl.h>) over real channels, a client and a
 * compositor connection in one process: requests and events both ways
 * with handles for fds, new ids and the delete_id handshake, batching
 * (16 KiB and 16 handles a batch), the flow-control window and the held
 * bytes behind it (a client that reads late catches up; one that never
 * reads is disconnected with no_memory, and reads the error), the
 * half-window acknowledgement, every malformed batch refused with
 * wl_display.error, and jwl_conn_send's own refusals. Each test ends with
 * the job's handles and message bytes back where they started. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <os.h>
#include "jwltest.h"
#include "utest.h"

#define POKED_PER_BATCH ((JWL_BATCH_MAX - JWL_HEADER_BYTES) / 12)   /* 1364 */

struct pair {
    struct jwl_conn *cl, *sv;
};

/* What the job holds: handles and message bytes. */
static void held_now(uint64_t *handles, uint64_t *bytes)
{
    struct job_info ji;
    *handles = *bytes = 0;
    if (jam_job_get_info(own_job(), &ji) == OK) {
        *handles = ji.used[JOB_LIMIT_HANDLES];
        *bytes = ji.used[JOB_LIMIT_MSG_BYTES];
    }
}

static status_t conn_on(handle_t ch, enum jwl_side side, struct jwl_conn **out)
{
    struct jwl_conn_config cfg = { .ch = ch, .side = side, .display = &jt_display,
                                   .known = jt_known, .nknown = JT_NKNOWN };
    return jwl_conn_create(&cfg, out);
}

/* A connected pair with wl_registry at 2, jt_all (version 3) at 3 and a
 * jt_thing at 4, made by the client and read by the compositor. */
static bool pair_up(struct pair *p)
{
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(conn_on(a, JWL_CLIENT, &p->cl), OK);
    CHECK_ST(conn_on(b, JWL_SERVER, &p->sv), OK);
    union jwl_arg r[2] = { { .n = 0 } };
    CHECK_ST(jwl_conn_send(p->cl, &jt_display, JWL_DISPLAY_ID, 1, r, 1), OK);
    CHECK_EQ(r[0].n, 2);
    r[0].u = 7;
    r[1].any = (struct jwl_new_any){ .iface = &jt_all, .version = 3 };
    CHECK_ST(jwl_conn_send(p->cl, &jt_registry, 2, 0, r, 2), OK);
    CHECK_EQ(r[1].any.id, 3);
    CHECK_ST(jwl_conn_send(p->cl, &jt_all, 3, JT_MAKE, r, 1), OK);
    CHECK_EQ(r[0].n, 4);
    CHECK_ST(jwl_conn_flush(p->cl), OK);
    struct jwl_msg m;
    for (unsigned i = 0; i < 3; i++)
        CHECK_ST(jwl_conn_next(p->sv, &m), OK);
    CHECK(m.id == 3 && m.opcode == JT_MAKE && jwl_conn_object(p->sv, 4)->version == 3);
    CHECK_ST(jwl_conn_next(p->sv, &m), ERR_SHOULD_WAIT);
    return true;
}

static void pair_down(struct pair *p)
{
    jwl_conn_destroy(p->cl);
    jwl_conn_destroy(p->sv);
}

static handle_t new_vmo(void)
{
    handle_t h = HANDLE_INVALID;
    (void)jam_vmo_create(PAGE_SIZE, 0, HANDLE_INVALID, &h);   /* checked by its user */
    return h;
}

bool t_jwl_conn_messages(void)
{
    uint64_t h0, b0, h1, b1;
    held_now(&h0, &b0);
    struct pair p;
    if (!pair_up(&p))
        return false;
    /* sync: the compositor answers done, destroys the callback, delete_id */
    union jwl_arg a[3] = { { .n = 0 } };
    CHECK_ST(jwl_conn_send(p.cl, &jt_display, JWL_DISPLAY_ID, 0, a, 1), OK);
    uint32_t cb = a[0].n;
    CHECK_EQ(cb, 5);
    a[0].h = new_vmo();
    a[1].h = new_vmo();
    CHECK(a[0].h != HANDLE_INVALID && a[1].h != HANDLE_INVALID);
    CHECK_ST(jwl_conn_send(p.cl, &jt_all, 3, JT_GIVE, a, 2), OK);
    CHECK_ST(jwl_conn_flush(p.cl), OK);
    struct jwl_msg m;
    CHECK_ST(jwl_conn_next(p.sv, &m), OK);
    CHECK(m.id == JWL_DISPLAY_ID && m.opcode == 0 && m.args[0].n == cb);
    CHECK_ST(jwl_conn_next(p.sv, &m), OK);
    CHECK(m.opcode == JT_GIVE && m.nhandles == 2);
    uint64_t size = 0;
    CHECK_ST(jam_vmo_get_size(m.args[1].h, &size), OK);
    CHECK_EQ(size, PAGE_SIZE);
    handle_t back = m.args[0].h;   /* one goes back in an event, one is closed */
    jam_handle_close(m.args[1].h);
    a[0].u = 42;
    CHECK_ST(jwl_conn_send(p.sv, &jt_callback, cb, 0, a, 1), OK);
    CHECK_ST(jwl_conn_delete(p.sv, cb), OK);
    a[0].h = back;
    CHECK_ST(jwl_conn_send(p.sv, &jt_all, 3, JT_EV_HANDLE, a, 1), OK);
    a[0].u = 7;
    a[1].s = "jt_all";
    a[2].u = 3;
    CHECK_ST(jwl_conn_send(p.sv, &jt_registry, 2, 0, a, 3), OK);
    CHECK_ST(jwl_conn_flush(p.sv), OK);
    CHECK_ST(jwl_conn_next(p.cl, &m), OK);
    CHECK(m.id == cb && m.args[0].u == 42);
    CHECK_ST(jwl_conn_next(p.cl, &m), OK);   /* delete_id was taken on the way */
    CHECK(m.opcode == JT_EV_HANDLE && m.nhandles == 1);
    CHECK_ST(jam_vmo_get_size(m.args[0].h, &size), OK);
    jwl_msg_close_handles(&m);
    CHECK_ST(jwl_conn_next(p.cl, &m), OK);
    CHECK(m.id == 2 && !strcmp(m.args[1].s, "jt_all") && m.args[2].u == 3);
    CHECK_ST(jwl_conn_next(p.cl, &m), ERR_SHOULD_WAIT);
    CHECK(jwl_map_entry(&p.cl->map, cb)->deleted);
    CHECK_ST(jwl_conn_delete(p.cl, cb), OK);   /* free at once: delete_id came first */
    CHECK_ST(jwl_conn_send(p.cl, &jt_display, JWL_DISPLAY_ID, 0, a, 1), OK);
    CHECK_EQ(a[0].n, cb);
    CHECK(p.cl->stats.msgs_out == 6 && p.sv->stats.msgs_in == 5 && p.cl->stats.msgs_in == 4);
    pair_down(&p);
    held_now(&h1, &b1);
    CHECK_EQ(h1, h0);
    CHECK_EQ(b1, b0);
    return true;
}

/* Many messages and handles: batches of at most 16 KiB and 16 handles,
 * every message arriving once, in order. */
bool t_jwl_conn_batches(void)
{
    uint64_t h0, b0, h1, b1;
    held_now(&h0, &b0);
    struct pair p;
    if (!pair_up(&p))
        return false;
    union jwl_arg a[JWL_ARGS_MAX];
    uint64_t before = p.cl->stats.batches_out;
    for (unsigned i = 0; i < 3000; i++) {
        for (unsigned k = 0; k < JWL_ARGS_MAX; k++)
            a[k].u = i + k;
        CHECK_ST(jwl_conn_send(p.cl, &jt_all, 3, JT_MAX, a, JWL_ARGS_MAX), OK);
    }
    for (unsigned i = 0; i < 40; i++) {
        a[0].h = new_vmo();
        a[1].h = new_vmo();
        CHECK_ST(jwl_conn_send(p.cl, &jt_all, 3, JT_GIVE, a, 2), OK);
    }
    CHECK_ST(jwl_conn_flush(p.cl), OK);
    uint64_t batches = p.cl->stats.batches_out - before;
    /* 186 88-byte messages fill a batch: 17 batches, the last with 24 and
     * room for 8 two-handle messages; the other 32 need 4 more batches */
    CHECK_EQ(batches, 21);
    struct jwl_msg m;
    for (unsigned i = 0; i < 3000; i++) {
        CHECK_ST(jwl_conn_next(p.sv, &m), OK);
        if (m.opcode != JT_MAX || m.args[0].u != i || m.args[19].u != i + 19)
            FAIL("message %u came as op %u, %u", i, m.opcode, m.args[0].u);
    }
    for (unsigned i = 0; i < 40; i++) {
        CHECK_ST(jwl_conn_next(p.sv, &m), OK);
        CHECK(m.opcode == JT_GIVE && m.nhandles == 2);
        uint64_t size;
        CHECK_ST(jam_vmo_get_size(m.args[1].h, &size), OK);
        jwl_msg_close_handles(&m);
    }
    CHECK_ST(jwl_conn_next(p.sv, &m), ERR_SHOULD_WAIT);
    CHECK_EQ(p.sv->stats.batches_in, batches + 1);   /* + pair_up's */
    pair_down(&p);
    held_now(&h1, &b1);
    CHECK_EQ(h1, h0);
    CHECK_EQ(b1, b0);
    return true;
}

/* The compositor sends n jt_thing.poked events numbered from *seq. */
static status_t poke(struct pair *p, unsigned n, uint32_t *seq)
{
    status_t st = OK;
    for (unsigned i = 0; i < n && st == OK; i++) {
        union jwl_arg a = { .u = (*seq)++ };
        st = jwl_conn_send(p->sv, &jt_thing, 4, 0, &a, 1);
    }
    return st;
}

/* The client reads every queued event, checking they come numbered from
 * *seq; how many. */
static unsigned drain(struct pair *p, uint32_t *seq, bool *in_order)
{
    struct jwl_msg m;
    unsigned n = 0;
    while (jwl_conn_next(p->cl, &m) == OK) {
        if (m.id != 4 || m.args[0].u != (*seq)++)
            *in_order = false;
        n++;
    }
    return n;
}

/* A client that reads late: the window fills, batches are held, and once
 * it reads and acknowledges every event arrives, in order. */
bool t_jwl_conn_window(void)
{
    uint64_t h0, b0, h1, b1;
    held_now(&h0, &b0);
    struct pair p;
    if (!pair_up(&p))
        return false;
    uint32_t sent = 0, got = 0;
    bool in_order = true;
    CHECK_ST(poke(&p, 34 * POKED_PER_BATCH, &sent), OK);
    CHECK_ST(jwl_conn_flush(p.sv), OK);
    CHECK_EQ(p.sv->stats.batches_out, JWL_WINDOW);
    CHECK(jwl_conn_backlogged(p.sv));
    CHECK_EQ(p.sv->held_bytes, 2 * JWL_BATCH_MAX);
    CHECK_EQ(drain(&p, &got, &in_order), JWL_WINDOW * POKED_PER_BATCH);
    uint64_t out = p.cl->stats.batches_out;
    CHECK_ST(jwl_conn_flush(p.cl), OK);   /* nothing to say: an acknowledgement alone */
    CHECK_EQ(p.cl->stats.batches_out, out + 1);
    struct jwl_msg m;
    CHECK_ST(jwl_conn_next(p.sv, &m), ERR_SHOULD_WAIT);
    CHECK_EQ(p.sv->peer_acked, JWL_WINDOW);
    CHECK_ST(jwl_conn_flush(p.sv), OK);
    CHECK(!jwl_conn_backlogged(p.sv) && p.sv->held_bytes == 0);
    CHECK_EQ(drain(&p, &got, &in_order), 2 * POKED_PER_BATCH);
    CHECK(in_order && got == sent);
    CHECK_EQ(p.sv->stats.held_peak, 2 * JWL_BATCH_MAX);
    /* an acknowledgement goes when half the window is read, not before;
     * a request first, so the count told is the count read */
    union jwl_arg a = { .u = 1 };
    CHECK_ST(jwl_conn_send(p.cl, &jt_thing, 4, 0, &a, 1), OK);
    CHECK_ST(jwl_conn_flush(p.cl), OK);
    CHECK_EQ(p.cl->told, p.cl->nread);
    CHECK_ST(poke(&p, 15 * POKED_PER_BATCH, &sent), OK);
    CHECK_ST(jwl_conn_flush(p.sv), OK);
    CHECK_EQ(drain(&p, &got, &in_order), 15 * POKED_PER_BATCH);
    out = p.cl->stats.batches_out;
    CHECK_ST(jwl_conn_flush(p.cl), OK);
    CHECK_EQ(p.cl->stats.batches_out, out);
    CHECK_ST(poke(&p, POKED_PER_BATCH, &sent), OK);
    CHECK_ST(jwl_conn_flush(p.sv), OK);
    CHECK_EQ(drain(&p, &got, &in_order), POKED_PER_BATCH);
    CHECK_ST(jwl_conn_flush(p.cl), OK);
    CHECK_EQ(p.cl->stats.batches_out, out + 1);
    CHECK_ST(jwl_conn_flush(p.cl), OK);
    CHECK_EQ(p.cl->stats.batches_out, out + 1);
    CHECK(in_order);
    pair_down(&p);
    held_now(&h1, &b1);
    CHECK_EQ(h1, h0);
    CHECK_EQ(b1, b0);
    return true;
}

/* A client that never reads: the window, 64 KiB held, then no_memory, which
 * the client finds after everything else when it does read. */
bool t_jwl_conn_never_reads(void)
{
    uint64_t h0, b0, h1, b1;
    held_now(&h0, &b0);
    struct pair p;
    if (!pair_up(&p))
        return false;
    uint32_t sent = 0;
    status_t st = OK;
    for (unsigned i = 0; i < 100 && st == OK; i++) {
        st = poke(&p, POKED_PER_BATCH, &sent);
        if (st == OK)
            st = jwl_conn_flush(p.sv);
    }
    CHECK_ST(st, ERR_INVALID_ARGS);
    CHECK(p.sv->error.code == JWL_ERROR_NO_MEMORY && p.sv->error.object == JWL_DISPLAY_ID);
    CHECK_EQ(p.sv->stats.batches_out, JWL_WINDOW + 1);   /* the error past the window */
    CHECK_EQ(p.sv->held_bytes, 0);
    CHECK_ST(jwl_conn_flush(p.sv), ERR_INVALID_ARGS);   /* dead from now on */
    union jwl_arg a = { .u = 1 };
    CHECK_ST(jwl_conn_send(p.sv, &jt_thing, 4, 0, &a, 1), ERR_INVALID_ARGS);
    uint32_t got = 0;
    bool in_order = true;
    struct jwl_msg m;
    unsigned n = 0;
    while (jwl_conn_next(p.cl, &m) == OK && m.id == 4) {
        in_order &= m.args[0].u == got++;
        n++;
    }
    CHECK(in_order && n == JWL_WINDOW * POKED_PER_BATCH);
    CHECK(m.id == JWL_DISPLAY_ID && m.opcode == 0);
    CHECK(m.args[0].o == JWL_DISPLAY_ID && m.args[1].u == JWL_ERROR_NO_MEMORY);
    CHECK(strstr(m.args[2].s, "too slowly") != NULL);
    CHECK_ST(jwl_conn_next(p.cl, &m), ERR_SHOULD_WAIT);
    jwl_conn_destroy(p.sv);
    CHECK_ST(jwl_conn_next(p.cl, &m), ERR_PEER_CLOSED);
    CHECK_ST(jwl_conn_flush(p.cl), ERR_PEER_CLOSED);
    jwl_conn_destroy(p.cl);
    held_now(&h1, &b1);
    CHECK_EQ(h1, h0);
    CHECK_EQ(b1, b0);
    return true;
}

/* jwl_conn_send's refusals; handles are consumed even then. */
bool t_jwl_conn_send_refusals(void)
{
    uint64_t h0, b0, h1, b1;
    held_now(&h0, &b0);
    struct pair p;
    if (!pair_up(&p))
        return false;
    union jwl_arg a[6] = { { .u = 0 } };
    CHECK_ST(jwl_conn_send(p.cl, &jt_thing, 9, 0, a, 1), ERR_NOT_FOUND);
    CHECK_ST(jwl_conn_send(p.cl, &jt_thing, 3, 0, a, 1), ERR_WRONG_TYPE);
    CHECK_ST(jwl_conn_send(p.cl, &jt_all, 3, 99, a, 0), ERR_NOT_SUPPORTED);
    CHECK_ST(jwl_conn_send(p.cl, &jt_all, 3, JT_MAKE, a, 2), ERR_INVALID_ARGS);
    a[0].h = new_vmo();
    a[1].h = new_vmo();
    CHECK_ST(jwl_conn_send(p.cl, &jt_thing, 3, JT_GIVE, a, 2), ERR_NOT_SUPPORTED);   /* thing has 2 */
    CHECK_ST(jwl_conn_send(p.cl, &jt_all, 4, JT_GIVE, a, 2), ERR_WRONG_TYPE);
    uint64_t size;
    CHECK_ST(jam_vmo_get_size(a[0].h, &size), ERR_BAD_HANDLE);   /* consumed */
    CHECK_ST(jam_vmo_get_size(a[1].h, &size), ERR_BAD_HANDLE);
    a[4].o = 2;   /* jt_all.args wants a jt_thing */
    a[3].s = NULL;
    CHECK_ST(jwl_conn_send(p.cl, &jt_all, 3, JT_ARGS, a, 6), ERR_INVALID_ARGS);
    a[4].o = 77;
    CHECK_ST(jwl_conn_send(p.cl, &jt_all, 3, JT_ARGS, a, 6), ERR_INVALID_ARGS);
    a[0].u = 7;   /* bind at a version past the table's: no id made */
    a[1].any = (struct jwl_new_any){ .iface = &jt_all, .version = 4 };
    CHECK_ST(jwl_conn_send(p.cl, &jt_registry, 2, 0, a, 2), ERR_INVALID_ARGS);
    a[0].n = 0;
    CHECK_ST(jwl_conn_send(p.cl, &jt_all, 3, JT_MAKE, a, 1), OK);
    CHECK_EQ(a[0].n, 5);
    static char big[JWL_MSG_MAX];   /* 4095 bytes and the NUL: too big a message */
    memset(big, 'x', sizeof(big) - 1);
    a[0].s = big;
    a[1].s = "y";
    CHECK_ST(jwl_conn_send(p.cl, &jt_all, 3, JT_STRS, a, 2), ERR_OUT_OF_RANGE);
    CHECK_ST(jwl_conn_post_error(p.cl, 1, 0, "no"), ERR_NOT_SUPPORTED);
    /* version: a since-2 event to an object bound at 1 isn't sent */
    a[0].u = 7;
    a[1].any = (struct jwl_new_any){ .iface = &jt_all, .version = 1 };
    CHECK_ST(jwl_conn_send(p.cl, &jt_registry, 2, 0, a, 2), OK);
    CHECK_ST(jwl_conn_flush(p.cl), OK);
    struct jwl_msg m;
    while (jwl_conn_next(p.sv, &m) == OK)
        ;
    CHECK(jwl_conn_object(p.sv, 6) && jwl_conn_object(p.sv, 6)->version == 1);
    CHECK_ST(jwl_conn_send(p.sv, &jt_all, 6, JT_EV_NEWER, a, 1), ERR_NOT_SUPPORTED);
    CHECK_ST(jwl_conn_send(p.sv, &jt_all, 3, JT_EV_NEWER, a, 1), OK);
    CHECK_ST(jwl_conn_delete(p.sv, JWL_DISPLAY_ID), ERR_INVALID_ARGS);
    CHECK_ST(jwl_conn_delete(p.sv, 99), ERR_NOT_FOUND);
    /* the compositor's error naming an object that isn't there names the display */
    CHECK_ST(jwl_conn_post_error(p.sv, 1234, JWL_ERROR_IMPLEMENTATION, "bad \x01 byte"),
             ERR_INVALID_ARGS);
    CHECK(!strcmp(p.sv->error.text, "bad ? byte"));
    while (jwl_conn_next(p.cl, &m) == OK && m.id != JWL_DISPLAY_ID)
        ;
    CHECK(m.id == JWL_DISPLAY_ID && m.args[0].o == JWL_DISPLAY_ID && m.args[1].u == 3);
    pair_down(&p);
    held_now(&h1, &b1);
    CHECK_EQ(h1, h0);
    CHECK_EQ(b1, b0);
    return true;
}
