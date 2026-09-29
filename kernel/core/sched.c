/* Scheduler. See sched.h for the model; the subtle parts:
 *
 * - A CPU's run queue lock is held ACROSS switch_context and released by
 *   the thread that runs next (finish_switch). So a thread that was just
 *   switched out cannot be stolen until its registers are fully saved.
 * - A thread can be woken between marking itself BLOCKED and actually
 *   switching out. The waker then queues it (possibly on another CPU)
 *   while its stack is still live. `on_cpu` stays true until the switch
 *   completes, and whoever picks the thread waits for it to drop.
 * - Preemption happens on the way out of an interrupt, never while a
 *   spinlock is held (preempt_count > 0).
 */
#include <jam/dbghook.h>
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/aspace.h>
#include <jam/uentry.h>
#include <jam/smp.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

#define STACK_SIZE  THREAD_STACK_SIZE
#define WATCHDOG_S  5

void switch_context(uint64_t *save_rsp, uint64_t load_rsp);
void thread_start(void);

struct runqueue {
    spinlock_t       lock;
    struct list_node queues[PRIO_MAX + 1];
    uint32_t         bitmap;      /* bit p set: queues[p] non-empty */
    volatile uint32_t nr_ready;
    struct thread   *idle;
    struct thread   *prev;        /* handed from schedule to finish_switch */
};

static struct runqueue rqs[MAX_CPUS];

/* Debug trace: each CPU's last few switches. */
#define TRACE_N 8
struct switch_event {
    struct thread *prev, *next;
    int prev_state;
    uint64_t tick;
};
static struct switch_event trace[MAX_CPUS][TRACE_N];
static uint32_t trace_pos[MAX_CPUS];

static void dump_trace(uint32_t cpu)
{
    kprintf("  cpu %u recent switches (oldest first):\n", cpu);
    for (uint32_t k = 0; k < TRACE_N; k++) {
        struct switch_event *e = &trace[cpu][(trace_pos[cpu] + k) % TRACE_N];
        if (!e->prev)
            continue;
        kprintf("    tick %lu: %s (state %d) -> %s\n", e->tick, e->prev->name, e->prev_state,
                e->next->name);
    }
}
static struct kmem_cache *thread_cache;
static volatile uint64_t next_id = 1;

/* Sleeping threads; CPU 0's tick wakes the ones that are due. */
static spinlock_t sleep_lock = SPINLOCK_INIT("sleepers");
static struct list_node sleepers = LIST_INIT(sleepers);

/* Stacks of exited threads. Up to stack_cache_limit are kept mapped and
 * reused (no TLB shootdown, no page allocation). Stacks over the limit must
 * be unmapped and freed (kstack_free), but reap() runs in finish_switch with
 * interrupts off, where the TLB shootdown that has to come first can't be
 * done. So they wait on `stack_doomed` (linked through each stack's lowest
 * word: still mapped, and nothing runs on it any more) until the next
 * sched_stack_trim, which thread creation and thread exit call. An exiting
 * thread trims the stacks of threads reaped before it, so during a burst of
 * exits the list holds only the last few (those reaped after the last exit
 * or creation); they are counted as cached pages meanwhile, so the leak
 * check stays exact. Freed stacks' virtual ranges are reused by kstack_alloc
 * (vmm.c). */
static spinlock_t stack_lock = SPINLOCK_INIT("stack cache");
static void *stack_cache[SCHED_STACK_CACHE_MAX];
static unsigned stack_cache_n, stack_cache_limit = SCHED_STACK_CACHE_MAX;
static void *stack_doomed;
static volatile unsigned stack_doomed_n;
static volatile uint64_t stacks_freed;

/* ---- preemption ---------------------------------------------------------- */

/* Single GS-relative instructions: see percpu.h for why. Once the count
 * is non-zero the thread cannot move, so this_cpu() is safe inside. */
void preempt_disable(void)
{
    percpu_preempt_inc();
}

void preempt_enable_no_resched(void)
{
    percpu_preempt_dec();
}

void preempt_enable(void)
{
    if (percpu_preempt_dec() != 0 || !irqs_enabled())
        return;
    preempt_check();
}

