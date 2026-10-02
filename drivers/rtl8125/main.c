/* rtl8125: the driver of the PC's Realtek RTL8125B (drv/rtl8125), started
 * by devmgr for 10ec:8125 only on a boot with `netprobe` (the boot menu's
 * "Jam OS (network: listen only)") or `netsend` ("Jam OS (network: send
 * test)"); any other boot never touches the chip.
 *
 * The mode comes from the arguments, once (args.h):
 *   - `netprobe`: the listen-only probe (probe.c). Nothing is sent.
 *   - full mode, which needs a valid vlan=<n> (the kernel's VLAN, passed
 *     on by devmgr): without one, "no VLAN: the network stays off" and
 *     the chip is never touched. With `netsend` it runs the ARP send
 *     test (sendtest.c); without it there is nothing to do yet (the
 *     netdev server that netstack talks to is not built yet).
 * Every frame full mode sends goes through tx.c, which tags it with the
 * VLAN; every frame it keeps went through rx.c's VLAN check.
 *
 * The steps, one log line or a few each, every wait bounded:
 *   a. what the firmware left (reads only): PCI command, chip command
 *      (a transmitter left ON is reported loudly), interrupts, rings,
 *      wake-on-LAN, the PCIe link;
 *   b. the chip's id from TXCFG (0x641: 8125B; anything else: stop);
 *   c. its MAC address, into the log only;
 *   d. reset (rge's order), then the PHY's id, its patch version, the
 *      MAC's patch state and the receive registers' values;
 *   e. wake-on-LAN off (rge_wol), as long as Jam OS runs;
 *   f. bus mastering on, the rings (where they landed: 64-bit DMA), the
 *      bring-up (full mode: the transmit ring and enable, through tx.c),
 *      autonegotiation 10/100/1000/2500 without pause, the tally;
 *   g. the mode's run, on one MSI-X vector and the loop (loop.c);
 *   h. the tally again: the chip's count of frames sent against the
 *      driver's (0 for the probe), the plan's PAUSE check;
 *   i. receiver and transmitter off, the chip reset, bus mastering off,
 *      everything unpinned;
 *   j. one RESULTS line.
 * It always exits 0 (devmgr then leaves it finished): each mode is run
 * once, and its log says what went wrong.
 *
 * EVERY REGISTER THE DRIVER WRITES (offsets in BAR 2; rge's names):
 *   0x34 INT_CFG0     bit 0 cleared (the 8125B's interrupt type)
 *   0x37 CMD          0x80 stop request with the receive enable kept, then
 *                     the receive enable alone, 0x10 reset, then 0x08
 *                     (probe) or, from tx.c only, 0x0c (full mode)
 *   0x38 IMR          0, then the receive and link bits (probe), plus the
 *                     transmit bits (full mode)
 *   0x3c ISR          write-1-to-clear acknowledgements
 *   0x44 RXCFG        accept bits cleared (reset, stop); 0x41000c00, then
 *                     accept-all 0x0f (probe) or our address and broadcast
 *                     0x0a (full mode); tag stripping (23:22) never set
 *   0x08, 0x0c MAR    all ones (probe) or 0 (full mode: no multicast)
 *   0x10, 0x14 DTCCR  the tally dump's address and its dump bit
 *   0x50 EECMD        config unlock (0xc0) set and cleared around set-up
 *   0x52 CFG1         speed-down bit cleared
 *   0x54 CFG3, 0x56 CFG5  wake-on-LAN bits cleared
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
 *   0xb0 MACOCP       the MAC's window: c0bc (RealWoW off), c0b6 (wake
 *                     PME off), e8de, c0aa, c0a6, c01e, d42c, c0ac, c140,
 *                     c142, eb58, e614, e63e, c0b4, eb6a, eb50, e056, e040,
 *                     ea1c, e0c0, e052, d430, e080 (EEE plus off), eb54,
 *                     e098, e032, e092; and the read index
 *   0xb8 PHYOCP       the PHY's window: a466, a468 (out of band exit),
 *                     a436 (the index to read the patch version), a428,
 *                     a5ea, a5d4 (2500 advertised), and the MII BMCR,
 *                     ANAR (pause bits cleared) and GTCR; and the read index
 * and in full mode only, from tx.c only (notx.h):
 *   0x20, 0x24 TXDESC the transmit ring's address
 *   0x40 TXCFG        0x03000700
 *   0x57 TDFNR        0x10
 *   0x90 TXSTART      1: the doorbell
 * Never written: 0x28-0x2f (the high-priority ring), 0x00-0x05 (the
 * address), 0xe0 CPLUSCMD, the CSI window, the MAC's and PHY's patch RAM,
 * and PCI config space. */
