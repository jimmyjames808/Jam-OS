/* epoll (docs/M13-PLAN.md section 7): an epoll description holds a wait
 * set of its own (<netwait.h>); epoll_ctl adds, changes and removes its
 * items, epoll_wait waits. What a kind is waited on with, and why no
 * wake is lost to the bytes libc keeps itself: ready.h.
 *
 * **Items.** An item is (descriptor number, description), as Linux's: the
 * same description added again under another number is another item. An
 * item doesn't hold its description: when the description's last
 * reference goes, fd_put calls epoll_forget, which takes it out of every
 * set before the kind closes its handles (so a pipe whose only other
 * user is an epoll set still ends). A kind with no arm (a file, /dev/null)
 * is refused with EPERM, as Linux refuses a regular file; an epoll
 * description in a set with EINVAL (a port can't be bound to a port), so
 * poll and select refuse one too.
 *
 * **Level and edge.** The set is level-triggered: an item reported ready
 * stays pending and is looked at again by every wait. EPOLLET makes its
 * entry an edge one (NETWAIT_EDGE): reported once, then asleep until one
 * of its signals rises again (a reader that drained the pipe, then a
 * write); the kind's own readiness counts for an edge item only when it
 * rises (none kept, then some). An edge item ready but not delivered,
 * because the caller's array was full, is touched so the next wait looks
 * at it. EPOLLONESHOT: once delivered, the item's entry is removed until
 * EPOLL_CTL_MOD brings it back. EPOLLEXCLUSIVE and EPOLLWAKEUP are
 * accepted and change nothing (there is no herd of sets to keep from
 * waking, and no suspend). A socket's edge is netstack's next signal
 * (<netwait.h>): drain it to EAGAIN, as epoll(7) asks.
 *
 * **Threads.** Any thread may wait on or change a set while others do:
 * each set's lock guards its items and its netwait (a set belongs to one
 * thread at a time, as <netwait.h> asks), and is never held while a
 * thread sleeps. A waiter looks (under the lock), and if nothing is
 * ready counts itself in `waiters` before letting go of the lock and
 * sleeping in netwait_block; a change made while anyone is counted there
 * wakes the set (netwait_wake), so a waiter never sleeps through an item
 * added after its look. A waiter that delivered something while others
 * sleep wakes the set too: what it left may be ready for them.
 *
 * **Locks.** sets_lock (the list of every set, which epoll_forget walks)
 * comes before a set's lock; the descriptor table's lock is never taken
 * under either (descriptions are got before, and put after). */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/epoll.h>

#include "ready.h"

#define EPOLL_KNOWN (EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM |     \
                     EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG | EPOLLRDHUP |       \
                     EPOLLEXCLUSIVE | EPOLLWAKEUP | EPOLLONESHOT | EPOLLET)

/* One item. */
struct item {
    struct fd_desc *d;          /* NULL: a free place; not held (epoll_forget) */
    int32_t         fd;         /* the number it was added under */
    uint32_t        events;     /* EPOLL* asked, EPOLLET and EPOLLONESHOT among them */
    uint64_t        data;       /* the program's epoll_data */
    bool            bound;      /* id is an entry of the set (a delivered one-shot's isn't) */
    bool            reported;   /* this look: the set reported it */
    uint32_t        id;         /* its entry */
    struct fd_wait  wait;       /* what its kind's arm said */
    uint32_t        got;        /* EPOLL* ready at this look */
    uint32_t        libc_was;   /* an edge item: the kind's own bits at the last look */
};

struct epoll {
    lock_t               lock;                  /* everything below but next */
    struct netwait      *w;                     /* NETWAIT_MAX entries */
    uint32_t             waiters;               /* threads counted to sleep (above) */
    uint32_t             turn;                  /* where the next look's walk of the items
                                                 * starts, so none starves */
    struct epoll        *next;                  /* every set: sets_lock */
    struct item          items[NETWAIT_MAX];
    struct netwait_ready out[NETWAIT_MAX];      /* a look's reports */
};

static lock_t        sets_lock;
static struct epoll *sets;
static uint32_t      items_live;   /* items in every set: atomic; 0 lets epoll_forget go */

/* ---- items ----------------------------------------------------------------------------- */

static void item_unbind(struct epoll *ep, struct item *it)
{
    if (it->bound)
        (void)netwait_remove(ep->w, it->id);   /* ERR_NOT_FOUND only: already gone */
    it->bound = false;
}

static void item_drop(struct epoll *ep, struct item *it)
{
    item_unbind(ep, it);
    memset(it, 0, sizeof(*it));
    __atomic_sub_fetch(&items_live, 1, __ATOMIC_RELAXED);
}

