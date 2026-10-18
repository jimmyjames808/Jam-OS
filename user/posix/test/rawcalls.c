/* posixtest's raw section: the calls Chromium and its libraries make by
 * their Linux numbers (syscall() or musl's thin wrappers), each answered
 * or failing the way Chromium tolerates, and the ENOSYS log for the ones
 * that are not there (rawlog.c, which runs first).
 *
 *   - gettid: the first thread's id is the pid, another's differs from
 *     it and from the parent's pid, each is the one tgkill takes (a
 *     SIGUSR2 to each runs on that thread) and /proc/self/task names,
 *     and stays the same;
 *   - getrandom: bytes that differ, zero length, the flags;
 *   - sched_getaffinity and sched_setaffinity: every CPU in the mask, a
 *     mask that names no CPU refused, a thread of ours and one that is
 *     not;
 *   - prctl: a thread's name (its own, cut at 15 bytes), dumpable, no new
 *     privileges (never cleared), the parent-death signal, timer slack,
 *     the anonymous-memory name, seccomp, and an unknown option refused;
 *   - setpriority and getpriority on this thread, on another and on a
 *     number that is no thread, checked against the kernel's priority;
 *   - the scheduling policies (raw, since musl stubs its wrappers): always
 *     SCHED_OTHER, real-time refused, the priority ranges;
 *   - getcpu, membarrier and the robust list, and that none of the last
 *     two is a line in the ENOSYS log.
 */
#include <stdio.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../sys/calls.h"
#include "pt.h"

#ifndef PR_SET_VMA
#define PR_SET_VMA           0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif
#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#define PR_SET_SECCOMP_NR   22   /* not in musl's header */
#define NO_SUCH_TID         0x7ffffff0
#define PRIO_DEFAULT        16    /* the kernel's THREAD_PRIO_DEFAULT (<jam/abi.h>) */

/* The kernel's priority of the calling thread (the first thread). */
static int kernel_prio(void)
{
    struct thread_info ti;
    CHECK(jam_thread_get_info(startup_handle(SR_SELF_THREAD), &ti) == OK);
    return ti.priority;
}

/* A thread that runs fn(arg) when released by a byte on the pipe, and its
 * results. */
struct helper {
    pthread_t t;
    int       pipe[2];       /* the thread reads one byte from [0] to go on */
    volatile int up;         /* set when it is waiting */
    long      tid;           /* its gettid */
    long      name_before;   /* PR_GET_NAME's result and what it gave */
    char      name[16];
    int       nice;          /* its getpriority(PRIO_PROCESS, 0) after the main thread set it */
};

static void *helper_main(void *arg)
{
    struct helper *h = arg;
    h->tid = syscall(SYS_gettid);
    h->name_before = prctl(PR_GET_NAME, h->name);
    prctl(PR_SET_NAME, "helper-thread-1");
    __atomic_store_n(&h->up, 1, __ATOMIC_RELEASE);
    char c;
    CHECK(read(h->pipe[0], &c, 1) == 1);
    errno = 0;
    h->nice = getpriority(PRIO_PROCESS, 0);
    return NULL;
}

static void helper_start(struct helper *h)
{
    memset(h, 0, sizeof(*h));
    CHECK(pipe(h->pipe) == 0);
    CHECK(pthread_create(&h->t, NULL, helper_main, h) == 0);
    while (!__atomic_load_n(&h->up, __ATOMIC_ACQUIRE))
        sched_yield();
}

static void helper_end(struct helper *h)
{
    CHECK(write(h->pipe[1], "x", 1) == 1);
    CHECK(pthread_join(h->t, NULL) == 0);
    close(h->pipe[0]);
    close(h->pipe[1]);
}

/* ---- gettid, getrandom ------------------------------------------------------------- */

static volatile long usr2_tid;   /* gettid in the last SIGUSR2's handler */

static void on_usr2(int sig)
{
    (void)sig;
    usr2_tid = syscall(SYS_gettid);
}

