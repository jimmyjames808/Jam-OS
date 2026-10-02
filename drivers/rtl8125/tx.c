/* rtl8125: THE transmit path (drv/rtl8125). Nothing else in the driver
 * can make the chip send: regs.c's accessors refuse every transmit
 * register (notx.h), and tools/checknotx.sh checks that this is the only
 * file that writes one.
 *
 * Every function here that other files call starts with the gate
 * (rtl_tx_allowed): the driver must be in full mode, with a valid VLAN.
 * The listen-only probe never gets past it, and neither does a driver
 * started without a VLAN. The register writers check the same gate again
 * right before each write.
 *
 * Sending a frame (tx_send), in this order:
 *   1. the caller's untagged frame (14..1514 bytes) is copied, each byte
 *      read once, into this driver's own transmit buffer for the next
 *      descriptor, with the 802.1Q tag (TPID 0x8100, priority 0, the
 *      VLAN) inserted after the two addresses and short frames padded
 *      with zeros (netframe_tag; a frame that already carries a tag is
 *      refused). Nothing outside the driver can see or write that buffer,
 *      so what is checked next is what the chip reads;
 *   2. bytes 12-15 of that buffer are checked once more, with its length
 *      (netframe_tx_check), right before the descriptor is handed over;
 *   3. the descriptor (rge's 32-byte struct rge_tx_desc, txdesc.h):
 *      address, then length with start and end of frame, then the
 *      ownership bit; the chip's own tag insertion (the descriptor's VLAN
 *      field) stays 0, and so do its last 16 bytes;
 *   4. the doorbell (TXSTART).
 * The transmitter goes on only if the chip reads descriptors of that size
 * (MAC OCP 0xeb58 bit 0, read back: txdesc.h), so it never takes an
 * address or a length from the wrong bytes.
 * Descriptors come back in tx_reap; the chip's own count of frames sent is
 * compared with tx.queued at the end (regs.c, tally_tx_check).
 *
 * Registers and values: OpenBSD's rge(4) (if_rge.c rge_init, rge_encap,
 * rge_txeof, rge_txstart, rge_tx_list_init; if_rgereg.h), ISC licence. */
#include "rtl8125.h"

#define RTL_TXCFG_CONFIG 0x03000700u   /* rge's RGE_TXCFG_CONFIG: interframe gap, DMA burst */
#define RTL_TDFNR_8125   0x10          /* rge_init: descriptors fetched at once */
#define RTL_TXSTART_GO   0x0001        /* RGE_TXSTART_START: queue 0 has work */

/* May the driver transmit at all? Every entry point asks first. */
static bool gate(struct rtl *t, const char *what)
{
    if (rtl_tx_allowed(t->mode, t->vlan))
        return true;
    if (t->tx.gate++ < 4)
        drv_log("REFUSED %s: not in full mode with a VLAN (mode %u, vlan %u)", what,
                (unsigned)t->mode, t->vlan);
    return false;
}

/* The only register writes in this file. */
static void txw8(struct rtl *t, uint32_t reg, uint8_t v)
{
    if (rtl_tx_allowed(t->mode, t->vlan))
        drv_write8(t->r, reg, v);
}

static void txw16(struct rtl *t, uint32_t reg, uint16_t v)
{
    if (rtl_tx_allowed(t->mode, t->vlan))
        drv_write16(t->r, reg, v);
}

static void txw32(struct rtl *t, uint32_t reg, uint32_t v)
{
    if (rtl_tx_allowed(t->mode, t->vlan))
        drv_write32(t->r, reg, v);
}

static volatile uint8_t *desc(const struct rtl *t, uint32_t i)
{
    return t->ring + TX_RING_OFF + (i % TX_DESCS) * RTL_TXD_SIZE;
}

static uint32_t eor(uint32_t i)
{
    return i % TX_DESCS == TX_DESCS - 1 ? RTL_TXD_EOR : 0;
}

/* Descriptor i's buffer (two per page), as the chip sees it. */
static uint64_t buf_addr(const struct rtl *t, uint32_t i)
{
    i %= TX_DESCS;
    return t->txbuf_addr[i / 2] + (i % 2) * TX_BUF;
}

