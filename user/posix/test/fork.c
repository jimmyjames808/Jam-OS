/* posixtest's fork section (docs/FORK-PLAN.md, track FL1b): fork, vfork
 * and clone without CLONE_VM. Each case runs in a child of ours
 * ("posixtest --child-fork <case>", started with libjam's spawn) in a job
 * of its own, which forks, checks both sides and waits for its children;
 * the job then holds nothing (every child gone, nothing charged or held).
 * A forked child's descriptors aren't its own until FL2, so it reports
 * through a MAP_SHARED page (the line of the first check that failed, in
 * the kernel log too) and its exit code, never by printing.
 *
 *   twice: fork returns twice (0 in the child, its pid in the parent); the
 *     ids (getpid, getppid, the group and session; getpgid and
 *     /proc/<pid>/stat of the child from the parent; the child's own table
 *     of children empty); memory separate both ways (data, bss, heap,
 *     stack, a private mapping) and a MAP_SHARED anonymous page shared;
 *     waitpid's WNOHANG while it lives, its exit code, ECHILD after.
 *   threads: threads in malloc and in every call whose lock fork holds
 *     (mmap, sigprocmask, getpid, open and close, umask, getcwd,
 *     getrlimit, kill), and threads starting and ending, while we fork 16
 *     times with pthread_atfork handlers: each child ran the child handler,
 *     and malloc, a new thread, mmap and the signal calls work there; the
 *     parent ran prepare and parent 16 times each.
 *   torn: writers keeping A >= B >= A - 1 across two mappings (two VMOs,
 *     A written first) while we fork 20 times: no child sees it broken.
 *   huge: a 1 TiB reservation with a page in every GiB forked within a
 *     second, charged at most our pages again (plus the tables), the child
 *     reading those pages and its write not ours.
 *   nomem: in a job of 32 MiB, three quarters of what is left committed:
 *     fork is ENOMEM, twice, with nothing left charged; once that memory
 *     is unmapped, fork works.
 *   signals: SIGCHLD with the child's pid and status; kill and kill(-pgid)
 *     reaching the child's handler (the dispositions and the mask
 *     inherited, its signal thread its own); SIGKILL (WTERMSIG); a signal
 *     pending in the parent not pending in the child; alarm and a POSIX
 *     timer gone in the child, ours kept; a SIGSEGV handler run in the
 *     child on its sigaltstack (the exception entry and the kernel's
 *     exception stack set again).
 *   clone: syscall(SYS_clone, SIGCHLD, ...) with CLONE_PARENT_SETTID and
 *     CLONE_CHILD_SETTID (the child's word its pid: its first thread's id),
 *     and its refusals (CLONE_VM alone, a new stack, an unknown flag);
 *     CLONE_FS, raw and by clone(), as a copy of the directory and umask
 *     (the child's chdir and umask its own); a fork from a thread that
 *     isn't the first (the child's id its pid); clone(fn, stack, SIGCHLD,
 *     arg) ending with fn's value; vfork (its wait: forknote.c's vfork
 *     case).
 *   small: clone(fn, two pages, SIGCHLD), as gtest's death tests make it
 *     (the parent unmaps the pages at once): fn's frames on those pages,
 *     the calls that take a path (stat, lstat, access, open, mkdir and
 *     rmdir, chdir and getcwd, readlink) working there, a fork and a clone
 *     of its own reaped, and fn's value its exit code.
 * Then forkfd.c's cases (FL2) and forknote.c's (FL4: vfork, groups, junk). */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define PG        4096ul
#define RW        (PROT_READ | PROT_WRITE)
#define ANON      (MAP_PRIVATE | MAP_ANONYMOUS)
#define CASE_NS   (60 * NS_PER_S)    /* a case's whole run */
#define KID_NS    (10 * NS_PER_S)    /* a child's wait for its parent's word */
#define GIB       (1ull << 30)
#define TIB       (1ull << 40)
#define WRITERS   4
#define HAMMERS   4
#define JOB_PAGES 8192               /* nomem's job: 32 MiB */
#define ALT_SIZE  (64u << 10)        /* the alternate signal stack */

/* What forked children report: a MAP_SHARED page. */
struct report {
    volatile int     line;    /* the first check that failed in a child, or 0 */
    volatile int     go;      /* the parent's word to its child */
    volatile int64_t v[8];    /* what a child passes back */
};
static struct report *rep;

