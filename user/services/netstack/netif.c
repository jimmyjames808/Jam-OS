/* netstack: the network card's session, in the loop (dev.h has the
 * model). stack.h's edge runs on the session's rings: a frame lwIP sends
 * goes into the tx ring (dropped and counted when it is full: netstack
 * never waits for the driver), and every frame in the rx ring goes to
 * stack_input. Counts are published once a turn (dev_work), and the
 * driver is woken only if it said it sleeps.
 *
 * The to_stack event is bound ONCE and watched again after each wake; its
 * bits are cleared before the rings are looked at, so a signal that comes
 * meanwhile is kept for the next wait. A session's port keys carry its
 * generation, so a packet left from an ended session is ignored. */
#include <devmgr.h>
#include <idl/netdev.h>
#include "dev.h"
#include "stack.h"

#define TO_STACK_BITS (NETDEV_SIG_RX | NETDEV_SIG_TX_ROOM | NETDEV_SIG_LINK)

static uint8_t rxbuf[NETDEV_FRAME_MAX];   /* the frame being taken off the rx ring */

static uint64_t key_of(const struct dev *d, uint32_t base)
{
    return base | (uint64_t)d->gen << 8;
}

/* stack.h's tx on the session's tx ring. */
static status_t ring_tx(void *ctx, const uint8_t *frame, size_t len)
{
    struct dev *d = ctx;
    if (!d->session)
        return ERR_BAD_STATE;
    if (len < NETDEV_FRAME_MIN || len > NETDEV_FRAME_MAX)
        return ERR_OUT_OF_RANGE;
    if (!netdev_room(&d->tx)) {
        d->rep.tx_full++;
        return ERR_NO_RESOURCES;
    }
    netdev_put(&d->tx, frame, (uint32_t)len);
    d->tx_dirty = true;
    return OK;
}

/* The edge with no session: nothing goes out, the card's MAC kept. */
static void edge_off(const struct dev *d)
{
    struct stack_edge e = stack_no_device;
    if (d->found.st == OK)
        memcpy(e.mac, d->found.mac, sizeof(e.mac));
    stack_set_edge(&e);
    stack_set_link(false);
}

static status_t map_ring(handle_t vmo, void **out)
{
    uint64_t va = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, NETDEV_RING_BYTES,
                               VMAR_READ | VMAR_WRITE, &va);
    if (st == OK)
        *out = (void *)(uintptr_t)va;
    return st;
}

static void unmap_ring(void *map)
{
    if (map)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)map,
                             NETDEV_RING_BYTES);   /* nothing left to undo if it fails */
}

/* Plan the next connect: soon after a session that worked (backoff 0),
 * later each time one fails in a row. */
static void retry_later(struct dev *d)
{
    d->backoff = d->backoff ? d->backoff * 2 : DEV_RETRY_MIN;
    if (d->backoff > DEV_RETRY_MAX)
        d->backoff = DEV_RETRY_MAX;
    d->retry_at = now() + d->backoff;
}

/* Drop the session (its handles, mappings and bindings) and plan a new one. */
static void detach(struct dev *d, const char *why)
{
    if (!d->session)
        return;
    (void)jam_port_unbind(d->port, d->to_stack, key_of(d, KEY_EVENT));   /* may have fired */
    handle_t hs[] = { d->session, d->tx_vmo, d->rx_vmo, d->to_driver, d->to_stack };
    for (unsigned k = 0; k < DEV_HANDLES; k++)
        jam_handle_close(hs[k]);   /* the session's binding goes with its only handle */
    unmap_ring(d->tx_map);
    unmap_ring(d->rx_map);
    d->session = d->tx_vmo = d->rx_vmo = d->to_driver = d->to_stack = HANDLE_INVALID;
    d->tx_map = d->rx_map = NULL;
    d->gen++;
    d->rx_pending = d->tx_dirty = d->info_out = false;
    d->rep.session = false;
    d->rep.speed = 0;
    d->link_up = false;
    edge_off(d);
    nstack_log("the network driver's session ended (%s): asking for a new one", why);
    retry_later(d);
}

/* No session: say why (once per reason) and try again later. */
static void failed(struct dev *d, int32_t st)
{
    if (st != d->last_st)
        nstack_log("no session with the network driver yet (%s): trying again",
                   status_str(st));
    d->last_st = st;
    retry_later(d);
}

