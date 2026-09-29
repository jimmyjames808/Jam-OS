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
#include <jam/cmdline.h>
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

#define STACK_SIZE  (64 * 1024)
#define WATCHDOG_S  5

void switch_context(uint64_t *save_rsp, uint64_t load_rsp);
void thread_start(void);

struct runqueue {
    spinlock_t       lock;
    struct list_node queues[PRIO_MAX + 1];
    uint32_t         bitmap;      /* bit p set: queues[p] non-empty */
    volatile uint32_t nr_ready;
    /* What placement reads about this CPU, kept on the run queue's own
     * lines (written under the lock by schedule()), so a waker on another
     * CPU never has to touch this CPU's struct cpu or its current thread:
     * busy = a non-idle thread is running, cur_prio = its priority (-1
     * when idle). Read racily, like nr_ready. */
    volatile uint32_t busy;
    volatile int     cur_prio;
    struct thread   *idle;
    struct thread   *prev;        /* handed from schedule to finish_switch */
};

static struct runqueue rqs[MAX_CPUS];

/* Topology as placement sees it, written once per CPU at bring-up
 * (sched_init_bsp, sched_run_ap_idle, sched_topology_init) and read-only
 * afterwards, so these lines stay shared in every cache. struct cpu's first
 * line, where the core id lives, is written on every syscall and switch
 * (user_rsp, kernel_rsp), so reading it from another CPU is a cache miss;
 * the M5 wake-affine code did that for every CPU on every wake that could
 * not stay on the waker's CPU (see select_cpu_affine). */
struct cpu_topo {
    int16_t sibling;   /* the other hyperthread of this core, or -1 */
    uint8_t type;      /* enum core_type */
    uint8_t pad;
};
static struct cpu_topo topo[MAX_CPUS];
static cpumask_t online_mask;   /* CPUs whose run queue is in service (atomic bits) */

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

/* Sleeping threads (any wait with a deadline): per-CPU one-shot timers
 * (M5.5). A thread that blocks with a deadline goes on the queue of the CPU
 * it blocks on, sorted by deadline, and that CPU's timer is armed for the
 * queue's head (lapic_timer_set), so it is woken within microseconds of
 * its deadline instead of at CPU 0's next 10 ms tick (M5). Races:
 *   - Only the owning CPU adds to its queue and arms its timer (the
 *     blocking thread has preemption off; the timer interrupt runs there),
 *     always under the queue lock with interrupts off.
 *   - Removal happens from anywhere: the expiring timer interrupt, or the
 *     thread itself once it is awake (on whatever CPU it runs on now), under
 *     the queue lock of the CPU it slept on (t->sleep_cpu, written only by
 *     t when it queues itself). A removal can only make the head later, so
 *     the armed timer may fire for nothing: harmless (lapic_early_irqs).
 *   - The expiring interrupt calls thread_wake(t) with the lock held, and t
 *     takes the same lock before it returns from its wait, so t can't
 *     return, exit and be freed while its waker is still in thread_wake
 *     (C6, as with the old global list).
 * Expiry compares TSC values (uptime_to_tsc rounds up), so a thread woken
 * at its deadline sees uptime_ns() >= the deadline and doesn't re-block.
 * In the periodic timer mode, or with lapic_oneshot off, each CPU's tick
 * expires its own queue. */
struct sleepq {
    spinlock_t       lock;
    struct list_node list;   /* struct thread, by wake_at_tsc */
} __attribute__((aligned(64)));
static struct sleepq sleepqs[MAX_CPUS];

/* The owning CPU, queue lock held: arm the timer for the head. */
static void sleepq_arm(struct sleepq *q)
{
    lapic_timer_set(list_empty(&q->list)
                        ? 0 : list_first(&q->list, struct thread, sleep_node)->wake_at_tsc);
}

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
    return rqs[cpu].nr_ready + rqs[cpu].busy;
}

