/* Races in the scheduler's switch and wake paths (and one in the TLB
 * shootdown a migrating thread needs), each forced with the dbghook
 * injection points (jam/dbghook.h, free when unset) so the interleaving
 * happens every time. Each checks that the race's window, forced open,
 * does no harm. Each needs >= 4 CPUs to build its interleaving, so on a
 * smaller machine they skip rather than fail. */
#include <jam/cmdline.h>
#include <jam/dbghook.h>
#include <jam/ipi.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/time.h>
#include <jam/x86.h>

static bool enabled(void)
{
    if (cpu_count < 4) {
        kprintf("ktest: %s skipped (needs >= 4 CPUs to force the interleaving)\n",
                ktest_current);
        return false;
    }
    return true;
}

static bool wait_for(volatile int *v, int want, uint64_t ms)
{
    uint64_t end = uptime_ns() + ms * 1000000;
    while (*v != want)
        if (uptime_ns() > end)
            return false;
    return true;
}

/* ---- 1. a wakeup on the waker's own CPU waits for the next tick ------------ */

static spinlock_t lat_lock = SPINLOCK_INIT("repro lat");
static struct waitqueue lat_wq;
static volatile int lat_go, lat_stop;
static volatile uint64_t lat_ran_tsc, lat_ran_seq;

static void lat_high(void *arg)
{
    (void)arg;
    uint64_t f = spin_lock_irqsave(&lat_lock);
    while (!lat_stop) {
        while (!lat_go && !lat_stop)
            waitqueue_wait(&lat_wq, &lat_lock, &f);
        lat_go = 0;
        lat_ran_tsc = rdtsc();
        lat_ran_seq++;
    }
    spin_unlock_irqrestore(&lat_lock, f);
}

/* Returns the worst wake-to-run latency in microseconds. */
static uint64_t lat_measure(uint32_t high_cpu, uint32_t waker_cpu, int rounds)
{
    kt_pin_self(waker_cpu);
    uint64_t worst = 0, sum = 0;
    for (int i = 0; i < rounds; i++) {
        thread_sleep_ms(3);   /* the high thread is blocked again */
        uint64_t seq = lat_ran_seq;
        uint64_t f = spin_lock_irqsave(&lat_lock);
        lat_go = 1;
        spin_unlock_irqrestore(&lat_lock, f);
        uint64_t t0 = rdtsc();
        waitqueue_wake_one(&lat_wq);
        /* Busy, preemptible, no locks: a higher-priority thread made
         * runnable on this CPU should take it over at once. */
        while (lat_ran_seq == seq && rdtsc() - t0 < tsc_hz / 10)
            cpu_relax();
        KT_ASSERT(lat_ran_seq != seq);
        uint64_t us = (lat_ran_tsc - t0) / (tsc_hz / 1000000);
        sum += us;
        if (us > worst)
            worst = us;
    }
    kprintf("repro: high-prio thread on cpu %u woken from cpu %u: worst %lu us, avg %lu us\n",
            high_cpu, waker_cpu, worst, sum / rounds);
    (void)high_cpu;
    return worst;
}

KTEST(repro_local_wake_latency)
{
    if (!enabled())
        return;
    waitqueue_init(&lat_wq, "repro lat wq");
    lat_go = lat_stop = 0;
    cpumask_t m;
    cpumask_one(&m, 1);
    struct thread *h = thread_create_on("repro-high", lat_high, NULL, 28, &m);
    thread_set_priority(current_thread(), PRIO_DEFAULT);
    uint64_t remote = lat_measure(1, 2, 20);   /* IPI path */
    uint64_t local = lat_measure(1, 1, 20);    /* same CPU: no IPI */
    uint64_t f = spin_lock_irqsave(&lat_lock);
    lat_stop = 1;
    spin_unlock_irqrestore(&lat_lock, f);
    waitqueue_wake_all(&lat_wq);
    thread_join(h);
    kt_unpin_self();
    (void)remote;
    /* spin_unlock_irqrestore re-checks need_resched once interrupts are
     * back on, so a higher-priority thread woken on the waker's own CPU
     * runs almost immediately instead of waiting up to a whole tick (10 ms,
     * which is what this measures without that check). */
    KT_IDLE_ASSERT(local < 2000);   /* a latency: idle only */
}

