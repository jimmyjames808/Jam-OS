/* utest: handles in the IDL (tools/genidl.py): handle arguments in requests
 * (a client's are gone after the call, whatever its status; the server hands
 * them to the handler, which owns them, and closes them itself when the
 * handler isn't reached), lists of handles (`handle[<=N]`), typed handles
 * (`handle<vmo>`), and events carrying them, on idltest_h and idltest_ev
 * (abi/idl/idltest.idl). Also blocking and asynchronous calls on one end
 * (finding 5 of the M12 protocols review: the txid split). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/idltest.h>
#include <os.h>
#include "utest.h"

#define CALL_WAIT (5 * NS_PER_S)
#define HELD_MAX  8

struct hsrv {
    uint32_t       ran;                /* handlers that ran */
    handle_t       held[HELD_MAX];     /* take's handles, kept */
    uint32_t       nheld;
    struct idl_txn parked;             /* park's request, answered later */
    handle_t       parked_h;
    uint32_t       parked_key;
    /* idltest_ev's batch */
    uint32_t       batches, batch_key, rest_len;
    handle_t       first, rest[4];
};

static status_t h_take(void *ctx, uint32_t key, handle_t h, uint32_t *out_held)
{
    struct hsrv *s = ctx;
    (void)key;
    s->ran++;
    if (s->nheld == HELD_MAX) {
        jam_handle_close(h);
        return ERR_NO_RESOURCES;
    }
    s->held[s->nheld++] = h;
    *out_held = s->nheld;
    return OK;
}

static status_t h_vmo_size(void *ctx, handle_t v, uint64_t *out_size)
{
    struct hsrv *s = ctx;
    s->ran++;
    status_t st = jam_vmo_get_size(v, out_size);
    jam_handle_close(v);
    return st;
}

static status_t h_count(void *ctx, uint32_t tag, const handle_t *hs, uint32_t hs_len,
                        uint32_t *out_tag, uint32_t *out_count)
{
    struct hsrv *s = ctx;
    s->ran++;
    for (uint32_t i = 0; i < hs_len; i++)
        jam_handle_close(hs[i]);
    *out_tag = tag;
    *out_count = hs_len;
    return OK;
}

static status_t h_bounce(void *ctx, const handle_t *hs, uint32_t hs_len, handle_t *out_back,
                         uint32_t *out_back_len)
{
    struct hsrv *s = ctx;
    s->ran++;
    for (uint32_t i = 0; i < hs_len; i++)
        out_back[i] = hs[i];
    *out_back_len = hs_len;
    return OK;
}

static status_t h_mixed(void *ctx, handle_t end, uint32_t n, const handle_t *rest,
                        uint32_t rest_len, handle_t *out_v, handle_t *out_list,
                        uint32_t *out_list_len, uint32_t *out_n_back)
{
    struct hsrv *s = ctx;
    s->ran++;
    jam_handle_close(end);
    handle_t other;
    if (n == 99) {   /* breaks the protocol: a channel end where a VMO goes */
        status_t st = jam_channel_create(out_v, &other);
        if (st == OK)
            jam_handle_close(other);
    } else {
        status_t st = jam_vmo_create(4096, 0, out_v);
        if (st != OK)
            return st;
    }
    uint32_t keep = rest_len < 3 ? rest_len : 3;
    for (uint32_t i = 0; i < keep; i++)
        out_list[i] = rest[i];
    for (uint32_t i = keep; i < rest_len; i++)
        jam_handle_close(rest[i]);
    *out_list_len = keep;
    *out_n_back = n + 1;
    return OK;
}

static status_t h_park(void *ctx, struct idl_txn txn, uint32_t key, handle_t h,
                       uint32_t *out_key)
{
    struct hsrv *s = ctx;
    (void)out_key;
    s->ran++;
    s->parked = txn;
    s->parked_h = h;
    s->parked_key = key;
    return IDL_LATER;
}

static status_t h_refuse(void *ctx, handle_t h)
{
    struct hsrv *s = ctx;
    s->ran++;
    jam_handle_close(h);
    return ERR_BAD_STATE;
}

static status_t h_chans(void *ctx, const handle_t *cs, uint32_t cs_len, uint32_t *out_n)
{
    struct hsrv *s = ctx;
    s->ran++;
    for (uint32_t i = 0; i < cs_len; i++)
        jam_handle_close(cs[i]);
    *out_n = cs_len;
    return OK;
}

static const struct idltest_h_ops ops = {
    .take = h_take, .vmo_size = h_vmo_size, .count = h_count, .bounce = h_bounce,
    .mixed = h_mixed, .park = h_park, .refuse = h_refuse, .chans = h_chans,
};

/* ---- helpers ----------------------------------------------------------------- */

/* Has the peer of our end `peer` been closed (it was the other end's last handle)? */
static bool closed(handle_t peer)
{
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(peer, SIG_PEER_CLOSED, now() + CALL_WAIT, &seen), OK);
    CHECK_ST(jam_handle_close(peer), OK);
    return true;
}

/* Is h gone from our table? */
static bool gone(handle_t h)
{
    CHECK_ST(jam_handle_close(h), ERR_BAD_HANDLE);
    return true;
}

/* n new channel pairs: our ends in a[], their peers in b[]. */
static bool pairs(handle_t *a, handle_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        CHECK_ST(jam_channel_create(&a[i], &b[i]), OK);
    return true;
}

/* Does a byte written on `end` come out of `peer`: are they one pair? */
static bool linked(handle_t end, handle_t peer, uint8_t tag)
{
    uint8_t got = 0;
    uint32_t n = 0, nh = 0;
    CHECK_ST(jam_channel_write(end, &tag, 1, NULL, 0), OK);
    CHECK_ST(drv_channel_read(peer, &got, 1, &n, NULL, 0, &nh), OK);
    CHECK(n == 1 && got == tag && nh == 0);
    return true;
}

