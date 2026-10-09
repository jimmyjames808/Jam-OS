/* thread_interrupt (kernel/sched/wait.c) against the interruptible waits
 * of object_wait_one, port_wait and sleep_until, from kernel threads
 * (futex_wait's are in test_futex.c, which has an address space for the
 * word): an interrupt before a wait, during it and after it ends that
 * wait or the next, none lost and none counted twice, under a storm on
 * two CPUs too; and channel_call, which is never interrupted.
 *
 * Every test leaves no interrupt latched on any thread it ran, the test
 * thread itself included (the next test would see it). */
#include <jam/channel.h>
#include <jam/event.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/percpu.h>
#include <jam/port.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>

enum { W_OBJECT, W_PORT, W_SLEEP, W_KINDS };

struct iw {
    struct event *ev;     /* never signalled unless a test says so */
    struct port  *port;   /* never sent to */
};

static void iw_make(struct iw *w)
{
    KT_EQ(event_create(&w->ev), OK);
    KT_EQ(port_create(&w->port), OK);
}

static void iw_drop(struct iw *w)
{
    kobject_unref(&w->ev->base);
    kobject_unref(&w->port->base);
}

/* One interruptible wait of kind k until deadline. */
static status_t wait_kind(struct iw *w, unsigned k, uint64_t deadline)
{
    struct port_packet pkt;
    switch (k) {
    case W_OBJECT: return object_wait_one_interruptible(&w->ev->base, SIG_SIGNALED, deadline, NULL);
    case W_PORT:   return port_wait_interruptible(w->port, deadline, &pkt);
    default:       return thread_sleep_until_interruptible(deadline);
    }
}

/* What a wait of kind k that nothing ends returns at its deadline. */
static status_t timed_out(unsigned k)
{
    return k == W_SLEEP ? OK : ERR_TIMED_OUT;
}

/* Before: latched (twice: still one), it ends the next wait at once; the
 * wait after that runs to its deadline. A wait whose condition is met
 * returns OK and leaves the interrupt for the next. The plain waits never
 * see it. */
KTEST(intr_before_wait_latched_once)
{
    struct iw w;
    iw_make(&w);
    struct thread *me = current_thread();
    for (unsigned k = 0; k < W_KINDS; k++) {
        thread_interrupt(me);
        thread_interrupt(me);
        uint64_t t0 = uptime_ns();
        KT_EQ(wait_kind(&w, k, t0 + 5 * NS_PER_S), ERR_INTERRUPTED);
        KT_ASSERT(uptime_ns() - t0 < NS_PER_S);
        KT_ASSERT(!thread_interrupt_pending(me));
        KT_EQ(wait_kind(&w, k, uptime_ns() + NS_PER_MS), timed_out(k));
    }
    KT_EQ(event_signal(w.ev, 0, SIG_SIGNALED), OK);
    thread_interrupt(me);
    KT_EQ(wait_kind(&w, W_OBJECT, DEADLINE_NEVER), OK);
    KT_ASSERT(thread_interrupt_pending(me));
    /* The non-interruptible forms wait on as if nothing came. */
    KT_EQ(object_wait_one(&w.ev->base, SIG_USER_ALL, uptime_ns() + NS_PER_MS, NULL),
          ERR_TIMED_OUT);
    KT_EQ(thread_sleep_cancellable(NS_PER_MS), OK);
    KT_EQ(wait_kind(&w, W_PORT, DEADLINE_NEVER), ERR_INTERRUPTED);
    KT_ASSERT(!thread_interrupt_pending(me));
    iw_drop(&w);
}

/* ---- during and after --------------------------------------------------------- */

struct iw_waiter {
    struct iw       *w;
    unsigned         k;
    bool             go_second;   /* the test let it make its second wait (RELEASE) */
    status_t         first, second;
    uint64_t         second_deadline;
    struct thread   *t;
};

