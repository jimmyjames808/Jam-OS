/* e1000e: the chip (drv/e1000e): reset, its address, the PHY, the
 * filters and offloads, the link, its counters, stop.
 *
 * Reset follows em's order (e1000_reset_hw_82571 for the 82574: the
 * interrupts masked, receiver and transmitter off, the GIO master
 * disabled so no DMA is in flight, then CTRL.RST and the EEPROM's
 * auto-read). Everything a previous driver (killed, crashed) left queued
 * is gone before bus mastering goes on: the safe-rebind rule.
 *
 * Every wait is bounded in time (CODING-GUIDE "Every hardware wait is
 * bounded"); the longest, the reset, is 100 ms. */
#include "e1000e.h"

#define GIO_WAIT_NS    (10 * NS_PER_MS)
#define RESET_WAIT_NS  (100 * NS_PER_MS)
#define EEPROM_WAIT_NS (10 * NS_PER_MS)
#define MDIC_WAIT_NS   (10 * NS_PER_MS)
#define LINK_LINES     12             /* link changes logged, then one in 64 */

uint32_t rd32(const struct e1k *t, uint32_t reg)
{
    return drv_read32(t->r, reg);
}

void wr32(struct e1k *t, uint32_t reg, uint32_t v)
{
    drv_write32(t->r, reg, v);
}

/* Wait until (reg & mask) == want, at most ns; false if it never was. */
static bool wait_for(struct e1k *t, uint32_t reg, uint32_t mask, uint32_t want, uint64_t ns)
{
    uint64_t end = drv_clock_ns() + ns;
    for (;;) {
        uint32_t v = rd32(t, reg);
        if (v != 0xffffffffu && (v & mask) == want)
            return true;
        if (drv_clock_ns() >= end)
            return false;
        __builtin_ia32_pause();
    }
}

status_t chip_reset(struct e1k *t)
{
    uint32_t status = rd32(t, E1K_STATUS);
    if (status == 0xffffffffu) {
        drv_report("e1000e %04x:%04x: its registers read all ones: the chip is gone", t->vid,
                   t->did);
        return ERR_NOT_FOUND;
    }
    drv_log("found: status %#x, ctrl %#x, rctl %#x, tctl %#x (what the firmware or the last "
            "driver left)", status, rd32(t, E1K_CTRL), rd32(t, E1K_RCTL), rd32(t, E1K_TCTL));
    wr32(t, E1K_IMC, 0xffffffffu);
    wr32(t, E1K_RCTL, 0);
    wr32(t, E1K_TCTL, 0);
    wr32(t, E1K_CTRL, rd32(t, E1K_CTRL) | CTRL_GIO_MDIS);
    if (!wait_for(t, E1K_STATUS, STATUS_GIO_MEN, 0, GIO_WAIT_NS))
        drv_log("the GIO master did not stop in 10 ms (status %#x): resetting anyway",
                rd32(t, E1K_STATUS));
    wr32(t, E1K_CTRL, rd32(t, E1K_CTRL) | CTRL_RST);
    (void)drv_sleep_until(drv_clock_ns() + NS_PER_MS);   /* em: msec_delay(1) after RST */
    if (!wait_for(t, E1K_CTRL, CTRL_RST, 0, RESET_WAIT_NS)) {
        drv_report("e1000e %04x:%04x: still in reset after 100 ms (ctrl %#x)", t->vid, t->did,
                   rd32(t, E1K_CTRL));
        return ERR_TIMED_OUT;
    }
    if (!wait_for(t, E1K_EECD, EECD_AUTO_RD, EECD_AUTO_RD, RESET_WAIT_NS))
        drv_log("the EEPROM's auto-read did not finish (eecd %#x)", rd32(t, E1K_EECD));
    wr32(t, E1K_IMC, 0xffffffffu);
    (void)rd32(t, E1K_ICR);   /* what the reset raised: nothing is waiting for it */
    wr32(t, E1K_ICR, 0xffffffffu);
    return OK;
}

/* One EEPROM word through EERD (10.2.2.4). */
static status_t eeprom_word(struct e1k *t, uint32_t addr, uint16_t *out)
{
    wr32(t, E1K_EERD, addr << EERD_ADDR_SHIFT | EERD_START);
    if (!wait_for(t, E1K_EERD, EERD_DONE, EERD_DONE, EEPROM_WAIT_NS))
        return ERR_TIMED_OUT;
    *out = (uint16_t)(rd32(t, E1K_EERD) >> EERD_DATA_SHIFT);
    return OK;
}

