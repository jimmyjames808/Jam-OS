/* speed: TCP, both ways. One side sends for the hello's seconds and ends
 * its direction; the other counts to that end and answers with a report
 * (speed.h has the bytes). The sender's own figure is what went into its
 * socket; the receiver's is what arrived, from its first byte to the end. */
#include <ipv4.h>
#include "speed.h"

#define CHUNK   (64u * 1024)

static uint8_t buf[CHUNK];

/* Bytes for `seconds`, then our end; the receiver's report said. */
static status_t send_for(struct net_sock *s, uint32_t seconds)
{
    uint64_t t0 = now(), end = t0 + (uint64_t)seconds * NS_PER_S, sent = 0;
    status_t st = OK;
    while (st == OK && now() < end) {   /* each turn writes up to a chunk, by `end` */
        size_t n = 0;
        st = net_write(s, buf, sizeof(buf), end, &n);
        sent += n;
    }
    uint64_t t1 = now();
    if (st == ERR_TIMED_OUT)
        st = OK;   /* the time is up */
    if (st == OK)
        st = net_shutdown(s);
    if (st != OK) {
        printf("speed: sending: %s\n", status_str(st));
        return st;
    }
    say_rate("sent", sent, t1 - t0);
    uint8_t rep[SPEED_REPORT];
    size_t got = 0;
    while (st == OK && got < sizeof(rep)) {   /* each turn reads some of it */
        size_t n = 0;
        st = net_read(s, rep + got, sizeof(rep) - got, now() + SPEED_WAIT, &n);
        if (st == OK && !n)
            st = ERR_PEER_CLOSED;
        got += n;
    }
    if (st != OK || memcmp(rep, "JSPR", 4)) {
        printf("speed: no report from the other side (%s)\n", st != OK ? status_str(st)
                                                                       : "a wrong one");
        return st != OK ? st : ERR_INVALID_ARGS;
    }
    say_rate("the other side got", get64(rep + 8), (uint64_t)get32(rep + 4) * NS_PER_MS);
    return net_tcp_wait_closed(s, now() + SPEED_WAIT);
}

/* Bytes to the sender's end, counted; our report back. */
static status_t receive_all(struct net_sock *s, uint32_t seconds)
{
    uint64_t t0 = 0, t1 = 0, got = 0, limit = now() + (seconds + 20ull) * NS_PER_S;
    status_t st = OK;
    for (;;) {   /* each turn reads, by the idle deadline and the whole run's */
        size_t n = 0;
        uint64_t idle = now() + SPEED_WAIT;
        st = net_read(s, buf, sizeof(buf), idle < limit ? idle : limit, &n);
        if (st != OK || !n)
            break;
        if (!got)
            t0 = now();
        got += n;
        t1 = now();
    }
    if (st != OK) {
        printf("speed: receiving: %s (after %lu bytes)\n", status_str(st), (unsigned long)got);
        return st;
    }
    say_rate("received", got, t1 - t0);
    uint8_t rep[SPEED_REPORT];
    memcpy(rep, "JSPR", 4);
    put32(rep + 4, (uint32_t)((t1 - t0) / NS_PER_MS));
    put64(rep + 8, got);
    st = net_write(s, rep, sizeof(rep), now() + SPEED_WAIT, NULL);
    if (st == OK)
        st = net_shutdown(s);
    if (st == OK)
        st = net_tcp_wait_closed(s, now() + SPEED_WAIT);
    return st;
}

