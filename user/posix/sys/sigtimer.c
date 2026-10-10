/* Timers that signal (docs/M13-PLAN.md section 6): setitimer and
 * getitimer (ITIMER_REAL: SIGALRM; musl's alarm is setitimer), and the
 * POSIX timers, timer_create and its kin (SIGEV_SIGNAL, SIGEV_NONE, and
 * SIGEV_THREAD_ID, which musl's SIGEV_THREAD timers use).
 *
 * Each timer is a kernel timer bound to the signal thread's port
 * (PERSISTENT, SIG_SIGNALED; sigthread.c). When it fires, the signal
 * thread calls sig_timer_fired, which first asks the kernel timer whether
 * it is still signalled: arming a timer clears the signal before it sets
 * the new deadline, so a packet from an arming since replaced finds it
 * clear and is dropped. Firing clears it too (cancel, or the next
 * interval's set). An interval timer that fires late counts the
 * expirations it missed as its overrun, and so does one whose signal is
 * still pending when it fires again (it isn't queued twice, as Linux).
 *
 * Times are uptime deadlines: a CLOCK_REALTIME absolute time is turned
 * into one when the timer is set (a later change of the wall clock
 * doesn't move it, as with clock_nanosleep). ITIMER_VIRTUAL and
 * ITIMER_PROF (CPU time) and the CPU-time clocks are EINVAL.
 *
 * Lock: tlock guards the table; it is taken before sig_lock (signal.c),
 * never after. */
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include "sig.h"

#define TIMERS_MAX 32            /* timer_create's, at once */
#define REAL       TIMERS_MAX    /* the table's slot for ITIMER_REAL */

struct ptimer {
    bool     used;
    bool     armed;
    handle_t h;          /* the kernel timer, bound to the signal port */
    int      clock;      /* CLOCK_REALTIME, CLOCK_MONOTONIC or CLOCK_BOOTTIME */
    int      notify;     /* SIGEV_SIGNAL, SIGEV_NONE or SIGEV_THREAD_ID */
    int      signo;
    uint32_t tid;        /* SIGEV_THREAD_ID's thread */
    int64_t  value;      /* sigev_value */
    uint64_t next;       /* the next expiry (uptime ns), while armed */
    uint64_t interval;   /* ns; 0: once */
    int32_t  overrun;    /* the last signal's missed expirations */
    int32_t  missed;     /* expirations while its signal was still pending */
};

static lock_t tlock;
static struct ptimer timers[TIMERS_MAX + 1];   /* tlock */

/* struct sigevent as the kernel takes it (musl's ksigevent). */
struct k_sigevent {
    union sigval value;
    int signo;
    int notify;
    int tid;
};


/* ---- the table ---------------------------------------------------------------------- */

/* A kernel timer for slot i bound to the signal thread. tlock. */
static long make_locked(struct ptimer *t, unsigned i)
{
    if (t->h)
        return 0;
    status_t st = jam_timer_create(&t->h);
    if (st == OK && (st = sig_timer_bind(t->h, i)) != OK) {
        jam_handle_close(t->h);
        t->h = HANDLE_INVALID;
    }
    return st == OK ? 0 : st == ERR_NO_MEMORY || st == ERR_NO_RESOURCES ? -EAGAIN : lx_err(st);
}

/* Arm t: first expiry at `at` (uptime ns; 0: disarm), then every
 * interval ns. tlock. */
static void arm_locked(struct ptimer *t, uint64_t at, uint64_t interval)
{
    t->armed = at != 0;
    t->next = at;
    t->interval = at ? interval : 0;
    t->overrun = t->missed = 0;
    if (at)
        (void)jam_timer_set(t->h, at);   /* our own timer: can't fail */
    else
        (void)jam_timer_cancel(t->h);
}

/* What is left of t: the time to its next expiry and its interval, ns. */
static void left_locked(const struct ptimer *t, uint64_t *value, uint64_t *interval)
{
    uint64_t n = now();
    *value = t->armed ? (t->next > n ? t->next - n : 1) : 0;
    *interval = t->interval;
}

/* ---- firing (the signal thread) ---------------------------------------------------- */

