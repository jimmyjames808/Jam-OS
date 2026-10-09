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
        lx_pwritev, lx_lseek, lx_close, lx_fstat, lx_ioctl, lx_fsync, lx_fdatasync,
        lx_ftruncate, lx_getdents64;

/* fcntl's table-only commands until B2's fcntl (stdfd.c). */
lx_call lx_fcntl;

/* poll, select and epoll (poll.c, select.c, epoll.c). */
lx_call lx_poll, lx_ppoll, lx_select, lx_pselect6, lx_epoll_create, lx_epoll_create1,
        lx_epoll_ctl, lx_epoll_wait, lx_epoll_pwait;

/* Signals before signals exist (nosig.c). */
lx_call lx_rt_sigprocmask;
