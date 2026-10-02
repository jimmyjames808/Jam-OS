/* e1000e: the netdev server (drv/e1000e): abi/idl/netdev.idl on DR_SERVE
 * and on the session channel, with <jam/netdev.h>'s rings.
 *
 *   info   the address, the VLAN, the MTU, the link and its change count,
 *          "82574L"
 *   stats  struct netdev_stats: the driver's counts since it started and
 *          the chip's (GPTC, GPRC, the error and missed counters)
 *   open   a session: a channel of its own (info and stats work on it,
 *          open is refused there), the tx and rx ring VMOs and the two
 *          events, each with the rights <jam/netdev.h> names. One session
 *          at a time: ERR_BAD_STATE while one is open, but a session whose
 *          opener has gone is ended first, so a restarted netstack opens
 *          at once.
 *
 * Who reaches it: devmgr hands DR_SERVE's client end out on the NIC's
 * device channel (init's, for netstack) and its control channel (test
 * programs under user/tests/). Nothing here decides who may ask.
 *
 * A session lives until its channel closes (netstack closed it or died)
 * or the driver stops: then the rings are unmapped and closed, and frames
 * received meanwhile are dropped and counted (rx_no_session). Each open
 * makes new rings with both counts 0; the port keys carry the session's
 * generation, so a packet for an old session is ignored. */
#include <idl/netdev.h>
#include "e1000e.h"

#define DRAIN_MAX     32          /* messages taken from one channel per turn */
#define SESSION_LINES 16          /* session lines logged, then one in 64 */

uint64_t session_key(const struct e1k *t, uint64_t kind)
{
    return kind | t->gen << 8;
}

void session_signal(struct e1k *t, signals_t bits)
{
    if (t->s.ch != HANDLE_INVALID)
        (void)drv_event_signal(t->s.to_stack, 0, bits);   /* netstack gone: nobody waits */
}

static bool session_line(struct e1k *t)
{
    uint32_t n = t->session_lines++;
    return n < SESSION_LINES || n % 64 == 0;
}

/* Everything a session holds, closed (what exists of it). */
static void session_free(struct session *s)
{
    if (s->ch != HANDLE_INVALID)
        drv_handle_close(s->ch);   /* its port binding goes with it */
    if (s->tx_map)
        (void)drv_vmo_unmap(s->tx_map, NETDEV_RING_BYTES);   /* can't fail for our own mapping */
    if (s->rx_map)
        (void)drv_vmo_unmap(s->rx_map, NETDEV_RING_BYTES);
    handle_t hs[4] = { s->tx_vmo, s->rx_vmo, s->to_driver, s->to_stack };
    for (unsigned i = 0; i < 4; i++)
        if (hs[i] != HANDLE_INVALID)
            drv_handle_close(hs[i]);
    *s = (struct session){ .ch = HANDLE_INVALID, .tx_vmo = HANDLE_INVALID,
                           .rx_vmo = HANDLE_INVALID, .to_driver = HANDLE_INVALID,
                           .to_stack = HANDLE_INVALID };
}

void session_end(struct e1k *t, const char *why)
{
    struct session *s = &t->s;
    if (s->ch == HANDLE_INVALID)
        return;
    t->ring_errors_done += s->tx.errors + s->rx.errors;
    if (session_line(t))
        drv_log("session %lu ended (%s): %lu frame(s) taken, %lu given; %lu ring error(s)",
                (unsigned long)t->st.sessions, why, (unsigned long)s->tx.count,
                (unsigned long)s->rx.count, (unsigned long)(s->tx.errors + s->rx.errors));
    session_free(s);
    t->gen++;
}

/* A ring VMO of ours, mapped, its header written. */
static status_t ring_make(handle_t *vmo, void **map, struct netdev_end *e, uint32_t kind,
                          bool producer)
{
    status_t st = drv_vmo_create(NETDEV_RING_BYTES, 0, vmo);
    if (st == OK)
        st = drv_vmo_map(*vmo, 0, NETDEV_RING_BYTES, VMAR_READ | VMAR_WRITE, map);
    if (st == OK)
        netdev_end_make(e, *map, kind, producer);
    return st;
}

/* Our side of a new session into t->s (channel, rings, events, port
 * bindings); *peer is the session channel's other end. */
