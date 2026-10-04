/* channel_read_wait (kernel/object/channel.c): a reader that waits on its
 * endpoint is handed the next message straight from its writer, with no
 * queue, no failed read and no signal in between; the queue still comes
 * first and keeps its order; a message that doesn't fit is queued and
 * stays queued (ERR_BUFFER_TOO_SMALL); and the wait ends as other waits
 * do (the peer closing, a deadline, a cancel). The system call on top of
 * it is utest's replywait.c; a reader killed inside it is test_chanrw.c. */
#include <jam/channel.h>
#include <jam/event.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/object.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>

/* A reader on its own thread: one channel_read_wait into buf. */
struct reader {
    struct channel *ch;
    uint8_t         buf[64];
    struct khandle  h[2];
    uint32_t        hcap;
    uint64_t        deadline;
    struct chan_read r;
    status_t        st;
    bool            done;   /* atomic */
};

static void reader_main(void *arg)
{
    struct reader *rd = arg;
    rd->r = (struct chan_read){
        .buf = chan_kbytes(rd->buf, sizeof(rd->buf)), .h = rd->h, .hcap = rd->hcap,
    };
    rd->st = channel_read_wait(rd->ch, &rd->r, rd->deadline);
    __atomic_store_n(&rd->done, true, __ATOMIC_RELEASE);
}

/* Start rd on ch and wait until it is listed there (it blocks). */
static struct thread *reader_start(struct reader *rd, struct channel *ch)
{
    rd->ch = ch;
    if (!rd->deadline)
        rd->deadline = uptime_ns() + kt_patience_ms(10000) * NS_PER_MS;
    struct thread *t = thread_create("chanwait reader", reader_main, rd, PRIO_DEFAULT);
    uint64_t d = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (thread_state(t) != T_BLOCKED && !__atomic_load_n(&rd->done, __ATOMIC_ACQUIRE)) {
        KT_ASSERT(uptime_ns() < d);
        thread_sleep_ms(1);
    }
    return t;
}

static uint32_t read_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* A message written while the reader waits goes straight to it: never
 * queued, never readable. */
KTEST(chanwait_handed_straight)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct reader rd = { 0 };
    struct thread *t = reader_start(&rd, b);
    uint32_t msg[3] = { 7, 8, 9 };
    KT_EQ(channel_write(a, msg, sizeof(msg), NULL, 0), OK);
    thread_join(t);
    KT_EQ(rd.st, OK);
    KT_EQ(rd.r.nb, sizeof(msg));
    KT_EQ(rd.r.nh, 0);
    KT_ASSERT(!memcmp(rd.buf, msg, sizeof(msg)));
    KT_ASSERT(!(kobject_signals((struct kobject *)b) & SIG_READABLE));
    uint32_t q;
    KT_EQ(channel_read(b, &q, 4, NULL, NULL, 0, NULL), ERR_SHOULD_WAIT);
    kobject_unref((struct kobject *)a);
    kobject_unref((struct kobject *)b);
}

/* Queued messages come first, in order, with no wait. */
KTEST(chanwait_queue_first)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    for (uint32_t i = 1; i <= 3; i++)
        KT_EQ(channel_write(a, &i, 4, NULL, 0), OK);
    for (uint32_t i = 1; i <= 3; i++) {
        uint32_t v = 0;
        struct chan_read r = { .buf = chan_kbytes(&v, 4) };
        KT_EQ(channel_read_wait(b, &r, uptime_ns()), OK);   /* a deadline already past */
        KT_EQ(v, i);
    }
    uint32_t v = 0;
    struct chan_read r = { .buf = chan_kbytes(&v, 4) };
    KT_EQ(channel_read_wait(b, &r, uptime_ns()), ERR_TIMED_OUT);
    kobject_unref((struct kobject *)a);
    kobject_unref((struct kobject *)b);
}

/* A message that doesn't fit the waiting reader is queued, the reader is
 * told its sizes, and it stays first in line, ahead of what comes next. */
KTEST(chanwait_too_big_stays_queued)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct reader rd = { 0 };
    struct thread *t = reader_start(&rd, b);
    uint8_t big[100];
    memset(big, 0x5a, sizeof(big));
    KT_EQ(channel_write(a, big, sizeof(big), NULL, 0), OK);
    thread_join(t);
    KT_EQ(rd.st, ERR_BUFFER_TOO_SMALL);
    KT_EQ(rd.r.nb, sizeof(big));
    uint32_t small = 42;
    KT_EQ(channel_write(a, &small, 4, NULL, 0), OK);
    uint8_t got[128];
    uint32_t nb = 0;
    KT_EQ(channel_read(b, got, sizeof(got), &nb, NULL, 0, NULL), OK);
    KT_EQ(nb, sizeof(big));
    KT_EQ(channel_read(b, got, sizeof(got), &nb, NULL, 0, NULL), OK);
    KT_EQ(read_u32(got), 42);
    kobject_unref((struct kobject *)a);
    kobject_unref((struct kobject *)b);
}

/* Handles are handed with the message when the reader has room; with no
 * room the message (and its handle) waits on the queue. */