#include "rtl8125.h"

#define REGS_BYTES    0x10000            /* BAR 2 (docs/HARDWARE.md: 64 KiB) */

/* Does this start touch the chip at all? Logs why not. */
static bool will_run(const struct rtl_args *a)
{
    if (a->mode == RTL_MODE_PROBE) {
        drv_log("netprobe: listen only, nothing is sent");
        return true;
    }
    if (!a->vlan) {
        drv_log("no VLAN: the network stays off (the chip is left alone)");
        return false;
    }
    if (!a->sendtest) {
        drv_log("vlan %u: the netdev server is not built yet; the chip is left alone", a->vlan);
        return false;
    }
    uint32_t ip = a->arp_target;
    drv_log("netsend: full mode on vlan %u, ARP probes for %u.%u.%u.%u%s", a->vlan, ip >> 24,
            (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff,
            a->bad_target ? " (an arpto= word that isn't an address was ignored)" : "");
    return true;
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
        st = loop_init(t);
    if (st != OK)
        drv_log("can't map the registers or bind the interrupt (%s): nothing touched",
                status_str(st));
    return st == OK;
}

static void tally_at(struct rtl *t, const char *when, struct tally *x, bool *ok)
{
    status_t st = tally_dump(t, x);
    *ok = st == OK;
    if (st == OK)
        tally_log(when, x);
    else
        drv_log("tally %s: %s", when, status_str(st));
}

/* a-f. False if the chip can't be run (logged). */
static bool bring_up(struct rtl *t, struct outcome *o)
{
    chip_snapshot(t);
    if (chip_identify(t) != OK)
        return false;
    chip_read_mac(t);
    if (chip_reset(t) != OK)
        return false;
    o->reset = true;
    o->phy = chip_log_after_reset(t, &o->rcode);
    chip_wol_off(t);
    status_t st = drv_dma_bus_master(t->dma, 1);   /* reset first, bus mastering after */
    if (st != OK) {
        drv_log("bus mastering refused (%s)", status_str(st));
        return false;
    }
    t->bus_master = true;
    if (ring_setup(t) != OK)
        return false;
    st = chip_start(t);
    tally_at(t, "at start", &o->start, &o->start_ok);
    if (st != OK)
        drv_log("bring-up failed (%s): stopping", status_str(st));
    return st == OK;
}

/* h and i: the last frames, the tally check, the chip stopped. */
static void finish(struct rtl *t, struct outcome *o)
{
    if (t->rx_on) {
        if (t->mode == RTL_MODE_PROBE) {
            (void)census_harvest(t, false);
        } else {
            (void)rx_harvest(t);
            (void)tx_reap(t);
        }
        tally_at(t, "at end", &o->end, &o->end_ok);
    }
    if (o->reset)
        (void)tally_tx_check(t, o, o->txcheck, sizeof(o->txcheck));   /* logged */
    if (t->rx_on || t->bus_master)
        chip_stop(t);   /* before bus mastering goes off, so no DMA is cut mid-frame */
    if (t->bus_master && drv_dma_bus_master(t->dma, 0) == OK)
        t->bus_master = false;
    ring_free(t);
}

int driver_main(const struct driver_start *ds)
{
    struct rtl_args a = rtl_args_parse(ds->args, ds->nargs);
    if (!will_run(&a))
        return 0;
    struct rtl *t = drv_malloc(sizeof(*t));
    struct outcome *o = drv_malloc(sizeof(*o));
    if (!t || !o) {
        drv_log("out of memory: nothing touched");
        return 0;
    }
    *t = (struct rtl){ 0 };
    *o = (struct outcome){ 0 };
    t->mode = a.mode;   /* the one place the mode is set: tx.c's gate reads it */
    t->vlan = a.vlan;   /* and the VLAN, from rtl_vlan_arg alone */
    t->since = drv_clock_ns();
    if (!take_handles(t, ds))
        return 0;
    if (bring_up(t, o)) {
        if (t->mode == RTL_MODE_PROBE)
            probe_run(t, o);
        else
            sendtest_run(t, &a, o);
    }
    finish(t, o);
    if (o->reset && t->mode == RTL_MODE_PROBE)
        probe_report(t, o);
    else if (o->reset)
        sendtest_report(t, o);
    drv_log("done in %lu ms: the chip reset, bus mastering %s, nothing pinned%s",
            (unsigned long)((drv_clock_ns() - t->since) / NS_PER_MS),
            t->bus_master ? "STILL ON" : "off",
            t->ring_pinned || t->buf_pinned || t->txbuf_pinned ? " (NOT)" : "");
    return 0;
}
