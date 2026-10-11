/* utest: programs' TCP on /svc/net (net.idl's tcp, tcp_listener and
 * accept; netstack's tcpsock.c), through bin/netstack over the fake
 * driver (netdrv.h), the test as the programs: who may listen (the listen
 * permission, ports below 1024, backlogs), the limits and fair shares on
 * connections and listeners (per opener, ordinary openers together, all),
 * accept's forms (at once, timing out, one at a time), an opener's end
 * resetting its connections, and the counts going back to nothing
 * (nettcp_limits); a listener with bulk tx rings whose connections outgrow
 * its opener's ring bytes: the rest get the default tx ring, not a reset
 * (nettcp_bulk_rings, the test as the peer on the fake driver too); a
 * connection's options through net_stream.set_options: the ranges, Nagle's
 * algorithm seen on the wire, and a keep-alive that ends a connection whose
 * peer says nothing (nettcp_options); a listener's address reuse
 * (tcp_listener's flags): refused flags, and a listener made again on a
 * port whose connection is in TIME_WAIT (nettcp_reuse_api). The bytes
 * themselves are tools/tcp-test.sh's (QEMU, a real peer) and the in-process
 * nettcp_* tests'. Each ends with netstack killed and its job empty. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/net.h>
#include <idl/svc.h>
#include <net.h>
#include <os.h>
#include "netdrv.h"
#include "netpkt.h"
#include "tcppeer.h"
#include "utest.h"

#define PER    NET_TCP_PER_OPENER
#define RING   SOCKRING_MIN           /* each ring: 12 KiB a connection with its header */

/* An opener of shared channel net. */
static bool connect_on(handle_t net, handle_t *out)
{
    CHECK_ST(svc_connect_until(net, now() + NETDRV_WAIT, out), OK);
    return true;
}

static bool counts(struct net_counters *c)
{
    CHECK_ST(net_get_counters(netdrv_net(), c), OK);
    return true;
}

/* n connections from opener o into s[*k...]. */
static bool conns(handle_t o, struct net_sock *s, unsigned *k, unsigned n)
{
    for (unsigned i = 0; i < n; i++, (*k)++)
        CHECK_ST(net_tcp_open(o, PEER_IP, 9000, RING, RING, &s[*k]), OK);
    return true;
}

