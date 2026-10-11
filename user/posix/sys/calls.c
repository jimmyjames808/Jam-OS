/* __jam_syscall: every system call musl makes ends here (the override of
 * arch/x86_64/syscall_arch.h, and syscall_cp's for cancellation points),
 * with Linux's call number and six arguments. A table indexed by the
 * number gives the handler (calls.h: one per call, in its area's file;
 * calls.def lists them); a number with none is -ENOSYS, as an old Linux
 * kernel answers a call it doesn't have, so musl falls back where it can
 * and fails cleanly where it can't, and the ENOSYS log (enosys.c) names it
 * once if the program asked. A program links only the handlers it can
 * call: calls.def says how.
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

/* The handlers a call site names (calls.def's LX lines) are weak here, so
 * naming one in the table doesn't link it: a program that never makes the
 * call has no handler for it (NULL: -ENOSYS, but nothing can make it). */
#define LX(name)    extern lx_call lx_##name __attribute__((weak));
#define LX_CP(name)
#include "calls.def"
#undef LX
#undef LX_CP

#define LX(name)    [SYS_##name] = lx_##name,
#define LX_CP(name) [SYS_##name] = lx_##name,
static lx_call *const calls[] = {
#include "calls.def"
};
#undef LX
#undef LX_CP

/* Linux's futex operations, for kind_of. */
#define FUTEX_OP(op) ((op) & 127)
#define FUTEX_WAIT_OP 0
#define FUTEX_WAIT_BITSET_OP 9

/* What the boundary does around call n (sig.h's SIG_CALL_*). */
static unsigned kind_of(long n, long b, long c, long d)
{
    switch (n) {
    case SYS_mmap: case SYS_munmap: case SYS_mprotect: case SYS_madvise: case SYS_mremap:
    case SYS_brk: case SYS_exit: case SYS_exit_group:
        return SIG_CALL_QUIET;
    case SYS_futex:
        if (FUTEX_OP(b) == FUTEX_WAIT_BITSET_OP)
            return 0;   /* its timeout is absolute: a restart waits to the same time */
        if (FUTEX_OP(b) != FUTEX_WAIT_OP)
            return SIG_CALL_QUIET;   /* wakes and requeues: musl's unlocks */
        return d ? SIG_CALL_NORESTART : 0;   /* a timed wait: musl recomputes its time */
    case SYS_poll: case SYS_ppoll: case SYS_select: case SYS_pselect6: case SYS_epoll_wait:
    case SYS_epoll_pwait: case SYS_nanosleep: case SYS_clock_nanosleep: case SYS_rt_sigsuspend:
    case SYS_rt_sigtimedwait: case SYS_pause: case SYS_connect:
        return SIG_CALL_NORESTART;   /* signal(7): never restarted (connect goes on) */
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
    if (n < 0 || (unsigned long)n >= sizeof(calls) / sizeof(calls[0]) || !calls[n]) {
        lx_enosys_call(n);   /* the log, when the program turned it on */
        return -ENOSYS;
    }
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
