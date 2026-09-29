/* M5.5 performance pass: spin before idle, placement, per-CPU kmalloc
 * magazines, per-CPU one-shot timers, the serial transmit ring, PCIDs. */
#include <jam/aspace.h>
#include <jam/cpu.h>
#include <jam/ipi.h>
#include <jam/object.h>
#include <jam/pcid.h>
#include <jam/vmo.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/serial.h>
#include <jam/time.h>
#include <jam/x86.h>

#define MS 1000000ull

static uint32_t pin_self(uint32_t cpu)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    thread_set_affinity(current_thread(), &m);
    return cpu;
}

static void unpin_self(void)
{
    cpumask_t m;
    cpumask_all(&m);
    thread_set_affinity(current_thread(), &m);
}

/* Read a user address in the address space loaded on this CPU (as
 * test_aspace.c does: stac/clac around it when the CPU has SMAP). */
static uint64_t user_peek(uint64_t va)
{
    bool smap = cpu_features.smap;
    if (smap)
        __asm__ volatile("stac" ::: "memory");
    uint64_t v = *(volatile uint64_t *)va;
    if (smap)
        __asm__ volatile("clac" ::: "memory");
    return v;
}

/* ---- spin before idle ---------------------------------------------------- */

#define PP_ROUNDS 200

static struct {
    spinlock_t       lock;
    struct waitqueue wq;
    volatile int     turn;   /* 0: pinger's move, 1: ponger's */
    volatile bool    stop;
} pp;

static volatile uint32_t pp_ran_on[MAX_CPUS];
/* When set, the pinger waits (up to 10 ms) for this CPU to be polling
 * before each wake, so the test doesn't depend on who wins that race. */
static struct cpu *volatile pp_wait_polling;

static void ponger(void *arg)
{
    (void)arg;
    uint64_t f = spin_lock_irqsave(&pp.lock);
    for (;;) {
        while (pp.turn != 1 && !pp.stop)
            waitqueue_wait(&pp.wq, &pp.lock, &f);
        if (pp.stop)
            break;
        pp_ran_on[this_cpu()->index]++;   /* interrupts are off: this CPU */
        pp.turn = 0;
        waitqueue_wake_all(&pp.wq);
    }
    spin_unlock_irqrestore(&pp.lock, f);
}

/* PP_ROUNDS block+wake round trips between this thread (on cpu a) and one
 * allowed on `mb`; pp_ran_on counts where the other one ran. Returns the
 * other thread's pair placements. */
static uint64_t pingpong_mask(uint32_t a, const cpumask_t *mb)
{
    pin_self(a);
    spin_init(&pp.lock, "m55 pingpong");
    waitqueue_init(&pp.wq, "m55 pingpong waiters");
    pp.turn = 0;
    pp.stop = false;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        pp_ran_on[i] = 0;
    struct thread *t = thread_create_on("m55-pong", ponger, NULL, PRIO_DEFAULT + 2, mb);
    uint64_t f = spin_lock_irqsave(&pp.lock);
    for (int i = 0; i < PP_ROUNDS; i++) {
        if (pp_wait_polling) {
            uint64_t until = uptime_ns() + 10 * MS;
            while (!pp_wait_polling->idle_polling && uptime_ns() < until)
                cpu_relax();
        }
        pp.turn = 1;
        waitqueue_wake_all(&pp.wq);
        while (pp.turn != 0)
            waitqueue_wait(&pp.wq, &pp.lock, &f);
    }
    pp.stop = true;
    waitqueue_wake_all(&pp.wq);
    spin_unlock_irqrestore(&pp.lock, f);
    uint64_t pairs = t->pair_wakes;   /* we still hold a reference */
    thread_join(t);
    unpin_self();
    return pairs;
}

/* The same with the other thread pinned to cpu b; returns the polled
 * wakeups cpu b saw meanwhile. */
static uint64_t pingpong(uint32_t a, uint32_t b)
{
    cpumask_t m;
    cpumask_one(&m, b);
    uint64_t polled0 = cpus[b]->polled_wakes;
    pingpong_mask(a, &m);
    return cpus[b]->polled_wakes - polled0;
}

/* With a long spin window, the ponger's CPU is polling whenever it is woken:
 * nearly every wakeup skips the IPI. With the spin off none can. */
