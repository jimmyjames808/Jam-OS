/* Futexes (kernel/sched/futex.c), from kernel threads that take on a user
 * address space as a user thread does (uthread_main) and call the system
 * calls' bodies on its memory: the word is read and written only through
 * copy_from_user and copy_to_user. Every check is made by the test itself
 * after its threads are joined, never by a thread that holds the address
 * space. */
#include <jam/aspace.h>
#include <jam/futex.h>
#include <jam/irq.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/usercopy.h>
#include <jam/vmo.h>

#define PG    PAGE_SIZE
#define FBASE 0x40000000ull   /* where each test's two pages are mapped */
#define FX_N  16              /* threads a test runs at most */

/* ---- an address space and threads on it ------------------------------------ */

static struct aspace *fx_space(void)
{
    struct aspace *as;
    struct vmo *v;
    KT_EQ(aspace_create(&as), OK);
    KT_EQ(vmo_create(2 * PG, 0, &v), OK);
    uint64_t a = FBASE;
    KT_EQ(aspace_map(as, v, 0, 2 * PG, ASPACE_READ | ASPACE_WRITE | ASPACE_FIXED, &a), OK);
    kobject_unref(vmo_kobject(v));   /* the mapping keeps it */
    return as;
}

struct fx_thread {
    struct aspace *as;         /* the address space it takes on */
    void         (*fn)(struct fx_thread *);
    uint32_t       i;          /* its index in the test */
    uint64_t       arg;        /* the test's */
    int64_t        st;         /* what it got */
    uint64_t       ns;         /* how long that took */
    volatile bool  done;
};

static void fx_main(void *p)
{
    struct fx_thread *x = p;
    struct thread *t = current_thread();
    aspace_ref(x->as);
    irq_disable();
    t->aspace = x->as;
    aspace_switch(NULL, x->as);
    irq_enable();
    x->fn(x);
    irq_disable();
    t->aspace = NULL;
    aspace_switch(x->as, NULL);
    irq_enable();
    aspace_unref(x->as);
    __atomic_store_n(&x->done, true, __ATOMIC_RELEASE);
}

/* cpu < 0: any CPU. */
static struct thread *fx_spawn(struct fx_thread *x, int cpu)
{
    cpumask_t m;
    if (cpu >= 0)
        cpumask_one(&m, (uint32_t)cpu % cpu_count);
    else
        cpumask_all(&m);
    struct thread *t = thread_create_on("kt-futex", fx_main, x, PRIO_DEFAULT, &m);
    KT_ASSERT(t);
    return t;
}

/* Run fn on as in a thread of its own and wait for it: x->st is its answer. */
static int64_t fx_run(struct aspace *as, void (*fn)(struct fx_thread *), uint64_t arg)
{
    struct fx_thread x = { .as = as, .fn = fn, .arg = arg };
    thread_join(fx_spawn(&x, -1));
    return x.st;
}

/* Inside a thread on the address space. */
static void put32(uint64_t ua, uint32_t v)
{
    if (copy_to_user(ua, &v, sizeof(v)) != OK)
        panic("test_futex: can't write %lx", ua);
}

static uint32_t get32(uint64_t ua)
{
    uint32_t v;
    if (copy_from_user(&v, ua, sizeof(v)) != OK)
        panic("test_futex: can't read %lx", ua);
    return v;
}

/* Wait until `n` threads are queued on ua of as. */
static void until_queued(struct aspace *as, uint64_t ua, uint32_t n)
{
    uint64_t end = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (futex_queued(as, ua) != n && uptime_ns() < end)
        thread_sleep_ms(1);
    KT_EQ(futex_queued(as, ua), n);
}

/* A waiter: on x->arg while it holds 0, for up to 5 s. */
static void waiter(struct fx_thread *x)
{
    uint64_t t0 = uptime_ns();
    x->st = futex_wait(x->arg, 0, t0 + kt_patience_ms(5000) * NS_PER_MS, 0);
    x->ns = uptime_ns() - t0;
}