status_t tx_arm(struct rtl *t)
{
    if (!gate(t, "tx_arm"))
        return ERR_ACCESS_DENIED;
    if (!t->txbufs || !t->ring)
        return ERR_BAD_STATE;
    for (uint32_t i = 0; i < TX_DESCS; i++) {   /* rge_tx_list_init */
        volatile uint8_t *d = desc(t, i);
        for (uint32_t k = RTL_TXD_RESERVED; k < RTL_TXD_SIZE; k += 4)
            *(volatile uint32_t *)(d + k) = 0;
        *(volatile uint64_t *)(d + RTL_TXD_ADDR) = buf_addr(t, i);
        *(volatile uint32_t *)(d + RTL_TXD_EXTSTS) = 0;
        *(volatile uint32_t *)(d + RTL_TXD_CMDSTS) = eor(i);   /* the driver's: OWN clear */
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->tx_prod = t->tx_cons = 0;
    uint64_t ring = t->ring_addr + TX_RING_OFF;
    txw32(t, RTL_TXDESC_LO, (uint32_t)ring);
    txw32(t, RTL_TXDESC_HI, (uint32_t)(ring >> 32));
    txw32(t, RTL_TXCFG, RTL_TXCFG_CONFIG);
    txw8(t, RTL_TDFNR, RTL_TDFNR_8125);
    drv_log("transmit ring: %u descriptors of %u bytes at %#lx, vlan %u on every frame",
            TX_DESCS, RTL_TXD_SIZE, (unsigned long)ring, t->vlan);
    return OK;
}

status_t tx_enable(struct rtl *t)
{
    if (!gate(t, "tx_enable"))
        return ERR_ACCESS_DENIED;
    uint16_t fmt = mac_rd(t, RTL_MAC_TXD_FORMAT);
    bool ok = rtl_txd_format_ok(fmt);
    drv_log("transmit descriptors: %u bytes; the chip's format bit (mac %#x bit 0) is %u: %s",
            RTL_TXD_SIZE, RTL_MAC_TXD_FORMAT, fmt & RTL_MAC_TXD_32,
            ok ? "they agree" : "THEY DISAGREE");
    if (!ok) {
        drv_report("the chip would read transmit descriptors of another size (mac %#x = %#x): "
                   "the transmitter stays OFF", RTL_MAC_TXD_FORMAT, fmt);
        return ERR_BAD_STATE;
    }
    txw8(t, RTL_CMD, RTL_CMD_TXENB | RTL_CMD_RXENB);   /* rge_init: both at once */
    t->tx_on = true;
    return OK;
}

status_t tx_send(struct rtl *t, const uint8_t *frame, size_t len)
{
    if (!gate(t, "tx_send"))
        return ERR_ACCESS_DENIED;
    if (!t->tx_on)
        return ERR_BAD_STATE;
    if (t->tx_prod - t->tx_cons >= TX_DESCS - 1) {
        t->tx.full++;
        return ERR_NO_RESOURCES;
    }
    uint32_t i = t->tx_prod % TX_DESCS;
    uint8_t *buf = t->txbufs + (size_t)i * TX_BUF;
    size_t n = netframe_tag(buf, TX_BUF, frame, len, t->vlan);
    /* The last look, at the bytes the chip will read, right before it may. */
    if (!n || !netframe_tx_check(buf, n, t->vlan)) {
        t->tx.refused++;
        return ERR_INVALID_ARGS;
    }
    volatile uint8_t *d = desc(t, i);
    *(volatile uint64_t *)(d + RTL_TXD_ADDR) = buf_addr(t, i);
    *(volatile uint32_t *)(d + RTL_TXD_EXTSTS) = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* buffer and address before the ownership */
    *(volatile uint32_t *)(d + RTL_TXD_CMDSTS) = rtl_txd_cmd(i, TX_DESCS, (uint32_t)n);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the descriptor before the doorbell */
    t->tx_prod++;
    t->tx.queued++;
    txw16(t, RTL_TXSTART, RTL_TXSTART_GO);
    return OK;
}

unsigned tx_reap(struct rtl *t)
{
    if (!gate(t, "tx_reap"))
        return 0;
    unsigned n = 0;
    bool waiting = false;
    while (t->tx_cons != t->tx_prod) {
        volatile uint8_t *d = desc(t, t->tx_cons);
        uint32_t st = *(volatile uint32_t *)(d + RTL_TXD_CMDSTS);
        if (st & RTL_TXD_OWN) {
            waiting = true;
            break;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (st & RTL_TXD_ERR)
            t->tx.errors++;
        else
            t->tx.done++;
        t->tx.collisions += !!(st & (RTL_TXD_COLL | RTL_TXD_EXCESSCOLL));
        *(volatile uint32_t *)(d + RTL_TXD_CMDSTS) = eor(t->tx_cons);
        t->tx_cons++;
        n++;
    }
    /* rge_txeof: some chips ignore a doorbell rung while they are still
     * sending, so ring it again for frames still waiting. */
    if (waiting && t->tx_on) {
        t->tx.kicks++;
        txw16(t, RTL_TXSTART, RTL_TXSTART_GO);
    }
    return n;
}

uint32_t tx_pending(const struct rtl *t)
{
    if (!rtl_tx_allowed(t->mode, t->vlan))
        return 0;
    return t->tx_prod - t->tx_cons;
}

void tx_log(const struct rtl *t)
{
    if (!rtl_tx_allowed(t->mode, t->vlan))
        return;
    drv_log("tx: %u queued, %u sent, %u with an error, %u with collisions, %u still out; refused: "
            "%u by the tag check, %u for a full ring; %u doorbell(s) again; gate refusals %u",
            t->tx.queued, t->tx.done, t->tx.errors, t->tx.collisions, t->tx_prod - t->tx_cons,
            t->tx.refused, t->tx.full, t->tx.kicks, t->tx.gate);
}