void sig_timer_fired(unsigned index)
{
    if (index > TIMERS_MAX)
        return;
    lock_take(&tlock);
    struct ptimer *t = &timers[index];
    signals_t seen = 0;
    if (!t->used || !t->armed ||
        jam_object_wait_one(t->h, SIG_SIGNALED, 0, &seen) != OK || !(seen & SIG_SIGNALED)) {
        lock_give(&tlock);   /* re-armed or stopped since: an old packet */
        return;
    }
    uint64_t n = now(), late = 0;
    if (t->interval) {
        late = n > t->next ? (n - t->next) / t->interval : 0;
        t->next += (late + 1) * t->interval;
        (void)jam_timer_set(t->h, t->next);
    } else {
        t->armed = false;
        (void)jam_timer_cancel(t->h);
    }
    int32_t over = late > INT32_MAX ? INT32_MAX : (int32_t)late;
    if (t->notify != SIGEV_NONE && sig_is_pending(t->signo)) {
        t->missed = t->missed > INT32_MAX - 1 - over ? INT32_MAX : t->missed + 1 + over;
        lock_give(&tlock);
        return;
    }
    t->overrun = over > INT32_MAX - t->missed ? INT32_MAX : over + t->missed;
    t->missed = 0;
    struct sig_info si = {
        .signo = t->signo, .code = index == REAL ? SI_KERNEL : SI_TIMER,
        .value = t->value, .timer = (int32_t)index, .overrun = t->overrun,
    };
    if (t->notify == SIGEV_SIGNAL)
        sig_send_process(&si);
    else if (t->notify == SIGEV_THREAD_ID)
        (void)sig_send_thread(t->tid, &si);   /* a thread that has gone gets nothing */   /* a thread that has gone gets nothing */
    lock_give(&tlock);
}

/* ---- times -------------------------------------------------------------------------- */

static uint64_t ns_of(int64_t sec, int64_t nsec)
{
    uint64_t ns;
    if (__builtin_mul_overflow((uint64_t)sec, NS_PER_S, &ns) ||
        __builtin_add_overflow(ns, (uint64_t)nsec, &ns))
        return UINT64_MAX / 2;   /* far enough */
    return ns;
}

static void to_timespec(uint64_t ns, struct timespec *ts)
{
    ts->tv_sec = (time_t)(ns / NS_PER_S);
    ts->tv_nsec = (long)(ns % NS_PER_S);
}

static bool good_ts(const struct timespec *ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < (long)NS_PER_S;
}

/* An expiry `v` ns away (or at v, abs) on clock clk as an uptime
 * deadline; 0 stays 0 (disarm). */
static uint64_t deadline_of(int clk, uint64_t v, bool abs)
{
    if (!v)
        return 0;
    uint64_t n = now(), at;
    if (abs && clk == CLOCK_REALTIME) {
        struct timespec w;
        (void)lx_clock_gettime(CLOCK_REALTIME, (long)(uintptr_t)&w, 0, 0, 0, 0);
        uint64_t wall = ns_of(w.tv_sec, w.tv_nsec);
        return v > wall ? n + (v - wall) : n;
    }
    if (abs)
        return v > n ? v : n;
    return __builtin_add_overflow(n, v, &at) ? UINT64_MAX / 2 : at;
}

/* ---- setitimer and getitimer -------------------------------------------------------- */

static void to_timeval(uint64_t ns, struct timeval *tv)
{
    tv->tv_sec = (time_t)(ns / NS_PER_S);
    tv->tv_usec = (suseconds_t)((ns % NS_PER_S + 999) / 1000);   /* never 0 for a time left */
    if (tv->tv_usec == 1000000) {
        tv->tv_sec++;
        tv->tv_usec = 0;
    }
}

static void real_left(struct itimerval *out)
{
    uint64_t v, i;
    left_locked(&timers[REAL], &v, &i);
    to_timeval(v, &out->it_value);
    to_timeval(i, &out->it_interval);
}

long lx_getitimer(long which, long cur, long c, long d, long e, long f)
{
    if (which != ITIMER_REAL)
        return -EINVAL;
    lock_take(&tlock);
    real_left((struct itimerval *)(uintptr_t)cur);
    lock_give(&tlock);
    return 0;
}

long lx_setitimer(long which, long nv, long ov, long d, long e, long f)
{
    if (which != ITIMER_REAL)
        return -EINVAL;
    const struct itimerval *in = (const struct itimerval *)(uintptr_t)nv;
    if (in && (in->it_value.tv_sec < 0 || in->it_value.tv_usec < 0 ||
               in->it_value.tv_usec >= 1000000 || in->it_interval.tv_sec < 0 ||
               in->it_interval.tv_usec < 0 || in->it_interval.tv_usec >= 1000000))
        return -EINVAL;
    lock_take(&tlock);
    struct ptimer *t = &timers[REAL];
    long r = make_locked(t, REAL);
    if (!r) {
        t->used = true;
        t->notify = SIGEV_SIGNAL;
        t->signo = SIGALRM;
        t->clock = CLOCK_MONOTONIC;
        if (ov)
            real_left((struct itimerval *)(uintptr_t)ov);
        if (in)
            arm_locked(t, deadline_of(CLOCK_MONOTONIC,
                                      ns_of(in->it_value.tv_sec, in->it_value.tv_usec * 1000), false),
                       ns_of(in->it_interval.tv_sec, in->it_interval.tv_usec * 1000));
    }
    lock_give(&tlock);
    return r;
}

