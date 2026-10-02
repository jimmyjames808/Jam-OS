/* fetch: one GET and its answer, and the redirects after it. The request
 * says `Connection: close`, so a body without a length ends with the
 * connection. The answer's head must end within HTTP_HEAD_MAX bytes and
 * HEAD_WAIT; the body is taken by its Content-Length, its chunks
 * (<http.h> http_chunks_step) or the connection's end, and must keep
 * coming (IDLE_WAIT). Bytes past a Content-Length are ignored. */
#include <ipv4.h>
#include "fetch.h"

static uint8_t head_buf[HTTP_HEAD_MAX];
static uint8_t body_buf[64 * 1024];

/* A connection's error in words. */
static const char *why(status_t st)
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

static status_t send_request(struct conn *c, const struct http_url *u)
{
    char req[HTTP_PATH_MAX + HTTP_HOST_MAX + 160], hp[HTTP_HOST_MAX + 8];
    http_url_hostport(u, hp, sizeof(hp));
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: jamos-fetch/1\r\n"
                     "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                     u->path, hp);
    return conn_write(c, req, (size_t)n, now() + CONNECT_WAIT);
}

/* The answer's head into head_buf: its *len bytes, and *have bytes read
 * in all (the body's first bytes after it). */
static status_t read_head(struct conn *c, size_t *len, size_t *have)
{
    uint64_t deadline = now() + HEAD_WAIT;
    size_t n = 0;
    for (;;) {   /* each turn reads more of it, by the deadline */
        status_t st = http_head_end(head_buf, n, len);
        if (st == OK) {
            *have = n;
            return OK;
        }
        if (st == ERR_OUT_OF_RANGE) {
            printf("fetch: the server's answer has a head over %u bytes: refused\n",
                   HTTP_HEAD_MAX);
            return st;
        }
        size_t got = 0;
        st = conn_read(c, head_buf + n, sizeof(head_buf) - n, deadline, &got);
        if (st == ERR_TIMED_OUT)
            printf("fetch: the server sent no whole answer head in %u s (%lu bytes of it)\n",
                   (unsigned)(HEAD_WAIT / NS_PER_S), (unsigned long)n);
        else if (st == OK && !got)
            printf("fetch: the server closed the connection before its answer's head ended\n");
        else if (st != OK && st != ERR_CANCELED)
            printf("fetch: reading the answer: %s\n", why(st));
        if (st != OK)
            return st;
        if (!got)
            return ERR_PEER_CLOSED;
        n += got;
    }
}

/* How the body ends. */
struct body {
    enum { BY_LENGTH, BY_CHUNKS, BY_CLOSE } how;
    uint64_t           left;     /* BY_LENGTH: bytes still to come */
    struct http_chunks chunks;   /* BY_CHUNKS */
    bool               done;
};

/* n bytes of the connection's into the body. */
static status_t feed(struct body *b, const uint8_t *in, size_t n)
{
    if (b->how == BY_CLOSE)
        return fetch_out(in, n);
    if (b->how == BY_LENGTH) {
        size_t k = b->left < n ? (size_t)b->left : n;
        b->left -= k;
        b->done = !b->left;
        return k ? fetch_out(in, k) : OK;
    }
    while (n && !b->chunks.done) {   /* each step takes a byte at least */
        size_t used, data, dn;
        status_t st = http_chunks_step(&b->chunks, in, n, &used, &data, &dn);
        if (st != OK) {
            printf("fetch: the server's chunked body is broken: refused\n");
            return st;
        }
        if (dn && (st = fetch_out(in + data, dn)) != OK)
            return st;
        in += used;
        n -= used;
    }
    b->done = b->chunks.done;
    return OK;
}

