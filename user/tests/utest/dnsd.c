/* utest: bin/dns's edge to the network (user/services/dns/socks.c), its
 * resolver driving it, against a fake netstack served in-process on a
 * channel pair (net.idl's generated server: udp on the opener channel,
 * sock_recv and sock_send_to on the sockets it hands out).
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
#include "dnsd.h"
#include "utest.h"

#define SERVER 0x0a021501u   /* 10.2.21.1 */
#define S      NS_PER_S

struct dnsd D;   /* bin/dns's state: its main.c isn't linked here */

/* The fake netstack. */
static struct {
    status_t       answer;      /* what the next udp answers */
    unsigned       opens;
    uint16_t       ports[4];    /* the ports asked for */
    handle_t       far;         /* our end of the last socket handed out */
    bool           recv_waiting;
    struct idl_txn recv;        /* its sock_recv, answered later */
    unsigned       sends;
    uint32_t       to;
    uint16_t       to_port, len;
    uint8_t        sent[DNS_MSG_MAX];
} F;

static status_t f_udp(void *ctx, uint16_t port, handle_t *out, uint16_t *out_port)
{
    (void)ctx;
    if (F.opens < 4)
        F.ports[F.opens] = port;
    F.opens++;
    if (F.answer != OK)
        return F.answer;
    handle_t near;
    status_t st = jam_channel_create(&near, &F.far);
    if (st == OK) {
        *out = near;
        *out_port = port;
    }
    return st;
}

static status_t f_recv(void *ctx, struct idl_txn txn, uint32_t timeout_ms, uint32_t *a,
                       uint16_t *p, uint16_t *len, uint32_t *dropped, uint8_t data[1472])
{
    (void)ctx, (void)timeout_ms, (void)a, (void)p, (void)len, (void)dropped, (void)data;
    F.recv = txn;
    F.recv_waiting = true;
    return IDL_LATER;
}

static status_t f_send(void *ctx, uint32_t to, uint16_t port, uint16_t len,
                       const uint8_t data[1472])
{
    (void)ctx;
    F.sends++;
    F.to = to;
    F.to_port = port;
    F.len = len < DNS_MSG_MAX ? len : DNS_MSG_MAX;
    memcpy(F.sent, data, F.len);
    return OK;
}

static const struct net_ops opener_ops = { .udp = f_udp };
static const struct net_ops sock_ops = { .sock_recv = f_recv, .sock_send_to = f_send };

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
    /* Open: the socket's recv is armed and the waiting query goes out. */
    CHECK_ST(net_serve_one(F.far, &sock_ops, NULL), OK);
    CHECK_ST(net_serve_one(F.far, &sock_ops, NULL), OK);
    CHECK(F.recv_waiting);
    CHECK_EQ(F.sends, 1);
    CHECK_EQ(F.to, SERVER);
    CHECK_EQ(F.to_port, DNS_PORT);
    /* The server's answer reaches the resolver; the socket closes. */
    uint8_t data[1472] = { 0 };
    uint16_t n = answer_of(data);
    CHECK_ST(net_reply_sock_recv(F.recv, OK, SERVER, DNS_PORT, n, 0, data), OK);
    if (!deliver())
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
    CHECK_EQ(F.sends, 1);   /* nothing went out for it */
    jam_handle_close(F.far);
    jam_handle_close(fake_net);
    jam_handle_close(D.net);
    jam_handle_close(D.port);
    return true;
}
