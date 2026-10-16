/* posixtest's efault section (docs/CR-PLAN.md, "CBF1 notes"): a call
 * given a pointer it can't read or write returns -1 with EFAULT and the
 * process goes on, as on Linux (the layer's checked copies, ucopy.S).
 *
 *   - base's CheckMemoryReadOnly exactly: getrlimit(RLIMIT_NPROC, <a
 *     read-only page>) is -1 with EFAULT;
 *   - every converted call, made raw (syscall(), so libc's wrappers touch
 *     nothing first), with each bad pointer of its kind: a read-only page
 *     where the call writes, a PROT_NONE page where it reads, an address
 *     where nothing is mapped, NULL (where NULL isn't "not given") and an
 *     address that isn't one (a general protection fault, not a page
 *     fault). Where Linux keeps what a failed call would have taken (a
 *     socketpair's bytes, an eventfd's count, a ready epoll event) the
 *     next good call gets it;
 *   - with a SIGSEGV and a SIGBUS handler installed the calls still say
 *     EFAULT and the handlers never run; a real fault outside the copies
 *     still reaches the handler, on this thread and another;
 *   - a child that never asked for a handler gets EFAULT too (the entry is
 *     set from the start) and then ends by a real fault's SIGSEGV. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define PG       4096ul
#define SELF     "/boot/bin/posixtest"
#define WILD     ((void *)0x10)                       /* nothing is ever mapped there */
#define NONCANON ((void *)0x8000000000001000ull)      /* not an address: #GP, not #PF */
#define ARCH_GET_FS_    0x1003
#define FUTEX_WAIT_PRIV 128   /* FUTEX_WAIT | FUTEX_PRIVATE_FLAG */

extern char **environ;

static char *ro;     /* a read-only page: where a call writes */
static char *none;   /* a PROT_NONE page: where a call reads */

/* The bad pointers of each kind. */
enum { OUT, OUT_OPT, IN, IN_OPT, BOTH, KINDS };
static void *bad[KINDS][6];
static int   nbad[KINDS];

static void kind(int k, void *a, void *b, void *c, void *d, void *e)
{
    void *all[] = { a, b, c, d, e };
    nbad[k] = 0;
    for (unsigned i = 0; i < 5; i++)
        if (all[i] != (void *)1)
            bad[k][nbad[k]++] = all[i];
}

static void kinds(void)
{
    void *skip = (void *)1;
    kind(OUT, ro, WILD, NULL, NONCANON, skip);       /* written: NULL is a bad address */
    kind(OUT_OPT, ro, WILD, NONCANON, skip, skip);   /* written, NULL meaning "not asked" */
    kind(IN, none, WILD, NULL, NONCANON, skip);      /* read */
    kind(IN_OPT, none, WILD, NONCANON, skip, skip);  /* read, NULL meaning "not given" */
    kind(BOTH, ro, none, WILD, NULL, NONCANON);      /* read, then written */
}

static int checked;   /* calls checked, for the section's line */

/* expr, with P each bad pointer of kind k, is -1 with EFAULT every time. */
#define EFAULTS(k, expr)                                                                   \
    do {                                                                                   \
        for (int i_ = 0; i_ < nbad[k]; i_++) {                                             \
            void *P = bad[k][i_];                                                          \
            errno = 0;                                                                     \
            long r_ = (long)(expr);                                                        \
            int e_ = errno;                                                                \
            checked++;                                                                     \
            if (r_ != -1 || e_ != EFAULT) {                                                \
                printf("posixtest: efault: %s with %p: %ld, errno %d\n", #expr, P, r_, e_); \
                CHECK(!"EFAULT, and on");                                                  \
            }                                                                              \
        }                                                                                  \
    } while (0)

static void pages(void)
{
    ro = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    none = mmap(NULL, PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(ro != MAP_FAILED && none != MAP_FAILED);
    memset(ro, 0, PG);
    struct pollfd pf = { .fd = 0, .events = POLLIN };   /* poll's and select's reads find a call */
    memcpy(ro, &pf, sizeof(pf));
    CHECK(mprotect(ro, PG, PROT_READ) == 0);
    kinds();
}

/* ---- base's way, and the calls that answer into memory ---------------------------- */

static void check_base(void)
{
    /* protected_memory_posix.cc's CheckMemoryReadOnly, as it is. */
    errno = 0;
    CHECK(getrlimit(RLIMIT_NPROC, (struct rlimit *)(void *)ro) == -1 && errno == EFAULT);
    struct rlimit good;
    CHECK(getrlimit(RLIMIT_NPROC, &good) == 0);   /* and a good one still answers */
    EFAULTS(OUT, syscall(SYS_getrlimit, RLIMIT_NOFILE, P));
    EFAULTS(OUT_OPT, syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, P));
    EFAULTS(IN, syscall(SYS_setrlimit, RLIMIT_NOFILE, P));
    EFAULTS(IN_OPT, syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, P, NULL));
}

