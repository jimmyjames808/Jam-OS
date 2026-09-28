/* M4.5 regression tests: receiving handles into a full table, the channel
 * "has room" signal, user signals, the port binding cap and channel_call
 * cancellation. All through the handle-level sys_ API, as M5 programs will
 * use it. */
#include <jam/channel.h>
#include <jam/handle.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/port.h>
#include <jam/sched.h>
#include <jam/sys.h>
#include <jam/time.h>

#define SECOND 1000000000ull

static signals_t signals_of(struct handle_table *t, handle_t h)
{
    struct kobject *obj;
    KT_EQ(handle_get(t, h, OBJ_NONE, 0, &obj, NULL), OK);
    signals_t s = kobject_signals(obj);
    kobject_unref(obj);
    return s;
}

/* Duplicate `h` until the table is full; returns how many were made. */
static uint32_t fill_table(struct handle_table *t, handle_t h)
{
    uint32_t n = 0;
    handle_t d;
    status_t st;
    while ((st = handle_duplicate(t, h, RIGHT_SAME, &d)) == OK)
        n++;
    KT_EQ(st, ERR_NO_RESOURCES);
    KT_EQ(t->used, HANDLE_TABLE_MAX);
    return n;
}

/* A read that can't place its handles fails and leaves the message queued;
 * after one slot frees up the same message reads fine. */
KTEST(m45_read_into_full_table)
{
    uint64_t live = channel_live_count();
    struct handle_table t;
    handle_table_init(&t);
    handle_t a, b, ev, ev2, filler;
    KT_EQ(sys_channel_create(&t, &a, &b), OK);
    KT_EQ(sys_event_create(&t, &ev), OK);
    KT_EQ(handle_duplicate(&t, ev, RIGHT_SAME, &ev2), OK);
    uint32_t msg = 7;
    KT_EQ(sys_channel_write(&t, a, &msg, 4, &ev2, 1), OK);   /* ev2 moves into b's queue */

    KT_EQ(sys_event_create(&t, &filler), OK);
    fill_table(&t, filler);

    uint32_t got = 0, nb = 0, nh = 0;
    handle_t rh[4];
    KT_EQ(sys_channel_read(&t, b, &got, 4, &nb, rh, 4, &nh), ERR_NO_RESOURCES);
    KT_EQ(nh, 1);
    KT_ASSERT(signals_of(&t, b) & SIG_READABLE);   /* still queued */

    KT_EQ(handle_close(&t, filler), OK);            /* one free slot */
    KT_EQ(sys_channel_read(&t, b, &got, 4, &nb, rh, 4, &nh), OK);
    KT_EQ(got, 7);
    KT_EQ(nh, 1);
    KT_EQ(sys_event_signal(&t, rh[0], 0, SIG_SIGNALED), OK);   /* it is the event */
    KT_ASSERT(signals_of(&t, ev) & SIG_SIGNALED);

    /* A buffer too small for the handles is still BUFFER_TOO_SMALL, not a
     * reservation: nothing is taken from the table. */
    handle_t ev3;
    KT_EQ(handle_close(&t, rh[0]), OK);
    KT_EQ(handle_duplicate(&t, ev, RIGHT_SAME, &ev3), OK);
    KT_EQ(sys_channel_write(&t, a, &msg, 4, &ev3, 1), OK);
    uint32_t used = t.used;
    KT_EQ(sys_channel_read(&t, b, &got, 4, &nb, rh, 0, &nh), ERR_BUFFER_TOO_SMALL);
    KT_EQ(t.used, used);

    handle_table_destroy(&t);
    KT_EQ(channel_live_count(), live);
}

/* channel_call reserves slots for the reply before sending, so a full table
 * fails the call up front and the server never sees the request. */
KTEST(m45_call_with_full_table_fails_before_send)
{
    uint64_t live = channel_live_count();
    struct handle_table t;
    handle_table_init(&t);
    handle_t a, b, filler;
    KT_EQ(sys_channel_create(&t, &a, &b), OK);
    KT_EQ(sys_event_create(&t, &filler), OK);
    fill_table(&t, filler);

    uint32_t req[2] = { 0, 1 }, rep[2];
    uint32_t rn = 0, rhn = 0;
    handle_t rh[2];
    KT_EQ(sys_channel_call(&t, a, req, 8, NULL, 0, rep, 8, &rn, rh, 2, &rhn,
                           uptime_ns() + SECOND), ERR_NO_RESOURCES);
    uint32_t nb = 0, nh = 0;
    KT_EQ(sys_channel_read(&t, b, rep, 8, &nb, NULL, 0, &nh), ERR_SHOULD_WAIT);
    KT_EQ(t.used, HANDLE_TABLE_MAX);   /* the failed reservation gave its slots back */

    handle_table_destroy(&t);
    KT_EQ(channel_live_count(), live);
}

struct writable_waiter {
    struct handle_table *t;
    handle_t             h;
    status_t             st;
    signals_t            seen;
};

static void wait_writable(void *arg)
{
    struct writable_waiter *w = arg;
    w->st = sys_object_wait_one(w->t, w->h, SIG_WRITABLE, uptime_ns() + 5 * SECOND, &w->seen);
}

/* SIG_WRITABLE drops when the peer's queue is full and comes back when a
 * read makes room, waking a writer that waits for it. */
