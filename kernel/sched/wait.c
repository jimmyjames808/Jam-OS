/* Blocking: waits with a deadline (per-CPU sleeper queues and one-shot
 * timers), cancellation and interrupts, wait queues and mutexes. Waking
 * is thread_wake in sched.c.
 *
 * Three kinds of wait. A plain one ends only on its own condition or
 * deadline. A cancellable one also ends at thread_cancel (ERR_CANCELED,
 * for good: the thread's process is dying). An interruptible one also
 * ends at thread_interrupt (ERR_INTERRUPTED: POSIX's EINTR), which is
 * latched: one that comes while the thread isn't in such a wait ends its
 * next one at once, and however many come before a wait takes it, they
 * end one wait. Only the system calls' own waits are interruptible
 * (object_wait_one, port_wait, futex_wait, sleep_until); every other wait
 * sees thread_interrupt's wake as a spurious wakeup and waits on. */
#include <jam/lapic.h>
#include <jam/panic.h>
#include <jam/pathstat.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/time.h>

#include "sched_internal.h"

/* Sleeping threads (any wait with a deadline): per-CPU one-shot timers. A
 * thread that blocks with a deadline goes on the queue of the CPU it blocks
 * on, sorted by deadline, and that CPU's timer is armed for the queue's
 * head (lapic_timer_set), so it is woken within microseconds of its
 * deadline instead of at the next 10 ms tick. A head due at or after the
 * next tick is left to the tick, which re-arms for it (after_next_tick).
 * Races:
 *   - Only the owning CPU adds to its queue and arms its timer (the
 *     blocking thread has preemption off; the timer interrupt runs there),
 *     always under the queue lock with interrupts off.
 *   - Removal happens from anywhere: the expiring timer interrupt, or the
 *     thread itself once it is awake (on whatever CPU it runs on now), under
 *     the queue lock of the CPU it slept on (t->sleep_cpu, written only by
 *     t when it queues itself). A removal can only make the head later, so
 *     the armed timer may fire for nothing: harmless.
 *   - The expiring interrupt calls thread_wake(t) with the lock held, and t
 *     takes the same lock before it returns from its wait, so t can't
 *     return, exit and be freed while its waker is still in thread_wake.
 * Expiry compares TSC values (uptime_to_tsc rounds up), so a thread woken
 * at its deadline sees uptime_ns() >= the deadline and doesn't re-block.
 * In the periodic timer mode, or with lapic_oneshot off, each CPU's tick
 * expires its own queue. */
struct sleepq {
    spinlock_t       lock;   /* guards list */
    struct list_node list;   /* struct thread, by wake_at_tsc */
} __attribute__((aligned(64)));
static struct sleepq sleepqs[MAX_CPUS];

/* Interrupts off: does a deadline come at or after this CPU's next tick?
 * Then it needs no timer of its own: every tick expires the queue and
 * re-arms the timer for the head (sched_timer_expire, from lapic.c's
 * on_timer), so the deadline is met to the microsecond all the same, and
 * the far deadlines most waits carry (a call's 5 s) cost no timer write.
 * This relies on the tick never stopping (there is no tickless idle; M10's
 * row in docs/ROADMAP.md carries the obligation: a CPU that stops its tick
 * arms its timer for this queue's head first). In
 * the periodic mode tick_deadline stays 0: nothing is armed there anyway. */
static bool after_next_tick(uint64_t wake_at_tsc)
{
    return wake_at_tsc >= this_cpu()->tick_deadline;
}

/* The owning CPU, queue lock held: arm the timer for the head. */
static void sleepq_arm(const struct sleepq *q)
{
    lapic_timer_set(list_empty(&q->list)
                        ? 0 : list_first(&q->list, struct thread, sleep_node)->wake_at_tsc);
}

void sleepq_init(uint32_t cpu)
{
    spin_init(&sleepqs[cpu].lock, "sleepers");
    list_init(&sleepqs[cpu].list);
}

/* ---- blocking ----------------------------------------------------------- */

/* What may end a wait besides its own condition and deadline. */
enum wait_kind {
    WAIT_PLAIN,    /* nothing: a cancel or an interrupt is a spurious wakeup */
    WAIT_CANCEL,   /* thread_cancel (ERR_CANCELED) */
    WAIT_INTR,     /* thread_cancel, and thread_interrupt (ERR_INTERRUPTED) */
};

