/* The Linux call layer's handlers (user/posix/sys/): one per Linux system
 * call musl makes, by area, each in its area's file. calls.c's table maps
 * Linux's call numbers to them.
 *
 * Every handler has one shape, lx_call: the call's six arguments as musl
 * passed them (Linux's argument shapes: pointers and sizes as longs) in,
 * Linux's result out: a value, or -errno (errno.c's lx_err turns an ERR_*
 * into one). A handler names all six and uses what its call has (the
 * layer builds with -Wno-unused-parameter for that).
 *
 * This header holds plain C only: calls.c includes it beside Linux's
 * <sys/syscall.h>, whose SYS_* names clash with Jam OS's own
 * (<jam/syscall_nums.h>). The area files include layer.h, which includes
 * this.
 *
 * Adding a call: write `long lx_<name>(long a, ...)` in the area's file,
 * declare it below in the area's block, and add its line to the area's
 * block of calls.def (LX or LX_CP: that file says which), which makes
 * calls.c's table (a number given twice fails the build:
 * -Woverride-init). An area that isn't built yet has no block. */
#pragma once

#include <stddef.h>

typedef long lx_call(long a, long b, long c, long d, long e, long f);

/* ---- the call boundary (signal.c, from calls.c) ------------------------------------- */

/* What kind of call a frame is, for the boundary. */
#define SIG_CALL_QUIET     1u   /* no delivery around it: memory, futex wakes, exits */
#define SIG_CALL_NORESTART 2u   /* never restarted after a handler (poll, sleeps, waits
                                 * for signals, timed futex waits): EINTR */
#define SIG_CALL_PIPE      4u   /* EPIPE raises SIGPIPE on the calling thread */

/* One call through the boundary: the handler and Linux's six arguments,
 * and for a cancellation point the thread's cancel flag (musl's). */
struct sig_frame {
    lx_call      *fn;
    unsigned      kind;     /* SIG_CALL_* */
    volatile int *cancel;   /* a cancellation point's flag, or NULL */
    long          a[6];
};

/* Run f's call as a call boundary: the handlers due before it run, then
 * the call, then the handlers due after it. An EINTR is returned, or the
 * call made again: when no handler ran (an interrupt left over from a
 * signal already delivered), or when every handler that ran had
 * SA_RESTART and the call may restart. A set cancel flag ends the call
 * with EINTR (musl's cancellation acts on it). */
long sig_call(const struct sig_frame *f);

/* Memory (mem.c): anonymous mappings and the break. */
lx_call lx_mmap, lx_munmap, lx_mprotect, lx_madvise, lx_mremap, lx_brk, lx_mlock, lx_munlock,
        lx_mlock2, lx_mlockall, lx_munlockall;

/* Threads (thread.c). */
lx_call lx_set_tid_address, lx_gettid, lx_arch_prctl, lx_exit, lx_exit_group, lx_futex,
        lx_sched_yield, lx_set_robust_list, lx_get_robust_list;

/* Priorities and scheduling policies of a thread (sched.c), and the
 * process's attributes and a thread's name (prctl.c). */
lx_call lx_setpriority, lx_getpriority, lx_getcpu, lx_sched_getscheduler,
        lx_sched_setscheduler, lx_sched_getparam, lx_sched_setparam,
        lx_sched_get_priority_max, lx_sched_get_priority_min, lx_prctl;

/* ---- the ENOSYS log (enosys.c, enosysnames.c) ------------------------------------------ */

/* What the layer does not answer, logged once each when the program asked
 * for it (JAMOS_ENOSYS_LOG=1 in its environment; otherwise these return at
 * once). enosys.c says what a gap is and how the lines look. */
/* A call number no handler answers. */
void   lx_enosys_call(long n);
/* An option of an answered call that is refused: "<what> <value>" (what:
 * "prctl option", "futex operation"). */
void   lx_enosys_option(const char *what, long value);
/* An ioctl request nothing understood (terminal probes are not gaps). */
void   lx_enosys_ioctl(unsigned long req);
/* A /proc path that is not there. */
void   lx_enosys_path(const char *path);
/* Linux's name for call number n ("clone3"), or NULL. */
const char *lx_sysname(long n);
/* For posixtest: turn the log on or off whatever the environment says, and
 * forget what was logged; the lines written since the process began; the
 * last line written, into buf (cap bytes, NUL-ended; returns its length). */
void   lx_enosys_set(int on);
unsigned lx_enosys_lines(void);
size_t lx_enosys_last(char *buf, size_t cap);

/* Clocks and sleeps, and CPU time (time.c). */
lx_call lx_clock_gettime, lx_clock_getres, lx_clock_settime, lx_clock_nanosleep,
        lx_nanosleep, lx_times, lx_getrusage;

