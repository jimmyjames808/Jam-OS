/* serve: one client's connection (serve.h has its states). Its request's
 * head is read into the client's own buffer (at most HEAD_MAX bytes, all of
 * it by HEAD_WAIT), checked by <http.h>'s strict parser, and answered with
 * the share's file whatever path it asked for: GET and HEAD, one byte range
 * (206), keep-alive unless the client says close (or sent its next request
 * before this answer: no pipelining). Nothing a request says
 * is ever opened or looked up. The file's bytes go a chunk at a time: the
 * chunk is filled by a read of the share's (share_want), written into the
 * connection's tx ring as room comes, and the next one asked for once it
 * is all in. */
#include <ipv4.h>
#include "serve.h"

struct client clients[SERVE_CLIENTS];

static unsigned slot_of(const struct client *c)
{
    return (unsigned)(c - clients);
}

static void interest(struct client *c, uint32_t want)
{
    (void)netwait_modify(serve_w, c->id, want);   /* its own entry: can't fail */
}

/* "10.2.21.174:41000" into buf. */
static const char *who(const struct client *c, char buf[24])
{
    char a[IPV4_TEXT_MAX];
    snprintf(buf, 24, "%s:%u", ipv4_format(c->peer, a), c->peer_port);
    return buf;
}

static void log_request(const struct client *c, const char *note)
{
    char w[24];
    serve_log("serve: %s %s %s %u, %lu bytes%s%s\n", who(c, w),
              c->req.method == HTTP_HEAD ? "HEAD" : "GET", c->req.target[0] ? c->req.target : "?",
              c->status, (unsigned long)c->sent, note ? ": " : "", note ? note : "");
}

void client_end(struct client *c, const char *why)
{
    if (!c->used)
        return;
    if (why && c->state == C_SEND)
        log_request(c, why);
    else if (why)
        serve_log("serve: %s: %s\n", who(c, (char[24]){ 0 }), why);
    share_forget(c->sh, c);
    (void)netwait_remove(serve_w, c->id);   /* in the set since client_new */
    net_close(&c->s);   /* bytes it sent that we never read: a reset */
    c->sh->clients--;
    c->used = false;
    c->waiting = false;
}

bool client_new(struct share *sh, struct net_sock *s, uint32_t peer, uint16_t port)
{
    struct client *c = NULL;
    for (unsigned i = 0; i < SERVE_CLIENTS && !c; i++)
        if (!clients[i].used)
            c = &clients[i];
    if (c && !c->chunk)
        c->chunk = malloc(CHUNK);   /* kept for the slot's next clients */
    if (!c || !c->chunk)
        return false;
    uint8_t *chunk = c->chunk;
    memset(c, 0, sizeof(*c));
    *c = (struct client){ .used = true, .state = C_HEAD, .sh = sh, .s = *s, .peer = peer,
                          .peer_port = port, .deadline = now() + HEAD_WAIT, .chunk = chunk };
    struct netwait_sock ws;
    net_sock_waitable(&c->s, &ws);
    if (netwait_add_sock(serve_w, &ws, NETWAIT_READ, serve_key(T_CLIENT, slot_of(c)), &c->id) !=
        OK) {
        c->used = false;
        return false;
    }
    sh->clients++;
    return true;
}

/* The response's head (and, for an error, its short body) into c->out. */
static void head_out(struct client *c, uint64_t from, uint64_t n, const char *body)
{
    const struct share *sh = c->sh;
    char extra[96] = "", name[64];
    const char *base = strrchr(sh->name, '/');
    base = base ? base + 1 : sh->name;
    size_t k = 0;
    for (; base[k] && k + 1 < sizeof(name); k++) {
        char ch = base[k];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '.' || ch == '-' || ch == '_';
        name[k] = ok ? ch : '_';
    }
    name[k] = 0;
    if (c->status == 206)
        snprintf(extra, sizeof(extra), "Content-Range: bytes %lu-%lu/%lu\r\n", (unsigned long)from,
                 (unsigned long)(from + n - 1), (unsigned long)sh->size);
    else if (c->status == 416)
        snprintf(extra, sizeof(extra), "Content-Range: bytes */%lu\r\n", (unsigned long)sh->size);
    bool ok = c->status < 300;
    int len = snprintf(c->out, sizeof(c->out),
                       "HTTP/1.1 %u %s\r\nServer: jamos-serve\r\nContent-Type: %s\r\n"
                       "Content-Length: %lu\r\n%s%s%s%s%sConnection: %s\r\n\r\n%s",
                       c->status, http_reason(c->status), ok ? sh->type : "text/plain",
                       (unsigned long)(ok ? n : strlen(body)), ok ? "Accept-Ranges: bytes\r\n" : "",
                       extra, ok ? "Content-Disposition: inline; filename=\"" : "",
                       ok ? name : "", ok ? "\"\r\n" : "", c->close_after ? "close" : "keep-alive",
                       ok ? "" : body);
    c->out_len = len < 0 ? 0 : (size_t)len < sizeof(c->out) ? (size_t)len : sizeof(c->out) - 1;
    c->out_sent = 0;
}

static void send_some(struct client *c);

/* An answer that ends the connection: 400, 416, 431, 501, 505. */
static void refuse(struct client *c, unsigned status)
{
    char body[48];
    c->status = status;
    c->close_after = true;
    c->left = 0;
    c->sent = 0;
    c->chunk_len = c->chunk_sent = 0;
    snprintf(body, sizeof(body), "%u %s\n", status, http_reason(status));
    head_out(c, 0, 0, body);
}