KTEST(spin_idle_skips_ipi)
{
    if (cpu_count < 3)
        return;
    uint64_t keep = sched_idle_spin_ns;
    sched_idle_spin_ns = 50 * MS;   /* QEMU is slow: make the window cover a round trip */
    pp_wait_polling = cpus[2];      /* wake only once cpu 2 is in its window */
    uint64_t polled_on = pingpong(1, 2);
    pp_wait_polling = NULL;
    sched_idle_spin_ns = 0;
    thread_sleep_ms(60);   /* let the window cpu 2 already opened run out */
    uint64_t polled_off = pingpong(1, 2);
    sched_idle_spin_ns = keep;
    kprintf("spin-idle: %lu of %d wakeups polled with a 50 ms window, %lu with none\n",
            polled_on, PP_ROUNDS, polled_off);
    /* Alone in QEMU ~90% poll; with the Mac busy, TCG's vCPUs stall and
     * the window runs out in host time (~1/3). The PC benchmark measures
     * the real share; here the point is "many" against "none". */
    KT_ASSERT(polled_on >= PP_ROUNDS / 10);
    KT_EQ(polled_off, 0);
}

/* ---- hybrid placement order ---------------------------------------------- */

/* The PC's shape: cpus 0-15 are 8 P-cores with Hyper-Threading (0/1, 2/3,
 * ...), 16-27 are 12 E-cores. */
static int16_t f_sib[MAX_CPUS];
static uint8_t f_type[MAX_CPUS];
static uint32_t f_load[MAX_CPUS];

static void fake_pc(void)
{
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        f_sib[i] = i < 16 ? (int16_t)(i ^ 1) : -1;
        f_type[i] = i < 16 ? CORE_PERFORMANCE : CORE_EFFICIENCY;
        f_load[i] = 0;
    }
}

static cpumask_t f_mask(uint32_t from, uint32_t to)
{
    cpumask_t m = { { 0 } };
    for (uint32_t i = from; i <= to; i++)
        m.bits[i / 64] |= 1ull << (i % 64);
    return m;
}

static uint32_t f_pick(const cpumask_t *m, uint32_t last)
{
    return sched_pick_cpu_fake(m, f_sib, f_type, f_load, last, true);
}

KTEST(placement_order_fake_topology)
{
    fake_pc();
    cpumask_t all = f_mask(0, 27), not0 = f_mask(1, 27);
    f_load[0] = 1;   /* cpu 0 busy: its sibling, cpu 1, is only half a core */
    KT_EQ(f_pick(&not0, 999), 2);    /* 0: the first whole idle P-core */
    KT_EQ(f_pick(&not0, 7), 7);      /* ...its last CPU if that is one */
    KT_EQ(f_pick(&not0, 1), 2);      /* a last CPU in a worse class loses */
    /* One thread on every P-core: an idle E-core comes next. */
    for (uint32_t i = 0; i < 16; i += 2)
        f_load[i] = 1;
    KT_EQ(f_pick(&all, 999), 16);
    KT_EQ(f_pick(&all, 21), 21);
    KT_EQ(f_pick(&all, 3), 16);      /* an idle E-core beats the old HT sibling */
    /* E-cores full too: the idle HT sibling of a busy P-core. */
    for (uint32_t i = 16; i < 28; i++)
        f_load[i] = 1;
    KT_EQ(f_pick(&all, 999), 1);
    KT_EQ(f_pick(&all, 5), 5);
    /* Everything busy: least loaded, ties to P-cores, then the last CPU. */
    for (uint32_t i = 0; i < 28; i++)
        f_load[i] = 2;
    f_load[20] = 1;
    KT_EQ(f_pick(&all, 999), 20);
    f_load[20] = 2;
    KT_EQ(f_pick(&all, 999), 0);
    KT_EQ(f_pick(&all, 9), 9);
    /* Affinity limits the choice: a whole core beats an idle E-core. */
    fake_pc();
    cpumask_t two = f_mask(3, 3);
    two.bits[0] |= 1ull << 20;
    KT_EQ(f_pick(&two, 20), 3);
    f_load[2] = 1;                   /* now cpu 3 is only half a core */
    KT_EQ(f_pick(&two, 999), 20);
    /* Switched off: the M5 least-loaded rule fills CPUs in index order. */
    fake_pc();
    f_load[0] = 1;
    KT_EQ(sched_pick_cpu_fake(&not0, f_sib, f_type, f_load, 999, false), 1);
    /* No SMT, no hybrid (QEMU's default shape): any idle CPU is a whole core. */
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        f_sib[i] = -1;
        f_type[i] = CORE_UNKNOWN;
    }
    KT_EQ(f_pick(&not0, 999), 1);
}

static volatile bool spin_release;
static volatile uint32_t spin_started;

static void busy_spinner(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&spin_started, 1, __ATOMIC_RELAXED);
    while (!spin_release)
        cpu_relax();
}

