/* tcptest serve: a listener and every connection it accepts in one wait
 * set (<netwait.h>), served by one thread (tcptest.h). Each connection
 * goes through three phases: reading the peer's bytes to its end (READ),
 * writing ours and the shutdown (WRITE), then waiting for CLOSED (HUP). */
#include <net.h>
#include <netwait.h>
#include <os.h>
#include "tcptest.h"

#define CONNS_MAX 64u
#define BATCH     16u

struct conn {
    struct net_sock s;
    bool            used;
    uint32_t        id;        /* its wait set entry */
    uint16_t        port;      /* its peer's port: the streams' seeds */
    uint64_t        got;       /* the peer's bytes read */
    uint64_t        sent;      /* ours written */
    bool            writing;   /* the peer's end came: our turn */
};

static struct conn conns[CONNS_MAX];
static struct net_listener lst;
static struct netwait *w;
static uint64_t want;          /* bytes each way a connection */
static uint32_t done, failed, accepted;

static void conn_end(struct conn *c, status_t why)
{
    if (why == OK)
        done++;
    else {
        failed++;
        printf("tcptest: connection from port %u: %s (read %lu, wrote %lu)\n", c->port,
               status_str(why), (unsigned long)c->got, (unsigned long)c->sent);
    }
    (void)netwait_remove(w, c->id);   /* in the set since accept */
    net_close(&c->s);
    c->used = false;
}

static void conn_new(void)
{
    struct conn *c = NULL;
    for (unsigned i = 0; i < CONNS_MAX && !c; i++)
        if (!conns[i].used)
            c = &conns[i];
    struct net_sock s;
    uint16_t port = 0;
    status_t st = net_tcp_accept_take(&lst, &s, NULL, &port);
    if (st == ERR_SHOULD_WAIT)
        return;
    (void)net_tcp_accept_send(&lst);   /* the next; failing, the listener's HUP says why */
    if (st != OK || !c) {
        printf("tcptest: accept: %s\n", c ? status_str(st) : "no room");
        failed++;
        if (st == OK)
            net_close(&s);
        return;
    }
    *c = (struct conn){ .s = s, .used = true, .port = port };
    struct netwait_sock ws;
    net_sock_waitable(&c->s, &ws);
    st = netwait_add_sock(w, &ws, NETWAIT_READ, c, &c->id);
    accepted++;
    if (st != OK) {
        failed++;
        net_close(&c->s);
        c->used = false;
    }
}

/* Read what is there, checked; at the peer's end, our turn to write. */
static void conn_read(struct conn *c)
{
    static uint8_t buf[16 * 1024];
    size_t n;
    while ((n = net_read_some(&c->s, buf, sizeof(buf))) > 0) {   /* bounded by the ring */
        if (c->got + n > want || !tt_same(c->port & 0xff, c->got, buf, n)) {
            conn_end(c, ERR_INTERNAL);
            return;
        }
        c->got += n;
    }
    if (!sockring_at_end(&c->s.r.rx))
        return;
    if (c->got != want) {
        conn_end(c, ERR_OUT_OF_RANGE);
        return;
    }
    c->writing = true;
    (void)netwait_modify(w, c->id, NETWAIT_WRITE);   /* its own entry: can't fail */
}

/* Write what fits; all of it: the shutdown, then wait for CLOSED. */
static void conn_write(struct conn *c)
{
    static uint8_t buf[16 * 1024];
    while (c->sent < want) {   /* bounded by the ring's room */
        size_t n = want - c->sent < sizeof(buf) ? (size_t)(want - c->sent) : sizeof(buf);
        tt_fill((c->port + 1) & 0xff, c->sent, buf, n);
        size_t k = net_write_some(&c->s, buf, n);
        c->sent += k;
        if (k < n)
            return;
    }
    (void)net_shutdown(&c->s);   /* it has rings */
    (void)netwait_modify(w, c->id, 0);   /* HUP only, from now on */
}

static void conn_ready(struct conn *c, const struct netwait_ready *r)
{
    if (r->ready & NETWAIT_HUP) {
        uint32_t state;
        status_t err;
        net_tcp_status(&c->s, &state, &err);
        conn_end(c, c->sent == want && err == OK ? OK : (err != OK ? err : ERR_BAD_STATE));
        return;
    }
    if (!c->writing && (r->ready & NETWAIT_READ))
        conn_read(c);
    else if (c->writing && (r->ready & NETWAIT_WRITE))
        conn_write(c);
}

static status_t setup(uint16_t port, uint32_t n)
{
    struct netwait_handle h;
    uint32_t id;
    handle_t net = svc_get(SVC_NET_LISTEN);
    status_t st = net ? net_wait_up(net, now() + TT_WAIT, NULL) : ERR_NOT_FOUND;
    if (st == OK)
        st = net_tcp_listen(net, port, NET_BACKLOG_MAX, 16384, 16384, &lst);
    if (st == OK)
        st = netwait_create(n + 1, &w);
    net_listener_waitable(&lst, &h);
    if (st == OK)
        st = netwait_add_handle(w, &h, NETWAIT_READ, NULL, &id);
    if (st == OK)
        st = net_tcp_accept_send(&lst);
    if (st == OK)
        printf("tcptest: listening on port %u\n", lst.port);
    return st;
}

int tt_serve(uint16_t port, uint32_t n, uint64_t bytes)
{
    struct netwait_ready r[BATCH];
    uint64_t deadline = now() + TT_WAIT, t0 = 0;
    want = bytes;
    status_t st = n && n <= CONNS_MAX ? setup(port, n) : ERR_INVALID_ARGS;
    while (st == OK && done + failed < n) {   /* each turn waits, to the deadline */
        uint32_t k;
        st = netwait_wait(w, deadline, r, BATCH, &k);
        for (uint32_t i = 0; st == OK && i < k; i++) {
            if (r[i].user) {
                conn_ready(r[i].user, &r[i]);
            } else if (r[i].ready & NETWAIT_HUP) {
                st = ERR_PEER_CLOSED;   /* the listener's channel: netstack is gone */
            } else {
                if (!t0)
                    t0 = now();
                conn_new();
            }
        }
    }
    uint64_t t = now() - t0;
    netwait_destroy(w);   /* before the sockets it holds are closed */
    for (unsigned i = 0; i < CONNS_MAX; i++)
        if (conns[i].used)
            net_close(&conns[i].s);
    net_listener_close(&lst);
    if (st != OK || failed) {
        printf("tcptest: FAIL: serve: %s, %u of %u done, %u failed\n", status_str(st), done, n,
               failed);
        return 1;
    }
    printf("tcptest: PASS: served %u connections at once from one wait set (%u accepted), "
           "%lu bytes each way each, %lu.%lu MB/s in all\n", done, accepted, (unsigned long)bytes,
           (unsigned long)(tt_rate10(2 * bytes * n, t) / 10),
           (unsigned long)(tt_rate10(2 * bytes * n, t) % 10));
    return 0;
}
