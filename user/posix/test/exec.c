/* posixtest's exec section (docs/FORK-PLAN.md, track FL3): execve in
 * place. Each case runs in a child of ours ("posixtest --child-exec case
 * <case>", started with libjam's spawn) in a job of its own, which forks;
 * the forked child sets things up and execs this program again through
 * /proc/self/exe ("posixtest --child-exec run <mode> ..."). The new image
 * checks what it was left and reports by its exit code (a failed check
 * also writes `posixtest: exec: FAILED exec.c line N` to the kernel log);
 * it and the case keep in step through two pipes at READY and GO. The job
 * then holds nothing.
 *
 *   same: the pid, getppid, and /proc/<pid>/stat's pid, parent, group,
 *     session and start time read by the parent are the same before and
 *     after the exec; the parent's kill reaches the new image's handler and
 *     its SIGTERM ends it, which the parent's waitpid sees.
 *   threads: a thread that isn't the first execs; the first (joining it)
 *     and three others (one spinning in its own code, one in a condition
 *     wait, one in a pipe read) are gone: /proc/self/task has one entry,
 *     the kernel counts the new image's two threads (it and its signal
 *     thread).
 *   cloexec: descriptors with FD_CLOEXEC (a pipe's write end, an eventfd,
 *     /dev/null) are closed, the pipe's reader seeing its end of file while
 *     the new image lives; those without (a pipe, a memfd, a socketpair
 *     end) work there.
 *   signals: handlers back to the default, SIG_IGN kept, the mask kept,
 *     two blocked signals still pending (a handler set again runs once
 *     unblocked).
 *   charge: the job pays for a forked copy of 32 MiB at the fork and gets
 *     it back at the exec (the line `the fork's copy ... back at the exec`).
 *   fail: an exec that fails returns -1 with its errno (ENOENT for no such
 *     program, ENOEXEC for a file that isn't one, E2BIG for an argv past
 *     2 MiB: the last two while the program is built), and the process goes
 *     on as it was: a thread still running, a handler, a pipe, a child's
 *     SIGCHLD through the signal thread. (efault.c has EFAULT.)
 *   chromium: Chromium's child sequence (base::LaunchProcess): fork,
 *     dup2 of the output pipe to 1 and 2 and of the IPC socketpair end to 3,
 *     every other descriptor closed by reading /proc/self/fd, execve; the
 *     new image has exactly those descriptors, writes to 1 and talks on 3.
 *   inotify: an inotify descriptor and an epoll set made without
 *     FD_CLOEXEC are gone after an execve of the process that holds them
 *     (not a forked child: it has none), as an epoll set is (Q3,
 *     docs/INOTIFY-PLAN.md); a pipe end made the same way is kept, and the
 *     new image has exactly the descriptors it should and makes an
 *     inotify descriptor of its own.
 *   clone: gtest's threadsafe death test's child: clone(fn, two pages,
 *     SIGCHLD), whose fn closes, changes directory and execs this program
 *     by its path; the new image's parent is the case, its directory the
 *     one fn changed to, and the case's waitpid sees it end with 0.
 *
 * "posixtest --child-exec native" is utest's (exec_native: a native
 * starter, the shell's way, waits on a POSIX program that execs). */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pt.h"

#define SELF_EXE  "/proc/self/exe"
#define READY     40                 /* the new image's pipe to the case */
#define GO        41                 /* the case's pipe to the new image */
#define CASE_NS   (60 * NS_PER_S)    /* a case's whole run */
#define WAIT_MS   10000              /* a side's wait for the other */
#define PG        4096ul
#define COPY      (32ul << 20)       /* charge's memory */
#define FDW       (1024 / 64)        /* words of a descriptor bitmap */

/* ---- the new image's checks ---------------------------------------------------------- */

static int run_line;   /* the first check that failed in the new image, or 0 */

static void run_failed(int line)
{
    if (!run_line)
        run_line = line;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "posixtest: exec: FAILED exec.c line %d\n", line);
    (void)jam_debug_write(buf, (size_t)n);
}
#define RUN(c)                     \
    do {                           \
        if (!(c))                  \
            run_failed(__LINE__);  \
    } while (0)

