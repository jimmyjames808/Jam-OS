/* rtl8125: the receive descriptor, as pure functions (drv/rtl8125: ring.c
 * uses them, utest tests them in user/tests/utest/rtlrx.c).
 *
 * rge's 8125 receive descriptor (struct rge_rx_desc in OpenBSD's
 * if_rgereg.h, ISC licence) is 32 bytes, the chip's "version 3" format
 * (RXCFG bit 24, set in rge's RXCFG 0x41000c00): 16 bytes the chip writes
 * (RSS and header information, unused here), the buffer's 64-bit address
 * at 16, the extended status at 24 (the tag, unused: tag stripping is
 * off), the command and status word at 28. The driver hands a descriptor
 * to the chip with the buffer's size and the ownership bit (and the
 * ring's end on the last one); the chip clears the ownership bit when it
 * has written a frame, with its length (CRC included) and status.
 *
 * The chip's write-back is not only the status: in this format the
 * address field shares its 8 bytes with the frame's timestamp (Realtek's
 * own driver's struct RxDescV3: `addr` in a union with TimeStampLow and
 * TimeStampHigh), and the first 16 bytes take the RSS and header
 * information. So a descriptor coming back may no longer hold its
 * buffer's address, and every hand-back writes the whole descriptor
 * again, the address included, as rge_newbuf does for every descriptor
 * it gives the chip. The driver used to write the address once, at the
 * start: the PC (boot-0075, 2026-10-02) received until about the 256th
 * frame, the end of the ring's first lap, and nothing after. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RX_DESC_SIZE    32u
#define RX_DESC_ADDR    16u
#define RX_DESC_EXTSTS  24u
#define RX_DESC_CMDSTS  28u
#define RX_OWN          0x80000000u   /* the chip's until it has filled it */
#define RX_EOR          0x40000000u   /* the last descriptor: the ring wraps after it */
#define RX_SOF          0x02000000u
#define RX_EOF          0x01000000u
#define RX_ERRSUM       0x00100000u
#define RX_LEN          0x00003fffu   /* bytes received, the CRC included */
#define RX_CRC          4u
#define RTL_RXCFG_DESC_V3 0x01000000u /* RXCFG bit 24: the 32-byte descriptor above */

/* The command word handing descriptor i of n (a buffer of bufsz bytes)
 * to the chip: ownership and the buffer's size, the ring's end on the
 * last. */
static inline uint32_t rtl_rxd_cmd(uint32_t i, uint32_t n, uint32_t bufsz)
{
    return RX_OWN | (bufsz & RX_LEN) | (i % n == n - 1 ? RX_EOR : 0);
}

/* Hand descriptor d (number i of n, its buffer of bufsz bytes at device
 * address addr) to the chip: the buffer's address and the extended status
 * written, then (after a release fence: the rest before the ownership
 * bit) the command word. */
static inline void rtl_rxd_arm(volatile uint8_t *d, uint64_t addr, uint32_t i, uint32_t n,
                               uint32_t bufsz)
{
    *(volatile uint64_t *)(d + RX_DESC_ADDR) = addr;
    *(volatile uint32_t *)(d + RX_DESC_EXTSTS) = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *(volatile uint32_t *)(d + RX_DESC_CMDSTS) = rtl_rxd_cmd(i, n, bufsz);
}

/* The address field of a descriptor the chip handed back (what it may
 * have written there: the driver counts it, never uses it). */
static inline uint64_t rtl_rxd_addr(const volatile uint8_t *d)
{
    return *(const volatile uint64_t *)(d + RX_DESC_ADDR);
}
