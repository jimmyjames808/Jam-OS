/* The network for programs (<net.h>): a socket's datagrams, through its
 * rings (<sockring.h>; net.c opens and maps them).
 *
 * This side produces the tx ring and consumes the rx ring. While data
 * flows nothing here makes a system call: a datagram is copied into or out
 * of the ring and the count published, and netstack is signalled
 * (to_stack) only when its line says it sleeps.
 *
 * Our own `waits` flags (rx's consumer flag, tx's producer flag) say
 * whether netstack should signal `to_prog`. Only what waits sets them:
 *   - a call that sleeps (net_recvfrom, net_sendto, net_sock_wait) clears
 *     the event's bits, raises the flag it needs and looks once more
 *     (sockring_sleep) before it sleeps, and lowers the flag when it wakes;
 *   - a loop that sleeps on a port of its own: net_sock_bind raises rx's
 *     consumer flag and leaves it up (netstack then signals once a turn in
 *     which it put datagrams in the ring);
 *   - a wait set (<netwait.h>) raises and lowers them itself.
 * The calls that don't wait (net_sendto_async, net_sock_take) never touch
 * the flags, so they work under any of the three. A datagram netstack
 * publishes is never slept through: whoever waits raised its flag and
 * looked again after it cleared the event's bits.
 *
 * netstack's side of the rings is trusted more than a program's, but the
 * same code reads it: its counts clamped, a broken record skipped. An
 * event can't say netstack died, so every wait here is on a port with the
 * event and the channel's end (SIG_PEER_CLOSED) both bound. */
#include <net.h>
#include <netwait.h>
#include "netint.h"

#define SEND_WAIT   (5 * NS_PER_S)   /* net_sendto: for room, then for netstack to take it */
#define PROG_BITS   (SOCKRING_SIG_RX | SOCKRING_SIG_TX_ROOM | SOCKRING_SIG_STATE)
#define KEY_EVENT   1u               /* the waiter port's keys */
#define KEY_CHANNEL 2u

static bool has_rings(const struct net_sock *s)
{
    return s->ch && s->map;
}

bool net_sock_gone(const struct net_sock *s)
{
    signals_t seen = 0;
    return jam_object_wait_one(s->ch, SIG_PEER_CLOSED, 0, &seen) == OK;
}

void net_sock_clear(const struct net_sock *s)
{
    (void)jam_event_signal(s->to_prog, PROG_BITS, 0);   /* ours: can't fail */
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
    if (st != OK)
        return st;
    s->bound_port = port;
    s->bound_key = key;
    /* Up for good (sockring_sleep would lower it on finding datagrams):
     * then, as sockring_sleep's fence and second look do, one datagram
     * already there is signalled to ourselves, so the key fires for it. */
    __atomic_store_n(&s->r.rx.cons->waits, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (sockring_ready(&s->r.rx))
        (void)jam_event_signal(s->to_prog, 0, SOCKRING_SIG_RX);   /* ours: can't fail */
    return OK;
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

void net_sock_waitable(struct net_sock *s, struct netwait_sock *out)
{
    *out = (struct netwait_sock){ .rings = &s->r, .to_prog = s->to_prog, .ch = s->ch };
}

status_t net_sock_sleep(struct net_sock *s, uint64_t deadline)
{
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

status_t net_sock_wait(struct net_sock *s, uint64_t deadline)
{
    if (!has_rings(s))
        return ERR_BAD_STATE;
    if (net_sock_gone(s))
        return OK;   /* a look finds it: ERR_PEER_CLOSED */
    net_sock_clear(s);
    status_t st = sockring_sleep(&s->r.rx, 1) ? net_sock_sleep(s, deadline) : OK;
    sockring_awake(&s->r.rx);
    return st;
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
    return has_rings(s) ? put(s, addr, port, data, len) : ERR_BAD_STATE;
}

/* Wait until the tx ring has `need` bytes of room (sockring_sleep's
 * rule), the deadline, or netstack's end. */
static status_t wait_room(struct net_sock *s, uint32_t need, uint64_t deadline)
{
    while (sockring_room(&s->r.tx) < need) {
        if (net_sock_gone(s))
            return ERR_PEER_CLOSED;
        net_sock_clear(s);   /* every bit: a binding fires when none of its bits was set before */
        if (!sockring_sleep(&s->r.tx, need))
            continue;
        status_t st = net_sock_sleep(s, deadline);
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
    status_t st = take(s, d);
    if (st != ERR_SHOULD_WAIT)
        return st;
    if (net_sock_gone(s))
        return ERR_PEER_CLOSED;
    /* Clear the bits, then look once more: a datagram published from now
     * on is a new edge (for a loop bound with net_sock_bind). */
    net_sock_clear(s);
    return take(s, d);
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