/* The thread with id tid ran SIGUSR2's handler within a few seconds. */
static bool usr2_ran_on(long tid)
{
    for (int i = 0; i < 2000 && usr2_tid != tid; i++)
        usleep(1000);
    return usr2_tid == tid;
}

static void check_tid(void)
{
    long me = syscall(SYS_gettid);
    CHECK(me > 0);
    CHECK(syscall(SYS_gettid) == me);
    CHECK(me == getpid());   /* the first thread's id is the pid, as on Linux */
    struct helper h;
    helper_start(&h);
    CHECK(h.tid > 0 && h.tid != me);
    CHECK(h.tid != getpid() && h.tid != getppid());   /* another thread's is no pid */
    /* The id is the one the signal calls take: a thread of ours, by the pid. */
    CHECK(syscall(SYS_tgkill, getpid(), me, 0) == 0);
    CHECK(syscall(SYS_tgkill, getpid(), h.tid, 0) == 0);
    struct sigaction sa = { .sa_handler = on_usr2, .sa_flags = SA_RESTART }, old;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR2, &sa, &old) == 0);
    usr2_tid = 0;
    CHECK(syscall(SYS_tgkill, getpid(), getpid(), SIGUSR2) == 0 && usr2_ran_on(me));
    usr2_tid = 0;
    CHECK(syscall(SYS_tgkill, getpid(), h.tid, SIGUSR2) == 0 && usr2_ran_on(h.tid));
    CHECK(sigaction(SIGUSR2, &old, NULL) == 0);
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d", (int)getpid());
    CHECK(access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "/proc/self/task/%ld", h.tid);
    CHECK(access(path, F_OK) == 0);
    helper_end(&h);
}

static void check_random(void)
{
    unsigned char a[64] = { 0 }, b[64] = { 0 }, zero[64] = { 0 };
    CHECK(syscall(SYS_getrandom, a, sizeof(a), 0) == 64);
    CHECK(syscall(SYS_getrandom, b, sizeof(b), 0) == 64);
    CHECK(memcmp(a, b, sizeof(a)) != 0 && memcmp(a, zero, sizeof(a)) != 0);
    CHECK(syscall(SYS_getrandom, NULL, 0, 1 /* GRND_NONBLOCK */) == 0);   /* Chromium's probe */
    CHECK(syscall(SYS_getrandom, a, 8, 2 /* GRND_RANDOM */ | 1) == 8);
    errno = 0;
    CHECK(syscall(SYS_getrandom, a, 8, 0x80) == -1 && errno == EINVAL);
}

/* ---- affinity -------------------------------------------------------------------- */

static void check_affinity(void)
{
    cpu_set_t set, one, none, far;
    CPU_ZERO(&set);
    CHECK(sched_getaffinity(0, sizeof(set), &set) == 0);
    int cpus = CPU_COUNT(&set);
    CHECK(cpus >= 1 && CPU_ISSET(0, &set));
    CHECK(cpus == sysconf(_SC_NPROCESSORS_ONLN));
    CHECK(sched_setaffinity(0, sizeof(set), &set) == 0);
    CPU_ZERO(&one);
    CPU_SET(0, &one);
    CHECK(sched_setaffinity(0, sizeof(one), &one) == 0);
    CHECK(sched_setaffinity(getpid(), sizeof(one), &one) == 0);
    CPU_ZERO(&set);
    CHECK(sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) == cpus);   /* accepted, no effect */

    CPU_ZERO(&none);
    errno = 0;
    CHECK(sched_setaffinity(0, sizeof(none), &none) == -1 && errno == EINVAL);
    CPU_ZERO(&far);
    CPU_SET(1000, &far);   /* a CPU there is not */
    errno = 0;
    CHECK(sched_setaffinity(0, sizeof(far), &far) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(sched_setaffinity(NO_SUCH_TID, sizeof(one), &one) == -1 && errno == ESRCH);
    errno = 0;
    CHECK(sched_getaffinity(NO_SUCH_TID, sizeof(set), &set) == -1 && errno == ESRCH);
    CHECK(pthread_setaffinity_np(pthread_self(), sizeof(one), &one) == 0);

    struct helper h;
    helper_start(&h);
    CHECK(pthread_setaffinity_np(h.t, sizeof(one), &one) == 0);   /* another thread of ours */
    CHECK(pthread_getaffinity_np(h.t, sizeof(set), &set) == 0 && CPU_COUNT(&set) == cpus);
    helper_end(&h);
}

