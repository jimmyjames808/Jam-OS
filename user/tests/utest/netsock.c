/* utest: programs' sockets and pings on /svc/net (abi/idl/net.idl,
 * <net.h>), through bin/netstack over the fake driver (netdrv.h): the
 * test is the program (its own openers, through svc.connect on the
 * shared channel, as svc_open does), the network card (frames in and out
 * of the rings, checked byte by byte) and init (netctl).
 *
 * Covered: a UDP datagram both ways through a socket (ports, bytes,
 * checksums), a sock_recv answered later when the datagram comes, timeouts,
 * sock_connect's filter, the destinations a program can't send to; ping
 * (the request's bytes, the reply's round trip and TTL, another opener's
 * echo id not answered, an unreachable, a timeout); iface and wait_change;
 * the limits (sockets per opener and in all, openers, requests in flight);
 * a hostile program (garbage, the wrong channel's methods, handles,
 * oversized messages, closing mid-receive and mid-echo); a slow reader
 * whose queue fills while netstack serves everyone else (the slow-peer
 * rule); netctl's DHCP socket (from 0.0.0.0 to the broadcast address, and
 * no program can do that). Each test ends with netstack killed and its job
 * empty (netdrv_stop): nothing leaked. */
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
#include "utest.h"

#define NO_REPLY 1   /* reply_of: nothing came (not a status on the wire) */
#define ETH_UDP_DATA 42u   /* a UDP datagram's data in a frame (no IP options) */

static uint8_t f[NETDEV_FRAME_MAX], g[NETDEV_FRAME_MAX];
static struct net_dgram dg;
static uint32_t txc = 0x40000000u;   /* our async txids, far from the kernel's */

static bool opener(handle_t *out)
{
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, out), OK);
    return true;
}

/* The UDP checksum of u (n bytes) from src to dst is right. */
static bool udp_sum_ok(const uint8_t *u, size_t n, uint32_t src, uint32_t dst)
{
    uint8_t ph[12];
    pkt_put32(ph, src);
    pkt_put32(ph + 4, dst);
    pkt_put16(ph + 8, 17);
    pkt_put16(ph + 10, (uint32_t)n);
    uint32_t s = pkt_sum16(ph, 12) + pkt_sum16(u, n);
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    CHECK(pkt_get16(u + 6) != 0);   /* lwIP makes one */
    CHECK_EQ(s, 0xffff);
    return true;
}

/* The next frame is our UDP datagram sport -> dst:dport with these bytes. */
static bool sent_udp(uint32_t dst, uint32_t sport, uint32_t dport, const void *data, size_t len)
{
    uint32_t n;
    const uint8_t *u;
    size_t un;
    CHECK_ST(netdrv_recv(f, &n, NETDRV_WAIT), OK);
    CHECK(pkt_is_ipv4(f, n, dst, 17, &u, &un));
    CHECK(!memcmp(f, pkt_peer_mac, 6));
    CHECK_EQ(un, 8 + len);
    CHECK_EQ(pkt_get16(u), sport);
    CHECK_EQ(pkt_get16(u + 2), dport);
    CHECK_EQ(pkt_get16(u + 4), 8 + len);
    CHECK(!memcmp(u + 8, data, len));
    CHECK(udp_sum_ok(u, un, OUR_IP, dst));
    return true;
}

/* The reply to the async call txid on ch within `wait`: its status (and
 * results), or NO_REPLY. */
static status_t reply_of(handle_t ch, uint32_t txid, uint64_t wait, uint32_t *rtt, uint8_t *ttl,
                         uint16_t *size)
{
    _Alignas(8) uint8_t rep[NET_REP_MAX];
    struct idl_msg m;
    signals_t seen;
    if (jam_object_wait_one(ch, SIG_READABLE, now() + wait, &seen) != OK)
        return NO_REPLY;
    status_t st = idl_reply_read(ch, rep, sizeof(rep), &m);
    if (st != OK)
        return st;
    if (m.txid != txid) {
        idl_msg_drop(&m);
        return ERR_INTERNAL;
    }
    return net_echo_result(rep, &m, rtt, ttl, size);
}