static status_t read_body(struct conn *c, const struct http_response *r, size_t start,
                          size_t have)
{
    struct body b = { .how = r->chunked ? BY_CHUNKS : r->length >= 0 ? BY_LENGTH : BY_CLOSE };
    b.left = r->length >= 0 ? (uint64_t)r->length : 0;
    b.done = b.how == BY_LENGTH && !b.left;
    http_chunks_init(&b.chunks);
    status_t st = feed(&b, head_buf + start, have - start);
    while (st == OK && !b.done) {   /* each turn reads, by the idle deadline */
        size_t got = 0;
        st = conn_read(c, body_buf, sizeof(body_buf), now() + IDLE_WAIT, &got);
        if (st == OK && !got) {
            if (b.how == BY_CLOSE)
                return OK;
            printf("fetch: the server closed the connection before the body's end\n");
            return ERR_PEER_CLOSED;
        }
        if (st == ERR_TIMED_OUT)
            printf("fetch: the server sent nothing for %u s: given up\n",
                   (unsigned)(IDLE_WAIT / NS_PER_S));
        else if (st != OK && st != ERR_CANCELED)
            printf("fetch: reading the body: %s\n", why(st));
        if (st == OK)
            st = feed(&b, body_buf, got);
    }
    return st;
}

/* Connect to u and send the request: says what went wrong. */
static status_t start(struct conn *c, const struct http_url *u)
{
    uint32_t addr;
    char a[IPV4_TEXT_MAX];
    status_t st = conn_resolve(c, u->host, &addr);
    if (st != OK) {
        if (st != ERR_CANCELED)
            printf("fetch: %s: %s\n", u->host, st == ERR_NOT_FOUND ? "no such name" : why(st));
        return st;
    }
    st = conn_open(c, addr, u->port);
    if (st == OK)
        st = send_request(c, u);
    if (st != OK && st != ERR_CANCELED)
        printf("fetch: %s port %u: %s\n", ipv4_format(addr, a), u->port, why(st));
    return st;
}

static bool redirect(unsigned status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* The answer was a redirect: where to, into *next. */
static status_t follow(const struct http_url *from, const struct http_response *r, unsigned hop,
                       struct http_url *next)
{
    if (hop == FETCH_REDIRECTS) {
        printf("fetch: more than %u redirects: given up\n", FETCH_REDIRECTS);
        return ERR_OUT_OF_RANGE;
    }
    status_t st = http_url_resolve(from, r->location, next);
    if (st == ERR_NOT_SUPPORTED)
        printf("fetch: redirected (%u) to %s: only http:// is spoken here (https needs TLS, "
               "which Jam OS doesn't have yet)\n", r->status, r->location);
    else if (st != OK)
        printf("fetch: redirected (%u) to a Location it can't follow\n", r->status);
    else
        printf("fetch: %u %s: to http://%s:%u%s\n", r->status, r->reason, next->host, next->port,
               next->path);
    return st;
}

status_t get_url(struct conn *c, const struct http_url *url, struct http_url *final)
{
    struct http_url u = *url;
    for (unsigned hop = 0;; hop++) {   /* at most FETCH_REDIRECTS + 1 */
        struct http_response r;
        size_t len, have;
        status_t st = start(c, &u);
        if (st == OK)
            st = read_head(c, &len, &have);
        if (st == OK && (st = http_response_parse(head_buf, len, &r)) != OK)
            printf("fetch: the server's answer head is %s: refused\n",
                   st == ERR_NOT_SUPPORTED ? "in a transfer coding fetch doesn't take"
                                           : "not HTTP/1.x as fetch takes it");
        if (st == OK && redirect(r.status) && r.location[0]) {
            conn_close(c);
            if ((st = follow(&u, &r, hop, &u)) != OK)
                return st;
            continue;
        }
        if (st == OK && (r.status < 200 || r.status > 299)) {
            printf("fetch: the server answered %u %s\n", r.status, r.reason);
            st = ERR_NOT_FOUND;
        }
        if (st == OK) {
            fetch_started(r.chunked ? -1 : r.length, r.status);
            if (http_response_has_body(&r, false))
                st = read_body(c, &r, len, have);
        }
        conn_close(c);
        *final = u;
        return st;
    }
}
