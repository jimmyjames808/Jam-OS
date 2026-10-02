/* rtl8125: the netdev server (drv/rtl8125): the netdev protocol on
 * DR_SERVE and on a session's channel, the session's rings and events,
 * and the frames between them and the driver (server.h has the model).
 *
 * The rings are netstack's to write at any moment, so everything read
 * from them goes through <jam/netdev.h>'s ring code: its counts read once
 * and clamped, a tx slot's length and flags read once and checked before
 * that many bytes are copied into v->frame, and the frame sent from that
 * copy (the driver then copies it again into its own DMA buffer, where it
 * is tagged and checked: tx.c). A bad slot is skipped and counted, never
 * sent. The driver never waits for room in the rx ring: a full one drops
 * the frame and counts it. */
#include <jam/netframe.h>
#include "server.h"

#define BUDGET   64u      /* requests taken from one channel per srv_work */

static void session_free(struct srv_session *s)
{
    if (s->txmap)
        (void)drv_vmo_unmap(s->txmap, NETDEV_RING_BYTES);   /* nothing to do if it fails */
    if (s->rxmap)
        (void)drv_vmo_unmap(s->rxmap, NETDEV_RING_BYTES);
    handle_t hs[] = { s->ch, s->txv, s->rxv, s->to_driver, s->to_stack };
    for (unsigned i = 0; i < sizeof(hs) / sizeof(hs[0]); i++)
        if (hs[i] != HANDLE_INVALID)
            drv_handle_close(hs[i]);   /* the port bindings go with the handles */
    *s = (struct srv_session){ 0 };
}

static void session_end(struct srv *v, const char *why)
{
    if (!v->open)
        return;
    v->ring_errors_past += v->s.tx.errors + v->s.rx.errors;
    session_free(&v->s);
    v->open = false;
    v->gen++;   /* packets of the old session's keys are ignored from now */
    v->session_ready = v->tx_ready = v->tx_blocked = v->rx_dirty = false;
    drv_log("netdev: the session ended (%s)", why);
}

/* Is the session channel's client gone? (A wait that is over at once.) */
static bool session_orphaned(const struct srv *v)
{
    signals_t seen = 0;
    return drv_object_wait_one(v->s.ch, SIG_PEER_CLOSED, 0, &seen) == OK &&
           (seen & SIG_PEER_CLOSED);
}

/* One ring: a new VMO, mapped, its header written, and a handle for
 * netstack with NETDEV_RING_RIGHTS. */
static status_t make_ring(handle_t *vmo, uint8_t **map, handle_t *out)
{
    status_t st = drv_vmo_create(NETDEV_RING_BYTES, 0, vmo);
    if (st == OK)
        st = drv_vmo_map(*vmo, 0, NETDEV_RING_BYTES, VMAR_READ | VMAR_WRITE, (void **)map);
    if (st == OK)
        st = drv_handle_duplicate(*vmo, NETDEV_RING_RIGHTS, out);
    return st;
}

