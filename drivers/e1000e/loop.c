/* e1000e: the driver's one loop (drv/e1000e), and the card as the netdev
 * server sees it (struct srv_dev: <jam/netserver.h>; the server itself is
 * drivers/lib/netserver.c, shared by the network drivers).
 *
 * One thread, one port; everything it waits for arrives there:
 *   KEY_IRQ      the chip's MSI-X vector 0 (receive, transmit written
 *                back, link change: chip.c routes every cause to it),
 *                bound PERSISTENT: acked, then ICR read and cleared
 *   KEY_SERVE    DR_SERVE's peer closed: devmgr is stopping us
 *   the server's keys (SRV_KEY_FIRST and up): DR_SERVE readable, the
 *                session's channel, netstack's NETDEV_SIG_TX; srv_packet
 *                notes them and srv_work, at the top of each turn, does
 *                the work (each piece within a budget)
 * After srv_work, tx_flush rings the doorbell once for every frame it
 * queued. After the chip's work the server is told: the rx ring
 * published once per batch, transmit descriptors freed, the link
 * changed. Every wait has a deadline of at most a second, so a lost
 * interrupt costs time, never a stall: the rings and the link are looked
 * at then too, and the chip's 32-bit counters are added up before they
 * can wrap. Nothing waits inside a request (the service-loop rule,
 * CODING-GUIDE.md): a full rx ring drops and counts, and a busy channel
 * is served a budget's worth per turn.
 *
 * netstack's frames go to tx.c's tx_send and nowhere else (copied, tagged
 * and checked there); rx.c hands kept frames to srv_rx; info and stats
 * come from the driver's state and the chip's counters. */
#include "e1000e.h"

#define POLL_NS NS_PER_S

/* ---- the card, as the server sees it ------------------------------------------------ */

static status_t dev_send(void *ctx, const uint8_t *frame, size_t len)
{
    return tx_send(ctx, frame, len);
}

static uint32_t dev_room(void *ctx)
{
    return tx_room(ctx);
}

static void dev_info(void *ctx, struct srv_info *out)
{
    struct e1k *t = ctx;
    if (chip_link_poll(t))   /* fresh, and a change is signalled as usual */
        srv_link(&t->v);
    *out = (struct srv_info){
        .vlan = t->vlan, .speed = t->speed, .changes = t->link_changes,
        .link = (t->link ? NETDEV_LINK_UP : 0) | (t->link && t->full ? NETDEV_LINK_FULL : 0),
        .chip = CHIP_NAME,
    };
    for (unsigned i = 0; i < 6; i++)
        out->mac[i] = t->mac[i];
}

/* The driver's counts, added into s, and the chip's since it started. */
static void dev_stats(void *ctx, struct netdev_stats *s)
{
    struct e1k *t = ctx;
    const uint64_t *d = t->rx_drop;
    s->rx_untagged += d[NETFRAME_RX_UNTAGGED];
    s->rx_priority += d[NETFRAME_RX_PRIORITY];
    s->rx_other_vlan += d[NETFRAME_RX_OTHER_VLAN] + d[NETFRAME_RX_OUTER] + d[NETFRAME_RX_NESTED];
    s->rx_bad += d[NETFRAME_RX_RUNT] + d[NETFRAME_RX_LONG] + t->rx_errors;
    s->tx_done += t->tx_done;
    chip_counters(t);
    s->chip_counted = NETDEV_CHIP_TX_OK | NETDEV_CHIP_RX_OK | NETDEV_CHIP_TX_ERR |
                      NETDEV_CHIP_RX_ERR | NETDEV_CHIP_RX_MISSED;
    s->chip_tx_ok = t->chip.tx_ok;
    s->chip_rx_ok = t->chip.rx_ok;
    s->chip_tx_err = t->chip.tx_err;
    s->chip_rx_err = t->chip.rx_err;
    s->chip_rx_missed = t->chip.missed;
}

static const struct srv_dev dev = {
    .send = dev_send, .room = dev_room, .info = dev_info, .stats = dev_stats,
};

/* ---- the loop ------------------------------------------------------------------------ */

/* The rings and the link: whatever the chip has done since the last look. */
static void service(struct e1k *t)
{
    (void)rx_harvest(t);
    srv_rx_done(&t->v);   /* the rx ring published once per batch */
    rx_tick(t);
    if (tx_reap(t))
        srv_tx_room(&t->v);   /* descriptors came back: frames left waiting go on */
    if (chip_link_poll(t))
        srv_link(&t->v);
}

static void interrupt(struct e1k *t, uint64_t fires)
{
    t->irqs += fires;
    (void)drv_interrupt_ack(t->irq);   /* before the cause: a new fire is not lost */
    uint32_t icr = rd32(t, E1K_ICR);
    if (icr == 0xffffffffu)
        icr = 0;   /* the chip is gone: its registers read all ones */
    if (icr)
        wr32(t, E1K_ICR, icr);   /* write 1 to clear (with MSI-X a read doesn't) */
    t->icr_seen |= icr;
    t->rxo_irqs += !!(icr & ICR_RXO);
    t->rxdmt_irqs += !!(icr & ICR_RXDMT0);
    service(t);
}

static status_t loop_init(struct e1k *t)
{
    status_t st = drv_port_bind(t->port, t->serve, KEY_SERVE, SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st == OK && t->irq != HANDLE_INVALID)
        st = drv_port_bind(t->port, t->irq, KEY_IRQ, SIG_INTERRUPT, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = srv_init(&t->v, t->port, t->serve, &dev, t);
    return st;
}

status_t loop_run(struct e1k *t)
{
    status_t st = loop_init(t);
    if (st != OK) {
        drv_log("can't bind the port (%s)", status_str(st));
        return st;
    }
    char m[NETDEV_MODE_TEXT];
    drv_log("netdev: serving %s: netstack's frames %s by tx.c, ours kept by rx.c",
            netdev_mode_str(t->vlan, m),
            t->vlan == NETFRAME_MODE_UNTAGGED ? "checked untagged" : "tagged");
    uint64_t next_poll = drv_clock_ns() + POLL_NS;
    while (st == OK) {
        bool busy = srv_work(&t->v);
        tx_flush(t);   /* one doorbell for every frame srv_work queued */
        if (t->v.stopping)
            break;   /* DR_SERVE's clients are all gone */
        struct port_packet p;
        st = drv_port_wait(t->port, busy ? 0 : next_poll, &p);
        if (st == OK && p.key == KEY_SERVE) {
            break;   /* devmgr is stopping us */
        } else if (st == OK && p.key == KEY_IRQ) {
            interrupt(t, p.signal.count);
        } else if (st == OK) {
            (void)srv_packet(&t->v, &p);   /* the server's (an ended session's: ignored) */
        } else if (st == ERR_TIMED_OUT) {
            st = OK;
        } else {
            drv_log("port wait failed (%s)", status_str(st));
            break;
        }
        if (drv_clock_ns() >= next_poll) {
            t->polls++;
            service(t);
            chip_counters(t);
            next_poll = drv_clock_ns() + POLL_NS;
        }
    }
    return st;
}
