/* Tests for events, timers, ports and their handle-level layer, and the
 * cap on a port's bindings. */
#include <jam/event.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/port.h>
#include <jam/sched.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/timer.h>


/* A watched object that counts its own destruction. */
struct pobj {
    struct kobject base;       /* a real object a port can watch */
    volatile int *destroyed;   /* incremented by destroy */
};

static void pobj_destroy(struct kobject *o)
{
    struct pobj *t = (struct pobj *)o;
    if (t->destroyed)
        (*t->destroyed)++;
    kfree(t);
}

static const struct kobject_ops pobj_ops = { .name = "test object", .destroy = pobj_destroy };

static struct pobj *pobj_new(volatile int *destroyed)
{
    struct pobj *t = kzalloc(sizeof(*t));
    kobject_init(&t->base, OBJ_EVENT, &pobj_ops, "test object", 0);
    t->destroyed = destroyed;
    return t;
}

static struct event *new_event(void)
{
    struct event *e;
    KT_EQ(event_create(&e), OK);
    return e;
}

static struct port *new_port(void)
{
    struct port *p;
    KT_EQ(port_create(&p), OK);
    return p;
}

static struct ktimer *new_timer(void)
{
    struct ktimer *t;
    KT_EQ(timer_create(&t), OK);
    return t;
}

/* Nothing queued: a short wait times out. */
static void expect_empty(struct port *p)
{
    struct port_packet pkt;
    KT_EQ(port_wait(p, uptime_ns() + 5 * NS_PER_MS, &pkt), ERR_TIMED_OUT);
    KT_ASSERT(!(kobject_signals(&p->base) & SIG_READABLE));
}

static void expect_signal(struct port *p, uint64_t key, uint64_t count)
{
    struct port_packet pkt;
    KT_EQ(port_wait(p, uptime_ns() + 2000 * NS_PER_MS, &pkt), OK);
    KT_EQ(pkt.type, PORT_PACKET_SIGNAL);
    KT_EQ(pkt.status, OK);
    KT_EQ(pkt.key, key);
    KT_EQ(pkt.signal.count, count);
}

static void stats_equal(const struct port_stats *a)
{
    struct port_stats b;
    port_get_stats(&b);
    KT_EQ(b.ports, a->ports);
    KT_EQ(b.bindings, a->bindings);
    KT_EQ(b.user_packets, a->user_packets);
}

/* ---- events -------------------------------------------------------------- */

static void event_later(void *arg)
{
    thread_sleep_ms(30);
    event_signal(arg, 0, SIG_SIGNALED);
}

KTEST(event_signal_wait)
{
    struct event *e = new_event();
    signals_t seen;
    KT_EQ(kobject_signals(&e->base), 0);

    KT_EQ(event_signal(e, 0, SIG_SIGNALED | (1u << 24)), OK);
    KT_EQ(object_wait_one(&e->base, SIG_SIGNALED, DEADLINE_NEVER, &seen), OK);
    KT_EQ(seen, SIG_SIGNALED | (1u << 24));

    /* Cleared: a wait times out, never early. */
    KT_EQ(event_signal(e, SIG_SIGNALED, 0), OK);
    KT_EQ(kobject_signals(&e->base), 1u << 24);
    uint64_t t0 = uptime_ns();
    KT_EQ(object_wait_one(&e->base, SIG_SIGNALED, t0 + 20 * NS_PER_MS, &seen), ERR_TIMED_OUT);
    KT_ASSERT(uptime_ns() - t0 >= 20 * NS_PER_MS);

    /* Set by another thread. */
    struct thread *t = thread_create("event signaller", event_later, e, PRIO_DEFAULT);
    KT_EQ(object_wait_one(&e->base, SIG_SIGNALED, uptime_ns() + 2000 * NS_PER_MS, &seen), OK);
    KT_ASSERT(seen & SIG_SIGNALED);
    thread_join(t);

    /* Clear and set in one call: clear applies first. */
    KT_EQ(event_signal(e, SIG_USER_ALL | SIG_SIGNALED, 1u << 31), OK);
    KT_EQ(kobject_signals(&e->base), 1u << 31);
    kobject_unref(&e->base);
}

KTEST(event_rejects_non_user_bits)
{
    struct event *e = new_event();
    KT_EQ(event_signal(e, 0, SIG_READABLE), ERR_INVALID_ARGS);
    KT_EQ(event_signal(e, 0, SIG_SIGNALED | SIG_PEER_CLOSED), ERR_INVALID_ARGS);
    KT_EQ(event_signal(e, 1u << 23, 0), ERR_INVALID_ARGS);
    KT_EQ(kobject_signals(&e->base), 0);   /* rejected calls change nothing */
    KT_EQ(event_signal(e, 0, SIG_USER_ALL), OK);
    KT_EQ(kobject_signals(&e->base), SIG_USER_ALL);
    kobject_unref(&e->base);
}

/* ---- timers -------------------------------------------------------------- */

/* Wait for t to fire; check it was not early and at most 30 ms late. */
static void expect_fires(struct ktimer *t, uint64_t deadline)
{
    KT_EQ(object_wait_one(&t->base, SIG_SIGNALED, deadline + 2000 * NS_PER_MS, NULL), OK);
    uint64_t now = uptime_ns();
    KT_ASSERT(now >= deadline);
    if (now - deadline > 30 * NS_PER_MS)
        panic("ktest %s: timer %lu us late", ktest_current, (now - deadline) / 1000);
}

