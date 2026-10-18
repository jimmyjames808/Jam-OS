/* utest: /svc/net against programs that take too much or send nonsense,
 * and netctl's DHCP socket, through bin/netstack over the fake driver
 * (netdrv.h; netsock.c has the ordinary use, netrings.c the rings' abuse
 * and the fair shares). The limits (openers, sockets per opener, the
 * ordinary openers' shares and the totals, requests in flight), the caps
 * (an opener at half of all the sockets, two openers at all of them), hostile
 * messages on every kind of channel, closing in the middle of a receive or
 * an echo, a slow reader whose rx ring fills while everyone else is served
 * at once (the slow-peer rule), and the DHCP socket's broadcasts from
 * 0.0.0.0, which no program's socket can send. netdrv_stop checks that
 * netstack's job ends empty each time. */
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
#include "tcppeer.h"
#include "utest.h"

#define ANY_ERROR 1   /* expect: any status below 0 */
#define NO_ANSWER 2   /* expect: no answer at all */

static uint8_t f[NETDEV_FRAME_MAX];
static struct net_dgram dg;
static uint32_t txc = 0x50000000u;   /* our async txids, far from the kernel's */

/* A listening program's opener: the tests take fixed ports. */
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

/* Wait until a status-returning step says OK (netstack takes a close in
 * its own time). */
#define EVENTUALLY_OK(expr)                                                     \
    do {                                                                        \
        uint64_t end_ = now() + NETDRV_WAIT;                                    \
        status_t st_;                                                           \
        while ((st_ = (expr)) != OK && now() < end_)                            \
            jam_sleep_until(now() + 10 * NS_PER_MS);                              \
        CHECK_ST(st_, OK);                                                      \
    } while (0)

/* ---- limits ------------------------------------------------------------------------ */

/* An opener of the system channel (/svc/net-sys). */
static bool sys_opener(handle_t *out)
{
    CHECK_ST(svc_connect_until(netdrv_net_sys(), now() + NETDRV_WAIT, out), OK);
    return true;
}

/* n sockets on opener o into s[*k...]. */
static bool socks(handle_t o, struct net_sock *s, unsigned *k, unsigned n)
{
    for (unsigned i = 0; i < n; i++, (*k)++)
        CHECK_ST(net_udp_open(o, 0, &s[*k]), OK);
    return true;
}

bool t_netsock_limits(void)
{
    static handle_t o[NET_OPENERS];
    static struct net_sock s[NET_SOCKETS_MAX];
    const unsigned np = NET_PROG_OPENERS;
    struct net_sock x;
    struct net_counters c;
    handle_t extra;
    unsigned k = 0;
    CHECK(netdrv_start());
    /* Openers: ordinary ones (either of their channels) up to their share,
     * system ones up to the total. */
    for (unsigned i = 0; i < np; i++)
        CHECK(opener(&o[i]));
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &extra), ERR_NO_RESOURCES);
    CHECK_ST(svc_connect_until(netdrv_net_listen(), now() + NETDRV_WAIT, &extra),
             ERR_NO_RESOURCES);
    for (unsigned i = np; i < NET_OPENERS; i++)
        CHECK(sys_opener(&o[i]));
    CHECK_ST(svc_connect_until(netdrv_net_sys(), now() + NETDRV_WAIT, &extra), ERR_NO_RESOURCES);
    /* Sockets: NET_SOCKETS_PER_OPENER an opener, NET_PROG_SOCKETS for the
     * ordinary ones together, NET_SOCKETS_MAX in all. */
    CHECK(socks(o[0], s, &k, NET_SOCKETS_PER_OPENER));
    CHECK_ST(net_udp_open(o[0], 0, &x), ERR_NO_RESOURCES);   /* its 256 */
    CHECK(socks(o[1], s, &k, NET_PROG_SOCKETS - NET_SOCKETS_PER_OPENER));
    CHECK_ST(net_udp_open(o[2], 0, &x), ERR_NO_RESOURCES);   /* the programs' share */
    CHECK(socks(o[np], s, &k, NET_SOCKETS_MAX - k));   /* the reserve: all of them */
    CHECK_ST(net_udp_open(o[np + 1], 0, &x), ERR_NO_RESOURCES);   /* no more */
    CHECK(counters(&c));
    CHECK(c.openers == NET_OPENERS && c.sockets == NET_SOCKETS_MAX);
    CHECK(c.refused_shares >= 3);
    net_close(&s[0]);
    EVENTUALLY_OK(net_udp_open(o[2], 0, &x));
    net_close(&x);
    /* An opener's end ends its sockets (their status says so; they stay
     * until the program closes them), and frees its slot. */
    jam_handle_close(o[1]);
    for (k = NET_SOCKETS_PER_OPENER; k < NET_PROG_SOCKETS; k++) {
        struct sockring_status ss;
        uint64_t until = now() + NETDRV_WAIT;
        do {
            sockring_status_get(&s[k].r, &ss);
        } while (ss.state != SOCKRING_STATE_CLOSED && now() < until);
        CHECK_EQ(ss.state, SOCKRING_STATE_CLOSED);
        CHECK_ST(ss.error, ERR_PEER_CLOSED);
        net_close(&s[k]);
    }
    EVENTUALLY_OK(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &o[1]));
    /* Requests in flight: NET_LATER_PER_OPENER, then refused. */
    struct net_info in;
    CHECK_ST(net_info(o[4], &in), OK);
    uint32_t first = txc + 1;
    for (unsigned i = 0; i <= NET_LATER_PER_OPENER; i++)
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
    for (k = 1; k < NET_SOCKETS_MAX; k++)
        net_close(&s[k]);   /* nothing to do for the ones closed already */
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counters(&c));
    } while ((c.openers || c.sockets || c.later || c.ring_bytes) && now() < end);
    CHECK(!c.openers && !c.sockets && !c.later && !c.ring_bytes);
    CHECK(netdrv_stop());
    return true;
}

