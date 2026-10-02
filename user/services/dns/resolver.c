/* dns: the queries in flight.
 *
 * Each name being resolved has a record of its own (struct dns_query):
 * its query id and local port, both random (io->random), its tries and
 * its deadline. Nothing one query does waits for another: dns_input
 * finds the query by the local port the reply came to, dns_tick does
 * what is due for each query on its own, and dns_deadline is the
 * earliest of them. So a server that never answers one name delays only
 * that name's askers (the plan's slow-peer rule).
 *
 * A query is sent DNS_TRIES times at most, to the servers in turn, the
 * waits 1, 2, 3 and 4 s (10 s in all). Retries keep the id and port, so
 * a late reply to an earlier try still counts. SERVFAIL (or REFUSED,
 * NOTIMP, FORMERR) moves on to the next try at once. A reply counts
 * only from a server in the list, from port 53, to the query's port,
 * with its id and question (msg.c); anything else is counted and
 * dropped, and the query goes on waiting, so a forged or broken
 * datagram can't end a query early.
 *
 * Askers of a name already in flight join its query (up to
 * DNS_MAX_WAITERS) instead of sending another. A CNAME whose target the
 * reply doesn't answer is followed with a new query for the target (a
 * new id and port), up to DNS_CNAME_MAX CNAMEs in all. Answers go into
 * the cache under the name asked. An answer is handed to every asker
 * after the query's record is freed, so the edge may call back in. */
#include <os.h>
#include "dns.h"

static bool unicast(uint32_t a)
{
    uint8_t top = (uint8_t)(a >> 24);
    return top != 0 && top != 127 && top < 224;
}

void dns_init(struct dns_resolver *r, const struct dns_io *io)
{
    memset(r, 0, sizeof(*r));
    r->io = io;
}

void dns_set_servers(struct dns_resolver *r, const uint32_t *servers, unsigned n)
{
    uint32_t keep[DNS_MAX_SERVERS];
    uint8_t k = 0;
    for (unsigned i = 0; i < n && k < DNS_MAX_SERVERS; i++)
        if (unicast(servers[i]))
            keep[k++] = servers[i];
    bool same = k == r->nservers && !memcmp(keep, r->servers, k * sizeof(uint32_t));
    memcpy(r->servers, keep, k * sizeof(uint32_t));
    r->nservers = k;
    if (!same)
        dns_cache_flush(&r->cache);
}

/* ---- one query -------------------------------------------------------------- */

static bool port_in_use(const struct dns_resolver *r, uint16_t port)
{
    for (unsigned i = 0; i < DNS_MAX_QUERIES; i++)
        if (r->q[i].used && r->q[i].port == port)
            return true;
    return false;
}

static uint16_t random_port(struct dns_resolver *r)
{
    uint16_t port = 0;
    for (unsigned guard = 0; guard < 8 && (!port || port_in_use(r, port)); guard++)
        port = (uint16_t)(DNS_PORT_MIN + r->io->random(r->io->ctx) % (65536u - DNS_PORT_MIN));
    return port;   /* 8 collisions in a row with 16 of 16384 ports: not in practice */
}

/* End q: free its record, then answer every asker. */
static void finish(struct dns_resolver *r, struct dns_query *q, status_t st,
                   const uint32_t *addr, unsigned n, uint32_t ttl)
{
    uint64_t cookies[DNS_MAX_WAITERS];
    unsigned nwait = q->nwait;
    memcpy(cookies, q->cookies, sizeof(cookies));
    uint16_t port = q->port;
    memset(q, 0, sizeof(*q));
    if (port)
        r->io->release(r->io->ctx, port);
    if (st == OK)
        r->stats.answered++;
    else
        r->stats.failed++;
    for (unsigned i = 0; i < nwait; i++)
        r->io->answer(r->io->ctx, cookies[i], st, addr, n, ttl);
}

/* Send q's next try. The port is chosen at the first send of a name,
 * and again while the edge says the one chosen is taken. */
