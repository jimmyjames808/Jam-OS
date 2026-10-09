/* utest: a TCP connection's options (net_stream.set_options: tcp.c's
 * ntcp_conn_options over stack.c's lwIP), in-process on nettcp.h's fixture
 * (the test is the peer, byte by byte on the wire, and the program, on the
 * rings):
 *
 *   nettcp_nodelay    a connection starts with Nagle's algorithm off: three
 *                     10-byte writes leave as three segments before the peer
 *                     acks any; with it on, the first leaves at once and the
 *                     next two wait for its ACK and leave as one segment of
 *                     20 bytes; turning it off sends what it held at once
 *   nettcp_keepalive  keep-alive off sends nothing to an idle connection;
 *                     with it on (idle 1 s, interval 1 s) a probe goes (an
 *                     empty segment one byte before our next sequence
 *                     number), an answered probe keeps the connection and
 *                     starts the idle time again, switching it off stops the
 *                     probes, and a peer that never answers gets `cnt`
 *                     probes and then a reset: CLOSED, ERR_TIMED_OUT
 *   nettcp_reuse      a listener with address reuse (net.idl's LISTEN_REUSE):
 *                     two active listeners never share a port; with the flag
 *                     one is made on a port whose connections are in TIME_WAIT
 *                     (without it, ERR_ALREADY_BOUND), and beside open
 *                     connections of an earlier listener that had the flag too,
 *                     never beside those of one that hadn't
 *
 * The options' protocol side (the ranges, the reply) is nettcpsock.c's. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <sockring.h>
#include "netpkt.h"
#include "nettcp.h"
#include "stack.h"
#include "tcp.h"
#include "tcppeer.h"
#include "utest.h"

/* The program writes n bytes of stream `id` from `at`, publishing them. */
static bool write_bytes(struct cx *x, uint32_t id, uint64_t at, uint32_t n)
{
    uint8_t buf[64];
    CHECK(n <= sizeof(buf));
    tp_fill(id, at, buf, n);
    CHECK_EQ(sockring_stream_write(&x->prog.tx, buf, n), n);
    fix_publish(x, &x->prog.tx);
    return true;
}

/* One turn: exactly `segs` segments for the peer, `bytes` of stream `id`
 * in all (from `at`), taken in order. */
static bool takes(struct cx *x, uint32_t id, uint64_t at, unsigned segs, uint32_t bytes)
{
    struct tp_seg s[8];
    uint8_t got[64];
    size_t have = 0;
    unsigned n;
    CHECK(fix_turn(x, s, 8, &n, false));
    CHECK_EQ(n, segs);
    tp_absorb(&x->p, s, n, got, sizeof(got), &have);
    CHECK_EQ(have, bytes);
    CHECK(tp_same(id, at, got, have));
    return true;
}

bool t_nettcp_nodelay(void)
{
    struct cx x;
    CHECK(fix_up());
    CHECK(fix_connect(&x, 8200, 16384, 16384));
    /* The default: each write leaves at once, the peer acking none. */
    for (unsigned i = 0; i < 3; i++) {
        CHECK(write_bytes(&x, 1, i * 10, 10));
        CHECK(takes(&x, 1, i * 10, 1, 10));
    }
    tp_send(&x.p, TP_ACK, NULL, 0);
    /* Nagle on: the first leaves (nothing is unacked), the next two wait. */
    ntcp_conn_options(x.c, false, false, 7200, 75, 9);
    CHECK(write_bytes(&x, 1, 30, 10));
    CHECK(takes(&x, 1, 30, 1, 10));
    CHECK(write_bytes(&x, 1, 40, 10));
    CHECK(write_bytes(&x, 1, 50, 10));
    CHECK(takes(&x, 1, 0, 0, 0));
    fix_tick();   /* no timer sends them: only the ACK does */
    CHECK(takes(&x, 1, 0, 0, 0));
    tp_send(&x.p, TP_ACK, NULL, 0);
    CHECK(takes(&x, 1, 40, 1, 20));   /* both, in one segment */
    /* A write held back goes the moment Nagle is switched off. */
    CHECK(write_bytes(&x, 1, 60, 10));
    CHECK(takes(&x, 1, 0, 0, 0));
    ntcp_conn_options(x.c, true, false, 7200, 75, 9);
    CHECK(takes(&x, 1, 60, 1, 10));
    tp_send(&x.p, TP_ACK, NULL, 0);
    CHECK(takes(&x, 1, 0, 0, 0));
    fix_free(&x);
    CHECK(fix_down());
    return true;
}