int tcp_client(uint32_t addr, uint16_t port, bool receive, uint32_t seconds)
{
    struct net_sock s;
    char a[IPV4_TEXT_MAX];
    status_t st = net_wait_up(net_svc(), now() + SPEED_WAIT, NULL);
    /* Bulk rings both ways: a whole scaled window to receive into, and as
     * much in flight when sending (<net.h> NET_TCP_BULK). */
    if (st == OK &&
        (st = net_tcp_open(net_svc(), addr, port, NET_TCP_BULK, NET_TCP_BULK, &s)) == OK &&
        (st = net_tcp_wait_open(&s, now() + SPEED_WAIT)) != OK)
        net_close(&s);
    ipv4_format(addr, a);
    if (st != OK) {
        printf("speed: %s port %u: %s\n", a, port,
               st == ERR_NOT_FOUND ? "refused (is `python3 tools/speed.py server` running?)"
                                   : status_str(st));
        return 1;
    }
    printf("speed: connected to %s port %u: %s for %u s over TCP\n", a, port,
           receive ? "receiving" : "sending", seconds);
    uint8_t hello[SPEED_HELLO] = { 'J', 'S', 'P', 'D', 1, receive ? 1 : 0 };
    put32(hello + 8, seconds);
    st = net_write(&s, hello, sizeof(hello), now() + SPEED_WAIT, NULL);
    if (st == OK)
        st = receive ? receive_all(&s, seconds) : send_for(&s, seconds);
    net_close(&s);
    if (st != OK)
        printf("speed: FAILED: %s\n", status_str(st));
    return st == OK ? 0 : 1;
}

/* One test from a connection the listener took. */
static void serve_one(struct net_sock *s, uint32_t peer)
{
    char a[IPV4_TEXT_MAX];
    uint8_t hello[SPEED_HELLO];
    size_t got = 0;
    status_t st = OK;
    while (st == OK && got < sizeof(hello)) {   /* each turn reads some of it */
        size_t n = 0;
        st = net_read(s, hello + got, sizeof(hello) - got, now() + SPEED_WAIT, &n);
        if (st == OK && !n)
            st = ERR_PEER_CLOSED;
        got += n;
    }
    uint32_t seconds = st == OK ? get32(hello + 8) : 0;
    if (st == OK && (memcmp(hello, "JSPD", 4) || hello[4] != 1 || hello[5] > 1 || !seconds ||
                     seconds > SPEED_MAX_S))
        st = ERR_INVALID_ARGS;
    if (st != OK) {
        printf("speed: %s: not a speed test (%s)\n", ipv4_format(peer, a), status_str(st));
        return;
    }
    printf("speed: %s: %s for %u s\n", ipv4_format(peer, a), hello[5] ? "sending" : "receiving",
           seconds);
    st = hello[5] ? send_for(s, seconds) : receive_all(s, seconds);
    if (st != OK)
        printf("speed: FAILED: %s\n", status_str(st));
}

int tcp_listen(uint16_t port)
{
    handle_t net = svc_get(SVC_NET_LISTEN);
    struct net_listener l;
    status_t st = net ? net_wait_up(net, now() + SPEED_WAIT, NULL) : ERR_ACCESS_DENIED;
    if (st == OK)
        st = net_tcp_listen(net, port, 4, NET_TCP_BULK, NET_TCP_BULK, &l);
    if (st != OK) {
        printf("speed: can't listen on port %u: %s\n", port,
               st == ERR_ACCESS_DENIED ? "no listen permission (`svc net listen`)"
                                       : status_str(st));
        return 1;
    }
    struct net_info i;
    char a[IPV4_TEXT_MAX];
    if (net_info(net, &i) != OK)
        i.address = 0;
    printf("speed: listening on %s port %u (on the Mac: python3 tools/speed.py client %s "
           "[-r]); Ctrl+C stops it\n", ipv4_format(i.address, a), l.port, a);
    for (;;) {   /* one test at a time, until Ctrl+C kills us */
        struct net_sock s;
        uint32_t peer = 0;
        st = net_tcp_accept(&l, DEADLINE_NEVER, &s, &peer, NULL);
        if (st != OK) {
            printf("speed: accept: %s\n", status_str(st));
            net_listener_close(&l);
            return 1;
        }
        serve_one(&s, peer);
        net_close(&s);
    }
}
