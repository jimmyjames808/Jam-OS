/* M5.5 performance pass: spin before idle, placement, per-CPU kmalloc
 * magazines, per-CPU one-shot timers, the serial transmit ring, PCIDs. */
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/sched.h>
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

/* ---- spin before idle ---------------------------------------------------- */

#define PP_ROUNDS 200

static struct {
    spinlock_t       lock;
    struct waitqueue wq;
    volatile int     turn;   /* 0: pinger's move, 1: ponger's */
    volatile bool    stop;
} pp;

static volatile uint32_t pp_ran_on[MAX_CPUS];

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
    uint64_t polled_on = pingpong(1, 2);
    sched_idle_spin_ns = 0;
    thread_sleep_ms(60);   /* let the window cpu 2 already opened run out */
    uint64_t polled_off = pingpong(1, 2);
    sched_idle_spin_ns = keep;
    kprintf("spin-idle: %lu of %d wakeups polled with a 50 ms window, %lu with none\n",
            polled_on, PP_ROUNDS, polled_off);
    KT_ASSERT(polled_on >= PP_ROUNDS / 2);
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
