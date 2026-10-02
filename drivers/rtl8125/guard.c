/* rtl8125: failing closed (drv/rtl8125; guard.h has the rule and its
 * pure functions). docs/history/M9-REVIEW.md, design question A.
 *
 * At every step of the loop (loop.c), in every mode:
 *   1. a tally dump that has landed since the last look is compared with
 *      tx.c's queued count as it is now (the chip's count since the start
 *      can't be more: guard.h says why frames in flight never trip it);
 *   2. PHYSTAT (chip.c's last read) must not say the link sends PAUSE;
 *   3. the next dump is asked for when it is due: a second after the last
 *      ask, or 1 ms after it when the step took transmit descriptors back,
 *      so the check follows every reap. Nothing waits for the dump: the
 *      next steps look at it (the loop wakes within a millisecond while
 *      one is out).
 * A verdict other than GO turns the transmitter (and the receiver) off at
 * once, resets the chip (chip_stop: no DMA after it), logs one line with
 * the evidence and sets t->tripped; the loop then ends and the driver
 * exits 1. devmgr restarts a driver that exits with an error, with
 * backoff, and gives up after 5 restarts within 60 s with a log line and
 * a RESULTS line of its own (user/services/devmgr/supervise.c), so a chip
 * that keeps doing it is left stopped. The restarted driver starts from a
 * reset like any other, with a new baseline. */
#include "rtl8125.h"

/* The chip's frames sent in a dump (good and errored), as the guard counts. */
static uint64_t sent_in(const struct tally *x)
{
    return x->tx_ok + x->tx_err;
}

static void trip(struct rtl *t, enum rtl_guard_verdict v, const struct rtl_guard_look *l)
{
    wr8(t, RTL_CMD, 0);   /* transmitter and receiver off: the guard allows this always */
    chip_stop(t);         /* and the chip reset */
    t->tripped = v;
    /* One line, short enough for the log (about 200 characters): why, the
     * tally against the queued count, the link with its flow control. */
    char link[48], tally[72];
    chip_link_str(l->phystat, link, sizeof(link));   /* "..., pause rx, PAUSE TX" */
    if (l->tallied && l->count >= l->base)
        drv_snprintf(tally, sizeof(tally), "%lu sent (%lu now, %lu at start)",
                     (unsigned long)(l->count - l->base), (unsigned long)l->count,
                     (unsigned long)l->base);
    else if (l->tallied)
        drv_snprintf(tally, sizeof(tally), "%lu now, %lu at start", (unsigned long)l->count,
                     (unsigned long)l->base);
    else
        drv_snprintf(tally, sizeof(tally), "none for %u looks%s", l->unread,
                     t->tally0_ok ? "" : " (none at start)");
    drv_log("STOPPED THE CHIP: %s. tally %s, queued %lu; link %s (phystat %#06x); tx off, "
            "chip reset, exit 1", rtl_guard_why(v), tally, (unsigned long)l->queued, link,
            l->phystat);
}

void guard_step(struct rtl *t, bool reaped, uint64_t now)
{
    if (t->tripped || !t->rx_on)
        return;   /* stopped already, or not running */
    (void)tally_look(t, now);
    struct rtl_guard_look l = { .phystat = t->phystat, .queued = t->tx.queued };
    if (t->dump.landed != t->guard_seen) {
        t->guard_seen = t->dump.landed;
        l.tallied = t->tally0_ok;
        l.base = t->tally_tx0;
        l.count = sent_in(&t->tally_last);
    }
    /* Without the start's count no dump can be compared: each counts as
     * unread, as do dumps given up on since the last that landed. */
    l.unread = t->tally0_ok ? t->dump.late_in_row
                            : (uint32_t)(t->dump.landed + t->dump.late);
    enum rtl_guard_verdict v = rtl_guard(&l);
    if (v != RTL_GUARD_GO) {
        trip(t, v, &l);
        return;
    }
    if (rtl_dump_due(&t->dump, now, reaped))
        tally_ask(t, now);
}

const char *guard_note(const struct rtl *t)
{
    switch (t->tripped) {
    case RTL_GUARD_GO:        return "";
    case RTL_GUARD_FOREIGN:   return ", TRANSMITTER STOPPED: frames it did not queue";
    case RTL_GUARD_PAUSE_TX:  return ", TRANSMITTER STOPPED: PAUSE TX";
    case RTL_GUARD_BACKWARDS: return ", TRANSMITTER STOPPED: tally went backwards";
    case RTL_GUARD_UNREAD:    return ", TRANSMITTER STOPPED: tally unreadable";
    }
    return ", TRANSMITTER STOPPED";
}