KTEST(timer_fires_on_time)
{
    struct ktimer *t = new_timer();
    for (int i = 0; i < 5; i++) {
        uint64_t deadline = uptime_ns() + (10 + 17 * i) * NS_PER_MS;
        KT_EQ(timer_set(t, deadline), OK);
        KT_ASSERT(!(kobject_signals(&t->base) & SIG_SIGNALED));   /* re-arming clears it */
        expect_fires(t, deadline);
    }
    /* A deadline already passed fires at once. */
    KT_EQ(timer_set(t, uptime_ns() - 1), OK);
    KT_ASSERT(kobject_signals(&t->base) & SIG_SIGNALED);
    KT_EQ(timer_set(t, 0), OK);
    KT_ASSERT(kobject_signals(&t->base) & SIG_SIGNALED);
    kobject_unref(&t->base);
}

KTEST(timer_reset_and_cancel)
{
    struct ktimer *t = new_timer();

    /* Re-set to an earlier deadline: the service must re-plan. */
    uint64_t now = uptime_ns();
    KT_EQ(timer_set(t, now + 500 * NS_PER_MS), OK);
    thread_sleep_ms(5);
    KT_EQ(timer_set(t, now + 40 * NS_PER_MS), OK);
    expect_fires(t, now + 40 * NS_PER_MS);

    /* Re-set to a later deadline: the old one must not fire. */
    now = uptime_ns();
    KT_EQ(timer_set(t, now + 30 * NS_PER_MS), OK);
    KT_EQ(timer_set(t, now + 120 * NS_PER_MS), OK);
    KT_EQ(object_wait_one(&t->base, SIG_SIGNALED, now + 90 * NS_PER_MS, NULL), ERR_TIMED_OUT);
    expect_fires(t, now + 120 * NS_PER_MS);

    /* Cancel: disarms and clears. */
    now = uptime_ns();
    KT_EQ(timer_set(t, now + 30 * NS_PER_MS), OK);
    KT_EQ(timer_cancel(t), OK);
    KT_EQ(object_wait_one(&t->base, SIG_SIGNALED, now + 80 * NS_PER_MS, NULL), ERR_TIMED_OUT);
    KT_EQ(timer_set(t, 0), OK);
    KT_ASSERT(kobject_signals(&t->base) & SIG_SIGNALED);
    KT_EQ(timer_cancel(t), OK);
    KT_EQ(kobject_signals(&t->base), 0);
    KT_EQ(timer_cancel(t), OK);   /* already disarmed */

    /* Destroying an armed timer disarms it. */
    KT_EQ(timer_set(t, uptime_ns() + 20 * NS_PER_MS), OK);
    kobject_unref(&t->base);
    thread_sleep_ms(40);   /* the service must not touch the freed timer */
}

KTEST(timer_many_fire_in_deadline_order)
{
    enum { N = 16 };
    struct port *p = new_port();
    struct ktimer *ts[N];
    uint64_t deadline[N];
    uint64_t base = uptime_ns() + 30 * NS_PER_MS;
    for (int i = 0; i < N; i++) {
        ts[i] = new_timer();
        deadline[i] = base + (uint64_t)((i * 7) % N) * 4 * NS_PER_MS;   /* distinct, shuffled */
        KT_EQ(port_bind(p, &ts[i]->base, i, SIG_SIGNALED, PORT_BIND_ONCE), OK);
        KT_EQ(timer_set(ts[i], deadline[i]), OK);
    }
    uint64_t last = 0;
    for (int n = 0; n < N; n++) {
        struct port_packet pkt;
        KT_EQ(port_wait(p, uptime_ns() + 2000 * NS_PER_MS, &pkt), OK);
        KT_ASSERT(pkt.key < N);
        uint64_t now = uptime_ns();
        KT_ASSERT(now >= deadline[pkt.key]);
        KT_ASSERT(deadline[pkt.key] > last);   /* in deadline order */
        last = deadline[pkt.key];
    }
    KT_ASSERT(uptime_ns() - last <= 30 * NS_PER_MS);
    expect_empty(p);
    for (int i = 0; i < N; i++)
        kobject_unref(&ts[i]->base);
    kobject_unref(&p->base);
}

/* ---- ports --------------------------------------------------------------- */

