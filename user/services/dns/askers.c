/* dns: /svc/dns, the askers' side. The shared channel (init keeps its
 * server end across our restarts and publishes the client end) answers
 * only svc.connect: each opener gets a channel of its own, DNS_OPENERS
 * at once, on which it sends resolve, a `later` method.
 *
 * The fair shares: /svc/dns-sys is a second shared channel, the same in
 * every way but one: its openers are system askers (the network's own
 * services; init grants it to sntp, and no program from /data may have
 * it). Ordinary openers together get at most DNS_PROG_OPENERS of the
 * DNS_OPENERS, and their resolves at most DNS_PROG_QUERIES names in
 * flight and DNS_PROG_WAITERS askers of one name (the resolver's
 * dns_ask_as); the rest is the system askers' reserve. A class is fixed
 * at connect by the channel the opener came through: nothing sent later
 * can change it. One program can still take its class's whole share (the
 * resolver can't tell its openers from another program's).
 *
 * A resolve is a
 * request in flight (struct request) until its answer comes from the
 * resolver (io->answer: askers_answer, which may run inside dns_ask, for
 * a cached name or an address), or its timeout passes (askers_tick): then
 * the resolver forgets that asker (dns_cancel) and it is answered
 * ERR_TIMED_OUT. At most DNS_PER_OPENER in flight an opener. An opener
 * that closes its channel takes its requests with it. */
#include <idl/svc.h>
#include "dnsd.h"

static uint64_t key_of(unsigned i)
{
    return (KEY_ASKER + i) | (uint64_t)D.a[i].gen << 8;
}

/* ---- requests ------------------------------------------------------------------ */

static struct request *by_cookie(uint64_t cookie)
{
    for (unsigned i = 0; i < REQUESTS; i++)
        if (D.q[i].used && D.q[i].cookie == cookie)
            return &D.q[i];
    return NULL;
}

static void request_done(struct request *q)
{
    if (D.a[q->asker].inflight)
        D.a[q->asker].inflight--;
    q->used = false;
}

void askers_answer(void *ctx, uint64_t cookie, status_t st, const uint32_t *addr, unsigned n,
                   uint32_t ttl_s)
{
    (void)ctx;
    struct request *q = by_cookie(cookie);
    if (!q)
        return;
    uint32_t a[DNS_ADDRS_MAX] = { 0 };
    if (st == OK && (!n || !addr))
        st = ERR_INTERNAL;   /* the resolver answers OK with an address */
    for (unsigned i = 0; st == OK && i < n && i < DNS_ADDRS_MAX; i++)
        a[i] = addr[i];
    uint8_t count = st == OK ? (uint8_t)(n < DNS_ADDRS_MAX ? n : DNS_ADDRS_MAX) : 0;
    /* A reply that can't be written: its asker is going (its channel's end
     * comes to the loop). */
    (void)dns_reply_resolve(q->txn, st, count, a[0], a[1], a[2], a[3], ttl_s);
    request_done(q);
}

uint64_t askers_tick(uint64_t now)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < REQUESTS; i++) {
        struct request *q = &D.q[i];
        if (!q->used)
            continue;
        if (now < q->deadline) {
            next = q->deadline < next ? q->deadline : next;
            continue;
        }
        dns_cancel(&D.r, q->cookie);
        (void)dns_reply_resolve(q->txn, ERR_TIMED_OUT, 0, 0, 0, 0, 0, 0);
        request_done(q);
    }
    return next;
}

/* ---- an opener's channel ---------------------------------------------------------- */

static status_t op_resolve(void *ctx, struct idl_txn txn, const uint8_t name[256],
                           uint32_t timeout_ms, uint8_t *count, uint32_t *a0, uint32_t *a1,
                           uint32_t *a2, uint32_t *a3, uint32_t *ttl)
{
    (void)count, (void)a0, (void)a1, (void)a2, (void)a3, (void)ttl;   /* answered with the txn */
    struct asker *a = ctx;
    char text[DNS_TEXT_MAX];
    if (strnlen((const char *)name, DNS_TEXT_MAX) == DNS_TEXT_MAX || !timeout_ms ||
        timeout_ms > DNS_TIMEOUT_MAX)
        return ERR_INVALID_ARGS;
    memcpy(text, name, DNS_TEXT_MAX);
    if (a->inflight >= DNS_PER_OPENER)
        return ERR_NO_RESOURCES;
    struct request *q = NULL;
    for (unsigned i = 0; i < REQUESTS && !q; i++)
        if (!D.q[i].used)
            q = &D.q[i];
    if (!q)
        return ERR_NO_RESOURCES;   /* not in practice: room for every opener's share */
    uint64_t t = now();
    *q = (struct request){ .used = true, .cookie = ++D.next_cookie,
                           .asker = (unsigned)(a - D.a), .txn = txn,
                           .deadline = t + (uint64_t)timeout_ms * NS_PER_MS };
    a->inflight++;
    status_t st = dns_ask_as(&D.r, t, text, q->cookie, a->sys);
    if (st != OK) {   /* no answer comes: the reply is this */
        request_done(q);
        return st;
    }
    return IDL_LATER;   /* answered already (cached), or when the answer comes */
}