/* ---- prctl --------------------------------------------------------------------------- */

static void check_names(void)
{
    char name[16], keep[16];
    memset(name, 'x', sizeof(name));
    CHECK(prctl(PR_GET_NAME, name) == 0);
    CHECK(program_invocation_short_name && !strncmp(name, program_invocation_short_name, 15));
    CHECK(prctl(PR_SET_NAME, "chromium-main") == 0);
    CHECK(prctl(PR_GET_NAME, name) == 0 && !strcmp(name, "chromium-main"));
    CHECK(prctl(PR_SET_NAME, "0123456789abcdefghij") == 0);
    memset(name, 'x', sizeof(name));
    CHECK(prctl(PR_GET_NAME, name) == 0 && !strcmp(name, "0123456789abcde"));   /* 15 and a NUL */
    memcpy(keep, name, sizeof(keep));

    struct helper h;
    helper_start(&h);   /* sets its own name */
    CHECK(h.name_before == 0);
    CHECK(prctl(PR_GET_NAME, name) == 0 && !memcmp(name, keep, sizeof(name)));   /* ours unchanged */
    helper_end(&h);

    CHECK(pthread_setname_np(pthread_self(), "viaMusl") == 0);   /* musl's route to the same call */
    CHECK(pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 && !strcmp(name, "viaMusl"));
    errno = 0;
    CHECK(prctl(PR_SET_NAME, 0UL) == -1 && errno == EFAULT);
    prctl(PR_SET_NAME, program_invocation_short_name);
}

static void check_settings(void)
{
    CHECK(prctl(PR_GET_DUMPABLE) == 1);
    CHECK(prctl(PR_SET_DUMPABLE, 0) == 0 && prctl(PR_GET_DUMPABLE) == 0);
    CHECK(prctl(PR_SET_DUMPABLE, 1) == 0 && prctl(PR_GET_DUMPABLE) == 1);
    errno = 0;
    CHECK(prctl(PR_SET_DUMPABLE, 2) == -1 && errno == EINVAL);

    int nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    CHECK(nnp == 0 || nnp == 1);
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
    errno = 0;
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) == -1 && errno == EINVAL);   /* never cleared */
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);

    int sig = -1;
    CHECK(prctl(PR_SET_PDEATHSIG, SIGTERM) == 0);
    CHECK(prctl(PR_GET_PDEATHSIG, &sig) == 0 && sig == SIGTERM);
    errno = 0;
    CHECK(prctl(PR_SET_PDEATHSIG, 65) == -1 && errno == EINVAL);
    CHECK(prctl(PR_SET_PDEATHSIG, 0) == 0);
    CHECK(prctl(PR_GET_PDEATHSIG, &sig) == 0 && sig == 0);
    errno = 0;
    CHECK(prctl(PR_GET_PDEATHSIG, 0UL) == -1 && errno == EFAULT);

    CHECK(prctl(PR_GET_TIMERSLACK) == 50000);
    CHECK(prctl(PR_SET_TIMERSLACK, 1000) == 0 && prctl(PR_GET_TIMERSLACK) == 1000);
    CHECK(prctl(PR_SET_TIMERSLACK, 0) == 0 && prctl(PR_GET_TIMERSLACK) == 50000);   /* 0: the default */
}

