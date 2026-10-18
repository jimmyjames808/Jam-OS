/* posixtest's locks section: record locks between processes (D9,
 * docs/CR-PLAN.md's "RL notes": libc's reclock.c, extfs's locks.c), on a
 * file in /data:
 *   - a forked child's write lock in the way (F_SETLK EAGAIN), beside it
 *     not, F_GETLK and F_OFD_GETLK naming it and the child's pid; gone
 *     when the child exits;
 *   - the bytes a lock replaces, seen from the other process: a child's
 *     lock split by an unlock in its middle, two of its locks joined;
 *   - F_SETLKW and a blocking flock woken when the child lets go;
 *   - EDEADLK for whichever of two processes would close the circle;
 *     a wait cut by a signal (EINTR, no SA_RESTART) is forgotten: no
 *     EDEADLK from it afterwards;
 *   - the close rule (a close of any descriptor of the file, another
 *     open's or a dup's, drops the process's fcntl locks), a SIGKILL;
 *   - flock and fcntl locks never meet; fork's rules (fcntl locks not
 *     inherited, F_GETLK in the child names the parent; flock shared by
 *     the description, held while the child's copy lives);
 *   - OFD locks: two opens in one process conflict, an OFD lock meets
 *     the process's own fcntl lock, l_pid -1, F_OFD_ with an l_pid EINVAL,
 *     the description's close ends them;
 *   - SQLite's pattern (its PENDING, RESERVED and SHARED bytes: a writer's
 *     EXCLUSIVE waits for a reader's SHARED to go);
 *   - base's FileLockingTest's: a child started by posix_spawn of
 *     /proc/self/exe opens the file and locks it; its exit and its SIGKILL
 *     let go.
 * On a fat /data (no service keeps locks there) a lock is granted and
 * never kept, as before: the other process's lock is not seen. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ptfiles.h"

#define LF PT_DIR "/lockf"
#define SELF "/proc/self/exe"
/* SQLite's lock bytes (os_unix.c). */
#define PENDING      0x40000000
#define RESERVED     (PENDING + 1)
#define SHARED_FIRST (PENDING + 2)
#define SHARED_SIZE  510

extern char **environ;

static int setlk(int fd, int cmd, short type, off_t start, off_t len)
{
    struct flock l = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    return fcntl(fd, cmd, &l);
}