/* An echo request from opener ch (async): the frame it makes into req
 * (its length *n), checked. */
static bool echo_out(handle_t ch, uint32_t *txid, uint32_t addr, uint16_t seq, uint16_t size,
                     uint32_t ms, uint8_t *req, uint32_t *n)
{
    const uint8_t *m;
    size_t mn;
    *txid = idl_txid_next(&txc);
    CHECK_ST(net_echo_send(ch, *txid, addr, seq, size, ms), OK);
    CHECK_ST(netdrv_recv(req, n, NETDRV_WAIT), OK);
    CHECK(pkt_is_ipv4(req, *n, addr, 1, &m, &mn));
    CHECK_EQ(mn, 8u + size);
    CHECK_EQ(m[0], 8);
    CHECK_EQ(m[1], 0);
    CHECK_EQ(pkt_sum16(m, mn), 0xffff);
    CHECK_EQ(pkt_get16(m + 6), seq);
    for (unsigned i = 0; i < size; i++)
        CHECK_EQ(m[8 + i], (uint8_t)i);
    return true;
}

/* The router says req's destination is unreachable. */
static size_t unreachable(uint8_t *r, const uint8_t *req)
{
    pkt_eth(r, pkt_our_mac, pkt_peer_mac, ETH_IPV4);
    uint8_t *m = r + 34;
    m[0] = 3;
    m[1] = 1;
    pkt_put16(m + 2, 0);
    pkt_put32(m + 4, 0);
    memcpy(m + 8, req + 14, 28);   /* its IP header and the first 8 bytes of its ICMP */
    pkt_put16(m + 2, ~pkt_sum16(m, 36) & 0xffff);
    pkt_ipv4(r, 1, GW_IP, OUR_IP, 36);
    return 34 + 36;
}

static bool counters(struct net_counters *c)
{
    CHECK_ST(net_get_counters(netdrv_net(), c), OK);   /* the shared channel answers it */
    return true;
}

/* ---- UDP both ways ---------------------------------------------------------------- */

