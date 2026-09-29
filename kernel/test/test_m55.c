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

static void ponger(void *arg)
{
    (void)arg;
    uint64_t f = spin_lock_irqsave(&pp.lock);
    for (;;) {
        while (pp.turn != 1 && !pp.stop)
            waitqueue_wait(&pp.wq, &pp.lock, &f);
        if (pp.stop)
            break;
        pp.turn = 0;
        waitqueue_wake_all(&pp.wq);
    }
    spin_unlock_irqrestore(&pp.lock, f);
}

/* PP_ROUNDS block+wake round trips between this thread (on cpu a) and one
 * on cpu b; returns the polled wakeups cpu b saw meanwhile. */
static uint64_t pingpong(uint32_t a, uint32_t b)
{
    pin_self(a);
    spin_init(&pp.lock, "m55 pingpong");
    waitqueue_init(&pp.wq, "m55 pingpong waiters");
    pp.turn = 0;
    pp.stop = false;
    cpumask_t m;
    cpumask_one(&m, b);
    uint64_t polled0 = cpus[b]->polled_wakes;
    struct thread *t = thread_create_on("m55-pong", ponger, NULL, PRIO_DEFAULT + 2, &m);
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
    thread_join(t);
    uint64_t polled = cpus[b]->polled_wakes - polled0;
    unpin_self();
    return polled;
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