static void check_answers(void)
{
    EFAULTS(OUT, syscall(SYS_clock_gettime, CLOCK_MONOTONIC, P));
    EFAULTS(OUT_OPT, syscall(SYS_clock_getres, CLOCK_MONOTONIC, P));
    EFAULTS(OUT, syscall(SYS_uname, P));
    EFAULTS(OUT, syscall(SYS_sysinfo, P));
    EFAULTS(OUT, syscall(SYS_getrusage, RUSAGE_SELF, P));
    EFAULTS(OUT_OPT, syscall(SYS_times, P));
    EFAULTS(OUT, syscall(SYS_getcwd, P, 4096));
    EFAULTS(OUT, syscall(SYS_getresuid, P, P, P));
    EFAULTS(OUT, syscall(SYS_getresgid, P, P, P));
    EFAULTS(OUT, syscall(SYS_getgroups, 1, P));
    EFAULTS(OUT_OPT, syscall(SYS_getcpu, P, NULL, NULL));
    EFAULTS(OUT_OPT, syscall(SYS_sched_getparam, 0, P));
    EFAULTS(OUT, syscall(SYS_sched_getaffinity, 0, 128, P));
    EFAULTS(OUT, syscall(SYS_getrandom, P, 16, 0));
    EFAULTS(OUT, syscall(SYS_prctl, PR_GET_NAME, P, 0, 0, 0));
    EFAULTS(OUT, syscall(SYS_prctl, PR_GET_PDEATHSIG, P, 0, 0, 0));
    EFAULTS(OUT, syscall(SYS_arch_prctl, ARCH_GET_FS_, P));
    EFAULTS(OUT, syscall(SYS_rt_sigpending, P, 8));
    EFAULTS(OUT_OPT, syscall(SYS_rt_sigprocmask, SIG_BLOCK, NULL, P, 8));
    EFAULTS(OUT_OPT, syscall(SYS_rt_sigaction, SIGUSR2, NULL, P, 8));
    EFAULTS(OUT_OPT, syscall(SYS_sigaltstack, NULL, P));
    EFAULTS(OUT, syscall(SYS_getitimer, ITIMER_REAL, P));
    EFAULTS(OUT, syscall(SYS_newfstatat, AT_FDCWD, SELF, P, 0));
    EFAULTS(OUT, syscall(SYS_stat, SELF, P));
    EFAULTS(OUT, syscall(SYS_statfs, "/boot", P));
    EFAULTS(OUT, syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", P, 64));
    int fd = open(SELF, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    EFAULTS(OUT, syscall(SYS_fstat, fd, P));
    EFAULTS(OUT, syscall(SYS_fstatfs, fd, P));
    EFAULTS(OUT, syscall(SYS_pread64, fd, P, 16, 0));
    EFAULTS(OUT, syscall(SYS_read, fd, P, 16));
    EFAULTS(OUT, ioctl(fd, FIONREAD, P));
    struct flock lk = { .l_type = F_RDLCK, .l_whence = SEEK_SET };
    EFAULTS(BOTH, syscall(SYS_fcntl, fd, F_GETLK, P));
    EFAULTS(IN, syscall(SYS_fcntl, fd, F_SETLK, P));
    CHECK(fcntl(fd, F_GETLK, &lk) == 0 && lk.l_type == F_UNLCK);
    close(fd);
    int dir = open("/boot/bin", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dir >= 0);
    EFAULTS(OUT, syscall(SYS_getdents64, dir, P, 4096));
    close(dir);
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    EFAULTS(OUT, syscall(SYS_read, fd, P, 64));
    close(fd);
    fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    EFAULTS(OUT, syscall(SYS_read, fd, P, 64));
    close(fd);
}

/* ---- the calls that read what they are given ---------------------------------------- */

static void check_paths(void)
{
    EFAULTS(IN, syscall(SYS_openat, AT_FDCWD, P, O_RDONLY, 0));
    EFAULTS(IN, syscall(SYS_open, P, O_RDONLY, 0));
    struct stat st;
    EFAULTS(IN, syscall(SYS_newfstatat, AT_FDCWD, P, &st, 0));
    EFAULTS(IN, syscall(SYS_access, P, R_OK));
    EFAULTS(IN, syscall(SYS_mkdir, P, 0755));
    EFAULTS(IN, syscall(SYS_unlink, P));
    EFAULTS(IN, syscall(SYS_chdir, P));
    EFAULTS(IN, syscall(SYS_readlink, P, ro, 16));
    EFAULTS(IN, syscall(SYS_symlink, P, "/data/efault-link"));
    EFAULTS(IN, syscall(SYS_rename, P, "/data/efault-x"));
    EFAULTS(IN, syscall(SYS_statfs, P, ro));
    EFAULTS(IN, syscall(SYS_memfd_create, P, 0));
    EFAULTS(IN_OPT, syscall(SYS_utimensat, AT_FDCWD, SELF, P, 0));
    char *argv[] = { "posixtest", "--child-efault", "exit", NULL };
    EFAULTS(IN, syscall(SYS_execve, P, argv, environ));
    EFAULTS(IN_OPT, syscall(SYS_execve, SELF, P, environ));   /* NULL argv is allowed */
    EFAULTS(IN_OPT, syscall(SYS_execve, SELF, argv, P));
}

static void check_signals_and_time(void)
{
    struct timespec zero = { 0, 0 };
    EFAULTS(IN, syscall(SYS_nanosleep, P, NULL));
    EFAULTS(IN, syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, P, NULL));
    EFAULTS(IN_OPT, syscall(SYS_rt_sigprocmask, SIG_BLOCK, P, NULL, 8));
    EFAULTS(IN_OPT, syscall(SYS_rt_sigaction, SIGUSR2, P, NULL, 8));
    EFAULTS(IN, syscall(SYS_rt_sigsuspend, P, 8));
    EFAULTS(IN, syscall(SYS_rt_sigtimedwait, P, NULL, &zero, 8));
    EFAULTS(IN, syscall(SYS_rt_sigqueueinfo, getpid(), 0, P));
    EFAULTS(IN, syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid), 0, P));
    EFAULTS(IN_OPT, syscall(SYS_sigaltstack, P, NULL));
    EFAULTS(IN_OPT, syscall(SYS_setitimer, ITIMER_REAL, P, NULL));
    EFAULTS(OUT_OPT, syscall(SYS_setitimer, ITIMER_REAL, NULL, P));
    EFAULTS(IN_OPT, syscall(SYS_timer_create, CLOCK_MONOTONIC, P, ro + 1024));
    int id = -1;
    EFAULTS(OUT, syscall(SYS_timer_create, CLOCK_MONOTONIC, NULL, P));
    CHECK(syscall(SYS_timer_create, CLOCK_MONOTONIC, NULL, &id) == 0);
    EFAULTS(IN, syscall(SYS_timer_settime, id, 0, P, NULL));
    EFAULTS(OUT, syscall(SYS_timer_gettime, id, P));
    CHECK(syscall(SYS_timer_delete, id) == 0);
    EFAULTS(IN, syscall(SYS_futex, P, FUTEX_WAIT_PRIV, 1, NULL, NULL, 0));
    int word = 7;
    EFAULTS(IN_OPT, syscall(SYS_futex, &word, FUTEX_WAIT_PRIV, 7, P, NULL, 0));
    EFAULTS(IN, syscall(SYS_sched_setaffinity, 0, 128, P));
    EFAULTS(IN_OPT, syscall(SYS_sched_setparam, 0, P));
    EFAULTS(IN_OPT, syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, P));
    EFAULTS(IN, syscall(SYS_prctl, PR_SET_NAME, P, 0, 0, 0));
}