static void send_try(struct dns_resolver *r, struct dns_query *q, uint64_t now)
{
    if (!r->nservers) {
        finish(r, q, ERR_BAD_STATE, NULL, 0, 0);
        return;
    }
    uint8_t buf[DNS_MSG_MAX];
    size_t len = dns_build_query(buf, sizeof(buf), q->id, q->cur);
    if (!len) {
        finish(r, q, ERR_INVALID_ARGS, NULL, 0, 0);   /* dns_name_ok passed: not in practice */
        return;
    }
    uint32_t server = r->servers[q->tries % r->nservers];
    status_t st = ERR_ALREADY_BOUND;
    for (unsigned i = 0; i < DNS_PORT_TRIES && st == ERR_ALREADY_BOUND; i++) {
        if (!q->port || i > 0)
            q->port = random_port(r);
        st = r->io->send(r->io->ctx, q->port, server, buf, len);
    }
    if (st == OK)
        r->stats.sent++;
    else
        r->stats.send_failed++;
    if (st == ERR_ALREADY_BOUND)
        q->port = 0;   /* no port of ours: none to release */
    q->tries++;
    q->deadline = now + (uint64_t)DNS_TRY_MS * q->tries * NS_PER_MS;
}

/* Ask for q->cur from scratch: a new id and port. */
static void ask(struct dns_resolver *r, struct dns_query *q, uint64_t now)
{
    if (q->port)
        r->io->release(r->io->ctx, q->port);
    q->port = 0;
    q->id = (uint16_t)r->io->random(r->io->ctx);
    q->tries = 0;
    send_try(r, q, now);
}

/* ---- the askers ---------------------------------------------------------------- */

static struct dns_query *in_flight(struct dns_resolver *r, const char *name)
{
    for (unsigned i = 0; i < DNS_MAX_QUERIES; i++)
        if (r->q[i].used && dns_name_eq(r->q[i].name, name))
            return &r->q[i];
    return NULL;
}

static struct dns_query *free_query(struct dns_resolver *r)
{
    for (unsigned i = 0; i < DNS_MAX_QUERIES; i++)
        if (!r->q[i].used)
            return &r->q[i];
    return NULL;
}

/* Answer at once from the cache, if name is there. */
static bool cached(struct dns_resolver *r, uint64_t now, const char *name, uint64_t cookie)
{
    uint32_t addr[DNS_MAX_ADDRS], ttl;
    uint8_t n;
    if (dns_cache_get(&r->cache, now, name, addr, &n, &ttl) != OK)
        return false;
    r->stats.cached++;
    r->io->answer(r->io->ctx, cookie, OK, addr, n, ttl);
    return true;
}

status_t dns_ask(struct dns_resolver *r, uint64_t now, const char *name, uint64_t cookie)
{
    r->stats.asked++;
    uint32_t literal;
    if (dns_ipv4_literal(name, &literal)) {
        r->stats.literal++;
        r->io->answer(r->io->ctx, cookie, OK, &literal, 1, DNS_TTL_MAX);
        return OK;
    }
    char norm[DNS_NAME_MAX + 1];
    size_t len;
    if (!dns_name_ok(name, &len))
        return ERR_INVALID_ARGS;
    memcpy(norm, name, len);
    norm[len] = 0;   /* without the root's dot */
    if (cached(r, now, norm, cookie))
        return OK;
    if (!r->nservers)
        return ERR_BAD_STATE;
    struct dns_query *q = in_flight(r, norm);
    if (q) {
        if (q->nwait == DNS_MAX_WAITERS)
            return ERR_NO_RESOURCES;
        q->cookies[q->nwait++] = cookie;
        return OK;
    }
    if (!(q = free_query(r)))
        return ERR_NO_RESOURCES;
    *q = (struct dns_query){ .used = true, .ttl = DNS_TTL_MAX, .nwait = 1 };
    q->cookies[0] = cookie;
    memcpy(q->name, norm, len + 1);
    memcpy(q->cur, norm, len + 1);
    ask(r, q, now);
    return OK;
}