static bool cancel_seen(const struct thread *t)
{
    return __atomic_load_n(&t->cancel_pending, __ATOMIC_ACQUIRE);
}

static bool interrupt_seen(const struct thread *t)
{
    return __atomic_load_n(&t->interrupt_pending, __ATOMIC_ACQUIRE);
}

/* Does something end a wait of this kind now? ERR_CANCELED first: a
 * thread being killed leaves whatever else is pending. */
static status_t wait_ended(const struct thread *t, enum wait_kind kind)
{
    if (kind != WAIT_PLAIN && cancel_seen(t))
        return ERR_CANCELED;
    if (kind == WAIT_INTR && interrupt_seen(t))
        return ERR_INTERRUPTED;
    return OK;
}

/* The current thread is already T_BLOCKED (set while the lock that guards
 * its wake condition was held, so no waker can slip in unnoticed) AND with
 * preemption disabled by the caller: a preemption between marking itself
 * blocked and arming the deadline would switch it out with nothing left to
 * wake it (stress saw it as "sleeper made no progress"). Arm the deadline,
 * drop `lock`, re-enable preemption, switch out, and on return re-take
 * `lock` (unless !relock). Returns ERR_CANCELED or ERR_INTERRUPTED if the
 * wait's kind lets one end it (before or during the wait; an interrupt
 * not once the deadline has passed), else OK. An
 * interrupt is reported, not taken: the waiter takes it
 * (thread_interrupt_consume) only if it returns ERR_INTERRUPTED itself,
 * so a wait whose condition came true meanwhile leaves it for the next. */
static status_t block_prepared(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns,
                               enum wait_kind kind, bool relock)
{
    struct thread *t = current_thread();
    if (deadline_ns != DEADLINE_NEVER) {
        /* Preemption is off (see above): this CPU's queue stays ours. */
        uint32_t cpu = this_cpu()->index;
        struct sleepq *q = &sleepqs[cpu];
        t->wake_at_ns = deadline_ns;
        t->wake_at_tsc = uptime_to_tsc(deadline_ns);
        t->sleep_cpu = cpu;
        uint64_t f = spin_lock_irqsave(&q->lock);
        struct list_node *pos = q->list.prev;   /* most deadlines go last */
        while (pos != &q->list &&
               container_of(pos, struct thread, sleep_node)->wake_at_tsc > t->wake_at_tsc)
            pos = pos->prev;
        list_add(pos, &t->sleep_node);
        PATH_COUNT(PATH_SLEEPQ);
        if (q->list.next == &t->sleep_node && !after_next_tick(t->wake_at_tsc)) {
            PATH_COUNT(PATH_TIMER_ARM);
            sleepq_arm(q);   /* the new head */
        }
        spin_unlock_irqrestore(&q->lock, f);
    }
    if (lock)
        spin_unlock_irqrestore(lock, *irqflags);
    /* Cancellation and interrupts: our T_BLOCKED store and thread_cancel's
     * (thread_interrupt's) flag store are each followed by a read of the
     * other (the flag here, our state in its thread_wake), with full
     * fences between, so at least one side sees the other: either we see
     * the flag and don't sleep, or it sees us blocked and wakes us. We may
     * only skip the switch by moving BLOCKED -> RUNNING ourselves: if a
     * waker already made us READY it has also queued us, and then we must
     * go through schedule() like any thread woken before it switched out
     * (overwriting READY put a running thread on a run queue;
     * cancel_races_wakeup caught it as "dead thread was scheduled"). */
    bool skip = false;
    if (kind != WAIT_PLAIN) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (wait_ended(t, kind) != OK) {
            int expect = T_BLOCKED;
            skip = __atomic_compare_exchange_n(&t->state, &expect, T_RUNNING, false,
                                               __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        }
    }
    /* Registered for wakeup (wait queue / deadline): being preempted from
     * here on is harmless, so preemption can come back on. */
    preempt_enable_no_resched();
    PATH_MARK(PATH_MK_BLOCK);
    if (!skip)
        schedule();
    /* Always take the queue lock before touching sleep_node when we may
     * have been on a sleeper queue: sched_timer_expire deletes the node and
     * calls thread_wake(t) while holding it, so a lockless check here could
     * let this thread return (and re-block or exit, freeing itself) while
     * the waker is still inside thread_wake(t). Serialising on the lock
     * keeps t alive until the waker is done. The queue is the one of the
     * CPU we slept on, not the one we run on now. */
    if (deadline_ns != DEADLINE_NEVER) {
        struct sleepq *q = &sleepqs[t->sleep_cpu];
        uint64_t f = spin_lock_irqsave(&q->lock);
        if (t->sleep_node.next)
            list_del(&t->sleep_node);
        spin_unlock_irqrestore(&q->lock, f);
    }
    if (lock && relock)
        *irqflags = spin_lock_irqsave(lock);
    status_t st = wait_ended(t, kind);
    /* A deadline that passed is the wait's own end, as a met condition is:
     * the caller's loop finds it and times out, the interrupt left for
     * the next wait. (test: intr_deadline_comes_first) */
    if (st == ERR_INTERRUPTED && deadline_ns != DEADLINE_NEVER && uptime_ns() >= deadline_ns)
        st = OK;
    return st;
}

