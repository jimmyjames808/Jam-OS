/* e1000e: start and stop (drv/e1000e), bound by devmgr to QEMU's Intel
 * 82574L (8086:10d3), the network card of every network test in QEMU
 * (tools/qemu-test.sh QEMU_NET); e1000e.h has the files and the rules.
 *
 * Handles (roles from <jam/driver.h>):
 *   DR_BAR(0)   the registers (128 KiB)
 *   DR_DMA      its dma_cap: bus mastering goes on after the reset
 *   DR_PCIDEV   its function (the ids, for the log)
 *   DR_IRQ(0)   MSI-X vector 0 (or MSI): every cause (chip.c routes them)
 *   DR_SERVE    the channel it serves abi/idl/netdev.idl on (the netdev
 *               server, <jam/netserver.h>; loop.c plugs the card in)
 * Arguments: `vlan=<id>` or `vlan=none` (devmgr passes the boot's network
 * mode to every network driver; <jam/netdev.h> netdev_vlan_args). Without
 * a valid one the chip is never touched: "no VLAN: the network stays
 * off", exit 0.
 *
 * Start, in this order: the registers mapped; the chip quiet and reset
 * (whatever a previous driver left queued is gone: the safe-rebind rule);
 * its address from the EEPROM; filters, flow control off, the VLAN
 * offloads off, autonegotiation without pause; then bus mastering on, the
 * rings pinned, the receiver and the transmitter on, interrupts on; then
 * the loop until devmgr closes DR_SERVE. Stop: the session ended, the
 * receiver and transmitter off, the chip reset, bus mastering off,
 * everything unpinned, exit 0. A failure on the way up exits 1 (devmgr
 * restarts it with backoff).
 *
 * EVERY REGISTER THE DRIVER WRITES (BAR 0; the 82574 datasheet's names):
 *   CTRL      GIO master disable, RST (reset); then ASDE | SLU with FRCSPD,
 *             FRCDPLX, RFCE, TFCE, VME (the chip's VLAN tags) and PHY_RST
 *             cleared
 *   CTRL_EXT  PBA_CLR, DRV_LOAD (cleared at stop)
 *   EERD      the EEPROM's words 0-2 (the address)
 *   MDIC      the PHY's ANAR (pause bits cleared) and BMCR (autonegotiation
 *             restarted)
 *   FCAL FCAH FCT FCTTV FCRTL FCRTH  0 (no flow control)
 *   VET       0x8100 (unused: VME is off)
 *   MTA, VFTA 0 (no multicast; the VLAN filter is off)
 *   RAL0/RAH0 our address, valid; RAL/RAH 1-15: 0
 *   RXCSUM RFCTL ITR RDTR RADV TIDV TADV EIAC IAM  0
 *   IVAR      every cause on vector 0
 *   IMS, IMC  the causes on; all off at stop and around the reset
 *   ICR       write 1 to clear
 *   RDBAL RDBAH RDLEN RDH RDT  the receive ring; RDT as descriptors go back
 *   RCTL      0 (stop); EN | BAM | SECRC (BSIZE 2048; never UPE, MPE, LPE,
 *             SBP, VFE, CFIEN)
 *   TCTL      0 (stop); EN | PSP | CT | COLD, in tx.c only
 *   TDBAL TDBAH TDLEN TDH TXDCTL TIPG  in tx.c only
 *   TDT       the transmit doorbell, in tx.c only
 * Read only: STATUS, EECD, the statistics (GPRC, GPTC, CRCERRS, ALGNERRC,
 * RXERRC, MPC, ECOL, LATECOL, XONTXC, XOFFTXC). Never written: PCI config
 * space, the flash (BAR 1), the MSI-X table (the kernel's). */
#include "e1000e.h"

/* The handles and the registers; false (logged) if any is missing. */
static bool take_handles(struct e1k *t, const struct driver_start *ds)
{
    handle_t bar = drv_handle(ds, DR_BAR(0));
    t->dev = drv_handle(ds, DR_PCIDEV);
    t->dma = drv_handle(ds, DR_DMA);
    t->irq = drv_handle(ds, DR_IRQ(0));
    t->serve = drv_handle(ds, DR_SERVE);
    if (bar == HANDLE_INVALID || t->dma == HANDLE_INVALID || t->serve == HANDLE_INVALID) {
        drv_log("missing handles (BAR 0 %#x, DMA %#x, serve %#x): nothing touched", bar, t->dma,
                t->serve);
        return false;
    }
    uint32_t ids = 0;
    if (t->dev != HANDLE_INVALID && drv_pci_config_read(t->dev, 0, 4, &ids) == OK) {
        t->vid = (uint16_t)ids;
        t->did = (uint16_t)(ids >> 16);
    }
    status_t st = drv_mmio_map(bar, 0, E1K_REGS_BYTES, VMO_CACHE_UC, &t->r);
    if (st == OK)
        st = drv_port_create(&t->port);
    if (st != OK)
        drv_log("can't map the registers or make the port (%s): nothing touched",
                status_str(st));
    return st == OK;
}