/* Take a pending reschedule if this is a safe point (preemptible, not in an
 * interrupt, interrupts on). Callers that release a lock with irqsave reach
 * here only after restoring interrupts: preempt_enable() runs while they are
 * still off (so its need_resched test is skipped and a thread woken on this
 * same CPU waits for the next tick), so spin_unlock_irqrestore calls this
 * again once interrupts are back on. The scheduler itself never routes
 * through here (it uses the no_resched unlock variants), so this cannot
 * recurse into schedule(). (C5) */
void preempt_check(void)
{
    if (!irqs_enabled())
        return;
    /* Look at need_resched with interrupts off so the CPU we test is the CPU
     * we act on. */
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    bool go = c->need_resched && c->irq_depth == 0 && c->preempt_count == 0 && c->current;
    irq_restore(f);
    if (go)
        schedule();
}

/* ---- run queue primitives (rq->lock held) --------------------------------- */

static void enqueue(struct runqueue *rq, struct thread *t, uint32_t cpu)
{
    if (t->rq_node.next)
        panic("sched: \"%s\" queued on cpu %u while already queued on cpu %u (state %d, on_cpu %d)",
              t->name, cpu, t->cpu, t->state, t->on_cpu);
    t->state = T_READY;
    t->cpu = cpu;
    t->ready_since = cpus[cpu]->ticks;
    list_add_tail(&rq->queues[t->prio], &t->rq_node);
    rq->bitmap |= 1u << t->prio;
    rq->nr_ready++;
}

static void dequeue(struct runqueue *rq, struct thread *t)
{
    list_del(&t->rq_node);
    if (list_empty(&rq->queues[t->prio]))
        rq->bitmap &= ~(1u << t->prio);
    rq->nr_ready--;
}

static struct thread *pick_best(struct runqueue *rq)
{
    if (!rq->bitmap)
        return NULL;
    unsigned p = 31 - __builtin_clz(rq->bitmap);
    struct thread *t = list_first(&rq->queues[p], struct thread, rq_node);
    dequeue(rq, t);
    return t;
}

/* Highest-priority ready thread on rq that may run on `cpu`. */
static struct thread *pick_stealable(struct runqueue *rq, uint32_t cpu)
{
    for (int p = PRIO_MAX; p >= 0; p--) {
        if (!(rq->bitmap & (1u << p)))
            continue;
        for (struct list_node *n = rq->queues[p].next; n != &rq->queues[p]; n = n->next) {
            struct thread *t = container_of(n, struct thread, rq_node);
            if (cpumask_has(&t->affinity, cpu)) {
                dequeue(rq, t);
                return t;
            }
        }
    }
    return NULL;
}

/* ---- placement ------------------------------------------------------------- */

static uint32_t load_of(uint32_t cpu)
{
    struct cpu *c = cpus[cpu];
    return rqs[cpu].nr_ready + (c->current && !c->current->is_idle ? 1 : 0);
}

/* Least-loaded allowed CPU; ties prefer P-cores, then the thread's last CPU. */
static uint32_t select_cpu(struct thread *t)
{
    uint32_t best = UINT32_MAX, best_load = UINT32_MAX;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (!cpus[i]->online || !cpumask_has(&t->affinity, i))
            continue;
        uint32_t l = load_of(i) * 4;
        if (cpus[i]->type == CORE_EFFICIENCY)
            l += 1;
        if (i == t->cpu)
            l = l ? l - 1 : 0;
        if (l < best_load) {
            best_load = l;
            best = i;
        }
    }
    if (best == UINT32_MAX)
        panic("sched: thread \"%s\" has no online CPU in its affinity mask", t->name);
    return best;
}

/* Wake-affine placement for a wakee whose waker, running on `waker`, is
 * about to block (thread_wake_sync). On the waker's CPU the wakee runs the
 * moment the waker blocks, with the data it was just sent still in that
 * CPU's cache; that is only right if nothing else is queued there (it would
 * wait behind it) and t may run there. Otherwise the waker's HT sibling, if
 * it is idle, shares the core's caches. Otherwise the usual choice. All the
 * loads are racy, like select_cpu's: a wrong guess costs time, never
 * correctness, since thread_wake queues t under the chosen CPU's lock. */
