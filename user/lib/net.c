/* The network for programs (<net.h>): thin calls over abi/idl/net.idl.
 *
 * Every wait is netstack's: a blocking sock_recv or echo carries its
 * timeout to netstack, which answers by then, and the call itself waits
 * a little longer (MARGIN), so a reply never comes after its caller gave
 * up and lies on the channel for a later call to trip over. The forms
 * that don't wait keep their own txids (idl_txid_next) on the socket's
 * channel, which then carries no blocking call. */
#include <idl/net.h>
#include <jam/netdev.h>
#include <net.h>

#define MARGIN    NS_PER_S          /* the call waits this much past netstack's timeout */
#define CALL_WAIT (5 * NS_PER_S)    /* a call netstack answers at once */

_Static_assert(sizeof(struct net_counters) == NET_COUNTERS_SIZE, "net.counts' u8[256]");
_Static_assert(sizeof(((struct net_sock_send_to_req *)0)->data) == NET_DGRAM_MAX,
               "net.idl's datagram size");

handle_t net_svc(void)
{
    return svc_get(SVC_NET);
}

/* The time from now to deadline in ms, rounded up, as a timeout_ms. */
static uint32_t timeout_of(uint64_t deadline)
{
    if (deadline == DEADLINE_NEVER)
        return NET_WAIT_FOREVER;
    uint64_t t = now();
    if (deadline <= t)
        return 0;
    uint64_t ms = (deadline - t + NS_PER_MS - 1) / NS_PER_MS;
    return ms >= NET_WAIT_FOREVER ? NET_WAIT_FOREVER - 1 : (uint32_t)ms;
}

/* The call's own deadline: MARGIN past netstack's (a deadline already
 * past is netstack's "don't wait", answered at once). */
static uint64_t call_deadline(uint64_t deadline)
{
    uint64_t t = now();
    if (deadline < t)
        deadline = t;
    return deadline >= DEADLINE_NEVER - MARGIN ? DEADLINE_NEVER : deadline + MARGIN;
}

status_t net_info(handle_t net, struct net_info *out)
{
    uint8_t device = 0, link = 0;
    *out = (struct net_info){ 0 };
    status_t st = net_iface_until(net, now() + CALL_WAIT, &out->address, &out->mask,
                                  &out->gateway, &out->dns[0], &out->dns[1], out->mac, &device,
                                  &link, &out->vlan, &out->speed, &out->version);
    out->device = device;
    out->link = link;
    return st;
}

status_t net_wait_up(handle_t net, uint64_t deadline, struct net_info *out)
{
    struct net_info i;
    for (unsigned guard = 0; guard < 1000; guard++) {   /* each turn is one change */
        status_t st = net_info(net, &i);
        if (st != OK)
            return st;
        if (i.address) {
            if (out)
                *out = i;
            return OK;
        }
        uint32_t ms = timeout_of(deadline), v;
        if (!ms)
            return ERR_TIMED_OUT;
        st = net_wait_change_until(net, call_deadline(deadline), i.version, ms, &v);
        if (st != OK)
            return st;
    }
    return ERR_TIMED_OUT;
}

status_t net_get_counters(handle_t net, struct net_counters *out)
{
    return net_counts_until(net, now() + CALL_WAIT, (uint8_t *)out);
}

status_t net_get_chip_counters(handle_t net, void *out)
{
    return net_chip_counts_until(net, now() + CALL_WAIT, out);
}

status_t net_udp_open(handle_t net, uint16_t port, struct net_sock *out)
{
    *out = (struct net_sock){ 0 };
    return net_udp_until(net, now() + CALL_WAIT, port, &out->ch, &out->port);
}

void net_sock_adopt(struct net_sock *s, handle_t ch, uint16_t port)
{
    *s = (struct net_sock){ .ch = ch, .port = port };
}

void net_close(struct net_sock *s)
{
    if (s->ch)
        jam_handle_close(s->ch);
    *s = (struct net_sock){ 0 };
}

status_t net_connect(struct net_sock *s, uint32_t addr, uint16_t port)
{
    return net_sock_connect_until(s->ch, now() + CALL_WAIT, addr, port);
}