/* Let lwIP's timers run until a segment for x comes, or `ms` have passed
 * (*n stays 0 then): the segments in s. A reset is allowed. */
static bool wait_segs(struct cx *x, uint64_t ms, struct tp_seg *s, unsigned max, unsigned *n)
{
    uint64_t end = now() + ms * NS_PER_MS;
    *n = 0;
    while (now() < end && !*n) {
        fix_tick();
        CHECK(fix_turn(x, s, max, n, true));
    }
    return true;
}

/* A keep-alive probe: an empty segment with ACK, one byte before the next
 * sequence number we expect from the peer's side of the wire. */
static bool is_probe(const struct cx *x, const struct tp_seg *s)
{
    CHECK_EQ(s->flags, TP_ACK);
    CHECK_EQ(s->len, 0);
    CHECK_EQ(s->seq, x->p.rcv - 1);
    return true;
}

bool t_nettcp_keepalive(void)
{
    struct cx x;
    struct tp_seg s[8];
    unsigned n;
    CHECK(fix_up());
    CHECK(fix_connect(&x, 8201, 16384, 16384));
    /* Off by default, and set to idle 1 s, interval 1 s, 3 probes: the
     * first probe comes after the idle second (lwIP's timer ticks every
     * 500 ms), not before. */
    ntcp_conn_options(x.c, true, true, 1, 1, 3);
    CHECK(fix_turn(&x, s, 8, &n, false));
    CHECK_EQ(n, 0);
    uint64_t t0 = now();
    CHECK(wait_segs(&x, 3000, s, 8, &n));
    CHECK_EQ(n, 1);
    CHECK(is_probe(&x, &s[0]));
    CHECK(now() - t0 >= 900 * NS_PER_MS);
    /* Answered: still open, and the idle time starts again, so the next
     * probe is another idle second away and the count starts again. */
    tp_send(&x.p, TP_ACK, NULL, 0);
    CHECK(fix_turn(&x, s, 8, &n, false));
    CHECK_EQ(n, 0);
    CHECK_EQ(fix_status(&x).state, SOCKRING_STATE_OPEN);
    t0 = now();
    CHECK(wait_segs(&x, 3000, s, 8, &n));
    CHECK_EQ(n, 1);
    CHECK(is_probe(&x, &s[0]));
    CHECK(now() - t0 >= 900 * NS_PER_MS);
    tp_send(&x.p, TP_ACK, NULL, 0);
    /* Off again: an idle connection is left alone (3 idle seconds, two
     * probe times at least, and not a segment). */
    ntcp_conn_options(x.c, true, false, 1, 1, 3);
    CHECK(wait_segs(&x, 2500, s, 8, &n));
    CHECK_EQ(n, 0);
    CHECK_EQ(fix_status(&x).state, SOCKRING_STATE_OPEN);
    /* On with 2 probes and a peer that says nothing: both go, then the
     * reset, and the connection is CLOSED, ERR_TIMED_OUT. */
    tp_send(&x.p, TP_ACK, NULL, 0);   /* (the idle time counts from the last segment heard) */
    ntcp_conn_options(x.c, true, true, 1, 1, 2);
    unsigned probes = 0, resets = 0;
    uint64_t end = now() + 8 * NS_PER_S;
    while (fix_status(&x).state == SOCKRING_STATE_OPEN && now() < end) {
        CHECK(wait_segs(&x, 4000, s, 8, &n));
        for (unsigned i = 0; i < n; i++) {
            if (s[i].flags & TP_RST) {
                resets++;
            } else {
                CHECK(is_probe(&x, &s[i]));
                probes++;
            }
        }
    }
    CHECK_EQ(probes, 2);
    CHECK_EQ(resets, 1);
    struct sockring_status st = fix_status(&x);
    CHECK_EQ(st.state, SOCKRING_STATE_CLOSED);
    CHECK_EQ(st.error, ERR_TIMED_OUT);
    fix_free(&x);
    CHECK(fix_down());
    return true;
}

/* ---- a listener's address reuse -------------------------------------------------------- */

/* A peer connects from port `from` to listener l and the program accepts;
 * then the program closes first (its END, the peer's ACK and FIN, ours):
 * the connection is CLOSED, freed, and lwIP keeps its TIME_WAIT. */