/* Everything open hands out. On failure the caller frees s and out[]. */
static status_t make_session(struct srv *v, struct srv_session *s, handle_t out[5])
{
    status_t st = drv_channel_create(&s->ch, &out[0]);
    if (st == OK)
        st = make_ring(&s->txv, &s->txmap, &out[1]);
    if (st == OK)
        st = make_ring(&s->rxv, &s->rxmap, &out[2]);
    if (st == OK)
        st = drv_event_create(&s->to_driver);
    if (st == OK)
        st = drv_handle_duplicate(s->to_driver, NETDEV_TO_DRIVER_RIGHTS, &out[3]);
    if (st == OK)
        st = drv_event_create(&s->to_stack);
    if (st == OK)
        st = drv_handle_duplicate(s->to_stack, NETDEV_TO_STACK_RIGHTS, &out[4]);
    uint32_t gen = v->gen;
    if (st == OK)
        st = drv_port_bind(v->port, s->ch, SRV_KEY_SESSION | (uint64_t)gen << 8,
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = drv_port_bind(v->port, s->to_driver, SRV_KEY_TX | (uint64_t)gen << 8, NETDEV_SIG_TX,
                           PORT_BIND_PERSISTENT);
    if (st != OK)
        return st;
    netdev_end_make(&s->tx, s->txmap, NETDEV_RING_TX, false);
    netdev_end_make(&s->rx, s->rxmap, NETDEV_RING_RX, true);
    return OK;
}

static status_t op_open(void *ctx, handle_t *session, handle_t *tx, handle_t *rx,
                        handle_t *to_driver, handle_t *to_stack)
{
    struct srv *v = ctx;
    if (v->open && !session_orphaned(v))
        return ERR_BAD_STATE;
    session_end(v, "its opener has gone; a new open");
    struct srv_session s = { 0 };
    handle_t out[5] = { HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID,
                        HANDLE_INVALID };
    status_t st = make_session(v, &s, out);
    if (st != OK) {
        session_free(&s);
        for (unsigned i = 0; i < 5; i++)
            if (out[i] != HANDLE_INVALID)
                drv_handle_close(out[i]);
        drv_log("netdev: open failed (%s)", status_str(st));
        return st == ERR_NO_MEMORY || st == ERR_NO_RESOURCES ? ERR_NO_MEMORY : st;
    }
    v->s = s;
    v->open = true;
    v->session_ready = v->tx_ready = true;   /* look once: nothing is lost before the first wait */
    v->st.sessions++;
    *session = out[0];
    *tx = out[1];
    *rx = out[2];
    *to_driver = out[3];
    *to_stack = out[4];
    drv_log("netdev: session %lu open", (unsigned long)v->st.sessions);
    return OK;
}

static status_t op_info(void *ctx, uint8_t mac[6], uint16_t *vlan, uint16_t *mtu, uint32_t *link,
                        uint32_t *speed, uint32_t *changes, uint8_t chip[16])
{
    struct srv *v = ctx;
    struct srv_info i = { 0 };
    v->dev->info(v->ctx, &i);
    for (unsigned k = 0; k < 6; k++)
        mac[k] = i.mac[k];
    for (unsigned k = 0; k < 16; k++)
        chip[k] = (uint8_t)i.chip[k];
    *vlan = i.vlan;
    *mtu = NETDEV_MTU;
    *link = i.link;
    *speed = i.speed;
    *changes = i.changes;
    return OK;
}

static status_t op_stats(void *ctx, uint8_t counts[256])
{
    struct srv *v = ctx;
    struct netdev_stats s = v->st;
    s.ring_errors = v->ring_errors_past + (v->open ? v->s.tx.errors + v->s.rx.errors : 0);
    v->dev->stats(v->ctx, &s);
    for (unsigned k = 0; k < sizeof(s.reserved) / sizeof(s.reserved[0]); k++)
        s.reserved[k] = 0;
    __builtin_memcpy(counts, &s, NETDEV_STATS_SIZE);
    return OK;
}

/* DR_SERVE: every method. A session channel: open is refused. */
static const struct netdev_ops serve_ops = { .info = op_info, .stats = op_stats, .open = op_open };
static const struct netdev_ops session_ops = { .info = op_info, .stats = op_stats };

status_t srv_init(struct srv *v, handle_t port, handle_t serve, const struct srv_dev *dev,
                  void *ctx)
{
    *v = (struct srv){ .port = port, .serve = serve, .dev = dev, .ctx = ctx, .gen = 1 };
    v->serve_ready = true;   /* requests may be queued from before a restart */
    return drv_port_bind(port, serve, SRV_KEY_SERVE, SIG_READABLE, PORT_BIND_PERSISTENT);
}

bool srv_packet(struct srv *v, const struct port_packet *p)
{
    uint32_t kind = (uint32_t)(p->key & 0xff);
    bool current = (p->key >> 8) == v->gen && v->open;
    if (kind == SRV_KEY_SERVE && p->key == SRV_KEY_SERVE)
        v->serve_ready = true;
    else if (kind == SRV_KEY_SESSION)
        v->session_ready |= current;
    else if (kind == SRV_KEY_TX)
        v->tx_ready |= current;
    else
        return false;
    return true;
}

/* Up to BUDGET requests from ch. True: the budget ran out (more may be
 * there). *closed: the channel's clients are all gone. */
static bool serve_some(handle_t ch, const struct netdev_ops *ops, void *ctx, bool *closed)
{
    for (unsigned n = 0; n < BUDGET; n++) {
        status_t st = netdev_serve_one(ch, ops, ctx);
        if (st == OK)
            continue;
        *closed = st != ERR_SHOULD_WAIT;   /* ERR_PEER_CLOSED, or the channel broke */
        return false;
    }
    return true;
}

/* One frame out of the tx ring and to the driver, counted either way. */
static void tx_one(struct srv *v)
{
    uint32_t len = 0;
    status_t st = netdev_take(&v->s.tx, v->frame, sizeof(v->frame), &len);
    if (st == ERR_INVALID_ARGS) {
        v->st.tx_bad_flags++;
        return;
    }
    if (st != OK) {
        v->st.tx_bad_len++;
        return;
    }
    st = v->dev->send(v->ctx, v->frame, len);
    if (st == ERR_INVALID_ARGS) {
        v->st.tx_bad_tag++;   /* the driver's check refused it (a tag EtherType) */
        return;
    }
    if (st != OK) {
        v->tx_failed++;       /* not sent for another reason (the transmitter off) */
        return;
    }
    v->st.tx_frames++;
    v->st.tx_bytes += len + NETFRAME_TAG_LEN;
}

/* The tx ring: the event's bit cleared first (a signal meanwhile is kept),
 * then every frame waiting, as far as there are descriptors. */
static void tx_pass(struct srv *v)
{
    v->tx_ready = false;
    (void)drv_event_signal(v->s.to_driver, NETDEV_SIG_TX, 0);   /* our own event: can't fail */
    netdev_awake(&v->s.tx);
    uint32_t n = netdev_ready(&v->s.tx), taken = 0;
    for (; taken < n; taken++) {
        if (!v->dev->room(v->ctx)) {
            v->tx_blocked = true;
            break;
        }
        tx_one(v);
    }
    if (taken && netdev_publish(&v->s.tx))
        (void)drv_event_signal(v->s.to_stack, 0, NETDEV_SIG_TX_ROOM);
    if (v->tx_blocked)
        return;   /* no flag raised: srv_tx_room comes back when descriptors do */
    if (!netdev_sleep(&v->s.tx))
        v->tx_ready = true;   /* frames came meanwhile: another pass, after the port */
}

bool srv_work(struct srv *v)
{
    if (v->serve_ready) {
        bool closed = false;
        v->serve_ready = serve_some(v->serve, &serve_ops, v, &closed);
        if (closed && !v->stopping) {
            v->stopping = true;
            drv_log("netdev: DR_SERVE's clients are gone");
        }
    }
    if (v->open && v->session_ready) {
        bool closed = false;
        v->session_ready = serve_some(v->s.ch, &session_ops, v, &closed);
        if (closed)
            session_end(v, "its channel closed");
    }
    if (v->open && v->tx_ready && !v->tx_blocked)
        tx_pass(v);
    return !v->stopping && (v->serve_ready || (v->open && (v->session_ready ||
                                                           (v->tx_ready && !v->tx_blocked))));
}

void srv_rx(struct srv *v, const uint8_t *frame, size_t len)
{
    if (!v->open) {
        v->st.rx_no_session++;
        return;
    }
    if (len < NETDEV_FRAME_MIN || len > NETDEV_FRAME_MAX) {
        v->st.rx_bad++;
        return;
    }
    if (!netdev_room(&v->s.rx)) {
        v->st.rx_ring_full++;
        return;
    }
    netdev_put(&v->s.rx, frame, (uint32_t)len);
    v->st.rx_frames++;
    v->st.rx_bytes += len;
    v->rx_dirty = true;
}

void srv_rx_done(struct srv *v)
{
    if (!v->open || !v->rx_dirty)
        return;
    v->rx_dirty = false;
    if (netdev_publish(&v->s.rx))
        (void)drv_event_signal(v->s.to_stack, 0, NETDEV_SIG_RX);
}

void srv_tx_room(struct srv *v)
{
    if (v->open && v->tx_blocked && v->dev->room(v->ctx)) {
        v->tx_blocked = false;
        v->tx_ready = true;
    }
}

void srv_link(struct srv *v)
{
    v->st.link_changes++;
    if (v->open)
        (void)drv_event_signal(v->s.to_stack, 0, NETDEV_SIG_LINK);
}

void srv_end(struct srv *v)
{
    session_end(v, "the driver is stopping");
}

void srv_log(const struct srv *v)
{
    const struct netdev_stats *s = &v->st;
    drv_log("netdev: %lu session(s); rx %lu frame(s) to netstack, %lu with no session, %lu with "
            "the ring full; tx %lu frame(s) from netstack, refused %lu by length, %lu by flags, "
            "%lu by the tag check, %lu not sent; ring errors %lu", (unsigned long)s->sessions,
            (unsigned long)s->rx_frames, (unsigned long)s->rx_no_session,
            (unsigned long)s->rx_ring_full, (unsigned long)s->tx_frames,
            (unsigned long)s->tx_bad_len, (unsigned long)s->tx_bad_flags,
            (unsigned long)s->tx_bad_tag, (unsigned long)v->tx_failed,
            (unsigned long)(v->ring_errors_past + (v->open ? v->s.tx.errors + v->s.rx.errors
                                                           : 0)));
}