/* ---- descriptors: the bytes, and what a failed read leaves -------------------------- */

static void check_pipes(void)
{
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
    EFAULTS(OUT, syscall(SYS_pipe2, P, 0));
    EFAULTS(OUT, syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, P));
    /* Chromium's and the sanitizers' "can this be read": a write to a pipe. */
    EFAULTS(IN, syscall(SYS_write, p[1], P, 1));
    EFAULTS(IN, syscall(SYS_writev, p[1], P, 1));
    struct iovec v = { .iov_len = 1 };
    for (int i = 0; i < nbad[IN]; i++) {   /* a good array naming a bad buffer */
        v.iov_base = bad[IN][i];
        errno = 0;
        CHECK(writev(p[1], &v, 1) == -1 && errno == EFAULT);
    }
    char c = 'x';
    CHECK(write(p[1], &c, 1) == 1);
    EFAULTS(OUT, ioctl(p[0], FIONREAD, P));
    EFAULTS(IN, syscall(SYS_readv, p[0], P, 1));
    EFAULTS(OUT, syscall(SYS_read, p[0], P, 1));   /* the kernel's read: that message is lost */
    close(p[0]);
    close(p[1]);

    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) == 0);
    CHECK(write(sp[1], "y", 1) == 1);
    EFAULTS(OUT, syscall(SYS_read, sp[0], P, 1));   /* a socketpair's bytes stay */
    EFAULTS(OUT, syscall(SYS_recvfrom, sp[0], P, 1, 0, NULL, NULL));
    EFAULTS(IN, syscall(SYS_recvmsg, sp[0], P, 0));
    char got = 0;
    CHECK(read(sp[0], &got, 1) == 1 && got == 'y');
    EFAULTS(IN, syscall(SYS_sendto, sp[1], P, 1, 0, NULL, 0));
    EFAULTS(IN, syscall(SYS_sendmsg, sp[1], P, 0));
    struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1 };
    v.iov_base = &c;
    for (int i = 0; i < nbad[IN_OPT]; i++) {   /* a good header naming bad control data */
        m.msg_control = bad[IN_OPT][i];
        m.msg_controllen = 64;
        errno = 0;   /* raw: musl's sendmsg copies the control data itself first */
        CHECK(syscall(SYS_sendmsg, sp[1], &m, 0) == -1 && errno == EFAULT);
        checked++;
    }
    socklen_t len = sizeof(struct sockaddr_storage);
    struct sockaddr_storage sa;
    EFAULTS(OUT, syscall(SYS_getsockname, sp[0], P, &len));
    EFAULTS(BOTH, syscall(SYS_getsockname, sp[0], &sa, P));
    int type = 0;
    len = sizeof(type);
    EFAULTS(OUT, syscall(SYS_getsockopt, sp[0], SOL_SOCKET, SO_TYPE, P, &len));
    EFAULTS(BOTH, syscall(SYS_getsockopt, sp[0], SOL_SOCKET, SO_TYPE, &type, P));
    EFAULTS(OUT, ioctl(sp[0], FIONREAD, P));
    EFAULTS(IN, ioctl(sp[0], FIONBIO, P));
    close(sp[0]);
    close(sp[1]);

    int e = eventfd(3, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(e >= 0);
    EFAULTS(OUT, syscall(SYS_read, e, P, 8));   /* the count stays */
    EFAULTS(IN, syscall(SYS_write, e, P, 8));
    uint64_t n = 0;
    CHECK(read(e, &n, 8) == 8 && n == 3);
    close(e);

    int mf = memfd_create("efault", MFD_CLOEXEC);
    CHECK(mf >= 0 && write(mf, "abcd", 4) == 4);
    EFAULTS(IN, syscall(SYS_write, mf, P, 4));
    EFAULTS(OUT, syscall(SYS_pread64, mf, P, 4, 0));   /* the kernel writes it: checked after */
    close(mf);
}

