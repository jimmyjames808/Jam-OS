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

typedef struct {
    uint64_t bits[MAX_CPUS / 64];
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

struct waitqueue {
    spinlock_t       lock;
    struct list_node waiters;
};

struct thread {
    uint64_t          rsp;           /* saved stack pointer while switched out */
    uint64_t          id;
    char              name[24];
    volatile int      state;
    int               prio;          /* effective priority (base or boosted) */
    int               base_prio;
    uint64_t          ready_since;   /* tick count on its CPU when queued */
    uint64_t          boosts;
    volatile bool     on_cpu;        /* its stack is still in use by a CPU */
    bool              is_idle;
    uint32_t          cpu;           /* CPU it runs on / is queued on */
    uint32_t          slice;
    cpumask_t         affinity;

    struct list_node  rq_node;       /* run queue */
    struct list_node  wait_node;     /* wait queue */
    struct list_node  sleep_node;    /* sleep list */
    uint64_t          wake_at_ns;

    void             *stack_top;
    volatile uint32_t refs;
    volatile bool     exited;
    struct waitqueue  exit_wq;

    uint64_t          switches_in;

    /* M5 user state. NULL for kernel threads, which run on the kernel's
     * page tables and never touch the FPU. */
    struct aspace    *aspace;        /* address space (a reference) */
    struct process   *process;       /* owning process */
    void             *ustate;        /* XSAVE area (fpu_ustate_alloc) */

    /* Set once by thread_cancel, never cleared: every cancellable wait
     * returns ERR_CANCELED from then on. */
    volatile bool     cancel_pending;

    /* Lock checker: mutexes this thread holds, innermost last. */
    uint32_t          sleep_depth;
    const void       *sleep_held[MAX_HELD_MUTEXES];
    uint8_t           sleep_cls[MAX_HELD_MUTEXES];
};

/* Create and start a thread. The caller gets a reference: release it with
 * thread_join (waits for exit) or thread_detach. */
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg, int prio);
/* Same, restricted to `mask` from the start. */
struct thread *thread_create_on(const char *name, void (*fn)(void *), void *arg, int prio,
                                const cpumask_t *mask);
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
 * Use for every wait a process could be killed in (M5). */
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
void thread_set_priority(struct thread *t, int prio);

static inline struct thread *current_thread(void) { return percpu_current(); }

void schedule(void);
/* Make a BLOCKED thread runnable again. */
void thread_wake(struct thread *t);

/* Boot: turn the running boot code into thread "main" and give the BSP an
 * idle thread. APs turn their own startup context into their idle thread. */
void sched_init_bsp(void);
_Noreturn void sched_run_ap_idle(void);
/* Called from the timer interrupt on every CPU. */
void sched_tick(void);
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
unsigned sched_stack_cache_set_limit(unsigned limit);
/* Stacks freed (unmapped, pages returned) since boot. */
uint64_t sched_stacks_freed(void);
/* Tell `cpu` to look at its run queue soon (IPI if remote). */
void sched_kick(uint32_t cpu);
void sched_print_stats(void);

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

struct mutex {
    spinlock_t       lock;
    struct thread   *owner;
    struct waitqueue wq;
    uint16_t         dep_cls;   /* lock checker class + 1, 0 until first use */
};
void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
/* ERR_CANCELED if the thread is cancelled while waiting (not taken). */
status_t mutex_lock_cancellable(struct mutex *m);
void mutex_unlock(struct mutex *m);
