/* utest: bin/dns's edge to the network (user/services/dns/socks.c), its
 * resolver driving it, against a fake netstack served in-process on a
 * channel pair (net.idl's generated server: udp_rings on the opener
 * channel; the socket's datagrams through the rings it hands out, read and
 * written here as netstack would, <sockring.h>).
 *
 * Covered: a socket's open is written without waiting, and the first
 * datagram waits for its answer; a port another program has
 * (ERR_ALREADY_BOUND, known only when the open is answered) makes the
 * next try pick another port, not fail again on the same one; the query
 * goes out to the server's port 53 once the socket opens; the server's
 * answer reaches the resolver through the loop's port and its socket is
 * closed once no query uses it; a port released while its open is still
 * in flight is closed when the open is answered, and its slot freed. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/net.h>
#include <os.h>
#include <sockring.h>
#include "dnsd.h"
#include "utest.h"

#define SERVER 0x0a021501u   /* 10.2.21.1 */
#define S      NS_PER_S

struct dnsd D;   /* bin/dns's state: its main.c isn't linked here */

/* The fake netstack. */
static struct {
    status_t        answer;      /* what the next udp_rings answers */
    unsigned        opens;
    uint16_t        ports[4];    /* the ports asked for */
    handle_t        far;         /* our end of the last socket handed out */
    handle_t        vmo, to_stack, to_prog;   /* its rings, all ours */
    uint8_t        *map;
    struct sockring r;           /* netstack's ends of them */
    unsigned        sends;
    uint32_t        to;
    uint16_t        to_port, len;
    uint8_t         sent[SOCKRING_DGRAM_MAX];
} F;

/* The last socket's rings, gone (the program keeps what it has). */
static void rings_drop(void)
{
    if (F.map)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)F.map,
                             sockring_bytes(SOCKRING_UDP_TX, SOCKRING_UDP_RX));   /* ours */
    handle_t hs[] = { F.vmo, F.to_stack, F.to_prog };
    for (unsigned k = 0; k < 3; k++)
        if (hs[k])
            jam_handle_close(hs[k]);
    F.vmo = F.to_stack = F.to_prog = HANDLE_INVALID;
    F.map = NULL;
}

/* New rings, and the program's handles to them (<sockring.h>'s rights). */
static status_t rings_new(handle_t hs[3])
{
    uint64_t va = 0, bytes = sockring_bytes(SOCKRING_UDP_TX, SOCKRING_UDP_RX);
    rings_drop();
    status_t st = jam_vmo_create(bytes, 0, HANDLE_INVALID, &F.vmo);
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), F.vmo, 0, bytes,
                          VMAR_READ | VMAR_WRITE, &va);
    F.map = (uint8_t *)(uintptr_t)va;
    if (st == OK)
        st = sockring_make(&F.r, F.map, SOCKRING_DGRAM, SOCKRING_UDP_TX, SOCKRING_UDP_RX);
    if (st == OK)
        st = jam_event_create(&F.to_stack);
    if (st == OK)
        st = jam_event_create(&F.to_prog);
    if (st == OK)
        st = jam_handle_duplicate(F.vmo, SOCKRING_VMO_RIGHTS, &hs[0]);
    if (st == OK)
        st = jam_handle_duplicate(F.to_stack, SOCKRING_TO_STACK_RIGHTS, &hs[1]);
    if (st == OK)
        st = jam_handle_duplicate(F.to_prog, SOCKRING_TO_PROG_RIGHTS, &hs[2]);
    return st;   /* a failure fails the test: what was made goes with utest */
}

static status_t f_udp_rings(void *ctx, uint16_t port, uint32_t tx, uint32_t rx, handle_t *out,
                            handle_t *out_ring, handle_t *out_to_stack, handle_t *out_to_prog,
                            uint16_t *out_port, uint32_t *out_tx, uint32_t *out_rx)
{
    (void)ctx, (void)tx, (void)rx;
    if (F.opens < 4)
        F.ports[F.opens] = port;
    F.opens++;
    if (F.answer != OK)
        return F.answer;
    handle_t near, hs[3] = { 0 };
    status_t st = jam_channel_create(&near, &F.far);
    if (st == OK)
        st = rings_new(hs);
    if (st != OK)
        return st;
    *out = near;
    *out_ring = hs[0];
    *out_to_stack = hs[1];
    *out_to_prog = hs[2];
    *out_port = port;
    *out_tx = SOCKRING_UDP_TX;
    *out_rx = SOCKRING_UDP_RX;
    return OK;
}

static const struct net_ops opener_ops = { .udp_rings = f_udp_rings };

/* What the program put in the tx ring, as netstack takes it. */
static void take_sent(void)
{
    struct sockring_dgram h;
    while (F.map && sockring_dgram_take(&F.r.tx, &h, F.sent) == OK) {
        F.sends++;
        F.to = h.addr;
        F.to_port = h.port;
        F.len = h.len;
    }
    if (F.map)
        (void)sockring_publish(&F.r.tx);
}