static int run_end(void)
{
    return run_line ? 1 : 0;
}

/* One byte from fd within WAIT_MS: it, or -1 (end of file, an error, or
 * the time up). */
static int byte_from(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    char c;
    if (poll(&p, 1, WAIT_MS) != 1 || read(fd, &c, 1) != 1)
        return -1;
    return (unsigned char)c;
}

static bool say(int fd, char c)
{
    return write(fd, &c, 1) == 1;
}

/* The descriptors open now, by /proc/self/fd (the listing's own left
 * out), into bits. */
static bool fd_bits(uint64_t bits[FDW])
{
    memset(bits, 0, FDW * sizeof(bits[0]));
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return false;
    for (struct dirent *e; (e = readdir(d));) {
        int fd = atoi(e->d_name);
        if (e->d_name[0] != '.' && fd != dirfd(d) && fd >= 0 && fd < FDW * 64)
            bits[fd / 64] |= 1ull << (fd % 64);
    }
    closedir(d);
    return true;
}

static volatile sig_atomic_t usr1s;

static void on_usr1(int sig)
{
    (void)sig;
    usr1s++;
}

static void handle(int sig, void (*fn)(int))
{
    struct sigaction sa = { .sa_handler = fn };
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

/* same: our pid and parent as the forked child had them; then usr1 from
 * the parent, then its SIGTERM. */
static int run_same(char **argv)
{
    RUN(getpid() == atoi(argv[4]) && getppid() == atoi(argv[5]));
    handle(SIGUSR1, on_usr1);
    RUN(say(READY, 'r'));
    uint64_t end = now() + (uint64_t)WAIT_MS * NS_PER_MS;
    while (!usr1s && now() < end)
        sched_yield();
    RUN(usr1s == 1);
    RUN(say(READY, 'u'));
    if (run_line)
        return run_end();
    while (now() < end + (uint64_t)WAIT_MS * NS_PER_MS)
        pause();   /* SIGTERM's default ends us */
    return 2;
}

/* threads: we alone in /proc/self/task, and the kernel counts us and our
 * signal thread. */
static int run_threads(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    RUN(d);
    for (struct dirent *e; d && (e = readdir(d));)
        n += e->d_name[0] != '.';
    if (d)
        closedir(d);
    RUN(n == 1);
    struct process_info info;
    RUN(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &info) == OK);
    RUN(info.threads == 2);
    if (info.threads != 2)
        printf("posixtest: exec: %u threads after the exec\n", info.threads);
    return run_end();
}

/* cloexec: 20, 23 and 25 closed; 21 (a pipe), 22 (a memfd) and 24 (a
 * socketpair end) ours and working; then GO. */
static int run_cloexec(void)
{
    RUN(fcntl(20, F_GETFD) == -1 && errno == EBADF);
    RUN(fcntl(23, F_GETFD) == -1 && errno == EBADF);
    RUN(fcntl(25, F_GETFD) == -1 && errno == EBADF);
    RUN(fcntl(21, F_GETFD) == 0 && fcntl(22, F_GETFD) == 0 && fcntl(24, F_GETFD) == 0);
    char buf[8] = { 0 };
    RUN(pread(22, buf, 5, 0) == 5 && !memcmp(buf, "memfd", 5));
    RUN(write(21, "kept", 4) == 4);
    RUN(send(24, "sock", 4, 0) == 4);
    RUN(byte_from(GO) == 'g');
    return run_end();
}

/* signals: USR1 and HUP back to the default, USR2 still ignored, TERM and
 * USR1 still blocked and pending. */