status_t chip_read_mac(struct e1k *t)
{
    uint16_t w[3];
    status_t st = OK;
    for (uint32_t i = 0; i < 3 && st == OK; i++)
        st = eeprom_word(t, i, &w[i]);
    uint32_t ral = rd32(t, E1K_RAL(0)), rah = rd32(t, E1K_RAH(0));
    const char *from = "the EEPROM";
    if (st != OK) {
        if (!(rah & RAH_AV)) {
            drv_report("e1000e: no address: the EEPROM doesn't answer and RAH0 isn't valid");
            return ERR_NOT_FOUND;
        }
        w[0] = (uint16_t)ral;
        w[1] = (uint16_t)(ral >> 16);
        w[2] = (uint16_t)rah;
        from = "RAL0/RAH0 (the EEPROM doesn't answer)";
    }
    for (unsigned i = 0; i < 3; i++) {
        t->mac[2 * i] = (uint8_t)w[i];
        t->mac[2 * i + 1] = (uint8_t)(w[i] >> 8);
    }
    drv_log("address %02x:%02x:%02x:%02x:%02x:%02x, from %s", t->mac[0], t->mac[1], t->mac[2],
            t->mac[3], t->mac[4], t->mac[5], from);
    return OK;
}

/* One PHY register through MDIC (10.2.2.7); false on a timeout or an error. */
static bool mdic(struct e1k *t, uint32_t op, unsigned reg, uint16_t *v)
{
    wr32(t, E1K_MDIC, op | (uint32_t)reg << MDIC_REG_SHIFT | E1K_PHY_ADDR << MDIC_PHY_SHIFT |
                      (op == MDIC_OP_WRITE ? *v : 0));
    uint64_t end = drv_clock_ns() + MDIC_WAIT_NS;
    uint32_t m;
    while (!((m = rd32(t, E1K_MDIC)) & MDIC_READY) && drv_clock_ns() < end)
        __builtin_ia32_pause();
    if (!(m & MDIC_READY) || (m & MDIC_ERROR)) {
        drv_log("PHY register %u: %s (mdic %#x)", reg, m & MDIC_ERROR ? "error" : "timed out", m);
        return false;
    }
    *v = (uint16_t)(m & MDIC_DATA_MASK);
    return true;
}

/* Autonegotiation without pause: neither symmetric nor asymmetric pause
 * advertised, so the link never resolves to flow control. */
static void phy_no_pause(struct e1k *t)
{
    uint16_t anar = 0, bmcr = 0;
    if (!mdic(t, MDIC_OP_READ, MII_ANAR, &anar) || !mdic(t, MDIC_OP_READ, MII_BMCR, &bmcr))
        return;
    uint16_t want = anar & (uint16_t)~(ANAR_PAUSE | ANAR_PAUSE_ASYM);
    if (want != anar)
        (void)mdic(t, MDIC_OP_WRITE, MII_ANAR, &want);
    bmcr |= BMCR_AUTOEN | BMCR_RESTART;
    (void)mdic(t, MDIC_OP_WRITE, MII_BMCR, &bmcr);
    drv_log("phy: anar %#x -> %#x (no pause), autonegotiation restarted", anar, want);
}

