/* Benchmarks: "bench" on the kernel command line ("Benchmark" in the boot
 * menu). Every result goes into the RESULTS box.
 *
 * How the numbers are made, so they can be trusted and compared:
 *   - Time comes from the TSC, fenced with lfence on both sides so rdtsc
 *     can't run early or late around the code being timed. The TSC ticks at
 *     a constant rate, so results are wall-clock time whatever the core's
 *     clock speed was.
 *   - The cost of taking a timestamp is measured first and subtracted from
 *     every sample. Operations shorter than a few hundred ns are timed in
 *     batches of BATCH and divided, so the timestamp cost can't dominate.
 *   - Each result is the median (typical case) and the 99th percentile
 *     (tail) of SAMPLES samples, never a mean: one timer interrupt landing
 *     in a sample would drag a mean around. The tail is kept, not trimmed,
 *     so interrupts and scheduling show up there honestly.
 *   - Every benchmark first runs untimed for WARM_NS, which also brings the
 *     core up to full clock.
 *   - Threads are pinned. On a hybrid CPU the cross-CPU tests name the core
 *     types: P = a P-core thread, P2 = a different P-core, HT = the same
 *     P-core's other hyperthread, E = an E-core. CPU 0 is avoided (it runs
 *     the boot thread and the sleep/timer tick work).
 *   - Nothing is printed while measuring.
 *   - Lines starting "user:" are measured in ring 3 by bin/utest
 *     (user/tests/utest/bench.c), started with userboot pinned to the named CPUs
 *     at priority 24 like the kernel's benchmark threads: same TSC method,
 *     same warm-up, raw cycle counts sent back over a channel and turned
 *     into these lines here (the kernel's timestamp cost is subtracted).
 *     They include everything a program pays: syscall entry and exit, user
 *     copies, handle lookups, address-space switches between processes.
 *     The address-space switch line is the kernel alone: two CR3 loads
 *     (and their active-CPU mask updates) timed with interrupts off.
 * The other lines are kernel threads. The lock-order checker is on for
 * every lock, as it always is.
 *   - The optimisations each have a run-time switch. A line that one of
 *     them should move is measured twice in the same run, switch off and
 *     then on, and printed as "<switch> off median/p99 on median/p99"
 *     (the other switches stay as booted). Switches: spinidle (an idle CPU
 *     polls for SCHED_IDLE_SPIN_NS before `hlt`; off, a wakeup of an idle
 *     CPU includes the hardware's wake-from-halt time). */
#include <jam/aspace.h>
#include <jam/bootfs.h>
#include <jam/channel.h>
#include <jam/cpu.h>
#include <jam/interrupt.h>
#include <jam/interrupt_test.h>
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/pathstat.h>
#include <jam/pcid.h>
#include <jam/percpu.h>
#include <jam/port.h>
#include <jam/process.h>
#include <jam/report.h>
#include <jam/sched.h>
#include <jam/serial.h>
#include <jam/spinlock.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/userboot.h>
#include <jam/x86.h>

#include "bench_internal.h"

#define SAMPLES   4000
#define BATCH     64
#define WARM_NS   20000000ull   /* 20 ms */

/* ---- timing -------------------------------------------------------------- */

static inline uint64_t stamp(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return (uint64_t)hi << 32 | lo;
}

static uint64_t ps_per_cycle_x1024;   /* picoseconds per TSC cycle, << 10 */
static uint64_t stamp_cost;           /* cycles, median of a back-to-back pair */
static uint64_t stamp_step;           /* cycles, smallest nonzero TSC advance seen */

static uint64_t cycles_to_ps(uint64_t c)
{
    return c * ps_per_cycle_x1024 >> 10;
}

/* Picoseconds for one operation out of a timed span of n operations. */
static uint64_t span_ps(uint64_t t0, uint64_t t1, uint64_t n)
{
    uint64_t c = t1 - t0;
    c = c > stamp_cost ? c - stamp_cost : 0;
    return cycles_to_ps(c) / n;
}

