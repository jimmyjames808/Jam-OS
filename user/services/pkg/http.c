/* One GET of a file of the repository over plain HTTP/1.1 (pkg.h), with every
 * wait bounded: the resolver's answer, the connection, the answer's head,
 * each piece of the body (a body that sends nothing for IDLE_WAIT has
 * stalled), and the whole transfer (a floor of 16 KiB a second on top of a
 * minute, so a server that trickles can't hold pkg for ever). Every wait is
 * one netwait_wait on a set that also holds the shell's stop channel, so
 * Ctrl+C ends any of them at once.
 *
 * The repository is trusted for nothing but bytes: the answer must be 200
 * (no redirect is followed: the repository is where the owner said it is),
 * its length must be the one the signed index promised when that is known,
 * and never more than the caller's bound. The body is pushed to a sink in
 * pieces; the sink's PK_FEED_END ends the transfer early without an error
 * (a package's head is all a question needs).
 *
 * The pure parts of HTTP are <http.h>'s (http_response_parse, the chunked
 * decoder); this file only moves bytes. */
#include <stdarg.h>
#include <dns.h>
#include <idl/dns.h>
#include <ipv4.h>
#include <net.h>
#include <netwait.h>
#include "pkg.h"

#define ROLE_STOP     PROGRAM_SR_STOP
#define TAG_STOP      ((void *)1)
#define TAG_SOCK      ((void *)2)
#define TAG_DNS       ((void *)3)
#define DNS_MS        10000u
#define CONNECT_WAIT  (10 * NS_PER_S)
#define HEAD_WAIT     (20 * NS_PER_S)
#define IDLE_WAIT     (30 * NS_PER_S)
#define FLOOR_BPS     (16u * 1024)
#define BASE_BUDGET   (60 * NS_PER_S)

struct conn {
    struct net_sock s;
    bool            open;
    struct netwait *w;
    uint32_t        sock_id, stop_id;
};

static uint8_t head_buf[HTTP_HEAD_MAX];
static uint8_t body_buf[64 * 1024];

bool pk_net_stopped(void)
{
    signals_t seen = 0;
    handle_t stop = startup_handle(ROLE_STOP);
    return stop && jam_object_wait_one(stop, SIG_READABLE | SIG_PEER_CLOSED, 0, &seen) == OK &&
           (seen & (SIG_READABLE | SIG_PEER_CLOSED));
}

static status_t conn_init(struct conn *c)
{
    memset(c, 0, sizeof(*c));
    status_t st = netwait_create(4, &c->w);
    handle_t stop = startup_handle(ROLE_STOP);
    if (st == OK && stop) {
        struct netwait_handle h = { stop, SIG_READABLE, 0, SIG_PEER_CLOSED };
        st = netwait_add_handle(c->w, &h, NETWAIT_READ, TAG_STOP, &c->stop_id);
    }
    return st;
}

/* Wait for the entry tagged `tag` (or the stop channel) until the deadline:
 * OK with its readiness; ERR_CANCELED: stopped; ERR_TIMED_OUT; the entry's
 * error when it has one and nothing to read. */
static status_t wait_for(struct conn *c, void *tag, uint64_t deadline, uint32_t *ready)
{
    for (;;) {   /* each turn waits, to the deadline */
        struct netwait_ready r[4];
        uint32_t n = 0;
        status_t st = netwait_wait(c->w, deadline, r, 4, &n);
        if (st != OK)
            return st;
        *ready = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (r[i].user == TAG_STOP)
                return ERR_CANCELED;
            if (r[i].user != tag)
                continue;
            *ready |= r[i].ready;
            if ((r[i].ready & NETWAIT_ERROR) && !(r[i].ready & NETWAIT_READ))
                return r[i].error != OK ? r[i].error : ERR_PEER_CLOSED;
        }
        if (*ready)
            return OK;
    }
}

