/* utest: /svc/net against programs that take too much or send nonsense,
 * and netctl's DHCP socket, through bin/netstack over the fake driver
 * (netdrv.h; netsock.c has the ordinary use). The limits (openers,
 * sockets per opener and in all, requests in flight), hostile messages on
 * every kind of channel, closing in the middle of a receive or an echo,
 * a slow reader whose queue fills while everyone else is served at once
 * (the slow-peer rule), and the DHCP socket's broadcasts from 0.0.0.0,
 * which no program's socket can send. netdrv_stop checks that netstack's
 * job ends empty each time. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/net.h>
#include <idl/netctl.h>
#include <idl/svc.h>
#include <jam/netdev.h>
#include <net.h>
#include <netbytes.h>
#include <os.h>
#include "netdrv.h"
#include "netpkt.h"
#include "utest.h"

#define ANY_ERROR 1   /* expect: any status below 0 */
#define NO_ANSWER 2   /* expect: no answer at all */

static uint8_t f[NETDEV_FRAME_MAX];
static struct net_dgram dg;
static uint32_t txc = 0x50000000u;   /* our async txids, far from the kernel's */

static bool opener(handle_t *out)
{
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, out), OK);
    return true;
}

static bool counters(struct net_counters *c)
{
    CHECK_ST(net_get_counters(netdrv_net(), c), OK);
    return true;
}

/* Wait until a status-returning step says OK (netstack takes a close in
 * its own time). */
#define EVENTUALLY_OK(expr)                                                     \
    do {                                                                        \
        uint64_t end_ = now() + NETDRV_WAIT;                                    \
        status_t st_;                                                           \
        while ((st_ = (expr)) != OK && now() < end_)                            \
            jam_nanosleep(now() + 10 * NS_PER_MS);                              \
        CHECK_ST(st_, OK);                                                      \
    } while (0)

/* ---- limits ------------------------------------------------------------------------ */

bool t_netsock_limits(void)
{
    static handle_t o[NET_OPENERS];
    static struct net_sock s[NET_SOCKETS_MAX];
    struct net_sock x;
    struct net_counters c;
    handle_t extra;
    CHECK(netdrv_start());
    for (unsigned i = 0; i < NET_OPENERS; i++)
        CHECK(opener(&o[i]));
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &extra), ERR_NO_RESOURCES);
    for (unsigned k = 0; k < NET_SOCKETS_PER_OPENER; k++) {
        CHECK_ST(net_udp_open(o[0], 0, &s[k]), OK);
        CHECK_ST(net_udp_open(o[1], 0, &s[NET_SOCKETS_PER_OPENER + k]), OK);
    }
    CHECK_ST(net_udp_open(o[0], 0, &x), ERR_NO_RESOURCES);   /* its 16 */
    CHECK_ST(net_udp_open(o[2], 0, &x), ERR_NO_RESOURCES);   /* the 32 */
    CHECK(counters(&c));
    CHECK(c.openers == NET_OPENERS && c.sockets == NET_SOCKETS_MAX);
    net_close(&s[0]);
    EVENTUALLY_OK(net_udp_open(o[2], 0, &x));
    net_close(&x);
    /* An opener's end closes its sockets, and frees its slot. */
    jam_handle_close(o[1]);
    for (unsigned k = 0; k < NET_SOCKETS_PER_OPENER; k++) {
        signals_t seen;
        CHECK_ST(jam_object_wait_one(s[NET_SOCKETS_PER_OPENER + k].ch, SIG_PEER_CLOSED,
                                     now() + NETDRV_WAIT, &seen), OK);
        net_close(&s[NET_SOCKETS_PER_OPENER + k]);
    }
    EVENTUALLY_OK(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &o[1]));
    /* Requests in flight: NET_LATER_PER_OPENER, then refused. */
    struct net_info in;
    CHECK_ST(net_info(o[4], &in), OK);
    uint32_t first = txc + 1;
    for (unsigned k = 0; k <= NET_LATER_PER_OPENER; k++)
        CHECK_ST(net_wait_change_send(o[3], idl_txid_next(&txc), in.version, NET_WAIT_FOREVER),
                 OK);
    _Alignas(8) uint8_t rep[NET_REP_MAX];
    struct idl_msg m;
    uint32_t v;
    signals_t seen;
    CHECK_ST(jam_object_wait_one(o[3], SIG_READABLE, now() + NETDRV_WAIT, &seen), OK);
    CHECK_ST(idl_reply_read(o[3], rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, first + NET_LATER_PER_OPENER);
    CHECK_ST(net_wait_change_result(rep, &m, &v), ERR_NO_RESOURCES);
    CHECK(counters(&c));
    CHECK_EQ(c.later, NET_LATER_PER_OPENER);
    for (unsigned i = 0; i < NET_OPENERS; i++)
        jam_handle_close(o[i]);
    for (unsigned k = 1; k < NET_SOCKETS_PER_OPENER; k++)
        net_close(&s[k]);
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counters(&c));
    } while ((c.openers || c.sockets || c.later) && now() < end);
    CHECK(!c.openers && !c.sockets && !c.later);
    CHECK(netdrv_stop());
    return true;
}

