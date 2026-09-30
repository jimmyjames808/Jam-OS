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
 *
 * Thread lifecycle (creation, exit, the stack cache) is in thread.c;
 * blocking, sleeping, wait queues and mutexes are in wait.c.
 * sched_internal.h is what the three files share.
 */
#include <jam/atomic.h>
#include <jam/cmdline.h>
#include <jam/dbghook.h>
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/serial.h>
#include <jam/smp.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/x86.h>

#include "sched_internal.h"

#define WATCHDOG_S  5

void switch_context(uint64_t *save_rsp, uint64_t load_rsp);

struct runqueue {
    spinlock_t       lock;                  /* guards everything below and the threads queued */
    struct list_node queues[PRIO_MAX + 1];  /* one list of ready threads per priority */
    uint32_t         bitmap;                /* bit p set: queues[p] non-empty */
    uint32_t         nr_ready;              /* threads queued (read racily by placement) */
    /* What placement reads about this CPU, kept on the run queue's own
     * lines (written under the lock by schedule()), so a waker on another
     * CPU never has to touch this CPU's struct cpu or its current thread:
     * busy = a non-idle thread is running, cur_prio = its priority (-1
     * when idle). Read racily, like nr_ready. */
    uint32_t         busy;
    int              cur_prio;
    struct thread   *idle;
    struct thread   *prev;                  /* handed from schedule to finish_switch */
};

/* The racy reads: nr_ready, busy and cur_prio change only under the lock,
 * but other CPUs read them without it, so every access is atomic
 * (relaxed: a stale answer only changes a placement). */
static inline uint32_t rq_ready(const struct runqueue *rq)
{
    return __atomic_load_n(&rq->nr_ready, __ATOMIC_RELAXED);
}

static inline uint32_t rq_busy(const struct runqueue *rq)
{
    return __atomic_load_n(&rq->busy, __ATOMIC_RELAXED);
}

static inline int rq_cur_prio(const struct runqueue *rq)
{
    return __atomic_load_n(&rq->cur_prio, __ATOMIC_RELAXED);
}

static struct runqueue rqs[MAX_CPUS];

/* Topology as placement sees it, written once per CPU at bring-up
 * (sched_init_bsp, sched_run_ap_idle, sched_topology_init) and read-only
 * afterwards, so these lines stay shared in every cache. struct cpu's first
 * line, where the core id lives, is written on every syscall and switch
 * (user_rsp, kernel_rsp), so reading it from another CPU is a cache miss,
 * and wake-affine placement would pay one for every CPU on every wake that
 * can't stay on the waker's CPU (see select_cpu_affine). */
struct cpu_topo {
    int16_t sibling;   /* the other hyperthread of this core, or -1 */
    uint8_t type;      /* enum core_type */
    uint8_t pad;       /* 0 */
};
static struct cpu_topo topo[MAX_CPUS];
static cpumask_t online_mask;   /* CPUs whose run queue is in service (atomic bits) */

