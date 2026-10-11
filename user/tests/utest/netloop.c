/* utest: loopback (127.0.0.0/8) through bin/netstack over the fake driver
 * (netdrv.h), the test as the programs and as the network card:
 *
 *   netloop_udp     datagrams both ways between two sockets over 127.0.0.1,
 *                   127.0.0.5 being the same interface (answered from
 *                   127.0.0.1, a connected socket's peer filter too), with
 *                   and without an address on the card; a loopback-only
 *                   socket (net_socket.loopback_only) that a datagram to
 *                   the card's address doesn't reach and that can't send
 *                   out; loopback's broadcast refused
 *   netloop_ping    a ping to 127.0.0.1 answered, to its broadcast refused
 *   netloop_tcp     a connection over 127.0.0.1 (connect, accept, 300 KiB
 *                   each way through the rings and windows, an orderly
 *                   close), one refused (no listener there), one made with
 *                   no address on the card, and a loopback-only listener
 *                   that a SYN to the card's address finds closed (a reset)
 *   netloop_martian a frame from the network with a loopback address in it,
 *                   as source or as destination, is dropped and counted
 *
 * In every case the card sees no frame for loopback traffic (the quiet
 * check and tx_frames): the networking rule, that nothing outside the mode
 * ever leaves. Each test ends with netstack killed and its job empty. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/net.h>
#include <idl/netctl.h>
#include <idl/svc.h>
#include <jam/netdev.h>
#include <net.h>
#include <os.h>
#include "netdrv.h"
#include "netpkt.h"
#include "tcppeer.h"
#include "utest.h"

#define LO      0x7f000001u   /* 127.0.0.1 */
#define LO5     0x7f000005u   /* 127.0.0.5: the same interface */
#define LO_BCAST 0x7fffffffu

static uint8_t f[NETDEV_FRAME_MAX];
static struct net_dgram dg;

/* An opener that may listen (the tests take fixed ports). */
static bool opener(handle_t *out)
{
    CHECK_ST(svc_connect_until(netdrv_net_listen(), now() + NETDRV_WAIT, out), OK);
    return true;
}

static bool counters(struct net_counters *c)
{
    CHECK_ST(net_get_counters(netdrv_net(), c), OK);
    return true;
}

/* The card has been sent nothing since c0 was taken. */
static bool wire_quiet(const struct net_counters *c0)
{
    struct net_counters c;
    uint32_t n;
    CHECK(counters(&c));
    CHECK_EQ(c.tx_frames, c0->tx_frames);
    CHECK_ST(netdrv_recv(f, &n, NETDRV_QUIET), ERR_TIMED_OUT);
    return true;
}

/* The card's address back after netctl_clear: set, and the announcement it
 * makes (a gratuitous ARP) taken, so the wire is quiet again. */
static bool readdress(void)
{
    uint32_t n;
    CHECK_ST(netctl_set_ipv4(netdrv_ctl(), OUR_IP, MASK24, GW_IP), OK);
    CHECK_ST(netdrv_recv(f, &n, NETDRV_WAIT), OK);
    CHECK_EQ(pkt_get16(f + 12), ETH_ARP);
    return true;
}

/* One datagram from `from` to to:port arrives at `at`, from fromaddr. */
static bool dgram(struct net_sock *from, uint32_t to, uint16_t port, struct net_sock *at,
                  uint32_t fromaddr, const char *text)
{
    size_t n = strlen(text);
    CHECK_ST(net_sendto(from, to, port, text, n), OK);
    CHECK_ST(net_recvfrom(at, &dg, now() + NETDRV_WAIT), OK);
    CHECK_EQ(dg.addr, fromaddr);
    CHECK_EQ(dg.port, from->port);
    CHECK(dg.len == n && !memcmp(dg.data, text, n));
    return true;
}

/* ---- UDP --------------------------------------------------------------------------- */

