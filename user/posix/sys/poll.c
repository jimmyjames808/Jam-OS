/* poll, ppoll, select and pselect (docs/M13-PLAN.md section 7), on a wait
 * set (<netwait.h>) kept per thread, with a cache of its entries. How a
 * descriptor kind is waited on, and why no wake is lost to the bytes libc
 * keeps itself: ready.h.
 *
 * **The cache.** A thread's set lives from its first poll to its exit (a
 * thread-specific key's destructor frees it). Each description asked for
 * keeps its place in the set from call to call, by its serial, with what
 * its kind's arm said and the interest it was bound for: one polled again
 * the same way costs nothing to set up, so a loop polling the same few
 * descriptors costs the ready ones (one look each), not a bind and an
 * unbind of each every time. A place not asked for in CACHE_AGE calls is removed, and so
 * is one the set reports ready when the call didn't ask for it (it would
 * only be looked at again on every call). A description closed in the
 * meantime needs nothing: its handles' bindings ended in the kernel as
 * they closed, and its serial is never given again, so the place is never
 * mistaken for a new description's; it ages out.
 *
 * **A call.** Gather: each pollfd's description, held for the call (the
 * same description twice, the same fd or a dup, is one item, asked for
 * the union of the events). Bind: each item's arm, its place kept, bound
 * again or made. Wait: look at the set (a past deadline), then at libc's
 * own readiness; if nothing is ready, sleep in netwait_block until a
 * packet or the deadline, and look again. thread_interrupt ends the sleep:
 * EINTR (signals restart or not: signal.c's rule).
 *
 * **Limits.** 1024 pollfds a call (RLIMIT_NOFILE's answer) and NETWAIT_MAX
 * set entries: more is EINVAL. select (select.c) is a poll_run.
 *
 * Everything here is the calling thread's own: no locks. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "pollcore.h"
#include "ready.h"

#define CACHE_AGE    64u      /* calls a place may go unasked before it is removed */
#define NO_ITEM      0xffffu

/* A description's place in the set, kept from call to call. */
struct place {
    uint64_t       serial;     /* the description's; 0: a free place */
    uint32_t       last;       /* the call that last asked for it */
    uint16_t       item;       /* this call's item, or NO_ITEM */
    bool           bound;      /* id is an entry in the set */
    uint32_t       id;         /* its entry */
    uint32_t       interest;   /* NETWAIT_* it was armed and bound for */
    struct fd_wait wait;       /* what its kind's arm said */
};

/* One description asked for in this call. */
struct item {
    struct fd_desc *d;         /* held for the call */
    uint32_t        events;    /* POLL* asked: its pollfds' together */
    uint32_t        always;    /* POLL* ready with no wait: a kind with no arm */
    uint32_t        got;       /* POLL* ready at the last look */
};

struct pollset {
    struct netwait      *w;                       /* NETWAIT_MAX entries */
    uint32_t             calls;                   /* calls made so far */
    uint32_t             nitems;                  /* this call's items */
    struct place         places[NETWAIT_MAX];
    struct item          items[NETWAIT_MAX];
    uint16_t             map[POLL_FDS_MAX];       /* pollfd -> its item, or NO_ITEM */
    struct netwait_ready out[NETWAIT_MAX];        /* a look's reports */
    struct pollfd        fds[POLL_FDS_MAX];       /* select's, as pollfds (poll_scratch) */
};

static __thread struct pollset *self_set;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static pthread_key_t  set_key;
static bool           key_ok;

/* ---- the thread's set ----------------------------------------------------------------- */

static void set_free(void *p)
{
    struct pollset *ps = p;
    netwait_destroy(ps->w);   /* its port, and every binding with it */
    free(ps);
    self_set = NULL;
}

static void key_make(void)
{
    key_ok = pthread_key_create(&set_key, set_free) == 0;
}

/* The calling thread's set, made on its first call; NULL: no memory. */
static struct pollset *set_get(void)
{
    if (self_set)
        return self_set;
    pthread_once(&key_once, key_make);
    struct pollset *ps = calloc(1, sizeof(*ps));
    if (!ps || !key_ok || netwait_create(NETWAIT_MAX, &ps->w) != OK) {
        free(ps);
        return NULL;
    }
    for (uint32_t i = 0; i < NETWAIT_MAX; i++)
        ps->places[i].item = NO_ITEM;
    if (pthread_setspecific(set_key, ps) != 0) {   /* freed at the thread's exit, or never made */
        netwait_destroy(ps->w);
        free(ps);
        return NULL;
    }
    self_set = ps;
    return ps;
}

