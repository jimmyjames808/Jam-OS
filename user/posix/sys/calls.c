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

    /* descriptors 0 to 2, until the descriptor table: stdfd.c */
    [SYS_read] = lx_read,
    [SYS_write] = lx_write,
    [SYS_readv] = lx_readv,
    [SYS_writev] = lx_writev,
    [SYS_close] = lx_close,
    [SYS_fstat] = lx_fstat,
    [SYS_ioctl] = lx_ioctl,
    [SYS_lseek] = lx_lseek,
    [SYS_fcntl] = lx_fcntl,

    /* signals, until signals: nosig.c */
    [SYS_rt_sigprocmask] = lx_rt_sigprocmask,
};

long __jam_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    if (n < 0 || (unsigned long)n >= sizeof(calls) / sizeof(calls[0]) || !calls[n])
        return -ENOSYS;
    return calls[n](a, b, c, d, e, f);
}
