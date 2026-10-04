/* Path statistics: what one call (or one context switch) costs, counted
 * and timed from inside the kernel, for the benchmark (kernel/test/
 * bench_path.c) and its tests. `make KTESTS=0` compiles every probe away.
 *
 * A trace follows a few member threads (kernel threads, or every thread
 * of a member process) and nothing else. While it is armed, every probe
 * a member passes outside interrupt handlers does one of two things:
 *   PATH_COUNT / PATH_ADD   adds to a counter (enum path_ev): system
 *                           calls, user and kernel copies and their
 *                           bytes, kmallocs, handle-table operations,
 *                           spinlock acquisitions, scheduler passes, FPU
 *                           saves, CR3 loads, clock reads, ...;
 *   PATH_MARK / PATH_MARK_ARG  records a TSC timestamp at a named point
 *                           of the path (enum path_mark).
 * Counts are exact, so they mean the same in QEMU as on the PC; the
 * timestamps only mean something on real hardware.
 *
 * Windows. The lead member (member 0) counts calls: each boundary mark it
 * passes (PATH_MK_CALL at channel_call's start, or PATH_MK_YIELD) is one.
 * Counters move only while the lead's call count is in [lo, lo + calls),
 * and marks are kept only for the first `marked` calls of that window, so
 * a run's start-up and its end (the result message, the exit) never
 * reach the numbers, and the per-call figures are the counts divided by
 * `calls`.
 *
 * Cost. Disarmed, a probe is one load of path_active and a branch not
 * taken. Armed, every probe anywhere pays a function call and the member
 * check, and a member's probe an atomic add (and a TSC read for a mark).
 * Only one trace exists at a time (path_begin refuses a second).
 *
 * Interrupt handlers are not counted (they are not the call's work), but
 * PATH_IRQ counts the interrupts that land on a member, and anything
 * the scheduler does on the way out of one (a preemption) is counted:
 * an extra scheduler pass per call is a finding, not noise. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum path_ev {
    PATH_SYSCALL,        /* system calls entered */
    PATH_IRQ,            /* interrupts taken while a member ran */
    PATH_TRAP,           /* exceptions (page faults, ...) while a member ran */
    PATH_UCOPY_IN,       /* copy_from_user calls (non-empty) */
    PATH_UCOPY_IN_B,     /* ... their bytes */
    PATH_UCOPY_OUT,      /* copy_to_user calls (non-empty) */
    PATH_UCOPY_OUT_B,    /* ... their bytes */
    PATH_KCOPY,          /* message copies inside the kernel (msg_new, delivery) */
    PATH_KCOPY_B,        /* ... their bytes */
    PATH_KMALLOC,        /* kmalloc and kzalloc */
    PATH_KFREE,          /* kfree of a non-NULL pointer */
    PATH_HANDLE,         /* handle-table operations (each takes the table lock) */
    PATH_LOCK,           /* spinlock acquisitions */
    PATH_SCHED,          /* schedule() passes */
    PATH_SWITCH,         /* thread switches (a pass that picked another thread) */
    PATH_WAKE,           /* wakes that made a thread READY and queued it (or handed it a CPU) */
    PATH_HANDOFF,        /* switches straight to a thread a waker handed its CPU to */
    PATH_IPI,            /* reschedule IPIs sent by a wake */
    PATH_FPU_SAVE,       /* user FPU state saved (XSAVE/XSAVEOPT/FXSAVE) */
    PATH_FPU_CALLED,     /* ... only its control words kept: switched out in a call */
    PATH_FPU_RESTORE,    /* user FPU state restored */
    PATH_FPU_KEPT,       /* restores skipped: the CPU still held the state */
    PATH_CR3,            /* address-space switches (CR3 loads) */
    PATH_CR3_FLUSH,      /* ... of them that dropped the TLB's user entries */
    PATH_JOB,            /* job charges and credits (message bytes, handles) */
    PATH_JOB_LEVEL,      /* job-tree levels a charge walked (one atomic each) */
    PATH_SLEEPQ,         /* waits with a deadline put on a sleeper queue */
    PATH_TIMER_ARM,      /* one-shot timer re-arms for a new earliest sleeper */
    PATH_EMPTY_READ,     /* channel reads that found nothing (ERR_SHOULD_WAIT) */
    PATH_OBSERVER,       /* observer callbacks fired by a signal change */
    PATH_CLOCK,          /* clock reads (uptime_ns) */
    PATH_LOCK_SLOW,      /* lock checker steps that turned interrupts off */
    PATH_CALLS,          /* boundaries the lead passed in the window (= calls) */
    PATH_EV_N
};