/* ---- places ------------------------------------------------------------------------------ */

static void place_drop(struct pollset *ps, struct place *pl)
{
    if (pl->bound)
        (void)netwait_remove(ps->w, pl->id);   /* ERR_NOT_FOUND only: already gone */
    memset(pl, 0, sizeof(*pl));
    pl->item = NO_ITEM;
}

static struct place *place_find(struct pollset *ps, uint64_t serial)
{
    for (uint32_t i = 0; i < NETWAIT_MAX; i++)
        if (ps->places[i].serial == serial)
            return &ps->places[i];
    return NULL;
}

/* Remove the place this call doesn't ask for that was asked for longest
 * ago: false if every place is this call's. */
static bool evict_one(struct pollset *ps)
{
    struct place *old = NULL;
    for (uint32_t i = 0; i < NETWAIT_MAX; i++) {
        struct place *pl = &ps->places[i];
        if (pl->serial && pl->item == NO_ITEM && (!old || ps->calls - pl->last >
                                                              ps->calls - old->last))
            old = pl;
    }
    if (old)
        place_drop(ps, old);
    return old != NULL;
}

/* A free place, making one if need be; NULL: every place is this call's. */
static struct place *place_take(struct pollset *ps)
{
    struct place *pl = place_find(ps, 0);
    if (!pl && evict_one(ps))
        pl = place_find(ps, 0);
    return pl;
}

/* Item k's place, with its kind armed for in: kept if the arm and the
 * interest are the same as last time, else bound again. 0, or -errno. */
static long place_bind(struct pollset *ps, uint16_t k, uint32_t in, const struct fd_wait *wt)
{
    struct item *it = &ps->items[k];
    struct place *pl = place_find(ps, it->d->serial);
    if (pl && pl->interest == in && !memcmp(&pl->wait, wt, sizeof(*wt))) {
        pl->item = k;
        pl->last = ps->calls;
        return 0;
    }
    if (pl)
        place_drop(ps, pl);
    else if (!(pl = place_take(ps)))
        return -EINVAL;   /* more descriptions than a set holds */
    pl->serial = it->d->serial;
    pl->item = k;   /* this call's: never the one evicted below */
    pl->last = ps->calls;
    pl->interest = in;
    pl->wait = *wt;
    status_t st;
    do
        st = ready_add(ps->w, wt, in, pl, &pl->id);
    while (st == ERR_NO_RESOURCES && evict_one(ps));
    if (st != OK) {
        place_drop(ps, pl);
        return st == ERR_NO_RESOURCES ? -EINVAL : lx_err(st);
    }
    pl->bound = true;
    return 0;
}

/* ---- a call ---------------------------------------------------------------------------- */

static uint16_t item_of(const struct pollset *ps, const struct fd_desc *d)
{
    for (uint16_t k = 0; k < ps->nitems; k++)
        if (ps->items[k].d == d)
            return k;
    return NO_ITEM;
}

/* Each pollfd's description, held; one item per description. 0, or -EINVAL
 * for more descriptions than a set holds. */
static long gather(struct pollset *ps, struct pollfd *fds, uint32_t n)
{
    ps->nitems = 0;
    for (uint32_t i = 0; i < n; i++) {
        fds[i].revents = 0;
        ps->map[i] = NO_ITEM;
        if (fds[i].fd < 0)
            continue;   /* ignored, as POSIX says */
        struct fd_desc *d = fd_get(fds[i].fd);
        if (!d) {
            fds[i].revents = POLLNVAL;
            continue;
        }
        uint16_t k = item_of(ps, d);
        if (k != NO_ITEM) {
            fd_put(d);   /* held once already */
        } else if (ps->nitems == NETWAIT_MAX) {
            fd_put(d);
            return -EINVAL;
        } else {
            k = (uint16_t)ps->nitems++;
            ps->items[k] = (struct item){ .d = d };
        }
        ps->items[k].events |= (uint16_t)fds[i].events;
        ps->map[i] = k;
    }
    return 0;
}

static long bind_all(struct pollset *ps)
{
    for (uint16_t k = 0; k < ps->nitems; k++) {
        struct item *it = &ps->items[k];
        if (!it->d->ops->arm) {
            it->always = ready_always(it->d);
            continue;
        }
        uint32_t in = ready_interest(it->events);
        struct fd_wait wt;
        long r = ready_arm(it->d, in, &wt);
        if (!r)
            r = place_bind(ps, k, in, &wt);
        if (r)
            return r;
    }
    return 0;
}

