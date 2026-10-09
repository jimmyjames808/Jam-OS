/* __jam_syscall: every system call musl makes ends here (the override of
 * arch/x86_64/syscall_arch.h, and syscall_cp's for cancellation points),
 * with Linux's call number and six arguments. A table indexed by the
 * number gives the handler (calls.h: one per call, in its area's file);
 * a number with none is -ENOSYS, as an old Linux kernel answers a call it
 * doesn't have, so musl falls back where it can and fails cleanly where
 * it can't.
 *
 * This is the only file of the layer that sees Linux's numbers
 * (<sys/syscall.h>): their names clash with Jam OS's own. */
#include <errno.h>
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

    /* signals, until signals: nosig.c */
    [SYS_rt_sigprocmask] = lx_rt_sigprocmask,
};

long __jam_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    if (n < 0 || (unsigned long)n >= sizeof(calls) / sizeof(calls[0]) || !calls[n])
        return -ENOSYS;
    return calls[n](a, b, c, d, e, f);
}