/* ---- 2. finish_switch reads prev->state after releasing it ---------------- */

static struct thread *volatile fs_target;
static volatile int fs_phase, fs_go;
static volatile int fs_seen_state = -1;
static spinlock_t fs_lock = SPINLOCK_INIT("repro fs");
static struct waitqueue fs_wq;

static void fs_hook(void *arg)
{
    struct thread *prev = arg;
    if (prev != fs_target || fs_phase != 1)
        return;
    fs_phase = 2;          /* prev is off this CPU and its rq lock is free */
    udelay(20000);         /* stands in for an SMI / stalled vCPU here */
    fs_seen_state = thread_state(prev);
    fs_phase = 3;
}

static void fs_victim(void *arg)
{
    (void)arg;
    while (!fs_target)
        cpu_relax();
    uint64_t f = spin_lock_irqsave(&fs_lock);
    while (!fs_go)
        waitqueue_wait(&fs_wq, &fs_lock, &f);
    spin_unlock_irqrestore(&fs_lock, f);
}   /* returns: thread_exit */

static void idle_fn(void *arg)
{
    (void)arg;
}

KTEST(repro_finish_switch_double_reap)
{
    KT_NEEDS_IDLE("stages a race step by step on pinned CPUs, each step within 2 s");
    if (!enabled())
        return;
    kt_pin_self(0);
    waitqueue_init(&fs_wq, "repro fs wq");
    fs_go = 0;
    fs_target = NULL;
    fs_phase = 1;
    fs_seen_state = -1;
    __atomic_store_n(&dbg_hooks[DBG_FINISH_SWITCH], fs_hook, __ATOMIC_RELEASE);
    cpumask_t m;
    cpumask_one(&m, 1);
    struct thread *x = thread_create_on("repro-victim", fs_victim, NULL, PRIO_DEFAULT, &m);
    fs_target = x;
    KT_ASSERT(wait_for(&fs_phase, 2, 2000));   /* cpu 1 switched x out, now stalled */
    cpumask_one(&m, 2);
    thread_set_affinity(x, &m);
    uint64_t f = spin_lock_irqsave(&fs_lock);
    fs_go = 1;
    spin_unlock_irqrestore(&fs_lock, f);
    waitqueue_wake_one(&fs_wq);                /* x runs on cpu 2 and exits there */
    KT_ASSERT(wait_for(&fs_phase, 3, 2000));
    __atomic_store_n(&dbg_hooks[DBG_FINISH_SWITCH], NULL, __ATOMIC_RELEASE);
    thread_sleep_ms(5);
    kprintf("repro: cpu 1's finish_switch saw \"repro-victim\" in state %d (T_DEAD = %d)\n",
            fs_seen_state, T_DEAD);
    /* If it was reaped twice its stack is in the cache twice: the next two
     * threads get the same stack. Pinned to this CPU, lower priority, so
     * neither runs before the check. */
    cpumask_one(&m, 0);
    struct thread *y1 = thread_create_on("repro-y1", idle_fn, NULL, PRIO_MIN, &m);
    struct thread *y2 = thread_create_on("repro-y2", idle_fn, NULL, PRIO_MIN, &m);
    /* finish_switch reads prev->state BEFORE clearing on_cpu, so it never
     * acts on the recycled T_DEAD it can observe late (the hook reads that
     * late value on purpose, showing the window exists) and does not reap
     * the exited thread a second time. If it had, the two stacks would be
     * one. */
    KT_ASSERT(y1->stack_top != y2->stack_top);
    thread_join(y1);
    thread_join(y2);
    kt_unpin_self();
}

/* ---- 3. thread_wake locks a run queue chosen from a stale t->cpu ------------ */

