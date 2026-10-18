/* Record locks (D9, docs/CR-PLAN.md's "RL notes"): fcntl's F_GETLK,
 * F_SETLK and F_SETLKW, the F_OFD_ ones, and flock, kept by the file's
 * service (abi/idl/file.idl's lock, getlock and lockowner), so every
 * process meets them, as Linux's are kept by its kernel:
 *   - fcntl's locks are the process's. The service takes their owner from
 *     the kernel's stamp on each request (no pid is sent), so a forked
 *     child holds none of its parent's; they go when the process ends
 *     (the service's rule: its open closes), and with any close() of a
 *     descriptor of the file (Linux's rule, lock_closing below: fd.c's
 *     close and dup2 call it), even one of another description;
 *   - F_OFD_ and flock locks are the open file description's: the service
 *     makes the description a lock owner at its first such lock
 *     (lockowner) and the description keeps its token, which a forked
 *     child's copy shares (file.c's fork hook), so they go with the
 *     description's last close in whichever process (file.c's close);
 *   - F_SETLKW and a flock without LOCK_NB ask again each time the
 *     service's wake channel says the file's locks changed (or after
 *     WAIT_NAP when it gave none, WAIT_LONGEST at most), until the lock is
 *     granted, EDEADLK, or a signal (EINTR; signal.c restarts it under
 *     SA_RESTART, as Linux restarts F_SETLKW).
 *
 * A description of another kind (a pipe, a memfd, /dev) and a file on a
 * mount with one writer a file (fat, the boot image: fobj.c's `multi`,
 * whose services keep no locks), or a service that answers
 * ERR_NOT_SUPPORTED, keep the old rule: the lock is checked as Linux
 * checks it and granted, never kept, and F_GETLK says F_UNLCK. On fat a
 * second process can't open a file for writing at all while one writes
 * it (fobj.c), so its writers never meet.
 *
 * Not kept, as on Linux they would be: locks across execve (the hand-over
 * opens each file afresh: the old opens close, and their locks with
 * them), and a description handed to a child by posix_spawn or over a
 * socket (an open of its own, with none of the description's OFD or
 * flock locks). */
#include <errno.h>
#include <fcntl.h>
#include <fs_idl.h>
#include <string.h>
#include <sys/file.h>
#include "files.h"
#include "kinds.h"
#include "proc.h"

#define WAIT_NAP     (10 * NS_PER_MS)   /* a conflict without a wake channel: ask again then */
#define WAIT_LONGEST NS_PER_S           /* a wake that never comes: ask again anyway */
#define HELD_MAX     64u                /* files this process holds fcntl locks on, known */

/* The files this process has fcntl locks on (the close rule's), by mount
 * and id (fobj.c's: the file's id there), and the process that took them
 * (a forked child's copy names its parent: not its locks). Past HELD_MAX
 * every close of a file asks (`all`). */
static struct held {
    char     mount[NS_NAME_MAX];
    uint64_t id;
    int32_t  pid;
} held[HELD_MAX];
static unsigned nheld;
static bool     all;
static lock_t   held_lock;

static bool same_file(const struct held *h, const struct fobj *o)
{
    return h->id == o->id && !strcmp(h->mount, o->mount);
}

/* o's file holds fcntl locks of ours now. */
static void held_add(const struct fobj *o)
{
    int32_t pid = lx_pid();
    lock_take(&held_lock);
    bool there = false;
    for (unsigned i = 0; i < nheld && !there; i++)
        there = same_file(&held[i], o) && held[i].pid == pid;
    if (!there && nheld == HELD_MAX)
        all = true;
    else if (!there && o->mount[0]) {
        held[nheld] = (struct held){ .id = o->id, .pid = pid };
        strcpy(held[nheld].mount, o->mount);
        __atomic_store_n(&nheld, nheld + 1, __ATOMIC_RELEASE);
    } else if (!there) {
        all = true;   /* a file whose mount isn't known (sent over a socket) */
    }
    lock_give(&held_lock);
}

