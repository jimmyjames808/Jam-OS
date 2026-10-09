/* Files (docs/M13-PLAN.md section 3): the file kind of descriptor, open
 * (open, openat, creat), truncate, and a file's MAP_PRIVATE copy.
 *
 * A file description holds the `file` channel and the transfer buffer
 * the service gave with it (libos's struct jfile, the buffer mapped for
 * as long as the file is open) and the absolute path it was opened by.
 * Reads and writes go through the buffer a buffer-full at a time under
 * the description's lock (the offset, and the buffer, are the
 * description's: two threads never share a transfer). A write of up to a
 * buffer (64 KiB) is one call to the service, so an O_APPEND write of
 * that size lands whole however many writers append (the service
 * appends: FS_APPEND), as a single write(2) does on Linux.
 *
 * O_APPEND leaves the offset at the end of the file. Its value is asked
 * of the service only when something reads it (a read, lseek, a
 * hand-over): `at_end` until then, so appending costs no stat.
 *
 * A child given a file (posix_spawn's hand-over) gets its own open of the
 * same path with the same flags, made by the parent (fd_give): two
 * processes sharing one buffer would overwrite each other's transfers.
 * It starts at the parent's offset and moves its own (M13 Q5). */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_idl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "files.h"

/* open's flags this layer knows: anything else is EINVAL (stricter than
 * open(2), which ignores unknown bits; openat2(2) refuses them too). */
#define OPEN_KNOWN (O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND | O_NONBLOCK | \
                    O_DSYNC | O_ASYNC | O_DIRECT | O_LARGEFILE | O_DIRECTORY | O_NOFOLLOW |    \
                    O_NOATIME | O_CLOEXEC | O_SYNC | O_PATH | O_TMPFILE)
/* The status flags a description keeps (F_GETFL's). */
#define FILE_STATUS (O_ACCMODE | O_APPEND | O_NONBLOCK | O_DSYNC | O_ASYNC | O_DIRECT | \
                     O_LARGEFILE | O_NOATIME | O_SYNC | O_PATH)

struct file {
    struct jfile jf;      /* the channel and buffer; jf.ch 0: O_PATH, nothing open */
    char        *path;    /* absolute; NULL if adopted without one */
    bool         at_end;  /* the offset is the end of the file, not asked yet; d->lock */
};

static const struct fd_ops file_ops;

/* The FS_* flags an open with status flags s needs (access and append). */
static uint32_t fs_flags(uint32_t s)
{
    uint32_t acc = s & O_ACCMODE, f = 0;
    if (acc != O_WRONLY)
        f |= FS_READ;
    if (acc != O_RDONLY)
        f |= FS_WRITE;
    if (s & O_APPEND)
        f |= FS_APPEND;
    return f;
}

/* errno for a service's answer about f (EROFS needs its mount). */
static long file_err(const struct file *f, status_t st)
{
    struct path p;
    if (st != ERR_ACCESS_DENIED)
        return path_err(NULL, st);
    return f->path && !path_set(f->path, &p) ? path_err(&p, st) : -EACCES;
}

/* ---- the kind ------------------------------------------------------------------ */

/* The offset, asked of the service if an append left it at the end. d->lock. */
static long settle_offset(struct fd_desc *d)
{
    struct file *f = d->priv;
    if (!f->at_end)
        return 0;
    uint64_t size;
    status_t st = file_stat_within(f->jf.ch, FILE_TIMEOUT, &size, NULL, NULL, NULL);
    if (st != OK)
        return file_err(f, st);
    d->offset = (int64_t)size;
    f->at_end = false;
    return 0;
}

/* One buffer-full of a transfer at off: the bytes moved, or -errno. */
static long piece(struct file *f, const struct iovec *iov, int n, size_t done, uint32_t want,
                  uint64_t off, bool write)
{
    uint32_t actual = 0;
    status_t st;
    if (write) {
        fd_iov_gather(iov, n, done, f->jf.buf, want);
        st = file_write_within(f->jf.ch, FILE_TIMEOUT, off, want, &actual);
    } else {
        st = file_read_within(f->jf.ch, FILE_TIMEOUT, off, want, &actual);
    }
    if (st == OK && actual > want)
        st = ERR_IO;   /* a broken service */
    if (st != OK)
        return file_err(f, st);
    if (!write)
        fd_iov_scatter(iov, n, done, f->jf.buf, actual);
    return actual;
}

