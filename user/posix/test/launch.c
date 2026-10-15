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
 *     others we had open; a negative start is EBADF. */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <string.h>
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

void pt_launch(char **argv __attribute__((unused)))
{
    check_big_args("/bin/posixtest");
    check_big_args("/proc/self/exe");
    check_too_big();
    check_closefrom();
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

int pt_child_launch(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[2], "args"))
        return child_args(argc, argv);
    if (argc >= 3 && !strcmp(argv[2], "fds"))
        return child_fds();
    return 2;
}
