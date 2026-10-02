/* The network for programs (<net.h>): TCP connections and listeners over
 * net.idl's tcp, tcp_listener and accept.
 *
 * A connection is a net_sock whose rings carry a byte stream: this side
 * produces the tx ring and consumes the rx ring, and SOCKRING_END on tx
 * is its shutdown. As netsock.c's datagrams, nothing here makes a system
 * call while bytes flow, but to wake netstack when its line says it
 * sleeps. A call that waits clears to_prog's bits first and only then
 * looks (the ring, the status line), raising its own flag with
 * sockring_sleep before it sleeps: so a byte, room or a change of state
 * that comes after the look is a new signal, and nothing is slept
 * through. netstack signals SOCKRING_SIG_STATE whenever the status
 * changes, without being asked.
 *
 * A listener's accept is answered later by netstack (when a connection
 * comes), so it is always sent without waiting: the answer lands on the
 * listener's channel, which a caller can wait on alone (net_tcp_accept)
 * or in a wait set (READ). One accept is asked at a time; one that a
 * caller stopped waiting for stays asked, and the next take gets it. */
#include <idl/net.h>
#include <net.h>
#include <netwait.h>
#include "netint.h"

#define CALL_WAIT (5 * NS_PER_S)   /* a call netstack answers at once */

static bool has_rings(const struct net_sock *s)
{
    return s->ch && s->map;
}

void net_tcp_status(const struct net_sock *s, uint32_t *state, status_t *error)
{
    struct sockring_status st;
    sockring_status_get(&s->r, &st);
    *state = st.state;
    *error = st.error;
}

/* CLOSED: why (never OK: a clean close with nothing to read is ERR_PEER_CLOSED
 * to a writer); else OK. */
static status_t closed_why(const struct net_sock *s)
{
    uint32_t state;
    status_t err;
    net_tcp_status(s, &state, &err);
    if (state != SOCKRING_STATE_CLOSED)
        return OK;
    return err != OK ? err : ERR_PEER_CLOSED;
}

status_t net_tcp_open(handle_t net, uint32_t addr, uint16_t port, uint32_t tx_bytes,
                      uint32_t rx_bytes, struct net_sock *out)
{
    handle_t ch = HANDLE_INVALID, hs[3] = { 0 };
    uint32_t tx = 0, rx = 0;
    uint16_t local = 0;
    *out = (struct net_sock){ 0 };
    status_t st = net_tcp_until(net, now() + CALL_WAIT, addr, port, tx_bytes, rx_bytes, &ch,
                                &hs[0], &hs[1], &hs[2], &local, &tx, &rx);
    return st == OK ? net_sock_attach(out, ch, local, hs, SOCKRING_STREAM, tx, rx) : st;
}

/* Wait until the state is `want` or CLOSED (or the deadline): OK, or
 * the CLOSED error (want CLOSED: its error as it is, OK for a clean close;
 * else ERR_PEER_CLOSED for a clean one). */
static status_t wait_state(struct net_sock *s, uint32_t want, uint64_t deadline)
{
    if (!has_rings(s))
        return ERR_BAD_STATE;
    for (;;) {   /* each turn sleeps until a signal or the deadline */
        net_sock_clear(s);   /* before the look: a change after it is a new signal */
        uint32_t state;
        status_t err;
        net_tcp_status(s, &state, &err);
        if (state == want && want != SOCKRING_STATE_CLOSED)
            return OK;
        if (state == SOCKRING_STATE_CLOSED)
            return err != OK || want == SOCKRING_STATE_CLOSED ? err : ERR_PEER_CLOSED;
        if (net_sock_gone(s))
            return ERR_PEER_CLOSED;
        if (deadline <= now())
            return ERR_TIMED_OUT;
        status_t st = net_sock_sleep(s, deadline);
        if (st != OK && st != ERR_TIMED_OUT)
            return st;
    }
}

status_t net_tcp_wait_open(struct net_sock *s, uint64_t deadline)
{
    return wait_state(s, SOCKRING_STATE_OPEN, deadline);
}

