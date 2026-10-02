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
 * has written a frame, with its length (CRC included) and status. */
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

/* Hand descriptor d (number i of n) to the chip: the extended status
 * cleared, then (after a release fence: the rest before the ownership
 * bit) the command word. */
static inline void rtl_rxd_arm(volatile uint8_t *d, uint32_t i, uint32_t n, uint32_t bufsz)
{
    *(volatile uint32_t *)(d + RX_DESC_EXTSTS) = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *(volatile uint32_t *)(d + RX_DESC_CMDSTS) = rtl_rxd_cmd(i, n, bufsz);
}