bool t_netsock_udp(void)
{
    handle_t o;
    struct net_sock s, e;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));   /* netstack learns the peer's MAC */
    CHECK(opener(&o));
    CHECK_ST(net_udp_open(o, 5000, &s), OK);
    CHECK_EQ(s.port, 5000);
    struct net_sock x;
    CHECK_ST(net_udp_open(o, 5000, &x), ERR_ALREADY_BOUND);
    CHECK_ST(net_udp_open(o, 67, &x), ERR_ACCESS_DENIED);
    CHECK_ST(net_udp_open(o, 0, &e), OK);
    CHECK(e.port >= NET_PORT_EPHEMERAL);
    /* In: the datagram, its sender and port. */
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 40000, OUR_IP, 5000, "hello", 5)));
    CHECK_ST(net_recvfrom(&s, &dg, now() + NETDRV_WAIT), OK);
    CHECK_EQ(dg.addr, PEER_IP);
    CHECK_EQ(dg.port, 40000);
    CHECK_EQ(dg.len, 5);
    CHECK(!memcmp(dg.data, "hello", 5));
    CHECK_EQ(dg.data[5], 0);
    CHECK_EQ(dg.dropped, 0);
    /* Out: to the peer, checksummed; the biggest datagram too. */
    CHECK_ST(net_sendto(&s, PEER_IP, 40000, "world!", 6), OK);
    CHECK(sent_udp(PEER_IP, 5000, 40000, "world!", 6));
    static uint8_t big[NET_DGRAM_MAX];
    for (unsigned i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i * 13);
    CHECK_ST(net_sendto(&e, PEER_IP, 7, big, sizeof(big)), OK);
    CHECK(sent_udp(PEER_IP, e.port, 7, big, sizeof(big)));
    /* Nothing queued: at once, or at the timeout. */
    CHECK_ST(net_recvfrom(&s, &dg, 0), ERR_SHOULD_WAIT);
    uint64_t t0 = now();
    CHECK_ST(net_recvfrom(&s, &dg, now() + 200 * NS_PER_MS), ERR_TIMED_OUT);
    CHECK(now() - t0 >= 150 * NS_PER_MS);
    /* A sock_recv waiting, answered when the datagram comes. */
    CHECK_ST(net_recv_arm(&e), OK);
    CHECK_ST(net_sock_take(&e, &dg), ERR_SHOULD_WAIT);
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 53, OUR_IP, e.port, "late", 4)));
    signals_t seen;
    CHECK_ST(jam_object_wait_one(e.ch, SIG_READABLE, now() + NETDRV_WAIT, &seen), OK);
    CHECK_ST(net_sock_take(&e, &dg), OK);
    CHECK(dg.port == 53 && dg.len == 4 && !memcmp(dg.data, "late", 4));
    /* Connected: only the peer's datagrams, and net_send goes to it. */
    CHECK_ST(net_connect(&s, PEER_IP, 40000), OK);
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_other_mac, OTHER_IP, 40000, OUR_IP, 5000, "no", 2)));
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 40001, OUR_IP, 5000, "no", 2)));
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 40000, OUR_IP, 5000, "yes", 3)));
    CHECK_ST(net_recvfrom(&s, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.len == 3 && !memcmp(dg.data, "yes", 3));
    CHECK_ST(net_send(&s, "ok", 2), OK);
    CHECK(sent_udp(PEER_IP, 5000, 40000, "ok", 2));
    /* What a program can't send to. */
    CHECK_ST(net_sendto(&s, 0xffffffffu, 67, "x", 1), ERR_INVALID_ARGS);
    CHECK_ST(net_sendto(&s, OUR_IP | 0xff, 9, "x", 1), ERR_INVALID_ARGS);   /* the subnet's */
    CHECK_ST(net_sendto(&s, 0x7f000001u, 9, "x", 1), ERR_INVALID_ARGS);
    CHECK_ST(net_sendto(&s, 0xe0000001u, 9, "x", 1), ERR_INVALID_ARGS);
    CHECK_ST(net_sendto(&s, PEER_IP, 0, "x", 1), ERR_INVALID_ARGS);
    CHECK_ST(net_sock_send_to_until(s.ch, now() + NETDRV_WAIT, PEER_IP, 9, NET_DGRAM_MAX + 1,
                                    big), ERR_INVALID_ARGS);
    CHECK_ST(netdrv_recv(f, &(uint32_t){ 0 }, NETDRV_QUIET), ERR_TIMED_OUT);   /* none left */
    uint16_t port, peer_port;
    uint32_t peer, queued, dropped;
    CHECK_ST(net_sock_state(s.ch, &port, &peer, &peer_port, &queued, &dropped), OK);
    CHECK(port == 5000 && peer == PEER_IP && peer_port == 40000 && !queued && !dropped);
    net_close(&s);
    net_close(&e);
    jam_handle_close(o);
    CHECK(netdrv_stop());
    return true;
}

/* ---- ping -------------------------------------------------------------------------- */

/* libos's blocking net_ping, in a thread of its own while the test answers
 * as the network. */
struct pinger {
    handle_t net;
    uint64_t wait;          /* its deadline, from when it starts */
    status_t st;
    uint32_t rtt;
    uint8_t  ttl;
    volatile bool done;
};

static void pinger(void *arg)
{
    struct pinger *p = arg;
    p->st = net_ping(p->net, PEER_IP, 11, 24, now() + p->wait, &p->rtt, &p->ttl);
    p->done = true;
}

/* One net_ping in a thread: answered (reply) or not; its status. */
static bool blocking_ping(handle_t net, bool reply, status_t want)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static uint8_t req[NETDEV_FRAME_MAX];
    struct pinger p = { .net = net, .wait = reply ? NETDRV_WAIT : 300 * NS_PER_MS };
    handle_t th;
    uint32_t n;
    CHECK_ST(thread_spawn("pinger", pinger, &p, stack, sizeof(stack), &th), OK);
    CHECK_ST(netdrv_recv(req, &n, NETDRV_WAIT), OK);
    CHECK_EQ(n, 34u + 8 + 24);
    if (reply)
        CHECK(netdrv_send(g, pkt_echo_reply_to(g, req, 8 + 24)));
    for (uint64_t end = now() + 2 * NETDRV_WAIT; !p.done && now() < end;)
        jam_nanosleep(now() + 5 * NS_PER_MS);
    CHECK(p.done);
    jam_handle_close(th);
    CHECK_ST(p.st, want);
    CHECK(!reply || p.ttl == 64);
    return true;
}

