/* utest: programs' sockets on their rings (<sockring.h>) against
 * bin/netstack over the fake driver (netdrv.h), and libos against a
 * netstack that lies:
 *
 *   netsock_shares         an ordinary program takes every opener, socket,
 *                          wait and ring byte it can: a system opener (the
 *                          network's own services, /svc/net-sys) still opens
 *                          sockets with big rings, waits, and has datagrams
 *                          both ways
 *   netsock_hostile_rings  a program writes garbage into its rings (counts
 *                          backwards, ahead, off a record's edge; records
 *                          too long, with flags; its rx ring's count ahead):
 *                          netstack counts it in the socket's status line,
 *                          sends nothing of it, and goes on serving another
 *                          program and pings; it can't shrink or copy the
 *                          VMO, and once it closes its socket netstack has
 *                          shrunk the VMO to nothing
 *   netsock_len_lies       libos's net_sock_take over a hand-made netstack
 *                          whose rx records say more bytes than a datagram
 *                          holds: skipped (counted), never read past
 *                          d->data; a good one after them is taken */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/net.h>
#include <idl/svc.h>
#include <jam/netdev.h>
#include <net.h>
#include <os.h>
#include <sockring.h>
#include "netdrv.h"
#include "netpkt.h"
#include "utest.h"

#define BIG (SOCKRING_MAX)   /* a ring size: 256 KiB */

static uint8_t f[NETDEV_FRAME_MAX];
static struct net_dgram dg;
static uint32_t txc = 0x60000000u;   /* our async txids, far from the kernel's */

static bool counters(struct net_counters *c)
{
    CHECK_ST(net_get_counters(netdrv_net(), c), OK);
    return true;
}

/* The next frame netstack sent: a UDP datagram to the peer from sport
 * with `data`. */
static bool sent_to_peer(uint32_t sport, const char *data)
{
    uint32_t n;
    const uint8_t *u;
    size_t un;
    CHECK_ST(netdrv_recv(f, &n, NETDRV_WAIT), OK);
    CHECK(pkt_is_ipv4(f, n, PEER_IP, 17, &u, &un));
    CHECK(pkt_get16(u) == sport && un == 8 + strlen(data) && !memcmp(u + 8, data, un - 8));
    return true;
}

/* ---- fair shares ------------------------------------------------------------------- */

struct taker {
    handle_t o[NET_PROG_OPENERS];
    struct net_sock s[NET_PROG_SOCKETS];
    unsigned no, ns;
};

/* Ring bytes: big rings until an opener's and then the class's bytes run
 * out; all closed again after. */
static bool take_ring_bytes(struct taker *t)
{
    struct net_sock big[24];
    unsigned n = 0, per = SOCKRING_OPENER_BYTES / (unsigned)sockring_bytes(BIG, BIG);
    status_t st = OK;
    for (unsigned i = 0; st == OK && i < t->no; i++)
        for (unsigned k = 0; st == OK && k <= per && n < 24; k++) {
            st = net_udp_open_rings(t->o[i], 0, BIG, BIG, &big[n]);
            if (st == OK)
                n++;
            else if (k == per)
                st = OK;   /* the opener's 2 MiB: the next opener */
        }
    CHECK_ST(st, ERR_NO_RESOURCES);   /* the class's bytes ran out first */
    CHECK_EQ(n * sockring_bytes(BIG, BIG) <= NET_PROG_RING_BYTES, 1);
    CHECK((n + 1) * sockring_bytes(BIG, BIG) > NET_PROG_RING_BYTES);
    for (unsigned i = 0; i < n; i++)
        net_close(&big[i]);
    return true;
}

/* Every ordinary opener, socket and wait it can get. */
static bool take_everything(struct taker *t)
{
    handle_t x;
    struct net_counters c;
    while (t->no < NET_PROG_OPENERS &&
           svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &t->o[t->no]) == OK)
        t->no++;
    CHECK_EQ(t->no, NET_PROG_OPENERS);
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &x), ERR_NO_RESOURCES);
    CHECK(take_ring_bytes(t));
    for (unsigned i = 0; i < t->no && t->ns < NET_PROG_SOCKETS; i++)
        while (t->ns < NET_PROG_SOCKETS && net_udp_open(t->o[i], 0, &t->s[t->ns]) == OK)
            t->ns++;
    CHECK_EQ(t->ns, NET_PROG_SOCKETS);
    struct net_info in;
    CHECK_ST(net_info(t->o[0], &in), OK);
    for (unsigned i = 0; i < t->no; i++)
        for (unsigned k = 0; k < NET_LATER_PER_OPENER; k++)
            CHECK_ST(net_wait_change_send(t->o[i], idl_txid_next(&txc), in.version,
                                          NET_WAIT_FOREVER), OK);
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counters(&c));
    } while (c.later < NET_PROG_LATER && now() < end);
    CHECK_EQ(c.later, NET_PROG_LATER);   /* the rest were refused */
    CHECK(c.refused_shares > 0);
    return true;
}

