/* posixtest's raw section, the ENOSYS log (user/posix/sys/enosys.c):
 *
 *   - off, a call no handler answers is -ENOSYS and nothing is logged;
 *   - on, the first such call writes one line naming the program, the
 *     number and Linux's name for it; the same number again, from this
 *     thread or another, writes nothing; a new number writes a line; a
 *     number with no name, and a negative one, are still lines;
 *   - an option (the layer's own hook), an ioctl request nothing
 *     understands, a path and a library dlopen() was asked for (it always
 *     fails: a static program) are each logged once; the terminal probes
 *     (isatty) are not gaps;
 *   - the table is bounded: after 256 different gaps one line says so and
 *     no more are made;
 *   - the environment turns it on at the program's first gap: a child
 *     started with JAMOS_ENOSYS_LOG=1 logs, one without it, or with "0",
 *     does not.
 *
 * It reaches into the layer for the log's hooks (calls.h: the test's one
 * look inside, as fileskid.c's is fd.h). The lines themselves are in the
 * kernel log, where a person can read them: the RESULTS box and the
 * serial log hold "enosys: posixtest: syscall 163 (acct)". */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../sys/calls.h"
#include "pt.h"

#define SELF        "/boot/bin/posixtest"
#define TABLE       256   /* the log's table (enosys.c's SEEN_MAX) */
#define FAR_CALL    20000 /* numbers far past Linux's last: never answered */
#define UNKNOWN_IOC 0x7f01

extern char **environ;

/* A call no handler answers, made with the number only known at run time
 * (so musl's syscall() names every handler). */
static long raw_call(long n)
{
    errno = 0;
    return syscall(n, 0, 0, 0, 0, 0, 0);
}

static bool ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

static void *other_thread(void *arg)
{
    (void)arg;
    raw_call(SYS_acct);
    return NULL;
}

