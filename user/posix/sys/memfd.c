/* memfd_create and shm_open's objects (docs/M13-PLAN.md section 3): a
 * file in memory, a VMO_KEEP_PAGES VMO grown with the file.
 *
 * A kept VMO's pages are committed and charged to our job when it grows,
 * and no holder can take them away, so a process we hand the descriptor
 * (or a mapping's VMO) to can read it without a fault it can't handle
 * (the compositor maps a Wayland client's pool this way). The price: a
 * kept VMO never shrinks. A smaller ftruncate keeps the VMO's size and
 * makes the file smaller in libc (struct memobj's size); the bytes past
 * the end are zeroed when the file grows over them again, so they read
 * as zero as Linux's do.
 *
 * A memory object (struct memobj) is shared by every description on it
 * (a description's priv is the object, with a reference; dev.c's
 * /dev/shm name holds one too; an open of /proc/self/fd/N, procfs.c's,
 * is one more description on it, read-only if asked so). Its size and seals are under its lock,
 * taken after the description's (fd.h's lock, for the offset). The size
 * is this process's: a child handed the descriptor starts with the size
 * and the seals as they were, then each process keeps its own (they
 * share the pages).
 *
 * Seals (F_ADD_SEALS, with MFD_ALLOW_SEALING): F_SEAL_SHRINK, F_SEAL_GROW,
 * F_SEAL_WRITE and F_SEAL_FUTURE_WRITE refuse what they name (EPERM) in
 * this process, F_SEAL_SEAL refuses more seals. They travel with the
 * object (a child's hand-over, SCM_RIGHTS: the wire's seals). And once
 * F_SEAL_WRITE or F_SEAL_FUTURE_WRITE is set, every handle given out for
 * the object (vmo_for: a MAP_SHARED mapping, a child, SCM_RIGHTS) lacks
 * RIGHT_WRITE, so the seal holds in a receiver by the kernel's rights,
 * not by its libc's word: a writable shared mapping made after the seal
 * fails (EACCES), and so does a write through any VMO handle given after
 * it. Mappings made before stay writable (Linux's F_SEAL_FUTURE_WRITE;
 * F_SEAL_WRITE's EBUSY while one exists is not built: libc keeps no
 * mapping's protection). A process that must trust a size another gave
 * it checks the VMO itself (VMAR_KEPT_ONLY), never a seal. */
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "memfd.h"

#define PAGE        4096ull
#define NAME_LEN    250                 /* memfd_create's names (Linux: 249 + "memfd:") */
#define SEALS_KNOWN (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | \
                     F_SEAL_FUTURE_WRITE)
#define MAX_SIZE    (VMO_MAX_SIZE < INT64_MAX ? VMO_MAX_SIZE : INT64_MAX)   /* the largest file */
#define ZERO_CHUNK  4096u
/* Linux 6.3's memfd_create flags, newer than musl's <sys/mman.h>. */
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008u
#define MFD_EXEC        0x0010u
#endif
/* A read-only description's VMO, for a child or a mapping: no writing,
 * no resizing. */
#define RO_RIGHTS   (RIGHT_READ | RIGHT_MAP | RIGHT_WAIT | RIGHT_TRANSFER | RIGHT_DUPLICATE | \
                     RIGHT_INSPECT)

struct memobj {
    lock_t   lock;            /* size, vmo_size, seals */
    uint32_t refs;            /* descriptions and names on it; atomic */
    handle_t vmo;             /* the kept VMO */
    uint64_t size;            /* the file's size (<= vmo_size) */
    uint64_t vmo_size;        /* the VMO's (whole pages); never shrinks */
    unsigned seals;           /* F_SEAL_* */
    char     name[NAME_LEN];  /* memfd_create's name, or the shm name */
    uint32_t fork_gen;        /* the last fork whose walk met it (memfd_fork) */
};

/* What a child is handed (the hand-over's data for POSIXFDS_MEMFD). */
struct memfd_wire {
    uint64_t size;            /* the file's size */
    uint64_t off;             /* the description's offset */
    uint32_t seals;           /* F_SEAL_* */
    uint32_t reserved;        /* 0 */
    char     name[NAME_LEN];  /* NUL-terminated */
};
_Static_assert(sizeof(struct memfd_wire) <= POSIXFDS_DATA_MAX, "fits a hand-over");

static const uint8_t zeros[ZERO_CHUNK];

static struct memobj *obj_of(struct fd_desc *d)
{
    return d->priv;
}