static void sort(uint64_t *a, unsigned n)
{
    static const unsigned gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (unsigned g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
        for (unsigned i = gaps[g]; i < n; i++) {
            uint64_t v = a[i];
            unsigned j = i;
            for (; j >= gaps[g] && a[j - gaps[g]] > v; j -= gaps[g])
                a[j] = a[j - gaps[g]];
            a[j] = v;
        }
}

static void fmt_ps(char *buf, size_t n, uint64_t ps)
{
    if (ps < 10000000)   /* under 10 us: ns with one decimal */
        ksnprintf(buf, n, "%lu.%lu ns", ps / 1000, ps / 100 % 10);
    else
        ksnprintf(buf, n, "%lu us", ps / 1000000);
}

static void result(const char *what, uint64_t *s, unsigned n)
{
    sort(s, n);
    char med[24], p99[24];
    fmt_ps(med, sizeof(med), s[(n - 1) / 2]);
    fmt_ps(p99, sizeof(p99), s[(n - 1) * 99 / 100]);
    report("bench: %-44s median %-10s p99 %s", what, med, p99);
}

/* "1539.5ns", "12.3us", "10ms": a value with its unit, no space. */
static void fmt_short(char *buf, size_t n, uint64_t ps)
{
    if (ps < 10000000)
        ksnprintf(buf, n, "%lu.%luns", ps / 1000, ps / 100 % 10);
    else if (ps < 10000000000ull)
        ksnprintf(buf, n, "%lu.%luus", ps / 1000000, ps / 100000 % 10);
    else
        ksnprintf(buf, n, "%lums", ps / 1000000000);
}

/* One line for a measurement made with a switch off and then on in
 * the same run: "median/p99" each way. */
static void result2(const char *what, const char *sw, uint64_t *off, uint64_t *on, unsigned n)
{
    sort(off, n);
    sort(on, n);
    char a[16], b[16], c[16], d[16];
    fmt_short(a, sizeof(a), off[(n - 1) / 2]);
    fmt_short(b, sizeof(b), off[(n - 1) * 99 / 100]);
    fmt_short(c, sizeof(c), on[(n - 1) / 2]);
    fmt_short(d, sizeof(d), on[(n - 1) * 99 / 100]);
    report("bench: %-44s %s off %s/%s on %s/%s", what, sw, a, b, c, d);
}

/* ---- CPUs ------------------------------------------------------------------ */

static int cpu_p = -1, cpu_p2 = -1, cpu_ht = -1, cpu_e = -1;

/* P is a P-core thread that is not CPU 0 and whose core is not CPU 0's
 * core, so its hyperthread sibling can be tested too (the first run on the
 * PC picked CPU 1, whose sibling is CPU 0, and skipped the HT test). */
static void pick_cpus(void)
{
    bool hybrid = false;
    for (uint32_t i = 0; i < cpu_count; i++)
        hybrid |= cpus[i]->type == CORE_EFFICIENCY;
    enum core_type big = hybrid ? CORE_PERFORMANCE : cpus[0]->type;
    for (uint32_t i = 1; i < cpu_count && cpu_p < 0; i++)
        if (cpus[i]->type == big && cpus[i]->core_id != cpus[0]->core_id)
            cpu_p = (int)i;
    for (uint32_t i = 1; i < cpu_count && cpu_p < 0; i++)
        if (cpus[i]->type == big)
            cpu_p = (int)i;   /* no SMT, or only one core */
    if (cpu_p < 0) {
        cpu_p = 0;            /* one CPU: the cross-CPU tests are skipped */
        return;
    }
    for (uint32_t i = 1; i < cpu_count; i++) {
        struct cpu *c = cpus[i];
        if ((int)i == cpu_p)
            continue;
        if (cpu_ht < 0 && c->core_id == cpus[cpu_p]->core_id)
            cpu_ht = (int)i;
        else if (cpu_p2 < 0 && c->type == big && c->core_id != cpus[cpu_p]->core_id &&
                 c->core_id != cpus[0]->core_id)
            cpu_p2 = (int)i;
        if (hybrid && cpu_e < 0 && c->type == CORE_EFFICIENCY)
            cpu_e = (int)i;
    }
}

void bench_cpus(int *p, int *p2)
{
    if (cpu_p < 0)
        pick_cpus();
    *p = cpu_p;
    *p2 = cpu_p2;
}

uint64_t bench_cycles_to_ps(uint64_t c)
{
    /* Independent of bench_run's setup, so a test can use it too. */
    return tsc_hz ? c * 1000000ull / (tsc_hz / 1000000ull) : 0;
}

static const char *kind(int cpu)
{
    if (cpu == cpu_p)
        return "P";
    if (cpu == cpu_p2)
        return "P2";
    if (cpu == cpu_ht)
        return "HT";
    if (cpu == cpu_e)
        return "E";
    return "?";
}

static struct thread *spawn_on(int cpu, void (*fn)(void *), void *arg)
{
    cpumask_t m;
    cpumask_one(&m, (uint32_t)cpu);
    return thread_create_on("bench", fn, arg, PRIO_BENCH, &m);
}

/* Run fn(arg) in a thread pinned to cpu and wait for it. */
static void run_on(int cpu, void (*fn)(void *), void *arg)
{
    thread_join(spawn_on(cpu, fn, arg));
}

static uint64_t *samples, *samples_off, *samples_on;

/* The switches, each flipped between its off and on setting for one
 * measurement and put back afterwards (on = the boot setting, or the
 * default if the boot turned the feature off). SW_ALL flips every one of
 * them at once (the word "m55": none of them against all of them). */
enum sw {
    SW_SPINIDLE, SW_PLACEORDER, SW_AFFINEPAIR, SW_KMCACHE, SW_ONESHOT, SW_SERIALIRQ, SW_FPUOPT,
    SW_PCID, SW_COUNT, SW_ALL = SW_COUNT
};
static const char *const sw_name[SW_COUNT + 1] = {
    "spinidle", "placeorder", "affinepair", "kmcache", "oneshot", "serialirq", "fpuopt", "pcid",
    "m55"
};
static uint64_t sw_boot[SW_COUNT];

static uint64_t sw_get(enum sw s)
{
    switch (s) {
    case SW_SPINIDLE:   return __atomic_load_n(&sched_idle_spin_ns, __ATOMIC_RELAXED);
    case SW_PLACEORDER: return __atomic_load_n(&sched_place_order, __ATOMIC_RELAXED);
    case SW_AFFINEPAIR: return __atomic_load_n(&sched_affine_pair, __ATOMIC_RELAXED);
    case SW_KMCACHE:    return __atomic_load_n(&heap_percpu, __ATOMIC_RELAXED);
    case SW_ONESHOT:    return __atomic_load_n(&lapic_oneshot, __ATOMIC_RELAXED);
    case SW_SERIALIRQ:  return __atomic_load_n(&serial_async, __ATOMIC_RELAXED);
    case SW_FPUOPT:     return __atomic_load_n(&fpu_opt, __ATOMIC_RELAXED);
    case SW_PCID:       return pcid_is_on();
    default:            break;
    }
    return 0;
}

static void sw_put(enum sw s, uint64_t v)
{
    switch (s) {
    case SW_SPINIDLE:   __atomic_store_n(&sched_idle_spin_ns, v, __ATOMIC_RELAXED); break;
    case SW_PLACEORDER: __atomic_store_n(&sched_place_order, (bool)v, __ATOMIC_RELAXED); break;
    case SW_AFFINEPAIR: __atomic_store_n(&sched_affine_pair, (bool)v, __ATOMIC_RELAXED); break;
    case SW_KMCACHE:    __atomic_store_n(&heap_percpu, (bool)v, __ATOMIC_RELAXED); break;
    case SW_ONESHOT:    __atomic_store_n(&lapic_oneshot, (bool)v, __ATOMIC_RELAXED); break;
    case SW_SERIALIRQ:  serial_set_async(v); break;
    case SW_FPUOPT:     __atomic_store_n(&fpu_opt, (bool)v, __ATOMIC_RELAXED); break;
    case SW_PCID:       pcid_set(v); break;   /* no-op without PCIDs */
    default:            break;
    }
}

static const uint64_t sw_default_on[SW_COUNT] = { SCHED_IDLE_SPIN_NS, 1, 1, 1, 1, 1, 1, 1 };

static void sw_save(void)
{
    for (unsigned s = 0; s < SW_COUNT; s++)
        sw_boot[s] = sw_get((enum sw)s);
}

static void sw_set(enum sw s, bool on)
{
    if (s == SW_ALL) {
        for (unsigned i = 0; i < SW_COUNT; i++)
            sw_set((enum sw)i, on);
        return;
    }
    sw_put(s, on ? (sw_boot[s] ? sw_boot[s] : sw_default_on[s]) : 0);
}

static void sw_restore(enum sw s)
{
    if (s == SW_ALL) {
        for (unsigned i = 0; i < SW_COUNT; i++)
            sw_restore((enum sw)i);
        return;
    }
    sw_put(s, sw_boot[s]);
}

/* Run `measure` (which fills `samples`) with switch s off, then on, and
 * report both on one line. */
static void off_on(enum sw s, const char *what, void (*measure)(int), int arg, unsigned n)
{
    uint64_t *keep = samples;
    sw_set(s, false);
    samples = samples_off;
    measure(arg);
    sw_set(s, true);
    samples = samples_on;
    measure(arg);
    sw_restore(s);
    samples = keep;
    result2(what, sw_name[s], samples_off, samples_on, n);
}

static void warm_until(uint64_t *deadline)
{
    *deadline = uptime_ns() + WARM_NS;
}

/* ---- single-CPU operations, timed in batches ---------------------------- */

static void bench_stamp(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp(), t1 = stamp();
        samples[i] = t1 - t0;
    }
    sort(samples, SAMPLES);
    stamp_cost = samples[SAMPLES / 2];
    /* Resolution: an emulated TSC (QEMU without KVM) may only move in big
     * steps, which makes anything shorter than a step read as 0. */
    stamp_step = UINT64_MAX;
    for (unsigned i = 0; i < 1000; i++) {
        uint64_t a = stamp(), b;
        while ((b = stamp()) == a)
            ;
        if (b - a < stamp_step)
            stamp_step = b - a;
    }
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp(), t1 = stamp();
        samples[i] = cycles_to_ps(t1 - t0);
    }
}

