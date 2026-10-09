/* tcptest small and keepalive: a connection's options (net_tcp_set_options)
 * seen from the peer (tcptest.h). The peer is a server that reads its
 * bytes to our FIN and sends as many back (tools/tcppeer.py). */
#include <net.h>
#include <os.h>
#include "tcptest.h"

#define STEP (20 * NS_PER_MS)   /* between two small writes */

static status_t open_with(uint32_t addr, uint16_t port, uint32_t flags, uint32_t idle,
                          struct net_sock *s, uint64_t deadline)
{
    status_t st = net_wait_up(net_svc(), deadline, NULL);
    if (st == OK)
        st = net_tcp_connect(net_svc(), addr, port, deadline, s);
    if (st == OK && (st = net_tcp_set_options(s, flags, idle, idle, 3)) != OK)
        net_close(s);
    return st;
}

/* The peer's bytes back to its end, every one checked: how many. */
static status_t read_back(struct net_sock *s, uint64_t deadline, uint64_t *got)
{
    static uint8_t buf[256];
    *got = 0;
    for (;;) {   /* each turn reads bytes, or ends */
        size_t n;
        status_t st = net_read(s, buf, sizeof(buf), deadline, &n);
        if (st != OK || !n)
            return st;
        if (!tt_same(0xb2, *got, buf, n))
            return ERR_INTERNAL;
        *got += n;
    }
}

int tt_small(uint32_t addr, uint16_t port, uint32_t writes, uint32_t size, bool nagle)
{
    struct net_sock s = { 0 };
    uint8_t buf[256];
    uint64_t deadline = now() + TT_WAIT, got = 0;
    if (!size || size > sizeof(buf) || !writes)
        return 2;
    status_t st = open_with(addr, port, nagle ? 0 : NET_STREAM_OPT_NODELAY, 0, &s, deadline);
    for (uint32_t i = 0; st == OK && i < writes; i++) {
        tt_fill(0xa1, (uint64_t)i * size, buf, size);
        st = net_write(&s, buf, size, deadline, NULL);
        (void)jam_sleep_until(now() + STEP);   /* the peer's ACK comes later still */
    }
    if (st == OK)
        st = net_shutdown(&s);
    if (st == OK)
        st = read_back(&s, deadline, &got);
    if (st == OK && got != (uint64_t)writes * size)
        st = ERR_OUT_OF_RANGE;
    if (st == OK)
        st = net_tcp_wait_closed(&s, deadline);
    if (s.ch)
        net_close(&s);
    if (st != OK) {
        printf("tcptest: FAIL: small: %s\n", status_str(st));
        return 1;
    }
    printf("tcptest: PASS: %u writes of %u bytes, %s\n", writes, size,
           nagle ? "Nagle on" : "no delay");
    return 0;
}

int tt_keepalive(uint32_t addr, uint16_t port, uint32_t hold_s)
{
    struct net_sock s = { 0 };
    uint8_t buf[64];
    uint64_t deadline = now() + TT_WAIT, got = 0;
    status_t st = open_with(addr, port, NET_STREAM_OPT_NODELAY | NET_STREAM_OPT_KEEPALIVE, 1, &s,
                            deadline);
    if (st == OK)   /* idle: the peer is probed every second and answers */
        (void)jam_sleep_until(now() + (uint64_t)hold_s * NS_PER_S);
    if (st == OK) {
        uint32_t state;
        status_t err;
        net_tcp_status(&s, &state, &err);
        if (state != SOCKRING_STATE_OPEN)
            st = err != OK ? err : ERR_BAD_STATE;
    }
    if (st == OK) {   /* still there: carry bytes both ways */
        tt_fill(0xa1, 0, buf, sizeof(buf));
        st = net_write(&s, buf, sizeof(buf), deadline, NULL);
    }
    if (st == OK)
        st = net_shutdown(&s);
    if (st == OK)
        st = read_back(&s, deadline, &got);
    if (st == OK && got != sizeof(buf))
        st = ERR_OUT_OF_RANGE;
    if (st == OK)
        st = net_tcp_wait_closed(&s, deadline);
    if (s.ch)
        net_close(&s);
    if (st != OK) {
        printf("tcptest: FAIL: keepalive: %s\n", status_str(st));
        return 1;
    }
    printf("tcptest: PASS: idle %u s with keep-alive, still open\n", hold_s);
    return 0;
}