static void wake_all(struct fx_thread *x)
{
    x->st = futex_wake(x->arg, UINT32_MAX, 0);
}

static void wake_two(struct fx_thread *x)
{
    x->st = futex_wake(x->arg, 2, 0);
}

static void cleanup(struct aspace *as)
{
    aspace_unref(as);
}

/* ---- the tests ------------------------------------------------------------- */

/* A word that no longer holds what the caller expects: ERR_BAD_STATE at
 * once, nothing left queued. */
static void changed_body(struct fx_thread *x)
{
    put32(FBASE, 7);
    uint64_t t0 = uptime_ns();
    x->st = futex_wait(FBASE, 6, t0 + kt_patience_ms(2000) * NS_PER_MS, 0);
    x->ns = uptime_ns() - t0;
}

KTEST(futex_value_changed_returns_at_once)
{
    struct aspace *as = fx_space();
    struct fx_thread x = { .as = as, .fn = changed_body };
    thread_join(fx_spawn(&x, -1));
    KT_EQ(x.st, ERR_BAD_STATE);
    KT_ASSERT(x.ns < kt_patience_ms(1000) * NS_PER_MS);   /* it didn't sleep */
    KT_EQ(futex_queued(as, FBASE), 0);
    cleanup(as);
}

/* A deadline ends the wait with ERR_TIMED_OUT, not before it; one already
 * past ends it at once. */
static uint64_t deadline_ns_seen[3];
static int64_t deadline_st[3];
static void deadline_body(struct fx_thread *x)
{
    put32(FBASE, 1);
    uint64_t t0 = uptime_ns();
    deadline_st[0] = futex_wait(FBASE, 1, t0 + 5 * NS_PER_MS, 0);
    deadline_ns_seen[0] = uptime_ns() - t0;
    t0 = uptime_ns();
    deadline_st[1] = futex_wait(FBASE, 1, 0, 0);
    deadline_ns_seen[1] = uptime_ns() - t0;
    deadline_st[2] = futex_wait(FBASE, 1, uptime_ns(), 0);
    x->st = OK;
}

KTEST(futex_deadline)
{
    struct aspace *as = fx_space();
    KT_EQ(fx_run(as, deadline_body, 0), OK);
    KT_EQ(deadline_st[0], ERR_TIMED_OUT);
    KT_ASSERT(deadline_ns_seen[0] >= 5 * NS_PER_MS);
    KT_EQ(deadline_st[1], ERR_TIMED_OUT);
    KT_ASSERT(deadline_ns_seen[1] < kt_patience_ms(1000) * NS_PER_MS);
    KT_EQ(deadline_st[2], ERR_TIMED_OUT);
    KT_EQ(futex_queued(as, FBASE), 0);
    cleanup(as);
}

/* Bad addresses and flags are refused, nothing queued and nothing woken:
 * unaligned, unmapped, outside user space, across the top. */
enum { BAD_N = 14 };
static int64_t bad_st[BAD_N];
static void bad_body(struct fx_thread *x)
{
    unsigned k = 0;
    bad_st[k++] = futex_wait(FBASE + 1, 0, DEADLINE_NEVER, 0);
    bad_st[k++] = futex_wait(FBASE + 2, 0, DEADLINE_NEVER, 0);
    bad_st[k++] = futex_wait(FBASE + 16 * PG, 0, DEADLINE_NEVER, 0);   /* unmapped */
    bad_st[k++] = futex_wait(0, 0, DEADLINE_NEVER, 0);
    bad_st[k++] = futex_wait(0xffff800000000000ull, 0, DEADLINE_NEVER, 0);
    bad_st[k++] = futex_wait(0x8000000000000000ull, 0, DEADLINE_NEVER, 0);
    bad_st[k++] = futex_wait(USER_TOP - 2, 0, DEADLINE_NEVER, 0);
    bad_st[k++] = futex_wait(FBASE, 0, DEADLINE_NEVER, 1);   /* flags are reserved */
    bad_st[k++] = futex_wake(FBASE + 1, 1, 0);
    bad_st[k++] = futex_wake(FBASE, 1, 1);
    bad_st[k++] = futex_wake(0xffff800000000000ull, 1, 0);
    bad_st[k++] = futex_requeue(FBASE, 1, FBASE + 3, 1, 0);
    bad_st[k++] = futex_requeue(FBASE + 1, 1, FBASE, 1, 0);
    bad_st[k++] = futex_requeue(FBASE, 1, FBASE + 4, 1, 1);
    x->st = k;
}