static int run_signals(void)
{
    struct sigaction o;
    RUN(sigaction(SIGUSR1, NULL, &o) == 0 && o.sa_handler == SIG_DFL);
    RUN(sigaction(SIGHUP, NULL, &o) == 0 && o.sa_handler == SIG_DFL);
    RUN(sigaction(SIGUSR2, NULL, &o) == 0 && o.sa_handler == SIG_IGN);
    sigset_t m, p;
    RUN(sigprocmask(SIG_BLOCK, NULL, &m) == 0);
    RUN(sigismember(&m, SIGTERM) && sigismember(&m, SIGUSR1) && !sigismember(&m, SIGHUP));
    RUN(sigpending(&p) == 0);
    RUN(sigismember(&p, SIGTERM) && sigismember(&p, SIGUSR1) && !sigismember(&p, SIGUSR2));
    handle(SIGUSR1, on_usr1);
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, SIGUSR1);
    RUN(sigprocmask(SIG_UNBLOCK, &one, NULL) == 0);
    RUN(usr1s == 1);   /* delivered on the way out of the call */
    signal(SIGTERM, SIG_IGN);   /* drops the pending one */
    RUN(sigpending(&p) == 0 && !sigismember(&p, SIGTERM));
    return run_end();
}

/* wait: say so, then wait for GO. */
static int run_wait(void)
{
    RUN(say(READY, 'r'));
    RUN(byte_from(GO) == 'g');
    return run_end();
}

/* clone: made by a clone child on two pages (exec_clone): the case
 * (argv[4]) its parent, the directory its child changed to its own. */
static int run_clone(char **argv)
{
    char cwd[16];
    RUN(getppid() == (pid_t)atoi(argv[4]));
    RUN(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"));
    return run_end();
}

/* fds: exactly the descriptors the forked child kept (argv[4], a bitmap
 * of 0 to 63), 1 the output pipe, 3 the IPC socket. */
static int run_fds(char **argv)
{
    uint64_t got[FDW];
    RUN(fd_bits(got));
    RUN(got[0] == strtoull(argv[4], NULL, 16));
    for (unsigned i = 1; i < FDW; i++)
        RUN(got[i] == 0);
    RUN(fcntl(1, F_GETFD) == 0 && fcntl(3, F_GETFD) == 0);
    RUN(write(1, "hello from the new image", 24) == 24);
    char buf[16];
    RUN(send(3, "ipc", 3, 0) == 3 && recv(3, buf, sizeof(buf), 0) == 3 && !memcmp(buf, "ack", 3));
    return run_end();
}

/* inotify: 20 (inotify) and 22 (epoll) gone though not close-on-exec, 21
 * (a pipe end) kept, the listing argv[4]'s bitmap (the case's own, less
 * those two and 1 and 2), and an inotify descriptor of its own working. */
static int run_inotify(char **argv)
{
    uint64_t got[FDW];
    char buf[16];
    errno = 0;
    RUN(fcntl(20, F_GETFD) == -1 && errno == EBADF);
    errno = 0;
    RUN(fcntl(22, F_GETFD) == -1 && errno == EBADF);
    RUN(fcntl(21, F_GETFD) == 0);
    RUN(fd_bits(got) && strlen(argv[4]) == FDW * 16);
    for (unsigned i = 0; i < FDW && strlen(argv[4]) == FDW * 16; i++) {
        char word[17] = { 0 };
        memcpy(word, argv[4] + i * 16, 16);
        /* 1 and 2 left out: the kernel log, where a starter gave no pipe,
         * is not kept by an execve (it has no give). */
        RUN((got[i] & (i ? ~0ull : ~6ull)) == strtoull(word, NULL, 16));
    }
    int in = inotify_init1(IN_NONBLOCK);
    RUN(in >= 0 && inotify_add_watch(in, "/etc", IN_CREATE) > 0);
    errno = 0;
    RUN(read(in, buf, sizeof(buf)) == -1 && errno == EAGAIN);
    return run_end();
}

/* native: the pid the program we replaced had; "ready" on 1; then the
 * starter's SIGTERM. */
static int run_native(char **argv)
{
    RUN(getpid() == atoi(argv[4]));
    RUN(write(1, "ready", 5) == 5);
    if (run_line)
        return run_end();
    uint64_t end = now() + 2ull * WAIT_MS * NS_PER_MS;
    while (now() < end)
        pause();
    return 2;
}

/* ---- the cases' helpers ---------------------------------------------------------------- */

/* Reap pid: its wait status, or -1. */
static int reap(pid_t pid)
{
    int st = 0;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r == -1 && errno == EINTR);
    CHECK(r == pid);
    return r == pid ? st : -1;
}

