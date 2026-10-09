/* Channel tests: the object-level API, the handle-level sys_ layer,
 * cross-CPU ping-pong / concurrent channel_call under load, receiving
 * handles into a full table, the writable signal, and a call cancelled by
 * closing its own endpoint. Channels holding channels are in
 * test_channel_refs.c. */
#include <jam/channel.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>

#define CH(p) ((struct kobject *)(p))

/* A test-only object that counts its destruction, to check that handles
 * inside messages are released exactly once. */
struct cobj {
    struct kobject base;       /* a real object, so it can travel in messages */
    volatile int *destroyed;   /* incremented by destroy */
};

static void cobj_destroy(struct kobject *o)
{
    struct cobj *c = (struct cobj *)o;
    if (c->destroyed)
        __atomic_add_fetch(c->destroyed, 1, __ATOMIC_RELAXED);
    kfree(c);
}

static const struct kobject_ops cobj_ops = { .name = "test object", .destroy = cobj_destroy };

static struct khandle cobj_handle(volatile int *destroyed, rights_t rights)
{
    struct cobj *c = kzalloc(sizeof(*c));
    kobject_init(&c->base, OBJ_EVENT, &cobj_ops, "test object", 0);
    c->destroyed = destroyed;
    return khandle_from_new(&c->base, rights);
}

/* Write a message of 4-byte txid 0 plus a 32-bit value. */
static status_t write_u32(struct channel *ch, uint32_t v)
{
    uint32_t m[2] = { 0, v };
    return channel_write(ch, m, sizeof(m), NULL, 0);
}

static status_t read_u32(struct channel *ch, uint32_t *v)
{
    uint32_t m[2], n;
    status_t st = channel_read(ch, m, sizeof(m), &n, NULL, 0, NULL);
    if (st == OK) {
        KT_EQ(n, sizeof(m));
        *v = m[1];
    }
    return st;
}

KTEST(channel_basic)
{
    uint64_t live = channel_live_count();
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    KT_GLOBAL_EQ(channel_live_count(), live + 2);
    KT_EQ(kobject_signals(CH(a)), SIG_WRITABLE);
    KT_EQ(kobject_signals(CH(b)), SIG_WRITABLE);

    /* FIFO, each direction independent. */
    for (uint32_t i = 0; i < 3; i++)
        KT_EQ(write_u32(a, 100 + i), OK);
    KT_EQ(write_u32(b, 7), OK);
    KT_EQ(kobject_signals(CH(b)), SIG_WRITABLE | SIG_READABLE);
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t v;
        KT_EQ(read_u32(b, &v), OK);
        KT_EQ(v, 100 + i);
    }
    KT_EQ(kobject_signals(CH(b)), SIG_WRITABLE);
    uint32_t v;
    KT_EQ(read_u32(a, &v), OK);
    KT_EQ(v, 7);

    /* Empty messages are messages too. */
    uint32_t nb = 99, nh = 99;
    KT_EQ(channel_write(a, NULL, 0, NULL, 0), OK);
    KT_EQ(channel_read(b, NULL, 0, &nb, NULL, 0, &nh), OK);
    KT_ASSERT(nb == 0 && nh == 0);

    kobject_unref(CH(a));
    kobject_unref(CH(b));
    KT_GLOBAL_EQ(channel_live_count(), live);
}

KTEST(channel_should_wait)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    uint8_t buf[8];
    uint32_t nb = 99, nh = 99;
    KT_EQ(channel_read(b, buf, sizeof(buf), &nb, NULL, 0, &nh), ERR_SHOULD_WAIT);
    KT_ASSERT(nb == 0 && nh == 0);
    KT_EQ(channel_read(a, buf, sizeof(buf), NULL, NULL, 0, NULL), ERR_SHOULD_WAIT);
    kobject_unref(CH(a));
    kobject_unref(CH(b));
}

KTEST(channel_buffer_too_small)
{
    volatile int destroyed = 0;
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    uint8_t msg[100];
    for (int i = 0; i < 100; i++)
        msg[i] = (uint8_t)i;
    struct khandle kh = cobj_handle(&destroyed, RIGHTS_BASIC);
    KT_EQ(channel_write(a, msg, sizeof(msg), &kh, 1), OK);
    KT_ASSERT(kh.obj == NULL);

    uint8_t buf[128];
    struct khandle got[2] = { 0 };
    uint32_t nb = 0, nh = 0;
    KT_EQ(channel_read(b, buf, 10, &nb, got, 2, &nh), ERR_BUFFER_TOO_SMALL);
    KT_ASSERT(nb == 100 && nh == 1);
    KT_EQ(channel_read(b, buf, sizeof(buf), &nb, got, 0, &nh), ERR_BUFFER_TOO_SMALL);
    KT_ASSERT(nb == 100 && nh == 1);
    KT_ASSERT(kobject_signals(CH(b)) & SIG_READABLE);   /* still queued */

    KT_EQ(channel_read(b, buf, sizeof(buf), &nb, got, 2, &nh), OK);
    KT_ASSERT(nb == 100 && nh == 1 && !memcmp(buf, msg, 100));
    KT_ASSERT(got[0].obj && got[0].rights == RIGHTS_BASIC);
    KT_EQ(destroyed, 0);
    khandle_release(&got[0]);
    KT_EQ(destroyed, 1);
    kobject_unref(CH(a));
    kobject_unref(CH(b));
}

KTEST(channel_limits_and_args)
{
    volatile int destroyed = 0;
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);

    /* The biggest message: 64 KiB and 64 handles. */
    uint8_t *big = kmalloc(CHANNEL_MAX_BYTES + 1);
    for (uint32_t i = 0; i <= CHANNEL_MAX_BYTES; i++)
        big[i] = (uint8_t)(i * 7);
    struct khandle hs[CHANNEL_MAX_HANDLES + 1];
    for (int i = 0; i <= CHANNEL_MAX_HANDLES; i++)
        hs[i] = cobj_handle(&destroyed, RIGHTS_BASIC);
    KT_EQ(channel_write(a, big, CHANNEL_MAX_BYTES + 1, hs, 1), ERR_OUT_OF_RANGE);
    KT_EQ(channel_write(a, big, 4, hs, CHANNEL_MAX_HANDLES + 1), ERR_OUT_OF_RANGE);
    KT_ASSERT(hs[0].obj != NULL);
    KT_EQ(channel_write(a, big, CHANNEL_MAX_BYTES, hs, CHANNEL_MAX_HANDLES), OK);
    for (int i = 0; i < CHANNEL_MAX_HANDLES; i++)
        KT_ASSERT(hs[i].obj == NULL);
    uint8_t *back = kzalloc(CHANNEL_MAX_BYTES);
    struct khandle got[CHANNEL_MAX_HANDLES];
    uint32_t nb, nh;
    KT_EQ(channel_read(b, back, CHANNEL_MAX_BYTES, &nb, got, CHANNEL_MAX_HANDLES, &nh), OK);
    KT_ASSERT(nb == CHANNEL_MAX_BYTES && nh == CHANNEL_MAX_HANDLES);
    KT_ASSERT(!memcmp(back, big, CHANNEL_MAX_BYTES));
    for (int i = 0; i < CHANNEL_MAX_HANDLES; i++)
        khandle_release(&got[i]);
    KT_EQ(destroyed, CHANNEL_MAX_HANDLES);

    /* A NULL khandle is not a handle. */
    struct khandle none = { 0 };
    KT_EQ(channel_write(a, big, 4, &none, 1), ERR_INVALID_ARGS);
    KT_EQ(channel_write(a, NULL, 4, NULL, 0), ERR_INVALID_ARGS);
    khandle_release(&hs[CHANNEL_MAX_HANDLES]);
    KT_EQ(destroyed, CHANNEL_MAX_HANDLES + 1);
    kfree(big);
    kfree(back);

    /* Queue limit: CHANNEL_MAX_QUEUED on one endpoint, then SHOULD_WAIT. */
    for (uint32_t i = 0; i < CHANNEL_MAX_QUEUED; i++)
        KT_EQ(write_u32(a, i), OK);
    KT_EQ(write_u32(a, 9999), ERR_SHOULD_WAIT);
    KT_EQ(write_u32(b, 1), OK);   /* the other direction is independent */
    uint32_t v;
    KT_EQ(read_u32(b, &v), OK);
    KT_EQ(v, 0);
    KT_EQ(write_u32(a, CHANNEL_MAX_QUEUED), OK);
    KT_EQ(write_u32(a, 9999), ERR_SHOULD_WAIT);
    kobject_unref(CH(a));
    kobject_unref(CH(b));   /* frees 1024 queued messages */
}

