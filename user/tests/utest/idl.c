/* utest: answering later and calls that don't wait (tools/genidl.py), on
 * the test protocol `idltest` (abi/idl/idltest.idl). A server in this
 * process keeps `wait` and `make_vmo` requests and answers them when a
 * `release` names their key, while it goes on serving: from the same
 * thread step by step, to clients blocked in calls on other threads, and
 * to a client that sends without waiting and takes the replies from a
 * port. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/idltest.h>
#include <os.h>
#include "utest.h"

#define KEPT_MAX   8
#define CALL_WAIT  (5 * NS_PER_S)
#define PORT_KEY   77

/* A request the server answers later. */
struct kept {
    bool           used;
    bool           vmo;    /* make_vmo (else wait) */
    uint32_t       key;    /* answered when `release` names it */
    uint32_t       size;   /* make_vmo: the VMO's */
    struct idl_txn txn;
};

struct server {
    struct kept kept[KEPT_MAX];
    uint32_t    nkept;     /* requests kept so far (read by other threads) */
    uint32_t    answered;  /* waits answered: the next one's order */
};

static status_t keep(struct server *s, struct idl_txn txn, bool vmo, uint32_t key, uint32_t size)
{
    for (unsigned i = 0; i < KEPT_MAX; i++) {
        struct kept *k = &s->kept[i];
        if (k->used)
            continue;
        *k = (struct kept){ .used = true, .vmo = vmo, .key = key, .size = size, .txn = txn };
        __atomic_add_fetch(&s->nkept, 1, __ATOMIC_RELEASE);
        return IDL_LATER;
    }
    return ERR_NO_RESOURCES;
}

static status_t answer(struct server *s, struct kept *k)
{
    k->used = false;
    if (!k->vmo)
        return idltest_reply_wait(k->txn, OK, k->key, s->answered++);
    handle_t vmo = HANDLE_INVALID;
    status_t st = jam_vmo_create(k->size, 0, HANDLE_INVALID, &vmo);
    return idltest_reply_make_vmo(k->txn, st, vmo, k->size);
}

static status_t s_echo(void *ctx, uint32_t value, uint32_t *out_value)
{
    (void)ctx;
    if (!value)
        return ERR_INVALID_ARGS;
    *out_value = value;
    return OK;
}

/* Key 0 is released already: answered at once, through the results. */
static status_t s_wait(void *ctx, struct idl_txn txn, uint32_t key, uint32_t *out_key,
                       uint32_t *out_order)
{
    struct server *s = ctx;
    if (key)
        return keep(s, txn, false, key, 0);
    *out_key = 0;
    *out_order = s->answered++;
    return OK;
}

static status_t s_release(void *ctx, uint32_t key, uint32_t *out_woken)
{
    struct server *s = ctx;
    uint32_t n = 0;
    for (unsigned i = 0; i < KEPT_MAX; i++)
        if (s->kept[i].used && s->kept[i].key == key) {
            (void)answer(s, &s->kept[i]);   /* a client that went needs no answer */
            n++;
        }
    *out_woken = n;
    return OK;
}

static status_t s_make_vmo(void *ctx, struct idl_txn txn, uint32_t key, uint32_t size,
                           handle_t *out_vmo, uint32_t *out_size_back)
{
    (void)out_vmo;
    (void)out_size_back;
    if (!size || size > 65536)
        return ERR_INVALID_ARGS;
    return keep(ctx, txn, true, key, size);
}

static const struct idltest_ops ops = {
    .echo = s_echo, .wait = s_wait, .release = s_release, .make_vmo = s_make_vmo,
};

/* Serve every request queued on sv. */
static bool serve_queued(handle_t sv, struct server *s, unsigned want)
{
    for (unsigned i = 0; i < want; i++)
        CHECK_ST(idltest_serve_one(sv, &ops, s), OK);
    CHECK_ST(idltest_serve_one(sv, &ops, s), ERR_SHOULD_WAIT);
    return true;
}

/* The next reply on c: it must answer txid. */
static bool next_reply(handle_t c, void *rep, struct idl_msg *m, uint32_t txid)
{
    CHECK_ST(idl_reply_read(c, rep, IDLTEST_REP_MAX, m), OK);
    CHECK_EQ(m->txid, txid);
    return true;
}

