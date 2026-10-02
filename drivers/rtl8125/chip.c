/* rtl8125: the chip (drv/rtl8125): what the firmware left, which chip it
 * is, reset, the receive-only bring-up, autonegotiation, the link, and
 * the stop. The order and values are rge's (if_rge.c: rge_attach,
 * rge_init, rge_stop, rge_reset, rge_exit_oob, rge_set_phy_power,
 * rge_ifmedia_upd), with everything about transmitting left out and
 * without the firmware tables (the MAC's break points, the PCIe PHY
 * table, the PHY's tuning and its patch: the owner's call, M9-PLAN
 * question 5): the chip runs on whatever the board's firmware loaded,
 * and chip_log_after_reset says what that was. Every wait is bounded. */
#include "rtl8125.h"

#define RESET_FIFO_NS  (150 * NS_PER_MS)   /* rge: 3000 x 50 us, twice */
#define RESET_WAIT_NS  (10 * NS_PER_MS)    /* rge: RGE_TIMEOUT x 100 us */
#define PHY_STATE_NS   (100 * NS_PER_MS)   /* rge: RGE_TIMEOUT x 1 ms */
#define LINK_LINES     12                  /* link-change lines logged at most */

/* Poll reg (1 or 2 bytes wide) until (value & mask) == want or ns pass. */
static bool wait_reg(struct rtl *t, uint32_t reg, unsigned width, uint16_t mask, uint16_t want,
                     uint64_t ns)
{
    uint64_t end = drv_clock_ns() + ns;
    for (;;) {
        uint16_t v = width == 1 ? rd8(t, reg) : rd16(t, reg);
        if ((v & mask) == want)
            return true;
        if (drv_clock_ns() > end)
            return false;
        delay_us(50);
    }
}

static bool wait_phy_state(struct rtl *t, uint16_t want)
{
    uint64_t end = drv_clock_ns() + PHY_STATE_NS;
    while ((phy_rd(t, PHY_STATE) & 7) != want) {
        if (drv_clock_ns() > end)
            return false;
        delay_us(1000);
    }
    return true;
}

/* The PCIe link and MSI-X as the function reports them (config reads only). */
static void log_pci_caps(struct rtl *t)
{
    uint32_t ptr = 0;
    (void)drv_pci_config_read(t->dev, 0x34, 1, &ptr);
    for (unsigned guard = 0; ptr >= 0x40 && ptr < 0x100 && guard < 48; guard++) {
        uint32_t hdr = 0, a = 0, b = 0;
        if (drv_pci_config_read(t->dev, ptr, 2, &hdr) != OK)
            return;
        if ((hdr & 0xff) == 0x10 && drv_pci_config_read(t->dev, ptr + 0x10, 2, &a) == OK &&
            drv_pci_config_read(t->dev, ptr + 0x12, 2, &b) == OK)
            drv_log("pcie: link gen %u x%u, ASPM control %u (L0s %s, L1 %s)", b & 0xf,
                    (b >> 4) & 0x3f, a & 3, a & 1 ? "on" : "off", a & 2 ? "on" : "off");
        if ((hdr & 0xff) == 0x11 && drv_pci_config_read(t->dev, ptr + 2, 2, &a) == OK)
            drv_log("pcie: MSI-X %u vectors, %s", (a & 0x7ff) + 1,
                    a & 0x8000 ? "enabled" : "disabled");
        ptr = hdr >> 8 & 0xfc;
    }
}