KTEST(futex_bad_address_refused)
{
    struct aspace *as = fx_space();
    KT_EQ(fx_run(as, bad_body, 0), BAD_N);
    for (unsigned k = 0; k < BAD_N; k++)
        KT_EQ(bad_st[k], ERR_INVALID_ARGS);
    KT_EQ(futex_queued(as, FBASE + 16 * PG), 0);
    KT_EQ(futex_queued(as, FBASE), 0);
    cleanup(as);
}

/* futex_wake wakes as many as it is told, oldest first, and says how
 * many; the waiters see OK. */
KTEST(futex_wake_counts_oldest_first)
{
    enum { N = 6 };
    struct aspace *as = fx_space();
    struct fx_thread x[N];
    struct thread *t[N];
    for (int i = 0; i < N; i++) {
        x[i] = (struct fx_thread){ .as = as, .fn = waiter, .i = i, .arg = FBASE };
        t[i] = fx_spawn(&x[i], i);
        until_queued(as, FBASE, i + 1);   /* queued in index order */
    }
    KT_EQ(fx_run(as, wake_two, FBASE), 2);
    uint64_t end = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while ((!x[0].done || !x[1].done) && uptime_ns() < end)
        thread_sleep_ms(1);
    KT_ASSERT(x[0].done && x[1].done);
    KT_EQ(x[0].st, OK);
    KT_EQ(x[1].st, OK);
    KT_EQ(futex_queued(as, FBASE), N - 2);
    KT_EQ(fx_run(as, wake_all, FBASE), N - 2);
    for (int i = 0; i < N; i++) {
        thread_join(t[i]);
        KT_EQ(x[i].st, OK);
    }
    KT_EQ(fx_run(as, wake_all, FBASE), 0);
    cleanup(as);
}

/* futex_requeue wakes `wake`, moves `requeue` of the rest to the other
 * word (a wake there wakes them, one on the first word doesn't), and
 * leaves the others. To the same word it moves nothing. */
static void requeue_body(struct fx_thread *x)
{
    x->st = futex_requeue(FBASE, 1, FBASE + 64, 3, 0);
}
static void requeue_same(struct fx_thread *x)
{
    x->st = futex_requeue(FBASE + 64, 0, FBASE + 64, 10, 0);
}

KTEST(futex_requeue_moves_waiters)
{
    enum { N = 5 };
    struct aspace *as = fx_space();
    struct fx_thread x[N];
    struct thread *t[N];
    for (int i = 0; i < N; i++) {
        x[i] = (struct fx_thread){ .as = as, .fn = waiter, .i = i, .arg = FBASE };
        t[i] = fx_spawn(&x[i], i);
        until_queued(as, FBASE, i + 1);
    }
    KT_EQ(fx_run(as, requeue_body, 0), 4);   /* 1 woken + 3 moved */
    thread_join(t[0]);
    KT_EQ(x[0].st, OK);
    KT_EQ(futex_queued(as, FBASE), 1);
    KT_EQ(futex_queued(as, FBASE + 64), 3);
    KT_EQ(fx_run(as, requeue_same, 0), 0);
    KT_EQ(futex_queued(as, FBASE + 64), 3);
    KT_EQ(fx_run(as, wake_all, FBASE + 64), 3);   /* the moved ones */
    for (int i = 1; i < 4; i++) {
        thread_join(t[i]);
        KT_EQ(x[i].st, OK);
    }
    KT_EQ(fx_run(as, wake_all, FBASE), 1);   /* the one left behind */
    thread_join(t[4]);
    KT_EQ(x[4].st, OK);
    cleanup(as);
}

