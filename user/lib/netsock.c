/* The network for programs (<net.h>): a socket's datagrams, through its
 * rings (<sockring.h>; net.c opens and maps them).
 *
 * This side produces the tx ring and consumes the rx ring. While data
 * flows nothing here makes a system call: a datagram is copied into or out
 * of the ring and the count published, and netstack is signalled
 * (to_stack) only when its line says it sleeps. Waking works the other way
 * the same: before this side waits it clears its event's bits, raises its
 * flag and looks at the ring once more (sockring_sleep), so a datagram
 * netstack published meanwhile is never slept through.
 *
 * netstack's side of the rings is trusted more than a program's, but the
 * same code reads it: its counts clamped, a broken record skipped. An
 * event can't say netstack died, so every wait here is on a port with the
 * event and the channel's end (SIG_PEER_CLOSED) both bound. */
#include <net.h>

#define SEND_WAIT   (5 * NS_PER_S)   /* net_sendto: for room, then for netstack to take it */
#define PROG_BITS   (SOCKRING_SIG_RX | SOCKRING_SIG_TX_ROOM | SOCKRING_SIG_STATE)
#define KEY_EVENT   1u               /* the waiter port's keys */
#define KEY_CHANNEL 2u

static bool has_rings(const struct net_sock *s)
{
    return s->ch && s->map;
}

/* netstack ended: its end of the socket's channel is closed. */
static bool gone(const struct net_sock *s)
{
    signals_t seen = 0;
    return jam_object_wait_one(s->ch, SIG_PEER_CLOSED, 0, &seen) == OK;
}

/* Bind the event and the channel on port with key (both, or neither). */
static status_t bind_both(const struct net_sock *s, handle_t port, uint64_t k1, uint64_t k2,
                          uint32_t flags)
{
    status_t st = jam_port_bind(port, s->to_prog, k1, PROG_BITS, flags);
    if (st != OK)
        return st;
    st = jam_port_bind(port, s->ch, k2, SIG_PEER_CLOSED, flags);
    if (st != OK)
        (void)jam_port_unbind(port, s->to_prog, k1);   /* just bound: can't fail */
    return st;
}

status_t net_sock_bind(struct net_sock *s, handle_t port, uint64_t key, uint32_t flags)
{
    if (!has_rings(s))
        return ERR_BAD_STATE;
    net_sock_unbind(s);
    status_t st = bind_both(s, port, key, key, flags);
    if (st == OK) {
        s->bound_port = port;
        s->bound_key = key;
    }
    return st;
}

void net_sock_unbind(struct net_sock *s)
{
    if (!s->bound_port)
        return;
    /* A ONCE binding that fired is gone already: nothing to undo then. */
    (void)jam_port_unbind(s->bound_port, s->to_prog, s->bound_key);
    (void)jam_port_unbind(s->bound_port, s->ch, s->bound_key);
    s->bound_port = HANDLE_INVALID;
}

status_t net_sock_wait(struct net_sock *s, uint64_t deadline)
{
    if (!has_rings(s))
        return ERR_BAD_STATE;
    if (!s->waiter) {
        handle_t p = HANDLE_INVALID;
        status_t st = jam_port_create(&p);
        if (st != OK)
            return st;
        st = bind_both(s, p, KEY_EVENT, KEY_CHANNEL, PORT_BIND_PERSISTENT);
        if (st != OK) {
            jam_handle_close(p);
            return st;
        }
        s->waiter = p;
    }
    struct port_packet pkt;
    return jam_port_wait(s->waiter, deadline, &pkt);
}

/* Records netstack refused since we last looked: into send_errors. */
static void note_refused(struct net_sock *s)
{
    struct sockring_status st;
    sockring_status_get(&s->r, &st);
    if (st.tx_refused > s->refused_seen) {
        s->send_errors += (uint32_t)(st.tx_refused - s->refused_seen);
        s->last_error = st.error;
        s->refused_seen = st.tx_refused;
    }
}

/* ---- sending -------------------------------------------------------------------------- */

/* Put one record and publish it, waking netstack if it sleeps. */
static status_t put(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                    size_t len)
{
    if (len > NET_DGRAM_MAX)
        return ERR_INVALID_ARGS;
    struct sockring_dgram h = { .addr = addr, .port = port, .len = (uint16_t)len };
    status_t st = sockring_dgram_put(&s->r.tx, &h, data);
    if (st != OK)
        return st;
    if (sockring_publish(&s->r.tx))
        (void)jam_event_signal(s->to_stack, 0, SOCKRING_SIG_TX);   /* gone: the channel says */
    return OK;
}

