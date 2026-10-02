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
 * always allowed). tx.c writes them itself, and only after
 * rtl_tx_allowed: the driver runs in full mode (not the listen-only
 * probe) and has a valid VLAN to tag every frame with. The mode is set
 * once at the start, from the driver's arguments (args.h).
 *
 * user/tests/utest/netframe.c tests both functions; tools/checknotx.sh
 * checks the sources: no file but tx.c can reach a transmit register, and
 * each of tx.c's entry points starts with the gate. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/netframe.h>

#define RTL_TXDESC_LO   0x20     /* the transmit ring's address, low 32 bits */
#define RTL_TXDESC_HI   0x24
#define RTL_CMD         0x37     /* 8: command */
#define RTL_CMD_TXENB   0x04     /* transmitter enable: tx.c alone sets it */
#define RTL_TXCFG       0x40     /* 32: transmit configuration; the chip's id in 30:26, 23:20 */
#define RTL_TDFNR       0x57     /* 8: transmit descriptor fetch number */
#define RTL_TXSTART     0x90     /* 16: the transmit doorbell */

/* What the driver was started to do (args.h decides, once). */
enum rtl_mode {
    RTL_MODE_OFF,     /* nothing: the chip is never touched */
    RTL_MODE_PROBE,   /* listen only (`netprobe`): no transmit code runs at all */
    RTL_MODE_FULL,    /* receive and transmit on the configured VLAN */
};

/* May regs.c write `val` (width 1, 2 or 4 bytes, little-endian) at
 * register `reg`? Never a transmit register, in any mode. */
static inline bool rtl_write_allowed(uint32_t reg, unsigned width, uint32_t val)
{
    if (width != 1 && width != 2 && width != 4)
        return false;
    for (unsigned i = 0; i < width; i++) {
        uint32_t b = reg + i;
        uint8_t v = (uint8_t)(val >> (8 * i));
        if (b >= RTL_TXDESC_LO && b < RTL_TXDESC_LO + 16)
            return false;
        if ((b >= RTL_TXCFG && b < RTL_TXCFG + 4) || (b >= RTL_TXSTART && b < RTL_TXSTART + 4))
            return false;
        if (b == RTL_TDFNR)
            return false;
        if (b == RTL_CMD && (v & RTL_CMD_TXENB))
            return false;
    }
    return true;
}

/* The gate at the top of every tx.c entry point: may the driver transmit
 * at all? Only in full mode, and only with a VLAN to tag frames with. */
static inline bool rtl_tx_allowed(enum rtl_mode mode, uint32_t vlan)
{
    return mode == RTL_MODE_FULL && netframe_vlan_ok(vlan);
}
