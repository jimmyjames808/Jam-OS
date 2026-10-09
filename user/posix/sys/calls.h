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
 * declare it below in the area's block, and add `[SYS_<name>] = lx_<name>`
 * to the area's block of calls.c's table (a number given twice fails the
 * build: -Woverride-init). An area that isn't built yet has no block. */
#pragma once

typedef long lx_call(long a, long b, long c, long d, long e, long f);

/* Memory (mem.c): anonymous mappings and the break. */
lx_call lx_mmap, lx_munmap, lx_mprotect, lx_madvise, lx_mremap, lx_brk;

/* Threads (thread.c). */
lx_call lx_set_tid_address, lx_gettid, lx_arch_prctl, lx_exit, lx_exit_group, lx_futex,
        lx_sched_yield;

/* Clocks and sleeps (time.c). */
lx_call lx_clock_gettime, lx_clock_getres, lx_clock_settime, lx_clock_nanosleep,
        lx_nanosleep;

/* The machine and randomness (misc.c). */
lx_call lx_uname, lx_getrandom, lx_sysinfo, lx_sched_getaffinity;

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
lx_call lx_pipe, lx_pipe2, lx_socketpair, lx_dup, lx_dup2, lx_dup3, lx_fcntl, lx_memfd_create,
        lx_eventfd, lx_eventfd2;

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

/* Signals before signals exist (nosig.c). */
lx_call lx_rt_sigprocmask;