/* The next message on c is the reply to txid with this status. */
static bool reply_is(handle_t c, uint32_t txid, status_t want)
{
    _Alignas(8) uint8_t rep[IDLTEST_H_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, txid);
    CHECK(m.n >= sizeof(struct idl_rep_hdr) && m.nh == 0);
    CHECK_ST(((struct idl_rep_hdr *)rep)->status, want);
    return true;
}

/* A raw message on c: n bytes at q (txid as set there) with nh new channel ends,
 * whose peers go to peers[]. */
static bool raw(handle_t c, const void *q, uint32_t n, unsigned nh, handle_t *peers)
{
    handle_t hs[IDL_HANDLES_MAX];
    if (!pairs(hs, peers, nh))
        return false;
    CHECK_ST(jam_channel_write(c, q, n, hs, nh), OK);
    return true;
}

struct hthread {
    handle_t    sv;
    struct hsrv s;
    status_t    st;
};

static void h_serve(void *arg)
{
    struct hthread *t = arg;
    t->st = idltest_h_serve(t->sv, &ops, &t->s);
}

/* ---- handle arguments -------------------------------------------------------- */

bool t_idl_handle_args(void)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static struct hthread srv;
    handle_t c, th, x[3], y[3];
    srv = (struct hthread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);
    CHECK_ST(thread_spawn("idl-h", h_serve, &srv, stack, sizeof(stack), &th), OK);
    if (!pairs(x, y, 3))
        return false;

    /* A handle argument: gone from our table, the handler's. */
    uint32_t held = 0;
    CHECK_ST(idltest_h_take_within(c, CALL_WAIT, 5, x[0], &held), OK);
    CHECK_EQ(held, 1);
    if (!gone(x[0]))
        return false;
    CHECK_ST(idltest_h_take(c, 6, x[1], &held), OK);
    CHECK_EQ(held, 2);
    CHECK_ST(idltest_h_take_until(c, now() + CALL_WAIT, 7, x[2], &held), OK);
    CHECK_EQ(held, 3);
    CHECK_ST(jam_handle_close(c), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK(srv.s.ran == 3 && srv.s.nheld == 3);
    /* They are the very objects we passed: each held end pairs with its y. */
    for (unsigned i = 0; i < 3; i++)
        if (!linked(srv.s.held[i], y[i], (uint8_t)(10 + i)))
            return false;

    /* The handler owns them: its close is the pair's end. */
    for (unsigned i = 0; i < 3; i++) {
        CHECK_ST(jam_handle_close(srv.s.held[i]), OK);
        if (!closed(y[i]))
            return false;
    }
    CHECK_ST(jam_handle_close(srv.sv), OK);
    return true;
}

/* A handle argument of a call that fails is gone too, whatever the status:
 * the kernel moved it (or closed it), or the stub closed it because it never
 * called. */
bool t_idl_handle_args_fail(void)
{
    handle_t c, sv, x, y;
    uint32_t held = 0;

    /* The server never answers: the call times out; the handle sits in the
     * server's queue, ours is gone, and it closes with that queue. */
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_h_take_within(c, 20 * NS_PER_MS, 1, x, &held), ERR_TIMED_OUT);
    if (!gone(x))
        return false;
    CHECK_ST(jam_handle_close(sv), OK);
    if (!closed(y))
        return false;
    CHECK_ST(jam_handle_close(c), OK);

    /* The server is gone: the write fails, the handle with it. */
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_h_take_within(c, CALL_WAIT, 1, x, &held), ERR_PEER_CLOSED);
    if (!gone(x) || !closed(y))
        return false;
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_h_take_send(c, 77, 1, x), ERR_PEER_CLOSED);   /* asynchronously too */
    if (!gone(x) || !closed(y))
        return false;
    CHECK_ST(jam_handle_close(c), OK);

    /* Not a channel at all. */
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_h_take(HANDLE_INVALID, 1, x, &held), ERR_BAD_HANDLE);
    if (!gone(x) || !closed(y))
        return false;

    /* The stub refuses before calling (a txid of 0, a list past its N, a list
     * with no handles behind its count): it closes what it was given. */
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_h_take_send(c, 0, 1, x), ERR_INVALID_ARGS);
    if (!gone(x) || !closed(y))
        return false;
    handle_t a[9], b[9];
    uint32_t tag = 0, count = 0;
    if (!pairs(a, b, 9))
        return false;
    CHECK_ST(idltest_h_count(c, 1, a, 9, &tag, &count), ERR_INVALID_ARGS);   /* N is 8 */
    for (unsigned i = 0; i < 9; i++)
        if (!gone(a[i]) || !closed(b[i]))
            return false;
    CHECK_ST(idltest_h_count(c, 1, NULL, 3, &tag, &count), ERR_INVALID_ARGS);
    CHECK_ST(idltest_h_count_send(c, 5, 1, a, 9), ERR_INVALID_ARGS);
    CHECK_ST(idltest_h_count_send(c, 5, 1, NULL, 1), ERR_INVALID_ARGS);
    CHECK_ST(idltest_h_chans(c, NULL, 2, &count), ERR_INVALID_ARGS);
    /* Nothing reached the server's queue. */
    uint8_t byte;
    uint32_t n = 0, nh = 0;
    CHECK_ST(drv_channel_read(sv, &byte, 1, &n, NULL, 0, &nh), ERR_SHOULD_WAIT);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);

    /* The handler runs and fails: it had the handle, and closed it. */
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static struct hthread srv;
    handle_t th;
    srv = (struct hthread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);
    CHECK_ST(thread_spawn("idl-h", h_serve, &srv, stack, sizeof(stack), &th), OK);
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_h_refuse_within(c, CALL_WAIT, x), ERR_BAD_STATE);
    if (!gone(x) || !closed(y))
        return false;
    CHECK_ST(jam_handle_close(c), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK_ST(jam_handle_close(srv.sv), OK);
    return true;
}

