/* utest: several protocols in one .idl file (tools/genidl.py): idltest and
 * idltest_two (abi/idl/idltest.idl), one for each kind of channel. Each
 * has ordinals of its own, so a server of one refuses a request of the
 * other (ERR_NOT_SUPPORTED) instead of running one of its own methods.
 * And messages up to 16 KiB, whose buffers the generated code takes from
 * the heap (idltest_two.big, big_later). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/idltest.h>
#include <os.h>
#include "utest.h"

static status_t s_echo(void *ctx, uint32_t value, uint32_t *out_value)
{
    (*(unsigned *)ctx)++;
    *out_value = value + 1;
    return OK;
}

static const struct idltest_ops one = { .echo = s_echo };
static const struct idltest_two_ops two = { .echo = s_echo };

/* The next reply on c answers txid with status want (and value + 1). */
static bool echoed(handle_t c, uint32_t txid, bool second, status_t want, uint32_t value)
{
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX > IDLTEST_TWO_REP_MAX ? IDLTEST_REP_MAX
                                                                  : IDLTEST_TWO_REP_MAX];
    struct idl_msg m;
    uint32_t v = 0;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, txid);
    if (second)
        CHECK_ST(idltest_two_echo_result(rep, &m, &v), want);
    else
        CHECK_ST(idltest_echo_result(rep, &m, &v), want);
    if (want == OK)
        CHECK_EQ(v, value + 1);
    return true;
}

bool t_idl_wrong_protocol(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    unsigned ran = 0;
    uint32_t last = 0;
    CHECK(IDLTEST_TWO_ECHO != IDLTEST_ECHO);
    CHECK_EQ(IDLTEST_TWO_ECHO >> 16, IDLTEST_TWO_PROTOCOL_ID);

    /* Each protocol's own request on its own server. */
    uint32_t t = idl_txid_next(&last);
    CHECK_ST(idltest_echo_send(c, t, 4), OK);
    CHECK_ST(idltest_serve_one(sv, &one, &ran), OK);
    if (!echoed(c, t, false, OK, 4))
        return false;
    t = idl_txid_next(&last);
    CHECK_ST(idltest_two_echo_send(c, t, 6), OK);
    CHECK_ST(idltest_two_serve_one(sv, &two, &ran), OK);
    if (!echoed(c, t, true, OK, 6))
        return false;
    CHECK_EQ(ran, 2);

    /* The other protocol's request, same method name and ordinal: refused
     * before any handler runs. */
    t = idl_txid_next(&last);
    CHECK_ST(idltest_two_echo_send(c, t, 8), OK);
    CHECK_ST(idltest_serve_one(sv, &one, &ran), OK);
    if (!echoed(c, t, true, ERR_NOT_SUPPORTED, 0))
        return false;
    t = idl_txid_next(&last);
    CHECK_ST(idltest_echo_send(c, t, 8), OK);
    CHECK_ST(idltest_two_serve_one(sv, &two, &ran), OK);
    if (!echoed(c, t, false, ERR_NOT_SUPPORTED, 0))
        return false;
    CHECK_EQ(ran, 2);

    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- messages past 8 KiB: buffers from the heap ------------------------------- */

struct big_server {
    struct idl_txn kept;   /* big_later's request, answered by the test */
    bool           have;
};

static status_t s_big(void *ctx, const uint8_t a[4096], const uint8_t b[4096],
                      const uint8_t c[4096], uint8_t out_sum[4096], uint32_t *out_total)
{
    (void)ctx;
    uint32_t t = 0;
    for (unsigned i = 0; i < 4096; i++) {
        out_sum[i] = (uint8_t)(a[i] + b[i] + c[i]);
        t += a[i] + b[i] + c[i];
    }
    *out_total = t;
    return OK;
}

static status_t s_big_later(void *ctx, struct idl_txn txn, uint32_t key, uint8_t out_data[4096])
{
    struct big_server *s = ctx;
    (void)key;
    (void)out_data;
    s->kept = txn;
    s->have = true;
    return IDL_LATER;
}

static const struct idltest_two_ops big_ops = { .big = s_big, .big_later = s_big_later };

struct big_thread {
    handle_t          sv;
    struct big_server s;
    status_t          st;
};

static void big_serve(void *arg)
{
    struct big_thread *t = arg;
    t->st = idltest_two_serve(t->sv, &big_ops, &t->s);
}

static uint32_t big_inputs(uint8_t *a, uint8_t *b, uint8_t *c)
{
    uint32_t total = 0;
    for (unsigned i = 0; i < 4096; i++) {
        a[i] = (uint8_t)i;
        b[i] = (uint8_t)(i * 7);
        c[i] = (uint8_t)(255 - i);
        total += a[i] + b[i] + c[i];
    }
    return total;
}

bool t_idl_big_messages(void)
{
    /* The request is 12296 bytes (the limit was 8192), and the protocol's
     * buffers pass IDL_STACK_MAX: the client, _send, the loops and
     * reply_<method> take theirs from the heap. */
    CHECK(IDLTEST_TWO_REQ_MAX > 8192 && IDLTEST_TWO_REQ_MAX + IDLTEST_TWO_REP_MAX > IDL_STACK_MAX);
    static uint8_t a[4096], b[4096], c[4096], sum[4096], data[4096], got[4096];
    uint32_t want = big_inputs(a, b, c), total = 0;

    /* A blocking call to idltest_two_serve on a thread of its own. */
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static struct big_thread srv;
    handle_t cl, th;
    srv = (struct big_thread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&cl, &srv.sv), OK);
    CHECK_ST(thread_spawn("idl-big", big_serve, &srv, stack, sizeof(stack), &th), OK);
    CHECK_ST(idltest_two_big_within(cl, 5 * NS_PER_S, a, b, c, sum, &total), OK);
    CHECK_EQ(total, want);
    for (unsigned i = 0; i < 4096; i++)
        CHECK_EQ(sum[i], (uint8_t)(a[i] + b[i] + c[i]));
    CHECK_ST(jam_handle_close(cl), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);   /* the client closed: the loop ended, its buffers freed */
    CHECK_ST(jam_handle_close(srv.sv), OK);

    /* _send, serve_one and a big reply answered later. */
    handle_t sv;
    CHECK_ST(jam_channel_create(&cl, &sv), OK);
    struct big_server s = { 0 };
    uint32_t last = 0, t1 = idl_txid_next(&last), t2 = idl_txid_next(&last);
    CHECK_ST(idltest_two_big_send(cl, t1, a, b, c), OK);
    CHECK_ST(idltest_two_big_later_send(cl, t2, 3), OK);
    CHECK_ST(idltest_two_serve_one(sv, &big_ops, &s), OK);
    CHECK_ST(idltest_two_serve_one(sv, &big_ops, &s), OK);
    CHECK(s.have);
    for (unsigned i = 0; i < 4096; i++)
        data[i] = (uint8_t)(3 + i);
    CHECK_ST(idltest_two_reply_big_later(s.kept, OK, data), OK);
    static _Alignas(8) uint8_t rep[IDLTEST_TWO_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(cl, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, t1);
    total = 0;
    CHECK_ST(idltest_two_big_result(rep, &m, sum, &total), OK);
    CHECK_EQ(total, want);
    CHECK_ST(idl_reply_read(cl, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, t2);
    CHECK_ST(idltest_two_big_later_result(rep, &m, got), OK);
    for (unsigned i = 0; i < 4096; i++)
        CHECK_EQ(got[i], (uint8_t)(3 + i));
    CHECK_ST(jam_handle_close(cl), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}
