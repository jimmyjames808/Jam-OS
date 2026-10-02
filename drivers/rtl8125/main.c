/* rtl8125: the listen-only probe of the PC's Realtek RTL8125B (drv/rtl8125),
 * started by devmgr for 10ec:8125 only when the boot word `netprobe` is
 * on the command line (the boot menu's "Jam OS (network: listen only)");
 * a boot without it never touches the chip (docs/M9-PLAN.md, stage 0).
 *
 * It SENDS NOTHING. There is no transmit code: no transmit ring, the
 * transmitter enable bit is never set, the transmit doorbell never
 * written, pause is never advertised, wake-on-LAN and the chip's
 * management firmware are left off; notx.h refuses any write to those
 * registers, and tools/checknotx.sh (in `make check`) checks the sources.
 * The chip's own count of frames sent is dumped at the end as the proof.
 *
 * The steps, one log line or a few each, every wait bounded:
 *   a. what the firmware left (reads only): PCI command, chip command
 *      (a transmitter left ON is reported loudly), interrupts, rings,
 *      wake-on-LAN, the PCIe link;
 *   b. the chip's id from TXCFG (0x641: 8125B; anything else: stop);
 *   c. its MAC address, into the log only;
 *   d. reset (rge's order), then the PHY's id, its patch version, the
 *      MAC's patch state and the receive registers' values;
 *   e. bus mastering on, the receive ring (where it landed: 64-bit DMA?),
 *      receive-only bring-up, autonegotiation 10/100/1000/2500 without
 *      pause, the link (10 s at most);
 *   f. one MSI-X vector, each frame noted as found after an interrupt or
 *      at the 1 s poll that stands in for a lost one;
 *   g. 60 s of listening, tags kept, every frame accepted: the count by
 *      tag, VLAN and EtherType (census.c), link changes logged;
 *   h. the tally counters: frames sent must be 0;
 *   i. the receiver off, the chip reset, bus mastering off, unpinned;
 *   j. one RESULTS line with the verdict ("trunk carrying 21", ...).
 * It always exits 0 (devmgr then leaves it finished): a probe is run once,
 * and its log says what went wrong.
 *
 * EVERY REGISTER THE PROBE WRITES (offsets in BAR 2; rge's names):
 *   0x34 INT_CFG0     bit 0 cleared (the 8125B's interrupt type)
 *   0x37 CMD          0x80 stop request with the receive enable kept, then
 *                     the receive enable alone, 0x10 reset, 0x08 receive
 *                     enable. NEVER 0x04 (transmit enable)
 *   0x38 IMR          0, then the receive and link bits only
 *   0x3c ISR          write-1-to-clear acknowledgements
 *   0x44 RXCFG        accept bits cleared (reset, stop); 0x41000c00 and
 *                     accept-all (0x0f); tag stripping (23:22) never set
 *   0x08, 0x0c MAR    all ones (every multicast)
 *   0x10, 0x14 DTCCR  the tally dump's address and its dump bit
 *   0x50 EECMD        config unlock (0xc0) set and cleared around set-up
 *   0x52 CFG1         speed-down bit cleared
 *   0x58 0x5c 0x8c 0xf4 TIMERINT0-3  0
 *   0x6f PMCH         0xc0 set (PHY power)
 *   0xd0 DLLPR        0xc0 set
 *   0xd3 MCUCMD       0x80 (out of band) cleared
 *   0xda RXMAXSIZE    2048
 *   0xe2 IM           0 (no hardware moderation)
 *   0xe4, 0xe8 RXDESC the receive ring's address
 *   0xf2 PPSW         bit 3 (the RXDV gate) set at reset, cleared at start
 *   0x0a00-0x0a7c     INTMITI 0-31: 0
 *   0x1880            bits 5:4 cleared
 *   0x4500 RSS_CTRL   0;   0x4800 RXQUEUE_CTRL  bits 4:2 cleared
 *   0xb0 MACOCP       the MAC's window: c0bc (RealWoW off), e8de, c0aa,
 *                     c0a6, c01e, d42c, c0ac, c140, c142, eb58, e614,
 *                     e63e, c0b4, eb6a, eb50, e056, e040, ea1c, e0c0,
 *                     e052, d430, e080 (EEE plus off), eb54, e098, e032,
 *                     e092; and the read index
 *   0xb8 PHYOCP       the PHY's window: a466, a468 (out of band exit),
 *                     a436 (the index to read the patch version), a428,
 *                     a5ea, a5d4 (2500 advertised), and the MII BMCR,
 *                     ANAR (pause bits cleared) and GTCR; and the read index
 * Never written: 0x20-0x2f (transmit rings), 0x40 TXCFG, 0x90 TXSTART,
 * 0x00-0x05 (the address), 0x57 TDFNR, 0xe0 CPLUSCMD, the CSI window,
 * the MAC's and PHY's patch RAM, and PCI config space. */
#include "rtl8125.h"

