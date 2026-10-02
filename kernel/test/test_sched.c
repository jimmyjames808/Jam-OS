/* Scheduler placement and fairness (kernel/sched/sched.c): spin before
 * idle, the hybrid placement order, client/server pairs on sibling
 * hyperthreads, wake-affine channel_call and the starvation boost. The
 * races in the switch and wake paths are test_sched_races.c. */
#include <jam/channel.h>
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/object.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/x86.h>

/* ---- spin before idle ---------------------------------------------------- */

#define PP_ROUNDS 200

static struct {
    spinlock_t       lock;   /* guards turn */
    struct waitqueue wq;     /* each side waits here for its move */
    volatile int     turn;   /* 0: pinger's move, 1: ponger's */
    volatile bool    stop;   /* set when the rounds are done */
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
    kt_pin_self(a);
    spin_init(&pp.lock, "kt pingpong");
    waitqueue_init(&pp.wq, "kt pingpong waiters");
    pp.turn = 0;
    pp.stop = false;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        pp_ran_on[i] = 0;
    struct thread *t = thread_create_on("kt-pong", ponger, NULL, PRIO_DEFAULT + 2, mb);
    uint64_t f = spin_lock_irqsave(&pp.lock);
    for (int i = 0; i < PP_ROUNDS; i++) {
        if (pp_wait_polling) {
            uint64_t until = uptime_ns() + 10 * NS_PER_MS;
            while (!__atomic_load_n(&pp_wait_polling->idle_polling, __ATOMIC_RELAXED) &&
               uptime_ns() < until)
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
    kt_unpin_self();
    return pairs;
}

/* The same with the other thread pinned to cpu b; returns the polled
 * wakeups cpu b saw meanwhile. */
static uint64_t pingpong(uint32_t a, uint32_t b)
{
    cpumask_t m;
    cpumask_one(&m, b);
    uint64_t polled0 = __atomic_load_n(&cpus[b]->polled_wakes, __ATOMIC_RELAXED);
    pingpong_mask(a, &m);
    return __atomic_load_n(&cpus[b]->polled_wakes, __ATOMIC_RELAXED) - polled0;
}

/* With a long spin window, the ponger's CPU is polling whenever it is woken:
 * nearly every wakeup skips the IPI. With the spin off none can. */
KTEST(spin_idle_skips_ipi)
{
    KT_NEEDS_IDLE("counts wakeups that find CPU 2 idle and polling");
    if (cpu_count < 3)
        return;
    uint64_t keep = __atomic_load_n(&sched_idle_spin_ns, __ATOMIC_RELAXED);
    /* QEMU is slow: make the window cover a round trip. */
    __atomic_store_n(&sched_idle_spin_ns, 50 * NS_PER_MS, __ATOMIC_RELAXED);
    pp_wait_polling = cpus[2];      /* wake only once cpu 2 is in its window */
    uint64_t polled_on = pingpong(1, 2);
    pp_wait_polling = NULL;
    __atomic_store_n(&sched_idle_spin_ns, 0, __ATOMIC_RELAXED);
    thread_sleep_ms(60);   /* let the window cpu 2 already opened run out */
    uint64_t polled_off = pingpong(1, 2);
    __atomic_store_n(&sched_idle_spin_ns, keep, __ATOMIC_RELAXED);
    kprintf("spin-idle: %lu of %d wakeups polled with a 50 ms window, %lu with none\n",
            polled_on, PP_ROUNDS, polled_off);
    /* Since the spin leaves idle_polling set for schedule() to clear,
     * 199-200 of 200 poll on a quiet Mac, 178 with three QEMUs sharing it
     * (TCG vCPUs stall); clearing it in the spin polled only ~1/3. */
    KT_ASSERT(polled_on >= PP_ROUNDS * 3 / 4);
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
    /* Switched off: the plain least-loaded rule fills CPUs in index order. */
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

/* Real placement: busy threads kept off cpu 0 (where this thread runs,
 * busy too) each get a core of their own while whole cores are idle. With
 * SMT that means never both hyperthreads of one core, and never cpu 0's
 * sibling. Two things it depends on have their own tests: a CPU taking its
 * next thread must not look idle to placement (repro_picked_cpu_looks_idle),
 * and a thread queued behind another must be stolen onto a whole core, not
 * by whatever idle CPU looks first (steal_prefers_whole_core). Run it in a
 * loop with -smp N,threads=2 (ktest=placement loops=300). */
KTEST(placement_spreads_over_cores)
{
    KT_NEEDS_IDLE("asserts which CPUs busy threads are placed on: every core must be idle");
    if (cpu_count < 4 || !__atomic_load_n(&sched_place_order, __ATOMIC_RELAXED))
        return;
    kt_pin_self(0);
    uint32_t cores = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        bool first = true;
        for (uint32_t j = 0; j < i; j++)
            first &= cpus[j]->core_id != cpus[i]->core_id;
        cores += first;
    }
    /* Every core but cpu 0's. Live, services hold cores now and then: with
     * as many spinners as idle cores a service on one leaves a spinner only
     * half cores (all equally good, cpu 1 first), so leave half the cores
     * spare (the PC's 19 give 8 either way; QEMU's 4 give 3 at boot, 1 live). */
    uint32_t n = ktest_live ? (cores - 1) / 2 : cores - 1;
    if (n > 8)
        n = 8;
    struct thread *th[8];
    cpumask_t m;
    cpumask_all(&m);
    m.bits[0] &= ~1ull;
    spin_release = false;
    spin_started = 0;
    for (uint32_t k = 0; k < n; k++)
        th[k] = thread_create_on("kt-spin", busy_spinner, NULL, PRIO_DEFAULT - 1, &m);
    /* Wait busy, not asleep: asleep, this thread would leave core 0 idle,
     * and cpu 1 would rightly count as a whole idle core. */
    uint64_t end = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    while (spin_started < n && uptime_ns() < end)
        cpu_relax();
    bool all_started = spin_started == n;
    uint32_t where[8];
    for (uint32_t k = 0; k < n; k++)
        where[k] = th[k]->cpu;
    spin_release = true;
    for (uint32_t k = 0; k < n; k++)
        thread_join(th[k]);
    kt_unpin_self();
    KT_ASSERT(all_started);
    for (uint32_t k = 0; k < n; k++) {
        kprintf("placement: busy thread %u on cpu %u (core %u)\n", k, where[k],
                cpus[where[k]]->core_id);
        KT_ASSERT(cpus[where[k]]->core_id != cpus[0]->core_id);
        for (uint32_t j = 0; j < k; j++)
            KT_ASSERT(cpus[where[j]]->core_id != cpus[where[k]]->core_id);
    }
}

/* ---- work stealing and the topology ------------------------------------- */

static volatile bool st_release;
static volatile uint32_t st_started;

static void st_spinner(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&st_started, 1, __ATOMIC_RELAXED);
    while (!st_release)
        cpu_relax();
}

static void st_nothing(void *arg)
{
    (void)arg;
}

/* Wait (busy: this thread's core must stay busy) until n spinners run. */
static bool st_wait_started(uint32_t n)
{
    uint64_t end = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    while (__atomic_load_n(&st_started, __ATOMIC_RELAXED) < n)
        if (uptime_ns() > end)
            return false;
    return true;
}

/* A thread queued behind a busy one is stolen by the first idle CPU that
 * looks, and that one may be only half a core: here cpu 0's HT sibling,
 * with this thread busy on cpu 0. The steal must not keep it there while
 * a whole idle core (or an idle E-core) could run it: it goes where
 * placement would put it now. Each round: a hog on cpu v, the victim
 * queued behind it (lower priority), its mask widened to {h, v, w}, and
 * then h made to look for work at once (a thread that ends at once) while
 * w, halted, waits for its next tick. */
KTEST(steal_prefers_whole_core)
{
    KT_NEEDS_IDLE("asserts where a stolen thread runs: every other core must be idle");
    KT_SKIP_LIVE("asserts where a stolen thread runs among three chosen cores: a service on "
                 "one of them changes the right answer");
    if (cpu_count < 4 || !__atomic_load_n(&sched_place_order, __ATOMIC_RELAXED))
        return;
    int h = -1;
    for (uint32_t i = 1; i < cpu_count; i++)
        if (cpus[i]->core_id == cpus[0]->core_id)
            h = (int)i;
    uint32_t v = 0, w = 0;
    for (uint32_t i = 1; i < cpu_count && !w; i++) {
        if (cpus[i]->core_id == cpus[0]->core_id)
            continue;
        if (!v)
            v = i;
        else if (cpus[i]->core_id != cpus[v]->core_id)
            w = i;
    }
    if (h < 0 || !w) {
        kprintf("steal: needs cpu 0's HT sibling and two more cores (QEMU: -smp N,threads=2): "
                "not tested\n");
        return;
    }
    kt_pin_self(0);
    uint32_t on_half = 0;
    const int rounds = 5;
    for (int r = 0; r < rounds; r++) {
        cpumask_t mv;
        cpumask_one(&mv, v);
        st_release = false;
        st_started = 0;
        struct thread *hog = thread_create_on("kt-steal-hog", st_spinner, NULL,
                                              PRIO_DEFAULT - 1, &mv);
        KT_ASSERT(st_wait_started(1));
        struct thread *victim = thread_create_on("kt-steal-victim", st_spinner, NULL,
                                                 PRIO_DEFAULT - 2, &mv);
        cpumask_t m = mv;
        m.bits[h / 64] |= 1ull << (h % 64);
        m.bits[w / 64] |= 1ull << (w % 64);
        thread_set_affinity(victim, &m);
        cpumask_t mh;
        cpumask_one(&mh, (uint32_t)h);
        struct thread *poke = thread_create_on("kt-steal-poke", st_nothing, NULL,
                                               PRIO_DEFAULT, &mh);
        KT_ASSERT(st_wait_started(2));
        uint32_t ran = victim->cpu;
        st_release = true;
        thread_join(poke);
        thread_join(victim);
        thread_join(hog);
        kprintf("steal: round %d: victim queued on cpu %u ran on cpu %u (core %u); half core "
                "cpu %d, whole core cpu %u\n", r, v, ran, cpus[ran]->core_id, h, w);
        on_half += ran == (uint32_t)h;
    }
    kt_unpin_self();
    KT_EQ(on_half, 0);
}

/* ---- client/server pairs on sibling hyperthreads ------------------------- */

/* A thread pinned to cpu a and an unpinned partner (kept off cpu 0 and a)
 * wake each other in turn. As a pair, the partner is placed on a's idle HT
 * sibling; switched off, the hybrid order gives it a whole idle core. */
KTEST(affine_pair_uses_sibling)
{
    KT_NEEDS_IDLE("asserts placement on an idle HT sibling");
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
    bool keep = __atomic_load_n(&sched_affine_pair, __ATOMIC_RELAXED);
    __atomic_store_n(&sched_affine_pair, true, __ATOMIC_RELAXED);
    uint64_t pairs_on = pingpong_mask(a, &m);
    uint32_t on_sib_on = pp_ran_on[sib];
    __atomic_store_n(&sched_affine_pair, false, __ATOMIC_RELAXED);
    uint64_t pairs_off = pingpong_mask(a, &m);
    uint32_t on_sib_off = pp_ran_on[sib];
    __atomic_store_n(&sched_affine_pair, keep, __ATOMIC_RELAXED);
    kprintf("affine-pair: partner of cpu %u ran on its sibling cpu %d for %u of %d rounds "
            "(%lu pair placements); switched off: %u rounds, %lu\n", a, sib, on_sib_on,
            PP_ROUNDS, pairs_on, on_sib_off, pairs_off);
    KT_ASSERT(pairs_on >= PP_ROUNDS / 2);
    KT_ASSERT(on_sib_on >= PP_ROUNDS / 2);
    KT_EQ(pairs_off, 0);
    if (__atomic_load_n(&sched_place_order, __ATOMIC_RELAXED))
        KT_ASSERT(on_sib_off <= PP_ROUNDS / 4);   /* whole idle cores come first */
}

/* ---- wake-affine channel_call ---------------------------------------------------- */

#define AFF_CALLS 200

static struct channel *aff_client_ep;
static volatile uint32_t aff_ran_on[MAX_CPUS];

static void aff_server(void *arg)
{
    struct channel *ep = arg;
    for (;;) {
        signals_t s = 0;
        object_wait_one((struct kobject *)ep, SIG_READABLE | SIG_PEER_CLOSED,
                        uptime_ns() + 10 * NS_PER_S, &s);
        uint64_t m[2];
        uint32_t nb = 0;
        status_t st = channel_read(ep, m, sizeof(m), &nb, NULL, 0, NULL);
        if (st == OK) {
            preempt_disable();
            aff_ran_on[this_cpu()->index]++;
            preempt_enable();
            channel_write(ep, m, nb, NULL, 0);
        } else if (st != ERR_SHOULD_WAIT) {
            return;
        }
    }
}

static volatile uint32_t aff_bad;

static void aff_client(void *arg)
{
    (void)arg;
    for (int i = 0; i < AFF_CALLS; i++) {
        uint64_t req[2] = { 0, (uint64_t)i }, rep[2];
        uint32_t n = 0;
        if (channel_call(aff_client_ep, req, sizeof(req), NULL, 0, rep, sizeof(rep), &n, NULL, 0,
                         NULL, uptime_ns() + 10 * NS_PER_S) != OK || rep[1] != (uint64_t)i)
            aff_bad++;
    }
}

/* One ping-pong: client pinned to `ccpu`, server allowed on `smask`.
 * Returns the server's affine wake count; aff_ran_on says where it ran. */
static uint64_t aff_round(uint32_t ccpu, const cpumask_t *smask, uint64_t *client_affine)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    aff_client_ep = a;
    aff_bad = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        aff_ran_on[i] = 0;
    struct thread *srv = thread_create_on("aff-server", aff_server, b, PRIO_DEFAULT, smask);
    cpumask_t cm;
    cpumask_one(&cm, ccpu);
    struct thread *cl = thread_create_on("aff-client", aff_client, NULL, PRIO_DEFAULT, &cm);
    /* Read the counters before join drops our references. */
    while (!__atomic_load_n(&cl->exited, __ATOMIC_ACQUIRE))
        thread_sleep_ms(1);
    *client_affine = cl->affine_wakes;
    thread_join(cl);
    kobject_unref((struct kobject *)a);   /* the server sees PEER_CLOSED */
    while (!__atomic_load_n(&srv->exited, __ATOMIC_ACQUIRE))
        thread_sleep_ms(1);
    uint64_t sa = srv->affine_wakes;
    thread_join(srv);
    kobject_unref((struct kobject *)b);
    KT_EQ(aff_bad, 0);
    return sa;
}

