/* posixtest's signals between processes (the signal section's second
 * half; signal.c has the first): a child's signal channel (`signal`,
 * SR_SIGNALS) carries kill and sigqueue, the children's ends are SIGCHLD,
 * and a default action ends a process with a status waitpid reads.
 *
 *   - 10,000 SIGUSR1 to a child, each answered (a byte its handler writes
 *     to a pipe) before the next: none lost; then 500 sigqueue values in a
 *     burst, all counted and summed by the child (real-time signals
 *     queue); then SIGTERM, which it handles and exits 0;
 *   - SIGCHLD for 200 children: a handler that reaps with WNOHANG reaps
 *     all 200 (standard signals pending twice are one: the handler loops);
 *   - default actions: a busy loop (no calls at all) ended by SIGINT, a
 *     child waiting in pause ended by SIGTERM, SIGKILL, raise, abort,
 *     SIGPIPE: each WIFSIGNALED with its signal; SIGTERM ignored by the
 *     child: it exits 0;
 *   - a process group: kill(-pgid) reaches both children in it;
 *   - what a child keeps: our ignored signals and our mask;
 *   - kill of a child that has ended but isn't reaped is 0, of one reaped
 *     ESRCH. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define SELF     "/boot/bin/posixtest"
#define PINGS    10000
#define QUEUED   500
#define CHILDREN 200

extern char **environ;

/* ---- the children ------------------------------------------------------------------- */

static volatile sig_atomic_t usr1, queued, term;
static volatile long         queued_sum;

static void on_usr1(int sig)
{
    (void)sig;
    usr1++;
    (void)write(1, "a", 1);
}

static void on_queued(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)uc;
    queued++;
    queued_sum += si->si_value.sival_int;
}

static void on_term(int sig)
{
    (void)sig;
    term = 1;
}