static void during_main(void *arg)
{
    struct iw_waiter *x = arg;
    x->first = wait_kind(x->w, x->k, uptime_ns() + 10 * NS_PER_S);
    while (!__atomic_load_n(&x->go_second, __ATOMIC_ACQUIRE))
        thread_sleep_ms(1);
    x->second = wait_kind(x->w, x->k, x->second_deadline);
}

/* During: a thread blocked in each wait is ended by one interrupt (and only
 * one: its next wait runs to its deadline). */
KTEST(intr_during_wait_ends_it_once)
{
    struct iw w;
    iw_make(&w);
    for (unsigned k = 0; k < W_KINDS; k++) {
        struct iw_waiter x = { .w = &w, .k = k };
        x.t = thread_create("kt-intr", during_main, &x, PRIO_DEFAULT);
        thread_sleep_ms(5);   /* most likely blocked by now (either way works) */
        thread_interrupt(x.t);
        x.second_deadline = uptime_ns() + 20 * NS_PER_MS;
        __atomic_store_n(&x.go_second, true, __ATOMIC_RELEASE);
        thread_join(x.t);
        KT_EQ(x.first, ERR_INTERRUPTED);
        KT_EQ(x.second, timed_out(k));
    }
    iw_drop(&w);
}

static void after_main(void *arg)
{
    struct iw_waiter *x = arg;
    x->first = wait_kind(x->w, W_OBJECT, uptime_ns() + 10 * NS_PER_S);   /* signalled: OK */
    while (!__atomic_load_n(&x->go_second, __ATOMIC_ACQUIRE))
        thread_sleep_ms(1);
    x->second = wait_kind(x->w, x->k, x->second_deadline);
}

/* After: an interrupt that comes once a wait has ended is not lost: it
 * ends the next one. */
KTEST(intr_after_wait_not_lost)
{
    struct iw w;
    iw_make(&w);
    KT_EQ(event_signal(w.ev, 0, SIG_SIGNALED), OK);
    for (unsigned k = W_PORT; k < W_KINDS; k++) {
        struct iw_waiter x = { .w = &w, .k = k };
        x.t = thread_create("kt-intr", after_main, &x, PRIO_DEFAULT);
        thread_sleep_ms(5);
        thread_interrupt(x.t);
        x.second_deadline = uptime_ns() + 10 * NS_PER_S;
        uint64_t t0 = uptime_ns();
        __atomic_store_n(&x.go_second, true, __ATOMIC_RELEASE);
        thread_join(x.t);
        KT_EQ(x.first, OK);
        KT_EQ(x.second, ERR_INTERRUPTED);
        KT_ASSERT(uptime_ns() - t0 < 5 * NS_PER_S);
    }
    iw_drop(&w);
}

/* ---- the storm ------------------------------------------------------------------ */

#define STORM_ROUNDS 3000

struct storm {
    struct iw    *w;
    uint32_t      taken;     /* ERR_INTERRUPTED returns (RELEASE: the interrupter waits on it) */
    uint32_t      lost;      /* waits that ran to their far deadline */
    uint32_t      wrong;     /* any other status */
    bool          stop;      /* the test is over (RELEASE) */
};

static void storm_waiter(void *arg)
{
    struct storm *s = arg;
    for (unsigned i = 0; !__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE); i++) {
        status_t st = wait_kind(s->w, i % W_KINDS, uptime_ns() + kt_patience_ms(2000) * NS_PER_MS);
        if (st == ERR_INTERRUPTED)
            __atomic_add_fetch(&s->taken, 1, __ATOMIC_RELEASE);
        else if (st == timed_out(i % W_KINDS))
            s->lost += !__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE);
        else
            s->wrong++;
    }
}

/* One interrupt at a time, each sent at a random moment against a waiter
 * on another CPU that goes straight into its next wait: before it blocks,
 * as it blocks, while it sleeps. Each must end exactly one wait: the
 * interrupter waits for the count to move before it sends the next, and
 * a lost one would leave the waiter asleep to its far deadline. */