enum path_mark {
    PATH_MK_SYS_ENTER,   /* syscall_entry_c, before the dispatch (arg: number) */
    PATH_MK_SYS_EXIT,    /* syscall_entry_c, before the return to ring 3 (arg: number) */
    PATH_MK_CALL,        /* channel_call starts (the boundary of a call trace) */
    PATH_MK_YIELD,       /* thread_yield (the boundary of a switch trace) */
    PATH_MK_MSG_MADE,    /* a message is allocated, charged and copied */
    PATH_MK_SENT,        /* send_msg: queued or handed over, the reader woken */
    PATH_MK_REPLY,       /* channel_call: the reply is ours (out of the wait) */
    PATH_MK_READ,        /* channel_read done (arg: 1 if it found nothing) */
    PATH_MK_WAIT_IN,     /* object_wait_one / port_wait start */
    PATH_MK_WAIT_OUT,    /* object_wait_one / port_wait return */
    PATH_MK_BLOCK,       /* block_prepared: about to schedule() away */
    PATH_MK_SCHED_IN,    /* schedule() entered */
    PATH_MK_SCHED_LOCKED,/* schedule(): run queue lock taken */
    PATH_MK_SCHED_PICKED,/* schedule(): next thread chosen and accounted */
    PATH_MK_ARCH_FPU,    /* arch_thread_switch: FPU state saved and loaded */
    PATH_MK_ARCH_DONE,   /* arch_thread_switch: address space switched */
    PATH_MK_SCHED_DONE,  /* schedule(): back from the switch, on the new thread */
    PATH_MK_N
};

struct thread;
struct process;

#ifdef JAM_NO_KTESTS
#define PATH_ADD(ev, n)         ((void)(n))
#define PATH_COUNT(ev)          ((void)0)
#define PATH_MARK_ARG(mk, arg)  ((void)(arg))
#define PATH_MARK(mk)           ((void)0)
#define PATH_SW_COUNT(prev, next, ev) ((void)0)
#define PATH_SW_MARK(prev, next, mk)  ((void)0)
#define PATH_SYSCALL_NR(nr)           ((void)(nr))
#else
struct path_trace;
/* The armed trace, or NULL: what every probe reads first. */
extern struct path_trace *path_active;
void path_add_slow(struct path_trace *pt, enum path_ev ev, uint64_t n);
void path_mark_slow(struct path_trace *pt, enum path_mark mk, uint32_t arg);
void path_sys_slow(struct path_trace *pt, uint64_t nr);
void path_sw_add_slow(struct path_trace *pt, const struct thread *prev,
                      const struct thread *next, enum path_ev ev);
void path_sw_mark_slow(struct path_trace *pt, const struct thread *prev,
                       const struct thread *next, enum path_mark mk);

#define PATH_ADD(ev, n)                                                          \
    do {                                                                         \
        struct path_trace *_pt = __atomic_load_n(&path_active, __ATOMIC_ACQUIRE); \
        if (__builtin_expect(_pt != 0, 0))                                       \
            path_add_slow(_pt, (ev), (n));                                       \
    } while (0)
