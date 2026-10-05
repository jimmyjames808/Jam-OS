/* utest: libjwl's transport (<jwl.h>) over real channels, a client and a
 * compositor connection in one process: requests and events both ways
 * with handles for fds, new ids and the delete_id handshake, batching
 * (16 KiB and 16 handles a batch), the flow-control window and the held
 * bytes behind it (a client that reads late catches up; one that never
 * reads is disconnected with no_memory, and reads the error), the
 * half-window acknowledgement, every malformed batch refused with
 * wl_display.error, and jwl_send's own refusals. Each test ends with
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

/* Message op of iface, sent on id as c's side sends (as a stub would). */
static status_t say(struct jwl_conn *c, uint32_t id, const struct jwl_interface *iface,
                    uint32_t op, const union jwl_arg *a)
{
    const struct jwl_message *m =
        c->side == JWL_CLIENT ? &iface->requests[op] : &iface->events[op];
    return jwl_send(c, id, op, m, a);
}

/* A connected pair with wl_registry at 2, jt_all (version 3) at 3 and a
 * jt_thing at 4, made by the client and read by the compositor. */
static bool pair_up(struct pair *p)
{
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(conn_on(a, JWL_CLIENT, &p->cl), OK);
    CHECK_ST(conn_on(b, JWL_SERVER, &p->sv), OK);
    union jwl_arg r[4];
    CHECK_ST(jwl_conn_make(p->cl, &jt_registry, 1, NULL, &r[0].n), OK);
    CHECK_EQ(r[0].n, 2);
    CHECK_ST(say(p->cl, JWL_DISPLAY_ID, &jt_display, 1, r), OK);
    r[0].u = 7;
    r[1].s = "jt_all";
    r[2].u = 3;
    CHECK_ST(jwl_conn_make(p->cl, &jt_all, 3, NULL, &r[3].n), OK);
    CHECK_ST(say(p->cl, 2, &jt_registry, 0, r), OK);
    CHECK_EQ(r[3].n, 3);
    CHECK_ST(jwl_conn_make(p->cl, &jt_thing, 3, NULL, &r[0].n), OK);
    CHECK_ST(say(p->cl, 3, &jt_all, JT_MAKE, r), OK);
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
    /* sync; two handles; jt_thing.destroy, a destructor request */
    union jwl_arg a[3];
    uint32_t cb;
    CHECK_ST(jwl_conn_make(p.cl, &jt_callback, 1, NULL, &cb), OK);
    CHECK_EQ(cb, 5);
    a[0].n = cb;
    CHECK_ST(say(p.cl, JWL_DISPLAY_ID, &jt_display, 0, a), OK);
    a[0].h = new_vmo();
    a[1].h = new_vmo();
    CHECK(a[0].h != HANDLE_INVALID && a[1].h != HANDLE_INVALID);
    CHECK_ST(say(p.cl, 3, &jt_all, JT_GIVE, a), OK);
    CHECK_ST(say(p.cl, 4, &jt_thing, 1, a), OK);
    CHECK(jwl_map_entry(&p.cl->map, 4)->state == JWL_ZOMBIE);   /* until delete_id */
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
    CHECK_ST(jwl_conn_next(p.sv, &m), OK);
    CHECK(m.id == 4 && m.opcode == 1 && jwl_map_entry(&p.sv->map, 4)->state == JWL_FREE);
    /* done destroys the callback: freed at once here, delete_id queued */
    a[0].u = 42;
    CHECK_ST(say(p.sv, cb, &jt_callback, 0, a), OK);
    CHECK(jwl_map_entry(&p.sv->map, cb)->state == JWL_FREE);
    a[0].h = back;
    CHECK_ST(say(p.sv, 3, &jt_all, JT_EV_HANDLE, a), OK);
    a[0].u = 7;
    a[1].s = "jt_all";
    a[2].u = 3;
    CHECK_ST(say(p.sv, 2, &jt_registry, 0, a), OK);
    CHECK_ST(jwl_conn_flush(p.sv), OK);
    CHECK_ST(jwl_conn_next(p.cl, &m), OK);   /* delete_id(4) taken on the way */
    CHECK(m.id == cb && m.args[0].u == 42);
    CHECK(jwl_map_entry(&p.cl->map, 4)->state == JWL_FREE);
    CHECK(jwl_map_entry(&p.cl->map, cb)->state == JWL_ZOMBIE);
    CHECK_ST(jwl_conn_next(p.cl, &m), OK);   /* and delete_id(cb) */
    CHECK(m.opcode == JT_EV_HANDLE && m.nhandles == 1);
    CHECK_ST(jam_vmo_get_size(m.args[0].h, &size), OK);
    jwl_msg_close_handles(&m);
    CHECK_ST(jwl_conn_next(p.cl, &m), OK);
    CHECK(m.id == 2 && !strcmp(m.args[1].s, "jt_all") && m.args[2].u == 3);
    CHECK_ST(jwl_conn_next(p.cl, &m), ERR_SHOULD_WAIT);
    CHECK(jwl_map_entry(&p.cl->map, cb)->state == JWL_FREE);
    uint32_t again;
    CHECK_ST(jwl_conn_make(p.cl, &jt_callback, 1, NULL, &again), OK);
    CHECK_EQ(again, cb);   /* the last freed, reused */
    CHECK(p.cl->stats.msgs_out == 6 && p.sv->stats.msgs_in == 6 && p.cl->stats.msgs_in == 5);
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
        CHECK_ST(say(p.cl, 3, &jt_all, JT_MAX, a), OK);
    }
    for (unsigned i = 0; i < 40; i++) {
        a[0].h = new_vmo();
        a[1].h = new_vmo();
        CHECK_ST(say(p.cl, 3, &jt_all, JT_GIVE, a), OK);
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
        st = say(p->sv, 4, &jt_thing, 0, &a);
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
    CHECK_ST(say(p.cl, 4, &jt_thing, 0, &a), OK);
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
    CHECK_ST(say(p.sv, 4, &jt_thing, 0, &a), ERR_INVALID_ARGS);
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

/* jwl_send's refusals; handles are consumed even then. */
bool t_jwl_conn_send_refusals(void)
{
    uint64_t h0, b0, h1, b1;
    held_now(&h0, &b0);
    struct pair p;
    if (!pair_up(&p))
        return false;
    union jwl_arg a[6] = { { .u = 0 } };
    CHECK_ST(say(p.cl, 9, &jt_thing, 0, a), ERR_NOT_FOUND);
    CHECK_ST(say(p.cl, 3, &jt_thing, 0, a), ERR_WRONG_TYPE);   /* a jt_all, not a jt_thing */
    CHECK_ST(jwl_send(p.cl, 3, 99, &jt_all.requests[0], a), ERR_WRONG_TYPE);
    CHECK_ST(jwl_send(p.cl, 3, 0x10000, &jt_all.requests[0], a), ERR_WRONG_TYPE);
    CHECK_ST(jwl_send(p.cl, 3, 0, NULL, a), ERR_INVALID_ARGS);
    CHECK_ST(jwl_send(p.sv, 3, 0, &jt_all.requests[0], a), ERR_WRONG_TYPE);   /* not an event */
    a[0].h = new_vmo();
    a[1].h = new_vmo();
    CHECK_ST(say(p.cl, 4, &jt_all, JT_GIVE, a), ERR_WRONG_TYPE);
    uint64_t size;
    CHECK_ST(jam_vmo_get_size(a[0].h, &size), ERR_BAD_HANDLE);   /* consumed */
    CHECK_ST(jam_vmo_get_size(a[1].h, &size), ERR_BAD_HANDLE);
    a[3].s = NULL;
    a[4].o = 2;   /* jt_all.args wants a jt_thing */
    a[5].a = (struct jwl_array){ .data = NULL, .size = 0 };
    CHECK_ST(say(p.cl, 3, &jt_all, JT_ARGS, a), ERR_INVALID_ARGS);
    a[4].o = 77;
    CHECK_ST(say(p.cl, 3, &jt_all, JT_ARGS, a), ERR_INVALID_ARGS);
    static char big[JWL_MSG_MAX];   /* 4095 bytes and the NUL: too big a message */
    memset(big, 'x', sizeof(big) - 1);
    a[0].s = big;
    a[1].s = "y";
    CHECK_ST(say(p.cl, 3, &jt_all, JT_STRS, a), ERR_OUT_OF_RANGE);
    /* a send that fails past the checks gives its new id back */
    uint32_t lost;
    CHECK_ST(jwl_conn_make(p.cl, &jt_thing, 3, NULL, &lost), OK);
    a[1].n = lost;
    CHECK_ST(say(p.cl, 3, &jt_all, JT_MAKE_NAMED, a), ERR_OUT_OF_RANGE);
    CHECK(jwl_map_entry(&p.cl->map, lost)->state == JWL_FREE);
    CHECK_ST(jwl_conn_post_error(p.cl, 1, 0, "no"), ERR_NOT_SUPPORTED);
    CHECK_ST(jwl_conn_delete(p.sv, JWL_DISPLAY_ID), ERR_INVALID_ARGS);
    CHECK_ST(jwl_conn_delete(p.sv, 99), ERR_NOT_FOUND);
    /* the compositor's error naming an object that isn't there names the display */
    CHECK_ST(jwl_conn_post_error(p.sv, 1234, JWL_ERROR_IMPLEMENTATION, "bad \x01 byte"),
             ERR_INVALID_ARGS);
    CHECK(!strcmp(p.sv->error.text, "bad ? byte"));
    struct jwl_msg m;
    CHECK_ST(jwl_conn_next(p.cl, &m), OK);
    CHECK(m.id == JWL_DISPLAY_ID && m.args[0].o == JWL_DISPLAY_ID && m.args[1].u == 3);
    pair_down(&p);
    held_now(&h1, &b1);
    CHECK_EQ(h1, h0);
    CHECK_EQ(b1, b0);
    return true;
}

/* New ids sent must be made, pending, of the interface and version the
 * message makes; a since-2 event isn't sent to an object bound at 1. */
bool t_jwl_conn_new_ids(void)
{
    uint64_t h0, b0, h1, b1;
    held_now(&h0, &b0);
    struct pair p;
    if (!pair_up(&p))
        return false;
    union jwl_arg a[4] = { { .u = 0 } };
    a[0].n = 50;
    CHECK_ST(say(p.cl, 3, &jt_all, JT_MAKE, a), ERR_INVALID_ARGS);   /* never made */
    a[0].n = 4;
    CHECK_ST(say(p.cl, 3, &jt_all, JT_MAKE, a), ERR_INVALID_ARGS);   /* announced already */
    uint32_t cb, v1, ok;
    CHECK_ST(jwl_conn_make(p.cl, &jt_callback, 1, NULL, &cb), OK);
    a[0].n = cb;
    CHECK_ST(say(p.cl, 3, &jt_all, JT_MAKE, a), ERR_INVALID_ARGS);   /* not a jt_thing */
    CHECK_ST(jwl_conn_delete(p.cl, cb), OK);   /* else a gap: the compositor refuses the next */
    CHECK_ST(jwl_conn_make(p.cl, &jt_thing, 1, NULL, &v1), OK);
    a[0].n = v1;
    CHECK_ST(say(p.cl, 3, &jt_all, JT_MAKE, a), ERR_INVALID_ARGS);   /* jt_all is version 3 */
    a[0].o = v1;
    CHECK_ST(say(p.cl, 3, &jt_all, JT_ANY_OBJ, a), ERR_INVALID_ARGS);   /* not announced */
    CHECK_ST(say(p.cl, v1, &jt_thing, 0, a), ERR_BAD_STATE);
    CHECK_ST(jwl_conn_delete(p.cl, v1), OK);   /* never announced: gone at once */
    CHECK(jwl_map_entry(&p.cl->map, v1)->state == JWL_FREE);
    CHECK_ST(jwl_conn_make(p.cl, &jt_thing, 3, NULL, &ok), OK);
    a[0].n = ok;
    CHECK_ST(say(p.cl, 3, &jt_all, JT_MAKE, a), OK);
    /* a typed child has its parent's version, even above its own table's
     * (a wl_callback of a version 4 wl_surface): made at 4, it fits only a
     * version 4 parent */
    CHECK_ST(jwl_conn_make(p.cl, &jt_thing, 4, NULL, &v1), OK);
    a[0].n = v1;
    CHECK_ST(say(p.cl, 3, &jt_all, JT_MAKE, a), ERR_INVALID_ARGS);   /* jt_all is version 3 */
    CHECK_ST(jwl_conn_delete(p.cl, v1), OK);
    CHECK_ST(jwl_conn_make(p.cl, &jt_thing, 0, NULL, &v1), ERR_INVALID_ARGS);
    /* bind: never above the table's version (its events must decode) */
    uint32_t b;
    CHECK_ST(jwl_conn_make(p.cl, &jt_all, 4, NULL, &b), OK);
    a[0].u = 7;
    a[1].s = "jt_all";
    a[2].u = 4;
    a[3].n = b;
    CHECK_ST(say(p.cl, 2, &jt_registry, 0, a), ERR_INVALID_ARGS);
    CHECK_ST(jwl_conn_delete(p.cl, b), OK);
    /* bind: the id must be what the name and version say */
    CHECK_ST(jwl_conn_make(p.cl, &jt_all, 1, NULL, &b), OK);
    a[0].u = 7;
    a[1].s = "jt_all";
    a[2].u = 2;
    a[3].n = b;
    CHECK_ST(say(p.cl, 2, &jt_registry, 0, a), ERR_INVALID_ARGS);
    a[1].s = "jt_thing";
    a[2].u = 1;
    CHECK_ST(say(p.cl, 2, &jt_registry, 0, a), ERR_INVALID_ARGS);
    a[1].s = "jt_all";
    CHECK_ST(say(p.cl, 2, &jt_registry, 0, a), OK);
    CHECK_ST(jwl_conn_flush(p.cl), OK);
    struct jwl_msg m;
    unsigned taken = 0;
    while (jwl_conn_next(p.sv, &m) == OK)
        taken++;
    CHECK_EQ(taken, 2);   /* the make and the bind; no protocol error */
    CHECK_ST(p.sv->status, OK);
    CHECK(jwl_conn_object(p.sv, b) && jwl_conn_object(p.sv, b)->version == 1);
    a[0].u = 1;
    CHECK_ST(say(p.sv, b, &jt_all, JT_EV_NEWER, a), ERR_NOT_SUPPORTED);
    CHECK_ST(say(p.sv, 3, &jt_all, JT_EV_NEWER, a), OK);
    pair_down(&p);
    held_now(&h1, &b1);
    CHECK_EQ(h1, h0);
    CHECK_EQ(b1, b0);
    return true;
}
