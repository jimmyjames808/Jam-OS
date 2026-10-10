/* Processes in the Linux call layer (docs/M13-PLAN.md section 5): what
 * proc.c (the ids, process groups and sessions, the table of children
 * and the waits) and spawn.c (posix_spawn, the hand-over, execve) share,
 * and what the signal track hooks into.
 *
 * A pid is the kernel's small id (process_info.pid). Process groups and
 * sessions are bookkeeping in libc over this process and its children:
 * the only processes a program can reach are the ones it started. */
#pragma once

#include "layer.h"

/* <posixfds.h> includes <os.h> itself: after layer.h, which includes
 * <os.h> the way musl's headers need (<jamos/libjam.h>). */
#include <posixfds.h>

/* A process ended by a signal's default action exits with this plus the
 * signal's number (1 to 64): a code outside 0 to 255, so a program's own
 * exit(130) stays an exit. waitpid reports such a code as WIFSIGNALED
 * with that WTERMSIG; a process the kernel killed (process_kill, a fault,
 * PROCESS_KILLED_CODE) is WTERMSIG SIGKILL. */
#define LX_EXIT_SIGNAL 0x10000

/* Our own ids: the pid (the kernel's), the parent's (from the hand-over;
 * 1 without one) and our process group and session (the hand-over's; our
 * own pid without one, or where it says 0). */
int32_t lx_pid(void);
int32_t lx_pgid(void);
int32_t lx_sid(void);

/* A child posix_spawn started: proc and sigch (our end of its signal
 * channel, or 0; both moved into the table, closed when the child is
 * reaped), its pid, process group and session. 0, or -errno (-EAGAIN: the
 * table is full; the child is then killed and both handles closed). */
long    lx_child_add(handle_t proc, handle_t sigch, int32_t pid, int32_t pgid, int32_t sid);
/* pgid names a process group a child may be put in: ours, or one of our
 * children's in our session. */
bool    lx_group_exists(int32_t pgid);

/* Wait for the child pid (a pid we started) to end and reap it; the code
 * to exit with to end as it did into *code (its exit code, or
 * LX_EXIT_SIGNAL plus the signal that ended it). 0, or -errno. execve's
 * wait. */
long    lx_child_wait(int32_t pid, int64_t *code);
/* The child execve is waiting for (the signal thread forwards every
 * signal to it), or 0. */
int32_t lx_exec_child(void);

/* Signal our children: pid > 0, that one; else those in group pgid (> 0),
 * or all of them (pgid 0). SIGKILL is process_kill; any other signal a
 * `send` on the child's channel with code (si_code) and value; 0 checks
 * only. A child that has ended but isn't reaped matches and gets nothing.
 * How many matched (0: none, kill's ESRCH), or -errno. */
long    lx_child_signal(int32_t pid, int32_t pgid, int signo, int code, int32_t value);
/* A packet from the signal thread's port with a child's key (below
 * SIG_KEY_OURS): that child's end noticed, the waits woken, SIGCHLD. */
void    lx_child_packet(const struct port_packet *pkt);