/* ---- hostile programs --------------------------------------------------------------- */

/* Write n bytes (and handle h, if any) on ch; the answer must be `want`
 * (a status, ANY_ERROR or NO_ANSWER). */
static bool raw(handle_t ch, const void *msg, uint32_t n, handle_t h, status_t want)
{
    CHECK_ST(jam_channel_write(ch, msg, n, h ? &h : NULL, h ? 1 : 0), OK);
    signals_t seen;
    status_t st = jam_object_wait_one(ch, SIG_READABLE, now() + (want == NO_ANSWER ? NETDRV_QUIET
                                                                                  : NETDRV_WAIT),
                                      &seen);
    if (want == NO_ANSWER) {
        CHECK_ST(st, ERR_TIMED_OUT);
        return true;
    }
    CHECK_ST(st, OK);
    struct idl_rep_hdr r = { 0 };
    uint32_t got = 0, nh = 0;
    CHECK_ST(drv_channel_read(ch, &r, sizeof(r), &got, NULL, 0, &nh), OK);
    CHECK_EQ(got, sizeof(r));
    CHECK_EQ(r.txid, ((const struct idl_req_hdr *)msg)->txid);
    if (want == ANY_ERROR)
        CHECK(r.status < 0);
    else
        CHECK_ST(r.status, want);
    return true;
}

/* Garbage of every shape on ch. */
static bool garbage(handle_t ch, status_t big)
{
    static uint8_t huge[8192];
    struct idl_req_hdr h = { 77, (NET_PROTOCOL_ID << 16) | 99 };
    handle_t ev;
    CHECK(raw(ch, "\x01\x02", 2, 0, NO_ANSWER));
    CHECK(raw(ch, "\x05\0\0\0\x01\x02", 6, 0, ANY_ERROR));
    CHECK(raw(ch, &h, sizeof(h), 0, ERR_NOT_SUPPORTED));
    h.ordinal = NETCTL_INFO;   /* another protocol's method */
    CHECK(raw(ch, &h, sizeof(h), 0, ERR_NOT_SUPPORTED));
    struct net_sock_send_to_req q = { .txid = 78, .ordinal = NET_SOCK_SEND_TO, .len = 0xffff };
    CHECK(raw(ch, &q, sizeof(q) - 1, 0, ERR_INVALID_ARGS));   /* a byte short */
    memcpy(huge, &h, sizeof(h));
    CHECK(raw(ch, huge, sizeof(huge), 0, big));
    h.ordinal = NET_IFACE;
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK(raw(ch, &h, sizeof(h), ev, ERR_INVALID_ARGS));   /* no method takes handles */
    return true;
}

