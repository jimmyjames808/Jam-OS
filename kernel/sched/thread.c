/* Thread lifecycle: creation, exit, join, priority and affinity, and the
 * cache of kernel stacks that thread creation takes from and reaping
 * gives back to. The scheduler proper (run queues, the switch, waking) is
 * sched.c; finish_switch there calls thread_reap for a thread that died. */
#include <jam/aspace.h>
#include <jam/atomic.h>
#include <jam/irq.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pathstat.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/uentry.h>
#include <jam/x86.h>

#include "sched_internal.h"

#define STACK_SIZE  THREAD_STACK_SIZE

static struct kmem_cache *thread_cache;
static uint64_t next_id = 1;

/* Stacks of exited threads. Up to stack_cache_limit are kept mapped and
 * reused (no TLB shootdown, no page allocation). Stacks over the limit must
 * be unmapped and freed (kstack_free), but thread_reap runs in finish_switch
 * with interrupts off, where the TLB shootdown that has to come first can't
 * be done. So they wait on `stack_doomed` (linked through each stack's
 * lowest word: still mapped, and nothing runs on it any more) until the
 * next sched_stack_trim, which thread creation and thread exit call. An
 * exiting thread trims the stacks of threads reaped before it, so during a
 * burst of exits the list holds only the last few (those reaped after the
 * last exit or creation); they are counted as cached pages meanwhile, so
 * the leak check stays exact. Freed stacks' virtual ranges are reused by
 * kstack_alloc (vmm.c). */
static spinlock_t stack_lock = SPINLOCK_INIT("stack cache");
static void *stack_cache[SCHED_STACK_CACHE_MAX];
static unsigned stack_cache_n, stack_cache_limit = SCHED_STACK_CACHE_MAX;
static void *stack_doomed;
static unsigned stack_doomed_n;   /* stacks on stack_doomed; stack_lock (stored atomically:
                                     sched_stack_trim reads it without) */
static uint64_t stacks_freed;

/* May this context free stacks (kstack_free shoots down TLBs, which needs
 * interrupts on and no spinlock held: see check_callable in ipi.c)? */
static bool can_trim(void)
{
    if (!irqs_enabled())
        return false;
    preempt_disable();
    struct cpu *c = this_cpu();
    bool ok = !c->irq_depth && !c->held_depth;
    preempt_enable_no_resched();
    return ok;
}

/* Take the doomed list (and, over `limit`, the excess cached stacks too)
 * under the lock, free them outside it. */
