/* utest: the RTL8125 driver failing closed (drivers/rtl8125/guard.h, the
 * pure decision and the tally dump's bookkeeping; guard.c, regs.c,
 * chip.c and tx.c linked in and run over a fake chip: its registers and
 * its DMA memory are this file's arrays, and the test plays the chip). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "rtl8125.h"
#include "utest.h"

#define BASE 112ull   /* the chip's count at the start: it survives reboots (M9-PLAN) */
#define MS   1000000ull

/* A small generator for the runs below (any sequence will do). */
static uint32_t next_rand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* The decision: given the tally, the queued count and PHYSTAT, carry on
 * or stop. Frames in flight (queued, not yet sent) never stop it; one
 * frame more than queued does, as does PAUSE TX with the link up. */
bool t_rtl8125_guard(void)
{
    const uint16_t up = RTL_PHYSTAT_LINK | 0x0010 | 0x0001;   /* 1000 full */
    struct rtl_guard_look l = { .phystat = up, .tallied = true, .base = BASE };
    l.count = BASE, l.queued = 0;
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_GO);                 /* the probe: nothing sent */
    l.count = BASE + 1;
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_FOREIGN);            /* one frame of its own */
    l.count = BASE + 3, l.queued = 5;
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_GO);                 /* two still in flight */
    l.count = BASE + 5;
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_GO);                 /* all sent */
    l.count = BASE + 6;
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_FOREIGN);
    l.count = BASE - 1;
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_BACKWARDS);
    /* the 64-bit counts: past 2^32 frames nothing wraps */
    l.base = 0xfffffff0ull, l.queued = 0x100000020ull, l.count = l.base + l.queued;
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_GO);
    l.count++;
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_FOREIGN);
    /* PAUSE TX: with the link up only, and before anything else */
    l = (struct rtl_guard_look){ .phystat = up | RTL_PHYSTAT_TXFLOW };
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_PAUSE_TX);
    l.phystat = RTL_PHYSTAT_TXFLOW;                        /* link down: a stale bit */
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_GO);
    l.phystat = up | RTL_PHYSTAT_RXFLOW;                   /* honouring pause sends nothing */
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_GO);
    l.phystat = 0xffff;                                    /* the chip gone: not a PAUSE bit */
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_GO);
    /* no count: a few looks are forgiven, RTL_GUARD_UNREAD_MAX are not */
    l = (struct rtl_guard_look){ .phystat = up, .unread = RTL_GUARD_UNREAD_MAX - 1 };
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_GO);
    l.unread++;
    CHECK_EQ(rtl_guard(&l), RTL_GUARD_UNREAD);
    for (unsigned v = RTL_GUARD_GO; v <= RTL_GUARD_UNREAD; v++)
        CHECK(rtl_guard_why(v)[0] != '?');

    /* A healthy chip over a long run: the driver queues frames, the chip
     * sends each some time after it was queued, a dump is the chip's count
     * at a moment between the ask and the look that sees it. Compared with
     * the queued count at that look, nothing trips; compared with the
     * count at the ask (the wrong moment), frames queued and sent in
     * between would. */
    uint32_t s = 7;
    uint64_t queued = 0, sent = 0, at_ask = 0, snap = 0;
    unsigned wrong = 0;
    bool out = false, snapped = false;
    for (unsigned step = 0; step < 100000; step++) {
        queued += next_rand(&s) % 4;                       /* tx_send: counted, then the doorbell */
        sent += next_rand(&s) % (queued - sent + 1);       /* the chip, behind */
        if (!out) {
            out = true, snapped = false, at_ask = queued;
            continue;
        }
        if (!snapped && next_rand(&s) % 3 == 0)
            snapped = true, snap = sent;                   /* the chip's DMA */
        if (!snapped || next_rand(&s) % 2)
            continue;                                      /* not seen yet */
        out = false;
        struct rtl_guard_look k = { .phystat = up, .tallied = true, .base = BASE,
                                    .count = BASE + snap, .queued = queued };
        if (rtl_guard(&k) != RTL_GUARD_GO)
            FAIL("step %u: a healthy chip stopped (%lu sent, %lu queued)", step,
                 (unsigned long)snap, (unsigned long)queued);
        k.queued = at_ask;
        wrong += rtl_guard(&k) != RTL_GUARD_GO;
    }
    if (!wrong)
        FAIL("the run never had frames queued and sent between an ask and its look");
    return true;
}

/* The dump's bookkeeping: one out at a time, due once a second, or 1 ms
 * after a reap or a request; seen when the chip clears the bit, given up
 * on after 10 ms. */