#define REGS_BYTES    0x10000            /* BAR 2 (docs/HARDWARE.md: 64 KiB) */
#define LINK_WAIT_NS  (10 * NS_PER_S)
#define LISTEN_NS     (60 * NS_PER_S)
#define PORT_WAIT_NS  NS_PER_S           /* a lost interrupt costs a second, never a stall */
#define LINK_POLL_NS  (50 * NS_PER_MS)   /* while waiting for the link */

#define KEY_IRQ   1
#define KEY_SERVE 2

/* Wait for one event until `deadline` (or a 1 s poll) and handle it.
 * False when devmgr closed our channel: it is stopping, so stop. */
static bool step(struct rtl *t, uint64_t deadline, uint64_t since, bool listening)
{
    struct port_packet p;
    status_t st = drv_port_wait(t->port, deadline, &p);
    if (st == OK && p.key == KEY_SERVE)
        return false;
    if (st == OK && p.key == KEY_IRQ) {
        /* rge_intr: the mask off while the status is read and acked, and
         * on again at the end, so status that came meanwhile fires anew. */
        t->c.irqs += (uint32_t)p.signal.count;
        (void)drv_interrupt_ack(t->irq);   /* before the status: a new fire is not lost */
        wr32(t, RTL_IMR, 0);
        uint32_t isr = rd32(t, RTL_ISR);
        if (isr == 0xffffffffu)
            isr = 0;   /* the chip is gone: its registers read all ones */
        if (isr)
            wr32(t, RTL_ISR, isr);
        if ((isr & RTL_ISR_TX_ANY) && !(t->c.isr_seen & RTL_ISR_TX_ANY))
            drv_log("WARNING: transmit status bits in isr %#x", isr);
        t->c.isr_seen |= isr;
        t->c.linkchg_irqs += !!(isr & RTL_ISR_LINKCHG);
        (void)census_harvest(t, true);
        (void)chip_link_poll(t, since);
        wr32(t, RTL_IMR, RTL_IMR_PROBE);
        return true;
    }
    if (st != OK && st != ERR_TIMED_OUT) {
        drv_log("port wait failed (%s): polling", status_str(st));
        delay_us(1000);
    }
    if (listening)
        t->c.polls++;
    (void)census_harvest(t, false);
    (void)chip_link_poll(t, since);
    return true;
}

/* e: the link, 10 s at most. False if devmgr is stopping. */
static bool wait_link(struct rtl *t, uint64_t since)
{
    uint64_t end = drv_clock_ns() + LINK_WAIT_NS;
    while (!t->link && drv_clock_ns() < end) {
        uint64_t next = drv_clock_ns() + LINK_POLL_NS;
        if (!step(t, next < end ? next : end, since, false))
            return false;
    }
    return true;
}

/* g: 60 s of listening. False if cut short. */
static bool listen_60s(struct rtl *t, uint64_t since)
{
    uint64_t end = drv_clock_ns() + LISTEN_NS;
    drv_log("listening for 60 s: every frame accepted, tags kept, nothing sent");
    for (uint64_t now = drv_clock_ns(); now < end; now = drv_clock_ns()) {
        uint64_t next = now + PORT_WAIT_NS;
        if (!step(t, next < end ? next : end, since, true)) {
            drv_log("devmgr is stopping: the listening ends early");
            return false;
        }
    }
    return true;
}

static void log_tally(const char *when, const struct tally *x)
{
    drv_log("tally %s: tx ok %lu err %lu abort %u underrun %u collisions %u/%u; rx ok %lu "
            "(unicast %lu, broadcast %lu, multicast %u) err %u missed %u align %u", when,
            (unsigned long)x->tx_ok, (unsigned long)x->tx_err, x->tx_abort, x->tx_underrun,
            x->tx_1col, x->tx_mcol, (unsigned long)x->rx_ok, (unsigned long)x->rx_ok_phy,
            (unsigned long)x->rx_ok_brd, x->rx_ok_mul, x->rx_err, x->miss, x->fae);
}

/* What the RESULTS line needs from the run. */
struct outcome {
    uint32_t phy;
    uint16_t rcode;
    bool     reset;            /* the chip was reset: the run has a RESULTS line */
    bool     start_ok, end_ok; /* the tally dumps worked */
    struct tally start, end;
    bool     cut;              /* devmgr stopped it early */
};

static void report(const struct rtl *t, const struct outcome *o)
{
    char link[48] = "no link in 10 s";
    if (t->link_at) {
        char s[40];
        chip_link_str(t->phystat, s, sizeof(s));
        uint64_t ms = (t->link_at - t->an_at) / NS_PER_MS;   /* from autonegotiation's start */
        drv_snprintf(link, sizeof(link), "link %s in %lu.%lu s", s, (unsigned long)(ms / 1000),
                     (unsigned long)(ms % 1000 / 100));
    }
    const struct census *c = &t->c;
    uint32_t v = census_vlan(c, PROBE_VLAN), u = census_kind(c, NETFRAME_UNTAGGED);
    uint32_t other = c->frames - v - u;
    char tally[40] = "tx tally UNREAD";
    if (o->end_ok)
        drv_snprintf(tally, sizeof(tally), "tx tally %lu%s", (unsigned long)o->end.tx_ok,
                     o->start_ok && o->end.tx_ok != o->start.tx_ok ? " (GREW)" : "");
    drv_report("8125B xid %03x, phy %08x patch %04x, %s, %s%u frames: vlan %u: %u, untagged %u, "
               "other %u, irqs %u, %s%s -> %s", t->xid, o->phy, o->rcode, link,
               t->link_at ? (o->cut ? "cut short: " : "60 s: ") : "", c->frames, PROBE_VLAN, v,
               u, other, c->irqs, tally, t->refused ? ", WRITES REFUSED" : "",
               t->link_at ? census_verdict(c) : "nothing heard (no link)");
}