static struct thread *volatile ab_target;
static volatile int ab_phase;
static volatile uint64_t ab_wakes;
static spinlock_t ab_lock = SPINLOCK_INIT("repro ab");
static struct thread *volatile ab_stale_waker;

static void ab_sched_hook(void *arg)
{
    struct thread *prev = arg;
    if (prev != ab_target || thread_state(prev) != T_BLOCKED)
        return;
    uint32_t me = this_cpu()->index;   /* rq lock held: IRQs off */
    if (ab_phase == 1 && me == 1) {
        ab_phase = 2;     /* on cpu 1, blocked, on_cpu still set */
        udelay(3000);
    } else if (ab_phase == 4 && me == 3) {
        ab_phase = 5;     /* on cpu 3 inside schedule(), already read T_BLOCKED */
        udelay(5000);
    }
}

static void ab_wake_hook(void *arg)
{
    if (arg != ab_target || current_thread() != ab_stale_waker || ab_phase != 2)
        return;
    ab_phase = 3;
    uint64_t end = rdtsc() + tsc_hz;
    while (ab_phase != 5 && rdtsc() < end)   /* a slow lock acquisition */
        cpu_relax();
}

static volatile int ab_stop;

static void ab_victim(void *arg)
{
    (void)arg;
    while (!ab_target)
        cpu_relax();
    while (!ab_stop) {
        uint64_t f = spin_lock_irqsave(&ab_lock);
        thread_block(&ab_lock, &f, DEADLINE_NEVER);   /* callers tolerate spurious wakes */
        ab_wakes++;
        spin_unlock_irqrestore(&ab_lock, f);
    }
}

static void ab_stale(void *arg)
{
    (void)arg;
    KT_ASSERT(wait_for(&ab_phase, 2, 2000));
    thread_wake(ab_target);   /* e.g. wake_sleepers: a deadline that just expired */
}

KTEST(repro_wake_stale_cpu)
{
    KT_NEEDS_IDLE("stages a race step by step on pinned CPUs, each step within 2 s");
    if (!enabled())
        return;
    kt_pin_self(0);
    ab_phase = 0;
    ab_stop = 0;
    ab_target = NULL;
    __atomic_store_n(&dbg_hooks[DBG_SCHED_PREV], ab_sched_hook, __ATOMIC_RELEASE);
    __atomic_store_n(&dbg_hooks[DBG_WAKE_ONCPU], ab_wake_hook, __ATOMIC_RELEASE);
    cpumask_t m;
    cpumask_one(&m, 2);
    ab_stale_waker = thread_create_on("repro-stale", ab_stale, NULL, PRIO_DEFAULT, &m);
    ab_phase = 1;
    cpumask_one(&m, 1);
    ab_target = thread_create_on("repro-aba", ab_victim, NULL, PRIO_DEFAULT, &m);
    KT_ASSERT(wait_for(&ab_phase, 3, 2000));   /* stale waker read t->cpu = 1 */
    while (__atomic_load_n(&ab_target->on_cpu, __ATOMIC_RELAXED))
        cpu_relax();
    cpumask_one(&m, 3);
    thread_set_affinity(ab_target, &m);
    ab_phase = 4;
    thread_wake(ab_target);   /* the real wakeup: runs on cpu 3, blocks again */
    thread_join(ab_stale_waker);
    __atomic_store_n(&dbg_hooks[DBG_SCHED_PREV], NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&dbg_hooks[DBG_WAKE_ONCPU], NULL, __ATOMIC_RELEASE);
    thread_sleep_ms(20);

    uint64_t before = ab_wakes;
    for (int i = 0; i < 5; i++) {
        thread_wake(ab_target);
        thread_sleep_ms(10);
    }
    struct thread *t = ab_target;
    bool anywhere = false;
    for (uint32_t i = 0; i < cpu_count; i++)
        anywhere |= cpus[i]->current == t;
    kprintf("repro: \"%s\" state %d on_cpu %d queued %d current-somewhere %d, "
            "wakes %lu -> %lu\n", t->name, thread_state(t),
            __atomic_load_n(&t->on_cpu, __ATOMIC_RELAXED), t->rq_node.next != NULL,
            anywhere, before, ab_wakes);
    (void)anywhere;
    /* thread_wake re-reads t->cpu under the run queue lock and retries if
     * it moved, so a stale waker can't mark the thread RUNNING under the
     * wrong CPU's lock and strand it. Every later wake lands. */
    KT_ASSERT(ab_wakes > before);
    /* Retire the victim so it doesn't leak its stack for the whole run. */
    ab_stop = 1;
    thread_wake(ab_target);
    thread_join(ab_target);
    kt_unpin_self();
}