/* A whole head of len bytes is in: what to answer. */
static void start_response(struct client *c, size_t len)
{
    struct share *sh = c->sh;
    unsigned bad = http_request_parse(c->head, len, &c->req);
    /* Bytes after the head: a pipelined request, which is not taken (this
     * one is answered, then the connection closes, as RFC 9112 9.3.2 allows). */
    bool more = c->have > len;
    c->have = 0;
    c->sent = 0;
    c->chunk_len = c->chunk_sent = 0;
    sh->requests++;
    uint64_t from = 0, n = 0;
    if (!bad && http_request_span(&c->req, sh->size, &from, &n) != OK)
        bad = 416;
    if (bad) {
        refuse(c, bad);
    } else {
        c->status = c->req.range ? 206 : 200;
        c->close_after = c->req.close || more;
        c->at = from;
        c->left = c->req.method == HTTP_HEAD ? 0 : n;
        head_out(c, from, n, "");
    }
    c->state = C_SEND;
    c->deadline = now() + IDLE_WAIT;
    interest(c, NETWAIT_WRITE);
    send_some(c);
}

/* What came of the request's head. */
static void read_head(struct client *c)
{
    for (;;) {   /* each turn reads, or decides */
        size_t len, n = net_read_some(&c->s, c->head + c->have, HEAD_MAX - c->have);
        c->have += n;
        status_t st = http_head_end(c->head, c->have, &len);
        if (st == OK) {
            start_response(c, len);
            return;
        }
        if (st == ERR_OUT_OF_RANGE || c->have == HEAD_MAX) {
            c->req = (struct http_request){ 0 };
            c->sh->requests++;
            refuse(c, 431);
            c->state = C_SEND;
            c->deadline = now() + IDLE_WAIT;
            interest(c, NETWAIT_WRITE);
            send_some(c);
            return;
        }
        if (n)
            continue;
        if (sockring_at_end(&c->s.r.rx))
            client_end(c, c->have ? "the client ended inside its request" : NULL);
        return;
    }
}

/* All of the response is in the ring: log it, then the next request or the end. */
static void finished(struct client *c)
{
    log_request(c, NULL);
    if (c->close_after) {
        (void)net_shutdown(&c->s);   /* it has rings */
        c->state = C_CLOSING;
        c->deadline = now() + CLOSE_WAIT;
        interest(c, NETWAIT_READ);
        return;
    }
    c->state = C_HEAD;   /* the next request on the kept connection */
    c->deadline = now() + KEEP_WAIT;
    interest(c, NETWAIT_READ);
}

/* Write what fits: the head, then the chunk; then ask for the next chunk,
 * or end the response. */
static void send_some(struct client *c)
{
    for (;;) {   /* each turn writes a piece, or stops: no room, a read out, the end */
        size_t k;
        if (c->out_sent < c->out_len) {
            k = net_write_some(&c->s, c->out + c->out_sent, c->out_len - c->out_sent);
            c->out_sent += k;
        } else if (c->chunk_sent < c->chunk_len) {
            k = net_write_some(&c->s, c->chunk + c->chunk_sent, c->chunk_len - c->chunk_sent);
            c->chunk_sent += k;
            c->sent += k;
            c->sh->bytes += k;
        } else if (c->left) {
            if (!c->waiting)
                share_want(c->sh, c);   /* may answer at once (client_chunk) */
            if (c->used && c->waiting)
                interest(c, 0);   /* HUP only, until the chunk comes */
            return;
        } else {
            finished(c);
            return;
        }
        if (!k)
            return;   /* no room: WRITE says when */
        c->deadline = now() + IDLE_WAIT;
    }
}

void client_chunk(struct client *c, size_t n, status_t st)
{
    c->waiting = false;
    if (st != OK || !n) {
        client_end(c, st != OK ? "reading the file failed" : "the file ended early (changed?)");
        return;
    }
    c->chunk_len = n;
    c->chunk_sent = 0;
    c->at += n;
    c->left -= n;
    interest(c, NETWAIT_WRITE);
    send_some(c);
}

void client_ready(struct client *c, const struct netwait_ready *r)
{
    if (!c->used)
        return;
    if (c->state == C_HEAD && (r->ready & NETWAIT_READ)) {
        read_head(c);
    } else if (c->state == C_SEND && (r->ready & NETWAIT_WRITE)) {
        send_some(c);
    } else if (c->state == C_CLOSING && (r->ready & NETWAIT_READ)) {
        uint8_t junk[512];
        while (net_read_some(&c->s, junk, sizeof(junk)))   /* bounded by its rx ring */
            ;
        if (sockring_at_end(&c->s.r.rx))
            client_end(c, NULL);
    } else if (r->ready & (NETWAIT_HUP | NETWAIT_ERROR)) {
        client_end(c, c->state == C_SEND ? "the client went away" : NULL);
    }
}

uint64_t clients_expire(uint64_t t)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < SERVE_CLIENTS; i++) {
        struct client *c = &clients[i];
        if (!c->used)
            continue;
        if (t < c->deadline) {
            next = c->deadline < next ? c->deadline : next;
            continue;
        }
        client_end(c, c->state == C_SEND               ? "timed out"
                      : c->state == C_HEAD && c->have ? "no whole request in time"
                                                      : NULL);
    }
    return next;
}