bool t_netsock_hostile(void)
{
    handle_t o, o2, o3;
    struct net_sock s, s2;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));
    CHECK(opener(&o));
    CHECK_ST(net_udp_open(o, 6000, &s), OK);
    CHECK(garbage(o, ERR_INVALID_ARGS));
    CHECK(garbage(s.ch, ERR_INVALID_ARGS));
    CHECK(garbage(netdrv_net(), ANY_ERROR));
    /* Each kind of channel refuses the other's methods. */
    uint16_t a16;
    uint32_t a32;
    struct net_info in;
    handle_t h;
    CHECK_ST(net_sock_state(o, &a16, &a32, &a16, &a32, &a32), ERR_NOT_SUPPORTED);
    CHECK_ST(net_info(s.ch, &in), ERR_NOT_SUPPORTED);
    CHECK_ST(net_udp_until(netdrv_net(), now() + NETDRV_WAIT, 0, &h, &a16), ERR_NOT_SUPPORTED);
    CHECK_ST(net_echo_until(netdrv_net(), now() + NETDRV_WAIT, PEER_IP, 1, 0, 100, NULL, NULL,
                            NULL), ERR_NOT_SUPPORTED);
    CHECK_ST(net_info(netdrv_net(), &in), OK);   /* answered at once: allowed there */
    /* Two sock_recvs at once: the second refused. */
    CHECK_ST(net_sock_recv_send(s.ch, idl_txid_next(&txc), NET_WAIT_FOREVER), OK);
    CHECK_ST(net_sock_recv_until(s.ch, now() + NETDRV_WAIT, NET_WAIT_FOREVER, &a32, &a16, &a16,
                                 &a32, dg.data), ERR_BAD_STATE);
    /* Closing mid-receive, mid-echo and mid-wait: then their answers come. */
    CHECK(opener(&o2));
    CHECK_ST(net_udp_open(o2, 6001, &s2), OK);
    CHECK_ST(net_recv_arm(&s2), OK);
    net_close(&s2);
    net_close(&s);
    CHECK(opener(&o3));
    CHECK_ST(net_echo_send(o3, idl_txid_next(&txc), PEER_IP, 5, 8, 2000), OK);
    CHECK_ST(net_wait_change_send(o3, idl_txid_next(&txc), in.version, NET_WAIT_FOREVER), OK);
    uint32_t n;
    CHECK_ST(netdrv_recv(f, &n, NETDRV_WAIT), OK);   /* the echo request */
    jam_handle_close(o3);
    jam_handle_close(o2);
    jam_nanosleep(now() + 50 * NS_PER_MS);
    CHECK(netdrv_send(f, pkt_echo_reply_to(f, f, 8 + 8)));   /* its reply, nobody's now */
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 9, OUR_IP, 6001, "x", 1)));
    const uint8_t *m;
    size_t mn;
    CHECK_ST(netdrv_recv(f, &n, NETDRV_WAIT), OK);   /* its socket is gone: port unreachable */
    CHECK(pkt_is_ipv4(f, n, PEER_IP, 1, &m, &mn));
    CHECK(m[0] == 3 && m[1] == 3);
    CHECK(netdrv_ping(2, false));   /* still answering */
    struct net_counters c;
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK_ST(net_get_counters(o, &c), OK);
    } while ((c.sockets || c.later || c.openers != 1) && now() < end);
    CHECK(!c.sockets && !c.later && c.openers == 1);
    jam_handle_close(o);
    CHECK(netdrv_stop());
    return true;
}

/* ---- a slow reader ------------------------------------------------------------------ */