/* The log's first lines: off, on, once, a new number, a thread, names. */
static void check_calls(void)
{
    char last[256];
    lx_enosys_set(0);
    unsigned n0 = lx_enosys_lines();
    CHECK(raw_call(SYS_acct) == -1 && errno == ENOSYS);
    CHECK(lx_enosys_lines() == n0);   /* off: no line */

    lx_enosys_set(1);
    n0 = lx_enosys_lines();
    CHECK(raw_call(SYS_acct) == -1 && errno == ENOSYS);   /* the answer is the same */
    CHECK(lx_enosys_lines() == n0 + 1);
    char want[96];
    snprintf(want, sizeof(want), ": syscall %d (acct)\n", (int)SYS_acct);
    lx_enosys_last(last, sizeof(last));
    CHECK(!strncmp(last, "enosys: ", 8));
    CHECK(program_invocation_short_name && strstr(last, program_invocation_short_name));
    CHECK(ends_with(last, want));

    CHECK(raw_call(SYS_acct) == -1 && errno == ENOSYS);
    CHECK(lx_enosys_lines() == n0 + 1);   /* the same number: not twice */
    pthread_t t;
    CHECK(pthread_create(&t, NULL, other_thread, NULL) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(lx_enosys_lines() == n0 + 1);   /* nor from another thread */

    CHECK(raw_call(SYS_swapoff) == -1 && errno == ENOSYS);
    CHECK(lx_enosys_lines() == n0 + 2);   /* a different number is a new line */
    snprintf(want, sizeof(want), ": syscall %d (swapoff)\n", (int)SYS_swapoff);
    lx_enosys_last(last, sizeof(last));
    CHECK(ends_with(last, want));

    CHECK(raw_call(9999) == -1 && errno == ENOSYS);
    lx_enosys_last(last, sizeof(last));
    CHECK(ends_with(last, ": syscall 9999\n"));   /* no name: no brackets */
    CHECK(raw_call(-5) == -1 && errno == ENOSYS);
    lx_enosys_last(last, sizeof(last));
    CHECK(ends_with(last, ": syscall -5\n"));
    CHECK(lx_enosys_lines() == n0 + 4);

    /* A call that is answered is not a gap, however it fails. */
    n0 = lx_enosys_lines();
    CHECK(raw_call(SYS_getpid) > 0);
    errno = 0;
    CHECK(syscall(SYS_dup, 12345) == -1 && errno == EBADF);
    CHECK(lx_enosys_lines() == n0);
}

/* The hooks for options, ioctls and paths: once each. */
static void check_other_gaps(void)
{
    char last[256];
    lx_enosys_set(1);
    unsigned n0 = lx_enosys_lines();
    lx_enosys_option("prctl option", 77);
    lx_enosys_option("prctl option", 77);
    CHECK(lx_enosys_lines() == n0 + 1);
    lx_enosys_last(last, sizeof(last));
    CHECK(ends_with(last, ": prctl option 77\n"));
    lx_enosys_option("prctl option", 78);
    lx_enosys_option("futex operation", 77);   /* the same value of another call */
    CHECK(lx_enosys_lines() == n0 + 3);

    n0 = lx_enosys_lines();
    lx_enosys_path("/proc/cpuinfo");
    lx_enosys_path("/proc/cpuinfo");
    lx_enosys_path("/proc/meminfo");
    CHECK(lx_enosys_lines() == n0 + 2);
    lx_enosys_last(last, sizeof(last));
    CHECK(ends_with(last, ": path /proc/meminfo\n"));

    /* dlopen: fails (a static program), and the library is a gap, once. */
    n0 = lx_enosys_lines();
    CHECK(dlopen("libjam-test.so.1", RTLD_NOW) == NULL);
    CHECK(dlopen("libjam-test.so.1", RTLD_NOW) == NULL);
    CHECK(dlerror() != NULL);
    CHECK(lx_enosys_lines() == n0 + 1);
    lx_enosys_last(last, sizeof(last));
    CHECK(ends_with(last, ": dlopen libjam-test.so.1\n"));
    CHECK(dlopen(NULL, RTLD_NOW) == NULL);
    CHECK(lx_enosys_lines() == n0 + 2);

    /* ioctl: through the descriptor table, on a pipe, which understands none. */
    int p[2];
    CHECK(pipe(p) == 0);
    n0 = lx_enosys_lines();
    int arg = 0;
    errno = 0;
    CHECK(ioctl(p[0], UNKNOWN_IOC, &arg) == -1 && errno == ENOTTY);
    CHECK(ioctl(p[0], UNKNOWN_IOC, &arg) == -1 && errno == ENOTTY);
    CHECK(lx_enosys_lines() == n0 + 1);
    lx_enosys_last(last, sizeof(last));
    CHECK(ends_with(last, ": ioctl 0x7f01\n"));
    CHECK(isatty(p[0]) == 0);   /* TCGETS: a probe, not a gap */
    CHECK(lx_enosys_lines() == n0 + 1);
    close(p[0]);
    close(p[1]);
}

/* The table is bounded. */
static void check_bounded(void)
{
    char last[256];
    lx_enosys_set(1);
    unsigned n0 = lx_enosys_lines();
    for (int i = 0; i < TABLE + 44; i++)
        raw_call(FAR_CALL + i);
    CHECK(lx_enosys_lines() == n0 + TABLE + 1);   /* the table's worth, then the one notice */
    lx_enosys_last(last, sizeof(last));
    CHECK(strstr(last, "more than 256 different gaps"));
    raw_call(FAR_CALL + 1000);
    CHECK(lx_enosys_lines() == n0 + TABLE + 1);   /* and no more */
    lx_enosys_set(0);   /* forgets the table: the log works again if it is turned on */
    lx_enosys_set(1);
    n0 = lx_enosys_lines();
    raw_call(FAR_CALL + 1000);
    CHECK(lx_enosys_lines() == n0 + 1);
    lx_enosys_set(0);
}

/* A child that makes the same unknown call twice and exits 0 if it logged
 * exactly the lines argv[3] says (the environment decided). */
int pt_child_raw(int argc, char **argv)
{
    if (argc < 4 || strcmp(argv[2], "enosys"))
        return 100;
    unsigned want = (unsigned)strtoul(argv[3], NULL, 10);
    unsigned n0 = lx_enosys_lines();
    raw_call(SYS_acct);
    raw_call(SYS_acct);
    CHECK(lx_enosys_lines() - n0 == want);
    return pt_failures() ? 1 : 0;
}

static int run_child(const char *const *envp, const char *lines)
{
    char *argv[] = { "posixtest", "--child-raw", "enosys", (char *)lines, NULL };
    pid_t pid;
    if (posix_spawn(&pid, SELF, NULL, NULL, argv, (char *const *)envp) != 0)
        return -1;
    int s;
    if (waitpid(pid, &s, 0) != pid)
        return -1;
    return WIFEXITED(s) ? WEXITSTATUS(s) : -2;
}

/* The environment: JAMOS_ENOSYS_LOG=1 turns the log on. */
static void check_environment(void)
{
    const char *on[] = { "JAMOS_ENOSYS_LOG=1", NULL };
    const char *zero[] = { "JAMOS_ENOSYS_LOG=0", NULL };
    const char *empty[] = { "JAMOS_ENOSYS_LOG=", NULL };
    const char *none[] = { "PATH=/bin", NULL };
    CHECK(run_child(on, "1") == 0);     /* two calls, one line */
    CHECK(run_child(zero, "0") == 0);
    CHECK(run_child(empty, "0") == 0);
    CHECK(run_child(none, "0") == 0);
}

void pt_raw_log(void)
{
    check_calls();
    check_other_gaps();
    check_bounded();
    check_environment();
}
