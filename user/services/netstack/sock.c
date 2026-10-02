/* netstack: programs' UDP sockets (progs.h has the model). A socket is a
 * slot here, an lwIP socket (stack.h's stack_udp) and a channel of its
 * own; closing the channel, or its opener going, closes all three.
 *
 * What a program may do is checked here, before lwIP sees it: a port of
 * 1024 or more (or 0, lwIP's pick), unicast destinations only, at most
 * NET_DGRAM_MAX bytes. The DHCP socket (netctl's dhcp_open) is the one
 * exception: port 68, sends to port 67 of 255.255.255.255 or a unicast
 * address, out of the interface even with no address.
 *
 * Receiving never waits for the program: a datagram goes to the sock_recv
 * waiting for it, or into the socket's queue (copied: lwIP's buffer goes
 * back at once), or is dropped and counted when the queue is full. */
#include <idl/net.h>
#include "ctl.h"
#include "progs.h"

#define BROADCAST   0xffffffffu

static uint8_t outbuf[NET_DGRAM_MAX];   /* a sock_recv answer's data: its tail always 0 */

static uint64_t key_of(unsigned i)
{
    return (KEY_SOCK + i) | (uint64_t)pg.s[i].gen << 8;
}

/* May a program send to a? Unicast, and not its subnet's broadcast. */
static bool sendable(uint32_t a)
{
    if (!ctl_unicast(a))
        return false;
    struct stack_state st;
    stack_get(&st);
    uint32_t m = st.ip.mask;
    return !m || (a & m) != (st.ip.address & m) || (a & ~m) != ~m;
}

