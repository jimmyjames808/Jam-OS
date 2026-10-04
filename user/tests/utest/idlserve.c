/* utest: the generated server loop and the generated clients' timeouts
 * (tools/genidl.py), on the test protocol `null` (abi/idl/null.idl).
 *
 * <proto>_serve answers each request in the system call that takes the
 * next one (channel_reply_wait): the replies, handle results among them,
 * still reach their callers; a request the protocol can't take (one that
 * carries handles, one too big for any method, one with no txid) is
 * answered or dropped as before and the loop goes on; a reply to a client
 * that has gone has its handles closed, and the loop ends when the client
 * end is closed. <proto>_<method>_within gives a call a timeout from when
 * it starts, by the kernel's clock. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/null.h>
#include <os.h>
#include "utest.h"

#define CALL_WAIT (5 * NS_PER_S)   /* every call here is answered well before */

static status_t s_ping(void *ctx, uint64_t value, uint64_t *out_value)
{
    (void)ctx;
    if (!value)
        return ERR_NOT_FOUND;   /* an error status reaches the client too */
    *out_value = value;
    return OK;
}

static status_t s_add(void *ctx, uint32_t a, uint32_t b, uint32_t *out_sum)
{
    (void)ctx;
    *out_sum = a + b;
    return OK;
}

static status_t s_make_vmo(void *ctx, uint32_t size, uint8_t fill, handle_t *out_vmo,
                           uint64_t *out_size_back)
{
    (void)ctx;
    (void)fill;
    if (!size || size > 65536)
        return ERR_INVALID_ARGS;
    status_t st = jam_vmo_create(size, 0, HANDLE_INVALID, out_vmo);
    if (st == OK)
        *out_size_back = size;
    return st;
}

static const struct null_ops ops = { .ping = s_ping, .add = s_add, .make_vmo = s_make_vmo };

struct serve_thread {
    handle_t sv;
    status_t st;   /* null_serve's, once the client end is closed */
};

static void serve_main(void *arg)
{
    struct serve_thread *t = arg;
    t->st = null_serve(t->sv, &ops, NULL);
}

/* A raw call on c: n bytes of req (a null.ping's header first) with nh
 * handles; the reply's status into *rep_st. */
static status_t raw_call(handle_t c, const void *req, uint32_t n, handle_t *hs, uint32_t nh,
                         status_t *rep_st)
{
    struct idl_rep_hdr rep = { 0, 0 };
    uint32_t rn = 0;
    struct channel_call_args a = {
        .h = c, .wn = n, .wbytes = (uint64_t)(uintptr_t)req, .wh = (uint64_t)(uintptr_t)hs,
        .whn = nh, .rcap = sizeof(rep), .rbytes = (uint64_t)(uintptr_t)&rep,
        .ractual = (uint64_t)(uintptr_t)&rn, .flags = CHANNEL_CALL_TIMEOUT,
        .deadline_ns = CALL_WAIT,
    };
    status_t st = jam_channel_call(&a);
    *rep_st = st == OK && rn == sizeof(rep) ? rep.status : ERR_INTERNAL;
    return st;
}

/* The requests the protocol can't take, on a served channel: each is
 * answered ERR_INVALID_ARGS (or dropped, without a txid), and the loop
 * goes on. */
