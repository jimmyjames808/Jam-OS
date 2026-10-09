/* What poll.c and epoll.c share (ready.h): interest and readiness as
 * POLL* bits, a kind's arm, adding it to a set, timeouts as deadlines,
 * and the signal mask for a wait. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include "ready.h"

uint32_t ready_interest(uint32_t events)
{
    uint32_t in = 0;
    if (events & (POLLIN | POLLRDNORM | POLLRDHUP))
        in |= NETWAIT_READ;
    if (events & (POLLOUT | POLLWRNORM))
        in |= NETWAIT_WRITE;
    return in;
}

uint32_t ready_always(const struct fd_desc *d)
{
    return (fd_can_read(d) ? POLLIN | POLLRDNORM : 0) | (fd_can_write(d) ? POLLOUT | POLLWRNORM : 0);
}

uint32_t ready_bits(uint32_t nw, const struct fd_wait *w)
{
    uint32_t b = 0;
    if (nw & NETWAIT_READ)
        b |= POLLIN | POLLRDNORM;
    if (nw & NETWAIT_RX_END)
        b |= POLLRDHUP;
    if (nw & NETWAIT_WRITE)
        b |= POLLOUT | POLLWRNORM;
    if (nw & NETWAIT_HUP)
        b |= w->kind == FD_WAIT_HANDLE && !w->h.read ? POLLERR : POLLHUP;
    if (nw & NETWAIT_ERROR)
        b |= POLLERR;
    return b;
}

uint32_t ready_libc(struct fd_desc *d, uint32_t interest)
{
    uint32_t nw = d->ops->ready ? d->ops->ready(d, interest) & interest : 0;
    return (nw & NETWAIT_READ ? POLLIN | POLLRDNORM : 0) |
           (nw & NETWAIT_WRITE ? POLLOUT | POLLWRNORM : 0);
}

status_t ready_add(struct netwait *s, const struct fd_wait *w, uint32_t interest, void *user,
                   uint32_t *out_id)
{
    if (w->kind == FD_WAIT_HANDLE)
        return netwait_add_handle(s, &w->h, interest, user, out_id);
    if (w->kind == FD_WAIT_SOCK)
        return netwait_add_sock(s, &w->sock, interest, user, out_id);
    return ERR_INVALID_ARGS;   /* a kind's bug: refused, not guessed at */
}

long ready_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out)
{
    memset(out, 0, sizeof(*out));   /* compared whole, padding too */
    long r = d->ops->arm(d, interest, out);
    if (!r && out->kind != FD_WAIT_HANDLE && out->kind != FD_WAIT_SOCK)
        r = -EINVAL;   /* a kind's bug: refused */
    return r;
}

uint64_t ready_deadline_ms(long ms)
{
    if (ms < 0)
        return DEADLINE_NEVER;
    uint64_t ns, at;
    if (__builtin_mul_overflow((uint64_t)ms, NS_PER_MS, &ns) ||
        __builtin_add_overflow(now(), ns, &at))
        return DEADLINE_NEVER;
    return at;
}

long ready_deadline(long sec, long nsec, uint64_t *out)
{
    if (sec < 0 || nsec < 0 || nsec >= (long)NS_PER_S)
        return -EINVAL;
    uint64_t ns, at;
    if (__builtin_mul_overflow((uint64_t)sec, NS_PER_S, &ns) ||
        __builtin_add_overflow(ns, (uint64_t)nsec, &ns) || __builtin_add_overflow(now(), ns, &at))
        at = DEADLINE_NEVER;
    *out = at;
    return 0;
}

/* The mask is nosig.c's (rt_sigprocmask) until signals exist: set for the
 * wait and put back after, which is all ppoll and pselect can mean while
 * no signal is ever delivered. Signals (signal.c) make the swap and the
 * wait one step, so a signal unblocked for the wait interrupts it. */
long ready_mask_set(const void *mask, long size, uint64_t *old)
{
    if (!mask)
        return 0;
    return lx_rt_sigprocmask(SIG_SETMASK, (long)(uintptr_t)mask, (long)(uintptr_t)old, size, 0,
                             0);
}

void ready_mask_restore(const void *mask, uint64_t old)
{
    if (mask)   /* can't fail: the size is right and the mask was ours */
        (void)lx_rt_sigprocmask(SIG_SETMASK, (long)(uintptr_t)&old, 0, sizeof(old), 0, 0);
}
