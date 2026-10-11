/* posixtest's process section (docs/M13-PLAN.md, track B3): posix_spawn,
 * the waits, the ids, process groups, execve, system and popen.
 *
 *   - 200 children spawned at once, each sleeping a little, reaped in
 *     whatever order they end (waitpid(-1)), each with its own status;
 *     then ECHILD;
 *   - WNOHANG: 0 while they live, each reaped once as it ends;
 *   - process groups: a new group and children put in it, reaped by
 *     group (waitpid(-pgid)) and by our own (waitpid(0)); setpgid and
 *     getpgid on a child, a session made with POSIX_SPAWN_SETSID;
 *   - the ids a child sees: getppid (our pid), getpgid, getsid;
 *   - ends: an exit code, killed (SIGKILL), a signal's code; waitid with
 *     WNOWAIT, then reaped;
 *   - descriptors: one inherited, one moved by posix_spawn_file_actions,
 *     an FD_CLOEXEC one not inherited;
 *   - execve as spawn, wait, exit; posix_spawnp through PATH; fork and
 *     vfork ENOSYS; a missing program ENOENT;
 *   - system and popen (through bin/sh, and test/ls for `ls`), the
 *     listing read whole and compared with fs_readdir's;
 *   - a hand-over cut short: the reader gives up at once and closes what
 *     it was given; a parent killed while it spawns leaves no child
 *     hanging (every process in its job ends);
 *   - a spawn that fails (no such program) takes nothing of ours with it:
 *     a pipe's bytes read but not taken are still ours to read. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <posixfds.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define SELF        "/boot/bin/posixtest"
#define MANY        200
#define EXIT_SIGNAL 0x10000   /* the layer's code for "ended by signal N" (proc.h) */

extern char **environ;

static void nap_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

/* ---- the children's side ---------------------------------------------------------- */

static int child_ids(char **argv)
{
    pid_t me = getpid();
    CHECK(getppid() == atoi(argv[3]));
    pid_t pgid = strcmp(argv[4], "self") ? atoi(argv[4]) : me;
    pid_t sid = strcmp(argv[5], "self") ? atoi(argv[5]) : me;
    CHECK(getpgid(0) == pgid && getpgrp() == pgid);
    CHECK(getsid(0) == sid);
    return pt_failures() ? 1 : 0;
}

static int child_fds(char **argv)
{
    for (int i = 3; argv[i] && strcmp(argv[i], "--"); i++)
        CHECK(write(atoi(argv[i]), "ok", 2) == 2);
    int i = 3;
    while (argv[i] && strcmp(argv[i], "--"))
        i++;
    for (i += argv[i] != NULL; argv[i]; i++) {
        errno = 0;
        CHECK(fcntl(atoi(argv[i]), F_GETFD) == -1 && errno == EBADF);
    }
    return pt_failures() ? 1 : 0;
}

/* The spawner a test kills mid-spawn: children forever, each reaped. */
static _Noreturn int child_spawner(void)
{
    char *const argv[] = { "posixtest", "--child-proc", "exit", "0", NULL };
    for (;;) {
        pid_t pid;
        (void)posix_spawn(&pid, SELF, NULL, NULL, argv, environ);
        while (waitpid(-1, NULL, WNOHANG) > 0)
            continue;
    }
}

int pt_child_proc(int argc, char **argv)
{
    const char *what = argc > 2 ? argv[2] : "";
    if (!strcmp(what, "exit")) {
        if (argc > 4)
            nap_ms(atol(argv[4]));
        return atoi(argv[3]);
    }
    if (!strcmp(what, "ids") && argc == 6)
        return child_ids(argv);
    if (!strcmp(what, "fds"))
        return child_fds(argv);
    if (!strcmp(what, "kill"))
        jam_process_kill(startup_handle(SR_SELF_PROCESS));
    if (!strcmp(what, "sigexit"))
        jam_process_exit(EXIT_SIGNAL + atoi(argv[3]));
    if (!strcmp(what, "exec")) {
        char *const args[] = { "posixtest", "--child-proc", "exit", argv[3], NULL };
        execv(SELF, args);
        return 99;
    }
    if (!strcmp(what, "noexec")) {
        char *const args[] = { "nothing", NULL };
        execv("/boot/bin/no-such-program", args);
        return errno == ENOENT ? 7 : 8;
    }
    if (!strcmp(what, "spawner"))
        return child_spawner();
    if (!strcmp(what, "write") && argc > 3)   /* its text on descriptor 1 */
        return write(1, argv[3], strlen(argv[3])) == (ssize_t)strlen(argv[3]) ? 0 : 1;
    return 100;
}