KTEST(wake_affine_channel_call)
{
    KT_NEEDS_IDLE("counts wakeups placed on the caller's CPU and its idle sibling");
    if (cpu_count < 2)
        return;
    kt_pin_self(0);   /* keep the test thread off the client's CPU */
    /* The client's CPU: one with an HT sibling other than CPU 0 if any. */
    uint32_t c = 1;
    int sib = -1;
    for (uint32_t i = 1; i < cpu_count && sib < 0; i++)
        for (uint32_t j = 1; j < cpu_count; j++)
            if (j != i && cpus[j]->core_id == cpus[i]->core_id) {
                c = i;
                sib = (int)j;
                break;
            }
    cpumask_t any;
    cpumask_all(&any);
    uint64_t client_aff;
    uint64_t server_aff = aff_round(c, &any, &client_aff);
    kprintf("wake-affine: server placed affine %lu times, ran on cpu %u for %u of %d calls; "
            "client placed affine %lu times\n", server_aff, c, aff_ran_on[c], AFF_CALLS,
            client_aff);
    /* The request wakes the server onto the caller's CPU, and the reply
     * wakes the caller back onto it: nearly every call stays on one CPU. */
    KT_ASSERT(server_aff >= AFF_CALLS / 2);
    KT_ASSERT(aff_ran_on[c] >= AFF_CALLS / 2);
    KT_ASSERT(client_aff >= AFF_CALLS / 2);

    /* Server not allowed on the caller's CPU: it goes to the caller's idle
     * HT sibling when there is one (QEMU needs -smp N,threads=2). */
    cpumask_t not_c = any;
    not_c.bits[c / 64] &= ~(1ull << (c % 64));
    not_c.bits[0] &= ~1ull;   /* nor CPU 0, where this thread waits */
    server_aff = aff_round(c, &not_c, &client_aff);
    if (sib >= 0) {
        kprintf("wake-affine: server kept off cpu %u ran on its sibling cpu %d for %u of %d "
                "calls (%lu affine wakes)\n", c, sib, aff_ran_on[sib], AFF_CALLS, server_aff);
        KT_ASSERT(server_aff >= AFF_CALLS / 2);
        KT_ASSERT(aff_ran_on[sib] >= AFF_CALLS / 2);
    } else {
        kprintf("wake-affine: no HT sibling for cpu %u, sibling placement not tested\n", c);
        KT_EQ(server_aff, 0);
    }
    kt_unpin_self();
}