KTEST(port_once_vs_persistent)
{
    struct port_stats before;
    port_get_stats(&before);
    struct port *p = new_port();
    struct event *once = new_event(), *pers = new_event();
    KT_EQ(port_bind(p, &once->base, 1, SIG_SIGNALED, PORT_BIND_ONCE), OK);
    KT_EQ(port_bind(p, &pers->base, 2, SIG_SIGNALED | (1u << 30), PORT_BIND_PERSISTENT), OK);
    expect_empty(p);

    /* ONCE: one packet, then nothing however often it re-matches. */
    event_signal(once, 0, SIG_SIGNALED);
    KT_ASSERT(kobject_signals(&p->base) & SIG_READABLE);
    struct port_packet pkt;
    KT_EQ(port_wait(p, DEADLINE_NEVER, &pkt), OK);
    KT_EQ(pkt.key, 1);
    KT_EQ(pkt.type, PORT_PACKET_SIGNAL);
    KT_EQ(pkt.signal.trigger, SIG_SIGNALED);
    KT_EQ(pkt.signal.observed, SIG_SIGNALED);
    KT_EQ(pkt.signal.count, 1);
    event_signal(once, SIG_SIGNALED, 0);
    event_signal(once, 0, SIG_SIGNALED);
    expect_empty(p);

    /* PERSISTENT: edges while its packet is queued coalesce into it. A
     * change that stays matching is not an edge. */
    for (int i = 0; i < 5; i++) {
        event_signal(pers, 0, SIG_SIGNALED);
        event_signal(pers, 0, 1u << 30);          /* still matching */
        event_signal(pers, SIG_SIGNALED | (1u << 30), 0);
    }
    event_signal(pers, 0, 1u << 30);
    KT_EQ(port_wait(p, DEADLINE_NEVER, &pkt), OK);
    KT_EQ(pkt.key, 2);
    KT_EQ(pkt.signal.count, 6);
    KT_EQ(pkt.signal.trigger, SIG_SIGNALED | (1u << 30));
    KT_EQ(pkt.signal.observed, 1u << 30);   /* as of the latest edge */
    expect_empty(p);
    /* Still matching: no new edge until it goes low and high again. */
    event_signal(pers, 0, SIG_SIGNALED);
    expect_empty(p);
    event_signal(pers, SIG_SIGNALED | (1u << 30), 0);
    event_signal(pers, 0, SIG_SIGNALED);
    expect_signal(p, 2, 1);

    kobject_unref(&p->base);
    kobject_unref(&once->base);
    kobject_unref(&pers->base);
    stats_equal(&before);
}