static const struct dns_ops asker_ops = { .resolve = op_resolve };

static void asker_close(unsigned i)
{
    for (unsigned k = 0; k < REQUESTS; k++) {
        struct request *q = &D.q[k];
        if (q->used && q->asker == i) {
            dns_cancel(&D.r, q->cookie);
            request_done(q);
        }
    }
    jam_handle_close(D.a[i].ch);   /* its binding goes with our only handle */
    D.a[i].ch = HANDLE_INVALID;
    D.a[i].pending = false;
    D.a[i].inflight = 0;
}

static void serve_asker(unsigned i)
{
    struct asker *a = &D.a[i];
    a->pending = false;
    for (unsigned k = 0; k < BUDGET; k++) {
        status_t st = dns_serve_one(a->ch, &asker_ops, a);
        if (st == OK)
            continue;
        if (st != ERR_SHOULD_WAIT)
            asker_close(i);   /* gone (ERR_PEER_CLOSED), or its channel broke */
        return;
    }
    a->pending = true;   /* its budget is spent: more may be queued */
}

/* ---- the shared channel ------------------------------------------------------------ */

/* Ordinary openers now. */
static unsigned prog_openers(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < DNS_OPENERS; i++)
        n += D.a[i].ch && !D.a[i].sys;
    return n;
}

/* svc.connect: a channel of the caller's own; ctx is non-NULL on
 * /svc/dns-sys's shared channel. */
static status_t on_connect(void *ctx, handle_t *out)
{
    bool sys = ctx != NULL;
    if (!sys && prog_openers() >= DNS_PROG_OPENERS) {
        D.refused_shares++;
        return ERR_NO_RESOURCES;
    }
    for (unsigned i = 0; i < DNS_OPENERS; i++) {
        struct asker *a = &D.a[i];
        if (a->ch)
            continue;
        handle_t mine, theirs;
        status_t st = jam_channel_create(&mine, &theirs);
        if (st != OK)
            return st;
        a->gen++;
        st = jam_port_bind(D.port, mine, key_of(i), SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
        if (st != OK) {
            jam_handle_close(mine);
            jam_handle_close(theirs);
            return st;
        }
        *a = (struct asker){ .ch = mine, .gen = a->gen, .pending = true, .sys = sys };
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

/* The shared channel answers nothing but connect: a later answer there
 * could go to any of its holders. */
static const struct dns_ops shared_ops = { 0 };

static uint32_t shared_dispatch(void *ctx, const void *req, uint32_t n, void *rep, handle_t *rhs,
                                uint32_t *rhn)
{
    return dns_dispatch(&shared_ops, ctx, req, n, rep, rhs, rhn);
}

/* One of the shared channels (*ch; sys: /svc/dns-sys's). */
static void serve_shared(handle_t *ch, bool *pending, bool sys)
{
    *pending = false;
    for (unsigned k = 0; k < BUDGET; k++) {
        status_t st = svc_serve_request(*ch, shared_dispatch, on_connect, sys ? &D : NULL);
        if (st == OK)
            continue;
        if (st != ERR_SHOULD_WAIT) {
            /* Every holder is gone, init's too: no new openers. */
            printf("dns: /svc/%s's channel is closed (%s): no new openers\n",
                   sys ? SVC_DNS_SYS : SVC_DNS, status_str(st));
            jam_handle_close(*ch);
            *ch = HANDLE_INVALID;
        }
        return;
    }
    *pending = true;
}

status_t askers_init(handle_t shared, handle_t shared_sys)
{
    D.shared = shared;
    D.shared_pending = true;   /* connects may be queued from before a restart */
    D.shared_sys = shared_sys;
    D.shared_sys_pending = shared_sys != 0;
    status_t st = jam_port_bind(D.port, shared, KEY_SHARED, SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st == OK && shared_sys)
        st = jam_port_bind(D.port, shared_sys, KEY_SHARED_SYS, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    return st;
}

void askers_packet(uint64_t key)
{
    uint32_t low = (uint32_t)(key & 0xff), gen = (uint32_t)(key >> 8);
    if (key == KEY_SHARED) {
        D.shared_pending = D.shared != 0;
        return;
    }
    if (key == KEY_SHARED_SYS) {
        D.shared_sys_pending = D.shared_sys != 0;
        return;
    }
    unsigned i = low - KEY_ASKER;
    if (i < DNS_OPENERS && D.a[i].ch && D.a[i].gen == gen)
        D.a[i].pending = true;
}

void askers_serve(void)
{
    if (D.shared && D.shared_pending)
        serve_shared(&D.shared, &D.shared_pending, false);
    if (D.shared_sys && D.shared_sys_pending)
        serve_shared(&D.shared_sys, &D.shared_sys_pending, true);
    for (unsigned i = 0; i < DNS_OPENERS; i++)
        if (D.a[i].ch && D.a[i].pending)
            serve_asker(i);
}

bool askers_pending(void)
{
    if ((D.shared && D.shared_pending) || (D.shared_sys && D.shared_sys_pending))
        return true;
    for (unsigned i = 0; i < DNS_OPENERS; i++)
        if (D.a[i].ch && D.a[i].pending)
            return true;
    return false;
}
