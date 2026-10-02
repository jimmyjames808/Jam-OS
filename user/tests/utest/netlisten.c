/* utest: the listen permission (netstack's listen.h): which UDP ports an
 * opener of /svc/net may take and an opener of /svc/net-listen may, through
 * bin/netstack over the fake driver (netdrv.h); that a program without it
 * still gets datagrams on a port netstack picked; and the list's line for
 * it (<wants.h>): `svc net listen` grants both channels and says so to the
 * owner, and no other spelling is a want. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/net.h>
#include <idl/svc.h>
#include <jam/netdev.h>
#include <net.h>
#include <os.h>
#include <wants.h>
#include "netdrv.h"
#include "netpkt.h"
#include "utest.h"

static uint8_t f[NETDEV_FRAME_MAX];
static struct net_dgram dg;

/* Port `port` on opener o: want (and the socket closed again). */
static bool takes(handle_t o, uint16_t port, status_t want)
{
    struct net_sock s;
    status_t st = net_udp_open(o, port, &s);
    if (st != want)
        FAIL("port %u: %s, want %s", port, status_str(st), status_str(want));
    if (st == OK)
        net_close(&s);
    return true;
}

static bool plain_opener(handle_t o)
{
    struct net_sock s;
    CHECK(takes(o, 5000, ERR_ACCESS_DENIED));                   /* a server's port */
    CHECK(takes(o, NET_PORT_LOW, ERR_ACCESS_DENIED));
    CHECK(takes(o, NET_PORT_EPHEMERAL - 1, ERR_ACCESS_DENIED));
    CHECK(takes(o, 123, ERR_ACCESS_DENIED));
    CHECK(takes(o, NET_PORT_DHCP_CLIENT, ERR_ACCESS_DENIED));
    CHECK(takes(o, NET_PORT_EPHEMERAL, OK));                    /* a port of its own picking */
    CHECK(takes(o, 65535, OK));
    /* netstack's pick, and a reply to it comes in */
    CHECK_ST(net_udp_open(o, 0, &s), OK);
    CHECK(s.port >= NET_PORT_EPHEMERAL);
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 123, OUR_IP, s.port, "tick", 4)));
    CHECK_ST(net_recvfrom(&s, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.addr == PEER_IP && dg.port == 123 && dg.len == 4 && !memcmp(dg.data, "tick", 4));
    net_close(&s);
    return true;
}

static bool listen_opener(handle_t o)
{
    struct net_sock s;
    CHECK(takes(o, NET_PORT_LOW, OK));
    CHECK(takes(o, NET_PORT_EPHEMERAL - 1, OK));
    CHECK(takes(o, NET_PORT_EPHEMERAL, OK));
    CHECK(takes(o, NET_PORT_LOW - 1, ERR_ACCESS_DENIED));       /* never below 1024 */
    CHECK(takes(o, NET_PORT_DHCP_CLIENT, ERR_ACCESS_DENIED));
    /* a datagram nobody asked for, to the port it listens on */
    CHECK_ST(net_udp_open(o, 5000, &s), OK);
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 40000, OUR_IP, 5000, "hi", 2)));
    CHECK_ST(net_recvfrom(&s, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.addr == PEER_IP && dg.port == 40000 && dg.len == 2);
    net_close(&s);
    return true;
}

bool t_netlisten_udp(void)
{
    handle_t plain, lis, h;
    uint16_t port;
    struct net_info in;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));   /* netstack learns the peer's MAC */
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &plain), OK);
    CHECK_ST(svc_connect_until(netdrv_net_listen(), now() + NETDRV_WAIT, &lis), OK);
    CHECK(plain_opener(plain));
    CHECK(listen_opener(lis));
    /* the listen channel's shared end answers what /svc/net's does, no more */
    CHECK_ST(net_info(netdrv_net_listen(), &in), OK);
    CHECK(in.address == OUR_IP);
    CHECK_ST(net_udp_until(netdrv_net_listen(), now() + NETDRV_WAIT, 5000, &h, &port),
             ERR_NOT_SUPPORTED);
    /* a port a listener holds is taken for everyone */
    struct net_sock s;
    CHECK_ST(net_udp_open(lis, 60000, &s), OK);
    CHECK(takes(plain, 60000, ERR_ALREADY_BOUND));
    net_close(&s);
    jam_handle_close(plain);
    jam_handle_close(lis);
    CHECK(netdrv_stop());
    return true;
}

/* The list's text: grants want[] (NULL-terminated), or refused. */
static bool parsed(const char *text, const char *const *want, const char *shown)
{
    struct wants w;
    status_t st = wants_parse(text, strlen(text), &w);
    if (!want) {
        if (st != ERR_INVALID_ARGS)
            FAIL("\"%s\": %s, want refused", text, status_str(st));
        return true;
    }
    CHECK_ST(st, OK);
    unsigned n = 0;
    while (want[n])
        n++;
    CHECK_EQ(w.n, n);
    for (unsigned i = 0; i < n; i++)
        if (strcmp(w.grant[i], want[i]))
            FAIL("\"%s\": grant %u is %s, want %s", text, i, w.grant[i], want[i]);
    if (shown && strcmp(w.text, shown))
        FAIL("\"%s\": shown as \"%s\"", text, w.text);
    CHECK_EQ(w.rights, 0);
    return true;
}

bool t_netlisten_wants(void)
{
    static const char *const listen[] = { "/svc/net", "/svc/net-listen", NULL };
    static const char *const net[] = { "/svc/net", NULL };
    CHECK(parsed("svc net listen\n", listen, "net, accepting connections from the network"));
    CHECK(parsed("svc net\n", net, "net"));
    CHECK(parsed("svc net-listen\n", NULL, NULL));     /* only the long form */
    CHECK(parsed("svc dns listen\n", NULL, NULL));
    CHECK(parsed("svc net listen now\n", NULL, NULL));
    CHECK(parsed("svc net Listen\n", NULL, NULL));
    CHECK(parsed("svc net  listen\n", NULL, NULL));   /* words split at single spaces */
    CHECK(parsed("right listen\n", NULL, NULL));
    return true;
}