/* D2: an end marked CHANNEL_NO_HANDLES refuses every message carrying
 * handles (a write, a call's request, one for a reader waiting there),
 * takes plain ones, and can be sent on whatever its writers try: its queue
 * never holds a channel end. The mark is refused while a message with
 * handles is queued, and never comes off. */
static void no_handles_reader(void *arg)
{
    struct channel *ch = arg;
    uint8_t buf[16];
    struct khandle h[1];
    struct chan_read r = { .buf = chan_kbytes(buf, sizeof(buf)), .h = h, .hcap = 1 };
    status_t st = channel_read_wait(ch, &r, uptime_ns() + kt_patience_ms(10000) * NS_PER_MS);
    KT_EQ(st, OK);
    KT_EQ(r.nh, 0);   /* the plain message, not the refused one */
}

KTEST(chan_no_handles_mark)
{
    uint64_t live = channel_live_count();
    volatile int destroyed = 0;
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    KT_EQ(channel_set_flags(b, 1u << 7), ERR_INVALID_ARGS);
    struct khandle kh = cobj_handle(&destroyed, RIGHTS_BASIC);
    KT_EQ(channel_write(a, "abcd", 4, &kh, 1), OK);   /* queued on b, with a handle */
    KT_EQ(channel_set_flags(b, CHANNEL_NO_HANDLES), ERR_BAD_STATE);
    struct khandle got;
    uint8_t buf[16];
    uint32_t nb, nh;
    KT_EQ(channel_read(b, buf, sizeof(buf), &nb, &got, 1, &nh), OK);
    khandle_release(&got);
    KT_EQ(channel_set_flags(b, CHANNEL_NO_HANDLES), OK);
    KT_EQ(channel_set_flags(b, CHANNEL_NO_HANDLES), OK);   /* again: no change */
    KT_EQ(channel_set_flags(b, 0), OK);                    /* nothing comes off */

    kh = cobj_handle(&destroyed, RIGHTS_BASIC);
    KT_EQ(channel_write(a, "abcd", 4, &kh, 1), ERR_NOT_SUPPORTED);
    KT_ASSERT(kh.obj != NULL);   /* the object layer leaves it (the sys layer closes it) */
    uint32_t q[2] = { 0, 1 }, rep[2];
    KT_EQ(channel_call(a, q, sizeof(q), &kh, 1, rep, sizeof(rep), &nb, NULL, 0, NULL, 0),
          ERR_NOT_SUPPORTED);
    KT_ASSERT(!(kobject_signals(CH(b)) & SIG_READABLE));   /* nothing queued */
    /* A reader waiting on b isn't handed one either; it gets the plain one. */
    struct thread *t = thread_create("no-handles reader", no_handles_reader, b, PRIO_DEFAULT);
    thread_sleep_ms(5);
    KT_EQ(channel_write(a, "abcd", 4, &kh, 1), ERR_NOT_SUPPORTED);
    KT_EQ(write_u32(a, 7), OK);
    thread_join(t);
    /* The other direction is not marked. */
    KT_EQ(channel_write(b, "abcd", 4, &kh, 1), OK);
    KT_EQ(channel_read(a, buf, sizeof(buf), &nb, &got, 1, &nh), OK);
    khandle_release(&got);

    /* What the mark is for: a writer can't put a channel end in b's queue,
     * so b can always be sent on (the cycle rule refuses an end whose
     * queue holds one). */
    struct channel *c0, *c1, *s0, *s1;
    KT_EQ(channel_create(&c0, &c1), OK);
    KT_EQ(channel_create(&s0, &s1), OK);
    struct khandle kc = khandle_from_new(CH(c1), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(channel_write(a, "c", 1, &kc, 1), ERR_NOT_SUPPORTED);
    khandle_release(&kc);
    kobject_ref(CH(b));   /* the message takes a handle of b's: ours stays */
    struct khandle kb = khandle_from_new(CH(b), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(channel_write(s0, "b", 1, &kb, 1), OK);
    kobject_unref(CH(c0));
    kobject_unref(CH(s0));
    kobject_unref(CH(s1));   /* drops the message, and b's handle in it */
    kobject_unref(CH(a));
    kobject_unref(CH(b));
    KT_EQ(destroyed, 2);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* M12.1 (kernel core, finding 1): the mark is the end's and never comes
 * off, so only the end's sole holder may set it. An end with two handles
 * (a /svc end every program holds a duplicate of) is refused, a handle in
 * a message counting as one; once the end is back to one handle, its
 * holder marks it. A holder of a shared copy could otherwise stop every
 * handle-carrying reply (svc.connect's) to all the others. */
KTEST(chan_no_handles_mark_needs_sole_holder)
{
    uint64_t live = channel_live_count();
    struct channel *a, *b, *c0, *c1;
    KT_EQ(channel_create(&a, &b), OK);
    KT_EQ(channel_create(&c0, &c1), OK);
    struct khandle mine = khandle_from_new(CH(b), RIGHTS_BASIC | RIGHTS_IO);
    kobject_ref(CH(b));   /* each handle has a reference of its own */
    struct khandle theirs = khandle_from_new(CH(b), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(channel_set_flags(b, CHANNEL_NO_HANDLES), ERR_BAD_STATE);
    /* The shared end still takes a reply with a handle. */
    struct khandle kc = khandle_from_new(CH(c1), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(channel_write(a, "c", 1, &kc, 1), OK);
    struct khandle got;
    uint8_t buf[8];
    uint32_t nb, nh;
    KT_EQ(channel_read(b, buf, sizeof(buf), &nb, &got, 1, &nh), OK);
    khandle_release(&got);
    /* The other copy in transit, in a message on another channel: still two. */
    struct channel *s0, *s1;
    KT_EQ(channel_create(&s0, &s1), OK);
    KT_EQ(channel_write(s0, "b", 1, &theirs, 1), OK);
    KT_EQ(channel_set_flags(b, CHANNEL_NO_HANDLES), ERR_BAD_STATE);
    kobject_unref(CH(s1));   /* drops the message and the handle in it */
    kobject_unref(CH(s0));
    KT_EQ(kobject_handles(CH(b)), 1);
    KT_EQ(channel_set_flags(b, CHANNEL_NO_HANDLES), OK);   /* the sole holder */
    khandle_release(&mine);
    kobject_unref(CH(a));
    kobject_unref(CH(c0));
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* Q2: once the writer closes its end, what it wrote and nobody read is
 * charged to the reader's job: the job of the table that last received
 * the reading end (here moved from one table to another, which must count).
 * A message that job can't pay is dropped with every one after it (its
 * handles released): the reader reads a prefix of the stream, then
 * ERR_PEER_CLOSED. Reading credits the reader's job. */
KTEST(chan_unread_charged_to_reader)
{
    uint64_t live = channel_live_count();
    volatile int destroyed = 0;
    struct job *jr = kt_fresh_job(), *j2 = kt_fresh_job();
    struct handle_table tr, t2;
    handle_table_init(&tr);
    handle_table_init(&t2);
    tr.job = jr;
    t2.job = j2;
    uint8_t msg[64] = { 0 };
    uint32_t nb, nh;

    handle_t w, r, r2;
    KT_EQ(sys_channel_create(&tr, &w, &r), OK);
    for (int i = 0; i < 4; i++)
        KT_EQ(sys_channel_write(&tr, w, msg, sizeof(msg), NULL, 0), OK);
    KT_EQ(job_used(jr, JOB_LIMIT_MSG_BYTES), 0);   /* a kernel thread wrote them: no job */
    struct khandle kh;
    KT_EQ(handle_remove(&tr, r, &kh), OK);
    KT_EQ(handle_insert(&t2, &kh, &r2), OK);   /* the reading end moves to j2's table */
    KT_EQ(handle_close(&tr, w), OK);           /* the writer closes */
    uint64_t four = job_used(j2, JOB_LIMIT_MSG_BYTES), one = four / 4;
    KT_ASSERT(one >= sizeof(msg) && four == 4 * one);
    KT_EQ(job_used(jr, JOB_LIMIT_MSG_BYTES), 0);
    KT_EQ(sys_channel_read(&t2, r2, msg, sizeof(msg), &nb, NULL, 0, &nh), OK);
    KT_EQ(job_used(j2, JOB_LIMIT_MSG_BYTES), 3 * one);   /* reading credits the reader */
    KT_EQ(handle_close(&t2, r2), OK);                     /* the rest go with the end */
    KT_EQ(job_used(j2, JOB_LIMIT_MSG_BYTES), 0);

    /* Room for two and a half: two are kept, the other three (one with a
     * handle) dropped. */
    KT_EQ(sys_channel_create(&t2, &w, &r), OK);
    for (uint8_t i = 0; i < 5; i++) {
        msg[0] = i;
        if (i == 3) {
            struct khandle c = cobj_handle(&destroyed, RIGHTS_BASIC);
            handle_t hc;
            KT_EQ(handle_insert(&t2, &c, &hc), OK);
            KT_EQ(sys_channel_write(&t2, w, msg, sizeof(msg), &hc, 1), OK);
        } else {
            KT_EQ(sys_channel_write(&t2, w, msg, sizeof(msg), NULL, 0), OK);
        }
    }
    KT_EQ(job_set_limit(j2, JOB_LIMIT_MSG_BYTES, 2 * one + one / 2), OK);
    KT_EQ(handle_close(&t2, w), OK);
    KT_EQ(job_used(j2, JOB_LIMIT_MSG_BYTES), 2 * one);
    KT_EQ(destroyed, 1);   /* the dropped handle */
    for (uint8_t i = 0; i < 2; i++) {
        KT_EQ(sys_channel_read(&t2, r, msg, sizeof(msg), &nb, NULL, 0, &nh), OK);
        KT_EQ(msg[0], i);
    }
    KT_EQ(sys_channel_read(&t2, r, msg, sizeof(msg), &nb, NULL, 0, &nh), ERR_PEER_CLOSED);
    KT_EQ(job_used(j2, JOB_LIMIT_MSG_BYTES), 0);

    handle_table_destroy(&tr);
    handle_table_destroy(&t2);
    kt_job_is_empty(jr);
    kt_job_is_empty(j2);
    job_unref(jr);
    job_unref(j2);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

KTEST(channel_own_endpoint_rejected)
{
    uint64_t live = channel_live_count();
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct khandle ka = khandle_from_new(CH(a), RIGHTS_BASIC | RIGHTS_IO);
    struct khandle kb = khandle_from_new(CH(b), RIGHTS_BASIC | RIGHTS_IO);
    /* Direct: an endpoint may not carry itself or its own peer. */
    KT_EQ(channel_write(a, "abcd", 4, &ka, 1), ERR_NOT_SUPPORTED);
    KT_EQ(channel_write(a, "abcd", 4, &kb, 1), ERR_NOT_SUPPORTED);
    KT_EQ(channel_write(b, "abcd", 4, &kb, 1), ERR_NOT_SUPPORTED);
    KT_ASSERT(ka.obj == CH(a) && kb.obj == CH(b));   /* untouched */
    KT_ASSERT(!(kobject_signals(CH(b)) & SIG_READABLE));
    khandle_release(&ka);
    khandle_release(&kb);

    /* Indirect: an endpoint whose queue already holds a channel endpoint may
     * not be sent either -- that is the edge that would close a cycle. */
    struct channel *c0, *c1, *z0, *z1;
    KT_EQ(channel_create(&c0, &c1), OK);
    KT_EQ(channel_create(&z0, &z1), OK);
    struct khandle kz1 = khandle_from_new(CH(z1), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(channel_write(c0, "z", 1, &kz1, 1), OK);   /* z1 now queued on c1 */
    struct channel *s0, *s1;
    KT_EQ(channel_create(&s0, &s1), OK);
    struct khandle kc1 = khandle_from_new(CH(c1), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(channel_write(s0, "c", 1, &kc1, 1), ERR_NOT_SUPPORTED);   /* c1 holds a channel */
    KT_ASSERT(kc1.obj == CH(c1));   /* untouched */
    khandle_release(&kc1);          /* closes c1 -> drops z1 */
    kobject_unref(CH(c0));
    kobject_unref(CH(z0));
    kobject_unref(CH(s0));
    kobject_unref(CH(s1));
    KT_GLOBAL_EQ(channel_live_count(), live);
}

KTEST(channel_peer_closed)
{
    uint64_t live = channel_live_count();
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    KT_EQ(write_u32(a, 42), OK);
    kobject_unref(CH(a));   /* last reference: a closes and is destroyed */
    KT_EQ(kobject_signals(CH(b)), SIG_READABLE | SIG_PEER_CLOSED);
    KT_EQ(object_wait_one(CH(b), SIG_PEER_CLOSED, 0, NULL), OK);
    uint32_t v;
    KT_EQ(read_u32(b, &v), OK);   /* what was queued can still be read */
    KT_EQ(v, 42);
    KT_EQ(read_u32(b, &v), ERR_PEER_CLOSED);
    KT_EQ(write_u32(b, 1), ERR_PEER_CLOSED);
    KT_EQ(kobject_signals(CH(b)), SIG_PEER_CLOSED);
    kobject_unref(CH(b));

    /* The last handle going away closes an endpoint even while references
     * remain (on_zero_handles). */
    KT_EQ(channel_create(&a, &b), OK);
    struct khandle ka = khandle_from_new(CH(a), RIGHTS_BASIC | RIGHTS_IO);
    kobject_ref(CH(a));
    khandle_release(&ka);
    KT_EQ(kobject_signals(CH(b)), SIG_PEER_CLOSED);
    KT_EQ(write_u32(b, 1), ERR_PEER_CLOSED);
    KT_EQ(write_u32(a, 1), ERR_BAD_STATE);   /* a itself is closed */
    kobject_unref(CH(a));
    kobject_unref(CH(b));
    KT_GLOBAL_EQ(channel_live_count(), live);
}

KTEST(channel_transfer_handles)
{
    uint64_t live = channel_live_count();
    struct channel *a, *b, *c, *d;
    KT_EQ(channel_create(&a, &b), OK);
    KT_EQ(channel_create(&c, &d), OK);

    /* Send d over a->b, then use it on the far side. */
    struct khandle kd = khandle_from_new(CH(d), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(channel_write(a, "take", 4, &kd, 1), OK);
    KT_ASSERT(kd.obj == NULL);
    char buf[8];
    struct khandle got = { 0 };
    uint32_t nb, nh;
    KT_EQ(channel_read(b, buf, sizeof(buf), &nb, &got, 1, &nh), OK);
    KT_ASSERT(nb == 4 && nh == 1 && !memcmp(buf, "take", 4));
    KT_ASSERT(got.obj == CH(d) && got.rights == (RIGHTS_BASIC | RIGHTS_IO));
    KT_EQ(got.obj->handles, 1);

    struct channel *d2 = (struct channel *)got.obj;
    KT_EQ(write_u32(c, 0x5eed), OK);
    uint32_t v;
    KT_EQ(read_u32(d2, &v), OK);
    KT_EQ(v, 0x5eed);
    KT_EQ(write_u32(d2, 0xbeef), OK);
    KT_EQ(read_u32(c, &v), OK);
    KT_EQ(v, 0xbeef);

    khandle_release(&got);   /* d's last handle: c sees PEER_CLOSED */
    KT_ASSERT(kobject_signals(CH(c)) & SIG_PEER_CLOSED);
    kobject_unref(CH(c));
    kobject_unref(CH(a));
    kobject_unref(CH(b));
    KT_GLOBAL_EQ(channel_live_count(), live);
}

KTEST(channel_destroy_releases_queued)
{
    uint64_t live = channel_live_count();
    volatile int destroyed = 0;
    struct channel *a, *b, *c, *d;

    /* Destroying the endpoint that holds the queue releases every handle. */
    KT_EQ(channel_create(&a, &b), OK);
    for (int i = 0; i < 10; i++) {
        struct khandle hs[3];
        for (int j = 0; j < 3; j++)
            hs[j] = cobj_handle(&destroyed, RIGHTS_BASIC);
        KT_EQ(channel_write(a, "msg!", 4, hs, 3), OK);
    }
    /* A queued endpoint of another channel is closed with the queue. */
    KT_EQ(channel_create(&c, &d), OK);
    struct khandle kd = khandle_from_new(CH(d), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(channel_write(a, "chan", 4, &kd, 1), OK);
    KT_EQ(destroyed, 0);
    kobject_unref(CH(b));
    KT_EQ(destroyed, 30);
    KT_ASSERT(kobject_signals(CH(c)) & SIG_PEER_CLOSED);
    KT_EQ(write_u32(a, 1), ERR_PEER_CLOSED);
    kobject_unref(CH(a));
    kobject_unref(CH(c));

    /* Same through the last handle going away, with a reference kept. */
    destroyed = 0;
    KT_EQ(channel_create(&a, &b), OK);
    struct khandle kb = khandle_from_new(CH(b), RIGHTS_BASIC | RIGHTS_IO);
    kobject_ref(CH(b));
    for (int i = 0; i < 5; i++) {
        struct khandle h = cobj_handle(&destroyed, RIGHTS_BASIC);
        KT_EQ(channel_write(a, "msg!", 4, &h, 1), OK);
    }
    khandle_release(&kb);
    KT_EQ(destroyed, 5);
    KT_EQ(kobject_signals(CH(b)), 0);
    kobject_unref(CH(b));
    kobject_unref(CH(a));

    /* Messages queued on BOTH endpoints, both destroyed. */
    destroyed = 0;
    KT_EQ(channel_create(&a, &b), OK);
    struct khandle h1 = cobj_handle(&destroyed, RIGHTS_BASIC);
    struct khandle h2 = cobj_handle(&destroyed, RIGHTS_BASIC);
    KT_EQ(channel_write(a, "1234", 4, &h1, 1), OK);
    KT_EQ(channel_write(b, "1234", 4, &h2, 1), OK);
    kobject_unref(CH(a));
    KT_EQ(destroyed, 1);
    kobject_unref(CH(b));
    KT_EQ(destroyed, 2);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* Writers and a reader racing the close of the reading endpoint: every
 * handle sent must be released exactly once, whichever side wins. */
enum { RACE_WRITERS = 4 };
static struct channel *race_a, *race_b;
static volatile int race_made, race_destroyed, race_stop;

static void race_writer(void *arg)
{
    (void)arg;
    for (;;) {
        struct khandle kh = cobj_handle(&race_destroyed, RIGHTS_BASIC);
        __atomic_add_fetch(&race_made, 1, __ATOMIC_RELAXED);
        status_t st = channel_write(race_a, "race", 4, &kh, 1);
        if (st == OK)
            continue;
        khandle_release(&kh);   /* not sent: still ours */
        if (st == ERR_PEER_CLOSED)
            return;
        KT_EQ(st, ERR_SHOULD_WAIT);
        thread_yield();
    }
}

static void race_reader(void *arg)
{
    (void)arg;
    for (;;) {
        char buf[4];
        struct khandle kh;
        status_t st = channel_read(race_b, buf, 4, NULL, &kh, 1, NULL);
        if (st == OK)
            khandle_release(&kh);
        else if (st == ERR_BAD_STATE)
            break;   /* our endpoint was closed under us */
        else
            KT_EQ(st, ERR_SHOULD_WAIT);
    }
    kobject_unref(CH(race_b));
}

KTEST(channel_close_race)
{
    uint64_t live = channel_live_count();
    for (int round = 0; round < 5; round++) {
        race_made = race_destroyed = 0;
        KT_EQ(channel_create(&race_a, &race_b), OK);
        struct khandle kb = khandle_from_new(CH(race_b), RIGHTS_BASIC | RIGHTS_IO);
        kobject_ref(CH(race_b));   /* the reader's reference */
        struct thread *w[RACE_WRITERS];
        for (int i = 0; i < RACE_WRITERS; i++)
            w[i] = thread_create("race writer", race_writer, NULL, PRIO_DEFAULT);
        struct thread *r = thread_create("race reader", race_reader, NULL, PRIO_DEFAULT);
        thread_sleep_ms(10 + round * 5);
        khandle_release(&kb);
        for (int i = 0; i < RACE_WRITERS; i++)
            thread_join(w[i]);
        thread_join(r);
        kobject_unref(CH(race_a));
        KT_ASSERT(race_made > 0);
        KT_EQ(race_destroyed, race_made);
    }
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* ---- the handle-level layer ------------------------------------------------ */

static struct kobject *peek(struct handle_table *t, handle_t h)
{
    struct kobject *o = NULL;
    if (handle_get(t, h, OBJ_NONE, 0, &o, NULL) != OK)
        return NULL;
    kobject_unref(o);
    return o;
}

KTEST(channel_sys_layer)
{
    uint64_t live = channel_live_count();
    volatile int destroyed = 0;
    struct handle_table tbl;
    handle_table_init(&tbl);
    handle_t ha, hb;
    KT_EQ(sys_channel_create(&tbl, &ha, &hb), OK);
    struct kobject *oa;
    rights_t r;
    KT_EQ(handle_get(&tbl, ha, OBJ_CHANNEL, 0, &oa, &r), OK);
    KT_EQ(r, RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_SIGNAL);
    kobject_unref(oa);

    struct khandle k1 = cobj_handle(&destroyed, RIGHTS_BASIC);
    struct khandle k2 = cobj_handle(&destroyed, RIGHT_WAIT);   /* no TRANSFER */
    struct kobject *o1 = k1.obj;
    handle_t h1, h2;
    KT_EQ(handle_insert(&tbl, &k1, &h1), OK);
    KT_EQ(handle_insert(&tbl, &k2, &h2), OK);
    uint32_t used = tbl.used;
    uint8_t buf[16];
    uint32_t nb, nh;
    handle_t rh[4];

    /* A refused list is consumed all the same (M12's Q1). One handle
     * without RIGHT_TRANSFER: nothing is sent, and both are closed. */
    handle_t send[2] = { h1, h2 };
    KT_EQ(sys_channel_write(&tbl, ha, "abcd", 4, send, 2), ERR_ACCESS_DENIED);
    KT_EQ(tbl.used, used - 2);
    KT_ASSERT(!peek(&tbl, h1) && !peek(&tbl, h2));
    KT_EQ(destroyed, 2);
    KT_EQ(sys_channel_read(&tbl, hb, buf, sizeof(buf), &nb, rh, 4, &nh), ERR_SHOULD_WAIT);
    /* The same handle twice: the second take fails, the first is closed. */
    k1 = cobj_handle(&destroyed, RIGHTS_BASIC);
    KT_EQ(handle_insert(&tbl, &k1, &h1), OK);
    handle_t twice[2] = { h1, h1 };
    KT_EQ(sys_channel_write(&tbl, ha, "abcd", 4, twice, 2), ERR_BAD_HANDLE);
    KT_ASSERT(!peek(&tbl, h1));
    KT_EQ(destroyed, 3);
    /* Sending an endpoint of the channel itself: refused, and the list is
     * closed, that endpoint too (so its peer sees PEER_CLOSED). */
    handle_t hx, hy;
    KT_EQ(sys_channel_create(&tbl, &hx, &hy), OK);
    k1 = cobj_handle(&destroyed, RIGHTS_BASIC);
    KT_EQ(handle_insert(&tbl, &k1, &h1), OK);
    handle_t self[2] = { h1, hy };
    KT_EQ(sys_channel_write(&tbl, hx, "abcd", 4, self, 2), ERR_NOT_SUPPORTED);
    KT_ASSERT(!peek(&tbl, h1) && !peek(&tbl, hy));
    KT_EQ(destroyed, 4);
    KT_ASSERT(kt_signals_of(&tbl, hx) & SIG_PEER_CLOSED);
    KT_EQ(handle_close(&tbl, hx), OK);
    /* A write to a handle that isn't a channel: its list goes too. */
    k1 = cobj_handle(&destroyed, RIGHTS_BASIC);
    KT_EQ(handle_insert(&tbl, &k1, &h1), OK);
    o1 = peek(&tbl, h1);
    handle_t h3;
    KT_EQ(handle_duplicate(&tbl, h1, RIGHT_SAME, &h3), OK);
    KT_EQ(sys_channel_write(&tbl, h3, "abcd", 4, &h1, 1), ERR_WRONG_TYPE);
    KT_ASSERT(!peek(&tbl, h1) && peek(&tbl, h3) == o1);
    KT_EQ(destroyed, 4);   /* h3 still holds it */
    h1 = h3;

    /* Wrong type and missing rights. */
    KT_EQ(sys_channel_write(&tbl, h1, "abcd", 4, NULL, 0), ERR_WRONG_TYPE);
    handle_t ha_ro, hb_wo;
    KT_EQ(handle_duplicate(&tbl, ha, RIGHT_READ | RIGHT_WAIT, &ha_ro), OK);
    KT_EQ(handle_duplicate(&tbl, hb, RIGHT_WRITE, &hb_wo), OK);
    KT_EQ(sys_channel_write(&tbl, ha_ro, "abcd", 4, NULL, 0), ERR_ACCESS_DENIED);
    KT_EQ(sys_channel_read(&tbl, hb_wo, buf, sizeof(buf), &nb, rh, 4, &nh), ERR_ACCESS_DENIED);
    KT_EQ(sys_channel_call(&tbl, ha_ro, buf, 4, NULL, 0, buf, 16, &nb, NULL, 0, NULL, 0),
          ERR_ACCESS_DENIED);

    /* A successful send removes the handle; the reader gets a new one. */
    KT_EQ(sys_channel_write(&tbl, ha, "hand", 4, &h1, 1), OK);
    KT_ASSERT(peek(&tbl, h1) == NULL);
    KT_EQ(sys_channel_read(&tbl, hb, buf, sizeof(buf), &nb, rh, 0, &nh), ERR_BUFFER_TOO_SMALL);
    KT_EQ(nh, 1);
    KT_EQ(sys_channel_read(&tbl, hb, buf, sizeof(buf), &nb, rh, 4, &nh), OK);
    KT_ASSERT(nb == 4 && nh == 1 && !memcmp(buf, "hand", 4));
    KT_ASSERT(peek(&tbl, rh[0]) == o1);
    h1 = rh[0];

    /* Send a channel endpoint and use it through its new handle value. */
    handle_t hc, hd;
    KT_EQ(sys_channel_create(&tbl, &hc, &hd), OK);
    KT_EQ(sys_channel_write(&tbl, ha, "chan", 4, &hd, 1), OK);
    KT_EQ(sys_channel_read(&tbl, hb, buf, sizeof(buf), &nb, rh, 4, &nh), OK);
    handle_t hd2 = rh[0];
    KT_EQ(sys_channel_write(&tbl, hc, "ping", 4, NULL, 0), OK);
    KT_EQ(sys_channel_read(&tbl, hd2, buf, sizeof(buf), &nb, NULL, 0, &nh), OK);
    KT_ASSERT(nb == 4 && !memcmp(buf, "ping", 4));

    /* A write or a call that fails after the handles were taken closes
     * them: they are gone whatever the status. */
    KT_EQ(handle_close(&tbl, hb), OK);
    KT_EQ(handle_close(&tbl, hb_wo), OK);   /* b's last handle: a sees PEER_CLOSED */
    KT_EQ(sys_channel_write(&tbl, ha, "abcd", 4, &h1, 1), ERR_PEER_CLOSED);
    KT_ASSERT(!peek(&tbl, h1));
    KT_EQ(destroyed, 5);
    k1 = cobj_handle(&destroyed, RIGHTS_BASIC);
    KT_EQ(handle_insert(&tbl, &k1, &h1), OK);
    KT_EQ(sys_channel_call(&tbl, ha, buf, 4, &h1, 1, buf, 16, &nb, rh, 4, &nh, DEADLINE_NEVER),
          ERR_PEER_CLOSED);
    KT_ASSERT(!peek(&tbl, h1));
    KT_EQ(destroyed, 6);

    handle_table_destroy(&tbl);
    KT_EQ(destroyed, 6);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* ---- cross-CPU ping-pong ------------------------------------------------------ */

enum { PINGPONG_ROUNDS = 10000 };
static struct channel *pp_a, *pp_b;
static volatile uint64_t pp_ns;

static void pp_pinger(void *arg)
{
    (void)arg;
    uint64_t t0 = uptime_ns();
    for (uint32_t i = 0; i < PINGPONG_ROUNDS; i++) {
        KT_EQ(write_u32(pp_a, i), OK);
        KT_EQ(object_wait_one(CH(pp_a), SIG_READABLE, DEADLINE_NEVER, NULL), OK);
        uint32_t v;
        KT_EQ(read_u32(pp_a, &v), OK);
        KT_EQ(v, i + 1);
    }
    pp_ns = uptime_ns() - t0;
}

static void pp_ponger(void *arg)
{
    (void)arg;
    for (;;) {
        object_wait_one(CH(pp_b), SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, NULL);
        uint32_t v;
        status_t st = read_u32(pp_b, &v);
        if (st == ERR_PEER_CLOSED)
            return;
        KT_EQ(st, OK);
        KT_EQ(write_u32(pp_b, v + 1), OK);
    }
}

KTEST(channel_pingpong)
{
    KT_EQ(channel_create(&pp_a, &pp_b), OK);
    cpumask_t m1, m2;
    cpumask_all(&m1);
    cpumask_all(&m2);
    if (cpu_count >= 3) {   /* two CPUs the main thread is unlikely to need */
        cpumask_one(&m1, cpu_count - 1);
        cpumask_one(&m2, cpu_count - 2);
    }
    struct thread *pong = thread_create_on("pong", pp_ponger, NULL, PRIO_DEFAULT, &m1);
    struct thread *ping = thread_create_on("ping", pp_pinger, NULL, PRIO_DEFAULT, &m2);
    thread_join(ping);
    kobject_unref(CH(pp_a));   /* the ponger sees PEER_CLOSED and exits */
    thread_join(pong);
    kobject_unref(CH(pp_b));
    kprintf("channel: %u cross-CPU round trips, avg %lu ns each\n", PINGPONG_ROUNDS,
            pp_ns / PINGPONG_ROUNDS);
}

/* ---- channel_call ---------------------------------------------------------------- */

enum { CALLERS = 8, CALLS_EACH = 500 };

struct call_req {
    uint32_t txid, caller, seq;   /* channel_call's txid; who asked; the caller's call number */
};
struct call_reply {
    uint32_t txid, caller, seq, check;   /* as the request, plus a check value */
};

static struct channel *call_a, *call_b;
static volatile int calls_ok, calls_served;

/* Echo server: replies to every request; after the first one it also sends
 * one unsolicited message (txid 0), which must end up in the client's
 * queue, not in any caller's reply. */
static void call_server(void *arg)
{
    (void)arg;
    bool sent_extra = false;
    for (;;) {
        object_wait_one(CH(call_b), SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, NULL);
        struct call_req q;
        uint32_t nb;
        status_t st = channel_read(call_b, &q, sizeof(q), &nb, NULL, 0, NULL);
        if (st == ERR_PEER_CLOSED)
            return;
        if (st == ERR_SHOULD_WAIT)
            continue;
        KT_EQ(st, OK);
        KT_EQ(nb, sizeof(q));
        KT_ASSERT(q.txid != 0);
        struct call_reply r = { q.txid, q.caller, q.seq, q.seq * 3 + q.caller };
        KT_EQ(channel_write(call_b, &r, sizeof(r), NULL, 0), OK);
        __atomic_add_fetch(&calls_served, 1, __ATOMIC_RELAXED);
        if (!sent_extra) {
            sent_extra = true;
            KT_EQ(write_u32(call_b, 0xabcdef), OK);
        }
    }
}

static void call_client(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    for (uint32_t s = 0; s < CALLS_EACH; s++) {
        struct call_req q = { 0, id, s };
        struct call_reply r;
        uint32_t nb = 0, nh = 99;
        status_t st = channel_call(call_a, &q, sizeof(q), NULL, 0, &r, sizeof(r), &nb, NULL, 0,
                                   &nh, uptime_ns() + 10000000000ull);
        KT_EQ(st, OK);
        KT_ASSERT(nb == sizeof(r) && nh == 0);
        KT_ASSERT(q.txid != 0 && r.txid == q.txid);
        KT_ASSERT(r.caller == id && r.seq == s && r.check == s * 3 + id);
        __atomic_add_fetch(&calls_ok, 1, __ATOMIC_RELAXED);
    }
}

KTEST(channel_call_concurrent)
{
    uint64_t live = channel_live_count();
    KT_EQ(channel_create(&call_a, &call_b), OK);
    calls_ok = calls_served = 0;
    struct thread *srv = thread_create("call server", call_server, NULL, PRIO_DEFAULT);
    struct thread *cl[CALLERS];
    uint64_t t0 = uptime_ns();
    for (uintptr_t i = 0; i < CALLERS; i++)
        cl[i] = thread_create("caller", call_client, (void *)i, PRIO_DEFAULT);
    for (int i = 0; i < CALLERS; i++)
        thread_join(cl[i]);
    uint64_t ns = uptime_ns() - t0;
    KT_EQ(calls_ok, CALLERS * CALLS_EACH);
    KT_EQ(calls_served, CALLERS * CALLS_EACH);

    /* Replies never reached the queue; the one unsolicited message did. */
    uint32_t v;
    KT_EQ(read_u32(call_a, &v), OK);
    KT_EQ(v, 0xabcdef);
    KT_EQ(read_u32(call_a, &v), ERR_SHOULD_WAIT);

    kobject_unref(CH(call_a));
    thread_join(srv);
    kobject_unref(CH(call_b));
    KT_GLOBAL_EQ(channel_live_count(), live);
    kprintf("channel: %u calls from %u threads in %lu ms, %lu calls/s, avg %lu ns each\n",
            CALLERS * CALLS_EACH, CALLERS, ns / 1000000,
            (uint64_t)(CALLERS * CALLS_EACH * 1000000000ull / ns), ns / (CALLERS * CALLS_EACH));
}

KTEST(channel_call_timeout)
{
    volatile int destroyed = 0;
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct call_req q = { 0, 1, 2 };
    struct call_reply r;
    uint32_t nb, nh;
    struct khandle kh = cobj_handle(&destroyed, RIGHTS_BASIC);
    uint64_t t0 = uptime_ns();
    KT_EQ(channel_call(a, &q, sizeof(q), &kh, 1, &r, sizeof(r), &nb, NULL, 0, &nh,
                       t0 + 50000000), ERR_TIMED_OUT);
    KT_ASSERT(uptime_ns() - t0 >= 50000000);
    KT_ASSERT(kh.obj == NULL);   /* the request was sent: its handle went with it */

    /* The request is waiting at the server end, txid stamped. */
    struct call_req got;
    struct khandle gh;
    KT_EQ(channel_read(b, &got, sizeof(got), &nb, &gh, 1, &nh), OK);
    KT_ASSERT(got.txid == q.txid && got.txid != 0 && got.caller == 1 && nh == 1);
    khandle_release(&gh);
    KT_EQ(destroyed, 1);

    /* A reply after the caller gave up is dropped (M12's D5). */
    struct call_reply late = { q.txid, 1, 2, 0 };
    KT_EQ(channel_write(b, &late, sizeof(late), NULL, 0), OK);
    KT_EQ(channel_read(a, &r, sizeof(r), &nb, NULL, 0, NULL), ERR_SHOULD_WAIT);

    /* A deadline already past still sends, then returns at once. */
    KT_EQ(channel_call(a, &q, sizeof(q), NULL, 0, &r, sizeof(r), &nb, NULL, 0, &nh, 0),
          ERR_TIMED_OUT);
    KT_ASSERT(kobject_signals(CH(b)) & SIG_READABLE);
    /* Requests must have room for the txid. */
    KT_EQ(channel_call(a, &q, 3, NULL, 0, &r, sizeof(r), &nb, NULL, 0, &nh, 0),
          ERR_INVALID_ARGS);
    kobject_unref(CH(a));
    kobject_unref(CH(b));
}

/* The txid a call on ch stamps (it sends, then times out at once), read
 * back off the peer. */
static uint32_t stamped_txid(struct channel *ch, struct channel *peer)
{
    struct call_req q = { 0, 7, 0 }, got = { 0, 0, 0 };
    struct call_reply r;
    uint32_t nb = 0;
    KT_EQ(channel_call(ch, &q, sizeof(q), NULL, 0, &r, sizeof(r), &nb, NULL, 0, NULL, 0),
          ERR_TIMED_OUT);
    KT_EQ(channel_read(peer, &got, sizeof(got), &nb, NULL, 0, NULL), OK);
    KT_EQ(got.txid, q.txid);   /* written back for a kernel caller */
    return got.txid;
}

/* D1: a call's txid has CHANNEL_TXID_KERNEL set and comes from a counter
 * of its channel pair's, one for both ends: calls from either end draw the
 * next number, and a new pair starts its own count (no global counter
 * whose wrap a busy system would reach). */
KTEST(chan_txid_per_pair)
{
    struct channel *a, *b, *c, *d;
    KT_EQ(channel_create(&a, &b), OK);
    uint32_t t1 = stamped_txid(a, b), t2 = stamped_txid(b, a), t3 = stamped_txid(a, b);
    KT_EQ(t1, CHANNEL_TXID_KERNEL | 1);
    KT_EQ(t2, CHANNEL_TXID_KERNEL | 2);   /* the other end: the same counter */
    KT_EQ(t3, CHANNEL_TXID_KERNEL | 3);
    KT_EQ(channel_create(&c, &d), OK);
    KT_EQ(stamped_txid(d, c), CHANNEL_TXID_KERNEL | 1);   /* a pair of its own */
    KT_EQ(stamped_txid(a, b), CHANNEL_TXID_KERNEL | 4);
    kobject_unref(CH(a));
    kobject_unref(CH(b));
    kobject_unref(CH(c));
    kobject_unref(CH(d));
}

/* Two ends calling each other at once, each with a server reading its own
 * end: every request reaches the other end's server and every reply its
 * own caller. With a counter per end both would draw the same txids, and
 * one end's request would be taken for the other's reply. */
enum { XCALLS = 2000, XQUIT = 0xffffffffu };

struct xmsg {
    uint32_t txid, val, who;   /* who: the server that answered (0: a request) */
};

struct xend {
    struct channel *ep;    /* this side's end: the server reads it, the caller calls on it */
    uint32_t        who;   /* this side's server's number */
    uint32_t        bad;   /* the caller's wrong replies */
};

static void xserver(void *arg)
{
    struct xend *e = arg;
    for (;;) {
        struct xmsg m;
        struct chan_read r = { .buf = chan_kbytes(&m, sizeof(m)) };
        if (channel_read_wait(e->ep, &r, DEADLINE_NEVER) != OK || r.nb != sizeof(m))
            return;
        if (m.val == XQUIT)
            return;
        m.val += 1;
        m.who = e->who;
        if (channel_write(e->ep, &m, sizeof(m), NULL, 0) != OK)
            return;
    }
}

static void xcaller(void *arg)
{
    struct xend *e = arg;
    for (uint32_t k = 0; k < XCALLS; k++) {
        struct xmsg q = { 0, 2 * k + e->who, 0 }, r = { 0, 0, 0 };
        uint32_t nb = 0;
        status_t st = channel_call(e->ep, &q, sizeof(q), NULL, 0, &r, sizeof(r), &nb, NULL, 0,
                                   NULL, uptime_ns() + kt_patience_ms(10000) * NS_PER_MS);
        /* The reply comes from the other side's server (who 2 for side 1). */
        if (st != OK || nb != sizeof(r) || r.txid != q.txid || r.val != q.val + 1 ||
            r.who != 3 - e->who || !(q.txid & CHANNEL_TXID_KERNEL))
            e->bad++;
    }
}

KTEST(chan_txid_both_ends_call)
{
    uint64_t live = channel_live_count();
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct xend ea = { a, 1, 0 }, eb = { b, 2, 0 };
    struct thread *srv[2] = { thread_create("xserver a", xserver, &ea, PRIO_DEFAULT),
                              thread_create("xserver b", xserver, &eb, PRIO_DEFAULT) };
    struct thread *cl[2] = { thread_create("xcaller a", xcaller, &ea, PRIO_DEFAULT),
                             thread_create("xcaller b", xcaller, &eb, PRIO_DEFAULT) };
    thread_join(cl[0]);
    thread_join(cl[1]);
    KT_EQ(ea.bad, 0);
    KT_EQ(eb.bad, 0);
    struct xmsg quit = { 0, XQUIT, 0 };
    KT_EQ(channel_write(b, &quit, sizeof(quit), NULL, 0), OK);   /* to a's server */
    KT_EQ(channel_write(a, &quit, sizeof(quit), NULL, 0), OK);   /* to b's server */
    thread_join(srv[0]);
    thread_join(srv[1]);
    kobject_unref(CH(a));
    kobject_unref(CH(b));
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* Replies with 32 bytes and a handle, too much for the caller's buffer. */
static volatile int big_reply_destroyed;

static void big_reply_server(void *arg)
{
    struct channel *b = arg;
    KT_EQ(object_wait_one(CH(b), SIG_READABLE, DEADLINE_NEVER, NULL), OK);
    uint32_t q[4], nb;
    KT_EQ(channel_read(b, q, sizeof(q), &nb, NULL, 0, NULL), OK);
    uint32_t r[8] = { q[0] };
    struct khandle kh = cobj_handle(&big_reply_destroyed, RIGHTS_BASIC);
    KT_EQ(channel_write(b, r, sizeof(r), &kh, 1), OK);
}

KTEST(channel_call_reply_too_small)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    big_reply_destroyed = 0;
    struct thread *t = thread_create("big reply", big_reply_server, b, PRIO_DEFAULT);
    uint32_t q[2] = { 0, 5 }, r[2], nb = 0, nh = 0;
    struct khandle rh[1];
    KT_EQ(channel_call(a, q, sizeof(q), NULL, 0, r, sizeof(r), &nb, rh, 1, &nh,
                       uptime_ns() + 5000000000ull), ERR_BUFFER_TOO_SMALL);
    KT_ASSERT(nb == 32 && nh == 1);
    thread_join(t);
    KT_EQ(big_reply_destroyed, 1);   /* the dropped reply's handle */
    KT_EQ(read_u32(a, &q[0]), ERR_SHOULD_WAIT);
    kobject_unref(CH(a));
    kobject_unref(CH(b));
}

/* Reads every request, never replies, then drops the endpoint's last handle. */
enum { CLOSE_CALLERS = 4 };
static struct khandle close_kb;
static struct channel *close_a;
static volatile int close_results[CLOSE_CALLERS];

static void closing_server(void *arg)
{
    (void)arg;
    struct channel *b = (struct channel *)close_kb.obj;
    for (int got = 0; got < CLOSE_CALLERS;) {
        object_wait_one(CH(b), SIG_READABLE, DEADLINE_NEVER, NULL);
        uint32_t q[2];
        if (channel_read(b, q, sizeof(q), NULL, NULL, 0, NULL) == OK)
            got++;
    }
    thread_sleep_ms(20);   /* let the callers settle into waiting */
    khandle_release(&close_kb);
}

static void closing_caller(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    uint32_t q[2] = { 0, i }, r[2];
    close_results[i] = channel_call(close_a, q, sizeof(q), NULL, 0, r, sizeof(r), NULL, NULL,
                                    0, NULL, uptime_ns() + 5000000000ull);
}

KTEST(channel_call_peer_closed)
{
    uint64_t live = channel_live_count();
    struct channel *b;
    KT_EQ(channel_create(&close_a, &b), OK);
    close_kb = khandle_from_new(CH(b), RIGHTS_BASIC | RIGHTS_IO);
    struct thread *srv = thread_create("closing server", closing_server, NULL, PRIO_DEFAULT);
    struct thread *cl[CLOSE_CALLERS];
    uint64_t t0 = uptime_ns();
    for (uintptr_t i = 0; i < CLOSE_CALLERS; i++) {
        close_results[i] = 1;
        cl[i] = thread_create("caller", closing_caller, (void *)i, PRIO_DEFAULT);
    }
    for (int i = 0; i < CLOSE_CALLERS; i++)
        thread_join(cl[i]);
    thread_join(srv);
    for (int i = 0; i < CLOSE_CALLERS; i++)
        KT_EQ(close_results[i], ERR_PEER_CLOSED);
    KT_ASSERT(uptime_ns() - t0 < 4000000000ull);   /* woken, not timed out */

    /* Calling once the peer is gone fails at once. */
    uint32_t q[2] = { 0 }, r[2];
    KT_EQ(channel_call(close_a, q, sizeof(q), NULL, 0, r, sizeof(r), NULL, NULL, 0, NULL,
                       DEADLINE_NEVER), ERR_PEER_CLOSED);
    kobject_unref(CH(close_a));
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* ---- handles into a full table, the writable signal -------------------------- */

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
    KT_ASSERT(kt_signals_of(&t, b) & SIG_READABLE);   /* still queued */

    KT_EQ(handle_close(&t, filler), OK);            /* one free slot */
    KT_EQ(sys_channel_read(&t, b, &got, 4, &nb, rh, 4, &nh), OK);
    KT_EQ(got, 7);
    KT_EQ(nh, 1);
    KT_EQ(sys_event_signal(&t, rh[0], 0, SIG_SIGNALED), OK);   /* it is the event */
    KT_ASSERT(kt_signals_of(&t, ev) & SIG_SIGNALED);

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
    KT_GLOBAL_EQ(channel_live_count(), live);
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
                           uptime_ns() + NS_PER_S), ERR_NO_RESOURCES);
    uint32_t nb = 0, nh = 0;
    KT_EQ(sys_channel_read(&t, b, rep, 8, &nb, NULL, 0, &nh), ERR_SHOULD_WAIT);
    KT_EQ(t.used, HANDLE_TABLE_MAX);   /* the failed reservation gave its slots back */

    handle_table_destroy(&t);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

struct writable_waiter {
    struct handle_table *t;      /* the waiter's handle table */
    handle_t             h;      /* the endpoint it waits on */
    status_t             st;     /* what its wait returned */
    signals_t            seen;   /* the signals it saw */
};

static void wait_writable(void *arg)
{
    struct writable_waiter *w = arg;
    w->st = sys_object_wait_one(w->t, w->h, SIG_WRITABLE, uptime_ns() + 5 * NS_PER_S, &w->seen);
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
    KT_ASSERT(kt_signals_of(&t, a) & SIG_WRITABLE);

    uint32_t v = 0;
    for (uint32_t i = 0; i < CHANNEL_MAX_QUEUED; i++)
        KT_EQ(sys_channel_write(&t, a, &v, 4, NULL, 0), OK);
    KT_EQ(kt_signals_of(&t, a) & SIG_WRITABLE, 0);
    KT_EQ(sys_channel_write(&t, a, &v, 4, NULL, 0), ERR_SHOULD_WAIT);
    KT_ASSERT(kt_signals_of(&t, b) & SIG_WRITABLE);   /* the other direction is empty */

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
    KT_EQ(kt_signals_of(&t, a) & SIG_WRITABLE, 0);

    /* Peer gone: not writable, PEER_CLOSED, and it stays that way. */
    KT_EQ(handle_close(&t, b), OK);
    KT_EQ(kt_signals_of(&t, a) & (SIG_WRITABLE | SIG_PEER_CLOSED), SIG_PEER_CLOSED);

    handle_table_destroy(&t);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

struct caller {
    struct handle_table *t;    /* the caller's handle table */
    handle_t             h;    /* the endpoint it calls on */
    status_t             st;   /* what the call returned */
};

static void call_forever(void *arg)
{
    struct caller *c = arg;
    uint32_t req[2] = { 0, 42 }, rep[2];
    uint32_t rn = 0, rhn = 0;
    c->st = sys_channel_call(c->t, c->h, req, 8, NULL, 0, rep, 8, &rn, NULL, 0, &rhn,
                             uptime_ns() + 10 * NS_PER_S);
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
    KT_EQ(sys_object_wait_one(&t, b, SIG_READABLE, uptime_ns() + NS_PER_S, &seen), OK);
    thread_sleep_ms(10);                 /* let it block */
    uint64_t t0 = uptime_ns();
    KT_EQ(handle_close(&t, a), OK);      /* our last handle: the endpoint closes */
    thread_join(th);
    KT_EQ(c.st, ERR_CANCELED);
    KT_ASSERT(uptime_ns() - t0 < NS_PER_S);   /* promptly, not at the deadline */
    handle_table_destroy(&t);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* M12's D5: a reply to a channel_call that gave up (its deadline) is
 * dropped with its handles, never queued on the caller's end, which a
 * caller that only makes blocking calls would never read. A message with
 * any other txid is queued as before. */
KTEST(channel_late_reply_dropped)
{
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    uint32_t q[2] = { 0, 1 }, rep[2], nb = 0;
    KT_EQ(channel_call(a, q, sizeof(q), NULL, 0, rep, sizeof(rep), &nb, NULL, 0, NULL,
                       uptime_ns() + 2 * NS_PER_MS),
          ERR_TIMED_OUT);
    uint32_t req[2];
    KT_EQ(channel_read(b, req, sizeof(req), &nb, NULL, 0, NULL), OK);
    KT_ASSERT(req[0] & CHANNEL_TXID_KERNEL);
    volatile int destroyed = 0;
    struct khandle kh = cobj_handle(&destroyed, RIGHTS_BASIC);
    uint32_t r[2] = { req[0], 2 };
    KT_EQ(channel_write(b, r, sizeof(r), &kh, 1), OK);   /* the late reply: sent... */
    KT_ASSERT(kh.obj == NULL);
    KT_EQ(destroyed, 1);                                   /* ...and dropped, its handle too */
    KT_ASSERT(!(kobject_signals(CH(a)) & SIG_READABLE));
    KT_EQ(channel_write(b, r, sizeof(r), NULL, 0), OK);   /* a second: forgotten, so queued */
    KT_ASSERT(kobject_signals(CH(a)) & SIG_READABLE);
    KT_EQ(channel_read(a, rep, sizeof(rep), &nb, NULL, 0, NULL), OK);
    uint32_t other[2] = { CHANNEL_TXID_KERNEL | 12345, 3 };   /* no call of a's */
    KT_EQ(channel_write(b, other, sizeof(other), NULL, 0), OK);
    KT_EQ(channel_read(a, rep, sizeof(rep), &nb, NULL, 0, NULL), OK);
    KT_EQ(rep[1], 3);
    kobject_unref(CH(a));
    kobject_unref(CH(b));
}