/* The set's interest for an item's events. */
static uint32_t item_interest(uint32_t events)
{
    return ready_interest(events) | (events & EPOLLET ? NETWAIT_EDGE : 0);
}

/* Bind an item's entry (its wait and events already set): 0, or -errno. */
static long item_bind(struct epoll *ep, struct item *it)
{
    status_t st = ready_add(ep->w, &it->wait, item_interest(it->events), it, &it->id);
    if (st == ERR_NO_RESOURCES)
        return -ENOSPC;   /* Linux's answer for too many items */
    if (st != OK)
        return lx_err(st);
    it->bound = true;
    it->libc_was = 0;
    return 0;
}

static struct item *item_find(struct epoll *ep, int fd, const struct fd_desc *d)
{
    for (uint32_t i = 0; i < NETWAIT_MAX; i++)
        if (ep->items[i].d == d && ep->items[i].fd == fd)
            return &ep->items[i];
    return NULL;
}

void epoll_forget(struct fd_desc *d)
{
    if (!__atomic_load_n(&items_live, __ATOMIC_ACQUIRE))
        return;   /* no item anywhere: the common case, with no lock */
    lock_take(&sets_lock);
    for (struct epoll *ep = sets; ep; ep = ep->next) {
        lock_take(&ep->lock);
        for (uint32_t i = 0; i < NETWAIT_MAX; i++)
            if (ep->items[i].d == d)
                item_drop(ep, &ep->items[i]);
        lock_give(&ep->lock);
    }
    lock_give(&sets_lock);
}

/* ready_wake_all's half: a set with a waiter counted is woken, and its
 * waiters look again (a waiter counts itself under the set's lock after
 * its look, so one not counted yet looks after the change). */
void epoll_wake_all(void)
{
    if (!__atomic_load_n(&items_live, __ATOMIC_ACQUIRE))
        return;   /* no item anywhere: nothing a change could be waited on in */
    lock_take(&sets_lock);
    for (struct epoll *ep = sets; ep; ep = ep->next) {
        lock_take(&ep->lock);
        if (ep->waiters)
            (void)netwait_wake(ep->w);   /* fails only for memory: as ctl's wake */
        lock_give(&ep->lock);
    }
    lock_give(&sets_lock);
}

/* ---- the epoll kind -------------------------------------------------------------------- */

static long epoll_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out)
{
    return -EINVAL;   /* a port can't be bound to a port: not in poll, nor in a set */
}

static long epoll_give(struct fd_desc *d, struct fd_given *out)
{
    return -ENOTSUP;   /* a child gets no epoll descriptor: its items are our descriptions */
}

static void epoll_close(struct fd_desc *d)
{
    struct epoll *ep = d->priv;
    lock_take(&sets_lock);
    for (struct epoll **p = &sets; *p; p = &(*p)->next) {
        if (*p == ep) {
            *p = ep->next;
            break;
        }
    }
    lock_give(&sets_lock);
    for (uint32_t i = 0; i < NETWAIT_MAX; i++)   /* nobody else can see it now */
        if (ep->items[i].d)
            item_drop(ep, &ep->items[i]);
    netwait_destroy(ep->w);
    jam_free(ep);
}

/* A set is a port of ours, which two processes can't share meaningfully:
 * absent in a forked child (docs/FORK-PLAN.md section 1.6), which holds
 * no set at all. */
static const struct fd_ops epoll_ops = {
    .name = "epoll", .arm = epoll_arm, .give = epoll_give, .fork = fd_fork_absent,
    .close = epoll_close,
};

void epoll_forked(void)
{
    sets_lock = (lock_t){ 0 };
    sets = NULL;
    __atomic_store_n(&items_live, 0, __ATOMIC_RELAXED);
}

bool epoll_is(const struct fd_desc *d)
{
    return d->ops == &epoll_ops;
}

long lx_epoll_create1(long flags, long b, long c, long d, long e, long f)
{
    if (flags & ~(long)EPOLL_CLOEXEC)
        return -EINVAL;
    struct epoll *ep = jam_calloc(1, sizeof(*ep));
    if (!ep || netwait_create(NETWAIT_MAX, &ep->w) != OK) {
        jam_free(ep);
        return -ENOMEM;
    }
    struct fd_desc *desc = fd_new(&epoll_ops, O_RDWR, ep);
    if (!desc) {
        netwait_destroy(ep->w);
        jam_free(ep);
        return -ENOMEM;
    }
    lock_take(&sets_lock);
    ep->next = sets;
    sets = ep;
    lock_give(&sets_lock);
    return fd_install(desc, 0, flags & EPOLL_CLOEXEC);   /* a failure closes it: epoll_close */
}

