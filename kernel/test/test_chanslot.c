/* Message slots (kernel/object/channel.c, "Slots"): a small reply to a
 * channel_call that waits for it is built in its writer's per-thread slot
 * and handed over, the writer taking the caller's free slot in exchange.
 * (channel_slots is on in every test boot: only the benchmark turns it
 * off, and puts it back.) The tests: the same two slots serve call after call; a reply that
 * doesn't fit a slot is allocated as before and leaves the caller's slot
 * alone; a slot message whose reader isn't waiting for it after all is
 * queued whole, as an ordinary message; handles travel in a slot and are
 * released when the reply doesn't fit; and a user caller killed at any
 * point of its calls loses nothing and leaves nothing behind. */
#include <jam/channel.h>
#include <jam/event.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/userboot.h>

#define BIG_REPLY 2000   /* bytes: more than a slot holds */

/* An echo server on its own kernel thread: each request goes back as it
 * came, bytes and handles. A request whose second word is BIG_ASK gets a
 * BIG_REPLY-byte reply instead (its txid, then a pattern). Ends when the
 * client's end closes. */
#define BIG_ASK 0xb16b16u

static uint8_t big_byte(uint32_t i)
{
    return (uint8_t)(i * 37 + 11);
}

static void echo_server(void *arg)
{
    struct channel *ep = arg;
    uint8_t *buf = kmalloc(CHANNEL_MAX_BYTES);
    struct khandle hs[CHANNEL_MAX_HANDLES];
    for (;;) {
        signals_t s = 0;
        object_wait_one((struct kobject *)ep, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER,
                        &s);
        uint32_t nb = 0, nh = 0;
        status_t st = channel_read(ep, buf, CHANNEL_MAX_BYTES, &nb, hs, CHANNEL_MAX_HANDLES, &nh);
        if (st == ERR_SHOULD_WAIT)
            continue;
        if (st != OK)
            break;   /* the client closed its end */
        uint32_t w1 = 0;
        if (nb >= 8)
            memcpy(&w1, buf + 4, 4);
        if (w1 == BIG_ASK) {
            for (uint32_t i = 4; i < BIG_REPLY; i++)
                buf[i] = big_byte(i);
            nb = BIG_REPLY;
        }
        if (channel_write(ep, buf, nb, hs, nh) != OK)
            for (uint32_t i = 0; i < nh; i++)
                khandle_release(&hs[i]);
    }
    kfree(buf);
}

/* The server thread holds the only reference to its end: it drops it on
 * the way out. */
static void echo_server_main(void *arg)
{
    echo_server(arg);
    kobject_unref((struct kobject *)arg);
}

/* An echo server pinned to cpu (theirs), and the client's end (mine). */
static struct thread *echo_start(uint32_t cpu, struct channel **mine)
{
    struct channel *theirs;
    KT_EQ(channel_create(mine, &theirs), OK);
    cpumask_t m;
    cpumask_one(&m, cpu);
    return thread_create_on("chanslot echo", echo_server_main, theirs, PRIO_DEFAULT, &m);
}

/* Close the client's end: the server sees it, ends, and drops its own. */
static void echo_stop(struct thread *server, struct channel *mine)
{
    kobject_unref((struct kobject *)mine);
    thread_join(server);
}

/* Request k of a run: n bytes (txid, k, then a pattern from k). */
static void fill_req(uint8_t *b, uint32_t n, uint32_t k)
{
    memcpy(b + 4, &k, 4);
    for (uint32_t i = 8; i < n; i++)
        b[i] = (uint8_t)(k + i);
}

static bool echo_ok(const uint8_t *req, const uint8_t *rep, uint32_t n)
{
    return !memcmp(req, rep, n);   /* the txid included: the kernel wrote it into req */
}

/* One call of n bytes; the reply must be the request. */
static void call_echo(struct channel *ch, uint32_t n, uint32_t k)
{
    uint8_t req[512], rep[512];
    fill_req(req, n, k);
    uint32_t nb = 0, nh = 99;
    KT_EQ(channel_call(ch, req, n, NULL, 0, rep, sizeof(rep), &nb, NULL, 0, &nh,
                       uptime_ns() + kt_patience_ms(10000) * NS_PER_MS),
          OK);
    KT_EQ(nb, n);
    KT_EQ(nh, 0);
    KT_ASSERT(echo_ok(req, rep, n));
}

/* The caller and the server swap two slots back and forth: after the
 * first few calls no new one appears, whatever the size (up to what a
 * slot holds). Under load a reply can come before the caller has offered
 * its slot (the server preempted onto our CPU at once), which costs one
 * allocation: the count is checked on an idle machine only. */