static uint32_t select_cpu_affine(struct thread *t, uint32_t waker)
{
    if (cpus[waker]->online && cpumask_has(&t->affinity, waker) && !rqs[waker].nr_ready) {
        t->affine_wakes++;   /* we own t's placement: we moved it to READY */
        return waker;
    }
    uint32_t core = cpus[waker]->core_id;
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu *c = cpus[i];
        if (i == waker || c->core_id != core || !c->online || !cpumask_has(&t->affinity, i))
            continue;
        struct thread *cur = c->current;
        if (!rqs[i].nr_ready && (!cur || cur->is_idle)) {
            t->affine_wakes++;
            return i;
        }
    }
    return select_cpu(t);
}

void sched_kick(uint32_t cpu)
{
    struct cpu *c = cpus[cpu];
    c->need_resched = true;
    preempt_disable();
    bool remote = c != this_cpu();
    preempt_enable_no_resched();
    if (remote)
        ipi_send(cpu, VEC_RESCHEDULE);
}

/* ---- the switch ------------------------------------------------------------ */

static void reap(struct thread *t);

/* First thing on the far side of every switch_context. */
static void finish_switch(void)
{
    struct cpu *c = this_cpu();
    struct runqueue *rq = &rqs[c->index];
    struct thread *prev = rq->prev;
    if (!prev) {
        lockdep_off();
        dump_trace(c->index);
        for (uint32_t i = 0; i < cpu_count; i++)
            if (i != c->index && cpus[i]->current == c->current)
                kprintf("  !! cpu %u also has \"%s\" as current\n", i, c->current->name);
        panic("sched: finish_switch on cpu %u with no previous thread (now running \"%s\", "
              "cpu field %u, on_cpu %d, state %d)", c->index, c->current->name,
              c->current->cpu, c->current->on_cpu, c->current->state);
    }
    rq->prev = NULL;
    /* Read prev->state BEFORE the on_cpu release store. While on_cpu is set
     * and this rq lock is held nobody else can pick prev, so its state is
     * stable; once on_cpu clears, prev can be woken, run, exit and be reaped
     * on another CPU, so a later read here could see a recycled state and
     * reap it a second time. (C2) */
    int prev_state = prev->state;
    /* Clear on_cpu BEFORE dropping the lock: a waker holding this lock and
     * seeing on_cpu set then knows prev has not yet reached schedule(). */
    __atomic_store_n(&prev->on_cpu, false, __ATOMIC_RELEASE);
    spin_unlock_no_resched(&rq->lock);
    DBG_HOOK(DBG_FINISH_SWITCH, prev);

    if (prev_state == T_DEAD)
        reap(prev);
    else if (prev_state == T_MIGRATING)
        thread_wake(prev);
}

void schedule(void)
{
    /* Interrupts off FIRST: until then this thread may migrate, and the
     * CPU pointer would be stale. */
    uint64_t flags = irq_save();
    struct cpu *c = this_cpu();
    if (c->preempt_count) {
        lockdep_print_held();
        panic("sched: schedule() with preemption disabled (preempt_count %u)",
              c->preempt_count);
    }
    struct runqueue *rq = &rqs[c->index];
    spin_lock(&rq->lock);
    c->need_resched = false;

    struct thread *prev = c->current;
    prev->prio = prev->base_prio;   /* a boost lasts one turn on the CPU */
    if (prev->state == T_RUNNING && !prev->is_idle) {
        if (cpumask_has(&prev->affinity, c->index))
            enqueue(rq, prev, c->index);
        else
            prev->state = T_MIGRATING;   /* moved in finish_switch */
    }
    /* T_BLOCKED / T_DEAD: not queued. T_READY: a waker already queued it. */
    DBG_HOOK(DBG_SCHED_PREV, prev);

    struct thread *next = pick_best(rq);
    if (!next)
        next = rq->idle;
    if (next == prev) {
        prev->state = T_RUNNING;
        prev->slice = SLICE_TICKS;   /* refresh: a thread that used its slice
                                      * while alone must be sliced again once
                                      * a same-priority peer is queued (C4) */
        spin_unlock_no_resched(&rq->lock);
        irq_restore(flags);
        return;
    }
    while (__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE))
        cpu_relax();   /* still being switched out on another CPU */

    if (next->state != T_READY && !next->is_idle)
        panic("sched: picked \"%s\" in state %d on cpu %u", next->name, next->state, c->index);
    next->state = T_RUNNING;
    next->on_cpu = true;
    next->cpu = c->index;
    next->slice = SLICE_TICKS;
    next->switches_in++;
    c->current = next;
    c->switches++;
    rq->prev = prev;
    trace[c->index][trace_pos[c->index]++ % TRACE_N] =
        (struct switch_event){ prev, next, prev->state, c->ticks };
    arch_thread_switch(prev, next);   /* kernel stack, FPU, address space (M5) */
    switch_context(&prev->rsp, next->rsp);

    /* Back on prev's stack, possibly much later and on another CPU. */
    finish_switch();
    irq_restore(flags);
}

