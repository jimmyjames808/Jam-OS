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
 * with that WTERMSIG; a process the kernel killed for a fault is that
 * fault's signal (sig_fault_signal: SIGSEGV, SIGILL, ...), any other kill
 * (process_kill, PROCESS_KILLED_CODE) WTERMSIG SIGKILL. */
#define LX_EXIT_SIGNAL 0x10000

/* Our own ids: the pid (the kernel's), the parent's (from the hand-over;
 * 1 without one) and our process group and session (the hand-over's; our
 * own pid without one, or where it says 0). */
int32_t lx_pid(void);
int32_t lx_pgid(void);
int32_t lx_sid(void);

/* fork's step (layer.h's LX_FORK_*): our ids and the table of children. */
void    proc_fork(int step);
/* A child posix_spawn started: proc and sigch (our end of its signal
 * channel, or 0), and job: 0 for a child we made ourselves, or the job of
 * one init's /svc/exec started for us (its process then has no
 * RIGHT_WRITE: SIGKILL kills its job instead); all three moved into the
 * table, closed when the child is reaped. Its pid, process group and
 * session. 0, or -errno (-EAGAIN: the table is full; the child is then
 * killed and the handles closed). */
long    lx_child_add(handle_t proc, handle_t sigch, handle_t job, int32_t pid, int32_t pgid,
                     int32_t sid);
/* One of init's sessions as a child (pty.c's jamos_session_open): proc,
 * its shell's process (wait and inspect rights only), and ctl, its
 * session_ctl channel (kill's signals go there: SIGHUP, SIGTERM and
 * SIGKILL hang it up), both moved; its pid, which leads a group and a
 * session of its own. 0, or -errno (the session hung up, both closed). */
long    lx_session_add(handle_t proc, handle_t ctl, int32_t pid);
/* pgid names a process group a child may be put in: ours, or one of our
 * children's in our session. */
bool    lx_group_exists(int32_t pgid);

/* Wait for the child pid (a pid we started) to end and reap it; the code
 * to exit with to end as it did into *code (its exit code, or
 * LX_EXIT_SIGNAL plus the signal that ended it). 0, or -errno. execve's
 * wait. */
long    lx_child_wait(int32_t pid, int64_t *code);
/* The CPU time (ns) of the children a wait has reaped (Linux's
 * RUSAGE_CHILDREN: not those reaped unwaited, with SIGCHLD ignored). */
uint64_t lx_children_cpu_ns(void);
/* The child execve is waiting for (the signal thread forwards every
 * signal to it), or 0. */
int32_t lx_exec_child(void);

/* vfork (docs/FORK-PLAN.md section 3): in the child, we are a vfork child
 * (lx_vforked), and our exec is told to the parent (lx_exec_notice: the
 * last thing before an exec in place, or once execve's program is started
 * when it isn't in place; nothing if we aren't one, or have told it); in
 * the parent, wait until the child pid has exec'd or ended
 * (lx_vfork_wait; the signals that come meanwhile wait for its return). */
void    lx_vforked(void);
void    lx_exec_notice(void);
void    lx_vfork_wait(int32_t pid);

/* What /proc says of a child (procfs.c, procinfo.c): its process group and
 * session, whether its end was noticed (a zombie until reaped), and its
 * process_info now. 0, or -ESRCH: not a child of ours (or reaped). */
struct lx_child_facts {
    int32_t             pgid, sid;
    bool                dead;
    struct process_info info;
};
long    lx_child_facts(int32_t pid, struct lx_child_facts *out);
/* Is pid a child of ours (not yet reaped)? */
bool    lx_child_exists(int32_t pid);
/* A child's mappings (kp_process_maps): the count, or -ESRCH / -errno. */
long    lx_child_maps(int32_t pid, uint64_t from, struct map_info *out, uint32_t cap);
/* Our children's pids (those not reaped): up to cap into out, in the order
 * they were made; how many there are in all. */
unsigned lx_child_pids(int32_t *out, unsigned cap);

/* Signal our children: pid > 0, that one; else those in group pgid (> 0),
 * or all of them (pgid 0). SIGKILL is process_kill; any other signal a
 * `send` on the child's channel with code (si_code) and value; 0 checks
 * only. A child that has ended but isn't reaped matches and gets nothing.
 * How many matched (0: none, kill's ESRCH), or -errno. */
long    lx_child_signal(int32_t pid, int32_t pgid, int signo, int code, int32_t value);
/* A packet from the signal thread's port with a child's key (below
 * SIG_KEY_OURS): that child's end noticed, the waits woken, SIGCHLD; or
 * its notices (signal.idl's events to the client) read. */
void    lx_child_packet(const struct port_packet *pkt);