static spinlock_t bench_lock = SPINLOCK_INIT("bench lock");

static void op_lock(void)
{
    spin_lock(&bench_lock);
    spin_unlock(&bench_lock);
}

static void op_kmalloc(void)
{
    kfree(kmalloc(64));
}

static void op_page(void)
{
    pmm_free_page_phys(pmm_alloc_page_phys(0));
}

static void (*batch_op)(void);

static void bench_batch(void *arg)
{
    (void)arg;
    uint64_t until;
    warm_until(&until);
    while (uptime_ns() < until)
        batch_op();
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        for (unsigned k = 0; k < BATCH; k++)
            batch_op();
        samples[i] = span_ps(t0, stamp(), BATCH);
    }
}

static void batch_measure(int unused)
{
    (void)unused;
    run_on(cpu_p, bench_batch, NULL);
}

static void batch(const char *what, void (*op)(void))
{
    batch_op = op;
    batch_measure(0);
    result(what, samples, SAMPLES);
}

/* ---- the same operation on every CPU at once --------------------------------- */

#define PAR_SAMPLES 400
static volatile uint32_t par_ready;
static volatile bool par_go;
static void (*par_op)(void);

static void bench_all(void *arg)
{
    uint64_t *mine = arg;
    __atomic_add_fetch(&par_ready, 1, __ATOMIC_RELEASE);
    while (!par_go)
        cpu_relax();
    uint64_t until;
    warm_until(&until);
    while (uptime_ns() < until)
        par_op();
    for (unsigned i = 0; i < PAR_SAMPLES; i++) {
        uint64_t t0 = stamp();
        for (unsigned k = 0; k < BATCH; k++)
            par_op();
        mine[i] = span_ps(t0, stamp(), BATCH);
    }
}

/* op on every CPU at once; cpu_count * PAR_SAMPLES samples into `all`. */
static bool all_cpus(void (*op)(void), uint64_t *all)
{
    struct thread **th = kmalloc(cpu_count * sizeof(*th));
    if (!th)
        return false;
    par_op = op;
    par_ready = 0;
    par_go = false;
    for (uint32_t i = 0; i < cpu_count; i++)
        th[i] = spawn_on((int)i, bench_all, all + i * PAR_SAMPLES);
    while (par_ready < cpu_count)
        thread_yield();
    par_go = true;
    for (uint32_t i = 0; i < cpu_count; i++)
        thread_join(th[i]);
    kfree(th);
    return true;
}

static void page_all_cpus(void)
{
    unsigned n = cpu_count * PAR_SAMPLES;
    uint64_t *all = kmalloc(n * sizeof(uint64_t));
    if (!all || !all_cpus(op_page, all)) {
        kprintf("bench: out of memory for the all-CPU test\n");
        kfree(all);
        return;
    }
    char what[64];
    ksnprintf(what, sizeof(what), "page alloc+free, all %u CPUs at once", cpu_count);
    result(what, all, n);
    kfree(all);
}

/* kmalloc on every CPU at once: without the per-CPU magazines every CPU
 * takes the kmalloc-64 cache lock twice per pair (switch kmcache). */
static void kmalloc_all_cpus(void)
{
    unsigned n = cpu_count * PAR_SAMPLES;
    uint64_t *off = kmalloc(n * sizeof(uint64_t)), *on = kmalloc(n * sizeof(uint64_t));
    bool ok = off && on;
    if (ok) {
        sw_set(SW_KMCACHE, false);
        ok = all_cpus(op_kmalloc, off);
        sw_set(SW_KMCACHE, true);
        ok = ok && all_cpus(op_kmalloc, on);
        sw_restore(SW_KMCACHE);
    }
    if (ok) {
        char what[64];
        ksnprintf(what, sizeof(what), "kmalloc(64)+kfree, all %u CPUs at once", cpu_count);
        result2(what, sw_name[SW_KMCACHE], off, on, n);
    } else {
        kprintf("bench: out of memory for the all-CPU test\n");
    }
    kfree(off);
    kfree(on);
}

/* ---- context switch: two threads yielding on one CPU ----------------------
 * The partner starts first and the timer waits until it runs, and the time
 * is divided by the switches the scheduler actually counted on that CPU,
 * not by an assumed number, so a yield that finds nobody to switch to can
 * never pass for a switch (INVALID is reported instead). On the PC this
 * confirmed the first run's 28.8 ns was a real switch. */

static volatile bool yield_done, partner_running;
static uint64_t yield_switches;

static void yield_partner(void *arg)
{
    (void)arg;
    partner_running = true;
    while (!yield_done)
        thread_yield();
}

static void yield_timer(void *arg)
{
    (void)arg;
    while (!partner_running)
        thread_yield();
    uint64_t until;
    warm_until(&until);
    while (uptime_ns() < until)
        thread_yield();
    struct cpu *c = cpus[cpu_p];
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t sw0 = __atomic_load_n(&c->switches, __ATOMIC_RELAXED);
        uint64_t t0 = stamp();
        for (unsigned k = 0; k < BATCH; k++)
            thread_yield();
        uint64_t t1 = stamp();
        uint64_t sw = __atomic_load_n(&c->switches, __ATOMIC_RELAXED) - sw0;
        yield_switches += sw;
        samples[i] = sw ? span_ps(t0, t1, sw) : 0;
    }
    yield_done = true;
}