/* Real placement: busy threads kept off cpu 0 (where this thread runs) each
 * get a core of their own while whole cores are idle. With SMT that means
 * never both hyperthreads of one core, and never cpu 0's sibling. */
KTEST(placement_spreads_over_cores)
{
    if (cpu_count < 4 || !sched_place_order)
        return;
    pin_self(0);
    uint32_t cores = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        bool first = true;
        for (uint32_t j = 0; j < i; j++)
            first &= cpus[j]->core_id != cpus[i]->core_id;
        cores += first;
    }
    uint32_t n = cores - 1;   /* every core but cpu 0's */
    if (n > 8)
        n = 8;
    struct thread *th[8];
    cpumask_t m;
    cpumask_all(&m);
    m.bits[0] &= ~1ull;
    spin_release = false;
    spin_started = 0;
    for (uint32_t k = 0; k < n; k++)
        th[k] = thread_create_on("m55-spin", busy_spinner, NULL, PRIO_DEFAULT - 1, &m);
    while (spin_started < n)
        thread_sleep_ms(1);
    uint32_t where[8];
    for (uint32_t k = 0; k < n; k++)
        where[k] = th[k]->cpu;
    spin_release = true;
    for (uint32_t k = 0; k < n; k++)
        thread_join(th[k]);
    unpin_self();
    for (uint32_t k = 0; k < n; k++) {
        kprintf("placement: busy thread %u on cpu %u (core %u)\n", k, where[k],
                cpus[where[k]]->core_id);
        KT_ASSERT(cpus[where[k]]->core_id != cpus[0]->core_id);
        for (uint32_t j = 0; j < k; j++)
            KT_ASSERT(cpus[where[j]]->core_id != cpus[where[k]]->core_id);
    }
}

/* ---- client/server pairs on sibling hyperthreads ------------------------- */

/* A thread pinned to cpu a and an unpinned partner (kept off cpu 0 and a)
 * wake each other in turn. As a pair, the partner is placed on a's idle HT
 * sibling; switched off, the hybrid order gives it a whole idle core. */
KTEST(affine_pair_uses_sibling)
{
    if (cpu_count < 4)
        return;
    uint32_t a = 0;
    int sib = -1;
    for (uint32_t i = 1; i < cpu_count && sib < 0; i++)
        for (uint32_t j = 1; j < cpu_count; j++)
            if (j != i && cpus[j]->core_id == cpus[i]->core_id) {
                a = i;
                sib = (int)j;
                break;
            }
    if (sib < 0) {
        kprintf("affine-pair: no HT siblings (QEMU needs -smp N,threads=2): not tested\n");
        return;
    }
    cpumask_t m;
    cpumask_all(&m);
    m.bits[0] &= ~1ull;
    m.bits[a / 64] &= ~(1ull << (a % 64));
    bool keep = sched_affine_pair;
    sched_affine_pair = true;
    uint64_t pairs_on = pingpong_mask(a, &m);
    uint32_t on_sib_on = pp_ran_on[sib];
    sched_affine_pair = false;
    uint64_t pairs_off = pingpong_mask(a, &m);
    uint32_t on_sib_off = pp_ran_on[sib];
    sched_affine_pair = keep;
    kprintf("affine-pair: partner of cpu %u ran on its sibling cpu %d for %u of %d rounds "
            "(%lu pair placements); switched off: %u rounds, %lu\n", a, sib, on_sib_on,
            PP_ROUNDS, pairs_on, on_sib_off, pairs_off);
    KT_ASSERT(pairs_on >= PP_ROUNDS / 2);
    KT_ASSERT(on_sib_on >= PP_ROUNDS / 2);
    KT_EQ(pairs_off, 0);
    if (sched_place_order)
        KT_ASSERT(on_sib_off <= PP_ROUNDS / 4);   /* whole idle cores come first */
}

/* ---- per-CPU kmalloc magazines ---------------------------------------------- */

/* Free pages plus those in the thread stack cache (the helper threads'
 * stacks move between the two), as the ktest leak check counts them. */
static uint64_t free_now(void)
{
    uint64_t total, free;
    pmm_stats(&total, &free);   /* drains the magazines first */
    return free + sched_stack_cache_pages();
}

/* Private caches, so nothing else touches their slabs: 248-byte objects
 * make exactly 16 per one-page slab ((4096 - 64) / 248). */
static struct kmem_cache *mag_cache, *oom_cache;

static struct kmem_cache *private_cache(struct kmem_cache **c, const char *name)
{
    if (!*c)
        *c = kmem_cache_create(name, 248, 8);   /* caches are never destroyed */
    return *c;
}

static void *xfer_obj;
static struct kmem_cache *xfer_cache;