static void check_waits(void)
{
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0 && write(p[1], "z", 1) == 1);
    struct timeval tv = { 0, 0 };
    struct timespec ts = { 0, 0 };
    EFAULTS(IN, syscall(SYS_poll, P, 1, 0));
    struct pollfd pf = { .fd = p[0], .events = POLLIN };
    CHECK(poll(&pf, 1, 0) == 1 && pf.revents == POLLIN);
    errno = 0;
    CHECK(syscall(SYS_poll, ro, 1, 0) == -1 && errno == EFAULT);   /* read, then revents written */
    EFAULTS(IN_OPT, syscall(SYS_ppoll, &pf, 1, P, NULL, 8));
    EFAULTS(IN_OPT, syscall(SYS_ppoll, &pf, 1, &ts, P, 8));
    EFAULTS(IN_OPT, syscall(SYS_select, p[0] + 1, P, NULL, NULL, &tv));
    EFAULTS(IN_OPT, syscall(SYS_select, 1, NULL, NULL, NULL, P));
    EFAULTS(IN_OPT, syscall(SYS_pselect6, 1, NULL, NULL, NULL, P, NULL));
    EFAULTS(IN_OPT, syscall(SYS_pselect6, 1, NULL, NULL, NULL, &ts, P));
    errno = 0;
    CHECK(syscall(SYS_select, 1, ro, NULL, NULL, &tv) == -1 && errno == EFAULT);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    EFAULTS(IN, syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, p[0], P));
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 42 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    EFAULTS(OUT, syscall(SYS_epoll_wait, ep, P, 4, 0));   /* the event stays */
    EFAULTS(IN_OPT, syscall(SYS_epoll_pwait, ep, &ev, 4, 0, P, 8));
    struct epoll_event out = { 0 };
    CHECK(epoll_wait(ep, &out, 1, 0) == 1 && out.data.u64 == 42);
    close(ep);
    close(p[0]);
    close(p[1]);
}