static void context_switch(void)
{
    yield_done = partner_running = false;
    yield_switches = 0;
    struct thread *b = spawn_on(cpu_p, yield_partner, NULL);
    struct thread *a = spawn_on(cpu_p, yield_timer, NULL);
    thread_join(a);
    thread_join(b);
    uint64_t expect = 2ull * SAMPLES * BATCH;
    if (yield_switches < expect * 9 / 10) {
        report("bench: context switch: INVALID, only %lu of ~%lu yields switched",
               yield_switches, expect);
        return;
    }
    result("context switch (yield between 2 threads, P)", samples, SAMPLES);
}

/* ---- ping-pong: cache line, and block/wake ------------------------------ */

static volatile uint64_t line_flag __attribute__((aligned(64)));
static volatile bool line_stop;

static void line_responder(void *arg)
{
    (void)arg;
    uint64_t seen = 0;
    while (!line_stop) {
        uint64_t v = __atomic_load_n(&line_flag, __ATOMIC_ACQUIRE);
        if (v != seen && (v & 1)) {
            seen = v + 1;
            __atomic_store_n(&line_flag, seen, __ATOMIC_RELEASE);
        }
        cpu_relax();
    }
}

static void line_initiator(void *arg)
{
    (void)arg;
    uint64_t v = 0, until;
    warm_until(&until);
    unsigned n = 0;
    while (n < SAMPLES) {
        bool timed = uptime_ns() >= until;
        uint64_t t0 = stamp();
        __atomic_store_n(&line_flag, ++v, __ATOMIC_RELEASE);   /* odd: ping */
        while (__atomic_load_n(&line_flag, __ATOMIC_ACQUIRE) != v + 1)
            cpu_relax();
        uint64_t t1 = stamp();
        v++;                                                   /* even: pong seen */
        if (timed)
            samples[n++] = span_ps(t0, t1, 1);
    }
    line_stop = true;
}

static void cache_line(int other)
{
    line_flag = 0;
    line_stop = false;
    struct thread *r = spawn_on(other, line_responder, NULL);
    run_on(cpu_p, line_initiator, NULL);
    thread_join(r);
    char what[64];
    ksnprintf(what, sizeof(what), "cache-line round trip P->%s (hardware floor)", kind(other));
    result(what, samples, SAMPLES);
}

struct wake_pp {
    spinlock_t       lock;   /* guards turn */
    struct waitqueue wq;     /* each side waits here for its move */
    volatile int     turn;   /* 0: initiator's move, 1: responder's */
    volatile bool    stop;   /* set by the initiator when done */
};

static struct wake_pp wpp;

static void wake_responder(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&wpp.lock);
        while (wpp.turn != 1 && !wpp.stop)
            waitqueue_wait(&wpp.wq, &wpp.lock, &f);
        bool stop = wpp.stop;
        wpp.turn = 0;
        spin_unlock_irqrestore(&wpp.lock, f);
        waitqueue_wake_all(&wpp.wq);
        if (stop)
            return;
    }
}

static void wake_round(void)
{
    uint64_t f = spin_lock_irqsave(&wpp.lock);
    wpp.turn = 1;
    spin_unlock_irqrestore(&wpp.lock, f);
    waitqueue_wake_all(&wpp.wq);
    f = spin_lock_irqsave(&wpp.lock);
    while (wpp.turn != 0)
        waitqueue_wait(&wpp.wq, &wpp.lock, &f);
    spin_unlock_irqrestore(&wpp.lock, f);
}

static void wake_initiator(void *arg)
{
    (void)arg;
    uint64_t until;
    warm_until(&until);
    while (uptime_ns() < until)
        wake_round();
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        wake_round();
        samples[i] = span_ps(t0, stamp(), 1);
    }
    uint64_t f = spin_lock_irqsave(&wpp.lock);
    wpp.stop = true;
    spin_unlock_irqrestore(&wpp.lock, f);
    waitqueue_wake_all(&wpp.wq);
}

static void wakeup_measure(int other)
{
    spin_init(&wpp.lock, "bench pingpong");
    waitqueue_init(&wpp.wq, "bench pingpong waiters");
    wpp.turn = 0;
    wpp.stop = false;
    struct thread *r;
    if (other >= 0) {
        r = spawn_on(other, wake_responder, NULL);
    } else {   /* unpinned, but kept off CPU 0 and P */
        cpumask_t m;
        cpumask_all(&m);
        m.bits[0] &= ~1ull;
        m.bits[cpu_p / 64] &= ~(1ull << (cpu_p % 64));
        r = thread_create_on("bench", wake_responder, NULL, PRIO_BENCH, &m);
    }
    run_on(cpu_p, wake_initiator, NULL);
    thread_join(r);
}

/* A pair that wakes each other with plain wakes (no wake-affine hint):
 * the responder may run anywhere but CPU 0 and P. As a pair it runs
 * on P's HT sibling; without, the hybrid order gives it a whole idle core. */
static void wakeup_pair(void)
{
    off_on(SW_AFFINEPAIR, "block+wake round trip P->unpinned partner", wakeup_measure, -1,
           SAMPLES);
}

/* Across CPUs the responder's CPU is idle between rounds: halted (spin
 * before idle off) or polling (on). */
static void wakeup(int other)
{
    char what[64];
    if (other == cpu_p) {
        wakeup_measure(other);
        result("block+wake round trip, same CPU (P)", samples, SAMPLES);
        return;
    }
    ksnprintf(what, sizeof(what), "block+wake round trip P->%s (idle CPU)", kind(other));
    off_on(SW_SPINIDLE, what, wakeup_measure, other, SAMPLES);
}

/* ---- IPI, channel_call, TLB shootdown -------------------------------------- */

static void nothing(void *arg)
{
    (void)arg;
}

static int ipi_target;

static void bench_ipi(void *arg)
{
    (void)arg;
    uint64_t until;
    warm_until(&until);
    while (uptime_ns() < until)
        smp_call_on((uint32_t)ipi_target, nothing, NULL);
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        smp_call_on((uint32_t)ipi_target, nothing, NULL);
        samples[i] = span_ps(t0, stamp(), 1);
    }
}

static void ipi_measure(int other)
{
    ipi_target = other;
    run_on(cpu_p, bench_ipi, NULL);
}

/* The target is idle: halted, or polling (it still needs the IPI, but is
 * not waking from halt). */
static void ipi(int other)
{
    char what[64];
    ksnprintf(what, sizeof(what), "IPI function call round trip P->%s", kind(other));
    off_on(SW_SPINIDLE, what, ipi_measure, other, SAMPLES);
}