/* A cancelled waiter (its process is being killed) leaves with
 * ERR_CANCELED and is no longer queued. */
static void forever(struct fx_thread *x)
{
    x->st = futex_wait(x->arg, 0, DEADLINE_NEVER, 0);
}

KTEST(futex_cancel_leaves_the_queue)
{
    struct aspace *as = fx_space();
    struct fx_thread x = { .as = as, .fn = forever, .arg = FBASE + 8 };
    struct thread *t = fx_spawn(&x, -1);
    until_queued(as, FBASE + 8, 1);
    thread_cancel(t);
    thread_join(t);
    KT_EQ(x.st, ERR_CANCELED);
    KT_EQ(futex_queued(as, FBASE + 8), 0);
    cleanup(as);
}

/* Private futexes: the same address in another address space is another
 * futex. */
KTEST(futex_private_to_its_space)
{
    struct aspace *a = fx_space(), *b = fx_space();
    struct fx_thread x = { .as = b, .fn = waiter, .arg = FBASE };
    struct thread *t = fx_spawn(&x, -1);
    until_queued(b, FBASE, 1);
    KT_EQ(fx_run(a, wake_all, FBASE), 0);
    KT_EQ(futex_queued(b, FBASE), 1);
    KT_EQ(fx_run(b, wake_all, FBASE), 1);
    thread_join(t);
    KT_EQ(x.st, OK);
    cleanup(a);
    cleanup(b);
}

/* No lost wake: pairs of threads on different CPUs pass a token back
 * and forth, each waiting on its own word until the other stores 1 there
 * and wakes it. The other answers at once, so its store and wake often
 * land while the waiter is between queueing and reading its word: a wake
 * lost there would stall the pair (the waiter times out with 1 in its
 * word). */
static uint32_t ring_threads, ring_rounds;
static uint32_t ring_lost, ring_stalls, ring_done;   /* RELAXED adds */

static uint64_t ring_word(uint32_t k)
{
    return FBASE + 64 * (uint64_t)k;
}

static void ring_body(struct fx_thread *x)
{
    uint64_t me = ring_word(x->i), other = ring_word(x->i ^ 1);
    for (uint32_t r = 0; r < ring_rounds; r++) {
        while (!get32(me)) {
            int64_t st = futex_wait(me, 0, uptime_ns() + kt_patience_ms(5000) * NS_PER_MS, 0);
            if (st != ERR_TIMED_OUT)
                continue;
            if (get32(me))
                __atomic_add_fetch(&ring_lost, 1, __ATOMIC_RELAXED);
            else
                __atomic_add_fetch(&ring_stalls, 1, __ATOMIC_RELAXED);
            if (__atomic_load_n(&ring_stalls, __ATOMIC_RELAXED) > ring_threads)
                return;   /* broken: stop */
        }
        put32(me, 0);
        put32(other, 1);
        futex_wake(other, 1, 0);
    }
    __atomic_add_fetch(&ring_done, 1, __ATOMIC_RELAXED);
}

static void ring_start(struct fx_thread *x)
{
    for (uint32_t k = 0; k < ring_threads; k++)
        put32(ring_word(k), k % 2 == 0);   /* the even one of each pair goes first */
    x->st = OK;
}