void chip_snapshot(struct rtl *t)
{
    uint32_t pcmd = 0;
    (void)drv_pci_config_read(t->dev, 4, 2, &pcmd);   /* 0 if refused */
    uint8_t cmd = rd8(t, RTL_CMD), cfg3 = rd8(t, RTL_CFG3), cfg5 = rd8(t, RTL_CFG5);
    uint8_t mcu = rd8(t, RTL_MCUCMD);
    drv_log("before: pci command %#06x (memory %s, bus master %s: devmgr turns it off before "
            "a driver starts)", pcmd, pcmd & 2 ? "on" : "off", pcmd & 4 ? "on" : "off");
    drv_log("before: chip command %#04x: receiver %s, transmitter %s", cmd,
            cmd & RTL_CMD_RXENB ? "ON" : "off", cmd & RTL_CMD_TXENB ? "ON" : "off");
    drv_log("before: imr %#x isr %#x rxcfg %#x cplus %#x phystat %#x", rd32(t, RTL_IMR),
            rd32(t, RTL_ISR), rd32(t, RTL_RXCFG), rd16(t, RTL_CPLUSCMD), rd16(t, RTL_PHYSTAT));
    drv_log("before: cfg1 %#x cfg2 %#x cfg3 %#x cfg5 %#x mcu %#x (%s)", rd8(t, RTL_CFG1),
            rd8(t, RTL_CFG2), cfg3, cfg5, mcu,
            mcu & RTL_MCUCMD_IS_OOB ? "the chip's firmware owns it" : "not out of band");
    drv_log("before: receive ring at %#010x%08x, transmit ring at %#010x%08x",
            rd32(t, RTL_RXDESC_HI), rd32(t, RTL_RXDESC_LO), rd32(t, RTL_TXDESC_HI),
            rd32(t, RTL_TXDESC_LO));
    drv_log("before: wake-on-LAN %s (cfg3 link/magic %#x, cfg5 wake %#x)",
            (cfg3 & (RTL_CFG3_WOL_LINK | RTL_CFG3_WOL_MAGIC)) || (cfg5 & RTL_CFG5_WOL_ANY)
            ? "armed by the firmware" : "off", cfg3 & (RTL_CFG3_WOL_LINK | RTL_CFG3_WOL_MAGIC),
            cfg5 & RTL_CFG5_WOL_ANY);
    if (cmd != 0xff && (cmd & RTL_CMD_TXENB))
        drv_report("WARNING: the firmware left the TRANSMITTER ON (command %#x): it may have "
                   "sent untagged frames before Jam OS started (PXE?)", cmd);
    log_pci_caps(t);
}

status_t chip_identify(struct rtl *t)
{
    uint32_t txcfg = rd32(t, RTL_TXCFG);
    if (txcfg == 0xffffffffu) {
        drv_report("its registers read all ones: the chip is gone or asleep");
        return ERR_NOT_FOUND;
    }
    t->xid = (txcfg >> 20) & 0x7cf;
    const char *name = t->xid == 0x641 ? "RTL8125B" : t->xid == 0x609 ? "RTL8125 (the first)"
                     : t->xid == 0x688 || t->xid == 0x689 ? "RTL8125D"
                     : t->xid == 0x649 || t->xid == 0x64a ? "RTL8126"
                     : t->xid == 0x6c9 ? "RTL8127" : "unknown";
    drv_log("chip: xid %03x (txcfg %#010x): %s", t->xid, txcfg, name);
    if (t->xid == 0x641)
        return OK;
    drv_report("xid %03x (%s): the probe knows the 8125B (641) only; stopping, nothing touched",
               t->xid, name);
    return ERR_NOT_SUPPORTED;
}

/* Into the log only (the owner's own hardware); never into a doc. */
void chip_log_mac(struct rtl *t)
{
    uint8_t m[6], o[6];
    uint32_t a0 = rd32(t, RTL_ADDR0);
    uint16_t a1 = rd16(t, RTL_ADDR1);
    for (unsigned i = 0; i < 6; i++) {
        m[i] = rd8(t, RTL_MAC0 + i);
        o[i] = (uint8_t)(i < 4 ? a0 >> (8 * i) : (uint32_t)a1 >> (8 * (i - 4)));
    }
    bool same = true;
    for (unsigned i = 0; i < 6; i++)
        same &= m[i] == o[i];
    drv_log("mac: %02x:%02x:%02x:%02x:%02x:%02x as the chip reports it; its own copy at 0x19e0 "
            "%s%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5],
            same ? "is the same, " : "DIFFERS: ", o[0], o[1], o[2], o[3], o[4], o[5]);
}