static long xfer(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos, bool write)
{
    struct file *f = d->priv;
    long total = fd_iov_total(iov, n);
    if (total < 0 || !f->jf.ch)
        return total < 0 ? total : -EBADF;
    lock_take(&d->lock);
    long r = pos == FD_AT_OFFSET && !write ? settle_offset(d) : 0;
    uint64_t off = pos == FD_AT_OFFSET ? (uint64_t)d->offset : (uint64_t)pos;
    size_t done = 0;
    if (!r && (uint64_t)total > (uint64_t)INT64_MAX - off)
        r = write ? -EFBIG : -EINVAL;
    while (!r && done < (size_t)total) {
        uint32_t want = (size_t)total - done < f->jf.buf_size ? (uint32_t)((size_t)total - done)
                                                              : f->jf.buf_size;
        long k = piece(f, iov, n, done, want, off + done, write);
        if (k < 0) {
            r = done ? 0 : k;   /* what was moved counts; the error comes next time */
            break;
        }
        done += (size_t)k;
        if ((uint32_t)k < want)
            break;   /* the end of the file, or the service took less */
    }
    bool append = write && (fd_status(d) & O_APPEND);
    if (pos == FD_AT_OFFSET && append && done)
        f->at_end = true;
    else if (pos == FD_AT_OFFSET && !append)
        d->offset = (int64_t)(off + done);
    if (!r && done && write && (fd_status(d) & (O_SYNC | O_DSYNC)))
        r = file_err(f, file_sync_within(f->jf.ch, FILE_TIMEOUT));
    lock_give(&d->lock);
    return r ? r : (long)done;
}

static long kind_read(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return xfer(d, iov, n, pos, false);
}

static long kind_write(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return xfer(d, iov, n, pos, true);
}

/* The file's size now. */
static long size_now(struct fd_desc *d, uint64_t *size)
{
    struct file *f = d->priv;
    if (!f->jf.ch) {
        struct path p;
        struct path_info i;
        long r = f->path ? path_set(f->path, &p) : -EBADF;
        if (!r)
            r = path_info(&p, &i);
        *size = r ? 0 : i.size;
        return r;
    }
    status_t st = file_stat_within(f->jf.ch, FILE_TIMEOUT, size, NULL, NULL, NULL);
    return st == OK ? 0 : file_err(f, st);
}

/* lseek's new offset from whence (Linux's SEEK_DATA and SEEK_HOLE too: a
 * FAT file is all data, its one hole at the end). d->lock. */
static long seek_to(struct fd_desc *d, int64_t off, int whence, int64_t *to)
{
    uint64_t size = 0;
    long r = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? settle_offset(d) : size_now(d, &size);
    if (r)
        return r;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? d->offset : (int64_t)size;
    switch (whence) {
    case SEEK_SET:
    case SEEK_CUR:
    case SEEK_END:
        if (__builtin_add_overflow(base, off, to) || *to < 0)
            return -EINVAL;
        return 0;
    case SEEK_DATA:
    case SEEK_HOLE:
        if (off < 0 || (uint64_t)off >= size)
            return -ENXIO;
        *to = whence == SEEK_DATA ? off : (int64_t)size;
        return 0;
    }
    return -EINVAL;
}

static long kind_seek(struct fd_desc *d, int64_t off, int whence)
{
    struct file *f = d->priv;
    int64_t to = 0;
    lock_take(&d->lock);
    long r = seek_to(d, off, whence, &to);
    if (!r) {
        d->offset = to;
        f->at_end = false;
    }
    lock_give(&d->lock);
    return r ? r : to;
}