/* Debug trace: each CPU's last few switches. */
#define TRACE_N 8
struct switch_event {
    struct thread *prev, *next;   /* the threads switched from and to */
    int prev_state;               /* prev's state at the switch */
    uint64_t tick;                /* this CPU's tick count then */
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
 * recurse into schedule(). (test: repro_local_wake_latency) */
void preempt_check(void)
{
    if (!irqs_enabled())
        return;
    /* Look at need_resched with interrupts off so the CPU we test is the CPU
     * we act on. */
    uint64_t f = irq_save();
    struct cpu *c = this_cpu();
    bool go = cpu_need_resched(c) && c->irq_depth == 0 && c->preempt_count == 0 && c->current;
    irq_restore(f);
    if (go)
        schedule();
}

/* ---- run queue primitives (rq->lock held) --------------------------------- */

static void enqueue(struct runqueue *rq, struct thread *t, uint32_t cpu)
{
    if (t->rq_node.next)
        panic("sched: \"%s\" queued on cpu %u while already queued on cpu %u (state %d, on_cpu %d)",
              t->name, cpu, thread_cpu(t), thread_state(t),
              __atomic_load_n(&t->on_cpu, __ATOMIC_RELAXED));
    thread_set_state(t, T_READY);
    thread_set_cpu(t, cpu);
    t->ready_since = cpu_ticks(cpus[cpu]);
    list_add_tail(&rq->queues[t->prio], &t->rq_node);
    rq->bitmap |= 1u << t->prio;
    COUNTER_ADD(&rq->nr_ready, 1);
}

static void dequeue(struct runqueue *rq, struct thread *t)
{
    list_del(&t->rq_node);
    if (list_empty(&rq->queues[t->prio]))
        rq->bitmap &= ~(1u << t->prio);
    COUNTER_SUB(&rq->nr_ready, 1);
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
    return rq_ready(&rqs[cpu]) + rq_busy(&rqs[cpu]);
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

/* Hybrid placement order, best first:
 *   0  an idle P-core whose HT sibling is idle too (or that has none): a
 *      whole core to itself;
 *   1  an idle E-core (E-cores have no SMT: nothing shares it);
 *   2  the idle HT sibling of a busy P-core (half a core);
 *   3  every CPU busy: the least loaded, ties to P-cores, then the thread's
 *      last CPU (the plain least-loaded rule, which is also the whole rule
 *      with the order switched off).
 * Within classes 0-2 the thread's last CPU wins (its cache may still be
 * warm), then the lowest index. "Idle" means nothing running and nothing
 * queued. Without hybrid cores every CPU looks like a P-core, so the order
 * is just "whole idle core > idle sibling > busy". The loads are read
 * racily: a wrong guess costs time, not correctness. The least-loaded rule
 * alone fills CPUs in index order, i.e. both hyperthreads of a P-core before
 * the next core, and E-cores last. */
bool sched_place_order = true;

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

static uint32_t select_cpu(const struct thread *t)
{
    cpumask_t cand;
    for (unsigned w = 0; w < MAX_CPUS / 64; w++)
        cand.bits[w] = usable_word(t, w);
    uint32_t best = pick_cpu(&cand, topo, NULL, thread_cpu(t),
                             __atomic_load_n(&sched_place_order, __ATOMIC_RELAXED));
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
 * The sibling comes from the topology table. Finding it by reading every
 * CPU's struct cpu (core_id, online, current) cost up to cpu_count cache
 * misses on every wake that couldn't stay on the waker's CPU, twice per
 * round trip when client and server are pinned to different cores: 8-11%
 * on the benchmark's pinned cross-CPU channel_call lines. */
static uint32_t select_cpu_affine(struct thread *t, uint32_t waker)
{
    if (usable(t, waker) && !rq_ready(&rqs[waker])) {
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

/* Tell `cpu` to look at its run queue soon (IPI if remote and not polling
 * in idle). */
static void sched_kick(uint32_t cpu)
{
    struct cpu *c = cpus[cpu];
    cpu_set_need_resched(c, true);
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
        __atomic_add_fetch(&c->polled_wakes, 1, __ATOMIC_RELAXED);   /* racy statistic */
        return;
    }
    ipi_send(cpu, VEC_RESCHEDULE);
}

/* ---- the switch ------------------------------------------------------------ */

/* First thing on the far side of every switch_context. */
void finish_switch(void)
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
              thread_cpu(c->current), __atomic_load_n(&c->current->on_cpu, __ATOMIC_RELAXED),
              thread_state(c->current));
    }
    rq->prev = NULL;
    /* Read prev->state BEFORE the on_cpu release store. While on_cpu is set
     * and this rq lock is held nobody else can pick prev, so its state is
     * stable; once on_cpu clears, prev can be woken, run, exit and be reaped
     * on another CPU, so a later read here could see a recycled state and
     * reap it a second time. (test: repro_finish_switch_double_reap) */
    int prev_state = thread_state(prev);
    /* Clear on_cpu BEFORE dropping the lock: a waker holding this lock and
     * seeing on_cpu set then knows prev has not yet reached schedule(). */
    __atomic_store_n(&prev->on_cpu, false, __ATOMIC_RELEASE);
    spin_unlock_no_resched(&rq->lock);
    DBG_HOOK(DBG_FINISH_SWITCH, prev);

    if (prev_state == T_DEAD)
        thread_reap(prev);
    else if (prev_state == T_MIGRATING)
        thread_wake(prev);
}

/* CPU time: prev ran from the last switch until now. Before c->current
 * changes, so a reader that still sees prev as current adds a run that
 * starts at the new switch_tsc (a few cycles at most). */
static inline void account_switch(struct cpu *c, struct thread *prev, const struct thread *next)
{
    uint64_t now = rdtsc(), last = __atomic_load_n(&c->switch_tsc, __ATOMIC_RELAXED);
    uint64_t d = last && now > last ? now - last : 0;
    COUNTER_ADD(&prev->run_tsc, d);
    if (prev->is_idle)
        COUNTER_ADD(&c->idle_tsc, d);
    __atomic_store_n(&c->switch_tsc, now, __ATOMIC_RELAXED);
    __atomic_store_n(&c->idle_now, next->is_idle, __ATOMIC_RELAXED);
}

uint64_t thread_cpu_tsc(struct thread *t)
{
    uint64_t v = __atomic_load_n(&t->run_tsc, __ATOMIC_RELAXED);
    uint32_t i = thread_cpu(t);
    if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) && i < cpu_count && cpus[i] &&
        cpus[i]->current == t) {
        uint64_t last = __atomic_load_n(&cpus[i]->switch_tsc, __ATOMIC_RELAXED), now = rdtsc();
        if (last && now > last)
            v += now - last;
    }
    return v;
}