/* rge_reset: the receive filter off, the receive gate shut, a stop
 * request, the FIFOs drained (bounded), then the soft reset. Where rge
 * writes the receive and transmit enables back, this keeps only the
 * receive one, so a transmitter the firmware left on is switched off. */
static void reset(struct rtl *t)
{
    wr32(t, RTL_RXCFG, rd32(t, RTL_RXCFG) & ~(RTL_RXCFG_ACCEPT | RTL_RXCFG_RUNT |
                                             RTL_RXCFG_ERRPKT));
    set8(t, RTL_PPSW, 0x08);   /* the RXDV gate on */
    wr8(t, RTL_CMD, (uint8_t)((rd8(t, RTL_CMD) & RTL_CMD_RXENB) | RTL_CMD_STOPREQ));
    delay_us(200);
    uint16_t fifos = RTL_MCUCMD_RXFIFO_EMPTY | RTL_MCUCMD_TXFIFO_EMPTY;
    if (!wait_reg(t, RTL_MCUCMD, 1, fifos, fifos, RESET_FIFO_NS))
        drv_log("reset: the FIFOs did not drain in 150 ms (mcu %#x); resetting anyway",
                rd8(t, RTL_MCUCMD));
    if (!wait_reg(t, RTL_IM, 2, 0x0103, 0x0103, RESET_FIFO_NS))
        drv_log("reset: im %#x did not reach 0x0103 in 150 ms", rd16(t, RTL_IM));
    wr8(t, RTL_CMD, rd8(t, RTL_CMD) & RTL_CMD_RXENB);
    wr8(t, RTL_CMD, RTL_CMD_RESET);
    if (!wait_reg(t, RTL_CMD, 1, RTL_CMD_RESET, 0, RESET_WAIT_NS))
        drv_log("reset: the chip did not finish its reset in 10 ms");
}

/* rge_hw_reset: interrupts masked and acked, the timers off, reset. */
static void hw_reset(struct rtl *t)
{
    wr32(t, RTL_IMR, 0);
    wr32(t, RTL_ISR, rd32(t, RTL_ISR));
    for (unsigned i = 0; i < 4; i++)
        wr32(t, RTL_TIMERINT(i), 0);
    reset(t);
}

/* rge_exit_oob: take the chip from its own firmware (the "out of band"
 * management that runs while no driver does), RealWoW off. */
static void exit_oob(struct rtl *t)
{
    mac_wr(t, 0xc0bc, 0x00ff);   /* RealWoW off */
    reset(t);
    clr8(t, RTL_MCUCMD, RTL_MCUCMD_IS_OOB);
    mac_mod(t, 0xe8de, 0x4000, 0);
    if (!wait_reg(t, RTL_TWICMD, 2, 0x0200, 0x0200, NS_PER_MS))
        drv_log("exit oob: twicmd %#x without bit 9 after 1 ms", rd16(t, RTL_TWICMD));
    mac_wr(t, 0xc0aa, 0x07d0);
    mac_wr(t, 0xc0a6, 0x01b5);
    mac_wr(t, 0xc01e, 0x5555);
    if (!wait_reg(t, RTL_TWICMD, 2, 0x0200, 0x0200, NS_PER_MS))
        drv_log("exit oob: twicmd %#x without bit 9 after 1 ms", rd16(t, RTL_TWICMD));
    if (mac_rd(t, 0xd42c) & 0x0100) {   /* the PHY was in its own power save */
        if (!wait_phy_state(t, 2))
            drv_log("exit oob: phy state %u, not 2, after 100 ms", phy_rd(t, PHY_STATE) & 7);
        mac_mod(t, 0xd42c, 0x0100, 0);
        phy_mod(t, 0xa466, 0x0001, 0);
        phy_mod(t, 0xa468, 0x000a, 0);
    }
}