/* ---- waking and placing ------------------------------------------------------ */

/* The CPU a wake from here may be placed on under wake-affine rules, or -1.
 * `consume`: this is a plain thread_wake, which honours the current
 * thread's wake_sync flag and clears it (one wakee per flag). Never from an
 * interrupt handler: the interrupted thread's flag is not about this wake. */
static int affine_hint(bool consume)
{
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    struct thread *me = c->current;
    int cpu = -1;
    if (!c->irq_depth && me && !me->is_idle && (!consume || me->wake_sync)) {
        me->wake_sync = false;
        cpu = (int)c->index;
    }
    irq_restore(f);
    return cpu;
}

static void thread_wake_common(struct thread *t, bool sync);

void thread_wake(struct thread *t)
{
    thread_wake_common(t, false);
}

void thread_wake_sync(struct thread *t)
{
    thread_wake_common(t, true);
}

void thread_set_wake_sync(bool on)
{
    current_thread()->wake_sync = on;   /* only ever written by its own thread */
}

static void thread_wake_common(struct thread *t, bool sync)
{
    int s = t->state;
    if (s != T_BLOCKED && s != T_MIGRATING)
        return;

    /* Woken before it finished switching out (e.g. a 0 ms sleep hit by the
     * timer on its own CPU): don't queue it anywhere, just let it keep
     * running. Waiting for it to switch out instead can deadlock when the
     * waker interrupted that very thread. Its CPU's run queue lock orders
     * this against schedule(), which holds that lock from reading the state
     * until on_cpu clears. */
    if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
        DBG_HOOK(DBG_WAKE_ONCPU, t);
        /* t->cpu can change under us if t migrates while still switching out
         * (it is woken and re-run elsewhere before its old CPU cleared
         * on_cpu). Lock the CPU t->cpu names, then re-read t->cpu under that
         * lock; if it moved, drop the lock and retry with its new CPU. This
         * is the task_rq_lock pattern: marking T_RUNNING under the wrong run
         * queue's lock races schedule() on the real CPU and loses the thread
         * (it ends RUNNING on no CPU and no queue). (C1) */
        for (;;) {
            uint32_t c = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
            struct runqueue *own = &rqs[c];
            uint64_t f = spin_lock_irqsave(&own->lock);
            if (__atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE) != c) {
                spin_unlock_irqrestore(&own->lock, f);
                continue;   /* migrated: relock its current run queue */
            }
            /* A CAS, not a check-then-store: a cancellable wait moves its own
             * thread BLOCKED -> RUNNING without this lock (block_prepared),
             * and could then run on and exit; a plain store here would turn
             * its T_DEAD back into T_RUNNING. */
            int blocked = T_BLOCKED;
            bool still_here = t->on_cpu &&
                              __atomic_compare_exchange_n(&t->state, &blocked, T_RUNNING, false,
                                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
            spin_unlock_irqrestore(&own->lock, f);
            if (still_here)
                return;
            break;
        }
        s = t->state;   /* it switched out meanwhile: wake it normally */
        if (s != T_BLOCKED && s != T_MIGRATING)
            return;
    }
    /* Only a BLOCKED thread is placed wake-affine: a MIGRATING one is
     * leaving the waker's CPU because its mask forbids it. The hint is
     * looked at (and a wake_sync flag used up) only once we know this wake
     * places t; a wake that finds t still on its CPU or already woken above
     * leaves the flag for the wake it was meant for. */
    bool was_blocked = s == T_BLOCKED;
    if (!__atomic_compare_exchange_n(&t->state, &s, T_READY, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED))
        return;   /* someone else woke it */

    int hint = was_blocked ? affine_hint(!sync) : -1;
    uint32_t cpu = hint >= 0 ? select_cpu_affine(t, (uint32_t)hint) : select_cpu(t);
    struct runqueue *rq = &rqs[cpu];
    uint64_t f = spin_lock_irqsave(&rq->lock);
    enqueue(rq, t, cpu);
    struct thread *cur = cpus[cpu]->current;
    bool kick = !cur || cur->is_idle || t->prio > cur->prio;
    spin_unlock_irqrestore(&rq->lock, f);
    if (kick)
        sched_kick(cpu);
}