bool t_netsock_ping(void)
{
    static uint8_t req[NETDEV_FRAME_MAX], req2[NETDEV_FRAME_MAX];
    handle_t o, o2;
    uint32_t txid, txid2, n, n2, rtt = 0;
    uint8_t ttl = 0;
    uint16_t size = 0;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));
    CHECK(opener(&o));
    CHECK(opener(&o2));
    /* Answered: the round trip, the TTL, the size. */
    CHECK(echo_out(o, &txid, PEER_IP, 7, 32, NETDRV_WAIT / NS_PER_MS, req, &n));
    jam_nanosleep(now() + 2 * NS_PER_MS);
    CHECK(netdrv_send(g, pkt_echo_reply_to(g, req, 8 + 32)));
    CHECK_ST(reply_of(o, txid, NETDRV_WAIT, &rtt, &ttl, &size), OK);
    CHECK(rtt >= 2000 && rtt < NETDRV_WAIT / 1000);
    CHECK_EQ(ttl, 64);
    CHECK_EQ(size, 32);
    /* Two openers ping the same peer with the same seq: their ids differ,
     * and a reply carrying one's id never answers the other. */
    CHECK(echo_out(o, &txid, PEER_IP, 8, 0, NETDRV_WAIT / NS_PER_MS, req, &n));
    CHECK(echo_out(o2, &txid2, PEER_IP, 8, 0, 400, req2, &n2));
    CHECK(pkt_get16(req + 38) != pkt_get16(req2 + 38));
    CHECK(netdrv_send(g, pkt_echo_reply_to(g, req, 8)));
    CHECK_ST(reply_of(o, txid, NETDRV_WAIT, &rtt, &ttl, &size), OK);
    CHECK_ST(reply_of(o2, txid2, NETDRV_WAIT, &rtt, &ttl, &size), ERR_TIMED_OUT);
    /* A reply nobody waits for any more: nothing. Unreachable: ERR_NOT_FOUND. */
    CHECK(netdrv_send(g, pkt_echo_reply_to(g, req2, 8)));
    CHECK_ST(reply_of(o2, txid2, NETDRV_QUIET, NULL, NULL, NULL), NO_REPLY);
    CHECK(echo_out(o, &txid, PEER_IP, 9, 16, NETDRV_WAIT / NS_PER_MS, req, &n));
    CHECK(netdrv_send(g, unreachable(g, req)));
    CHECK_ST(reply_of(o, txid, NETDRV_WAIT, NULL, NULL, NULL), ERR_NOT_FOUND);
    /* At once: a bad address or timeout, the same seq to the same peer twice. */
    CHECK(echo_out(o, &txid, PEER_IP, 10, 0, 300, req, &n));
    uint32_t t3 = idl_txid_next(&txc);
    CHECK_ST(net_echo_send(o, t3, PEER_IP, 10, 0, 300), OK);
    CHECK_ST(reply_of(o, t3, NETDRV_WAIT, NULL, NULL, NULL), ERR_BAD_STATE);
    CHECK_ST(reply_of(o, txid, NETDRV_WAIT, NULL, NULL, NULL), ERR_TIMED_OUT);
    CHECK_ST(net_echo_until(o2, now() + NETDRV_WAIT, 0, 1, 0, 100, NULL, NULL, NULL),
             ERR_INVALID_ARGS);
    CHECK_ST(net_echo_until(o2, now() + NETDRV_WAIT, PEER_IP, 1, 0, 0, NULL, NULL, NULL),
             ERR_INVALID_ARGS);
    CHECK_ST(net_echo_until(o2, now() + NETDRV_WAIT, PEER_IP, 1, NET_DGRAM_MAX + 1, 100, NULL,
                            NULL, NULL), ERR_INVALID_ARGS);
    CHECK(blocking_ping(o2, true, OK));
    CHECK(blocking_ping(o2, false, ERR_TIMED_OUT));
    struct net_counters c;
    CHECK(counters(&c));
    CHECK(c.echoes_sent >= 7 && c.echoes_answered == 3);
    CHECK_EQ(c.later, 0);
    jam_handle_close(o);
    jam_handle_close(o2);
    CHECK(netdrv_stop());
    return true;
}