static void free_it(void *arg)
{
    (void)arg;
    kmem_cache_free(xfer_cache, xfer_obj);
}

KTEST(kmag_refill_drain_accounting)
{
    if (!heap_percpu)
        return;
    struct kmem_cache *c = private_cache(&mag_cache, "m55 magazine");
    uint32_t me = pin_self(cpu_count > 1 ? 1 : 0);
    uint64_t free0 = free_now();
    KT_EQ(kmem_cached_objects(me, c), 0);
    void *o[17];
    /* Empty magazine, no slab: one new slab, a batch into the magazine, and
     * its top handed out. */
    o[0] = kmem_cache_alloc(c);
    KT_ASSERT(o[0]);
    KT_EQ(kmem_cached_objects(me, c), MAG_BATCH - 1);
    for (int i = 1; i < 16; i++) {
        o[i] = kmem_cache_alloc(c);
        KT_ASSERT(o[i]);
    }
    /* Two refills emptied the slab into the magazine and out again. */
    KT_EQ(kmem_cached_objects(me, c), 0);
    o[16] = kmem_cache_alloc(c);   /* no partial slab left: a second one */
    KT_EQ(kmem_cached_objects(me, c), MAG_BATCH - 1);
    /* Exact accounting: the stats drain the 7 parked objects, and the two
     * slabs with live objects are what is used. */
    KT_EQ(free_now(), free0 - 2);
    KT_EQ(kmem_cached_objects(me, c), 0);
    /* Frees fill the magazine; the one that finds it full drains a batch. */
    for (int i = 0; i < 17; i++)
        kmem_cache_free(c, o[i]);
    KT_EQ(kmem_cached_objects(me, c), MAG_MAX - MAG_BATCH + 1);
    KT_EQ(free_now(), free0);   /* drained: both slabs empty and given back */

    /* An object freed on another CPU goes into that CPU's magazine. */
    if (cpu_count > 2) {
        xfer_cache = c;
        xfer_obj = kmem_cache_alloc(c);
        cpumask_t m;
        cpumask_one(&m, 2);
        uint64_t there = kmem_cached_objects(2, c);
        thread_join(thread_create_on("m55-free", free_it, NULL, PRIO_DEFAULT, &m));
        KT_EQ(kmem_cached_objects(2, c), there + 1);
        KT_EQ(free_now(), free0);
        KT_EQ(kmem_cached_objects(2, c), 0);
    }
    unpin_self();
}

static void park_16(void *arg)
{
    (void)arg;
    void *o[16];
    for (int i = 0; i < 16; i++)
        o[i] = kmem_cache_alloc(oom_cache);
    for (int i = 0; i < 16; i++)
        kmem_cache_free(oom_cache, o[i]);
}

/* A whole slab's objects parked in another CPU's magazine, and no free page
 * for a new slab: the allocation drains every magazine (emptying that slab)
 * and succeeds. */
KTEST(kmag_oom_drains_magazines)
{
    if (!heap_percpu || cpu_count < 3)
        return;
    struct kmem_cache *c = private_cache(&oom_cache, "m55 magazine oom");
    uint64_t free0 = free_now();
    cpumask_t m;
    cpumask_one(&m, 2);
    thread_join(thread_create_on("m55-park", park_16, NULL, PRIO_DEFAULT, &m));
    thread_sleep_ms(20);   /* let the helper be reaped (its stack is cached) */
    KT_EQ(kmem_cached_objects(2, c), 16);
    pin_self(1);
    /* Take every page (chained through struct page, no memory touched).
     * The other CPUs are idle between tests. */
    struct page *chain = NULL, *p;
    while ((p = pmm_alloc_pages(0, 0))) {
        p->private = (uint64_t)chain;
        chain = p;
    }
    void *obj = kmem_cache_alloc(c);
    uint64_t left = kmem_cached_objects(2, c);
    while (chain) {
        struct page *next = (struct page *)chain->private;
        chain->private = 0;
        pmm_free_pages(chain, 0);
        chain = next;
    }
    unpin_self();
    KT_ASSERT(obj);
    KT_EQ(left, 0);
    kmem_cache_free(c, obj);
    KT_EQ(free_now(), free0);
}

/* ---- per-CPU one-shot timers --------------------------------------------- */

#define NSLEEP 5
/* QEMU's emulated APIC timer and slow TCG wakeups make ~1.5 ms errors
 * common there; the tick's are up to 10 ms. The PC is measured by the
 * benchmark ("sleep ... wake-up error" lines: tens of us expected). */
