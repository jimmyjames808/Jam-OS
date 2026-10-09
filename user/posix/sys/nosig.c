/* Signals before signals (docs/M13-PLAN.md section 6 builds them, in
 * signal.c, and this file goes then): no signal is ever delivered, so
 * the only call musl needs answered is rt_sigprocmask, which pthread_create
 * and others use to block signals around their critical steps. Each
 * thread keeps its mask (blocking and unblocking work as Linux's, and a
 * new thread starts with nothing blocked); nothing else reads it.
 * sigaction and the rest are -ENOSYS (the table has no entry). */
#include <errno.h>
#include <signal.h>
#include "layer.h"

#define SIG_UNBLOCKABLE ((1ull << (SIGKILL - 1)) | (1ull << (SIGSTOP - 1)))

static __thread uint64_t blocked;   /* bit n - 1: signal n */

long lx_rt_sigprocmask(long how, long set, long old, long size, long e, long f)
{
    if (size != sizeof(uint64_t))
        return -EINVAL;
    uint64_t was = blocked, s = 0;
    if (set) {
        s = *(const uint64_t *)(uintptr_t)set & ~SIG_UNBLOCKABLE;
        switch (how) {
        case SIG_BLOCK:   blocked |= s; break;
        case SIG_UNBLOCK: blocked &= ~s; break;
        case SIG_SETMASK: blocked = s; break;
        default:          return -EINVAL;
        }
    }
    if (old)
        *(uint64_t *)(uintptr_t)old = was;
    return 0;
}
