/* fork.c's cases for vfork's wait and a child's notices to its parent
 * (docs/FORK-PLAN.md sections 1.7 and 3, track FL4). Each runs in a job of
 * its own, as fork.c's do; a forked child reports through a MAP_SHARED
 * page and its exit code.
 *
 *   vfork: the parent runs again only once its child has ended (vfork then
 *     _exit; a failed execve first releases nothing) or exec'd (vfork then
 *     execve in place: the new image waits for our word, so a parent that
 *     waited for its end would wait until its time ran out); a signal sent
 *     to the parent meanwhile runs its handler only once vfork has
 *     returned.
 *   groups: a child's setpgid(0, 0) seen by the parent's getpgid,
 *     /proc/<pid>/stat, kill(-pgid) (the child's handler, not ours) and
 *     waitpid(-pgid); a vfork child's, made just before its end, by
 *     waitpid(-pgid); a child's setsid by getsid and getpgid (and our
 *     setpgid of it EPERM).
 *   junk: what a hostile child writes on its signal channel changes
 *     nothing: a message too short, an unknown ordinal, a txid, bytes
 *     over, a group no one has (or 0, or negative), `send` the wrong way
 *     (no signal for us), a session notice with bytes, a handle (refused:
 *     our end takes none); a group notice written by another process that
 *     holds the child's end (a program it started with a duplicate) is
 *     dropped; a flood of junk doesn't hide the true notice behind it; a
 *     message too big for any notice ends our listening to that child and
 *     nothing else (kill and waitpid reach it, the job ends empty). */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <idl/signal.h>
#include <jamos/libjam.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define PG      4096ul
#define KID_NS  (10 * NS_PER_S)     /* a side's wait for the other's word */
#define SLOW_NS (100 * NS_PER_MS)   /* a vfork child's pause before it goes on */
#define READY   40                  /* exec.c's "run wait": the new image's pipe to us */
#define GO      41                  /* and ours to it */
#define WAIT_MS 10000               /* our wait for the new image's byte */
#define FLOOD   4000                /* junk notices in a flood */
#define BIG     4096                /* a message bigger than any notice */
#define NOWHERE 0x7fff0000          /* a process group no one has */

/* What children report: a MAP_SHARED page. */
struct note {
    volatile int     line;   /* the first check that failed in a child, or 0 */
    volatile int64_t v[8];   /* words between the two sides */
};
static struct note *rep;

static void map_report(void)
{
    rep = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(rep != MAP_FAILED);
    if (rep == MAP_FAILED)
        exit(1);
}

/* A check in a child: the first failure's line in the report and the log. */
static void kid_failed(int line)
{
    if (!rep->line)
        rep->line = line;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "posixtest: fork: a child FAILED forknote.c line %d\n",
                     line);
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

static bool until_set(volatile int64_t *word, uint64_t ns)
{
    uint64_t end = now() + ns;
    while (!*word && now() < end)
        sched_yield();
    return *word != 0;
}

static void nap(uint64_t ns)
{
    struct timespec t = { .tv_sec = (time_t)(ns / NS_PER_S), .tv_nsec = (long)(ns % NS_PER_S) };
    while (nanosleep(&t, &t) == -1 && errno == EINTR)
        ;
}

/* waitpid(pid) (a group when negative) to its end, which should be an
 * exit with code, by the child `want`. */
static void reaped(pid_t pid, pid_t want, int code)
{
    int st = 0;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r == -1 && errno == EINTR);
    CHECK(r == want && WIFEXITED(st) && WEXITSTATUS(st) == code);
    if (r != want || !WIFEXITED(st) || WEXITSTATUS(st) != code)
        printf("posixtest: fork: waitpid(%d): %d, status %#x, wanted %d exit %d\n", (int)pid,
               (int)r, st, (int)want, code);
    if (rep->line)
        printf("posixtest: fork: a child failed at forknote.c line %d\n", rep->line);
}

static volatile sig_atomic_t usr1s;

/* SIGUSR1's handler, ours and our children's: counted, and the pid it ran
 * in, for the other side. */
static void on_usr1(int sig)
{
    (void)sig;
    usr1s++;
    rep->v[7] = getpid();
}

