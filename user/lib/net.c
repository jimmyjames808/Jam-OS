/* The network for programs (<net.h>): the calls on an opener's channel
 * over abi/idl/net.idl, and opening and closing a socket (its rings
 * mapped). A socket's datagrams are netsock.c's.
 *
 * Every wait is netstack's: a blocking echo or wait_change carries its
 * timeout to netstack, which answers by then, and the call itself waits
 * a little longer (MARGIN), so a reply never comes after its caller gave
 * up and lies on the channel for a later call to trip over. */
#include <idl/net.h>
#include <jam/netdev.h>
#include <net.h>

#define MARGIN    NS_PER_S          /* the call waits this much past netstack's timeout */
#define CALL_WAIT (5 * NS_PER_S)    /* a call netstack answers at once */

_Static_assert(sizeof(struct net_counters) == NET_COUNTERS_SIZE, "net.counts' u8[256]");

handle_t net_svc(void)
{
    handle_t h = svc_get(SVC_NET_SYS);
    return h ? h : svc_get(SVC_NET);
}

status_t net_svc_open(handle_t *out)
{
    status_t st = svc_open(SVC_NET_SYS, out);
    return st == ERR_NOT_FOUND ? svc_open(SVC_NET, out) : st;
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

/* Take on a socket: its channel and rings (all closed on a failure). */
static status_t attach(struct net_sock *s, handle_t ch, uint16_t port, const handle_t hs[3],
                       uint32_t tx, uint32_t rx)
{
    *s = (struct net_sock){ .ch = ch, .port = port, .ring = hs[0], .to_stack = hs[1],
                            .to_prog = hs[2] };
    uint64_t va = 0, len = sockring_bytes(tx, rx);
    status_t st = sockring_size_ok(tx) && sockring_size_ok(rx) ? OK : ERR_BAD_STATE;
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), s->ring, 0, len, VMAR_READ | VMAR_WRITE,
                          &va);
    if (st == OK) {
        s->map = (uint8_t *)(uintptr_t)va;
        s->map_len = len;
        st = sockring_attach(&s->r, s->map, len, SOCKRING_DGRAM, tx, rx);
    }
    if (st != OK)
        net_close(s);
    return st;
}

status_t net_udp_open_rings(handle_t net, uint16_t port, uint32_t tx_bytes, uint32_t rx_bytes,
                            struct net_sock *out)
{
    handle_t ch = HANDLE_INVALID, hs[3] = { 0 };
    uint32_t tx = 0, rx = 0;
    *out = (struct net_sock){ 0 };
    status_t st = net_udp_rings_until(net, now() + CALL_WAIT, port, tx_bytes, rx_bytes, &ch,
                                      &hs[0], &hs[1], &hs[2], &port, &tx, &rx);
    return st == OK ? attach(out, ch, port, hs, tx, rx) : st;
}

status_t net_udp_open(handle_t net, uint16_t port, struct net_sock *out)
{
    return net_udp_open_rings(net, port, 0, 0, out);
}

status_t net_udp_open_async(handle_t net, uint32_t txid, uint16_t port)
{
    return net_udp_rings_send(net, txid, port, 0, 0);
}

status_t net_udp_opened(const void *rep, struct idl_msg *m, struct net_sock *out)
{
    handle_t ch = HANDLE_INVALID, hs[3] = { 0 };
    uint32_t tx = 0, rx = 0;
    uint16_t port = 0;
    *out = (struct net_sock){ 0 };
    status_t st = net_udp_rings_result(rep, m, &ch, &hs[0], &hs[1], &hs[2], &port, &tx, &rx);
    return st == OK ? attach(out, ch, port, hs, tx, rx) : st;
}

status_t net_sock_adopt(struct net_sock *s, handle_t ch, uint16_t port)
{
    handle_t hs[3] = { 0 };
    uint32_t tx = 0, rx = 0;
    status_t st = net_sock_rings_until(ch, now() + CALL_WAIT, 0, 0, &hs[0], &hs[1], &hs[2], &tx,
                                       &rx);
    if (st != OK) {
        jam_handle_close(ch);
        *s = (struct net_sock){ 0 };
        return st;
    }
    return attach(s, ch, port, hs, tx, rx);
}

void net_close(struct net_sock *s)
{
    net_sock_unbind(s);
    if (s->map)   /* before the channel: netstack shrinks the VMO once it is closed */
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)s->map,
                             s->map_len);   /* our own mapping: nothing else to do */
    handle_t hs[] = { s->waiter, s->ring, s->to_stack, s->to_prog, s->ch };
    for (unsigned i = 0; i < sizeof(hs) / sizeof(hs[0]); i++)
        if (hs[i])
            jam_handle_close(hs[i]);
    *s = (struct net_sock){ 0 };
}

status_t net_connect(struct net_sock *s, uint32_t addr, uint16_t port)
{
    return net_sock_connect_until(s->ch, now() + CALL_WAIT, addr, port);
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