static void on(int sig, void (*h)(int))
{
    struct sigaction sa = { .sa_handler = h, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static int child_count(void)
{
    on(SIGUSR1, on_usr1);
    on(SIGTERM, on_term);
    struct sigaction sa = { .sa_sigaction = on_queued, .sa_flags = SA_SIGINFO };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN, &sa, NULL);
    (void)write(1, "r", 1);
    while (!term)
        pause();
    for (int i = 0; i < 1000 && queued < QUEUED; i++)   /* SIGTERM may come first */
        usleep(10000);
    long want = (long)QUEUED * (QUEUED + 1) / 2;
    return usr1 == PINGS && queued == QUEUED && queued_sum == want ? 0 : 1;
}

static int child_inherit(void)
{
    struct sigaction sa;
    sigset_t m;
    if (sigaction(SIGINT, NULL, &sa) || sa.sa_handler != SIG_IGN)
        return 2;
    if (sigaction(SIGUSR2, NULL, &sa) || sa.sa_handler != SIG_DFL)
        return 3;   /* a handler is never inherited */
    if (sigprocmask(SIG_SETMASK, NULL, &m) || !sigismember(&m, SIGUSR1))
        return 4;
    return 0;
}

int pt_child_sig(int argc, char **argv)
{
    const char *what = argc > 2 ? argv[2] : "";
    if (!strcmp(what, "count"))
        return child_count();
    if (!strcmp(what, "inherit"))
        return child_inherit();
    if (!strcmp(what, "spin")) {
        (void)write(1, "r", 1);
        for (volatile unsigned long n = 0;; n++)   /* no calls at all */
            ;
    }
    if (!strcmp(what, "pause")) {
        (void)write(1, "r", 1);
        for (;;)
            pause();
    }
    if (!strcmp(what, "ignore")) {
        signal(SIGTERM, SIG_IGN);
        (void)write(1, "r", 1);
        usleep(200000);
        return 0;
    }
    if (!strcmp(what, "raise"))
        raise(SIGTERM);
    if (!strcmp(what, "abort"))
        abort();
    if (!strcmp(what, "pipe")) {
        for (int i = 0; i < 1000; i++) {   /* once our reader is gone, SIGPIPE ends us */
            (void)write(1, "x", 1);
            usleep(1000);
        }
        return 5;
    }
    return 100;
}

/* ---- this process ------------------------------------------------------------------- */

/* Start `posixtest --child-sig what` with its 1 the write end of a pipe
 * whose read end goes into *out (-1: its 1 is ours); attr may be NULL. */
static pid_t start(const char *what, int *out, const posix_spawnattr_t *attr)
{
    char *argv[] = { "posixtest", "--child-sig", (char *)what, NULL };
    int p[2] = { -1, -1 };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out) {
        CHECK(pipe2(p, O_CLOEXEC) == 0);   /* the child gets the write end alone, as 1 */
        posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    }
    pid_t pid = -1;
    CHECK(posix_spawn(&pid, SELF, &fa, attr, argv, environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    if (out) {
        close(p[1]);
        *out = p[0];
    }
    return pid;
}

/* Read the child's "r" (it is ready). */
static bool ready(int fd)
{
    char c = 0;
    return read(fd, &c, 1) == 1 && c == 'r';
}

static int status_of(pid_t pid)
{
    int st = -1;
    CHECK(waitpid(pid, &st, 0) == pid);
    return st;
}

static bool ended_by(pid_t pid, int sig)
{
    int st = status_of(pid);
    return WIFSIGNALED(st) && WTERMSIG(st) == sig;
}

static void check_pings(void)
{
    int fd;
    pid_t pid = start("count", &fd, NULL);
    CHECK(ready(fd));
    bool lost = false;
    for (int i = 0; i < PINGS && !lost; i++) {
        char c = 0;
        CHECK(kill(pid, SIGUSR1) == 0);
        lost = read(fd, &c, 1) != 1 || c != 'a';
    }
    CHECK(!lost);
    for (int i = 1; i <= QUEUED; i++)
        CHECK(sigqueue(pid, SIGRTMIN, (union sigval){ .sival_int = i }) == 0);
    CHECK(kill(pid, SIGTERM) == 0);
    int st = status_of(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(fd);
}

static volatile sig_atomic_t reaped, chld_runs;

static void on_chld(int sig)
{
    (void)sig;
    chld_runs++;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        reaped++;
}

static void check_sigchld(void)
{
    on(SIGCHLD, on_chld);
    char *argv[] = { "posixtest", "--child-proc", "exit", "0", NULL };
    for (int i = 0; i < CHILDREN; i++) {
        pid_t pid;
        CHECK(posix_spawn(&pid, SELF, NULL, NULL, argv, environ) == 0);
    }
    for (int i = 0; i < 3000 && reaped < CHILDREN; i++)
        usleep(10000);
    CHECK(reaped == CHILDREN && chld_runs >= 1);
    signal(SIGCHLD, SIG_DFL);
    errno = 0;
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
}

static void check_defaults(void)
{
    int fd;
    pid_t pid = start("spin", &fd, NULL);
    CHECK(ready(fd));
    CHECK(kill(pid, SIGINT) == 0 && ended_by(pid, SIGINT));
    close(fd);
    pid = start("pause", &fd, NULL);
    CHECK(ready(fd));
    CHECK(kill(pid, SIGTERM) == 0 && ended_by(pid, SIGTERM));
    close(fd);
    pid = start("pause", &fd, NULL);
    CHECK(ready(fd));
    CHECK(kill(pid, SIGKILL) == 0 && ended_by(pid, SIGKILL));
    close(fd);
    pid = start("ignore", &fd, NULL);
    CHECK(ready(fd));
    CHECK(kill(pid, SIGTERM) == 0);
    int st = status_of(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(fd);
    CHECK(ended_by(start("raise", NULL, NULL), SIGTERM));
    CHECK(ended_by(start("abort", NULL, NULL), SIGABRT));
    pid = start("pipe", &fd, NULL);
    close(fd);   /* its writes find no reader */
    CHECK(ended_by(pid, SIGPIPE));
    /* An ended child not reaped yet: kill is 0; reaped: ESRCH. */
    pid = start("raise", NULL, NULL);
    siginfo_t si;
    CHECK(waitid(P_PID, pid, &si, WEXITED | WNOWAIT) == 0);
    CHECK(kill(pid, SIGUSR1) == 0);
    CHECK(ended_by(pid, SIGTERM));
    errno = 0;
    CHECK(kill(pid, SIGUSR1) == -1 && errno == ESRCH);
}

static void check_group(void)
{
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    int f1, f2;
    pid_t a = start("pause", &f1, &at);
    posix_spawnattr_setpgroup(&at, a);
    pid_t b = start("pause", &f2, &at);
    posix_spawnattr_destroy(&at);
    CHECK(ready(f1) && ready(f2));
    CHECK(kill(-a, SIGTERM) == 0);
    CHECK(ended_by(a, SIGTERM) && ended_by(b, SIGTERM));
    close(f1);
    close(f2);
}

static void check_inherit(void)
{
    sigset_t s, old;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &s, &old) == 0);
    signal(SIGINT, SIG_IGN);
    on(SIGUSR2, on_term);
    int st = status_of(start("inherit", NULL, NULL));
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
}

void pt_sigproc(void)
{
    check_pings();
    check_sigchld();
    check_defaults();
    check_group();
    check_inherit();
}