KTEST(futex_no_lost_wake)
{
    struct aspace *as = fx_space();
    ring_threads = cpu_count < 2 ? 2 : cpu_count > FX_N ? FX_N : cpu_count & ~1u;
    ring_rounds = 3000;
    ring_lost = ring_stalls = ring_done = 0;
    KT_EQ(fx_run(as, ring_start, 0), OK);
    struct fx_thread x[FX_N];
    struct thread *t[FX_N];
    for (uint32_t k = 0; k < ring_threads; k++) {
        x[k] = (struct fx_thread){ .as = as, .fn = ring_body, .i = k };
        t[k] = fx_spawn(&x[k], (int)k);   /* each on a CPU of its own */
    }
    for (uint32_t k = 0; k < ring_threads; k++)
        thread_join(t[k]);
    KT_EQ(ring_lost, 0);
    KT_EQ(ring_stalls, 0);
    KT_EQ(ring_done, ring_threads);
    for (uint32_t k = 0; k < ring_threads; k++)
        KT_EQ(futex_queued(as, ring_word(k)), 0);
    cleanup(as);
}

/* ---- thread_interrupt ---------------------------------------------------------- */

/* Before: an interrupt latched before the wait ends it at once (twice is
 * still once: the wait after runs to its deadline). */
static int64_t intr_st[2];
static void intr_before_body(struct fx_thread *x)
{
    put32(FBASE, 0);
    thread_interrupt(current_thread());
    thread_interrupt(current_thread());
    intr_st[0] = futex_wait(FBASE, 0, uptime_ns() + 5 * NS_PER_S, 0);
    intr_st[1] = futex_wait(FBASE, 0, uptime_ns() + NS_PER_MS, 0);
    x->st = OK;
}

/* Two waits in a row on FBASE: x->st the first's, x->arg gets the second's. */
static void two_waits(struct fx_thread *x)
{
    x->st = futex_wait(FBASE, 0, uptime_ns() + kt_patience_ms(5000) * NS_PER_MS, 0);
    x->arg = (uint64_t)futex_wait(FBASE, 0, uptime_ns() + 5 * NS_PER_MS, 0);
}

/* During: an interrupt ends a queued waiter's wait, taking it off the
 * queue, and only that wait. A wake that picked the waiter first wins:
 * OK, and the interrupt ends its next wait instead (none lost). */
KTEST(futex_interrupt_before_during_after)
{
    struct aspace *as = fx_space();
    KT_EQ(fx_run(as, intr_before_body, 0), OK);
    KT_EQ(intr_st[0], ERR_INTERRUPTED);
    KT_EQ(intr_st[1], ERR_TIMED_OUT);

    struct fx_thread x = { .as = as, .fn = two_waits };
    struct thread *t = fx_spawn(&x, -1);
    until_queued(as, FBASE, 1);
    thread_interrupt(t);
    thread_join(t);
    KT_EQ(x.st, ERR_INTERRUPTED);
    KT_EQ((int64_t)x.arg, ERR_TIMED_OUT);
    KT_EQ(futex_queued(as, FBASE), 0);

    struct fx_thread y = { .as = as, .fn = two_waits };
    t = fx_spawn(&y, -1);
    until_queued(as, FBASE, 1);
    KT_EQ(fx_run(as, wake_all, FBASE), 1);   /* picked: the wake is the waiter's ... */
    thread_interrupt(t);                     /* ... before this comes */
    thread_join(t);
    KT_EQ(y.st, OK);
    KT_EQ((int64_t)y.arg, ERR_INTERRUPTED);
    KT_EQ(futex_queued(as, FBASE), 0);
    cleanup(as);
}

/* ---- a word on a pager VMO's page ------------------------------------------ */

#define PBASE (FBASE + 0x100000ull)   /* where the pager VMO's two pages are mapped */

/* fx_space's address space with key's pager VMO of pg (two pages, every
 * one absent until the test answers for it) mapped read-only at PBASE. */