/* A system opener is served all the same: sockets, big rings, a wait, and
 * datagrams both ways. */
static bool system_served(void)
{
    handle_t sys;
    struct net_sock a, b;
    struct net_counters c0, c;
    CHECK_ST(svc_connect_until(netdrv_net_sys(), now() + NETDRV_WAIT, &sys), OK);
    CHECK_ST(net_udp_open(sys, 0, &a), OK);
    CHECK_ST(net_udp_open_rings(sys, 0, BIG, BIG, &b), OK);
    CHECK(counters(&c0));
    struct net_info in;
    CHECK_ST(net_info(sys, &in), OK);
    CHECK_ST(net_wait_change_send(sys, idl_txid_next(&txc), in.version, NET_WAIT_FOREVER), OK);
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counters(&c));
    } while (c.later == c0.later && now() < end);
    CHECK_EQ(c.later, c0.later + 1);
    CHECK_ST(net_sendto(&a, PEER_IP, 53, "query", 5), OK);
    CHECK(sent_to_peer(a.port, "query"));
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 53, OUR_IP, a.port, "answer", 6)));
    CHECK_ST(net_recvfrom(&a, &dg, now() + NETDRV_WAIT), OK);
    CHECK(dg.len == 6 && !memcmp(dg.data, "answer", 6));
    net_close(&a);
    net_close(&b);
    jam_handle_close(sys);
    return true;
}

bool t_netsock_shares(void)
{
    static struct taker t;
    t = (struct taker){ 0 };
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));
    bool ok = take_everything(&t) && system_served();
    for (unsigned i = 0; i < t.ns; i++)
        net_close(&t.s[i]);
    for (unsigned i = 0; i < t.no; i++)
        jam_handle_close(t.o[i]);
    CHECK(ok);
    CHECK(netdrv_stop());
    return true;
}

/* ---- a hostile program's rings -------------------------------------------------------- */

/* Kick netstack to look at s's tx ring. */
static void kick(const struct net_sock *s)
{
    (void)jam_event_signal(s->to_stack, 0, SOCKRING_SIG_TX);   /* a test's own: can't fail */
}

/* Publish a hand-written tx count (and flags) on h and kick netstack. */
static void scribble(struct net_sock *h, uint64_t count, uint32_t flags)
{
    struct sockring_page *pg = (struct sockring_page *)h->map;
    __atomic_store_n(&pg->tx_prod.flags, flags, __ATOMIC_RELEASE);
    __atomic_store_n(&pg->tx_prod.count, count, __ATOMIC_RELEASE);
    kick(h);
}

/* A record header by hand at byte `at` of h's tx ring. */
static void raw_record(struct net_sock *h, uint64_t at, uint32_t len, uint32_t flags)
{
    struct sockring_dgram r = { .addr = PEER_IP, .port = 9, .len = (uint16_t)len, .flags = flags };
    memcpy(h->r.tx.data + (at & (h->r.tx.size - 1)), &r, sizeof(r));
}

/* The other program is served promptly, and pings answered. */
static bool others_served(struct net_sock *g, uint32_t seq)
{
    uint64_t t0 = now();
    CHECK_ST(net_sendto(g, PEER_IP, 9, "fine", 4), OK);
    CHECK(sent_to_peer(g->port, "fine"));
    CHECK(now() - t0 < 500 * NS_PER_MS);
    CHECK(netdrv_ping(seq, false));
    return true;
}

/* The status line of h says ring errors (netstack writes it in its own time). */
static bool errors_noted(struct net_sock *h, uint64_t more_than)
{
    struct sockring_status st;
    uint64_t end = now() + NETDRV_WAIT;
    do {
        sockring_status_get(&h->r, &st);
    } while (st.ring_errors <= more_than && now() < end);
    CHECK(st.ring_errors > more_than);
    return true;
}

