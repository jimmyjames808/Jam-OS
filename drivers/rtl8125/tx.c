/* rtl8125: THE transmit path (drv/rtl8125). Nothing else in the driver
 * can make the chip send: regs.c's accessors refuse every transmit
 * register (notx.h), and tools/checknotx.sh checks that this is the only
 * file that writes one.
 *
 * Every function here that other files call starts with the gate
 * (rtl_tx_allowed): the driver must be in full mode, with a network mode
 * (a valid VLAN, or untagged). The listen-only probe never gets past it,
 * and neither does a driver started with the network off. The register writers check the same gate again
 * right before each write.
 *
 * Sending a frame (tx_send), in this order:
 *   1. the caller's untagged frame (14..1514 bytes) is copied, each byte
 *      read once, into this driver's own transmit buffer for the next
 *      descriptor (netframe_tx_copy): with a VLAN, the 802.1Q tag (TPID
 *      0x8100, priority 0, the VLAN) inserted after the two addresses and
 *      short frames padded with zeros to 64 (netframe_tag); untagged, the
 *      frame as it is, padded to 60 (netframe_plain). Either way a frame
 *      that already carries a tag is refused, so netstack never chooses
 *      one. Nothing outside the driver can see or write that buffer, so
 *      what is checked next is what the chip reads;
 *   2. that buffer is checked once more, with its length, right before
 *      the descriptor is handed over (netframe_tx_final: bytes 12-15
 *      exactly the tag, or untagged bytes 12-13 not a tag's TPID);
 *   3. the descriptor (rge's 32-byte struct rge_tx_desc, txdesc.h):
 *      address, then length with start and end of frame, then the
 *      ownership bit; the chip's own tag insertion (the descriptor's VLAN
 *      field) stays 0, and so do its last 16 bytes;
 *   4. the doorbell (TXSTART).
 * The transmitter goes on only if the chip reads descriptors of that size
 * (MAC OCP 0xeb58 bit 0, read back: txdesc.h), so it never takes an
 * address or a length from the wrong bytes.
 * Descriptors come back in tx_reap; the chip's own count of frames sent is
 * compared with tx.queued while the driver runs (guard.c: more sent than
 * queued stops the chip) and at the end (regs.c, tally_tx_check).
 *
 * Registers and values: OpenBSD's rge(4) (if_rge.c rge_init, rge_encap,
 * rge_txeof, rge_txstart, rge_tx_list_init; if_rgereg.h), ISC licence. */
#include "rtl8125.h"

#define RTL_TXCFG_CONFIG 0x03000700u   /* rge's RGE_TXCFG_CONFIG: interframe gap, DMA burst */
#define RTL_TDFNR_8125   0x10          /* rge_init: descriptors fetched at once */
#define RTL_TXSTART_GO   0x0001        /* RGE_TXSTART_START: queue 0 has work */
#define TX_STALL_NS      (100 * NS_PER_MS)   /* still the chip's this long after: stalled */
#define STALL_DUMPS      6             /* stall dumps logged per run, ... */
#define STALL_GAP_NS     NS_PER_S      /* ... at most one a second */
#define TICK_NS          (10 * NS_PER_S)     /* tx_tick's line: at most one in 10 s */