#define ONESHOT_BOUND (5 * MS)
static const uint64_t sleep_ms[NSLEEP] = { 9, 1, 7, 3, 5 };
static uint64_t sleep_base;
static volatile uint32_t sleep_seq;
static uint32_t sleep_rank[NSLEEP];
static int64_t sleep_err[NSLEEP];

static volatile bool sleep_go;
static struct waitqueue sleep_wq;
static spinlock_t sleep_lock_t = SPINLOCK_INIT("m55 sleepers go");

static void sleeper(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    uint64_t f = spin_lock_irqsave(&sleep_lock_t);
    while (!sleep_go)   /* thread creation is slow in QEMU: start together */
        waitqueue_wait(&sleep_wq, &sleep_lock_t, &f);
    spin_unlock_irqrestore(&sleep_lock_t, f);
    uint64_t deadline = sleep_base + sleep_ms[i] * MS;
    uint64_t now = uptime_ns();
    if (now < deadline)
        thread_sleep_ns(deadline - now);
    uint64_t woke = uptime_ns();
    sleep_rank[i] = __atomic_fetch_add(&sleep_seq, 1, __ATOMIC_RELAXED);
    sleep_err[i] = (int64_t)(woke - deadline);
}

/* Five threads on one CPU sleep to deadlines queued out of order: they wake
 * in deadline order, and (with one-shot timers) each within ONESHOT_BOUND of its
 * deadline, where CPU 0's 10 ms tick used to be the resolution. */
KTEST(oneshot_timer_order_and_accuracy)
{
    uint32_t cpu = cpu_count > 1 ? 1 : 0;
    cpumask_t m;
    cpumask_one(&m, cpu);
    struct thread *th[NSLEEP];
    sleep_seq = 0;
    sleep_go = false;
    waitqueue_init(&sleep_wq, "m55 sleepers");
    for (uint32_t i = 0; i < NSLEEP; i++)
        th[i] = thread_create_on("m55-sleep", sleeper, (void *)(uintptr_t)i, PRIO_DEFAULT + 4,
                                 &m);
    uint64_t f = spin_lock_irqsave(&sleep_lock_t);
    sleep_base = uptime_ns() + 10 * MS;
    sleep_go = true;
    spin_unlock_irqrestore(&sleep_lock_t, f);
    waitqueue_wake_all(&sleep_wq);
    for (uint32_t i = 0; i < NSLEEP; i++)
        thread_join(th[i]);
    bool oneshot = lapic_timer_has_oneshot() && lapic_oneshot;
    for (uint32_t i = 0; i < NSLEEP; i++)
        kprintf("oneshot: %lu ms sleeper woke %ld us late (rank %u)%s\n", sleep_ms[i],
                sleep_err[i] / 1000, sleep_rank[i], oneshot ? "" : " (tick resolution)");
    for (uint32_t i = 0; i < NSLEEP; i++) {
        uint32_t earlier = 0;
        for (uint32_t j = 0; j < NSLEEP; j++)
            earlier += sleep_ms[j] < sleep_ms[i];
        KT_EQ(sleep_rank[i], earlier);
        KT_ASSERT(sleep_err[i] >= 0);   /* never early */
        if (oneshot)
            KT_ASSERT(sleep_err[i] < (int64_t)ONESHOT_BOUND);
    }
}

/* A sleeper whose wait ends early (woken by something else) leaves its
 * CPU's queue, and the queue keeps working for the ones still on it. */
static struct waitqueue early_wq;
static spinlock_t early_lock;
static volatile bool early_flag;
static volatile uint64_t early_woke;

static void early_sleeper(void *arg)
{
    (void)arg;
    uint64_t f = spin_lock_irqsave(&early_lock);
    uint64_t deadline = uptime_ns() + 1000 * MS;
    while (!early_flag && uptime_ns() < deadline)
        waitqueue_wait_until(&early_wq, &early_lock, &f, deadline);
    spin_unlock_irqrestore(&early_lock, f);
    early_woke = uptime_ns();
}

KTEST(oneshot_timer_early_wake_leaves_queue)
{
    uint32_t cpu = cpu_count > 1 ? 1 : 0;
    cpumask_t m;
    cpumask_one(&m, cpu);
    spin_init(&early_lock, "m55 early");
    waitqueue_init(&early_wq, "m55 early waiters");
    early_flag = false;
    early_woke = 0;
    struct thread *t = thread_create_on("m55-early", early_sleeper, NULL, PRIO_DEFAULT, &m);
    thread_sleep_ms(5);
    uint64_t f = spin_lock_irqsave(&early_lock);
    early_flag = true;
    spin_unlock_irqrestore(&early_lock, f);
    uint64_t t0 = uptime_ns();
    waitqueue_wake_all(&early_wq);
    thread_join(t);
    KT_ASSERT(early_woke - t0 < 500 * MS);   /* the wake, not the 1 s deadline */
    /* The queue it left still wakes a later sleeper on that CPU on time. */
    sleep_seq = 0;
    sleep_go = true;
    sleep_base = uptime_ns();
    struct thread *s = thread_create_on("m55-sleep", sleeper, (void *)(uintptr_t)1, PRIO_DEFAULT,
                                        &m);
    thread_join(s);
    KT_ASSERT(sleep_err[1] >= 0);
    if (lapic_timer_has_oneshot() && lapic_oneshot)
        KT_ASSERT(sleep_err[1] < (int64_t)ONESHOT_BOUND);
}

