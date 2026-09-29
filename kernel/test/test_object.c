/* Tests for the object/handle foundation, using a tiny test-only object;
 * user signal bits; handle generations. */
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/sched.h>
#include <jam/sys.h>
#include <jam/time.h>

struct tobj {
    struct kobject base;
    volatile int *destroyed;
    volatile int *zero_handles;
};

static void tobj_destroy(struct kobject *o)
{
    struct tobj *t = (struct tobj *)o;
    if (t->destroyed)
        (*t->destroyed)++;
    kfree(t);
}

static void tobj_zero(struct kobject *o)
{
    struct tobj *t = (struct tobj *)o;
    if (t->zero_handles)
        (*t->zero_handles)++;
}

static const struct kobject_ops tobj_ops = {
    .name = "test object", .destroy = tobj_destroy, .on_zero_handles = tobj_zero,
};

static struct tobj *tobj_new(volatile int *destroyed, volatile int *zero)
{
    struct tobj *t = kzalloc(sizeof(*t));
    kobject_init(&t->base, OBJ_EVENT, &tobj_ops, "test object", 0);
    t->destroyed = destroyed;
    t->zero_handles = zero;
    return t;
}

KTEST(handle_basic)
{
    volatile int destroyed = 0, zero = 0;
    struct handle_table tbl;
    handle_table_init(&tbl);

    struct tobj *o = tobj_new(&destroyed, &zero);
    struct khandle kh = khandle_from_new(&o->base, RIGHT_READ | RIGHTS_BASIC);
    handle_t h;
    KT_EQ(handle_insert(&tbl, &kh, &h), OK);
    KT_ASSERT(kh.obj == NULL && h != HANDLE_INVALID);

    struct kobject *got;
    rights_t r;
    KT_EQ(handle_get(&tbl, h, OBJ_EVENT, RIGHT_READ, &got, &r), OK);
    KT_ASSERT(got == &o->base && r == (RIGHT_READ | RIGHTS_BASIC));
    kobject_unref(got);

    KT_EQ(handle_get(&tbl, h, OBJ_CHANNEL, 0, &got, NULL), ERR_WRONG_TYPE);
    KT_EQ(handle_get(&tbl, h, OBJ_NONE, RIGHT_WRITE, &got, NULL), ERR_ACCESS_DENIED);
    KT_EQ(handle_get(&tbl, 0, OBJ_NONE, 0, &got, NULL), ERR_BAD_HANDLE);
    KT_EQ(handle_get(&tbl, 0x12345678, OBJ_NONE, 0, &got, NULL), ERR_BAD_HANDLE);

    /* Duplicate with fewer rights; can't gain rights. */
    handle_t h2;
    KT_EQ(handle_duplicate(&tbl, h, RIGHT_WAIT, &h2), OK);
    KT_EQ(handle_duplicate(&tbl, h2, RIGHT_WAIT, &h2), ERR_ACCESS_DENIED);  /* no DUPLICATE */
    KT_EQ(handle_duplicate(&tbl, h, RIGHT_WRITE, &h2), ERR_INVALID_ARGS);
    KT_EQ(o->base.handles, 2);

    /* Closing one handle keeps the object; closing the last runs
     * on_zero_handles, and the last reference destroys it. */
    KT_EQ(handle_close(&tbl, h), OK);
    KT_EQ(zero, 0);
    KT_EQ(handle_close(&tbl, h), ERR_BAD_HANDLE);   /* stale now */
    handle_t h3;
    KT_EQ(handle_replace(&tbl, h2, RIGHT_WAIT, &h3), OK);
    KT_EQ(handle_get(&tbl, h2, OBJ_NONE, 0, &got, NULL), ERR_BAD_HANDLE);
    KT_EQ(handle_close(&tbl, h3), OK);
    KT_EQ(zero, 1);
    KT_EQ(destroyed, 1);
    handle_table_destroy(&tbl);
}