status_t net_tcp_wait_closed(struct net_sock *s, uint64_t deadline)
{
    return wait_state(s, SOCKRING_STATE_CLOSED, deadline);
}

status_t net_tcp_connect(handle_t net, uint32_t addr, uint16_t port, uint64_t deadline,
                         struct net_sock *out)
{
    status_t st = net_tcp_open(net, addr, port, 0, 0, out);
    if (st == OK)
        st = net_tcp_wait_open(out, deadline);
    if (st != OK)
        net_close(out);
    return st;
}

/* ---- bytes ------------------------------------------------------------------------------ */

size_t net_write_some(struct net_sock *s, const void *data, size_t len)
{
    if (!has_rings(s) || s->r.tx.ended || closed_why(s) != OK)
        return 0;
    uint32_t n = sockring_stream_write(&s->r.tx, data, len > UINT32_MAX ? UINT32_MAX
                                                                       : (uint32_t)len);
    if (n && sockring_publish(&s->r.tx))
        (void)jam_event_signal(s->to_stack, 0, SOCKRING_SIG_TX);   /* gone: the channel says */
    return n;
}

/* No room for the rest now: why not to wait (an error), or
 * ERR_SHOULD_WAIT. */
static status_t write_stop(struct net_sock *s, uint64_t deadline)
{
    if (s->r.tx.ended)
        return ERR_BAD_STATE;
    status_t st = closed_why(s);
    if (st != OK)
        return st;
    if (net_sock_gone(s))
        return ERR_PEER_CLOSED;
    return deadline <= now() ? ERR_TIMED_OUT : ERR_SHOULD_WAIT;
}

status_t net_write(struct net_sock *s, const void *data, size_t len, uint64_t deadline,
                   size_t *out_n)
{
    size_t done = 0;
    status_t st = has_rings(s) ? OK : ERR_BAD_STATE;
    while (st == OK && done < len) {   /* each turn moves bytes or sleeps (to the deadline) */
        net_sock_clear(s);
        done += net_write_some(s, (const uint8_t *)data + done, len - done);
        if (done == len)
            break;
        st = write_stop(s, deadline);
        if (st != ERR_SHOULD_WAIT)
            break;
        st = OK;
        if (!sockring_sleep(&s->r.tx, 1))
            continue;   /* room came while it looked */
        st = net_sock_sleep(s, deadline);
        sockring_awake(&s->r.tx);
        if (st == ERR_TIMED_OUT)
            st = OK;   /* the next turn tries once more, then says ERR_TIMED_OUT */
    }
    if (out_n)
        *out_n = done;
    return st;
}

size_t net_read_some(struct net_sock *s, void *buf, size_t cap)
{
    if (!has_rings(s))
        return 0;
    uint32_t n = sockring_stream_read(&s->r.rx, buf, cap > UINT32_MAX ? UINT32_MAX
                                                                     : (uint32_t)cap);
    if (n && sockring_publish(&s->r.rx))
        (void)jam_event_signal(s->to_stack, 0, SOCKRING_SIG_RX_ROOM);
    return n;
}

/* Nothing to read now: why it won't wait (the end OK, an error), or
 * ERR_SHOULD_WAIT. */
static status_t read_stop(struct net_sock *s, uint64_t deadline, bool past)
{
    if (sockring_at_end(&s->r.rx))
        return OK;
    status_t st = closed_why(s);
    if (st != OK)
        return st;
    if (net_sock_gone(s))
        return ERR_PEER_CLOSED;
    if (deadline <= now())
        return past ? ERR_SHOULD_WAIT : ERR_TIMED_OUT;
    return ERR_SHOULD_WAIT;
}

status_t net_read(struct net_sock *s, void *buf, size_t cap, uint64_t deadline, size_t *out_n)
{
    bool past = deadline <= now();
    *out_n = 0;
    if (!has_rings(s))
        return ERR_BAD_STATE;
    for (;;) {   /* each turn reads, stops, or sleeps (to the deadline) */
        net_sock_clear(s);
        *out_n = net_read_some(s, buf, cap);
        if (*out_n)
            return OK;
        status_t st = read_stop(s, deadline, past);
        if (st != ERR_SHOULD_WAIT || past)
            return st;
        if (!sockring_sleep(&s->r.rx, 1))
            continue;
        st = net_sock_sleep(s, deadline);
        sockring_awake(&s->r.rx);
        if (st != OK && st != ERR_TIMED_OUT)
            return st;
    }
}