/* The caps: one opener may hold half of all the sockets, and two such
 * openers (an ordinary one and a system one) all of them. The last socket
 * of each still works (its slot is far past a byte: the port keys carry it
 * whole), and every socket closes. */
bool t_netsock_caps(void)
{
    static struct net_sock s[NET_SOCKETS_MAX];
    handle_t a, b, extra;
    struct net_sock x;
    struct net_counters c;
    unsigned k = 0;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));   /* netstack knows the peer's MAC: a datagram goes out at once */
    CHECK(opener(&a));
    CHECK_ST(svc_connect_until(netdrv_net_sys(), now() + NETDRV_WAIT, &b), OK);
    CHECK_ST(svc_connect_until(netdrv_net_sys(), now() + NETDRV_WAIT, &extra), OK);
    CHECK(socks(a, s, &k, NET_SOCKETS_PER_OPENER));
    CHECK_ST(net_udp_open(a, 0, &x), ERR_NO_RESOURCES);   /* one past its cap */
    CHECK(counters(&c));
    CHECK_EQ(c.sockets, NET_SOCKETS_PER_OPENER);
    CHECK(socks(b, s, &k, NET_SOCKETS_PER_OPENER));
    CHECK_ST(net_udp_open(b, 0, &x), ERR_NO_RESOURCES);   /* the other at its cap too */
    CHECK_ST(net_udp_open(extra, 0, &x), ERR_NO_RESOURCES);   /* and all of them taken */
    CHECK(counters(&c));
    CHECK_EQ(c.sockets, NET_SOCKETS_MAX);
    CHECK_EQ(c.ring_bytes, NET_SOCKETS_MAX * sockring_bytes(SOCKRING_UDP_TX, SOCKRING_UDP_RX));
    /* The last socket out and in: its tx ring's event (a slot past 255) is
     * heard, and a datagram for its port reaches its rx ring. */
    struct net_sock *last = &s[NET_SOCKETS_MAX - 1];
    uint32_t n;
    const uint8_t *u;
    size_t un;
    CHECK_ST(net_sendto(last, PEER_IP, 9, "last", 4), OK);
    CHECK_ST(netdrv_recv(f, &n, NETDRV_WAIT), OK);
    CHECK(pkt_is_ipv4(f, n, PEER_IP, 17, &u, &un));
    CHECK(pkt_get16(u) == last->port && un == 8 + 4 && !memcmp(u + 8, "last", 4));
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 40000, OUR_IP, last->port,
                                      "back", 4)));
    CHECK_ST(net_recvfrom(last, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.len == 4 && !memcmp(dg.data, "back", 4));
    for (k = 0; k < NET_SOCKETS_MAX; k++)
        net_close(&s[k]);   /* each channel's end is heard, whatever its slot */
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counters(&c));
    } while ((c.sockets || c.ring_bytes) && now() < end);
    CHECK(!c.sockets && !c.ring_bytes);
    jam_handle_close(a);
    jam_handle_close(b);
    jam_handle_close(extra);
    CHECK(netdrv_stop());
    return true;
}

