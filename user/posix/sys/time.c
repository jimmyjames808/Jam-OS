/* Clocks and sleeps (docs/M13-PLAN.md section 10), with the kernel's
 * calls: CLOCK_MONOTONIC and CLOCK_BOOTTIME (and their raw and coarse
 * kinds) are the uptime, clock_get; CLOCK_REALTIME is the wall clock,
 * wallclock_get's UTC moved on by the uptime since it was read (with no
 * wall clock, the uptime counted from 1970); the CPU-time clocks are the
 * kernel's per-process and per-thread counts (kposix.c). Resolution 1 ns.
 * The clocks without a system call (the kernel's time page) replace these
 * reads when the page exists.
 *
 * nanosleep and clock_nanosleep sleep with sleep_until on the uptime: an
 * absolute CLOCK_REALTIME time is turned into an uptime deadline at the
 * call (a wall-clock change during the sleep doesn't move it). A sleep
 * ended by thread_interrupt is EINTR, with the time left. Setting a
 * clock is EPERM: the clock's power is init's and sntp's. */
#include <errno.h>
#include <time.h>
#include "layer.h"

#define CLOCK_MAX 11   /* CLOCK_TAI, the highest clock id Linux has */

/* The wall clock now, in ns since 1970 UTC. */
static int64_t wall_ns(void)
{
    struct wall_clock wc;
    uint64_t t = now();
    if (jam_wallclock_get(&wc) != OK)
        return (int64_t)t;
    return wc.utc_ns + (int64_t)(t - wc.uptime_ns);
}

/* Clock clk's time now, in ns; -EINVAL for a clock we don't have. */
static long clock_ns(long clk, int64_t *ns)
{
    uint64_t v;
    status_t st;
    switch (clk) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_TAI:
        *ns = wall_ns();
        return 0;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        *ns = (int64_t)now();
        return 0;
    case CLOCK_PROCESS_CPUTIME_ID:
        st = kp_cpu_ns(lx_self_process(), HANDLE_INVALID, &v);
        break;
    case CLOCK_THREAD_CPUTIME_ID:
        st = kp_cpu_ns(HANDLE_INVALID, lx_self_thread(), &v);
        break;
    default:
        return -EINVAL;   /* another process's clock (clock_getcpuclockid), or none */
    }
    if (st != OK)
        return -EINVAL;
    *ns = (int64_t)v;
    return 0;
}

static void to_timespec(int64_t ns, struct timespec *ts)
{
    ts->tv_sec = ns / (int64_t)NS_PER_S;
    ts->tv_nsec = ns % (int64_t)NS_PER_S;
    if (ts->tv_nsec < 0) {
        ts->tv_sec--;
        ts->tv_nsec += NS_PER_S;
    }
}

long lx_clock_gettime(long clk, long out, long c, long d, long e, long f)
{
    int64_t ns;
    long r = clock_ns(clk, &ns);
    if (r)
        return r;
    to_timespec(ns, (struct timespec *)(uintptr_t)out);
    return 0;
}

long lx_clock_getres(long clk, long out, long c, long d, long e, long f)
{
    int64_t ns;
    long r = clock_ns(clk, &ns);
    if (r)
        return r;
    if (out)
        *(struct timespec *)(uintptr_t)out = (struct timespec){ 0, 1 };
    return 0;
}

long lx_clock_settime(long clk, long in, long c, long d, long e, long f)
{
    return clk >= 0 && clk <= CLOCK_MAX ? -EPERM : -EINVAL;
}

/* A timespec as ns; -EINVAL if it isn't one. Past INT64_MAX: INT64_MAX. */
static long timespec_ns(const struct timespec *ts, int64_t *ns)
{
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= (long)NS_PER_S)
        return -EINVAL;
    if (__builtin_mul_overflow(ts->tv_sec, (int64_t)NS_PER_S, ns) ||
        __builtin_add_overflow(*ns, ts->tv_nsec, ns))
        *ns = INT64_MAX;
    return 0;
}

/* t ns on clk's scale (absolute) as an uptime deadline. */
static uint64_t uptime_deadline(long clk, int64_t t)
{
    if (clk == CLOCK_REALTIME) {
        int64_t up = (int64_t)now(), wall = wall_ns();
        if (t - wall > INT64_MAX - up)
            return DEADLINE_NEVER;
        t = up + (t - wall);
    }
    return t < 0 ? 0 : (uint64_t)t;
}

long lx_clock_nanosleep(long clk, long flags, long req, long rem, long e, long f)
{
    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC && clk != CLOCK_BOOTTIME)
        return clk == CLOCK_PROCESS_CPUTIME_ID || clk == CLOCK_THREAD_CPUTIME_ID ? -ENOTSUP
                                                                              : -EINVAL;
    if (flags & ~TIMER_ABSTIME)
        return -EINVAL;
    int64_t ns;
    long r = timespec_ns((const struct timespec *)(uintptr_t)req, &ns);
    if (r)
        return r;
    uint64_t deadline;
    if (flags & TIMER_ABSTIME)
        deadline = uptime_deadline(clk == CLOCK_REALTIME ? clk : CLOCK_MONOTONIC, ns);
    else if (__builtin_add_overflow(now(), (uint64_t)ns, &deadline))
        deadline = DEADLINE_NEVER;
    status_t st = jam_sleep_until(deadline);
    if (st == OK)
        return 0;
    if (st == ERR_INTERRUPTED && rem && !(flags & TIMER_ABSTIME)) {
        uint64_t t = now();
        to_timespec(deadline > t ? (int64_t)(deadline - t) : 0, (struct timespec *)(uintptr_t)rem);
    }
    return lx_err(st);
}

long lx_nanosleep(long req, long rem, long c, long d, long e, long f)
{
    return lx_clock_nanosleep(CLOCK_MONOTONIC, 0, req, rem, 0, 0);
}