bool t_idl_answer_later(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct server s = { 0 };
    uint32_t last = 0, t[8];
    for (unsigned i = 0; i < 8; i++)
        t[i] = idl_txid_next(&last);
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg m;
    uint32_t v = 0, key = 0, order = 0, woken = 0;

    /* Two waits kept, an echo answered at once, a release answering one. */
    CHECK_ST(idltest_wait_send(c, t[0], 1), OK);
    CHECK_ST(idltest_wait_send(c, t[1], 2), OK);
    CHECK_ST(idltest_echo_send(c, t[2], 7), OK);
    CHECK_ST(idltest_release_send(c, t[3], 2), OK);
    if (!serve_queued(sv, &s, 4))
        return false;
    if (!next_reply(c, rep, &m, t[2]))
        return false;
    CHECK_ST(idltest_echo_result(rep, &m, &v), OK);
    CHECK_EQ(v, 7);
    if (!next_reply(c, rep, &m, t[1]))   /* answered from inside release */
        return false;
    CHECK_ST(idltest_wait_result(rep, &m, &key, &order), OK);
    CHECK(key == 2 && order == 0);
    if (!next_reply(c, rep, &m, t[3]))
        return false;
    CHECK_ST(idltest_release_result(rep, &m, &woken), OK);
    CHECK_EQ(woken, 1);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), ERR_SHOULD_WAIT);   /* 1 still kept */

    /* The other one, and a wait answered through its results. */
    CHECK_ST(idltest_release_send(c, t[4], 1), OK);
    CHECK_ST(idltest_wait_send(c, t[5], 0), OK);
    if (!serve_queued(sv, &s, 2) || !next_reply(c, rep, &m, t[0]))
        return false;
    CHECK_ST(idltest_wait_result(rep, &m, &key, &order), OK);
    CHECK(key == 1 && order == 1);
    if (!next_reply(c, rep, &m, t[4]))
        return false;
    CHECK_ST(idltest_release_result(rep, &m, &woken), OK);
    CHECK_EQ(woken, 1);
    if (!next_reply(c, rep, &m, t[5]))
        return false;
    CHECK_ST(idltest_wait_result(rep, &m, &key, &order), OK);
    CHECK(key == 0 && order == 2);

    /* Errors: txid 0, a refused request, another method's reply, replies
     * that are no replies. */
    CHECK_ST(idltest_echo_send(c, 0, 7), ERR_INVALID_ARGS);
    CHECK_ST(idltest_echo_send(c, t[6], 0), OK);
    CHECK_ST(idltest_echo_send(c, t[7], 9), OK);
    if (!serve_queued(sv, &s, 2) || !next_reply(c, rep, &m, t[6]))
        return false;
    CHECK_ST(idltest_echo_result(rep, &m, &v), ERR_INVALID_ARGS);
    if (!next_reply(c, rep, &m, t[7]))
        return false;
    CHECK_ST(idltest_wait_result(rep, &m, &key, &order), ERR_INTERNAL);
    uint8_t big[64] = { 0 };
    big[0] = 0x2a;
    CHECK_ST(jam_channel_write(sv, big, sizeof(big), NULL, 0), OK);
    CHECK_ST(jam_channel_write(sv, big, 2, NULL, 0), OK);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), ERR_INTERNAL);   /* too big */
    CHECK_EQ(m.txid, 0x2a);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), ERR_INTERNAL);   /* no txid */
    CHECK_EQ(m.txid, 0);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), ERR_SHOULD_WAIT);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- blocking clients on other threads ---------------------------------------- */

struct server_thread {
    handle_t      sv;
    struct server s;
    status_t      st;      /* idltest_serve's, when the client end closed */
};

static void serve_thread(void *arg)
{
    struct server_thread *t = arg;
    t->st = idltest_serve(t->sv, &ops, &t->s);
}

struct waiter {
    handle_t c;
    uint32_t key;
    status_t st;           /* the call's */
    uint32_t got_key;
};

static void wait_thread(void *arg)
{
    struct waiter *w = arg;
    uint32_t order = 0;
    w->st = idltest_wait_until(w->c, now() + CALL_WAIT, w->key, &w->got_key, &order);
}

