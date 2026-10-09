/* The descriptor table (fd.h has the model): the slots, the open file
 * descriptions' references, the hand-over made into descriptors at the
 * start, and the calls that only route to a description's kind (read,
 * write and their vector and positional forms, lseek, fstat, ioctl,
 * fsync, ftruncate, getdents64, close).
 *
 * The table is a static array of FD_MAX slots under one lock
 * (table_lock), never held across a kind's operation. A slot is empty, a
 * description, or RESERVED (fd_alloc's: taken by a call still making its
 * description). A description is freed when its last reference goes:
 * each slot pointing at it holds one, and so does each call using it.
 *
 * The start: start.c reads the hand-over before musl runs, so fd_stage
 * only records (no malloc, no errno): each descriptor's record and
 * handles in `staged`, its data in a VMO mapped for the purpose (whole
 * hand-overs are at most FD_MAX * POSIXFDS_DATA_MAX bytes of data, and
 * only the pages touched are committed). fd_start, a constructor that
 * runs before the program's own, makes them descriptions through
 * fd_adopters and lets the staging go. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "fd.h"
#include "kinds.h"

/* The reserved mark: a slot fd_alloc gave out, not filled yet. */
static struct fd_desc reserved_mark;
#define RESERVED (&reserved_mark)

static struct fd_desc *slots[FD_MAX];   /* table_lock */
static bool            cloexec[FD_MAX]; /* table_lock */
static lock_t          table_lock;

/* ---- descriptions ---------------------------------------------------------------- */

struct fd_desc *fd_new(const struct fd_ops *ops, uint32_t status, void *priv)
{
    struct fd_desc *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->ops = ops;
    d->priv = priv;
    d->refs = 1;
    d->status = status;
    return d;
}

void fd_ref(struct fd_desc *d)
{
    __atomic_add_fetch(&d->refs, 1, __ATOMIC_RELAXED);
}

void fd_put(struct fd_desc *d)
{
    if (__atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL))
        return;
    if (d->ops->close)
        d->ops->close(d);
    free(d);
}

uint32_t fd_status(const struct fd_desc *d)
{
    return __atomic_load_n(&d->status, __ATOMIC_RELAXED);
}

long fd_status_set(struct fd_desc *d, uint32_t mask, uint32_t bits)
{
    lock_take(&d->lock);   /* two setters never lose each other's bits */
    uint32_t old = fd_status(d), new = (old & ~mask) | (bits & mask);
    long r = d->ops->setfl && new != old ? d->ops->setfl(d, old, new) : 0;
    if (!r)
        __atomic_store_n(&d->status, new, __ATOMIC_RELAXED);
    lock_give(&d->lock);
    return r;
}

bool fd_can_read(const struct fd_desc *d)
{
    uint32_t s = fd_status(d);
    return !(s & O_PATH) && (s & O_ACCMODE) != O_WRONLY;
}

bool fd_can_write(const struct fd_desc *d)
{
    uint32_t s = fd_status(d);
    return !(s & O_PATH) && (s & O_ACCMODE) != O_RDONLY;
}

/* ---- the table ----------------------------------------------------------------- */

static void start_once(void);

static bool in_range(long fd)
{
    return fd >= 0 && fd < FD_MAX;
}

struct fd_desc *fd_get(long fd)
{
    start_once();
    if (!in_range(fd))
        return NULL;
    lock_take(&table_lock);
    struct fd_desc *d = slots[fd] == RESERVED ? NULL : slots[fd];
    if (d)
        fd_ref(d);
    lock_give(&table_lock);
    return d;
}

long fd_alloc(long min)
{
    if (!in_range(min))
        return -EINVAL;
    long r = -EMFILE;
    lock_take(&table_lock);
    for (long fd = min; fd < FD_MAX; fd++)
        if (!slots[fd]) {
            slots[fd] = RESERVED;
            r = fd;
            break;
        }
    lock_give(&table_lock);
    return r;
}

