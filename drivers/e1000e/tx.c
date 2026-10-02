/* e1000e: THE transmit path (drv/e1000e). Nothing else in the driver
 * turns the transmitter on, writes the transmit ring's registers or rings
 * the transmit doorbell (TDT); chip.c only turns the transmitter off.
 *
 * Every entry point starts with the gate: a valid VLAN (1..4094). A
 * driver started without one never gets here (main.c exits first), and
 * the gate makes sure of it.
 *
 * A frame from netstack's tx ring (tx_pass), in this order:
 *   1. its slot's length and flags read once (netdev_take): a length
 *      outside 14..1514 or flags not 0 is refused and counted, and the
 *      frame is copied out of the shared ring into t->frame, which
 *      netstack can't see;
 *   2. copied again, each byte read once, into this driver's transmit
 *      buffer for the next descriptor, with the 802.1Q tag (TPID 0x8100,
 *      priority 0, the VLAN) inserted after the two addresses and short
 *      frames padded with zeros (netframe_tag); a frame whose EtherType is
 *      already a tag (0x8100, 0x88a8, 0x9100) is refused and counted;
 *   3. bytes 12-15 of that buffer checked once more, with its length
 *      (netframe_tx_check), right before its descriptor is written: the
 *      buffer is DMA memory only this driver maps;
 *   4. the legacy descriptor: address, length, EOP | IFCS | RS; its VLE
 *      bit (the chip's own tag insertion) and its VLAN field stay 0, and
 *      CTRL.VME is off as well;
 *   5. after the pass, one doorbell (TDT) for every frame queued.
 * Netstack writing its slot again at any point changes nothing that was
 * checked: every check is on the driver's own copy.
 *
 * Descriptors come back in tx_reap (DD). The chip's own count of frames
 * sent (GPTC) is in netdev.stats beside tx_frames, so a test sees that
 * the chip sent nothing of its own. */
#include "e1000e.h"

#define GATE_LINES 4

/* May the driver transmit at all? Every entry point asks first. */
static bool gate(struct e1k *t, const char *what)
{
    static unsigned refused;
    if (netframe_vlan_ok(t->vlan))
        return true;
    if (refused++ < GATE_LINES)
        drv_log("REFUSED %s: no VLAN (vlan %u)", what, t->vlan);
    return false;
}

status_t tx_enable(struct e1k *t)
{
    if (!gate(t, "tx_enable"))
        return ERR_ACCESS_DENIED;
    if (!t->ring)
        return ERR_BAD_STATE;
    uint64_t ring = t->ring_addr + TX_RING_OFF;
    wr32(t, E1K_TDBAL, (uint32_t)ring);
    wr32(t, E1K_TDBAH, (uint32_t)(ring >> 32));
    wr32(t, E1K_TDLEN, TX_DESCS * DESC_SIZE);
    wr32(t, E1K_TDH, 0);
    wr32(t, E1K_TDT, 0);
    wr32(t, E1K_TXDCTL, TXDCTL_VALUE);
    wr32(t, E1K_TIPG, TIPG_VALUE);
    t->tx_prod = t->tx_cons = 0;
    wr32(t, E1K_TCTL, TCTL_VALUE);
    t->tx_on = true;
    drv_log("transmitter on: %u descriptors at %#lx, vlan %u on every frame (in software; "
            "the chip's tag insertion off)", TX_DESCS, (unsigned long)ring, t->vlan);
    return OK;
}

void tx_disable(struct e1k *t)
{
    wr32(t, E1K_TCTL, 0);
    t->tx_on = false;
}

unsigned tx_reap(struct e1k *t)
{
    if (!gate(t, "tx_reap") || !t->ring)
        return 0;
    unsigned n = 0;
    while (t->tx_cons != t->tx_prod) {
        volatile uint8_t *d = ring_tx_desc(t, t->tx_cons);
        if (!(*(volatile uint8_t *)(d + TXD_STA) & TXD_STA_DD))
            break;
        *(volatile uint64_t *)(d + TXD_LEN) = 0;
        t->tx_cons++;
        t->st.tx_done++;
        n++;
    }
    return n;
}

/* One frame of len bytes at t->frame: tagged into descriptor tx_prod's
 * buffer, checked, the descriptor written (no doorbell). OK, or
 * ERR_INVALID_ARGS: refused (counted). */
static status_t queue(struct e1k *t, uint32_t len)
{
    uint32_t i = t->tx_prod % TX_DESCS;
    uint8_t *buf = t->bufs + TX_BUF_OFF + (size_t)i * BUF_SIZE;
    size_t n = netframe_tag(buf, BUF_SIZE, t->frame, len, t->vlan);
    /* The last look, at the bytes the chip will read, right before it may. */
    if (!n || !netframe_tx_check(buf, n, t->vlan)) {
        t->st.tx_bad_tag++;
        return ERR_INVALID_ARGS;
    }
    volatile uint8_t *d = ring_tx_desc(t, i);
    *(volatile uint64_t *)(d + TXD_ADDR) = ring_tx_buf_addr(t, i);
    /* length, CSO 0, CMD (never VLE), STA 0, CSS 0, special (VLAN) 0 */
    *(volatile uint64_t *)(d + TXD_LEN) =
        (uint64_t)n | (uint64_t)(TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS) << (8 * (TXD_CMD - 8));
    t->tx_prod++;
    t->st.tx_frames++;
    t->st.tx_bytes += n;
    return OK;
}

/* Frames from the ring while the descriptor ring has room; false if it
 * filled up before the frames ran out. */
static bool take_frames(struct e1k *t, uint32_t ready, unsigned *queued)
{
    struct netdev_end *e = &t->s.tx;
    for (uint32_t k = 0; k < ready; k++) {
        if (t->tx_prod - t->tx_cons >= TX_DESCS - 1)
            return false;
        uint32_t len = 0;
        status_t st = netdev_take(e, t->frame, sizeof(t->frame), &len);
        if (st == ERR_INVALID_ARGS)
            t->st.tx_bad_flags++;
        else if (st != OK)
            t->st.tx_bad_len++;
        else if (queue(t, len) == OK)
            (*queued)++;
    }
    return true;
}

void tx_pass(struct e1k *t)
{
    t->s.tx_wake = false;
    if (!gate(t, "tx_pass") || !t->tx_on || t->s.ch == HANDLE_INVALID)
        return;
    struct netdev_end *e = &t->s.tx;
    /* Our flag down and our event bit cleared BEFORE looking at the ring,
     * so a signal that comes while we work is kept for the next wait. */
    netdev_awake(e);
    (void)drv_event_signal(t->s.to_driver, NETDEV_SIG_TX, 0);
    (void)tx_reap(t);
    uint64_t before = e->count;
    unsigned queued = 0;
    bool room = take_frames(t, netdev_ready(e), &queued);
    if (queued) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the descriptors before the doorbell */
        wr32(t, E1K_TDT, t->tx_prod % TX_DESCS);
    }
    if (e->count != before && netdev_publish(e))
        session_signal(t, NETDEV_SIG_TX_ROOM);
    t->tx_blocked = !room;
    if (!room)
        return;   /* tx_reap's caller tries again when descriptors come back */
    if (!netdev_sleep(e))
        (void)drv_event_signal(t->s.to_driver, 0, NETDEV_SIG_TX);   /* more came: our port */
}