static bool refused_requests(handle_t c)
{
    status_t rs;
    /* A ping that carries a channel end: answered, the end closed. */
    handle_t x, y;
    CHECK_ST(jam_channel_create(&x, &y), OK);
    struct null_ping_req q = { 0, NULL_PING, 5 };
    CHECK_ST(raw_call(c, &q, sizeof(q), &x, 1, &rs), OK);
    CHECK_ST(rs, ERR_INVALID_ARGS);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(y, SIG_PEER_CLOSED, now() + CALL_WAIT, &seen), OK);
    CHECK_ST(jam_handle_close(y), OK);
    /* Bigger than any request of the protocol. */
    static uint8_t big[NULL_REQ_MAX + 8];
    memcpy(big, &q, sizeof(q));
    CHECK_ST(raw_call(c, big, sizeof(big), NULL, 0, &rs), OK);
    CHECK_ST(rs, ERR_INVALID_ARGS);
    /* No txid: dropped without a reply; the next call is answered. */
    CHECK_ST(jam_channel_write(c, &q, 2, NULL, 0), OK);
    uint64_t v = 0;
    CHECK_ST(null_ping_within(c, CALL_WAIT, 9, &v), OK);
    CHECK_EQ(v, 9);
    return true;
}

bool t_idl_serve_reply_wait(void)
{
    static uint8_t stack[32768] __attribute__((aligned(16)));
    static struct serve_thread srv;
    handle_t c, th;
    srv = (struct serve_thread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);
    CHECK_ST(thread_spawn("idl-serve", serve_main, &srv, stack, sizeof(stack), &th), OK);

    uint64_t v = 0;
    uint32_t sum = 0;
    CHECK_ST(null_ping_within(c, CALL_WAIT, 7, &v), OK);
    CHECK_EQ(v, 7);
    CHECK_ST(null_ping_within(c, CALL_WAIT, 0, &v), ERR_NOT_FOUND);
    CHECK_ST(null_add_until(c, now() + CALL_WAIT, 2, 3, &sum), OK);
    CHECK_EQ(sum, 5);
    CHECK_ST(null_add(c, 40, 2, &sum), OK);
    CHECK_EQ(sum, 42);

    /* A handle result, and a refusal that carries none. */
    handle_t vmo = HANDLE_INVALID;
    uint64_t size = 0, vs = 0;
    CHECK_ST(null_make_vmo_within(c, CALL_WAIT, 8192, 1, &vmo, &size), OK);
    CHECK_ST(jam_vmo_get_size(vmo, &vs), OK);
    CHECK(vs == 8192 && size == 8192);
    CHECK_ST(jam_handle_close(vmo), OK);
    vmo = HANDLE_INVALID;
    CHECK_ST(null_make_vmo_within(c, CALL_WAIT, 0, 1, &vmo, &size), ERR_INVALID_ARGS);
    CHECK(vmo == HANDLE_INVALID);

    if (!refused_requests(c))
        return false;
    CHECK_ST(jam_handle_close(c), OK);   /* the loop ends: OK */
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK_ST(jam_handle_close(srv.sv), OK);
    return true;
}

/* A reply whose client has gone: its handle result is closed, not left in
 * the server's table, and the loop ends (the client end is closed). Run on
 * this thread: the request is queued before the loop starts. */
bool t_idl_serve_gone_client(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    uint32_t last = 0;
    CHECK_ST(null_make_vmo_send(c, idl_txid_next(&last), 4096, 1), OK);
    CHECK_ST(jam_handle_close(c), OK);
    struct job_info before, after;
    CHECK_ST(info_of(own_job(), &before), OK);
    CHECK_ST(null_serve(sv, &ops, NULL), OK);
    CHECK_ST(info_of(own_job(), &after), OK);
    CHECK_EQ(after.used[JOB_LIMIT_HANDLES], before.used[JOB_LIMIT_HANDLES]);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* _within: a timeout from when the call starts. Nobody serves s here. */
bool t_idl_within_times_out(void)
{
    handle_t c, s;
    CHECK_ST(jam_channel_create(&c, &s), OK);
    uint64_t v = 0, t0 = now();
    CHECK_ST(null_ping_within(c, 20 * NS_PER_MS, 1, &v), ERR_TIMED_OUT);
    CHECK(now() - t0 >= 20 * NS_PER_MS);
    CHECK_ST(null_ping_within(c, 0, 1, &v), ERR_TIMED_OUT);   /* 0: at once */
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(s), OK);
    return true;
}