static status_t dns_answer(struct conn *c, handle_t ch, uint32_t txid, uint64_t deadline,
                           uint32_t *addr)
{
    for (;;) {   /* each turn reads a reply, or waits for one */
        _Alignas(8) uint8_t rep[DNS_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(ch, rep, sizeof(rep), &m);
        if ((st == OK || st == ERR_INTERNAL) && m.txid == txid) {
            uint32_t n = 0, a[DNS_ADDRS_MAX] = { 0 }, ttl = 0;
            if (st == OK)
                st = dns_resolve_result(rep, &m, a, &n, &ttl);
            if (st == OK && (!n || n > DNS_ADDRS_MAX))
                st = ERR_INTERNAL;
            if (st == OK)
                *addr = a[0];
            return st;
        }
        if (st == OK || st == ERR_INTERNAL) {
            idl_msg_drop(&m);   /* not ours */
            continue;
        }
        uint32_t ready;
        if (st != ERR_SHOULD_WAIT || (st = wait_for(c, TAG_DNS, deadline, &ready)) != OK)
            return st;
    }
}

static status_t resolve(struct conn *c, const char *host, uint32_t *addr)
{
    const char *end = NULL;
    if (ipv4_parse(host, addr, &end) && !*end)
        return OK;
    size_t len = strnlen(host, DNS_NAME_MAX + 1);
    if (!len || len > DNS_NAME_MAX)
        return ERR_INVALID_ARGS;
    handle_t ch;
    uint32_t id = 0, last = 0, txid = idl_txid_next(&last);
    status_t st = svc_open(SVC_DNS, &ch);
    if (st != OK)
        return st == ERR_NOT_FOUND ? ERR_PEER_CLOSED : st;
    struct netwait_handle h = { ch, SIG_READABLE, 0, SIG_PEER_CLOSED };
    st = netwait_add_handle(c->w, &h, NETWAIT_READ, TAG_DNS, &id);
    if (st == OK)
        st = dns_resolve_send(ch, txid, host, (uint32_t)len, DNS_MS);
    if (st == OK)
        st = dns_answer(c, ch, txid, now() + CONNECT_WAIT, addr);
    if (id)
        (void)netwait_remove(c->w, id);   /* ours: can't fail */
    jam_handle_close(ch);
    return st;
}

static void conn_close(struct conn *c);

/* One try at a connection to addr:port, OPEN, by the deadline. */
static status_t open_once(struct conn *c, uint32_t addr, uint16_t port, uint64_t deadline)
{
    status_t st = net_tcp_open(net_svc(), addr, port, 0, NET_TCP_BULK, &c->s);
    if (st != OK)
        return st;
    c->open = true;
    struct netwait_sock ws;
    net_sock_waitable(&c->s, &ws);
    st = netwait_add_sock(c->w, &ws, NETWAIT_WRITE, TAG_SOCK, &c->sock_id);
    while (st == OK) {   /* each turn looks, or waits to the deadline */
        uint32_t state, ready;
        status_t err;
        net_tcp_status(&c->s, &state, &err);
        if (state == SOCKRING_STATE_OPEN)
            return OK;
        if (state == SOCKRING_STATE_CLOSED)
            return err != OK ? err : ERR_PEER_CLOSED;
        st = wait_for(c, TAG_SOCK, deadline, &ready);
    }
    return st;
}

/* A connection to addr:port, OPEN, waiting at most CONNECT_WAIT. The address
 * comes before the link does, and a connect made in between fails with
 * ERR_BAD_STATE: it is tried again until the deadline. */
static status_t conn_open(struct conn *c, uint32_t addr, uint16_t port)
{
    uint64_t deadline = now() + CONNECT_WAIT;
    status_t st = net_wait_up(net_svc(), deadline, NULL);
    while (st == OK) {   /* each turn is one try */
        st = open_once(c, addr, port, deadline);
        if (st != ERR_BAD_STATE || now() >= deadline)
            break;
        if (pk_net_stopped())
            return ERR_CANCELED;
        conn_close(c);
        (void)jam_sleep_until(now() + 200 * NS_PER_MS);   /* its status says only that it woke */
        st = OK;
    }
    return st;
}

static status_t conn_write(struct conn *c, const void *data, size_t n, uint64_t deadline)
{
    status_t st = netwait_modify(c->w, c->sock_id, NETWAIT_WRITE);
    for (size_t done = 0; st == OK && done < n;) {   /* each turn writes, or waits */
        size_t k = net_write_some(&c->s, (const uint8_t *)data + done, n - done);
        done += k;
        if (k || done == n)
            continue;
        uint32_t state, ready;
        status_t err;
        net_tcp_status(&c->s, &state, &err);
        if (state == SOCKRING_STATE_CLOSED)
            return err != OK ? err : ERR_PEER_CLOSED;
        st = wait_for(c, TAG_SOCK, deadline, &ready);
    }
    return st;
}

/* Some bytes (at most cap) by the deadline: *got > 0, or 0 at the peer's end. */
static status_t conn_read(struct conn *c, void *buf, size_t cap, uint64_t deadline, size_t *got)
{
    status_t st = netwait_modify(c->w, c->sock_id, NETWAIT_READ);
    while (st == OK) {   /* each turn reads, ends, or waits */
        *got = net_read_some(&c->s, buf, cap);
        if (*got || sockring_at_end(&c->s.r.rx))
            return OK;
        uint32_t state, ready = 0;
        status_t err;
        net_tcp_status(&c->s, &state, &err);
        if (state == SOCKRING_STATE_CLOSED)
            return err != OK ? err : ERR_PEER_CLOSED;
        st = wait_for(c, TAG_SOCK, deadline, &ready);
    }
    return st;
}

static void conn_close(struct conn *c)
{
    if (c->sock_id)
        (void)netwait_remove(c->w, c->sock_id);   /* ours: can't fail */
    c->sock_id = 0;
    if (c->open)
        net_close(&c->s);
    c->open = false;
}

/* ---- the request ---------------------------------------------------------------------- */

static const char *why_net(status_t st)
{
    switch (st) {
    case ERR_NOT_FOUND:     return "refused (nothing listens there), or no such name";
    case ERR_TIMED_OUT:     return "no answer in time";
    case ERR_BAD_STATE:     return "no network address or link (see `net`)";
    case ERR_PEER_CLOSED:   return "the connection was reset, or netstack is gone";
    case ERR_ACCESS_DENIED: return "an address programs may not reach";
    case ERR_NO_RESOURCES:  return "too many connections open";
    case ERR_IO:            return "the DNS servers failed";
    case ERR_INVALID_ARGS:  return "not a host name";
    default:                return status_str(st);
    }
}

static status_t say(struct pk_get *g, status_t st, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static status_t say(struct pk_get *g, status_t st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->why, sizeof(g->why), fmt, ap);
    va_end(ap);
    return st;
}

static status_t send_request(struct conn *c, const struct http_url *u, const char *path,
                             uint64_t deadline)
{
    char req[HTTP_PATH_MAX + HTTP_HOST_MAX + 160], hp[HTTP_HOST_MAX + 8];
    http_url_hostport(u, hp, sizeof(hp));
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: jamos-pkg/1\r\n"
                     "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                     path, hp);
    return conn_write(c, req, (size_t)n, deadline);
}

/* The answer's head into head_buf: *len bytes of it, *have bytes read in all. */
static status_t read_head(struct conn *c, struct pk_get *g, size_t *len, size_t *have)
{
    uint64_t deadline = now() + HEAD_WAIT;
    size_t n = 0;
    for (;;) {   /* each turn reads more of it, by the deadline */
        status_t st = http_head_end(head_buf, n, len);
        if (st == OK) {
            *have = n;
            return OK;
        }
        if (st == ERR_OUT_OF_RANGE)
            return say(g, st, "the server's answer has a head over %u bytes", HTTP_HEAD_MAX);
        size_t got = 0;
        st = conn_read(c, head_buf + n, sizeof(head_buf) - n, deadline, &got);
        if (st == OK && !got)
            return say(g, ERR_PEER_CLOSED, "the server closed the connection before its "
                       "answer's head ended");
        if (st != OK)
            return say(g, st, "reading the server's answer: %s", why_net(st));
        n += got;
    }
}

struct body {
    enum { BY_LENGTH, BY_CHUNKS, BY_CLOSE } how;
    uint64_t           left;
    struct http_chunks chunks;
    bool               done;
    uint64_t           got;      /* bytes of the body delivered */
};

/* n bytes of the connection's into the sink. */
static status_t feed(struct pk_get *g, struct body *b, const uint8_t *in, size_t n,
                     pk_feed_fn sink, void *ctx)
{
    status_t st = OK;
    while (n && st == OK && !b->done) {   /* each turn takes a byte at least */
        const uint8_t *data = in;
        size_t used = n, dn = n;
        if (b->how == BY_LENGTH) {
            dn = b->left < n ? (size_t)b->left : n;
            used = n;
            b->left -= dn;
            b->done = !b->left;
        } else if (b->how == BY_CHUNKS) {
            size_t at;
            st = http_chunks_step(&b->chunks, in, n, &used, &at, &dn);
            if (st != OK)
                return say(g, st, "the server's chunked body is broken");
            data = in + at;
            b->done = b->chunks.done;
        }
        if (b->got + dn > g->max)
            return say(g, ERR_OUT_OF_RANGE, "the server sent more than %llu bytes",
                       (unsigned long long)g->max);
        if (dn)
            st = sink(ctx, data, dn);
        b->got += dn;
        in += used;
        n -= used;
        if (g->progress && st == OK)
            g->progress(g->progress_ctx, b->got, g->want);
    }
    return st;
}

static status_t read_body(struct conn *c, struct pk_get *g, const struct http_response *r,
                          size_t start, size_t have, uint64_t end, pk_feed_fn sink, void *ctx)
{
    struct body b = { .how = r->chunked ? BY_CHUNKS : r->length >= 0 ? BY_LENGTH : BY_CLOSE };
    b.left = r->length >= 0 ? (uint64_t)r->length : 0;
    b.done = b.how == BY_LENGTH && !b.left;
    http_chunks_init(&b.chunks);
    status_t st = feed(g, &b, head_buf + start, have - start, sink, ctx);
    while (st == OK && !b.done) {   /* each turn reads, by the idle and the total deadlines */
        size_t got = 0;
        uint64_t t = now(), dl = t + IDLE_WAIT < end ? t + IDLE_WAIT : end;
        st = t >= end ? ERR_TIMED_OUT : conn_read(c, body_buf, sizeof(body_buf), dl, &got);
        if (st == OK && !got) {
            if (b.how == BY_CLOSE)
                break;
            return say(g, ERR_PEER_CLOSED, "the server closed the connection before the "
                       "body's end");
        }
        if (st == ERR_TIMED_OUT)
            return say(g, st, "the transfer stalled or took too long");
        if (st != OK && st != ERR_CANCELED)
            return say(g, st, "reading the body: %s", why_net(st));
        if (st == OK)
            st = feed(g, &b, body_buf, got, sink, ctx);
    }
    if (st == OK && g->want && b.got != g->want)
        return say(g, ERR_ACCESS_DENIED, "the server sent %llu bytes, not the %llu the index "
                   "names", (unsigned long long)b.got, (unsigned long long)g->want);
    return st;
}

static status_t start(struct conn *c, struct pk_get *g, const struct http_url *u,
                      const char *path, uint64_t end)
{
    uint32_t addr;
    status_t st = resolve(c, u->host, &addr);
    if (st != OK)
        return say(g, st, "%s: %s", u->host, why_net(st));
    st = conn_open(c, addr, u->port);
    if (st == OK)
        st = send_request(c, u, path, end);
    if (st != OK)
        return say(g, st, "%s port %u: %s", u->host, u->port, why_net(st));
    return OK;
}

/* The head of the answer, parsed and held to what pkg takes. */
static status_t take_head(struct pk_get *g, struct http_response *r, size_t len)
{
    status_t st = http_response_parse(head_buf, len, r);
    if (st != OK)
        return say(g, st, "the server's answer is not HTTP/1.x as pkg takes it");
    if (r->status != 200)
        return say(g, ERR_NOT_FOUND, "the server answered %u %s", r->status, r->reason);
    if (r->length >= 0 && ((uint64_t)r->length > g->max || (g->want && (uint64_t)r->length != g->want)))
        return say(g, ERR_ACCESS_DENIED, "the file is %lld bytes, not what the index names",
                   (long long)r->length);
    return OK;
}

status_t pk_get_run(const struct http_url *base, const char *rel, struct pk_get *g,
                    pk_feed_fn sink, void *ctx)
{
    struct conn c;
    struct http_response r;
    char path[HTTP_PATH_MAX];
    size_t len, have;
    uint64_t span = g->want ? g->want : g->max;
    uint64_t end = now() + BASE_BUDGET + span / FLOOR_BPS * NS_PER_S;
    g->why[0] = '\0';
    if ((size_t)snprintf(path, sizeof(path), "%s%s", base->path, rel) >= sizeof(path))
        return say(g, ERR_INVALID_ARGS, "the repository's address is too long");
    status_t st = conn_init(&c);
    if (st == OK)
        st = start(&c, g, base, path, end);
    if (st == OK)
        st = read_head(&c, g, &len, &have);
    if (st == OK)
        st = take_head(g, &r, len);
    if (st == OK && http_response_has_body(&r, false))
        st = read_body(&c, g, &r, len, have, end, sink, ctx);
    conn_close(&c);
    if (c.w)
        netwait_destroy(c.w);
    return st == PK_FEED_END ? OK : st;
}