static void check_refused(void)
{
    char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    CHECK(prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, page, 4096, "partition_alloc") == 0);
    errno = 0;
    CHECK(prctl(PR_SET_VMA, 1, page, 4096, "x") == -1 && errno == EINVAL);
    munmap(page, 4096);
    CHECK(prctl(PR_GET_SECCOMP) == 0);
    errno = 0;
    CHECK(prctl(PR_SET_SECCOMP_NR, 2, 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(prctl(PR_SET_PTRACER, 1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(prctl(0x7e57) == -1 && errno == EINVAL);

    /* Each refusal is one line of the log, once. */
    lx_enosys_set(1);
    unsigned n0 = lx_enosys_lines();
    prctl(0x7e57);
    prctl(0x7e57);
    CHECK(lx_enosys_lines() == n0 + 1);
    char last[256];
    lx_enosys_last(last, sizeof(last));
    CHECK(strstr(last, ": prctl option 32343\n"));
    prctl(PR_SET_NAME, "a-known-option");   /* answered options are no gap */
    CHECK(lx_enosys_lines() == n0 + 1);
    lx_enosys_set(0);
}

/* ---- priorities -------------------------------------------------------------------- */

static int nice_of(long who)
{
    errno = 0;
    int v = getpriority(PRIO_PROCESS, (id_t)who);
    CHECK(errno == 0);
    return v;
}

static void check_priority(void)
{
    int orig = kernel_prio();
    if (orig == PRIO_DEFAULT)
        CHECK(nice_of(0) == 0);
    /* The nice values the 25 kernel steps hit exactly; each with its priority. */
    static const struct { int nice, prio; } steps[] = {
        { 10, 8 }, { 19, 0 }, { -5, 18 }, { -20, 24 }, { 5, 12 }, { 0, PRIO_DEFAULT },
    };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        CHECK(setpriority(PRIO_PROCESS, 0, steps[i].nice) == 0);
        CHECK(kernel_prio() == steps[i].prio);
        CHECK(nice_of(0) == steps[i].nice);
    }
    CHECK(setpriority(PRIO_PROCESS, 0, 100) == 0 && kernel_prio() == 0);   /* clamped to 19 */
    CHECK(nice_of(0) == 19);
    CHECK(setpriority(PRIO_PROCESS, 0, -100) == 0 && kernel_prio() == 24);   /* and to -20 */
    CHECK(setpriority(PRIO_PROCESS, 0, 0) == 0);
    CHECK(setpriority(PRIO_PROCESS, syscall(SYS_gettid), 10) == 0 && kernel_prio() == 8);
    CHECK(nice_of(syscall(SYS_gettid)) == 10);
    CHECK(setpriority(PRIO_PROCESS, getpid(), 5) == 0);   /* the process: its first thread */
    CHECK(kernel_prio() == 12 && nice_of(getpid()) == 5);
    CHECK(setpriority(PRIO_PROCESS, 0, 0) == 0);

    /* Another thread of ours: its own priority, not ours. */
    struct helper h;
    helper_start(&h);
    CHECK(setpriority(PRIO_PROCESS, h.tid, 10) == 0);
    CHECK(nice_of(h.tid) == 10);
    CHECK(kernel_prio() == PRIO_DEFAULT);   /* ours is as it was */
    CHECK(nice_of(0) == 0);
    helper_end(&h);
    CHECK(h.nice == 10);   /* the thread read its own back as getpriority(.., 0) */

    errno = 0;
    CHECK(setpriority(PRIO_PROCESS, NO_SUCH_TID, 5) == -1 && errno == ESRCH);
    errno = 0;
    CHECK(getpriority(PRIO_PROCESS, NO_SUCH_TID) == -1 && errno == ESRCH);
    errno = 0;
    CHECK(setpriority(3, 0, 5) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(setpriority(PRIO_PGRP, 0, 5) == -1 && errno == ESRCH);   /* other processes' threads */
    errno = 0;
    CHECK(setpriority(PRIO_USER, 0, 5) == -1 && errno == ESRCH);
    CHECK(kernel_prio() == PRIO_DEFAULT);
    jam_thread_set_priority(startup_handle(SR_SELF_THREAD), orig);   /* as the test found it */
}

/* ---- scheduling policies (raw: musl's own wrappers are stubs) ----------------------- */

static void check_policies(void)
{
    struct sched_param sp = { .sched_priority = 5 };
    CHECK(syscall(SYS_sched_getscheduler, 0) == SCHED_OTHER);
    CHECK(syscall(SYS_sched_getparam, 0, &sp) == 0 && sp.sched_priority == 0);
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &sp) == 0);
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_BATCH, &sp) == 0);   /* accepted, no change */
    CHECK(syscall(SYS_sched_getscheduler, 0) == SCHED_OTHER);
    CHECK(syscall(SYS_sched_setparam, 0, &sp) == 0);

    sp.sched_priority = 1;
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_RR, &sp) == -1 && errno == EPERM);   /* needs the power */
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp) == -1 && errno == EPERM);
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &sp) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(syscall(SYS_sched_setparam, 0, &sp) == -1 && errno == EINVAL);
    sp.sched_priority = 100;
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp) == -1 && errno == EINVAL);
    sp.sched_priority = 0;
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_RR, &sp) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, 77, &sp) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, NULL) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, NO_SUCH_TID, SCHED_OTHER, &sp) == -1 && errno == ESRCH);
    errno = 0;
    CHECK(syscall(SYS_sched_getscheduler, NO_SUCH_TID) == -1 && errno == ESRCH);
    errno = 0;
    CHECK(syscall(SYS_sched_getscheduler, -1) == -1 && errno == EINVAL);

    CHECK(sched_get_priority_max(SCHED_FIFO) == 99 && sched_get_priority_min(SCHED_FIFO) == 1);
    CHECK(sched_get_priority_max(SCHED_RR) == 99 && sched_get_priority_min(SCHED_RR) == 1);
    CHECK(sched_get_priority_max(SCHED_OTHER) == 0 && sched_get_priority_min(SCHED_OTHER) == 0);
    CHECK(sched_get_priority_max(SCHED_IDLE) == 0);
    errno = 0;
    CHECK(sched_get_priority_max(77) == -1 && errno == EINVAL);

    /* musl's pthread calls use them for a thread. */
    int policy = -1;
    CHECK(pthread_getschedparam(pthread_self(), &policy, &sp) == 0);
    CHECK(policy == SCHED_OTHER && sp.sched_priority == 0);
    CHECK(pthread_setschedprio(pthread_self(), 0) == 0);
    sp.sched_priority = 1;
    CHECK(pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == EPERM);
}