static void map_report(void)
{
    rep = mmap(NULL, PG, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(rep != MAP_FAILED);
    if (rep == MAP_FAILED)
        exit(1);
}

/* A check in a forked child: the first failure's line in the report and
 * the log (its descriptors are the parent's until FL2). */
static void kid_failed(int line)
{
    if (!rep->line)
        rep->line = line;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "posixtest: fork: a child FAILED line %d\n", line);
    (void)jam_debug_write(buf, (size_t)n);
}
#define KID(c)                     \
    do {                           \
        if (!(c))                  \
            kid_failed(__LINE__);  \
    } while (0)

static _Noreturn void kid_end(int code)
{
    _exit(rep->line ? 1 : code);
}

/* Spin until *word is set or the time is up. */
static bool until_set(volatile int64_t *word, uint64_t ns)
{
    uint64_t end = now() + ns;
    while (!*word && now() < end)
        sched_yield();
    return *word != 0;
}

/* Reap pid: its wait status, or -1. */
static int reap(pid_t pid)
{
    int st = 0;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r == -1 && errno == EINTR);
    CHECK(r == pid);
    if (rep && rep->line)
        printf("posixtest: fork: a child failed at fork.c line %d\n", rep->line);
    return r == pid ? st : -1;
}

/* Reap pid, which should have exited with code. */
static void reaped(pid_t pid, int code)
{
    int st = reap(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == code);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != code)
        printf("posixtest: fork: wait status %#x, wanted exit %d\n", st, code);
}

/* ---- twice ------------------------------------------------------------------------ */

static int g_data = 0x1111;
static int g_bss;

static void kid_twice(pid_t parent, pid_t pgrp, pid_t sid, int *heap, volatile int *stack,
                      int *priv)
{
    KID(getpid() > 0 && getpid() != parent);
    KID(getppid() == parent);
    KID(getpgrp() == pgrp && getsid(0) == sid);
    KID(g_data == 0x1111 && g_bss == 0x2222 && *heap == 0x3333 && *stack == 0x4444 &&
        *priv == 0x5555);
    errno = 0;
    KID(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);   /* its table: empty */
    rep->v[0] = getpid();
    g_data = g_bss = *heap = *stack = *priv = 0x6666;
    rep->v[1] = 1;
    KID(until_set(&rep->v[2], KID_NS));   /* the parent has written its own */
    KID(g_data == 0x6666 && g_bss == 0x6666 && *heap == 0x6666 && *stack == 0x6666 &&
        *priv == 0x6666);
    kid_end(42);
}

/* /proc/<pid>/stat's parent pid, or -1. */
static int stat_ppid(pid_t pid)
{
    char path[48], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char *end = strrchr(buf, ')');
    int ppid;
    char state;
    return end && sscanf(end + 1, " %c %d", &state, &ppid) == 2 ? ppid : -1;
}

static void fork_twice(void)
{
    map_report();
    g_bss = 0x2222;
    int *heap = malloc(sizeof(int));
    volatile int stack = 0x4444;
    int *priv = mmap(NULL, PG, RW, ANON, -1, 0);
    CHECK(heap && priv != MAP_FAILED);
    if (!heap || priv == MAP_FAILED)
        return;
    *heap = 0x3333;
    *priv = 0x5555;
    pid_t self = getpid(), pgrp = getpgrp(), sid = getsid(0);
    pid_t pid = fork();
    if (pid == 0)
        kid_twice(self, pgrp, sid, heap, &stack, priv);
    CHECK(pid > 0);
    if (pid <= 0)
        return;
    CHECK(getpid() == self);
    CHECK(getpgid(pid) == pgrp && getsid(pid) == sid);
    CHECK(stat_ppid(pid) == self);
    CHECK(until_set(&rep->v[1], KID_NS));
    CHECK(waitpid(pid, NULL, WNOHANG) == 0);   /* it waits for our word */
    CHECK(g_data == 0x1111 && g_bss == 0x2222 && *heap == 0x3333 && stack == 0x4444 &&
          *priv == 0x5555);
    g_data = g_bss = *heap = stack = *priv = 0x7777;
    rep->v[2] = 1;
    reaped(pid, 42);
    CHECK(rep->v[0] == pid);
    errno = 0;
    CHECK(waitpid(pid, NULL, 0) == -1 && errno == ECHILD);
}

/* ---- threads ---------------------------------------------------------------------- */

static volatile int stop;
static volatile int prepared, parented, childed;