/* ---- 4. kernel unmap: the CPU the caller migrates to keeps a stale TLB ---- */

static volatile uint64_t *tlb_va;
static volatile uint64_t tlb_seen;
static volatile int tlb_hook_preempt = -1;

static void tlb_read(void *arg)
{
    (void)arg;
    tlb_seen = *tlb_va;
}

/* Fired between vmm_unmap's local flush and the remote shootdown.
 * Preemption is disabled across that window, so it records preempt_count
 * (> 0 proves migration cannot slip in and strand a CPU with a stale entry).
 * It cannot force a migration itself: schedule() with preemption disabled is
 * a hard error, which is exactly the guarantee we are checking. */
static void tlb_unmap_hook(void *arg)
{
    (void)arg;
    tlb_hook_preempt = (int)this_cpu()->preempt_count;
}

KTEST(repro_unmap_migrate_stale_tlb)
{
    if (!enabled())
        return;
    kt_pin_self(1);
    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t va = vmm_reserve(PAGE_SIZE);
    uint64_t pa1 = pmm_alloc_page_phys(PMM_ZERO);
    uint64_t pa2 = pmm_alloc_page_phys(PMM_ZERO);
    *(uint64_t *)phys_to_virt(pa1) = 0xfeedface;
    *(uint64_t *)phys_to_virt(pa2) = 0xc0ffee;

    vmm_map(pml4, va, pa1, PAGE_SIZE, VM_WRITE | VM_GLOBAL | VM_SMALL);
    tlb_va = (volatile uint64_t *)va;
    KT_EQ(*(volatile uint64_t *)va, 0xfeedface);   /* cpu 1 caches the translation */
    smp_call_on(2, tlb_read, NULL);                /* cpu 2 caches it too */
    KT_EQ(tlb_seen, 0xfeedface);

    tlb_hook_preempt = -1;
    __atomic_store_n(&dbg_hooks[DBG_UNMAP_PRE_SHOOT], tlb_unmap_hook, __ATOMIC_RELEASE);
    vmm_unmap(pml4, va, PAGE_SIZE);
    __atomic_store_n(&dbg_hooks[DBG_UNMAP_PRE_SHOOT], NULL, __ATOMIC_RELEASE);
    /* Preemption was held across the flush + shootdown: no migration could
     * strand a CPU with a stale entry. */
    KT_ASSERT(tlb_hook_preempt > 0);

    /* Remap the same VA to a different physical page and read it on both the
     * unmapping CPU and cpu 2. A stale TLB entry on either would still read
     * the old page's 0xfeedface; a correct global flush gives 0xc0ffee. */
    vmm_map(pml4, va, pa2, PAGE_SIZE, VM_WRITE | VM_GLOBAL | VM_SMALL);
    KT_EQ(*(volatile uint64_t *)va, 0xc0ffee);
    tlb_seen = 0;
    smp_call_on(2, tlb_read, NULL);
    KT_EQ(tlb_seen, 0xc0ffee);

    vmm_unmap(pml4, va, PAGE_SIZE);
    pmm_free_page_phys(pa1);
    pmm_free_page_phys(pa2);
    kt_unpin_self();
}

/* ---- 5. a thread whose slice ran out while alone is never sliced again ---- */

