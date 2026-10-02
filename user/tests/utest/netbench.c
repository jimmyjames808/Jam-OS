/* utest: how fast datagrams go through a program's socket, bin/netstack
 * and the card's rings (netdrv.h's fake driver), for BENCH.md. The test
 * is the program and the card: every datagram the socket sends comes out
 * of netstack's tx ring as a frame, which the test sends back into the rx
 * ring as the peer's answer, which the socket receives. So each datagram
 * crosses netstack twice, and the test's own work as the card is in the
 * time too (the same before and after).
 *
 *   round trip   one datagram at a time: the blocking net_sendto, the
 *                frame back, the blocking net_recvfrom
 *   stream       BENCH_WIN datagrams in flight with the forms that don't
 *                wait: datagrams a second each way, and the time per one
 *
 * Which CPUs: the test and netstack are two processes; with one CPU they
 * share it, with more the scheduler usually puts them on two (QEMU runs at
 * -smp 1 and 2 give the two cases). The numbers are printed; the test
 * fails only if a datagram is lost or wrong. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/svc.h>
#include <jam/netdev.h>
#include <net.h>
#include <os.h>
#include "netdrv.h"
#include "netpkt.h"
#include "utest.h"

#define BENCH_RTT   400u    /* round trips */
#define BENCH_N     3000u   /* datagrams a stream */
#define BENCH_WIN   16u     /* in flight */
#define PEER_PORT   7000u

static uint8_t bf[NETDEV_FRAME_MAX], bg[NETDEV_FRAME_MAX], payload[NET_DGRAM_MAX];
static struct net_dgram bd;

/* The frame netstack sent for the socket, back as the peer's answer. */
static bool reflect(const struct net_sock *s, size_t size)
{
    uint32_t n = 0;
    CHECK_ST(netdrv_recv(bf, &n, NETDRV_WAIT), OK);
    CHECK_EQ(n, size + 42 < 60 ? 60 : size + 42);
    CHECK(netdrv_send(bg, pkt_udp_from(bg, pkt_peer_mac, PEER_IP, PEER_PORT, OUR_IP, s->port,
                                       bf + 42, size)));
    return true;
}

static bool round_trips(struct net_sock *s, size_t size, uint64_t *ns)
{
    uint64_t t0 = now();
    for (unsigned i = 0; i < BENCH_RTT; i++) {
        payload[0] = (uint8_t)i;
        CHECK_ST(net_sendto(s, PEER_IP, PEER_PORT, payload, size), OK);
        CHECK(reflect(s, size));
        CHECK_ST(net_recvfrom(s, &bd, now() + NETDRV_WAIT), OK);
        CHECK(bd.len == size && bd.data[0] == (uint8_t)i);
    }
    *ns = (now() - t0) / BENCH_RTT;
    return true;
}

static bool stream(struct net_sock *s, size_t size, uint64_t *ns)
{
    uint32_t sent = 0, got = 0;
    uint64_t t0 = now();
    while (got < BENCH_N) {
        uint32_t burst = 0;
        for (; burst < BENCH_WIN && sent < BENCH_N; burst++, sent++) {
            payload[0] = (uint8_t)sent;
            CHECK_ST(net_sendto_async(s, PEER_IP, PEER_PORT, payload, size), OK);
        }
        for (uint32_t k = 0; k < burst; k++)
            CHECK(reflect(s, size));
        while (got < sent) {
            status_t st = net_sock_take(s, &bd);
            if (st == ERR_SHOULD_WAIT) {
                CHECK_ST(net_sock_wait(s, now() + NETDRV_WAIT), OK);
                continue;
            }
            CHECK_ST(st, OK);
            CHECK(bd.len == size && bd.data[0] == (uint8_t)got);
            got++;
        }
    }
    *ns = (now() - t0) / BENCH_N;
    return true;
}

static void say(const char *what, size_t size, uint64_t ns)
{
    printf("utest: netsock_bench: %s, %u bytes: %lu.%02lu us a datagram (%lu a second each "
           "way)\n", what, (unsigned)size, (unsigned long)(ns / 1000),
           (unsigned long)(ns % 1000 / 10), (unsigned long)(ns ? NS_PER_S / ns : 0));
}

bool t_netsock_bench(void)
{
    handle_t o;
    struct net_sock a, b;
    uint64_t ns;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));   /* netstack learns the peer's MAC */
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &o), OK);
    CHECK_ST(net_udp_open(o, 6000, &a), OK);
    CHECK_ST(net_udp_open(o, 6001, &b), OK);
    CHECK_ST(net_recv_arm(&b), OK);
    static const size_t sizes[] = { 64, NET_DGRAM_MAX };
    for (unsigned i = 0; i < 2; i++) {
        CHECK(round_trips(&a, sizes[i], &ns));
        say("round trip", sizes[i], ns);
        CHECK(stream(&b, sizes[i], &ns));
        say("stream", sizes[i], ns);
    }
    net_close(&a);
    net_close(&b);
    jam_handle_close(o);
    return netdrv_stop();
}