void fd_set(long fd, struct fd_desc *d, bool cloexec_on)
{
    lock_take(&table_lock);
    slots[fd] = d;
    cloexec[fd] = cloexec_on;
    lock_give(&table_lock);
}

void fd_free(long fd)
{
    lock_take(&table_lock);
    if (slots[fd] == RESERVED)
        slots[fd] = NULL;
    lock_give(&table_lock);
}

long fd_install(struct fd_desc *d, long min, bool cloexec_on)
{
    long fd = fd_alloc(min);
    if (fd < 0)
        fd_put(d);
    else
        fd_set(fd, d, cloexec_on);
    return fd;
}

long fd_replace(long fd, struct fd_desc *d, bool cloexec_on)
{
    if (!in_range(fd)) {
        fd_put(d);
        return -EBADF;
    }
    lock_take(&table_lock);
    struct fd_desc *old = slots[fd];
    if (old != RESERVED) {
        slots[fd] = d;
        cloexec[fd] = cloexec_on;
    }
    lock_give(&table_lock);
    if (old == RESERVED) {
        fd_put(d);
        return -EBUSY;
    }
    if (old)
        fd_put(old);   /* outside the lock: its close may call its service */
    return fd;
}

long fd_close(long fd)
{
    start_once();
    if (!in_range(fd))
        return -EBADF;
    lock_take(&table_lock);
    struct fd_desc *d = slots[fd] == RESERVED ? NULL : slots[fd];
    if (d)
        slots[fd] = NULL;
    lock_give(&table_lock);
    if (!d)
        return -EBADF;
    fd_put(d);
    return 0;
}

long fd_cloexec(long fd)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return -EBADF;
    lock_take(&table_lock);
    long r = cloexec[fd];
    lock_give(&table_lock);
    fd_put(d);
    return r;
}

long fd_set_cloexec(long fd, bool on)
{
    long r = -EBADF;
    start_once();
    if (!in_range(fd))
        return r;
    lock_take(&table_lock);
    if (slots[fd] && slots[fd] != RESERVED) {
        cloexec[fd] = on;
        r = 0;
    }
    lock_give(&table_lock);
    return r;
}

long fd_next(long from, struct fd_desc **d, bool *cloexec_out)
{
    start_once();
    long r = -1;
    lock_take(&table_lock);
    for (long fd = from < 0 ? 0 : from; fd < FD_MAX; fd++)
        if (slots[fd] && slots[fd] != RESERVED) {
            *d = slots[fd];
            fd_ref(*d);
            *cloexec_out = cloexec[fd];
            r = fd;
            break;
        }
    lock_give(&table_lock);
    return r;
}

long fd_give(long fd, struct fd_given *out)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return -EBADF;
    memset(out, 0, sizeof(*out));
    long r = d->ops->give ? d->ops->give(d, out) : -ENOTSUP;
    fd_put(d);
    return r;
}

void fd_given_drop(struct fd_given *g)
{
    for (uint32_t i = 0; i < g->nhandles; i++)
        jam_handle_close(g->handles[i]);
    free(g->data);
    memset(g, 0, sizeof(*g));
}

/* ---- the hand-over at the start ----------------------------------------------- */

#define STAGE_DATA_MAX ((uint64_t)FD_MAX * POSIXFDS_DATA_MAX)

struct staged {
    struct posixfds_fd rec;                     /* as the reader checked it */
    handle_t           hs[POSIXFDS_KIND_HANDLES];
    uint64_t           data_at;                 /* its data's place in stage_data */
};

static struct staged staged[FD_MAX];   /* before main: one thread */
static unsigned      nstaged;
static bool          stage_default;    /* no hand-over: stdfd_default (dev.c) */
static uint8_t      *stage_data;       /* mapped stage_vmo, made at the first data */
static handle_t      stage_vmo;
static uint64_t      stage_used;
static lock_t        start_lock;
static bool          started;          /* start_lock; read atomically */