bool t_netsock_slow_reader(void)
{
    handle_t a, b;
    struct net_sock sa, sb, silent;
    struct net_counters c0, c;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));
    CHECK(opener(&a));
    CHECK(opener(&b));
    CHECK_ST(net_udp_open(a, 7000, &sa), OK);
    CHECK_ST(net_udp_open(a, 7002, &silent), OK);
    CHECK_ST(net_udp_open(b, 7001, &sb), OK);
    CHECK_ST(net_recv_arm(&silent), OK);   /* waits for a peer that never answers */
    CHECK(counters(&c0));
    for (uint32_t i = 0; i < NET_RX_QUEUE + 8; i++)
        CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 4000, OUR_IP, 7000, &i,
                                          sizeof(i))));
    /* Everyone else is served at once meanwhile. */
    uint64_t t0 = now();
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 4000, OUR_IP, 7001, "fast", 4)));
    CHECK_ST(net_recvfrom(&sb, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.len == 4 && !memcmp(dg.data, "fast", 4));
    CHECK(now() - t0 < 500 * NS_PER_MS);   /* the plan's 200 ms, with room for QEMU */
    CHECK(netdrv_ping(2, false));
    uint16_t port, pp;
    uint32_t peer, queued, dropped;
    CHECK_ST(net_sock_state(sa.ch, &port, &peer, &pp, &queued, &dropped), OK);
    CHECK_EQ(queued, NET_RX_QUEUE);
    CHECK_EQ(dropped, 8);
    CHECK(counters(&c));
    CHECK_EQ(c.dgrams_dropped - c0.dgrams_dropped, 8);
    CHECK_EQ(c.rx_buffers_used, c0.rx_buffers_used);   /* lwIP's buffers aren't held */
    CHECK_EQ(c.queued, NET_RX_QUEUE);
    /* The queue, oldest first, then empty. */
    for (uint32_t i = 0; i < NET_RX_QUEUE; i++) {
        uint32_t v;
        CHECK_ST(net_recvfrom(&sa, &dg, now() + NETDRV_WAIT), OK);
        memcpy(&v, dg.data, sizeof(v));
        CHECK_EQ(v, i);
        CHECK_EQ(dg.dropped, 8);
    }
    CHECK_ST(net_recvfrom(&sa, &dg, 0), ERR_SHOULD_WAIT);
    CHECK_ST(net_sock_take(&silent, &dg), ERR_SHOULD_WAIT);
    net_close(&sa);
    net_close(&sb);
    net_close(&silent);
    jam_handle_close(a);
    jam_handle_close(b);
    CHECK(netdrv_stop());
    return true;
}

/* ---- a datagram longer than it can be (M9-REVIEW item 5) ----------------------------- */

/* The sock_recv request s armed, read off the far end: its txid. */
static bool recv_asked(handle_t far, uint32_t *txid)
{
    struct net_sock_recv_req q;
    uint32_t n = 0, nh = 0;
    CHECK_ST(drv_channel_read(far, &q, sizeof(q), &n, NULL, 0, &nh), OK);
    CHECK(n == sizeof(q) && q.ordinal == NET_SOCK_RECV);
    *txid = q.txid;
    return true;
}

static bool recv_answer(handle_t far, uint32_t txid, uint16_t len)
{
    static struct net_sock_recv_rep r;
    r = (struct net_sock_recv_rep){ .txid = txid, .status = OK, .address = PEER_IP,
                                    .port = 4000, .len = len };
    CHECK_ST(drv_channel_write(far, &r, sizeof(r), NULL, 0), OK);
    return true;
}

/* libos's net_sock_take over a hand-made netstack: a reply saying more
 * bytes than a datagram holds (1472) is refused, so a caller never reads
 * past d->data; the socket then goes on. (net_recvfrom shares the check.) */