void dns_cancel(struct dns_resolver *r, uint64_t cookie)
{
    for (unsigned i = 0; i < DNS_MAX_QUERIES; i++) {
        struct dns_query *q = &r->q[i];
        if (!q->used)
            continue;
        unsigned k = 0;
        while (k < q->nwait) {
            if (q->cookies[k] == cookie)
                q->cookies[k] = q->cookies[--q->nwait];   /* the last one moves here */
            else
                k++;
        }
        if (q->nwait)
            continue;
        if (q->port)
            r->io->release(r->io->ctx, q->port);
        memset(q, 0, sizeof(*q));
    }
}

/* ---- replies and time ------------------------------------------------------------ */

/* A checked reply to q: act on what it says. */
static void outcome(struct dns_resolver *r, struct dns_query *q, uint64_t now,
                    const struct dns_result *res)
{
    uint32_t ttl = res->ttl < q->ttl ? res->ttl : q->ttl;
    unsigned hops = q->hops + res->cnames;
    if (hops > DNS_CNAME_MAX) {
        finish(r, q, ERR_OUT_OF_RANGE, NULL, 0, 0);
        return;
    }
    size_t len;
    switch (res->outcome) {
    case DNS_ADDRS:
        dns_cache_put(&r->cache, now, q->name, res->addr, res->naddr, ttl);
        finish(r, q, OK, res->addr, res->naddr, ttl);
        break;
    case DNS_NO_NAME:
    case DNS_NO_DATA:
        finish(r, q, ERR_NOT_FOUND, NULL, 0, 0);
        break;
    case DNS_TRUNCATED:
        finish(r, q, ERR_NOT_SUPPORTED, NULL, 0, 0);
        break;
    case DNS_SERVER_FAIL:
        if (q->tries < DNS_TRIES)
            send_try(r, q, now);
        else
            finish(r, q, ERR_IO, NULL, 0, 0);
        break;
    case DNS_FOLLOW:
        if (!dns_name_ok(res->next, &len)) {
            finish(r, q, ERR_NOT_FOUND, NULL, 0, 0);   /* a target we can't ask for */
            break;
        }
        q->hops = (uint8_t)hops;
        q->ttl = ttl;
        memcpy(q->cur, res->next, len);
        q->cur[len] = 0;
        ask(r, q, now);
        break;
    }
}

static bool from_server(const struct dns_resolver *r, const struct dns_datagram *d)
{
    if (d->src_port != DNS_PORT)
        return false;
    for (unsigned i = 0; i < r->nservers; i++)
        if (r->servers[i] == d->src)
            return true;
    return false;
}

void dns_input(struct dns_resolver *r, uint64_t now, const struct dns_datagram *d)
{
    r->stats.received++;
    struct dns_query *q = NULL;
    for (unsigned i = 0; i < DNS_MAX_QUERIES && !q; i++)
        if (r->q[i].used && r->q[i].port && r->q[i].port == d->port)
            q = &r->q[i];
    if (!q || !from_server(r, d)) {
        r->stats.foreign++;
        return;
    }
    struct dns_result res;
    status_t st = dns_parse_reply(d->msg, d->len, q->id, q->cur, &res);
    if (st == ERR_NOT_FOUND)
        r->stats.foreign++;
    else if (st != OK)
        r->stats.malformed++;
    else
        outcome(r, q, now, &res);
}

void dns_tick(struct dns_resolver *r, uint64_t now)
{
    for (unsigned i = 0; i < DNS_MAX_QUERIES; i++) {
        struct dns_query *q = &r->q[i];
        if (!q->used || now < q->deadline)
            continue;
        if (q->tries < DNS_TRIES) {
            send_try(r, q, now);
            continue;
        }
        r->stats.timeouts++;
        finish(r, q, ERR_TIMED_OUT, NULL, 0, 0);
    }
}

uint64_t dns_deadline(const struct dns_resolver *r)
{
    uint64_t d = DEADLINE_NEVER;
    for (unsigned i = 0; i < DNS_MAX_QUERIES; i++)
        if (r->q[i].used && r->q[i].deadline < d)
            d = r->q[i].deadline;
    return d;
}