/* Who may listen, and how much. */
static bool listeners(void)
{
    handle_t plain, lo, lo2, sys;
    struct net_listener l[NET_LISTENERS_MAX], x;
    unsigned n = 0;
    CHECK(connect_on(netdrv_net(), &plain));
    CHECK(connect_on(netdrv_net_listen(), &lo));
    CHECK(connect_on(netdrv_net_listen(), &lo2));
    CHECK(connect_on(netdrv_net_sys(), &sys));
    CHECK_ST(net_tcp_listen(plain, 5000, 4, 0, 0, &x), ERR_ACCESS_DENIED);   /* no permission */
    CHECK_ST(net_tcp_listen(plain, 0, 4, 0, 0, &x), ERR_ACCESS_DENIED);
    CHECK_ST(net_tcp_listen(lo, 80, 4, 0, 0, &x), ERR_ACCESS_DENIED);       /* below 1024 */
    CHECK_ST(net_tcp_listen(lo, 5000, 0, 0, 0, &x), ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_listen(lo, 5000, NET_BACKLOG_MAX + 1, 0, 0, &x), ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_listen(lo, 5000, 4, 1000, 0, &x), ERR_INVALID_ARGS);
    /* An opener's four, then the ordinary openers' backlog (96 of 128). */
    for (unsigned i = 0; i < NET_LISTENERS_PER_OPENER; i++, n++)
        CHECK_ST(net_tcp_listen(lo, (uint16_t)(5000 + i), NET_BACKLOG_MAX, 0, 0, &l[n]), OK);
    CHECK_ST(net_tcp_listen(lo, 5100, 1, 0, 0, &x), ERR_NO_RESOURCES);
    CHECK_ST(net_tcp_listen(lo2, 5000, 1, 0, 0, &x), ERR_ALREADY_BOUND);
    for (unsigned i = 0; i < 2; i++, n++)
        CHECK_ST(net_tcp_listen(lo2, (uint16_t)(5010 + i), NET_BACKLOG_MAX, 0, 0, &l[n]), OK);
    CHECK_ST(net_tcp_listen(lo2, 5020, 1, 0, 0, &x), ERR_NO_RESOURCES);    /* the share */
    /* /svc/net-sys carries no listen permission (the network's services
     * don't listen). A closed listener's backlog is free again; port 0:
     * netstack's pick. */
    CHECK_ST(net_tcp_listen(sys, 0, 1, 0, 0, &x), ERR_ACCESS_DENIED);
    net_listener_close(&l[--n]);
    status_t st;
    uint64_t until = now() + NETDRV_WAIT;   /* netstack sees the close in its own time */
    while ((st = net_tcp_listen(lo2, 0, NET_BACKLOG_MAX, 0, 0, &l[n])) == ERR_NO_RESOURCES &&
           now() < until)
        jam_sleep_until(now() + 10 * NS_PER_MS);
    CHECK_ST(st, OK);
    CHECK(l[n++].port >= NET_PORT_EPHEMERAL);
    /* accept: at once, timing out, one at a time. */
    handle_t hs[4];
    uint32_t a, b, c;
    uint16_t p;
    CHECK_ST(net_listener_accept_until(l[0].ch, now() + NETDRV_WAIT, 0, &hs[0], &hs[1], &hs[2],
                                       &hs[3], &a, &p, &b, &c), ERR_SHOULD_WAIT);
    CHECK_ST(net_listener_accept_until(l[0].ch, now() + NETDRV_WAIT, 50, &hs[0], &hs[1], &hs[2],
                                       &hs[3], &a, &p, &b, &c), ERR_TIMED_OUT);
    CHECK_ST(net_listener_accept_until(l[0].ch, now() + NETDRV_WAIT, NET_TIMEOUT_MAX + 1, &hs[0],
                                       &hs[1], &hs[2], &hs[3], &a, &p, &b, &c),
             ERR_INVALID_ARGS);   /* the timeout rule: 0, 1..NET_TIMEOUT_MAX, NET_WAIT_FOREVER */
    CHECK_ST(net_listener_accept_until(l[0].ch, now() + NETDRV_WAIT, NET_WAIT_FOREVER - 1, &hs[0],
                                       &hs[1], &hs[2], &hs[3], &a, &p, &b, &c),
             ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_accept_send(&l[1]), OK);
    CHECK_ST(net_listener_accept_until(l[1].ch, now() + NETDRV_WAIT, 0, &hs[0], &hs[1], &hs[2],
                                       &hs[3], &a, &p, &b, &c), ERR_BAD_STATE);
    struct net_sock s;
    CHECK_ST(net_tcp_accept(&l[1], now() + 50 * NS_PER_MS, &s, NULL, NULL), ERR_TIMED_OUT);
    struct net_counters cn;
    CHECK(counts(&cn));
    CHECK_EQ(cn.tcp_listeners, n);
    for (unsigned i = 0; i < n; i++)
        net_listener_close(&l[i]);
    handle_t os[] = { plain, lo, lo2, sys };
    for (unsigned i = 0; i < 4; i++)
        jam_handle_close(os[i]);
    return true;
}

/* Connections: an opener's limit, the ordinary openers' share, the reserve,
 * an opener's end; and what can't be connected to. */