bool t_netsock_len_lies(void)
{
    handle_t near, far;
    struct net_sock s;
    uint32_t txid;
    CHECK_ST(jam_channel_create(&near, &far), OK);
    net_sock_adopt(&s, near, 7000);
    CHECK_ST(net_recv_arm(&s), OK);
    CHECK(recv_asked(far, &txid));
    CHECK(recv_answer(far, txid, NET_DGRAM_MAX + 1));
    CHECK_ST(net_sock_take(&s, &dg), ERR_OUT_OF_RANGE);
    CHECK_ST(net_recv_arm(&s), OK);   /* as a caller does after a failed receive */
    CHECK(recv_asked(far, &txid));
    CHECK(recv_answer(far, txid, NET_DGRAM_MAX));
    CHECK_ST(net_sock_take(&s, &dg), OK);
    CHECK_EQ(dg.len, NET_DGRAM_MAX);
    CHECK(recv_asked(far, &txid));   /* armed again by the take */
    CHECK(recv_answer(far, txid, 0xffff));
    CHECK_ST(net_sock_take(&s, &dg), ERR_OUT_OF_RANGE);
    net_close(&s);
    jam_handle_close(far);
    return true;
}

/* ---- a busy client (M9-REVIEW item 1) ------------------------------------------------ */

struct flooder {
    handle_t ch;            /* its opener channel */
    volatile bool stop;
    uint64_t written;       /* requests it got onto the channel */
};

/* Keep ch full of `counts` requests, written without waiting; replies
 * read and dropped so they never back up. */
static void flood(void *arg)
{
    struct flooder *fl = arg;
    uint32_t txid = 0x70000000u;
    while (!fl->stop) {
        if (net_counts_send(fl->ch, ++txid) == OK) {
            fl->written++;
            continue;
        }
        _Alignas(8) uint8_t rep[NET_REP_MAX];
        struct idl_msg m;
        while (idl_reply_read(fl->ch, rep, sizeof(rep), &m) != ERR_SHOULD_WAIT)
            idl_msg_drop(&m);   /* the channel was full: make room */
    }
}

/* One opener that always has requests waiting holds up nobody else: the
 * other opener's calls and the card's frames are answered meanwhile (the
 * service-loop rule). netstack used to skip its port wait while any
 * channel had work left, so it never learnt of another channel's request
 * or a frame while the flood went on. */
bool t_netsock_busy_client(void)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    struct flooder fl = { 0 };
    handle_t b, th;
    uint32_t addr, mask, gw, d1, d2, speed, version;
    uint8_t mac[6], device, link;
    uint16_t vlan;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));
    CHECK(opener(&fl.ch));
    CHECK(opener(&b));
    CHECK_ST(thread_spawn("flooder", flood, &fl, stack, sizeof(stack), &th), OK);
    while (fl.written < 4 * 64)   /* well past a turn's budget: the flood is on */
        jam_nanosleep(now() + NS_PER_MS);
    for (uint32_t i = 0; i < 5; i++) {
        uint64_t t0 = now();
        CHECK_ST(net_iface_until(b, now() + NETDRV_WAIT, &addr, &mask, &gw, &d1, &d2, mac, &device,
                                 &link, &vlan, &speed, &version), OK);
        CHECK(now() - t0 < 500 * NS_PER_MS);
        CHECK_EQ(addr, OUR_IP);
        CHECK(netdrv_ping(2 + i, false));   /* frames in and out go on too */
    }
    uint64_t before = fl.written;
    jam_nanosleep(now() + 50 * NS_PER_MS);
    CHECK(fl.written > before);   /* still flooding: it was served meanwhile, not starved */
    fl.stop = true;
    signals_t seen;
    CHECK_ST(jam_object_wait_one(th, SIG_TERMINATED, now() + NETDRV_WAIT, &seen), OK);
    jam_handle_close(th);
    jam_handle_close(fl.ch);
    jam_handle_close(b);
    CHECK(netdrv_stop());
    return true;
}

/* ---- netctl's DHCP socket ----------------------------------------------------------- */

/* The next frame: the DHCP socket's broadcast from 0.0.0.0:68 to
 * 255.255.255.255:67 with msg (n bytes). */