/* netstack's memory at the caps (docs/BENCH.md): everything the caps allow
 * at once with the default rings, 512 datagram sockets and 1,024 TCP
 * connections (an ordinary opener 256 sockets and 512 connections, a
 * second ordinary one 256 connections, a system one 256 sockets and 256
 * connections), asked for in stages, with netstack's job and counters said
 * after each. Every one must fit the ring byte budgets (64 MiB an opener,
 * 128 MiB in all, 96 MiB ordinary openers'). The fake host answers every
 * SYN, so the connections are open (all 1,024 lwIP pcbs established),
 * idle: the rings are only as touched as an idle connection touches them.
 * Closing gives every handle back (the pages left are said). */
struct memrow {
    uint64_t pages, handles;
    struct net_counters c;
};

static bool memnow(struct memrow *m)
{
    CHECK(netdrv_job_used(&m->pages, &m->handles));
    CHECK_ST(net_get_counters(netdrv_net(), &m->c), OK);
    return true;
}

static void memsay(const char *what, const struct memrow *m, const struct memrow *base)
{
    printf("utest: netsock_mem: %s: netstack's job %lu pages (%lu KiB; +%lu KiB), %lu handles, "
           "heap used %u, ring bytes %u KiB, %u sockets, %u connections\n", what,
           (unsigned long)m->pages, (unsigned long)(m->pages * 4),
           (unsigned long)((m->pages - base->pages) * 4), (unsigned long)m->handles,
           m->c.heap_used, m->c.ring_bytes / 1024, m->c.sockets, m->c.tcp_conns);
}

/* A connection from opener o to the fake host (PEER_IP port 9000), which
 * answers netstack's SYN at once: the connection is open when this
 * returns the call. Frames before the SYN (earlier connections' ACKs) are
 * passed over. */
static bool host_answers(handle_t o, struct net_sock *s)
{
    uint8_t fr[PKT_FRAME_MAX];
    CHECK_ST(net_tcp_open(o, PEER_IP, 9000, 0, 0, s), OK);
    for (unsigned k = 0; k < 64; k++) {   /* a few frames at most */
        uint32_t n;
        const uint8_t *t;
        size_t len;
        CHECK_ST(netdrv_recv(fr, &n, NETDRV_WAIT), OK);
        if (!pkt_is_ipv4(fr, n, PEER_IP, 6, &t, &len) || len < 20 ||
            (t[13] & (TP_SYN | TP_ACK)) != TP_SYN)
            continue;
        struct tp p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = pkt_get16(t + 2),
                        .our = pkt_get16(t), .snd = 0x52000000u, .rcv = pkt_get32(t + 4) + 1,
                        .win = 0xffff };
        CHECK(netdrv_send(fr, tp_frame(fr, &p, TP_SYN | TP_ACK, NULL, 0)));
        CHECK_ST(net_tcp_wait_open(s, now() + NETDRV_WAIT), OK);
        return true;
    }
    FAIL("no SYN for connection to port 9000");
}

bool t_netsock_mem(void)
{
    static struct net_sock us[NET_SOCKETS_MAX], ts[NET_TCP_MAX];
    handle_t a, b, c;
    struct memrow m0, m1, m2, m3;
    unsigned nu = 0, nt = 0;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));
    CHECK(opener(&a));
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &c), OK);
    CHECK_ST(svc_connect_until(netdrv_net_sys(), now() + NETDRV_WAIT, &b), OK);
    CHECK(memnow(&m0));
    memsay("no sockets", &m0, &m0);
    CHECK(socks(a, us, &nu, NET_SOCKETS_PER_OPENER));
    CHECK(socks(b, us, &nu, NET_SOCKETS_PER_OPENER));
    CHECK(memnow(&m1));
    CHECK_EQ(m1.c.sockets, NET_SOCKETS_MAX);
    memsay("512 sockets", &m1, &m0);
    for (unsigned i = 0; i < NET_TCP_PER_OPENER; i++, nt++)
        CHECK(host_answers(a, &ts[nt]));
    for (unsigned i = 0; i < NET_PROG_TCP - NET_TCP_PER_OPENER; i++, nt++)
        CHECK(host_answers(c, &ts[nt]));
    for (unsigned i = 0; i < NET_TCP_MAX - NET_PROG_TCP; i++, nt++)
        CHECK(host_answers(b, &ts[nt]));
    CHECK(memnow(&m2));
    CHECK_EQ(m2.c.tcp_conns, NET_TCP_MAX);
    memsay("512 sockets and 1,024 connections open", &m2, &m0);
    struct net_sock x;
    CHECK_ST(net_tcp_open(b, PEER_IP, 9000, 0, 0, &x), ERR_NO_RESOURCES);   /* all taken */
    for (unsigned i = 0; i < nt; i++) {   /* all of them still open */
        uint32_t state;
        status_t err;
        net_tcp_status(&ts[i], &state, &err);
        CHECK_EQ(state, SOCKRING_STATE_OPEN);
    }
    for (unsigned i = 0; i < nt; i++)
        net_close(&ts[i]);
    for (unsigned i = 0; i < nu; i++)
        net_close(&us[i]);
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(memnow(&m3));
    } while ((m3.c.sockets || m3.c.tcp_conns || m3.c.ring_bytes || m3.handles > m0.handles) &&
             now() < end);
    CHECK(!m3.c.sockets && !m3.c.tcp_conns && !m3.c.ring_bytes);
    CHECK(m3.handles <= m0.handles);   /* every socket's handles are given back */
    memsay("all closed", &m3, &m0);
    jam_handle_close(a);
    jam_handle_close(b);
    jam_handle_close(c);
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