status_t chip_reset(struct rtl *t)
{
    uint64_t t0 = drv_clock_ns();
    bool oob = rd8(t, RTL_MCUCMD) & RTL_MCUCMD_IS_OOB;
    /* rge_stop's part, then rge_chipinit (without rge_hw_init's tables). */
    wr32(t, RTL_RXCFG, rd32(t, RTL_RXCFG) & ~(RTL_RXCFG_ACCEPT | RTL_RXCFG_RUNT |
                                             RTL_RXCFG_ERRPKT));
    hw_reset(t);
    mac_mod(t, 0xc0ac, 0x1f80, 0);
    exit_oob(t);
    set8(t, RTL_PMCH, 0xc0);   /* rge_set_phy_power(on) */
    mii_wr(t, MII_BMCR, BMCR_AUTOEN);
    bool phy_up = wait_phy_state(t, 3);
    hw_reset(t);
    uint8_t cmd = rd8(t, RTL_CMD);
    drv_log("reset: %lu ms, out of band %s, phy %s, command now %#x", (unsigned long)
            ((drv_clock_ns() - t0) / NS_PER_MS), oob ? "yes (left it)" : "no",
            phy_up ? "ready" : "NOT ready after 100 ms", cmd);
    if (cmd == 0xff)
        return ERR_NOT_FOUND;
    return OK;
}

uint32_t chip_log_after_reset(struct rtl *t, uint16_t *rcode)
{
    drv_log("after reset: rxcfg %#x rxmaxsize %u cplus %#x mcu %#x int_cfg0 %#x ppsw %#x im %#x",
            rd32(t, RTL_RXCFG), rd16(t, RTL_RXMAXSIZE), rd16(t, RTL_CPLUSCMD),
            rd8(t, RTL_MCUCMD), rd8(t, RTL_INT_CFG0), rd8(t, RTL_PPSW), rd16(t, RTL_IM));
    drv_log("after reset: cfg1 %#x cfg2 %#x cfg3 %#x cfg5 %#x imr %#x isr %#x",
            rd8(t, RTL_CFG1), rd8(t, RTL_CFG2), rd8(t, RTL_CFG3), rd8(t, RTL_CFG5),
            rd32(t, RTL_IMR), rd32(t, RTL_ISR));
    uint16_t fc26 = mac_rd(t, 0xfc26), fc48 = mac_rd(t, 0xfc48);
    drv_log("mac mcu: fc26 %#06x fc48 %#06x: %s", fc26, fc48, fc26 & 0x8000
            ? "break points set (by the firmware): the MAC runs patched code"
            : "no break points: the MAC runs its ROM code");
    uint32_t id = (uint32_t)mii_rd(t, MII_PHYID1) << 16 | mii_rd(t, MII_PHYID2);
    phy_wr(t, PHY_RAM_INDEX, PHY_RAM_RCODE);   /* rge_phy_config: read the ram code version */
    *rcode = phy_rd(t, PHY_RAM_DATA);
    drv_log("phy: id %08x, bmsr %#06x, state %u, patch (ram code) version %#06x: %s", id,
            mii_rd(t, MII_BMSR), phy_rd(t, PHY_STATE) & 7, *rcode,
            *rcode == RCODE_8125B ? "rge's 8125B patch version: the firmware loaded the patch"
            : "not rge's 0x0b99: no patch, or another one");
    return id;
}

/* rge_init's MAC settings for the 8125B, in its order, over the OCP window
 * (their meaning is not documented anywhere public; rge's values). Left
 * out: the transmit descriptor fetch number (TDFNR), the PCIe settings
 * through CSI and the ASPM/CLKREQ changes. */