/* ---- helpers ---------------------------------------------------------------------- */

/* Spawn this program as `posixtest --child-proc <args...>`; its pid, or
 * -1. */
static pid_t spawn_self(const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                        const char *const *args)
{
    char *argv[16] = { "posixtest", "--child-proc" };
    int n = 2;
    for (; args[n - 2] && n < 15; n++)
        argv[n] = (char *)args[n - 2];
    argv[n] = NULL;
    pid_t pid = -1;
    int e = posix_spawn(&pid, SELF, fa, at, argv, environ);
    CHECK(e == 0);
    return e ? -1 : pid;
}

static pid_t spawn_exit(int code, long nap)
{
    char c[16], ms[16];
    snprintf(c, sizeof(c), "%d", code);
    snprintf(ms, sizeof(ms), "%ld", nap);
    const char *args[] = { "exit", c, ms, NULL };
    return spawn_self(NULL, NULL, args);
}

/* Wait for pid; its status, or -1. */
static int reap(pid_t pid)
{
    int s;
    pid_t got = waitpid(pid, &s, 0);
    CHECK(got == pid);
    return got == pid ? s : -1;
}

/* ---- the checks ------------------------------------------------------------------- */

static void check_many(void)
{
    static pid_t pids[MANY];
    static bool seen[MANY];
    for (int i = 0; i < MANY; i++) {
        pids[i] = spawn_exit(i % 250, (i * 37) % 50);
        seen[i] = false;
    }
    for (int n = 0; n < MANY; n++) {
        int s;
        pid_t p = waitpid(-1, &s, 0);
        int i = 0;
        while (i < MANY && pids[i] != p)
            i++;
        CHECK(i < MANY && !seen[i]);
        if (i == MANY || seen[i])
            continue;
        seen[i] = true;
        CHECK(WIFEXITED(s) && WEXITSTATUS(s) == i % 250);
    }
    errno = 0;
    CHECK(waitpid(-1, NULL, 0) == -1 && errno == ECHILD);
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
}

static void check_nohang(void)
{
    enum { N = 20 };
    for (int i = 0; i < N; i++)
        spawn_exit(i, 100 + i * 5);
    CHECK(waitpid(-1, NULL, WNOHANG) == 0);
    int reaped = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (reaped < N) {
        int s;
        pid_t p = waitpid(-1, &s, WNOHANG);
        CHECK(p >= 0);
        if (p > 0) {
            CHECK(WIFEXITED(s));
            reaped++;
        } else if (p == 0) {
            nap_ms(5);
        }
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (p < 0 || t.tv_sec - t0.tv_sec > 20)
            break;
    }
    CHECK(reaped == N);
}

/* A child with POSIX_SPAWN_SETPGROUP pgrp. */
static pid_t spawn_in_group(pid_t pgrp, long nap)
{
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, pgrp);
    char ms[16];
    snprintf(ms, sizeof(ms), "%ld", nap);
    const char *args[] = { "exit", "5", ms, NULL };
    pid_t pid = spawn_self(NULL, &at, args);
    posix_spawnattr_destroy(&at);
    return pid;
}