/* Until the server has kept n requests in all. */
static bool kept_at_least(const struct server *s, uint32_t n)
{
    uint64_t end = now() + CALL_WAIT;
    while (__atomic_load_n(&s->nkept, __ATOMIC_ACQUIRE) < n)
        CHECK(now() < end && jam_nanosleep(now() + NS_PER_MS) == OK);
    return true;
}

bool t_idl_later_blocking_clients(void)
{
    static uint8_t stacks[3][16384] __attribute__((aligned(16)));
    static struct server_thread srv;
    static struct waiter w[2];
    handle_t c, th[3];
    srv = (struct server_thread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);
    CHECK_ST(thread_spawn("idl-server", serve_thread, &srv, stacks[0], sizeof(stacks[0]), &th[0]),
             OK);
    for (unsigned i = 0; i < 2; i++) {
        w[i] = (struct waiter){ .c = c, .key = 5 + i, .st = ERR_INTERNAL };
        CHECK_ST(thread_spawn("idl-waiter", wait_thread, &w[i], stacks[1 + i],
                              sizeof(stacks[1 + i]), &th[1 + i]),
                 OK);
    }
    if (!kept_at_least(&srv.s, 2))
        return false;
    /* Both calls are waiting; the server still answers others. */
    uint32_t v = 0, woken = 0;
    CHECK_ST(idltest_echo_until(c, now() + CALL_WAIT, 3, &v), OK);
    CHECK_EQ(v, 3);
    CHECK_ST(idltest_release_until(c, now() + CALL_WAIT, 6, &woken), OK);
    CHECK_EQ(woken, 1);
    CHECK_ST(idltest_release_until(c, now() + CALL_WAIT, 5, &woken), OK);
    CHECK_EQ(woken, 1);
    if (!wait_threads(&th[1], 2))
        return false;
    CHECK(w[0].st == OK && w[0].got_key == 5);
    CHECK(w[1].st == OK && w[1].got_key == 6);

    /* A call that gives up: its late reply comes as a plain message. */
    uint32_t key = 0, order = 0;
    CHECK_ST(idltest_wait_until(c, now() + 20 * NS_PER_MS, 9, &key, &order), ERR_TIMED_OUT);
    if (!kept_at_least(&srv.s, 3))
        return false;
    CHECK_ST(idltest_release_until(c, now() + CALL_WAIT, 9, &woken), OK);
    CHECK_EQ(woken, 1);
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK(m.txid != 0);
    idl_msg_drop(&m);

    CHECK_ST(jam_handle_close(c), OK);
    if (!wait_threads(th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK_ST(jam_handle_close(srv.sv), OK);
    return true;
}

/* ---- handles in a later reply --------------------------------------------------- */

bool t_idl_later_handles(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct server s = { 0 };
    uint32_t last = 0, ta = idl_txid_next(&last), tb = idl_txid_next(&last);
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idltest_make_vmo_send(c, ta, 4, 8192), OK);
    CHECK_ST(idltest_make_vmo_send(c, tb, 4, 0), OK);   /* refused at once */
    if (!serve_queued(sv, &s, 2) || !next_reply(c, rep, &m, tb))
        return false;
    handle_t vmo = HANDLE_INVALID;
    uint32_t size = 0;
    CHECK_ST(idltest_make_vmo_result(rep, &m, &vmo, &size), ERR_INVALID_ARGS);
    CHECK(vmo == HANDLE_INVALID);
    uint32_t woken = 0;
    CHECK_ST(s_release(&s, 4, &woken), OK);
    CHECK_EQ(woken, 1);
    if (!next_reply(c, rep, &m, ta))
        return false;
    CHECK_EQ(m.nh, 1);
    CHECK_ST(idltest_make_vmo_result(rep, &m, &vmo, &size), OK);
    CHECK_EQ(m.nh, 0);
    uint64_t vs = 0;
    CHECK_ST(jam_vmo_get_size(vmo, &vs), OK);
    CHECK(vs == 8192 && size == 8192);
    CHECK_ST(jam_handle_close(vmo), OK);

    /* OK without the handle result: the client gets ERR_INTERNAL. */
    struct idl_txn txn = { sv, 0x51 };
    CHECK_ST(idltest_reply_make_vmo(txn, OK, HANDLE_INVALID, 1), OK);
    if (!next_reply(c, rep, &m, 0x51))
        return false;
    CHECK_ST(idltest_make_vmo_result(rep, &m, NULL, NULL), ERR_INTERNAL);

    /* The client is gone: the reply fails and its handle is closed. */
    CHECK_ST(jam_vmo_create(4096, 0, HANDLE_INVALID, &vmo), OK);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(idltest_reply_make_vmo(txn, OK, vmo, 4096), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_close(vmo), ERR_BAD_HANDLE);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- calls that don't wait, answers through a port ---------------------------- */

#define ASYNC_CALLS 7

/* A call sent and not answered yet. */
struct pending {
    uint32_t txid;
    uint32_t method;       /* IDLTEST_* */
    uint32_t key;          /* its argument */
    bool     done;
};

/* Take every reply queued on c and decode it by its call's method. */
static bool take_replies(handle_t c, struct pending *p, uint32_t *order_of_key, unsigned *left)
{
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg m;
    status_t st;
    while ((st = idl_reply_read(c, rep, sizeof(rep), &m)) == OK) {
        struct pending *call = NULL;
        for (unsigned i = 0; i < ASYNC_CALLS; i++)
            if (!p[i].done && p[i].txid == m.txid)
                call = &p[i];
        CHECK(call);
        uint32_t key = 0, order = 0, woken = 0, size = 0;
        handle_t vmo = HANDLE_INVALID;
        if (call->method == IDLTEST_WAIT) {
            CHECK_ST(idltest_wait_result(rep, &m, &key, &order), OK);
            order_of_key[key] = order;
        } else if (call->method == IDLTEST_RELEASE) {
            CHECK_ST(idltest_release_result(rep, &m, &woken), OK);
            CHECK_EQ(woken, call->key == 3 ? 2 : 1);   /* key 3: a wait and the VMO */
        } else {
            CHECK_ST(idltest_make_vmo_result(rep, &m, &vmo, &size), OK);
            CHECK_ST(jam_handle_close(vmo), OK);
        }
        call->done = true;
        (*left)--;
    }
    CHECK_ST(st, ERR_SHOULD_WAIT);
    return true;
}

bool t_idl_async_through_port(void)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static struct server_thread srv;
    handle_t c, port, th;
    srv = (struct server_thread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);
    CHECK_ST(jam_port_create(&port), OK);
    CHECK_ST(jam_port_bind(port, c, PORT_KEY, SIG_READABLE, PORT_BIND_PERSISTENT), OK);
    CHECK_ST(thread_spawn("idl-server", serve_thread, &srv, stack, sizeof(stack), &th), OK);

    /* Waits on keys 1-3 and a VMO on key 3, then the releases in the order
     * 3, 1, 2: the waits are answered in that order, not the order sent. */
    static const uint32_t methods[ASYNC_CALLS] = { IDLTEST_WAIT, IDLTEST_WAIT, IDLTEST_WAIT,
        IDLTEST_MAKE_VMO, IDLTEST_RELEASE, IDLTEST_RELEASE, IDLTEST_RELEASE };
    static const uint32_t keys[ASYNC_CALLS] = { 1, 2, 3, 3, 3, 1, 2 };
    struct pending p[ASYNC_CALLS];
    uint32_t last = 0, order_of_key[4] = { 0 };
    for (unsigned i = 0; i < ASYNC_CALLS; i++) {
        p[i] = (struct pending){ .txid = idl_txid_next(&last), .method = methods[i],
                                 .key = keys[i] };
        status_t st = methods[i] == IDLTEST_WAIT ? idltest_wait_send(c, p[i].txid, keys[i])
                    : methods[i] == IDLTEST_RELEASE ? idltest_release_send(c, p[i].txid, keys[i])
                    : idltest_make_vmo_send(c, p[i].txid, keys[i], 4096);
        CHECK_ST(st, OK);
    }
    unsigned left = ASYNC_CALLS;
    uint64_t end = now() + CALL_WAIT;
    while (left) {
        struct port_packet pk;
        CHECK_ST(jam_port_wait(port, end, &pk), OK);
        CHECK_EQ(pk.key, PORT_KEY);
        if (!take_replies(c, p, order_of_key, &left))
            return false;
    }
    CHECK(order_of_key[3] == 0 && order_of_key[1] == 1 && order_of_key[2] == 2);

    CHECK_ST(jam_handle_close(c), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK_ST(jam_handle_close(srv.sv), OK);
    CHECK_ST(jam_handle_close(port), OK);
    return true;
}
