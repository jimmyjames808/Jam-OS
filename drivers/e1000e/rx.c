/* e1000e: received frames (drv/e1000e).
 *
 * The chip's tag stripping (CTRL.VME) and VLAN filter (RCTL.VFE) are off,
 * so each frame arrives as it was on the wire, its CRC stripped
 * (RCTL.SECRC). The driver keeps only what netframe_rx_check keeps:
 * 802.1Q-tagged with the configured VLAN, any priority. Untagged frames,
 * priority tags (VLAN 0), other VLANs, outer QinQ tags and a tag inside
 * ours are dropped and counted, as are runts, frames too long, frames the
 * chip flagged and frames spread over several buffers (none should be:
 * RCTL.LPE is off, so nothing over 1522 bytes arrives, and a buffer is
 * 2048).
 *
 * To decide, the driver reads a frame's length (from the descriptor) and
 * bytes 12-17 (copied once out of the buffer); a kept frame is copied
 * without its tag (netframe_untag) into the driver's scratch copy and
 * handed to the netdev server (srv_rx), which puts it into the session's
 * rx ring, or drops and counts it with no session (rx_no_session) or the
 * ring full (rx_ring_full): the driver never waits for netstack. The
 * loop publishes the ring once per batch (srv_rx_done). The buffer is the
 * driver's from the moment the chip sets DD until the descriptor goes
 * back with the tail (RDT), so the chip can't change it in between. */
#include "e1000e.h"

/* A frame (len bytes at buf, as it was on the wire): to the server
 * untagged if it is ours, else counted under its reason (rx_drop, which
 * the stats add up: loop.c). */
static void keep_or_drop(struct e1k *t, const uint8_t *buf, uint32_t len)
{
    uint8_t head[NETFRAME_TAGGED] = { 0 };
    for (unsigned k = 12; k < NETFRAME_TAGGED && k < len; k++)
        head[k] = buf[k];
    enum netframe_rx v = netframe_rx_check(head, len, t->vlan);
    t->rx_drop[v]++;
    if (v != NETFRAME_RX_KEEP)
        return;
    size_t n = netframe_untag(t->frame, sizeof(t->frame), buf, len);
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