static bool garbage_rings(struct net_sock *h, struct net_sock *g)
{
    struct sockring_status st;
    uint64_t c = 0;   /* netstack's count of h's tx ring: it starts at 0 */
    scribble(h, UINT64_MAX, 0);                  /* far ahead */
    CHECK(errors_noted(h, 0));
    CHECK(others_served(g, 2));
    sockring_status_get(&h->r, &st);
    c = __atomic_load_n(&((struct sockring_page *)h->map)->tx_cons.count, __ATOMIC_ACQUIRE);
    scribble(h, c - 16, 0);                      /* backwards */
    CHECK(errors_noted(h, st.ring_errors));
    sockring_status_get(&h->r, &st);
    scribble(h, c + 40, 0x80);                   /* off a record's edge, an unknown flag */
    CHECK(errors_noted(h, st.ring_errors));
    static const uint32_t lens[] = { SOCKRING_DGRAM_MAX + 1, 0xffff, 20 };
    for (unsigned i = 0; i < 3; i++) {
        c = __atomic_load_n(&((struct sockring_page *)h->map)->tx_cons.count, __ATOMIC_ACQUIRE);
        sockring_status_get(&h->r, &st);
        raw_record(h, c, lens[i], i == 2);           /* too long, or with flags */
        scribble(h, c + 64, 0);
        CHECK(errors_noted(h, st.ring_errors));
    }
    CHECK(others_served(g, 3));
    CHECK_ST(netdrv_recv(f, &(uint32_t){ 0 }, NETDRV_QUIET), ERR_TIMED_OUT);   /* none of it sent */
    return true;
}

/* Its rx ring's count ahead of what netstack put there: no room, the
 * datagram dropped and counted, netstack unharmed. */
static bool garbage_rx(struct net_sock *h, struct net_sock *g)
{
    struct sockring_page *pg = (struct sockring_page *)h->map;
    __atomic_store_n(&pg->rx_cons.count, 1u << 30, __ATOMIC_RELEASE);
    CHECK(netdrv_send(f, pkt_udp_from(f, pkt_peer_mac, PEER_IP, 9, OUR_IP, h->port, "x", 1)));
    struct sockring_status st;
    uint64_t end = now() + NETDRV_WAIT;
    do {
        sockring_status_get(&h->r, &st);
    } while (!st.rx_dropped && now() < end);
    CHECK_EQ(st.rx_dropped, 1);
    CHECK(others_served(g, 4));
    return true;
}

/* What the program's VMO handle can't do; then, its socket closed, the
 * VMO it still holds has been shrunk to nothing by netstack. */
static bool vmo_rules(struct net_sock *h)
{
    handle_t dup = HANDLE_INVALID;
    CHECK_ST(jam_vmo_set_size(h->ring, 0), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_duplicate(h->ring, RIGHT_SAME, &dup), ERR_ACCESS_DENIED);
    handle_t ring = h->ring;
    h->ring = HANDLE_INVALID;   /* kept back from net_close */
    net_close(h);
    uint64_t size = 1;
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK_ST(jam_vmo_get_size(ring, &size), OK);
    } while (size && now() < end);
    CHECK_EQ(size, 0);
    jam_handle_close(ring);
    return true;
}

bool t_netsock_hostile_rings(void)
{
    handle_t o, o2;
    struct net_sock h, g;
    CHECK(netdrv_start());
    CHECK(netdrv_ping(1, true));
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &o), OK);
    CHECK_ST(svc_connect_until(netdrv_net(), now() + NETDRV_WAIT, &o2), OK);
    CHECK_ST(net_udp_open(o, 0, &h), OK);
    CHECK_ST(net_udp_open(o2, 0, &g), OK);
    bool ok = garbage_rings(&h, &g) && garbage_rx(&h, &g) && vmo_rules(&h);
    net_close(&h);
    net_close(&g);
    jam_handle_close(o);
    jam_handle_close(o2);
    CHECK(ok);
    struct net_counters c;
    uint64_t end = now() + NETDRV_WAIT;
    do {
        CHECK(counters(&c));
    } while ((c.sockets || c.ring_bytes) && now() < end);
    CHECK(!c.sockets && !c.ring_bytes);
    CHECK(netdrv_stop());
    return true;
}

/* ---- a netstack that lies -------------------------------------------------------------- */