/* ---- the POSIX timers --------------------------------------------------------------- */

/* The timer id as a used slot, or NULL. tlock. */
static struct ptimer *timer_at(long id)
{
    return id >= 0 && id < TIMERS_MAX && timers[id].used ? &timers[id] : NULL;
}

/* A sigevent's notification checked into t. 0 or -EINVAL. */
static long take_event(const struct k_sigevent *ev, long id, struct ptimer *t)
{
    if (!ev) {
        *t = (struct ptimer){ .notify = SIGEV_SIGNAL, .signo = SIGALRM, .value = id };
        return 0;
    }
    if (ev->notify != SIGEV_SIGNAL && ev->notify != SIGEV_NONE && ev->notify != SIGEV_THREAD_ID)
        return -EINVAL;
    if (ev->notify != SIGEV_NONE && (ev->signo < 1 || ev->signo > SIG_MAX))
        return -EINVAL;
    if (ev->notify == SIGEV_THREAD_ID && ev->tid <= 0)
        return -EINVAL;
    *t = (struct ptimer){ .notify = ev->notify, .signo = ev->signo, .tid = (uint32_t)ev->tid,
                          .value = (intptr_t)ev->value.sival_ptr };
    return 0;
}

long lx_timer_create(long clk, long sev, long idp, long d, long e, long f)
{
    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC && clk != CLOCK_BOOTTIME)
        return -EINVAL;
    lock_take(&tlock);
    long id = 0;
    while (id < TIMERS_MAX && timers[id].used)
        id++;
    long r = id == TIMERS_MAX ? -EAGAIN : 0;
    struct ptimer t;
    if (!r)
        r = take_event((const struct k_sigevent *)(uintptr_t)sev, id, &t);
    if (!r) {
        t.h = timers[id].h;   /* a kernel timer from an earlier one: kept */
        t.clock = (int)clk;
        r = make_locked(&t, (unsigned)id);
    }
    if (!r) {
        t.used = true;
        timers[id] = t;
        *(int *)(uintptr_t)idp = (int)id;
    }
    lock_give(&tlock);
    return r;
}

static void spec_left(const struct ptimer *t, struct itimerspec *out)
{
    uint64_t v, i;
    left_locked(t, &v, &i);
    to_timespec(v, &out->it_value);
    to_timespec(i, &out->it_interval);
}

long lx_timer_settime(long id, long flags, long nv, long ov, long e, long f)
{
    const struct itimerspec *in = (const struct itimerspec *)(uintptr_t)nv;
    if ((flags & ~TIMER_ABSTIME) || !good_ts(&in->it_value) || !good_ts(&in->it_interval))
        return -EINVAL;
    lock_take(&tlock);
    struct ptimer *t = timer_at(id);
    if (t) {
        if (ov)
            spec_left(t, (struct itimerspec *)(uintptr_t)ov);
        uint64_t v = ns_of(in->it_value.tv_sec, in->it_value.tv_nsec);
        arm_locked(t, deadline_of(t->clock, v, flags & TIMER_ABSTIME),
                   ns_of(in->it_interval.tv_sec, in->it_interval.tv_nsec));
    }
    lock_give(&tlock);
    return t ? 0 : -EINVAL;
}

long lx_timer_gettime(long id, long cur, long c, long d, long e, long f)
{
    lock_take(&tlock);
    struct ptimer *t = timer_at(id);
    if (t)
        spec_left(t, (struct itimerspec *)(uintptr_t)cur);
    lock_give(&tlock);
    return t ? 0 : -EINVAL;
}

long lx_timer_getoverrun(long id, long b, long c, long d, long e, long f)
{
    lock_take(&tlock);
    struct ptimer *t = timer_at(id);
    long r = t ? t->overrun : -EINVAL;
    lock_give(&tlock);
    return r;
}

long lx_timer_delete(long id, long b, long c, long d, long e, long f)
{
    lock_take(&tlock);
    struct ptimer *t = timer_at(id);
    if (t) {
        arm_locked(t, 0, 0);
        t->used = false;   /* its kernel timer stays, cancelled, for the next */
    }
    lock_give(&tlock);
    return t ? 0 : -EINVAL;
}