/* ---- interrupts -------------------------------------------------------------
 * An interrupt object's vector, raised by a fixed IPI to the CPU the vector
 * allocator gave it (an E-core on the PC), standing in for the device's
 * MSI: the handler there raises SIG_INTERRUPT, the persistent port binding
 * queues the packet and wakes the thread blocked in port_wait on P, which
 * then acks. No real device: the numbers are the kernel's part alone. */

static struct kobject *birq;
static struct port *birq_port;
static uint32_t birq_cpu;
static uint8_t birq_vec;

static void birq_round(void)
{
    struct port_packet pkt;
    ipi_send(birq_cpu, birq_vec);
    port_wait(birq_port, DEADLINE_NEVER, &pkt);
    interrupt_ack(birq);
}

static void bench_interrupt(void *arg)
{
    (void)arg;
    uint64_t until;
    warm_until(&until);
    while (uptime_ns() < until)
        birq_round();
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        birq_round();
        samples[i] = span_ps(t0, stamp(), 1);
    }
}

static void interrupt_round_trip(void)
{
    if (interrupt_create_virtual(&birq) != OK || port_create(&birq_port) != OK ||
        port_bind(birq_port, birq, 1, SIG_INTERRUPT, PORT_BIND_PERSISTENT) != OK ||
        !interrupt_vector_of(birq, &birq_cpu, &birq_vec)) {
        report("bench: interrupt round trip: no interrupt object");
        return;
    }
    run_on(cpu_p, bench_interrupt, NULL);
    char where[16], what[64];
    const char *k = kind((int)birq_cpu);
    if (k[0] == '?')
        ksnprintf(where, sizeof(where), "cpu%u", birq_cpu);
    else
        ksnprintf(where, sizeof(where), "%s", k);
    ksnprintf(what, sizeof(what), "interrupt: vector on %s -> port_wait wakes P", where);
    result(what, samples, SAMPLES);
    kobject_unref(&birq_port->base);
    kobject_unref(birq);
}

static void chan_server(void *arg)
{
    struct channel *ep = arg;
    for (;;) {
        signals_t s = 0;
        object_wait_one((struct kobject *)ep, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER,
                        &s);
        uint64_t m[2];
        uint32_t nb = 0;
        status_t st = channel_read(ep, m, sizeof(m), &nb, NULL, 0, NULL);
        if (st == OK)
            channel_write(ep, m, nb, NULL, 0);
        else if (st != ERR_SHOULD_WAIT)
            return;   /* the client closed its end */
    }
}

static struct channel *chan_client_ep;

static void chan_round(void)
{
    uint64_t req[2] = { 0, 42 }, rep[2];
    uint32_t n = 0;
    channel_call(chan_client_ep, req, sizeof(req), NULL, 0, rep, sizeof(rep), &n, NULL, 0, NULL,
                 DEADLINE_NEVER);
}

static void bench_chan(void *arg)
{
    (void)arg;
    uint64_t until;
    warm_until(&until);
    while (uptime_ns() < until)
        chan_round();
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        chan_round();
        samples[i] = span_ps(t0, stamp(), 1);
    }
}

/* Client pinned to P, server allowed on `mask`. */
static void chan_call_mask(const cpumask_t *mask, const char *what)
{
    struct channel *a, *b;
    if (channel_create(&a, &b) != OK)
        return;
    chan_client_ep = a;
    struct thread *srv = thread_create_on("bench", chan_server, b, PRIO_BENCH, mask);
    run_on(cpu_p, bench_chan, NULL);
    kobject_unref((struct kobject *)a);   /* closes it: the server sees PEER_CLOSED */
    thread_join(srv);
    kobject_unref((struct kobject *)b);
    if (what)
        result(what, samples, SAMPLES);
}

static void chan_call_measure(int server_cpu)
{
    cpumask_t m;
    cpumask_one(&m, (uint32_t)server_cpu);
    chan_call_mask(&m, NULL);
}

static void chan_call(int server_cpu)
{
    char what[64];
    if (server_cpu == cpu_p) {
        chan_call_measure(server_cpu);
        result("channel_call round trip, same CPU (P)", samples, SAMPLES);
        return;
    }
    ksnprintf(what, sizeof(what), "channel_call round trip P->%s, 1 client", kind(server_cpu));
    off_on(SW_SPINIDLE, what, chan_call_measure, server_cpu, SAMPLES);
}

/* The server may run anywhere but CPU 0 (the orchestrator's), so placement
 * decides where it runs. Wake-affine puts it on the caller's CPU, as
 * the caller blocks right after sending; kept off P as well, on P's idle HT
 * sibling. */
static void chan_call_placed(void)
{
    cpumask_t m;
    cpumask_all(&m);
    m.bits[0] &= ~1ull;
    chan_call_mask(&m, "channel_call round trip, P client, server unpinned");
    if (cpu_ht < 0)
        return;
    m.bits[cpu_p / 64] &= ~(1ull << (cpu_p % 64));
    chan_call_mask(&m, "channel_call round trip, P client, server not on P");
}

/* ---- sleep accuracy -------------------------------------------------------
 * A thread on P sleeps for a set time; the sample is how late it woke
 * (actual - requested). With one-shot timers the CPU's timer is armed for
 * the deadline; without, the sleeper waits for its CPU's next 10 ms tick. */

#define TIMER_SAMPLES 200
static uint64_t sleep_req_ns;

static void bench_sleep(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 5; i++)   /* warm-up */
        thread_sleep_ns(sleep_req_ns);
    for (unsigned i = 0; i < TIMER_SAMPLES; i++) {
        uint64_t t0 = stamp();
        thread_sleep_ns(sleep_req_ns);
        uint64_t ps = span_ps(t0, stamp(), 1), req = sleep_req_ns * 1000;
        samples[i] = ps > req ? ps - req : 0;
    }
}

static void sleep_measure(int us)
{
    sleep_req_ns = (uint64_t)us * 1000;
    run_on(cpu_p, bench_sleep, NULL);
}

static void sleep_accuracy(int us)
{
    char what[64];
    ksnprintf(what, sizeof(what), "sleep %u us (P): how late it wakes", us);
    off_on(SW_ONESHOT, what, sleep_measure, us, TIMER_SAMPLES);
}

/* ---- serial output --------------------------------------------------------
 * What a 100-character line costs the CPU that writes it to COM1 (the
 * serial part of every klog line): off, synchronous output, which
 * waits for the UART character by character (~87 us each at 115200 baud
 * on real hardware); on, a copy into the transmit ring that the UART's
 * interrupt drains. Each sample starts with the ring empty (waited for,
 * untimed). The lines appear on the serial log only. */

