/* Clocks and sleeps (docs/M13-PLAN.md section 10): CLOCK_MONOTONIC and
 * CLOCK_BOOTTIME (and their raw and coarse kinds) are the uptime, clock_get's;
 * CLOCK_REALTIME is the wall clock, wallclock_get's UTC (with no wall
 * clock, the uptime counted from 1970); the CPU-time clocks are the
 * kernel's per-process and per-thread counts (kposix.c). Resolution 1 ns.
 *
 * The uptime and the wall clock are read without a system call, from the
 * kernel's time page (struct time_page, <jam/abi.h>): mapped read-only
 * at the first read (clock_page, then vmar_map with VMAR_KEPT_ONLY, so
 * no read of it ever faults) and kept for the process's life. The TSC,
 * read with lfence first, gives exactly clock_get's uptime
 * (time_page_uptime); the wall clock's offset is read under the page's
 * sequence word. A process the page can't be mapped into (the call fails,
 * its job can't pay for the page table, or the page says its TSC fields
 * are not the kernel's clock) reads both with the calls, for good: the
 * same values, a system call each.
 *
 * nanosleep and clock_nanosleep sleep with sleep_until on the uptime: an
 * absolute CLOCK_REALTIME time is turned into an uptime deadline at the
 * call (a wall-clock change during the sleep doesn't move it). A sleep
 * ended by thread_interrupt is EINTR, with the time left. Setting a
 * clock is EPERM: the clock's power is init's and sntp's.
 *
 * times and getrusage give the same CPU time as the CPU-time clocks:
 * ours, a thread's, and that of the children a wait has reaped (proc.c). */
#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "layer.h"
#include "proc.h"

#define CLOCK_MAX 11   /* CLOCK_TAI, the highest clock id Linux has */

/* The page once mapped; PAGE_NONE once it couldn't be (both for good). */
#define PAGE_NONE ((const struct time_page *)1)
static const struct time_page *page;

/* Map the page, or say why not (PAGE_NONE). */
static const struct time_page *page_map(void)
{
    handle_t h;
    uint64_t addr = 0;
    if (jam_clock_page(&h) != OK)
        return PAGE_NONE;
    status_t st = jam_vmar_map(lx_self_vmar(), h, 0, PAGE_SIZE, VMAR_READ | VMAR_KEPT_ONLY, &addr);
    jam_handle_close(h);   /* the mapping keeps the page */
    if (st != OK)
        return PAGE_NONE;
    const struct time_page *p = (const struct time_page *)(uintptr_t)addr;
    if (p->flags & TIME_PAGE_TSC)
        return p;
    (void)jam_vmar_unmap(lx_self_vmar(), addr, PAGE_SIZE);   /* ours, just made */
    return PAGE_NONE;
}

/* The time page, mapped at the first call; NULL: use the calls. Two
 * threads that map it at once keep the first one's mapping. */
static const struct time_page *time_page(void)
{
    const struct time_page *p = __atomic_load_n(&page, __ATOMIC_ACQUIRE);
    if (p)
        return p == PAGE_NONE ? NULL : p;
    const struct time_page *mine = page_map(), *was = NULL;
    if (!__atomic_compare_exchange_n(&page, &was, mine, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        if (mine != PAGE_NONE)
            (void)jam_vmar_unmap(lx_self_vmar(), (uint64_t)(uintptr_t)mine, PAGE_SIZE);
        mine = was;
    }
    return mine == PAGE_NONE ? NULL : mine;
}

/* The TSC, read after every instruction before it has finished (lfence),
 * so a clock read never comes out earlier than a read before it. */
static inline uint64_t tsc_ordered(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}

/* The uptime now, in ns: clock_get's. */
static uint64_t up_ns(void)
{
    const struct time_page *p = time_page();
    return p ? time_page_uptime(p, tsc_ordered()) : now();
}

/* The wall clock now, in ns since 1970 UTC. */
static int64_t wall_ns(void)
{
    const struct time_page *p = time_page();
    if (p) {
        uint32_t s, flags;
        int64_t off;
        do {
            s = time_page_begin(p);
            off = __atomic_load_n(&p->wall_offset, __ATOMIC_RELAXED);
            flags = __atomic_load_n(&p->wall_flags, __ATOMIC_RELAXED);
        } while (time_page_retry(p, s));
        int64_t up = (int64_t)time_page_uptime(p, tsc_ordered());
        return flags ? up + off : up;
    }
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
        *ns = (int64_t)up_ns();
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
        int64_t up = (int64_t)up_ns(), wall = wall_ns();
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
    else if (__builtin_add_overflow(up_ns(), (uint64_t)ns, &deadline))
        deadline = DEADLINE_NEVER;
    status_t st = jam_sleep_until(deadline);
    if (st == OK)
        return 0;
    if (st == ERR_INTERRUPTED && rem && !(flags & TIMER_ABSTIME)) {
        uint64_t t = up_ns();
        to_timespec(deadline > t ? (int64_t)(deadline - t) : 0, (struct timespec *)(uintptr_t)rem);
    }
    return lx_err(st);
}

long lx_nanosleep(long req, long rem, long c, long d, long e, long f)
{
    return lx_clock_nanosleep(CLOCK_MONOTONIC, 0, req, rem, 0, 0);
}

/* ---- CPU time: times and getrusage ---------------------------------------------------- */

#define CLK_TCK 100   /* times' clock ticks a second: musl's sysconf(_SC_CLK_TCK) */

/* Linux's struct tms, clock_t each. */
struct lx_tms {
    long utime, stime, cutime, cstime;
};

static long ns_to_ticks(uint64_t ns)
{
    return (long)(ns / (NS_PER_S / CLK_TCK));
}

/* The kernel counts a thread's time in its own calls with its user time
 * (K1's cpu_ns), so the system times are 0 and the user times the whole. */
long lx_times(long out, long b, long c, long d, long e, long f)
{
    uint64_t self = 0;
    (void)kp_cpu_ns(lx_self_process(), HANDLE_INVALID, &self);   /* our own process: can't fail */
    if (out)
        *(struct lx_tms *)(uintptr_t)out = (struct lx_tms){
            .utime = ns_to_ticks(self), .cutime = ns_to_ticks(lx_children_cpu_ns()),
        };
    return ns_to_ticks(up_ns());   /* ticks since boot: only differences mean anything */
}

long lx_getrusage(long who, long out, long c, long d, long e, long f)
{
    uint64_t ns = 0;
    status_t st = OK;
    if (who == RUSAGE_SELF)
        st = kp_cpu_ns(lx_self_process(), HANDLE_INVALID, &ns);
    else if (who == RUSAGE_THREAD)
        st = kp_cpu_ns(HANDLE_INVALID, lx_self_thread(), &ns);
    else if (who == RUSAGE_CHILDREN)
        ns = lx_children_cpu_ns();
    else
        return -EINVAL;
    if (st != OK)
        return lx_err(st);
    struct rusage *ru = (struct rusage *)(uintptr_t)out;
    memset(ru, 0, sizeof(*ru));
    ru->ru_utime.tv_sec = (time_t)(ns / NS_PER_S);
    ru->ru_utime.tv_usec = (suseconds_t)(ns % NS_PER_S / 1000);
    return 0;
}
