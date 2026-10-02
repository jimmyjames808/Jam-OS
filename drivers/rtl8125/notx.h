/* rtl8125: the listen-only rule as one pure function (drv/rtl8125).
 *
 * The probe must never be able to send: it has no transmit code, and on
 * top of that every register write it makes goes through
 * rtl_write_allowed (regs.c's accessors call it and drop a refused write).
 * The transmit side of the chip is reached only through these registers
 * (offsets from OpenBSD's if_rgereg.h, ISC licence):
 *
 *   0x20-0x27  the normal-priority transmit ring's address (TNPDS)
 *   0x28-0x2f  the high-priority transmit ring's address (older Realtek
 *              chips; kept forbidden on the 8125 too)
 *   0x37       the command register: bit 2 is the transmitter enable (TE)
 *   0x40-0x43  the transmit configuration (read only here: its id bits)
 *   0x90-0x93  the transmit doorbell (TXSTART on the 8125)
 *
 * A write that touches any of those bytes is refused, except a write of
 * the command register with TE clear. Without a transmit ring address and
 * TE, the chip has nothing to send and no transmitter to send it with.
 * user/tests/utest/netframe.c tests this function; tools/checknotx.sh
 * checks the sources so no write can bypass it. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RTL_TXDESC_LO   0x20     /* the transmit ring's address, low 32 bits */
#define RTL_TXDESC_HI   0x24
#define RTL_CMD         0x37     /* 8: command */
#define RTL_CMD_TXENB   0x04     /* transmitter enable: never set by the probe */
#define RTL_TXCFG       0x40     /* 32: transmit configuration; the chip's id in 30:26, 23:20 */
#define RTL_TXSTART     0x90     /* 16: the transmit doorbell */

/* May the probe write `val` (width 1, 2 or 4 bytes, little-endian) at
 * register `reg`? */
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
        if (b == RTL_CMD && (v & RTL_CMD_TXENB))
            return false;
    }
    return true;
}