/* Word w of the CPUs t may run on that are in service. */
static inline uint64_t usable_word(const struct thread *t, unsigned w)
{
    return t->affinity.bits[w] & __atomic_load_n(&online_mask.bits[w], __ATOMIC_RELAXED);
}

static bool usable(const struct thread *t, uint32_t cpu)
{
    return cpumask_has(&t->affinity, cpu) && cpumask_has(&online_mask, cpu);
}

/* Hybrid placement order (M5.5), best first:
 *   0  an idle P-core whose HT sibling is idle too (or that has none): a
 *      whole core to itself;
 *   1  an idle E-core (E-cores have no SMT: nothing shares it);
 *   2  the idle HT sibling of a busy P-core (half a core);
 *   3  every CPU busy: the least loaded, ties to P-cores, then the thread's
 *      last CPU (the M5 rule, which is also the whole rule with the order
 *      switched off).
 * Within classes 0-2 the thread's last CPU wins (its cache may still be
 * warm), then the lowest index. "Idle" means nothing running and nothing
 * queued. Without hybrid cores every CPU looks like a P-core, so the order
 * is just "whole idle core > idle sibling > busy". The loads are read
 * racily: a wrong guess costs time, not correctness. The M5 rule filled
 * CPUs in index order, i.e. both hyperthreads of a P-core before the next
 * core, and E-cores last. */
volatile bool sched_place_order = true;

static inline uint32_t place_key(uint32_t load, int32_t sib_load, uint8_t type, bool last,
                                 bool order)
{
    if (!order || load) {
        uint32_t l = load * 4 + (type == CORE_EFFICIENCY);
        if (last)
            l = l ? l - 1 : 0;
        return (order ? 3u << 24 : 0) | l;
    }
    uint32_t cls = type == CORE_EFFICIENCY ? 1 : sib_load <= 0 ? 0 : 2;
    return cls << 24 | !last;
}

/* The best CPU in `cand` for a thread that last ran on `last`. `fake_load`
 * (tests only) replaces the run queues' loads. UINT32_MAX if cand is empty. */
static uint32_t pick_cpu(const cpumask_t *cand, const struct cpu_topo *tp,
                         const uint32_t *fake_load, uint32_t last, bool order)
{
    uint32_t best = UINT32_MAX, best_key = UINT32_MAX;
    for (unsigned w = 0; w < MAX_CPUS / 64; w++) {
        for (uint64_t b = cand->bits[w]; b; b &= b - 1) {
            uint32_t i = w * 64 + (uint32_t)__builtin_ctzll(b);
            uint32_t l = fake_load ? fake_load[i] : load_of(i);
            int32_t sl = -1;
            int sib = tp[i].sibling;
            if (order && !l && sib >= 0)
                sl = (int32_t)(fake_load ? fake_load[sib] : load_of((uint32_t)sib));
            uint32_t key = place_key(l, sl, tp[i].type, i == last, order);
            if (key < best_key) {
                best_key = key;
                best = i;
            }
        }
    }
    return best;
}

static uint32_t select_cpu(struct thread *t)
{
    cpumask_t cand;
    for (unsigned w = 0; w < MAX_CPUS / 64; w++)
        cand.bits[w] = usable_word(t, w);
    uint32_t best = pick_cpu(&cand, topo, NULL, t->cpu, sched_place_order);
    if (best == UINT32_MAX)
        panic("sched: thread \"%s\" has no online CPU in its affinity mask", t->name);
    return best;
}

#ifndef JAM_NO_KTESTS
uint32_t sched_pick_cpu_fake(const cpumask_t *cand, const int16_t *sibling,
                             const uint8_t *type, const uint32_t *load, uint32_t last,
                             bool order)
{
    static struct cpu_topo fake[MAX_CPUS];
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        fake[i] = (struct cpu_topo){ sibling[i], type[i], 0 };
    return pick_cpu(cand, fake, load, last, order);
}
#endif