static long kind_stat(struct fd_desc *d, struct stat *st)
{
    struct file *f = d->priv;
    struct path p;
    struct path_info i = { 0 };
    long r = f->path ? path_set(f->path, &p) : 0;
    if (r)
        return r;
    if (!f->jf.ch)
        r = path_info(&p, &i);
    status_t s = f->jf.ch ? file_stat_within(f->jf.ch, FILE_TIMEOUT, &i.size, NULL, &i.mtime,
                                             &i.id)
                          : OK;
    if (s != OK)
        return file_err(f, s);
    if (r)
        return r;
    if (f->path) {
        path_fill_stat(&p, &i, st);
        return 0;
    }
    p = (struct path){ .abs = "/", .where = PATH_MOUNT };   /* no path: a plain file */
    path_fill_stat(&p, &i, st);
    st->st_mode = S_IFREG | 0644;
    return 0;
}

static long kind_sync(struct fd_desc *d)
{
    struct file *f = d->priv;
    if (!f->jf.ch)
        return -EBADF;
    status_t st = file_sync_within(f->jf.ch, FILE_TIMEOUT);
    return st == OK ? 0 : file_err(f, st);
}

static long kind_truncate(struct fd_desc *d, int64_t len)
{
    struct file *f = d->priv;
    if (!f->jf.ch)
        return -EBADF;
    status_t st = file_truncate_within(f->jf.ch, FILE_TIMEOUT, (uint64_t)len);
    return st == OK ? 0 : file_err(f, st);
}

static long kind_ioctl(struct fd_desc *d, unsigned long req, long arg)
{
    if (req != FIONREAD)
        return -ENOTTY;
    uint64_t size;
    lock_take(&d->lock);
    long r = settle_offset(d);
    if (!r)
        r = size_now(d, &size);
    int64_t left = !r && (int64_t)size > d->offset ? (int64_t)size - d->offset : 0;
    lock_give(&d->lock);
    if (!r)
        *(int *)(uintptr_t)arg = left > INT32_MAX ? INT32_MAX : (int)left;
    return r;
}

/* A fresh open of f's file with status flags s (no creating, no
 * truncating): for a child, or for O_APPEND changed by fcntl. */
static long reopen(const struct file *f, uint32_t s, struct jfile *out)
{
    struct path p;
    long r = f->path ? path_set(f->path, &p) : -ENOTSUP;
    handle_t fs = HANDLE_INVALID, ch, buf;
    uint64_t size;
    if (!r)
        r = path_fs(&p, &fs);
    if (r)
        return r;
    uint32_t len;
    const char *rel = path_rel(&p, &len);
    status_t st = fs_open_within(fs, FILE_TIMEOUT, rel, len, fs_flags(s), &ch, &buf, &size);
    jam_handle_close(fs);
    if (st == OK)
        st = file_adopt(ch, buf, fs_flags(s), out);
    return st == OK ? 0 : path_err(&p, st);
}

/* O_APPEND turned on or off: the service appends only for an open made
 * with FS_APPEND, so the file is opened again with or without it.
 * d->lock (fd_status_set's). */
static long kind_setfl(struct fd_desc *d, uint32_t old, uint32_t new)
{
    struct file *f = d->priv;
    if (!((old ^ new) & O_APPEND) || !f->jf.ch)
        return 0;
    long r = settle_offset(d);
    struct jfile jf;
    if (!r)
        r = reopen(f, new, &jf);
    if (r)
        return r;
    file_close(&f->jf);
    f->jf = jf;
    return 0;
}

static const char *kind_path(struct fd_desc *d)
{
    const struct file *f = d->priv;
    return f->path;
}

static long kind_give(struct fd_desc *d, struct fd_given *out)
{
    struct file *f = d->priv;
    uint32_t s = fd_status(d);
    if (!f->jf.ch || !f->path)
        return -ENOTSUP;   /* O_PATH, or a file we were given without its path */
    struct jfile jf;
    lock_take(&d->lock);
    long r = settle_offset(d);
    int64_t off = d->offset;
    lock_give(&d->lock);
    char *data = r ? NULL : strdup(f->path);
    if (!r && !data)
        r = -ENOMEM;
    if (!r)
        r = reopen(f, s, &jf);
    if (r) {
        free(data);
        return r;
    }
    *out = (struct fd_given){
        .kind = POSIXFDS_FILE, .status = s & POSIXFDS_STATUS, .offset = (uint64_t)off,
        .nhandles = 2, .data = data, .data_len = (uint32_t)strlen(data) + 1,
    };
    file_give(&jf, &out->handles[0], &out->handles[1]);
    return 0;
}