static void at_prepare(void) { prepared++; }
static void at_parent(void) { parented++; }
static void at_child(void) { childed++; }

/* A thread in malloc and the calls whose locks fork holds. */
static void *hammer(void *arg)
{
    unsigned n = (unsigned)(uintptr_t)arg;
    while (!stop) {
        n = n * 1103515245u + 12345u;
        void *p = malloc(16 + (n >> 8) % 70000);
        void *m = mmap(NULL, PG, RW, ANON, -1, 0);
        sigset_t old;
        sigprocmask(SIG_BLOCK, NULL, &old);
        (void)getpid();
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0)
            close(fd);
        umask(022);
        char cwd[64];
        (void)getcwd(cwd, sizeof(cwd));
        struct rlimit rl;
        getrlimit(RLIMIT_NOFILE, &rl);
        kill(0, 0);   /* the group: proc.c's table and its allocation */
        if (m != MAP_FAILED)
            munmap(m, PG);
        free(p);
    }
    return NULL;
}

static void *short_lived(void *arg)
{
    return arg;
}

/* Threads starting and ending (an ending thread holds musl's thread list
 * lock in the layer's thread end). */
static void *churn(void *arg)
{
    (void)arg;
    while (!stop) {
        pthread_t t;
        if (pthread_create(&t, NULL, short_lived, NULL) == 0)
            pthread_join(t, NULL);
    }
    return NULL;
}

static void *seven(void *arg)
{
    (void)arg;
    return (void *)7;
}

static void kid_threads(pid_t parent)
{
    KID(childed == 1);
    KID(getpid() != parent && getppid() == parent);
    void *small = malloc(24), *big = malloc(1 << 20);
    KID(small && big);
    free(small);
    free(big);
    pthread_t t;
    void *ret = NULL;
    KID(pthread_create(&t, NULL, seven, NULL) == 0 && pthread_join(t, &ret) == 0 && ret == (void *)7);
    void *m = mmap(NULL, 4 * PG, RW, ANON, -1, 0);
    KID(m != MAP_FAILED && munmap(m, 4 * PG) == 0);
    sigset_t s, old;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    KID(sigprocmask(SIG_BLOCK, &s, &old) == 0 && sigprocmask(SIG_SETMASK, &old, NULL) == 0);
    kid_end(0);
}

static void fork_threads(void)
{
    map_report();
    CHECK(pthread_atfork(at_prepare, at_parent, at_child) == 0);
    pthread_t th[HAMMERS + 1];
    for (unsigned i = 0; i < HAMMERS; i++)
        CHECK(pthread_create(&th[i], NULL, hammer, (void *)(uintptr_t)(i + 1)) == 0);
    CHECK(pthread_create(&th[HAMMERS], NULL, churn, NULL) == 0);
    pid_t self = getpid(), kids[16];
    for (unsigned i = 0; i < 16; i++) {
        kids[i] = fork();
        if (kids[i] == 0)
            kid_threads(self);
        CHECK(kids[i] > 0);
        sched_yield();
    }
    for (unsigned i = 0; i < 16; i++)
        if (kids[i] > 0)
            reaped(kids[i], 0);
    stop = 1;
    for (unsigned i = 0; i <= HAMMERS; i++)
        pthread_join(th[i], NULL);
    CHECK(prepared == 16 && parented == 16 && childed == 0);
}

/* ---- torn ------------------------------------------------------------------------- */

static volatile uint64_t *ra, *rb;   /* two mappings: two VMOs */

