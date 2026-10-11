/* init's sessions from a POSIX program (docs/M13-PLAN.md section 15:
 * tinyssh's in place of openpty, fork, login_tty and execve of a shell;
 * abi/idl/session.idl). A session is a Jam OS shell on a terminal whose
 * screen and keyboard are a descriptor of the caller's, the master. The
 * program's list must ask for `svc session` (a port's or a test's only).
 *
 * The master reads what the terminal shows (each bare LF as CR LF) and
 * writes what the remote terminal types, as bytes; poll and O_NONBLOCK work
 * on it as on a pipe. ioctl TIOCSWINSZ on it is the remote terminal's new
 * size (the programs on the terminal get SIGWINCH); TIOCGWINSZ the size
 * set last. Closing it hangs the session up (the programs on the terminal
 * get SIGHUP; the shell ends with 2). Once the shell has ended and nothing
 * is left on the terminal, a read gives what was left, then EIO. It is
 * opened FD_CLOEXEC: a child never holds it unasked.
 *
 * The session is a child for waitpid (its pid is the shell's): it ends
 * with the shell's code (`exit [code]`, the command's status, 2 for a
 * hang-up), SIGCHLD says so. kill(pid, SIGHUP), SIGTERM or SIGKILL hangs
 * it up; any other signal is accepted and does nothing. */
#ifndef _JAMOS_SESSION_H
#define _JAMOS_SESSION_H

#include <sys/ioctl.h>
#include <sys/types.h>

/* A new session of ws's size (NULL: 80x24), with TERM=term for its
 * programs (NULL or "": the shell's own, vt100; [A-Za-z0-9._+-], at most
 * 63 bytes), an interactive shell for command NULL or "", else the shell
 * runs that one line (no banner, no prompt) and ends with its status.
 * 0 and *master, *pid; or -1 and errno: ENOENT (our namespace has no
 * /svc/session: the list doesn't ask for it), EAGAIN (8 sessions run),
 * EINVAL (a size of 0, a byte of term outside its set, a control
 * character in command), E2BIG (command longer than the shell's 240
 * bytes), EMFILE, ENOMEM. */
int jamos_session_open(const struct winsize *ws, const char *term, const char *command,
                       int *master, pid_t *pid);

#endif