static bool time_wait_connection(struct ntcp_listener *l, uint16_t from)
{
    struct cx x;
    struct tp_seg s[8];
    unsigned n;
    CHECK(fix_handshake(&x, l->port, from, 8192));
    tp_send(&x.p, TP_ACK, NULL, 0);
    CHECK(fix_accept(l, &x));
    if (sockring_finish(&x.prog.tx))
        ntcp_kick(x.c);
    CHECK(fix_turn(&x, s, 8, &n, false));
    CHECK(n >= 1 && (s[n - 1].flags & TP_FIN));
    tp_absorb(&x.p, s, n, NULL, 0, &(size_t){ 0 });
    tp_send(&x.p, TP_ACK, NULL, 0);
    tp_send(&x.p, TP_FIN | TP_ACK, NULL, 0);
    CHECK(fix_turn(&x, s, 8, &n, false));
    CHECK_EQ(fix_status(&x).state, SOCKRING_STATE_CLOSED);
    CHECK_EQ(fix_status(&x).error, OK);
    fix_free(&x);
    return true;
}

/* A peer connects from port `from` to listener l and the program accepts;
 * the connection stays open. */
static bool open_connection(struct ntcp_listener *l, uint16_t from, struct cx *x)
{
    CHECK(fix_handshake(x, l->port, from, 8192));
    tp_send(&x->p, TP_ACK, NULL, 0);
    CHECK(fix_accept(l, x));
    CHECK_EQ(fix_status(x).state, SOCKRING_STATE_OPEN);
    return true;
}

bool t_nettcp_reuse(void)
{
    struct ntcp_listener *a, *b, *c;
    struct cx y, z;
    struct tp_seg s[8];
    unsigned n;
    CHECK(fix_up());
    struct ntcp_listen_req req = { .port = 7300, .backlog = 4, .rx_size = 8192,
                                   .rings = fix_lst_rings, .drop = fix_lst_drop, .reuse = true };
    /* Two active listeners never share a port, flag or not. */
    CHECK_ST(ntcp_listen(&req, &a), OK);
    CHECK_ST(ntcp_listen(&req, &b), ERR_ALREADY_BOUND);
    req.reuse = false;
    CHECK_ST(ntcp_listen(&req, &b), ERR_ALREADY_BOUND);
    /* Its connection closed by us first leaves a TIME_WAIT on the port. */
    CHECK(time_wait_connection(a, 30100));
    CHECK(fix_counts().time_wait >= 1);
    ntcp_unlisten(a);
    CHECK_ST(ntcp_listen(&req, &b), ERR_ALREADY_BOUND);   /* without the flag: the port is held */
    req.reuse = true;
    CHECK_ST(ntcp_listen(&req, &b), OK);                  /* with it: made at once */
    /* ... and it works: a peer connects, the program accepts. */
    CHECK(open_connection(b, 30101, &y));
    /* The listener closed, its open connection (it has the flag too) stays: a
     * listener with the flag can stand beside it, one without can't. */
    ntcp_unlisten(b);
    req.reuse = false;
    CHECK_ST(ntcp_listen(&req, &c), ERR_ALREADY_BOUND);
    req.reuse = true;
    CHECK_ST(ntcp_listen(&req, &c), OK);
    CHECK(open_connection(c, 30102, &z));   /* beside the first still */
    ntcp_unlisten(c);
    ntcp_conn_abort(y.c, ERR_CANCELED);
    ntcp_conn_abort(z.c, ERR_CANCELED);
    CHECK(fix_turn(&y, s, 8, &n, true));
    fix_free(&y);
    fix_free(&z);
    /* A listener made without the flag: its open connection doesn't have it,
     * so no listener may stand beside that connection, flag or not (as Linux
     * wants both to ask) until it is gone. */
    req.port = 7301;
    req.reuse = false;
    CHECK_ST(ntcp_listen(&req, &a), OK);
    CHECK(open_connection(a, 30103, &y));
    ntcp_unlisten(a);
    CHECK_ST(ntcp_listen(&req, &b), ERR_ALREADY_BOUND);
    req.reuse = true;
    CHECK_ST(ntcp_listen(&req, &b), ERR_ALREADY_BOUND);
    ntcp_conn_abort(y.c, ERR_CANCELED);
    CHECK(fix_turn(&y, s, 8, &n, true));
    fix_free(&y);
    CHECK_ST(ntcp_listen(&req, &b), OK);   /* nothing holds the port now */
    ntcp_unlisten(b);
    tp_forget();
    CHECK(fix_down());
    return true;
}