struct memobj *memobj_new(const char *name, bool sealing, long *err)
{
    struct memobj *m = jam_calloc(1, sizeof(*m));
    status_t st = m ? jam_vmo_create(0, VMO_KEEP_PAGES, &m->vmo) : ERR_NO_MEMORY;
    if (st != OK) {
        jam_free(m);
        *err = lx_err(st);
        return NULL;
    }
    m->refs = 1;
    m->seals = sealing ? 0 : F_SEAL_SEAL;   /* no seals ever without MFD_ALLOW_SEALING */
    size_t n = strnlen(name, NAME_LEN - 1);
    memcpy(m->name, name, n);
    m->name[n] = '\0';
    return m;
}

void memobj_ref(struct memobj *m)
{
    __atomic_add_fetch(&m->refs, 1, __ATOMIC_RELAXED);
}

void memobj_unref(struct memobj *m)
{
    if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL))
        return;
    jam_handle_close(m->vmo);
    jam_free(m);
}

uint64_t memobj_size(struct memobj *m)
{
    lock_take(&m->lock);
    uint64_t size = m->size;
    lock_give(&m->lock);
    return size;
}

void *memfd_priv_new(struct memobj *m)
{
    memobj_ref(m);
    return m;
}

/* Zero [from, to) of the VMO: 0 or -errno. */
static long zero_range(handle_t vmo, uint64_t from, uint64_t to)
{
    for (uint64_t at = from; at < to;) {
        uint64_t n = to - at < ZERO_CHUNK ? to - at : ZERO_CHUNK;
        status_t st = jam_vmo_write(vmo, at, zeros, n);
        if (st != OK)
            return lx_err(st);
        at += n;
    }
    return 0;
}

/* The file grows to size: the VMO first if it must (all its new pages
 * committed now, or ENOMEM and nothing changed), then the old pages'
 * bytes from the old end on are zeroed (a smaller ftruncate, or a
 * mapping's write past the end, may have left some). Lock held. */
static long grow_locked(struct memobj *m, uint64_t size)
{
    if (m->seals & F_SEAL_GROW)
        return -EPERM;
    if (size > MAX_SIZE)
        return -EFBIG;
    uint64_t pages = (size + PAGE - 1) & ~(PAGE - 1);
    if (pages > m->vmo_size) {
        status_t st = jam_vmo_set_size(m->vmo, pages);
        if (st != OK)
            return st == ERR_OUT_OF_RANGE ? -EFBIG : lx_err(st);
    }
    long r = zero_range(m->vmo, m->size, size < m->vmo_size ? size : m->vmo_size);
    if (pages > m->vmo_size)
        m->vmo_size = pages;
    if (!r)
        m->size = size;
    return r;
}

long memobj_truncate(struct memobj *m, uint64_t size)
{
    lock_take(&m->lock);
    long r = 0;
    if (size > m->size)
        r = grow_locked(m, size);
    else if (size < m->size && (m->seals & F_SEAL_SHRINK))
        r = -EPERM;
    else
        m->size = size;   /* the VMO keeps its pages: see the file's header */
    lock_give(&m->lock);
    return r;
}

/* One piece of a read or write at off, under the object's lock: bytes
 * (a read stops at the end of the file), or -errno. */
static long piece_locked(struct memobj *m, void *buf, size_t n, uint64_t off, bool write)
{
    if (write && (m->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)))
        return -EPERM;
    if (write && (off > MAX_SIZE || n > MAX_SIZE - off))
        return -EFBIG;
    long r = write && off + n > m->size ? grow_locked(m, off + n) : 0;
    if (!write)
        n = off >= m->size ? 0 : m->size - off < n ? (size_t)(m->size - off) : n;
    status_t st = OK;
    if (!r && n)   /* the kernel reads or writes the caller's buffer (a write's: checked by rw) */
        st = write ? jam_vmo_write(m->vmo, off, buf, n) : jam_vmo_read(m->vmo, off, buf, n);
    return r ? r : st != OK ? uc_or_fault(lx_err(st), buf, n, !write) : (long)n;
}

/* A read or write of iov at pos, or at (and moving) the description's
 * offset; an append write at the end. The description's lock, then the
 * object's, so an append and its grow are one step. */
static long memfd_rw(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos,
                     bool write)
{
    long n = fd_iov_total(iov, iovcnt);
    if (n <= 0)
        return n;
    struct memobj *m = obj_of(d);
    bool at_offset = pos == FD_AT_OFFSET;
    lock_take(&d->lock);
    lock_take(&m->lock);
    uint64_t off = at_offset ? (uint64_t)d->offset : (uint64_t)pos;
    if (write && (fd_status(d) & O_APPEND))
        off = m->size;
    long done = 0, r = 0;
    for (int i = 0; i < iovcnt && done < n && r >= 0; i++) {
        size_t len = iov[i].iov_len < (size_t)(n - done) ? iov[i].iov_len : (size_t)(n - done);
        r = len ? piece_locked(m, iov[i].iov_base, len, off + (uint64_t)done, write) : 0;
        if (r > 0)
            done += r;
        if (r >= 0 && (size_t)r < len)
            break;   /* the end of the file */
    }
    lock_give(&m->lock);
    if (at_offset && done)
        d->offset = (int64_t)(off + (uint64_t)done);
    lock_give(&d->lock);
    return done ? done : r;
}

