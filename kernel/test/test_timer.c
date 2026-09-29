/* Sleeping with a deadline (kernel/sched/wait.c, per-CPU one-shot timers):
 * sleepers wake in deadline order and on time, one that leaves its queue
 * early leaves it working, and a far-future deadline doesn't wrap. */
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/lapic.h>
#include <jam/sched.h>
#include <jam/time.h>

/* ---- per-CPU one-shot timers --------------------------------------------- */

#define NSLEEP 5
/* QEMU's emulated APIC timer and slow TCG wakeups make ~1.5 ms errors
 * common there; the tick's are up to 10 ms. The PC is measured by the
 * benchmark ("sleep ... wake-up error" lines: tens of us expected). */
#define ONESHOT_BOUND (5 * NS_PER_MS)
static const uint64_t sleep_ms[NSLEEP] = { 9, 1, 7, 3, 5 };
static uint64_t sleep_base;
static volatile uint32_t sleep_seq;
static uint32_t sleep_rank[NSLEEP];
static int64_t sleep_err[NSLEEP];

static volatile bool sleep_go;
static struct waitqueue sleep_wq;
static spinlock_t sleep_lock_t = SPINLOCK_INIT("kt sleepers go");

static void sleeper(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    uint64_t f = spin_lock_irqsave(&sleep_lock_t);
    while (!sleep_go)   /* thread creation is slow in QEMU: start together */
        waitqueue_wait(&sleep_wq, &sleep_lock_t, &f);
    spin_unlock_irqrestore(&sleep_lock_t, f);
    uint64_t deadline = sleep_base + sleep_ms[i] * NS_PER_MS;
    uint64_t now = uptime_ns();
    if (now < deadline)
        thread_sleep_ns(deadline - now);
    uint64_t woke = uptime_ns();
    sleep_rank[i] = __atomic_fetch_add(&sleep_seq, 1, __ATOMIC_RELAXED);
    sleep_err[i] = (int64_t)(woke - deadline);
}

/* Five threads on one CPU sleep to deadlines queued out of order: they wake
 * in deadline order, and (with one-shot timers) each within ONESHOT_BOUND of its
 * deadline, far finer than the 10 ms tick a sleeper waits for without them. */
KTEST(oneshot_timer_order_and_accuracy)
{
    uint32_t cpu = cpu_count > 1 ? 1 : 0;
    cpumask_t m;
    cpumask_one(&m, cpu);
    struct thread *th[NSLEEP];
    sleep_seq = 0;
    sleep_go = false;
    waitqueue_init(&sleep_wq, "kt sleepers");
    for (uint32_t i = 0; i < NSLEEP; i++)
        th[i] = thread_create_on("kt-sleep", sleeper, (void *)(uintptr_t)i, PRIO_DEFAULT + 4,
                                 &m);
    uint64_t f = spin_lock_irqsave(&sleep_lock_t);
    sleep_base = uptime_ns() + 10 * NS_PER_MS;
    sleep_go = true;
    spin_unlock_irqrestore(&sleep_lock_t, f);
    waitqueue_wake_all(&sleep_wq);
    for (uint32_t i = 0; i < NSLEEP; i++)
        thread_join(th[i]);
    bool oneshot = lapic_timer_has_oneshot() && __atomic_load_n(&lapic_oneshot, __ATOMIC_RELAXED);
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
    uint64_t deadline = uptime_ns() + 1000 * NS_PER_MS;
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
    spin_init(&early_lock, "kt early");
    waitqueue_init(&early_wq, "kt early waiters");
    early_flag = false;
    early_woke = 0;
    struct thread *t = thread_create_on("kt-early", early_sleeper, NULL, PRIO_DEFAULT, &m);
    thread_sleep_ms(5);
    uint64_t f = spin_lock_irqsave(&early_lock);
    early_flag = true;
    spin_unlock_irqrestore(&early_lock, f);
    uint64_t t0 = uptime_ns();
    waitqueue_wake_all(&early_wq);
    thread_join(t);
    KT_ASSERT(early_woke - t0 < 500 * NS_PER_MS);   /* the wake, not the 1 s deadline */
    /* The queue it left still wakes a later sleeper on that CPU on time. */
    sleep_seq = 0;
    sleep_go = true;
    sleep_base = uptime_ns();
    struct thread *s = thread_create_on("kt-sleep", sleeper, (void *)(uintptr_t)1, PRIO_DEFAULT,
                                        &m);
    thread_join(s);
    KT_ASSERT(sleep_err[1] >= 0);
    if (lapic_timer_has_oneshot() && __atomic_load_n(&lapic_oneshot, __ATOMIC_RELAXED))
        KT_ASSERT(sleep_err[1] < (int64_t)ONESHOT_BOUND);
}

/* A far-future deadline (anything whose TSC value overflows
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
    /* The unsaturated conversion, exactly (128-bit) and as a plain 64-bit
     * computation gets it (wrapping mod 2^64). */
    uint64_t ns = UINT64_MAX - 1;
    /* uptime_to_tsc(0) = the boot TSC */
    unsigned __int128 exact = (unsigned __int128)(ns / 1000000000ull) * tsc_hz + uptime_to_tsc(0);
    far_deadline = ns;
    if (exact > UINT64_MAX) {
        /* This TSC is fast enough to wrap: pick a deadline whose wrapped
         * value lands about one second ago (it moves 1:1 with the
         * deadline's whole seconds). A slower TSC (QEMU can run below
         * 2 GHz) never wraps; then UINT64_MAX - 1 itself is the test. */
        uint64_t w = (uint64_t)exact;
        if (w > now)
            far_deadline -= ((w - now) / tsc_hz + 1) * 1000000000ull;
        else
            far_deadline -= 1000000000ull;
    }
    kprintf("far-deadline: tsc_hz %lu, wraps %d, rdtsc %lx\n", tsc_hz, exact > UINT64_MAX, now);
    kprintf("far-deadline: deadline %lx ns -> tsc %lx\n", far_deadline,
            uptime_to_tsc(far_deadline));
    uint32_t cpu = cpu_count > 1 ? 1 : 0;
    cpumask_t m;
    cpumask_one(&m, cpu);
    far_blocks = 0;
    struct thread *t = thread_create_on("kt-far", far_sleeper, NULL, PRIO_DEFAULT, &m);
    thread_sleep_ms(100);
    uint64_t n = __atomic_load_n(&far_blocks, __ATOMIC_RELAXED);
    thread_cancel(t);
    thread_join(t);
    kprintf("far-deadline: sleeper blocked %lu times in 100 ms\n", n);
    KT_ASSERT(uptime_to_tsc(far_deadline) > now);
    KT_ASSERT(n <= 2);
}