static int getlk(int fd, int cmd, short type, off_t start, off_t len, struct flock *out)
{
    *out = (struct flock){ .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    return fcntl(fd, cmd, out);
}

static bool busy(int fd, int cmd, short type, off_t start, off_t len)
{
    errno = 0;
    return setlk(fd, cmd, type, start, len) == -1 && errno == EAGAIN;
}

static void nap_ms(long ms)
{
    struct timespec t = { ms / 1000, ms % 1000 * 1000000 };
    while (nanosleep(&t, &t) && errno == EINTR)
        ;
}

static long ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- forked children ------------------------------------------------------------- */

/* A child and its two pipes: up (it says), down (it hears; EOF ends it). */
struct kid {
    pid_t pid;
    int   up, down;
};

static void say(int fd)
{
    char c = 1;
    (void)!write(fd, &c, 1);
}

static bool heard(int fd)
{
    char c;
    return read(fd, &c, 1) == 1;
}

static bool kid_start(struct kid *k, int (*fn)(int up, int down))
{
    int u[2], d[2];
    if (pipe(u) || pipe(d))
        return false;
    pid_t p = fork();
    if (p == 0) {
        close(u[0]);
        close(d[1]);
        _exit(fn(u[1], d[0]));
    }
    close(u[1]);
    close(d[0]);
    *k = (struct kid){ .pid = p, .up = u[0], .down = d[1] };
    return p > 0 && heard(k->up);   /* every child says once it holds its locks */
}

/* Its end (down closed): its exit code, or -1. */
static int kid_end(struct kid *k)
{
    close(k->down);
    int st = 0;
    pid_t p = waitpid(k->pid, &st, 0);
    close(k->up);
    return p == k->pid && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int kid_hold(int up, int down)
{
    int fd = open(LF, O_RDWR);
    if (fd < 0 || setlk(fd, F_SETLK, F_WRLCK, 10, 10))
        return 1;
    say(up);
    heard(down);
    return 0;
}

static int kid_split(int up, int down)
{
    int fd = open(LF, O_RDWR);
    if (fd < 0 || setlk(fd, F_SETLK, F_RDLCK, 0, 100))
        return 1;
    say(up);
    if (!heard(down) || setlk(fd, F_SETLK, F_UNLCK, 40, 20))
        return 2;
    say(up);
    if (!heard(down) || setlk(fd, F_SETLK, F_WRLCK, 0, 10) || setlk(fd, F_SETLK, F_WRLCK, 10, 10))
        return 3;
    say(up);
    heard(down);
    return 0;
}

/* Holds the whole file (fcntl, or flock), lets go 200 ms after the word. */
static int let_go(int up, int down, bool use_flock)
{
    int fd = open(LF, O_RDWR);
    if (fd < 0 || (use_flock ? flock(fd, LOCK_EX) : setlk(fd, F_SETLK, F_WRLCK, 0, 0)))
        return 1;
    say(up);
    if (!heard(down))
        return 2;
    nap_ms(200);
    if (use_flock ? flock(fd, LOCK_UN) : setlk(fd, F_SETLK, F_UNLCK, 0, 0))
        return 3;
    heard(down);
    return 0;
}

static int kid_let_go(int up, int down)
{
    return let_go(up, down, false);
}

static int kid_let_go_flock(int up, int down)
{
    return let_go(up, down, true);
}

/* Holds byte 1, then waits for byte 0 (the parent's): 0 when granted, 7 on
 * EDEADLK. */
static int kid_deadlock(int up, int down)
{
    int fd = open(LF, O_RDWR);
    if (fd < 0 || setlk(fd, F_SETLK, F_WRLCK, 1, 1))
        return 1;
    say(up);
    if (!heard(down))
        return 2;
    if (setlk(fd, F_SETLKW, F_WRLCK, 0, 1) == 0)
        return 0;
    return errno == EDEADLK ? 7 : 3;
}

/* Holds byte 0; at the word waits for byte 5 (the parent's, whose wait for
 * byte 0 a signal cut): 0 granted, 7 EDEADLK. */
static int kid_after_eintr(int up, int down)
{
    int fd = open(LF, O_RDWR);
    if (fd < 0 || setlk(fd, F_SETLK, F_WRLCK, 0, 1))
        return 1;
    say(up);
    if (!heard(down))
        return 2;
    if (setlk(fd, F_SETLKW, F_WRLCK, 5, 1) == 0)
        return 0;
    return errno == EDEADLK ? 7 : 3;
}

/* The close rule: locks through one open, then another open of the file
 * closed; again, then a dup closed. */
static int kid_close(int up, int down)
{
    int fd = open(LF, O_RDWR), other = open(LF, O_RDONLY);
    if (fd < 0 || other < 0 || setlk(fd, F_SETLK, F_WRLCK, 0, 0))
        return 1;
    say(up);
    if (!heard(down))
        return 2;
    close(other);
    say(up);
    if (!heard(down) || setlk(fd, F_SETLK, F_WRLCK, 0, 0))
        return 3;
    int d = dup(fd);
    say(up);
    if (d < 0 || !heard(down))
        return 4;
    close(d);
    say(up);
    heard(down);
    return 0;
}

static int kid_sleep(int up, int down)
{
    int fd = open(LF, O_RDWR);
    if (fd < 0 || setlk(fd, F_SETLK, F_WRLCK, 0, 0))
        return 1;
    say(up);
    heard(down);
    return 0;
}

/* SQLite's reader: SHARED (PENDING read-locked on the way), then gone at
 * the word. */
static int kid_reader(int up, int down)
{
    int fd = open(LF, O_RDWR);
    if (fd < 0 || setlk(fd, F_SETLK, F_RDLCK, PENDING, 1) ||
        setlk(fd, F_SETLK, F_RDLCK, SHARED_FIRST, SHARED_SIZE) ||
        setlk(fd, F_SETLK, F_UNLCK, PENDING, 1))
        return 1;
    say(up);
    if (!heard(down) || setlk(fd, F_SETLK, F_UNLCK, 0, 0))
        return 2;
    say(up);
    heard(down);
    return 0;
}

/* fork's rules, in the child of a parent holding fcntl [0, 10) and flock
 * on `inherited`. */
static int inherited = -1;

static int kid_forked(int up, int down)
{
    struct flock l;
    if (getlk(inherited, F_GETLK, F_WRLCK, 0, 10, &l) || l.l_type != F_WRLCK ||
        l.l_pid != getppid())
        return 1;   /* not inherited: the parent's */
    if (!busy(inherited, F_SETLK, F_WRLCK, 0, 10))
        return 2;
    if (flock(inherited, LOCK_EX | LOCK_NB))
        return 3;   /* the description's: shared */
    say(up);
    heard(down);
    return 0;
}

/* ---- a child by posix_spawn (base's FileLockingTest's) --------------------------- */

/* posixtest --child-locks <path>: open, lock the whole file, say so on
 * stdout, wait for stdin's end. */
int pt_child_locks(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    int fd = open(argv[2], O_RDWR);
    if (fd < 0 || setlk(fd, F_SETLK, F_WRLCK, 0, 0))
        return 1;
    (void)!write(1, "L", 1);
    char c;
    while (read(0, &c, 1) > 0)
        ;
    return 0;
}

static pid_t spawn_locker(int *to, int *from)
{
    int in[2], out[2];   /* close-on-exec: the child's copies are its dup2'd 0 and 1 alone */
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC))
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    char *argv[] = { "posixtest", "--child-locks", LF, NULL };
    pid_t pid = -1;
    int e = posix_spawn(&pid, SELF, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    *to = in[1];
    *from = out[0];
    char c = 0;
    return e == 0 && read(*from, &c, 1) == 1 && c == 'L' ? pid : -1;
}

/* ---- the cases --------------------------------------------------------------------- */

static void conflicts(int fd)
{
    struct kid k;
    struct flock l;
    CHECK(kid_start(&k, kid_hold));
    CHECK(busy(fd, F_SETLK, F_WRLCK, 15, 11));
    CHECK(busy(fd, F_SETLK, F_RDLCK, 19, 1));
    CHECK(setlk(fd, F_SETLK, F_RDLCK, 0, 10) == 0);   /* beside it */
    CHECK(getlk(fd, F_GETLK, F_WRLCK, 0, 0, &l) == 0 && l.l_type == F_WRLCK && l.l_start == 10 &&
          l.l_len == 10 && l.l_pid == k.pid);
    CHECK(getlk(fd, F_GETLK, F_RDLCK, 20, 5, &l) == 0 && l.l_type == F_UNLCK);
    CHECK(getlk(fd, F_OFD_GETLK, F_RDLCK, 10, 1, &l) == 0 && l.l_type == F_WRLCK &&
          l.l_pid == k.pid);
    CHECK(getlk(fd, F_GETLK, F_UNLCK, 0, 0, &l) == -1 && errno == EINVAL);
    CHECK(kid_end(&k) == 0);
    CHECK(setlk(fd, F_SETLK, F_WRLCK, 0, 0) == 0);   /* its exit let go */
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
}

static void split_and_join(int fd)
{
    struct kid k;
    struct flock l;
    CHECK(kid_start(&k, kid_split));
    CHECK(busy(fd, F_SETLK, F_WRLCK, 40, 20));
    say(k.down);
    CHECK(heard(k.up));
    CHECK(setlk(fd, F_SETLK, F_WRLCK, 40, 20) == 0);   /* its middle, unlocked */
    CHECK(busy(fd, F_SETLK, F_WRLCK, 39, 1));
    CHECK(busy(fd, F_SETLK, F_WRLCK, 60, 1));
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
    say(k.down);
    CHECK(heard(k.up));
    CHECK(getlk(fd, F_GETLK, F_RDLCK, 0, 0, &l) == 0 && l.l_type == F_WRLCK && l.l_start == 0 &&
          l.l_len == 20);   /* two locks joined */
    CHECK(getlk(fd, F_GETLK, F_WRLCK, 20, 1, &l) == 0 && l.l_type == F_RDLCK && l.l_start == 20 &&
          l.l_len == 20);   /* the read lock's rest */
    CHECK(kid_end(&k) == 0);
}

/* For the section's line: the waits' times, and who got EDEADLK. */
static long        woke_ms[2];
static const char *deadlocked = "nobody";

static void waits(int fd, int fd2)
{
    struct kid k;
    CHECK(kid_start(&k, kid_let_go));
    say(k.down);
    long t0 = ms_now();
    CHECK(setlk(fd, F_SETLKW, F_WRLCK, 0, 1) == 0);
    long waited = ms_now() - t0;
    CHECK(waited >= 100);
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
    CHECK(kid_end(&k) == 0);
    CHECK(kid_start(&k, kid_let_go_flock));
    errno = 0;
    CHECK(flock(fd2, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    say(k.down);
    t0 = ms_now();
    CHECK(flock(fd2, LOCK_EX) == 0);
    long flocked = ms_now() - t0;
    CHECK(flocked >= 100);
    CHECK(flock(fd2, LOCK_UN) == 0);
    CHECK(kid_end(&k) == 0);
    woke_ms[0] = waited;
    woke_ms[1] = flocked;
}

static void on_alarm(int sig)
{
    (void)sig;
}

static void deadlocks(int fd)
{
    struct kid k;
    CHECK(setlk(fd, F_SETLK, F_WRLCK, 0, 1) == 0);
    CHECK(kid_start(&k, kid_deadlock));
    say(k.down);
    nap_ms(300);   /* the child's wait counted first, most likely */
    errno = 0;
    int r = setlk(fd, F_SETLKW, F_WRLCK, 1, 1);
    bool ours = r == -1 && errno == EDEADLK;
    CHECK(r == 0 || ours);
    if (ours)
        CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 1) == 0);   /* the child's wait goes on */
    int code = kid_end(&k);
    CHECK(ours ? code == 0 : code == 7);
    deadlocked = ours ? "the parent" : code == 7 ? "the child" : "nobody";
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);

    /* A wait cut by a signal is no wait: the child's for our byte 5 then
     * is no circle. */
    struct sigaction sa = { .sa_handler = on_alarm }, old;
    CHECK(sigaction(SIGALRM, &sa, &old) == 0);
    CHECK(setlk(fd, F_SETLK, F_WRLCK, 5, 1) == 0);
    CHECK(kid_start(&k, kid_after_eintr));
    struct itimerval it = { .it_value = { 0, 300000 } };
    CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
    errno = 0;
    CHECK(setlk(fd, F_SETLKW, F_WRLCK, 0, 1) == -1 && errno == EINTR);
    CHECK(sigaction(SIGALRM, &old, NULL) == 0);
    say(k.down);
    nap_ms(200);
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 5, 1) == 0);
    CHECK(kid_end(&k) == 0);
}