/* a-h; the caller does i and j. */
static void probe(struct rtl *t, struct outcome *o, uint64_t since)
{
    chip_snapshot(t);
    if (chip_identify(t) != OK)
        return;
    chip_log_mac(t);
    if (chip_reset(t) != OK)
        return;
    o->reset = true;
    o->phy = chip_log_after_reset(t, &o->rcode);
    status_t st = drv_dma_bus_master(t->dma, 1);   /* reset first, bus mastering after */
    if (st != OK) {
        drv_log("bus mastering refused (%s)", status_str(st));
        return;
    }
    t->bus_master = true;
    if (ring_setup(t) != OK)
        return;
    chip_rx_start(t);
    st = tally_dump(t, &o->start);
    o->start_ok = st == OK;
    if (st == OK)
        log_tally("at start", &o->start);
    else
        drv_log("tally at start: %s", status_str(st));
    o->cut = !wait_link(t, since);
    if (!o->cut && t->link)
        o->cut = !listen_60s(t, since);
    (void)census_harvest(t, false);
    census_log(&t->c);
    drv_log("interrupts: %u packet(s), %u poll(s) without one, isr bits seen %#x; link changes "
            "%u, interrupts with the link bit %u (the first link-up's included); ocp timeouts %u",
            t->c.irqs, t->c.polls, t->c.isr_seen, t->c.link_changes, t->c.linkchg_irqs,
            t->ocp_timeouts);
    st = tally_dump(t, &o->end);
    o->end_ok = st == OK;
    if (st == OK)
        log_tally("at end", &o->end);
    else
        drv_log("tally at end: %s", status_str(st));
}

/* The handles, the registers and the port; false (logged) if any is missing. */
static bool take_handles(struct rtl *t, const struct driver_start *ds)
{
    handle_t bar = drv_handle(ds, DR_BAR(2));
    t->dev = drv_handle(ds, DR_PCIDEV);
    t->dma = drv_handle(ds, DR_DMA);
    t->irq = drv_handle(ds, DR_IRQ(0));
    t->serve = drv_handle(ds, DR_SERVE);
    if (bar == HANDLE_INVALID || t->dev == HANDLE_INVALID || t->dma == HANDLE_INVALID ||
        t->irq == HANDLE_INVALID) {
        drv_log("missing handles (BAR 2 %#x, device %#x, DMA %#x, interrupt %#x): nothing "
                "touched", bar, t->dev, t->dma, t->irq);
        return false;
    }
    status_t st = drv_mmio_map(bar, 0, REGS_BYTES, VMO_CACHE_UC, &t->r);
    if (st == OK)
        st = drv_port_create(&t->port);
    if (st == OK)
        st = drv_port_bind(t->port, t->irq, KEY_IRQ, SIG_INTERRUPT, PORT_BIND_PERSISTENT);
    if (st == OK && t->serve != HANDLE_INVALID)
        st = drv_port_bind(t->port, t->serve, KEY_SERVE, SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK)
        drv_log("can't map the registers or bind the interrupt (%s): nothing touched",
                status_str(st));
    return st == OK;
}

int driver_main(const struct driver_start *ds)
{
    if (!drv_has_arg(ds, "netprobe")) {
        drv_log("started without the word netprobe: the network chip is left alone");
        return 0;
    }
    struct rtl *t = drv_malloc(sizeof(*t));
    struct outcome *o = drv_malloc(sizeof(*o));
    if (!t || !o) {
        drv_log("out of memory: nothing touched");
        return 0;
    }
    *t = (struct rtl){ 0 };
    *o = (struct outcome){ 0 };
    uint64_t since = drv_clock_ns();
    if (!take_handles(t, ds))
        return 0;
    probe(t, o, since);
    if (t->rx_on || t->bus_master)
        chip_stop(t);   /* i: before bus mastering goes off, so no DMA is cut mid-frame */
    if (t->bus_master && drv_dma_bus_master(t->dma, 0) == OK)
        t->bus_master = false;
    ring_free(t);
    if (o->reset)
        report(t, o);
    drv_log("done in %lu ms: the chip reset, bus mastering %s, nothing pinned%s",
            (unsigned long)((drv_clock_ns() - since) / NS_PER_MS),
            t->bus_master ? "STILL ON" : "off", t->ring_pinned || t->buf_pinned ? " (NOT)" : "");
    return 0;
}