#define PATH_COUNT(ev) PATH_ADD(ev, 1)
#define PATH_MARK_ARG(mk, arg)                                                   \
    do {                                                                         \
        struct path_trace *_pt = __atomic_load_n(&path_active, __ATOMIC_ACQUIRE); \
        if (__builtin_expect(_pt != 0, 0))                                       \
            path_mark_slow(_pt, (mk), (uint32_t)(arg));                          \
    } while (0)
#define PATH_MARK(mk) PATH_MARK_ARG(mk, 0)
/* A system call entered: PATH_SYSCALL, and the count for its number. */
#define PATH_SYSCALL_NR(nr)                                                      \
    do {                                                                         \
        struct path_trace *_pt = __atomic_load_n(&path_active, __ATOMIC_ACQUIRE); \
        if (__builtin_expect(_pt != 0, 0))                                       \
            path_sys_slow(_pt, (nr));                                            \
    } while (0)
/* Inside a switch, where "current" is already the next thread: the event
 * belongs to the trace if prev or next is a member (prev's index first). */
#define PATH_SW_COUNT(prev, next, ev)                                            \
    do {                                                                         \
        struct path_trace *_pt = __atomic_load_n(&path_active, __ATOMIC_ACQUIRE); \
        if (__builtin_expect(_pt != 0, 0))                                       \
            path_sw_add_slow(_pt, (prev), (next), (ev));                         \
    } while (0)
#define PATH_SW_MARK(prev, next, mk)                                             \
    do {                                                                         \
        struct path_trace *_pt = __atomic_load_n(&path_active, __ATOMIC_ACQUIRE); \
        if (__builtin_expect(_pt != 0, 0))                                       \
            path_sw_mark_slow(_pt, (prev), (next), (mk));                        \
    } while (0)

/* ---- running a trace (kernel/test/pathstat.c) ----------------------------- */

#define PATH_MEMBERS   4      /* threads or processes a trace follows */
#define PATH_SYS_N     160    /* system call numbers counted one by one (above: the last) */
#define PATH_MARKS_MAX 8192   /* timestamps kept per trace */

/* One timestamp. `who` is the member index; `arg` is the mark's. */
struct path_stamp {
    uint64_t tsc;        /* rdtsc at the mark */
    uint16_t mark;       /* enum path_mark */
    uint8_t  who;        /* member index */
    uint8_t  cpu;        /* CPU index (low 8 bits) */
    uint32_t arg;        /* the mark's argument */
};

/* What a finished trace holds (path_end copies it out). */
struct path_result {
    uint64_t calls;                  /* boundaries counted in the window */
    uint64_t count[PATH_EV_N];       /* totals over the window */
    uint64_t sys[PATH_SYS_N];        /* PATH_SYSCALL by number */
    uint32_t nstamps;                /* stamps kept */
    bool     full;                   /* stamps were dropped: the buffer filled */
    const struct path_stamp *stamps; /* valid until the next path_begin */
};

/* Claim the trace: false if one is running. `boundary` is PATH_MK_CALL or
 * PATH_MK_YIELD; counting starts after `skip` boundaries of the lead, lasts
 * `calls` of them, and the first `marked` of those also keep timestamps. */
bool path_begin(enum path_mark boundary, uint64_t skip, uint64_t calls, uint64_t marked);
/* Members (before path_arm; the first one added is the lead). False when
 * the member table is full. */
bool path_add_thread(struct thread *t);
bool path_add_process(struct process *p);
/* Start following the members. */
void path_arm(void);
/* Has the lead passed the end of the window? */
bool path_window_done(void);
/* Stop, wait until no CPU is still inside a probe, and fill *out. Needs a
 * context that may sleep. */
void path_end(struct path_result *out);
/* Cost of one PATH_MARK on this CPU, in TSC cycles: the median of a run of
 * back-to-back marks by the current thread (needs no trace running). */
uint64_t path_mark_cost(void);
/* Names for printing. */
const char *path_ev_name(enum path_ev ev);
const char *path_mark_name(enum path_mark mk);
#endif
