/* rtl8125: who may make the chip transmit, as pure functions (drv/rtl8125).
 *
 * One file transmits: tx.c. Everything else in the driver writes the
 * chip's registers through regs.c's accessors, and those pass every write
 * to rtl_write_allowed first, which refuses the transmit registers in
 * every mode. The transmit side of the chip is reached only through these
 * registers (offsets from OpenBSD's if_rgereg.h, ISC licence):
 *
 *   0x20-0x27  the normal-priority transmit ring's address (TNPDS)
 *   0x28-0x2f  the high-priority transmit ring's address (older Realtek
 *              chips; kept forbidden on the 8125 too)
 *   0x37       the command register: bit 2 is the transmitter enable (TE)
 *   0x40-0x43  the transmit configuration (its id bits are read only)
 *   0x57       the transmit descriptor fetch number (TDFNR)
 *   0x90-0x93  the transmit doorbell (TXSTART on the 8125)
 *
 * A write that touches any of those bytes is refused, except a write of
 * the command register with TE clear (switching the transmitter off is
 * always allowed). Refused as well, though nothing in the driver writes
 * them, tx.c included (M9-REVIEW item 6 (d); offsets from Realtek's own
 * r8125 driver, GPL, used here as facts only, and Linux's r8169; the
 * PC's probe logs them, chip_txq_log, until a run confirms them):
 *
 *   0x2100-0x217f  the other transmit queues' ring addresses (queue q's at
 *                  0x2100 + 8 * (q - 1); the 8125 has two queues)
 *   0x2800-0x283f  every queue's tail pointer (the doorbell in the chip's
 *                  "no close" mode, TXCFG bit 6, never set) and its close
 *                  pointer, 16 bits each, 4 bytes a queue
 *   0x0d30-0x0d3f  the same pointers on the 8125BP (32 bits, 8 a queue)
 *
 * and, through the two OCP windows (a 32-bit write of BUSY, the
 * register's address over 2 in bits 30:16 and the value in 15:0; any
 * narrower write to a window is refused), the values that would change
 * how the transmitter reads or what the link sends (item 6 (c)):
 *
 *   MAC 0xeb58     with bit 0 other than the descriptor size tx.c writes
 *                  (txdesc.h): the chip would read the ring in other steps
 *   MAC 0xe63e     with bits 11:10 set: more than one transmit queue
 *   PHY 0xa408     the MII ANAR (rge_read_phy's 0xa400 + 2 * 4) with a
 *                  pause bit: Jam OS never advertises pause
 *   PHY 0xa412     the MII GTCR (0xa400 + 2 * 9) with a test mode (15:13)
 *
 * tx.c writes the transmit registers itself, and only after
 * rtl_tx_allowed: the driver runs in full mode (not the listen-only
 * probe) with a network mode: a valid VLAN to tag every frame with, or
 * untagged (no frame ever tagged: <jam/netframe.h>). The mode is set
 * once at the start, from the driver's arguments (args.h).
 *
 * user/tests/utest/netframe.c tests both functions; tools/checknotx.sh
 * checks the sources: no file but tx.c can reach a transmit register, and
 * each of tx.c's entry points starts with the gate. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/netframe.h>
#include "txdesc.h"

#define RTL_TXDESC_LO   0x20     /* the transmit ring's address, low 32 bits */
#define RTL_TXDESC_HI   0x24
#define RTL_TXHDESC_LO  0x28     /* the high-priority ring's (r8169's TxHDescStartAddr) */
#define RTL_TXHDESC_HI  0x2c
#define RTL_CMD         0x37     /* 8: command */
#define RTL_CMD_TXENB   0x04     /* transmitter enable: tx.c alone sets it */
#define RTL_TXCFG       0x40     /* 32: transmit configuration; the chip's id in 30:26, 23:20 */
#define RTL_TXCFG_NOCLOSE 0x40u  /* bit 6: the tail-pointer doorbells (Realtek's "no close") */
#define RTL_TDFNR       0x57     /* 8: transmit descriptor fetch number */
#define RTL_TXSTART     0x90     /* 16: the transmit doorbell, bit q for queue q */
#define RTL_TXQ_DESC    0x2100   /* queue 1's ring address (r8125's TNPDS_Q1_LOW_8125) */
#define RTL_TXQ_DESC_END 0x2180
#define RTL_TXQ_PTR     0x2800   /* queue 0's tail pointer (SW_TAIL_PTR0_8125), 16 bits; its
                                  * close pointer at + 2 (HW_CLO_PTR0_8125); 4 bytes a queue */
#define RTL_TXQ_PTR_END 0x2840
#define RTL_TXQ_PTR_BP  0x0d30   /* the 8125BP's (SW_TAIL_PTR0_8125BP), 32 bits, 8 a queue */
#define RTL_TXQ_PTR_BP_END 0x0d40