static void close_rule(int fd)
{
    struct kid k;
    CHECK(kid_start(&k, kid_close));
    CHECK(busy(fd, F_SETLK, F_WRLCK, 0, 1));
    say(k.down);
    CHECK(heard(k.up));   /* it closed its other open */
    CHECK(setlk(fd, F_SETLK, F_WRLCK, 0, 1) == 0);
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
    say(k.down);
    CHECK(heard(k.up));   /* locked again, and dup'd */
    CHECK(busy(fd, F_SETLK, F_WRLCK, 0, 1));
    say(k.down);
    CHECK(heard(k.up));   /* the dup closed */
    CHECK(setlk(fd, F_SETLK, F_WRLCK, 0, 1) == 0);
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
    CHECK(kid_end(&k) == 0);

    /* SIGKILL: its locks go with it. */
    CHECK(kid_start(&k, kid_sleep));
    CHECK(busy(fd, F_SETLK, F_WRLCK, 0, 1));
    CHECK(kill(k.pid, SIGKILL) == 0);
    int st = 0;
    CHECK(waitpid(k.pid, &st, 0) == k.pid && WIFSIGNALED(st));
    CHECK(setlk(fd, F_SETLK, F_WRLCK, 0, 1) == 0);
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
    close(k.up);
    close(k.down);
}