bool t_netloop_udp(void)
{
    handle_t o;
    struct net_sock a, b, l;
    struct net_counters c0;
    CHECK(netdrv_start());
    CHECK(opener(&o));
    CHECK_ST(net_udp_open(o, 5000, &a), OK);
    CHECK_ST(net_udp_open(o, 0, &b), OK);
    CHECK(counters(&c0));
    /* Both ways; an address of the block is the one interface. */
    CHECK(dgram(&b, LO, 5000, &a, LO, "ping"));
    CHECK(dgram(&a, LO, b.port, &b, LO, "pong"));
    CHECK(dgram(&b, LO5, 5000, &a, LO, "five"));
    /* A connected socket takes the answers of the address it chose. */
    CHECK_ST(net_connect(&b, 0x7f000009u, 5000), OK);
    CHECK_ST(net_send(&b, "c", 1), OK);
    CHECK_ST(net_recvfrom(&a, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.addr == LO && dg.port == b.port && dg.len == 1);
    CHECK_ST(net_sendto(&a, LO, b.port, "r", 1), OK);
    CHECK_ST(net_recvfrom(&b, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.addr == LO && dg.port == 5000 && dg.len == 1);
    CHECK_ST(net_connect(&b, 0, 0), OK);
    /* Loopback's own broadcast is refused; nothing listening is not an error. */
    CHECK_ST(net_sendto(&b, LO_BCAST, 5000, "x", 1), ERR_INVALID_ARGS);
    CHECK_ST(net_sendto(&b, LO, 9, "x", 1), OK);
    /* Needs neither an address nor the network. */
    CHECK(wire_quiet(&c0));
    CHECK_ST(netctl_clear(netdrv_ctl()), OK);
    CHECK(dgram(&b, LO, 5000, &a, LO, "noaddr"));
    CHECK(readdress());
    CHECK(counters(&c0));
    /* A loopback-only socket: reachable from the host, and it sends to the
     * host only. */
    CHECK_ST(net_udp_open(o, 5001, &l), OK);
    CHECK_ST(net_socket_loopback_only(l.ch), OK);
    CHECK(dgram(&b, LO, 5001, &l, LO, "inside"));
    CHECK_ST(net_sendto(&l, PEER_IP, 9, "x", 1), ERR_INVALID_ARGS);
    CHECK_ST(net_sendto(&l, LO, b.port, "ok", 2), OK);
    CHECK_ST(net_recvfrom(&b, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.addr == LO && dg.port == 5001 && dg.len == 2);
    /* The card was sent nothing for all of it. */
    CHECK(wire_quiet(&c0));
    /* From the network (these make netstack answer the peer: no more wire
     * checks): the loopback-only socket is not reached, an ordinary one is. */
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 40000, OUR_IP, 5001, "net", 3)));
    CHECK_ST(net_recvfrom(&l, &dg, now() + 300 * NS_PER_MS), ERR_TIMED_OUT);
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 40000, OUR_IP, 5000, "net", 3)));
    CHECK_ST(net_recvfrom(&a, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.addr == PEER_IP && dg.port == 40000 && dg.len == 3);
    net_close(&a);
    net_close(&b);
    net_close(&l);
    jam_handle_close(o);
    CHECK(netdrv_stop());
    return true;
}

/* ---- ping -------------------------------------------------------------------------- */

bool t_netloop_ping(void)
{
    handle_t o;
    struct net_counters c0;
    uint32_t rtt = 0;
    uint8_t ttl = 0;
    CHECK(netdrv_start());
    CHECK(opener(&o));
    CHECK(counters(&c0));
    CHECK_ST(net_ping(o, LO, 1, 32, now() + NETDRV_WAIT, &rtt, &ttl), OK);
    CHECK(ttl > 0);
    CHECK_ST(net_ping(o, LO5, 2, 0, now() + NETDRV_WAIT, &rtt, &ttl), OK);
    CHECK_ST(net_ping(o, LO_BCAST, 3, 8, now() + NETDRV_WAIT, &rtt, &ttl), ERR_INVALID_ARGS);
    CHECK(wire_quiet(&c0));
    jam_handle_close(o);
    CHECK(netdrv_stop());
    return true;
}