static void kind_close(struct fd_desc *d)
{
    struct file *f = d->priv;
    if (f->jf.ch)
        file_close(&f->jf);
    free(f->path);
    free(f);
}

static const struct fd_ops file_ops = {
    .name = "file", .read = kind_read, .write = kind_write, .seek = kind_seek,
    .stat = kind_stat, .ioctl = kind_ioctl, .sync = kind_sync, .truncate = kind_truncate,
    .setfl = kind_setfl, .path = kind_path, .give = kind_give, .close = kind_close,
};

/* A description for an open jf (consumed: closed on failure) of path abs. */
static long file_new(struct jfile *jf, const char *abs, uint32_t status, struct fd_desc **out)
{
    struct file *f = calloc(1, sizeof(*f));
    char *path = abs ? strdup(abs) : NULL;
    struct fd_desc *d = f && (path || !abs) ? fd_new(&file_ops, status, f) : NULL;
    if (!d) {
        if (jf->ch)
            file_close(jf);
        free(path);
        free(f);
        return -ENOMEM;
    }
    f->jf = *jf;
    f->path = path;
    *out = d;
    return 0;
}

long file_adopt_fd(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                   struct fd_desc **out)
{
    /* The data is our own layer's: the path, NUL-terminated and absolute;
     * anything else from a starter that doesn't know it is no path. */
    const char *path = data && rec->data_len >= 2 && ((const char *)data)[0] == '/' &&
                               memchr(data, '\0', rec->data_len) ? data : NULL;
    if ((rec->status & O_PATH) || rec->offset > INT64_MAX) {
        jam_handle_close(hs[0]);
        jam_handle_close(hs[1]);
        return -EINVAL;
    }
    struct jfile jf;
    status_t st = file_adopt(hs[0], hs[1], fs_flags(rec->status), &jf);
    if (st != OK)
        return lx_err(st);
    long r = file_new(&jf, path, rec->status, out);
    if (!r)
        (*out)->offset = (int64_t)rec->offset;
    return r;
}

/* ---- open ------------------------------------------------------------------------ */

/* The FS_* flags for open's flags (status already in FILE_STATUS). */
static uint32_t open_fs_flags(long flags)
{
    uint32_t f = fs_flags((uint32_t)flags & FILE_STATUS);
    if (flags & O_CREAT)
        f |= FS_CREATE | FS_WRITE | ((flags & O_EXCL) ? FS_EXCL : 0);
    if (flags & O_TRUNC)
        f |= FS_TRUNCATE | FS_WRITE;   /* O_RDONLY|O_TRUNC truncates too, as Linux's */
    return f;
}

/* A directory found where open was asked to open: a directory
 * description if the flags allow one. */
static long open_dir(const struct path *p, long flags, struct fd_desc **out)
{
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
        return -EISDIR;
    return dir_new(p, (uint32_t)flags & (FILE_STATUS | O_DIRECTORY), out);
}

/* O_DIRECTORY, O_PATH or a trailing slash: what the path is decides. */
static long open_by_stat(const struct path *p, long flags, struct fd_desc **out)
{
    struct path_info i;
    long r = path_info(p, &i);
    if (r == -ENOENT && (flags & O_CREAT) && p->must_dir)
        return -EISDIR;   /* open("new/", O_CREAT): Linux's answer */
    if (r)
        return r;
    if (i.is_dir)
        return open_dir(p, flags, out);
    if (flags & O_DIRECTORY)
        return -ENOTDIR;
    if (i.is_svc || !(flags & O_PATH))
        return -ENXIO;
    struct jfile none = { 0 };
    return file_new(&none, p->abs, O_PATH, out);
}