/* A datagram from the server into the rx ring, the program woken. */
static bool answer_in(const uint8_t *data, uint16_t n)
{
    struct sockring_dgram h = { .addr = SERVER, .port = DNS_PORT, .len = n };
    CHECK_ST(sockring_dgram_put(&F.r.rx, &h, data), OK);
    CHECK(sockring_publish(&F.r.rx));   /* bin/dns sleeps on it: its flag says so */
    CHECK_ST(jam_event_signal(F.to_prog, 0, SOCKRING_SIG_RX), OK);
    return true;
}

/* The fake netstack answers what is queued on its opener end; bin/dns's
 * loop hands the answer to socks.c, as its serve_net does. */
static bool open_round(handle_t fake_net)
{
    CHECK_ST(net_serve_one(fake_net, &opener_ops, NULL), OK);
    _Alignas(8) uint8_t rep[NET_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(D.net, rep, sizeof(rep), &m), OK);
    CHECK(socks_open_reply(rep, &m));
    return true;
}

static unsigned slots_used(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SOCK_SLOTS; i++)
        n += D.s[i].state != SOCK_FREE;
    return n;
}

/* The query F.sent answered: one A record, 10.9.0.1, TTL 60. */
static uint16_t answer_of(uint8_t *out)
{
    static const uint8_t rr[] = { 0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 9, 0, 1 };
    memcpy(out, F.sent, F.len);
    out[2] = 0x81;
    out[3] = 0x80;   /* a response, recursion desired and available, no error */
    out[7] = 1;      /* one answer */
    memcpy(out + F.len, rr, sizeof(rr));
    return (uint16_t)(F.len + sizeof(rr));
}

/* The socket's packet on the loop's port, then the loop's socks_serve. */
static bool deliver(void)
{
    struct port_packet p;
    CHECK_ST(jam_port_wait(D.port, now() + 2 * S, &p), OK);
    uint32_t low = (uint32_t)(p.key & 0xff);
    CHECK(low >= KEY_SOCK && low < KEY_SOCK + SOCK_SLOTS);
    socks_packet(low - KEY_SOCK, (uint32_t)(p.key >> 8));
    CHECK_ST(socks_serve(), OK);
    return true;
}

static bool far_closed(void)
{
    signals_t seen;
    return jam_object_wait_one(F.far, SIG_PEER_CLOSED, now() + S, &seen) == OK;
}

bool t_dnsd_sockets(void)
{
    handle_t fake_net;
    memset(&D, 0, sizeof(D));
    memset(&F, 0, sizeof(F));
    CHECK_ST(jam_port_create(&D.port), OK);
    CHECK_ST(jam_channel_create(&D.net, &fake_net), OK);
    socks_init();
    dns_init(&D.r, &D.io);
    uint32_t server = SERVER;
    dns_set_servers(&D.r, &server, 1);

    /* The first port is taken: the open says so only when answered. */
    F.answer = ERR_ALREADY_BOUND;
    CHECK_ST(dns_ask(&D.r, 0, "a.example", 1), OK);
    CHECK_EQ(F.sends, 0);
    if (!open_round(fake_net))
        return false;
    CHECK_EQ(F.opens, 1);
    F.answer = OK;   /* the next try: another port, which opens */
    dns_tick(&D.r, S);
    if (!open_round(fake_net))
        return false;
    CHECK_EQ(F.opens, 2);
    CHECK(F.ports[1] != F.ports[0] && F.ports[1] >= DNS_PORT_MIN);
    /* Open: the waiting query goes out through the tx ring. */
    take_sent();
    CHECK_EQ(F.sends, 1);
    CHECK_EQ(F.to, SERVER);
    CHECK_EQ(F.to_port, DNS_PORT);
    /* The server's answer reaches the resolver; the socket closes. */
    uint8_t data[SOCKRING_DGRAM_MAX] = { 0 };
    uint16_t n = answer_of(data);
    if (!answer_in(data, n) || !deliver())
        return false;
    CHECK_EQ(D.r.stats.answered, 1);
    CHECK(far_closed());
    CHECK_EQ(slots_used(), 0);
    jam_handle_close(F.far);

    /* Released while its open is in flight: closed when the open answers. */
    CHECK_ST(dns_ask(&D.r, 2 * S, "b.example", 2), OK);
    dns_cancel(&D.r, 2);
    CHECK_EQ(slots_used(), 1);
    if (!open_round(fake_net))
        return false;
    CHECK(far_closed());
    CHECK_EQ(slots_used(), 0);
    take_sent();
    CHECK_EQ(F.sends, 1);   /* nothing went out for it */
    jam_handle_close(F.far);
    rings_drop();
    jam_handle_close(fake_net);
    jam_handle_close(D.net);
    jam_handle_close(D.port);
    return true;
}
