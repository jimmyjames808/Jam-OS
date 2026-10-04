/* The direct hand-off (kernel/sched/sched.c, "the direct hand-off"): a
 * wake from a thread that blocks right after hands it its CPU, and the run
 * queue's rules still hold. A higher-priority thread queued on the CPU
 * meanwhile runs first; a waker that doesn't block queues its wakee (by
 * sched_handoff_done, or its next schedule() when it is preempted); a pair
 * passing the CPU back and forth runs on one slice between them and leaves
 * room for a thread of the same priority on that CPU.
 *
 * Whether a wake really is handed over depends on nothing else being
 * queued on the CPU then, so the counts of offers are checked on an idle
 * machine only; the order in which threads run holds under load too. */
#include <jam/ktest.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/time.h>

static spinlock_t ho_lock = SPINLOCK_INIT("handoff test");
static uint32_t ho_order;   /* ho_lock: how many sleepers have run so far */

/* A thread that blocks until `go`, then notes when it ran. */
struct sleeper {
    struct thread *t;
    bool           go;    /* ho_lock */
    uint32_t       ran;   /* ho_lock: its place in ho_order once it ran, 0 before */
};

static void sleeper_main(void *arg)
{
    struct sleeper *s = arg;
    uint64_t f = spin_lock_irqsave(&ho_lock);
    while (!s->go)
        thread_block(&ho_lock, &f, DEADLINE_NEVER);
    s->ran = ++ho_order;
    spin_unlock_irqrestore(&ho_lock, f);
}

/* The test's CPU (pinned there) and a sleeper on it at priority prio,
 * blocked and switched out. */
static void sleeper_start(struct sleeper *s, uint32_t cpu, int prio)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    s->t = thread_create_on("handoff sleeper", sleeper_main, s, prio, &m);
    uint64_t d = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (thread_state(s->t) != T_BLOCKED || __atomic_load_n(&s->t->on_cpu, __ATOMIC_ACQUIRE)) {
        KT_ASSERT(uptime_ns() < d);
        thread_sleep_ms(1);   /* it shares our CPU */
    }
}

/* Block (ho_lock held) until every sleeper in s ran. */
static void wait_ran(struct sleeper *s, unsigned n, uint64_t *f)
{
    uint64_t d = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    for (unsigned i = 0; i < n; i++)
        while (!s[i].ran) {
            KT_ASSERT(uptime_ns() < d);
            thread_block(&ho_lock, f, uptime_ns() + NS_PER_MS);
        }
}

static uint32_t test_cpu(void)
{
    return kt_pin_self(cpu_count > 1 ? 1 : 0);
}

/* The waker hands its CPU to s, then wakes h, of a higher priority, onto
 * the same CPU's queue, then blocks: h runs first, s after it. */
KTEST(handoff_higher_priority_queued_runs_first)
{
    uint32_t cpu = test_cpu();
    int prio = __atomic_load_n(&current_thread()->base_prio, __ATOMIC_RELAXED);
    struct sleeper s[2] = { { 0 }, { 0 } };   /* s[0] the wakee handed the CPU, s[1] "h" */
    ho_order = 0;
    sleeper_start(&s[0], cpu, prio);
    sleeper_start(&s[1], cpu, prio + 2);
    uint64_t offers = s[0].t->handoff_offers, taken = s[0].t->handoffs;

    uint64_t f = spin_lock_irqsave(&ho_lock);
    s[0].go = s[1].go = true;
    thread_set_handoff(true);
    thread_wake_sync(s[0].t);   /* handed this CPU */
    thread_set_handoff(false);
    thread_wake(s[1].t);        /* queued here, and it outranks s[0] */
    wait_ran(s, 2, &f);         /* the block that would run s[0] */
    spin_unlock_irqrestore(&ho_lock, f);
    sched_handoff_done();

    KT_EQ(s[1].ran, 1);
    KT_EQ(s[0].ran, 2);
    KT_IDLE_EQ(s[0].t->handoff_offers - offers, 1);   /* it was handed the CPU... */
    KT_EQ(s[0].t->handoffs - taken, 0);                /* ...and queued behind h */
    thread_join(s[0].t);
    thread_join(s[1].t);
    kt_unpin_self();
}

/* A waker that hands its CPU over and then doesn't block: the wakee is
 * queued by sched_handoff_done and runs as soon as the waker blocks; or,
 * when the waker yields first, by that schedule(), ahead of the waker. */
KTEST(handoff_waker_not_blocking_queues_wakee)
{
    uint32_t cpu = test_cpu();
    int prio = __atomic_load_n(&current_thread()->base_prio, __ATOMIC_RELAXED);
    for (int yield = 0; yield < 2; yield++) {
        struct sleeper s = { 0 };
        ho_order = 0;
        sleeper_start(&s, cpu, prio);
        uint64_t offers = s.t->handoff_offers, taken = s.t->handoffs;
        thread_set_handoff(true);
        uint64_t f = spin_lock_irqsave(&ho_lock);
        s.go = true;
        thread_wake_sync(s.t);
        spin_unlock_irqrestore(&ho_lock, f);
        thread_set_handoff(false);
        KT_IDLE_ASSERT(!__atomic_load_n(&s.t->rq_node.next, __ATOMIC_RELAXED));   /* handed */
        if (yield) {
            thread_yield();   /* a schedule() that doesn't take it: it queues it first */
            f = spin_lock_irqsave(&ho_lock);
            KT_IDLE_EQ(s.ran, 1);   /* it ran before we did again */
            spin_unlock_irqrestore(&ho_lock, f);
        }
        sched_handoff_done();   /* queued now, if nothing took it yet */
        f = spin_lock_irqsave(&ho_lock);
        KT_IDLE_ASSERT(yield || !s.ran);   /* not run: we never blocked */
        wait_ran(&s, 1, &f);
        spin_unlock_irqrestore(&ho_lock, f);
        KT_IDLE_EQ(s.t->handoff_offers - offers, 1);
        KT_EQ(s.t->handoffs - taken, 0);
        thread_join(s.t);
    }
    kt_unpin_self();
}