KTEST(chanslot_reused_across_calls)
{
    struct channel *mine;
    struct thread *srv = echo_start(kt_pin_self(0), &mine);
    for (uint32_t k = 0; k < 4; k++)
        call_echo(mine, 16, k);
    void *seen[8] = { current_thread()->msg_slot };
    unsigned nseen = 1;
    for (uint32_t k = 4; k < 260; k++) {
        call_echo(mine, 8 + (k * 7) % 400, k);
        void *s = current_thread()->msg_slot;
        KT_ASSERT(s);   /* a handed reply leaves us its slot */
        bool known = false;
        for (unsigned i = 0; i < nseen; i++)
            known |= seen[i] == s;
        if (!known && nseen < 8)
            seen[nseen++] = s;
    }
    KT_IDLE_ASSERT(nseen <= 2);
    echo_stop(srv, mine);
    kt_unpin_self();
}

/* A reply too big for a slot is allocated (and would be charged), and
 * the caller keeps the slot it had. */
KTEST(chanslot_big_reply_allocated)
{
    struct channel *mine;
    struct thread *srv = echo_start(kt_pin_self(0), &mine);
    call_echo(mine, 16, 1);   /* we have a slot from now on */
    void *slot = current_thread()->msg_slot;
    KT_ASSERT(slot);
    uint8_t *rep = kmalloc(BIG_REPLY);
    KT_ASSERT(rep);
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t req[2] = { 0, BIG_ASK }, nb = 0, nh = 0;
        KT_EQ(channel_call(mine, req, sizeof(req), NULL, 0, rep, BIG_REPLY, &nb, NULL, 0, &nh,
                           uptime_ns() + kt_patience_ms(10000) * NS_PER_MS),
              OK);
        KT_EQ(nb, BIG_REPLY);
        KT_ASSERT(!memcmp(rep, req, 4));
        for (uint32_t i = 4; i < BIG_REPLY; i++)
            KT_EQ(rep[i], big_byte(i));
        KT_ASSERT(current_thread()->msg_slot == slot);
    }
    kfree(rep);
    echo_stop(srv, mine);
    kt_unpin_self();
}

/* A caller on one end, waiting for its reply. */
struct waiting_caller {
    struct channel *ch;
    uint32_t        req[2];   /* the request; the kernel writes its txid into req[0] */
    uint32_t        rep[2];   /* the reply */
    status_t        st;       /* what the call returned */
};

static void wait_for_reply(void *arg)
{
    struct waiting_caller *c = arg;
    uint32_t nb = 0, nh = 0;
    c->st = channel_call(c->ch, c->req, sizeof(c->req), NULL, 0, c->rep, sizeof(c->rep), &nb,
                         NULL, 0, &nh, uptime_ns() + kt_patience_ms(10000) * NS_PER_MS);
}

/* A writer sees a caller waiting on the other end and builds its message
 * in its slot, but the message is not that caller's reply (another txid,
 * or none): it is queued as an ordinary message, whole and with its
 * handle, and the caller still gets its own reply. */
KTEST(chanslot_message_for_nobody_queued)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct waiting_caller c = { .ch = a, .req = { 0, 77 } };
    struct thread *t = thread_create("chanslot caller", wait_for_reply, &c, PRIO_DEFAULT);
    /* Its request on b's queue means it waits (it lists itself first). */
    uint64_t d = uptime_ns() + kt_patience_ms(10000) * NS_PER_MS;
    while (!(kobject_signals((struct kobject *)b) & SIG_READABLE)) {
        KT_ASSERT(uptime_ns() < d);
        thread_sleep_ms(1);
    }

    struct event *ev;
    KT_EQ(event_create(&ev), OK);
    kobject_ref((struct kobject *)ev);   /* ours, to look at it afterwards */
    struct khandle kh = khandle_from_new((struct kobject *)ev, RIGHTS_BASIC);
    uint32_t stray[3] = { 0xfeed, 1, 2 };   /* nobody's txid */
    kfree(current_thread()->msg_slot);   /* ours to drop: a slot after the write is a new one */
    current_thread()->msg_slot = NULL;
    KT_EQ(channel_write(b, stray, sizeof(stray), &kh, 1), OK);
    KT_ASSERT(!kh.obj);
    /* Built in our slot (we had none, so a new one), then copied out of it
     * into a queued message: the slot is ours again. */
    KT_ASSERT(current_thread()->msg_slot);

    /* The caller's request, answered. */
    uint32_t q[2], nb = 0, nh = 0;
    KT_EQ(channel_read(b, q, sizeof(q), &nb, NULL, 0, &nh), OK);
    KT_EQ(q[1], 77u);
    uint32_t r[2] = { q[0], 78 };
    KT_EQ(channel_write(b, r, sizeof(r), NULL, 0), OK);
    thread_join(t);
    KT_EQ(c.st, OK);
    KT_EQ(c.rep[0], q[0]);
    KT_EQ(c.rep[1], 78u);

    /* The stray one waits on a's queue, whole. */
    uint32_t got[3] = { 0 };
    struct khandle gh = { 0 };
    KT_EQ(channel_read(a, got, sizeof(got), &nb, &gh, 1, &nh), OK);
    KT_EQ(nb, sizeof(got));
    KT_EQ(nh, 1);
    KT_ASSERT(!memcmp(got, stray, sizeof(got)));
    KT_ASSERT(gh.obj == (struct kobject *)ev);
    khandle_release(&gh);
    KT_EQ(channel_read(a, got, sizeof(got), &nb, NULL, 0, &nh), ERR_SHOULD_WAIT);
    KT_EQ(__atomic_load_n(&ev->base.handles, __ATOMIC_RELAXED), 0u);
    kobject_unref((struct kobject *)ev);
    kobject_unref((struct kobject *)a);
    kobject_unref((struct kobject *)b);
}

