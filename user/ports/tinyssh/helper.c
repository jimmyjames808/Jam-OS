/* tinysshd's helpers started with posix_spawn (helper.h). */
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/spawn.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include "helper.h"

extern char **environ;

pid_t helper_spawn(char *const argv[], int in, int out)
{
    /* The signer alone sees the host key (/data/etc/ssh: `:rk`); the key
     * check sees /data/etc/authorized_keys and nothing else of etc
     * (`:ra`). */
    static const char *const sign_grants[] = { "/data:rk", NULL };
    static const char *const other_grants[] = { "/data:ra", NULL };
    const char *const *grants = !strcmp(argv[0], "tinysshd-sign") ? sign_grants : other_grants;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    pid_t pid = -1;
    int e = posix_spawn_file_actions_init(&fa);
    if (e) {
        errno = e;
        return -1;
    }
    e = posix_spawnattr_init(&at);
    /* Both always replaced: tinysshd's own 0 and 1 are its connection,
     * which would move to the child (a socket can't be shared). */
    if (!e)
        e = in >= 0 ? posix_spawn_file_actions_adddup2(&fa, in, 0)
                    : posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (!e)
        e = out >= 0 ? posix_spawn_file_actions_adddup2(&fa, out, 1)
                     : posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (!e)
        e = posix_spawnattr_setgrants_np(&at, grants);
    if (!e)
        e = posix_spawn(&pid, SSHD_SELF, &fa, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (e) {
        errno = e;
        return -1;
    }
    return pid;
}

int helper_wait(pid_t pid)
{
    int status;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r == -1 && errno == EINTR);
    if (r != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}