/* ---- starvation boost (the stress runs on the PC always show 0 boosts) ---- */

/* A CPU-bound thread and a lower-priority one, both pinned to one CPU so no
 * other CPU can steal the low one: it only ever runs when the scheduler's
 * once-a-second starvation boost lifts it. So within ~3 s it must have run,
 * and the boost counter must have moved. On the PC this runs in the
 * TSC-deadline tick mode QEMU can't emulate. */
static volatile bool sb_stop;
static volatile uint64_t sb_low_runs;

static void sb_hog(void *arg)
{
    (void)arg;
    while (!sb_stop)
        cpu_relax();
}

static void sb_low(void *arg)
{
    (void)arg;
    while (!sb_stop)
        sb_low_runs++;
}

KTEST(starvation_boost_rescues_low_priority)
{
    if (cpu_count < 2)
        return;
    uint32_t cpu = cpu_count - 1;
    cpumask_t m;
    cpumask_one(&m, cpu);
    sb_stop = false;
    sb_low_runs = 0;
    uint64_t boosts0 = sched_boost_count();
    struct thread *hog = thread_create_on("kt-hog", sb_hog, NULL, PRIO_DEFAULT + 4, &m);
    thread_sleep_ms(20);   /* the hog owns the CPU before the low one arrives */
    struct thread *low = thread_create_on("kt-low", sb_low, NULL, PRIO_DEFAULT - 4, &m);
    uint64_t t0 = uptime_ns();
    while (!sb_low_runs && uptime_ns() - t0 < kt_patience_ms(4000) * NS_PER_MS)
        thread_sleep_ms(10);
    uint64_t waited = uptime_ns() - t0, runs = sb_low_runs, boosts = sched_boost_count() - boosts0;
    sb_stop = true;
    thread_join(hog);
    thread_join(low);
    kprintf("starvation boost: low-priority thread first ran after %lu ms, %lu boost(s)\n",
            (unsigned long)(waited / NS_PER_MS), (unsigned long)boosts);
    KT_ASSERT(runs > 0);
    KT_ASSERT(boosts > 0);
    /* A boost comes within ~1-2 s (the checks run once a second). Timing:
     * under load the boosted thread still waits its turn behind the load. */
    KT_IDLE_ASSERT(waited < 3500 * NS_PER_MS);
}