long lx_epoll_create(long size, long b, long c, long d, long e, long f)
{
    return size <= 0 ? -EINVAL : lx_epoll_create1(0, 0, 0, 0, 0, 0);
}

/* ---- epoll_ctl --------------------------------------------------------------------------- */

static long ctl_add(struct epoll *ep, int fd, struct fd_desc *td, const struct epoll_event *ev,
                    const struct fd_wait *wt)
{
    if (item_find(ep, fd, td))
        return -EEXIST;
    struct item *it = NULL;
    for (uint32_t i = 0; !it && i < NETWAIT_MAX; i++)
        if (!ep->items[i].d)
            it = &ep->items[i];
    if (!it)
        return -ENOSPC;
    *it = (struct item){ .d = td, .fd = fd, .events = ev->events, .data = ev->data.u64,
                         .wait = *wt };
    long r = item_bind(ep, it);
    if (r) {
        memset(it, 0, sizeof(*it));
        return r;
    }
    __atomic_add_fetch(&items_live, 1, __ATOMIC_RELEASE);
    return 0;
}

static long ctl_mod(struct epoll *ep, struct item *it, const struct epoll_event *ev,
                    const struct fd_wait *wt)
{
    if (ev->events & EPOLLEXCLUSIVE)
        return -EINVAL;   /* as Linux: only ADD may say it */
    item_unbind(ep, it);
    it->events = ev->events;
    it->data = ev->data.u64;
    it->wait = *wt;
    return item_bind(ep, it);   /* a failure leaves it disabled, as a fired one-shot */
}

/* The checks Linux makes before it looks at the set. */
static long ctl_check(struct fd_desc *ed, struct fd_desc *td, int op, const struct epoll_event *ev)
{
    if (!epoll_is(ed) || td == ed || epoll_is(td))
        return -EINVAL;
    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL)
        return -EINVAL;
    if (op == EPOLL_CTL_DEL)
        return 0;
    if (!ev)
        return -EFAULT;
    if (ev->events & ~(uint32_t)EPOLL_KNOWN)
        return -EINVAL;   /* fail closed: an unknown bit is refused, not ignored */
    return td->ops->arm ? 0 : -EPERM;
}

static long ctl(struct fd_desc *ed, int op, int fd, struct fd_desc *td,
                const struct epoll_event *uev)
{
    struct epoll_event ev = { 0 };
    if (uev && op != EPOLL_CTL_DEL && uc_get(&ev, uev, sizeof(ev)))   /* read once */
        return -EFAULT;   /* DEL never reads it (Linux's) */
    long r = ctl_check(ed, td, op, uev ? &ev : NULL);
    struct fd_wait wt = { 0 };
    if (!r && op != EPOLL_CTL_DEL)
        r = ready_arm(td, ready_interest(ev.events), &wt);
    if (r)
        return r;
    struct epoll *ep = ed->priv;
    lock_take(&ep->lock);
    struct item *it = item_find(ep, fd, td);
    if (op == EPOLL_CTL_ADD)
        r = ctl_add(ep, fd, td, &ev, &wt);
    else if (!it)
        r = -ENOENT;
    else if (op == EPOLL_CTL_MOD)
        r = ctl_mod(ep, it, &ev, &wt);
    else
        item_drop(ep, it);
    if (!r && op != EPOLL_CTL_DEL && ep->waiters)
        (void)netwait_wake(ep->w);   /* fails only for memory: a waiter's next look sees it */
    lock_give(&ep->lock);
    return r;
}

long lx_epoll_ctl(long epfd, long op, long fd, long ev, long e, long f)
{
    struct fd_desc *ed = fd_get(epfd);
    if (!ed)
        return -EBADF;
    struct fd_desc *td = fd_get(fd);
    if (!td) {
        fd_put(ed);
        return -EBADF;
    }
    long r = ctl(ed, (int)op, (int)fd, td, (const struct epoll_event *)(uintptr_t)ev);
    fd_put(td);   /* after the set's lock: a last put calls epoll_forget */
    fd_put(ed);
    return r;
}

/* ---- epoll_wait --------------------------------------------------------------------------- */

/* The kind's own readiness of every live item: all of it for a level
 * item, only what rose since the last look for an edge one. */
static void look_libc(struct epoll *ep)
{
    for (uint32_t i = 0; i < NETWAIT_MAX; i++) {
        struct item *it = &ep->items[i];
        if (!it->d || !it->bound)
            continue;
        uint32_t b = ready_libc(it->d, ready_interest(it->events));
        it->got |= it->events & EPOLLET ? b & ~it->libc_was : b;
        it->libc_was = b;
    }
}

