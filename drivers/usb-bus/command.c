/* usb-bus: commands on the command ring (xHCI 4.6): one at a time, each
 * with a timeout; one that never completes is cut short with Command
 * Abort (4.6.1.2). hc.c sets the ring up (program_rings). */
#include "usbbus.h"

/* The command at t (of this type) got no completion in timeout_ms.
 * Command Abort: the ring stops, the command completes with Command
 * Aborted (or finishes meanwhile). A ring that doesn't stop in 5 s leaves
 * the controller dead. */
static void abort_command(struct hc *h, volatile struct trb *t, uint32_t type,
                          uint64_t timeout_ms)
{
    drv_log("command type %u: no completion in %lu ms; aborting it", type,
            (unsigned long)timeout_ms);
    /* The whole register holds a valid pointer (our enqueue point and
     * cycle), as Linux writes it: if the ring has stopped by the time
     * the high dword lands, some controllers take the 64-bit value as
     * the new Command Ring Pointer -- 0 would send the next command
     * fetch to physical address 0. */
    uint64_t next = h->ctx_dev + DMA_CMDRING + (uint64_t)h->cmd_enq * sizeof(struct trb);
    hc_op_write64(h, OP_CRCR, next | h->cmd_cycle | CRCR_CA);
    uint64_t end = drv_clock_ns() + 5000 * NS_PER_MS;
    while (drv_clock_ns() < end && (hc_op_read(h, OP_CRCR) & CRCR_CRR))
        hc_wait(h, drv_clock_ns() + 5 * NS_PER_MS);
    hc_poll(h);
    if (hc_op_read(h, OP_CRCR) & CRCR_CRR) {
        h->dead = true;
        drv_report("FAILED: the command ring did not stop 5 s after Command Abort");
    } else if (!h->cmd.done) {
        /* Stopped without taking it (never fetched): the next doorbell
         * would run it late, against contexts we free on the timeout.
         * A No Op in its place (what Linux does); its completion is
         * logged as not the outstanding one. */
        t->d3 = TRB_TYPE(TRB_NOOP_CMD) | (t->d3 & TRB_C);
    }
}

uint32_t hc_command(struct hc *h, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                    uint32_t *slot_out, uint64_t timeout_ms)
{
    if (h->dead || !h->running)
        return CC_GONE;
    struct trb *cr = (struct trb *)(h->ctx + DMA_CMDRING);
    uint64_t trb = h->ctx_dev + DMA_CMDRING + (uint64_t)h->cmd_enq * sizeof(struct trb);
    volatile struct trb *t = &cr[h->cmd_enq];
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t->d3 = (d3 & ~TRB_C) | h->cmd_cycle;
    if (++h->cmd_enq == RING_TRBS - 1) {
        volatile struct trb *l = &cr[RING_TRBS - 1];
        l->d3 = (l->d3 & ~TRB_C) | h->cmd_cycle;
        h->cmd_enq = 0;
        h->cmd_cycle ^= 1;
    }
    h->cmd.busy = true;
    h->cmd.done = false;
    h->cmd.trb = trb;
    h->cmd.cc = 0;
    h->cmd.slot = 0;
    hc_doorbell(h, 0, 0);
    uint64_t deadline = drv_clock_ns() + timeout_ms * NS_PER_MS;
    while (!h->cmd.done && !h->dead && drv_clock_ns() < deadline)
        hc_wait(h, deadline);
    if (!h->cmd.done && !h->dead)
        abort_command(h, t, TRB_TYPE_OF(d3), timeout_ms);
    h->cmd.busy = false;
    if (!h->cmd.done)
        return h->dead ? CC_GONE : CC_TIMEOUT;
    if (slot_out)
        *slot_out = h->cmd.slot;
    return h->cmd.cc;
}