static status_t block_kind(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns,
                           enum wait_kind kind, bool relock)
{
    preempt_disable();   /* re-enabled in block_prepared */
    thread_set_state(current_thread(), T_BLOCKED);
    return block_prepared(lock, irqflags, deadline_ns, kind, relock);
}

void thread_block(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns)
{
    block_kind(lock, irqflags, deadline_ns, WAIT_PLAIN, true);
}

status_t thread_block_cancellable(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns)
{
    return block_kind(lock, irqflags, deadline_ns, WAIT_CANCEL, true);
}

status_t thread_block_cancellable_unlocked(spinlock_t *lock, uint64_t irqflags,
                                           uint64_t deadline_ns)
{
    return block_kind(lock, &irqflags, deadline_ns, WAIT_CANCEL, false);
}

status_t thread_block_interruptible(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns)
{
    return block_kind(lock, irqflags, deadline_ns, WAIT_INTR, true);
}

status_t thread_block_interruptible_unlocked(spinlock_t *lock, uint64_t irqflags,
                                             uint64_t deadline_ns)
{
    return block_kind(lock, &irqflags, deadline_ns, WAIT_INTR, false);
}

void thread_sleep_ns(uint64_t ns)
{
    uint64_t deadline = uptime_ns() + ns;
    while (uptime_ns() < deadline)
        thread_block(NULL, NULL, deadline);
}

status_t thread_sleep_cancellable(uint64_t ns)
{
    uint64_t deadline = uptime_ns() + ns;
    while (uptime_ns() < deadline)
        if (thread_block_cancellable(NULL, NULL, deadline) != OK)
            return ERR_CANCELED;
    return OK;
}

status_t thread_sleep_until_interruptible(uint64_t deadline_ns)
{
    while (uptime_ns() < deadline_ns) {
        status_t st = thread_block_interruptible(NULL, NULL, deadline_ns);
        if (st != OK)
            return thread_interrupt_consume(st);
    }
    return OK;
}