/* The machine and randomness (misc.c), and the resource limits (rlimit.c). */
lx_call lx_uname, lx_getrandom, lx_sysinfo, lx_sched_getaffinity, lx_sched_setaffinity,
        lx_membarrier;
lx_call lx_getrlimit, lx_setrlimit, lx_prlimit64;

/* The descriptor table: the calls that route to a descriptor's kind (fd.c). */
lx_call lx_read, lx_write, lx_readv, lx_writev, lx_pread64, lx_pwrite64, lx_preadv,
        lx_pwritev, lx_preadv2, lx_pwritev2, lx_lseek, lx_close, lx_fstat, lx_ioctl, lx_fsync,
        lx_fdatasync, lx_ftruncate, lx_getdents64;

/* Files, directories and what they are (file.c, dir.c, meta.c). */
lx_call lx_open, lx_openat, lx_creat, lx_truncate;
lx_call lx_mkdir, lx_mkdirat, lx_unlink, lx_unlinkat, lx_rmdir, lx_rename, lx_renameat,
        lx_renameat2, lx_chdir, lx_fchdir, lx_getcwd;
lx_call lx_stat, lx_lstat, lx_newfstatat, lx_statfs, lx_fstatfs, lx_access, lx_faccessat,
        lx_faccessat2, lx_chmod, lx_fchmod, lx_fchmodat, lx_chown, lx_lchown, lx_fchown,
        lx_fchownat, lx_utimensat, lx_link, lx_linkat, lx_symlink, lx_symlinkat, lx_readlink,
        lx_readlinkat, lx_mknod, lx_mknodat, lx_umask, lx_sync, lx_syncfs, lx_fadvise64,
        lx_fallocate;

/* Pipes, dup and fcntl, memfd and eventfd (pipe.c, dup.c, memfd.c,
 * eventfd.c; /dev and /etc are dev.c's and etc.c's, reached through the
 * path calls). */
lx_call lx_pipe, lx_pipe2, lx_socketpair, lx_dup, lx_dup2, lx_dup3, lx_fcntl, lx_flock,
        lx_memfd_create, lx_eventfd, lx_eventfd2;

/* The one user (user.c). */
lx_call lx_getuid, lx_geteuid, lx_getgid, lx_getegid, lx_getresuid, lx_getresgid, lx_setuid,
        lx_setgid, lx_setreuid, lx_setregid, lx_setresuid, lx_setresgid, lx_setfsuid,
        lx_setfsgid, lx_getgroups, lx_setgroups;

/* poll, select and epoll (poll.c, select.c, epoll.c). */
lx_call lx_poll, lx_ppoll, lx_select, lx_pselect6, lx_epoll_create, lx_epoll_create1,
        lx_epoll_ctl, lx_epoll_wait, lx_epoll_pwait;

/* Processes (proc.c: ids, groups, sessions, waits; spawn.c: execve). */
lx_call lx_getpid, lx_getppid, lx_getpgid, lx_setpgid, lx_getsid, lx_setsid, lx_wait4,
        lx_waitid, lx_execve;

/* Signals (sigcalls.c) and the timers that send them (sigtimer.c). */
lx_call lx_rt_sigaction, lx_rt_sigprocmask, lx_rt_sigpending, lx_rt_sigsuspend, lx_pause,
        lx_rt_sigtimedwait, lx_sigaltstack, lx_kill, lx_tkill, lx_tgkill, lx_rt_sigqueueinfo,
        lx_rt_tgsigqueueinfo;
lx_call lx_getitimer, lx_setitimer, lx_timer_create, lx_timer_settime, lx_timer_gettime,
        lx_timer_getoverrun, lx_timer_delete;

/* Sockets (socket.c: making, binding, connecting and naming them;
 * sockio.c: their bytes; sockopt.c: their options). A socket's wait that
 * has a timeout (SO_RCVTIMEO, SO_SNDTIMEO) and is ended by a signal returns
 * -LX_EINTR_FINAL, which the boundary turns into EINTR and never restarts
 * (Linux's rule for those waits): no errno has this value. */
#define LX_EINTR_FINAL 0x7ff
lx_call lx_socket, lx_bind, lx_connect, lx_listen, lx_accept, lx_accept4, lx_shutdown,
        lx_getsockname, lx_getpeername;
lx_call lx_sendto, lx_recvfrom, lx_sendmsg, lx_recvmsg, lx_sendmmsg, lx_recvmmsg;
lx_call lx_setsockopt, lx_getsockopt;
/* exit_group, before the process ends: wait (up to the linger time) for
 * netstack to take what the program wrote to its connections (socket.c). */
void sock_exit_drain(void);
