/* Thread priorities and scheduling policies, for the programs that set
 * them by Linux's calls (Chromium's threads: nice values for background
 * work, a real-time policy asked for audio and refused).
 *
 * setpriority and getpriority on a thread (PRIO_PROCESS with a thread id)
 * map Linux's nice, -20 (most urgent) to 19, onto the kernel's thread
 * priorities, 0 to THREAD_PRIO_USER_MAX with THREAD_PRIO_DEFAULT for nice
 * 0: nice 19 is priority 0, nice -20 is the user maximum. The mapping is
 * not one to one (20 nice values below zero are squeezed into 8 steps
 * above the default), so getpriority gives back the nice value of the
 * priority a thread has, which is what was set only for nice values the
 * steps hit. A process group or a user (PRIO_PGRP, PRIO_USER) names
 * threads of other processes, which a program cannot reach: ESRCH.
 *
 * The scheduling policy is always SCHED_OTHER with priority 0: the other
 * non-real-time policies are accepted and change nothing, the real-time
 * ones are EPERM (a program without the power to set them, as on Linux for
 * an ordinary user), and the calls say so with the errnos Linux gives.
 * getcpu gives CPU 0: a thread's CPU is not told to it.
 *
 * The ints of Linux's calls arrive as the low 32 bits of a long (a raw
 * syscall() of a C program leaves the rest undefined), so each handler
 * takes the int it means. */
#include <errno.h>
#include <sched.h>
#include <sys/resource.h>
#include "layer.h"
#include "threads.h"

#define NICE_MIN    (-20)
#define NICE_MAX    19
#define RT_PRIO_MIN 1    /* Linux's real-time priorities */
#define RT_PRIO_MAX 99

/* The kernel priority for a nice value (clamped to Linux's range). */
static int nice_to_prio(int nice)
{
    int n = nice < NICE_MIN ? NICE_MIN : nice > NICE_MAX ? NICE_MAX : nice;
    if (n >= 0)   /* nice 0..19 onto the default down to 0, rounded to nearest */
        return THREAD_PRIO_DEFAULT - (n * THREAD_PRIO_DEFAULT + NICE_MAX / 2) / NICE_MAX;
    int up = THREAD_PRIO_USER_MAX - THREAD_PRIO_DEFAULT;
    return THREAD_PRIO_DEFAULT + (-n * up - NICE_MIN / 2) / -NICE_MIN;
}

/* The nice value of a kernel priority. */
static int prio_to_nice(int prio)
{
    if (prio <= THREAD_PRIO_DEFAULT)
        return ((THREAD_PRIO_DEFAULT - prio) * NICE_MAX + THREAD_PRIO_DEFAULT / 2) /
               THREAD_PRIO_DEFAULT;
    int up = THREAD_PRIO_USER_MAX - THREAD_PRIO_DEFAULT;
    int steps = (prio > THREAD_PRIO_USER_MAX ? THREAD_PRIO_USER_MAX : prio) - THREAD_PRIO_DEFAULT;
    return -((steps * -NICE_MIN + up / 2) / up);
}

static status_t set_prio(handle_t t, void *arg)
{
    return jam_thread_set_priority(t, *(int *)arg);
}

static status_t get_prio(handle_t t, void *arg)
{
    struct thread_info info;
    status_t st = jam_thread_get_info(t, &info);
    if (st == OK)
        *(int *)arg = info.priority;
    return st;
}

static status_t exists(handle_t t, void *arg)
{
    return OK;
}

/* Only a thread of ours can be named by a priority call. */
static long prio_target_ok(int which)
{
    if (which < PRIO_PROCESS || which > PRIO_USER)
        return -EINVAL;
    if (which != PRIO_PROCESS) {
        lx_enosys_option("setpriority/getpriority target", which);
        return -ESRCH;
    }
    return 0;
}

long lx_setpriority(long which, long who, long nice, long d, long e, long f)
{
    long r = prio_target_ok((int)which);
    if (r)
        return r;
    int prio = nice_to_prio((int)nice);
    return thread_apply((uint32_t)who, set_prio, &prio);
}

/* The system call's value is 20 - nice, so it is never negative; musl
 * turns it back. */
long lx_getpriority(long which, long who, long c, long d, long e, long f)
{
    long r = prio_target_ok((int)which);
    if (r)
        return r;
    int prio = 0;
    r = thread_apply((uint32_t)who, get_prio, &prio);
    return r ? r : 20 - prio_to_nice(prio);
}

long lx_getcpu(long cpu, long node, long cache, long d, long e, long f)
{
    if (cpu)
        *(unsigned *)(uintptr_t)cpu = 0;
    if (node)
        *(unsigned *)(uintptr_t)node = 0;
    return 0;
}

/* ---- policies --------------------------------------------------------------------- */

long lx_sched_getscheduler(long pid_l, long b, long c, long d, long e, long f)
{
    int pid = (int)pid_l;
    if (pid < 0)
        return -EINVAL;
    long r = thread_apply(pid, exists, NULL);
    return r ? r : SCHED_OTHER;
}

long lx_sched_getparam(long pid_l, long param, long c, long d, long e, long f)
{
    int pid = (int)pid_l;
    if (pid < 0 || !param)
        return -EINVAL;
    long r = thread_apply(pid, exists, NULL);
    if (!r)
        *(int *)(uintptr_t)param = 0;   /* Linux's sched_param is this one int */
    return r;
}

long lx_sched_setparam(long pid_l, long param, long c, long d, long e, long f)
{
    int pid = (int)pid_l;
    if (pid < 0 || !param)
        return -EINVAL;
    if (*(const int *)(uintptr_t)param != 0)
        return -EINVAL;   /* SCHED_OTHER has priority 0 only */
    return thread_apply(pid, exists, NULL);
}

long lx_sched_setscheduler(long pid_l, long policy, long param, long d, long e, long f)
{
    int pid = (int)pid_l, pol = (int)policy;
    if (pid < 0 || pol < 0 || !param)
        return -EINVAL;
    pol &= ~SCHED_RESET_ON_FORK;
    int prio = *(const int *)(uintptr_t)param;
    bool rt = pol == SCHED_FIFO || pol == SCHED_RR;
    if (!rt && pol != SCHED_OTHER && pol != SCHED_BATCH && pol != SCHED_IDLE)
        return -EINVAL;
    if (rt ? prio < RT_PRIO_MIN || prio > RT_PRIO_MAX : prio != 0)
        return -EINVAL;
    long r = thread_apply(pid, exists, NULL);
    if (r || !rt)
        return r;
    lx_enosys_option("sched_setscheduler real-time policy", pol);
    return -EPERM;
}

long lx_sched_get_priority_max(long policy, long b, long c, long d, long e, long f)
{
    switch ((int)policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        return RT_PRIO_MAX;
    case SCHED_OTHER:
    case SCHED_BATCH:
    case SCHED_IDLE:
        return 0;
    }
    return -EINVAL;
}

long lx_sched_get_priority_min(long policy, long b, long c, long d, long e, long f)
{
    switch ((int)policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        return RT_PRIO_MIN;
    case SCHED_OTHER:
    case SCHED_BATCH:
    case SCHED_IDLE:
        return 0;
    }
    return -EINVAL;
}