#define SERIAL_SAMPLES 32
static const char serial_line[] =
    "bench: serial timing line, 100 characters long, sent 64 times; ignore it "
    "..........................\n";
_Static_assert(sizeof(serial_line) == 101, "100 characters and the NUL");

static void bench_serial(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < SERIAL_SAMPLES; i++) {
        uint64_t until = uptime_ns() + 100000000ull;
        while (serial_pending() && uptime_ns() < until)
            thread_sleep_ns(100000);
        uint64_t t0 = stamp();
        serial_write(serial_line, sizeof(serial_line) - 1);
        samples[i] = span_ps(t0, stamp(), 1);
    }
}

static void serial_measure(int unused)
{
    (void)unused;
    run_on(cpu_p, bench_serial, NULL);
}

static void serial_output(void)
{
    if (!serial_is_async()) {
        report("bench: serial: no COM1, or its interrupt is off/not working: line skipped");
        return;
    }
    uint64_t drop0 = __atomic_load_n(&serial_dropped, __ATOMIC_RELAXED);
    off_on(SW_SERIALIRQ, "serial_write of a 100-character line (P)", serial_measure, 0,
           SERIAL_SAMPLES);
    if (__atomic_load_n(&serial_dropped, __ATOMIC_RELAXED) != drop0)
        report("bench: serial: %lu characters dropped meanwhile",
               __atomic_load_n(&serial_dropped, __ATOMIC_RELAXED) - drop0);
}

/* ---- placement of busy threads ---------------------------------------------
 * Where the scheduler puts CPU-bound threads when there is room: one per
 * core except CPU 0's (which runs this thread), all unpinned but kept off
 * CPU 0. Not a time: the line counts how many landed on a core another
 * busy thread (or CPU 0) already uses, and how many on E-cores. With the
 * hybrid order that is "none shared" while whole cores are idle. */

#define PLACE_MAX 64
static volatile bool place_release;
static volatile uint32_t place_started;

static void place_spinner(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&place_started, 1, __ATOMIC_RELAXED);
    while (!place_release)
        cpu_relax();
}

static uint32_t count_cores(void)
{
    uint32_t cores = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        bool first = true;
        for (uint32_t j = 0; j < i; j++)
            first &= cpus[j]->core_id != cpus[i]->core_id;
        cores += first;
    }
    return cores;
}

/* Place n spinners; *shared = how many share a core, *on_e = on E-cores. */
static void place_busy(uint32_t n, uint32_t *shared, uint32_t *on_e)
{
    struct thread *th[PLACE_MAX];
    uint32_t where[PLACE_MAX];
    cpumask_t m;
    cpumask_all(&m);
    m.bits[0] &= ~1ull;
    place_release = false;
    place_started = 0;
    for (uint32_t k = 0; k < n; k++)
        th[k] = thread_create_on("bench", place_spinner, NULL, PRIO_BENCH, &m);
    while (place_started < n)
        thread_yield();
    for (uint32_t k = 0; k < n; k++)
        where[k] = thread_cpu(th[k]);
    place_release = true;
    for (uint32_t k = 0; k < n; k++)
        thread_join(th[k]);
    *shared = *on_e = 0;
    for (uint32_t k = 0; k < n; k++) {
        bool share = cpus[where[k]]->core_id == cpus[0]->core_id;
        for (uint32_t j = 0; j < n; j++)
            share |= j != k && cpus[where[j]]->core_id == cpus[where[k]]->core_id;
        *shared += share;
        *on_e += cpus[where[k]]->type == CORE_EFFICIENCY;
    }
}

static void placement(void)
{
    uint32_t n = count_cores() - 1;
    if (n < 2)
        return;
    if (n > PLACE_MAX)
        n = PLACE_MAX;
    uint32_t s0, e0, s1, e1;
    sw_set(SW_PLACEORDER, false);
    place_busy(n, &s0, &e0);
    sw_set(SW_PLACEORDER, true);
    place_busy(n, &s1, &e1);
    sw_restore(SW_PLACEORDER);
    report("bench: placement of %u busy threads (1 per core but cpu0's) placeorder off %u share a "
           "core, %u on E; on %u, %u", n, s0, e0, s1, e1);
}
static void bench_shootdown(void *arg)
{
    (void)arg;
    uint64_t va = (uint64_t)&line_flag & ~(PAGE_SIZE - 1);   /* any kernel page */
    uint64_t until;
    warm_until(&until);
    while (uptime_ns() < until)
        tlb_shootdown(va, PAGE_SIZE);
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        tlb_shootdown(va, PAGE_SIZE);
        samples[i] = span_ps(t0, stamp(), 1);
    }
}

/* ---- user FPU state save + restore (breakdown) --------------------------------
 * One save and one restore of a user FPU area, as arch_thread_switch does
 * for a user thread going out and one coming in, timed in batches with
 * interrupts off (the registers are ours meanwhile; fpu_clobbered tells
 * the lazy-restore bookkeeping afterwards). The area is in its initial
 * state, as for a thread that hasn't used AVX: fpuopt off is XSAVE, on is
 * XSAVEOPT, which skips components that are unmodified or in init state. */

static void bench_fpu(void *arg)
{
    (void)arg;
    struct thread *me = current_thread();
    if (fpu_ustate_alloc(me) != OK) {
        for (unsigned i = 0; i < SAMPLES; i++)
            samples[i] = 0;
        return;
    }
    void *area = me->ustate;
    uint64_t until;
    warm_until(&until);
    for (unsigned i = 0; i < SAMPLES;) {
        uint64_t f = irq_save();
        uint64_t t0 = stamp();
        for (unsigned k = 0; k < BATCH; k++) {
            fpu_restore(area);
            fpu_save(area);
        }
        uint64_t t1 = stamp();
        fpu_clobbered();
        irq_restore(f);
        if (uptime_ns() >= until)
            samples[i++] = span_ps(t0, t1, BATCH);
    }
    fpu_ustate_free(me);
}

static void fpu_measure(int unused)
{
    (void)unused;
    run_on(cpu_p, bench_fpu, NULL);
}

static void fpu_state(void)
{
    char what[64];
    ksnprintf(what, sizeof(what), "XRSTOR + XSAVE of user FPU state (%u B, P)", fpu_area_size());
    off_on(SW_FPUOPT, what, fpu_measure, 0, SAMPLES);
}
/* ---- address-space switch --------------------------------------------------- */