/* ---- handle lists ------------------------------------------------------------ */

bool t_idl_handle_lists(void)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static struct hthread srv;
    handle_t c, th;
    srv = (struct hthread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);
    CHECK_ST(thread_spawn("idl-h", h_serve, &srv, stack, sizeof(stack), &th), OK);

    /* A request list: the message's own handle count, from none to N. */
    uint32_t tag = 0, count = 99;
    handle_t a[IDL_HANDLES_MAX], b[IDL_HANDLES_MAX];
    CHECK_ST(idltest_h_count_within(c, CALL_WAIT, 11, NULL, 0, &tag, &count), OK);
    CHECK(tag == 11 && count == 0);
    for (uint32_t n = 1; n <= 8; n += 7) {
        if (!pairs(a, b, n))
            return false;
        CHECK_ST(idltest_h_count_within(c, CALL_WAIT, 20 + n, a, n, &tag, &count), OK);
        CHECK(tag == 20 + n && count == n);
        for (uint32_t i = 0; i < n; i++)   /* the handler closed them all */
            if (!gone(a[i]) || !closed(b[i]))
                return false;
    }

    /* 64 handles each way (the most a message carries), moved through the
     * server and back as the same objects. */
    handle_t back[64];
    uint32_t back_len = 0;
    if (!pairs(a, b, 64))
        return false;
    CHECK_ST(idltest_h_bounce_within(c, CALL_WAIT, a, 64, back, &back_len), OK);
    CHECK_EQ(back_len, 64);
    for (unsigned i = 0; i < 64; i++) {
        if (!gone(a[i]) || !linked(back[i], b[i], (uint8_t)i))
            return false;
        CHECK_ST(jam_handle_close(back[i]), OK);
        if (!closed(b[i]))
            return false;
    }
    CHECK_ST(idltest_h_bounce_within(c, CALL_WAIT, NULL, 0, back, &back_len), OK);
    CHECK_EQ(back_len, 0);
    /* A NULL for the results closes them. */
    if (!pairs(a, b, 5))
        return false;
    CHECK_ST(idltest_h_bounce_within(c, CALL_WAIT, a, 5, NULL, NULL), OK);
    for (unsigned i = 0; i < 5; i++)
        if (!closed(b[i]))
            return false;

    /* Singles and a list in a request and a reply: a channel end, n, four
     * more; a VMO, the first three of them, n + 1. */
    handle_t end, end_peer, v, list[3];
    uint32_t list_len = 0, n_back = 0;
    if (!pairs(a, b, 4))
        return false;
    CHECK_ST(jam_channel_create(&end, &end_peer), OK);
    CHECK_ST(idltest_h_mixed_within(c, CALL_WAIT, end, 7, a, 4, &v, list, &list_len, &n_back), OK);
    CHECK(list_len == 3 && n_back == 8);
    if (!gone(end) || !closed(end_peer))
        return false;
    uint64_t size = 0;
    CHECK_ST(jam_vmo_get_size(v, &size), OK);
    CHECK_EQ(size, 4096);
    CHECK_ST(jam_handle_close(v), OK);
    for (unsigned i = 0; i < 3; i++) {   /* the first three come back */
        if (!linked(list[i], b[i], (uint8_t)(40 + i)))
            return false;
        CHECK_ST(jam_handle_close(list[i]), OK);
        if (!closed(b[i]))
            return false;
    }
    if (!closed(b[3]))   /* the fourth the handler closed */
        return false;
    for (unsigned i = 0; i < 4; i++)
        if (!gone(a[i]))
            return false;
    CHECK_ST(jam_handle_close(c), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK_ST(jam_handle_close(srv.sv), OK);
    return true;
}

/* A reply with a list through an asynchronous call (idl_reply_read takes up to
 * IDL_HANDLES_MAX handles): the _result decodes it, the singles first. */