/* Review repro: a far-future deadline (anything whose TSC value overflows
 * 64 bits: INT64_MAX on a >2 GHz TSC, UINT64_MAX-1 even at 1 GHz) must not
 * wrap to the past. User code reaches this through nanosleep, object/port
 * waits and timer_set (which parks the prio-28 timer service on it). */
static volatile uint64_t far_blocks;
static uint64_t far_deadline;

static void far_sleeper(void *arg)
{
    (void)arg;
    const uint64_t deadline = far_deadline;   /* as sysc_nanosleep would loop */
    while (uptime_ns() < deadline) {
        __atomic_add_fetch(&far_blocks, 1, __ATOMIC_RELAXED);
        if (thread_block_cancellable(NULL, NULL, deadline) != OK)
            break;
    }
}

KTEST(oneshot_far_deadline_does_not_wrap)
{
    uint64_t now = rdtsc();
    kprintf("far-deadline: uptime_to_tsc(UINT64_MAX-1) = %lx, rdtsc = %lx\n",
            uptime_to_tsc(UINT64_MAX - 1), now);
    /* Pick a deadline ~2^64 ns out whose TSC value wraps to about one
     * second ago (the wrapped value moves down 1:1 with the deadline). */
    uint64_t w = uptime_to_tsc(UINT64_MAX - 1);
    far_deadline = UINT64_MAX - 1;
    if (w != UINT64_MAX && w > now)   /* wrapped: aim the wrap into the past */
        far_deadline -= (w - now) / tsc_hz * 1000000000ull + 1000000000ull;
    kprintf("far-deadline: deadline %lx ns -> tsc %lx\n", far_deadline,
            uptime_to_tsc(far_deadline));
    uint32_t cpu = cpu_count > 1 ? 1 : 0;
    cpumask_t m;
    cpumask_one(&m, cpu);
    far_blocks = 0;
    struct thread *t = thread_create_on("m55-far", far_sleeper, NULL, PRIO_DEFAULT, &m);
    thread_sleep_ms(100);
    uint64_t n = __atomic_load_n(&far_blocks, __ATOMIC_RELAXED);
    thread_cancel(t);
    thread_join(t);
    kprintf("far-deadline: sleeper blocked %lu times in 100 ms\n", n);
    KT_ASSERT(uptime_to_tsc(far_deadline) > now);
    KT_ASSERT(n <= 2);
}

/* ---- serial transmit ring ------------------------------------------------------ */

/* The ring drops (and counts) what doesn't fit, never overwrites, and keeps
 * order across wrap-around. */
KTEST(serial_ring_drops_when_full)
{
    char buf[8];
    struct serial_ring r = { buf, sizeof(buf), 0xfffffff0u, 0xfffffff0u, 0 };   /* near wrap */
    for (int i = 0; i < 10; i++)
        serial_ring_put(&r, (char)('a' + i));
    KT_EQ(serial_ring_used(&r), 8);
    KT_EQ(r.dropped, 2);
    for (int i = 0; i < 8; i++)
        KT_EQ(serial_ring_get(&r), 'a' + i);
    KT_EQ(serial_ring_get(&r), -1);
    for (int round = 0; round < 100; round++) {   /* head and tail wrap 32 bits */
        for (int i = 0; i < 5; i++)
            KT_ASSERT(serial_ring_put(&r, (char)i));
        for (int i = 0; i < 5; i++)
            KT_EQ(serial_ring_get(&r), i);
    }
    KT_EQ(r.dropped, 2);
}

/* Queued output is sent by the UART's transmit interrupt (QEMU emulates
 * COM1 and its IRQ 4). */
