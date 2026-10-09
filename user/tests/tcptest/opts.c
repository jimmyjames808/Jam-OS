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