bool t_rtl8125_dump(void)
{
    struct rtl_dump d = { 0 };
    uint64_t t0 = 5000 * MS;
    CHECK_EQ(rtl_dump_look(&d, true, t0), RTL_DUMP_NONE);
    CHECK(rtl_dump_due(&d, t0, false));                    /* never asked: at once */
    rtl_dump_asked(&d, t0);
    CHECK(!rtl_dump_due(&d, t0 + 5000 * MS, true));        /* one out at a time */
    CHECK_EQ(rtl_dump_look(&d, true, t0 + 9 * MS), RTL_DUMP_WAITING);
    CHECK_EQ(rtl_dump_look(&d, false, t0 + 9 * MS), RTL_DUMP_LANDED);
    CHECK_EQ(d.landed, 1);
    CHECK_EQ(rtl_dump_look(&d, false, t0 + 9 * MS), RTL_DUMP_NONE);
    CHECK(!rtl_dump_due(&d, t0 + 999 * MS, false));
    CHECK(rtl_dump_due(&d, t0 + 1000 * MS, false));        /* once a second */
    CHECK(!rtl_dump_due(&d, t0 + MS / 2, true));
    CHECK(rtl_dump_due(&d, t0 + MS, true));                /* after a reap: 1 ms */
    rtl_dump_asked(&d, t0 + 2 * MS);
    CHECK_EQ(rtl_dump_look(&d, true, t0 + 12 * MS), RTL_DUMP_LATE);
    CHECK(!d.busy && d.late == 1 && d.late_in_row == 1);
    rtl_dump_asked(&d, t0 + 13 * MS);
    CHECK_EQ(rtl_dump_look(&d, true, t0 + 23 * MS), RTL_DUMP_LATE);
    CHECK_EQ(d.late_in_row, 2);
    rtl_dump_asked(&d, t0 + 24 * MS);
    CHECK_EQ(rtl_dump_look(&d, false, t0 + 25 * MS), RTL_DUMP_LANDED);
    CHECK(d.late_in_row == 0 && d.late == 2 && d.landed == 2);
    return true;
}

/* ---- the fake chip --------------------------------------------------------------------- */

static uint8_t fregs[0x10000] __attribute__((aligned(4096)));
static uint8_t fring[RING_VMO] __attribute__((aligned(4096)));
static struct rtl fake_rtl;

/* A running driver over the fake: the chip's count was BASE at the start,
 * the transmitter on (full mode), the link up at 1000 full. */
static struct rtl *fake(enum rtl_mode mode)
{
    struct rtl *t = &fake_rtl;
    memset(t, 0, sizeof(*t));
    memset(fregs, 0, sizeof(fregs));
    memset(fring, 0, sizeof(fring));
    t->r = fregs;
    t->ring = fring;
    t->ring_addr = 0x123450000ull;
    t->mode = mode;
    t->vlan = 21;
    /* what the reset's waits look for: the FIFOs empty, IM at 0x0103 */
    fregs[RTL_MCUCMD] = RTL_MCUCMD_RXFIFO_EMPTY | RTL_MCUCMD_TXFIFO_EMPTY;
    fregs[RTL_IM] = 0x03;
    fregs[RTL_IM + 1] = 0x01;
    fregs[RTL_CMD] = RTL_CMD_RXENB | (mode == RTL_MODE_FULL ? RTL_CMD_TXENB : 0);
    t->rx_on = true;
    t->tx_on = mode == RTL_MODE_FULL;
    t->link = true;
    t->phystat = RTL_PHYSTAT_LINK | RTL_PHYSTAT_1000 | RTL_PHYSTAT_FDX;
    t->tally0_ok = true;
    t->tally_tx0 = BASE;
    return t;
}

static uint32_t dtccr(void)
{
    uint32_t v;
    memcpy(&v, fregs + RTL_DTCCR_LO, 4);
    return v;
}

/* Has the driver asked for a dump (the chip's dump bit set)? */
static bool asked(void)
{
    return dtccr() & RTL_DTCCR_CMD;
}

/* The chip does the dump: its counters into the tally area, the bit cleared. */
static void chip_dumps(uint64_t tx_ok, uint64_t tx_err)
{
    struct tally x = { .tx_ok = tx_ok, .tx_err = tx_err, .rx_ok = 77 };
    memcpy(fring + TALLY_OFF, &x, sizeof(x));
    uint32_t v = dtccr() & ~RTL_DTCCR_CMD;
    memcpy(fregs + RTL_DTCCR_LO, &v, 4);
}

