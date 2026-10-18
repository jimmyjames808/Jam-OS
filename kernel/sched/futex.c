/* Futexes (<jam/futex.h>): threads waiting on a word of their own
 * process's memory, and the calls that wake them. libos's lock sleeps on
 * one; musl's mutexes and condition variables will (M13).
 *
 * A waiter is a struct on its own kernel stack, queued on one of
 * FUTEX_BUCKETS buckets by a hash of its key: the address space (the
 * calling thread's, a reference it holds while it waits) and the word's
 * user address. Nothing is allocated, so a wait can't fail for memory and
 * nothing is charged to a job.
 *
 * No lost wakes. The word is user memory, and reading it may fault and
 * sleep, so it can't be read under the bucket's spinlock (Linux reads it
 * there with page faults disabled). Instead the waiter queues itself
 * first, then reads the word with a full fence between: a waker stores to
 * the word, then takes the bucket's lock (a locked instruction: a full
 * fence) and looks for waiters. Each side writes, fences, then reads what
 * the other wrote, so at least one sees the other: the waiter sees the
 * new value (and leaves), or the waker sees the waiter (and wakes it).
 *
 * Locks: a bucket's ("futex bucket") is taken with interrupts off and
 * nothing held, two of them at once only by futex_requeue (lower address
 * first, the second nested). thread_wake runs under it, as the waiter's
 * wake-up has to: the waiter takes its bucket's lock before it returns,
 * so it can't return and free its waiter struct while a waker still
 * touches it. A requeue moves a waiter to another bucket (w->bucket
 * changes under both locks); the waiter finds its lock through
 * waiter_lock, which re-checks the bucket once it holds the lock. */
#include <jam/futex.h>
#include <jam/list.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/syscall_impl.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/usercopy.h>

#define FUTEX_BUCKETS 256   /* a power of 2 */

struct futex_bucket {
    spinlock_t       lock;      /* guards waiters and every queued waiter's fields */
    struct list_node waiters;   /* struct futex_waiter, oldest first (lazily made) */
} __attribute__((aligned(64)));

static struct futex_bucket buckets[FUTEX_BUCKETS] = {
    [0 ... FUTEX_BUCKETS - 1] = { .lock = SPINLOCK_INIT("futex bucket") },
};

struct futex_waiter {
    struct list_node     node;     /* on bucket->waiters while queued */
    struct futex_bucket *bucket;   /* the bucket it is queued on: written under that
                                    * bucket's lock and its new one's (requeue) */
    const void          *as;       /* key: the address space ... */
    uint64_t             addr;     /* ... and the word's user address */
    struct thread       *t;        /* the waiting thread */
    bool                 woken;    /* taken off the queue by a wake (bucket lock) */
};

static struct futex_bucket *bucket_of(const void *as, uint64_t addr)
{
    uint64_t h = (addr >> 2) ^ ((uint64_t)(uintptr_t)as >> 6);
    h *= 0x9e3779b97f4a7c15ull;
    return &buckets[h >> (64 - 8)];
}

_Static_assert(FUTEX_BUCKETS == 1 << 8, "bucket_of takes the top 8 bits");

/* With b's lock held. */
static struct list_node *waiters(struct futex_bucket *b)
{
    if (!b->waiters.next)
        list_init(&b->waiters);   /* the static table can't point at itself */
    return &b->waiters;
}

/* Lock the bucket w is queued on now (it may move until we hold its lock). */
static struct futex_bucket *waiter_lock(struct futex_waiter *w, uint64_t *irqflags)
{
    for (;;) {
        struct futex_bucket *b = __atomic_load_n(&w->bucket, __ATOMIC_ACQUIRE);
        *irqflags = spin_lock_irqsave(&b->lock);
        if (__atomic_load_n(&w->bucket, __ATOMIC_RELAXED) == b)
            return b;
        spin_unlock_irqrestore(&b->lock, *irqflags);
    }
}

/* Take w off its queue unless a wake already did; whether one did. */
static bool waiter_leave(struct futex_waiter *w)
{
    uint64_t f;
    struct futex_bucket *b = waiter_lock(w, &f);
    bool woken = w->woken;
    if (!woken)
        list_del(&w->node);
    spin_unlock_irqrestore(&b->lock, f);
    return woken;
}

static status_t check_args(uint64_t uaddr, uint32_t flags)
{
    if (flags || (uaddr & 3) || !user_range_ok(uaddr, sizeof(uint32_t)))
        return ERR_INVALID_ARGS;
    return OK;
}

static const void *key_space(void)
{
    return current_thread()->aspace;   /* NULL for the kernel's ring-3 tests: one space */
}

