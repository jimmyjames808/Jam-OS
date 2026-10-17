/* eventfd (docs/M13-PLAN.md section 3): a 64-bit counter in memory and an
 * event whose user bits say what a read or a write would do, for poll
 * and for the waits here.
 *
 * A write adds its 8-byte value to the counter (UINT64_MAX itself is
 * EINVAL); if that would pass EFD_MAX it waits for a read to make room
 * (EAGAIN with O_NONBLOCK). A read takes the whole counter, or 1 with
 * EFD_SEMAPHORE, and waits while it is 0 (EAGAIN with O_NONBLOCK). Reads
 * and writes need at least 8 bytes (EINVAL), in the first iovec piece.
 *
 * The event's EFD_READABLE bit is up exactly while the counter is above
 * 0, its EFD_WRITABLE bit while it is below EFD_MAX; both change with the
 * counter, under the description's lock (fd.h), so a waiter that sees a
 * bit and then finds the counter taken by another thread just waits
 * again. Waits are on the event, with no lock held. poll (section 7)
 * waits on the same two bits.
 *
 * The counter is this process's: a child handed the descriptor gets a
 * copy (the counter at the spawn, and an event of its own), so a write
 * in one process is not seen by the other. Sharing it would need the
 * counter in shared memory, which the hand-over's eventfd kind (one
 * event) has no handle for. */
#include <errno.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include "kinds.h"

#define EFD_READABLE (1u << 24)        /* the counter is above 0 (an event user bit) */
#define EFD_WRITABLE (1u << 25)        /* the counter is below EFD_MAX */
#define EFD_MAX      (UINT64_MAX - 1)  /* the most the counter holds */

struct efd {
    uint64_t count;   /* the counter; the description's lock */
    bool     sem;     /* EFD_SEMAPHORE: a read takes 1 */
    handle_t ev;      /* the event: EFD_READABLE, EFD_WRITABLE */
};

/* What a child is handed (the hand-over's data for POSIXFDS_EVENTFD). */
struct efd_wire {
    uint64_t count;     /* the counter at the spawn */
    uint32_t sem;       /* 1 for EFD_SEMAPHORE, else 0 */
    uint32_t reserved;  /* 0 */
};

static struct efd *efd_of(struct fd_desc *d)
{
    return d->priv;
}

/* The event's bits from the counter. The description's lock held (or e
 * not shared yet). */
static void signal_locked(struct efd *e)
{
    signals_t set = (e->count ? EFD_READABLE : 0) | (e->count < EFD_MAX ? EFD_WRITABLE : 0);
    /* An event we made, with all its rights: nothing it can refuse. */
    (void)jam_event_signal(e->ev, (EFD_READABLE | EFD_WRITABLE) & ~set, set);
}

/* A new eventfd's state: NULL and *err (-errno) if it can't be made. */
static struct efd *efd_new(uint64_t value, bool semaphore, long *err)
{
    struct efd *e = calloc(1, sizeof(*e));
    status_t st = e ? jam_event_create(&e->ev) : ERR_NO_MEMORY;
    if (st != OK) {
        free(e);
        *err = lx_err(st);
        return NULL;
    }
    e->count = value;
    e->sem = semaphore;
    signal_locked(e);
    return e;
}

/* Wait for bit on the event: 0, or -errno (-EINTR for a signal). */
static long wait_bit(struct efd *e, signals_t bit)
{
    signals_t seen;
    status_t st = jam_object_wait_one(e->ev, bit, DEADLINE_NEVER, &seen);
    return st == OK ? 0 : lx_err(st);
}

static long efd_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    if (iovcnt < 1 || iov[0].iov_len < sizeof(uint64_t))
        return -EINVAL;
    if (uc_writable(iov[0].iov_base, sizeof(uint64_t)))
        return -EFAULT;   /* before the count is taken */
    struct efd *e = efd_of(d);
    for (;;) {
        lock_take(&d->lock);
        uint64_t v = e->sem && e->count ? 1 : e->count;
        e->count -= v;
        if (v)
            signal_locked(e);
        lock_give(&d->lock);
        if (v)
            return uc_put(iov[0].iov_base, &v, sizeof(v)) ? -EFAULT : (long)sizeof(v);
        long r = fd_status(d) & O_NONBLOCK ? -EAGAIN : wait_bit(e, EFD_READABLE);
        if (r)
            return r;
    }
}