/* Wake-affine placement for a wakee whose waker, running on `waker`, is
 * about to block (thread_wake_sync). On the waker's CPU the wakee runs the
 * moment the waker blocks, with the data it was just sent still in that
 * CPU's cache; that is only right if nothing else is queued there (it would
 * wait behind it) and t may run there. Otherwise the waker's HT sibling, if
 * it is idle, shares the core's caches. Otherwise the usual choice. All the
 * loads are racy, like select_cpu's: a wrong guess costs time, never
 * correctness, since thread_wake queues t under the chosen CPU's lock.
 *
 * M5.5: the sibling comes from the topology table. M5 found it by reading
 * every CPU's struct cpu (core_id, online, current), up to cpu_count cache
 * lines on every wake that couldn't stay on the waker's CPU: twice per
 * round trip when client and server are pinned to different cores. That
 * was the 8-11% the pinned cross-CPU channel_call lines lost in M5 (the
 * P->HT line, whose scan stopped at its sibling, cpu 3, lost only 2%). */
static uint32_t select_cpu_affine(struct thread *t, uint32_t waker)
{
    if (usable(t, waker) && !rqs[waker].nr_ready) {
        t->affine_wakes++;   /* we own t's placement: we moved it to READY */
        return waker;
    }
    int sib = topo[waker].sibling;
    if (sib >= 0 && usable(t, (uint32_t)sib) && !load_of((uint32_t)sib)) {
        t->affine_wakes++;
        return (uint32_t)sib;
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
    if (!remote)
        return;
    /* A CPU spinning in idle_loop sees need_resched by itself. The fence
     * orders our need_resched store (and the enqueue before it) against
     * the read of its polling flag: the other half of idle_loop's
     * handshake. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->idle_polling, __ATOMIC_RELAXED)) {
        c->polled_wakes++;   /* racy statistic */
        return;
    }
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
    /* Whatever runs next, this CPU is no longer spinning in idle_loop (an
     * interrupt during the spin can switch the idle thread out from here). */
    if (c->idle_polling)
        __atomic_store_n(&c->idle_polling, false, __ATOMIC_SEQ_CST);

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
    rq->busy = !next->is_idle;
    rq->cur_prio = next->is_idle ? -1 : next->prio;
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

/* Who is waking: the current thread and its CPU, when the wake comes from
 * thread context (never from an interrupt handler: the interrupted thread
 * has nothing to do with the wakee, and its wake_sync flag is not about
 * this wake). `sync`: place wake-affine. A plain thread_wake (`consume`)
 * honours the current thread's wake_sync flag and clears it (one wakee per
 * flag); thread_wake_sync always places wake-affine. */
struct waker {
    struct thread *me;   /* NULL: interrupt handler or the idle thread */
    uint32_t       cpu;
    bool           sync;
};

static struct waker waker_now(bool consume)
{
    struct waker w = { NULL, 0, false };
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    struct thread *me = c->current;
    if (!c->irq_depth && me && !me->is_idle) {
        w.me = me;
        w.cpu = c->index;
        w.sync = !consume || me->wake_sync;
        if (w.sync)
            me->wake_sync = false;
    }
    irq_restore(f);
    return w;
}

/* Client/server pairs on sibling hyperthreads (M5.5). Every wake from
 * thread context records the waker in the wakee (partner_id, and how many
 * wakes in a row came from it). Two threads that each were woken by the
 * other at least PAIR_MIN times running are a pair. When one of them wakes
 * the other and does NOT block right after (a plain wake: the sync case is
 * select_cpu_affine's), the wakee goes to the waker's idle HT sibling:
 * both halves of the pair then share the core's L1/L2, and the next wake is
 * the cheapest cross-CPU one (BENCH.md: block+wake P->HT 1.1 us, P->P2
 * 1.5 us). Without it the hybrid order would give the wakee a whole idle
 * core, which is right for independent work and wrong for a pair. E-cores
 * have no sibling (topo[].sibling = -1), so a pair on an E-core is placed
 * as usual. Only the waker that moved t to READY writes t's partner fields
 * (after the CAS), so they have one writer at a time; the waker's own
 * fields were written by whoever woke it last and are read racily. A
 * wrong guess costs time, not correctness. */
#define PAIR_MIN 2
volatile bool sched_affine_pair = true;

static void note_waker(struct thread *t, const struct thread *me)
{
    if (t->partner_id == me->id) {
        if (t->partner_streak < UINT32_MAX)
            t->partner_streak++;
    } else {
        t->partner_id = me->id;
        t->partner_streak = 1;
    }
}

static bool is_pair(const struct thread *t, const struct thread *me)
{
    return t->partner_id == me->id && t->partner_streak >= PAIR_MIN &&
           me->partner_id == t->id && me->partner_streak >= PAIR_MIN;
}

static uint32_t select_cpu_pair(struct thread *t, uint32_t waker)
{
    int sib = topo[waker].sibling;
    if (sib >= 0 && usable(t, (uint32_t)sib) && !load_of((uint32_t)sib)) {
        t->pair_wakes++;
        return (uint32_t)sib;
    }
    return select_cpu(t);
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

    uint32_t cpu;
    struct waker w = { NULL, 0, false };
    if (was_blocked)
        w = waker_now(!sync);
    if (w.me)
        note_waker(t, w.me);
    if (w.me && w.sync)
        cpu = select_cpu_affine(t, w.cpu);
    else if (w.me && sched_affine_pair && is_pair(t, w.me))
        cpu = select_cpu_pair(t, w.cpu);
    else
        cpu = select_cpu(t);
    struct runqueue *rq = &rqs[cpu];
    uint64_t f = spin_lock_irqsave(&rq->lock);
    enqueue(rq, t, cpu);
    bool kick = t->prio > rq->cur_prio;   /* -1 when idle: see struct runqueue */
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
        if (q->list.next == &t->sleep_node)
            sleepq_arm(q);   /* the new head */
        spin_unlock_irqrestore(&q->lock, f);
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
    /* Always take the queue lock before touching sleep_node when we may
     * have been on a sleeper queue: sched_timer_expire deletes the node and
     * calls thread_wake(t) while holding it, so a lockless check here could
     * let this thread return (and re-block or exit, freeing itself) while
     * the waker is still inside thread_wake(t). Serialising on the lock
     * keeps t alive until the waker is done. (C6) The queue is the one of
     * the CPU we slept on, not the one we run on now. */
    if (deadline_ns != DEADLINE_NEVER) {
        struct sleepq *q = &sleepqs[t->sleep_cpu];
        uint64_t f = spin_lock_irqsave(&q->lock);
        if (t->sleep_node.next)
            list_del(&t->sleep_node);
        spin_unlock_irqrestore(&q->lock, f);
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

/* ---- idle, work stealing ---------------------------------------------------- */

/* With interrupts off: move one waiting thread from a busy CPU to us. Two
 * run queue locks are always taken lower CPU index first. */
static void try_steal(uint32_t me)
{
    for (uint32_t k = 1; k < cpu_count; k++) {
        uint32_t v = (me + k) % cpu_count;
        if (!cpumask_has(&online_mask, v) || !rqs[v].nr_ready || !rqs[v].busy)
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

/* Spin before idle (M5.5). An idle CPU first polls its run queue and
 * need_resched for up to sched_idle_spin_ns, with interrupts on and `pause`
 * between looks, and only then halts. A wakeup that lands within the window
 * is seen at once: no wake-from-halt (most of the ~1.5 us cross-CPU
 * block+wake round trip on the PC, BENCH.md), and no IPI either, because
 * the CPU says it is polling (cpu->idle_polling) and sched_kick skips the
 * IPI for a polling CPU.
 *
 * The flag handshake is Dekker's: the idle CPU clears idle_polling, fences
 * and then looks at its queue and need_resched once more before `hlt`; a
 * waker queues, sets need_resched, fences and then reads idle_polling. At
 * least one sees the other: either the waker sees polling cleared and sends
 * the IPI (which, if it lands before the `hlt`, stays pending through the
 * sti shadow and ends the halt at once), or the idle CPU sees the work and
 * doesn't halt. schedule() also clears the flag, for the case where an
 * interrupt that arrived during the spin switches the idle thread out.
 *
 * Power: a spinning CPU runs at full clock and, on a P-core, takes issue
 * slots from its hyperthread sibling (pause yields most of them). The
 * window is spent once per idle entry, so with the 100 Hz tick waking idle
 * CPUs it costs at most 100 x the window per second per idle CPU (0.1% at
 * 10 us) plus the window after every wakeup; the gain is for wakeups that
 * come within the window, i.e. tightly coupled threads on different CPUs.
 * Tickless idle (M10) will make the tick part go away. Tunable at boot
 * ("idlespin=<us>", "nospinidle" = 0) and at run time (the benchmark). */
volatile uint64_t sched_idle_spin_ns = SCHED_IDLE_SPIN_NS;

static bool idle_has_work(struct cpu *c)
{
    return rqs[c->index].nr_ready || c->need_resched;
}

_Noreturn static void idle_loop(void)
{
    struct cpu *c = this_cpu();
    for (;;) {
        irq_disable();
        if (!rqs[c->index].nr_ready)
            try_steal(c->index);
        if (idle_has_work(c)) {
            irq_enable();
            schedule();
            continue;
        }
        uint64_t spin_ns = sched_idle_spin_ns;
        if (spin_ns) {
            __atomic_store_n(&c->idle_polling, true, __ATOMIC_SEQ_CST);
            irq_enable();
            uint64_t end = rdtsc() + spin_ns * (tsc_hz / 1000000) / 1000;
            while (!idle_has_work(c) && rdtsc() < end)
                cpu_relax();
            irq_disable();
            __atomic_store_n(&c->idle_polling, false, __ATOMIC_SEQ_CST);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (idle_has_work(c)) {
                irq_enable();
                schedule();
                continue;
            }
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
    rq->cur_prio = -1;
    topo[cpu].sibling = -1;
    spin_init(&sleepqs[cpu].lock, "sleepers");
    list_init(&sleepqs[cpu].list);
}

/* This CPU's run queue is ready: placement may use it from now on. */
static void rq_online(struct cpu *c)
{
    topo[c->index].type = (uint8_t)c->type;
    __atomic_fetch_or(&online_mask.bits[c->index / 64], 1ull << (c->index % 64),
                      __ATOMIC_RELEASE);
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
    sched_idle_spin_ns = cmdline_has("nospinidle")
                             ? 0 : cmdline_get_u64("idlespin", SCHED_IDLE_SPIN_NS / 1000, 0) * 1000;
    sched_place_order = !cmdline_has("noplaceorder");
    sched_affine_pair = !cmdline_has("noaffinepair");

    /* The code running now becomes thread "main". */
    struct thread *main = thread_alloc("main", PRIO_DEFAULT);
    if (!main)
        panic("sched: out of memory for thread main");
    main->state = T_RUNNING;
    main->on_cpu = true;
    main->cpu = 0;
    main->refs = 2;   /* never joined; keep it alive */
    this_cpu()->current = main;
    rqs[0].busy = 1;
    rqs[0].cur_prio = main->prio;

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
    rq_online(this_cpu());
}

_Noreturn void sched_run_ap_idle(void)
{
    struct cpu *c = this_cpu();
    struct thread *idle = make_idle(c->index);
    idle->on_cpu = true;
    idle->stack_top = c->kstack_top;
    rqs[c->index].idle = idle;
    c->current = idle;
    rq_online(c);
    idle_loop();
}

void sched_topology_init(void)
{
    for (uint32_t i = 0; i < cpu_count; i++) {
        topo[i].sibling = -1;
        for (uint32_t j = 0; j < cpu_count; j++)
            if (j != i && cpus[j]->online && cpus[j]->core_id == cpus[i]->core_id)
                topo[i].sibling = (int16_t)j;   /* at most one: 2-way SMT */
    }
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
