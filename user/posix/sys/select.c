/* select and pselect (docs/M13-PLAN.md section 7): the sets made into
 * pollfds and given to poll_run (poll.c), the answers made back into
 * sets as Linux's select makes them. FD_SETSIZE is 1024.
 */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#include "layer.h"
#include "pollcore.h"

static bool in_set(const fd_set *s, int fd)
{
    return s && FD_ISSET(fd, s);
}

/* The sets as pollfds in fds: their count, or -EBADF. A descriptor only
 * in the exception set is checked and left out: POLLPRI never comes, so
 * it can never be ready. */
static long select_fds(struct pollfd *fds, int n, const fd_set *r, const fd_set *w,
                       const fd_set *e)
{
    long k = 0;
    for (int fd = 0; fd < n; fd++) {
        bool rd = in_set(r, fd), wr = in_set(w, fd);
        if (!rd && !wr && in_set(e, fd)) {
            if (!poll_fd_open(fd))
                return -EBADF;
        }
        if (rd || wr)
            fds[k++] = (struct pollfd){ .fd = fd,
                                            .events = (short)((rd ? POLLIN : 0) |
                                                              (wr ? POLLOUT : 0)) };
    }
    return k;
}

/* Linux's sets: readable is POLLIN, a hang-up or an error; writable is
 * POLLOUT or an error, and here a hang-up too, so a writer finds out (a
 * write then says EPIPE). */
static long select_out(const struct pollfd *fds, long k, int n, fd_set *r, fd_set *w, fd_set *e)
{
    for (long i = 0; i < k; i++)
        if (fds[i].revents & POLLNVAL)
            return -EBADF;
    for (int fd = 0; fd < n; fd++) {
        if (r)
            FD_CLR(fd, r);
        if (w)
            FD_CLR(fd, w);
        if (e)
            FD_CLR(fd, e);
    }
    long count = 0;
    for (long i = 0; i < k; i++) {
        const struct pollfd *p = &fds[i];
        if ((p->events & POLLIN) && (p->revents & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(p->fd, r);
            count++;
        }
        if ((p->events & POLLOUT) && (p->revents & (POLLOUT | POLLHUP | POLLERR))) {
            FD_SET(p->fd, w);
            count++;
        }
    }
    return count;
}

static long select_run(long n, long r, long w, long e, uint64_t deadline)
{
    if (n < 0)
        return -EINVAL;
    if (n > FD_SETSIZE)
        n = FD_SETSIZE;   /* as Linux: no descriptor is that high anyway */
    fd_set *rs = (fd_set *)(uintptr_t)r, *ws = (fd_set *)(uintptr_t)w, *es = (fd_set *)(uintptr_t)e;
    struct pollfd *fds = poll_scratch();
    if (!fds)
        return -ENOMEM;
    long k = select_fds(fds, (int)n, rs, ws, es);
    if (k < 0)
        return k;
    long got = poll_run(fds, (unsigned long)k, deadline);
    return got < 0 ? got : select_out(fds, k, (int)n, rs, ws, es);
}

long lx_select(long n, long r, long w, long e, long tv, long f)
{
    uint64_t deadline = DEADLINE_NEVER;
    const struct timeval *t = (const struct timeval *)(uintptr_t)tv;
    if (t) {
        if (t->tv_sec < 0 || t->tv_usec < 0)
            return -EINVAL;
        long sec = t->tv_sec, usec = t->tv_usec;
        if (usec / 1000000 > LONG_MAX - sec)
            sec = LONG_MAX;   /* never, in effect */
        else
            sec += usec / 1000000;
        long rr = ready_deadline(sec, usec % 1000000 * 1000, &deadline);
        if (rr)
            return rr;
    }
    return select_run(n, r, w, e, deadline);
}

/* pselect6's sixth argument: the mask and its size. */
struct pselect_mask {
    const void *mask;
    long        size;
};

long lx_pselect6(long n, long r, long w, long e, long ts, long data)
{
    uint64_t deadline = DEADLINE_NEVER, old = 0;
    const struct timespec *t = (const struct timespec *)(uintptr_t)ts;
    const struct pselect_mask *m = (const struct pselect_mask *)(uintptr_t)data;
    const void *mask = m ? m->mask : NULL;
    long rr = t ? ready_deadline(t->tv_sec, t->tv_nsec, &deadline) : 0;
    if (!rr)
        rr = ready_mask_set(mask, m ? m->size : 0, &old);
    if (rr)
        return rr;
    rr = select_run(n, r, w, e, deadline);
    ready_mask_restore(mask, old, rr);
    return rr;
}