static void *writer(void *arg)
{
    unsigned k = (unsigned)(uintptr_t)arg * 8;   /* a cache line each */
    for (uint64_t n = 1; !stop; n++) {
        __atomic_store_n(&ra[k], n, __ATOMIC_RELEASE);
        __atomic_store_n(&rb[k], n, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void fork_torn(void)
{
    map_report();
    ra = mmap(NULL, PG, RW, ANON, -1, 0);
    rb = mmap(NULL, PG, RW, ANON, -1, 0);
    CHECK(ra != MAP_FAILED && rb != MAP_FAILED);
    if (ra == MAP_FAILED || rb == MAP_FAILED)
        return;
    pthread_t th[WRITERS];
    for (unsigned i = 0; i < WRITERS; i++)
        CHECK(pthread_create(&th[i], NULL, writer, (void *)(uintptr_t)i) == 0);
    for (unsigned f = 0; f < 20; f++) {
        pid_t pid = fork();
        if (pid == 0) {
            for (unsigned k = 0; k < WRITERS * 8; k += 8)
                KID(rb[k] <= ra[k] && ra[k] - rb[k] <= 1);
            kid_end(0);
        }
        CHECK(pid > 0);
        if (pid > 0)
            reaped(pid, 0);
    }
    stop = 1;
    for (unsigned i = 0; i < WRITERS; i++)
        pthread_join(th[i], NULL);
}

/* ---- huge ------------------------------------------------------------------------- */

static void fork_huge(void)
{
    map_report();
    char *r = mmap(NULL, TIB, PROT_NONE, ANON | MAP_NORESERVE, -1, 0);
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    for (uint64_t g = 0; g < TIB / GIB; g++) {
        CHECK(mprotect(r + g * GIB, PG, RW) == 0);
        r[g * GIB] = (char)(g + 1);
    }
    uint64_t before = pt_job_pages(), t0 = now();
    pid_t pid = fork();
    if (pid == 0) {
        for (uint64_t g = 0; g < TIB / GIB; g++)
            KID(r[g * GIB] == (char)(g + 1));
        r[0] = 99;   /* its own copy */
        rep->v[0] = 1;
        KID(until_set(&rep->v[1], KID_NS));
        kid_end(0);
    }
    uint64_t took = now() - t0;
    CHECK(pid > 0);
    if (pid > 0) {
        CHECK(until_set(&rep->v[0], KID_NS));
        uint64_t during = pt_job_pages();
        printf("posixtest: fork: a 1 TiB reservation with %llu pages forked in %llu us; "
               "%llu pages charged before, %llu with the child\n",
               (unsigned long long)(TIB / GIB), (unsigned long long)(took / 1000),
               (unsigned long long)before, (unsigned long long)during);
        CHECK(took < NS_PER_S);
        CHECK(during <= 2 * before + 4096);   /* our pages again and the tables, no more */
        rep->v[1] = 1;
        reaped(pid, 0);
        CHECK(r[0] == 1);
    }
    CHECK(munmap(r, TIB) == 0);
}

/* ---- nomem ------------------------------------------------------------------------ */

static uint64_t job_used(void)
{
    struct job_info ji;
    return jam_job_get_info(startup_handle(SR_JOB), &ji) == OK ? ji.used[JOB_LIMIT_PAGES] : 0;
}

static void fork_nomem(void)
{
    struct job_info ji;
    CHECK(jam_job_get_info(startup_handle(SR_JOB), &ji) == OK);
    uint64_t room = ji.limit[JOB_LIMIT_PAGES] - ji.used[JOB_LIMIT_PAGES];
    CHECK(ji.limit[JOB_LIMIT_PAGES] == JOB_PAGES && room > 512);
    if (ji.limit[JOB_LIMIT_PAGES] != JOB_PAGES || room <= 512)
        return;
    size_t size = room * 3 / 4 * PG;
    char *p = mmap(NULL, size, RW, ANON, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    for (size_t o = 0; o < size; o += PG)
        p[o] = 1;
    errno = 0;
    CHECK(fork() == -1 && errno == ENOMEM);   /* the pages its calls touch, once */
    uint64_t used = job_used();
    errno = 0;
    CHECK(fork() == -1 && errno == ENOMEM);
    uint64_t end = now() + NS_PER_S;   /* what the refused fork made goes as it closes */
    while (job_used() != used && now() < end)
        usleep(1000);
    CHECK(job_used() == used);
    CHECK(munmap(p, size) == 0);
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    CHECK(pid > 0);
    if (pid > 0)
        reaped(pid, 0);
}

/* ---- signals ---------------------------------------------------------------------- */

static volatile sig_atomic_t usr1s, usr2s;
static volatile pid_t chld_pid;
static volatile int chld_status, chld_code;
static sigjmp_buf segv_env;

static char *alt_base;            /* the alternate stack the fault handler runs on */
static volatile int segv_on_alt;

static void on_usr1(int sig) { (void)sig; usr1s++; }
static void on_usr2(int sig) { (void)sig; usr2s++; }

static void on_segv(int sig)
{
    (void)sig;
    char here;
    segv_on_alt = (uintptr_t)&here - (uintptr_t)alt_base < ALT_SIZE;
    siglongjmp(segv_env, 1);
}

static void on_chld(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)uc;
    chld_status = si->si_status;
    chld_code = si->si_code;
    chld_pid = si->si_pid;
}

static void handle(int sig, void (*fn)(int))
{
    struct sigaction sa = { .sa_handler = fn };
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(sig, &sa, NULL) == 0);
}

/* Spin until a handler's counter is set or the time is up. */
static bool until_handled(volatile sig_atomic_t *count, uint64_t ns)
{
    uint64_t end = now() + ns;
    while (!*count && now() < end)
        usleep(1000);
    return *count != 0;
}

/* A child that waits for SIGUSR1 (kill's): our handler is its own. */
static pid_t usr1_kid(void)
{
    rep->v[0] = 0;
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction now_sa;
        KID(sigaction(SIGUSR1, NULL, &now_sa) == 0 && now_sa.sa_handler == on_usr1);
        sigset_t m;
        KID(sigprocmask(SIG_BLOCK, NULL, &m) == 0 && sigismember(&m, SIGUSR2));
        usr1s = 0;
        rep->v[0] = 1;
        KID(until_handled(&usr1s, KID_NS));
        kid_end(0);
    }
    CHECK(pid > 0);
    CHECK(pid <= 0 || until_set(&rep->v[0], KID_NS));
    return pid;
}

static void fork_signals(void)
{
    map_report();
    handle(SIGUSR1, on_usr1);
    handle(SIGUSR2, on_usr2);
    sigset_t usr2;
    sigemptyset(&usr2);
    sigaddset(&usr2, SIGUSR2);
    CHECK(sigprocmask(SIG_BLOCK, &usr2, NULL) == 0);

    /* SIGCHLD: the child's pid and status. */
    struct sigaction sa = { .sa_sigaction = on_chld, .sa_flags = SA_SIGINFO };
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGCHLD, &sa, NULL) == 0);
    pid_t pid = fork();
    if (pid == 0)
        _exit(7);
    CHECK(pid > 0);
    uint64_t end = now() + KID_NS;
    while (chld_pid != pid && now() < end)
        usleep(1000);
    CHECK(chld_pid == pid && chld_code == CLD_EXITED && chld_status == 7);
    if (pid > 0)
        reaped(pid, 7);
    handle(SIGCHLD, SIG_DFL);

    /* kill, then kill(-pgid): the child's handler (and ours, for the group). */
    pid = usr1_kid();
    CHECK(pid <= 0 || kill(pid, SIGUSR1) == 0);
    if (pid > 0)
        reaped(pid, 0);
    usr1s = 0;
    pid = usr1_kid();
    CHECK(pid <= 0 || kill(-getpgrp(), SIGUSR1) == 0);
    if (pid > 0)
        reaped(pid, 0);
    CHECK(until_handled(&usr1s, KID_NS));

    /* SIGKILL. */
    pid = fork();
    if (pid == 0)
        for (;;)
            pause();
    CHECK(pid > 0 && kill(pid, SIGKILL) == 0);
    if (pid > 0) {
        int st = reap(pid);
        CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    }

    /* Pending in us, not in the child; alarm and a POSIX timer gone there. */
    CHECK(kill(getpid(), SIGUSR2) == 0);
    sigset_t pend;
    CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR2));
    struct itimerval it = { .it_value = { .tv_sec = 100 } };
    CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
    timer_t timer;
    struct sigevent ev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1 };
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &timer) == 0);
    struct itimerspec ts = { .it_value = { .tv_sec = 100 } };
    CHECK(timer_settime(timer, 0, &ts, NULL) == 0);
    pid = fork();
    if (pid == 0) {
        sigset_t p;
        KID(sigpending(&p) == 0 && !sigismember(&p, SIGUSR2));
        KID(sigprocmask(SIG_UNBLOCK, &usr2, NULL) == 0);
        KID(usr2s == 0);
        struct itimerval cur;
        KID(getitimer(ITIMER_REAL, &cur) == 0 && !cur.it_value.tv_sec && !cur.it_value.tv_usec);
        KID(alarm(0) == 0);
        struct itimerspec left;
        errno = 0;
        KID(timer_gettime(timer, &left) == -1 && errno == EINVAL);
        kid_end(0);
    }
    CHECK(pid > 0);
    if (pid > 0)
        reaped(pid, 0);
    CHECK(alarm(0) > 0);   /* ours is still set */
    struct itimerspec left;
    CHECK(timer_gettime(timer, &left) == 0 && left.it_value.tv_sec > 0);
    CHECK(timer_delete(timer) == 0);
    CHECK(usr2s == 0 && sigprocmask(SIG_UNBLOCK, &usr2, NULL) == 0 && usr2s == 1);

    /* A fault handler runs in the child, on its alternate stack: its
     * exception entry and its kernel exception stack are set again. */
    char *guard = mmap(NULL, PG, PROT_NONE, ANON, -1, 0);
    alt_base = malloc(ALT_SIZE);
    CHECK(guard != MAP_FAILED && alt_base);
    if (guard == MAP_FAILED || !alt_base)
        return;
    stack_t ss = { .ss_sp = alt_base, .ss_size = ALT_SIZE };
    CHECK(sigaltstack(&ss, NULL) == 0);
    struct sigaction segv = { .sa_handler = on_segv, .sa_flags = SA_ONSTACK };
    sigemptyset(&segv.sa_mask);
    CHECK(sigaction(SIGSEGV, &segv, NULL) == 0);
    pid = fork();
    if (pid == 0) {
        if (!sigsetjmp(segv_env, 1)) {
            *(volatile char *)guard = 1;
            KID(!"the fault came");
        }
        KID(segv_on_alt);
        kid_end(0);
    }
    CHECK(pid > 0);
    if (pid > 0)
        reaped(pid, 0);
    handle(SIGSEGV, SIG_DFL);
    ss = (stack_t){ .ss_flags = SS_DISABLE };
    CHECK(sigaltstack(&ss, NULL) == 0);
    free(alt_base);
    munmap(guard, PG);
}