static long efd_write(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    uint64_t v;
    if (iovcnt < 1 || iov[0].iov_len < sizeof(v))
        return -EINVAL;
    if (uc_get(&v, iov[0].iov_base, sizeof(v)))
        return -EFAULT;
    if (v == UINT64_MAX)
        return -EINVAL;
    struct efd *e = efd_of(d);
    for (;;) {
        lock_take(&d->lock);
        bool room = e->count <= EFD_MAX - v;
        if (room) {
            e->count += v;
            signal_locked(e);
        }
        lock_give(&d->lock);
        if (room)
            return sizeof(v);
        long r = fd_status(d) & O_NONBLOCK ? -EAGAIN : wait_bit(e, EFD_WRITABLE);
        if (r)
            return r;
    }
}

static long efd_stat(struct fd_desc *d, struct stat *st)
{
    struct handle_info hi;
    st->st_mode = 0600;   /* no file type: Linux's anonymous inode */
    st->st_nlink = 1;
    st->st_uid = st->st_gid = 1000;
    st->st_blksize = 4096;
    st->st_ino = jam_handle_info(efd_of(d)->ev, &hi) == OK ? (ino_t)hi.koid : 0;
    return 0;
}

static void efd_close(struct fd_desc *d)
{
    struct efd *e = efd_of(d);
    jam_handle_close(e->ev);
    free(e);
}

static long efd_give(struct fd_desc *d, struct fd_given *out)
{
    struct efd *e = efd_of(d);
    struct efd_wire w = { .sem = e->sem };
    lock_take(&d->lock);
    w.count = e->count;
    lock_give(&d->lock);
    long r = 0;
    void *data = kind_data(&w, sizeof(w), &r);
    if (r)
        return r;
    status_t st = jam_handle_duplicate(e->ev, RIGHT_SAME, &out->handles[0]);
    if (st != OK) {
        free(data);
        return lx_err(st);
    }
    out->kind = POSIXFDS_EVENTFD;
    out->status = fd_status(d);
    out->offset = 0;
    out->nhandles = 1;
    out->data = data;
    out->data_len = sizeof(w);
    return 0;
}

/* poll's hook (ready.h): the event's two bits. */
static long efd_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out)
{
    out->kind = FD_WAIT_HANDLE;
    out->h = (struct netwait_handle){ .h = efd_of(d)->ev, .read = EFD_READABLE,
                                      .write = EFD_WRITABLE };
    return 0;
}

/* fork (docs/FORK-PLAN.md section 1.6): as a spawned child's, the child's
 * counter is its own, its copy of ours at the clone, on a new event the
 * parent makes for it (none: the descriptor is left out of the child). */
static bool efd_fork(struct fd_desc *d, struct fd_fork *f)
{
    struct efd *e = efd_of(d);
    if (!fd_fork_child(f)) {
        handle_t ev;
        if (jam_event_create(&ev) == OK)
            fd_fork_put(f, 0, ev);
        return true;
    }
    e->ev = fd_fork_take(f, 0);
    if (!e->ev)
        return false;
    signal_locked(e);
    return true;
}

const struct fd_ops eventfd_ops = {
    .name = "eventfd",
    .read = efd_read,
    .write = efd_write,
    .stat = efd_stat,
    .arm = efd_arm,
    .give = efd_give,
    .fork = efd_fork,
    .close = efd_close,
};

long eventfd_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                   struct fd_desc **out)
{
    jam_handle_close(hs[0]);   /* the child's counter is its own: see the file's header */
    struct efd_wire w;
    if (rec->data_len != sizeof(w))
        return -EINVAL;
    memcpy(&w, data, sizeof(w));
    if (w.sem > 1 || w.reserved || w.count > EFD_MAX)
        return -EINVAL;
    long err = 0;
    struct efd *e = efd_new(w.count, w.sem, &err);
    if (!e)
        return err;
    *out = fd_new(&eventfd_ops, rec->status, e);
    if (!*out)
        efd_close(&(struct fd_desc){ .priv = e });
    return *out ? 0 : -ENOMEM;
}

long lx_eventfd2(long value, long flags, long c, long d, long e, long f)
{
    if (flags & ~(long)(EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE))
        return -EINVAL;
    long err = 0;
    struct efd *ef = efd_new((uint32_t)value, flags & EFD_SEMAPHORE, &err);
    if (!ef)
        return err;
    uint32_t status = O_RDWR | (flags & EFD_NONBLOCK ? O_NONBLOCK : 0);
    return kind_install(&eventfd_ops, ef, status, flags & EFD_CLOEXEC);
}

long lx_eventfd(long value, long b, long c, long d, long e, long f)
{
    return lx_eventfd2(value, 0, 0, 0, 0, 0);
}