/* Handles travel in a slot reply; a reply with more handles than the
 * caller has room for is dropped with its handles released. */
KTEST(chanslot_handles_in_slot_reply)
{
    struct channel *mine;
    struct thread *srv = echo_start(kt_pin_self(0), &mine);
    struct event *ev[2];
    struct khandle kh[2], got[2] = { 0 };
    for (int i = 0; i < 2; i++) {
        KT_EQ(event_create(&ev[i]), OK);
        kobject_ref((struct kobject *)ev[i]);
        kh[i] = khandle_from_new((struct kobject *)ev[i], RIGHTS_BASIC);
    }
    uint32_t req[4] = { 0, 5, 6, 7 }, rep[4], nb = 0, nh = 0;
    uint64_t d = uptime_ns() + kt_patience_ms(10000) * NS_PER_MS;
    KT_EQ(channel_call(mine, req, sizeof(req), kh, 2, rep, sizeof(rep), &nb, got, 2, &nh, d),
          OK);
    KT_ASSERT(nb == sizeof(rep) && nh == 2 && !memcmp(req, rep, sizeof(rep)));
    KT_ASSERT(got[0].obj == (struct kobject *)ev[0] && got[1].obj == (struct kobject *)ev[1]);
    /* Back they go; the echo has room for one only. */
    KT_EQ(channel_call(mine, req, sizeof(req), got, 2, rep, sizeof(rep), &nb, kh, 1, &nh, d),
          ERR_BUFFER_TOO_SMALL);
    KT_EQ(nh, 2);
    for (int i = 0; i < 2; i++) {
        KT_EQ(__atomic_load_n(&ev[i]->base.handles, __ATOMIC_RELAXED), 0u);
        kobject_unref((struct kobject *)ev[i]);
    }
    echo_stop(srv, mine);
    kt_unpin_self();
}

/* ---- a user caller killed at any point of its calls ------------------------ */

#define KILL_ROUNDS 12

/* utest's bench-call (user/tests/utest/bench.c) calls on SR_USER + 1 as
 * fast as it can; it is killed after a random few ms. Its calls get
 * replies in slots, its thread holds one: everything is gone once it is,
 * and the server just sees the end close. */
KTEST(chanslot_user_caller_killed)
{
    struct job *j = kt_fresh_job();
    uint32_t cpu = kt_pin_self(0);
    uint64_t rng = 0x5107ull + uptime_ns();
    for (unsigned i = 0; i < KILL_ROUNDS; i++) {
        struct channel *res_k, *res_u, *mine, *theirs;
        KT_EQ(channel_create(&res_k, &res_u), OK);
        KT_EQ(channel_create(&mine, &theirs), OK);
        cpumask_t m;
        cpumask_one(&m, cpu);
        struct thread *srv = thread_create_on("chanslot echo", echo_server_main, theirs,
                                              PRIO_DEFAULT, &m);
        struct userboot_handle x[2] = {
            { SR_USER, khandle_from_new((struct kobject *)res_u, RIGHTS_BASIC | RIGHTS_IO) },
            { SR_USER + 1, khandle_from_new((struct kobject *)mine, RIGHTS_BASIC | RIGHTS_IO) },
        };
        const char *argv[] = { "utest", "bench-call" };
        struct process *p;
        KT_EQ(userboot_spawn("bin/utest", argv, 2, j, x, 2, &m, &p), OK);
        thread_sleep_ms(1 + kt_rng(&rng) % 15);
        process_kill(p, PROCESS_KILLED_CODE, true);
        KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED,
                              uptime_ns() + kt_patience_ms(10000) * NS_PER_MS, NULL),
              OK);
        thread_join(srv);   /* the client's end closed with it */
        kobject_unref(process_kobject(p));
        kobject_unref((struct kobject *)res_k);
    }
    kt_unpin_self();
    for (uint64_t d = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
         job_used(j, JOB_LIMIT_PAGES) && uptime_ns() < d;)
        thread_sleep_ms(1);   /* dead is not yet freed */
    KT_EQ(job_used(j, JOB_LIMIT_MSG_BYTES), 0);
    kt_job_is_empty(j);
    job_unref(j);
}