uint64_t sched_cpu_idle_tsc(uint32_t i)
{
    if (i >= cpu_count || !cpus[i])
        return 0;
    struct cpu *c = cpus[i];
    uint64_t v = __atomic_load_n(&c->idle_tsc, __ATOMIC_RELAXED);
    uint64_t last = __atomic_load_n(&c->switch_tsc, __ATOMIC_RELAXED);
    if (__atomic_load_n(&c->idle_now, __ATOMIC_RELAXED)) {
        uint64_t now = rdtsc();
        if (last && now > last)
            v += now - last;
    }
    return v;
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
    cpu_set_need_resched(c, false);
    /* Whatever runs next, this CPU is no longer spinning in idle_loop (an
     * interrupt during the spin can switch the idle thread out from here). */
    if (__atomic_load_n(&c->idle_polling, __ATOMIC_RELAXED))
        __atomic_store_n(&c->idle_polling, false, __ATOMIC_SEQ_CST);

    struct thread *prev = c->current;
    /* A boost lasts one turn on the CPU. */
    prev->prio = __atomic_load_n(&prev->base_prio, __ATOMIC_RELAXED);
    if (thread_state(prev) == T_RUNNING && !prev->is_idle) {
        if (cpumask_has(&prev->affinity, c->index))
            enqueue(rq, prev, c->index);
        else
            thread_set_state(prev, T_MIGRATING);   /* moved in finish_switch */
    }
    /* T_BLOCKED / T_DEAD: not queued. T_READY: a waker already queued it. */
    DBG_HOOK(DBG_SCHED_PREV, prev);

    struct thread *next = pick_best(rq);
    if (!next)
        next = rq->idle;
    if (next == prev) {
        thread_set_state(prev, T_RUNNING);
        /* A boost just ended. */
        __atomic_store_n(&rq->cur_prio, prev->is_idle ? -1 : prev->prio, __ATOMIC_RELAXED);
        prev->slice = SLICE_TICKS;   /* refresh: a thread that used its slice
                                      * while alone must be sliced again once
                                      * a same-priority peer is queued
                                      * (test: repro_slice_not_reset) */
        spin_unlock_no_resched(&rq->lock);
        irq_restore(flags);
        return;
    }
    while (__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE))
        cpu_relax();   /* still being switched out on another CPU */

    if (thread_state(next) != T_READY && !next->is_idle)
        panic("sched: picked \"%s\" in state %d on cpu %u", next->name, thread_state(next),
              c->index);
    thread_set_state(next, T_RUNNING);
    __atomic_store_n(&next->on_cpu, true, __ATOMIC_RELAXED);
    thread_set_cpu(next, c->index);
    next->slice = SLICE_TICKS;
    next->switches_in++;
    account_switch(c, prev, next);
    c->current = next;
    __atomic_store_n(&rq->busy, !next->is_idle, __ATOMIC_RELAXED);
    __atomic_store_n(&rq->cur_prio, next->is_idle ? -1 : next->prio, __ATOMIC_RELAXED);
    COUNTER_ADD(&c->switches, 1);
    rq->prev = prev;
    trace[c->index][trace_pos[c->index]++ % TRACE_N] =
        (struct switch_event){ prev, next, thread_state(prev), cpu_ticks(c) };
    arch_thread_switch(prev, next);   /* kernel stack, FPU, address space */
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
    struct thread *me;    /* NULL: interrupt handler or the idle thread */
    uint32_t       cpu;   /* the CPU the wake comes from */
    bool           sync;  /* place wake-affine */
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