/* May the driver transmit at all? Every entry point asks first. */
static bool gate(struct rtl *t, const char *what)
{
    if (rtl_tx_allowed(t->mode, t->vlan))
        return true;
    if (t->tx.gate++ < 4)
        drv_log("REFUSED %s: not in full mode with a network mode (mode %u, vlan %u)", what,
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

/* One frame's wait from its doorbell to its descriptor back, counted. */
static void timed(struct txstats *x, uint64_t wait)
{
    if (!x->wait_n || wait < x->wait_min)
        x->wait_min = wait;
    if (wait > x->wait_max)
        x->wait_max = wait;
    x->wait_sum += wait;
    x->wait_n++;
}

/* The chip's count of frames sent (good and errored) since the driver
 * started, less the one at the start. `now`: a dump made now, waited for
 * (bounded: only the stall dumps, a few a run, whose verdict needs a count
 * from after the doorbell); else the last that landed (no wait). False if
 * unread. */
static bool chip_sent(struct rtl *t, bool now, uint64_t *out)
{
    struct tally x;
    if (!t->tally0_ok || (now ? tally_dump(t, &x) != OK : !tally_recent(t, drv_clock_ns(), &x)))
        return false;
    *out = x.tx_ok + x.tx_err - t->tally_tx0;
    return true;
}

/* Everything that tells "the chip never fetched descriptor c" from "it
 * sent the frame but never wrote the descriptor back": the descriptor's
 * bytes and its neighbours', the transmit registers read back, and the
 * chip's tally against the descriptors handed back. Reads only (the
 * tally dump aside). */
static void stall_dump(struct rtl *t, uint32_t c, uint64_t waited)
{
    volatile uint8_t *d = desc(t, c);
    uint32_t w[RTL_TXD_SIZE / 4];
    for (unsigned k = 0; k < RTL_TXD_SIZE / 4; k++)
        w[k] = *(volatile uint32_t *)(d + 4 * k);
    uint32_t before = *(volatile uint32_t *)(desc(t, c - 1) + RTL_TXD_CMDSTS);
    uint32_t after = *(volatile uint32_t *)(desc(t, c + 1) + RTL_TXD_CMDSTS);
    uint64_t ring = t->ring_addr + TX_RING_OFF;
    uint32_t back = t->tx.done + t->tx.errors;
    drv_log("tx STALL: descriptor %u (slot %u at %#lx) still the chip's %lu ms after its doorbell; "
            "%lu queued, %u back, %u doorbell(s) again", c, c % TX_DESCS, (unsigned long)(ring +
            (c % TX_DESCS) * RTL_TXD_SIZE), (unsigned long)(waited / NS_PER_MS),
            (unsigned long)t->tx.queued, back, t->tx.kicks);
    drv_log("tx STALL: it holds cmdsts %08x extsts %08x addr %08x%08x, then %08x %08x %08x %08x; "
            "cmdsts before it %08x, after it %08x", w[0], w[1], w[3], w[2], w[4], w[5], w[6], w[7],
            before, after);
    drv_log("tx STALL: tx ring %#010x%08x (ours %#lx), command %#x, txcfg %#010x, txstart %#06x, "
            "tdfnr %#x, isr %#x, imr %#x, mac eb58 %#06x", rd32(t, RTL_TXDESC_HI),
            rd32(t, RTL_TXDESC_LO), (unsigned long)ring, rd8(t, RTL_CMD), rd32(t, RTL_TXCFG),
            rd16(t, RTL_TXSTART), rd8(t, RTL_TDFNR), rd32(t, RTL_ISR), rd32(t, RTL_IMR),
            mac_rd(t, RTL_MAC_TXD_FORMAT));
    uint64_t sent;
    if (!chip_sent(t, true, &sent))
        drv_log("tx STALL: the tally could not be read");
    else
        drv_log("tx STALL: the chip's tally says %lu sent, %u handed back: %s",
                (unsigned long)sent, back, sent > back
                ? "it SENT frames it has not handed back (no write-back)"
                : "it sent nothing more: it NEVER FETCHED this descriptor");
}

/* Descriptor tx_cons is still the chip's: a stall once TX_STALL_NS have
 * passed, counted once, dumped for the first few (one a second at most). */
static void check_stall(struct rtl *t, uint64_t now)
{
    uint32_t c = t->tx_cons;
    uint64_t waited = now - t->tx_at[c % TX_DESCS];
    if (waited < TX_STALL_NS || t->stall_cons == c + 1)
        return;
    t->stall_cons = c + 1;
    t->tx.stalls++;
    if (t->stall_dumps >= STALL_DUMPS ||
        (t->stall_dumps && now - t->stall_dump_at < STALL_GAP_NS))
        return;
    t->stall_dumps++;
    t->stall_dump_at = now;
    stall_dump(t, c, waited);
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
    char m[NETDEV_MODE_TEXT];
    drv_log("transmit ring: %u descriptors of %u bytes at %#lx, %s%s", TX_DESCS, RTL_TXD_SIZE,
            (unsigned long)ring, netdev_mode_str(t->vlan, m),
            t->vlan == NETFRAME_MODE_UNTAGGED ? ": no frame ever tagged" : " on every frame");
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
    size_t n = netframe_tx_copy(buf, TX_BUF, frame, len, t->vlan);
    /* The last look, at the bytes the chip will read, right before it may. */
    if (!n || !netframe_tx_final(buf, n, t->vlan)) {
        t->tx.refused++;
        return ERR_INVALID_ARGS;
    }
    volatile uint8_t *d = desc(t, i);
    *(volatile uint64_t *)(d + RTL_TXD_ADDR) = buf_addr(t, i);
    *(volatile uint32_t *)(d + RTL_TXD_EXTSTS) = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* buffer and address before the ownership */
    *(volatile uint32_t *)(d + RTL_TXD_CMDSTS) = rtl_txd_cmd(i, TX_DESCS, (uint32_t)n);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the descriptor before the doorbell */
    t->tx_at[i] = drv_clock_ns();
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
    uint64_t now = drv_clock_ns();
    while (t->tx_cons != t->tx_prod) {
        volatile uint8_t *d = desc(t, t->tx_cons);
        uint32_t st = *(volatile uint32_t *)(d + RTL_TXD_CMDSTS);
        if (st & RTL_TXD_OWN) {
            waiting = true;
            break;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        t->tx_wait[t->tx_cons % TX_DESCS] = now - t->tx_at[t->tx_cons % TX_DESCS];
        timed(&t->tx, t->tx_wait[t->tx_cons % TX_DESCS]);
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
     * sending, so ring it again for a frame still waiting; not at every
     * look, though (txdesc.h, rtl_kick_due): at most one a second. */
    if (waiting && t->tx_on &&
        rtl_kick_due(&t->kick, t->tx_cons, t->tx_at[t->tx_cons % TX_DESCS], now)) {
        t->tx.kicks++;
        txw16(t, RTL_TXSTART, RTL_TXSTART_GO);
    }
    if (waiting)
        check_stall(t, now);
    return n;
}

bool tx_wait_of(const struct rtl *t, uint32_t seq, uint64_t *wait)
{
    if (!rtl_tx_allowed(t->mode, t->vlan))
        return false;
    uint32_t behind = t->tx_cons - seq;   /* free-running: 1..TX_DESCS when back and kept */
    if (behind == 0 || behind > TX_DESCS || t->tx_prod - seq > TX_DESCS)
        return false;
    *wait = t->tx_wait[seq % TX_DESCS];
    return true;
}

uint32_t tx_pending(const struct rtl *t)
{
    if (!rtl_tx_allowed(t->mode, t->vlan))
        return 0;
    return t->tx_prod - t->tx_cons;
}

void tx_wait_str(const struct rtl *t, char *buf, size_t size)
{
    if (!rtl_tx_allowed(t->mode, t->vlan)) {
        drv_snprintf(buf, size, "-");
        return;
    }
    const struct txstats *x = &t->tx;
    if (!x->wait_n) {
        drv_snprintf(buf, size, "none timed");
        return;
    }
    uint64_t avg = x->wait_sum / x->wait_n;
    drv_snprintf(buf, size, "%lu.%03lu/%lu.%03lu/%lu.%03lu ms", (unsigned long)(x->wait_min /
                 NS_PER_MS), (unsigned long)(x->wait_min % NS_PER_MS / NS_PER_US),
                 (unsigned long)(avg / NS_PER_MS), (unsigned long)(avg % NS_PER_MS / NS_PER_US),
                 (unsigned long)(x->wait_max / NS_PER_MS),
                 (unsigned long)(x->wait_max % NS_PER_MS / NS_PER_US));
}

void tx_log(const struct rtl *t)
{
    if (!rtl_tx_allowed(t->mode, t->vlan))
        return;
    char wait[48];
    tx_wait_str(t, wait, sizeof(wait));
    drv_log("tx: %lu queued, %u sent, %u with an error, %u with collisions, %u still out; "
            "refused: %u by the tag check, %u for a full ring; %u doorbell(s) again; gate "
            "refusals %u", (unsigned long)t->tx.queued, t->tx.done, t->tx.errors, t->tx.collisions, t->tx_prod - t->tx_cons,
            t->tx.refused, t->tx.full, t->tx.kicks, t->tx.gate);
    drv_log("tx: doorbell to descriptor back, min/avg/max %s over %u frame(s); %u stalled "
            "(still the chip's after %lu ms)", wait, t->tx.wait_n, t->tx.stalls,
            (unsigned long)(TX_STALL_NS / NS_PER_MS));
}

void tx_tick(struct rtl *t)
{
    if (!gate(t, "tx_tick"))
        return;
    uint64_t now = drv_clock_ns();
    uint32_t back = t->tx.done + t->tx.errors;
    if (now - t->tick_at < TICK_NS || (t->tx.queued == t->tick_queued && back == t->tick_back))
        return;
    t->tick_at = now;
    t->tick_queued = t->tx.queued;
    t->tick_back = back;
    char wait[48], chip[24] = "unread";
    tx_wait_str(t, wait, sizeof(wait));
    uint64_t sent;
    if (chip_sent(t, false, &sent))   /* the last dump: up to a second old */
        drv_snprintf(chip, sizeof(chip), "%lu", (unsigned long)sent);
    drv_log("tx so far: %lu queued, chip sent %s, %u back, %u pending; wait min/avg/max %s; %u "
            "stalled, %u doorbell(s) again, %u refused for a full ring",
            (unsigned long)t->tx.queued, chip, back,
            t->tx_prod - t->tx_cons, wait, t->tx.stalls, t->tx.kicks, t->tx.full);
}
