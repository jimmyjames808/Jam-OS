/* CHANNEL_BYTE_BOUND: an end whose queue is full at CHANNEL_BOUND_BYTES
 * of message bytes as well as at CHANNEL_MAX_QUEUED messages (a pipe's
 * read end). Without it a queue of 64 KiB messages grows to 64 MiB, the
 * root job's whole message budget, so a writer far ahead of its reader
 * gets ERR_NO_MEMORY (and so does every other writer in the system)
 * instead of ERR_SHOULD_WAIT and a wait for SIG_WRITABLE. */
#include <jam/channel.h>
#include <jam/handle.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/sched.h>
#include <jam/sys.h>
#include <jam/time.h>

#define BIG      CHANNEL_MAX_BYTES                     /* each message */
#define BIG_FULL (CHANNEL_BOUND_BYTES / CHANNEL_MAX_BYTES)   /* such messages a bounded end takes */

/* Write BIG-byte messages on a until it is full: how many went. */
static uint32_t fill(struct handle_table *t, handle_t a, const void *buf)
{
    uint32_t n = 0;
    while (n <= CHANNEL_MAX_QUEUED && sys_channel_write(t, a, buf, BIG, NULL, 0) == OK)
        n++;
    return n;
}

static status_t read_one(struct handle_table *t, handle_t b, void *buf)
{
    uint32_t nb = 0, nh = 0;
    return sys_channel_read(t, b, buf, BIG, &nb, NULL, 0, &nh);
}

struct bound_waiter {
    struct handle_table *t;
    handle_t             h;      /* the writer's end */
    status_t             st;     /* what its wait returned */
    signals_t            seen;
};

static void wait_room(void *arg)
{
    struct bound_waiter *w = arg;
    w->st = sys_object_wait_one(w->t, w->h, SIG_WRITABLE,
                                uptime_ns() + kt_patience_ms(5000) * NS_PER_MS, &w->seen);
}

/* A bounded end is full at CHANNEL_BOUND_BYTES: the write after is
 * ERR_SHOULD_WAIT and the writer not writable; a read makes room and
 * wakes a writer waiting for it; the other direction keeps the plain
 * limit; withdraw empties the count with the queue. */
KTEST(chan_bound_full_at_bytes)
{
    uint64_t live = channel_live_count();
    uint8_t *buf = kzalloc(BIG);
    KT_ASSERT(buf != NULL);
    struct handle_table t;
    handle_table_init(&t);
    handle_t a, b;
    KT_EQ(sys_channel_create(&t, &a, &b), OK);
    KT_EQ(sys_channel_set_flags(&t, b, 1u << 6), ERR_INVALID_ARGS);
    KT_EQ(sys_channel_set_flags(&t, b, CHANNEL_BYTE_BOUND), OK);

    KT_EQ(fill(&t, a, buf), BIG_FULL);
    KT_EQ(kt_signals_of(&t, a) & SIG_WRITABLE, 0);
    KT_EQ(sys_channel_write(&t, a, buf, 4, NULL, 0), ERR_SHOULD_WAIT);   /* small ones too */

    struct bound_waiter w = { &t, a, ERR_INTERNAL, 0 };
    struct thread *th = thread_create("bound-wait", wait_room, &w, PRIO_DEFAULT);
    thread_sleep_ms(20);
    KT_EQ(w.st, ERR_INTERNAL);   /* still waiting */
    KT_EQ(read_one(&t, b, buf), OK);
    thread_join(th);
    KT_EQ(w.st, OK);
    KT_ASSERT(w.seen & SIG_WRITABLE);
    KT_EQ(fill(&t, a, buf), 1);   /* one message's room */

    /* b's own writes to a: a is not bounded, BIG_FULL + 1 go. */
    for (uint32_t i = 0; i <= BIG_FULL; i++)
        KT_EQ(sys_channel_write(&t, b, buf, BIG, NULL, 0), OK);

    KT_EQ(sys_channel_withdraw(&t, a), (int64_t)BIG_FULL);
    KT_ASSERT(kt_signals_of(&t, a) & SIG_WRITABLE);
    KT_EQ(fill(&t, a, buf), BIG_FULL);   /* the bytes went with the queue */

    handle_table_destroy(&t);
    kfree(buf);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* Small messages still stop at CHANNEL_MAX_QUEUED on a bounded end; a
 * bound set on an end already past it stops the writer at once; and only
 * the end's sole holder may set it. */
KTEST(chan_bound_rules)
{
    uint64_t live = channel_live_count();
    uint8_t *buf = kzalloc(BIG);
    KT_ASSERT(buf != NULL);
    struct handle_table t;
    handle_table_init(&t);
    handle_t a, b, b2;
    KT_EQ(sys_channel_create(&t, &a, &b), OK);
    KT_EQ(sys_channel_set_flags(&t, b, CHANNEL_BYTE_BOUND), OK);
    uint32_t n = 0;
    while (n <= CHANNEL_MAX_QUEUED && sys_channel_write(&t, a, buf, 1, NULL, 0) == OK)
        n++;
    KT_EQ(n, CHANNEL_MAX_QUEUED);
    KT_EQ(handle_close(&t, a), OK);
    KT_EQ(handle_close(&t, b), OK);

    KT_EQ(sys_channel_create(&t, &a, &b), OK);
    for (uint32_t i = 0; i < BIG_FULL + 4; i++)
        KT_EQ(sys_channel_write(&t, a, buf, BIG, NULL, 0), OK);
    KT_ASSERT(kt_signals_of(&t, a) & SIG_WRITABLE);
    KT_EQ(handle_duplicate(&t, b, RIGHT_SAME, &b2), OK);
    KT_EQ(sys_channel_set_flags(&t, b, CHANNEL_BYTE_BOUND), ERR_BAD_STATE);   /* shared */
    KT_ASSERT(kt_signals_of(&t, a) & SIG_WRITABLE);
    KT_EQ(handle_close(&t, b2), OK);
    KT_EQ(sys_channel_set_flags(&t, b, CHANNEL_BYTE_BOUND | CHANNEL_NO_HANDLES), OK);
    KT_EQ(kt_signals_of(&t, a) & SIG_WRITABLE, 0);   /* past the bound already */
    KT_EQ(sys_channel_write(&t, a, buf, 4, NULL, 0), ERR_SHOULD_WAIT);
    for (uint32_t i = 0; i < 4; i++) {
        KT_EQ(read_one(&t, b, buf), OK);
        KT_EQ(kt_signals_of(&t, a) & SIG_WRITABLE, 0);   /* still at the bound */
    }
    KT_EQ(read_one(&t, b, buf), OK);
    KT_ASSERT(kt_signals_of(&t, a) & SIG_WRITABLE);

    handle_table_destroy(&t);
    kfree(buf);
    KT_GLOBAL_EQ(channel_live_count(), live);
}