/* ---- TCP --------------------------------------------------------------------------- */

/* The pattern's byte i of a stream started with `seed`. */
static uint8_t pat(uint32_t i, uint8_t seed)
{
    return (uint8_t)(i * 7 + seed);
}

/* total bytes from tx to rx through the rings, one thread moving both
 * ends without waiting (net_write_some, net_read_some), every byte checked. */
static bool pump(struct net_sock *tx, struct net_sock *rx, uint32_t total, uint8_t seed)
{
    static uint8_t out[8192], in[8192];
    uint32_t sent = 0, got = 0;
    uint64_t end = now() + 4 * NETDRV_WAIT;
    while (got < total && now() < end) {
        size_t moved = 0;
        if (sent < total) {
            uint32_t n = total - sent < sizeof(out) ? total - sent : (uint32_t)sizeof(out);
            for (uint32_t i = 0; i < n; i++)
                out[i] = pat(sent + i, seed);
            size_t k = net_write_some(tx, out, n);
            sent += (uint32_t)k;
            moved += k;
        }
        size_t r = net_read_some(rx, in, sizeof(in));
        for (size_t i = 0; i < r; i++)
            CHECK_EQ(in[i], pat(got + (uint32_t)i, seed));
        got += (uint32_t)r;
        moved += r;
        if (!moved)
            jam_sleep_until(now() + NS_PER_MS);
    }
    CHECK_EQ(got, total);
    return true;
}

/* A SYN from the peer to OUR_IP:port is answered with a reset (SYN-ACK:
 * a listener is there). */
static bool syn_gets(uint16_t port, uint8_t want_flags)
{
    struct tp p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = 41000, .our = port,
                    .snd = 0x61000000u, .win = 0xffff };
    CHECK(netdrv_send(f, tp_frame(f, &p, TP_SYN, NULL, 0)));
    for (unsigned k = 0; k < 8; k++) {   /* a few frames at most */
        uint32_t n;
        const uint8_t *t;
        size_t len;
        CHECK_ST(netdrv_recv(f, &n, NETDRV_WAIT), OK);
        if (!pkt_is_ipv4(f, n, PEER_IP, 6, &t, &len) || len < 20 || pkt_get16(t + 2) != p.port)
            continue;
        CHECK_EQ(t[13] & (TP_SYN | TP_ACK | TP_RST), want_flags);
        return true;
    }
    FAIL("no answer to the SYN for port %u", port);
}

