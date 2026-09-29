/* Cancellable waits: thread_cancel makes every cancellable wait
 * return ERR_CANCELED promptly, plain waits only see a spurious wakeup, and
 * a cancel racing a real wakeup never hangs or loses the wakeup. */
#include <jam/channel.h>
#include <jam/event.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/report.h>
#include <jam/ktest.h>
#include <jam/port.h>
#include <jam/sched.h>
#include <jam/sys.h>
#include <jam/time.h>


struct waiter {
    struct handle_table *t;         /* the waiter's handle table */
    handle_t             h;         /* the handle it waits on */
    volatile status_t    st;        /* what its wait returned */
    volatile bool        started;   /* it is about to wait */
};

static void wait_event(void *arg)
{
    struct waiter *w = arg;
    w->started = true;
    w->st = sys_object_wait_one(w->t, w->h, SIG_SIGNALED, DEADLINE_NEVER, NULL);
}

static void wait_port(void *arg)
{
    struct waiter *w = arg;
    struct port_packet pkt;
    w->started = true;
    w->st = sys_port_wait(w->t, w->h, DEADLINE_NEVER, &pkt);
}

static void call_silent_server(void *arg)
{
    struct waiter *w = arg;
    uint32_t req[2] = { 0, 1 }, rep[2], rn = 0, rhn = 0;
    w->started = true;
    w->st = sys_channel_call(w->t, w->h, req, 8, NULL, 0, rep, 8, &rn, NULL, 0, &rhn,
                             DEADLINE_NEVER);
}

static void sleep_long(void *arg)
{
    struct waiter *w = arg;
    w->started = true;
    w->st = thread_sleep_cancellable(60 * NS_PER_S);
}

/* Start fn, let it block, cancel it, and require ERR_CANCELED quickly. */
static void cancel_one(void (*fn)(void *), struct waiter *w)
{
    w->st = ERR_INTERNAL;
    w->started = false;
    struct thread *th = thread_create("cancel-me", fn, w, PRIO_DEFAULT);
    while (!w->started)
        thread_sleep_ms(1);
    thread_sleep_ms(10);
    KT_EQ(w->st, ERR_INTERNAL);   /* really blocked */
    uint64_t t0 = uptime_ns();
    thread_cancel(th);
    thread_join(th);
    KT_EQ(w->st, ERR_CANCELED);
    KT_ASSERT(uptime_ns() - t0 < NS_PER_S / 10);
}