status_t sock_open(unsigned slot, uint16_t port, bool dhcp, handle_t *out, uint16_t *out_port)
{
    unsigned i = dhcp ? NET_SOCKETS_MAX : 0;   /* programs' are the first NET_SOCKETS_MAX */
    if (dhcp && pg.s[i].ch)
        return ERR_ALREADY_BOUND;
    while (!dhcp && i < NET_SOCKETS_MAX && pg.s[i].ch)
        i++;
    if (!dhcp && i == NET_SOCKETS_MAX)
        return ERR_NO_RESOURCES;
    struct sock *s = &pg.s[i];
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    s->gen++;
    st = stack_udp_open(port, dhcp, s, &s->u, &s->port);
    if (st == OK)
        st = jam_port_bind(pg.port, mine, key_of(i), SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st != OK) {
        stack_udp_close(s->u);
        s->u = NULL;
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    s->ch = mine;
    s->dhcp = dhcp;
    s->opener = slot;
    s->opener_gen = dhcp ? 0 : pg.o[slot].gen;
    s->peer = s->peer_port = 0;
    s->head = s->n = 0;
    s->dropped = 0;
    s->waiting = false;
    s->pending = true;   /* a request may come before the first packet is read */
    if (!dhcp)
        pg.o[slot].socks++;
    *out = theirs;
    *out_port = s->port;
    return OK;
}

static void sock_close(unsigned i)
{
    struct sock *s = &pg.s[i];
    stack_udp_close(s->u);   /* no more datagrams for it */
    for (unsigned k = 0; k < s->n; k++)
        free(s->q[(s->head + k) % NET_RX_QUEUE]);
    jam_handle_close(s->ch);   /* its binding goes with our only handle; a waiting
                                * sock_recv goes unanswered: its channel is gone */
    struct opener *o = s->dhcp ? NULL : progs_opener(s->opener, s->opener_gen);
    if (o && o->socks)
        o->socks--;
    s->ch = HANDLE_INVALID;
    s->u = NULL;
    s->n = 0;
    s->waiting = false;
    s->pending = false;
}

void sock_close_opener(unsigned slot)
{
    for (unsigned i = 0; i < NET_SOCKETS_MAX; i++)
        if (pg.s[i].ch && pg.s[i].opener == slot && pg.s[i].opener_gen == pg.o[slot].gen)
            sock_close(i);
}

void sock_census(uint32_t *queued, uint32_t *open)
{
    *queued = *open = 0;
    for (unsigned i = 0; i < SOCK_SLOTS; i++)
        if (pg.s[i].ch) {
            (*open)++;
            *queued += pg.s[i].n;
        }
}

/* stack.h's stack_udp_input: a datagram for socket ctx. */
static void dgram_in(void *ctx, uint32_t from, uint16_t port, const uint8_t *data, size_t len)
{
    struct sock *s = ctx;
    if (!s->ch || len > NET_DGRAM_MAX)
        return;
    if (s->peer && (from != s->peer || port != s->peer_port))
        return;   /* not from its peer: not for it */
    pg.c.dgrams_in++;
    if (s->waiting) {
        s->waiting = false;
        memcpy(outbuf, data, len);
        memset(outbuf + len, 0, NET_DGRAM_MAX - len);
        (void)net_reply_sock_recv(s->txn, OK, from, port, (uint16_t)len, s->dropped, outbuf);
        return;   /* a reply that can't be written: the program is going */
    }
    struct dgram *d = s->n < NET_RX_QUEUE ? malloc(sizeof(*d) + len) : NULL;
    if (!d) {
        s->dropped++;
        pg.c.dgrams_dropped++;
        return;
    }
    d->addr = from;
    d->port = port;
    d->len = (uint16_t)len;
    memcpy(d->data, data, len);
    s->q[(s->head + s->n++) % NET_RX_QUEUE] = d;
}

/* ---- the socket's methods ------------------------------------------------------- */

static status_t op_send_to(void *ctx, uint32_t address, uint16_t port, uint16_t len,
                           const uint8_t data[1472])
{
    struct sock *s = ctx;
    if (!address && !port) {
        if (!s->peer)
            return ERR_BAD_STATE;
        address = s->peer;
        port = s->peer_port;
    }
    if (len > NET_DGRAM_MAX || !port)
        return ERR_INVALID_ARGS;
    if (s->dhcp ? port != NET_PORT_DHCP_SERVER || (address != BROADCAST && !sendable(address))
                : !sendable(address))
        return ERR_INVALID_ARGS;
    status_t st = stack_udp_send(s->u, address, port, data, len, s->dhcp);
    if (st == OK)
        pg.c.dgrams_out++;
    return st;
}

static status_t op_recv(void *ctx, struct idl_txn txn, uint32_t timeout_ms, uint32_t *out_address,
                        uint16_t *out_port, uint16_t *out_len, uint32_t *out_dropped,
                        uint8_t out_data[1472])
{
    struct sock *s = ctx;
    if (s->waiting)
        return ERR_BAD_STATE;
    if (s->n) {
        struct dgram *d = s->q[s->head];
        s->head = (s->head + 1) % NET_RX_QUEUE;
        s->n--;
        *out_address = d->addr;
        *out_port = d->port;
        *out_len = d->len;
        *out_dropped = s->dropped;
        memcpy(out_data, d->data, d->len);   /* the rest: the generated code zeroes it */
        free(d);
        return OK;
    }
    if (!timeout_ms)
        return ERR_SHOULD_WAIT;
    s->waiting = true;
    s->txn = txn;
    s->deadline = timeout_ms == NET_WAIT_FOREVER ? DEADLINE_NEVER
                                                 : now() + (uint64_t)timeout_ms * NS_PER_MS;
    return IDL_LATER;
}

static status_t op_connect(void *ctx, uint32_t address, uint16_t port)
{
    struct sock *s = ctx;
    if (s->dhcp)
        return ERR_NOT_SUPPORTED;
    if ((address || port) && (!port || !sendable(address)))
        return ERR_INVALID_ARGS;
    s->peer = address;
    s->peer_port = address ? port : 0;
    return OK;
}

static status_t op_state(void *ctx, uint16_t *out_port, uint32_t *out_peer,
                         uint16_t *out_peer_port, uint32_t *out_queued, uint32_t *out_dropped)
{
    struct sock *s = ctx;
    *out_port = s->port;
    *out_peer = s->peer;
    *out_peer_port = s->peer_port;
    *out_queued = s->n;
    *out_dropped = s->dropped;
    return OK;
}

static const struct net_ops sock_ops = {
    .sock_send_to = op_send_to,
    .sock_recv = op_recv,
    .sock_connect = op_connect,
    .sock_state = op_state,
};

void sock_serve(unsigned i)
{
    struct sock *s = &pg.s[i];
    s->pending = false;
    for (unsigned k = 0; k < PROGS_BUDGET; k++) {
        status_t st = net_serve_one(s->ch, &sock_ops, s);
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED)
            sock_close(i);
        else if (st != ERR_SHOULD_WAIT)
            printf("netstack: reading socket %u's channel: %s\n", s->port, status_str(st));
        return;
    }
    s->pending = true;   /* its budget is spent: more may be queued */
}

uint64_t sock_tick(uint64_t t)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < SOCK_SLOTS; i++) {
        struct sock *s = &pg.s[i];
        if (!s->ch || !s->waiting)
            continue;
        if (s->deadline <= t) {
            s->waiting = false;
            memset(outbuf, 0, sizeof(outbuf));
            (void)net_reply_sock_recv(s->txn, ERR_TIMED_OUT, 0, 0, 0, 0, outbuf);
        } else if (s->deadline < next) {
            next = s->deadline;
        }
    }
    return next;
}

/* ctl.h's ctl_dhcp_open: netctl's DHCP socket. */
static status_t dhcp_open(handle_t *out)
{
    uint16_t port;
    return sock_open(0, NET_PORT_DHCP_CLIENT, true, out, &port);
}

void sock_hooks(void)
{
    stack_udp_input = dgram_in;
    ctl_dhcp_open = dhcp_open;
}
