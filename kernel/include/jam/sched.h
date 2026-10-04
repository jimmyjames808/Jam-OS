/* Kernel threads and the scheduler.
 *
 * Each CPU has a run queue with 32 priority levels (31 = most urgent),
 * round-robin within a level and a time slice of SLICE_TICKS. Threads are
 * placed on the least-loaded CPU they are allowed on (ties go to P-cores),
 * and idle CPUs steal waiting threads from busy ones. The kernel is
 * preemptible except while a spinlock is held or interrupts are off. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/list.h>
#include <jam/percpu.h>
#include <jam/spinlock.h>
#include <jam/status.h>

#define PRIO_MIN     0
#define PRIO_MAX     31
#define PRIO_DEFAULT 16
#define SLICE_TICKS  2   /* 20 ms at 100 Hz */
/* Anti-starvation: a thread ready but not run for this long is boosted to
 * PRIO_BOOST for one slice. PRIO_MAX (31) is above the boost: real-time
 * threads may starve others, deliberately. */
#define STARVE_TICKS 100
#define PRIO_BOOST   30
/* Highest priority a user thread may set: its ceiling, see
 * thread_set_priority_cap. Kernel threads' ceiling is PRIO_MAX. */
#define PRIO_USER_MAX 24

typedef struct {
    uint64_t bits[MAX_CPUS / 64];   /* one bit per CPU index */
} cpumask_t;

static inline void cpumask_all(cpumask_t *m)
{
    for (unsigned i = 0; i < MAX_CPUS / 64; i++)
        m->bits[i] = ~0ull;
}
static inline void cpumask_one(cpumask_t *m, uint32_t cpu)
{
    for (unsigned i = 0; i < MAX_CPUS / 64; i++)
        m->bits[i] = 0;
    m->bits[cpu / 64] = 1ull << (cpu % 64);
}
static inline bool cpumask_has(const cpumask_t *m, uint32_t cpu)
{
    return m->bits[cpu / 64] & (1ull << (cpu % 64));
}

enum thread_state {
    T_RUNNING,     /* on a CPU right now */
    T_READY,       /* on a run queue */
    T_BLOCKED,     /* waiting (sleep, wait queue) */
    T_MIGRATING,   /* just switched out, must move to an allowed CPU */
    T_DEAD,
};

#define MAX_HELD_MUTEXES 8

struct aspace;
struct process;
struct uthread;

struct waitqueue {
    spinlock_t       lock;      /* guards waiters */
    struct list_node waiters;   /* threads waiting, oldest first */
};

struct thread {
    uint64_t          rsp;           /* saved stack pointer while switched out */
    uint64_t          id;            /* unique, never reused */
    char              name[24];      /* for logs and panics, NUL-terminated */
    int               state;         /* T_* */
    int               prio;          /* effective priority (base or boosted) */
    int               base_prio;     /* priority without a boost. Set from any CPU
                                      * (thread_set_priority) while schedule()
                                      * reads it: atomic loads and stores */
    int               prio_cap;      /* base_prio never exceeds this */
    uint64_t          ready_since;   /* tick count on its CPU when queued */
    uint64_t          boosts;        /* starvation boosts it has had */
    bool              on_cpu;        /* its stack is still in use by a CPU */
    bool              is_idle;       /* a CPU's idle thread */
    uint32_t          cpu;           /* CPU it runs on / is queued on. Written under
                                      * that CPU's run queue lock; a waker reads it
                                      * before it holds the lock (thread_cpu,
                                      * thread_set_cpu: atomic) */
    uint32_t          slice;         /* ticks left of its time slice */
    cpumask_t         affinity;      /* CPUs it may run on */

    struct list_node  rq_node;       /* run queue */
    struct list_node  wait_node;     /* wait queue */
    uint64_t          mutex_since;   /* waiting for a mutex since (uptime ns), 0 = not */
    struct list_node  sleep_node;    /* a CPU's sleeper queue (sched.c) */
    uint64_t          wake_at_ns;    /* deadline of its sleep (uptime ns) */
    uint64_t          wake_at_tsc;   /* the same deadline as a TSC value */
    uint32_t          sleep_cpu;     /* whose queue sleep_node is on */

    void             *stack_top;     /* top of its kernel stack */
    uint32_t          refs;          /* references: creator's and its own */
    bool              exited;        /* has run thread_exit; exit_wq.lock */
    struct waitqueue  exit_wq;       /* thread_join waits here */

    uint64_t          switches_in;   /* times it was switched in */
    /* CPU time: TSC cycles on a CPU up to its last switch out
     * (written by the CPU running it; thread_cpu_tsc adds the current run). */
    uint64_t          run_tsc;