/* Deliver it into evs[*n] (the caller's array) if it has anything asked
 * for and there is room. A store that faults sets *fault (Linux's EFAULT)
 * and the event is kept as if there were no room, for this and the rest. */
static void deliver(struct epoll *ep, struct item *it, struct epoll_event *evs, int max, int *n,
                    bool *fault)
{
    uint32_t b = it->got & (it->events | EPOLLERR | EPOLLHUP);
    bool edge_seen = it->reported && (it->events & EPOLLET);
    it->got = 0;
    it->reported = false;
    if (!b || !it->d)
        return;
    struct epoll_event e = { .events = b, .data.u64 = it->data };
    if (*n < max && !*fault && uc_put(&evs[*n], &e, sizeof(e)))
        *fault = true;
    if (*n == max || *fault) {   /* no room: the next wait must see it again */
        if (edge_seen)
            (void)netwait_touch(ep->w, it->id);   /* the set took it off its list */
        it->libc_was = 0;                         /* an edge item's own rise counts again */
        return;                                   /* a level item is still pending */
    }
    (*n)++;
    if (it->events & EPOLLONESHOT)
        item_unbind(ep, it);
}

/* One look, under the lock: the set's reports first, then the kind's own
 * readiness (ready.h's order). The set is asked for no more than fit:
 * those it reports go to the back of its list and the rest stay at the
 * front, so turns of a few events reach every ready item. Events
 * delivered, or -errno. */
static long collect(struct epoll *ep, struct epoll_event *evs, int max)
{
    uint32_t nr = 0, ask = (uint32_t)max < NETWAIT_MAX ? (uint32_t)max : NETWAIT_MAX;
    status_t st = netwait_wait(ep->w, 0, ep->out, ask, &nr);
    if (st == ERR_TIMED_OUT || st == ERR_CANCELED)
        nr = 0;
    else if (st != OK)
        return lx_err(st);
    for (uint32_t r = 0; r < nr; r++) {
        struct item *it = ep->out[r].user;
        if (it->d && it->bound && it->id == ep->out[r].id) {
            it->got |= ready_bits(ep->out[r].ready, &it->wait);
            it->reported = true;
        }
    }
    look_libc(ep);
    int n = 0;
    bool fault = false;
    for (uint32_t r = 0; r < nr; r++)
        deliver(ep, ep->out[r].user, evs, max, &n, &fault);
    for (uint32_t k = 0; k < NETWAIT_MAX; k++) {
        uint32_t i = (ep->turn + k) % NETWAIT_MAX;
        int before = n;
        deliver(ep, &ep->items[i], evs, max, &n, &fault);
        if (n > before)
            ep->turn = (i + 1) % NETWAIT_MAX;
    }
    return fault && !n ? -EFAULT : n;   /* what was delivered counts, as Linux */
}

static long epoll_run(struct epoll *ep, struct epoll_event *evs, int max, uint64_t deadline)
{
    for (;;) {   /* each turn looks, then sleeps until a packet or the deadline */
        lock_take(&ep->lock);
        long n = collect(ep, evs, max);
        bool sleep = !n && (deadline == DEADLINE_NEVER || now() < deadline);
        if (sleep)
            ep->waiters++;
        else if (n > 0 && ep->waiters)
            (void)netwait_wake(ep->w);   /* what is left may be ready for them */
        lock_give(&ep->lock);
        if (!sleep)
            return n;
        status_t st = netwait_block(ep->w, deadline);
        lock_take(&ep->lock);
        ep->waiters--;
        lock_give(&ep->lock);
        if (st != OK && st != ERR_TIMED_OUT)
            return lx_err(st);   /* ERR_INTERRUPTED: EINTR */
    }
}

long lx_epoll_pwait(long epfd, long evs, long max, long timeout, long mask, long size)
{
    if (max <= 0 || max > INT_MAX / (long)sizeof(struct epoll_event))
        return -EINVAL;
    struct fd_desc *ed = fd_get(epfd);
    if (!ed)
        return -EBADF;
    uint64_t old = 0;
    long r = epoll_is(ed) ? ready_mask_set((const void *)(uintptr_t)mask, size, &old) : -EINVAL;
    if (!r) {
        r = epoll_run(ed->priv, (struct epoll_event *)(uintptr_t)evs, (int)max,
                      ready_deadline_ms((int)timeout));
        ready_mask_restore((const void *)(uintptr_t)mask, old, r);
    }
    fd_put(ed);
    return r;
}

long lx_epoll_wait(long epfd, long evs, long max, long timeout, long e, long f)
{
    return lx_epoll_pwait(epfd, evs, max, timeout, 0, 0);
}