/* ---- thread lifecycle ------------------------------------------------------ */

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
        stack_doomed_n = 0;
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
static void *stack_get(void)
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

/* From reap, with interrupts off: cache the stack, or queue it for freeing. */
static void stack_put(void *top)
{
    uint64_t f = spin_lock_irqsave(&stack_lock);
    if (stack_cache_n < stack_cache_limit) {
        stack_cache[stack_cache_n++] = top;
    } else {
        *(void **)((char *)top - STACK_SIZE) = stack_doomed;
        stack_doomed = top;
        stack_doomed_n++;
    }
    spin_unlock_irqrestore(&stack_lock, f);
}

static void thread_put(struct thread *t)
{
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kmem_cache_free(thread_cache, t);
}

static void reap(struct thread *t)
{
    /* A user thread normally drops its address space itself on the way out
     * (uthread_exit_current); this covers any that didn't. Only now, after
     * its last switch, is the address space surely not loaded for it. */
    if (t->aspace) {
        aspace_unref(t->aspace);
        t->aspace = NULL;
    }
    fpu_ustate_free(t);   /* switched out for good: nothing saves into it now */
    stack_put(t->stack_top);
    thread_put(t);   /* the thread's reference to itself */
}

static struct thread *thread_alloc(const char *name, int prio)
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

struct thread *thread_try_create_capped(const char *name, void (*fn)(void *), void *arg,
                                        int prio, const cpumask_t *mask, int prio_cap)
{
    struct thread *t = thread_alloc(name, prio);
    if (!t)
        return NULL;
    t->stack_top = stack_get();
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

    t->state = T_BLOCKED;
    t->cpu = percpu_index();   /* placement hint only: "last ran here" */
    thread_wake(t);
    return t;
}

_Noreturn void thread_exit(void)
{
    struct thread *t = current_thread();
    uint64_t f = spin_lock_irqsave(&t->exit_wq.lock);
    t->exited = true;
    spin_unlock_irqrestore(&t->exit_wq.lock, f);
    waitqueue_wake_all(&t->exit_wq);
    sched_stack_trim();   /* stacks of threads reaped before us (see above) */
    irq_disable();
    t->state = T_DEAD;
    schedule();
    panic("sched: dead thread \"%s\" was scheduled", t->name);
}

void thread_join(struct thread *t)
{
    uint64_t f = spin_lock_irqsave(&t->exit_wq.lock);
    while (!t->exited)
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
    schedule();
}

void thread_set_priority(struct thread *t, int prio)
{
    /* Takes effect the next time t is queued or switched out. Never touch
     * t->prio here: while t is queued it names t's run queue list. */
    int cap = t->prio_cap;
    t->base_prio = prio < PRIO_MIN ? PRIO_MIN : prio > cap ? cap : prio;
}

