/* musl's cancellation-point call on Jam OS: overrides upstream's
 * src/thread/x86_64/syscall_cp.s (user/posix/musl/OVERRIDES).
 *
 * __syscall_cp_asm(&self->cancel, nr, u, v, w, x, y, z) is the call,
 * made through the layer with the thread's cancel flag
 * (__jam_syscall_cp). Upstream is assembly so that musl's cancellation
 * signal handler can tell, from the interrupted instruction's address
 * (between __cp_begin and __cp_end), that the thread was about to block;
 * here no signal interrupts a thread in user code, so there is no address
 * to look at: pthread_cancel interrupts the thread's wait instead
 * (src/thread/pthread_cancel.c's override), and the layer ends the call
 * with EINTR when it finds the flag set, before the call or after it. */
#include "glue.h"

long __syscall_cp_asm(volatile int *cancel, long nr, long u, long v, long w, long x, long y,
                      long z)
{
    const struct jam_cp_call c = { cancel, nr, { u, v, w, x, y, z } };
    return __jam_syscall_cp(&c);
}