KTEST(serial_irq_drains_ring)
{
    if (!serial_is_async() || !serial_async)
        return;
    static const char line[] = "serial: this line was sent by the transmit interrupt\n";
    serial_test_hold(true);
    uint32_t before = serial_pending();
    serial_write(line, sizeof(line) - 1);
    uint32_t queued = serial_pending() - before;
    uint64_t irqs0 = serial_irqs;
    serial_test_hold(false);
    uint64_t deadline = uptime_ns() + 2000 * MS;
    while (serial_pending() && uptime_ns() < deadline)
        thread_sleep_ms(1);
    uint32_t left = serial_pending();   /* before our own kprintf queues more */
    uint64_t irqs = serial_irqs - irqs0;
    kprintf("serial: %u bytes queued, drained with %lu interrupts (%lu tick rescues so far)\n",
            queued, irqs, serial_rescues);
    KT_EQ(queued, sizeof(line));   /* the newline went out as \r\n */
    KT_EQ(left, 0);
    KT_ASSERT(irqs > 0);
}

/* ---- PCIDs ------------------------------------------------------------------ */

#define KEEP PCID_TEST_KEEP

/* The per-CPU slot bookkeeping, on made-up CPUs (QEMU's TCG has no PCIDs,
 * so this is the only way it runs there). */
KTEST(pcid_slot_bookkeeping)
{
    /* First load of an address space on a CPU: a slot, flushed. */
    KT_EQ(pcid_test_decide(0, 1000, 1, true), 1);
    KT_EQ(pcid_test_decide(0, 1000, 1, true), 1 | KEEP);   /* same generation: kept */
    KT_EQ(pcid_test_decide(0, 1000, 2, true), 1);          /* entries changed: flushed */
    KT_EQ(pcid_test_decide(0, 1000, 2, true), 1 | KEEP);
    KT_EQ(pcid_test_decide(0, 1001, 7, true), 2);          /* another one, another slot */
    KT_EQ(pcid_test_decide(0, 1000, 2, true), 1 | KEEP);   /* the first kept its entries */
    KT_EQ(pcid_test_decide(0, 0, 0, true), 0 | KEEP);      /* the kernel's tables: PCID 0 */
    /* Slots are per CPU. */
    KT_EQ(pcid_test_decide(1, 1000, 2, true), 1);
    /* Eight more address spaces take every slot round robin: 1000 is gone
     * and comes back flushed, in a slot of its own. */
    for (uint64_t id = 2000; id < 2000 + PCID_SLOTS_PER_CPU; id++)
        KT_EQ(pcid_test_decide(0, id, 1, true) & KEEP, 0);
    uint32_t again = pcid_test_decide(0, 1000, 2, true);
    KT_EQ(again & KEEP, 0);
    KT_ASSERT(again >= 1 && again <= PCID_SLOTS_PER_CPU);
    /* Switched off: PCID 0, flushed on every load, as in M5. */
    KT_EQ(pcid_test_decide(2, 1000, 2, false), 0);
    KT_EQ(pcid_test_decide(2, 1000, 2, false), 0);
    KT_EQ(pcid_test_decide(2, 0, 0, false), 0);
    /* Flipping the real switch makes every CPU forget its slots. */
    if (pcid_usable()) {
        KT_EQ(pcid_test_decide(3, 1000, 2, true), 1);
        KT_EQ(pcid_test_decide(3, 1000, 2, true), 1 | KEEP);
        bool was = pcid_is_on();
        pcid_set(!was);
        pcid_set(was);
        /* Forgotten: flushed, in whichever slot comes next (the
         * round-robin position isn't reset, and needn't be: a flushing
         * load clears the slot's old entries). */
        uint32_t after = pcid_test_decide(3, 1000, 2, true);
        KT_EQ(after & KEEP, 0);
        KT_ASSERT(after >= 1 && after <= PCID_SLOTS_PER_CPU);
    }
}

/* A page mapped in two address spaces, each loaded and read on its own CPU,
 * which then switch AWAY (to another address space, then the kernel's
 * tables). A decommit interrupts neither CPU (neither is running them),
 * but when they load the address spaces again they must not use the old
 * translation: with PCIDs the TLB still holds it under their PCIDs, and
 * the generation check has to flush it (on the PC; in QEMU, without PCIDs,
 * every CR3 load flushes and this checks the M5 path). */
static struct aspace *pc_as[2], *pc_other;
static uint64_t pc_addr[2], pc_other_addr;
static volatile int pc_ready, pc_phase;
static volatile uint64_t pc_seen[2][2];
static uint64_t pc_flushes_before[2];