static void set_link(struct dev *d, uint32_t link, uint32_t speed)
{
    bool up = link & NETDEV_LINK_UP;
    if (up && (!d->link_up || speed != d->rep.speed))
        nstack_log("link up, %u Mb/s", speed);
    else if (!up && d->link_up)
        nstack_log("link down");
    d->link_up = up;
    d->rep.speed = up ? speed : 0;
    stack_set_link(up);
}

/* The thread's session: its rings mapped and checked, watched, the edge
 * on it. Takes the handles. */
static status_t attach(struct dev *d, const struct dev_found *f, const handle_t *hs)
{
    d->session = hs[DEV_H_SESSION];
    d->tx_vmo = hs[DEV_H_TX];
    d->rx_vmo = hs[DEV_H_RX];
    d->to_driver = hs[DEV_H_TO_DRIVER];
    d->to_stack = hs[DEV_H_TO_STACK];
    d->found = *f;
    d->gen++;
    status_t st = map_ring(d->tx_vmo, &d->tx_map);
    if (st == OK)
        st = map_ring(d->rx_vmo, &d->rx_map);
    if (st == OK && (!netdev_end_attach(&d->tx, d->tx_map, NETDEV_RING_TX, true) ||
                     !netdev_end_attach(&d->rx, d->rx_map, NETDEV_RING_RX, false)))
        st = ERR_INVALID_ARGS;   /* not netdev's ring layout */
    if (st == OK)
        st = jam_port_bind(d->port, d->session, key_of(d, KEY_SESSION),
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = jam_port_bind(d->port, d->to_stack, key_of(d, KEY_EVENT), TO_STACK_BITS,
                           PORT_BIND_ONCE);
    d->rep.session = true;   /* so detach undoes it all */
    if (st != OK) {
        detach(d, status_str(st));
        return st;
    }
    struct stack_edge e = { .tx = ring_tx, .ctx = d };
    memcpy(e.mac, f->mac, sizeof(e.mac));
    stack_set_edge(&e);
    d->rep.sessions++;
    d->rep.vlan = f->vlan;
    memcpy(d->rep.chip, f->chip, sizeof(d->rep.chip));
    d->changes = f->changes;
    d->rx_pending = true;   /* frames may be there already */
    d->backoff = 0;
    d->last_st = OK;
    nstack_log("on %s, VLAN %u, MAC %02x:%02x:%02x:%02x:%02x:%02x", f->chip[0] ? f->chip : "?",
               f->vlan, f->mac[0], f->mac[1], f->mac[2], f->mac[3], f->mac[4], f->mac[5]);
    set_link(d, f->link, f->speed);
    return OK;
}

/* The thread's answers. */
static void connect_reply(struct dev *d)
{
    for (unsigned guard = 0; guard < 4; guard++) {
        struct dev_found f;
        handle_t hs[DEV_HANDLES];
        uint32_t n = 0, nh = 0;
        status_t st = drv_channel_read(d->to_thread, &f, sizeof(f), &n, hs, DEV_HANDLES, &nh);
        if (st != OK)
            return;
        d->asked = false;
        if (n != sizeof(f) || (f.st == OK) != (nh == DEV_HANDLES) || d->session) {
            for (unsigned k = 0; k < nh; k++)
                jam_handle_close(hs[k]);
            continue;   /* not the thread's format, or one session already */
        }
        if (f.st == OK)
            (void)attach(d, &f, hs);   /* a failure is said and retried there */
        else
            failed(d, f.st);
    }
}

/* Ask the driver for the link, without waiting: the reply comes on the
 * session channel. */
static void ask_link(struct dev *d)
{
    static uint32_t last_txid;
    if (d->info_out)
        return;
    d->info_txid = idl_txid_next(&last_txid);
    d->info_out = netdev_info_send(d->session, d->info_txid) == OK;
}

/* Replies on the session channel, or its end. */
static void session_event(struct dev *d)
{
    for (unsigned guard = 0; guard < 8; guard++) {
        _Alignas(8) uint8_t rep[NETDEV_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(d->session, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st == ERR_PEER_CLOSED) {
            detach(d, "the driver closed it");
            return;
        }
        if (st != OK || !d->info_out || m.txid != d->info_txid) {
            idl_msg_drop(&m);   /* not ours to take */
            continue;
        }
        d->info_out = false;
        uint8_t mac[6], chip[16];
        uint16_t vlan, mtu;
        uint32_t link, speed, changes;
        if (netdev_info_result(rep, &m, mac, &vlan, &mtu, &link, &speed, &changes, chip) == OK) {
            d->changes = changes;
            set_link(d, link, speed);
        }
    }
}

/* The to_stack event fired: clear its bits, then look. */
static void to_stack_event(struct dev *d, signals_t seen)
{
    (void)jam_event_signal(d->to_stack, TO_STACK_BITS, 0);   /* ours: can't fail */
    if (seen & NETDEV_SIG_LINK)
        ask_link(d);
    d->rx_pending = true;
    if (jam_port_bind(d->port, d->to_stack, key_of(d, KEY_EVENT), TO_STACK_BITS,
                      PORT_BIND_ONCE) != OK)
        detach(d, "its event can't be watched");
}

void dev_packet(struct dev *d, const struct port_packet *p)
{
    uint32_t low = (uint32_t)(p->key & 0xff);
    bool current = d->session && (uint32_t)(p->key >> 8) == d->gen;
    if (p->key == KEY_CONNECT) {
        connect_reply(d);
    } else if (p->key == KEY_DEVMGR) {
        printf("netstack: devmgr is gone (and the network driver with it): ending, init "
               "starts me again with the new devmgr\n");
        jam_process_exit(3);
    } else if (low == KEY_SESSION && current) {
        session_event(d);
    } else if (low == KEY_EVENT && current) {
        to_stack_event(d, p->signal.observed);
    }
}

/* Up to DEV_RX_BUDGET frames off the rx ring into lwIP. A count out of
 * range takes nothing (the slots it would name hold no frames): the
 * session ends (rings_sane). */
static void drain_rx(struct dev *d)
{
    netdev_awake(&d->rx);
    uint64_t errors = d->rx.errors;
    uint32_t n = netdev_ready(&d->rx);
    if (d->rx.errors != errors)
        return;
    if (n > DEV_RX_BUDGET)
        n = DEV_RX_BUDGET;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len;
        if (netdev_take(&d->rx, rxbuf, sizeof(rxbuf), &len) == OK)
            stack_input(rxbuf, len);
        else
            d->rep.rx_bad++;
    }
    (void)netdev_publish(&d->rx);   /* the driver never waits for room in the rx ring */
    if (n == DEV_RX_BUDGET)
        d->rx_pending = true;       /* more may wait: after the loop's other work */
    else
        d->rx_pending = !netdev_sleep(&d->rx);
}

/* The driver's counts were in range: true. Else the rings can't be
 * trusted any more (one side's frames would be lost or made up), so the
 * session ends and a new one, with new rings, is asked for. */
static bool rings_sane(struct dev *d)
{
    uint64_t errors = d->tx.errors + d->rx.errors;
    if (!errors)
        return true;
    d->rep.ring_errors += errors;
    detach(d, "the driver's ring counts were out of range");
    return false;
}

uint64_t dev_work(struct dev *d)
{
    if (d->session && d->rx_pending)
        drain_rx(d);
    if (d->session && rings_sane(d) && d->tx_dirty) {
        d->tx_dirty = false;
        if (netdev_publish(&d->tx))
            (void)jam_event_signal(d->to_driver, 0, NETDEV_SIG_TX);   /* gone: its session
                                                                       * closes too */
    }
    if (d->session || d->asked || !d->retry_at || !d->ncards)
        return DEADLINE_NEVER;
    if (now() >= d->retry_at) {
        connect_ask(d);
        return DEADLINE_NEVER;
    }
    return d->retry_at;
}

bool dev_pending(const struct dev *d)
{
    return d->session && d->rx_pending;
}

void dev_get_report(const struct dev *d, struct dev_report *out)
{
    *out = d->rep;
}

status_t dev_init(struct dev *d, handle_t port)
{
    *d = (struct dev){ .port = port };
    for (unsigned i = 0; i < startup_handle_count(); i++) {
        uint32_t role;
        handle_t h = startup_handle_at(i, &role);
        if (role == SR_DEVMGR_DEVICE && d->ncards < DEV_CARDS)
            d->cards[d->ncards++] = h;
    }
    if (!d->ncards) {
        nstack_log("no network card (no device channel): the link stays down");
        return OK;
    }
    /* They all end with devmgr: one is enough to watch. */
    status_t st = jam_port_bind(port, d->cards[0], KEY_DEVMGR, SIG_PEER_CLOSED, PORT_BIND_ONCE);
    if (st == OK)
        st = connect_start(d);
    if (st == OK)
        st = jam_port_bind(port, d->to_thread, KEY_CONNECT, SIG_READABLE, PORT_BIND_PERSISTENT);
    if (st != OK)
        return st;
    connect_ask(d);
    return OK;
}