static void mac_setup(struct rtl *t)
{
    wr8(t, RTL_RSS_CTRL, 0);   /* one queue: no RSS */
    wr16(t, RTL_RXQUEUE_CTRL, rd16(t, RTL_RXQUEUE_CTRL) & ~0x001c);
    clr8(t, RTL_CFG1, RTL_CFG1_SPEED_DOWN);
    mac_wr(t, 0xc140, 0xffff);
    mac_wr(t, 0xc142, 0xffff);
    mac_mod(t, 0xeb58, 0, 0x0001);
    mac_mod(t, 0xe614, 0x0700, 0x0200);
    mac_mod(t, 0xe63e, 0x0c00, 0);
    mac_mod(t, 0xe63e, 0x0030, 0x0020);
    mac_mod(t, 0xc0b4, 0x0001, 0);
    mac_mod(t, 0xc0b4, 0, 0x0001);
    mac_mod(t, 0xc0b4, 0, 0x000c);
    mac_mod(t, 0xeb6a, 0x00ff, 0x0033);
    mac_mod(t, 0xeb50, 0x03e0, 0x0040);
    mac_mod(t, 0xe056, 0x00f0, 0);
    mac_mod(t, 0xe040, 0x1000, 0);
    mac_mod(t, 0xea1c, 0x0003, 0x0001);
    mac_wr(t, 0xe0c0, 0x4000);
    mac_mod(t, 0xe052, 0x0088, 0x0060);
    mac_mod(t, 0xd430, 0x0fff, 0x045f);
    set8(t, RTL_DLLPR, 0xc0);              /* PFM_EN | TX_10M_PS_EN */
    mac_mod(t, 0xe080, 0x0002, 0);         /* EEE plus off */
    mac_mod(t, 0xea1c, 0x0004, 0);
    mac_mod(t, 0xeb54, 0, 0x0001);         /* clear the TCAM entries */
    delay_us(1);
    mac_mod(t, 0xeb54, 0x0001, 0);
    wr16(t, 0x1880, rd16(t, 0x1880) & ~0x0030);
    clr8(t, RTL_INT_CFG0, RTL_INT_CFG0_EN);   /* the 8125B's interrupt type */
    for (unsigned i = 0; i < 4; i++)
        wr32(t, RTL_TIMERINT(i), 0);
    for (unsigned i = 0; i < 32; i++)     /* no interrupt moderation: one per frame */
        wr32(t, RTL_INTMITI(i), 0);
    mac_mod(t, 0xc0ac, 0, 0x1f80);
    mac_wr(t, 0xe098, 0xc302);
    mac_mod(t, 0xe032, 0x0003, 0);
    mac_mod(t, 0xe092, 0x00ff, 0);
}

void chip_rx_start(struct rtl *t)
{
    set8(t, RTL_EECMD, RTL_EECMD_WRITECFG);
    wr32(t, RTL_RXDESC_LO, (uint32_t)t->ring_addr);
    wr32(t, RTL_RXDESC_HI, (uint32_t)(t->ring_addr >> 32));
    wr32(t, RTL_RXCFG, RTL_RXCFG_8125B);   /* the tag stripping (RXCFG 23:22) stays off */
    mac_setup(t);
    wr16(t, RTL_RXMAXSIZE, RX_BUF);         /* a whole frame always fits one buffer */
    clr8(t, RTL_PPSW, 0x08);                /* the RXDV gate off */
    delay_us(2000);
    /* rge_iff, promiscuous: every frame, every multicast group. */
    wr32(t, RTL_RXCFG, (rd32(t, RTL_RXCFG) & ~RTL_RXCFG_VLANSTRIP) | RTL_RXCFG_ACCEPT);
    wr32(t, RTL_MAR0, 0xffffffffu);
    wr32(t, RTL_MAR4, 0xffffffffu);
    clr8(t, RTL_EECMD, RTL_EECMD_WRITECFG);
    delay_us(10);
    chip_autoneg(t);
    wr8(t, RTL_CMD, RTL_CMD_RXENB);         /* the receiver only */
    wr32(t, RTL_IMR, RTL_IMR_PROBE);        /* rge_setup_intr(NONE), receive bits only */
    wr32(t, RTL_TIMERINT(0), 0);
    wr16(t, RTL_IM, 0);
    t->rx_on = true;
    drv_log("receiver on: rxcfg %#x (tag stripping %s), rxmaxsize %u, imr %#x, command %#x",
            rd32(t, RTL_RXCFG), rd32(t, RTL_RXCFG) & RTL_RXCFG_VLANSTRIP ? "ON" : "off",
            rd16(t, RTL_RXMAXSIZE), rd32(t, RTL_IMR), rd8(t, RTL_CMD));
}

