/* AUDIT REPROS. They only run with "repro" on the command line as well as
 * "ktest=repro_<name>", and each one ends in a panic that names the defect
 * when the defect is present. Hooks (jam/dbghook.h) only widen windows the
 * real code already has. */
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

void (*volatile dbg_hooks[DBG_N])(void *arg);

static bool enabled(void)
{
    if (!cmdline_has("repro")) {
        kprintf("ktest: %s skipped (needs \"repro\" on the command line)\n", ktest_current);
        return false;
    }
    KT_ASSERT(cpu_count >= 4);
    return true;
}

static uint32_t cur_cpu(void)
{
    preempt_disable();
    uint32_t c = this_cpu()->index;
    preempt_enable_no_resched();
    return c;
}

static void pin_self(uint32_t cpu)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    thread_set_affinity(current_thread(), &m);
    KT_EQ(cur_cpu(), cpu);
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
    pin_self(waker_cpu);
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
    cpumask_t all;
    cpumask_all(&all);
    thread_set_affinity(current_thread(), &all);
    if (local > 1000)
        panic("REPRO CONFIRMED: prio-28 thread woken on the waker's own CPU ran up to %lu us "
              "later (remote wake: %lu us); the preemption point was skipped because "
              "spin_unlock_irqrestore runs preempt_enable with interrupts still off",
              local, remote);
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
    fs_seen_state = prev->state;
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
    if (!enabled())
        return;
    pin_self(0);
    waitqueue_init(&fs_wq, "repro fs wq");
    fs_go = 0;
    fs_target = NULL;
    fs_phase = 1;
    fs_seen_state = -1;
    dbg_hooks[DBG_FINISH_SWITCH] = fs_hook;
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
    dbg_hooks[DBG_FINISH_SWITCH] = NULL;
    thread_sleep_ms(5);
    kprintf("repro: cpu 1's finish_switch saw \"repro-victim\" in state %d (T_DEAD = %d)\n",
            fs_seen_state, T_DEAD);
    /* If it was reaped twice its stack is in the cache twice: the next two
     * threads get the same stack. Pinned to this CPU, lower priority, so
     * neither runs before the check. */
    cpumask_one(&m, 0);
    struct thread *y1 = thread_create_on("repro-y1", idle_fn, NULL, PRIO_MIN, &m);
    struct thread *y2 = thread_create_on("repro-y2", idle_fn, NULL, PRIO_MIN, &m);
    if (y1->stack_top == y2->stack_top)
        panic("REPRO CONFIRMED: finish_switch read prev->state (%d = T_DEAD) after dropping "
              "the rq lock, reaped an exited thread a second time, and two new threads "
              "(\"%s\", \"%s\") now share the stack %p",
              fs_seen_state, y1->name, y2->name, y1->stack_top);
    KT_ASSERT(fs_seen_state != T_DEAD);
    thread_join(y1);
    thread_join(y2);
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
    if (prev != ab_target || prev->state != T_BLOCKED)
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

static void ab_victim(void *arg)
{
    (void)arg;
    while (!ab_target)
        cpu_relax();
    for (;;) {
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
    if (!enabled())
        return;
    pin_self(0);
    ab_phase = 0;
    ab_target = NULL;
    dbg_hooks[DBG_SCHED_PREV] = ab_sched_hook;
    dbg_hooks[DBG_WAKE_ONCPU] = ab_wake_hook;
    cpumask_t m;
    cpumask_one(&m, 2);
    ab_stale_waker = thread_create_on("repro-stale", ab_stale, NULL, PRIO_DEFAULT, &m);
    ab_phase = 1;
    cpumask_one(&m, 1);
    ab_target = thread_create_on("repro-aba", ab_victim, NULL, PRIO_DEFAULT, &m);
    KT_ASSERT(wait_for(&ab_phase, 3, 2000));   /* stale waker read t->cpu = 1 */
    while (ab_target->on_cpu)
        cpu_relax();
    cpumask_one(&m, 3);
    thread_set_affinity(ab_target, &m);
    ab_phase = 4;
    thread_wake(ab_target);   /* the real wakeup: runs on cpu 3, blocks again */
    thread_join(ab_stale_waker);
    dbg_hooks[DBG_SCHED_PREV] = NULL;
    dbg_hooks[DBG_WAKE_ONCPU] = NULL;
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
            "wakes %lu -> %lu\n", t->name, t->state, t->on_cpu, t->rq_node.next != NULL,
            anywhere, before, ab_wakes);
    if (ab_wakes == before && t->state == T_RUNNING && !t->on_cpu && !anywhere)
        panic("REPRO CONFIRMED: thread \"%s\" is lost: state T_RUNNING but on no CPU and "
              "no run queue, so every later thread_wake is ignored. A stale waker locked "
              "cpu 1's run queue (from t->cpu read before the lock) while the thread was "
              "switching out on cpu 3, and marked it running", t->name);
    KT_ASSERT(ab_wakes > before);
}

/* ---- 4. kernel unmap: the CPU the caller migrates to keeps a stale TLB ---- */

static volatile uint64_t *tlb_va;
static volatile uint64_t tlb_seen;
static volatile int tlb_armed;

static void tlb_read(void *arg)
{
    (void)arg;
    tlb_seen = *tlb_va;
}

static void tlb_unmap_hook(void *arg)
{
    (void)arg;
    if (!tlb_armed)
        return;
    tlb_armed = 0;
    /* A preemption right here that moves the caller to cpu 2. */
    cpumask_t m;
    cpumask_one(&m, 2);
    thread_set_affinity(current_thread(), &m);
}

KTEST(repro_unmap_migrate_stale_tlb)
{
    if (!enabled())
        return;
    pin_self(1);
    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t va = vmm_reserve(PAGE_SIZE);
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    *(uint64_t *)phys_to_virt(pa) = 0xfeedface;
    vmm_map(pml4, va, pa, PAGE_SIZE, VM_WRITE | VM_GLOBAL | VM_SMALL);
    tlb_va = (volatile uint64_t *)va;
    smp_call_on(2, tlb_read, NULL);   /* cpu 2 caches the translation */
    KT_EQ(tlb_seen, 0xfeedface);

    dbg_hooks[DBG_UNMAP_PRE_SHOOT] = tlb_unmap_hook;
    tlb_armed = 1;
    vmm_unmap(pml4, va, PAGE_SIZE);    /* starts on cpu 1, shoots down from cpu 2 */
    dbg_hooks[DBG_UNMAP_PRE_SHOOT] = NULL;
    KT_EQ(cur_cpu(), 2);
    /* The page is gone for every CPU now; this read must fault. */
    uint64_t v = *(volatile uint64_t *)va;
    panic("REPRO CONFIRMED: after vmm_unmap returned, cpu 2 still reads the unmapped "
          "kernel page (value %lx): the caller migrated from cpu 1 to cpu 2 between the "
          "local invlpg and tlb_shootdown, which skips the calling CPU", v);
}
