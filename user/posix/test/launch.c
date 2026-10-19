/* posixtest's launch section (docs/CR-PLAN.md section 5, track CS1): what
 * Chromium's launcher needs of posix_spawn.
 *   - A big command line: a child with 500 arguments of 128 bytes and 64
 *     KiB of environment sees every byte of them, started from the boot
 *     image and from /proc/self/exe (libjam's spawn carries them in a VMO,
 *     SR_ARGS, when they don't fit the startup message); more than 2 MiB
 *     in all is E2BIG.
 *   - posix_spawn_file_actions_addclosefrom_np: the child holds exactly
 *     the descriptors the actions put there (dup2 at 3 and 4, and at 9
 *     after the closefrom), as /proc/self/fd lists them, and none of the
 *     others we had open; a negative start is EBADF.
 *   - Malformed SR_ARGS VMOs from a starter (docs/CR-REVIEW-POSIX.md,
 *     "Tests"): a bad magic or version, strings past the VMO, the pointers'
 *     room past 2 MiB (finding 4's wrap), a path without its NUL, ...: the
 *     child starts with argc 0 and says so in the log, never crashes.
 * Child modes `sealed` and `sealed-bare` are utest's (execsvc.c): what a
 * program started through init's /svc/exec is given (CR-REVIEW-POSIX Q1);
 * `from`, `from-out` and `inherit` too: a caller's directory and
 * environment kept for a child that isn't raised (docs/CR-PLAN.md,
 * "SX"); and `narrow`, `narrowed` and `raised`: a child given less than
 * its starter has no image of their program, so what it starts through
 * /svc/exec is raised (docs/CR-PLAN.md, "TS"). */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <jamos/spawn.h>
#include <spawn.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pt.h"

#define NARGS   500   /* the child's arguments after its mode */
#define ARG_LEN 128   /* each with its NUL */
#define NENV    256   /* environment strings */
#define ENV_LEN 256   /* each with its NUL: 64 KiB in all */

/* Argument i (ARG_LEN bytes with its NUL) into buf. */
static void make_arg(char *buf, int i)
{
    int n = snprintf(buf, ARG_LEN, "arg%d:", i);
    memset(buf + n, 'a' + i % 26, ARG_LEN - 1 - (size_t)n);
    buf[ARG_LEN - 1] = 0;
}

/* Environment string i (ENV_LEN bytes with its NUL) into buf. */
static void make_env(char *buf, int i)
{
    int n = snprintf(buf, ENV_LEN, "LAUNCH%d=", i);
    memset(buf + n, 'A' + i % 26, ENV_LEN - 1 - (size_t)n);
    buf[ENV_LEN - 1] = 0;
}

/* posix_spawn of prog with argv and envp; its exit status, or -1 if it
 * didn't start (*err: posix_spawn's answer). */
