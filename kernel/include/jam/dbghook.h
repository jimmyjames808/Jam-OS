/* AUDIT REPRO ONLY: test injection points. Each hook is NULL (off) unless a
 * repro ktest installs one; a hook only widens a window that the real code
 * already has (lock contention, an SMI, a preempted vCPU). */
#pragma once

enum {
    DBG_FINISH_SWITCH,     /* finish_switch, after the rq unlock, before prev->state */
    DBG_WAKE_ONCPU,        /* thread_wake, after reading t->cpu, before locking it */
    DBG_SCHED_PREV,        /* schedule, after prev->state was acted on, rq held */
    DBG_UNMAP_PRE_SHOOT,   /* vmm_unmap, after the local flush, before the shootdown */
    DBG_N
};

extern void (*volatile dbg_hooks[DBG_N])(void *arg);

#ifdef JAM_NO_KTESTS
#define DBG_HOOK(id, arg) ((void)(arg))
#else
#define DBG_HOOK(id, arg)                                \
    do {                                                 \
        void (*_h)(void *) = dbg_hooks[id];              \
        if (_h)                                          \
            _h(arg);                                     \
    } while (0)
#endif