/* Did o's file hold fcntl locks of ours (forgotten now)? */
static bool held_take(const struct fobj *o)
{
    int32_t pid = lx_pid();
    bool was = false;
    lock_take(&held_lock);
    for (unsigned i = 0; i < nheld;) {
        if (!same_file(&held[i], o)) {
            i++;
            continue;
        }
        was |= held[i].pid == pid;
        held[i] = held[--nheld];   /* ours, or a parent's (a forked child's copy) */
    }
    was |= all;
    lock_give(&held_lock);
    return was;
}

/* errno for a service's answer. */
static long lock_err(status_t st)
{
    switch (st) {
    case ERR_NO_RESOURCES: return -ENOLCK;
    case ERR_ACCESS_DENIED: return -EBADF;
    case ERR_INVALID_ARGS: return -EINVAL;
    case ERR_PEER_CLOSED: return -EIO;
    }
    return lx_err(st);
}

/* The file's open with a channel (lent to a child: ours again first). o->lock. */
static status_t ready_locked(const struct file_lockref *r)
{
    status_t st = fobj_back_locked(r->o, r->path);
    return st == OK && !r->o->jf.ch ? ERR_IO : st;
}

/* The description's lock owner, made at its first OFD or flock lock (*fresh:
 * the one it had went, so a new one). d->lock. */
static long owner_locked(const struct file_lockref *r, bool fresh, uint64_t *out)
{
    if (fresh && *r->owner) {
        if (*r->token)
            jam_handle_close(*r->token);
        *r->owner = 0;
        *r->token = HANDLE_INVALID;
    }
    for (unsigned tries = 0; !*r->owner && tries < 2; tries++) {
        uint64_t id = 0;
        handle_t tok[1];
        uint32_t n = 0;
        lock_take(&r->o->lock);
        status_t st = ready_locked(r);
        if (st == OK)
            st = file_lockowner_within(r->o->jf.ch, FILE_TIMEOUT, &id, tok, &n);
        lock_give(&r->o->lock);
        if (st != OK)
            return lock_err(st);
        if (n == 1 && id) {
            *r->owner = id;
            *r->token = tok[0];
        } else if (n == 1) {
            jam_handle_close(tok[0]);
        }
        /* none: a reply the service sent again after its restart; ask again */
    }
    *out = *r->owner;
    return *out ? 0 : -EIO;
}

/* One ask: *result FILE_LOCK_*, *wake the wake channel or 0. */
static long ask(struct fd_desc *d, const struct file_lockref *r, uint8_t type, uint8_t flags,
                uint64_t start, uint64_t end, uint8_t *result, handle_t *wake)
{
    uint8_t kind = flags & FILE_LOCK_KINDS;
    *wake = HANDLE_INVALID;
    for (bool fresh = false;; fresh = true) {
        uint64_t owner = 0;
        if (kind != FILE_LOCK_POSIX) {
            lock_take(&d->lock);
            long e = owner_locked(r, fresh, &owner);
            lock_give(&d->lock);
            if (e)
                return e;
        }
        handle_t w[1];
        uint32_t n = 0;
        lock_take(&r->o->lock);
        status_t st = ready_locked(r);
        if (st == OK)
            st = file_lock_within(r->o->jf.ch, FILE_TIMEOUT, type, flags, start, end, owner,
                                  result, w, &n);
        lock_give(&r->o->lock);
        if (st == OK && n == 1)
            *wake = w[0];
        if (st == ERR_NOT_FOUND && kind != FILE_LOCK_POSIX && !fresh)
            continue;   /* the owner went (the service started fresh): a new one */
        return st == OK ? 0 : lock_err(st);
    }
}

/* Lock (type FILE_LOCK_*) start..end of d's file for the owner kind in
 * flags, waiting if `wait`. 0, -EAGAIN (in the way), -EDEADLK, -EINTR,
 * -ENOLCK, or 1: the service keeps no locks (the old rule). */