KTEST(port_bind_when_already_matching)
{
    struct port *p = new_port();
    struct event *e = new_event();
    event_signal(e, 0, SIG_SIGNALED);
    KT_EQ(port_bind(p, &e->base, 7, SIG_SIGNALED, PORT_BIND_ONCE), OK);
    KT_ASSERT(kobject_signals(&p->base) & SIG_READABLE);
    KT_EQ(port_bind(p, &e->base, 8, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);
    expect_signal(p, 7, 1);
    expect_signal(p, 8, 1);
    expect_empty(p);
    KT_EQ(port_unbind(p, &e->base, 7), ERR_NOT_FOUND);   /* spent */
    KT_ASSERT(list_empty(&e->base.observers) == false);    /* 8 is still bound */
    kobject_unref(&p->base);
    KT_ASSERT(list_empty(&e->base.observers));
    kobject_unref(&e->base);
}

KTEST(port_wait_timeout)
{
    struct port *p = new_port();
    struct port_packet pkt;
    uint64_t t0 = uptime_ns();
    KT_EQ(port_wait(p, t0 + 30 * NS_PER_MS, &pkt), ERR_TIMED_OUT);
    KT_ASSERT(uptime_ns() - t0 >= 30 * NS_PER_MS);
    KT_EQ(port_wait(p, 0, &pkt), ERR_TIMED_OUT);   /* deadline in the past: poll */
    kobject_unref(&p->base);
}

KTEST(port_user_packets_fifo)
{
    struct port_stats before;
    port_get_stats(&before);
    struct port *p = new_port();
    enum { N = 100 };
    for (uint64_t i = 0; i < N; i++) {
        struct port_packet pkt = {
            .key = 1000 + i, .type = PORT_PACKET_SIGNAL /* forced to USER */, .status = 5,
            .user = { { i, i * 3, ~i, 42 } },
        };
        KT_EQ(port_queue_user(p, &pkt), OK);
    }
    KT_ASSERT(kobject_signals(&p->base) & SIG_READABLE);
    for (uint64_t i = 0; i < N; i++) {
        struct port_packet pkt;
        KT_EQ(port_wait(p, DEADLINE_NEVER, &pkt), OK);
        KT_EQ(pkt.key, 1000 + i);
        KT_EQ(pkt.type, PORT_PACKET_USER);
        KT_EQ(pkt.status, 5);
        KT_ASSERT(pkt.user.data[0] == i && pkt.user.data[1] == i * 3 &&
                  pkt.user.data[2] == ~i && pkt.user.data[3] == 42);
    }
    expect_empty(p);

    /* A full queue refuses more; destroying the port frees what is queued. */
    struct port_packet pkt = { .key = 1 };
    for (int i = 0; i < PORT_MAX_USER_PACKETS; i++)
        KT_EQ(port_queue_user(p, &pkt), OK);
    KT_EQ(port_queue_user(p, &pkt), ERR_NO_RESOURCES);
    kobject_unref(&p->base);
    stats_equal(&before);
}

KTEST(port_unbind)
{
    struct port_stats before;
    port_get_stats(&before);
    struct port *p = new_port();
    struct event *e = new_event();

    KT_EQ(port_bind(p, &e->base, 1, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);
    KT_EQ(port_unbind(p, &e->base, 2), ERR_NOT_FOUND);   /* wrong key */
    KT_EQ(port_unbind(p, &e->base, 1), OK);
    KT_ASSERT(list_empty(&e->base.observers));
    event_signal(e, 0, SIG_SIGNALED);
    expect_empty(p);
    KT_EQ(port_unbind(p, &e->base, 1), ERR_NOT_FOUND);

    /* A queued packet survives unbinding its binding. */
    event_signal(e, SIG_SIGNALED, 0);
    KT_EQ(port_bind(p, &e->base, 3, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);
    event_signal(e, 0, SIG_SIGNALED);
    KT_EQ(port_unbind(p, &e->base, 3), OK);
    event_signal(e, SIG_SIGNALED, 0);
    event_signal(e, 0, SIG_SIGNALED);   /* unbound: not counted */
    expect_signal(p, 3, 1);
    expect_empty(p);

    /* A fired ONCE binding is gone, but its packet stays queued. */
    KT_EQ(port_bind(p, &e->base, 4, SIG_SIGNALED, PORT_BIND_ONCE), OK);
    KT_EQ(port_unbind(p, &e->base, 4), ERR_NOT_FOUND);
    expect_signal(p, 4, 1);

    /* Several bindings with the same key all go. */
    event_signal(e, SIG_SIGNALED, 0);
    KT_EQ(port_bind(p, &e->base, 5, SIG_SIGNALED, PORT_BIND_ONCE), OK);
    KT_EQ(port_bind(p, &e->base, 5, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);
    KT_EQ(port_unbind(p, &e->base, 5), OK);
    KT_ASSERT(list_empty(&e->base.observers));
    event_signal(e, 0, SIG_SIGNALED);
    expect_empty(p);

    kobject_unref(&p->base);
    kobject_unref(&e->base);
    stats_equal(&before);
}

KTEST(port_binding_keeps_object_alive)
{
    volatile int destroyed = 0;
    struct port *p = new_port();

    struct pobj *o = pobj_new(&destroyed);
    KT_EQ(port_bind(p, &o->base, 1, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);
    kobject_unref(&o->base);   /* only the binding holds it now */
    KT_EQ(destroyed, 0);
    kobject_signal(&o->base, 0, SIG_SIGNALED);
    expect_signal(p, 1, 1);
    KT_EQ(port_unbind(p, &o->base, 1), OK);
    KT_EQ(destroyed, 1);

    /* A fired ONCE binding holds it until its packet is read. */
    o = pobj_new(&destroyed);
    KT_EQ(port_bind(p, &o->base, 2, SIG_SIGNALED, PORT_BIND_ONCE), OK);
    kobject_unref(&o->base);
    kobject_signal(&o->base, 0, SIG_SIGNALED);
    KT_EQ(destroyed, 1);
    expect_signal(p, 2, 1);
    KT_EQ(destroyed, 2);

    kobject_unref(&p->base);
}

KTEST(port_destroy_frees_everything)
{
    volatile int destroyed = 0;
    struct port_stats before;
    port_get_stats(&before);
    struct port *p = new_port();
    enum { N = 6 };
    struct pobj *os[N];
    for (int i = 0; i < N; i++)
        os[i] = pobj_new(&destroyed);

    KT_EQ(port_bind(p, &os[0]->base, 0, SIG_SIGNALED, PORT_BIND_ONCE), OK);        /* armed */
    /* fired, queued */
    KT_EQ(port_bind(p, &os[1]->base, 1, SIG_SIGNALED, PORT_BIND_ONCE), OK);
    KT_EQ(port_bind(p, &os[2]->base, 2, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);  /* idle */
    KT_EQ(port_bind(p, &os[3]->base, 3, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);  /* queued */
    /* unbound, queued */
    KT_EQ(port_bind(p, &os[4]->base, 4, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);
    KT_EQ(port_bind(p, &os[5]->base, 5, SIG_READABLE, PORT_BIND_PERSISTENT), OK);  /* two ways */
    KT_EQ(port_bind(p, &os[5]->base, 6, SIG_READABLE, PORT_BIND_ONCE), OK);
    kobject_signal(&os[1]->base, 0, SIG_SIGNALED);
    kobject_signal(&os[3]->base, 0, SIG_SIGNALED);
    kobject_signal(&os[4]->base, 0, SIG_SIGNALED);
    KT_EQ(port_unbind(p, &os[4]->base, 4), OK);
    kobject_signal(&os[5]->base, 0, SIG_READABLE);
    for (int i = 0; i < 10; i++) {
        struct port_packet pkt = { .key = 100 + i };
        KT_EQ(port_queue_user(p, &pkt), OK);
    }
    struct port_stats mid;
    port_get_stats(&mid);
    KT_EQ(mid.bindings, before.bindings + 7);
    KT_EQ(mid.user_packets, before.user_packets + 10);

    kobject_unref(&p->base);
    stats_equal(&before);
    for (int i = 0; i < N; i++) {
        KT_ASSERT(list_empty(&os[i]->base.observers));
        KT_EQ(os[i]->base.refs, 1);   /* every binding's reference returned */
        kobject_unref(&os[i]->base);
    }
    KT_EQ(destroyed, N);
}

KTEST(port_to_port_rejected)
{
    struct port *a = new_port(), *b = new_port();
    struct event *e = new_event();
    KT_EQ(port_bind(a, &b->base, 1, SIG_READABLE, PORT_BIND_ONCE), ERR_NOT_SUPPORTED);
    KT_EQ(port_bind(a, &a->base, 1, SIG_READABLE, PORT_BIND_PERSISTENT), ERR_NOT_SUPPORTED);
    KT_EQ(port_bind(a, &e->base, 1, 0, PORT_BIND_ONCE), ERR_INVALID_ARGS);
    KT_EQ(port_bind(a, &e->base, 1, SIG_SIGNALED, 7), ERR_INVALID_ARGS);
    KT_ASSERT(list_empty(&b->base.observers) && list_empty(&e->base.observers));
    kobject_unref(&a->base);
    kobject_unref(&b->base);
    kobject_unref(&e->base);
}

/* A thread in object_wait_one on a port wakes when a packet arrives. */
static volatile status_t owo_status;
static volatile signals_t owo_seen;

static void port_readable_waiter(void *arg)
{
    signals_t seen = 0;
    owo_status = object_wait_one(arg, SIG_READABLE, uptime_ns() + 3000 * NS_PER_MS, &seen);
    owo_seen = seen;
}

KTEST(port_object_wait_one_wakes)
{
    struct port *p = new_port();
    owo_status = ERR_INTERNAL;
    struct thread *t = thread_create("port waiter", port_readable_waiter, &p->base, PRIO_DEFAULT);
    thread_sleep_ms(30);
    KT_EQ(owo_status, ERR_INTERNAL);   /* still waiting */
    struct port_packet pkt = { .key = 9 };
    uint64_t t0 = uptime_ns();
    KT_EQ(port_queue_user(p, &pkt), OK);
    thread_join(t);
    KT_EQ(owo_status, OK);
    KT_ASSERT(owo_seen & SIG_READABLE);
    KT_ASSERT(uptime_ns() - t0 < 500 * NS_PER_MS);
    kobject_unref(&p->base);   /* destroys it with the packet still queued */
}

/* Several threads in port_wait on one port: every packet goes to exactly
 * one of them. */
enum { MW_WAITERS = 6, MW_PACKETS = 600 };
static struct port *mw_port;
static volatile uint8_t mw_seen[MW_PACKETS];
static volatile int mw_failures;

static void mw_waiter(void *arg)
{
    (void)arg;
    for (;;) {
        struct port_packet pkt;
        if (port_wait(mw_port, uptime_ns() + 5000 * NS_PER_MS, &pkt) != OK) {
            __atomic_add_fetch(&mw_failures, 1, __ATOMIC_RELAXED);
            return;
        }
        if (pkt.key == UINT64_MAX)
            return;   /* stop */
        if (pkt.key >= MW_PACKETS)
            __atomic_add_fetch(&mw_failures, 1, __ATOMIC_RELAXED);
        else
            __atomic_add_fetch(&mw_seen[pkt.key], 1, __ATOMIC_RELAXED);
        if (pkt.key % 7 == 0)
            thread_yield();
    }
}

KTEST(port_many_waiters)
{
    mw_port = new_port();
    mw_failures = 0;
    for (int i = 0; i < MW_PACKETS; i++)
        mw_seen[i] = 0;
    struct thread *ts[MW_WAITERS];
    for (int i = 0; i < MW_WAITERS; i++)
        ts[i] = thread_create("port consumer", mw_waiter, NULL, PRIO_DEFAULT);
    for (uint64_t i = 0; i < MW_PACKETS; i++) {
        struct port_packet pkt = { .key = i };
        KT_EQ(port_queue_user(mw_port, &pkt), OK);
        if (i % 50 == 0)
            thread_sleep_ms(1);
    }
    for (int i = 0; i < MW_WAITERS; i++) {
        struct port_packet stop = { .key = UINT64_MAX };
        KT_EQ(port_queue_user(mw_port, &stop), OK);
    }
    for (int i = 0; i < MW_WAITERS; i++)
        thread_join(ts[i]);
    KT_EQ(mw_failures, 0);
    for (int i = 0; i < MW_PACKETS; i++)
        KT_EQ(mw_seen[i], 1);
    expect_empty(mw_port);
    kobject_unref(&mw_port->base);
}

/* ---- handle layer -------------------------------------------------------- */

KTEST(port_sys_rights)
{
    struct port_stats before;
    port_get_stats(&before);
    struct handle_table tbl;
    handle_table_init(&tbl);
    handle_t ev, tm, pt, h;
    KT_EQ(sys_event_create(&tbl, &ev), OK);
    KT_EQ(sys_timer_create(&tbl, &tm), OK);
    KT_EQ(sys_port_create(&tbl, &pt), OK);

    struct kobject *o;
    rights_t r;
    KT_EQ(handle_get(&tbl, ev, OBJ_EVENT, 0, &o, &r), OK);
    KT_EQ(r, RIGHTS_BASIC | RIGHT_SIGNAL);
    kobject_unref(o);
    KT_EQ(handle_get(&tbl, tm, OBJ_TIMER, 0, &o, &r), OK);
    KT_EQ(r, RIGHTS_BASIC | RIGHTS_IO);
    kobject_unref(o);
    KT_EQ(handle_get(&tbl, pt, OBJ_PORT, 0, &o, &r), OK);
    KT_EQ(r, RIGHTS_BASIC | RIGHTS_IO);
    kobject_unref(o);

    /* Wrong types. */
    KT_EQ(sys_event_signal(&tbl, tm, 0, SIG_SIGNALED), ERR_WRONG_TYPE);
    KT_EQ(sys_timer_set(&tbl, ev, 0), ERR_WRONG_TYPE);
    KT_EQ(sys_timer_cancel(&tbl, pt), ERR_WRONG_TYPE);
    KT_EQ(sys_port_bind(&tbl, ev, tm, 1, SIG_SIGNALED, PORT_BIND_ONCE), ERR_WRONG_TYPE);
    KT_EQ(sys_port_wait(&tbl, ev, 0, NULL), ERR_WRONG_TYPE);
    KT_EQ(sys_port_bind(&tbl, pt, pt, 1, SIG_READABLE, PORT_BIND_ONCE), ERR_NOT_SUPPORTED);
    KT_EQ(sys_event_signal(&tbl, 0x7fff00, 0, SIG_SIGNALED), ERR_BAD_HANDLE);

    /* Missing rights. */
    KT_EQ(handle_duplicate(&tbl, ev, RIGHT_WAIT, &h), OK);
    KT_EQ(sys_event_signal(&tbl, h, 0, SIG_SIGNALED), ERR_ACCESS_DENIED);
    KT_EQ(handle_close(&tbl, h), OK);
    KT_EQ(handle_duplicate(&tbl, ev, RIGHT_SIGNAL, &h), OK);
    KT_EQ(sys_object_wait_one(&tbl, h, SIG_SIGNALED, 0, NULL), ERR_ACCESS_DENIED);
    KT_EQ(sys_port_bind(&tbl, pt, h, 1, SIG_SIGNALED, PORT_BIND_ONCE), ERR_ACCESS_DENIED);
    KT_EQ(sys_port_unbind(&tbl, pt, h, 1), ERR_NOT_FOUND);   /* unbind needs no rights */
    KT_EQ(handle_close(&tbl, h), OK);
    KT_EQ(handle_duplicate(&tbl, tm, RIGHT_READ | RIGHT_WAIT, &h), OK);
    KT_EQ(sys_timer_set(&tbl, h, 0), ERR_ACCESS_DENIED);
    KT_EQ(sys_timer_cancel(&tbl, h), ERR_ACCESS_DENIED);
    KT_EQ(handle_close(&tbl, h), OK);
    handle_t pr, pw;
    KT_EQ(handle_duplicate(&tbl, pt, RIGHT_READ | RIGHT_WAIT, &pr), OK);
    KT_EQ(handle_duplicate(&tbl, pt, RIGHT_WRITE, &pw), OK);
    struct port_packet pkt = { .key = 77 };
    KT_EQ(sys_port_queue(&tbl, pr, &pkt), ERR_ACCESS_DENIED);
    KT_EQ(sys_port_bind(&tbl, pr, ev, 1, SIG_SIGNALED, PORT_BIND_ONCE), ERR_ACCESS_DENIED);
    KT_EQ(sys_port_unbind(&tbl, pr, ev, 1), ERR_ACCESS_DENIED);
    KT_EQ(sys_port_wait(&tbl, pw, 0, &pkt), ERR_ACCESS_DENIED);
    KT_EQ(sys_object_wait_one(&tbl, pw, SIG_READABLE, 0, NULL), ERR_ACCESS_DENIED);

    /* The happy path, through handles only. */
    KT_EQ(sys_event_signal(&tbl, ev, 0, SIG_READABLE), ERR_INVALID_ARGS);
    KT_EQ(sys_port_bind(&tbl, pw, ev, 1, SIG_SIGNALED, PORT_BIND_ONCE), OK);
    KT_EQ(sys_port_bind(&tbl, pw, tm, 2, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);
    KT_EQ(sys_event_signal(&tbl, ev, 0, SIG_SIGNALED), OK);
    signals_t seen;
    KT_EQ(sys_object_wait_one(&tbl, pr, SIG_READABLE, uptime_ns() + 1000 * NS_PER_MS, &seen), OK);
    KT_EQ(sys_port_wait(&tbl, pr, DEADLINE_NEVER, &pkt), OK);
    KT_EQ(pkt.key, 1);
    KT_EQ(sys_timer_set(&tbl, tm, uptime_ns() + 10 * NS_PER_MS), OK);
    KT_EQ(sys_port_wait(&tbl, pr, uptime_ns() + 1000 * NS_PER_MS, &pkt), OK);
    KT_EQ(pkt.key, 2);
    KT_EQ(sys_timer_cancel(&tbl, tm), OK);
    pkt.key = 3;
    KT_EQ(sys_port_queue(&tbl, pw, &pkt), OK);
    KT_EQ(sys_port_wait(&tbl, pr, 0, &pkt), OK);
    KT_EQ(pkt.key, 3);
    KT_EQ(pkt.type, PORT_PACKET_USER);
    KT_EQ(sys_port_unbind(&tbl, pw, tm, 2), OK);
    KT_EQ(sys_port_wait(&tbl, pr, 0, &pkt), ERR_TIMED_OUT);

    /* Closing the port's last handle with a live binding cleans up. */
    KT_EQ(sys_port_bind(&tbl, pw, ev, 4, SIG_USER_ALL, PORT_BIND_PERSISTENT), OK);
    handle_table_destroy(&tbl);
    stats_equal(&before);
}

/* ---- stress -------------------------------------------------------------- */

/* Producers on different CPUs each signal their own ONCE-bound event and
 * wait for the consumer to acknowledge it (the consumer clears the event,
 * re-binds it, then acks), and toggle a PERSISTENT-bound event in between.
 * Every ONCE signal must produce exactly one packet; the PERSISTENT counts
 * must add up to exactly the number of rising edges. */
enum { SP_PRODUCERS = 8, SP_ROUNDS = 2000, SP_TOGGLES = 3, SP_PERS_KEY = 100 };

struct sp_producer {
    /* ONCE-bound, PERSISTENT-bound, and the consumer's ack */
    struct event     *once, *pers, *ack;
    uint64_t          sent;                /* rising edges made */
    volatile bool     failed;              /* a check failed */
};

static struct sp_producer sp[SP_PRODUCERS];

static void sp_main(void *arg)
{
    struct sp_producer *pr = arg;
    for (uint64_t r = 0; r < SP_ROUNDS; r++) {
        __atomic_store_n(&pr->sent, r + 1, __ATOMIC_RELEASE);
        event_signal(pr->once, 0, SIG_SIGNALED);
        for (int k = 0; k < SP_TOGGLES; k++) {
            event_signal(pr->pers, 0, SIG_SIGNALED);
            event_signal(pr->pers, SIG_SIGNALED, 0);
        }
        if (object_wait_one(&pr->ack->base, SIG_SIGNALED, uptime_ns() + 5000 * NS_PER_MS,
                            NULL) != OK) {
            pr->failed = true;
            return;
        }
        event_signal(pr->ack, SIG_SIGNALED, 0);
    }
}

KTEST(port_stress_producers)
{
    struct port_stats before;
    port_get_stats(&before);
    struct port *p = new_port();
    uint64_t recv[SP_PRODUCERS] = { 0 }, edges[SP_PRODUCERS] = { 0 };
    struct thread *ts[SP_PRODUCERS];
    for (int i = 0; i < SP_PRODUCERS; i++) {
        sp[i] = (struct sp_producer){ new_event(), new_event(), new_event(), 0, false };
        KT_EQ(port_bind(p, &sp[i].once->base, i, SIG_SIGNALED, PORT_BIND_ONCE), OK);
        KT_EQ(port_bind(p, &sp[i].pers->base, SP_PERS_KEY + i, SIG_SIGNALED,
                        PORT_BIND_PERSISTENT), OK);
    }
    for (int i = 0; i < SP_PRODUCERS; i++) {
        cpumask_t m;
        cpumask_one(&m, (uint32_t)i % cpu_count);
        ts[i] = thread_create_on("port producer", sp_main, &sp[i], PRIO_DEFAULT, &m);
    }

    uint64_t once_total = 0, edge_total = 0, packets = 0, coalesced = 0;
    const uint64_t want_once = SP_PRODUCERS * SP_ROUNDS;
    const uint64_t want_edges = want_once * SP_TOGGLES;
    while (once_total < want_once || edge_total < want_edges) {
        struct port_packet pkt;
        status_t st = port_wait(p, uptime_ns() + 5000 * NS_PER_MS, &pkt);
        if (st != OK)
            panic("ktest %s: lost packets (once %lu/%lu, edges %lu/%lu)", ktest_current,
                  once_total, want_once, edge_total, want_edges);
        packets++;
        KT_EQ(pkt.type, PORT_PACKET_SIGNAL);
        if (pkt.key < SP_PRODUCERS) {
            struct sp_producer *pr = &sp[pkt.key];
            KT_EQ(pkt.signal.count, 1);
            /* Exactly one outstanding signal per producer: a duplicate
             * would arrive with recv already equal to sent. */
            KT_EQ(recv[pkt.key] + 1, __atomic_load_n(&pr->sent, __ATOMIC_ACQUIRE));
            recv[pkt.key]++;
            once_total++;
            event_signal(pr->once, SIG_SIGNALED, 0);
            KT_EQ(port_bind(p, &pr->once->base, pkt.key, SIG_SIGNALED, PORT_BIND_ONCE), OK);
            event_signal(pr->ack, 0, SIG_SIGNALED);
        } else {
            uint64_t i = pkt.key - SP_PERS_KEY;
            KT_ASSERT(i < SP_PRODUCERS);
            KT_ASSERT(pkt.signal.count >= 1);
            coalesced += pkt.signal.count - 1;
            edges[i] += pkt.signal.count;
            edge_total += pkt.signal.count;
            KT_ASSERT(edges[i] <= __atomic_load_n(&sp[i].sent, __ATOMIC_ACQUIRE) * SP_TOGGLES);
        }
    }
    for (int i = 0; i < SP_PRODUCERS; i++) {
        thread_join(ts[i]);
        KT_ASSERT(!sp[i].failed);
        KT_EQ(recv[i], SP_ROUNDS);
        KT_EQ(edges[i], SP_ROUNDS * SP_TOGGLES);
    }
    expect_empty(p);
    kprintf("ktest: port_stress_producers: %lu packets, %lu edges coalesced, %u CPUs\n",
            packets, coalesced, cpu_count);

    /* The last re-armed ONCE bindings are still live: destroy cleans them. */
    kobject_unref(&p->base);
    for (int i = 0; i < SP_PRODUCERS; i++) {
        KT_ASSERT(list_empty(&sp[i].once->base.observers));
        KT_ASSERT(list_empty(&sp[i].pers->base.observers));
        kobject_unref(&sp[i].once->base);
        kobject_unref(&sp[i].pers->base);
        kobject_unref(&sp[i].ack->base);
    }
    stats_equal(&before);
}

/* Bind, unbind and destroy ports while other CPUs keep firing the watched
 * events and a consumer keeps dequeuing: the teardown races (unbind vs
 * fire vs dequeue of a spent ONCE packet, destroy vs fire) must neither
 * crash nor leak. */
enum { CH_MAX_TOGGLERS = 7, CH_ROUNDS = 400 };
#define CH_STOP UINT64_MAX
static volatile bool ch_stop;
static volatile uint64_t ch_toggles;

static void ch_toggler(void *arg)
{
    struct event *e = arg;
    uint64_t n = 0;
    while (!__atomic_load_n(&ch_stop, __ATOMIC_ACQUIRE)) {
        event_signal(e, 0, SIG_SIGNALED);
        event_signal(e, SIG_SIGNALED, 0);
        if (++n % 64 == 0)
            thread_yield();
    }
    __atomic_add_fetch(&ch_toggles, n, __ATOMIC_RELAXED);
}

static void ch_consumer(void *arg)
{
    struct port *p = arg;
    struct port_packet pkt;
    while (port_wait(p, uptime_ns() + 5000 * NS_PER_MS, &pkt) == OK && pkt.key != CH_STOP)
        ;
    kobject_unref(&p->base);   /* often the last reference: destroys a busy port */
}

/* One pass of binding every event (ONCE or PERSISTENT by pass j), each
 * followed by a random unbind now and then; returns the unbinds done. */
static uint64_t churn_bindings(struct port *p, struct event **evs, uint32_t n, int j,
                               uint64_t *seed)
{
    uint64_t unbound = 0;
    for (uint32_t i = 0; i < n; i++) {
        KT_EQ(port_bind(p, &evs[i]->base, i, SIG_SIGNALED,
                        (j + i) % 2 ? PORT_BIND_ONCE : PORT_BIND_PERSISTENT), OK);
        *seed ^= *seed << 13;
        *seed ^= *seed >> 7;
        *seed ^= *seed << 17;
        if (*seed % 3 == 0 && port_unbind(p, &evs[*seed % n]->base, *seed % n) == OK)
            unbound++;
    }
    return unbound;
}

KTEST(port_stress_churn)
{
    struct port_stats before;
    port_get_stats(&before);
    uint32_t n = cpu_count > 1 ? cpu_count - 1 : 1;
    if (n > CH_MAX_TOGGLERS)
        n = CH_MAX_TOGGLERS;
    struct event *evs[CH_MAX_TOGGLERS];
    struct thread *ts[CH_MAX_TOGGLERS];
    ch_stop = false;
    ch_toggles = 0;
    for (uint32_t i = 0; i < n; i++) {
        evs[i] = new_event();
        cpumask_t m;
        cpumask_one(&m, (i + 1) % cpu_count);
        ts[i] = thread_create_on("port toggler", ch_toggler, evs[i], PRIO_DEFAULT, &m);
    }
    uint64_t seed = 0x9e3779b97f4a7c15ull, unbound = 0;
    uint64_t t_end = uptime_ns() + 2000 * NS_PER_MS;   /* capped: slow with many CPUs under TCG */
    int rounds = 0;
    for (; rounds < CH_ROUNDS && uptime_ns() < t_end; rounds++) {
        struct port *p = new_port();
        kobject_ref(&p->base);
        struct thread *c = thread_create("port churn consumer", ch_consumer, p, PRIO_DEFAULT);
        for (int j = 0; j < 8; j++)
            unbound += churn_bindings(p, evs, n, j, &seed);
        struct port_packet stop = { .key = CH_STOP };
        KT_EQ(port_queue_user(p, &stop), OK);
        kobject_unref(&p->base);   /* the consumer may still be draining */
        thread_join(c);
    }
    __atomic_store_n(&ch_stop, true, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < n; i++) {
        thread_join(ts[i]);
        KT_ASSERT(list_empty(&evs[i]->base.observers));
        KT_EQ(evs[i]->base.refs, 1);
        kobject_unref(&evs[i]->base);
    }
    kprintf("ktest: port_stress_churn: %u togglers, %lu toggles, %lu unbinds, %d ports\n", n,
            ch_toggles, unbound, rounds);
    stats_equal(&before);
}

/* ---- the binding cap ------------------------------------------------------------ */

KTEST(m45_port_binding_cap)
{
    struct port_stats before, now;
    port_get_stats(&before);
    struct handle_table t;
    handle_table_init(&t);
    handle_t port, ev;
    KT_EQ(sys_port_create(&t, &port), OK);
    KT_EQ(sys_event_create(&t, &ev), OK);
    for (uint64_t k = 0; k < PORT_MAX_BINDINGS; k++)
        KT_EQ(sys_port_bind(&t, port, ev, k, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);
    KT_EQ(sys_port_bind(&t, port, ev, PORT_MAX_BINDINGS, SIG_SIGNALED, PORT_BIND_PERSISTENT),
          ERR_NO_RESOURCES);
    port_get_stats(&now);
    KT_EQ(now.bindings - before.bindings, PORT_MAX_BINDINGS);

    /* Freeing one makes room for one. */
    KT_EQ(sys_port_unbind(&t, port, ev, 0), OK);
    KT_EQ(sys_port_bind(&t, port, ev, PORT_MAX_BINDINGS, SIG_SIGNALED, PORT_BIND_PERSISTENT),
          OK);

    handle_table_destroy(&t);
    port_get_stats(&now);
    KT_EQ(now.bindings, before.bindings);
    KT_EQ(now.ports, before.ports);
}