static bool connections(void)
{
    static struct net_sock s[NET_TCP_MAX];
    handle_t o[3], sys;
    struct net_sock x;
    struct net_counters c;
    unsigned k = 0;
    for (unsigned i = 0; i < 3; i++)
        CHECK(connect_on(netdrv_net(), &o[i]));
    CHECK(connect_on(netdrv_net_sys(), &sys));
    /* Port 0, the subnet's broadcast, loopback, a ring size not allowed. */
    CHECK_ST(net_tcp_open(o[0], PEER_IP, 0, 0, 0, &x), ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_open(o[0], 0x0a0215ffu, 80, 0, 0, &x), ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_open(o[0], 0x7f000001u, 80, 0, 0, &x), ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_open(o[0], PEER_IP, 80, 3000, 0, &x), ERR_INVALID_ARGS);
    /* An opener's half of all of them; the ordinary openers' share; then
     * the reserve, a system opener's, to the total. */
    CHECK(conns(o[0], s, &k, PER));
    CHECK_ST(net_tcp_open(o[0], PEER_IP, 9000, RING, RING, &x), ERR_NO_RESOURCES);   /* its 512 */
    CHECK(conns(o[1], s, &k, NET_PROG_TCP - PER));
    CHECK_ST(net_tcp_open(o[2], PEER_IP, 9000, RING, RING, &x), ERR_NO_RESOURCES);   /* 768 */
    CHECK(conns(sys, s, &k, NET_TCP_MAX - NET_PROG_TCP));   /* the reserve: 1,024 in all */
    CHECK_ST(net_tcp_open(sys, PEER_IP, 9000, RING, RING, &x), ERR_NO_RESOURCES);   /* all */
    CHECK(counts(&c));
    CHECK_EQ(c.tcp_conns, NET_TCP_MAX);
    uint32_t state;
    status_t err;
    net_tcp_status(&s[0], &state, &err);
    CHECK_EQ(state, SOCKRING_STATE_CONNECTING);
    /* An opener's end resets its connections; they stay until closed. */
    jam_handle_close(o[1]);
    for (unsigned i = PER; i < NET_PROG_TCP; i++) {
        uint64_t until = now() + NETDRV_WAIT;
        do {
            net_tcp_status(&s[i], &state, &err);
        } while (state != SOCKRING_STATE_CLOSED && now() < until);
        CHECK_EQ(state, SOCKRING_STATE_CLOSED);
        CHECK_ST(err, ERR_PEER_CLOSED);
    }
    for (unsigned i = 0; i < k; i++)
        net_close(&s[i]);
    handle_t os[] = { o[0], o[2], sys };
    for (unsigned i = 0; i < 3; i++)
        jam_handle_close(os[i]);
    return true;
}

bool t_nettcp_limits(void)
{
    struct net_counters c;
    CHECK(netdrv_start());
    CHECK(listeners());
    CHECK(connections());
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counts(&c));
    } while ((c.tcp_conns || c.tcp_listeners || c.ring_bytes || c.openers) && now() < end);
    CHECK(!c.tcp_conns && !c.tcp_listeners && !c.ring_bytes && !c.openers);
    /* The listeners' share and the connections': a third refusal comes
     * only if the listener after a close is asked for before netstack saw
     * the close (the retry loop above), which is a race. */
    CHECK(c.refused_shares >= 2);
    CHECK(netdrv_stop());
    return true;
}

/* ---- bulk rings past an opener's bytes ---------------------------------------------- */

/* The peer's SYN-ACK from netstack's listener for p: its sequence number
 * into p->rcv. Other frames (none expected) are passed over. */
static bool syn_ack(struct tp *p)
{
    uint8_t f[PKT_FRAME_MAX];
    for (unsigned k = 0; k < 8; k++) {   /* a few frames at most */
        uint32_t n;
        const uint8_t *t;
        size_t len;
        CHECK_ST(netdrv_recv(f, &n, NETDRV_WAIT), OK);
        if (!pkt_is_ipv4(f, n, PEER_IP, 6, &t, &len) || len < 20 || pkt_get16(t + 2) != p->port)
            continue;
        CHECK_EQ(t[13] & (TP_SYN | TP_ACK), TP_SYN | TP_ACK);
        p->rcv = pkt_get32(t + 4) + 1;
        return true;
    }
    FAIL("no SYN-ACK for port %u", p->port);
}

/* The peer connects from port `from` to the listener; the program accepts. */
static bool peer_connects(struct net_listener *l, uint16_t from, struct net_sock *out)
{
    uint8_t f[PKT_FRAME_MAX];
    struct tp p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = from, .our = l->port,
                    .snd = 0x51000000u, .win = 0xffff };
    CHECK(netdrv_send(f, tp_frame(f, &p, TP_SYN, NULL, 0)));
    p.snd++;
    CHECK(syn_ack(&p));
    CHECK(netdrv_send(f, tp_frame(f, &p, TP_ACK, NULL, 0)));
    CHECK_ST(net_tcp_accept(l, now() + NETDRV_WAIT, out, NULL, NULL), OK);
    return true;
}