static void pcid_runner(void *arg)
{
    int i = (int)(uintptr_t)arg;
    preempt_disable();   /* stay on this CPU; interrupts stay on while waiting */
    uint64_t f = irq_save();
    aspace_switch(NULL, pc_as[i]);
    pc_seen[i][0] = user_peek(pc_addr[i]);   /* caches the translation */
    aspace_switch(pc_as[i], pc_other);
    (void)user_peek(pc_other_addr);
    aspace_switch(pc_other, NULL);
    irq_restore(f);
    pc_flushes_before[i] = pcid_flushed_loads(this_cpu()->index);
    __atomic_add_fetch(&pc_ready, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&pc_phase, __ATOMIC_ACQUIRE) < 1)
        cpu_relax();
    f = irq_save();
    aspace_switch(NULL, pc_as[i]);
    pc_seen[i][1] = user_peek(pc_addr[i]);
    aspace_switch(pc_as[i], NULL);
    irq_restore(f);
    preempt_enable();
}

KTEST(pcid_no_stale_translation_after_switching_away)
{
    if (cpu_count < 3)
        return;
    pin_self(0);
    struct vmo *v, *w;
    KT_EQ(vmo_create(PAGE_SIZE, 0, &v), OK);
    KT_EQ(vmo_create(PAGE_SIZE, 0, &w), OK);
    uint64_t old = 0x1111111111111111ull;
    KT_EQ(vmo_write(v, 0, &old, 8), OK);
    for (int i = 0; i < 2; i++) {
        KT_EQ(aspace_create(&pc_as[i]), OK);
        pc_addr[i] = 0x700000 + (uint64_t)i * 0x1000000;
        KT_EQ(aspace_map(pc_as[i], v, 0, PAGE_SIZE, ASPACE_READ | ASPACE_WRITE | ASPACE_FIXED,
                         &pc_addr[i]), OK);
        KT_EQ(aspace_fault(pc_as[i], pc_addr[i], ASPACE_READ), OK);
    }
    KT_EQ(aspace_create(&pc_other), OK);
    pc_other_addr = 0x900000;
    KT_EQ(aspace_map(pc_other, w, 0, PAGE_SIZE, ASPACE_READ | ASPACE_FIXED, &pc_other_addr), OK);
    KT_EQ(aspace_fault(pc_other, pc_other_addr, ASPACE_READ), OK);

    pc_ready = pc_phase = 0;
    struct thread *t[2];
    uint32_t cpu[2] = { 1, 2 };
    for (int i = 0; i < 2; i++) {
        cpumask_t m;
        cpumask_one(&m, cpu[i]);
        t[i] = thread_create_on("m55-pcid", pcid_runner, (void *)(uintptr_t)i, PRIO_DEFAULT,
                                &m);
    }
    while (__atomic_load_n(&pc_ready, __ATOMIC_ACQUIRE) < 2)
        thread_yield();
    uint64_t ipis[2] = { tlb_mask_flush_count(cpu[0]), tlb_mask_flush_count(cpu[1]) };
    KT_EQ(vmo_decommit(v, 0, PAGE_SIZE), OK);
    /* Neither CPU runs either address space now: nobody was interrupted. */
    KT_EQ(tlb_mask_flush_count(cpu[0]), ipis[0]);
    KT_EQ(tlb_mask_flush_count(cpu[1]), ipis[1]);
    /* Scribble on the freed page and fault a fresh zero page in behind
     * both mappings, so a stale translation reads garbage. */
    uint64_t decoy = pmm_alloc_page_phys(0);
    KT_ASSERT(decoy);
    *(volatile uint64_t *)phys_to_virt(decoy) = 0x2222222222222222ull;
    KT_EQ(aspace_fault(pc_as[0], pc_addr[0], ASPACE_READ), OK);
    KT_EQ(aspace_fault(pc_as[1], pc_addr[1], ASPACE_READ), OK);
    __atomic_store_n(&pc_phase, 1, __ATOMIC_RELEASE);
    thread_join(t[0]);
    thread_join(t[1]);
    bool on = pcid_is_on();
    for (int i = 0; i < 2; i++) {
        kprintf("pcid: cpu %u read %lx before, %lx after (PCIDs %s, %lu flushing loads since)\n",
                cpu[i], pc_seen[i][0], pc_seen[i][1], on ? "on" : "off",
                pcid_flushed_loads(cpu[i]) - pc_flushes_before[i]);
        KT_EQ(pc_seen[i][0], old);
        KT_EQ(pc_seen[i][1], 0);   /* the new zero page, not the old one */
        if (on)
            KT_ASSERT(pcid_flushed_loads(cpu[i]) > pc_flushes_before[i]);
    }
    pmm_free_page_phys(decoy);
    aspace_unref(pc_as[0]);
    aspace_unref(pc_as[1]);
    aspace_unref(pc_other);
    kobject_unref(vmo_kobject(v));
    kobject_unref(vmo_kobject(w));
    unpin_self();
}