static status_t session_make(struct e1k *t, handle_t *peer)
{
    struct session *s = &t->s;
    t->gen++;
    status_t st = drv_channel_create(&s->ch, peer);
    if (st != OK) {
        s->ch = HANDLE_INVALID;
        return st;
    }
    if (st == OK)
        st = ring_make(&s->tx_vmo, &s->tx_map, &s->tx, NETDEV_RING_TX, false);
    if (st == OK)
        st = ring_make(&s->rx_vmo, &s->rx_map, &s->rx, NETDEV_RING_RX, true);
    if (st == OK)
        st = drv_event_create(&s->to_driver);
    if (st == OK)
        st = drv_event_create(&s->to_stack);
    if (st == OK)
        st = drv_port_bind(t->port, s->ch, session_key(t, KEY_SESSION),
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = drv_port_bind(t->port, s->to_driver, session_key(t, KEY_TX), NETDEV_SIG_TX,
                           PORT_BIND_PERSISTENT);
    if (st != OK) {
        drv_handle_close(*peer);
        session_free(s);
    }
    return st;
}

/* Is the open session's opener gone? Then it ends now. */
static void reap_orphan(struct e1k *t)
{
    signals_t seen = 0;
    if (t->s.ch != HANDLE_INVALID &&
        drv_object_wait_one(t->s.ch, SIG_PEER_CLOSED, 0, &seen) == OK &&
        (seen & SIG_PEER_CLOSED))
        session_end(t, "its opener has gone");
}

static status_t do_open(void *ctx, handle_t *out_session, handle_t *out_tx, handle_t *out_rx,
                        handle_t *out_to_driver, handle_t *out_to_stack)
{
    struct e1k *t = ctx;
    reap_orphan(t);
    if (t->s.ch != HANDLE_INVALID)
        return ERR_BAD_STATE;
    handle_t peer;
    status_t st = session_make(t, &peer);
    if (st != OK)
        return st == ERR_NO_RESOURCES ? st : ERR_NO_MEMORY;
    struct session *s = &t->s;
    handle_t h[4] = { HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID };
    st = drv_handle_duplicate(s->tx_vmo, NETDEV_RING_RIGHTS, &h[0]);
    if (st == OK)
        st = drv_handle_duplicate(s->rx_vmo, NETDEV_RING_RIGHTS, &h[1]);
    if (st == OK)
        st = drv_handle_duplicate(s->to_driver, NETDEV_TO_DRIVER_RIGHTS, &h[2]);
    if (st == OK)
        st = drv_handle_duplicate(s->to_stack, NETDEV_TO_STACK_RIGHTS, &h[3]);
    if (st != OK) {
        for (unsigned i = 0; i < 4; i++)
            if (h[i] != HANDLE_INVALID)
                drv_handle_close(h[i]);
        drv_handle_close(peer);
        session_free(s);
        return ERR_NO_RESOURCES;
    }
    (void)netdev_sleep(&s->tx);   /* waiting for frames: netstack signals the first */
    t->st.sessions++;
    if (session_line(t))
        drv_log("session %lu opened (link %s)", (unsigned long)t->st.sessions, t->link ? "up" : "down");
    *out_session = peer;
    *out_tx = h[0];
    *out_rx = h[1];
    *out_to_driver = h[2];
    *out_to_stack = h[3];
    return OK;
}

static status_t do_info(void *ctx, uint8_t out_mac[6], uint16_t *out_vlan, uint16_t *out_mtu,
                        uint32_t *out_link, uint32_t *out_speed, uint32_t *out_changes,
                        uint8_t out_chip[16])
{
    struct e1k *t = ctx;
    (void)chip_link_poll(t);   /* fresh, and a change is signalled as usual */
    for (unsigned i = 0; i < 6; i++)
        out_mac[i] = t->mac[i];
    *out_vlan = t->vlan;
    *out_mtu = NETDEV_MTU;
    *out_link = (t->link ? NETDEV_LINK_UP : 0) | (t->link && t->full ? NETDEV_LINK_FULL : 0);
    *out_speed = t->speed;
    *out_changes = (uint32_t)t->st.link_changes;
    static const char name[] = CHIP_NAME;
    for (unsigned i = 0; i < sizeof(name); i++)
        out_chip[i] = (uint8_t)name[i];
    return OK;
}

static status_t do_stats(void *ctx, uint8_t out_counts[256])
{
    struct e1k *t = ctx;
    chip_counters(t);
    struct netdev_stats x = t->st;
    x.ring_errors = t->ring_errors_done + t->s.tx.errors + t->s.rx.errors;
    x.chip_counted = NETDEV_CHIP_TX_OK | NETDEV_CHIP_RX_OK | NETDEV_CHIP_TX_ERR |
                     NETDEV_CHIP_RX_ERR | NETDEV_CHIP_RX_MISSED;
    x.chip_tx_ok = t->chip.tx_ok;
    x.chip_rx_ok = t->chip.rx_ok;
    x.chip_tx_err = t->chip.tx_err;
    x.chip_rx_err = t->chip.rx_err;
    x.chip_rx_missed = t->chip.missed;
    __builtin_memcpy(out_counts, &x, sizeof(x));
    return OK;
}

static const struct netdev_ops serve_ops = { .info = do_info, .stats = do_stats,
                                             .open = do_open };
/* On the session channel: open is ERR_NOT_SUPPORTED (no handler). */
static const struct netdev_ops session_ops = { .info = do_info, .stats = do_stats };

/* One message from ch: OK once handled, else drv_channel_read's status. */
static status_t serve_one(struct e1k *t, handle_t ch, const struct netdev_ops *ops)
{
    _Alignas(8) uint8_t q[NETDEV_REQ_MAX + 8];
    _Alignas(8) uint8_t r[NETDEV_REP_MAX];
    handle_t hs[IDL_READ_HANDLES];
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(ch, q, sizeof(q), &n, hs, IDL_READ_HANDLES, &nh);
    if (st == ERR_BUFFER_TOO_SMALL)
        return idl_drain(ch, n, nh);
    if (st != OK)
        return st;
    if (nh) {
        idl_close_all(hs, nh);
        idl_reply_status(ch, q, n, ERR_INVALID_ARGS);
        return OK;
    }
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    uint32_t rn = netdev_dispatch(ops, t, q, n, r, rhs, &rhn);
    if (!rn || drv_channel_write(ch, r, rn, rhs, rhn) != OK)
        idl_close_all(rhs, rhn);   /* not sent: they're still ours */
    return OK;
}

void serve_some(struct e1k *t, bool session)
{
    bool *pending = session ? &t->s.pending : &t->serve_pending;
    *pending = false;
    for (int i = 0; i < DRAIN_MAX; i++) {
        handle_t ch = session ? t->s.ch : t->serve;
        if (ch == HANDLE_INVALID)
            return;
        status_t st = serve_one(t, ch, session ? &session_ops : &serve_ops);
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED && session)
            session_end(t, "its channel closed");
        else if (st == ERR_PEER_CLOSED)
            t->serve_closed = true;   /* devmgr is stopping us */
        else if (st != ERR_SHOULD_WAIT)
            drv_log("reading a channel: %s", status_str(st));
        return;
    }
    *pending = true;   /* more may be queued: the binding fires on edges only */
}