bool t_idl_handle_lists_async(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct hsrv s = { 0 };
    handle_t a[64], b[64], back[64], end, end_peer;
    uint32_t last = 0, t = idl_txid_next(&last), back_len = 0;
    _Alignas(8) uint8_t rep[IDLTEST_H_REP_MAX];
    struct idl_msg m;

    if (!pairs(a, b, 64))
        return false;
    CHECK_ST(idltest_h_bounce_send(c, t, a, 64), OK);
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK(m.txid == t && m.nh == 64);
    CHECK_ST(idltest_h_bounce_result(rep, &m, back, &back_len), OK);
    CHECK(back_len == 64 && m.nh == 0);
    for (unsigned i = 0; i < 64; i++) {
        if (!linked(back[i], b[i], (uint8_t)i))
            return false;
        CHECK_ST(jam_handle_close(back[i]), OK);
        if (!closed(b[i]))
            return false;
    }

    /* Mixed: the VMO first, then the list. */
    handle_t v, list[3], r[4], rp[4];
    uint32_t list_len = 0, n_back = 0;
    if (!pairs(r, rp, 4))
        return false;
    CHECK_ST(jam_channel_create(&end, &end_peer), OK);
    t = idl_txid_next(&last);
    CHECK_ST(idltest_h_mixed_send(c, t, end, 1, r, 2), OK);
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK(m.txid == t && m.nh == 3);
    CHECK_ST(idltest_h_mixed_result(rep, &m, &v, list, &list_len, &n_back), OK);
    CHECK(list_len == 2 && n_back == 2);
    CHECK_ST(jam_handle_close(v), OK);
    CHECK_ST(jam_handle_close(list[0]), OK);
    CHECK_ST(jam_handle_close(list[1]), OK);
    if (!closed(rp[0]) || !closed(rp[1]) || !closed(end_peer))
        return false;
    for (unsigned i = 2; i < 4; i++) {   /* never sent: still ours */
        CHECK_ST(jam_handle_close(r[i]), OK);
        if (!closed(rp[i]))
            return false;
    }

    /* A reply that doesn't fit (a list past its N, handles for nothing): ERR_INTERNAL,
     * everything it carried closed. */
    struct idltest_h_mixed_rep bad = { t + 1, OK, 5 };
    handle_t bx[5], by[5];
    if (!pairs(bx, by, 5))
        return false;
    CHECK_ST(jam_channel_write(sv, &bad, sizeof(bad), bx, 5), OK);   /* 1 + 4: N is 3 */
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK(m.txid == t + 1 && m.nh == 5);
    CHECK_ST(idltest_h_mixed_result(rep, &m, &v, list, &list_len, &n_back), ERR_INTERNAL);
    CHECK_EQ(m.nh, 0);
    for (unsigned i = 0; i < 5; i++)
        if (!closed(by[i]))
            return false;
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- the server refuses ------------------------------------------------------ */

bool t_idl_handle_server_refuses(void)
{
    handle_t c, sv, peers[IDL_HANDLES_MAX];
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct hsrv s = { 0 };
    struct idltest_h_take_req take = { 31, IDLTEST_H_TAKE, 1 };

    /* The right message first: the handler runs. */
    if (!raw(c, &take, sizeof(take), 1, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    CHECK(s.ran == 1 && s.nheld == 1);
    struct idl_msg m;
    _Alignas(8) uint8_t rep[IDLTEST_H_REP_MAX];
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK(m.txid == 31 && m.n == sizeof(struct idltest_h_take_rep));
    CHECK_ST(jam_handle_close(s.held[0]), OK);
    if (!closed(peers[0]))
        return false;
    s.nheld = 0;

    /* Refused before the handler, every handle closed, ERR_INVALID_ARGS: the
     * wrong number (none, two, the most a message carries), a size one off. */
    take.txid = 32;
    if (!raw(c, &take, sizeof(take), 0, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    if (!reply_is(c, 32, ERR_INVALID_ARGS))
        return false;
    take.txid = 33;
    if (!raw(c, &take, sizeof(take), 2, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    if (!reply_is(c, 33, ERR_INVALID_ARGS) || !closed(peers[0]) || !closed(peers[1]))
        return false;
    take.txid = 34;
    if (!raw(c, &take, sizeof(take), 64, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    if (!reply_is(c, 34, ERR_INVALID_ARGS))
        return false;
    for (unsigned i = 0; i < 64; i++)
        if (!closed(peers[i]))
            return false;
    take.txid = 35;
    if (!raw(c, &take, sizeof(take) - 1, 1, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    if (!reply_is(c, 35, ERR_INVALID_ARGS) || !closed(peers[0]))
        return false;

    /* A list past its N (nine for count), a single and a list short of the
     * single (mixed with no handle), a typed list with too many (chans). */
    struct idltest_h_count_req cnt = { 36, IDLTEST_H_COUNT, 1 };
    if (!raw(c, &cnt, sizeof(cnt), 9, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    if (!reply_is(c, 36, ERR_INVALID_ARGS))
        return false;
    for (unsigned i = 0; i < 9; i++)
        if (!closed(peers[i]))
            return false;
    struct idltest_h_mixed_req mx = { 37, IDLTEST_H_MIXED, 1 };
    if (!raw(c, &mx, sizeof(mx), 0, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    if (!reply_is(c, 37, ERR_INVALID_ARGS))
        return false;
    struct idltest_h_chans_req ch = { 38, IDLTEST_H_CHANS };
    if (!raw(c, &ch, sizeof(ch), 5, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    if (!reply_is(c, 38, ERR_INVALID_ARGS))
        return false;
    for (unsigned i = 0; i < 5; i++)
        if (!closed(peers[i]))
            return false;

    /* An unknown ordinal, a message too short for its header, one too short
     * for a txid: handles closed; the first two answered, the last dropped. */
    struct idl_req_hdr unknown = { 39, (IDLTEST_H_PROTOCOL_ID << 16) | 77 };
    if (!raw(c, &unknown, sizeof(unknown), 2, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    if (!reply_is(c, 39, ERR_NOT_SUPPORTED) || !closed(peers[0]) || !closed(peers[1]))
        return false;
    uint32_t short6[2] = { 40, 0 };
    if (!raw(c, short6, 6, 1, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    if (!reply_is(c, 40, ERR_INVALID_ARGS) || !closed(peers[0]))
        return false;
    if (!raw(c, short6, 2, 1, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), ERR_SHOULD_WAIT);
    if (!closed(peers[0]))
        return false;

    /* No handler: ERR_NOT_SUPPORTED, the handle closed. */
    static const struct idltest_h_ops none = { 0 };
    take.txid = 41;
    if (!raw(c, &take, sizeof(take), 1, peers))
        return false;
    CHECK_ST(idltest_h_serve_one(sv, &none, &s), OK);
    if (!reply_is(c, 41, ERR_NOT_SUPPORTED) || !closed(peers[0]))
        return false;
    CHECK_EQ(s.ran, 1);   /* only the first message reached a handler */

    /* A loop that reads no handles (dispatch_on): the call is refused, and its
     * handles are the caller's to close, as for any message it read. */
    uint8_t q[64], r[IDLTEST_H_REP_MAX + 64];
    handle_t rhs[IDLTEST_H_REP_HANDLES];
    uint32_t rhn = 0;
    memcpy(q, &take, sizeof(take));
    uint32_t rn = idltest_h_dispatch_on(sv, &ops, &s, q, sizeof(take), r, rhs, &rhn);
    CHECK(rn == sizeof(struct idl_rep_hdr) && rhn == 0);
    CHECK_ST(((struct idl_rep_hdr *)r)->status, ERR_INVALID_ARGS);
    CHECK_EQ(s.ran, 1);

    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* A handle argument through a slot (<proto>_take_slot, _run_slot): the handler
 * gets it, and a request refused is closed. */
bool t_idl_handle_slot(void)
{
    handle_t c, sv, x, y, peers[3];
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct hsrv s = { 0 };
    struct {
        _Alignas(8) uint8_t q[IDLTEST_H_REQ_MAX];
        _Alignas(8) uint8_t r[IDLTEST_H_REP_MAX];
        uint32_t n, nh;
        handle_t hs[IDLTEST_H_REQ_HANDLES];
    } sl;
    struct idl_slot slot = { .q = sl.q, .qcap = sizeof(sl.q), .n = &sl.n, .nh = &sl.nh,
                             .hs = sl.hs, .hcap = IDLTEST_H_REQ_HANDLES, .r = sl.r };
    handle_t rhs[IDLTEST_H_REP_HANDLES];
    uint32_t rhn = 0;
    uint32_t last = 0, t = idl_txid_next(&last);

    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_h_take_send(c, t, 3, x), OK);
    CHECK_ST(idltest_h_take_slot(sv, &slot, NULL), OK);
    CHECK_EQ(sl.nh, 1);
    uint32_t rn = idltest_h_run_slot(sv, &slot, &ops, &s, rhs, &rhn);
    CHECK(rn == sizeof(struct idltest_h_take_rep) && sl.nh == 0 && s.nheld == 1);
    CHECK_ST(jam_handle_close(s.held[0]), OK);
    if (!closed(y))
        return false;

    /* A wrong count: closed, refused. */
    struct idltest_h_take_req take = { 9, IDLTEST_H_TAKE, 1 };
    if (!raw(c, &take, sizeof(take), 2, peers))
        return false;
    CHECK_ST(idltest_h_take_slot(sv, &slot, NULL), OK);
    rn = idltest_h_run_slot(sv, &slot, &ops, &s, rhs, &rhn);
    CHECK_EQ(rn, sizeof(struct idl_rep_hdr));
    CHECK_ST(((struct idl_rep_hdr *)sl.r)->status, ERR_INVALID_ARGS);
    if (!closed(peers[0]) || !closed(peers[1]))
        return false;
    CHECK_EQ(s.ran, 1);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- answering later --------------------------------------------------------- */

bool t_idl_handle_later(void)
{
    handle_t c, sv, x, y;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct hsrv s = { 0 };
    uint32_t last = 0, t = idl_txid_next(&last), key = 0;

    /* The handler keeps the handle and the txn and answers later: the handle
     * is still its own, whatever the client does meanwhile. */
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_h_park_send(c, t, 12, x), OK);
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    CHECK(s.ran == 1 && s.parked.txid == t);
    _Alignas(8) uint8_t rep[IDLTEST_H_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), ERR_SHOULD_WAIT);
    if (!linked(s.parked_h, y, 5))
        return false;
    CHECK_ST(idltest_h_reply_park(s.parked, OK, s.parked_key), OK);
    CHECK_ST(jam_handle_close(s.parked_h), OK);
    if (!closed(y))
        return false;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, t);
    CHECK_ST(idltest_h_park_result(rep, &m, &key), OK);
    CHECK_EQ(key, 12);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- results of the wrong shape ---------------------------------------------- */

/* A handler that breaks the protocol's shape: a VMO and a list claiming 4 for
 * an N of 3 (how 1), a handle result left unset (2), a list handle left unset
 * (3). What it filled is closed by the server, and the client gets
 * ERR_INTERNAL. */
static status_t h_mixed_bad(void *ctx, handle_t end, uint32_t n, const handle_t *rest,
                            uint32_t rest_len, handle_t *out_v, handle_t *out_list,
                            uint32_t *out_list_len, uint32_t *out_n_back)
{
    struct hsrv *s = ctx;
    jam_handle_close(end);
    for (uint32_t i = 0; i < rest_len; i++)
        jam_handle_close(rest[i]);
    *out_n_back = 0;
    if (n == 1) {
        if (jam_vmo_create(4096, 0, out_v) != OK)
            return ERR_NO_MEMORY;
        s->first = *out_v;
        for (unsigned i = 0; i < 3; i++)
            if (jam_channel_create(&out_list[i], &s->rest[i]) != OK)
                return ERR_NO_MEMORY;
        *out_list_len = 4;
    } else if (n == 2) {
        *out_list_len = 0;   /* the VMO left HANDLE_INVALID */
        s->first = HANDLE_INVALID;
    } else {
        if (jam_vmo_create(4096, 0, out_v) != OK)
            return ERR_NO_MEMORY;
        s->first = *out_v;
        if (jam_channel_create(&out_list[0], &s->rest[0]) != OK)
            return ERR_NO_MEMORY;
        out_list[1] = HANDLE_INVALID;
        *out_list_len = 2;
    }
    return OK;
}

bool t_idl_handle_results_refused(void)
{
    static const struct idltest_h_ops bad = { .mixed = h_mixed_bad };
    handle_t c, sv, end, endp;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct hsrv s = { 0 };
    for (uint32_t how = 1; how <= 3; how++) {
        s.first = HANDLE_INVALID;
        CHECK_ST(jam_channel_create(&end, &endp), OK);
        struct idltest_h_mixed_req mx = { 50 + how, IDLTEST_H_MIXED, how };
        handle_t hs[1] = { end };
        CHECK_ST(jam_channel_write(c, &mx, sizeof(mx), hs, 1), OK);
        CHECK_ST(idltest_h_serve_one(sv, &bad, &s), OK);
        if (!reply_is(c, 50 + how, ERR_INTERNAL) || !closed(endp))
            return false;
        if (how != 2) {   /* the VMO and the channel ends it filled are closed */
            uint64_t size;
            CHECK_ST(jam_vmo_get_size(s.first, &size), ERR_BAD_HANDLE);
            unsigned filled = how == 1 ? 3 : 1;
            for (unsigned i = 0; i < filled; i++)
                if (!closed(s.rest[i]))
                    return false;
        }
    }

    /* reply_mixed refuses the same, closing the handles it was given and
     * sending none: a VMO left HANDLE_INVALID, a list past 3, a list with no
     * handles behind its count; a status of its own is sent as it is. */
    uint32_t t = 60;
    struct idl_txn txn = { sv, t };   /* a reply goes out on the server end */
    handle_t a[5], b[5], vmo, vmop;
    if (!pairs(a, b, 5))
        return false;
    CHECK_ST(idltest_h_reply_mixed(txn, OK, HANDLE_INVALID, a, 1, 5), OK);
    if (!reply_is(c, t, ERR_INTERNAL) || !closed(b[0]))
        return false;
    CHECK_ST(jam_channel_create(&vmo, &vmop), OK);
    CHECK_ST(idltest_h_reply_mixed(txn, OK, vmo, a + 1, 4, 5), OK);   /* 4 > N */
    if (!reply_is(c, t, ERR_INTERNAL) || !closed(vmop))
        return false;
    for (unsigned i = 1; i < 5; i++)
        if (!closed(b[i]))
            return false;
    CHECK_ST(jam_channel_create(&vmo, &vmop), OK);
    CHECK_ST(idltest_h_reply_mixed(txn, OK, vmo, NULL, 2, 5), OK);
    if (!reply_is(c, t, ERR_INTERNAL) || !closed(vmop))
        return false;
    CHECK_ST(jam_channel_create(&vmo, &vmop), OK);
    CHECK_ST(idltest_h_reply_mixed(txn, ERR_BAD_STATE, vmo, NULL, 0, 5), OK);
    if (!reply_is(c, t, ERR_BAD_STATE) || !closed(vmop))
        return false;
    /* And right: the single first, then the list, on the wire. */
    if (!pairs(a, b, 3))
        return false;
    CHECK_ST(jam_channel_create(&vmo, &vmop), OK);
    CHECK_ST(idltest_h_reply_mixed(txn, OK, vmo, a, 3, 5), OK);
    _Alignas(8) uint8_t rep[IDLTEST_H_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK(m.txid == t && m.nh == 4);
    if (!linked(m.hs[0], vmop, 1))
        return false;
    for (unsigned i = 0; i < 3; i++)
        if (!linked(m.hs[1 + i], b[i], (uint8_t)(2 + i)))
            return false;
    idl_msg_drop(&m);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- events with handles ----------------------------------------------------- */

static void e_batch(void *ctx, uint32_t key, handle_t first, const handle_t *rest,
                    uint32_t rest_len)
{
    struct hsrv *s = ctx;
    s->batches++;
    s->batch_key = key;
    if (s->first != HANDLE_INVALID)
        jam_handle_close(s->first);
    s->first = first;
    s->rest_len = rest_len;
    for (uint32_t i = 0; i < rest_len; i++)
        s->rest[i] = rest[i];
}

static const struct idltest_ev_ops ev_ops = { .batch = e_batch };

struct pclient {
    unsigned parcels;
    uint32_t key, len;
    handle_t hs[4];
};

static void c_parcel(void *ctx, uint32_t key, const handle_t *hs, uint32_t hs_len)
{
    struct pclient *k = ctx;
    k->parcels++;
    k->key = key;
    k->len = hs_len;
    for (uint32_t i = 0; i < hs_len; i++)
        k->hs[i] = hs[i];
}

static const struct idltest_ev_events ev_events = { .parcel = c_parcel };

bool t_idl_events_handle_lists(void)
{
    handle_t c, sv, peers[8];
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct hsrv s = { .first = HANDLE_INVALID };
    handle_t vmo, a[4], b[4];

    /* To the server: a VMO and a list of 0 and 4. */
    CHECK_ST(jam_vmo_create(4096, 0, &vmo), OK);
    CHECK_ST(idltest_ev_batch(c, 1, vmo, NULL, 0), OK);
    CHECK_ST(idltest_ev_serve_one(sv, &ev_ops, &s), OK);
    CHECK(s.batches == 1 && s.batch_key == 1 && s.rest_len == 0 && s.first == s.first);
    CHECK_ST(jam_handle_close(s.first), OK);
    s.first = HANDLE_INVALID;
    if (!pairs(a, b, 4))
        return false;
    CHECK_ST(jam_vmo_create(4096, 0, &vmo), OK);
    CHECK_ST(idltest_ev_batch(c, 2, vmo, a, 4), OK);
    CHECK_ST(idltest_ev_serve_one(sv, &ev_ops, &s), OK);
    CHECK(s.batches == 2 && s.batch_key == 2 && s.rest_len == 4);
    for (unsigned i = 0; i < 4; i++) {
        if (!gone(a[i]) || !linked(s.rest[i], b[i], (uint8_t)i))
            return false;
        CHECK_ST(jam_handle_close(s.rest[i]), OK);
        if (!closed(b[i]))
            return false;
    }
    CHECK_ST(jam_handle_close(s.first), OK);
    s.first = HANDLE_INVALID;

    /* The sender refuses a list past its N or with no handles behind its count
     * (closing the handles it was given); the server drops one that arrives
     * so (a fifth in the list, no first), closing them all. */
    handle_t more[5], morep[5];
    if (!pairs(more, morep, 5))
        return false;
    CHECK_ST(jam_vmo_create(4096, 0, &vmo), OK);
    CHECK_ST(idltest_ev_batch(c, 3, vmo, more, 5), ERR_INVALID_ARGS);
    CHECK_ST(idltest_ev_batch(c, 3, vmo, NULL, 1), ERR_INVALID_ARGS);
    CHECK_ST(jam_handle_close(vmo), ERR_BAD_HANDLE);
    for (unsigned i = 0; i < 5; i++)
        if (!gone(more[i]) || !closed(morep[i]))
            return false;
    struct idltest_ev_batch_msg bm = { 0, IDLTEST_EV_BATCH, 4 };
    if (!raw(c, &bm, sizeof(bm), 6, peers) || !raw(c, &bm, sizeof(bm), 0, peers + 6))
        return false;
    CHECK_ST(idltest_ev_serve_one(sv, &ev_ops, &s), OK);
    CHECK_ST(idltest_ev_serve_one(sv, &ev_ops, &s), OK);
    for (unsigned i = 0; i < 6; i++)
        if (!closed(peers[i]))
            return false;
    CHECK_EQ(s.batches, 2);

    /* To the client: a list of channel ends (a typed list), 0 and 3. */
    struct pclient k = { 0 };
    CHECK_ST(idltest_ev_parcel(sv, 5, NULL, 0), OK);
    CHECK_ST(idltest_ev_read_event(c, &ev_events, &k), OK);
    CHECK(k.parcels == 1 && k.key == 5 && k.len == 0);
    if (!pairs(a, b, 3))
        return false;
    CHECK_ST(idltest_ev_parcel(sv, 6, a, 3), OK);
    CHECK_ST(idltest_ev_read_event(c, &ev_events, &k), OK);
    CHECK(k.parcels == 2 && k.key == 6 && k.len == 3);
    for (unsigned i = 0; i < 3; i++) {
        if (!gone(a[i]) || !linked(k.hs[i], b[i], (uint8_t)(20 + i)))
            return false;
        CHECK_ST(jam_handle_close(k.hs[i]), OK);
        if (!closed(b[i]))
            return false;
    }
    /* Past its N, at the sender; and at the client, a message that is. */
    handle_t five[5], fivep[5];
    if (!pairs(five, fivep, 5))
        return false;
    CHECK_ST(idltest_ev_parcel(sv, 7, five, 5), ERR_INVALID_ARGS);
    for (unsigned i = 0; i < 5; i++)
        if (!gone(five[i]) || !closed(fivep[i]))
            return false;
    struct idltest_ev_parcel_msg pm = { 0, IDLTEST_EV_PARCEL, 8 };
    if (!raw(sv, &pm, sizeof(pm), 5, peers))
        return false;
    CHECK_ST(idltest_ev_read_event(c, &ev_events, &k), ERR_INVALID_ARGS);
    for (unsigned i = 0; i < 5; i++)
        if (!closed(peers[i]))
            return false;
    CHECK_EQ(k.parcels, 2);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- typed handles ----------------------------------------------------------- */

/* `handle<type>`: the generated server asks handle_info before the handler runs
 * and refuses another type with ERR_WRONG_TYPE, every handle of the message
 * closed; the generated client does the same for a result (ERR_INTERNAL: the
 * server broke the protocol); events likewise. */
bool t_idl_handle_typed(void)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static struct hthread srv;
    handle_t c, th, vmo, ev, x, y;
    srv = (struct hthread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);
    CHECK_ST(thread_spawn("idl-h", h_serve, &srv, stack, sizeof(stack), &th), OK);

    /* A VMO is right; a channel end and an event are not, and never reach the handler. */
    uint64_t size = 0;
    CHECK_ST(jam_vmo_create(8192, 0, &vmo), OK);
    CHECK_ST(idltest_h_vmo_size_within(c, CALL_WAIT, vmo, &size), OK);
    CHECK_EQ(size, 8192);
    CHECK(srv.s.ran == 1);
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_h_vmo_size_within(c, CALL_WAIT, x, &size), ERR_WRONG_TYPE);
    if (!gone(x) || !closed(y))
        return false;
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(idltest_h_vmo_size_within(c, CALL_WAIT, ev, &size), ERR_WRONG_TYPE);
    if (!gone(ev))
        return false;
    CHECK(srv.s.ran == 1);

    /* A single and a list: the wrong single closes the list's handles too. */
    handle_t a[3], b[3], list[3], v;
    uint32_t list_len, n_back;
    if (!pairs(a, b, 3))
        return false;
    CHECK_ST(jam_vmo_create(4096, 0, &vmo), OK);   /* not a channel end */
    CHECK_ST(idltest_h_mixed_within(c, CALL_WAIT, vmo, 1, a, 3, &v, list, &list_len, &n_back),
             ERR_WRONG_TYPE);
    for (unsigned i = 0; i < 3; i++)
        if (!gone(a[i]) || !closed(b[i]))
            return false;
    CHECK(srv.s.ran == 1);

    /* A typed list: every one must be of the type. */
    uint32_t n = 0;
    handle_t m1[3], m1p[3];
    if (!pairs(m1, m1p, 3))
        return false;
    CHECK_ST(idltest_h_chans_within(c, CALL_WAIT, m1, 3, &n), OK);
    CHECK_EQ(n, 3);
    for (unsigned i = 0; i < 3; i++)
        if (!gone(m1[i]) || !closed(m1p[i]))
            return false;
    CHECK_ST(jam_channel_create(&m1[0], &m1p[0]), OK);
    CHECK_ST(jam_vmo_create(4096, 0, &m1[1]), OK);
    CHECK_ST(jam_channel_create(&m1[2], &m1p[2]), OK);
    CHECK_ST(idltest_h_chans_within(c, CALL_WAIT, m1, 3, &n), ERR_WRONG_TYPE);
    if (!gone(m1[0]) || !gone(m1[1]) || !gone(m1[2]) || !closed(m1p[0]) || !closed(m1p[2]))
        return false;
    CHECK(srv.s.ran == 2);

    /* A result of the wrong type: the client breaks the call (ERR_INTERNAL),
     * closing everything that came with it. */
    handle_t end, endp, r[2], rp[2];
    if (!pairs(r, rp, 2))
        return false;
    CHECK_ST(jam_channel_create(&end, &endp), OK);
    CHECK_ST(idltest_h_mixed_within(c, CALL_WAIT, end, 99, r, 2, &v, list, &list_len, &n_back),
             ERR_INTERNAL);
    if (!closed(endp) || !closed(rp[0]) || !closed(rp[1]))
        return false;
    CHECK_ST(jam_handle_close(c), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK_ST(jam_handle_close(srv.sv), OK);

    /* The same through an asynchronous call's _result. */
    handle_t sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct hsrv s = { 0 };
    uint32_t last = 0, t = idl_txid_next(&last);
    _Alignas(8) uint8_t rep[IDLTEST_H_REP_MAX];
    struct idl_msg m;
    if (!pairs(r, rp, 2))
        return false;
    CHECK_ST(jam_channel_create(&end, &endp), OK);
    CHECK_ST(idltest_h_mixed_send(c, t, end, 99, r, 2), OK);
    CHECK_ST(idltest_h_serve_one(sv, &ops, &s), OK);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_ST(idltest_h_mixed_result(rep, &m, &v, list, &list_len, &n_back), ERR_INTERNAL);
    CHECK_EQ(m.nh, 0);
    if (!closed(endp))
        return false;

    /* A typed handle in an event to the server (the first of batch is a VMO), and a
     * typed list to the client (parcel's are channel ends). */
    struct hsrv es = { .first = HANDLE_INVALID };
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_ev_batch(c, 1, x, NULL, 0), OK);   /* a channel end for a VMO */
    CHECK_ST(idltest_ev_serve_one(sv, &ev_ops, &es), OK);
    CHECK_EQ(es.batches, 0);
    if (!closed(y))
        return false;
    handle_t p[2], pp[2];
    CHECK_ST(jam_channel_create(&p[0], &pp[0]), OK);
    CHECK_ST(jam_vmo_create(4096, 0, &p[1]), OK);
    CHECK_ST(idltest_ev_parcel(sv, 2, p, 2), OK);
    struct pclient k = { 0 };
    CHECK_ST(idltest_ev_read_event(c, &ev_events, &k), ERR_WRONG_TYPE);
    CHECK_EQ(k.parcels, 0);
    if (!closed(pp[0]))
        return false;
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- blocking and asynchronous calls on one end ------------------------------ */

static status_t echo_h(void *ctx, uint32_t value, uint32_t *out_value)
{
    (void)ctx;
    *out_value = value;
    return OK;
}

static void echo_serve(void *arg)
{
    static const struct idltest_ops echo_ops = { .echo = echo_h };
    struct hthread *t = arg;
    t->st = idltest_serve(t->sv, &echo_ops, NULL);
}

/* The txids the kernel stamps on a blocking call have bit 31 set and
 * idl_txid_next never does, so a reply to an asynchronous call is never taken
 * for the blocking one's (M12 review P, finding 5). An asynchronous call with
 * txid 1 (the lowest idl_txid_next gives, and the first a kernel counter would)
 * is outstanding on an end while a blocking call is made on it: the server
 * answers the asynchronous one first, and the blocking call still gets its own
 * reply; the other comes after, by its txid. */
bool t_idl_txid_mixing(void)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static struct hthread srv;
    handle_t c, th;
    srv = (struct hthread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);

    uint32_t last = 0, t = idl_txid_next(&last), v = 0;
    CHECK_EQ(t, 1);
    CHECK_ST(idltest_echo_send(c, t, 111), OK);
    CHECK_ST(thread_spawn("idl-mix", echo_serve, &srv, stack, sizeof(stack), &th), OK);
    CHECK_ST(idltest_echo_within(c, CALL_WAIT, 222, &v), OK);
    CHECK_EQ(v, 222);
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, 1);
    CHECK_ST(idltest_echo_result(rep, &m, &v), OK);
    CHECK_EQ(v, 111);
    /* And the other way round: a blocking call's request waits at the server,
     * an asynchronous call is made meanwhile and answered first. */
    CHECK_ST(idltest_echo_send(c, idl_txid_next(&last), 333), OK);
    CHECK_ST(idltest_echo_within(c, CALL_WAIT, 444, &v), OK);
    CHECK_EQ(v, 444);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, 2);
    CHECK_ST(idltest_echo_result(rep, &m, &v), OK);
    CHECK_EQ(v, 333);
    CHECK_ST(jam_handle_close(c), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK_ST(jam_handle_close(srv.sv), OK);
    return true;
}