/* A child that exits at once (--child-efault exit), for the waits' answers. */
static pid_t quick_child(void)
{
    char *argv[] = { "posixtest", "--child-efault", "exit", NULL };
    pid_t pid = -1;
    CHECK(posix_spawn(&pid, SELF, NULL, NULL, argv, environ) == 0);
    return pid;
}

static void check_children(void)
{
    for (int i = 0; i < nbad[OUT_OPT]; i++) {
        pid_t pid = quick_child();
        errno = 0;
        CHECK(syscall(SYS_wait4, pid, bad[OUT_OPT][i], 0, NULL) == -1 && errno == EFAULT);
        CHECK(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);   /* reaped, as Linux */
        pid = quick_child();
        errno = 0;
        CHECK(syscall(SYS_waitid, P_PID, pid, bad[OUT_OPT][i], WEXITED, NULL) == -1 &&
              errno == EFAULT);
        checked += 2;
    }
}

/* ---- handlers ------------------------------------------------------------------------ */

static volatile int handled;
static sigjmp_buf   back;

static void on_fault(int sig)
{
    (void)sig;
    handled++;
    siglongjmp(back, 1);
}

static void *thread_calls(void *arg)
{
    (void)arg;
    errno = 0;
    long r = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, ro);
    return (void *)(intptr_t)(r == -1 && errno == EFAULT);
}

static void check_handlers(void)
{
    struct sigaction sa = { .sa_handler = on_fault }, old_segv, old_bus;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGSEGV, &sa, &old_segv) == 0 && sigaction(SIGBUS, &sa, &old_bus) == 0);
    handled = 0;
    errno = 0;
    CHECK(getrlimit(RLIMIT_NPROC, (struct rlimit *)(void *)ro) == -1 && errno == EFAULT);
    EFAULTS(OUT, syscall(SYS_uname, P));
    EFAULTS(IN, syscall(SYS_write, 1, P, 1));
    pthread_t t;
    void *ok = NULL;
    CHECK(pthread_create(&t, NULL, thread_calls, NULL) == 0 && pthread_join(t, &ok) == 0);
    CHECK(ok == (void *)1);   /* another thread's fixup */
    CHECK(handled == 0);
    /* A real fault, not in a copy: the handler. */
    if (!sigsetjmp(back, 1))
        *(volatile char *)ro = 1;
    CHECK(handled == 1);
    CHECK(sigaction(SIGSEGV, &old_segv, NULL) == 0 && sigaction(SIGBUS, &old_bus, NULL) == 0);
}

/* The child that never asks for a handler: EFAULT, then the real fault. */
static void check_child_real(void)
{
    char *argv[] = { "posixtest", "--child-efault", "real", NULL };
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    pid_t pid = -1;
    CHECK(posix_spawn(&pid, SELF, &fa, NULL, argv, environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    char buf[64];
    size_t n = 0;
    ssize_t r;
    while (n + 1 < sizeof(buf) && (r = read(p[0], buf + n, sizeof(buf) - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = 0;
    close(p[0]);
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    bool segv = WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGKILL);
    if (!segv || strcmp(buf, "efault\n")) {
        printf("posixtest: efault: child real: status %#x, wrote \"%s\"\n", st, buf);
        CHECK(!"EFAULT, then SIGSEGV");
    }
}

void pt_efault(char **argv)
{
    (void)argv;
    pages();
    check_base();
    check_answers();
    check_paths();
    check_signals_and_time();
    check_pipes();
    check_waits();
    check_children();
    check_handlers();
    check_child_real();
    printf("posixtest: efault: %d calls with bad pointers\n", checked);
    munmap(ro, PG);
    munmap(none, PG);
}

int pt_child_efault(int argc, char **argv)
{
    const char *what = argc > 2 ? argv[2] : "";
    if (!strcmp(what, "exit"))
        return 0;
    if (strcmp(what, "real"))
        return 2;
    char *page = mmap(NULL, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return 3;
    if (getrlimit(RLIMIT_NPROC, (struct rlimit *)(void *)page) != -1 || errno != EFAULT)
        return 4;
    fputs("efault\n", stdout);
    fflush(stdout);
    *(volatile char *)page = 1;   /* the real fault: SIGSEGV */
    return 5;
}