static struct aspace *as_a, *as_b;

static void bench_as_switch(void *arg)
{
    (void)arg;
    uint64_t until;
    warm_until(&until);
    uint64_t f = irq_save();   /* this thread has no address space: restore before any switch */
    aspace_switch(NULL, as_a);
    while (uptime_ns() < until) {
        aspace_switch(as_a, as_b);
        aspace_switch(as_b, as_a);
    }
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        for (unsigned k = 0; k < BATCH / 2; k++) {
            aspace_switch(as_a, as_b);
            aspace_switch(as_b, as_a);
        }
        samples[i] = span_ps(t0, stamp(), BATCH);
    }
    aspace_switch(as_a, NULL);
    irq_restore(f);
}

static void as_switch_measure(int unused)
{
    (void)unused;
    run_on(cpu_p, bench_as_switch, NULL);
}

static void as_switch(void)
{
    if (aspace_create(&as_a) != OK)
        return;
    if (aspace_create(&as_b) != OK) {
        aspace_unref(as_a);
        return;
    }
    /* Without PCIDs (see the fpu line) there is nothing to switch. */
    if (pcid_usable()) {
        off_on(SW_PCID, "address-space switch (CR3 load + masks, P)", as_switch_measure, 0,
               SAMPLES);
    } else {
        as_switch_measure(0);
        result("address-space switch (CR3 load + masks, P)", samples, SAMPLES);
    }
    aspace_unref(as_b);
    aspace_unref(as_a);
}

/* ---- user space ----------------------------------------------------------- */

#define USAMPLES 4000   /* user/tests/utest/bench.c SAMPLES */

struct ubench_result {
    uint32_t txid, n, batch, reserved;   /* txid, samples (USAMPLES), calls per sample, 0 */
    uint64_t cycles[USAMPLES];           /* one timing per sample, TSC cycles */
};

static struct process *uspawn(struct job *j, const char *what, int cpu,
                              struct userboot_handle *extra, unsigned nextra)
{
    const char *argv[] = { "utest", what };
    cpumask_t m;
    cpumask_one(&m, (uint32_t)cpu);
    struct process *p;
    status_t st = userboot_spawn("bin/utest", argv, 2, j, extra, nextra, &m, &p);
    if (st != OK) {
        kprintf("bench: can't start utest %s (%s)\n", what, status_str(st));
        return NULL;
    }
    return p;
}

static void ureap(struct process *p)
{
    if (!p)
        return;
    if (object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 10000000000ull,
                        NULL) != OK) {
        process_kill(p, PROCESS_KILLED_CODE, true);
        object_wait_one(process_kobject(p), SIG_TERMINATED, DEADLINE_NEVER, NULL);
    }
    kobject_unref(process_kobject(p));
}

bool bench_user_run(const char *what, int cpu, int server_cpu, const char *label, bool trace)
{
    struct job *j;
    struct ubench_result *r = kmalloc(sizeof(*r));
    if (!r || userboot_root_job(&j) != OK) {
        kfree(r);
        return false;
    }
    struct channel *res_k, *res_u, *call_c = NULL, *call_s = NULL;
    if (channel_create(&res_k, &res_u) != OK) {
        kfree(r);
        job_unref(j);
        return false;
    }
    struct userboot_handle ex[2] = {
        { SR_USER, khandle_from_new((struct kobject *)res_u, RIGHTS_BASIC | RIGHTS_IO) },
    };
    unsigned nex = 1;
    struct process *server = NULL;
    if (server_cpu >= 0 && channel_create(&call_c, &call_s) == OK) {
        struct userboot_handle sx = {
            SR_USER, khandle_from_new((struct kobject *)call_s, RIGHTS_BASIC | RIGHTS_IO)
        };
        server = uspawn(j, "bench-echo", server_cpu, &sx, 1);
        ex[1] = (struct userboot_handle){
            SR_USER + 1, khandle_from_new((struct kobject *)call_c, RIGHTS_BASIC | RIGHTS_IO)
        };
        nex = 2;
    }
    char mode[24];
    ksnprintf(mode, sizeof(mode), "bench-%s", what);
    struct process *client = uspawn(j, mode, cpu, ex, nex);
    if (trace) {
        /* The client is the lead: its calls are what the window counts. */
        if (client)
            path_add_process(client);
        if (server)
            path_add_process(server);
        path_arm();
    }

    uint32_t nb = 0;
    status_t st = object_wait_one((struct kobject *)res_k, SIG_READABLE | SIG_PEER_CLOSED,
                                  uptime_ns() + 30000000000ull, NULL);
    if (st == OK)
        st = channel_read(res_k, r, sizeof(*r), &nb, NULL, 0, NULL);
    bool ok = st == OK && nb == sizeof(*r) && r->n == USAMPLES && r->batch;
    if (!ok)
        report("bench: %s: no result from the user program (%s)", label, status_str(st));
    else if (!trace)
        for (unsigned i = 0; i < USAMPLES; i++)
            samples[i] = span_ps(0, r->cycles[i], r->batch);
    kobject_unref((struct kobject *)res_k);
    ureap(client);   /* its end of the call channel closes: the server exits */
    ureap(server);
    kfree(r);
    job_unref(j);
    return ok;
}

static void user_bench(const char *what, int cpu, int server_cpu, const char *label)
{
    if (bench_user_run(what, cpu, server_cpu, label, false))
        result(label, samples, USAMPLES);
}

/* The same, measured with switch s off and then on. */
static struct {
    const char *what, *label;   /* the result line's name and the switch's label */
    int cpu, server_cpu;        /* client and server CPUs */
    bool ok;                    /* every run so far succeeded */
} ub;

static void user_bench_measure(int unused)
{
    (void)unused;
    ub.ok &= bench_user_run(ub.what, ub.cpu, ub.server_cpu, ub.label, false);
}

static void user_bench_off_on(enum sw s, const char *what, int cpu, int server_cpu,
                              const char *label)
{
    ub.what = what;
    ub.label = label;
    ub.cpu = cpu;
    ub.server_cpu = server_cpu;
    ub.ok = true;
    uint64_t *keep = samples;
    sw_set(s, false);
    samples = samples_off;
    user_bench_measure(0);
    sw_set(s, true);
    samples = samples_on;
    user_bench_measure(0);
    sw_restore(s);
    samples = keep;
    if (ub.ok)
        result2(label, sw_name[s], samples_off, samples_on, USAMPLES);
}

