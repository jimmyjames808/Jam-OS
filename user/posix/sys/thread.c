/* Threads (docs/M13-PLAN.md section 13): musl's pthreads, unmodified, on
 * these calls. clone makes a thread of this process (thread_create,
 * thread_start with its TLS as the FS base); set_tid_address and clone's
 * CLONE_CHILD_CLEARTID are the kernel's tid word (kposix.c), which musl's
 * pthread_join and its thread list lock rely on: the kernel stores 0
 * there and wakes it once the thread has left its stack for good, so a
 * joiner may unmap the stack at once. exit ends the thread, exit_group
 * the process. The futex calls are Jam OS's, which are private to the
 * process (a POSIX process shares no memory with another), so Linux's
 * shared and private futexes are the same here. The operations answered
 * are wait (relative timeout), wake, requeue, and the bitset wait (an
 * absolute timeout on the monotonic or, flagged, the wall clock) and
 * wake, both with a mask of every bit; PartitionAlloc and Mojo call
 * them by raw syscall().
 *
 * Each thread's handle is kept in a table by its id (threads, under
 * threads_lock) for the calls that act on another thread (signals); the
 * thread closes its own on the way out. The first thread's handle is the
 * startup message's (SR_SELF_THREAD) and stays open. A new thread starts
 * with its creator's signal mask and is listed for signals (sig.h) from
 * before it runs musl's code until it leaves; clone returns only once it
 * is listed, so the creator's next pthread_kill or pthread_cancel finds
 * it, as on Linux (the thread exists when clone returns). */
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <time.h>
#include "layer.h"
#include "proc.h"
#include "sig.h"
#include "threads.h"

/* Linux's values (x86-64): arch_prctl's codes and futex's operations. */
#define ARCH_SET_FS          0x1002
#define ARCH_GET_FS          0x1003
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu

#define THREADS_MAX 8192   /* threads made by clone, alive at once */

/* What clone accepts: the flags musl's pthread_create passes. */
#define CLONE_OK (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |       \
                  CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | \
                  CLONE_CHILD_CLEARTID | CLONE_DETACHED)

struct thread_slot {
    uint32_t tid;      /* the thread's id */
    handle_t handle;   /* its handle */
};

static lock_t threads_lock;
static struct thread_slot threads[THREADS_MAX];   /* [0 .. nthreads), any order */
static unsigned nthreads;

static __thread uint32_t self_tid;   /* this thread's id, once it has asked for it */

/* What a new thread needs, written on its own stack by clone. */
struct clone_start {
    int  (*fn)(void *);
    void  *arg;
    int   *clear_tid;   /* CLONE_CHILD_CLEARTID's word, or NULL */
    int   *set_tid;     /* CLONE_CHILD_SETTID's word, or NULL */
    uint64_t sigmask;   /* the creator's signal mask: the new thread's */
    volatile int *listed;   /* set to 1 once the thread is on the signal list (or never will be) */
};

int __jam_set_thread_area(void *tp)
{
    return (int)lx_err(jam_thread_set_fs_base((uint64_t)(uintptr_t)tp));
}

static bool table_add(uint32_t tid, handle_t h)
{
    lock_take(&threads_lock);
    bool ok = nthreads < THREADS_MAX;
    if (ok)
        threads[nthreads++] = (struct thread_slot){ tid, h };
    lock_give(&threads_lock);
    return ok;
}

/* Take tid's entry out; its handle, or HANDLE_INVALID if it has none. */
static handle_t table_take(uint32_t tid)
{
    handle_t h = HANDLE_INVALID;
    lock_take(&threads_lock);
    for (unsigned i = 0; i < nthreads; i++) {
        if (threads[i].tid == tid) {
            h = threads[i].handle;
            threads[i] = threads[--nthreads];
            break;
        }
    }
    lock_give(&threads_lock);
    return h;
}

handle_t lx_self_thread(void)
{
    handle_t h = HANDLE_INVALID;
    lock_take(&threads_lock);
    for (unsigned i = 0; i < nthreads && h == HANDLE_INVALID; i++)
        if (threads[i].tid == self_tid)
            h = threads[i].handle;
    lock_give(&threads_lock);
    return h != HANDLE_INVALID ? h : startup_handle(SR_SELF_THREAD);   /* the first thread */
}

/* This thread is ending: close its own handle (the thread goes on until
 * thread_exit; a handle is only a reference). */