/* The OCP windows (rge_write_mac_ocp, rge_write_phy_ocp) and what is
 * refused through them. */
#define RTL_MACOCP      0xb0     /* 32: the window to the MAC's OCP registers */
#define RTL_PHYOCP      0xb8     /* 32: the window to the PHY's OCP registers */
#define RTL_OCP_BUSY        0x80000000u   /* a write (MAC, PHY); the PHY's "done" on a read */
#define RTL_OCP_ADDR_SHIFT  16
#define RTL_MAC_TXQ_CTRL 0xe63eu /* MAC OCP: bits 11:10, log2 of the transmit queues
                                  * (rge_init, r8125's rtl8125_set_tx_q_num) */
#define RTL_MAC_TXQ_MASK 0x0c00u
#define RTL_PHY_ANAR    0xa408u  /* PHY OCP: the MII ANAR */
#define RTL_PHY_ANAR_PAUSE 0x0c00u   /* symmetric and asymmetric pause */
#define RTL_PHY_GTCR    0xa412u  /* PHY OCP: the MII GTCR */
#define RTL_PHY_GTCR_TEST 0xe000u

/* The OCP register (and the value) a 32-bit write `val` to either window
 * writes: false if it writes none, only setting the read index. */
static inline bool rtl_ocp_write_of(uint32_t val, uint16_t *reg, uint16_t *v)
{
    if (!(val & RTL_OCP_BUSY))
        return false;
    *reg = (uint16_t)(((val >> RTL_OCP_ADDR_SHIFT) & 0x7fff) << 1);
    *v = (uint16_t)val;
    return true;
}

/* May the OCP write `val` through window `win` (RTL_MACOCP or RTL_PHYOCP) go? */
static inline bool rtl_ocp_allowed(uint32_t win, uint32_t val)
{
    uint16_t reg, v;
    if (!rtl_ocp_write_of(val, &reg, &v))
        return true;   /* the read index */
    if (win == RTL_MACOCP)
        return !(reg == RTL_MAC_TXD_FORMAT && !rtl_txd_format_ok(v)) &&
               !(reg == RTL_MAC_TXQ_CTRL && (v & RTL_MAC_TXQ_MASK));
    return !(reg == RTL_PHY_ANAR && (v & RTL_PHY_ANAR_PAUSE)) &&
           !(reg == RTL_PHY_GTCR && (v & RTL_PHY_GTCR_TEST));
}

/* Is byte b one of the other queues' registers (above)? */
static inline bool rtl_other_txq(uint32_t b)
{
    return (b >= RTL_TXQ_DESC && b < RTL_TXQ_DESC_END) ||
           (b >= RTL_TXQ_PTR && b < RTL_TXQ_PTR_END) ||
           (b >= RTL_TXQ_PTR_BP && b < RTL_TXQ_PTR_BP_END);
}

/* What the driver was started to do (args.h decides, once). */
enum rtl_mode {
    RTL_MODE_OFF,     /* nothing: the chip is never touched */
    RTL_MODE_PROBE,   /* listen only (`netprobe`): no transmit code runs at all */
    RTL_MODE_FULL,    /* receive and transmit in the configured network mode */
};

/* May regs.c write `val` (width 1, 2 or 4 bytes, little-endian) at
 * register `reg`? Never a transmit register, in any mode. */
static inline bool rtl_write_allowed(uint32_t reg, unsigned width, uint32_t val)
{
    if (width != 1 && width != 2 && width != 4)
        return false;
    if (reg < RTL_MACOCP + 4 && reg + width > RTL_MACOCP)   /* the windows: whole writes only */
        return reg == RTL_MACOCP && width == 4 && rtl_ocp_allowed(RTL_MACOCP, val);
    if (reg < RTL_PHYOCP + 4 && reg + width > RTL_PHYOCP)
        return reg == RTL_PHYOCP && width == 4 && rtl_ocp_allowed(RTL_PHYOCP, val);
    for (unsigned i = 0; i < width; i++) {
        uint32_t b = reg + i;
        uint8_t v = (uint8_t)(val >> (8 * i));
        if (b >= RTL_TXDESC_LO && b < RTL_TXDESC_LO + 16)
            return false;
        if ((b >= RTL_TXCFG && b < RTL_TXCFG + 4) || (b >= RTL_TXSTART && b < RTL_TXSTART + 4))
            return false;
        if (b == RTL_TDFNR || rtl_other_txq(b))
            return false;
        if (b == RTL_CMD && (v & RTL_CMD_TXENB))
            return false;
    }
    return true;
}

/* The gate at the top of every tx.c entry point: may the driver transmit
 * at all? Only in full mode, and only with a configured network mode: a
 * VLAN to tag frames with, or untagged (`vlan` is the mode). */
static inline bool rtl_tx_allowed(enum rtl_mode mode, uint32_t vlan)
{
    return mode == RTL_MODE_FULL && netframe_mode_ok(vlan);
}
