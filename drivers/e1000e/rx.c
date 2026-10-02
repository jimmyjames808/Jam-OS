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
 * without its tag (netframe_untag) straight into the next slot of the
 * session's rx ring. With no session the frame is counted
 * (rx_no_session); with the ring full it is dropped and counted
 * (rx_ring_full): the driver never waits for netstack. The buffer is the
 * driver's from the moment the chip sets DD until the descriptor goes
 * back with the tail (RDT), so the chip can't change it in between. */
#include "e1000e.h"

/* A kept frame (len bytes at buf, tagged) into the session's rx ring, if
 * there is a session and room (*room, as read once this pass). */
static void deliver(struct e1k *t, const uint8_t *buf, uint32_t len, uint32_t *room)
{
    if (t->s.ch == HANDLE_INVALID) {
        t->st.rx_no_session++;
        return;
    }
    if (*room == 0) {
        t->st.rx_ring_full++;
        return;
    }
    size_t n = netframe_untag(netdev_slot_frame(&t->s.rx), NETDEV_FRAME_MAX, buf, len);
    if (n < NETDEV_FRAME_MIN) {
        t->st.rx_bad++;   /* can't happen: netframe_rx_check bounded the length */
        return;
    }
    netdev_commit(&t->s.rx, (uint32_t)n);
    (*room)--;
    t->st.rx_frames++;
    t->st.rx_bytes += n;
}

static void keep_or_drop(struct e1k *t, const uint8_t *buf, uint32_t len, uint32_t *room)
{
    uint8_t head[NETFRAME_TAGGED] = { 0 };
    for (unsigned k = 12; k < NETFRAME_TAGGED && k < len; k++)
        head[k] = buf[k];
    enum netframe_rx v = netframe_rx_check(head, len, t->vlan);
    t->rx_drop[v]++;
    switch (v) {
    case NETFRAME_RX_KEEP:
        deliver(t, buf, len, room);
        return;
    case NETFRAME_RX_UNTAGGED:
        t->st.rx_untagged++;
        return;
    case NETFRAME_RX_PRIORITY:
        t->st.rx_priority++;
        return;
    case NETFRAME_RX_OTHER_VLAN:
    case NETFRAME_RX_OUTER:
    case NETFRAME_RX_NESTED:
        t->st.rx_other_vlan++;
        return;
    default:
        t->st.rx_bad++;   /* a runt, or too long */
        return;
    }
}

/* One descriptor the chip has finished (status read once). */
static void take(struct e1k *t, volatile uint8_t *d, uint8_t status, uint32_t i, uint32_t *room)
{
    uint32_t len = *(volatile uint16_t *)(d + RXD_LEN);
    uint8_t errors = *(volatile uint8_t *)(d + RXD_ERRORS);
    bool eop = status & RXD_EOP, split = t->rx_split;
    t->rx_split = !eop;   /* a frame not ending here: the rest of it goes too */
    if (split || !eop || (errors & RXD_ERR_MASK) || len > BUF_SIZE) {
        t->st.rx_bad++;
        return;
    }
    /* The buffer is ours until the descriptor goes back (above), so it is
     * read as plain memory. */
    keep_or_drop(t, t->bufs + RX_BUF_OFF + (size_t)i * BUF_SIZE, len, room);
}

unsigned rx_harvest(struct e1k *t)
{
    if (!t->ring)
        return 0;
    bool session = t->s.ch != HANDLE_INVALID;
    uint32_t room = session ? netdev_room(&t->s.rx) : 0;
    uint64_t before = session ? t->s.rx.count : 0;
    unsigned n = 0;
    uint32_t last = 0;
    for (; n < RX_DESCS; n++) {
        uint32_t i = t->rx_next;
        volatile uint8_t *d = ring_rx_desc(t, i);
        uint8_t status = *(volatile uint8_t *)(d + RXD_STATUS);
        if (!(status & RXD_DD))
            break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* the frame and length after DD */
        take(t, d, status, i, &room);
        *(volatile uint64_t *)(d + RXD_LEN) = 0;   /* status 0: the chip's again */
        last = i;
        t->rx_next = (i + 1) % RX_DESCS;
    }
    if (n) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the cleared descriptors before the tail */
        wr32(t, E1K_RDT, last);
    }
    if (session && t->s.rx.count != before && netdev_publish(&t->s.rx))
        session_signal(t, NETDEV_SIG_RX);
    return n;
}