static void check_groups(void)
{
    pid_t lead = spawn_in_group(0, 50), ours[5], theirs[5];
    CHECK(lead > 0 && getpgid(lead) == lead);
    for (int i = 0; i < 5; i++) {
        theirs[i] = spawn_in_group(lead, i * 10);
        CHECK(getpgid(theirs[i]) == lead);
        ours[i] = spawn_exit(6, i * 10);
        CHECK(getpgid(ours[i]) == getpgrp());
    }
    for (int i = 0; i < 6; i++) {
        int s;
        pid_t p = waitpid(-lead, &s, 0);
        bool member = p == lead;
        for (int j = 0; j < 5; j++)
            member |= p == theirs[j];
        CHECK(member);
        CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 5);
    }
    errno = 0;
    CHECK(waitpid(-lead, NULL, WNOHANG) == -1 && errno == ECHILD);
    for (int i = 0; i < 5; i++) {
        int s;
        CHECK(waitpid(0, &s, 0) > 0 && WIFEXITED(s) && WEXITSTATUS(s) == 6);
    }
    /* setpgid on a live child: a group of its own, then back to ours. */
    pid_t c = spawn_exit(0, 200);
    CHECK(setpgid(c, c) == 0 && getpgid(c) == c);
    CHECK(setpgid(c, getpgrp()) == 0 && getpgid(c) == getpgrp());
    errno = 0;
    CHECK(setpgid(c, 999999) == -1 && errno == EPERM);
    CHECK(getpgid(1) == -1 && errno == ESRCH);   /* not ours */
    CHECK(reap(c) == 0);
}

static void check_ids(void)
{
    char me[16], pg[16], sid[16];
    snprintf(me, sizeof(me), "%d", getpid());
    snprintf(pg, sizeof(pg), "%d", getpgrp());
    snprintf(sid, sizeof(sid), "%d", getsid(0));
    CHECK(getpid() > 0 && getpid() < (1 << 22) && getppid() >= 1);
    const char *same[] = { "ids", me, pg, sid, NULL };
    CHECK(reap(spawn_self(NULL, NULL, same)) == 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
    const char *leader[] = { "ids", me, "self", "self", NULL };
    pid_t c = spawn_self(NULL, &at, leader);
    CHECK(getsid(c) == c && getpgid(c) == c);
    CHECK(reap(c) == 0);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, getpgrp());
    pid_t no;
    char *const argv[] = { "posixtest", NULL };
    CHECK(posix_spawn(&no, SELF, NULL, &at, argv, environ) == EPERM);
    posix_spawnattr_destroy(&at);
}

static void check_ends(void)
{
    const char *kill_args[] = { "kill", NULL }, *sig_args[] = { "sigexit", "15", NULL };
    int s = reap(spawn_self(NULL, NULL, kill_args));
    CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGKILL);
    s = reap(spawn_self(NULL, NULL, sig_args));
    CHECK(WIFSIGNALED(s) && WTERMSIG(s) == 15);
    pid_t c = spawn_exit(33, 0);
    siginfo_t si;
    CHECK(waitid(P_PID, c, &si, WEXITED | WNOWAIT) == 0);
    CHECK(si.si_pid == c && si.si_signo == SIGCHLD && si.si_code == CLD_EXITED &&
          si.si_status == 33);
    CHECK(waitid(P_ALL, 0, &si, WEXITED | WNOHANG) == 0 && si.si_pid == c);   /* reaped now */
    memset(&si, 0xff, sizeof(si));
    errno = 0;
    CHECK(waitid(P_ALL, 0, &si, WEXITED | WNOHANG) == -1 && errno == ECHILD);
    CHECK(waitid(P_ALL, 0, &si, WNOHANG) == -1 && errno == EINVAL);   /* no WEXITED & co */
    CHECK(waitpid(424242, NULL, 0) == -1 && errno == ECHILD);
}