static void fork_rules(void)
{
    struct kid k;
    inherited = open(LF, O_RDWR);
    CHECK(inherited >= 0);
    CHECK(setlk(inherited, F_SETLK, F_WRLCK, 0, 10) == 0);
    CHECK(flock(inherited, LOCK_EX | LOCK_NB) == 0);   /* flock and fcntl never meet */
    CHECK(kid_start(&k, kid_forked));
    close(inherited);   /* our fcntl locks go; the description lives on in the child */
    int fd = open(LF, O_RDWR);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    CHECK(setlk(fd, F_SETLK, F_WRLCK, 0, 10) == 0);
    CHECK(kid_end(&k) == 0);
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0);   /* its last copy closed */
    CHECK(flock(fd, LOCK_UN) == 0);
    close(fd);
}

static void ofd(void)
{
    struct flock l;
    int a = open(LF, O_RDWR), b = open(LF, O_RDWR);
    CHECK(a >= 0 && b >= 0);
    CHECK(setlk(a, F_OFD_SETLK, F_WRLCK, 0, 10) == 0);
    CHECK(busy(b, F_OFD_SETLK, F_WRLCK, 5, 1));   /* two opens: two owners */
    CHECK(getlk(b, F_OFD_GETLK, F_RDLCK, 0, 0, &l) == 0 && l.l_type == F_WRLCK &&
          l.l_start == 0 && l.l_len == 10 && l.l_pid == -1);
    CHECK(busy(b, F_SETLK, F_WRLCK, 5, 1));   /* our own fcntl lock meets it */
    struct flock bad = { .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_pid = 1 };
    errno = 0;
    CHECK(fcntl(b, F_OFD_SETLK, &bad) == -1 && errno == EINVAL);
    close(a);   /* the description's close: its locks go */
    CHECK(setlk(b, F_OFD_SETLK, F_WRLCK, 5, 1) == 0);
    close(b);
}