static int run(const char *prog, posix_spawn_file_actions_t *fa, char **argv, char **envp,
               int *err)
{
    pid_t pid = -1;
    int status = -1;
    *err = posix_spawn(&pid, prog, fa, NULL, argv, envp);
    if (*err)
        return -1;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* A child of prog given NARGS arguments and NENV environment strings
 * (posixtest --child-launch args ...) that checks every one. */
static void check_big_args(const char *prog)
{
    char **argv = calloc(NARGS + 4, sizeof(char *)), **envp = calloc(NENV + 1, sizeof(char *));
    char *args = malloc((size_t)NARGS * ARG_LEN), *envs = malloc((size_t)NENV * ENV_LEN);
    CHECK(argv && envp && args && envs);
    if (!argv || !envp || !args || !envs)
        return;
    argv[0] = "posixtest";
    argv[1] = "--child-launch";
    argv[2] = "args";
    for (int i = 0; i < NARGS; i++) {
        argv[3 + i] = args + (size_t)i * ARG_LEN;
        make_arg(argv[3 + i], i);
    }
    for (int i = 0; i < NENV; i++) {
        envp[i] = envs + (size_t)i * ENV_LEN;
        make_env(envp[i], i);
    }
    int err, code = run(prog, NULL, argv, envp, &err);
    printf("posixtest: launch: %s with %d arguments (%d KiB) and %d KiB of environment: "
           "%s %d\n", prog, NARGS + 3, NARGS * ARG_LEN / 1024, NENV * ENV_LEN / 1024,
           err ? "error" : "exit", err ? err : code);
    CHECK(err == 0 && code == 0);
    free(argv);
    free(envp);
    free(args);
    free(envs);
}

/* More than 2 MiB of arguments (Linux's usual ARG_MAX): E2BIG. */
static void check_too_big(void)
{
    size_t len = 1u << 20;
    char *big = malloc(len);
    CHECK(big != NULL);
    if (!big)
        return;
    memset(big, 'x', len - 1);
    big[len - 1] = 0;
    char *argv[] = { "posixtest", big, big, NULL };
    char *envp[] = { NULL };
    int err;
    run("/bin/posixtest", NULL, argv, envp, &err);
    CHECK(err == E2BIG);
    free(big);
}

/* The child's descriptors from 3 up (posixtest --child-launch fds) must
 * be 3, 4 and 9: the remapped ones. */
static void check_closefrom(void)
{
    int up[2], nul = open("/dev/null", O_RDONLY);
    CHECK(nul >= 0 && pipe(up) == 0);
    /* Open ones of ours a child would get but for the closefrom. */
    int extra[] = { dup(nul), dup(nul), dup2(nul, 30), dup2(nul, 100) };
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
        CHECK(extra[i] >= 5);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    CHECK(posix_spawn_file_actions_addclosefrom_np(&fa, -1) == EBADF);
    posix_spawn_file_actions_adddup2(&fa, up[1], 3);   /* it answers there */
    posix_spawn_file_actions_adddup2(&fa, nul, 4);
    posix_spawn_file_actions_adddup2(&fa, nul, 7);     /* gone with the rest */
    CHECK(posix_spawn_file_actions_addclosefrom_np(&fa, 5) == 0);
    posix_spawn_file_actions_adddup2(&fa, nul, 9);     /* after it: kept */
    char *argv[] = { "posixtest", "--child-launch", "fds", NULL };
    pid_t pid = -1;
    int status = -1, e = posix_spawn(&pid, "/bin/posixtest", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(up[1]);
    char got[64] = { 0 };
    ssize_t n = e ? -1 : read(up[0], got, sizeof(got) - 1);
    CHECK(e == 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    printf("posixtest: launch: closefrom 5 left the child %s\n", n > 0 ? got : "nothing");
    CHECK(n > 0 && !strcmp(got, "3 4 9"));
    close(up[0]);
    close(nul);
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
        close(extra[i]);
}

/* SR_ARGS's VMO as a starter wrote it: the header hd, then the body's n
 * bytes, in a VMO of size bytes (at least the header's). */
static handle_t args_vmo(const struct startup_args *hd, const void *body, size_t n,
                         uint64_t size)
{
    handle_t v = HANDLE_INVALID;
    status_t st = jam_vmo_create(size, 0, &v);
    if (st == OK)
        st = jam_vmo_write(v, 0, hd, sizeof(*hd));
    if (st == OK && n)
        st = jam_vmo_write(v, sizeof(*hd), body, n);
    CHECK(st == OK);
    return v;
}

/* This program started by libjam's spawn with no strings in its startup
 * message and v (consumed) as its SR_ARGS: its exit code, or -1 if it was
 * killed or never ended. */
static int run_with_args(handle_t v)
{
    struct spawn_handle x = { SR_ARGS, v };
    struct spawn_args a = { .path = "bin/posixtest", .job = startup_handle(SR_JOB),
                            .extra = &x, .nextra = 1 };
    handle_t proc = HANDLE_INVALID;
    if (spawn(&a, &proc) != OK)
        return -1;
    struct process_info info;
    status_t st = spawn_wait(proc, 30 * NS_PER_S, &info);
    if (st != OK)
        (void)jam_process_kill(proc);
    jam_handle_close(proc);
    return st == OK && !info.killed ? (int)info.exit_code : -1;
}

/* Malformed SR_ARGS VMOs (docs/CR-REVIEW-POSIX.md, "Tests"; finding 4's
 * wrap among them): each child starts with argc 0 (main's PT_NO_ARGS),
 * never killed; libjam's start says so in the log ("no usable argument
 * VMO"). A good VMO first, so the rig itself is known to work. */
static void check_bad_args(void)
{
    static const char good[] = "posixtest\0--child-launch\0ok";
    const struct startup_args ok = { STARTUP_ARGS_MAGIC, STARTUP_ARGS_VERSION, 3, 0,
                                     sizeof(good), 0 };
    CHECK(run_with_args(args_vmo(&ok, good, sizeof(good), 4096)) == 0);
    static const char one[] = "x", path[] = "x\0/abcd", rel[] = "x\0abc", nul[] = "x\0",
                      open_end[] = "abc", two[] = "a\0b";
    const struct {
        const char         *what;
        struct startup_args hd;
        const char         *body;
        size_t              n;
        uint64_t            size;
    } bad[] = {
        { "its magic", { STARTUP_ARGS_MAGIC ^ 1, STARTUP_ARGS_VERSION, 1, 0, 2, 0 }, one, 2, 4096 },
        { "its version", { STARTUP_ARGS_MAGIC, 2, 1, 0, 2, 0 }, one, 2, 4096 },
        { "strings past the VMO", { STARTUP_ARGS_MAGIC, 1, 1, 0, 1u << 20, 0 }, one, 2, 4096 },
        { "the pointers' wrap (262,143)", { STARTUP_ARGS_MAGIC, 1, 262143, 0, 2, 0 }, one, 2,
          4096 },
        { "the pointers' wrap (262,144)", { STARTUP_ARGS_MAGIC, 1, 262143, 1, 2, 0 }, one, 2,
          4096 },
        { "argc and envc of 2^31", { STARTUP_ARGS_MAGIC, 1, 1u << 31, 1u << 31, 2, 0 }, one, 2,
          4096 },
        { "a path without its NUL", { STARTUP_ARGS_MAGIC, 1, 1, 0, 2, 5 }, path, 7, 4096 },
        { "a relative path", { STARTUP_ARGS_MAGIC, 1, 1, 0, 2, 4 }, rel, 6, 4096 },
        { "a path of one byte", { STARTUP_ARGS_MAGIC, 1, 1, 0, 2, 1 }, nul, 3, 4096 },
        { "a string without its NUL", { STARTUP_ARGS_MAGIC, 1, 1, 0, 3, 0 }, open_end, 3, 4096 },
        { "more strings than bytes", { STARTUP_ARGS_MAGIC, 1, 1000, 0, 4, 0 }, two, 4, 4096 },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int code = run_with_args(args_vmo(&bad[i].hd, bad[i].body, bad[i].n, bad[i].size));
        if (code != PT_NO_ARGS)
            printf("posixtest: launch: SR_ARGS with %s: the child's code %d, not %d\n",
                   bad[i].what, code, PT_NO_ARGS);
        CHECK(code == PT_NO_ARGS);
    }
}

void pt_launch(char **argv __attribute__((unused)))
{
    check_big_args("/bin/posixtest");
    check_big_args("/proc/self/exe");
    check_too_big();
    check_closefrom();
    check_bad_args();
}

/* --child-launch args: argv[3 ..] and the environment as check_big_args
 * made them, every byte. 0 if so. */
static int child_args(int argc, char **argv)
{
    char want[ENV_LEN];
    int bad = argc != NARGS + 3;
    for (int i = 0; !bad && i < NARGS; i++) {
        make_arg(want, i);
        bad = strcmp(argv[3 + i], want) != 0;
    }
    int found = 0;
    for (char **e = environ; !bad && *e; e++) {
        int i;
        if (sscanf(*e, "LAUNCH%d=", &i) != 1 || i < 0 || i >= NENV)
            continue;   /* TZ, which libc adds */
        make_env(want, i);
        bad = strcmp(*e, want) != 0;
        found++;
    }
    if (bad || found != NENV)
        printf("posixtest: launch child: argc %d, %d environment strings of ours%s\n", argc,
               found, bad ? ", one wrong" : "");
    return bad || found != NENV;
}

/* --child-launch fds: our descriptors from 3 up, as /proc/self/fd lists
 * them (but the listing's own), written to 3 ("3 4 9"). */
static int child_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return 1;
    char seen[512] = { 0 };   /* seen[fd]: listed */
    struct dirent *e;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (e->d_name[0] != '.' && fd >= 3 && fd < (int)sizeof(seen) && fd != dirfd(d))
            seen[fd] = 1;
    }
    closedir(d);
    char out[64];
    size_t n = 0;
    for (int fd = 3; fd < (int)sizeof(seen) && n < sizeof(out) - 8; fd++)
        if (seen[fd])
            n += (size_t)snprintf(out + n, sizeof(out) - n, n ? " %d" : "%d", fd);
    return write(3, out, n) == (ssize_t)n ? 0 : 1;
}

/* Is fd a descriptor of this type (S_IFMT bits)? */
static bool fd_is(int fd, mode_t type)
{
    struct stat st;
    return fstat(fd, &st) == 0 && (st.st_mode & S_IFMT) == type;
}

/* Is fd closed? */
static bool fd_gone(int fd)
{
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

/* What every child of init's /svc/exec has (docs/CR-REVIEW-POSIX.md, Q1):
 * AT_SECURE, so no TMPDIR and no secure_getenv (HOME is there for
 * getenv), none of fontconfig's FONTCONFIG_FILE, FONTCONFIG_PATH and
 * FONTCONFIG_SYSROOT (docs/SANDBOX-PLAN.md, SB1: the caller set all
 * three), "/" for its current directory and the path init sealed (exe)
 * for /proc/self/exe. 0, or the first check that failed. */
static int sealed_common(const char *exe)
{
    char buf[256];
    if (getauxval(AT_SECURE) != 1)
        return 11;
    if (getenv("TMPDIR") || !getenv("HOME") || secure_getenv("HOME"))
        return 12;
    if (getenv("FONTCONFIG_FILE") || getenv("FONTCONFIG_PATH") || getenv("FONTCONFIG_SYSROOT"))
        return 22;
    if (!getcwd(buf, sizeof(buf)) || strcmp(buf, "/"))
        return 13;
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0 || (buf[n] = 0, strcmp(buf, exe)))
        return 14;
    return 0;
}

/* --child-launch sealed <path>: started through /svc/exec by utest's
 * exec_handover (user/tests/utest/execsvc.c) with a hand-over naming
 * what this program's approval may reach and its caller's may not, which
 * init rewrote: 3 a file sent as "/data/etc/allow" (really a program, so
 * an ELF) that can be read but not given on (ENOTSUP), 4 a directory and
 * 5 a duplicate of it gone, 6 a duplicate of 3 there; 1 the caller's pipe;
 * the caller's umask (027); 0 and 2 closed, so musl's AT_SECURE start
 * opened them on /dev/null. */
static int child_sealed(int argc, char **argv)
{
    int r = argc < 4 ? 10 : sealed_common(argv[3]);
    if (r)
        return r;
    char elf[4];
    if (read(3, elf, 4) != 4 || memcmp(elf, "\177ELF", 4))
        return 15;
    if (!fd_gone(4) || !fd_gone(5) || fd_gone(6))
        return 16;
    int s[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s))
        return 17;
    char cbuf[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec iov = { .iov_base = elf, .iov_len = 1 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                        .msg_controllen = sizeof(cbuf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    int three = 3;
    memcpy(CMSG_DATA(c), &three, sizeof(three));
    if (sendmsg(s[0], &m, 0) != -1 || errno != ENOTSUP)
        return 18;   /* a file without its path is not given on */
    close(s[0]);
    close(s[1]);
    if (umask(022) != 027)
        return 19;
    if (!fd_is(1, S_IFIFO) || !fd_is(0, S_IFCHR) || !fd_is(2, S_IFCHR))
        return 20;
    return 0;
}

/* --child-launch sealed-bare <path>: the same program started through
 * /svc/exec with no hand-over of its caller's: init wrote one for the
 * mark, so 0 to 2 are /dev/null (musl's AT_SECURE start). */
static int child_sealed_bare(int argc, char **argv)
{
    int r = argc < 4 ? 10 : sealed_common(argv[3]);
    if (r)
        return r;
    return fd_is(0, S_IFCHR) && fd_is(1, S_IFCHR) && fd_is(2, S_IFCHR) ? 0 : 21;
}

/* --child-launch inherit <dir>: started through /svc/exec by a caller in
 * dir whose approval gives all this program's does (its own: `from`
 * below), so not raised (init's exec.c, "who is raised"): AT_SECURE 0,
 * TMPDIR as the caller set it (dir) and HOME for secure_getenv too, and
 * dir for its current directory. */
static int child_inherit(int argc, char **argv)
{
    char buf[256];
    if (argc < 4)
        return 30;
    if (getauxval(AT_SECURE) != 0)
        return 31;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || strcmp(tmp, argv[3]))
        return 32;
    if (!secure_getenv("HOME"))
        return 33;
    if (!getcwd(buf, sizeof(buf)) || strcmp(buf, argv[3]))
        return 34;
    return 0;
}

/* --child-launch from <dir> <path> [args...]: a caller started through
 * /svc/exec (utest's exec_starter) goes to dir, sets TMPDIR to it and
 * starts path with args as Chromium's launcher does: fork, then execv in
 * the child (a program on /data: spawn, wait, exit through /svc/exec,
 * with our image). Its exit code, or 90 and up if it couldn't. */
static int child_from(int argc, char **argv)
{
    if (argc < 5 || chdir(argv[3]) || setenv("TMPDIR", argv[3], 1))
        return 90;
    pid_t pid = fork();
    if (pid < 0)
        return 91;
    if (pid == 0) {
        execv(argv[4], argv + 4);
        _exit(92);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid)
        return 93;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 94;
}

/* --child-launch from-out <dir> <line> <path> [args...]: the same with
 * posix_spawn, the child's descriptor 1 a pipe to us: 0 if it exits 0
 * having written exactly line and a newline (pxio's `cwd`), else 95 and
 * up. */
static int child_from_out(int argc, char **argv)
{
    int p[2];
    pid_t pid;
    posix_spawn_file_actions_t fa;
    if (argc < 6 || chdir(argv[3]) || setenv("TMPDIR", argv[3], 1) || pipe(p))
        return 95;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    int e = posix_spawn(&pid, argv[5], &fa, NULL, argv + 5, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (e)
        return 96;
    char out[512];
    size_t n = 0;
    ssize_t got;
    while (n < sizeof(out) - 1 && (got = read(p[0], out + n, sizeof(out) - 1 - n)) > 0)
        n += (size_t)got;
    close(p[0]);
    out[n] = '\0';
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
        return 97;
    size_t l = strlen(argv[4]);
    return n == l + 1 && !strncmp(out, argv[4], l) && out[l] == '\n' ? 0 : 98;
}

/* --child-launch narrow <path> [args...]: a caller started through
 * /svc/exec (utest's exec_starter) starts itself again from its image
 * with less than its namespace (posix_spawnattr_setgrants_np: /data,
 * /boot and /svc/exec alone), as `narrowed`, which starts path with
 * args. Its exit code, or 85 and up if it couldn't. */
static int child_narrow(int argc, char **argv)
{
    static const char *const grants[] = { "/data", "/boot", "/svc/exec", NULL };
    posix_spawnattr_t at;
    pid_t pid;
    int status;
    if (argc < 4 || posix_spawnattr_init(&at) || posix_spawnattr_setgrants_np(&at, grants))
        return 85;
    argv[2] = "narrowed";
    int e = posix_spawn(&pid, "/proc/self/exe", NULL, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    if (e)
        return 86;
    if (waitpid(pid, &status, 0) != pid)
        return 87;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 88;
}

/* --child-launch narrowed <path> [args...]: child_narrow's child, given
 * less than its starter, so given no image of their program, which would
 * make it count as its starter at /svc/exec (docs/CR-PLAN.md, "TS"); it
 * starts path with args (through /svc/exec: raised, its caller not
 * known). Their exit code, or 80 and up. */
static int child_narrowed(int argc, char **argv)
{
    pid_t pid;
    int status;
    if (argc < 4)
        return 80;
    if (startup_handle(SR_SELF_IMAGE))
        return 81;
    if (posix_spawn(&pid, argv[3], NULL, NULL, argv + 3, environ))
        return 82;
    if (waitpid(pid, &status, 0) != pid)
        return 83;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 84;
}

int pt_child_launch(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[2], "args"))
        return child_args(argc, argv);
    if (argc >= 3 && !strcmp(argv[2], "fds"))
        return child_fds();
    if (argc >= 3 && !strcmp(argv[2], "ok"))
        return 0;   /* check_bad_args's good VMO */
    if (argc >= 3 && !strcmp(argv[2], "sealed"))
        return child_sealed(argc, argv);
    if (argc >= 3 && !strcmp(argv[2], "sealed-bare"))
        return child_sealed_bare(argc, argv);
    if (argc >= 3 && !strcmp(argv[2], "inherit"))
        return child_inherit(argc, argv);
    if (argc >= 3 && !strcmp(argv[2], "from"))
        return child_from(argc, argv);
    if (argc >= 3 && !strcmp(argv[2], "from-out"))
        return child_from_out(argc, argv);
    if (argc >= 3 && !strcmp(argv[2], "narrow"))
        return child_narrow(argc, argv);
    if (argc >= 3 && !strcmp(argv[2], "narrowed"))
        return child_narrowed(argc, argv);
    if (argc >= 3 && !strcmp(argv[2], "raised"))
        return argc < 4 ? 10 : sealed_common(argv[3]);
    return 2;
}
