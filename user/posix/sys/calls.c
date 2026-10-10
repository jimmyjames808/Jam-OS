/* __jam_syscall: every system call musl makes ends here (the override of
 * arch/x86_64/syscall_arch.h, and syscall_cp's for cancellation points),
 * with Linux's call number and six arguments. A table indexed by the
 * number gives the handler (calls.h: one per call, in its area's file);
 * a number with none is -ENOSYS, as an old Linux kernel answers a call it
 * doesn't have, so musl falls back where it can and fails cleanly where
 * it can't.
 *
 * Every call is a call boundary for signals (signal.c's sig_call: the
 * handlers due run before and after it, and an EINTR is restarted or
 * not), but for the kinds kind_of names: memory and futex wakes see no
 * delivery (the plan's rule: musl's malloc and unlock paths), the waits
 * Linux never restarts return EINTR, and a write's EPIPE raises SIGPIPE.
 *
 * This is the only file of the layer that sees Linux's numbers
 * (<sys/syscall.h>): their names clash with Jam OS's own. */
#include <errno.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include "calls.h"
#include "glue.h"

static lx_call *const calls[] = {
    /* memory: mem.c */
    [SYS_mmap] = lx_mmap,
    [SYS_munmap] = lx_munmap,
    [SYS_mprotect] = lx_mprotect,
    [SYS_madvise] = lx_madvise,
    [SYS_mremap] = lx_mremap,
    [SYS_brk] = lx_brk,

    /* threads: thread.c */
    [SYS_set_tid_address] = lx_set_tid_address,
    [SYS_gettid] = lx_gettid,
    [SYS_arch_prctl] = lx_arch_prctl,
    [SYS_exit] = lx_exit,
    [SYS_exit_group] = lx_exit_group,
    [SYS_futex] = lx_futex,
    [SYS_sched_yield] = lx_sched_yield,

    /* clocks and sleeps: time.c */
    [SYS_clock_gettime] = lx_clock_gettime,
    [SYS_clock_getres] = lx_clock_getres,
    [SYS_clock_settime] = lx_clock_settime,
    [SYS_clock_nanosleep] = lx_clock_nanosleep,
    [SYS_nanosleep] = lx_nanosleep,

    /* the machine and randomness: misc.c */
    [SYS_uname] = lx_uname,
    [SYS_getrandom] = lx_getrandom,
    [SYS_sysinfo] = lx_sysinfo,
    [SYS_sched_getaffinity] = lx_sched_getaffinity,

    /* the descriptor table: fd.c */
    [SYS_read] = lx_read,
    [SYS_write] = lx_write,
    [SYS_readv] = lx_readv,
    [SYS_writev] = lx_writev,
    [SYS_pread64] = lx_pread64,
    [SYS_pwrite64] = lx_pwrite64,
    [SYS_preadv] = lx_preadv,
    [SYS_pwritev] = lx_pwritev,
    [SYS_preadv2] = lx_preadv2,
    [SYS_pwritev2] = lx_pwritev2,
    [SYS_lseek] = lx_lseek,
    [SYS_close] = lx_close,
    [SYS_fstat] = lx_fstat,
    [SYS_ioctl] = lx_ioctl,
    [SYS_fsync] = lx_fsync,
    [SYS_fdatasync] = lx_fdatasync,
    [SYS_ftruncate] = lx_ftruncate,
    [SYS_getdents64] = lx_getdents64,

    /* files, directories and what they are: file.c, dir.c, meta.c */
    [SYS_open] = lx_open,
    [SYS_openat] = lx_openat,
    [SYS_creat] = lx_creat,
    [SYS_truncate] = lx_truncate,
    [SYS_mkdir] = lx_mkdir,
    [SYS_mkdirat] = lx_mkdirat,
    [SYS_unlink] = lx_unlink,
    [SYS_unlinkat] = lx_unlinkat,
    [SYS_rmdir] = lx_rmdir,
    [SYS_rename] = lx_rename,
    [SYS_renameat] = lx_renameat,
    [SYS_renameat2] = lx_renameat2,
    [SYS_chdir] = lx_chdir,
    [SYS_fchdir] = lx_fchdir,
    [SYS_getcwd] = lx_getcwd,
    [SYS_stat] = lx_stat,
    [SYS_lstat] = lx_lstat,
    [SYS_newfstatat] = lx_newfstatat,
    [SYS_statfs] = lx_statfs,
    [SYS_fstatfs] = lx_fstatfs,
    [SYS_access] = lx_access,
    [SYS_faccessat] = lx_faccessat,
    [SYS_faccessat2] = lx_faccessat2,
    [SYS_chmod] = lx_chmod,
    [SYS_fchmod] = lx_fchmod,
    [SYS_fchmodat] = lx_fchmodat,
    [SYS_chown] = lx_chown,
    [SYS_lchown] = lx_lchown,
    [SYS_fchown] = lx_fchown,
    [SYS_fchownat] = lx_fchownat,
    [SYS_utimensat] = lx_utimensat,
    [SYS_link] = lx_link,
    [SYS_linkat] = lx_linkat,
    [SYS_symlink] = lx_symlink,
    [SYS_symlinkat] = lx_symlinkat,
    [SYS_readlink] = lx_readlink,
    [SYS_readlinkat] = lx_readlinkat,
    [SYS_mknod] = lx_mknod,
    [SYS_mknodat] = lx_mknodat,
    [SYS_umask] = lx_umask,
    [SYS_sync] = lx_sync,
    [SYS_syncfs] = lx_syncfs,
    [SYS_fadvise64] = lx_fadvise64,
    [SYS_fallocate] = lx_fallocate,

    /* pipes, dup and fcntl, memfd, eventfd: pipe.c, dup.c, memfd.c, eventfd.c */
    [SYS_pipe] = lx_pipe,
    [SYS_pipe2] = lx_pipe2,
    [SYS_socketpair] = lx_socketpair,
    [SYS_dup] = lx_dup,
    [SYS_dup2] = lx_dup2,
    [SYS_dup3] = lx_dup3,
    [SYS_fcntl] = lx_fcntl,
    [SYS_memfd_create] = lx_memfd_create,
    [SYS_eventfd] = lx_eventfd,
    [SYS_eventfd2] = lx_eventfd2,

    /* the one user: user.c */
    [SYS_getuid] = lx_getuid,
    [SYS_geteuid] = lx_geteuid,
    [SYS_getgid] = lx_getgid,
    [SYS_getegid] = lx_getegid,
    [SYS_getresuid] = lx_getresuid,
    [SYS_getresgid] = lx_getresgid,
    [SYS_setuid] = lx_setuid,
    [SYS_setgid] = lx_setgid,
    [SYS_setreuid] = lx_setreuid,
    [SYS_setregid] = lx_setregid,
    [SYS_setresuid] = lx_setresuid,
    [SYS_setresgid] = lx_setresgid,
    [SYS_setfsuid] = lx_setfsuid,
    [SYS_setfsgid] = lx_setfsgid,
    [SYS_getgroups] = lx_getgroups,
    [SYS_setgroups] = lx_setgroups,

    /* poll, select and epoll: poll.c, epoll.c */
    [SYS_poll] = lx_poll,
    [SYS_ppoll] = lx_ppoll,
    [SYS_select] = lx_select,
    [SYS_pselect6] = lx_pselect6,
    [SYS_epoll_create] = lx_epoll_create,
    [SYS_epoll_create1] = lx_epoll_create1,
    [SYS_epoll_ctl] = lx_epoll_ctl,
    [SYS_epoll_wait] = lx_epoll_wait,
    [SYS_epoll_pwait] = lx_epoll_pwait,

    /* processes: proc.c, spawn.c (fork, vfork and clone without CLONE_VM
     * have no entry: -ENOSYS) */
    [SYS_getpid] = lx_getpid,
    [SYS_getppid] = lx_getppid,
    [SYS_getpgid] = lx_getpgid,
    [SYS_setpgid] = lx_setpgid,
    [SYS_getsid] = lx_getsid,
    [SYS_setsid] = lx_setsid,
    [SYS_wait4] = lx_wait4,
    [SYS_waitid] = lx_waitid,
    [SYS_execve] = lx_execve,

    /* signals: sigcalls.c; their timers: sigtimer.c */
    [SYS_rt_sigaction] = lx_rt_sigaction,
    [SYS_rt_sigprocmask] = lx_rt_sigprocmask,
    [SYS_rt_sigpending] = lx_rt_sigpending,
    [SYS_rt_sigsuspend] = lx_rt_sigsuspend,
    [SYS_pause] = lx_pause,
    [SYS_rt_sigtimedwait] = lx_rt_sigtimedwait,
    [SYS_sigaltstack] = lx_sigaltstack,
    [SYS_kill] = lx_kill,
    [SYS_tkill] = lx_tkill,
    [SYS_tgkill] = lx_tgkill,
    [SYS_rt_sigqueueinfo] = lx_rt_sigqueueinfo,
    [SYS_rt_tgsigqueueinfo] = lx_rt_tgsigqueueinfo,
    [SYS_getitimer] = lx_getitimer,
    [SYS_setitimer] = lx_setitimer,
    [SYS_timer_create] = lx_timer_create,
    [SYS_timer_settime] = lx_timer_settime,
    [SYS_timer_gettime] = lx_timer_gettime,
    [SYS_timer_getoverrun] = lx_timer_getoverrun,
    [SYS_timer_delete] = lx_timer_delete,
};