/* The staging's data area, unmapped and closed. */
static void stage_data_free(void)
{
    if (stage_data)
        jam_vmar_unmap(lx_self_vmar(), (uint64_t)(uintptr_t)stage_data, STAGE_DATA_MAX);
    if (stage_vmo)
        jam_handle_close(stage_vmo);
    stage_data = NULL;
    stage_vmo = HANDLE_INVALID;
    stage_used = 0;
}

/* Room for n more bytes of data: its offset, or an error. */
static status_t stage_room(uint32_t n, uint64_t *at)
{
    if (!stage_data) {
        uint64_t addr = 0;
        status_t st = jam_vmo_create(STAGE_DATA_MAX, 0, &stage_vmo);
        if (st == OK)
            st = jam_vmar_map(lx_self_vmar(), stage_vmo, 0, STAGE_DATA_MAX,
                              VMAR_READ | VMAR_WRITE, &addr);
        if (st != OK) {
            stage_data_free();
            return st;
        }
        stage_data = (uint8_t *)(uintptr_t)addr;
    }
    uint64_t len = ((uint64_t)n + 7) & ~7ull;
    if (len > STAGE_DATA_MAX - stage_used)
        return ERR_NO_MEMORY;   /* the reader's limits make this impossible */
    *at = stage_used;
    stage_used += len;
    return OK;
}

status_t fd_stage(const struct posixfds_fd *rec, const void *data, const handle_t *hs)
{
    uint64_t at = 0;
    status_t st = nstaged < FD_MAX ? OK : ERR_NO_RESOURCES;
    if (st == OK && rec->data_len)
        st = stage_room(rec->data_len, &at);
    if (st != OK) {   /* the handles are ours whatever we return */
        for (unsigned i = 0; i < rec->nhandles; i++)
            jam_handle_close(hs[i]);
        return st;
    }
    struct staged *s = &staged[nstaged++];
    s->rec = *rec;
    memcpy(s->hs, hs, rec->nhandles * sizeof(handle_t));
    s->data_at = at;
    if (rec->data_len)
        memcpy(stage_data + at, data, rec->data_len);
    return OK;
}

void fd_stage_reset(void)
{
    for (unsigned i = 0; i < nstaged; i++)
        for (unsigned k = 0; k < staged[i].rec.nhandles; k++)
            jam_handle_close(staged[i].hs[k]);
    nstaged = 0;
    stage_data_free();
}

void fd_stage_default(void)
{
    stage_default = true;
}

/* The hand-over's kinds, as this program takes them: one line per kind,
 * each the kind's own file's (fd.h). */
fd_adopt_fn *const fd_adopters[POSIXFDS_KIND_COUNT] = {
    [POSIXFDS_PIPE] = pipe_adopt,
    [POSIXFDS_DEV] = dev_adopt,
    [POSIXFDS_ETC] = etc_adopt,
    [POSIXFDS_MEMFD] = memfd_adopt,
    [POSIXFDS_EVENTFD] = eventfd_adopt,
};

/* "posix: descriptor <n> (<kind>) left out: <why>\n" in the log. */
static void say_left_out(int fd, unsigned kind, const char *why)
{
    char line[96];
    int n = snprintf(line, sizeof(line), "posix: descriptor %d (kind %u) left out: %s\n", fd,
                     kind, why);
    if (n > 0)
        (void)jam_debug_write(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

/* One staged descriptor made a description and put in its slot. */
static void adopt_one(struct staged *s)
{
    const struct posixfds_fd *rec = &s->rec;
    fd_adopt_fn *adopt = rec->kind < POSIXFDS_KIND_COUNT ? fd_adopters[rec->kind] : NULL;
    if (!adopt) {
        for (unsigned k = 0; k < rec->nhandles; k++)
            jam_handle_close(s->hs[k]);
        say_left_out(rec->fd, rec->kind, "a kind not built yet");
        return;
    }
    struct fd_desc *d = NULL;
    long r = adopt(rec, rec->data_len ? stage_data + s->data_at : NULL, s->hs, &d);
    if (r < 0) {
        say_left_out(rec->fd, rec->kind, strerror((int)-r));
        return;
    }
    (void)fd_replace(rec->fd, d, false);   /* in range: the reader checked; nothing reserved */
}

/* The staged hand-over (or the defaults) made descriptors, once. */
static void start_locked(void)
{
    /* Marked first: an adopter that looks at the table must not start it
     * again (before main there is only this thread to see it). */
    __atomic_store_n(&started, true, __ATOMIC_RELEASE);
    if (stage_default)
        stdfd_default();
    for (unsigned i = 0; i < nstaged; i++)
        adopt_one(&staged[i]);
    nstaged = 0;
    stage_data_free();
}

static void start_once(void)
{
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE))
        return;
    lock_take(&start_lock);
    if (!started)
        start_locked();
    lock_give(&start_lock);
}