static void catch_usr1(void)
{
    struct sigaction sa = { .sa_handler = on_usr1 };
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
}

static bool until_usr1(uint64_t ns)
{
    uint64_t end = now() + ns;
    while (!usr1s && now() < end)
        usleep(1000);
    return usr1s != 0;
}

/* ---- vfork ------------------------------------------------------------------------ */

/* Each vfork alone in its function: nothing of ours lives across it. */
__attribute__((noinline)) static pid_t vfork_exit(void)
{
    pid_t pid = vfork();
    if (pid == 0) {
        nap(SLOW_NS);   /* a parent that went on at once would look meanwhile */
        rep->v[0] = 1;
        _exit(6);
    }
    return pid;
}

__attribute__((noinline)) static pid_t vfork_failed_exec(void)
{
    pid_t pid = vfork();
    if (pid == 0) {
        char *const args[] = { "nothing", NULL };
        KID(execv("/boot/bin/no-such-program", args) == -1);
        nap(SLOW_NS);
        rep->v[1] = 1;
        _exit(7);
    }
    return pid;
}

/* A thread of the parent's: SIGUSR1 to the process once the vfork child
 * runs, routed to the main thread (this one blocks it), waiting in vfork. */
static void *poker(void *arg)
{
    (void)arg;
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    CHECK(pthread_sigmask(SIG_BLOCK, &m, NULL) == 0);
    CHECK(until_set(&rep->v[3], KID_NS) && kill(getpid(), SIGUSR1) == 0);
    return NULL;
}

__attribute__((noinline)) static pid_t vfork_signal(void)
{
    pid_t pid = vfork();
    if (pid == 0) {
        rep->v[3] = 1;   /* the poker's word: the parent waits in vfork now */
        nap(SLOW_NS);
        KID(rep->v[7] == 0);   /* the parent's handler waits for vfork's return */
        _exit(8);
    }
    return pid;
}

__attribute__((noinline)) static pid_t vfork_exec(int ready, int go)
{
    pid_t pid = vfork();
    if (pid == 0) {
        nap(SLOW_NS);
        rep->v[2] = 1;
        KID(dup2(ready, READY) == READY && dup2(go, GO) == GO);
        char *const args[] = { "posixtest", "--child-exec", "run", "wait", NULL };
        execv("/proc/self/exe", args);
        _exit(127);
    }
    return pid;
}

/* One byte from fd within WAIT_MS: it, or -1. */
static int byte_from(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    char c;
    if (poll(&p, 1, WAIT_MS) != 1 || read(fd, &c, 1) != 1)
        return -1;
    return (unsigned char)c;
}

void pt_fork_vfork(void)
{
    map_report();
    pid_t pid = vfork_exit();
    CHECK(pid > 0 && rep->v[0] == 1);   /* its last write before _exit: seen at once */
    if (pid > 0)
        reaped(pid, pid, 6);

    pid = vfork_failed_exec();
    CHECK(pid > 0 && rep->v[1] == 1);
    if (pid > 0)
        reaped(pid, pid, 7);

    catch_usr1();
    pthread_t t;
    bool poking = pthread_create(&t, NULL, poker, NULL) == 0;
    CHECK(poking);
    pid = vfork_signal();
    CHECK(pid > 0 && until_usr1(KID_NS) && rep->v[7] == getpid());
    if (poking)
        pthread_join(t, NULL);
    if (pid > 0)
        reaped(pid, pid, 8);
    signal(SIGUSR1, SIG_DFL);

    int ready[2] = { -1, -1 }, go[2] = { -1, -1 }, st = 0;
    CHECK(pipe2(ready, O_CLOEXEC) == 0 && pipe2(go, O_CLOEXEC) == 0);
    pid = vfork_exec(ready[1], go[0]);
    close(ready[1]);
    close(go[0]);
    CHECK(pid > 0 && rep->v[2] == 1);
    CHECK(pid > 0 && waitpid(pid, &st, WNOHANG) == 0);   /* its new image runs: we went on */
    CHECK(byte_from(ready[0]) == 'r');
    CHECK(write(go[1], "g", 1) == 1);
    if (pid > 0)
        reaped(pid, pid, 0);
    close(ready[0]);
    close(go[1]);
}