/* ---- iface and wait_change --------------------------------------------------------- */

bool t_netsock_iface(void)
{
    handle_t o;
    struct net_info i;
    uint32_t v;
    CHECK(netdrv_start());
    CHECK(opener(&o));
    CHECK_ST(net_info(o, &i), OK);
    CHECK(i.address == OUR_IP && i.mask == MASK24 && i.gateway == GW_IP);
    CHECK(i.device && i.link && i.vlan == 21 && i.speed == 1000);
    CHECK(!memcmp(i.mac, pkt_our_mac, 6));
    CHECK_ST(net_wait_up(o, now(), NULL), OK);   /* it has an address: at once */
    /* A wait answered by a change, one by its timeout, one at once. */
    uint32_t t1 = idl_txid_next(&txc), t2 = idl_txid_next(&txc);
    CHECK_ST(net_wait_change_send(o, t1, i.version, NET_WAIT_FOREVER), OK);
    CHECK_ST(netctl_set_dns(netdrv_ctl(), GW_IP, 0), OK);
    _Alignas(8) uint8_t rep[NET_REP_MAX];
    struct idl_msg m;
    signals_t seen;
    CHECK_ST(jam_object_wait_one(o, SIG_READABLE, now() + NETDRV_WAIT, &seen), OK);
    CHECK_ST(idl_reply_read(o, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, t1);
    CHECK_ST(net_wait_change_result(rep, &m, &v), OK);
    CHECK_EQ(v, i.version + 1);
    CHECK_ST(net_wait_change_send(o, t2, v, 100), OK);
    CHECK_ST(jam_object_wait_one(o, SIG_READABLE, now() + NETDRV_WAIT, &seen), OK);
    CHECK_ST(idl_reply_read(o, rep, sizeof(rep), &m), OK);
    CHECK_ST(net_wait_change_result(rep, &m, &v), ERR_TIMED_OUT);
    /* The card's own counts: netstack asks the driver, then answers. */
    uint32_t t3 = idl_txid_next(&txc);
    uint8_t counts[NETDEV_STATS_SIZE];
    CHECK_ST(net_chip_counts_send(o, t3), OK);
    CHECK(netdrv_serve_session());
    CHECK_ST(jam_object_wait_one(o, SIG_READABLE, now() + NETDRV_WAIT, &seen), OK);
    CHECK_ST(idl_reply_read(o, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, t3);
    CHECK_ST(net_chip_counts_result(rep, &m, counts), OK);
    CHECK_EQ(((struct netdev_stats *)counts)->rx_frames, 42);
    jam_handle_close(o);
    CHECK(opener(&o));   /* a fresh channel for blocking calls */
    CHECK_ST(net_wait_change_until(o, now() + NETDRV_WAIT, i.version, 100, &v), OK);
    CHECK_ST(net_info(o, &i), OK);
    CHECK_EQ(i.dns[0], GW_IP);
    /* Cleared: no address, and net_wait_up waits for one. */
    CHECK_ST(netctl_clear(netdrv_ctl()), OK);
    CHECK_ST(net_wait_up(o, now() + 200 * NS_PER_MS, &i), ERR_TIMED_OUT);
    CHECK_ST(netctl_set_ipv4(netdrv_ctl(), OUR_IP, MASK24, GW_IP), OK);
    CHECK_ST(net_wait_up(o, now() + NETDRV_WAIT, &i), OK);
    CHECK_EQ(i.address, OUR_IP);
    jam_handle_close(o);
    CHECK(netdrv_stop());
    return true;
}