KTEST(intr_storm_none_lost_none_twice)
{
    struct iw w;
    iw_make(&w);
    struct storm s = { .w = &w };
    cpumask_t m;
    cpumask_one(&m, cpu_count > 1 ? 1 : 0);
    struct thread *t = thread_create_on("kt-storm", storm_waiter, &s, PRIO_DEFAULT, &m);
    uint64_t seed = rdtsc() | 1;
    unsigned sent = 0;
    for (; sent < STORM_ROUNDS; sent++) {
        if (kt_rng(&seed) % 2)
            udelay(kt_rng(&seed) % 20);
        thread_interrupt(t);
        uint64_t end = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
        while (__atomic_load_n(&s.taken, __ATOMIC_ACQUIRE) == sent && uptime_ns() < end)
            thread_yield();
        if (__atomic_load_n(&s.taken, __ATOMIC_ACQUIRE) == sent)
            break;   /* lost: the checks below say so */
    }
    thread_sleep_ms(5);   /* a second count for one interrupt would show by now */
    uint32_t taken = __atomic_load_n(&s.taken, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s.stop, true, __ATOMIC_RELEASE);
    thread_interrupt(t);   /* out of its last wait */
    thread_join(t);
    KT_EQ(sent, STORM_ROUNDS);
    KT_EQ(taken, STORM_ROUNDS);
    KT_EQ(s.lost, 0);
    KT_EQ(s.wrong, 0);
    iw_drop(&w);
}

/* ---- channel_call -------------------------------------------------------------- */

struct cc {
    struct channel *client, *server;
    struct thread  *caller;
    bool            done;       /* the caller is through (RELEASE) */
    status_t        st;
    uint32_t        reply;
    bool            latched;    /* the interrupts were still there after the call */
    status_t        next_wait;  /* its next interruptible wait */
};

static void cc_caller(void *arg)
{
    struct cc *c = arg;
    uint32_t req[2] = { 0, 41 }, rep[2] = { 0, 0 };
    uint32_t nb = 0;
    c->st = channel_call(c->client, req, sizeof(req), NULL, 0, rep, sizeof(rep), &nb, NULL, 0,
                         NULL, uptime_ns() + 10 * NS_PER_S);
    c->reply = rep[1];
    c->latched = thread_interrupt_pending(current_thread());
    c->next_wait = thread_sleep_until_interruptible(uptime_ns() + 5 * NS_PER_S);
    __atomic_store_n(&c->done, true, __ATOMIC_RELEASE);
}

/* A thread in channel_call, interrupted again and again while the server
 * takes its time: the call goes on to its reply; the interrupts wait,
 * latched, for the caller's next interruptible wait. */
KTEST(intr_channel_call_never_interrupted)
{
    struct cc c = { 0 };
    KT_EQ(channel_create(&c.client, &c.server), OK);
    c.caller = thread_create("kt-cc", cc_caller, &c, PRIO_DEFAULT);
    KT_EQ(object_wait_one((struct kobject *)c.server, SIG_READABLE, uptime_ns() + 5 * NS_PER_S,
                          NULL),
          OK);
    uint32_t req[2];
    uint32_t nb;
    KT_EQ(channel_read(c.server, req, sizeof(req), &nb, NULL, 0, NULL), OK);
    uint64_t end = uptime_ns() + 30 * NS_PER_MS;
    unsigned n = 0;
    while (uptime_ns() < end) {   /* the caller is in its call all this time */
        thread_interrupt(c.caller);
        n++;
        udelay(20);
    }
    KT_ASSERT(!__atomic_load_n(&c.done, __ATOMIC_ACQUIRE));
    req[1] += 1;
    KT_EQ(channel_write(c.server, req, sizeof(req), NULL, 0), OK);
    thread_join(c.caller);
    KT_EQ(c.st, OK);
    KT_EQ(c.reply, 42);
    KT_ASSERT(c.latched);
    KT_EQ(c.next_wait, ERR_INTERRUPTED);
    kprintf("intr: channel_call went on through %u interrupts\n", n);
    kobject_unref((struct kobject *)c.client);
    kobject_unref((struct kobject *)c.server);
}