void thread_set_priority_cap(struct thread *t, int cap)
{
    cap = cap < PRIO_MIN ? PRIO_MIN : cap > PRIO_MAX ? PRIO_MAX : cap;
    t->prio_cap = cap;
    if (t->base_prio > cap)
        t->base_prio = cap;
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

/* The current thread is already T_BLOCKED (set while the lock that guards
 * its wake condition was held, so no waker can slip in unnoticed) AND with
 * preemption disabled by the caller: a preemption between marking itself
 * blocked and arming the deadline would switch it out with nothing left to
 * wake it (found by the VMO agent: "sleeper made no progress" in stress).
 * Arm the deadline, drop `lock`, re-enable preemption, switch out, and on
 * return re-take `lock`. */
static bool cancel_seen(struct thread *t)
{
    return __atomic_load_n(&t->cancel_pending, __ATOMIC_ACQUIRE);
}

/* Returns true if a cancellable wait was cancelled (before or during). */
static bool block_prepared(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns,
                           bool cancellable)
{
    struct thread *t = current_thread();
    if (deadline_ns != DEADLINE_NEVER) {
        uint64_t f = spin_lock_irqsave(&sleep_lock);
        t->wake_at_ns = deadline_ns;
        list_add_tail(&sleepers, &t->sleep_node);
        spin_unlock_irqrestore(&sleep_lock, f);
    }
    if (lock)
        spin_unlock_irqrestore(lock, *irqflags);
    /* Cancellation: our T_BLOCKED store and thread_cancel's flag store are
     * each followed by a read of the other (the flag here, our state in its
     * thread_wake), with full fences between, so at least one side sees the
     * other: either we see the flag and don't sleep, or it sees us blocked
     * and wakes us. We may only skip the switch by moving BLOCKED -> RUNNING
     * ourselves: if a waker already made us READY it has also queued us, and
     * then we must go through schedule() like any thread woken before it
     * switched out (overwriting READY put a running thread on a run queue;
     * cancel_races_wakeup caught it as "dead thread was scheduled"). */
    bool skip = false;
    if (cancellable) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (cancel_seen(t)) {
            int expect = T_BLOCKED;
            skip = __atomic_compare_exchange_n(&t->state, &expect, T_RUNNING, false,
                                               __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        }
    }
    /* Registered for wakeup (wait queue / deadline): being preempted from
     * here on is harmless, so preemption can come back on. */
    preempt_enable_no_resched();
    if (!skip)
        schedule();
    /* Always take sleep_lock before touching sleep_node when we may have
     * been on the sleepers list: wake_sleepers deletes the node and calls
     * thread_wake(t) while holding sleep_lock, so a lockless check here
     * could let this thread return (and re-block or exit, freeing itself)
     * while the waker is still inside thread_wake(t). Serialising on
     * sleep_lock keeps t alive until the waker is done. (C6) */
    if (deadline_ns != DEADLINE_NEVER) {
        uint64_t f = spin_lock_irqsave(&sleep_lock);
        if (t->sleep_node.next)
            list_del(&t->sleep_node);
        spin_unlock_irqrestore(&sleep_lock, f);
    }
    if (lock)
        *irqflags = spin_lock_irqsave(lock);
    return cancellable && cancel_seen(t);
}

void thread_block(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns)
{
    preempt_disable();   /* re-enabled in block_prepared */
    current_thread()->state = T_BLOCKED;
    block_prepared(lock, irqflags, deadline_ns, false);
}

status_t thread_block_cancellable(spinlock_t *lock, uint64_t *irqflags, uint64_t deadline_ns)
{
    preempt_disable();   /* re-enabled in block_prepared */
    current_thread()->state = T_BLOCKED;
    return block_prepared(lock, irqflags, deadline_ns, true) ? ERR_CANCELED : OK;
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

static void wake_sleepers(void)
{
    uint64_t now = uptime_ns();
    spin_lock(&sleep_lock);   /* in the timer interrupt: IRQs already off */
    for (struct list_node *n = sleepers.next; n != &sleepers;) {
        struct thread *t = container_of(n, struct thread, sleep_node);
        n = n->next;
        if (t->wake_at_ns <= now) {
            list_del(&t->sleep_node);
            thread_wake(t);
        }
    }
    spin_unlock(&sleep_lock);
}

/* ---- idle, work stealing ---------------------------------------------------- */

/* With interrupts off: move one waiting thread from a busy CPU to us. Two
 * run queue locks are always taken lower CPU index first. */
static void try_steal(uint32_t me)
{
    for (uint32_t k = 1; k < cpu_count; k++) {
        uint32_t v = (me + k) % cpu_count;
        struct cpu *vc = cpus[v];
        if (!vc->online || !rqs[v].nr_ready || !vc->current || vc->current->is_idle)
            continue;
        struct runqueue *a = &rqs[me < v ? me : v], *b = &rqs[me < v ? v : me];
        spin_lock(&a->lock);
        spin_lock_nested(&b->lock, 1);
        struct thread *t = pick_stealable(&rqs[v], me);
        if (t) {
            enqueue(&rqs[me], t, me);
            this_cpu()->steals++;
        }
        spin_unlock_no_resched(&b->lock);
        spin_unlock_no_resched(&a->lock);
        if (t)
            return;
    }
}

_Noreturn static void idle_loop(void)
{
    struct cpu *c = this_cpu();
    for (;;) {
        irq_disable();
        if (!rqs[c->index].nr_ready)
            try_steal(c->index);
        if (rqs[c->index].nr_ready || c->need_resched) {
            irq_enable();
            schedule();
            continue;
        }
        __asm__ volatile("sti; hlt" ::: "memory");   /* sti's shadow covers hlt */
    }
}

static void idle_entry(void *arg)
{
    (void)arg;
    idle_loop();
}

static void init_rq(uint32_t cpu)
{
    struct runqueue *rq = &rqs[cpu];
    spin_init(&rq->lock, "runqueue");
    for (int p = 0; p <= PRIO_MAX; p++)
        list_init(&rq->queues[p]);
}

static struct thread *make_idle(uint32_t cpu)
{
    char name[24];
    ksnprintf(name, sizeof(name), "idle/%u", cpu);
    struct thread *t = thread_alloc(name, PRIO_MIN);
    if (!t)
        panic("sched: out of memory for the idle thread");
    t->is_idle = true;
    t->state = T_RUNNING;
    cpumask_one(&t->affinity, cpu);
    t->cpu = cpu;
    return t;
}

void sched_init_bsp(void)
{
    thread_cache = kmem_cache_create("thread", sizeof(struct thread), 64);
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        init_rq(i);

    /* The code running now becomes thread "main". */
    struct thread *main = thread_alloc("main", PRIO_DEFAULT);
    if (!main)
        panic("sched: out of memory for thread main");
    main->state = T_RUNNING;
    main->on_cpu = true;
    main->cpu = 0;
    main->refs = 2;   /* never joined; keep it alive */
    this_cpu()->current = main;

    /* BSP idle thread: a real thread with its own stack, first run when
     * main blocks. */
    struct thread *idle = make_idle(0);
    idle->state = T_READY;
    idle->stack_top = stack_get();
    if (!idle->stack_top)
        panic("sched: out of memory for the idle stack");
    uint64_t *sp = (uint64_t *)idle->stack_top;
    *--sp = (uint64_t)thread_start;
    *--sp = 0; *--sp = 0;
    *--sp = (uint64_t)idle_entry; *--sp = 0;
    *--sp = 0; *--sp = 0;
    idle->rsp = (uint64_t)sp;
    rqs[0].idle = idle;
}

_Noreturn void sched_run_ap_idle(void)
{
    struct cpu *c = this_cpu();
    struct thread *idle = make_idle(c->index);
    idle->on_cpu = true;
    idle->stack_top = c->kstack_top;
    rqs[c->index].idle = idle;
    c->current = idle;
    idle_loop();
}

/* ---- tick, watchdog, irq exit ----------------------------------------------- */

static void watchdog_check(struct cpu *c)
{
    /* Once a second, see whether the next CPU's tick count moved. */
    if (c->ticks % TICK_HZ || cpu_count < 2)
        return;
    struct cpu *w = cpus[(c->index + 1) % cpu_count];
    if (!w->online)
        return;
    if (w->ticks != c->wd_seen_ticks) {
        c->wd_seen_ticks = w->ticks;
        c->wd_stale_seconds = 0;
    } else if (++c->wd_stale_seconds == WATCHDOG_S) {
        watchdog_fire(w->index);
    }
}

static volatile uint64_t boost_total;

uint64_t sched_boost_count(void)
{
    return boost_total;
}

/* Once a second: boost threads that have waited too long on this CPU. */
static void boost_starved(struct cpu *c)
{
    struct runqueue *rq = &rqs[c->index];
    if (c->ticks % TICK_HZ || !rq->nr_ready)
        return;
    spin_lock(&rq->lock);   /* timer interrupt: IRQs already off */
    for (int p = PRIO_BOOST - 1; p >= 0; p--) {
        struct list_node *n = rq->queues[p].next;
        while (n != &rq->queues[p]) {
            struct thread *t = container_of(n, struct thread, rq_node);
            n = n->next;
            if (c->ticks - t->ready_since < STARVE_TICKS)
                continue;
            dequeue(rq, t);
            t->prio = PRIO_BOOST;
            t->boosts++;
            boost_total++;
            list_add_tail(&rq->queues[PRIO_BOOST], &t->rq_node);
            rq->bitmap |= 1u << PRIO_BOOST;
            rq->nr_ready++;
        }
    }
    struct thread *cur = c->current;
    if (cur && rq->bitmap && (31 - __builtin_clz(rq->bitmap)) > cur->prio)
        c->need_resched = true;
    spin_unlock_no_resched(&rq->lock);
}

void sched_tick(void)
{
    struct cpu *c = this_cpu();
    if (c->index == 0)
        wake_sleepers();
    watchdog_check(c);
    boost_starved(c);

    struct thread *t = c->current;
    if (!t)
        return;
    if (t->is_idle) {
        if (rqs[c->index].nr_ready)
            c->need_resched = true;
    } else if (t->slice && --t->slice == 0) {
        c->need_resched = true;
    }
}

void sched_irq_exit(uint64_t interrupted_rflags)
{
    struct cpu *c = this_cpu();
    if (c->need_resched && c->preempt_count == 0 && c->irq_depth == 0 &&
        (interrupted_rflags & (1u << 9)) && c->current)
        schedule();
}

void sched_print_stats(void)
{
    uint64_t sw = 0, st = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        sw += cpus[i]->switches;
        st += cpus[i]->steals;
    }
    kprintf("sched: %lu context switches, %lu steals, %lu starvation boosts across %u CPUs\n",
            sw, st, boost_total, cpu_count);
}

/* ---- wait queues and mutexes --------------------------------------------- */

void waitqueue_init(struct waitqueue *wq, const char *name)
{
    spin_init(&wq->lock, name);
    list_init(&wq->waiters);
}

static bool wq_wait(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags,
                    uint64_t deadline_ns, bool cancellable)
{
    struct thread *t = current_thread();
    bool same = lock == &wq->lock;
    uint64_t f = 0;
    preempt_disable();   /* re-enabled in block_prepared */
    if (!same)
        f = spin_lock_irqsave(&wq->lock);
    list_add_tail(&wq->waiters, &t->wait_node);
    t->state = T_BLOCKED;   /* before the wq lock drops: wakers pop under it */
    if (!same)
        spin_unlock_irqrestore(&wq->lock, f);
    bool cancelled = block_prepared(lock, irqflags, deadline_ns, cancellable);

    /* Take ourselves off the wait queue if we are still on it (timed out or
     * spurious). Always hold wq->lock while touching wait_node: wake() pops
     * the node and calls thread_wake(t) under wq->lock, so a lockless read
     * could let this thread return and free itself mid-wake. When same, the
     * caller's lock (already re-held by block_prepared) is wq->lock, so we
     * are serialised; otherwise take wq->lock explicitly. (C6) */
    if (!same) {
        uint64_t g = spin_lock_irqsave(&wq->lock);
        if (t->wait_node.next)
            list_del(&t->wait_node);
        spin_unlock_irqrestore(&wq->lock, g);
    } else if (t->wait_node.next) {
        list_del(&t->wait_node);
    }
    return cancelled;
}

void waitqueue_wait_until(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags,
                          uint64_t deadline_ns)
{
    wq_wait(wq, lock, irqflags, deadline_ns, false);
}

void waitqueue_wait(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags)
{
    wq_wait(wq, lock, irqflags, DEADLINE_NEVER, false);
}

status_t waitqueue_wait_cancellable(struct waitqueue *wq, spinlock_t *lock, uint64_t *irqflags,
                                    uint64_t deadline_ns)
{
    return wq_wait(wq, lock, irqflags, deadline_ns, true) ? ERR_CANCELED : OK;
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
    while (m->owner)
        waitqueue_wait(&m->wq, &m->lock, &f);
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
    while (m->owner) {
        if (waitqueue_wait_cancellable(&m->wq, &m->lock, &f, DEADLINE_NEVER) != OK) {
            /* mutex_unlock may have picked us as the one waiter to wake:
             * pass that on so the next waiter doesn't sleep through it. */
            if (!m->owner)
                waitqueue_wake_one(&m->wq);
            spin_unlock_irqrestore(&m->lock, f);
            lockdep_sleep_release(m);
            return ERR_CANCELED;
        }
    }
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
     * from waitqueue_wait, so it cannot free the mutex under us. (C9) */
    waitqueue_wake_one(&m->wq);
    spin_unlock_irqrestore(&m->lock, f);
}