static volatile int rr_stop;
static volatile uint64_t rr_y_ran_ns;

static void rr_spinner(void *arg)
{
    (void)arg;
    while (!rr_stop)
        cpu_relax();
}

static void rr_late(void *arg)
{
    (void)arg;
    rr_y_ran_ns = uptime_ns();
}

KTEST(repro_slice_not_reset)
{
    KT_NEEDS_IDLE("measures the wait for a CPU that runs one spinner and nothing else");
    if (!enabled())
        return;
    kt_pin_self(0);
    rr_stop = 0;
    rr_y_ran_ns = 0;
    cpumask_t m;
    cpumask_one(&m, 1);
    struct thread *x = thread_create_on("repro-spin", rr_spinner, NULL, PRIO_DEFAULT, &m);
    thread_sleep_ms(100);   /* x alone on cpu 1: its 20 ms slice ran out long ago */
    uint64_t t0 = uptime_ns();
    struct thread *y = thread_create_on("repro-late", rr_late, NULL, PRIO_DEFAULT, &m);
    while (!rr_y_ran_ns && uptime_ns() - t0 < 5000000000ull)
        thread_sleep_ms(1);
    rr_stop = 1;
    thread_join(x);
    thread_join(y);
    uint64_t ms = (rr_y_ran_ns - t0) / 1000000;
    kprintf("repro: same-priority thread waited %lu ms for a CPU running one spinner "
            "(slice is %u ticks = %u ms)\n", ms, SLICE_TICKS, SLICE_TICKS * 10);
    kt_unpin_self();
    /* schedule()'s next == prev path refreshes the slice, so a later
     * same-priority thread gets the CPU within a slice or two instead of
     * waiting ~1 s for the starvation boost. */
    KT_ASSERT(ms <= 100);
}

/* ---- 6. a CPU taking its next thread looks idle to placement -------------- */

/* schedule() takes the next thread off its queue (the queued count drops)
 * before it marks it running. If the CPU were marked busy only then, for a
 * moment a CPU starting work would have load 0: a placement on another CPU
 * reading it then would take it for a whole idle core and queue a second
 * thread behind the first (two busy threads on one core). The hook sits in
 * that moment and reads the load placement would see. */
static struct thread *volatile pk_marker;
static volatile int pk_seen;
static volatile uint32_t pk_load;

static void pk_hook(void *arg)
{
    if (!arg || arg != pk_marker || pk_seen)
        return;
    preempt_disable();
    pk_load = sched_cpu_load(this_cpu()->index);
    preempt_enable_no_resched();
    pk_seen = 1;
}

static void pk_body(void *arg)
{
    (void)arg;
}

KTEST(repro_picked_cpu_looks_idle)
{
    KT_NEEDS_IDLE("needs an idle CPU to wake a thread onto");
    if (!enabled())
        return;
    uint32_t cpu = cpu_count - 1;
    cpumask_t m;
    cpumask_one(&m, cpu);
    for (int round = 0; round < 3; round++) {
        pk_seen = 0;
        pk_load = 99;
        struct thread *t = thread_try_create_suspended("repro-picked", pk_body, NULL,
                                                       PRIO_DEFAULT, &m, PRIO_MAX);
        KT_ASSERT(t != NULL);
        pk_marker = t;
        __atomic_store_n(&dbg_hooks[DBG_SCHED_PICKED], pk_hook, __ATOMIC_RELEASE);
        thread_wake(t);
        thread_join(t);
        __atomic_store_n(&dbg_hooks[DBG_SCHED_PICKED], NULL, __ATOMIC_RELEASE);
        pk_marker = NULL;
        kprintf("repro: cpu %u taking its next thread: load %u as placement reads it\n", cpu,
                pk_load);
        KT_ASSERT(pk_seen);
        /* Busy from before the thread leaves the queue: at least 1 (live,
         * others may be queued there too). */
        KT_ASSERT(pk_load >= 1);
    }
}