static long set(struct fd_desc *d, uint8_t type, uint8_t flags, uint64_t start, uint64_t end,
                bool wait)
{
    struct file_lockref r;
    if (file_lockref(d, &r) || !r.o->multi)
        return 1;
    for (;;) {
        uint8_t result = FILE_LOCK_GRANTED;
        handle_t wake;
        long e = ask(d, &r, type, flags | (wait ? FILE_LOCK_WAIT : 0), start, end, &result,
                     &wake);
        if (e == -ENOTSUP)
            return 1;   /* ERR_NOT_SUPPORTED: a service that keeps none */
        if (e)
            return e;
        if (result == FILE_LOCK_GRANTED) {
            if ((flags & FILE_LOCK_KINDS) == FILE_LOCK_POSIX && type != FILE_LOCK_UNLOCK)
                held_add(r.o);
            return 0;
        }
        if (result != FILE_LOCK_CONFLICT || !wait) {
            if (wake)
                jam_handle_close(wake);
            return result == FILE_LOCK_DEADLOCK ? -EDEADLK : -EAGAIN;
        }
        uint64_t deadline = now() + (wake ? WAIT_LONGEST : WAIT_NAP);
        status_t st = wake ? jam_object_wait_one(wake, SIG_PEER_CLOSED, deadline, NULL)
                           : jam_sleep_until(deadline);
        if (wake)
            jam_handle_close(wake);
        if (st == ERR_INTERRUPTED)
            return -EINTR;   /* the service sees our end closed: the wait is forgotten */
    }
}

void lock_closing(struct fd_desc *d)
{
    struct file_lockref r;
    if ((!__atomic_load_n(&nheld, __ATOMIC_ACQUIRE) && !all) || file_lockref(d, &r) ||
        !r.o->multi || !held_take(r.o))
        return;
    uint8_t result;
    lock_take(&r.o->lock);
    status_t st = r.o->jf.ch ? OK : ERR_IO;   /* lent: a forked child's, which holds none */
    if (st == OK)
        st = file_lock_within(r.o->jf.ch, FILE_TIMEOUT, FILE_LOCK_UNLOCK, FILE_LOCK_POSIX, 0,
                              FILE_LOCK_EOF, 0, &result, NULL, NULL);
    lock_give(&r.o->lock);
}

/* ---- fcntl ------------------------------------------------------------------------ */

/* l's bytes from the start of d's file, Linux's flock_to_posix_lock: 0, or
 * -EINVAL (before the start), -EOVERFLOW (past the largest offset). */
static long range(struct fd_desc *d, const struct flock *l, uint64_t *start, uint64_t *end)
{
    int64_t base = 0;
    if (l->l_whence == SEEK_CUR) {
        lock_take(&d->lock);
        base = d->offset;
        lock_give(&d->lock);
    } else if (l->l_whence == SEEK_END) {
        struct stat st = { 0 };
        long r = d->ops->stat ? d->ops->stat(d, &st) : 0;
        if (r)
            return r;
        base = st.st_size;
    }
    if ((l->l_start > 0 && base > INT64_MAX - l->l_start))
        return -EOVERFLOW;
    int64_t s = base + l->l_start;
    if (s < 0)
        return -EINVAL;
    if (l->l_len > 0) {
        if (l->l_len - 1 > INT64_MAX - s)
            return -EOVERFLOW;
        *start = (uint64_t)s;
        *end = (uint64_t)(s + l->l_len - 1);
    } else if (l->l_len < 0) {
        if (s + l->l_len < 0)
            return -EINVAL;
        *start = (uint64_t)(s + l->l_len);
        *end = (uint64_t)s - 1;
    } else {
        *start = (uint64_t)s;
        *end = FILE_LOCK_EOF;
    }
    return 0;
}