/* Linux's futex operations, for kind_of. */
#define FUTEX_OP(op) ((op) & 127)
#define FUTEX_WAIT_OP 0

/* What the boundary does around call n (sig.h's SIG_CALL_*). */
static unsigned kind_of(long n, long b, long c, long d)
{
    switch (n) {
    case SYS_mmap: case SYS_munmap: case SYS_mprotect: case SYS_madvise: case SYS_mremap:
    case SYS_brk: case SYS_exit: case SYS_exit_group:
        return SIG_CALL_QUIET;
    case SYS_futex:
        if (FUTEX_OP(b) != FUTEX_WAIT_OP)
            return SIG_CALL_QUIET;   /* wakes and requeues: musl's unlocks */
        return d ? SIG_CALL_NORESTART : 0;   /* a timed wait: musl recomputes its time */
    case SYS_poll: case SYS_ppoll: case SYS_select: case SYS_pselect6: case SYS_epoll_wait:
    case SYS_epoll_pwait: case SYS_nanosleep: case SYS_clock_nanosleep: case SYS_rt_sigsuspend:
    case SYS_rt_sigtimedwait: case SYS_pause:
        return SIG_CALL_NORESTART;   /* signal(7): never restarted */
    case SYS_write: case SYS_writev: case SYS_pwrite64: case SYS_pwritev: case SYS_pwritev2:
        return SIG_CALL_PIPE;
    case SYS_sendto:
        return d & MSG_NOSIGNAL ? 0 : SIG_CALL_PIPE;
    case SYS_sendmsg:
        return c & MSG_NOSIGNAL ? 0 : SIG_CALL_PIPE;
    }
    return 0;
}

/* The call through the boundary; cancel: a cancellation point's flag. */
static long dispatch(long n, const long *args, volatile int *cancel)
{
    if (n < 0 || (unsigned long)n >= sizeof(calls) / sizeof(calls[0]) || !calls[n])
        return -ENOSYS;
    struct sig_frame f = { calls[n], kind_of(n, args[1], args[2], args[3]), cancel,
                           { args[0], args[1], args[2], args[3], args[4], args[5] } };
    return sig_call(&f);
}

long __jam_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    const long args[6] = { a, b, c, d, e, f };
    return dispatch(n, args, 0);
}

long __jam_syscall_cp(const struct jam_cp_call *c)
{
    return dispatch(c->nr, c->a, c->cancel);
}
