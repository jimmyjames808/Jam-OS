/* rtl8125: the transmit descriptor, as pure functions (drv/rtl8125: tx.c
 * uses them, utest tests them in user/tests/utest/netframe.c).
 *
 * The descriptor. rge's 8125 descriptor (struct rge_tx_desc in OpenBSD's
 * if_rgereg.h, ISC licence) is 32 bytes: the command and status word,
 * the extended status (checksum and tag insertion: always 0 here), the
 * buffer's 64-bit address, then 16 bytes rge leaves 0. rge_init sets bit
 * 0 of MAC OCP register 0xeb58, and with that bit set the 8125B reads its
 * transmit ring in 32-byte steps. With the bit clear it reads the older
 * 16-byte descriptor instead (the first three fields alone). The PC run of
 * 2026-10-02 (boot-0067) showed the mismatch the driver had then, bit set
 * and 16-byte descriptors: the chip sent the frames of descriptors 0 and
 * 2, never 1, and never handed 1 back. So the format is checked before
 * the transmitter goes on: the bit must say what RTL_TXD_SIZE says. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RTL_TXD_SIZE      32u          /* bytes per descriptor (rge's struct rge_tx_desc) */
#define RTL_TXD_CMDSTS    0u           /* command and status */
#define RTL_TXD_EXTSTS    4u           /* checksum and VLAN insertion: always 0 here */
#define RTL_TXD_ADDR      8u           /* the buffer's address, 64 bits */
#define RTL_TXD_RESERVED  16u          /* 16 bytes, always 0 (rge's reserved[4]) */
#define RTL_TXD_OWN      0x80000000u  /* the chip's until it has sent the frame */
#define RTL_TXD_EOR       0x40000000u  /* the last descriptor: the ring wraps after it */
#define RTL_TXD_SOF       0x20000000u
#define RTL_TXD_EOF       0x10000000u
#define RTL_TXD_ERR       0x00800000u
#define RTL_TXD_EXCESSCOLL 0x00100000u
#define RTL_TXD_COLL      0x000f0000u
#define RTL_TXD_LEN       0x0000ffffu
#define RTL_TXD_RING_ALIGN 256u        /* rge's RGE_ALIGN */

/* MAC OCP register 0xeb58, bit 0: set, the chip reads 32-byte transmit
 * descriptors (rge_init sets it); clear, 16-byte ones. */
#define RTL_MAC_TXD_FORMAT 0xeb58u
#define RTL_MAC_TXD_32     0x0001u

/* Does the format bit (MAC OCP 0xeb58 as read back) match RTL_TXD_SIZE? */
static inline bool rtl_txd_format_ok(uint16_t eb58)
{
    return (eb58 & RTL_MAC_TXD_32) == (RTL_TXD_SIZE == 32 ? RTL_MAC_TXD_32 : 0);
}

/* The command word handing descriptor i of n (len bytes, one buffer) to
 * the chip: ownership, start and end of frame, the ring's end on the last. */
static inline uint32_t rtl_txd_cmd(uint32_t i, uint32_t n, uint32_t len)
{
    return RTL_TXD_OWN | RTL_TXD_SOF | RTL_TXD_EOF | (i % n == n - 1 ? RTL_TXD_EOR : 0) |
           (len & RTL_TXD_LEN);
}