/* A plain open of a file in a mount. */
static long open_file(const struct path *p, long flags, struct fd_desc **out)
{
    handle_t fs, ch, buf;
    uint64_t size;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    uint32_t len, ff = open_fs_flags(flags);
    const char *rel = path_rel(p, &len);
    status_t st = fs_open_within(fs, FILE_TIMEOUT, rel, len, ff, &ch, &buf, &size);
    bool rdonly_create = (flags & (O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC)) == (O_RDONLY | O_CREAT);
    if (st == ERR_ACCESS_DENIED && rdonly_create)   /* there already, on a read-only mount */
        st = fs_open_within(fs, FILE_TIMEOUT, rel, len, ff = FS_READ, &ch, &buf, &size);
    jam_handle_close(fs);
    if (st == ERR_IS_DIR)
        return open_dir(p, flags, out);
    if (st == ERR_NOT_FOUND && rdonly_create && ff == FS_READ)
        st = ERR_ACCESS_DENIED;   /* not there, and it can't be made */
    if (st != OK)
        return path_err(p, st);
    struct jfile jf;
    st = file_adopt(ch, buf, ff & (FS_READ | FS_WRITE | FS_APPEND), &jf);
    if (st != OK)
        return path_err(p, st);
    return file_new(&jf, p->abs, (uint32_t)flags & FILE_STATUS, out);
}

/* open's checks of flags: 0 or -errno. */
static long open_flags_ok(long flags)
{
    if (flags & ~(long)OPEN_KNOWN)
        return -EINVAL;
    if ((flags & O_TMPFILE) == O_TMPFILE)
        return -EOPNOTSUPP;   /* no unnamed files on FAT */
    if ((flags & O_ACCMODE) == O_ACCMODE)
        return -EINVAL;
    if ((flags & (O_CREAT | O_DIRECTORY)) == (O_CREAT | O_DIRECTORY))
        return -EINVAL;       /* Linux 6.4 on */
    return 0;
}

/* What open makes for p: a description, or -errno. */
static long open_path(const struct path *p, long flags, struct fd_desc **out)
{
    if (flags & O_PATH)
        flags &= O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    if (p->where != PATH_MOUNT || (flags & (O_DIRECTORY | O_PATH)) || p->must_dir) {
        long r = open_by_stat(p, flags, out);
        if (r == -ENOENT && p->where != PATH_MOUNT && (flags & O_CREAT))
            r = -EROFS;   /* nothing can be made in "/" or /svc */
        return r;
    }
    return open_file(p, flags, out);
}

long lx_openat(long dirfd, long pathname, long flags, long mode, long e, long f)
{
    long r = open_flags_ok(flags);
    struct path p;
    if (!r)
        r = path_get(dirfd, (const char *)(uintptr_t)pathname, &p);
    if (r)
        return r;
    enum path_libc which = path_libc(&p);
    if (which == LIBC_DEV)
        return dev_open(p.abs, (int)flags, (mode_t)mode);
    if (which == LIBC_ETC)
        return etc_open(p.abs, (int)flags);
    long fd = fd_alloc(0);   /* EMFILE before anything is made, as Linux */
    if (fd < 0)
        return fd;
    struct fd_desc *d;
    r = open_path(&p, flags, &d);
    if (r) {
        fd_free(fd);
        return r;
    }
    fd_set(fd, d, flags & O_CLOEXEC);
    return fd;
}

long lx_open(long pathname, long flags, long mode, long d, long e, long f)
{
    return lx_openat(AT_FDCWD, pathname, flags, mode, 0, 0);
}

long lx_creat(long pathname, long mode, long c, long d, long e, long f)
{
    return lx_openat(AT_FDCWD, pathname, O_CREAT | O_WRONLY | O_TRUNC, mode, 0, 0);
}

