/* rtl8125: full mode's service: the netdev server for netstack
 * (drv/rtl8125; the server itself is server.c, which knows nothing of the
 * chip).
 *
 * This file is the server's view of the card (struct srv_dev): netstack's
 * frames go to tx.c's tx_send and nowhere else, so each is copied into
 * the driver's own buffer, tagged with the VLAN and checked there before
 * the chip may read it; rx.c's kept frames (our VLAN only, untagged) go to
 * srv_rx; info and stats come from the driver's state and the chip's
 * tally counters. It runs until devmgr stops the driver (its channel
 * closes); a session ending (netstack gone or restarted) only ends the
 * session, and the next open makes a new one. */
#include "rtl8125.h"

struct full {
    struct rtl *t;
    const struct outcome *o;      /* the tally at the start, for the chip's counts */
    struct srv v;
};

static status_t dev_send(void *ctx, const uint8_t *frame, size_t len)
{
    struct full *f = ctx;
    return tx_send(f->t, frame, len);
}

static uint32_t dev_room(void *ctx)
{
    struct full *f = ctx;
    uint32_t pending = tx_pending(f->t);
    return pending < TX_DESCS - 1 ? TX_DESCS - 1 - pending : 0;
}

static void dev_info(void *ctx, struct srv_info *out)
{
    const struct rtl *t = ((struct full *)ctx)->t;
    bool full = false;
    uint32_t speed = t->link ? chip_link_speed(t->phystat, &full) : 0;
    *out = (struct srv_info){
        .vlan = t->vlan, .speed = speed, .changes = t->link_seq,
        .link = (t->link ? NETDEV_LINK_UP : 0) | (t->link && full ? NETDEV_LINK_FULL : 0),
        .chip = "RTL8125B",
    };
    for (unsigned i = 0; i < 6; i++)
        out->mac[i] = t->mac[i];
}

/* The driver's counts, added into s; the chip's since the driver started
 * (the tally now, less the one at the start: a short bounded DMA wait). */
static void dev_stats(void *ctx, struct netdev_stats *s)
{
    struct full *f = ctx;
    struct rtl *t = f->t;
    const uint32_t *d = t->rx.drop;
    s->rx_untagged += d[NETFRAME_RX_UNTAGGED];
    s->rx_priority += d[NETFRAME_RX_PRIORITY];
    s->rx_other_vlan += d[NETFRAME_RX_OTHER_VLAN] + d[NETFRAME_RX_OUTER] + d[NETFRAME_RX_NESTED];
    s->rx_bad += d[NETFRAME_RX_RUNT] + d[NETFRAME_RX_LONG] + t->rx.errors + t->rx.split;
    s->tx_done += t->tx.done;
    struct tally now;
    if (!f->o->start_ok || tally_dump(t, &now) != OK)
        return;   /* chip_counted stays 0: the chip's counts are unknown */
    const struct tally *a = &f->o->start;
    s->chip_counted = NETDEV_CHIP_TX_OK | NETDEV_CHIP_RX_OK | NETDEV_CHIP_TX_ERR |
                      NETDEV_CHIP_RX_ERR | NETDEV_CHIP_RX_MISSED;
    s->chip_tx_ok = now.tx_ok - a->tx_ok;
    s->chip_rx_ok = now.rx_ok - a->rx_ok;
    s->chip_tx_err = now.tx_err - a->tx_err;
    s->chip_rx_err = (uint32_t)(now.rx_err - a->rx_err);
    s->chip_rx_missed = (uint16_t)(now.miss - a->miss);
}

/* The one card this driver runs (full_report reads its server's counts). */
static struct full card;

static const struct srv_dev dev = {
    .send = dev_send, .room = dev_room, .info = dev_info, .stats = dev_stats,
};

static void on_frame(struct rtl *t, const uint8_t *frame, size_t len, uint64_t at)
{
    (void)at;
    srv_rx(t->srv, frame, len);
}

void full_run(struct rtl *t, struct outcome *o)
{
    card = (struct full){ .t = t, .o = o };
    status_t st = srv_init(&card.v, t->port, t->serve, &dev, &card);
    if (st != OK) {
        drv_log("netdev: can't serve DR_SERVE (%s): stopping", status_str(st));
        return;
    }
    t->srv = &card.v;
    t->on_frame = on_frame;
    t->link_told = t->link_seq;
    drv_log("netdev: serving on vlan %u: netstack's frames tagged by tx.c, ours kept by rx.c",
            t->vlan);
    while (loop_step(t, DEADLINE_NEVER))
        ;
    o->cut = true;   /* it ends only when devmgr stops it */
    t->on_frame = NULL;
    srv_end(&card.v);
    srv_log(&card.v);
    t->srv = NULL;
    rx_log(t);
    tx_log(t);
}

void full_report(const struct rtl *t, const struct outcome *o)
{
    char link[48];
    chip_link_summary(t, link, sizeof(link));
    const struct netdev_stats *s = &card.v.st;
    drv_report("netdev vlan %u, %s, %lu session(s), rx %lu to netstack (%lu with none, %lu ring "
               "full), tx %lu from netstack (%u queued), %s%s", t->vlan, link,
               (unsigned long)s->sessions, (unsigned long)s->rx_frames,
               (unsigned long)s->rx_no_session, (unsigned long)s->rx_ring_full,
               (unsigned long)s->tx_frames, t->tx.queued, o->txcheck,
               t->refused || t->tx.gate ? ", WRITES REFUSED" : "");
}