status_t net_shutdown(struct net_sock *s)
{
    if (!has_rings(s))
        return ERR_BAD_STATE;
    if (sockring_finish(&s->r.tx))
        (void)jam_event_signal(s->to_stack, 0, SOCKRING_SIG_TX);   /* gone: the channel says */
    return OK;
}

/* ---- listeners ---------------------------------------------------------------------------- */

status_t net_tcp_listen(handle_t net, uint16_t port, uint32_t backlog, uint32_t tx_bytes,
                        uint32_t rx_bytes, struct net_listener *out)
{
    handle_t ch = HANDLE_INVALID;
    uint16_t got = 0;
    *out = (struct net_listener){ 0 };
    status_t st = net_tcp_listener_until(net, now() + CALL_WAIT, port, backlog, tx_bytes,
                                         rx_bytes, &ch, &got);
    if (st == OK)
        *out = (struct net_listener){ .ch = ch, .port = got };
    return st;
}

status_t net_tcp_accept_send(struct net_listener *l)
{
    if (l->asked)
        return OK;
    l->txid = idl_txid_next(&l->last_txid);
    status_t st = net_accept_send(l->ch, l->txid, NET_WAIT_FOREVER);
    l->asked = st == OK;
    return st;
}

status_t net_tcp_accept_take(struct net_listener *l, struct net_sock *out, uint32_t *peer,
                             uint16_t *peer_port)
{
    uint8_t rep[NET_REP_MAX];
    struct idl_msg m;
    *out = (struct net_sock){ 0 };
    if (!l->asked)
        return ERR_SHOULD_WAIT;
    status_t st = idl_reply_read(l->ch, rep, sizeof(rep), &m);
    if (st == ERR_INTERNAL && m.txid == l->txid)
        l->asked = false;   /* a broken answer: ask again */
    if (st != OK)
        return st;
    if (m.txid != l->txid) {   /* no call of ours: nothing else is asked on it */
        idl_msg_drop(&m);
        return ERR_SHOULD_WAIT;
    }
    l->asked = false;
    handle_t ch = HANDLE_INVALID, hs[3] = { 0 };
    uint32_t addr = 0, tx = 0, rx = 0;
    uint16_t port = 0;
    st = net_accept_result(rep, &m, &ch, &hs[0], &hs[1], &hs[2], &addr, &port, &tx, &rx);
    if (st == OK)
        st = net_sock_attach(out, ch, l->port, hs, SOCKRING_STREAM, tx, rx);
    if (st == OK && peer)
        *peer = addr;
    if (st == OK && peer_port)
        *peer_port = port;
    return st;
}

status_t net_tcp_accept(struct net_listener *l, uint64_t deadline, struct net_sock *out,
                        uint32_t *peer, uint16_t *peer_port)
{
    bool past = deadline <= now();
    status_t st = net_tcp_accept_send(l);
    while (st == OK) {   /* each turn takes an answer or waits for one (to the deadline) */
        st = net_tcp_accept_take(l, out, peer, peer_port);
        if (st != ERR_SHOULD_WAIT)
            return st;
        if (deadline <= now())
            return past ? ERR_SHOULD_WAIT : ERR_TIMED_OUT;
        signals_t seen = 0;
        st = jam_object_wait_one(l->ch, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st == ERR_TIMED_OUT)
            st = OK;   /* the next turn takes a last look */
        else if (st == OK && !(seen & SIG_READABLE))
            st = ERR_PEER_CLOSED;
    }
    return st;
}

void net_listener_waitable(const struct net_listener *l, struct netwait_handle *out)
{
    *out = (struct netwait_handle){ .h = l->ch, .read = SIG_READABLE, .hup = SIG_PEER_CLOSED };
}

void net_listener_close(struct net_listener *l)
{
    if (l->ch)
        jam_handle_close(l->ch);
    *l = (struct net_listener){ 0 };
}