static struct aspace *fx_pager_space(struct pager *pg, uint64_t key)
{
    struct aspace *as = fx_space();
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, key, 2 * PG, &v), OK);
    uint64_t a = PBASE;
    KT_EQ(aspace_map(as, v, 0, 2 * PG, ASPACE_READ | ASPACE_FIXED, &a), OK);
    kobject_unref(vmo_kobject(v));   /* the mapping keeps it */
    return as;
}

/* Against a supplied word (0): one that differs, then one that matches. */
static int64_t supplied_st[2];
static void supplied_body(struct fx_thread *x)
{
    supplied_st[0] = futex_wait(PBASE, 1, DEADLINE_NEVER, 0);
    supplied_st[1] = futex_wait(PBASE, 0, uptime_ns() + NS_PER_MS, 0);
    x->st = OK;
}

/* A word whose page its pager fails is the copy's ERR_IO, whichever status
 * the pager failed it with, or when the pager doesn't answer in time;
 * nothing is left queued. An address past the mapping stays
 * ERR_INVALID_ARGS, and the word once supplied is an ordinary one. */
KTEST(futex_word_pager_failed_is_io)
{
    struct pager *pg, *slow;
    KT_EQ(pager_create(NULL, 16, 600 * NS_PER_S, &pg), OK);
    KT_EQ(pager_create(NULL, 16, 30 * NS_PER_MS, &slow), OK);
    struct aspace *as = fx_pager_space(pg, 41), *sas = fx_pager_space(slow, 42);

    static const int64_t how[] = { ERR_IO, ERR_OUT_OF_RANGE };
    for (unsigned i = 0; i < 2; i++) {
        struct fx_thread x = { .as = as, .fn = waiter, .arg = PBASE + i * PG };
        struct thread *t = fx_spawn(&x, -1);
        struct pager_request q;
        kt_pager_next(pg, &q);   /* the read of the word waits for this */
        KT_EQ(q.key, 41);
        KT_EQ(q.offset, i * PG);
        KT_EQ(pager_op(pg, 41, PAGER_OP_FAIL, i * PG, PG, how[i]), OK);
        thread_join(t);
        KT_EQ(x.st, ERR_IO);
        KT_EQ(futex_queued(as, PBASE + i * PG), 0);
    }
    KT_EQ(fx_run(sas, waiter, PBASE), ERR_IO);   /* no answer within 30 ms */
    KT_EQ(futex_queued(sas, PBASE), 0);
    KT_EQ(fx_run(as, waiter, PBASE + 2 * PG), ERR_INVALID_ARGS);   /* unmapped */

    void *zeros = kzalloc(PG);
    KT_ASSERT(zeros);
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)zeros, .user = false };
    KT_EQ(pager_supply(pg, 41, 0, &src, PG), OK);
    kfree(zeros);
    KT_EQ(fx_run(as, supplied_body, 0), OK);
    KT_EQ(supplied_st[0], ERR_BAD_STATE);
    KT_EQ(supplied_st[1], ERR_TIMED_OUT);
    KT_EQ(futex_queued(as, PBASE), 0);
    cleanup(sas);
    cleanup(as);
    kt_pager_close(slow);
    kt_pager_close(pg);
}

/* A thread killed while the read of its word waits for the pager gets
 * the copy's ERR_CANCELED and is no longer queued. */
KTEST(futex_word_pager_wait_canceled)
{
    struct pager *pg;
    KT_EQ(pager_create(NULL, 16, 600 * NS_PER_S, &pg), OK);
    struct aspace *as = fx_pager_space(pg, 43);
    struct fx_thread x = { .as = as, .fn = forever, .arg = PBASE };
    struct thread *t = fx_spawn(&x, -1);
    struct pager_request q;
    kt_pager_next(pg, &q);   /* the read of the word waits for this */
    KT_EQ(q.key, 43);
    thread_sleep_ms(5);
    thread_cancel(t);
    thread_join(t);
    KT_EQ(x.st, ERR_CANCELED);
    KT_EQ(futex_queued(as, PBASE), 0);
    cleanup(as);
    kt_pager_close(pg);
}