/* rge_ifmedia_upd for "auto": 10/100/1000/2500, and never pause. */
void chip_autoneg(struct rtl *t)
{
    phy_mod(t, 0xa428, 0x0200, 0);          /* "Gigabit Lite" off */
    phy_mod(t, 0xa5ea, 0x0001, 0);
    phy_mod(t, PHY_ADV_2500, PHY_ADV_2500_FD, PHY_ADV_2500_FD);
    uint16_t anar = mii_rd(t, MII_ANAR);
    anar &= (uint16_t)~(ANAR_10 | ANAR_10_FD | ANAR_TX | ANAR_TX_FD | ANAR_PAUSE |
                        ANAR_PAUSE_ASYM);
    mii_wr(t, MII_ANAR, anar | ANAR_10 | ANAR_10_FD | ANAR_TX | ANAR_TX_FD);
    mii_wr(t, MII_GTCR, (uint16_t)((mii_rd(t, MII_GTCR) & ~(GTCR_1000_FDX | GTCR_1000_HDX)) |
                                   GTCR_1000_FDX | GTCR_1000_HDX));
    mii_wr(t, MII_BMCR, BMCR_RESET | BMCR_AUTOEN | BMCR_STARTNEG);
    uint16_t a = mii_rd(t, MII_ANAR);
    drv_log("autonegotiation: advertising anar %#06x gtcr %#06x 2500 %s, pause %s", a,
            mii_rd(t, MII_GTCR), phy_rd(t, PHY_ADV_2500) & PHY_ADV_2500_FD ? "yes" : "no",
            a & (ANAR_PAUSE | ANAR_PAUSE_ASYM) ? "ADVERTISED" : "not advertised");
}

void chip_link_str(uint16_t ps, char *buf, size_t size)
{
    if (!(ps & RTL_PHYSTAT_LINK)) {
        drv_snprintf(buf, size, "down");
        return;
    }
    unsigned speed = ps & RTL_PHYSTAT_2500 ? 2500 : ps & RTL_PHYSTAT_1000 ? 1000
                   : ps & RTL_PHYSTAT_100 ? 100 : ps & RTL_PHYSTAT_10 ? 10 : 0;
    bool full = (ps & RTL_PHYSTAT_FDX) || speed >= 1000;   /* rge_ifmedia_sts */
    drv_snprintf(buf, size, "%u %s%s%s", speed, full ? "full" : "half",
                 ps & RTL_PHYSTAT_RXFLOW ? ", pause rx" : "",
                 ps & RTL_PHYSTAT_TXFLOW ? ", PAUSE TX" : "");
}

bool chip_link_poll(struct rtl *t, uint64_t since)
{
    uint16_t ps = rd16(t, RTL_PHYSTAT);
    bool up = ps & RTL_PHYSTAT_LINK;
    if (ps == t->phystat && up == t->link)
        return up;
    if (up != t->link && t->link_at)
        t->c.link_changes++;   /* changes after the first link-up */
    if (up && !t->link_at)
        t->link_at = drv_clock_ns();
    t->link = up;
    t->phystat = ps;
    if (t->link_lines++ < LINK_LINES) {
        char s[48];
        chip_link_str(ps, s, sizeof(s));
        uint64_t ms = (drv_clock_ns() - since) / NS_PER_MS;
        drv_log("link: %s (phystat %#06x) at %lu.%02lu s", s, ps, (unsigned long)(ms / 1000),
                (unsigned long)(ms % 1000 / 10));
    }
    return up;
}

void chip_stop(struct rtl *t)
{
    wr32(t, RTL_RXCFG, rd32(t, RTL_RXCFG) & ~(RTL_RXCFG_ACCEPT | RTL_RXCFG_RUNT |
                                             RTL_RXCFG_ERRPKT));
    hw_reset(t);   /* the receiver off with the rest: the command register is 0 after it */
    mac_mod(t, 0xc0ac, 0x1f80, 0);
    t->rx_on = false;
    drv_log("stopped: command %#x, imr %#x", rd8(t, RTL_CMD), rd32(t, RTL_IMR));
}