static void trim_to(unsigned limit)
{
    if (!can_trim())
        return;
    void *list = NULL;
    uint64_t f = spin_lock_irqsave(&stack_lock);
    while (stack_cache_n > limit) {
        void *top = stack_cache[--stack_cache_n];
        *(void **)((char *)top - STACK_SIZE) = list;
        list = top;
    }
    if (stack_doomed) {
        void **tail = &list;
        while (*tail)
            tail = (void **)((char *)*tail - STACK_SIZE);
        *tail = stack_doomed;
        stack_doomed = NULL;
        __atomic_store_n(&stack_doomed_n, 0, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&stack_lock, f);
    while (list) {
        void *next = *(void **)((char *)list - STACK_SIZE);
        kstack_free(list, STACK_SIZE);
        __atomic_add_fetch(&stacks_freed, 1, __ATOMIC_RELAXED);
        list = next;
    }
}

void sched_stack_trim(void)
{
    if (__atomic_load_n(&stack_doomed_n, __ATOMIC_RELAXED))
        trim_to(SCHED_STACK_CACHE_MAX);
}

unsigned sched_stack_cache_set_limit(unsigned limit)
{
    if (limit > SCHED_STACK_CACHE_MAX)
        limit = SCHED_STACK_CACHE_MAX;
    uint64_t f = spin_lock_irqsave(&stack_lock);
    unsigned old = stack_cache_limit;
    stack_cache_limit = limit;
    spin_unlock_irqrestore(&stack_lock, f);
    trim_to(limit);
    return old;
}

uint64_t sched_stacks_freed(void)
{
    return __atomic_load_n(&stacks_freed, __ATOMIC_RELAXED);
}

/* NULL when out of memory. */
void *thread_stack_get(void)
{
    sched_stack_trim();
    uint64_t f = spin_lock_irqsave(&stack_lock);
    void *s = stack_cache_n ? stack_cache[--stack_cache_n] : NULL;
    spin_unlock_irqrestore(&stack_lock, f);
    return s ? s : kstack_alloc_try(STACK_SIZE);
}

/* Pages held by cached thread stacks, and by stacks waiting to be freed.
 * Tests use this so stacks parked here aren't mistaken for leaks. */
uint64_t sched_stack_cache_pages(void)
{
    uint64_t f = spin_lock_irqsave(&stack_lock);
    uint64_t n = (uint64_t)(stack_cache_n + stack_doomed_n) * (STACK_SIZE / PAGE_SIZE);
    spin_unlock_irqrestore(&stack_lock, f);
    return n;
}

/* From thread_reap, with interrupts off: cache the stack, or queue it for
 * freeing. */
static void stack_put(void *top)
{
    uint64_t f = spin_lock_irqsave(&stack_lock);
    if (stack_cache_n < stack_cache_limit) {
        stack_cache[stack_cache_n++] = top;
    } else {
        *(void **)((char *)top - STACK_SIZE) = stack_doomed;
        stack_doomed = top;
        COUNTER_ADD(&stack_doomed_n, 1);
    }
    spin_unlock_irqrestore(&stack_lock, f);
}

static void thread_put(struct thread *t)
{
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kmem_cache_free(thread_cache, t);
}

void thread_reap(struct thread *t)
{
    /* A user thread normally drops its address space itself on the way out
     * (uthread_exit_current); this covers any that didn't. Only now, after
     * its last switch, is the address space surely not loaded for it. */
    if (t->aspace) {
        aspace_unref(t->aspace);
        t->aspace = NULL;
    }
    fpu_ustate_free(t);   /* switched out for good: nothing saves into it now */
    kfree(t->msg_slot);   /* the channels' message slot, if it had one */
    t->msg_slot = NULL;
    stack_put(t->stack_top);
    thread_put(t);   /* the thread's reference to itself */
}

void thread_cache_init(void)
{
    thread_cache = kmem_cache_create("thread", sizeof(struct thread), 64);
}

struct thread *thread_alloc(const char *name, int prio)
{
    struct thread *t = kmem_cache_alloc(thread_cache);
    if (!t)
        return NULL;   /* boot callers panic; thread_try_create_on reports it */
    memset(t, 0, sizeof(*t));
    t->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
    size_t n = strlen(name);
    if (n >= sizeof(t->name))
        n = sizeof(t->name) - 1;
    memcpy(t->name, name, n);
    t->prio = prio < PRIO_MIN ? PRIO_MIN : prio > PRIO_MAX ? PRIO_MAX : prio;
    t->base_prio = t->prio;
    t->prio_cap = PRIO_MAX;
    t->refs = 1;
    cpumask_all(&t->affinity);
    waitqueue_init(&t->exit_wq, "thread exit");
    return t;
}

/* C side of thread_start, running on the new thread's stack. */
_Noreturn void thread_entry(void (*fn)(void *), void *arg)
{
    finish_switch();
    irq_enable();
    fn(arg);
    thread_exit();
}

/* The fallible form every other create goes through: NULL when out of
 * memory, else the thread, already runnable. */
static struct thread *thread_try_create_capped(const char *name, void (*fn)(void *),
                                               void *arg, int prio, const cpumask_t *mask,
                                               int prio_cap)
{
    struct thread *t = thread_try_create_suspended(name, fn, arg, prio, mask, prio_cap);
    if (t)
        thread_wake(t);
    return t;
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg, int prio)
{
    return thread_create_on(name, fn, arg, prio, NULL);
}

struct thread *thread_create_on(const char *name, void (*fn)(void *), void *arg, int prio,
                                const cpumask_t *mask)
{
    struct thread *t = thread_try_create_capped(name, fn, arg, prio, mask, PRIO_MAX);
    if (!t)
        panic("sched: out of memory for thread \"%s\"", name);
    return t;
}

struct thread *thread_create_capped(const char *name, void (*fn)(void *), void *arg, int prio,
                                    const cpumask_t *mask, int prio_cap)
{
    struct thread *t = thread_try_create_capped(name, fn, arg, prio, mask, prio_cap);
    if (!t)
        panic("sched: out of memory for thread \"%s\"", name);
    return t;
}

struct thread *thread_try_create_on(const char *name, void (*fn)(void *), void *arg, int prio,
                                    const cpumask_t *mask)
{
    return thread_try_create_capped(name, fn, arg, prio, mask, PRIO_MAX);
}

struct thread *thread_try_create_suspended(const char *name, void (*fn)(void *), void *arg,
                                           int prio, const cpumask_t *mask, int prio_cap)
{
    struct thread *t = thread_alloc(name, prio);
    if (!t)
        return NULL;
    t->stack_top = thread_stack_get();
    if (!t->stack_top) {
        kmem_cache_free(thread_cache, t);
        return NULL;
    }
    if (mask)
        t->affinity = *mask;
    thread_set_priority_cap(t, prio_cap);   /* before it can first run */
    t->prio = t->base_prio;
    t->refs = 2;   /* the caller's, and the thread's own (dropped by reap) */

    /* Frame for switch_context to pop: r15 r14 r13 r12 rbx rbp, ret. */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = (uint64_t)thread_start;
    *--sp = 0;                /* rbp */
    *--sp = 0;                /* rbx */
    *--sp = (uint64_t)fn;     /* r12 */
    *--sp = (uint64_t)arg;    /* r13 */
    *--sp = 0;                /* r14 */
    *--sp = 0;                /* r15 */
    t->rsp = (uint64_t)sp;

    thread_set_state(t, T_BLOCKED);
    thread_set_cpu(t, percpu_index());   /* placement hint only: "last ran here" */
    return t;
}

_Noreturn void thread_exit(void)
{
    struct thread *t = current_thread();
    uint64_t f = spin_lock_irqsave(&t->exit_wq.lock);
    __atomic_store_n(&t->exited, true, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&t->exit_wq.lock, f);
    waitqueue_wake_all(&t->exit_wq);
    sched_stack_trim();   /* stacks of threads reaped before us (see above) */
    irq_disable();
    thread_set_state(t, T_DEAD);
    schedule();
    panic("sched: dead thread \"%s\" was scheduled", t->name);
}

void thread_join(struct thread *t)
{
    uint64_t f = spin_lock_irqsave(&t->exit_wq.lock);
    while (!__atomic_load_n(&t->exited, __ATOMIC_ACQUIRE))
        waitqueue_wait(&t->exit_wq, &t->exit_wq.lock, &f);
    spin_unlock_irqrestore(&t->exit_wq.lock, f);
    thread_put(t);
}

void thread_detach(struct thread *t)
{
    thread_put(t);
}

void thread_yield(void)
{
    PATH_MARK(PATH_MK_YIELD);
    schedule();
}

void thread_set_priority(struct thread *t, int prio)
{
    /* Takes effect the next time t is queued or switched out. Never touch
     * t->prio here: while t is queued it names t's run queue list. */
    int cap = t->prio_cap;   /* fixed once the thread can run */
    int base = prio < PRIO_MIN ? PRIO_MIN : prio > cap ? cap : prio;
    /* One atomic store of the final value: schedule() reads base_prio on
     * another CPU under a lock this caller doesn't hold, and must never see
     * an unclamped or half-made value (it indexes the run queues with it). */
    __atomic_store_n(&t->base_prio, base, __ATOMIC_RELAXED);
}

void thread_set_priority_cap(struct thread *t, int cap)
{
    cap = cap < PRIO_MIN ? PRIO_MIN : cap > PRIO_MAX ? PRIO_MAX : cap;
    t->prio_cap = cap;
    if (__atomic_load_n(&t->base_prio, __ATOMIC_RELAXED) > cap)
        __atomic_store_n(&t->base_prio, cap, __ATOMIC_RELAXED);
}

void thread_set_affinity(struct thread *t, const cpumask_t *mask)
{
    t->affinity = *mask;
    if (t != current_thread())
        return;
    /* Any migration from here on already honours the new mask. */
    preempt_disable();
    bool must_move = !cpumask_has(mask, this_cpu()->index);
    preempt_enable_no_resched();
    if (must_move)
        schedule();   /* switches out as T_MIGRATING, resumes on an allowed CPU */
}