/* Breakdown of process->process channel_call against the kernel-thread
 * version (BENCH.md's investigation (a)):
 *   - "thread->thread, same process" does the same calls without the two
 *     address-space switches per round trip (both threads share a CR3);
 *   - "XSAVE + XRSTOR" is one save and one restore of a user FPU state,
 *     of which a same-CPU round trip does two of each.
 * The rest is syscall entry/exit (5 syscalls per round trip: the client's
 * call, the echo server's read, wait, read and write; the "syscall round
 * trip" line is one of them), user copies and handle lookups. */
static void user_benches(void)
{
    const void *img;
    uint64_t size;
    if (bootfs_data("bin/utest", &img, &size) != OK) {
        report("bench: no bin/utest in bootfs: user-space lines skipped");
        return;
    }
    user_bench("null", cpu_p, -1, "user: syscall round trip (unused number, P)");
    user_bench("clock", cpu_p, -1, "user: clock_get syscall (P)");
    user_bench("fault", cpu_p, -1, "user: page fault, fresh zero page (P)");
    if (pcid_usable())
        user_bench_off_on(SW_PCID, "call", cpu_p, cpu_p,
                          "user: process->process channel_call, same CPU (P)");
    else
        user_bench("call", cpu_p, cpu_p, "user: process->process channel_call, same CPU (P)");
    user_bench_off_on(SW_FPUOPT, "tcall", cpu_p, -1,
                      "user: thread->thread channel_call, 1 process (P)");
    int others[] = { cpu_p2, cpu_ht, cpu_e };
    for (unsigned i = 0; i < 3; i++) {
        if (others[i] < 0)
            continue;
        char what[64];
        ksnprintf(what, sizeof(what), "user: process->process channel_call P->%s",
                  kind(others[i]));
        user_bench_off_on(SW_ALL, "call", cpu_p, others[i], what);
    }
}

/* ---- driver --------------------------------------------------------------- */

static void free_samples(void)
{
    kfree(samples);
    kfree(samples_off);
    kfree(samples_on);
}

/* The header: the CPU, the TSC, which CPUs play P/P2/HT/E. */
static void print_header(void)
{
    const char *brand = cpu_features.brand;
    while (*brand == ' ')
        brand++;
    report("bench: %s, TSC %lu MHz, %u CPUs; P=cpu%d P2=cpu%d HT=cpu%d E=cpu%d", brand,
           tsc_hz / 1000000, cpu_count, cpu_p, cpu_p2, cpu_ht, cpu_e);
    report("bench: kernel threads, then ring 3 ('user:' lines, bin/utest), lock checker on, "
           "median and p99 of %u samples", SAMPLES);
    kprintf("bench: running (about 10 s); nothing is printed while measuring\n");
}

/* The timestamp's own cost, then the primitives on one CPU (P). */
static void local_benches(void)
{
    run_on(cpu_p, bench_stamp, NULL);
    result("timestamp cost (subtracted from all below)", samples, SAMPLES);
    uint64_t step_ps = cycles_to_ps(stamp_step);
    if (step_ps > 20000)   /* 20 ns: far coarser than any real TSC */
        report("bench: WARNING: TSC steps are %lu ns (emulated?): single-shot "
               "results under ~%lu ns mean nothing", step_ps / 1000, step_ps / 100);

    batch("spin_lock + spin_unlock, uncontended (P)", op_lock);
    /* A live kmalloc(64) object keeps its slab from emptying: without the
     * magazines, an empty slab goes straight back to the page allocator,
     * and a lone alloc+free pair would build and free a slab every time. */
    void *keeper = kmalloc(64);
    batch_op = op_kmalloc;
    off_on(SW_KMCACHE, "kmalloc(64) + kfree (P)", batch_measure, 0, SAMPLES);
    batch("page alloc + free, one CPU (P)", op_page);
    if (cpu_count > 1) {
        page_all_cpus();
        kmalloc_all_cpus();
    }
    kfree(keeper);

    context_switch();
    wakeup(cpu_p);
    chan_call(cpu_p);
}

/* P against each of P2, HT and E that exists: cache lines, wakeups, IPIs,
 * interrupts, channel calls, placement. */
static void cross_cpu_benches(void)
{
    int others[] = { cpu_p2, cpu_ht, cpu_e };
    for (unsigned i = 0; i < 3; i++)
        if (others[i] >= 0)
            cache_line(others[i]);
    for (unsigned i = 0; i < 3; i++)
        if (others[i] >= 0)
            wakeup(others[i]);
    if (cpu_ht >= 0)
        wakeup_pair();
    for (unsigned i = 0; i < 3; i++)
        if (others[i] >= 0)
            ipi(others[i]);
    if (cpu_count > 1)
        interrupt_round_trip();
    for (unsigned i = 0; i < 3; i++)
        if (others[i] >= 0)
            chan_call(others[i]);
    if (cpu_count > 2)
        chan_call_placed();
    if (cpu_count > 2)
        placement();
}

/* Serial output, sleep accuracy, TLB shootdown, address-space switches,
 * FPU state and the ring-3 benchmarks. */
static void system_benches(void)
{
    serial_output();
    if (lapic_timer_has_oneshot()) {
        sleep_accuracy(100);
        sleep_accuracy(1000);
    } else {
        report("bench: sleep accuracy: periodic timer (nodeadline), one-shot timers not in use");
    }
    if (cpu_count > 1) {
        run_on(cpu_p, bench_shootdown, NULL);
        char what[64];
        ksnprintf(what, sizeof(what), "TLB shootdown, 1 page, %u other CPUs", cpu_count - 1);
        result(what, samples, SAMPLES);
    }
    as_switch();
    fpu_state();
    user_benches();
}

void bench_run(void)
{
    samples = kmalloc(SAMPLES * sizeof(uint64_t));
    samples_off = kmalloc(SAMPLES * sizeof(uint64_t));
    samples_on = kmalloc(SAMPLES * sizeof(uint64_t));
    if (!samples || !samples_off || !samples_on) {
        kprintf("bench: out of memory\n");
        free_samples();
        return;
    }
    sw_save();
    ps_per_cycle_x1024 = (1000000000000ull << 10) / tsc_hz;
    pick_cpus();
    print_header();

    /* The orchestrating thread stays on CPU 0, away from every measured CPU,
     * so it can't be starved by (or compete with) a busy benchmark thread. */
    cpumask_t zero, all;
    cpumask_one(&zero, 0);
    cpumask_all(&all);
    thread_set_affinity(current_thread(), &zero);

    local_benches();
    cross_cpu_benches();
    system_benches();
    bench_path_run();
    thread_set_affinity(current_thread(), &all);
    free_samples();
}
