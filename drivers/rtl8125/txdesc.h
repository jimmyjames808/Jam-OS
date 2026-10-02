/* rtl8125: the transmit descriptor and the transmit path's bookkeeping,
 * as pure functions (drv/rtl8125: tx.c uses them, utest tests them in
 * user/tests/utest/rtltx.c).
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
 * the transmitter goes on: the bit must say what RTL_TXD_SIZE says.
 *
 * The bookkeeping: when to ring the doorbell again for a descriptor the
 * chip still owns (rtl_kick_due), and what the chip's own count of frames
 * sent says next to the driver's (rtl_tx_verdict). */
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

/* ---- the doorbell again, for a descriptor the chip still owns ----------------- */

/* rge_txeof rings the doorbell again whenever it stops at a descriptor the
 * chip still owns (some chips ignore a doorbell rung while they send).
 * Done on every interrupt, that turned into a loop on the PC (69912 in
 * 4 s): each doorbell with nothing the chip could take raised "transmit
 * descriptor unavailable", whose interrupt rang it again. Here a stuck
 * descriptor gets its first extra doorbell RTL_KICK_FIRST_NS after it was
 * queued, then the gap grows fourfold up to RTL_KICK_MAX_NS: at most one
 * a second however often the driver looks. */
#define RTL_KICK_FIRST_NS 1000000ull       /* 1 ms */
#define RTL_KICK_MAX_NS   1000000000ull    /* 1 s */

struct rtl_kick {
    uint32_t cons;        /* the descriptor being waited for (free-running index) */
    bool     armed;       /* cons and at are valid */
    uint64_t gap;         /* the gap after the next doorbell */
    uint64_t at;          /* when the next doorbell is due (uptime, ns) */
};

/* The driver is waiting for descriptor `cons`, queued at `queued_at`:
 * should it ring the doorbell again at `now`? Updates k. */
static inline bool rtl_kick_due(struct rtl_kick *k, uint32_t cons, uint64_t queued_at,
                                uint64_t now)
{
    if (!k->armed || k->cons != cons)   /* a new descriptor to wait for */
        *k = (struct rtl_kick){ .cons = cons, .armed = true, .gap = RTL_KICK_FIRST_NS * 4,
                                .at = queued_at + RTL_KICK_FIRST_NS };
    if (now < k->at)
        return false;
    k->at = now + k->gap;
    k->gap = k->gap * 4 > RTL_KICK_MAX_NS ? RTL_KICK_MAX_NS : k->gap * 4;
    return true;
}

/* ---- the chip's count against the driver's ------------------------------------- */

/* Frames the driver queued, the chip's tally of frames it sent (good and
 * errored) over the same time, and the descriptors it handed back. In
 * normal progress back <= chip <= queued: a frame is counted when the chip
 * has sent it, and handed back after. Fewer sent than queued means frames
 * are still in the ring (or were never fetched); more sent than queued
 * means the chip sent frames of its own (PAUSE, wake-on-LAN or management
 * frames: the plan's check); more handed back than sent can't happen. */
enum rtl_tx_verdict {
    RTL_TX_EQUAL,          /* chip == queued (and back == chip) */
    RTL_TX_FEWER_SENT,     /* chip < queued: frames still out, or never fetched */
    RTL_TX_MORE_SENT,      /* chip > queued: THE CHIP SENT FRAMES THE DRIVER DID NOT QUEUE */
    RTL_TX_UNSENT_BACK,    /* back > chip: descriptors handed back the tally didn't count */
    RTL_TX_NOT_BACK,       /* chip == queued but back < chip: sent, not all handed back */
};

static inline enum rtl_tx_verdict rtl_tx_verdict(uint64_t queued, uint64_t chip, uint64_t back)
{
    if (chip > queued)
        return RTL_TX_MORE_SENT;
    if (back > chip)
        return RTL_TX_UNSENT_BACK;
    if (chip < queued)
        return RTL_TX_FEWER_SENT;
    return back == chip ? RTL_TX_EQUAL : RTL_TX_NOT_BACK;
}

/* The RESULTS line's word for a verdict. */
static inline const char *rtl_tx_verdict_word(enum rtl_tx_verdict v)
{
    switch (v) {
    case RTL_TX_EQUAL:       return "equal";
    case RTL_TX_FEWER_SENT:  return "FEWER SENT";
    case RTL_TX_MORE_SENT:   return "MORE SENT";
    case RTL_TX_UNSENT_BACK: return "MORE BACK THAN SENT";
    case RTL_TX_NOT_BACK:    return "NOT ALL BACK";
    }
    return "?";
}