/* ---- groups ----------------------------------------------------------------------- */

/* /proc/<pid>/stat's process group and session: true if read. */
static bool stat_ids(pid_t pid, int *pgrp, int *sid)
{
    char path[48], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    char *end = strrchr(buf, ')');
    char state;
    int ppid;
    return end && sscanf(end + 1, " %c %d %d %d", &state, &ppid, pgrp, sid) == 4;
}

__attribute__((noinline)) static pid_t vfork_group(void)
{
    pid_t pid = vfork();
    if (pid == 0) {
        KID(setpgid(0, 0) == 0);
        _exit(3);
    }
    return pid;
}

void pt_fork_groups(void)
{
    map_report();
    pid_t mine = getpgrp();
    catch_usr1();
    pid_t pid = fork();
    if (pid == 0) {
        usr1s = 0;
        KID(setpgid(0, 0) == 0 && getpgrp() == getpid());
        rep->v[0] = 1;
        KID(until_usr1(KID_NS) && rep->v[7] == getpid());
        kid_end(0);
    }
    CHECK(pid > 0);
    if (pid > 0) {
        CHECK(until_set(&rep->v[0], KID_NS));
        CHECK(getpgid(pid) == pid);
        int pgrp = 0, sid = 0;
        CHECK(stat_ids(pid, &pgrp, &sid) && pgrp == pid && sid == getsid(0));
        CHECK(kill(-pid, SIGUSR1) == 0);
        reaped(-pid, pid, 0);
        CHECK(usr1s == 0);   /* the child's group, not ours */
        errno = 0;
        CHECK(kill(-pid, 0) == -1 && errno == ESRCH);   /* gone with it */
    }
    signal(SIGUSR1, SIG_DFL);

    pid = vfork_group();   /* its notice comes just before its end */
    CHECK(pid > 0);
    if (pid > 0)
        reaped(-pid, pid, 3);

    pid = fork();
    if (pid == 0) {
        KID(setsid() == getpid());
        rep->v[1] = 1;
        KID(until_set(&rep->v[2], KID_NS));
        kid_end(0);
    }
    CHECK(pid > 0);
    if (pid > 0) {
        CHECK(until_set(&rep->v[1], KID_NS));
        CHECK(getsid(pid) == pid && getpgid(pid) == pid);
        errno = 0;
        CHECK(setpgid(pid, mine) == -1 && errno == EPERM);   /* another session's now */
        rep->v[2] = 1;
        reaped(pid, pid, 0);
    }
}

/* ---- junk ------------------------------------------------------------------------- */

/* n bytes on our end of our signal channel, as a hostile child writes them. */
static status_t junk(const void *b, uint32_t n)
{
    return jam_channel_write(startup_handle(SR_SIGNALS), b, n, NULL, 0);
}

/* Everything the parent must drop, written before our true notice. */
static void junk_kid(pid_t mine)
{
    struct signal_group_msg g = { 0, SIGNAL_GROUP, getpid() };
    uint8_t b[16] = { 0 };
    KID(junk(b, 2) == OK);   /* no header */
    struct idl_req_hdr h = { 0, (IDL_ID_SIGNAL << 16) | 0xff };
    KID(junk(&h, sizeof(h)) == OK);   /* no such notice */
    g.txid = 7;
    KID(junk(&g, sizeof(g)) == OK);   /* a txid: a reply, not an event */
    g.txid = 0;
    memcpy(b, &g, sizeof(g));
    KID(junk(b, sizeof(b)) == OK);   /* bytes over */
    int32_t lies[] = { NOWHERE, 0, -mine, -1 };
    for (unsigned i = 0; i < sizeof(lies) / sizeof(lies[0]); i++) {
        g.pgid = lies[i];
        KID(junk(&g, sizeof(g)) == OK);
    }
    struct signal_send_msg s = { 0, SIGNAL_SEND, SIGUSR1, SIGNAL_CODE_USER, 0 };
    KID(junk(&s, sizeof(s)) == OK);   /* the starter's way: no signal for it */
    struct { struct signal_session_msg m; int32_t more; } __attribute__((packed)) ses = {
        { 0, SIGNAL_SESSION }, 1 };
    KID(junk(&ses, sizeof(ses)) == OK);
    handle_t ev = HANDLE_INVALID;
    KID(jam_event_create(&ev) == OK);
    KID(jam_channel_write(startup_handle(SR_SIGNALS), &h, sizeof(h), &ev, 1) ==
        ERR_NOT_SUPPORTED);   /* the parent's end takes no handles */
}

