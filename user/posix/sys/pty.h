/* Pseudo-terminals and init's sessions (pty.c): the master descriptor
 * kind, /dev/ptmx and /dev/pts/N (docs/M13-PLAN.md, "The terminal layer";
 * "X1 notes"). */
#pragma once

#include <sys/stat.h>
#include "fd.h"

/* Pseudo-terminals one process may hold at once (/dev/pts/0 to 7). */
#define PTY_MAX 8

/* Is abs (a clean absolute path) /dev/ptmx or under /dev/pts? */
bool pty_path(const char *abs);
/* open of such a path (dev.c's dev_open): /dev/ptmx makes a new
 * pseudo-terminal and gives its master; /dev/pts/N opens pseudo-terminal
 * N's slave, a terminal (tty.c), if this process holds its master. The
 * descriptor, or -errno: ENOSPC (PTY_MAX held already: posix_openpt's
 * EAGAIN), ENOENT (no such pseudo-terminal of ours), EIO (its console has
 * ended: every program on it has gone). /dev/pts itself is a directory
 * that isn't listed. */
long pty_open(const char *abs, int flags);
/* stat of such a path. */
long pty_stat(const char *abs, struct stat *st);

/* TIOCSWINSZ on a slave (tty.c): pseudo-terminal `index`, if it is still
 * the one opened as generation `gen`, is cols x rows cells from now on
 * (its console's conn_ctl.resize: SIGWINCH on its terminal). 0, or
 * -errno (EIO: it is gone). */
long pty_resize(int index, uint32_t gen, uint16_t cols, uint16_t rows);
