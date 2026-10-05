/* Test injection points (DBG_HOOK), for ktests only: `make KTESTS=0`
 * compiles them away. Each hook is NULL (off) unless a race ktest installs
 * one; a hook only widens a window that the real code already has (lock
 * contention, an SMI, a preempted vCPU), or fakes a failed check so a test
 * can see what follows one. */
#pragma once

enum {
    DBG_FINISH_SWITCH,     /* finish_switch, after the rq unlock, before prev->state */
    DBG_WAKE_ONCPU,        /* thread_wake, after reading t->cpu, before locking it */
    DBG_SCHED_PREV,        /* schedule, after prev->state was acted on, rq held */
    DBG_UNMAP_PRE_SHOOT,   /* vmm_unmap, after the local flush, before the shootdown */
    DBG_GATHER_PRE_FREE,   /* tlb_gather_finish, after the shootdown, before freeing
                              (arg: gather) */
    DBG_PROCESS_START,     /* process_start, process RUNNING, before its first thread is made
                              (arg: struct dbg_process_start, process.h) */
    DBG_DMA_RELEASED,      /* dma_cap.c release_batch, the batch's pages given back, before
                              the quarantine's counters record it (arg: the pci_dev) */
    DBG_STRESS_SHOOTDOWN,  /* stress run_seconds, a shootdown round's reads done, before they
                              are checked (arg: uint64_t *, CPUs that saw a stale mapping) */
    DBG_SCHED_PICKED,      /* schedule, the next thread taken off the run queue, before it is
                              marked running, rq held (arg: the thread, NULL: idle) */
    DBG_CHANNEL_CARRIED,   /* channel.c send_msg, the carried handles checked, before the
                              message is queued; pair lock held, interrupts off (arg: the
                              sending endpoint) */
    DBG_VMO_WRITE_COPY,    /* vmo_write, a page got, before the bytes are copied into it; no
                              lock held (arg: the VMO) */
    DBG_VTD_FAULT,         /* vtd_fault.c's log thread, a fault record taken from a unit's
                              ring, before it is logged; no lock held (arg: struct
                              vtd_fault_rec, vtd_internal.h) */
    DBG_CHANNEL_HANDED,    /* channel_send.c hand_to_locked, the waiter woken, before its
                              message is published; the endpoint's lock held, interrupts
                              off (arg: the waiting thread) */
    DBG_VTD_FLUSH,         /* vtd_unit.c vtd_flush_lines, the lines flushed for a unit that
                              doesn't snoop its tables (arg: struct vtd_flush_range,
                              vtd_internal.h) */
    DBG_N
};

/* Set and cleared by tests with __atomic_store_n (release); read once per
 * hook point with an acquire load. */
extern void (*dbg_hooks[DBG_N])(void *arg);

#ifdef JAM_NO_KTESTS
#define DBG_HOOK(id, arg) ((void)(arg))
#else
#define DBG_HOOK(id, arg)                                                  \
    do {                                                                   \
        void (*_h)(void *) = __atomic_load_n(&dbg_hooks[id], __ATOMIC_ACQUIRE); \
        if (_h)                                                            \
            _h(arg);                                                       \
    } while (0)
#endif