long lx_truncate(long pathname, long len, long c, long d, long e, long f)
{
    struct path p;
    long r = len < 0 ? -EINVAL : path_get(AT_FDCWD, (const char *)(uintptr_t)pathname, &p);
    if (r)
        return r;
    if (path_libc(&p) != LIBC_NONE)
        return path_libc_refuse(path_libc(&p));
    if (p.where != PATH_MOUNT || p.must_dir) {
        struct path_info i;
        r = path_info(&p, &i);
        return r ? r : i.is_dir ? -EISDIR : -EINVAL;
    }
    struct fd_desc *desc;
    r = open_file(&p, O_WRONLY, &desc);
    if (r)
        return r;
    r = kind_truncate(desc, len);
    fd_put(desc);
    return r;
}

/* ---- mmap of a descriptor ------------------------------------------------------- */

/* Fill vmo (len bytes) from f's file at off, under d->lock. */
static long fill_from_file(struct fd_desc *d, uint64_t off, uint64_t len, handle_t vmo)
{
    struct file *f = d->priv;
    for (uint64_t at = 0; at < len;) {
        uint32_t want = len - at < f->jf.buf_size ? (uint32_t)(len - at) : f->jf.buf_size;
        uint32_t actual = 0;
        status_t st = file_read_within(f->jf.ch, FILE_TIMEOUT, off + at, want, &actual);
        if (st == OK && actual > want)
            st = ERR_IO;
        if (st == OK && actual)
            st = jam_vmo_write(vmo, at, f->jf.buf, actual);
        if (st != OK)
            return file_err(f, st);
        at += actual;
        if (actual < want)
            break;   /* the end of the file: the rest stays zero */
    }
    return 0;
}

/* Fill vmo (len bytes) from a memfd's VMO from of size bytes, at off. */
static long fill_from_vmo(handle_t from, uint64_t size, uint64_t off, uint64_t len, handle_t vmo)
{
    enum { CHUNK = 65536 };
    uint8_t *buf = off < size ? malloc(CHUNK) : NULL;
    if (off < size && !buf)
        return -ENOMEM;
    uint64_t n = off < size ? (size - off < len ? size - off : len) : 0;
    status_t st = OK;
    for (uint64_t at = 0; st == OK && at < n; at += CHUNK) {
        uint64_t k = n - at < CHUNK ? n - at : CHUNK;
        st = jam_vmo_read(from, off + at, buf, k);
        if (st == OK)
            st = jam_vmo_write(vmo, at, buf, k);
    }
    free(buf);
    return lx_err(st);
}

/* MAP_PRIVATE: a copy of d's file or memfd from off, len bytes, in *out. */
static long map_copy(struct fd_desc *d, int64_t off, uint64_t len, handle_t *out)
{
    handle_t from = HANDLE_INVALID, vmo = HANDLE_INVALID;
    uint64_t size = 0;
    long r = d->ops == &file_ops ? 0 : memfd_vmo(d, &from, &size);
    if (!r)
        r = lx_err(jam_vmo_create(len, 0, &vmo));
    if (!r && d->ops == &file_ops) {
        lock_take(&d->lock);
        r = fill_from_file(d, (uint64_t)off, len, vmo);
        lock_give(&d->lock);
    } else if (!r) {
        r = fill_from_vmo(from, size, (uint64_t)off, len, vmo);
    }
    if (from)
        jam_handle_close(from);
    if (r && vmo)
        jam_handle_close(vmo);
    if (!r)
        *out = vmo;
    return r;
}

long file_map(long fd, int64_t off, uint64_t len, bool shared, handle_t *vmo, uint64_t *vmo_off)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return -EBADF;
    const struct file *f = d->priv;
    long r = 0;
    uint64_t size;
    if (off < 0)
        r = -EINVAL;
    else if (!fd_can_read(d) || (d->ops == &file_ops && !f->jf.ch))
        r = -EACCES;   /* Linux checks the read access first */
    else if (shared)
        r = memfd_vmo(d, vmo, &size);   /* a file: ENODEV until M12.5's pager */
    else
        r = map_copy(d, off, len, vmo);
    fd_put(d);
    if (!r)
        *vmo_off = shared ? (uint64_t)off : 0;
    return r;
}