/* The peer's bytes of stream `seed` to its end, every one checked. */
static status_t read_stream(struct net_sock *s, uint32_t seed, uint64_t bytes, uint64_t deadline)
{
    static uint8_t buf[4096];
    uint64_t got = 0;
    for (;;) {   /* each turn reads bytes, or ends */
        size_t n;
        status_t st = net_read(s, buf, sizeof(buf), deadline, &n);
        if (st != OK)
            return st;
        if (!n)
            return got == bytes ? OK : ERR_OUT_OF_RANGE;
        if (got + n > bytes || !tt_same(seed, got, buf, n))
            return ERR_INTERNAL;
        got += n;
    }
}

/* Wait until netstack has no listener (a closed one goes in its own time). */
static status_t no_listeners(handle_t net, uint64_t deadline)
{
    struct net_counters c;
    for (;;) {   /* each turn looks, or sleeps 10 ms */
        status_t st = net_get_counters(net, &c);
        if (st != OK || !c.tcp_listeners)
            return st;
        if (now() >= deadline)
            return ERR_TIMED_OUT;
        (void)jam_sleep_until(now() + 10 * NS_PER_MS);
    }
}

int tt_relisten(uint16_t port, uint64_t bytes)
{
    static uint8_t buf[4096];
    struct net_listener l = { 0 }, plain = { 0 }, again = { 0 };
    struct net_sock s = { 0 };
    uint16_t peer_port = 0;
    uint64_t deadline = now() + TT_WAIT;
    handle_t net = svc_get(SVC_NET_LISTEN);
    status_t st = net ? net_wait_up(net, deadline, NULL) : ERR_NOT_FOUND;
    if (st == OK)
        st = net_tcp_listen_flags(net, port, 1, 16384, 16384, NET_LISTEN_REUSE, &l);
    if (st == OK)
        st = net_tcp_accept(&l, deadline, &s, NULL, &peer_port);
    /* Our bytes and our FIN first, so that the connection ends in TIME_WAIT on
     * our side (the peer's own FIN waits for the window we don't open before
     * we have written): then its bytes to its end. */
    for (uint64_t at = 0; st == OK && at < bytes;) {
        size_t n = bytes - at < sizeof(buf) ? (size_t)(bytes - at) : sizeof(buf);
        tt_fill((peer_port + 1u) & 0xff, at, buf, n);
        st = net_write(&s, buf, n, deadline, NULL);
        at += n;
    }
    if (st == OK)
        st = net_shutdown(&s);
    if (st == OK)
        st = read_stream(&s, peer_port & 0xffu, bytes, deadline);
    if (st == OK)
        st = net_tcp_wait_closed(&s, deadline);
    net_close(&s);
    net_listener_close(&l);
    if (st == OK)
        st = no_listeners(net, deadline);
    /* The port is held by the connection's TIME_WAIT: a listener without the
     * flag is refused, one with it is made at once. */
    if (st == OK) {
        status_t refused = net_tcp_listen_flags(net, port, 1, 0, 0, 0, &plain);
        net_listener_close(&plain);   /* (nothing to close when it was refused) */
        if (refused != ERR_ALREADY_BOUND)
            st = refused == OK ? ERR_BAD_STATE : refused;
    }
    if (st == OK)
        st = net_tcp_listen_flags(net, port, 1, 0, 0, NET_LISTEN_REUSE, &again);
    net_listener_close(&again);
    if (st != OK) {
        printf("tcptest: FAIL: relisten: %s\n", status_str(st));
        return 1;
    }
    printf("tcptest: PASS: port %u listened on again at once with address reuse\n", port);
    return 0;
}