/* Before the program's own constructors (101 is the first priority not
 * reserved to the implementation), so they find their descriptors. */
__attribute__((constructor(101))) static void fd_start(void)
{
    start_once();
}

/* ---- helpers for kinds ----------------------------------------------------------- */

long fd_iov_total(const struct iovec *iov, int iovcnt)
{
    if (iovcnt < 0 || iovcnt > IOV_MAX)
        return -EINVAL;
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len > SSIZE_MAX - total)
            return -EINVAL;
        total += iov[i].iov_len;
    }
    return total > FD_RW_MAX ? FD_RW_MAX : (long)total;
}

void fd_iov_scatter(const struct iovec *iov, int iovcnt, size_t skip, const void *src, size_t n)
{
    const uint8_t *p = src;
    for (int i = 0; i < iovcnt && n; i++) {
        if (skip >= iov[i].iov_len) {
            skip -= iov[i].iov_len;
            continue;
        }
        size_t k = iov[i].iov_len - skip < n ? iov[i].iov_len - skip : n;
        memcpy((uint8_t *)iov[i].iov_base + skip, p, k);
        p += k;
        n -= k;
        skip = 0;
    }
}

void fd_iov_gather(const struct iovec *iov, int iovcnt, size_t skip, void *dst, size_t n)
{
    uint8_t *p = dst;
    for (int i = 0; i < iovcnt && n; i++) {
        if (skip >= iov[i].iov_len) {
            skip -= iov[i].iov_len;
            continue;
        }
        size_t k = iov[i].iov_len - skip < n ? iov[i].iov_len - skip : n;
        memcpy(p, (const uint8_t *)iov[i].iov_base + skip, k);
        p += k;
        n -= k;
        skip = 0;
    }
}

/* ---- the calls that route to a kind ------------------------------------------ */

/* read, readv, pread64, preadv and the writes: through the kind's read or
 * write with pos (FD_AT_OFFSET, or >= 0). */
static long rw(long fd, const struct iovec *iov, long iovcnt, int64_t pos, bool write)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return -EBADF;
    long r = fd_iov_total(iov, (int)(iovcnt > INT_MAX ? -1 : iovcnt));
    if (r >= 0 && !(write ? fd_can_write(d) : fd_can_read(d)))
        r = -EBADF;
    else if (r >= 0 && pos != FD_AT_OFFSET && pos < 0)
        r = d->ops->seek ? -EINVAL : -ESPIPE;
    else if (r >= 0 && write)
        r = d->ops->write ? d->ops->write(d, iov, (int)iovcnt, pos) : -EINVAL;
    else if (r >= 0)
        r = d->ops->read ? d->ops->read(d, iov, (int)iovcnt, pos) : -EINVAL;
    fd_put(d);
    return r;
}

/* One buffer as an iovec of one piece, its length cut to FD_RW_MAX (a
 * size_t count past SSIZE_MAX arrives here negative). */
static struct iovec one(long buf, long len)
{
    size_t n = (size_t)len > FD_RW_MAX ? FD_RW_MAX : (size_t)len;
    return (struct iovec){ (void *)(uintptr_t)buf, n };
}

