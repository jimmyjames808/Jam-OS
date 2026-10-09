/* The one user (docs/M13-PLAN.md section 11): every process runs as `jam`,
 * uid and gid 1000, never root, so a program that drops or refuses
 * privileges behaves as it would for an ordinary user on Linux. Nothing in
 * Jam OS reads these ids: authority is in handles.
 *
 * The get calls answer 1000. The set calls succeed when every id they
 * name is 1000 (or -1, "leave it"), and are EPERM for any other id, as an
 * ordinary user's would be; setgroups is EPERM (it needs a privilege no
 * user has here). The supplementary groups are the user's own group.
 *
 * Nothing changes, so there is no state and no lock. musl's set calls go
 * through __synccall, which in a process with more than one thread uses a
 * signal to reach each thread: until signals exist, call them while the
 * process has one thread. */
#include <errno.h>
#include <sys/types.h>
#include "layer.h"

#define ONE_ID 1000   /* the one user's uid and its group's gid (M13 Q7) */

static bool ours(long id)
{
    return (uint32_t)id == ONE_ID || (uint32_t)id == (uint32_t)-1;
}

long lx_getuid(long a, long b, long c, long d, long e, long f)
{
    return ONE_ID;
}

long lx_geteuid(long a, long b, long c, long d, long e, long f)
{
    return ONE_ID;
}

long lx_getgid(long a, long b, long c, long d, long e, long f)
{
    return ONE_ID;
}

long lx_getegid(long a, long b, long c, long d, long e, long f)
{
    return ONE_ID;
}

/* getresuid and getresgid: the real, effective and saved ids. */
static long three_ids(long r, long e, long s)
{
    *(uid_t *)(uintptr_t)r = ONE_ID;
    *(uid_t *)(uintptr_t)e = ONE_ID;
    *(uid_t *)(uintptr_t)s = ONE_ID;
    return 0;
}

long lx_getresuid(long r, long e, long s, long d, long e2, long f)
{
    return three_ids(r, e, s);
}

long lx_getresgid(long r, long e, long s, long d, long e2, long f)
{
    return three_ids(r, e, s);
}

long lx_setuid(long id, long b, long c, long d, long e, long f)
{
    return (uint32_t)id == ONE_ID ? 0 : -EPERM;
}

long lx_setgid(long id, long b, long c, long d, long e, long f)
{
    return (uint32_t)id == ONE_ID ? 0 : -EPERM;
}

long lx_setreuid(long r, long e, long c, long d, long e2, long f)
{
    return ours(r) && ours(e) ? 0 : -EPERM;
}

long lx_setregid(long r, long e, long c, long d, long e2, long f)
{
    return ours(r) && ours(e) ? 0 : -EPERM;
}

long lx_setresuid(long r, long e, long s, long d, long e2, long f)
{
    return ours(r) && ours(e) && ours(s) ? 0 : -EPERM;
}

long lx_setresgid(long r, long e, long s, long d, long e2, long f)
{
    return ours(r) && ours(e) && ours(s) ? 0 : -EPERM;
}

/* setfsuid and setfsgid answer the previous id, whatever they were asked:
 * Linux's way of saying it didn't change (or did). */
long lx_setfsuid(long id, long b, long c, long d, long e, long f)
{
    return ONE_ID;
}

long lx_setfsgid(long id, long b, long c, long d, long e, long f)
{
    return ONE_ID;
}

long lx_getgroups(long size, long list, long c, long d, long e, long f)
{
    if (size < 0)
        return -EINVAL;
    if (size == 0)
        return 1;   /* how many there are */
    *(gid_t *)(uintptr_t)list = ONE_ID;
    return 1;
}

long lx_setgroups(long size, long list, long c, long d, long e, long f)
{
    return -EPERM;
}