static void reaped0(pid_t pid)
{
    int st = reap(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st))
        printf("posixtest: exec: wait status %#x\n", st);
}

/* In a forked child: the pipes at READY and GO (no FD_CLOEXEC), then
 * this program again in mode `mode` with up to two more words. */
static _Noreturn void exec_run(const int ready[2], const int go[2], const char *mode,
                               const char *w1, const char *w2)
{
    if ((ready && dup2(ready[1], READY) != READY) || (go && dup2(go[0], GO) != GO))
        _exit(90);
    char *const args[] = { "posixtest", "--child-exec", "run", (char *)mode, (char *)w1,
                           (char *)w2, NULL };
    execv(SELF_EXE, args);
    _exit(91);
}

/* The case's two pipes, close-on-exec (the forked child moves its ends). */
static bool pipes(int ready[2], int go[2])
{
    bool ok = pipe2(ready, O_CLOEXEC) == 0 && pipe2(go, O_CLOEXEC) == 0;
    CHECK(ok);
    return ok;
}

/* After the fork: the case keeps ready[0] and go[1]. */
static void keep_ours(const int ready[2], const int go[2])
{
    close(ready[1]);
    close(go[0]);
}

static void close_ours(const int ready[2], const int go[2])
{
    close(ready[0]);
    close(go[1]);
}

/* /proc/<pid>/stat's fields that an exec keeps. */
struct ids {
    long pid, ppid, pgrp, session;
    unsigned long long start;
};

static bool read_ids(pid_t pid, struct ids *out)
{
    char path[32], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    char *rp = strrchr(buf, ')');
    if (!rp)
        return false;
    out->pid = atol(buf);
    /* after the name: state ppid pgrp session tty tpgid flags minflt
     * cminflt majflt cmajflt utime stime cutime cstime priority nice
     * threads itrealvalue starttime */
    char state;
    long skip;
    return sscanf(rp + 1, " %c %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld "
                          "%ld %ld %llu",
                  &state, &out->ppid, &out->pgrp, &out->session, &skip, &skip, &skip, &skip,
                  &skip, &skip, &skip, &skip, &skip, &skip, &skip, &skip, &skip, &skip, &skip,
                  &out->start) == 20;
}

/* ---- the cases --------------------------------------------------------------------------- */

static void exec_same(void)
{
    int ready[2], go[2];
    if (!pipes(ready, go))
        return;
    char me[16], kid[16];
    snprintf(me, sizeof(me), "%d", (int)getpid());
    pid_t pid = fork();
    if (pid == 0) {
        snprintf(kid, sizeof(kid), "%d", (int)getpid());
        if (dup2(go[0], GO) != GO || byte_from(GO) != 'g')   /* the parent has read stat */
            _exit(92);
        exec_run(ready, NULL, "same", kid, me);
    }
    CHECK(pid > 0);
    if (pid < 0)
        return;
    keep_ours(ready, go);
    struct ids before = { 0 }, after = { 0 };
    CHECK(read_ids(pid, &before));
    CHECK(say(go[1], 'g'));
    CHECK(byte_from(ready[0]) == 'r');   /* the new image runs, its handler set */
    CHECK(read_ids(pid, &after));
    CHECK(after.pid == pid && before.pid == pid);
    CHECK(after.ppid == getpid() && before.ppid == after.ppid);
    CHECK(before.pgrp == after.pgrp && before.session == after.session);
    CHECK(before.start == after.start);
    CHECK(kill(pid, SIGUSR1) == 0);
    CHECK(byte_from(ready[0]) == 'u');
    CHECK(kill(pid, SIGTERM) == 0);
    int st = reap(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    if (!WIFSIGNALED(st))
        printf("posixtest: exec: same: wait status %#x\n", st);
    close_ours(ready, go);
}

static pthread_mutex_t cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static volatile int running;

static void *spinner(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&running, 1, __ATOMIC_SEQ_CST);
    for (;;)
        __asm__ volatile("pause");
    return NULL;
}

static void *cond_waiter(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&cv_lock);
    __atomic_add_fetch(&running, 1, __ATOMIC_SEQ_CST);
    for (;;)
        pthread_cond_wait(&cv, &cv_lock);
    return NULL;
}