KTEST(handle_stale_generation)
{
    /* A closed handle value must never work again, even once its slot has
     * been reused. Slot reuse is FIFO, so the very next insert takes
     * a different slot; cycling the whole free list brings the slot back with
     * a bumped generation. Either way the stale value stays invalid. */
    volatile int destroyed = 0;
    struct handle_table tbl;
    handle_table_init(&tbl);
    struct khandle a = khandle_from_new(&tobj_new(&destroyed, NULL)->base, RIGHTS_BASIC);
    handle_t ha;
    KT_EQ(handle_insert(&tbl, &a, &ha), OK);
    KT_EQ(handle_close(&tbl, ha), OK);
    struct kobject *got;
    KT_EQ(handle_get(&tbl, ha, OBJ_NONE, 0, &got, NULL), ERR_BAD_HANDLE);

    /* Insert and close enough handles to reuse ha's slot at least once, then
     * confirm ha is still rejected (a matching-generation collision would let
     * it name the new object). */
    handle_t last = ha;
    for (int i = 0; i < 4096; i++) {
        struct khandle k = khandle_from_new(&tobj_new(&destroyed, NULL)->base, RIGHTS_BASIC);
        KT_EQ(handle_insert(&tbl, &k, &last), OK);
        KT_ASSERT(last != ha);
        KT_EQ(handle_get(&tbl, ha, OBJ_NONE, 0, &got, NULL), ERR_BAD_HANDLE);
        KT_EQ(handle_close(&tbl, last), OK);
    }
    handle_table_destroy(&tbl);
    KT_EQ(destroyed, 4097);   /* a, plus one per loop iteration */
}

KTEST(handle_take_and_growth)
{
    volatile int destroyed = 0;
    struct handle_table a, b;
    handle_table_init(&a);
    handle_table_init(&b);
    enum { N = 1000 };   /* forces several table growths */
    static handle_t hs[N];
    for (int i = 0; i < N; i++) {
        struct khandle kh = khandle_from_new(&tobj_new(&destroyed, NULL)->base,
                                             i % 2 ? RIGHTS_BASIC : RIGHT_WAIT);
        KT_EQ(handle_insert(&a, &kh, &hs[i]), OK);
    }
    KT_EQ(a.used, N);
    /* Move every other handle to table b ("sending" it). */
    for (int i = 0; i < N; i++) {
        struct khandle kh;
        status_t st = handle_take(&a, hs[i], &kh);
        if (i % 2) {
            KT_EQ(st, OK);
            handle_t nh;
            KT_EQ(handle_insert(&b, &kh, &nh), OK);
        } else {
            KT_EQ(st, ERR_ACCESS_DENIED);   /* no TRANSFER right */
        }
    }
    KT_EQ(a.used, N / 2);
    KT_EQ(b.used, N / 2);
    handle_table_destroy(&a);
    handle_table_destroy(&b);
    KT_EQ(destroyed, N);
}

/* ---- signals and waiting ---------------------------------------------- */

static void signal_later(void *arg)
{
    thread_sleep_ms(30);
    kobject_signal(arg, 0, SIG_SIGNALED);
}

KTEST(object_wait_one)
{
    volatile int destroyed = 0;
    struct tobj *o = tobj_new(&destroyed, NULL);
    signals_t seen;

    /* Already satisfied: returns at once. */
    kobject_signal(&o->base, 0, SIG_READABLE);
    KT_EQ(object_wait_one(&o->base, SIG_READABLE, DEADLINE_NEVER, &seen), OK);
    KT_ASSERT(seen & SIG_READABLE);

    /* Times out, no sooner than asked. */
    uint64_t t0 = uptime_ns();
    KT_EQ(object_wait_one(&o->base, SIG_SIGNALED, t0 + 50000000, &seen), ERR_TIMED_OUT);
    KT_ASSERT(uptime_ns() - t0 >= 50000000);
    KT_ASSERT(!(seen & SIG_SIGNALED));

    /* Woken by another thread (likely on another CPU). */
    struct thread *t = thread_create("signaller", signal_later, &o->base, PRIO_DEFAULT);
    t0 = uptime_ns();
    KT_EQ(object_wait_one(&o->base, SIG_SIGNALED, t0 + 2000000000ull, &seen), OK);
    KT_ASSERT(seen & SIG_SIGNALED);
    KT_ASSERT(uptime_ns() - t0 >= 20000000);
    thread_join(t);

    kobject_unref(&o->base);
    KT_EQ(destroyed, 1);
}

/* Many waiters on one object, all released by one signal. */
static struct kobject *herd_obj;
static volatile int herd_woken;

static void herd_waiter(void *arg)
{
    (void)arg;
    if (object_wait_one(herd_obj, SIG_SIGNALED, uptime_ns() + 5000000000ull, NULL) == OK)
        __atomic_add_fetch(&herd_woken, 1, __ATOMIC_RELAXED);
}

