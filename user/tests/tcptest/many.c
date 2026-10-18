/* tcptest many: a program's connections at netstack's caps (net.h's
 * NET_TCP_PER_OPENER and NET_PROG_TCP), all open at once to one of the
 * peer's servers, then carrying bytes. */
#include <net.h>
#include <os.h>
#include "tcptest.h"

static struct net_sock s[NET_TCP_MAX];

static void close_all(unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (s[i].ch)
            net_close(&s[i]);
}

/* Every connection has carried its bytes out and in, checked: the peer's
 * server (`--tcp-serve PORT:BYTES`) reads the bytes of stream 0xa1 to our
 * FIN, then sends the bytes of stream 0xb2 and its own FIN. */
static status_t carry(unsigned n, uint64_t bytes, uint64_t deadline)
{
    static uint8_t buf[256];
    for (unsigned i = 0; i < n; i++) {
        for (uint64_t at = 0; at < bytes;) {
            size_t k = bytes - at < sizeof(buf) ? (size_t)(bytes - at) : sizeof(buf);
            tt_fill(0xa1, at, buf, k);
            status_t st = net_write(&s[i], buf, k, deadline, NULL);
            if (st != OK)
                return st;
            at += k;
        }
        status_t st = net_shutdown(&s[i]);
        if (st != OK)
            return st;
    }
    for (unsigned i = 0; i < n; i++) {
        uint64_t got = 0;
        for (;;) {   /* each turn reads bytes, or ends */
            size_t k;
            status_t st = net_read(&s[i], buf, sizeof(buf), deadline, &k);
            if (st != OK)
                return st;
            if (!k)
                break;
            if (got + k > bytes || !tt_same(0xb2, got, buf, k))
                return ERR_INTERNAL;
            got += k;
        }
        if (got != bytes)
            return ERR_OUT_OF_RANGE;
        status_t st = net_tcp_wait_closed(&s[i], deadline);
        if (st != OK)
            return st;
    }
    return OK;
}

/* "tcptest: <n> connections <what>" and netstack's counts now. */
static void say(handle_t net, const char *what, unsigned n, uint64_t ns, bool full)
{
    struct net_counters c;
    status_t cs = net_get_counters(net, &c);
    printf("tcptest: %u connections %s", n, what);
    if (ns)
        printf(" in %lu ms", (unsigned long)(ns / NS_PER_MS));
    printf("%s; netstack: %u connections, ring bytes %u, heap used %u, rx buffers used %u\n",
           full ? ", the next refused" : "", cs == OK ? c.tcp_conns : 0,
           cs == OK ? c.ring_bytes : 0, cs == OK ? c.heap_used : 0,
           cs == OK ? c.rx_buffers_used : 0);
}

int tt_many(uint32_t addr, uint16_t port, uint32_t n, uint64_t bytes, uint32_t hold_s, bool full)
{
    uint64_t deadline = now() + TT_WAIT;
    handle_t net = net_svc();
    status_t st = net ? net_wait_up(net, deadline, NULL) : ERR_NOT_FOUND;
    unsigned opened = 0;
    if (st == OK && (!n || n > NET_TCP_MAX))
        st = ERR_INVALID_ARGS;
    uint64_t t0 = now();
    for (; st == OK && opened < n; opened++)   /* the handshakes go on while the rest are asked for */
        st = net_tcp_open(net, addr, port, 0, 0, &s[opened]);
    if (st != OK)
        printf("tcptest: connection %u: %s\n", opened, status_str(st));
    for (unsigned i = 0; st == OK && i < n; i++)
        st = net_tcp_wait_open(&s[i], deadline);
    if (st == OK && full) {   /* the next one is over a cap: refused */
        struct net_sock x;
        status_t over = net_tcp_open(net, addr, port, 0, 0, &x);
        if (over == OK)
            net_close(&x);
        if (over != ERR_NO_RESOURCES) {
            printf("tcptest: connection %u: %s, not refused\n", n + 1, status_str(over));
            st = over == OK ? ERR_BAD_STATE : over;
        }
    }
    if (st == OK) {
        say(net, "open", n, now() - t0, full);
        (void)jam_sleep_until(now() + (uint64_t)hold_s * NS_PER_S);   /* held, for the shell to look */
        for (unsigned i = 0; st == OK && i < n; i++) {
            uint32_t state;
            status_t err;
            net_tcp_status(&s[i], &state, &err);
            if (state != SOCKRING_STATE_OPEN)
                st = err != OK ? err : ERR_BAD_STATE;
        }
    }
    if (st == OK)   /* the hold is not part of the wait */
        st = carry(n, bytes, now() + TT_WAIT);
    if (st == OK) {   /* every ring has been used by now: closed, but not given back yet */
        say(net, "carried their bytes", n, 0, false);
        (void)jam_sleep_until(now() + (uint64_t)hold_s * NS_PER_S);
    }
    close_all(opened);
    if (st != OK) {
        printf("tcptest: FAIL: many: %s\n", status_str(st));
        return 1;
    }
    printf("tcptest: PASS: %u connections held %u s twice and carried %lu bytes each way each\n",
           n, hold_s, (unsigned long)bytes);
    return 0;
}