static void check_exec(void)
{
    const char *exec_args[] = { "exec", "42", NULL }, *noexec[] = { "noexec", NULL };
    int s = reap(spawn_self(NULL, NULL, exec_args));
    CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 42);
    s = reap(spawn_self(NULL, NULL, noexec));
    CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 7);
    pid_t p;
    char *const argv[] = { "posixtest", "--child-proc", "exit", "9", NULL };
    CHECK(posix_spawnp(&p, "posixtest", NULL, NULL, argv, environ) == 0);   /* PATH */
    s = reap(p);
    CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 9);
    CHECK(posix_spawn(&p, "/boot/bin/no-such-program", NULL, NULL, argv, environ) == ENOENT);
    CHECK(posix_spawnp(&p, "no-such-program", NULL, NULL, argv, environ) == ENOENT);
    errno = 0;
    CHECK(fork() == -1 && errno == ENOSYS);
    errno = 0;
    CHECK(vfork() == -1 && errno == ENOSYS);
}

static void check_fds(void)
{
    int p[2], q[2];
    CHECK(pipe(p) == 0 && pipe2(q, O_CLOEXEC) == 0);
    char w[16], moved[16] = "9", cx[16];
    snprintf(w, sizeof(w), "%d", p[1]);
    snprintf(cx, sizeof(cx), "%d", q[1]);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, q[1], 9);   /* a CLOEXEC one moved: inherited */
    const char *args[] = { "fds", w, moved, "--", cx, NULL };
    int s = reap(spawn_self(&fa, NULL, args));
    posix_spawn_file_actions_destroy(&fa);
    CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 0);
    close(p[1]);
    close(q[1]);
    char buf[8] = { 0 };
    CHECK(read(p[0], buf, sizeof(buf)) == 2 && !memcmp(buf, "ok", 2));
    CHECK(read(p[0], buf, sizeof(buf)) == 0);   /* the child's copy is gone too */
    CHECK(read(q[0], buf, sizeof(buf)) == 2 && read(q[0], buf, sizeof(buf)) == 0);
    close(p[0]);
    close(q[0]);
}

/* fs_readdir's listing of dir, a name a line, into buf (cap bytes). */
static size_t listing(const char *dir, char *buf, size_t cap)
{
    static struct fs_entry e;
    size_t n = 0;
    for (uint32_t i = 0; fs_readdir(dir, i, &e) == OK; i++)
        n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, "%s\n", e.name);
    return n;
}

static void check_popen(const char *cmd, const char *dir)
{
    static char want[16384], got[16384];
    size_t n = listing(dir, want, sizeof(want)), m = 0, r;
    CHECK(n > 0 && n < sizeof(want));
    FILE *f = popen(cmd, "r");
    CHECK(f != NULL);
    if (!f)
        return;
    while ((r = fread(got + m, 1, sizeof(got) - 1 - m, f)) > 0)
        m += r;
    got[m] = '\0';
    CHECK(m == n && !memcmp(got, want, n));
    CHECK(pclose(f) == 0);
}

static void check_shell(void)
{
    setenv("PATH", "/boot/test:/boot/bin", 1);
    int s = system("posixtest --child-proc exit 3");
    CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 3);
    s = system("no-such-program");
    CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 127);
    s = system("posixtest | ls");
    CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 2);   /* not a simple command: refused */
    check_popen("ls /boot", "/boot");
    check_popen("ls '/boot/bin'", "/boot/bin");
}

/* ---- a hand-over cut short -------------------------------------------------------- */

static unsigned sink_fds, sink_resets;
static handle_t sink_h[POSIXFDS_KIND_HANDLES];

static status_t cut_fd(void *ctx, const struct posixfds_fd *rec, const void *data,
                       const handle_t *hs)
{
    (void)ctx;
    (void)data;
    sink_fds++;
    for (unsigned i = 0; i < rec->nhandles; i++)
        sink_h[i] = hs[i];
    return OK;
}

static void cut_reset(void *ctx)
{
    (void)ctx;
    sink_resets++;
    jam_handle_close(sink_h[0]);
}

/* A first message, not the last, then its writer gone: what a child
 * reads when its starter dies after process_start mid-hand-over. */