status_t net_sendto_async(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                          size_t len)
{
    if (!has_rings(s))
        return ERR_BAD_STATE;
    for (;;) {
        status_t st = put(s, addr, port, data, len);
        if (st != ERR_SHOULD_WAIT)
            return st;
        /* Full: ask netstack for SIG_TX_ROOM, unless it made room meanwhile. */
        if (sockring_sleep(&s->r.tx, sockring_dgram_bytes((uint32_t)len)))
            return ERR_SHOULD_WAIT;
    }
}

/* Wait until the tx ring has `need` bytes of room (sockring_sleep's
 * rule), the deadline, or netstack's end. */
static status_t wait_room(struct net_sock *s, uint32_t need, uint64_t deadline)
{
    while (sockring_room(&s->r.tx) < need) {
        if (gone(s))
            return ERR_PEER_CLOSED;
        /* every bit: a binding fires when none of its bits was set before */
        (void)jam_event_signal(s->to_prog, PROG_BITS, 0);   /* ours: can't fail */
        if (!sockring_sleep(&s->r.tx, need))
            continue;
        status_t st = net_sock_wait(s, deadline);
        sockring_awake(&s->r.tx);
        if (st != OK)
            return st;
    }
    return OK;
}

status_t net_sendto(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                    size_t len)
{
    if (!has_rings(s))
        return ERR_BAD_STATE;
    if (len > NET_DGRAM_MAX)
        return ERR_INVALID_ARGS;
    uint64_t deadline = now() + SEND_WAIT;
    status_t st = wait_room(s, sockring_dgram_bytes((uint32_t)len), deadline);
    struct sockring_status before;
    sockring_status_get(&s->r, &before);
    if (st == OK)
        st = put(s, addr, port, data, len);
    if (st == OK)   /* taken when the whole ring is free again: netstack read it */
        st = wait_room(s, s->r.tx.size, deadline);
    if (st != OK)
        return st;
    struct sockring_status after;
    sockring_status_get(&s->r, &after);
    if (after.tx_refused == before.tx_refused)
        return OK;
    s->refused_seen = after.tx_refused;   /* reported here, not again by net_sock_take */
    return after.error < 0 ? after.error : ERR_INTERNAL;
}

status_t net_send(struct net_sock *s, const void *data, size_t len)
{
    return net_sendto(s, 0, 0, data, len);
}

/* ---- receiving ------------------------------------------------------------------------ */

/* The next record into *d: OK, or ERR_SHOULD_WAIT when none is published. */
static status_t take(struct net_sock *s, struct net_dgram *d)
{
    for (;;) {   /* each turn takes a record or drops what was published: bounded */
        struct sockring_dgram h;
        status_t st = sockring_dgram_take(&s->r.rx, &h, d->data);
        if (st == ERR_OUT_OF_RANGE)
            continue;   /* netstack broke a record: skipped, counted in r.rx.errors */
        if (st != OK)
            return st;
        if (sockring_publish(&s->r.rx))
            (void)jam_event_signal(s->to_stack, 0, SOCKRING_SIG_RX_ROOM);
        struct sockring_status ss;
        sockring_status_get(&s->r, &ss);
        d->addr = h.addr;
        d->port = h.port;
        d->len = h.len;
        d->dropped = ss.rx_dropped > UINT32_MAX ? UINT32_MAX : (uint32_t)ss.rx_dropped;
        memset(d->data + h.len, 0, NET_DGRAM_MAX - h.len);
        return OK;
    }
}

status_t net_sock_take(struct net_sock *s, struct net_dgram *d)
{
    if (!has_rings(s))
        return ERR_BAD_STATE;
    note_refused(s);
    sockring_awake(&s->r.rx);
    for (;;) {
        status_t st = take(s, d);
        if (st != ERR_SHOULD_WAIT)
            return st;
        if (gone(s))
            return ERR_PEER_CLOSED;
        /* Clear the bits, then look once more with the flag up: a datagram
         * published from now on signals the event, so the loop's key fires. */
        (void)jam_event_signal(s->to_prog, PROG_BITS, 0);   /* ours: can't fail */
        if (sockring_sleep(&s->r.rx, 1))
            return ERR_SHOULD_WAIT;
    }
}

status_t net_recvfrom(struct net_sock *s, struct net_dgram *d, uint64_t deadline)
{
    bool past = deadline <= now();
    for (;;) {   /* each turn ends in a datagram, an error, or a wait bounded by deadline */
        status_t st = net_sock_take(s, d);
        if (st != ERR_SHOULD_WAIT)
            return st;
        if (past)
            return ERR_SHOULD_WAIT;
        st = net_sock_wait(s, deadline);
        if (st == ERR_TIMED_OUT) {
            st = net_sock_take(s, d);   /* the last look */
            return st == ERR_SHOULD_WAIT ? ERR_TIMED_OUT : st;
        }
        if (st != OK)
            return st;
    }
}