KTEST(chanwait_handles)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct event *ev;
    KT_EQ(event_create(&ev), OK);   /* ours, to look at it afterwards */
    for (uint32_t room = 0; room < 2; room++) {
        struct reader rd = { .hcap = room };
        struct thread *t = reader_start(&rd, b);
        kobject_ref((struct kobject *)ev);   /* the handle's reference */
        struct khandle kh = khandle_from_new((struct kobject *)ev, RIGHTS_BASIC);
        KT_EQ(channel_write(a, "abcd", 4, &kh, 1), OK);
        KT_ASSERT(!kh.obj);
        thread_join(t);
        if (room) {
            KT_EQ(rd.st, OK);
            KT_EQ(rd.r.nh, 1);
            KT_ASSERT(rd.h[0].obj == (struct kobject *)ev);
            khandle_release(&rd.h[0]);
            continue;
        }
        KT_EQ(rd.st, ERR_BUFFER_TOO_SMALL);
        KT_EQ(rd.r.nh, 1);
        struct khandle got = { 0 };
        uint32_t nh = 0;
        KT_EQ(channel_read(b, rd.buf, 4, NULL, &got, 1, &nh), OK);   /* still queued */
        KT_EQ(nh, 1);
        khandle_release(&got);
    }
    KT_EQ(__atomic_load_n(&ev->base.handles, __ATOMIC_RELAXED), 0u);
    kobject_unref((struct kobject *)ev);
    kobject_unref((struct kobject *)a);
    kobject_unref((struct kobject *)b);
}

/* Two readers on one end: each message goes to exactly one of them. */
KTEST(chanwait_two_readers)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct reader rd[2] = { { 0 }, { 0 } };
    struct thread *t[2] = { reader_start(&rd[0], b), reader_start(&rd[1], b) };
    for (uint32_t i = 1; i <= 2; i++)
        KT_EQ(channel_write(a, &i, 4, NULL, 0), OK);
    thread_join(t[0]);
    thread_join(t[1]);
    KT_EQ(rd[0].st, OK);
    KT_EQ(rd[1].st, OK);
    KT_EQ(read_u32(rd[0].buf) + read_u32(rd[1].buf), 3);
    KT_ASSERT(read_u32(rd[0].buf) != read_u32(rd[1].buf));
    uint32_t q;
    KT_EQ(channel_read(b, &q, 4, NULL, NULL, 0, NULL), ERR_SHOULD_WAIT);
    kobject_unref((struct kobject *)a);
    kobject_unref((struct kobject *)b);
}

/* The wait ends with the peer closing, the deadline, or a cancel; a
 * message handed over before a cancel is delivered all the same. */
KTEST(chanwait_ends)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct reader rd = { .deadline = uptime_ns() + 2 * NS_PER_MS };
    thread_join(reader_start(&rd, b));
    KT_EQ(rd.st, ERR_TIMED_OUT);

    struct reader rc = { 0 };
    struct thread *t = reader_start(&rc, b);
    thread_cancel(t);
    thread_join(t);
    KT_EQ(rc.st, ERR_CANCELED);

    struct reader rp = { 0 };
    t = reader_start(&rp, b);
    kobject_unref((struct kobject *)a);   /* the peer closes */
    thread_join(t);
    KT_EQ(rp.st, ERR_PEER_CLOSED);
    kobject_unref((struct kobject *)b);
}

/* A request from channel_call to a reader waiting for it: the reply comes
 * back by its txid, and after the first few calls the two threads only
 * swap their two slots (no allocation: chanslot_reused_across_calls). */
static void slot_server(void *arg)
{
    struct channel *ep = arg;
    uint8_t buf[64];
    for (;;) {
        struct chan_read r = { .buf = chan_kbytes(buf, sizeof(buf)) };
        if (channel_read_wait(ep, &r, DEADLINE_NEVER) != OK)
            break;
        if (channel_write(ep, buf, r.nb, NULL, 0) != OK)
            break;
    }
    kobject_unref((struct kobject *)ep);
}

KTEST(chanwait_call_slots_reused)
{
    struct channel *mine, *theirs;
    KT_EQ(channel_create(&mine, &theirs), OK);
    cpumask_t m;
    cpumask_one(&m, kt_pin_self(0));
    struct thread *srv = thread_create_on("chanwait server", slot_server, theirs, PRIO_DEFAULT,
                                          &m);
    void *seen[8] = { NULL };
    unsigned nseen = 0;
    for (uint32_t k = 0; k < 200; k++) {
        uint32_t req[4] = { 0, k, ~k, 5 }, rep[4] = { 0 }, nb = 0;
        KT_EQ(channel_call(mine, req, sizeof(req), NULL, 0, rep, sizeof(rep), &nb, NULL, 0, NULL,
                           uptime_ns() + kt_patience_ms(10000) * NS_PER_MS),
              OK);
        KT_EQ(nb, sizeof(rep));
        KT_ASSERT(!memcmp(req, rep, sizeof(rep)));
        void *s = current_thread()->msg_slot;
        bool known = false;
        for (unsigned i = 0; i < nseen; i++)
            known |= seen[i] == s;
        if (k >= 4 && !known && nseen < 8)
            seen[nseen++] = s;
    }
    KT_IDLE_ASSERT(nseen <= 2);
    kobject_unref((struct kobject *)mine);   /* the server sees PEER_CLOSED */
    thread_join(srv);
    kt_unpin_self();
}