static void check_cut_reader(void)
{
    handle_t r, w, a, b;
    CHECK(jam_channel_create(&r, &w) == OK && jam_channel_create(&a, &b) == OK);
    struct {
        struct posixfds_msg m;
        char cwd[8];
        struct posixfds_fd fd;
    } msg = {
        .m = { .magic = POSIXFDS_MAGIC, .version = POSIXFDS_VERSION, .seq = 0, .nfds = 1,
               .nhandles = 1, .cwd_len = 2 },
        .cwd = "/",
        .fd = { .fd = 1, .kind = POSIXFDS_PIPE, .nhandles = 1, .status = 1 },
    };
    CHECK(jam_channel_write(w, &msg, sizeof(msg), &b, 1) == OK);   /* no POSIXFDS_LAST */
    jam_handle_close(w);
    struct posixfds_state st;
    static char cwd[POSIXFDS_PATH_MAX];
    const struct posixfds_sink sink = { NULL, cut_fd, cut_reset };
    uint64_t t0 = now();
    CHECK(posixfds_read(r, &sink, &st, cwd) == ERR_INVALID_ARGS);
    CHECK(now() - t0 < NS_PER_S);   /* at once: never a wait for the rest */
    CHECK(sink_fds == 1 && sink_resets == 1);
    signals_t seen;   /* the handle it was given was closed: our end sees it */
    CHECK(jam_object_wait_one(a, SIG_PEER_CLOSED, now() + NS_PER_S, &seen) == OK);
    jam_handle_close(a);
    jam_handle_close(r);
}

/* The threads still alive in job j. */
static uint64_t job_threads(handle_t j)
{
    struct job_info ji;
    return jam_job_get_info(j, &ji) == OK ? ji.used[JOB_LIMIT_THREADS] : 0;
}

/* A parent killed at random points of its spawns: every process in its
 * job (it and all its children) is gone soon after, none left waiting
 * for a hand-over. */
static void check_cut_kill(void)
{
    for (int round = 0; round < 5; round++) {
        handle_t job, m;
        CHECK(jam_job_create(startup_handle(SR_JOB), 0, &job) == OK);
        const char *argv[] = { "posixtest", "--child-proc", "spawner", NULL };
        struct spawn_args a = { .path = "bin/posixtest", .argc = 3, .argv = argv, .job = job,
                                .stack_size = 8 << 20 };
        CHECK(spawn(&a, &m) == OK);
        nap_ms(20 + (long)(os_random_u32() % 80));
        CHECK(jam_process_kill(m) == OK);
        uint64_t end = now() + 10 * NS_PER_S;
        while (job_threads(job) && now() < end)
            nap_ms(10);
        CHECK(job_threads(job) == 0);
        (void)jam_job_kill(job);   /* whatever is left (nothing, if the check held) */
        jam_handle_close(m);
        jam_handle_close(job);
    }
}

/* ---- what a spawn hands on, or keeps ------------------------------------------------ */

/* A spawn that can't find its program has given the child nothing: a
 * pipe's leftover (the bytes of a message a read didn't take) is still
 * ours. musl's execvp is one execve per PATH directory, so every
 * directory before the right one is such a spawn. */
static void check_failed_spawn_keeps(void)
{
    int p[2];
    CHECK(pipe2(p, O_NONBLOCK) == 0);
    char buf[8] = { 0 };
    CHECK(write(p[1], "abcd", 4) == 4 && read(p[0], buf, 1) == 1);   /* "bcd" left over */
    pid_t pid;
    char *const argv[] = { "nothing", NULL };
    CHECK(posix_spawn(&pid, "/boot/bin/no-such-program", NULL, NULL, argv, environ) == ENOENT);
    memset(buf, 0, sizeof(buf));
    CHECK(read(p[0], buf, sizeof(buf)) == 3 && !strcmp(buf, "bcd"));
    close(p[0]);
    close(p[1]);
}

void pt_proc(char **argv)
{
    (void)argv;
    check_many();
    check_nohang();
    check_groups();
    check_ids();
    check_ends();
    check_exec();
    check_fds();
    check_shell();
    check_cut_reader();
    check_cut_kill();
    check_failed_spawn_keeps();
}