/* futex_wait's body. */
static status_t wait_word(uint64_t uaddr, uint32_t expected, uint64_t deadline_ns,
                          uint32_t flags)
{
    status_t st = check_args(uaddr, flags);
    if (st != OK)
        return st;
    struct futex_waiter w = { .as = key_space(), .addr = uaddr, .t = current_thread() };
    struct futex_bucket *b = bucket_of(w.as, uaddr);
    uint64_t f = spin_lock_irqsave(&b->lock);
    list_add_tail(waiters(b), &w.node);
    w.bucket = b;
    spin_unlock_irqrestore(&b->lock, f);

    /* Queued, then read (top): never a lost wake. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t now_val;
    st = copy_from_user(&now_val, uaddr, sizeof(now_val));
    if (st != OK || now_val != expected) {
        if (waiter_leave(&w))
            return OK;   /* woken meanwhile: report the wake, not the value */
        /* The copy's own status: ERR_INVALID_ARGS for a bad address, ERR_IO
         * when the word's pager failed its page, ERR_CANCELED when the thread
         * was killed while it waited for that page. */
        return st != OK ? st : ERR_BAD_STATE;
    }
    for (;;) {
        b = waiter_lock(&w, &f);
        if (w.woken) {
            spin_unlock_irqrestore(&b->lock, f);
            return OK;
        }
        if (deadline_ns != DEADLINE_NEVER && uptime_ns() >= deadline_ns) {
            list_del(&w.node);
            spin_unlock_irqrestore(&b->lock, f);
            return ERR_TIMED_OUT;
        }
        /* Woken (or not even blocked) by a wake, the deadline, a cancel, an
         * interrupt or for nothing: the loop looks again under the lock. A
         * wake that picked us wins over a cancel or an interrupt, which
         * would otherwise lose it (the waker counted us woken). */
        st = thread_block_interruptible_unlocked(&b->lock, f, deadline_ns);
        if (st != OK)
            return waiter_leave(&w) ? OK : thread_interrupt_consume(st);
    }
}

status_t futex_wait(uint64_t uaddr, uint32_t expected, uint64_t deadline_ns, uint32_t flags)
{
    struct wait_note old = thread_wait_note(WAITON_FUTEX, 0, uaddr);   /* the debug listing's */
    status_t st = wait_word(uaddr, expected, deadline_ns, flags);
    thread_wait_unnote(old);
    return st;
}

/* With b's lock held: wake up to `count` waiters of (as, addr) on b. */
static uint32_t wake_locked(struct futex_bucket *b, const void *as, uint64_t addr,
                            uint32_t count)
{
    uint32_t n = 0;
    struct list_node *h = waiters(b);
    for (struct list_node *p = h->next, *next; p != h && n < count; p = next) {
        next = p->next;
        struct futex_waiter *w = container_of(p, struct futex_waiter, node);
        if (w->as != as || w->addr != addr)
            continue;
        list_del(&w->node);
        w->woken = true;
        thread_wake(w->t);
        n++;
    }
    return n;
}

int64_t futex_wake(uint64_t uaddr, uint32_t count, uint32_t flags)
{
    status_t st = check_args(uaddr, flags);
    if (st != OK)
        return st;
    const void *as = key_space();
    struct futex_bucket *b = bucket_of(as, uaddr);
    uint64_t f = spin_lock_irqsave(&b->lock);
    uint32_t n = wake_locked(b, as, uaddr, count);
    spin_unlock_irqrestore(&b->lock, f);
    return n;
}

int64_t futex_requeue(uint64_t uaddr, uint32_t wake, uint64_t uaddr2, uint32_t requeue,
                      uint32_t flags)
{
    status_t st = check_args(uaddr, flags);
    if (st == OK)
        st = check_args(uaddr2, 0);
    if (st != OK)
        return st;
    const void *as = key_space();
    struct futex_bucket *b1 = bucket_of(as, uaddr), *b2 = bucket_of(as, uaddr2);
    struct futex_bucket *lo = b1 < b2 ? b1 : b2, *hi = b1 < b2 ? b2 : b1;
    uint64_t f = spin_lock_irqsave(&lo->lock);
    if (hi != lo)
        spin_lock_nested(&hi->lock, 1);
    uint32_t n = wake_locked(b1, as, uaddr, wake), moved = 0;
    if (uaddr2 != uaddr) {
        struct list_node *h = waiters(b1), *to = waiters(b2);
        for (struct list_node *p = h->next, *next; p != h && moved < requeue; p = next) {
            next = p->next;
            struct futex_waiter *w = container_of(p, struct futex_waiter, node);
            if (w->as != as || w->addr != uaddr)
                continue;
            list_del(&w->node);
            w->addr = uaddr2;
            list_add_tail(to, &w->node);
            __atomic_store_n(&w->bucket, b2, __ATOMIC_RELEASE);
            moved++;
        }
    }
    if (hi != lo)
        spin_unlock_no_resched(&hi->lock);
    spin_unlock_irqrestore(&lo->lock, f);
    return (int64_t)n + moved;
}

uint32_t futex_queued(const struct aspace *as, uint64_t uaddr)
{
    struct futex_bucket *b = bucket_of(as, uaddr);
    uint64_t f = spin_lock_irqsave(&b->lock);
    uint32_t n = 0;
    struct list_node *h = waiters(b);
    for (struct list_node *p = h->next; p != h; p = p->next) {
        struct futex_waiter *w = container_of(p, struct futex_waiter, node);
        n += w->as == as && w->addr == uaddr;
    }
    spin_unlock_irqrestore(&b->lock, f);
    return n;
}

/* ---- the system calls ------------------------------------------------------------- */

int64_t sysc_futex_wait(uint64_t addr, uint32_t expected, uint64_t deadline_ns, uint32_t flags)
{
    return futex_wait(addr, expected, deadline_ns, flags);
}

int64_t sysc_futex_wake(uint64_t addr, uint32_t count, uint32_t flags)
{
    return futex_wake(addr, count, flags);
}

int64_t sysc_futex_requeue(uint64_t addr, uint32_t wake, uint64_t addr2, uint32_t requeue,
                           uint32_t flags)
{
    return futex_requeue(addr, wake, addr2, requeue, flags);
}
