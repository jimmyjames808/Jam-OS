/* speed -u: UDP datagrams from here to the Mac for a few seconds, as fast
 * as netstack takes them (the socket's tx ring kept full, waiting in a
 * wait set for room), then the end said a few times until the Mac's
 * report comes: how many arrived, so how many were lost on the way. */
#include <ipv4.h>
#include <netwait.h>
#include "speed.h"

#define TX_RING  (256u * 1024)
#define ENDS     10u                     /* end datagrams, at most, ... */
#define END_GAP  (200 * NS_PER_MS)       /* ... this far apart */

static struct netwait *w;
static uint32_t sock_id;

/* Wait for the socket (interest), until the deadline. */
static status_t wait_sock(uint32_t interest, uint64_t deadline)
{
    struct netwait_ready r[2];
    uint32_t n;
    status_t st = netwait_modify(w, sock_id, interest);
    if (st == OK)
        st = netwait_wait(w, deadline, r, 2, &n);
    if (st == OK && (r[0].ready & NETWAIT_ERROR))
        st = r[0].error;
    return st;
}

/* Datagrams for `seconds`: how many went into the ring. */
static status_t blast(struct net_sock *s, uint32_t addr, uint16_t port, uint32_t run,
                      uint32_t seconds, uint32_t *sent, uint64_t *ns)
{
    static uint8_t d[SPEED_DGRAM];
    memset(d, 0x5a, sizeof(d));
    memcpy(d, "JSPU", 4);
    put32(d + 4, run);
    uint64_t t0 = now(), end = t0 + (uint64_t)seconds * NS_PER_S;
    status_t st = OK;
    *sent = 0;
    while (st == OK && now() < end) {   /* each turn puts one, or waits for room */
        put32(d + 8, *sent);
        st = net_sendto_async(s, addr, port, d, sizeof(d));
        if (st == OK)
            ++*sent;
        else if (st == ERR_SHOULD_WAIT)
            st = wait_sock(NETWAIT_WRITE, end);
    }
    *ns = now() - t0;
    return st == ERR_TIMED_OUT ? OK : st;
}

/* The end, until the Mac's report for run comes. */
static status_t report(struct net_sock *s, uint32_t addr, uint16_t port, uint32_t run,
                       uint32_t sent, uint8_t out[24])
{
    uint8_t e[16] = { 'J', 'S', 'P', 'E' };
    put32(e + 4, run);
    put32(e + 8, sent);
    for (unsigned k = 0; k < ENDS; k++) {   /* each try sends the end, waits END_GAP */
        status_t st = net_sendto_async(s, addr, port, e, sizeof(e));
        if (st != OK && st != ERR_SHOULD_WAIT)
            return st;
        uint64_t until = now() + END_GAP;
        while (wait_sock(NETWAIT_READ, until) == OK) {   /* each turn takes one */
            struct net_dgram d;
            if (net_sock_take(s, &d) != OK)
                continue;
            if (d.len >= 24 && !memcmp(d.data, "JSPR", 4) && get32(d.data + 4) == run) {
                memcpy(out, d.data, 24);
                return OK;
            }
        }
    }
    return ERR_TIMED_OUT;
}

int udp_client(uint32_t addr, uint16_t port, uint32_t seconds)
{
    struct net_sock s;
    char a[IPV4_TEXT_MAX];
    uint32_t run = os_random_u32(), sent = 0;
    uint64_t ns = 0;
    status_t st = net_wait_up(net_svc(), now() + SPEED_WAIT, NULL);
    if (st == OK)
        st = net_udp_open_rings(net_svc(), 0, TX_RING, 0, &s);
    if (st != OK) {
        printf("speed: no UDP socket: %s\n", status_str(st));
        return 1;
    }
    struct netwait_sock ws;
    net_sock_waitable(&s, &ws);
    if ((st = netwait_create(2, &w)) == OK)
        st = netwait_add_sock(w, &ws, 0, NULL, &sock_id);
    printf("speed: sending UDP to %s port %u for %u s\n", ipv4_format(addr, a), port, seconds);
    if (st == OK)
        st = blast(&s, addr, port, run, seconds, &sent, &ns);
    uint8_t rep[24];
    if (st == OK)
        say_rate("sent", (uint64_t)sent * SPEED_DGRAM, ns);
    if (st == OK && (st = report(&s, addr, port, run, sent, rep)) != OK)
        printf("speed: no report from the Mac (is `python3 tools/speed.py server` running?)\n");
    if (st == OK) {
        uint32_t got = get32(rep + 8);
        printf("speed: %u datagrams sent, %u arrived, %u lost (%u%%)\n", sent, got,
               got < sent ? sent - got : 0, sent ? (sent - (got < sent ? got : sent)) * 100 / sent
                                                 : 0);
        say_rate("the Mac got", get64(rep + 16), (uint64_t)get32(rep + 12) * NS_PER_MS);
    }
    netwait_destroy(w);
    net_close(&s);
    if (st != OK)
        printf("speed: FAILED: %s\n", status_str(st));
    return st == OK ? 0 : 1;
}