/* ---- a ping-pong pair -------------------------------------------------------- */

static struct {
    struct thread *t[2];       /* the pair */
    uint32_t       turn;       /* ho_lock: whose move (0, 1), 2: nobody's yet */
    bool           stop;       /* ho_lock */
    uint64_t       rounds;     /* ho_lock: moves of t[1] */
    uint32_t       min_slice;  /* ho_lock: the smallest slice t[1] started a move with */
} pp;

static uint64_t spin_count;   /* atomic: the third thread's loops */
static bool spin_stop;        /* atomic */

static void pp_main(void *arg)
{
    uint32_t me = (uint32_t)(uintptr_t)arg;
    uint64_t f = spin_lock_irqsave(&ho_lock);
    for (;;) {
        while (pp.turn != me && !pp.stop)
            thread_block(&ho_lock, &f, DEADLINE_NEVER);
        if (pp.stop)
            break;
        if (me) {
            pp.rounds++;
            uint32_t sl = current_thread()->slice;
            pp.min_slice = sl < pp.min_slice ? sl : pp.min_slice;
        }
        pp.turn = !me;
        thread_set_handoff(true);
        thread_wake_sync(pp.t[!me]);   /* we block for our next move right after */
        thread_set_handoff(false);
    }
    thread_wake(pp.t[!me]);
    spin_unlock_irqrestore(&ho_lock, f);
    sched_handoff_done();
}

static void spinner(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&spin_stop, __ATOMIC_ACQUIRE))
        __atomic_add_fetch(&spin_count, 1, __ATOMIC_RELAXED);
}

static uint64_t pp_rounds(void)
{
    uint64_t f = spin_lock_irqsave(&ho_lock);
    uint64_t r = pp.rounds;
    spin_unlock_irqrestore(&ho_lock, f);
    return r;
}

/* Wait (bounded) until pp.rounds passes `rounds`. */
static void wait_rounds(uint64_t rounds)
{
    uint64_t d = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (pp_rounds() < rounds) {
        KT_ASSERT(uptime_ns() < d);
        thread_sleep_ms(1);
    }
}

/* Two threads waking each other on one CPU, each blocking right after:
 * every move is a hand-off, and the handed thread starts on what is left
 * of its partner's slice (it is not refreshed). A third thread of the same
 * priority on that CPU still runs: a hand-off is never made past it. */
KTEST(handoff_pingpong_pair_shares_its_slice)
{
    if (cpu_count < 2)
        return;   /* we watch from another CPU */
    kt_pin_self(0);
    cpumask_t m;
    cpumask_one(&m, 1);
    pp.turn = 2;
    pp.stop = false;
    pp.rounds = 0;
    pp.min_slice = SLICE_TICKS;
    for (uint32_t i = 0; i < 2; i++)
        pp.t[i] = thread_create_on("handoff pair", pp_main, (void *)(uintptr_t)i, PRIO_DEFAULT,
                                   &m);
    uint64_t taken = __atomic_load_n(&pp.t[1]->handoffs, __ATOMIC_RELAXED);
    uint64_t f = spin_lock_irqsave(&ho_lock);
    pp.turn = 0;
    thread_wake(pp.t[0]);
    spin_unlock_irqrestore(&ho_lock, f);
    /* Long enough for a few ticks: the slice runs down across moves. */
    uint64_t until = uptime_ns() + 40 * NS_PER_MS;
    while (uptime_ns() < until)
        thread_sleep_ms(2);
    wait_rounds(100);
    f = spin_lock_irqsave(&ho_lock);
    uint32_t min_slice = pp.min_slice;
    spin_unlock_irqrestore(&ho_lock, f);
    KT_IDLE_ASSERT(__atomic_load_n(&pp.t[1]->handoffs, __ATOMIC_RELAXED) - taken >= 50);
    KT_IDLE_ASSERT(min_slice < SLICE_TICKS);

    /* A spinner of the same priority joins them on their CPU: it runs, and
     * so do they. */
    __atomic_store_n(&spin_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&spin_stop, false, __ATOMIC_RELAXED);
    struct thread *x = thread_create_on("handoff spinner", spinner, NULL, PRIO_DEFAULT, &m);
    uint64_t d = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    while (!__atomic_load_n(&spin_count, __ATOMIC_RELAXED)) {
        KT_ASSERT(uptime_ns() < d);
        thread_sleep_ms(1);
    }
    wait_rounds(pp_rounds() + 5);
    uint64_t seen = __atomic_load_n(&spin_count, __ATOMIC_RELAXED);
    wait_rounds(pp_rounds() + 5);
    KT_ASSERT(__atomic_load_n(&spin_count, __ATOMIC_RELAXED) > seen);   /* still its turns */

    __atomic_store_n(&spin_stop, true, __ATOMIC_RELEASE);
    thread_join(x);
    f = spin_lock_irqsave(&ho_lock);
    pp.stop = true;
    thread_wake(pp.t[0]);
    thread_wake(pp.t[1]);
    spin_unlock_irqrestore(&ho_lock, f);
    thread_join(pp.t[0]);
    thread_join(pp.t[1]);
    kt_unpin_self();
}