bool t_netloop_tcp(void)
{
    handle_t o;
    struct net_listener l, lo_only;
    struct net_sock c, a, x;
    struct net_counters c0;
    uint32_t peer = 0;
    uint16_t peer_port = 0;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));   /* netstack knows the peer's MAC: its resets go out */
    CHECK(opener(&o));
    CHECK_ST(net_tcp_listen(o, 0, 4, 0, 0, &l), OK);
    CHECK(counters(&c0));
    /* Connect and accept over 127.0.0.1; both ends see the interface. */
    CHECK_ST(net_tcp_connect(o, LO, l.port, now() + NETDRV_WAIT, &c), OK);
    CHECK_ST(net_tcp_accept(&l, now() + NETDRV_WAIT, &a, &peer, &peer_port), OK);
    CHECK(peer == LO && peer_port == c.port);
    struct net_tcp_ends e;
    CHECK_ST(net_tcp_ends(&c, &e), OK);
    CHECK(e.local == LO && e.peer == LO && e.peer_port == l.port);
    /* Bytes both ways, more than a window (a ring's room) at a time. */
    CHECK(pump(&c, &a, 300 * 1024, 1));
    CHECK(pump(&a, &c, 300 * 1024, 2));
    /* An orderly close: the client's FIN is the server's end of stream, the
     * server's the client's, and both connections close cleanly. */
    CHECK_ST(net_shutdown(&c), OK);
    size_t n = 1;
    uint8_t b[16];
    CHECK_ST(net_read(&a, b, sizeof(b), now() + NETDRV_WAIT, &n), OK);
    CHECK_EQ(n, 0);
    CHECK_ST(net_shutdown(&a), OK);
    CHECK_ST(net_read(&c, b, sizeof(b), now() + NETDRV_WAIT, &n), OK);
    CHECK_EQ(n, 0);
    CHECK_ST(net_tcp_wait_closed(&c, now() + NETDRV_WAIT), OK);
    CHECK_ST(net_tcp_wait_closed(&a, now() + NETDRV_WAIT), OK);
    net_close(&c);
    net_close(&a);
    /* Nothing listens: refused (a reset, from netstack itself). */
    CHECK_ST(net_tcp_connect(o, LO, 1, now() + NETDRV_WAIT, &x), ERR_NOT_FOUND);
    /* No address on the card: loopback goes on (the listener, made on every
     * address, is still there). */
    CHECK(wire_quiet(&c0));
    CHECK_ST(netctl_clear(netdrv_ctl()), OK);
    CHECK_ST(net_tcp_connect(o, LO5, l.port, now() + NETDRV_WAIT, &c), OK);
    CHECK_ST(net_tcp_accept(&l, now() + NETDRV_WAIT, &a, &peer, &peer_port), OK);
    CHECK(peer == LO && peer_port == c.port);
    CHECK(pump(&c, &a, 4096, 3));
    net_close(&c);
    net_close(&a);
    CHECK(readdress());
    CHECK(netdrv_ping(2, true));   /* the address change forgot the peer's MAC */
    CHECK(counters(&c0));
    /* A loopback-only listener: the host reaches it, the network finds the
     * port closed. The listener on every address is the control. */
    CHECK_ST(net_tcp_listen_flags(o, 0, 4, 0, 0, NET_LISTEN_LOOPBACK, &lo_only), OK);
    CHECK_ST(net_tcp_connect(o, LO, lo_only.port, now() + NETDRV_WAIT, &c), OK);
    CHECK_ST(net_tcp_accept(&lo_only, now() + NETDRV_WAIT, &a, &peer, &peer_port), OK);
    net_close(&c);
    net_close(&a);
    CHECK(wire_quiet(&c0));
    CHECK(syn_gets(l.port, TP_SYN | TP_ACK));
    CHECK(syn_gets(lo_only.port, TP_RST | TP_ACK));
    net_listener_close(&lo_only);
    net_listener_close(&l);
    jam_handle_close(o);
    struct net_counters c1;
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counters(&c1));
    } while ((c1.tcp_conns || c1.tcp_listeners || c1.ring_bytes) && now() < end);
    CHECK(!c1.tcp_conns && !c1.tcp_listeners && !c1.ring_bytes);
    CHECK(netdrv_stop());
    return true;
}

/* ---- from the network ------------------------------------------------------------- */

/* A frame from the network with a loopback address is dropped before lwIP
 * and counted; an ordinary one after it is taken. */
bool t_netloop_martian(void)
{
    handle_t o;
    struct net_sock s;
    struct net_counters c0, c1;
    CHECK(netdrv_start());
    CHECK(opener(&o));
    CHECK_ST(net_udp_open(o, 5000, &s), OK);
    CHECK(counters(&c0));
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, LO, 40000, OUR_IP, 5000, "src", 3)));
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 40000, LO, 5000, "dst", 3)));
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, LO5, 40000, LO5, 5000, "both", 4)));
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 40000, OUR_IP, 5000, "ok", 2)));
    CHECK_ST(net_recvfrom(&s, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.addr == PEER_IP && dg.len == 2 && !memcmp(dg.data, "ok", 2));
    CHECK_ST(net_recvfrom(&s, &dg, 0), ERR_SHOULD_WAIT);   /* none of the others */
    CHECK(counters(&c1));
    CHECK_EQ(c1.rx_refused, c0.rx_refused + 3);
    net_close(&s);
    jam_handle_close(o);
    CHECK(netdrv_stop());
    return true;
}
