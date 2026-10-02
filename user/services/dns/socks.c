/* dns: the resolver's edge to the network (struct dns_io's send and
 * release): a UDP socket from netstack for each local port the resolver
 * picks, one per name in flight.
 *
 * Opening a socket is a call to netstack, so it is written without
 * waiting (net.udp with a txid of ours on the opener channel): the first
 * datagram for a port waits in its slot (SOCK_OPENING) and goes when the
 * answer comes. A port another program has (ERR_ALREADY_BOUND) can't be
 * known when send returns, so that first datagram is lost; the slot keeps
 * the failure, and the resolver's next try on the port is told
 * ERR_ALREADY_BOUND at once, so it picks another port (resolver.c's
 * send_try). Any other failure is a lost datagram too, and the next try
 * opens the port again. A port released while its open is in flight
 * keeps its slot until the answer comes, and the socket is closed then.
 *
 * Each open socket's channel is bound on the loop's port; its datagrams
 * go to dns_input with the local port they came to. */
#include "dnsd.h"

static struct sock *find(uint16_t port)
{
    for (unsigned i = 0; i < SOCK_SLOTS; i++)
        if (D.s[i].state != SOCK_FREE && !D.s[i].released && D.s[i].port == port)
            return &D.s[i];
    return NULL;
}

static void slot_free(struct sock *s)
{
    if (s->state == SOCK_OPEN)
        net_close(&s->s);   /* its binding goes with our only handle */
    uint32_t gen = s->gen + 1;
    *s = (struct sock){ .gen = gen };
}

/* Start opening a socket on port in a free slot, with the first datagram
 * waiting in it. */
static status_t open_with(uint16_t port, uint32_t server, const void *msg, size_t len)
{
    struct sock *s = NULL;
    for (unsigned i = 0; i < SOCK_SLOTS && !s; i++)
        if (D.s[i].state == SOCK_FREE)
            s = &D.s[i];
    if (!s || len > DNS_MSG_MAX)
        return ERR_NO_RESOURCES;   /* not in practice: one slot per query, and spares */
    uint32_t txid = idl_txid_next(&D.net_txid);
    status_t st = net_udp_send(D.net, txid, port);
    if (st != OK)
        return st;
    s->state = SOCK_OPENING;
    s->port = port;
    s->open_txid = txid;
    s->queued = true;
    s->q_server = server;
    s->q_len = (uint16_t)len;
    memcpy(s->q_msg, msg, len);
    return OK;
}

static status_t io_send(void *ctx, uint16_t port, uint32_t server, const void *msg, size_t len)
{
    (void)ctx;
    struct sock *s = find(port);
    if (s && s->state == SOCK_FAILED) {
        status_t why = s->failed;
        slot_free(s);
        if (why == ERR_ALREADY_BOUND)
            return why;   /* the resolver picks another port */
        s = NULL;         /* else try opening it again */
    }
    if (!s)
        return open_with(port, server, msg, len);
    if (s->state == SOCK_OPEN)
        return net_sendto_async(&s->s, server, DNS_PORT, msg, len);
    /* Still opening: this try replaces the one waiting (same id and query). */
    if (len > DNS_MSG_MAX)
        return ERR_INVALID_ARGS;
    s->queued = true;
    s->q_server = server;
    s->q_len = (uint16_t)len;
    memcpy(s->q_msg, msg, len);
    return OK;
}

static void io_release(void *ctx, uint16_t port)
{
    (void)ctx;
    struct sock *s = find(port);
    if (!s)
        return;
    if (s->state == SOCK_OPENING) {
        s->released = true;   /* closed when its open is answered */
        s->queued = false;
        return;
    }
    slot_free(s);
}

static uint32_t io_random(void *ctx)
{
    (void)ctx;
    return os_random_u32();
}

void socks_init(void)
{
    D.io = (struct dns_io){ .ctx = &D, .send = io_send, .release = io_release,
                            .answer = askers_answer, .random = io_random };
}

/* The open of slot s was answered: the socket, its port, or why not. */
static void opened(struct sock *s, status_t st, handle_t h, uint16_t port)
{
    if (s->released) {
        if (st == OK)
            jam_handle_close(h);
        slot_free(s);
        return;
    }
    if (st != OK) {
        s->state = SOCK_FAILED;
        s->failed = st;
        s->queued = false;
        return;
    }
    net_sock_adopt(&s->s, h, port);
    s->state = SOCK_OPEN;
    uint64_t key = (KEY_SOCK + (unsigned)(s - D.s)) | (uint64_t)s->gen << 8;
    st = jam_port_bind(D.port, h, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = net_recv_arm(&s->s);
    if (st != OK) {
        printf("dns: a socket on port %u can't be read (%s)\n", port, status_str(st));
        slot_free(s);   /* the resolver's next try opens it again */
        return;
    }
    if (s->queued)   /* a failure is a lost datagram: the next try covers it */
        (void)net_sendto_async(&s->s, s->q_server, DNS_PORT, s->q_msg, s->q_len);
    s->queued = false;
}

bool socks_open_reply(const void *rep, struct idl_msg *m)
{
    for (unsigned i = 0; i < SOCK_SLOTS; i++) {
        struct sock *s = &D.s[i];
        if (s->state != SOCK_OPENING || s->open_txid != m->txid)
            continue;
        handle_t h = HANDLE_INVALID;
        uint16_t port = 0;
        status_t st = net_udp_result(rep, m, &h, &port);
        if (st == OK && port != s->port) {
            jam_handle_close(h);   /* not the port asked for: netstack broke the protocol */
            st = ERR_INTERNAL;
        }
        opened(s, st, h, port);
        return true;
    }
    return false;
}

void socks_packet(unsigned i, uint32_t gen)
{
    if (i < SOCK_SLOTS && D.s[i].state == SOCK_OPEN && D.s[i].gen == gen)
        D.s[i].pending = true;
}

/* Up to BUDGET datagrams from slot s to the resolver. */
static status_t serve_one(struct sock *s)
{
    s->pending = false;
    for (unsigned k = 0; k < BUDGET; k++) {
        struct net_dgram d;
        status_t st = net_sock_take(&s->s, &d);
        if (st == ERR_SHOULD_WAIT)
            return OK;
        if (st == ERR_PEER_CLOSED)
            return st;
        if (st != OK) {
            (void)net_recv_arm(&s->s);   /* its recv failed: ask again (a failure: the next turn) */
            return OK;
        }
        struct dns_datagram dg = { .port = s->port, .src = d.addr, .src_port = d.port,
                                   .msg = d.data, .len = d.len };
        uint32_t gen = s->gen;
        dns_input(&D.r, now(), &dg);   /* may release (free) this very slot */
        if (s->state != SOCK_OPEN || s->gen != gen)
            return OK;
    }
    s->pending = true;   /* its budget is spent: more may be queued */
    return OK;
}

status_t socks_serve(void)
{
    for (unsigned i = 0; i < SOCK_SLOTS; i++) {
        struct sock *s = &D.s[i];
        if (s->state == SOCK_OPEN && s->pending && serve_one(s) == ERR_PEER_CLOSED)
            return ERR_PEER_CLOSED;
    }
    return OK;
}

bool socks_pending(void)
{
    for (unsigned i = 0; i < SOCK_SLOTS; i++)
        if (D.s[i].state == SOCK_OPEN && D.s[i].pending)
            return true;
    return false;
}
