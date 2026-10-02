/* rtl8125: register access (drv/rtl8125). Every register write in the
 * driver goes through here, and every write here passes rtl_write_allowed
 * (notx.h) first, so no transmit register can be written through it, in
 * any mode. The one other file that writes registers is tx.c, and only
 * the transmit ones, behind its own gate (tools/checknotx.sh checks that
 * no other file calls drv_write*). Also the tally counters: their dump,
 * and the comparison of the chip's count of frames sent with tx.c's.
 *
 * The 8125 hides most of its MAC and all of its PHY behind two 32-bit
 * windows (rge_write_mac_ocp, rge_read_phy_ocp in if_rge.c): the
 * register's address over 2 in bits 31:16 (with bit 31 as "write" or
 * "busy"), the 16-bit value in 15:0. The MII registers are in the PHY's
 * OCP space at 0xa400 + 2 * reg (rge_read_phy with address 0). Every wait
 * here has a deadline in time and is counted when it runs out. */
#include "rtl8125.h"

_Static_assert(sizeof(struct tally) == 64, "the chip dumps 64 bytes");

#define OCP_WAIT_NS   (20 * NS_PER_MS)   /* rge: 20000 x 1 us */
#define TALLY_WAIT_NS (10 * NS_PER_MS)   /* rge: 1000 x 10 us */

uint8_t rd8(const struct rtl *t, uint32_t reg)
{
    return drv_read8(t->r, reg);
}

uint16_t rd16(const struct rtl *t, uint32_t reg)
{
    return drv_read16(t->r, reg);
}

uint32_t rd32(const struct rtl *t, uint32_t reg)
{
    return drv_read32(t->r, reg);
}

/* A write the guard refused: never reaches the chip. */
static bool refused(struct rtl *t, uint32_t reg, unsigned width, uint32_t v)
{
    if (rtl_write_allowed(reg, width, v))
        return false;
    if (t->refused++ < 8)
        drv_log("REFUSED a %u-byte write of %#x at register %#x: a transmit register", width, v,
                reg);
    return true;
}

void wr8(struct rtl *t, uint32_t reg, uint8_t v)
{
    if (!refused(t, reg, 1, v))
        drv_write8(t->r, reg, v);
}

void wr16(struct rtl *t, uint32_t reg, uint16_t v)
{
    if (!refused(t, reg, 2, v))
        drv_write16(t->r, reg, v);
}

void wr32(struct rtl *t, uint32_t reg, uint32_t v)
{
    if (!refused(t, reg, 4, v))
        drv_write32(t->r, reg, v);
}

void set8(struct rtl *t, uint32_t reg, uint8_t bits)
{
    wr8(t, reg, (uint8_t)(rd8(t, reg) | bits));
}

void clr8(struct rtl *t, uint32_t reg, uint8_t bits)
{
    wr8(t, reg, (uint8_t)(rd8(t, reg) & ~bits));
}

void delay_us(uint64_t us)
{
    uint64_t end = drv_clock_ns() + us * NS_PER_US;
    if (us >= 1000) {
        (void)drv_sleep_until(end);   /* a sleep cut short only returns early */
        return;
    }
    while (drv_clock_ns() < end)
        __builtin_ia32_pause();
}

/* Spin until (reg & mask) == want, at most OCP_WAIT_NS; false (counted) if not. */
static bool spin_for(struct rtl *t, uint32_t reg, uint32_t mask, uint32_t want)
{
    uint64_t end = drv_clock_ns() + OCP_WAIT_NS;
    for (;;) {
        if ((rd32(t, reg) & mask) == want)
            return true;
        if (drv_clock_ns() > end) {
            t->ocp_timeouts++;
            return false;
        }
        __builtin_ia32_pause();
    }
}

/* The MAC's window has no busy bit to wait on (rge doesn't wait either). */
uint16_t mac_rd(struct rtl *t, uint16_t reg)
{
    wr32(t, RTL_MACOCP, (uint32_t)(reg >> 1) << RTL_OCP_ADDR_SHIFT);
    return (uint16_t)rd32(t, RTL_MACOCP);
}

void mac_wr(struct rtl *t, uint16_t reg, uint16_t v)
{
    wr32(t, RTL_MACOCP, RTL_OCP_BUSY | (uint32_t)(reg >> 1) << RTL_OCP_ADDR_SHIFT | v);
}

void mac_mod(struct rtl *t, uint16_t reg, uint16_t clear, uint16_t set)
{
    mac_wr(t, reg, (uint16_t)((mac_rd(t, reg) & ~clear) | set));
}

/* The PHY's window: a read sets the busy bit when the value is there, a
 * write clears it when done (rge_read_phy_ocp, rge_write_phy_ocp). */