static void forget_self(void)
{
    handle_t h = table_take(self_tid);
    if (h != HANDLE_INVALID)
        jam_handle_close(h);
}

/* A new thread starts here, on its own stack, its TLS in place. */
static _Noreturn void clone_entry(struct clone_start *s)
{
    struct clone_start c = *s;
    int64_t tid = kp_set_tid_word((uint32_t *)c.clear_tid);
    if (tid <= 0) {   /* only for a bad word: musl's is a global, always good */
        __atomic_store_n(c.listed, 1, __ATOMIC_RELEASE);
        jam_thread_exit();
    }
    self_tid = (uint32_t)tid;
    if (c.set_tid)
        *c.set_tid = (int)tid;
    sig_thread_begin(lx_self_thread(), self_tid, c.sigmask);
    __atomic_store_n(c.listed, 1, __ATOMIC_RELEASE);
    int r = c.fn(c.arg);
    lx_exit(r, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

/* Start thread h on stack (its top) with s copied below it. */
static status_t start_on(handle_t h, void *stack, const struct clone_start *s, void *tls)
{
    uintptr_t top = (uintptr_t)stack & ~(uintptr_t)15;
    struct clone_start *at = (struct clone_start *)(top - ((sizeof(*s) + 15) & ~(size_t)15));
    *at = *s;
    /* As if called: rsp + 8 16-aligned, with a zero return address. */
    uint64_t sp = (uint64_t)(uintptr_t)at - 8;
    *(uint64_t *)(uintptr_t)sp = 0;
    return jam_thread_start(h, (uint64_t)(uintptr_t)clone_entry, sp, (uint64_t)(uintptr_t)at, 0,
                            (uint64_t)(uintptr_t)tls);
}

/* clone's errno for a status: out of threads or memory is EAGAIN, which
 * pthread_create passes on. */
static int clone_err(status_t st)
{
    return st == ERR_NO_RESOURCES || st == ERR_NO_MEMORY ? -EAGAIN : (int)lx_err(st);
}

int __jam_clone(int (*fn)(void *), void *stack, int flags, void *arg, int *ptid, void *tls,
                int *ctid)
{
    if ((flags & (CLONE_VM | CLONE_THREAD)) != (CLONE_VM | CLONE_THREAD))
        return -ENOSYS;   /* a process: no fork of any kind */
    if (flags & ~(CLONE_OK | 0xff))   /* the low byte: the exit signal, unused */
        return -EINVAL;
    handle_t h;
    status_t st = jam_thread_create(lx_self_process(), "pthread", 7, 0, &h);
    if (st != OK)
        return clone_err(st);
    uint32_t tid;
    st = kp_thread_tid(h, &tid);
    if (st == OK && !table_add(tid, h))
        st = ERR_NO_RESOURCES;
    if (st != OK) {
        jam_handle_close(h);
        return clone_err(st);
    }
    if (flags & CLONE_PARENT_SETTID)
        *ptid = (int)tid;
    volatile int listed = 0;
    struct clone_start s = { fn, arg, flags & CLONE_CHILD_CLEARTID ? ctid : NULL,
                             flags & CLONE_CHILD_SETTID ? ctid : NULL, sig_mask_self(), &listed };
    st = start_on(h, stack, &s, flags & CLONE_SETTLS ? tls : NULL);
    if (st != OK) {
        jam_handle_close(table_take(tid));
        return clone_err(st);
    }
    while (!__atomic_load_n(&listed, __ATOMIC_ACQUIRE))
        jam_thread_yield();
    return (int)tid;
}

unsigned __jam_unmapself_prepare(void *base, unsigned long size)
{
    sig_thread_end();
    forget_self();
    mem_forget((uint64_t)(uintptr_t)base, size);
    return lx_self_vmar();
}

long lx_set_tid_address(long word, long b, long c, long d, long e, long f)
{
    int64_t tid = kp_set_tid_word((uint32_t *)(uintptr_t)word);
    if (tid <= 0)
        return lx_err((status_t)tid);
    self_tid = (uint32_t)tid;
    return (long)tid;
}

long lx_gettid(long a, long b, long c, long d, long e, long f)
{
    return self_tid;
}

/* The first thread's id (its handle is the startup message's, which is
 * not in the table), once asked for; 0 if the kernel did not say. */
static uint32_t first_tid(void)
{
    static uint32_t first;
    uint32_t t = __atomic_load_n(&first, __ATOMIC_ACQUIRE);
    if (!t && kp_thread_tid(startup_handle(SR_SELF_THREAD), &t) == OK)
        __atomic_store_n(&first, t, __ATOMIC_RELEASE);
    return t;
}

long thread_apply(long tid, thread_op *op, void *arg)
{
    if (tid < 0 || tid > UINT32_MAX)
        return -ESRCH;
    uint32_t first = first_tid(), pid = (uint32_t)lx_pid();
    handle_t h = HANDLE_INVALID;
    status_t st = OK;
    lock_take(&threads_lock);
    uint32_t want = tid ? (uint32_t)tid : self_tid;
    for (unsigned i = 0; i < nthreads && h == HANDLE_INVALID; i++)
        if (threads[i].tid == want)
            h = threads[i].handle;
    /* The first thread's handle is not in the table; nor, as in
     * lx_self_thread, is the caller's if it is the first. */
    if (h == HANDLE_INVALID && (!tid || want == first || want == pid))
        h = startup_handle(SR_SELF_THREAD);
    if (h != HANDLE_INVALID)
        st = op(h, arg);
    lock_give(&threads_lock);
    if (h == HANDLE_INVALID)
        return -ESRCH;
    return st == OK ? 0 : lx_err(st);
}

/* musl asks the kernel to wake a dead owner's robust mutexes; the answer
 * of a kernel without them is ENOSYS, and musl goes on (it walks the list
 * itself when a thread exits). */
long lx_set_robust_list(long head, long len, long c, long d, long e, long f)
{
    return -ENOSYS;
}

long lx_get_robust_list(long pid, long head, long len, long d, long e, long f)
{
    return -ENOSYS;
}

long lx_arch_prctl(long code, long addr, long c, long d, long e, long f)
{
    if (code == ARCH_SET_FS)
        return lx_err(jam_thread_set_fs_base((uint64_t)addr));
    if (code == ARCH_GET_FS) {
        /* The thread pointer points at itself (x86-64's TLS ABI; musl
         * keeps that word), so %fs:0 is the FS base. */
        uint64_t fs;
        __asm__ volatile("mov %%fs:0, %0" : "=r"(fs));
        return uc_put((void *)(uintptr_t)addr, &fs, sizeof(fs));
    }
    return -EINVAL;
}

long lx_exit(long code, long b, long c, long d, long e, long f)
{
    sig_thread_end();
    forget_self();
    jam_thread_exit();
}

long lx_exit_group(long code, long b, long c, long d, long e, long f)
{
    sock_exit_drain();   /* netstack drops what it hasn't taken when the process ends */
    jam_process_exit(code);
}

long lx_sched_yield(long a, long b, long c, long d, long e, long f)
{
    return lx_err(jam_thread_yield());
}

/* A relative timeout as a deadline on the uptime clock. */
static long deadline_after(const struct timespec *ts, uint64_t *deadline)
{
    if (!ts) {
        *deadline = DEADLINE_NEVER;
        return 0;
    }
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
        return -EINVAL;
    uint64_t ns, at;
    if (__builtin_mul_overflow((uint64_t)ts->tv_sec, NS_PER_S, &ns) ||
        __builtin_add_overflow(ns, (uint64_t)ts->tv_nsec, &ns) ||
        __builtin_add_overflow(now(), ns, &at))
        at = DEADLINE_NEVER;
    *deadline = at;
    return 0;
}

/* An absolute time ts on clock clk (CLOCK_REALTIME or CLOCK_MONOTONIC) as a
 * deadline on the uptime clock: how far it is from the clock's now, counted
 * from ours, so a wall-clock change during the wait does not move it. A
 * time that has passed is a deadline that has too. */
static long deadline_at(long clk, const struct timespec *ts, uint64_t *deadline)
{
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
        return -EINVAL;
    struct timespec cur;
    long r = lx_clock_gettime(clk, (long)(uintptr_t)&cur, 0, 0, 0, 0);
    if (r)
        return r;
    uint64_t at, d;
    if (__builtin_mul_overflow((uint64_t)ts->tv_sec, NS_PER_S, &at) ||
        __builtin_add_overflow(at, (uint64_t)ts->tv_nsec, &at))
        at = UINT64_MAX;
    uint64_t cur_ns = (uint64_t)cur.tv_sec * NS_PER_S + (uint64_t)cur.tv_nsec;
    if (at <= cur_ns) {
        *deadline = now();
        return 0;
    }
    if (__builtin_add_overflow(now(), at - cur_ns, &d))
        d = DEADLINE_NEVER;
    *deadline = d;
    return 0;
}

static long futex_wait(uint32_t *word, uint32_t val, uint64_t deadline)
{
    status_t st = jam_futex_wait(word, val, deadline, 0);
    switch (st) {
    case OK:                 return 0;
    case ERR_BAD_STATE:      return -EAGAIN;   /* the word wasn't val */
    case ERR_TIMED_OUT:      return -ETIMEDOUT;
    case ERR_CANCELED:       return -EINTR;
    case ERR_IO:             return -EFAULT;   /* the word's page couldn't be read in */
    }
    return uc_or_fault(lx_err(st), word, sizeof(*word), false);   /* the kernel reads it */
}

/* A count as Linux's futex takes it: an int, where INT_MAX means all. */
static uint32_t futex_count(long n)
{
    return n < 0 ? 0 : n > INT_MAX ? INT_MAX : (uint32_t)n;
}

static long futex_wake(uint32_t *word, long count)
{
    int64_t n = jam_futex_wake(word, futex_count(count), 0);
    return n < 0 ? lx_err((status_t)n) : (long)n;
}

/* Linux's bitset waits and wakes pair a waiter's mask with a waker's.
 * Jam OS's futex has no masks, so a wait is only honest with a mask of
 * every bit (what Chromium and glibc pass); a wake needs some bit, since
 * every waiter here has them all. 0 if val3 is a wait mask we take, else
 * the errno. */
static long wait_mask_ok(long val3)
{
    uint32_t bits = (uint32_t)val3;
    if (!bits)
        return -EINVAL;
    if (bits != FUTEX_BITSET_MATCH_ANY) {
        lx_enosys_option("futex wait bitset", bits);
        return -ENOSYS;
    }
    return 0;
}

/* FUTEX_WAIT_BITSET: the timeout is an absolute time, on CLOCK_MONOTONIC or,
 * with FUTEX_CLOCK_REALTIME, on the wall clock; none: wait for ever. */
static long futex_wait_bitset(uint32_t *word, long op, long val, const struct timespec *ts,
                              long val3)
{
    long r = wait_mask_ok(val3);
    if (r)
        return r;
    uint64_t deadline = DEADLINE_NEVER;
    if (ts) {
        r = deadline_at(op & FUTEX_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, ts,
                        &deadline);
        if (r)
            return r;
    }
    return futex_wait(word, (uint32_t)val, deadline);
}

long lx_futex(long addr, long op, long val, long timeout, long addr2, long val3)
{
    uint32_t *word = (uint32_t *)(uintptr_t)addr;
    op = (int)op;   /* Linux's int */
    long cmd = op & ~(long)(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    if ((op & FUTEX_CLOCK_REALTIME) && cmd != FUTEX_WAIT_BITSET)
        return -ENOSYS;   /* Linux takes the clock flag on the bitset wait only */
    struct timespec t;   /* a wait's timeout, the caller's: read once, first */
    const struct timespec *ts = timeout ? &t : NULL;
    if ((cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) && timeout &&
        uc_get(&t, (const void *)(uintptr_t)timeout, sizeof(t)))
        return -EFAULT;
    uint64_t deadline;
    long r;
    int64_t n;
    switch (cmd) {
    case FUTEX_WAIT:   /* the timeout is relative */
        r = deadline_after(ts, &deadline);
        return r ? r : futex_wait(word, (uint32_t)val, deadline);
    case FUTEX_WAIT_BITSET:
        return futex_wait_bitset(word, op, val, ts, val3);
    case FUTEX_WAKE:
        return futex_wake(word, val);
    case FUTEX_WAKE_BITSET:
        return (uint32_t)val3 ? futex_wake(word, val) : -EINVAL;
    case FUTEX_REQUEUE:
        /* Linux passes the requeue count in the timeout's place. */
        n = jam_futex_requeue(word, futex_count(val), (uint32_t *)(uintptr_t)addr2,
                              futex_count(timeout), 0);
        return n < 0 ? lx_err((status_t)n) : (long)n;
    }
    lx_enosys_option("futex operation", cmd);   /* CMP_REQUEUE, WAKE_OP, the PI calls */
    return -ENOSYS;
}