    /* Wake-affine placement. wake_sync is set by the thread itself
     * while it is about to block waiting for the thread it wakes (see
     * thread_set_wake_sync); affine_wakes counts the times THIS thread was
     * woken onto its waker's CPU or that CPU's idle HT sibling. */
    bool              wake_sync;
    uint64_t          affine_wakes;
    /* Client/server pairs (sched.c): the thread that woke this one
     * last (its id) and how many wakes in a row came from it; pair_wakes
     * counts the times this thread was placed on its partner's sibling. */
    uint64_t          partner_id;
    uint32_t          partner_streak;
    uint64_t          pair_wakes;
    /* The direct hand-off (sched.c). handoff_ok is set by the thread itself
     * around a wake it blocks right after (see thread_set_handoff).
     * handoff_offers counts the times a waker handed its CPU to THIS
     * thread, handoffs the times it was then switched to directly (the
     * rest were queued); written by the waker that made it READY and by
     * schedule() under the run queue lock, never at once. */
    bool              handoff_ok;
    uint64_t          handoff_offers;
    uint64_t          handoffs;

    /* User state. NULL for kernel threads, which run on the kernel's
     * page tables and never touch the FPU. */
    struct aspace    *aspace;        /* address space (a reference) */
    struct process   *process;       /* owning process */
    struct uthread   *uthread;       /* its thread object (process.h) */
    void             *ustate;        /* XSAVE area (fpu_ustate_alloc) */
    uint32_t          fpu_cpu;       /* CPU whose registers were last loaded
                                      * from ustate (fpu.c) */
    bool              in_syscall;    /* inside a system call, so its vector registers
                                      * are not live (fpu.c keeps only the control
                                      * words). Written only by the thread itself,
                                      * with interrupts off; read by its own switches */

    /* Set once by thread_cancel, never cleared: every cancellable wait
     * returns ERR_CANCELED from then on. */
    bool              cancel_pending;

    /* A free message buffer of the channels' (kernel/object/channel.c: its
     * slot), or NULL. A small message for a reader that waits for it is
     * built in its writer's slot and handed over without an allocation.
     * Touched only by the thread itself; freed when it is reaped. */
    void             *msg_slot;

    /* Lock checker: mutexes this thread holds, innermost last. */
    uint32_t          sleep_depth;
    const void       *sleep_held[MAX_HELD_MUTEXES];
    uint8_t           sleep_cls[MAX_HELD_MUTEXES];
};

/* Create and start a thread. The caller gets a reference: release it with
 * thread_join (waits for exit) or thread_detach. */
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg, int prio);
/* Same, restricted to `mask` from the start. Both panic if there is no
 * memory for the thread (fine for the kernel's own threads). */
struct thread *thread_create_on(const char *name, void (*fn)(void *), void *arg, int prio,
                                const cpumask_t *mask);
/* Same, with a priority ceiling from the start (prio is clamped to it, and
 * so is every later thread_set_priority): PRIO_USER_MAX for user threads.
 * mask may be NULL (any CPU). Panics when out of memory. */
struct thread *thread_create_capped(const char *name, void (*fn)(void *), void *arg, int prio,
                                    const cpumask_t *mask, int prio_cap);
/* The fallible forms: NULL when out of memory (threads user code asks
 * for, and the timer service). */
struct thread *thread_try_create_on(const char *name, void (*fn)(void *), void *arg, int prio,
                                    const cpumask_t *mask);
/* Same, but the thread doesn't run until the caller thread_wake()s it: for
 * callers that must publish the thread somewhere first (process_start). */
struct thread *thread_try_create_suspended(const char *name, void (*fn)(void *), void *arg,
                                           int prio, const cpumask_t *mask, int prio_cap);
_Noreturn void thread_exit(void);
void thread_join(struct thread *t);
void thread_detach(struct thread *t);
void thread_yield(void);
void thread_sleep_ns(uint64_t ns);
/* Block until thread_wake() or until uptime_ns() reaches deadline_ns
 * (DEADLINE_NEVER: no timeout). `lock` guards the caller's wake condition:
 * it is held on entry, released while blocked, and held again on return.
 * Wakeups can be early or spurious, so callers loop on their condition and
 * compare uptime_ns() against the deadline themselves. */
#define DEADLINE_NEVER UINT64_MAX
void thread_block(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns);
/* Same, but returns ERR_CANCELED (without blocking, or as soon as it is
 * woken) once thread_cancel has been called on this thread; OK otherwise.
 * Use for every wait a process could be killed in. */