static void check_getcpu(void)
{
    unsigned cpu = 99, node = 99;
    CHECK(syscall(SYS_getcpu, &cpu, &node, NULL) == 0 && cpu == 0 && node == 0);
    CHECK(syscall(SYS_getcpu, NULL, NULL, NULL) == 0);
    CHECK(sched_getcpu() == 0);
}

/* ---- calls that are meant to fail --------------------------------------------------- */

static void *quiet_thread(void *arg)
{
    return arg;
}

static void check_meant_to_fail(void)
{
    lx_enosys_set(1);
    unsigned n0 = lx_enosys_lines();
    errno = 0;
    CHECK(syscall(SYS_membarrier, 0 /* QUERY */, 0, 0) == -1 && errno == ENOSYS);
    errno = 0;
    CHECK(syscall(SYS_membarrier, 8 /* PRIVATE_EXPEDITED */, 0, 0) == -1 && errno == ENOSYS);
    errno = 0;
    CHECK(syscall(SYS_set_robust_list, 0, 24) == -1 && errno == ENOSYS);
    errno = 0;
    CHECK(syscall(SYS_get_robust_list, 0, 0, 0) == -1 && errno == ENOSYS);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, quiet_thread, NULL) == 0);   /* musl sets a robust list */
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(lx_enosys_lines() == n0);   /* the answers are on purpose: not gaps */
    lx_enosys_set(0);
}

void pt_raw(char **argv)
{
    (void)argv;
    pt_raw_log();
    pt_raw_futex();
    check_tid();
    check_random();
    check_affinity();
    check_names();
    check_settings();
    check_refused();
    check_priority();
    check_policies();
    check_getcpu();
    check_meant_to_fail();
}