static long getlk(struct fd_desc *d, struct flock *l, uint8_t kind, uint64_t start, uint64_t end)
{
    struct file_lockref r;
    uint8_t type = l->l_type == F_WRLCK ? FILE_LOCK_WRITE : FILE_LOCK_READ, held = 0;
    uint64_t hs = 0, he = 0, owner = 0;
    uint32_t pid = 0;
    status_t st = ERR_NOT_SUPPORTED;
    if (!file_lockref(d, &r) && r.o->multi) {
        if (kind != FILE_LOCK_POSIX) {   /* none yet: it holds nothing, so asks as a new owner */
            lock_take(&d->lock);
            long e = owner_locked(&r, false, &owner);
            lock_give(&d->lock);
            if (e)
                return e;
        }
        lock_take(&r.o->lock);
        st = ready_locked(&r);
        if (st == OK)
            st = file_getlock_within(r.o->jf.ch, FILE_TIMEOUT, type, kind, start, end, owner,
                                     &held, &hs, &he, &pid);
        lock_give(&r.o->lock);
    }
    if (st != OK && st != ERR_NOT_SUPPORTED)
        return lock_err(st);
    if (st != OK || held == FILE_LOCK_UNLOCK) {
        l->l_type = F_UNLCK;   /* nobody holds one in the way (or nobody keeps them) */
        return 0;
    }
    l->l_type = held == FILE_LOCK_WRITE ? F_WRLCK : F_RDLCK;
    l->l_whence = SEEK_SET;
    l->l_start = (off_t)hs;
    l->l_len = he == FILE_LOCK_EOF ? 0 : (off_t)(he - hs + 1);
    l->l_pid = pid ? (pid_t)pid : -1;   /* the holder's, a process's; an OFD lock's -1 */
    return 0;
}

long lock_fcntl(struct fd_desc *d, long cmd, long arg)
{
    struct flock l;
    if (uc_get(&l, (const void *)(uintptr_t)arg, sizeof(l)))
        return -EFAULT;
    if (l.l_whence != SEEK_SET && l.l_whence != SEEK_CUR && l.l_whence != SEEK_END)
        return -EINVAL;
    if (l.l_type != F_RDLCK && l.l_type != F_WRLCK && l.l_type != F_UNLCK)
        return -EINVAL;
    bool ofd = cmd == F_OFD_GETLK || cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW;
    if (ofd && l.l_pid)
        return -EINVAL;   /* Linux's: an OFD lock names no process */
    uint8_t kind = ofd ? FILE_LOCK_OFD : FILE_LOCK_POSIX;
    uint64_t start, end;
    long r = range(d, &l, &start, &end);
    if (r)
        return r;
    if (cmd == F_GETLK || cmd == F_OFD_GETLK) {
        if (l.l_type == F_UNLCK)
            return -EINVAL;
        r = getlk(d, &l, kind, start, end);
        return r ? r : uc_put((void *)(uintptr_t)arg, &l, sizeof(l));
    }
    if ((l.l_type == F_RDLCK && !fd_can_read(d)) || (l.l_type == F_WRLCK && !fd_can_write(d)))
        return -EBADF;
    uint8_t type = l.l_type == F_RDLCK   ? FILE_LOCK_READ
                   : l.l_type == F_WRLCK ? FILE_LOCK_WRITE
                                         : FILE_LOCK_UNLOCK;
    r = set(d, type, kind, start, end, cmd == F_SETLKW || cmd == F_OFD_SETLKW);
    return r > 0 ? 0 : r;   /* 1: nobody keeps them, granted */
}

long lock_flock(struct fd_desc *d, long op)
{
    long how = op & ~(long)LOCK_NB;
    if (how != LOCK_SH && how != LOCK_EX && how != LOCK_UN)
        return -EINVAL;
    if ((fd_status(d) & O_PATH))
        return -EBADF;
    uint8_t type = how == LOCK_SH ? FILE_LOCK_READ : how == LOCK_EX ? FILE_LOCK_WRITE
                                                                    : FILE_LOCK_UNLOCK;
    long r = set(d, type, FILE_LOCK_FLOCK, 0, FILE_LOCK_EOF, !(op & LOCK_NB));
    return r > 0 ? 0 : r == -EAGAIN ? -EWOULDBLOCK : r;
}