static void sqlite(int fd)
{
    struct kid k;
    CHECK(kid_start(&k, kid_reader));
    /* The writer: SHARED beside the reader's, then RESERVED and PENDING;
     * EXCLUSIVE waits for the reader's SHARED. */
    CHECK(setlk(fd, F_SETLK, F_RDLCK, PENDING, 1) == 0);
    CHECK(setlk(fd, F_SETLK, F_RDLCK, SHARED_FIRST, SHARED_SIZE) == 0);
    CHECK(setlk(fd, F_SETLK, F_UNLCK, PENDING, 1) == 0);
    CHECK(setlk(fd, F_SETLK, F_WRLCK, RESERVED, 1) == 0);
    CHECK(setlk(fd, F_SETLK, F_WRLCK, PENDING, 1) == 0);
    CHECK(busy(fd, F_SETLK, F_WRLCK, SHARED_FIRST, SHARED_SIZE));
    say(k.down);
    CHECK(heard(k.up));
    CHECK(setlk(fd, F_SETLK, F_WRLCK, SHARED_FIRST, SHARED_SIZE) == 0);
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
    CHECK(kid_end(&k) == 0);
}

static void spawned(int fd)
{
    int to, from, st = 0;
    pid_t pid = spawn_locker(&to, &from);
    CHECK(pid > 0);
    CHECK(busy(fd, F_SETLK, F_WRLCK, 0, 0));
    CHECK(busy(fd, F_SETLK, F_RDLCK, 0, 0));
    close(to);   /* its exit */
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(from);
    CHECK(setlk(fd, F_SETLK, F_WRLCK, 0, 0) == 0);
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
    pid = spawn_locker(&to, &from);   /* its SIGKILL */
    CHECK(pid > 0);
    CHECK(busy(fd, F_SETLK, F_WRLCK, 0, 0));
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st));
    close(to);
    close(from);
    CHECK(setlk(fd, F_SETLK, F_RDLCK, 0, 0) == 0);
    CHECK(setlk(fd, F_SETLK, F_UNLCK, 0, 0) == 0);
}

/* fat: a lock granted, never kept, the other process's never seen. */
static void fat_granted(void)
{
    struct kid k;
    struct flock l;
    CHECK(kid_start(&k, kid_hold));
    int fd = open(LF, O_RDONLY);   /* fat: one writer a file */
    CHECK(fd >= 0);
    CHECK(setlk(fd, F_SETLK, F_RDLCK, 10, 10) == 0);
    CHECK(getlk(fd, F_GETLK, F_WRLCK, 0, 0, &l) == 0 && l.l_type == F_UNLCK);
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0);
    close(fd);
    CHECK(kid_end(&k) == 0);
    printf("posixtest: locks: /data is fat: locks granted, not kept\n");
}

void pt_locks(char **argv)
{
    (void)argv;
    if (!pt_files_start()) {
        printf("posixtest: locks: no /data: skipped\n");
        return;
    }
    int fd = open(LF, O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0 && write(fd, "0123456789", 10) == 10);
    if (!pt_data_ext()) {
        close(fd);
        fat_granted();
        pt_files_end();
        return;
    }
    int fd2 = open(LF, O_RDWR);
    CHECK(fd2 >= 0);
    conflicts(fd);
    split_and_join(fd);
    waits(fd, fd2);
    deadlocks(fd);
    close_rule(fd);
    fork_rules();
    ofd();
    sqlite(fd);
    spawned(fd);
    close(fd);
    close(fd2);
    pt_files_end();
    printf("posixtest: locks: F_SETLKW woken after %ld ms, flock after %ld ms (the holder let "
           "go at 200); EDEADLK for %s\n", woke_ms[0], woke_ms[1], deadlocked);
}