static void *reader(void *arg)
{
    char c;
    __atomic_add_fetch(&running, 1, __ATOMIC_SEQ_CST);
    (void)read(*(int *)arg, &c, 1);   /* nobody writes */
    return NULL;
}

/* The thread that execs: once the three run, with us (the first thread)
 * waiting to join it, the signal thread and the three counted. */
static void *execer(void *arg)
{
    (void)arg;
    uint64_t end = now() + (uint64_t)WAIT_MS * NS_PER_MS;
    while (__atomic_load_n(&running, __ATOMIC_SEQ_CST) < 3 && now() < end)
        sched_yield();
    struct process_info info;
    if (jam_process_get_info(startup_handle(SR_SELF_PROCESS), &info) != OK || info.threads != 6)
        _exit(94);
    exec_run(NULL, NULL, "threads", NULL, NULL);
}

static void exec_threads(void)
{
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t[4];
        if (pthread_create(&t[0], NULL, spinner, NULL) ||
            pthread_create(&t[1], NULL, cond_waiter, NULL) ||
            pthread_create(&t[2], NULL, reader, &p[0]) ||
            pthread_create(&t[3], NULL, execer, NULL))
            _exit(93);
        pthread_join(t[3], NULL);   /* the exec ends this wait with us */
        _exit(95);
    }
    CHECK(pid > 0);
    if (pid > 0)
        reaped0(pid);
    close(p[0]);
    close(p[1]);
}

/* fd made into want, with FD_CLOEXEC or not, the old one closed. */
static void move_to(int fd, int want, bool cloexec)
{
    CHECK(fd >= 0 && dup3(fd, want, cloexec ? O_CLOEXEC : 0) == want && close(fd) == 0);
}

static void exec_cloexec(void)
{
    int ready[2], go[2], a[2], b[2], s[2];
    if (!pipes(ready, go))
        return;
    CHECK(pipe2(a, O_CLOEXEC) == 0 && pipe2(b, O_CLOEXEC) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s) == 0);
    int mem = memfd_create("kept", MFD_CLOEXEC);
    CHECK(mem >= 0 && write(mem, "memfd", 5) == 5);
    move_to(a[1], 20, true);
    move_to(b[1], 21, false);
    move_to(mem, 22, false);
    move_to(eventfd(0, 0), 23, true);
    move_to(s[1], 24, false);
    move_to(open("/dev/null", O_RDONLY), 25, true);
    pid_t pid = fork();
    if (pid == 0)
        exec_run(NULL, go, "cloexec", NULL, NULL);
    CHECK(pid > 0);
    for (int fd = 20; fd <= 25; fd++)
        close(fd);   /* the child's are the only ones now */
    keep_ours(ready, go);
    if (pid > 0) {
        char buf[8] = { 0 };
        CHECK(read(b[0], buf, 4) == 4 && !memcmp(buf, "kept", 4));   /* the new image runs */
        CHECK(recv(s[0], buf, 4, 0) == 4 && !memcmp(buf, "sock", 4));
        struct pollfd pf = { .fd = a[0], .events = POLLIN };
        CHECK(poll(&pf, 1, WAIT_MS) == 1 && read(a[0], buf, 1) == 0);   /* its writer closed */
        CHECK(say(go[1], 'g'));
        reaped0(pid);
    }
    close(a[0]);
    close(b[0]);
    close(s[0]);
    close_ours(ready, go);
}

/* inotify: this process holds the descriptors and execs itself, with no
 * fork between (a forked child has no inotify descriptor to lose), so its
 * exit code is the new image's. */
static void exec_inotify(void)
{
    int p[2];
    CHECK(pipe2(p, 0) == 0);
    move_to(p[0], 21, false);
    close(p[1]);
    int in = inotify_init1(0);
    CHECK(in >= 0 && inotify_add_watch(in, "/etc", IN_ALL_EVENTS) > 0);
    move_to(in, 20, false);
    move_to(epoll_create1(0), 22, false);
    uint64_t bits[FDW];
    CHECK(fd_bits(bits) && (bits[0] >> 20 & 7) == 7);
    bits[0] &= ~((1ull << 20) | (1ull << 22) | 6ull);   /* ... and 1 and 2, see run_inotify */
    char words[FDW * 16 + 1];   /* the other descriptors, 16 hex digits a word */
    for (unsigned i = 0; i < FDW; i++)
        snprintf(words + i * 16, 17, "%016llx", (unsigned long long)bits[i]);
    char *const args[] = { "posixtest", "--child-exec", "run", "inotify", words, NULL };
    execv(SELF_EXE, args);
    CHECK(!"the case's own execve");
}