/* Client/server pairs on sibling hyperthreads. Every wake from
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
bool sched_affine_pair = true;

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

/* Queue t, READY and on no run queue, on `cpu` and tell that CPU if t
 * should run before what it is running now. */
static void place_on(struct thread *t, uint32_t cpu)
{
    struct runqueue *rq = &rqs[cpu];
    uint64_t f = spin_lock_irqsave(&rq->lock);
    enqueue(rq, t, cpu);
    bool kick = t->prio > rq_cur_prio(rq);   /* -1 when idle: see struct runqueue */
    spin_unlock_irqrestore(&rq->lock, f);
    if (kick)
        sched_kick(cpu);
}

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
    int s = thread_state(t);
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
         * (it ends RUNNING on no CPU and no queue). (test: repro_wake_stale_cpu) */
        for (;;) {
            uint32_t c = thread_cpu(t);
            struct runqueue *own = &rqs[c];
            uint64_t f = spin_lock_irqsave(&own->lock);
            if (thread_cpu(t) != c) {
                spin_unlock_irqrestore(&own->lock, f);
                continue;   /* migrated: relock its current run queue */
            }
            /* A CAS, not a check-then-store: a cancellable wait moves its own
             * thread BLOCKED -> RUNNING without this lock (block_prepared),
             * and could then run on and exit; a plain store here would turn
             * its T_DEAD back into T_RUNNING. */
            int blocked = T_BLOCKED;
            bool still_here = __atomic_load_n(&t->on_cpu, __ATOMIC_RELAXED) &&
                              __atomic_compare_exchange_n(&t->state, &blocked, T_RUNNING, false,
                                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
            spin_unlock_irqrestore(&own->lock, f);
            if (still_here)
                return;
            break;
        }
        s = thread_state(t);   /* it switched out meanwhile: wake it normally */
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
    else if (w.me && __atomic_load_n(&sched_affine_pair, __ATOMIC_RELAXED) && is_pair(t, w.me))
        cpu = select_cpu_pair(t, w.cpu);
    else
        cpu = select_cpu(t);
    place_on(t, cpu);
}

/* ---- idle, work stealing ---------------------------------------------------- */

/* With interrupts off: move one waiting thread from a busy CPU to us. Two
 * run queue locks are always taken lower CPU index first. */