void chip_setup(struct e1k *t)
{
    /* Link: auto speed, set link up; no forced speed, no flow control,
     * no hardware VLAN (em_if_init / e1000_setup_link). */
    uint32_t ctrl = rd32(t, E1K_CTRL);
    ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPLX | CTRL_RFCE | CTRL_TFCE | CTRL_VME | CTRL_PHY_RST |
              CTRL_GIO_MDIS);
    wr32(t, E1K_CTRL, ctrl | CTRL_ASDE | CTRL_SLU);
    wr32(t, E1K_FCAL, 0);
    wr32(t, E1K_FCAH, 0);
    wr32(t, E1K_FCT, 0);
    wr32(t, E1K_FCTTV, 0);
    wr32(t, E1K_FCRTL, 0);
    wr32(t, E1K_FCRTH, 0);
    wr32(t, E1K_VET, NETFRAME_TPID_8021Q);
    /* Filters: our address only, broadcasts by RCTL.BAM; no multicast,
     * no VLAN filter (RCTL.VFE stays off, so the table is unused). */
    for (unsigned i = 0; i < E1K_MTA_WORDS; i++) {
        wr32(t, E1K_MTA + 4 * i, 0);
        wr32(t, E1K_VFTA + 4 * i, 0);
    }
    for (unsigned i = 1; i < E1K_RAR_COUNT; i++) {
        wr32(t, E1K_RAH(i), 0);
        wr32(t, E1K_RAL(i), 0);
    }
    wr32(t, E1K_RAL(0), (uint32_t)t->mac[0] | (uint32_t)t->mac[1] << 8 |
                        (uint32_t)t->mac[2] << 16 | (uint32_t)t->mac[3] << 24);
    wr32(t, E1K_RAH(0), (uint32_t)t->mac[4] | (uint32_t)t->mac[5] << 8 | RAH_AV);
    wr32(t, E1K_RXCSUM, 0);
    wr32(t, E1K_RFCTL, 0);
    /* No delays or throttling: each frame interrupts at once. */
    wr32(t, E1K_ITR, 0);
    wr32(t, E1K_RDTR, 0);
    wr32(t, E1K_RADV, 0);
    wr32(t, E1K_TIDV, 0);
    wr32(t, E1K_TADV, 0);
    /* Interrupts (em's 82574 MSI-X set-up): every cause on vector 0, no
     * auto-clear or auto-mask: the loop clears ICR itself. */
    wr32(t, E1K_IVAR, E1K_IVAR_VEC0);
    wr32(t, E1K_EIAC, 0);
    wr32(t, E1K_IAM, 0);
    wr32(t, E1K_CTRL_EXT, rd32(t, E1K_CTRL_EXT) | CTRL_EXT_PBA_CLR | CTRL_EXT_DRV_LOAD);
    phy_no_pause(t);
}

void chip_irq_enable(struct e1k *t, bool on)
{
    if (on) {
        wr32(t, E1K_IMS, E1K_IMS_ALL);
    } else {
        wr32(t, E1K_IMC, 0xffffffffu);
    }
}

bool chip_link_poll(struct e1k *t)
{
    uint32_t s = rd32(t, E1K_STATUS);
    if (s == 0xffffffffu)
        return false;   /* gone: nothing to believe */
    bool up = s & STATUS_LU, full = up && (s & STATUS_FD);
    static const uint32_t speeds[4] = { 10, 100, 1000, 1000 };
    uint32_t speed = up ? speeds[STATUS_SPEED(s)] : 0;
    if (up == t->link && speed == t->speed && full == t->full)
        return false;
    t->link = up;
    t->speed = speed;
    t->full = full;
    t->link_changes++;
    uint32_t n = t->link_lines++;
    if (n < LINK_LINES || n % 64 == 0)
        drv_log("link %s%s%s at %lu ms (change %lu)", up ? "up " : "down",
                up ? (speed == 1000 ? "1000" : speed == 100 ? "100" : "10") : "",
                up ? (full ? " full" : " half") : "",
                (unsigned long)((drv_clock_ns() - t->started) / NS_PER_MS),
                (unsigned long)t->link_changes);
    return true;
}

void chip_counters(struct e1k *t)
{
    struct chip_counts *c = &t->chip;
    c->rx_ok += rd32(t, E1K_GPRC);
    c->tx_ok += rd32(t, E1K_GPTC);
    c->rx_err += rd32(t, E1K_CRCERRS);
    c->rx_err += rd32(t, E1K_ALGNERRC);
    c->rx_err += rd32(t, E1K_RXERRC);
    c->missed += rd32(t, E1K_MPC);
    c->tx_err += rd32(t, E1K_ECOL);
    c->tx_err += rd32(t, E1K_LATECOL);
    c->pause_sent += rd32(t, E1K_XONTXC);
    c->pause_sent += rd32(t, E1K_XOFFTXC);
}

void chip_stop(struct e1k *t)
{
    chip_irq_enable(t, false);
    wr32(t, E1K_RCTL, 0);
    tx_disable(t);
    (void)drv_sleep_until(drv_clock_ns() + NS_PER_MS);   /* a frame in flight finishes */
    chip_counters(t);
    wr32(t, E1K_CTRL_EXT, rd32(t, E1K_CTRL_EXT) & ~CTRL_EXT_DRV_LOAD);
    if (chip_reset(t) != OK)
        drv_log("stop: the reset did not finish: bus mastering goes off anyway");
}
