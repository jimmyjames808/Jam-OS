/* rtl8125: failing closed when the chip sends what the driver didn't
 * queue, and the tally dump that tells, as pure functions (drv/rtl8125:
 * guard.c and regs.c use them; utest tests them in
 * user/tests/utest/rtlguard.c). docs/history/M9-REVIEW.md, design
 * question A (the owner's yes, 2026-10-02).
 *
 * The chip keeps its own count of the frames it has sent (its tally
 * counters, dumped by DMA: rge's struct rge_stats). Every frame tx.c
 * queues is counted by tx.c before its doorbell, and the chip counts a
 * frame only once it has sent it, so in a chip that sends nothing of its
 * own
 *
 *     the chip's count since the driver started <= frames queued
 *
 * at every moment, frames in flight included (queued, not yet sent:
 * the right side is ahead). A dump is the chip's count at some moment
 * between the driver asking for it and seeing it in memory, and the
 * queued count only grows, so the dump is compared with the queued count
 * read when the dump is seen: a frame queued after the chip's snapshot
 * only adds to the right side. Anything more on the left is a frame the
 * driver never queued (a PAUSE frame, a management or wake-up frame of
 * the chip's own firmware, a frame from a ring nobody armed), and the
 * driver stops the chip at once (guard.c).
 *
 * So does a link that resolved to sending PAUSE frames: PHYSTAT's
 * transmit flow control bit with the link up (Jam OS never advertises
 * pause, so only a PHY or firmware doing what it wasn't asked shows it).
 * A tally that went backwards can't be read as a count, and a tally that
 * can't be read at all for RTL_GUARD_UNREAD_MAX looks in a row means
 * nothing can be vouched for: both stop the chip too.
 *
 * Bits and offsets: OpenBSD's if_rgereg.h (RGE_PHYSTAT_*, RGE_DTCCR_*),
 * ISC licence. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RTL_PHYSTAT_LINK    0x0002u   /* rge's RGE_PHYSTAT_LINK */
#define RTL_PHYSTAT_RXFLOW  0x0020u   /* pause frames honoured (flow control resolved on) */
#define RTL_PHYSTAT_TXFLOW  0x0040u   /* pause frames SENT: the guard stops the chip */

/* Looks in a row without a usable tally before the guard gives up on it
 * (a look is a dump asked for: about one a second). */
#define RTL_GUARD_UNREAD_MAX 10u

enum rtl_guard_verdict {
    RTL_GUARD_GO,          /* nothing the driver didn't queue: carry on */
    RTL_GUARD_FOREIGN,     /* the chip sent more frames than tx.c queued */
    RTL_GUARD_PAUSE_TX,    /* the link resolved to sending PAUSE frames */
    RTL_GUARD_BACKWARDS,   /* the chip's count went below the start's */
    RTL_GUARD_UNREAD,      /* RTL_GUARD_UNREAD_MAX looks in a row without a count */
};

/* What one look has. */
struct rtl_guard_look {
    uint16_t phystat;      /* PHYSTAT as last read */
    bool     tallied;      /* a dump landed since the last look, and the start's was read */
    uint64_t base;         /* the chip's count of frames sent (ok + error) at the start */
    uint64_t count;        /* ... in the dump that landed */
    uint64_t queued;       /* frames tx.c had handed to the chip when that dump was seen */
    uint32_t unread;       /* looks in a row without a usable count */
};

static inline enum rtl_guard_verdict rtl_guard(const struct rtl_guard_look *l)
{
    if (l->phystat != 0xffff && (l->phystat & RTL_PHYSTAT_LINK) &&
        (l->phystat & RTL_PHYSTAT_TXFLOW))
        return RTL_GUARD_PAUSE_TX;
    if (!l->tallied)
        return l->unread >= RTL_GUARD_UNREAD_MAX ? RTL_GUARD_UNREAD : RTL_GUARD_GO;
    if (l->count < l->base)
        return RTL_GUARD_BACKWARDS;
    return l->count - l->base > l->queued ? RTL_GUARD_FOREIGN : RTL_GUARD_GO;
}

/* The log's words for a verdict. */
static inline const char *rtl_guard_why(enum rtl_guard_verdict v)
{
    switch (v) {
    case RTL_GUARD_GO:        return "nothing of its own";
    case RTL_GUARD_FOREIGN:   return "IT SENT FRAMES THE DRIVER DID NOT QUEUE";
    case RTL_GUARD_PAUSE_TX:  return "THE LINK SENDS PAUSE FRAMES";
    case RTL_GUARD_BACKWARDS: return "ITS COUNT OF FRAMES SENT WENT BACKWARDS";
    case RTL_GUARD_UNREAD:    return "ITS COUNT OF FRAMES SENT CAN'T BE READ";
    }
    return "?";
}

/* ---- the tally dump, asked for without waiting ------------------------------------ */

/* The chip writes its counters into memory when asked (DTCCR's dump bit),
 * and clears the bit when they are there. The driver's loop asks, goes on
 * with its work, and looks at the bit again later: nothing waits for the
 * DMA inside the loop (M9-REVIEW finding 13). A dump not there after
 * RTL_DUMP_WAIT_NS is given up on (counted). */
#define RTL_DUMP_WAIT_NS   10000000ull     /* 10 ms: rge's 1000 x 10 us */
#define RTL_DUMP_EVERY_NS  1000000000ull   /* the guard's look: once a second at least */
#define RTL_DUMP_GAP_NS    1000000ull      /* ... and after a reap or a request, 1 ms apart */

struct rtl_dump {
    bool     busy;         /* asked for, not seen yet */
    bool     asked_once;
    uint64_t asked_at;     /* the last ask (uptime, ns) */
    uint64_t landed;       /* dumps seen in memory */
    uint64_t landed_at;    /* the last one (uptime, ns) */
    uint32_t late;         /* dumps given up on */
    uint32_t late_in_row;  /* ... since the last one that landed */
};

enum rtl_dump_step {
    RTL_DUMP_NONE,         /* none asked for */
    RTL_DUMP_WAITING,      /* asked for, not there yet */
    RTL_DUMP_LANDED,       /* in memory now */
    RTL_DUMP_LATE,         /* not there after RTL_DUMP_WAIT_NS: given up */
};

/* Should a dump be asked for at `now`? Never while one is out; else once
 * RTL_DUMP_EVERY_NS have passed since the last ask, or, when `soon` (a
 * reap freed descriptors, a stats request), RTL_DUMP_GAP_NS. */
static inline bool rtl_dump_due(const struct rtl_dump *d, uint64_t now, bool soon)
{
    if (d->busy)
        return false;
    if (!d->asked_once)
        return true;
    uint64_t since = now - d->asked_at;
    return since >= RTL_DUMP_EVERY_NS || (soon && since >= RTL_DUMP_GAP_NS);
}

static inline void rtl_dump_asked(struct rtl_dump *d, uint64_t now)
{
    d->busy = true;
    d->asked_once = true;
    d->asked_at = now;
}

/* A look at the dump bit (`bit`: DTCCR's dump bit as read at `now`). */
static inline enum rtl_dump_step rtl_dump_look(struct rtl_dump *d, bool bit, uint64_t now)
{
    if (!d->busy)
        return RTL_DUMP_NONE;
    if (!bit) {
        d->busy = false;
        d->landed++;
        d->landed_at = now;
        d->late_in_row = 0;
        return RTL_DUMP_LANDED;
    }
    if (now - d->asked_at < RTL_DUMP_WAIT_NS)
        return RTL_DUMP_WAITING;
    d->busy = false;
    d->late++;
    d->late_in_row++;
    return RTL_DUMP_LATE;
}