/* A program we start with a duplicate of our end of our signal channel:
 * its group notice naming our parent's group, which the parent would take
 * from us. */
static void forger(pid_t mine)
{
    handle_t dup = HANDLE_INVALID, proc = HANDLE_INVALID;
    KID(jam_handle_duplicate(startup_handle(SR_SIGNALS), RIGHT_SAME, &dup) == OK);
    char arg[16];
    snprintf(arg, sizeof(arg), "%d", (int)mine);
    const char *const argv[] = { "posixtest", "--child-notice", arg, NULL };
    struct spawn_handle x = { SR_SIGNALS, dup };
    struct spawn_args a = { .path = "bin/posixtest", .argc = 3, .argv = argv,
                            .job = startup_handle(SR_JOB), .extra = &x, .nextra = 1,
                            .ns = NS_ALL };
    status_t st = spawn(&a, &proc);
    KID(st == OK);
    struct process_info info = { 0 };
    if (st == OK) {
        KID(spawn_wait(proc, KID_NS, &info) == OK && !info.killed && info.exit_code == 0);
        jam_handle_close(proc);
    }
}

int pt_child_notice(int argc, char **argv)
{
    handle_t ch = startup_handle(SR_SIGNALS);
    return ch && argc >= 3 && signal_group(ch, (int32_t)atoi(argv[2])) == OK ? 0 : 1;
}

void pt_fork_junk(void)
{
    map_report();
    pid_t mine = getpgrp();
    catch_usr1();
    pid_t pid = fork();
    if (pid == 0) {
        junk_kid(mine);
        rep->v[0] = 1;
        KID(until_set(&rep->v[1], KID_NS));
        struct signal_group_msg g = { 0, SIGNAL_GROUP, NOWHERE };
        for (unsigned i = 0; i < FLOOD; i++)
            (void)junk(&g, sizeof(g));   /* a full queue drops some: what is left is enough */
        KID(setpgid(0, 0) == 0);   /* the true notice, behind the flood */
        rep->v[2] = 1;
        KID(until_set(&rep->v[3], KID_NS));
        forger(mine);
        rep->v[4] = 1;
        KID(until_set(&rep->v[5], KID_NS));
        kid_end(0);
    }
    CHECK(pid > 0);
    if (pid > 0) {
        CHECK(until_set(&rep->v[0], KID_NS));
        CHECK(getpgid(pid) == mine && getsid(pid) == getsid(0));
        rep->v[1] = 1;
        CHECK(until_set(&rep->v[2], KID_NS));
        uint64_t end = now() + KID_NS;   /* a look reads a few: the flood takes some */
        while (getpgid(pid) != pid && now() < end)
            usleep(1000);
        CHECK(getpgid(pid) == pid);
        rep->v[3] = 1;
        CHECK(until_set(&rep->v[4], KID_NS));
        CHECK(getpgid(pid) == pid);   /* not ours: the forger isn't the child */
        rep->v[5] = 1;
        reaped(pid, pid, 0);
        CHECK(usr1s == 0);
    }

    rep->v[6] = 0;
    pid = fork();
    if (pid == 0) {
        usr1s = 0;
        static uint8_t big[BIG];
        KID(junk(big, sizeof(big)) == OK);
        KID(setpgid(0, 0) == 0);   /* behind the junk: never read */
        rep->v[6] = 1;
        KID(until_usr1(KID_NS));
        kid_end(0);
    }
    CHECK(pid > 0);
    if (pid > 0) {
        CHECK(until_set(&rep->v[6], KID_NS));
        CHECK(getpgid(pid) == mine);   /* its notices aren't read any more */
        CHECK(kill(pid, SIGUSR1) == 0);
        reaped(pid, pid, 0);
    }
    signal(SIGUSR1, SIG_DFL);
}