KTEST(cancel_every_wait_kind)
{
    uint64_t live = channel_live_count();
    struct handle_table t;
    handle_table_init(&t);
    handle_t ev, port, a, b;
    KT_EQ(sys_event_create(&t, &ev), OK);
    KT_EQ(sys_port_create(&t, &port), OK);
    KT_EQ(sys_channel_create(&t, &a, &b), OK);

    struct waiter w = { .t = &t };
    w.h = ev;
    cancel_one(wait_event, &w);
    w.h = port;
    cancel_one(wait_port, &w);
    w.h = a;
    cancel_one(call_silent_server, &w);
    cancel_one(sleep_long, &w);

    /* The cancelled call's request is still queued at the server; a late
     * reply to it lands in a's queue like any other message. */
    uint32_t req[2], nb = 0, nh = 0;
    KT_EQ(sys_channel_read(&t, b, req, 8, &nb, NULL, 0, &nh), OK);
    KT_EQ(sys_channel_write(&t, b, req, 8, NULL, 0), OK);
    KT_EQ(sys_channel_read(&t, a, req, 8, &nb, NULL, 0, &nh), OK);

    /* The event's observer list is clean: signalling it now touches no
     * dead waiter (the waiter's frame is long gone). */
    KT_EQ(sys_event_signal(&t, ev, 0, SIG_SIGNALED), OK);
    handle_table_destroy(&t);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

static struct mutex cancel_mutex;

static void lock_cancellable(void *arg)
{
    struct waiter *w = arg;
    w->started = true;
    w->st = mutex_lock_cancellable(&cancel_mutex);
    if (w->st == OK)
        mutex_unlock(&cancel_mutex);
}

static void lock_plain(void *arg)
{
    struct waiter *w = arg;
    w->started = true;
    mutex_lock(&cancel_mutex);   /* not cancellable: must still get it */
    w->st = OK;
    mutex_unlock(&cancel_mutex);
}

/* A cancelled mutex waiter gives up without the mutex; a cancelled plain
 * waiter keeps waiting (no busy loop) and gets it on release; and a waiter
 * queued behind a cancelled one is not left asleep. */
KTEST(cancel_mutex_waiters)
{
    mutex_init(&cancel_mutex, "cancel test mutex");
    mutex_lock(&cancel_mutex);

    struct waiter wc = { 0 }, wp = { 0 }, wn = { 0 };
    cancel_one(lock_cancellable, &wc);

    wp.st = ERR_INTERNAL;
    struct thread *tp = thread_create("plain-locker", lock_plain, &wp, PRIO_DEFAULT);
    wn.st = ERR_INTERNAL;
    struct thread *tn = thread_create("next-locker", lock_cancellable, &wn, PRIO_DEFAULT);
    thread_sleep_ms(20);
    thread_cancel(tp);             /* spurious wakeup only */
    thread_sleep_ms(20);
    KT_EQ(wp.st, ERR_INTERNAL);    /* still waiting */
    mutex_unlock(&cancel_mutex);
    thread_join(tp);
    thread_join(tn);
    KT_EQ(wp.st, OK);
    KT_EQ(wn.st, OK);
}

/* Cancelling before waiting: an unmet wait returns at once, a met one
 * still succeeds. */
static void pre_cancelled(void *arg)
{
    struct waiter *w = arg;
    while (!thread_cancel_pending())
        thread_sleep_ms(1);
    signals_t seen;
    KT_EQ(sys_object_wait_one(w->t, w->h, SIG_USER_ALL, DEADLINE_NEVER, &seen), ERR_CANCELED);
    w->st = sys_object_wait_one(w->t, w->h, SIG_SIGNALED, DEADLINE_NEVER, &seen);
}

KTEST(cancel_before_wait)
{
    struct handle_table t;
    handle_table_init(&t);
    handle_t ev;
    KT_EQ(sys_event_create(&t, &ev), OK);
    KT_EQ(sys_event_signal(&t, ev, 0, SIG_SIGNALED), OK);
    struct waiter w = { .t = &t, .h = ev, .st = ERR_INTERNAL };
    struct thread *th = thread_create("pre-cancelled", pre_cancelled, &w, PRIO_DEFAULT);
    thread_cancel(th);
    thread_join(th);
    KT_EQ(w.st, OK);
    handle_table_destroy(&t);
}

/* Race a real wakeup against a cancel, on different CPUs, many times, in
 * both orders: the waiter must always finish with OK or ERR_CANCELED (never
 * hang), and a packet is never lost: a cancelled waiter that finds one
 * queued still takes it, and one that gives up leaves it for us. */
KTEST(cancel_races_wakeup)
{
    struct handle_table t;
    handle_table_init(&t);
    handle_t port;
    KT_EQ(sys_port_create(&t, &port), OK);
    uint32_t ok = 0, canceled = 0;
    for (uint32_t i = 0; i < 2000; i++) {
        struct waiter w = { .t = &t, .h = port, .st = ERR_INTERNAL };
        cpumask_t m;
        cpumask_one(&m, cpu_count > 1 ? 1 + i % (cpu_count - 1) : 0);
        struct thread *th = thread_create_on("racer", wait_port, &w, PRIO_DEFAULT, &m);
        while (!w.started)
            thread_yield();
        for (uint32_t spin = i % 64; spin; spin--)
            __asm__ volatile("pause");
        struct port_packet pkt = { .key = i };
        if (i & 1) {
            KT_EQ(sys_port_queue(&t, port, &pkt), OK);
            thread_cancel(th);
        } else {
            thread_cancel(th);
            KT_EQ(sys_port_queue(&t, port, &pkt), OK);
        }
        thread_join(th);
        if (w.st == OK) {
            ok++;
        } else {
            KT_EQ(w.st, ERR_CANCELED);
            canceled++;
            /* Not taken by the cancelled waiter: still there for us. */
            KT_EQ(sys_port_wait(&t, port, 0, &pkt), OK);
            KT_EQ(pkt.key, i);
        }
    }
    report("cancel: 2000 races, %u woken, %u cancelled", ok, canceled);
    handle_table_destroy(&t);
}