bool t_nettcp_bulk_rings(void)
{
    enum { N = 5 };
    handle_t lo;
    struct net_listener l;
    static struct net_sock s[N];
    unsigned fit = SOCKRING_OPENER_BYTES / (unsigned)sockring_bytes(NET_TCP_BULK, RING);
    CHECK(fit < N);
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));   /* netstack knows the peer's MAC */
    CHECK(connect_on(netdrv_net_listen(), &lo));
    CHECK_ST(net_tcp_listen(lo, 5100, NET_BACKLOG_MAX, NET_TCP_BULK, RING, &l), OK);
    for (unsigned i = 0; i < N; i++) {
        CHECK(peer_connects(&l, (uint16_t)(40000 + i), &s[i]));
        CHECK_EQ(s[i].r.tx.size, i < fit ? NET_TCP_BULK : NET_TCP_TX);   /* past its bytes: */
        CHECK_EQ(s[i].r.rx.size, RING);                                  /* the default tx */
    }
    struct net_counters c;
    CHECK(counts(&c));
    CHECK_EQ(c.tcp_refused, 0);
    /* A protocol for each kind of channel: a connection's channel
     * (net_stream) is no opener's, socket's or listener's, and a listener's
     * is none of the others. */
    uint16_t port, peer_port;
    uint32_t addr, peer, queued, dropped, got;
    handle_t hs[4];
    struct net_info in;
    CHECK_ST(net_stream_state_until(s[0].ch, now() + NETDRV_WAIT, &addr, &port, &peer,
                                    &peer_port, &queued), OK);
    /* Both ends, for an accepted connection too (a getsockname and a getpeername). */
    CHECK(addr == OUR_IP && port == l.port && peer == PEER_IP && peer_port == 40000);
    CHECK_ST(net_stream_state_until(l.ch, now() + NETDRV_WAIT, &addr, &port, &peer, &peer_port,
                                    &queued), ERR_NOT_SUPPORTED);
    CHECK_ST(net_stream_state_until(lo, now() + NETDRV_WAIT, &addr, &port, &peer, &peer_port,
                                    &queued), ERR_NOT_SUPPORTED);
    CHECK_ST(net_socket_state(s[0].ch, &addr, &port, &peer, &peer_port, &queued, &dropped),
             ERR_NOT_SUPPORTED);
    CHECK_ST(net_listener_accept_until(s[0].ch, now() + NETDRV_WAIT, 0, &hs[0], &hs[1], &hs[2],
                                       &hs[3], &peer, &port, &got, &queued),
             ERR_NOT_SUPPORTED);   /* another protocol's method */
    /* ... and a connection's method on a listener's or an opener's channel
     * is refused too (too long for the one, another protocol for the other). */
    CHECK(net_stream_set_options(l.ch, NET_STREAM_OPT_NODELAY, 0, 0, 0) != OK);
    CHECK(net_stream_set_options(lo, NET_STREAM_OPT_NODELAY, 0, 0, 0) != OK);
    CHECK_ST(net_info(s[0].ch, &in), ERR_NOT_SUPPORTED);
    CHECK_ST(net_info(l.ch, &in), ERR_NOT_SUPPORTED);
    for (unsigned i = 0; i < N; i++)
        net_close(&s[i]);
    net_listener_close(&l);
    jam_handle_close(lo);
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counts(&c));
    } while ((c.tcp_conns || c.tcp_listeners || c.ring_bytes || c.openers) && now() < end);
    CHECK(!c.tcp_conns && !c.ring_bytes);
    CHECK(netdrv_stop());
    return true;
}

/* ---- a connection's options -------------------------------------------------------- */

/* What netstack sent the peer on p's connection until `quiet` passed after
 * the last of it: segments with data (and their bytes), empty ones that are
 * a keep-alive's (one byte before p->rcv), FINs and resets; p->rcv moves on
 * over the data and the FIN. */
struct seen {
    unsigned segs, probes, resets, fins;
    uint32_t bytes;
};

static bool drain(struct tp *p, uint64_t quiet, struct seen *w)
{
    uint8_t f[PKT_FRAME_MAX];
    uint32_t n;
    *w = (struct seen){ 0 };
    while (netdrv_recv(f, &n, quiet) == OK) {
        const uint8_t *t;
        size_t len;
        if (!pkt_is_ipv4(f, n, PEER_IP, 6, &t, &len) || len < 20 || pkt_get16(t + 2) != p->port)
            continue;
        size_t hl = (size_t)(t[12] >> 4) * 4, paylen = len - hl;
        uint32_t seq = pkt_get32(t + 4), flags = t[13];
        if (flags & TP_RST) {
            w->resets++;
            continue;
        }
        if (!paylen && !(flags & TP_FIN) && seq == p->rcv - 1)
            w->probes++;
        if (seq != p->rcv)   /* (a repeat, or out of order: not new) */
            continue;
        if (paylen) {
            w->segs++;
            w->bytes += (uint32_t)paylen;
            p->rcv += (uint32_t)paylen;
        }
        if (flags & TP_FIN) {   /* its FIN takes one sequence number */
            w->fins++;
            p->rcv++;
        }
    }
    return true;
}