KTEST(m45_writable_tracks_queue_room)
{
    uint64_t live = channel_live_count();
    struct handle_table t;
    handle_table_init(&t);
    handle_t a, b;
    KT_EQ(sys_channel_create(&t, &a, &b), OK);
    KT_ASSERT(signals_of(&t, a) & SIG_WRITABLE);

    uint32_t v = 0;
    for (uint32_t i = 0; i < CHANNEL_MAX_QUEUED; i++)
        KT_EQ(sys_channel_write(&t, a, &v, 4, NULL, 0), OK);
    KT_EQ(signals_of(&t, a) & SIG_WRITABLE, 0);
    KT_EQ(sys_channel_write(&t, a, &v, 4, NULL, 0), ERR_SHOULD_WAIT);
    KT_ASSERT(signals_of(&t, b) & SIG_WRITABLE);   /* the other direction is empty */

    struct writable_waiter w = { &t, a, ERR_INTERNAL, 0 };
    struct thread *th = thread_create("writable-wait", wait_writable, &w, PRIO_DEFAULT);
    thread_sleep_ms(20);
    KT_EQ(w.st, ERR_INTERNAL);   /* still waiting */
    uint32_t nb = 0, nh = 0;
    KT_EQ(sys_channel_read(&t, b, &v, 4, &nb, NULL, 0, &nh), OK);
    thread_join(th);
    KT_EQ(w.st, OK);
    KT_ASSERT(w.seen & SIG_WRITABLE);
    KT_EQ(sys_channel_write(&t, a, &v, 4, NULL, 0), OK);   /* full again */
    KT_EQ(signals_of(&t, a) & SIG_WRITABLE, 0);

    /* Peer gone: not writable, PEER_CLOSED, and it stays that way. */
    KT_EQ(handle_close(&t, b), OK);
    KT_EQ(signals_of(&t, a) & (SIG_WRITABLE | SIG_PEER_CLOSED), SIG_PEER_CLOSED);

    handle_table_destroy(&t);
    KT_EQ(channel_live_count(), live);
}

KTEST(m45_object_signal_user_bits)
{
    struct handle_table t;
    handle_table_init(&t);
    handle_t ev, a, b, weak;
    KT_EQ(sys_event_create(&t, &ev), OK);
    KT_EQ(sys_channel_create(&t, &a, &b), OK);

    KT_EQ(sys_object_signal(&t, ev, 0, 1u << 24), OK);
    KT_EQ(signals_of(&t, ev) & SIG_USER_ALL, 1u << 24);
    KT_EQ(sys_object_signal(&t, a, 0, 0x81000000u), OK);
    KT_EQ(signals_of(&t, a) & SIG_USER_ALL, 0x81000000u);
    KT_EQ(sys_object_signal(&t, a, 1u << 24, 0), OK);
    KT_EQ(signals_of(&t, a) & SIG_USER_ALL, 0x80000000u);
    KT_ASSERT(signals_of(&t, a) & SIG_WRITABLE);   /* kernel bits untouched */

    /* Kernel-owned bits are off limits, and the right is required. */
    KT_EQ(sys_object_signal(&t, a, 0, SIG_READABLE), ERR_INVALID_ARGS);
    KT_EQ(sys_object_signal(&t, a, SIG_WRITABLE, 0), ERR_INVALID_ARGS);
    KT_EQ(handle_duplicate(&t, ev, RIGHTS_BASIC, &weak), OK);
    KT_EQ(sys_object_signal(&t, weak, 0, 1u << 25), ERR_ACCESS_DENIED);

    /* A user bit wakes a waiter like any signal. */
    signals_t seen = 0;
    KT_EQ(sys_object_wait_one(&t, ev, 1u << 24, uptime_ns() + SECOND, &seen), OK);
    KT_ASSERT(seen & (1u << 24));

    handle_table_destroy(&t);
}

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

struct caller {
    struct handle_table *t;
    handle_t             h;
    status_t             st;
};

static void call_forever(void *arg)
{
    struct caller *c = arg;
    uint32_t req[2] = { 0, 42 }, rep[2];
    uint32_t rn = 0, rhn = 0;
    c->st = sys_channel_call(c->t, c->h, req, 8, NULL, 0, rep, 8, &rn, NULL, 0, &rhn,
                             uptime_ns() + 10 * SECOND);
}

/* Closing your own endpoint while a call on it is waiting cancels the call. */
KTEST(m45_call_canceled_by_own_close)
{
    uint64_t live = channel_live_count();
    struct handle_table t;
    handle_table_init(&t);
    handle_t a, b;
    KT_EQ(sys_channel_create(&t, &a, &b), OK);
    struct caller c = { &t, a, ERR_INTERNAL };
    struct thread *th = thread_create("caller", call_forever, &c, PRIO_DEFAULT);
    signals_t seen = 0;
    KT_EQ(sys_object_wait_one(&t, b, SIG_READABLE, uptime_ns() + SECOND, &seen), OK);
    thread_sleep_ms(10);                 /* let it block */
    uint64_t t0 = uptime_ns();
    KT_EQ(handle_close(&t, a), OK);      /* our last handle: the endpoint closes */
    thread_join(th);
    KT_EQ(c.st, ERR_CANCELED);
    KT_ASSERT(uptime_ns() - t0 < SECOND);   /* promptly, not at the deadline */
    handle_table_destroy(&t);
    KT_EQ(channel_live_count(), live);
}