status_t thread_block_cancellable(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns);
status_t thread_sleep_cancellable(uint64_t ns);
/* Ask t to stop waiting: set its cancel flag (for good) and wake it. Its
 * cancellable waits return ERR_CANCELED; plain waits just see a spurious
 * wakeup and carry on. */
void thread_cancel(struct thread *t);
/* Has thread_cancel been called on the current thread? */
bool thread_cancel_pending(void);
static inline void thread_sleep_ms(uint64_t ms) { thread_sleep_ns(ms * 1000000); }
/* Restrict where t may run. For the current thread this takes effect at
 * once (it migrates before returning). */
void thread_set_affinity(struct thread *t, const cpumask_t *mask);
/* Clamped to [PRIO_MIN, t's ceiling]. Takes effect the next time t is
 * queued or switched out. */
void thread_set_priority(struct thread *t, int prio);
/* Set t's priority ceiling (clamped to [PRIO_MIN, PRIO_MAX]); a base
 * priority above it comes down to it. Starvation boosts may still lift a
 * thread above its ceiling for one slice. */
void thread_set_priority_cap(struct thread *t, int cap);

static inline struct thread *current_thread(void) { return percpu_current(); }

void schedule(void);
/* Make a BLOCKED thread runnable again. */
void thread_wake(struct thread *t);
/* Wake-affine: the same, for a waker that is about to block waiting for t
 * (a channel_call request, or a reply from a server with nothing else
 * queued). t is placed on the waker's CPU if it may run there and nothing
 * else is queued there (it runs as soon as the waker blocks), else on the
 * waker's idle HT sibling, else as usual. From interrupt handlers it is a
 * plain thread_wake. */
void thread_wake_sync(struct thread *t);
/* While on, the next thread the current thread wakes (from thread context,
 * e.g. through an object observer) is woken as by thread_wake_sync; that
 * wake turns it off. channel_call turns it on around sending its request. */
void thread_set_wake_sync(bool on);
/* The direct hand-off. While the current thread's flag is on, a wake it
 * makes that wake-affine placement would put on this very CPU, with
 * nothing queued here, records the wakee as this CPU's next thread
 * instead of queueing it. The next
 * schedule() here switches straight to it, without the run queues, if the
 * waker is leaving the CPU (it blocked), has some of its time slice left
 * and nothing queued outranks the wakee; the wakee runs on the rest of
 * that slice. Otherwise it is queued then, as the wake would have queued
 * it. Turn the flag on only around a wake the thread blocks right after
 * (channel_call's request, channel_reply_wait's reply), and call
 * sched_handoff_done once the wait is over: if the thread never blocked,
 * that queues the wakee. Switch sched_handoff, boot "nohandoff". */
extern bool sched_handoff;
void thread_set_handoff(bool on);
void sched_handoff_done(void);

/* Boot: turn the running boot code into thread "main" and give the BSP an
 * idle thread. APs turn their own startup context into their idle thread. */
void sched_init_bsp(void);
_Noreturn void sched_run_ap_idle(void);
/* Once every AP that will start has started: record each CPU's HT sibling
 * for placement (until then placement knows no siblings). */
void sched_topology_init(void);
/* Called from the timer interrupt on every CPU, at each scheduler tick. */
void sched_tick(void);
/* From every timer interrupt: wake this CPU's sleepers that are
 * due and re-arm its timer for the next one. True if it woke any. */
bool sched_timer_expire(void);
/* From the interrupt exit path: switch if a reschedule is pending. */
void sched_irq_exit(uint64_t interrupted_rflags);
/* Total anti-starvation boosts so far. */
uint64_t sched_boost_count(void);
/* Pages held by cached thread stacks, including stacks over the cache limit
 * that wait to be freed (sched_stack_trim), for leak checks. */
uint64_t sched_stack_cache_pages(void);
/* Free the stacks waiting to be freed now, if this context may (interrupts
 * on, no spinlock held: freeing shoots down TLBs). Thread creation and exit
 * call it too. */
void sched_stack_trim(void);
/* Set how many exited threads' stacks are kept for reuse (at most
 * SCHED_STACK_CACHE_MAX; tests lower it); stacks over the new limit are
 * freed at once. Returns the old limit. */
#define SCHED_STACK_CACHE_MAX 256
/* Every thread's kernel stack (user threads are charged for it, process.c). */
#define THREAD_STACK_SIZE (64 * 1024)
unsigned sched_stack_cache_set_limit(unsigned limit);
/* Stacks freed (unmapped, pages returned) since boot. */
uint64_t sched_stacks_freed(void);