static bool peer_acks(struct tp *p)
{
    uint8_t f[PKT_FRAME_MAX];
    CHECK(netdrv_send(f, tp_frame(f, p, TP_ACK, NULL, 0)));
    return true;
}

/* A peer connects from port `from` to a new listener on `port` (flags as
 * net_tcp_listen_flags'); the program accepts. */
static bool peer_accepted(struct tp *p, uint16_t port, uint16_t from, uint32_t flags,
                          struct net_sock *s, handle_t lo, struct net_listener *l)
{
    uint8_t f[PKT_FRAME_MAX];
    CHECK_ST(net_tcp_listen_flags(lo, port, 4, 0, 0, flags, l), OK);
    *p = (struct tp){ .ip = PEER_IP, .mac = pkt_peer_mac, .port = from, .our = l->port,
                      .snd = 0x52000000u, .win = 0xffff };
    CHECK(netdrv_send(f, tp_frame(f, p, TP_SYN, NULL, 0)));
    p->snd++;
    CHECK(syn_ack(p));
    CHECK(peer_acks(p));
    CHECK_ST(net_tcp_accept(l, now() + NETDRV_WAIT, s, NULL, NULL), OK);
    return true;
}

bool t_nettcp_options(void)
{
    handle_t lo;
    struct net_listener l;
    struct net_sock s;
    struct tp p;
    struct seen w;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));   /* netstack knows the peer's MAC */
    CHECK(connect_on(netdrv_net_listen(), &lo));
    CHECK(peer_accepted(&p, 5200, 40200, 0, &s, lo, &l));
    /* The ranges: a bit that is none of the options, a time or a count past
     * its largest, each refused with nothing changed. */
    CHECK_ST(net_tcp_set_options(&s, 4, 0, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_set_options(&s, NET_STREAM_OPT_KEEPALIVE, NET_STREAM_KEEP_MAX + 1, 0, 0),
             ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_set_options(&s, NET_STREAM_OPT_KEEPALIVE, 0, NET_STREAM_KEEP_MAX + 1, 0),
             ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_set_options(&s, NET_STREAM_OPT_KEEPALIVE, 0, 0, NET_STREAM_KEEP_CNT_MAX + 1),
             ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_set_options(&s, NET_STREAM_OPT_KEEPALIVE, NET_STREAM_KEEP_MAX,
                                 NET_STREAM_KEEP_MAX, NET_STREAM_KEEP_CNT_MAX), OK);
    /* A connection starts with Nagle off: three small writes, three
     * segments before any ACK. */
    CHECK_ST(net_tcp_set_options(&s, NET_STREAM_OPT_NODELAY, 0, 0, 0), OK);
    for (unsigned i = 0; i < 3; i++) {
        CHECK_ST(net_write(&s, "0123456789", 10, now() + NETDRV_WAIT, NULL), OK);
        jam_sleep_until(now() + 20 * NS_PER_MS);
    }
    CHECK(drain(&p, NETDRV_QUIET, &w));
    CHECK(w.segs == 3 && w.bytes == 30);
    CHECK(peer_acks(&p));
    /* Nagle on: the first goes, the next two wait for its ACK and go as one. */
    CHECK_ST(net_tcp_set_options(&s, 0, 0, 0, 0), OK);
    CHECK_ST(net_write(&s, "0123456789", 10, now() + NETDRV_WAIT, NULL), OK);
    jam_sleep_until(now() + 20 * NS_PER_MS);
    CHECK_ST(net_write(&s, "0123456789", 10, now() + NETDRV_WAIT, NULL), OK);
    CHECK_ST(net_write(&s, "0123456789", 10, now() + NETDRV_WAIT, NULL), OK);
    CHECK(drain(&p, NETDRV_QUIET, &w));
    CHECK(w.segs == 1 && w.bytes == 10);
    CHECK(peer_acks(&p));
    CHECK(drain(&p, NETDRV_QUIET, &w));
    CHECK(w.segs == 1 && w.bytes == 20);
    CHECK(peer_acks(&p));
    /* Keep-alive with a silent peer: idle 1 s, every 1 s, 2 probes, then a
     * reset, and the connection is CLOSED with ERR_TIMED_OUT. Nothing is
     * sent before it is asked for. */
    CHECK(drain(&p, NETDRV_QUIET, &w));
    CHECK(!w.segs && !w.probes && !w.resets);
    CHECK_ST(net_tcp_set_options(&s, NET_STREAM_OPT_NODELAY | NET_STREAM_OPT_KEEPALIVE, 1, 1, 2),
             OK);
    CHECK(drain(&p, 5 * NS_PER_S, &w));
    CHECK(w.probes == 2 && w.resets == 1 && !w.segs);
    CHECK_ST(net_tcp_wait_closed(&s, now() + NETDRV_WAIT), ERR_TIMED_OUT);
    CHECK_ST(net_tcp_set_options(&s, 0, 0, 0, 0), OK);   /* a closed connection: nothing to set */
    net_close(&s);
    net_listener_close(&l);
    jam_handle_close(lo);
    CHECK(netdrv_stop());
    return true;
}

