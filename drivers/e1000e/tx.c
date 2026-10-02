/* e1000e: THE transmit path (drv/e1000e). Nothing else in the driver
 * turns the transmitter on, writes the transmit ring's registers or rings
 * the transmit doorbell (TDT); chip.c only turns the transmitter off.
 *
 * Every entry point starts with the gate: a network mode (a VLAN 1..4094,
 * or untagged). A driver started without one never gets here (main.c
 * exits first), and the gate makes sure of it.
 *
 * A frame from netstack reaches tx_send through the netdev server
 * (<jam/netserver.h>), its only caller, in this order:
 *   1. the server read its slot's length and flags once (netdev_take): a
 *      length outside 14..1514 or flags not 0 is refused and counted
 *      there, and the frame was copied out of the shared ring into the
 *      server's own buffer, which netstack can't see;
 *   2. tx_send copies it again, each byte read once, into this driver's
 *      transmit buffer for the next descriptor (netframe_tx_copy): with a
 *      VLAN, the 802.1Q tag (TPID 0x8100, priority 0, the VLAN) inserted
 *      after the two addresses and short frames padded with zeros to 64
 *      (netframe_tag); untagged, the frame as it is, padded to 60
 *      (netframe_plain). Either way a frame whose EtherType is already a
 *      tag (0x8100, 0x88a8, 0x9100) is refused (ERR_INVALID_ARGS: the
 *      server counts it), so netstack never chooses a tag;
 *   3. that buffer checked once more, with its length, right before its
 *      descriptor is written (netframe_tx_final: bytes 12-15 exactly the
 *      tag, or untagged bytes 12-13 not a tag's TPID): the buffer is DMA
 *      memory only this driver maps;
 *   4. the legacy descriptor: address, length, EOP | IFCS | RS; its VLE
 *      bit (the chip's own tag insertion) and its VLAN field stay 0, and
 *      CTRL.VME is off as well;
 *   5. tx_flush, after the server's pass (loop.c): one doorbell (TDT)
 *      for every frame queued.
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
    if (netframe_mode_ok(t->vlan))
        return true;
    if (refused++ < GATE_LINES)
        drv_log("REFUSED %s: the network is off (mode %u)", what, t->vlan);
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
    t->tx_prod = t->tx_cons = t->tx_tail = 0;
    wr32(t, E1K_TCTL, TCTL_VALUE);
    t->tx_on = true;
    char m[NETDEV_MODE_TEXT];
    drv_log("transmitter on: %u descriptors at %#lx, %s%s (in software; the chip's tag "
            "insertion off)", TX_DESCS, (unsigned long)ring, netdev_mode_str(t->vlan, m),
            t->vlan == NETFRAME_MODE_UNTAGGED ? ": no frame ever tagged" : " on every frame");
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
        t->tx_done++;
        n++;
    }
    return n;
}

uint32_t tx_room(struct e1k *t)
{
    if (!gate(t, "tx_room") || !t->tx_on)
        return 0;
    if (t->tx_prod - t->tx_cons >= TX_DESCS - 1)
        (void)tx_reap(t);   /* the interrupt for a sent one may still be on its way */
    uint32_t used = t->tx_prod - t->tx_cons;
    return used < TX_DESCS - 1 ? TX_DESCS - 1 - used : 0;
}

status_t tx_send(struct e1k *t, const uint8_t *frame, size_t len)
{
    if (!gate(t, "tx_send"))
        return ERR_ACCESS_DENIED;
    if (!t->tx_on)
        return ERR_BAD_STATE;
    if (t->tx_prod - t->tx_cons >= TX_DESCS - 1)
        return ERR_NO_RESOURCES;
    uint32_t i = t->tx_prod % TX_DESCS;
    uint8_t *buf = t->bufs + TX_BUF_OFF + (size_t)i * BUF_SIZE;
    size_t n = netframe_tx_copy(buf, BUF_SIZE, frame, len, t->vlan);
    /* The last look, at the bytes the chip will read, right before it may. */
    if (!n || !netframe_tx_final(buf, n, t->vlan))
        return ERR_INVALID_ARGS;
    volatile uint8_t *d = ring_tx_desc(t, i);
    *(volatile uint64_t *)(d + TXD_ADDR) = ring_tx_buf_addr(t, i);
    /* length, CSO 0, CMD (never VLE), STA 0, CSS 0, special (VLAN) 0 */
    *(volatile uint64_t *)(d + TXD_LEN) =
        (uint64_t)n | (uint64_t)(TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS) << (8 * (TXD_CMD - 8));
    t->tx_prod++;
    return OK;
}

void tx_flush(struct e1k *t)
{
    if (!gate(t, "tx_flush") || !t->tx_on || t->tx_tail == t->tx_prod)
        return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the descriptors before the doorbell */
    wr32(t, E1K_TDT, t->tx_prod % TX_DESCS);
    t->tx_tail = t->tx_prod;
}
