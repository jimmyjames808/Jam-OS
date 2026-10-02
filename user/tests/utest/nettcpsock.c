/* utest: programs' TCP on /svc/net (net.idl's tcp, tcp_listener and
 * accept; netstack's tcpsock.c), through bin/netstack over the fake
 * driver (netdrv.h), the test as the programs: who may listen (the listen
 * permission, ports below 1024, backlogs), the limits and fair shares on
 * connections and listeners (per opener, ordinary openers together, all),
 * accept's forms (at once, timing out, one at a time), an opener's end
 * resetting its connections, and the counts going back to nothing. The
 * bytes themselves are tools/tcp-test.sh's (QEMU, a real peer) and the
 * in-process nettcp_* tests'. Ends with netstack killed and its job empty. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/net.h>
#include <idl/svc.h>
#include <net.h>
#include <os.h>
#include "netdrv.h"
#include "netpkt.h"
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
        jam_nanosleep(now() + 10 * NS_PER_MS);
    CHECK_ST(st, OK);
    CHECK(l[n++].port >= NET_PORT_EPHEMERAL);
    /* accept: at once, timing out, one at a time. */
    handle_t hs[4];
    uint32_t a, b, c;
    uint16_t p;
    CHECK_ST(net_accept_until(l[0].ch, now() + NETDRV_WAIT, 0, &hs[0], &hs[1], &hs[2], &hs[3], &a,
                              &p, &b, &c), ERR_SHOULD_WAIT);
    CHECK_ST(net_accept_until(l[0].ch, now() + NETDRV_WAIT, 50, &hs[0], &hs[1], &hs[2], &hs[3], &a,
                              &p, &b, &c), ERR_TIMED_OUT);
    CHECK_ST(net_tcp_accept_send(&l[1]), OK);
    CHECK_ST(net_accept_until(l[1].ch, now() + NETDRV_WAIT, 0, &hs[0], &hs[1], &hs[2], &hs[3], &a,
                              &p, &b, &c), ERR_BAD_STATE);
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
    static struct net_sock s[NET_PROG_TCP + 2];
    handle_t o[4], sys;
    struct net_sock x;
    struct net_counters c;
    unsigned k = 0;
    for (unsigned i = 0; i < 4; i++)
        CHECK(connect_on(netdrv_net(), &o[i]));
    CHECK(connect_on(netdrv_net_sys(), &sys));
    /* Port 0, the subnet's broadcast, loopback, a ring size not allowed. */
    CHECK_ST(net_tcp_open(o[0], PEER_IP, 0, 0, 0, &x), ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_open(o[0], 0x0a0215ffu, 80, 0, 0, &x), ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_open(o[0], 0x7f000001u, 80, 0, 0, &x), ERR_INVALID_ARGS);
    CHECK_ST(net_tcp_open(o[0], PEER_IP, 80, 3000, 0, &x), ERR_INVALID_ARGS);
    for (unsigned i = 0; i < 3; i++)
        CHECK(conns(o[i], s, &k, PER));
    CHECK_ST(net_tcp_open(o[0], PEER_IP, 9000, RING, RING, &x), ERR_NO_RESOURCES);   /* its 64 */
    CHECK_ST(net_tcp_open(o[3], PEER_IP, 9000, RING, RING, &x), ERR_NO_RESOURCES);   /* 192 */
    CHECK(conns(sys, s, &k, 1));   /* the reserve */
    CHECK(counts(&c));
    CHECK_EQ(c.tcp_conns, NET_PROG_TCP + 1);
    uint32_t state;
    status_t err;
    net_tcp_status(&s[0], &state, &err);
    CHECK_EQ(state, SOCKRING_STATE_CONNECTING);
    /* An opener's end resets its connections; they stay until closed. */
    jam_handle_close(o[1]);
    for (unsigned i = PER; i < 2 * PER; i++) {
        uint64_t until = now() + NETDRV_WAIT;
        do {
            net_tcp_status(&s[i], &state, &err);
        } while (state != SOCKRING_STATE_CLOSED && now() < until);
        CHECK_EQ(state, SOCKRING_STATE_CLOSED);
        CHECK_ST(err, ERR_PEER_CLOSED);
    }
    for (unsigned i = 0; i < k; i++)
        net_close(&s[i]);
    handle_t os[] = { o[0], o[2], o[3], sys };
    for (unsigned i = 0; i < 4; i++)
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
    CHECK(c.refused_shares >= 3);
    CHECK(netdrv_stop());
    return true;
}