/* The hand-made netstack: one socket's rings, served by a thread for the
 * one sock_rings call net_sock_adopt makes. */
static struct {
    handle_t far, vmo, to_stack, to_prog;
    uint8_t *map;
    struct sockring r;
    status_t st;
} L;

static status_t l_rings(void *ctx, uint32_t tx, uint32_t rx, handle_t *out_ring,
                        handle_t *out_to_stack, handle_t *out_to_prog, uint32_t *out_tx,
                        uint32_t *out_rx)
{
    (void)ctx, (void)tx, (void)rx;
    uint64_t va = 0, bytes = sockring_bytes(SOCKRING_MIN, SOCKRING_MIN);
    status_t st = jam_vmo_create(bytes, 0, HANDLE_INVALID, &L.vmo);
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), L.vmo, 0, bytes, VMAR_READ | VMAR_WRITE,
                          &va);
    L.map = (uint8_t *)(uintptr_t)va;
    if (st == OK)
        st = sockring_make(&L.r, L.map, SOCKRING_DGRAM, SOCKRING_MIN, SOCKRING_MIN);
    if (st == OK)
        st = jam_event_create(&L.to_stack);
    if (st == OK)
        st = jam_event_create(&L.to_prog);
    if (st == OK)
        st = jam_handle_duplicate(L.vmo, SOCKRING_VMO_RIGHTS, out_ring);
    if (st == OK)
        st = jam_handle_duplicate(L.to_stack, SOCKRING_TO_STACK_RIGHTS, out_to_stack);
    if (st == OK)
        st = jam_handle_duplicate(L.to_prog, SOCKRING_TO_PROG_RIGHTS, out_to_prog);
    *out_tx = *out_rx = SOCKRING_MIN;
    return st;   /* a failure fails the test: what was made goes with utest */
}

static void l_serve(void *arg)
{
    (void)arg;
    static const struct net_ops ops = { .sock_rings = l_rings };
    signals_t seen;
    L.st = jam_object_wait_one(L.far, SIG_READABLE, now() + NETDRV_WAIT, &seen);
    if (L.st == OK)
        L.st = net_serve_one(L.far, &ops, NULL);
}

/* A record of len bytes (the header saying `say`) into the rx ring. */
static void lie(uint32_t say, uint32_t len)
{
    uint64_t at = L.r.rx.count;
    struct sockring_dgram h = { .addr = PEER_IP, .port = 4000, .len = (uint16_t)len };
    (void)sockring_dgram_put(&L.r.rx, &h, f);   /* room: the test's ring is empty */
    h.len = (uint16_t)say;
    memcpy(L.r.rx.data + (at & (L.r.rx.size - 1)), &h, sizeof(h));
    if (say > 0xffff)   /* more than the field holds: its bytes all ones */
        memset(L.r.rx.data + (at & (L.r.rx.size - 1)) + 6, 0xff, 2);
    (void)sockring_publish(&L.r.rx);
}

bool t_netsock_len_lies(void)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    handle_t near, th;
    struct net_sock s;
    L = (typeof(L)){ 0 };
    CHECK_ST(jam_channel_create(&near, &L.far), OK);
    CHECK_ST(thread_spawn("liar", l_serve, NULL, stack, sizeof(stack), &th), OK);
    status_t st = net_sock_adopt(&s, near, 7000);
    CHECK(wait_threads(&th, 1));
    CHECK_ST(L.st, OK);
    CHECK_ST(st, OK);
    lie(NET_DGRAM_MAX + 1, 16);
    CHECK_ST(net_sock_take(&s, &dg), ERR_SHOULD_WAIT);   /* skipped, never read */
    CHECK(s.r.rx.errors > 0);
    lie(NET_DGRAM_MAX, NET_DGRAM_MAX);
    CHECK_ST(net_sock_take(&s, &dg), OK);
    CHECK_EQ(dg.len, NET_DGRAM_MAX);
    lie(0x10000, 16);
    CHECK_ST(net_sock_take(&s, &dg), ERR_SHOULD_WAIT);
    net_close(&s);
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)L.map,
                         sockring_bytes(SOCKRING_MIN, SOCKRING_MIN));   /* ours */
    handle_t hs[] = { L.far, L.vmo, L.to_stack, L.to_prog };
    for (unsigned i = 0; i < 4; i++)
        jam_handle_close(hs[i]);
    return true;
}