/* One look: the set's reports, then libc's own readiness (in that order:
 * ready.h's "Why no wake is lost"), into revents. The pollfds ready, or
 * -errno. */
static long look(struct pollset *ps, struct pollfd *fds, uint32_t n)
{
    for (uint16_t k = 0; k < ps->nitems; k++)
        ps->items[k].got = ps->items[k].always;
    uint32_t nr = 0;
    status_t st = netwait_wait(ps->w, 0, ps->out, NETWAIT_MAX, &nr);
    if (st != OK && st != ERR_TIMED_OUT)
        return lx_err(st);
    for (uint32_t r = 0; st == OK && r < nr; r++) {
        struct place *pl = ps->out[r].user;
        if (!pl->serial)
            continue;   /* dropped by an earlier report of this look */
        if (pl->item == NO_ITEM)
            place_drop(ps, pl);   /* not asked for: it would be looked at every call */
        else
            ps->items[pl->item].got |= ready_bits(ps->out[r].ready, &pl->wait);
    }
    for (uint16_t k = 0; k < ps->nitems; k++) {
        struct item *it = &ps->items[k];
        it->got |= ready_libc(it->d, ready_interest(it->events));
    }
    long count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (ps->map[i] != NO_ITEM) {
            uint32_t ev = (uint16_t)fds[i].events | POLLERR | POLLHUP;
            fds[i].revents = (short)(ps->items[ps->map[i]].got & ev);
        }
        count += fds[i].revents != 0;
    }
    return count;
}

static long wait_ready(struct pollset *ps, struct pollfd *fds, uint32_t n, uint64_t deadline)
{
    for (;;) {   /* each turn looks, then sleeps until a packet or the deadline */
        long count = look(ps, fds, n);
        if (count)
            return count;
        if (deadline != DEADLINE_NEVER && now() >= deadline)
            return 0;
        status_t st = netwait_block(ps->w, deadline);
        if (st != OK && st != ERR_TIMED_OUT)
            return lx_err(st);   /* ERR_INTERRUPTED: EINTR */
    }
}

/* Let go of the call's descriptions; age the places. */
static void release(struct pollset *ps)
{
    for (uint16_t k = 0; k < ps->nitems; k++)
        fd_put(ps->items[k].d);
    ps->nitems = 0;
    for (uint32_t i = 0; i < NETWAIT_MAX; i++) {
        struct place *pl = &ps->places[i];
        pl->item = NO_ITEM;
        if (pl->serial && ps->calls - pl->last >= CACHE_AGE)
            place_drop(ps, pl);
    }
}

long poll_run(struct pollfd *fds, unsigned long n, uint64_t deadline)
{
    if (n > POLL_FDS_MAX)
        return -EINVAL;
    struct pollset *ps = set_get();
    if (!ps)
        return -ENOMEM;
    ps->calls++;
    long r = gather(ps, fds, (uint32_t)n);
    if (!r)
        r = bind_all(ps);
    if (!r)
        r = wait_ready(ps, fds, (uint32_t)n, deadline);
    release(ps);
    return r;
}

bool poll_fd_open(int fd)
{
    struct fd_desc *d = fd_get(fd);
    if (d)
        fd_put(d);
    return d != NULL;
}

struct pollfd *poll_scratch(void)
{
    struct pollset *ps = set_get();
    return ps ? ps->fds : NULL;
}

long lx_poll(long fds, long n, long timeout, long d, long e, long f)
{
    return poll_run((struct pollfd *)(uintptr_t)fds, (unsigned long)n,
                    ready_deadline_ms((int)timeout));
}

long lx_ppoll(long fds, long n, long ts, long mask, long size, long f)
{
    uint64_t deadline = DEADLINE_NEVER, old = 0;
    const struct timespec *t = (const struct timespec *)(uintptr_t)ts;
    long r = t ? ready_deadline(t->tv_sec, t->tv_nsec, &deadline) : 0;
    if (!r)
        r = ready_mask_set((const void *)(uintptr_t)mask, size, &old);
    if (r)
        return r;
    r = poll_run((struct pollfd *)(uintptr_t)fds, (unsigned long)n, deadline);
    ready_mask_restore((const void *)(uintptr_t)mask, old);
    return r;
}