static bool sent_dhcp(const uint8_t *msg, size_t n)
{
    uint32_t len;
    CHECK_ST(netdrv_recv(f, &len, NETDRV_WAIT), OK);
    CHECK(pkt_frame_ok(f, len));
    CHECK(!memcmp(f, pkt_bcast, 6));
    CHECK_EQ(pkt_get16(f + 12), ETH_IPV4);
    const uint8_t *ip = f + 14, *u = f + 34;
    CHECK_EQ(pkt_sum16(ip, 20), 0xffff);
    CHECK_EQ(ip[9], 17);
    CHECK_EQ(pkt_get32(ip + 12), 0);
    CHECK_EQ(pkt_get32(ip + 16), 0xffffffffu);
    CHECK(pkt_get16(u) == 68 && pkt_get16(u + 2) == 67 && pkt_get16(u + 4) == 8 + n);
    CHECK(!memcmp(u + 8, msg, n));
    return true;
}

bool t_netsock_dhcp(void)
{
    static uint8_t msg[300];
    handle_t d, d2, o;
    struct net_sock s, u, x;
    CHECK(netdrv_start());
    CHECK_ST(netctl_dhcp_open(netdrv_ctl(), &d), OK);
    CHECK_ST(netctl_dhcp_open(netdrv_ctl(), &d2), ERR_ALREADY_BOUND);
    net_sock_adopt(&s, d, NET_PORT_DHCP_CLIENT);
    CHECK(opener(&o));
    CHECK_ST(net_udp_open(o, NET_PORT_DHCP_CLIENT, &x), ERR_ACCESS_DENIED);
    /* A program's socket gets no broadcast, and can't send one. */
    CHECK_ST(net_udp_open(o, 7000, &u), OK);
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, GW_IP, 7000, 0xffffffffu, 7000, "b", 1)));
    CHECK_ST(net_recvfrom(&u, &dg, now() + NETDRV_QUIET), ERR_TIMED_OUT);
    CHECK_ST(net_sendto(&u, 0xffffffffu, 67, "b", 1), ERR_INVALID_ARGS);
    /* No address: the DHCP socket sends from 0.0.0.0 to the broadcast
     * address, port 67 only; a program's socket can't send at all. */
    CHECK_ST(netctl_clear(netdrv_ctl()), OK);
    for (unsigned i = 0; i < sizeof(msg); i++)
        msg[i] = (uint8_t)(i ^ 0x5a);
    CHECK_ST(net_sendto(&s, 0xffffffffu, NET_PORT_DHCP_SERVER, msg, sizeof(msg)), OK);
    CHECK(sent_dhcp(msg, sizeof(msg)));
    CHECK_ST(net_sendto(&s, 0xffffffffu, 68, msg, 1), ERR_INVALID_ARGS);
    CHECK_ST(net_sendto(&s, 0, 0, msg, 1), ERR_BAD_STATE);   /* no peer */
    CHECK_ST(net_connect(&s, GW_IP, 67), ERR_NOT_SUPPORTED);
    CHECK_ST(net_sendto(&u, PEER_IP, 9, "n", 1), ERR_BAD_STATE);
    /* It hears the server's answers: broadcast, and to an address it hasn't got. */
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, GW_IP, 67, 0xffffffffu, 68, "offer", 5)));
    CHECK_ST(net_recvfrom(&s, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.addr == GW_IP && dg.port == 67 && dg.len == 5 && !memcmp(dg.data, "offer", 5));
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, GW_IP, 67, NET_IPV4(10, 2, 21, 67), 68,
                                      "ack", 3)));
    CHECK_ST(net_recvfrom(&s, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.len == 3 && !memcmp(dg.data, "ack", 3));
    /* Closed: another may be opened. */
    net_close(&s);
    EVENTUALLY_OK(netctl_dhcp_open(netdrv_ctl(), &d));
    jam_handle_close(d);
    net_close(&u);
    jam_handle_close(o);
    CHECK(netdrv_stop());
    return true;
}
