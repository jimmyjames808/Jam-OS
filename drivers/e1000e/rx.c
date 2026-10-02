/* e1000e: received frames (drv/e1000e).
 *
 * The chip's tag stripping (CTRL.VME) and VLAN filter (RCTL.VFE) are off,
 * so each frame arrives as it was on the wire, its CRC stripped
 * (RCTL.SECRC). The driver keeps only what netframe_rx_mode keeps: with a
 * VLAN, 802.1Q-tagged with it, any priority (untagged frames, priority
 * tags (VLAN 0), other VLANs, outer QinQ tags and a tag inside ours are
 * dropped and counted); untagged, only untagged frames (every tagged one,
 * VLAN 0 too, dropped and counted). Runts are dropped and counted too, as
 * are frames too long, frames the chip flagged and frames spread over
 * several buffers (none should be: RCTL.LPE is off, so nothing over 1522
 * bytes arrives, and a buffer is 2048).
 *
 * To decide, the driver reads a frame's length (from the descriptor) and
 * bytes 12-17 (copied once out of the buffer); a kept frame is copied
 * without its tag (untagged, as it is: netframe_rx_take) into the
 * driver's scratch copy and handed to the netdev server (srv_rx), which
 * puts it into the session's rx ring, or drops and counts it with no
 * session (rx_no_session) or the ring full (rx_ring_full): the driver
 * never waits for netstack. The loop publishes the ring once per batch
 * (srv_rx_done). The buffer is the driver's from the moment the chip sets
 * DD until the descriptor goes back with the tail (RDT), so the chip
 * can't change it in between. */
#include "e1000e.h"

#define RX_TICK_NS (10 * NS_PER_S)   /* rx_tick's line: at most one in 10 s */

/* A frame (len bytes at buf, as it was on the wire): to the server
 * untagged if it is ours, else counted under its reason (rx_drop, which
 * the stats add up: loop.c). */
static void keep_or_drop(struct e1k *t, const uint8_t *buf, uint32_t len)
{
    uint8_t head[NETFRAME_TAGGED] = { 0 };
    for (unsigned k = 12; k < NETFRAME_TAGGED && k < len; k++)
        head[k] = buf[k];
    enum netframe_rx v = netframe_rx_mode(head, len, t->vlan);
    t->rx_drop[v]++;
    if (v != NETFRAME_RX_KEEP)
        return;
    size_t n = netframe_rx_take(t->frame, sizeof(t->frame), buf, len, t->vlan);
    srv_rx(&t->v, t->frame, n);   /* n 0 can't happen (rx_check bounded it): rx_bad there */
}

/* One descriptor the chip has finished (status read once). */
static void take(struct e1k *t, volatile uint8_t *d, uint8_t status, uint32_t i)
{
    uint32_t len = *(volatile uint16_t *)(d + RXD_LEN);
    uint8_t errors = *(volatile uint8_t *)(d + RXD_ERRORS);
    bool eop = status & RXD_EOP, split = t->rx_split;
    t->rx_split = !eop;   /* a frame not ending here: the rest of it goes too */
    if (split || !eop || (errors & RXD_ERR_MASK) || len > BUF_SIZE) {
        t->rx_errors++;
        return;
    }
    /* The buffer is ours until the descriptor goes back (above), so it is
     * read as plain memory. */
    keep_or_drop(t, t->bufs + RX_BUF_OFF + (size_t)i * BUF_SIZE, len);
}

unsigned rx_harvest(struct e1k *t)
{
    if (!t->ring)
        return 0;
    unsigned n = 0;
    uint32_t last = 0;
    for (; n < RX_DESCS; n++) {
        uint32_t i = t->rx_next;
        volatile uint8_t *d = ring_rx_desc(t, i);
        uint8_t status = *(volatile uint8_t *)(d + RXD_STATUS);
        if (!(status & RXD_DD))
            break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* the frame and length after DD */
        t->rx_taken++;
        take(t, d, status, i);
        *(volatile uint64_t *)(d + RXD_LEN) = 0;   /* status 0: the chip's again */
        last = i;
        t->rx_next = (i + 1) % RX_DESCS;
    }
    if (n) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the cleared descriptors before the tail */
        wr32(t, E1K_RDT, last);
    }
    return n;
}

/* Descriptors the chip may fill now: from the head (RDH) up to the tail
 * (RDT), the ring's size less one when the driver keeps up. */
static uint32_t rx_chips(struct e1k *t)
{
    uint32_t head = rd32(t, E1K_RDH), tail = rd32(t, E1K_RDT);
    return (tail + RX_DESCS - head) % RX_DESCS;
}

/* Where the received frames went (the RTL8125's rx_tick has the same
 * line): what the chip handed back and its own counts (frames it took in,
 * frames it missed for want of a descriptor), what the driver kept and
 * dropped, what went to netstack's ring, the descriptors the chip may
 * fill, and the interrupts that say it ran short. */
void rx_tick(struct e1k *t)
{
    uint64_t now = drv_clock_ns();
    if (!t->ring || now - t->rx_tick_at < RX_TICK_NS || t->rx_taken == t->rx_tick_taken)
        return;
    t->rx_tick_at = now;
    t->rx_tick_taken = t->rx_taken;
    chip_counters(t);
    const uint64_t *d = t->rx_drop;
    const struct netdev_stats *s = &t->v.st;
    /* two lines: a log line holds about 240 characters */
    drv_log("rx so far: %lu from the chip (counted %lu, %lu missed), %lu kept; dropped %lu "
            "untagged, %lu other vlans, %lu vlan 0, %lu bad; %lu to netstack, %lu ring full, %lu "
            "no session", (unsigned long)t->rx_taken, (unsigned long)t->chip.rx_ok,
            (unsigned long)t->chip.missed, (unsigned long)d[NETFRAME_RX_KEEP],
            (unsigned long)d[NETFRAME_RX_UNTAGGED],
            (unsigned long)(d[NETFRAME_RX_OTHER_VLAN] + d[NETFRAME_RX_OUTER] +
                            d[NETFRAME_RX_NESTED]),
            (unsigned long)d[NETFRAME_RX_PRIORITY],
            (unsigned long)(d[NETFRAME_RX_RUNT] + d[NETFRAME_RX_LONG] + t->rx_errors),
            (unsigned long)s->rx_frames, (unsigned long)s->rx_ring_full,
            (unsigned long)s->rx_no_session);
    drv_log("rx ring: %u of %u descriptors the chip's, next %u; %u overrun and %u running-low "
            "interrupts", rx_chips(t), RX_DESCS, t->rx_next, t->rxo_irqs, t->rxdmt_irqs);
}