uint16_t phy_rd(struct rtl *t, uint16_t reg)
{
    wr32(t, RTL_PHYOCP, (uint32_t)(reg >> 1) << RTL_OCP_ADDR_SHIFT);
    (void)spin_for(t, RTL_PHYOCP, RTL_OCP_BUSY, RTL_OCP_BUSY);   /* counted if late */
    return (uint16_t)rd32(t, RTL_PHYOCP);
}

void phy_wr(struct rtl *t, uint16_t reg, uint16_t v)
{
    wr32(t, RTL_PHYOCP, RTL_OCP_BUSY | (uint32_t)(reg >> 1) << RTL_OCP_ADDR_SHIFT | v);
    (void)spin_for(t, RTL_PHYOCP, RTL_OCP_BUSY, 0);
}

void phy_mod(struct rtl *t, uint16_t reg, uint16_t clear, uint16_t set)
{
    phy_wr(t, reg, (uint16_t)((phy_rd(t, reg) & ~clear) | set));
}

/* MII register reg (0..7 here) of the built-in PHY: rge_read_phy(sc, 0, reg). */
static uint16_t mii_ocp(unsigned reg)
{
    return (uint16_t)((0xa40 + reg / 8) << 4 | (reg % 8) << 1);
}

uint16_t mii_rd(struct rtl *t, unsigned reg)
{
    return phy_rd(t, mii_ocp(reg));
}

void mii_wr(struct rtl *t, unsigned reg, uint16_t v)
{
    phy_wr(t, mii_ocp(reg), v);
}

/* rge_kstat_read: the address (64-byte aligned) high half first, then the
 * low half, then the low half with the dump bit; the chip clears the bit
 * when the counters are in memory. */
status_t tally_dump(struct rtl *t, struct tally *out)
{
    uint64_t addr = t->ring_addr + TALLY_OFF;
    wr32(t, RTL_DTCCR_HI, (uint32_t)(addr >> 32));
    (void)rd8(t, RTL_CMD);   /* rge reads back between the halves */
    wr32(t, RTL_DTCCR_LO, (uint32_t)addr);
    wr32(t, RTL_DTCCR_LO, (uint32_t)addr | RTL_DTCCR_CMD);
    uint64_t end = drv_clock_ns() + TALLY_WAIT_NS;
    while (rd32(t, RTL_DTCCR_LO) & RTL_DTCCR_CMD) {
        if (drv_clock_ns() > end)
            return ERR_TIMED_OUT;
        delay_us(10);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const volatile uint8_t *p = t->ring + TALLY_OFF;
    struct tally x;
    for (unsigned i = 0; i < sizeof(x); i++)
        ((uint8_t *)&x)[i] = p[i];
    *out = x;
    return OK;
}

void tally_log(const char *when, const struct tally *x)
{
    drv_log("tally %s: tx ok %lu err %lu abort %u underrun %u collisions %u/%u; rx ok %lu "
            "(unicast %lu, broadcast %lu, multicast %u) err %u missed %u align %u", when,
            (unsigned long)x->tx_ok, (unsigned long)x->tx_err, x->tx_abort, x->tx_underrun,
            x->tx_1col, x->tx_mcol, (unsigned long)x->rx_ok, (unsigned long)x->rx_ok_phy,
            (unsigned long)x->rx_ok_brd, x->rx_ok_mul, x->rx_err, x->miss, x->fae);
}

bool tally_tx_check(const struct rtl *t, const struct outcome *o, char *out, size_t size)
{
    if (!o->start_ok || !o->end_ok) {
        drv_log("tx check: the tally could not be read (%s), so it can't be compared",
                o->start_ok ? "at the end" : "at the start");
        drv_snprintf(out, size, "chip tally UNREAD");
        return false;
    }
    uint64_t sent = o->end.tx_ok - o->start.tx_ok, err = o->end.tx_err - o->start.tx_err;
    bool same = sent == t->tx.done && err == t->tx.errors &&
                t->tx.queued == t->tx.done + t->tx.errors;
    drv_log("tx check: the driver queued %u frame(s), %u came back sent and %u with an error; "
            "the chip's tally: %lu sent, %lu errors: %s", t->tx.queued, t->tx.done,
            t->tx.errors, (unsigned long)sent, (unsigned long)err,
            same ? "equal: the chip sent nothing of its own"
            : sent > t->tx.done ? "THE CHIP SENT FRAMES THE DRIVER DID NOT QUEUE"
            : "they differ");
    drv_snprintf(out, size, "chip tally +%lu%s", (unsigned long)sent, same ? " (equal)"
                 : " (DIFFERS)");
    return same;
}