void thread_cancel(struct thread *t)
{
    __atomic_store_n(&t->cancel_pending, true, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    thread_wake(t);
}

bool thread_cancel_pending(void)
{
    return cancel_seen(current_thread());
}

void thread_interrupt(struct thread *t)
{
    __atomic_store_n(&t->interrupt_pending, true, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    thread_wake(t);
}

status_t thread_interrupt_consume(status_t st)
{
    /* Only the thread itself clears its flag, so it is still set here:
     * this takes the interrupt it saw and any that came since (latched
     * once, not counted). */
    if (st == ERR_INTERRUPTED)
        __atomic_store_n(&current_thread()->interrupt_pending, false, __ATOMIC_SEQ_CST);
    return st;
}

bool thread_interrupt_pending(const struct thread *t)
{
    return interrupt_seen(t);
}

uint64_t raise_latched, raise_kicked;

bool thread_raise(struct thread *t)
{
    /* The exchange orders whatever the raiser wrote before (libc's pending
     * signal) before the latch the thread takes, and tells a new latch
     * from one already there: only a new one kicks. */
    if (__atomic_exchange_n(&t->raise_pending, true, __ATOMIC_SEQ_CST))
        return false;
    __atomic_add_fetch(&raise_latched, 1, __ATOMIC_RELAXED);
    if (sched_kick_running(t))
        __atomic_add_fetch(&raise_kicked, 1, __ATOMIC_RELAXED);
    return true;
}

bool sched_timer_expire(void)
{
    struct sleepq *q = &sleepqs[this_cpu()->index];
    bool woke = false;
    spin_lock(&q->lock);   /* in the timer interrupt: IRQs already off */
    uint64_t now = rdtsc();
    while (!list_empty(&q->list)) {
        struct thread *t = list_first(&q->list, struct thread, sleep_node);
        if (t->wake_at_tsc > now)
            break;
        list_del(&t->sleep_node);
        thread_wake(t);
        woke = true;
    }
    sleepq_arm(q);
    spin_unlock(&q->lock);
    return woke;
}

/* ---- wait queues and mutexes --------------------------------------------- */

void waitqueue_init(struct waitqueue *wq, const char *name)
{
    spin_init(&wq->lock, name);
    list_init(&wq->waiters);
}

static status_t wq_wait(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags,
                        uint64_t deadline_ns, enum wait_kind kind)
{
    struct thread *t = current_thread();
    bool same = lock == &wq->lock;
    uint64_t f = 0;
    preempt_disable();   /* re-enabled in block_prepared */
    if (!same)
        f = spin_lock_irqsave(&wq->lock);
    list_add_tail(&wq->waiters, &t->wait_node);
    thread_set_state(t, T_BLOCKED);   /* before the wq lock drops: wakers pop under it */
    if (!same)
        spin_unlock_irqrestore(&wq->lock, f);
    status_t st = block_prepared(lock, irqflags, deadline_ns, kind, true);

    /* Take ourselves off the wait queue if we are still on it (timed out or
     * spurious). Always hold wq->lock while touching wait_node: wake() pops
     * the node and calls thread_wake(t) under wq->lock, so a lockless read
     * could let this thread return and free itself mid-wake. When same, the
     * caller's lock (already re-held by block_prepared) is wq->lock, so we
     * are serialised; otherwise take wq->lock explicitly. */
    if (!same) {
        uint64_t g = spin_lock_irqsave(&wq->lock);
        if (t->wait_node.next)
            list_del(&t->wait_node);
        spin_unlock_irqrestore(&wq->lock, g);
    } else if (t->wait_node.next) {
        list_del(&t->wait_node);
    }
    return st;
}

void waitqueue_wait_until(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags,
                          uint64_t deadline_ns)
{
    wq_wait(wq, lock, irqflags, deadline_ns, WAIT_PLAIN);
}

void waitqueue_wait(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags)
{
    wq_wait(wq, lock, irqflags, DEADLINE_NEVER, WAIT_PLAIN);
}

status_t waitqueue_wait_cancellable(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags,
                                    uint64_t deadline_ns)
{
    return wq_wait(wq, lock, irqflags, deadline_ns, WAIT_CANCEL);
}

status_t waitqueue_wait_interruptible(struct waitqueue *wq, spinlock_t *lock,
                                      uint64_t *irqflags, uint64_t deadline_ns)
{
    return wq_wait(wq, lock, irqflags, deadline_ns, WAIT_INTR);
}

static void wake(struct waitqueue *wq, bool all)
{
    uint64_t f = spin_lock_irqsave(&wq->lock);
    while (!list_empty(&wq->waiters)) {
        struct thread *t = list_first(&wq->waiters, struct thread, wait_node);
        list_del(&t->wait_node);
        thread_wake(t);
        if (!all)
            break;
    }
    spin_unlock_irqrestore(&wq->lock, f);
}

void waitqueue_wake_one(struct waitqueue *wq) { wake(wq, false); }
void waitqueue_wake_all(struct waitqueue *wq) { wake(wq, true); }

/* m->lock held, m->owner NULL: wake the first waiter to race for it, or,
 * once the longest waiter has waited MUTEX_HANDOFF_NS, make that waiter the
 * owner before waking it, so nobody can take the mutex in between.
 * Waiters that are still on the queue have mutex_since set (they set it
 * under m->lock before queueing and clear it after leaving). */
uint64_t mutex_handoffs;   /* statistics */

static void mutex_pass_on(struct mutex *m)
{
    struct waitqueue *wq = &m->wq;
    uint64_t g = spin_lock_irqsave(&wq->lock);
    struct thread *oldest = NULL;
    for (struct list_node *n = wq->waiters.next; n != &wq->waiters; n = n->next) {
        struct thread *t = container_of(n, struct thread, wait_node);
        if (t->mutex_since && (!oldest || t->mutex_since < oldest->mutex_since))
            oldest = t;
    }
    if (oldest && uptime_ns() - oldest->mutex_since >= MUTEX_HANDOFF_NS) {
        list_del(&oldest->wait_node);
        m->owner = oldest;
        __atomic_add_fetch(&mutex_handoffs, 1, __ATOMIC_RELAXED);
        thread_wake(oldest);
    } else if (!list_empty(&wq->waiters)) {
        struct thread *t = list_first(&wq->waiters, struct thread, wait_node);
        list_del(&t->wait_node);
        thread_wake(t);
    }
    spin_unlock_irqrestore(&wq->lock, g);
}

void mutex_init(struct mutex *m, const char *name)
{
    spin_init(&m->lock, name);
    m->owner = NULL;
    m->dep_cls = 0;
    waitqueue_init(&m->wq, "mutex waiters");
}

void mutex_lock(struct mutex *m)
{
    struct thread *me = current_thread();
    lockdep_sleep_acquire(m, m->lock.name, &m->dep_cls);
    uint64_t f = spin_lock_irqsave(&m->lock);
    if (m->owner == me)
        panic("mutex \"%s\": recursive lock by \"%s\"", m->lock.name, me->name);
    if (m->owner) {
        me->mutex_since = uptime_ns() | 1;
        while (m->owner && m->owner != me)   /* owner == me: handed to us */
            waitqueue_wait(&m->wq, &m->lock, &f);
        me->mutex_since = 0;
    }
    m->owner = me;
    spin_unlock_irqrestore(&m->lock, f);
}

status_t mutex_lock_cancellable(struct mutex *m)
{
    struct thread *me = current_thread();
    lockdep_sleep_acquire(m, m->lock.name, &m->dep_cls);
    uint64_t f = spin_lock_irqsave(&m->lock);
    if (m->owner == me)
        panic("mutex \"%s\": recursive lock by \"%s\"", m->lock.name, me->name);
    if (m->owner)
        me->mutex_since = uptime_ns() | 1;
    while (m->owner && m->owner != me) {   /* owner == me: handed to us */
        if (waitqueue_wait_cancellable(&m->wq, &m->lock, &f, DEADLINE_NEVER) != OK) {
            if (m->owner == me)
                break;   /* handed over as we were cancelled: it's ours */
            /* mutex_unlock may have picked us as the one waiter to wake:
             * pass that on so the next waiter doesn't sleep through it. */
            if (!m->owner)
                waitqueue_wake_one(&m->wq);
            me->mutex_since = 0;
            spin_unlock_irqrestore(&m->lock, f);
            lockdep_sleep_release(m);
            return ERR_CANCELED;
        }
    }
    me->mutex_since = 0;
    m->owner = me;
    spin_unlock_irqrestore(&m->lock, f);
    return OK;
}

void mutex_unlock(struct mutex *m)
{
    uint64_t f = spin_lock_irqsave(&m->lock);
    if (m->owner != current_thread())
        panic("mutex \"%s\": unlocked by a thread that does not own it", m->lock.name);
    m->owner = NULL;
    lockdep_sleep_release(m);
    /* Wake a waiter while still holding m->lock. Once ownership is dropped
     * and m->lock released, the woken waiter can take the mutex and free the
     * object it guards, so touching m->wq afterwards is a use-after-free. The
     * lock order is the same one mutex_lock establishes ("mutex" then "mutex
     * waiters"), and the woken thread must re-take m->lock before it returns
     * from waitqueue_wait, so it cannot free the mutex under us. */
    mutex_pass_on(m);
    spin_unlock_irqrestore(&m->lock, f);
}