static void exec_signals(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        handle(SIGUSR1, on_usr1);
        handle(SIGHUP, on_usr1);
        signal(SIGUSR2, SIG_IGN);
        sigset_t m, p;
        sigemptyset(&m);
        sigaddset(&m, SIGTERM);
        sigaddset(&m, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &m, NULL) || raise(SIGTERM) || raise(SIGUSR1) ||
            raise(SIGUSR2) || sigpending(&p) || !sigismember(&p, SIGTERM) ||
            !sigismember(&p, SIGUSR1) || usr1s)
            _exit(95);
        exec_run(NULL, NULL, "signals", NULL, NULL);
    }
    CHECK(pid > 0);
    if (pid > 0)
        reaped0(pid);
}

/* The job's pages once they are at most `most` (what lets go goes now or
 * soon), or the last seen after WAIT_MS. */
static uint64_t pages_down_to(uint64_t most)
{
    uint64_t end = now() + (uint64_t)WAIT_MS * NS_PER_MS, p;
    while ((p = pt_job_pages()) > most && now() < end)
        sched_yield();
    return p;
}

static void exec_charge(void)
{
    int ready[2], go[2];
    if (!pipes(ready, go))
        return;
    char *m = mmap(NULL, COPY, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    for (size_t at = 0; at < COPY; at += PG)
        m[at] = 1;
    uint64_t p0 = pt_job_pages();
    pid_t pid = fork();
    if (pid == 0) {
        if (dup2(go[0], GO) != GO || byte_from(GO) != 'g')
            _exit(96);
        exec_run(ready, NULL, "wait", NULL, NULL);
    }
    CHECK(pid > 0);
    if (pid < 0)
        return;
    keep_ours(ready, go);
    uint64_t p1 = pt_job_pages();
    CHECK(p1 >= p0 + COPY / PG);   /* the copy is paid for at the fork */
    CHECK(say(go[1], 'g'));
    CHECK(byte_from(ready[0]) == 'r');
    uint64_t p2 = pages_down_to(p1 - 3 * COPY / PG / 4);
    CHECK(p2 + 3 * COPY / PG / 4 <= p1);   /* back, but for the new image's own */
    printf("posixtest: exec: the fork's copy of %lu pages back at the exec: %llu pages before "
           "the fork, %llu with the child, %llu after its exec\n",
           COPY / PG, (unsigned long long)p0, (unsigned long long)p1, (unsigned long long)p2);
    CHECK(say(go[1], 'g'));
    reaped0(pid);
    close_ours(ready, go);
    munmap(m, COPY);
}

static volatile int ticks;
static volatile sig_atomic_t chlds;

static void *ticker(void *arg)
{
    (void)arg;
    for (;;) {
        __atomic_add_fetch(&ticks, 1, __ATOMIC_SEQ_CST);
        sched_yield();
    }
    return NULL;
}

static void on_chld(int sig)
{
    (void)sig;
    chlds++;
}

/* Still as we were: the thread ticking, our handler, a pipe, the signal
 * thread (a child's end comes through it as SIGCHLD). */
static void still_ours(int p[2])
{
    int t = __atomic_load_n(&ticks, __ATOMIC_SEQ_CST);
    uint64_t end = now() + (uint64_t)WAIT_MS * NS_PER_MS;
    while (__atomic_load_n(&ticks, __ATOMIC_SEQ_CST) == t && now() < end)
        sched_yield();
    CHECK(__atomic_load_n(&ticks, __ATOMIC_SEQ_CST) != t);
    int u = usr1s;
    CHECK(raise(SIGUSR1) == 0 && usr1s == u + 1);
    char c = 0;
    CHECK(write(p[1], "x", 1) == 1 && read(p[0], &c, 1) == 1 && c == 'x');
    int k = chlds;
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    CHECK(pid > 0);
    while (chlds == k && now() < end)
        sched_yield();
    CHECK(chlds == k + 1);
    if (pid > 0)
        reaped0(pid);
}

static void exec_fail(void)
{
    pthread_t t;
    int p[2];
    CHECK(pthread_create(&t, NULL, ticker, NULL) == 0 && pipe(p) == 0);
    handle(SIGUSR1, on_usr1);
    handle(SIGCHLD, on_chld);
    pid_t me = getpid();
    char *const args[] = { "posixtest", "--child-exec", "run", "wait", NULL };
    errno = 0;
    CHECK(execv("/boot/bin/no-such-program", args) == -1 && errno == ENOENT);
    still_ours(p);
    errno = 0;
    CHECK(execv("/boot/build.txt", args) == -1 && errno == ENOEXEC);   /* text: no ELF */
    still_ours(p);
    /* More than 2 MiB of argv: refused while the program is built. */
    enum { BIG = 40, PIECE = 64 << 10 };
    char **big = calloc(BIG + 1, sizeof(char *));
    for (int i = 0; big && i < BIG; i++) {
        big[i] = malloc(PIECE);
        if (big[i]) {
            memset(big[i], 'a', PIECE - 1);
            big[i][PIECE - 1] = '\0';
        }
    }
    errno = 0;
    CHECK(big && execv(SELF_EXE, big) == -1 && errno == E2BIG);
    for (int i = 0; big && i < BIG; i++)
        free(big[i]);
    free(big);
    still_ours(p);
    CHECK(getpid() == me);
    close(p[0]);
    close(p[1]);
}

/* Chromium's child: close by /proc/self/fd every descriptor but 0 to 3. */
static void close_superfluous(void)
{
    uint64_t bits[FDW];
    if (!fd_bits(bits))
        _exit(97);
    for (int fd = 4; fd < FDW * 64; fd++)
        if (bits[fd / 64] & (1ull << (fd % 64)))
            close(fd);
}

static void exec_chromium(void)
{
    int out[2], ipc[2], other[2];
    CHECK(pipe2(out, O_CLOEXEC) == 0 && pipe2(other, O_CLOEXEC) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ipc) == 0);
    int noise = memfd_create("noise", 0);   /* not close-on-exec: closed by the listing */
    CHECK(noise >= 0);
    pid_t pid = fork();
    if (pid == 0) {
        if (dup2(out[1], 1) != 1 || dup2(out[1], 2) != 2 || dup2(ipc[1], 3) != 3)
            _exit(98);
        close_superfluous();
        uint64_t kept[FDW];
        char word[24];
        if (!fd_bits(kept))
            _exit(99);
        snprintf(word, sizeof(word), "%llx", (unsigned long long)kept[0]);
        exec_run(NULL, NULL, "fds", word, NULL);
    }
    CHECK(pid > 0);
    close(out[1]);
    close(ipc[1]);
    close(noise);
    if (pid > 0) {
        char buf[32] = { 0 };
        size_t have = 0;
        while (have < 24) {
            ssize_t n = read(out[0], buf + have, 24 - have);
            if (n <= 0)
                break;
            have += (size_t)n;
        }
        CHECK(have == 24 && !memcmp(buf, "hello from the new image", 24));
        CHECK(recv(ipc[0], buf, sizeof(buf), 0) == 3 && !memcmp(buf, "ipc", 3));
        CHECK(send(ipc[0], "ack", 3, 0) == 3);
        reaped0(pid);
        CHECK(read(out[0], buf, 1) == 0);   /* every writer gone */
    }
    close(out[0]);
    close(ipc[0]);
    close(other[0]);
    close(other[1]);
}

