/* tinysshd's helpers as processes of their own (docs/M13-PLAN.md section
 * 15): upstream forks for the host key's signature (subprocess_sign.c) and
 * for the check of an offered key (subprocess_auth.c), so the process that
 * parses what the network sends never holds the secret key. Jam OS has no
 * fork: each helper is this program started again with posix_spawn, under
 * the helper's name (tinysshd.c picks the mode by argv[0]).
 *
 * A helper's descriptors: 0 and 1 the ones the caller names, 2 the
 * caller's (the server's log), nothing else (everything else of
 * tinysshd's is FD_CLOEXEC; its connection is on 0 and 1, which the two
 * dup2s replace in the child, so the socket never moves to a helper). Its
 * namespace: /data read-only alone; the signing helper's view shows the
 * host key (/data/etc/ssh, `:rk`), the key check's only the authorized
 * keys (`:ra`: <fsview.h>'s FS_VIEW_HIDE_KEYS); neither shows the rest of
 * /data/etc (FS_VIEW_HIDE_ETC). */
#pragma once

#include <sys/types.h>

/* bin/sshd: every mode of the SSH server is this one program. */
#define SSHD_SELF "/bin/sshd"

/* Start this program with argv (argv[0] its mode, NULL-terminated), its
 * descriptors 0 and 1 the caller's in and out (-1: /dev/null). The pid,
 * or -1 with errno. */
pid_t helper_spawn(char *const argv[], int in, int out);
/* Wait for helper pid to end: its exit code (0 to 255), or -1 if it was
 * killed or the wait failed. */
int helper_wait(pid_t pid);

/* The modes' mains (tinysshd.c picks one by argv[0]): the signing helper
 * (subprocess_sign.c), the key check (subprocess_auth.c) and the listener
 * (sshd.c). Each returns, or exits with, its exit code. */
int subprocess_sign_main(int argc, char **argv);
int subprocess_auth_main(int argc, char **argv);
int sshd_main(int argc, char **argv);