/* ---- a listener's address reuse ---------------------------------------------------- */

/* Wait until netstack has `want` listeners (a closed one goes in its own
 * time). */
static bool listeners_are(unsigned want)
{
    struct net_counters c;
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counts(&c));
        if (c.tcp_listeners == want)
            return true;
        jam_sleep_until(now() + 10 * NS_PER_MS);
    } while (now() < end);
    FAIL("netstack has %u listeners, want %u", c.tcp_listeners, want);
}

bool t_nettcp_reuse_api(void)
{
    handle_t lo;
    struct net_listener l, x;
    struct net_sock s;
    struct tp p;
    struct seen w;
    uint8_t f[PKT_FRAME_MAX];
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));   /* netstack knows the peer's MAC */
    CHECK(connect_on(netdrv_net_listen(), &lo));
    CHECK_ST(net_tcp_listen_flags(lo, 5300, 4, 0, 0, 2, &x), ERR_INVALID_ARGS);   /* no such flag */
    CHECK_ST(net_tcp_listen_flags(lo, 5300, 4, 0, 0, ~0u, &x), ERR_INVALID_ARGS);
    CHECK(peer_accepted(&p, 5300, 40300, NET_LISTEN_REUSE, &s, lo, &l));
    CHECK_ST(net_tcp_listen_flags(lo, 5300, 4, 0, 0, NET_LISTEN_REUSE, &x), ERR_ALREADY_BOUND);
    CHECK_ST(net_tcp_listen(lo, 5300, 4, 0, 0, &x), ERR_ALREADY_BOUND);   /* an active listener */
    /* The program closes first: its FIN, the peer's ACK and FIN, ours: the
     * connection is CLOSED and lwIP holds its TIME_WAIT. */
    CHECK_ST(net_shutdown(&s), OK);
    CHECK(drain(&p, NETDRV_QUIET, &w));
    CHECK_EQ(w.fins, 1);
    CHECK(peer_acks(&p));
    CHECK(netdrv_send(f, tp_frame(f, &p, TP_FIN | TP_ACK, NULL, 0)));
    p.snd++;
    CHECK_ST(net_tcp_wait_closed(&s, now() + NETDRV_WAIT), OK);
    CHECK(drain(&p, NETDRV_QUIET, &w));   /* its ACK of the peer's FIN */
    net_close(&s);
    net_listener_close(&l);
    CHECK(listeners_are(0));
    /* Without the flag the port is held; with it a listener is made at once. */
    CHECK_ST(net_tcp_listen(lo, 5300, 4, 0, 0, &x), ERR_ALREADY_BOUND);
    CHECK_ST(net_tcp_listen_flags(lo, 5300, 4, 0, 0, NET_LISTEN_REUSE, &l), OK);
    CHECK_EQ(l.port, 5300);
    net_listener_close(&l);
    CHECK(listeners_are(0));
    jam_handle_close(lo);
    CHECK(netdrv_stop());
    return true;
}
