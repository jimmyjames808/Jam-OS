/* Resource limits: getrlimit, setrlimit and prlimit64 over a table of this
 * process (its own: Linux's prlimit of another pid is ESRCH here, a
 * program names no other process's). The table starts as the usual
 * Linux defaults: an 8 MiB stack (the one a POSIX child gets), no core
 * files, 1024 descriptors, the rest unlimited. A limit is kept and
 * answered back; two are acted on, because libc and its tests rely on
 * them: RLIMIT_NOFILE (fd.c allocates no descriptor at or past it) and
 * RLIMIT_DATA (brk in mem.c grows no further than it). A child starts
 * with the defaults again: the hand-over message has no field for them.
 * As on Linux a limit may be lowered, and a hard limit only raised by a
 * privileged process, which no process here is. */
#include <errno.h>
#include <sys/resource.h>
#include "fd.h"
#include "layer.h"
#include "proc.h"

#define NLIMITS 16   /* Linux's RLIMIT_CPU .. RLIMIT_RTTIME */

struct limit {
    uint64_t cur, max;
};

#define INF UINT64_MAX   /* RLIM_INFINITY */

static struct limit limits[NLIMITS] = {
    [RLIMIT_CPU] = { INF, INF },
    [RLIMIT_FSIZE] = { INF, INF },
    [RLIMIT_DATA] = { INF, INF },
    [RLIMIT_STACK] = { 8ull << 20, INF },
    [RLIMIT_CORE] = { 0, INF },
    [RLIMIT_RSS] = { INF, INF },
    [RLIMIT_NPROC] = { INF, INF },
    [RLIMIT_NOFILE] = { FD_MAX, FD_MAX },
    [RLIMIT_MEMLOCK] = { 64 << 10, 64 << 10 },
    [RLIMIT_AS] = { INF, INF },
    [RLIMIT_LOCKS] = { INF, INF },
    [RLIMIT_SIGPENDING] = { INF, INF },
    [RLIMIT_MSGQUEUE] = { 819200, 819200 },
    [RLIMIT_NICE] = { 0, 0 },
    [RLIMIT_RTPRIO] = { 0, 0 },
    [RLIMIT_RTTIME] = { INF, INF },
};
static lock_t limits_lock;

uint64_t rlimit_cur(int resource)
{
    lock_take(&limits_lock);
    uint64_t v = limits[resource].cur;
    lock_give(&limits_lock);
    return v;
}

void rlimit_fork(int step)
{
    if (step == LX_FORK_TAKE)
        lock_take(&limits_lock);
    else if (step == LX_FORK_GIVE)
        lock_give(&limits_lock);
    else
        limits_lock = (lock_t){ 0 };   /* the child: the limits are ours */
}

/* Linux's struct rlimit64, which is also the layout of x86-64's rlimit:
 * set and get are the caller's (NULL: not given; getrlimit and setrlimit
 * pass theirs whatever they are, so NULL is EFAULT there, as on Linux). */
static long limit_call(long pid, long resource, long set, long get)
{
    if (pid && pid != lx_pid())
        return -ESRCH;
    if (resource < 0 || resource >= NLIMITS)
        return -EINVAL;
    struct limit want;
    if (set && uc_get(&want, (const void *)(uintptr_t)set, sizeof(want)))
        return -EFAULT;
    long r = 0;
    lock_take(&limits_lock);
    struct limit old = limits[resource];
    if (set) {
        if (want.cur > want.max)
            r = -EINVAL;
        else if (want.max > old.max)
            r = -EPERM;
        else
            limits[resource] = want;
    }
    lock_give(&limits_lock);
    if (!r && get)
        r = uc_put((void *)(uintptr_t)get, &old, sizeof(old));
    return r;
}

long lx_prlimit64(long pid, long resource, long set, long get, long e, long f)
{
    return limit_call(pid, resource, set, get);
}

long lx_getrlimit(long resource, long get, long c, long d, long e, long f)
{
    bool known = resource >= 0 && resource < NLIMITS;   /* Linux: EINVAL before EFAULT */
    return get || !known ? limit_call(0, resource, 0, get) : -EFAULT;
}

long lx_setrlimit(long resource, long set, long c, long d, long e, long f)
{
    bool known = resource >= 0 && resource < NLIMITS;
    return set || !known ? limit_call(0, resource, set, 0) : -EFAULT;
}