static void try_steal(uint32_t me)
{
    for (uint32_t k = 1; k < cpu_count; k++) {
        uint32_t v = (me + k) % cpu_count;
        if (!cpumask_has(&online_mask, v) || !rq_ready(&rqs[v]) || !rq_busy(&rqs[v]))
            continue;
        struct runqueue *a = &rqs[me < v ? me : v], *b = &rqs[me < v ? v : me];
        spin_lock(&a->lock);
        spin_lock_nested(&b->lock, 1);
        struct thread *t = pick_stealable(&rqs[v], me);
        if (t) {
            enqueue(&rqs[me], t, me);
            struct cpu *c = this_cpu();
            COUNTER_ADD(&c->steals, 1);
        }
        spin_unlock_no_resched(&b->lock);
        spin_unlock_no_resched(&a->lock);
        if (t)
            return;
    }
}

/* Spin before idle. An idle CPU first polls its run queue and
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
 * A tickless idle would make the tick part go away. Tunable at boot
 * ("idlespin=<us>", "nospinidle" = 0) and at run time (the benchmark). */
uint64_t sched_idle_spin_ns = SCHED_IDLE_SPIN_NS;

static bool idle_has_work(const struct cpu *c)
{
    return rq_ready(&rqs[c->index]) || cpu_need_resched(c);
}