/* Garbage of every shape on ch, a channel of the protocol `id` whose method
 * `ord` takes `size` bytes (nothing in `ord`'s request but its two header
 * words and fixed fields). */
static bool garbage(handle_t ch, uint32_t id, uint32_t ord, uint32_t size, status_t big)
{
    static uint8_t huge[8192];
    struct idl_req_hdr h = { 77, (id << 16) | 99 };
    handle_t ev;
    CHECK(raw(ch, "\x01\x02", 2, 0, NO_ANSWER));
    CHECK(raw(ch, "\x05\0\0\0\x01\x02", 6, 0, ANY_ERROR));
    CHECK(raw(ch, &h, sizeof(h), 0, ERR_NOT_SUPPORTED));
    h.ordinal = NETCTL_INFO;   /* another protocol's method */
    CHECK(raw(ch, &h, sizeof(h), 0, ERR_NOT_SUPPORTED));
    uint8_t q[64] = { 78 };   /* txid 78, then the method's ordinal */
    memcpy(q + 4, &ord, sizeof(ord));
    CHECK(size <= sizeof(q));
    CHECK(raw(ch, q, size - 1, 0, ERR_INVALID_ARGS));   /* a byte short */
    memcpy(huge, &h, sizeof(h));
    CHECK(raw(ch, huge, sizeof(huge), 0, big));
    h.ordinal = ord;
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
    CHECK(garbage(o, NET_PROTOCOL_ID, NET_ECHO, sizeof(struct net_echo_req), ERR_INVALID_ARGS));
    CHECK(garbage(s.ch, NET_SOCKET_PROTOCOL_ID, NET_SOCKET_CONNECT,
                  sizeof(struct net_socket_connect_req), ERR_INVALID_ARGS));
    CHECK(garbage(netdrv_net(), NET_PROTOCOL_ID, NET_ECHO, sizeof(struct net_echo_req),
                  ANY_ERROR));
    /* Each kind of channel refuses the other's methods. */
    uint16_t a16;
    uint32_t a32;
    struct net_info in;
    handle_t h;
    CHECK_ST(net_socket_state(o, &a32, &a16, &a32, &a16, &a32, &a32), ERR_NOT_SUPPORTED);
    CHECK_ST(net_info(s.ch, &in), ERR_NOT_SUPPORTED);
    CHECK_ST(net_udp_until(netdrv_net(), now() + NETDRV_WAIT, 0, &h, &a16), ERR_NOT_SUPPORTED);
    handle_t hs[3];
    CHECK_ST(net_udp_rings_until(netdrv_net(), now() + NETDRV_WAIT, 0, 0, 0, &h, &hs[0], &hs[1],
                                 &hs[2], &a16, &a32, &a32), ERR_NOT_SUPPORTED);
    CHECK_ST(net_socket_rings_until(o, now() + NETDRV_WAIT, 0, 0, &hs[0], &hs[1], &hs[2], &a32,
                                  &a32), ERR_NOT_SUPPORTED);
    CHECK_ST(net_echo_until(netdrv_net(), now() + NETDRV_WAIT, PEER_IP, 1, 0, 100, NULL, NULL,
                            NULL), ERR_NOT_SUPPORTED);
    CHECK_ST(net_info(netdrv_net(), &in), OK);   /* answered at once: allowed there */
    /* A socket's rings once; sizes that aren't ring sizes. */
    CHECK_ST(net_socket_rings_until(s.ch, now() + NETDRV_WAIT, 0, 0, &hs[0], &hs[1], &hs[2], &a32,
                                  &a32), ERR_BAD_STATE);
    static const uint32_t bad[] = { 1, 2048, 5000, SOCKRING_MAX * 2, 0xffffffffu };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK_ST(net_udp_rings_until(o, now() + NETDRV_WAIT, 0, bad[i], 0, &h, &hs[0], &hs[1],
                                     &hs[2], &a16, &a32, &a32), ERR_INVALID_ARGS);
    /* Closing mid-receive, mid-echo and mid-wait: then their answers come. */
    CHECK(opener(&o2));
    CHECK_ST(net_udp_open(o2, 6001, &s2), OK);
    CHECK_ST(net_sock_take(&s2, &dg), ERR_SHOULD_WAIT);   /* asleep on its rx ring */
    net_close(&s2);
    net_close(&s);
    CHECK(opener(&o3));
    CHECK_ST(net_echo_send(o3, idl_txid_next(&txc), PEER_IP, 5, 8, 2000), OK);
    CHECK_ST(net_wait_change_send(o3, idl_txid_next(&txc), in.version, NET_WAIT_FOREVER), OK);
    uint32_t n;
    CHECK_ST(netdrv_recv(f, &n, NETDRV_WAIT), OK);   /* the echo request */
    jam_handle_close(o3);
    jam_handle_close(o2);
    jam_sleep_until(now() + 50 * NS_PER_MS);
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
    static uint8_t big[1000];
    const uint32_t kept = SOCKRING_MIN / sockring_dgram_bytes(sizeof(big)), more = 8;
    handle_t a, b;
    struct net_sock sa, sb, silent;
    struct net_counters c0, c;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));
    CHECK(opener(&a));
    CHECK(opener(&b));
    CHECK_ST(net_udp_open_rings(a, 7000, 0, SOCKRING_MIN, &sa), OK);   /* a small rx ring */
    CHECK_ST(net_udp_open(a, 7002, &silent), OK);
    CHECK_ST(net_udp_open(b, 7001, &sb), OK);
    CHECK_ST(net_sock_take(&silent, &dg), ERR_SHOULD_WAIT);   /* waits for a peer that never answers */
    CHECK(counters(&c0));
    for (uint32_t i = 0; i < kept + more; i++) {
        memcpy(big, &i, sizeof(i));
        CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 4000, OUR_IP, 7000, big,
                                          sizeof(big))));
    }
    /* Everyone else is served at once meanwhile. */
    uint64_t t0 = now();
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 4000, OUR_IP, 7001, "fast", 4)));
    CHECK_ST(net_recvfrom(&sb, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.len == 4 && !memcmp(dg.data, "fast", 4));
    CHECK(now() - t0 < 500 * NS_PER_MS);   /* the plan's 200 ms, with room for QEMU */
    CHECK(netdrv_ping(2, false));
    uint16_t port, pp;
    uint32_t addr, peer, queued, dropped;
    CHECK_ST(net_socket_state(sa.ch, &addr, &port, &peer, &pp, &queued, &dropped), OK);
    CHECK_EQ(queued, kept * sockring_dgram_bytes(sizeof(big)));   /* bytes in its rx ring */
    CHECK_EQ(dropped, more);
    CHECK(counters(&c));
    CHECK_EQ(c.dgrams_dropped - c0.dgrams_dropped, more);
    CHECK_EQ(c.rx_buffers_used, c0.rx_buffers_used);   /* lwIP's buffers aren't held */
    CHECK_EQ(c.queued, queued);
    /* The ring, oldest first, then empty. */
    for (uint32_t i = 0; i < kept; i++) {
        uint32_t v;
        CHECK_ST(net_recvfrom(&sa, &dg, now() + NETDRV_WAIT), OK);
        memcpy(&v, dg.data, sizeof(v));
        CHECK_EQ(v, i);
        CHECK_EQ(dg.dropped, more);
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

/* ---- a busy client (M9 review item 1) ------------------------------------------------ */

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
        jam_sleep_until(now() + NS_PER_MS);
    for (uint32_t i = 0; i < 5; i++) {
        uint64_t t0 = now();
        CHECK_ST(net_iface_until(b, now() + NETDRV_WAIT, &addr, &mask, &gw, &d1, &d2, mac, &device,
                                 &link, &vlan, &speed, &version), OK);
        CHECK(now() - t0 < 500 * NS_PER_MS);
        CHECK_EQ(addr, OUR_IP);
        CHECK(netdrv_ping(2 + i, false));   /* frames in and out go on too */
    }
    uint64_t before = fl.written;
    jam_sleep_until(now() + 50 * NS_PER_MS);
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
    CHECK_ST(net_sock_adopt(&s, d, NET_PORT_DHCP_CLIENT), OK);
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
