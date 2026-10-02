/* rtl8125: full mode's receive path (drv/rtl8125).
 *
 * The chip keeps every tag (its tag stripping is off), so the driver sees
 * each frame as it was on the wire and keeps only what netframe_rx_check
 * keeps: 802.1Q-tagged with the configured VLAN, any priority. Untagged
 * frames (the trunk's native VLAN), priority-tagged ones (VLAN 0), other
 * VLANs, outer tags and a tag inside ours are dropped, each counted under
 * its reason; so are runts and frames too long. A kept frame loses its
 * tag on the way out (netframe_untag) into the driver's scratch copy and
 * goes to t->on_frame (the send test; later the netdev ring).
 *
 * To decide, the driver reads a frame's length (from the descriptor) and
 * bytes 12-17 (copied once out of the buffer). The buffer is the
 * driver's from the moment the chip clears the ownership bit until
 * ring_rx_done gives it back, so the chip can't change it in between. */
#include "rtl8125.h"

#define RX_TICK_NS (10 * NS_PER_S)   /* rx_tick's line: at most one in 10 s */

static const char *const reason[NETFRAME_RX_KINDS] = {
    [NETFRAME_RX_KEEP] = "kept", [NETFRAME_RX_RUNT] = "runt", [NETFRAME_RX_LONG] = "too long",
    [NETFRAME_RX_UNTAGGED] = "untagged", [NETFRAME_RX_PRIORITY] = "vlan 0",
    [NETFRAME_RX_OTHER_VLAN] = "other vlans", [NETFRAME_RX_OUTER] = "outer tag",
    [NETFRAME_RX_NESTED] = "a tag inside ours",
};

static void keep_or_drop(struct rtl *t, const struct rx_slot *s, uint64_t at)
{
    uint8_t head[NETFRAME_TAGGED] = { 0 };
    for (unsigned k = 12; k < NETFRAME_TAGGED && k < s->len; k++)
        head[k] = s->buf[k];
    enum netframe_rx v = netframe_rx_check(head, s->len, t->vlan);
    if (v != NETFRAME_RX_KEEP) {
        t->rx.drop[v]++;
        return;
    }
    /* The buffer is ours until ring_rx_done (above), so it is read as
     * plain memory: casting away volatile is safe here. */
    size_t n = netframe_untag(t->frame, sizeof(t->frame), (const uint8_t *)s->buf, s->len);
    if (!n) {
        t->rx.drop[NETFRAME_RX_LONG]++;   /* can't happen: rx_check bounded the length */
        return;
    }
    t->rx.kept++;
    if (t->on_frame)
        t->on_frame(t, t->frame, n, at);
    else
        t->rx.unused++;
}

unsigned rx_harvest(struct rtl *t)
{
    uint64_t at = drv_clock_ns();
    unsigned n = 0;
    struct rx_slot s;
    for (; n < RX_DESCS && ring_rx_peek(t, &s); n++) {
        t->rx.taken++;
        if (s.status & RX_ERRSUM)
            t->rx.errors++;
        else if (!s.whole)
            t->rx.split++;
        else
            keep_or_drop(t, &s, at);
        ring_rx_done(t);
    }
    return n;
}

void rx_log(const struct rtl *t)
{
    char drops[160];
    size_t len = 0;
    drops[0] = 0;
    for (unsigned k = NETFRAME_RX_RUNT; k < NETFRAME_RX_KINDS && len < sizeof(drops); k++)
        len += (size_t)drv_snprintf(drops + len, sizeof(drops) - len, "%s%s %u",
                                    k == NETFRAME_RX_RUNT ? "" : ", ", reason[k], t->rx.drop[k]);
    drv_log("rx on vlan %u: %u kept (%u with nobody to take them); dropped: %s; %u with the "
            "error bit, %u split", t->vlan, t->rx.kept, t->rx.unused, drops, t->rx.errors,
            t->rx.split);
}

/* Where the received frames went, next to tx_tick's "tx so far": what the
 * chip handed back and its own tally (frames it took in and frames it
 * missed for want of a descriptor), what the driver kept and dropped, what
 * went to netstack's ring, the descriptors the chip holds (all 256 while
 * the driver keeps up), the receive interrupts that say the chip ran out,
 * and descriptors whose address field the chip changed (rxdesc.h). A
 * receive that stops shows here as counts that stand still. */
void rx_tick(struct rtl *t)
{
    if (t->mode != RTL_MODE_FULL)
        return;
    uint64_t now = drv_clock_ns();
    if (now - t->rx_tick_at < RX_TICK_NS || t->rx.taken == t->rx_tick_taken)
        return;
    t->rx_tick_at = now;
    t->rx_tick_taken = t->rx.taken;
    char chip[48] = "tally unread";
    struct tally x;
    if (t->tally0_ok && tally_dump(t, &x) == OK)
        drv_snprintf(chip, sizeof(chip), "tally %lu, %u missed",
                     (unsigned long)(x.rx_ok - t->tally_rx0), (uint16_t)(x.miss - t->tally_miss0));
    const uint32_t *d = t->rx.drop;
    const struct netdev_stats *s = t->srv ? &t->srv->st : NULL;
    /* two lines: a log line holds about 240 characters */
    drv_log("rx so far: %u from the chip (%s), %u kept; dropped %u untagged, %u other vlans, %u "
            "vlan 0, %u bad; %lu to netstack, %lu ring full, %lu no session", t->rx.taken, chip,
            t->rx.kept, d[NETFRAME_RX_UNTAGGED],
            d[NETFRAME_RX_OTHER_VLAN] + d[NETFRAME_RX_OUTER] + d[NETFRAME_RX_NESTED],
            d[NETFRAME_RX_PRIORITY],
            d[NETFRAME_RX_RUNT] + d[NETFRAME_RX_LONG] + t->rx.errors + t->rx.split,
            (unsigned long)(s ? s->rx_frames : 0), (unsigned long)(s ? s->rx_ring_full : 0),
            (unsigned long)(s ? s->rx_no_session : 0));
    drv_log("rx ring: %u of %u descriptors the chip's, next %u; %u no-descriptor and %u "
            "fifo-overflow interrupts; %u address field(s) changed by the chip (first %#lx)",
            ring_rx_owned(t), RX_DESCS, t->next, t->ev.rdu_irqs, t->ev.rx_oflow_irqs,
            t->rx.addr_changed, (unsigned long)t->rx.addr_first);
}