static long memfd_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    return memfd_rw(d, iov, iovcnt, pos, false);
}

static long memfd_write(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    return memfd_rw(d, iov, iovcnt, pos, true);
}

static long memfd_seek(struct fd_desc *d, int64_t off, int whence)
{
    lock_take(&d->lock);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? d->offset :
                   whence == SEEK_END ? (int64_t)memobj_size(obj_of(d)) : -1;
    int64_t to;
    long r = base < 0 || __builtin_add_overflow(base, off, &to) || to < 0 ? -EINVAL : 0;
    if (!r)
        d->offset = to;
    lock_give(&d->lock);
    return r ? r : (long)to;
}

static long memfd_stat(struct fd_desc *d, struct stat *st)
{
    struct handle_info hi;
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_uid = st->st_gid = 1000;
    st->st_size = (off_t)memobj_size(obj_of(d));
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_ino = jam_handle_info(obj_of(d)->vmo, &hi) == OK ? (ino_t)hi.koid : 0;
    return 0;
}

static long memfd_truncate(struct fd_desc *d, int64_t len)
{
    return memobj_truncate(obj_of(d), (uint64_t)len);
}

/* A duplicate of the VMO with the rights d's access mode allows: none to
 * write once the object is write-sealed (the file's header). */
static long vmo_for(struct fd_desc *d, handle_t *out)
{
    struct memobj *m = obj_of(d);
    lock_take(&m->lock);
    bool sealed = m->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE);
    lock_give(&m->lock);
    bool ro = !fd_can_write(d) || sealed;
    status_t st = jam_handle_duplicate(m->vmo, ro ? RO_RIGHTS : RIGHT_SAME, out);
    return st == OK ? 0 : lx_err(st);
}

long memfd_vmo(struct fd_desc *d, handle_t *vmo, uint64_t *size)
{
    if (d->ops != &memfd_ops)
        return -ENODEV;
    long r = vmo_for(d, vmo);
    if (!r)
        *size = memobj_size(obj_of(d));
    return r;
}

static void memfd_close(struct fd_desc *d)
{
    memobj_unref(obj_of(d));
}

static long memfd_give(struct fd_desc *d, struct fd_given *out)
{
    struct memobj *m = obj_of(d);
    struct memfd_wire w = { 0 };
    lock_take(&d->lock);
    w.off = (uint64_t)d->offset;
    lock_give(&d->lock);
    lock_take(&m->lock);
    w.size = m->size;
    w.seals = m->seals;
    memcpy(w.name, m->name, sizeof(w.name));
    lock_give(&m->lock);
    long r = 0;
    void *data = kind_data(&w, sizeof(w), &r);
    if (!r && (r = vmo_for(d, &out->handles[0])))
        jam_free(data);
    if (r)
        return r;
    out->kind = POSIXFDS_MEMFD;
    out->status = fd_status(d);
    out->offset = 0;   /* the format's offset is a file's: ours is in the data */
    out->nhandles = 1;
    out->data = data;
    out->data_len = sizeof(w);
    return 0;
}

/* fork (docs/FORK-PLAN.md section 1.6): the child's object gets a
 * duplicate of the VMO, all our handle's rights (its size and seals are
 * its copy of ours, then its own, as a spawned child's), at the first
 * description the walk meets on it; its references are counted again,
 * one for each description kept and each name (shm_forked). The pages
 * are shared, as Linux's memfd is one file. */
static bool memfd_fork(struct fd_desc *d, struct fd_fork *f)
{
    struct memobj *m = obj_of(d);
    bool first = fd_fork_first(f, &m->fork_gen);
    if (!fd_fork_child(f)) {
        if (first)
            fd_fork_dup(f, 0, m->vmo);
        return true;
    }
    if (first) {
        m->vmo = fd_fork_take(f, 0);
        m->lock = (lock_t){ 0 };
        m->refs = 0;
    }
    if (!m->vmo)
        return false;
    m->refs++;
    return true;
}

bool memobj_forked_name(struct lx_fork_io *io, struct memobj *m)
{
    if (m->fork_gen != lx_fork_gen(io) || !m->vmo)
        return false;   /* no descriptor of the child's is on it: no handle came */
    m->refs++;
    return true;
}

const struct fd_ops memfd_ops = {
    .name = "memfd",
    .read = memfd_read,
    .write = memfd_write,
    .seek = memfd_seek,
    .stat = memfd_stat,
    .truncate = memfd_truncate,
    /* .ready, .arm: none (always ready, as the access mode allows) */
    .give = memfd_give,
    .fork = memfd_fork,
    .close = memfd_close,
};