/* The guard's stop: the transmitter off, the chip reset (its state says
 * so), the reason kept, and no more looks or asks. */
static bool stopped(struct rtl *t, enum rtl_guard_verdict why, uint64_t now)
{
    CHECK_EQ(t->tripped, why);
    CHECK(!(fregs[RTL_CMD] & RTL_CMD_TXENB));
    CHECK(!t->rx_on && !t->tx_on);
    CHECK(guard_note(t)[0] == ',');
    uint32_t v = 0;
    memcpy(fregs + RTL_DTCCR_LO, &v, 4);
    guard_step(t, true, now + 5000 * MS);
    CHECK(!asked());
    return true;
}

/* guard.c over the fake chip: it asks for a dump, carries on while it is
 * out, checks it when the chip has done it against tx.queued as it is
 * then, and stops the chip on a frame too many. */
bool t_rtl8125_guard_fake(void)
{
    uint64_t now = 1000000 * MS;
    struct rtl *t = fake(RTL_MODE_FULL);
    guard_step(t, false, now);
    CHECK(asked() && t->dump.busy);
    uint32_t want = (uint32_t)(t->ring_addr + TALLY_OFF) | RTL_DTCCR_CMD;
    CHECK_EQ(dtccr(), want);                                /* rge's dump address */
    guard_step(t, false, now += MS / 2);                   /* out: nothing waits for it */
    CHECK(t->dump.busy && !t->tripped);
    chip_dumps(BASE, 0);
    guard_step(t, false, now += MS / 2);
    CHECK(!t->tripped && !t->dump.busy && t->dump.landed == 1);
    CHECK(!asked());                                       /* the next one in a second */
    guard_step(t, false, now += 500 * MS);
    CHECK(!asked());
    /* after a reap: the next dump 1 ms after the last ask */
    t->tx.queued = 5;
    guard_step(t, true, now += 2 * MS);
    CHECK(asked());
    chip_dumps(BASE + 2, 1);                               /* 2 still in flight */
    guard_step(t, true, now += MS);
    CHECK(!t->tripped);
    /* frames queued after the ask, sent before the chip's dump */
    guard_step(t, true, now += 2 * MS);
    CHECK(asked());
    t->tx.queued = 9;
    chip_dumps(BASE + 8, 1);
    guard_step(t, true, now += MS);
    CHECK(!t->tripped);
    /* one frame more than queued */
    guard_step(t, true, now += 2 * MS);
    CHECK(asked());
    chip_dumps(BASE + 9, 1);
    guard_step(t, false, now += MS);
    if (!stopped(t, RTL_GUARD_FOREIGN, now))
        return false;

    /* the listen-only probe: any frame sent at all */
    t = fake(RTL_MODE_PROBE);
    guard_step(t, false, now);
    chip_dumps(BASE + 1, 0);
    guard_step(t, false, now += MS);
    if (!stopped(t, RTL_GUARD_FOREIGN, now))
        return false;

    /* PAUSE TX at once, without a dump */
    t = fake(RTL_MODE_FULL);
    t->phystat |= RTL_PHYSTAT_RXFLOW;
    guard_step(t, false, now);
    CHECK(!t->tripped);
    t->phystat |= RTL_PHYSTAT_TXFLOW;
    guard_step(t, false, now += MS);
    if (!stopped(t, RTL_GUARD_PAUSE_TX, now))
        return false;

    /* a count gone backwards */
    t = fake(RTL_MODE_FULL);
    guard_step(t, false, now);
    chip_dumps(BASE - 1, 0);
    guard_step(t, false, now += MS);
    if (!stopped(t, RTL_GUARD_BACKWARDS, now))
        return false;

    /* a chip that never does the dump: given up after RTL_GUARD_UNREAD_MAX */
    t = fake(RTL_MODE_FULL);
    unsigned steps = 0;
    for (; !t->tripped && steps < 100; steps++)
        guard_step(t, true, now += 11 * MS);
    CHECK_EQ(t->dump.late, RTL_GUARD_UNREAD_MAX);
    if (!stopped(t, RTL_GUARD_UNREAD, now))
        return false;

    /* without the start's count every dump is unread too */
    t = fake(RTL_MODE_FULL);
    t->tally0_ok = false;
    for (steps = 0; !t->tripped && steps < 100; steps++) {
        guard_step(t, true, now += 2 * MS);
        if (asked())
            chip_dumps(BASE, 0);
    }
    CHECK_EQ(t->dump.landed, RTL_GUARD_UNREAD_MAX);
    return stopped(t, RTL_GUARD_UNREAD, now);
}