KTEST(object_wait_many_waiters)
{
    volatile int destroyed = 0;
    struct tobj *o = tobj_new(&destroyed, NULL);
    herd_obj = &o->base;
    herd_woken = 0;
    enum { N = 40 };
    struct thread *ts[N];
    for (int i = 0; i < N; i++)
        ts[i] = thread_create("herd", herd_waiter, NULL, PRIO_DEFAULT);
    thread_sleep_ms(50);
    kobject_signal(herd_obj, 0, SIG_SIGNALED);
    for (int i = 0; i < N; i++)
        thread_join(ts[i]);
    KT_EQ(herd_woken, N);
    KT_ASSERT(list_empty(&o->base.observers));
    kobject_unref(herd_obj);
    KT_EQ(destroyed, 1);
}

/* ---- user signal bits ---------------------------------------------------------- */

KTEST(m45_object_signal_user_bits)
{
    struct handle_table t;
    handle_table_init(&t);
    handle_t ev, a, b, weak;
    KT_EQ(sys_event_create(&t, &ev), OK);
    KT_EQ(sys_channel_create(&t, &a, &b), OK);

    KT_EQ(sys_object_signal(&t, ev, 0, 1u << 24), OK);
    KT_EQ(kt_signals_of(&t, ev) & SIG_USER_ALL, 1u << 24);
    KT_EQ(sys_object_signal(&t, a, 0, 0x81000000u), OK);
    KT_EQ(kt_signals_of(&t, a) & SIG_USER_ALL, 0x81000000u);
    KT_EQ(sys_object_signal(&t, a, 1u << 24, 0), OK);
    KT_EQ(kt_signals_of(&t, a) & SIG_USER_ALL, 0x80000000u);
    KT_ASSERT(kt_signals_of(&t, a) & SIG_WRITABLE);   /* kernel bits untouched */

    /* Kernel-owned bits are off limits, and the right is required. */
    KT_EQ(sys_object_signal(&t, a, 0, SIG_READABLE), ERR_INVALID_ARGS);
    KT_EQ(sys_object_signal(&t, a, SIG_WRITABLE, 0), ERR_INVALID_ARGS);
    KT_EQ(handle_duplicate(&t, ev, RIGHTS_BASIC, &weak), OK);
    KT_EQ(sys_object_signal(&t, weak, 0, 1u << 25), ERR_ACCESS_DENIED);

    /* A user bit wakes a waiter like any signal. */
    signals_t seen = 0;
    KT_EQ(sys_object_wait_one(&t, ev, 1u << 24, uptime_ns() + NS_PER_S, &seen), OK);
    KT_ASSERT(seen & (1u << 24));

    handle_table_destroy(&t);
}

/* ---- handle generations don't wrap (they used to after 256 reuses) ---------- */

static void audit_dummy_destroy(struct kobject *o) { kfree(o); }
static const struct kobject_ops audit_dummy_ops = { .name = "audit dummy",
                                                    .destroy = audit_dummy_destroy };

KTEST(auditD_handle_generation_no_wrap)
{
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *first = kzalloc(sizeof(*first));
    kobject_init(first, OBJ_EVENT, &audit_dummy_ops, "audit dummy", 0);
    struct khandle kh = khandle_from_new(first, RIGHTS_BASIC);
    handle_t stale, h = 0;
    KT_EQ(handle_insert(&t, &kh, &stale), OK);
    KT_EQ(handle_close(&t, stale), OK);
    struct kobject *o = NULL;
    for (int i = 0; i < 256; i++) {
        o = kzalloc(sizeof(*o));
        kobject_init(o, OBJ_EVENT, &audit_dummy_ops, "audit dummy", 0);
        kh = khandle_from_new(o, RIGHTS_BASIC | RIGHT_SIGNAL);
        KT_EQ(handle_insert(&t, &kh, &h), OK);
        if (i < 255)
            KT_EQ(handle_close(&t, h), OK);
    }
    struct kobject *got = NULL;
    status_t st = handle_get(&t, stale, OBJ_NONE, 0, &got, NULL);
    kprintf("auditD: stale %x new %x lookup of stale -> %s\n", stale, h, status_str(st));
    if (st == OK)
        kobject_unref(got);
    handle_table_destroy(&t);
    KT_EQ(st, ERR_BAD_HANDLE);   /* 15-bit generation + FIFO reuse: no wrap */
}