/* The object a child is handed: the parent's size, seals and name, on
 * its VMO (consumed). NULL if the data is malformed. */
static struct memobj *obj_from_wire(handle_t vmo, const struct memfd_wire *w)
{
    uint64_t vmo_size;
    struct memobj *m = NULL;
    if (jam_vmo_get_size(vmo, &vmo_size) == OK && !w->reserved && !(w->seals & ~SEALS_KNOWN) &&
        w->size <= vmo_size && strnlen(w->name, NAME_LEN) < NAME_LEN)
        m = jam_calloc(1, sizeof(*m));
    if (!m) {
        jam_handle_close(vmo);
        return NULL;
    }
    *m = (struct memobj){ .refs = 1, .vmo = vmo, .size = w->size, .vmo_size = vmo_size,
                          .seals = w->seals };
    memcpy(m->name, w->name, NAME_LEN);
    return m;
}

long memfd_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                 struct fd_desc **out)
{
    struct memfd_wire w;
    if (rec->data_len != sizeof(w)) {
        jam_handle_close(hs[0]);
        return -EINVAL;
    }
    memcpy(&w, data, sizeof(w));
    struct memobj *m = obj_from_wire(hs[0], &w);
    if (!m)
        return -EINVAL;
    *out = w.off <= INT64_MAX ? fd_new(&memfd_ops, rec->status, m) : NULL;
    if (!*out) {
        memobj_unref(m);
        return -ENOMEM;
    }
    (*out)->offset = (int64_t)w.off;
    return 0;
}

long memfd_from_vmo(handle_t vmo, uint32_t status, struct fd_desc **out)
{
    struct memfd_wire w = { .seals = F_SEAL_SEAL };   /* no sealing: its maker's are unknown */
    if (jam_vmo_get_size(vmo, &w.size) != OK) {
        jam_handle_close(vmo);
        return -EINVAL;
    }
    struct memobj *m = obj_from_wire(vmo, &w);   /* consumes vmo */
    *out = m ? fd_new(&memfd_ops, status, m) : NULL;
    if (m && !*out)
        memobj_unref(m);
    return *out ? 0 : -ENOMEM;
}

long memfd_add_seals(struct fd_desc *d, unsigned seals)
{
    if (d->ops != &memfd_ops || (seals & ~SEALS_KNOWN))
        return -EINVAL;
    if (!fd_can_write(d))
        return -EPERM;   /* Linux: sealing needs a writable descriptor */
    struct memobj *m = obj_of(d);
    lock_take(&m->lock);
    long r = m->seals & F_SEAL_SEAL ? -EPERM : 0;
    if (!r)
        m->seals |= seals;
    lock_give(&m->lock);
    return r;
}

long memfd_get_seals(struct fd_desc *d)
{
    if (d->ops != &memfd_ops)
        return -EINVAL;
    struct memobj *m = obj_of(d);
    lock_take(&m->lock);
    long seals = m->seals;
    lock_give(&m->lock);
    return seals;
}

const char *memfd_name(struct fd_desc *d)
{
    return obj_of(d)->name;   /* set when the object is made, never changed */
}

long memfd_reopen(struct fd_desc *d, int flags)
{
    struct memobj *m = obj_of(d);
    bool write = (flags & O_ACCMODE) != O_RDONLY;
    struct handle_info hi;
    if (write && (jam_handle_info(m->vmo, &hi) != OK || !(hi.rights & RIGHT_WRITE)))
        return -EACCES;   /* no right to add: the twin we were given stays read-only */
    if (write && (flags & O_TRUNC)) {
        long r = memobj_truncate(m, 0);
        if (r)
            return r;
    }
    return kind_install(&memfd_ops, memfd_priv_new(m), (uint32_t)flags & OPEN_STATUS,
                        flags & O_CLOEXEC);
}

long lx_memfd_create(long name, long flags, long c, long d, long e, long f)
{
    if (flags & ~(long)(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL | MFD_EXEC))
        return -EINVAL;   /* MFD_HUGETLB and its sizes: not here */
    if ((flags & MFD_NOEXEC_SEAL) && (flags & MFD_EXEC))
        return -EINVAL;
    char n[NAME_LEN];   /* the caller's, copied */
    long len = uc_strnlen((const char *)(uintptr_t)name, NAME_LEN);
    if (len < 0)
        return len;
    if (len >= NAME_LEN)
        return -EINVAL;
    if (uc_get(n, (const char *)(uintptr_t)name, (size_t)len + 1))
        return -EFAULT;
    long err = 0;
    struct memobj *m = memobj_new(n, flags & (MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL), &err);
    if (!m)
        return err;
    return kind_install(&memfd_ops, m, O_RDWR, flags & MFD_CLOEXEC);   /* m's reference */
}