/* Spin before idle: how long an idle CPU polls for work before it
 * halts, in ns (0 = halt at once). Boot: "idlespin=<us>", "nospinidle". The
 * benchmark flips it at run time. */
#define SCHED_IDLE_SPIN_NS 10000
extern uint64_t sched_idle_spin_ns;
/* Hybrid placement order: idle whole P-core > idle E-core > idle HT
 * sibling of a busy core > least loaded (sched.c, select_cpu). Off: the
 * plain least-loaded rule. Boot: "noplaceorder". */
extern bool sched_place_order;
/* Client/server pairs on sibling hyperthreads: two threads that
 * keep waking each other are placed on one core's two hyperthreads when
 * the waker keeps running. Boot: "noaffinepair". */
extern bool sched_affine_pair;
#ifndef JAM_NO_KTESTS
/* Tests: CPU cpu's load as placement reads it (queued + running). */
uint32_t sched_cpu_load(uint32_t cpu);
/* Tests: run the placement rule on a made-up topology (arrays indexed by
 * CPU, MAX_CPUS long; sibling -1 = none, type = enum core_type). */
uint32_t sched_pick_cpu_fake(const cpumask_t *cand, const int16_t *sibling,
                             const uint8_t *type, const uint32_t *load, uint32_t last,
                             bool order);
#endif
void sched_print_stats(void);

/* CPU time (the shell's top, ps). Counted in TSC cycles at every switch
 * (one rdtsc); reads are lock-free and may be a few cycles stale. */
/* t's time on a CPU so far, its current run included (t must be held). */
uint64_t thread_cpu_tsc(struct thread *t);
/* How long CPU i's idle thread has run so far, the current run included. */
uint64_t sched_cpu_idle_tsc(uint32_t i);

/* ---- wait queues and mutexes --------------------------------------------- */

void waitqueue_init(struct waitqueue *wq, const char *name);
/* Block the current thread until woken. The caller re-checks its condition
 * in a loop: wakeups can be early. `lock` (held, may be NULL) is released
 * once the thread is queued, closing the check-then-sleep race. */
void waitqueue_wait(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags);
/* Same, giving up at deadline_ns (the caller checks uptime_ns()). */
void waitqueue_wait_until(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags,
                          uint64_t deadline_ns);
/* Cancellable form of waitqueue_wait_until (DEADLINE_NEVER for none). */
status_t waitqueue_wait_cancellable(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags,
                                    uint64_t deadline_ns);
void waitqueue_wake_one(struct waitqueue *wq);
void waitqueue_wake_all(struct waitqueue *wq);

/* Sleeping lock. Normally the woken waiter races newcomers for it (fast
 * under light contention); once some waiter has waited MUTEX_HANDOFF_NS,
 * mutex_unlock hands the mutex straight to the longest waiter instead, so
 * no thread can be starved by others barging in ahead of it. */
#define MUTEX_HANDOFF_NS 1000000ull
extern uint64_t mutex_handoffs;   /* times a mutex was handed to a waiter (atomic) */
/* A thread's state (T_*). It changes under the lock of the run queue or
 * wait queue the thread is on, and other CPUs read it without that lock,
 * so every access is atomic: relaxed, as the locks and on_cpu's
 * release/acquire order everything else. */
static inline int thread_state(const struct thread *t)
{
    return __atomic_load_n(&t->state, __ATOMIC_RELAXED);
}

static inline void thread_set_state(struct thread *t, int state)
{
    __atomic_store_n(&t->state, state, __ATOMIC_RELAXED);
}

/* t->cpu: a waker reads it to pick the run queue lock to take, then checks
 * it again under that lock (sched.c, the task_rq_lock pattern), so the
 * writers release what the readers acquire. */
static inline uint32_t thread_cpu(const struct thread *t)
{
    return __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
}

static inline void thread_set_cpu(struct thread *t, uint32_t cpu)
{
    __atomic_store_n(&t->cpu, cpu, __ATOMIC_RELEASE);
}

struct mutex {
    spinlock_t       lock;      /* guards owner and the hand-off */
    struct thread   *owner;     /* the holder, NULL if free */
    struct waitqueue wq;        /* threads waiting for it */
    uint16_t         dep_cls;   /* lock checker class + 1, 0 until first use */
};
void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
/* ERR_CANCELED if the thread is cancelled while waiting (not taken). */
status_t mutex_lock_cancellable(struct mutex *m);
void mutex_unlock(struct mutex *m);