/* The request's data: len bytes, the rest of the fixed array zero. */
static bool fill(uint8_t buf[NET_DGRAM_MAX], const void *data, size_t len)
{
    if (len > NET_DGRAM_MAX)
        return false;
    memcpy(buf, data, len);
    memset(buf + len, 0, NET_DGRAM_MAX - len);
    return true;
}

status_t net_sendto(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                    size_t len)
{
    uint8_t buf[NET_DGRAM_MAX];
    if (!fill(buf, data, len))
        return ERR_INVALID_ARGS;
    return net_sock_send_to_until(s->ch, now() + CALL_WAIT, addr, port, (uint16_t)len, buf);
}

status_t net_send(struct net_sock *s, const void *data, size_t len)
{
    return net_sendto(s, 0, 0, data, len);
}

/* A datagram netstack says is longer than d->data holds is refused, so a
 * caller that reads d->len bytes never reads past it. */
static status_t dgram_ok(status_t st, struct net_dgram *d)
{
    if (st != OK || d->len <= NET_DGRAM_MAX)
        return st;
    d->len = 0;
    return ERR_OUT_OF_RANGE;
}

status_t net_recvfrom(struct net_sock *s, struct net_dgram *d, uint64_t deadline)
{
    return dgram_ok(net_sock_recv_until(s->ch, call_deadline(deadline), timeout_of(deadline),
                                        &d->addr, &d->port, &d->len, &d->dropped, d->data),
                    d);
}

status_t net_ping(handle_t net, uint32_t addr, uint16_t seq, uint16_t size, uint64_t deadline,
                  uint32_t *rtt_us, uint8_t *ttl)
{
    uint32_t ms = timeout_of(deadline), rtt = 0;
    uint8_t t = 0;
    uint16_t got = 0;
    if (!ms)
        return ERR_TIMED_OUT;
    if (ms > NET_ECHO_TIMEOUT_MAX)
        ms = NET_ECHO_TIMEOUT_MAX;
    status_t st = net_echo_until(net, now() + (uint64_t)ms * NS_PER_MS + MARGIN, addr, seq, size,
                                 ms, &rtt, &t, &got);
    if (st == OK && rtt_us)
        *rtt_us = rtt;
    if (st == OK && ttl)
        *ttl = t;
    return st;
}

status_t net_sendto_async(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                          size_t len)
{
    uint8_t buf[NET_DGRAM_MAX];
    if (!fill(buf, data, len))
        return ERR_INVALID_ARGS;
    status_t st = net_sock_send_to_send(s->ch, idl_txid_next(&s->last_txid), addr, port,
                                        (uint16_t)len, buf);
    if (st == OK)
        s->sends++;
    return st;
}

status_t net_recv_arm(struct net_sock *s)
{
    if (s->recv_txid)
        return OK;
    uint32_t txid = idl_txid_next(&s->last_txid);
    status_t st = net_sock_recv_send(s->ch, txid, NET_WAIT_FOREVER);
    if (st == OK)
        s->recv_txid = txid;
    return st;
}

status_t net_sock_take(struct net_sock *s, struct net_dgram *d)
{
    for (;;) {   /* each turn takes a message off a queue the kernel bounds */
        _Alignas(8) uint8_t rep[NET_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(s->ch, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT || st == ERR_PEER_CLOSED || st == ERR_NO_MEMORY)
            return st;
        if (m.txid && m.txid == s->recv_txid) {
            s->recv_txid = 0;
            if (st == OK)
                st = dgram_ok(net_sock_recv_result(rep, &m, &d->addr, &d->port, &d->len,
                                                   &d->dropped, d->data), d);
            if (st == OK)
                (void)net_recv_arm(s);   /* a full queue: the next take finds it unarmed */
            return st;
        }
        if (st == OK)
            st = net_sock_send_to_result(rep, &m);   /* a send's answer (anything else: dropped) */
        if (s->sends)
            s->sends--;
        if (st != OK) {
            s->send_errors++;
            s->last_error = st;
        }
    }
}

status_t net_sock_wait(struct net_sock *s, uint64_t deadline)
{
    signals_t seen;
    return jam_object_wait_one(s->ch, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
}