/* Reset to running: the chip set up, bus mastering, rings, receiver,
 * transmitter, interrupts. */
static status_t bring_up(struct e1k *t)
{
    status_t st = chip_reset(t);
    if (st == OK)
        st = chip_read_mac(t);
    if (st != OK)
        return st;
    chip_setup(t);
    st = drv_dma_bus_master(t->dma, 1);   /* reset first, bus mastering after */
    if (st != OK) {
        drv_log("bus mastering refused (%s)", status_str(st));
        return st;
    }
    t->bus_master = true;
    if ((st = ring_setup(t)) != OK)
        return st;
    wr32(t, E1K_RCTL, RCTL_VALUE);
    if ((st = tx_enable(t)) != OK)
        return st;
    chip_irq_enable(t, true);
    (void)chip_link_poll(t);
    char m[NETDEV_MODE_TEXT];
    drv_report("e1000e %04x:%04x (82574L): %s, %s, rings %u/%u, link %s at start", t->vid,
               t->did, netdev_mode_str(t->vlan, m), t->irq != HANDLE_INVALID ? "msi-x vector 0" : "polled",
               RX_DESCS, TX_DESCS, t->link ? "up" : "down (autonegotiating)");
    return OK;
}

static void log_totals(const struct e1k *t)
{
    const struct netdev_stats *s = &t->v.st;
    const uint64_t *d = t->rx_drop;
    srv_log(&t->v);
    drv_log("rx: dropped %lu untagged, %lu vlan 0, %lu other vlans or tags, %lu runts or too "
            "long, %lu the chip flagged or spread; tx: %lu sent",
            (unsigned long)d[NETFRAME_RX_UNTAGGED], (unsigned long)d[NETFRAME_RX_PRIORITY],
            (unsigned long)(d[NETFRAME_RX_OTHER_VLAN] + d[NETFRAME_RX_OUTER] +
                            d[NETFRAME_RX_NESTED]),
            (unsigned long)(d[NETFRAME_RX_RUNT] + d[NETFRAME_RX_LONG]),
            (unsigned long)t->rx_errors, (unsigned long)t->tx_done);
    drv_log("chip: sent %lu (the driver queued %lu: %s), received %lu, errors rx %lu tx %lu, "
            "missed %lu, pause frames sent %lu; %lu interrupt(s), %lu poll(s), icr bits %#x",
            (unsigned long)t->chip.tx_ok, (unsigned long)s->tx_frames,
            t->chip.tx_ok == s->tx_frames ? "equal" : "DIFFERS", (unsigned long)t->chip.rx_ok,
            (unsigned long)t->chip.rx_err, (unsigned long)t->chip.tx_err,
            (unsigned long)t->chip.missed, (unsigned long)t->chip.pause_sent,
            (unsigned long)t->irqs, (unsigned long)t->polls, t->icr_seen);
}

static void stop(struct e1k *t)
{
    srv_end(&t->v);   /* the session, if any (nothing if the server never started) */
    if (t->bus_master || t->tx_on)
        chip_stop(t);   /* before bus mastering goes off, so no DMA is cut mid-frame */
    if (t->bus_master && drv_dma_bus_master(t->dma, 0) == OK)
        t->bus_master = false;
    ring_free(t);
    if (t->port != HANDLE_INVALID)
        drv_handle_close(t->port);
}

int driver_main(const struct driver_start *ds)
{
    uint16_t vlan = netdev_vlan_args(ds->args, ds->nargs);
    if (!vlan) {
        drv_log("no VLAN: the network stays off (the chip is left alone)");
        return 0;
    }
    struct e1k *t = drv_malloc(sizeof(*t));
    if (!t) {
        drv_log("out of memory: nothing touched");
        return 1;
    }
    *t = (struct e1k){ .vlan = vlan, .started = drv_clock_ns(), .port = HANDLE_INVALID,
                       .ring_vmo = HANDLE_INVALID, .buf_vmo = HANDLE_INVALID };
    if (!take_handles(t, ds))
        return 1;
    status_t st = bring_up(t);
    if (st == OK)
        st = loop_run(t);
    stop(t);
    log_totals(t);
    drv_log("stopped (%s): the chip reset, bus mastering %s, nothing pinned%s",
            st == OK ? "devmgr closed our channel" : status_str(st),
            t->bus_master ? "STILL ON" : "off", t->ring_pinned || t->buf_pinned ? " (NOT)" : "");
    return st == OK ? 0 : 1;
}