/* ---- clone ------------------------------------------------------------------------ */

static int clone_magic;

static int clone_fn(void *arg)
{
    KID(arg == &clone_magic && clone_magic == 0x5a5a);
    KID(getppid() == (pid_t)rep->v[0]);
    return rep->line ? 1 : 9;
}

/* vfork alone in its function: nothing of ours lives across it. */
__attribute__((noinline)) static pid_t do_vfork(void)
{
    pid_t pid = vfork();
    if (pid == 0)
        _exit(6);
    return pid;
}

static void fork_clone(void)
{
    map_report();
/* CLONE_FS without CLONE_VM (base's LaunchProcess passes it for a child
 * that execs at once) is a copy of our directory and umask, not a share
 * (fork.c, lx_clone): the child starts in ours with ours, and what it
 * changes stays its own. By the raw call and by clone(), whose
 * CLONE_CHILD_SETTID word holds the child's pid in the child. */
static pid_t fs_ctid;

static int clone_fs_fn(void *arg)
{
    char buf[64];
    KID(arg == &clone_magic);
    KID(fs_ctid == getpid() && syscall(SYS_gettid) == getpid());
    KID(getcwd(buf, sizeof(buf)) && !strcmp(buf, "/data"));
    KID(umask(022) == 027 && chdir("/") == 0);
    return rep->line ? 1 : 7;
}

static void clone_fs(void)
{
    char was[64], buf[64];
    CHECK(getcwd(was, sizeof(was)) != NULL);
    mode_t m = umask(027);
    CHECK(chdir("/data") == 0);
    long r = syscall(SYS_clone, SIGCHLD | CLONE_FS, 0, 0, 0, 0);
    if (r == 0) {
        KID(getcwd(buf, sizeof(buf)) && !strcmp(buf, "/data"));
        KID(umask(022) == 027 && chdir("/") == 0);
        kid_end(6);
    }
    CHECK(r > 0);
    if (r > 0)
        reaped((pid_t)r, 6);
    char *stk = malloc(64 << 10);
    CHECK(stk);
    pid_t pid = stk ? clone(clone_fs_fn, stk + (64 << 10), SIGCHLD | CLONE_FS | CLONE_CHILD_SETTID,
                            &clone_magic, NULL, NULL, &fs_ctid)
                    : -1;
    CHECK(pid > 0 && fs_ctid == 0);   /* the word is the child's copy's */
    if (pid > 0)
        reaped(pid, 7);
    free(stk);
    CHECK(getcwd(buf, sizeof(buf)) && !strcmp(buf, "/data"));   /* theirs, not ours */
    CHECK(umask(m) == 027);
    CHECK(chdir(was) == 0);
}

/* A fork from a thread that isn't the first: the child's one thread is
 * the forking one, and its id the child's pid (Linux's). */
static void *fork_in_thread(void *arg)
{
    (void)arg;
    CHECK(syscall(SYS_gettid) != getpid());
    pid_t pid = fork();
    if (pid == 0) {
        KID(syscall(SYS_gettid) == getpid() && gettid() == getpid());
        kid_end(4);
    }
    CHECK(pid > 0);
    if (pid > 0)
        reaped(pid, 4);
    return NULL;
}

    pid_t self = getpid();
    rep->v[0] = self;
    int ptid = 0, ctid = 0;
    long r = syscall(SYS_clone, SIGCHLD | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID, 0, &ptid,
                     &ctid, 0);
    if (r == 0) {
        KID(ctid == syscall(SYS_gettid) && ctid > 0);
        KID(getppid() == self);
        kid_end(5);
    }
    CHECK(r > 0 && ptid == r && ctid == 0);
    if (r > 0)
        reaped((pid_t)r, 5);
    errno = 0;
    CHECK(syscall(SYS_clone, CLONE_VM | SIGCHLD, 0, 0, 0, 0) == -1 && errno == ENOSYS);
    char *stk = malloc(64 << 10);
    CHECK(stk);
    if (!stk)
        return;
        KID(ctid == getpid());   /* the child's first thread's id is its pid (ForkWithFlags) */
    errno = 0;
    CHECK(syscall(SYS_clone, SIGCHLD, stk + (64 << 10), 0, 0, 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(syscall(SYS_clone, SIGCHLD | CLONE_NEWNS, 0, 0, 0, 0) == -1 && errno == EINVAL);

    clone_magic = 0x5a5a;
    clone_fs();
    pthread_t t;
    CHECK(pthread_create(&t, NULL, fork_in_thread, NULL) == 0 && pthread_join(t, NULL) == 0);
    pid_t pptid = 0;
    pid_t pid = clone(clone_fn, stk + (64 << 10), SIGCHLD | CLONE_PARENT_SETTID, &clone_magic,
                      &pptid);
    CHECK(pid > 0 && pptid == pid);
    if (pid > 0)
        reaped(pid, 9);
    errno = 0;
    CHECK(clone(clone_fn, stk + (64 << 10), CLONE_VM | SIGCHLD, &clone_magic) == -1 &&
          errno == ENOSYS);
    free(stk);

    pid = do_vfork();
    CHECK(pid > 0);
    if (pid > 0)
        reaped(pid, 6);
}

/* ---- small: clone's child on two pages ---------------------------------------------- */

/* gtest's death tests give clone's child two pages they map (and unmap in
 * the parent at once); the layer's own frames for a path are bigger than
 * that, so its calls run on a stack of its own there (fork.c), and fn's
 * on the one given, as on Linux. A guard page below the two makes running
 * over them a fault, not a write over whatever was mapped there. */
#define SMALL (2 * PG)

static uintptr_t small_lo, small_hi;   /* the stack given */
static char small_dir[64];

static int small_inner(void *arg)
{
    struct stat st;
    (void)arg;
    KID((uintptr_t)&st > small_lo && (uintptr_t)&st < small_hi);
    KID(stat("/proc/self/exe", &st) == 0);
    return 4;
}

static int small_fn(void *arg)
{
    struct stat st;
    char buf[64];
    int status = 0;
    KID(arg == &clone_magic);
    KID((uintptr_t)&st > small_lo && (uintptr_t)&st < small_hi);   /* fn on the stack given */
    KID(stat("/", &st) == 0 && S_ISDIR(st.st_mode));
    KID(lstat("/proc/self/exe", &st) == 0);
    KID(access("/data", W_OK) == 0);   /* /data: the fds case's too (the init run has no /tmp to write) */
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    KID(fd >= 0 && read(fd, buf, sizeof(buf)) > 0 && close(fd) == 0);
    KID(mkdir(small_dir, 0700) == 0 && rmdir(small_dir) == 0);
    KID(chdir("/data") == 0 && getcwd(buf, sizeof(buf)) && !strcmp(buf, "/data"));
    KID(readlink("/proc/self/exe", buf, sizeof(buf)) > 0);
    pid_t g = fork();   /* the grandchild goes on from the copy of the stack for calls */
    if (g == 0)
        _exit(stat("/", &st) == 0 ? 3 : 1);
    KID(g > 0 && waitpid(g, &status, 0) == g && WIFEXITED(status) && WEXITSTATUS(status) == 3);
    g = clone(small_inner, (char *)small_hi - 64, SIGCHLD, NULL);   /* this stack's copy */
    KID(g > 0 && waitpid(g, &status, 0) == g && WIFEXITED(status) && WEXITSTATUS(status) == 4);
    return rep->line ? 1 : 8;
}

static void fork_small(void)
{
    map_report();
    snprintf(small_dir, sizeof(small_dir), "/data/pxclone-%d", (int)getpid());
    char *guard = mmap(NULL, PG + SMALL, RW, ANON, -1, 0);
    CHECK(guard != MAP_FAILED && mprotect(guard, PG, PROT_NONE) == 0);
    if (guard == MAP_FAILED)
        return;
    small_lo = (uintptr_t)guard + PG;
    small_hi = small_lo + SMALL;
    clone_magic = 0x5a5a;
    pid_t pid = clone(small_fn, (char *)small_hi - 64, SIGCHLD, &clone_magic);
    CHECK(pid > 0);
    CHECK(munmap(guard, PG + SMALL) == 0);   /* as gtest: the child has its copy */
    if (pid > 0)
        reaped(pid, 8);
}

/* ---- the section ------------------------------------------------------------------ */

static const struct {
    const char *name;
    void (*run)(void);
    uint64_t pages;   /* the job's page limit; 0: none */
} cases[] = {
    { "twice", fork_twice, 0 },     { "threads", fork_threads, 0 },
    { "torn", fork_torn, 0 },       { "huge", fork_huge, 0 },
    { "nomem", fork_nomem, JOB_PAGES }, { "signals", fork_signals, 0 },
    { "clone", fork_clone, 0 },     { "small", fork_small, 0 },
    /* forkfd.c's (FL2): the descriptors and the table of mappings */
    { "fds", pt_fork_fds, 0 },      { "chromium", pt_fork_chromium, 0 },
    { "ttys", pt_fork_ttys, 0 },    { "maps", pt_fork_maps, 0 },
    /* forknote.c's (FL4): vfork's wait and a child's notices */
    { "vfork", pt_fork_vfork, 0 },  { "groups", pt_fork_groups, 0 },
    { "junk", pt_fork_junk, 0 },
};
#define NCASES (sizeof(cases) / sizeof(cases[0]))

int pt_child_fork(int argc, char **argv)
{
    for (unsigned i = 0; argc >= 3 && i < NCASES; i++)
        if (!strcmp(argv[2], cases[i].name)) {
            setvbuf(stdout, NULL, _IOLBF, 0);
            cases[i].run();
            return pt_failures() ? 1 : 0;
        }
    return 2;
}

/* Every unit our case's job holds (0: none left), after a moment to let
 * what its processes made go. */
static uint64_t left_in(handle_t job)
{
    struct job_info ji;
    uint64_t end = now() + 2 * NS_PER_S, sum;
    do {
        sum = 0;
        if (jam_job_get_info(job, &ji) != OK)
            return UINT64_MAX;
        for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
            sum += ji.used[k];
    } while (sum && now() < end && jam_sleep_until(now() + 10 * NS_PER_MS) == OK);
    return sum;
}

static void run_case(unsigned i)
{
    handle_t job, proc;
    if (jam_job_create(startup_handle(SR_JOB), 0, &job) != OK) {
        CHECK(!"a job for the case");
        return;
    }
    if (cases[i].pages)
        CHECK(jam_job_set_limit(job, JOB_LIMIT_PAGES, cases[i].pages) == OK);
    const char *const argv[] = { "posixtest", "--child-fork", cases[i].name, NULL };
    struct spawn_args a = { .path = "bin/posixtest", .argc = 3, .argv = argv, .job = job,
                            .ns = NS_ALL };
    status_t st = spawn(&a, &proc);
    CHECK(st == OK);
    struct process_info info = { 0 };
    if (st == OK) {
        st = spawn_wait(proc, CASE_NS, &info);
        jam_handle_close(proc);
    }
    if (st != OK || info.killed || info.exit_code != 0) {
        CHECK(!"the case passed");
        printf("posixtest: fork: case %s: %s, killed %d, exit %lld\n", cases[i].name,
               status_str(st), (int)info.killed, (long long)info.exit_code);
        jam_job_kill(job);   /* it, and anything it forked */
    }
    uint64_t left = left_in(job);
    CHECK(left == 0);
    if (left)
        printf("posixtest: fork: case %s left %llu units in its job\n", cases[i].name,
               (unsigned long long)left);
    jam_handle_close(job);
}

void pt_fork(char **argv)
{
    (void)argv;
    for (unsigned i = 0; i < NCASES; i++)
        run_case(i);
}