/* gtest's threadsafe death test's child (gtest-death-test.cc,
 * ExecDeathTestSpawnChild and ExecDeathTestChildMain): clone on two pages
 * mapped for it (unmapped in the parent at once; here with a guard page
 * below), and there a close, a chdir and an execv of this program by its
 * path. */
struct clone_exec {
    const char *path;
    char *const *argv;
    int close_fd;
};

static int clone_exec_child(void *p)
{
    const struct clone_exec *a = p;
    if (close(a->close_fd) != 0 || chdir("/") != 0)
        return 92;
    execv(a->path, a->argv);
    return 93;
}

static void exec_clone(void)
{
    char path[256], pid[16];
    ssize_t n = readlink(SELF_EXE, path, sizeof(path) - 1);
    CHECK(n > 0);
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    char *guard = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard != MAP_FAILED && mprotect(guard, PG, PROT_NONE) == 0);   /* a fault, not a write below */
    if (n <= 0 || guard == MAP_FAILED)
        return;
    path[n] = 0;
    snprintf(pid, sizeof(pid), "%d", (int)getpid());
    char *const args[] = { "posixtest", "--child-exec", "run", "clone", pid, NULL };
    struct clone_exec a = { path, args, p[0] };
    pid_t kid = clone(clone_exec_child, guard + 3 * PG - 64, SIGCHLD, &a);
    CHECK(kid > 0);
    CHECK(munmap(guard, 3 * PG) == 0);
    close(p[1]);
    if (kid > 0)
        reaped0(kid);
    close(p[0]);
}