long lx_read(long fd, long buf, long len, long d, long e, long f)
{
    struct iovec v = one(buf, len);
    return rw(fd, &v, 1, FD_AT_OFFSET, false);
}

long lx_write(long fd, long buf, long len, long d, long e, long f)
{
    struct iovec v = one(buf, len);
    return rw(fd, &v, 1, FD_AT_OFFSET, true);
}

long lx_pread64(long fd, long buf, long len, long pos, long e, long f)
{
    struct iovec v = one(buf, len);
    return rw(fd, &v, 1, pos, false);
}

long lx_pwrite64(long fd, long buf, long len, long pos, long e, long f)
{
    struct iovec v = one(buf, len);
    return rw(fd, &v, 1, pos, true);
}

long lx_readv(long fd, long iov, long n, long d, long e, long f)
{
    return rw(fd, (const struct iovec *)(uintptr_t)iov, n, FD_AT_OFFSET, false);
}

long lx_writev(long fd, long iov, long n, long d, long e, long f)
{
    return rw(fd, (const struct iovec *)(uintptr_t)iov, n, FD_AT_OFFSET, true);
}

/* On x86-64 the offset is one register (pos_l); Linux ignores pos_h. */
long lx_preadv(long fd, long iov, long n, long pos_l, long pos_h, long f)
{
    return rw(fd, (const struct iovec *)(uintptr_t)iov, n, pos_l, false);
}

long lx_pwritev(long fd, long iov, long n, long pos_l, long pos_h, long f)
{
    return rw(fd, (const struct iovec *)(uintptr_t)iov, n, pos_l, true);
}

long lx_lseek(long fd, long off, long whence, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    long r = desc->ops->seek ? desc->ops->seek(desc, off, (int)whence) : -ESPIPE;
    fd_put(desc);
    return r;
}

long lx_close(long fd, long b, long c, long d, long e, long f)
{
    return fd_close(fd);
}

long lx_fstat(long fd, long out, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    struct stat *st = (struct stat *)(uintptr_t)out;
    memset(st, 0, sizeof(*st));
    long r = 0;
    if (desc->ops->stat) {
        r = desc->ops->stat(desc, st);
    } else {
        st->st_mode = S_IFCHR | 0600;
        st->st_nlink = 1;
        st->st_blksize = 4096;
    }
    fd_put(desc);
    return r;
}

long lx_ioctl(long fd, long req, long arg, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    long r;
    switch ((unsigned long)req) {
    case FIONBIO:
        r = fd_status_set(desc, O_NONBLOCK, *(const int *)(uintptr_t)arg ? O_NONBLOCK : 0);
        break;
    case FIOCLEX:
    case FIONCLEX:
        r = fd_set_cloexec(fd, req == FIOCLEX);
        break;
    default:
        r = desc->ops->ioctl ? desc->ops->ioctl(desc, (unsigned long)req, arg) : -ENOTTY;
    }
    fd_put(desc);
    return r;
}

long lx_fsync(long fd, long b, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    long r = desc->ops->sync ? desc->ops->sync(desc) : -EINVAL;
    fd_put(desc);
    return r;
}

long lx_fdatasync(long fd, long b, long c, long d, long e, long f)
{
    return lx_fsync(fd, 0, 0, 0, 0, 0);
}

long lx_ftruncate(long fd, long len, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    long r;
    if (fd_status(desc) & O_PATH)
        r = -EBADF;
    else if (len < 0 || !fd_can_write(desc) || !desc->ops->truncate)
        r = -EINVAL;   /* Linux: not open for writing is EINVAL, as a negative length */
    else
        r = desc->ops->truncate(desc, len);
    fd_put(desc);
    return r;
}

long lx_getdents64(long fd, long buf, long len, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    long r = desc->ops->getdents ? desc->ops->getdents(desc, (void *)(uintptr_t)buf, (size_t)len)
                                 : -ENOTDIR;
    fd_put(desc);
    return r;
}
