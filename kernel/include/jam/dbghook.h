/* Test injection points (DBG_HOOK), for ktests only: `make KTESTS=0`
 * compiles them away. Each hook is NULL (off) unless a race ktest installs
 * one; a hook only widens a window that the real code already has (lock
 * contention, an SMI, a preempted vCPU). */
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