static const struct {
    const char *name;
    void (*run)(void);
} cases[] = {
    { "same", exec_same },       { "threads", exec_threads }, { "cloexec", exec_cloexec },
    { "signals", exec_signals }, { "charge", exec_charge },   { "fail", exec_fail },
    { "chromium", exec_chromium }, { "clone", exec_clone }, { "inotify", exec_inotify },
};
#define NCASES (sizeof(cases) / sizeof(cases[0]))

int pt_child_exec(int argc, char **argv)
{
    const char *what = argc >= 3 ? argv[2] : "";
    if (!strcmp(what, "run") && argc >= 4) {
        const char *mode = argv[3];
        if (!strcmp(mode, "same") && argc >= 6)
            return run_same(argv);
        if (!strcmp(mode, "threads"))
            return run_threads();
        if (!strcmp(mode, "cloexec"))
            return run_cloexec();
        if (!strcmp(mode, "signals"))
            return run_signals();
        if (!strcmp(mode, "wait"))
            return run_wait();
        if (!strcmp(mode, "fds") && argc >= 5)
            return run_fds(argv);
        if (!strcmp(mode, "clone") && argc >= 5)
            return run_clone(argv);
        if (!strcmp(mode, "native") && argc >= 5)
            return run_native(argv);
        if (!strcmp(mode, "inotify") && argc >= 5)
            return run_inotify(argv);
        return 3;
    }
    if (!strcmp(what, "native")) {   /* utest's: exec at once */
        char pid[16];
        snprintf(pid, sizeof(pid), "%d", (int)getpid());
        char *const args[] = { "posixtest", "--child-exec", "run", "native", pid, NULL };
        execv(SELF_EXE, args);
        return 4;
    }
    for (unsigned i = 0; !strcmp(what, "case") && argc >= 4 && i < NCASES; i++)
        if (!strcmp(argv[3], cases[i].name)) {
            setvbuf(stdout, NULL, _IOLBF, 0);
            cases[i].run();
            return pt_failures() ? 1 : 0;
        }
    return 2;
}

/* Every unit the case's job holds (0: none left), after a moment to let
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
    const char *const argv[] = { "posixtest", "--child-exec", "case", cases[i].name, NULL };
    struct spawn_args a = { .path = "bin/posixtest", .argc = 4, .argv = argv, .job = job,
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
        printf("posixtest: exec: case %s: %s, killed %d, exit %lld\n", cases[i].name,
               status_str(st), (int)info.killed, (long long)info.exit_code);
        jam_job_kill(job);   /* it, and anything it forked */
    }
    uint64_t left = left_in(job);
    CHECK(left == 0);
    if (left)
        printf("posixtest: exec: case %s left %llu units in its job\n", cases[i].name,
               (unsigned long long)left);
    jam_handle_close(job);
}

void pt_exec(char **argv)
{
    (void)argv;
    for (unsigned i = 0; i < NCASES; i++)
        run_case(i);
}