_Noreturn static void idle_loop(void)
{
    struct cpu *c = this_cpu();
    for (;;) {
        irq_disable();
        if (!rq_ready(&rqs[c->index]))
            try_steal(c->index);
        if (idle_has_work(c)) {
            irq_enable();
            schedule();
            continue;
        }
        uint64_t spin_ns = __atomic_load_n(&sched_idle_spin_ns, __ATOMIC_RELAXED);
        if (spin_ns) {
            __atomic_store_n(&c->idle_polling, true, __ATOMIC_SEQ_CST);
            irq_enable();
            uint64_t end = rdtsc() + spin_ns * (tsc_hz / 1000000) / 1000;
            while (!idle_has_work(c) && rdtsc() < end)
                cpu_relax();
            if (idle_has_work(c)) {
                /* Leave idle_polling set: schedule() clears it under the
                 * run queue lock, so a waker that enqueued before that
                 * still sees "polling" and skips a redundant IPI. */
                schedule();
                continue;
            }
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
    __atomic_store_n(&rq->cur_prio, -1, __ATOMIC_RELAXED);
    topo[cpu].sibling = -1;
    sleepq_init(cpu);
}

/* This CPU's run queue is ready: placement may use it from now on. */
static void rq_online(struct cpu *c)
{
    topo[c->index].type = (uint8_t)c->type;
    /* CPU time from here. */
    __atomic_store_n(&c->idle_now, c->current && c->current->is_idle, __ATOMIC_RELAXED);
    __atomic_store_n(&c->switch_tsc, rdtsc(), __ATOMIC_RELAXED);
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
    thread_set_state(t, T_RUNNING);
    cpumask_one(&t->affinity, cpu);
    thread_set_cpu(t, cpu);
    return t;
}

void sched_init_bsp(void)
{
    thread_cache_init();
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        init_rq(i);
    uint64_t spin_ns = cmdline_has("nospinidle")
                           ? 0 : cmdline_get_u64("idlespin", SCHED_IDLE_SPIN_NS / 1000, 0) * 1000;
    __atomic_store_n(&sched_idle_spin_ns, spin_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&sched_place_order, !cmdline_has("noplaceorder"), __ATOMIC_RELAXED);
    __atomic_store_n(&sched_affine_pair, !cmdline_has("noaffinepair"), __ATOMIC_RELAXED);

    /* The code running now becomes thread "main". */
    struct thread *main = thread_alloc("main", PRIO_DEFAULT);
    if (!main)
        panic("sched: out of memory for thread main");
    thread_set_state(main, T_RUNNING);
    __atomic_store_n(&main->on_cpu, true, __ATOMIC_RELAXED);
    thread_set_cpu(main, 0);
    main->refs = 2;   /* never joined; keep it alive */
    this_cpu()->current = main;
    __atomic_store_n(&rqs[0].busy, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&rqs[0].cur_prio, main->prio, __ATOMIC_RELAXED);

    /* BSP idle thread: a real thread with its own stack, first run when
     * main blocks. */
    struct thread *idle = make_idle(0);
    thread_set_state(idle, T_READY);
    idle->stack_top = thread_stack_get();
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
    __atomic_store_n(&idle->on_cpu, true, __ATOMIC_RELAXED);
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
            if (j != i && cpu_online(cpus[j]) && cpus[j]->core_id == cpus[i]->core_id)
                topo[i].sibling = (int16_t)j;   /* at most one: 2-way SMT */
    }
}

/* ---- tick, watchdog, irq exit ----------------------------------------------- */

static void watchdog_check(struct cpu *c)
{
    /* Once a second, see whether the next CPU's tick count moved. */
    if (cpu_ticks(c) % TICK_HZ || cpu_count < 2)
        return;
    struct cpu *w = cpus[(c->index + 1) % cpu_count];
    if (!cpu_online(w))
        return;
    if (cpu_ticks(w) != c->wd_seen_ticks) {
        c->wd_seen_ticks = cpu_ticks(w);
        c->wd_stale_seconds = 0;
    } else if (++c->wd_stale_seconds == WATCHDOG_S) {
        watchdog_fire(w->index);
    }
}

static uint64_t boost_total;   /* starvation boosts on every CPU (atomic) */

uint64_t sched_boost_count(void)
{
    return __atomic_load_n(&boost_total, __ATOMIC_RELAXED);
}

/* Once a second: boost threads that have waited too long on this CPU. */
static void boost_starved(struct cpu *c)
{
    struct runqueue *rq = &rqs[c->index];
    if (cpu_ticks(c) % TICK_HZ || !rq_ready(rq))
        return;
    spin_lock(&rq->lock);   /* timer interrupt: IRQs already off */
    for (int p = PRIO_BOOST - 1; p >= 0; p--) {
        struct list_node *n = rq->queues[p].next;
        while (n != &rq->queues[p]) {
            struct thread *t = container_of(n, struct thread, rq_node);
            n = n->next;
            if (cpu_ticks(c) - t->ready_since < STARVE_TICKS)
                continue;
            dequeue(rq, t);
            t->prio = PRIO_BOOST;
            t->boosts++;
            __atomic_add_fetch(&boost_total, 1, __ATOMIC_RELAXED);
            list_add_tail(&rq->queues[PRIO_BOOST], &t->rq_node);
            rq->bitmap |= 1u << PRIO_BOOST;
            COUNTER_ADD(&rq->nr_ready, 1);
        }
    }
    struct thread *cur = c->current;
    if (cur && rq->bitmap && (31 - __builtin_clz(rq->bitmap)) > cur->prio)
        cpu_set_need_resched(c, true);
    spin_unlock_no_resched(&rq->lock);
}

void sched_tick(void)
{
    struct cpu *c = this_cpu();
    if (c->index == 0) {
        serial_poll();   /* rescues a stalled serial transmitter (serial.c) */
        klog_poll();     /* wakes kernel log readers (sysc_console.c) */
    }
    watchdog_check(c);
    boost_starved(c);

    struct thread *t = c->current;
    if (!t)
        return;
    if (t->is_idle) {
        if (rq_ready(&rqs[c->index]))
            cpu_set_need_resched(c, true);
    } else if (t->slice && --t->slice == 0) {
        cpu_set_need_resched(c, true);
    }
}

void sched_irq_exit(uint64_t interrupted_rflags)
{
    struct cpu *c = this_cpu();
    if (cpu_need_resched(c) && c->preempt_count == 0 && c->irq_depth == 0 &&
        (interrupted_rflags & (1u << 9)) && c->current)
        schedule();
}

void sched_print_stats(void)
{
    uint64_t sw = 0, steals = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        sw += __atomic_load_n(&cpus[i]->switches, __ATOMIC_RELAXED);
        steals += __atomic_load_n(&cpus[i]->steals, __ATOMIC_RELAXED);
    }
    kprintf("sched: %lu context switches, %lu steals, %lu starvation boosts across %u CPUs\n",
            sw, steals, sched_boost_count(), cpu_count);
}
