/* Files (docs/M13-PLAN.md section 3): the file kind of descriptor, open
 * (open, openat, creat), truncate, and mmap of a descriptor.
 *
 * A file description holds its offset and status flags, the absolute
 * path it was opened by, and a reference to the file's one open in this
 * process (struct fobj, fobj.c: fat allows one writable open of a file,
 * so every description of a file here shares it). Reads and writes go
 * through that open's transfer buffer a buffer-full at a time, under the
 * description's lock (its offset) and then the open's (its buffer).
 * O_APPEND writes at the file's size, which the layer knows (it holds the
 * only writable open), so an append is exact however many descriptions
 * append; pwrite with O_APPEND appends too, as Linux's does (pwrite(2),
 * BUGS).
 *
 * A child given a file (posix_spawn's hand-over, fd_give) gets its own
 * open of the same path with the description's access, made by the
 * parent: two processes sharing one buffer would overwrite each other's
 * transfers. It starts at the parent's offset and moves its own (M13 Q5).
 * fat allows a file one writable open, so for a description that may
 * write the parent lends its own open to the child and takes it back at
 * its next use of the file (fobj.c); a mount with many writers (extfs)
 * just opens it again.
 *
 * On a mount with links the path a file was opened by is kept as it was
 * resolved (the mount's part as written: files.h), and open follows a
 * link out of the mount as every path call does; O_NOFOLLOW on a link is
 * ELOOP, O_CREAT | O_EXCL never follows one. A new file gets the mode
 * asked for less the umask. fstat there is file.getattr's. */
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
    struct fobj *o;          /* the file's open here; NULL for O_PATH */
    char        *path;       /* absolute; NULL if adopted without one */
    bool         nofollow;   /* O_PATH | O_NOFOLLOW: path names a link itself */
    bool         given;      /* given to a child on a mount with many writers, at the end of the
                              * file (given_end), and not used since: follow_given_locked */
    uint64_t     given_end;
};

static const struct fd_ops file_ops;

/* The FS_* access an open with status flags s needs. */
static uint32_t fs_access(uint32_t s)
{
    uint32_t acc = s & FD_ACCMODE, f = 0;
    if (acc != O_WRONLY)
        f |= FS_READ;
    if (acc != O_RDONLY)
        f |= FS_READ | FS_WRITE;   /* reading too: the one open serves every description */
    return f;
}

/* Does an open of a file with status flags s, refused with errno r, have a
 * second try with the write alone? The file's mode may allow only that (0200:
 * open(2) lets its owner open it O_WRONLY), which the one open's reading
 * too (fs_access) asked past. The refusal comes before any truncation. */
static bool write_alone_may(uint32_t s, long r)
{
    return r == -EACCES && (s & FD_ACCMODE) == O_WRONLY;
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

/* One buffer-full of a transfer at off: the bytes moved, or -errno. o->lock. */
static long piece(const struct file *f, const struct iovec *iov, int n, size_t done,
                  uint32_t want, uint64_t off, bool write)
{
    struct jfile *jf = &f->o->jf;
    uint32_t actual = 0;
    status_t st;
    if (write) {
        if (fd_iov_gather(iov, n, done, jf->buf, want))
            return -EFAULT;
        st = file_write_within(jf->ch, FILE_TIMEOUT, off, want, &actual);
    } else {
        st = file_read_within(jf->ch, FILE_TIMEOUT, off, want, &actual);
    }
    if (st == OK && actual > want)
        st = ERR_IO;   /* a broken service */
    if (st != OK)
        return file_err(f, st);
    if (!write && fd_iov_scatter(iov, n, done, jf->buf, actual))
        return -EFAULT;
    return actual;
}

/* The transfer at off, total bytes: the bytes moved, or -errno. o->lock. */
static long transfer(const struct file *f, const struct iovec *iov, int n, size_t total,
                     uint64_t off, bool write)
{
    struct fobj *o = f->o;
    if (!o->jf.ch)
        return -EIO;   /* the open was lost (fobj.c) */
    if (total > (uint64_t)INT64_MAX - off)
        return write ? -EFBIG : -EINVAL;
    size_t done = 0;
    while (done < total) {
        uint32_t want = total - done < o->jf.buf_size ? (uint32_t)(total - done) : o->jf.buf_size;
        long k = piece(f, iov, n, done, want, off + done, write);
        if (k < 0)
            return done ? (long)done : k;   /* what moved counts; the error comes next time */
        done += (size_t)k;
        if ((uint32_t)k < want)
            break;   /* the end of the file, or the service took less */
    }
    if (write && off + done > o->size)
        o->size = off + done;
    return (long)done;
}

/* A description at the end of a file it lent to a child (offset was_end)
 * moves to the end the child left (end): where a shared offset, Linux's,
 * would be after a child that wrote on from it, as output to `> f` does
 * (`xargs cmd > f`, `find -exec`). Offsets stay per process otherwise
 * (M13 Q5's A). d->lock and the object's held. */
static void follow_end_locked(struct fd_desc *d, uint64_t was_end, uint64_t end)
{
    if ((uint64_t)d->offset == was_end && end > was_end)
        d->offset = (int64_t)end;
}

/* The file's size now, asked of the service (a mount with many writers:
 * another process may have written since), into o->size. o->lock. */
static status_t append_end_locked(struct fobj *o)
{
    uint64_t size;
    status_t st = o->jf.ch ? file_stat_within(o->jf.ch, FILE_TIMEOUT, &size, NULL, NULL, NULL)
                           : ERR_IO;
    if (st == OK)
        o->size = size;
    return st;
}

/* On a mount with many writers nothing is lent: a child given a writable
 * description opens the file again beside ours, at our offset, and offsets
 * stay per process (M13 Q5's A). A description that was at the end of the
 * file when it was given away, and has not been used since, moves to the end
 * the children left, as a shared offset would (`xargs cmd > f`,
 * `(a; b) > f`): the rule follow_end_locked has for a lent file, which ext
 * needs as much as FAT does. d->lock and the object's held. */
static void follow_given_locked(struct fd_desc *d, struct file *f)
{
    if (!f->given)
        return;
    f->given = false;
    if (append_end_locked(f->o) == OK)
        follow_end_locked(d, f->given_end, f->o->size);
}

/* kind_give's part for a writable description on a many-writers mount (not
 * O_APPEND: the service appends): follow what the last child left, then
 * note whether the description is at the end now. d->lock held. */
static void give_locked(struct fd_desc *d, struct file *f)
{
    struct fobj *o = f->o;
    lock_take(&o->lock);
    if (append_end_locked(o) == OK) {
        if (f->given)
            follow_end_locked(d, f->given_end, o->size);
        f->given = (uint64_t)d->offset == o->size;
        f->given_end = o->size;
    } else {
        f->given = false;
    }
    lock_give(&o->lock);
}

/* A read or write at pos (FD_AT_OFFSET: the description's offset); a
 * write with O_APPEND at the end unless noappend. */
static long xfer(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos, bool write,
                 bool noappend)
{
    struct file *f = d->priv;
    long total = fd_iov_total(iov, n);
    if (total < 0 || !f->o)
        return total < 0 ? total : -EBADF;
    uint32_t s = fd_status(d);
    lock_take(&d->lock);
    lock_take(&f->o->lock);
    bool was_lent = f->o->lent;
    uint64_t was_end = f->o->lent_end;
    status_t back = fobj_back_locked(f->o, f->path);   /* lent to a child: ours again */
    if (back == OK && was_lent)
        follow_end_locked(d, was_end, f->o->size);
    if (back == OK)
        follow_given_locked(d, f);   /* a many-writers mount: after a child, the end it left */
    bool append = write && (s & O_APPEND) && !noappend;
    if (back == OK && append && f->o->multi && !f->o->append)
        back = append_end_locked(f->o);   /* O_APPEND set since the open (F_SETFL) */
    uint64_t off = append ? f->o->size : pos == FD_AT_OFFSET ? (uint64_t)d->offset : (uint64_t)pos;
    long r = back != OK ? file_err(f, back) : transfer(f, iov, n, (size_t)total, off, write);
    if (r > 0 && append && f->o->append) {   /* the service put it at the end: where was that? */
        status_t st = append_end_locked(f->o);
        if (st == OK && f->o->size >= (uint64_t)r)
            off = f->o->size - (uint64_t)r;
    }
    if (r > 0 && write && (s & (O_SYNC | O_DSYNC))) {
        status_t st = file_sync_within(f->o->jf.ch, FILE_TIMEOUT);
        if (st != OK)
            r = file_err(f, st);
    }
    lock_give(&f->o->lock);
    if (r >= 0 && pos == FD_AT_OFFSET)
        d->offset = (int64_t)(off + (uint64_t)r);
    lock_give(&d->lock);
    return r;
}

static long kind_read(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return xfer(d, iov, n, pos, false, false);
}

static long kind_write(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return xfer(d, iov, n, pos, true, false);
}

static long kind_write_at(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return xfer(d, iov, n, pos, true, true);
}

/* The file's size now: ours if we write it, else the service's. */
static long size_now(const struct file *f, uint64_t *size)
{
    if (!f->o) {
        struct path p;
        struct path_info i;
        long r = f->path ? path_set(f->path, &p) : -EBADF;
        if (!r)
            r = path_info_follow(&p, &i);
        *size = r ? 0 : i.size;
        return r;
    }
    lock_take(&f->o->lock);
    status_t st = fobj_back_locked(f->o, f->path);
    if (st == OK && (f->o->jf.flags & FS_WRITE) && !f->o->multi)
        *size = f->o->size;
    else if (st == OK)
        st = f->o->jf.ch ? file_stat_within(f->o->jf.ch, FILE_TIMEOUT, size, NULL, NULL, NULL)
                         : ERR_IO;
    lock_give(&f->o->lock);
    return st == OK ? 0 : file_err(f, st);
}

/* lseek's new offset from whence (Linux's SEEK_DATA and SEEK_HOLE too: a
 * FAT file is all data, its one hole at the end). d->lock. */
static long seek_to(struct fd_desc *d, int64_t off, int whence, int64_t *to)
{
    uint64_t size = 0;
    long r = whence == SEEK_SET || whence == SEEK_CUR ? 0 : size_now(d->priv, &size);
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
    if (f->given && f->o && whence != SEEK_SET) {   /* many writers: after a child, its end */
        lock_take(&f->o->lock);
        follow_given_locked(d, f);
        lock_give(&f->o->lock);
    }
    if (whence == SEEK_SET)
        f->given = false;   /* the description is placed by hand */
    long r = seek_to(d, off, whence, &to);
    if (!r)
        d->offset = to;
    lock_give(&d->lock);
    return r ? r : to;
}

static long kind_stat(struct fd_desc *d, struct stat *st)
{
    const struct file *f = d->priv;
    struct path p = { .abs = "/", .where = PATH_MOUNT };   /* no path: a plain file */
    struct path_info i = { 0 };
    long r = f->path ? path_set(f->path, &p) : 0;
    p.nofollow = f->nofollow;
    if (!r && !f->o)
        r = path_info_follow(&p, &i);
    if (!r && f->o) {
        lock_take(&f->o->lock);
        status_t s = fobj_back_locked(f->o, f->path);
        if (s == OK && !f->o->jf.ch)
            s = ERR_IO;
        if (s == OK && f->o->attrs) {   /* the inode's own */
            s = file_getattr_within(f->o->jf.ch, FILE_TIMEOUT, &i.mode, &i.nlink, &i.uid, &i.gid,
                                    &i.size, &i.blocks, &i.id, &i.atime_ns, &i.mtime_ns,
                                    &i.ctime_ns);
            i.has_attr = true;
        } else if (s == OK) {
            s = file_stat_within(f->o->jf.ch, FILE_TIMEOUT, &i.size, NULL, &i.mtime, &i.id);
        }
        lock_give(&f->o->lock);
        r = s == OK ? 0 : file_err(f, s);
    }
    if (r)
        return r;
    path_fill_stat(&p, &i, st);
    if (!f->path && !i.has_attr)
        st->st_mode = S_IFREG | 0644;
    return 0;
}

/* fsync, or ftruncate (len >= 0). */
static long sync_or_cut(struct fd_desc *d, int64_t len)
{
    const struct file *f = d->priv;
    if (!f->o)
        return -EBADF;
    lock_take(&f->o->lock);
    status_t st = fobj_back_locked(f->o, f->path);
    if (st == OK)
        st = !f->o->jf.ch ? ERR_IO
             : len < 0    ? file_sync_within(f->o->jf.ch, FILE_TIMEOUT)
                          : file_truncate_within(f->o->jf.ch, FILE_TIMEOUT, (uint64_t)len);
    if (st == OK && len >= 0)
        f->o->size = (uint64_t)len;
    lock_give(&f->o->lock);
    return st == OK ? 0 : file_err(f, st);
}

static long kind_sync(struct fd_desc *d)
{
    return sync_or_cut(d, -1);
}

static long kind_truncate(struct fd_desc *d, int64_t len)
{
    return sync_or_cut(d, len);
}

static long kind_ioctl(struct fd_desc *d, unsigned long req, long arg)
{
    if (req != FIONREAD)
        return -ENOTTY;
    uint64_t size;
    long r = size_now(d->priv, &size);
    lock_take(&d->lock);
    int64_t left = !r && (int64_t)size > d->offset ? (int64_t)size - d->offset : 0;
    lock_give(&d->lock);
    return r ? r : uc_put_int(arg, left > INT32_MAX ? INT32_MAX : (int)left);
}

static const char *kind_path(struct fd_desc *d)
{
    const struct file *f = d->priv;
    return f->path;
}

/* A child's own open of f's file (flags: its access; an O_APPEND one on a
 * mount with many writers appends there too), as two handles. */
static long reopen(const struct file *f, uint32_t s, handle_t *ch, handle_t *buf)
{
    struct path *p = jam_malloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    uint32_t ff = fs_access(s) | ((s & O_APPEND) && f->o->multi ? FS_APPEND : 0);
    long r = path_set(f->path, p);
    while (!r) {
        status_t st = fobj_open_settled(p, ff, ch, buf);
        r = st == OK ? 0 : path_err(p, st);   /* ERR_BUSY: another child holds it to write */
        if (write_alone_may(s, r)) {   /* a file of mode 0200 */
            st = fobj_open_settled(p, ff & ~(uint32_t)FS_READ, ch, buf);
            r = st == OK ? 0 : path_err(p, st);
        }
        if (!path_again(p, &r))
            break;
    }
    jam_free(p);
    return r;
}

/* A child's open to write f needs ours closed: lent, or, lent already
 * (another child had it), the description at the end the last child left
 * follows it (follow_end_locked), so this child writes after it. d->lock. */
static void lend_locked(struct fd_desc *d, const struct file *f)
{
    struct fobj *o = f->o;
    lock_take(&o->lock);
    struct path p;
    struct path_info i;
    if (!o->lent) {
        fobj_lend_locked(o);
    } else if (!path_set(f->path, &p) && !path_info(&p, &i)) {
        follow_end_locked(d, o->lent_end, i.size);
        o->lent_end = i.size;
    }
    lock_give(&o->lock);
}

static long kind_give(struct fd_desc *d, struct fd_given *out)
{
    struct file *f = d->priv;
    uint32_t s = fd_status(d);
    if (!f->o || !f->path)
        return -ENOTSUP;   /* O_PATH, or a file we were given without its path */
    char *data = jam_strdup(f->path);
    if (!data)
        return -ENOMEM;
    lock_take(&d->lock);
    if ((fs_access(s) & FS_WRITE) && !f->o->multi)
        lend_locked(d, f);   /* fat's one writer */
    else if ((fs_access(s) & FS_WRITE) && !(s & O_APPEND))
        give_locked(d, f);   /* many writers: no lending, but the offset follows the children */
    int64_t off = d->offset;
    lock_give(&d->lock);
    *out = (struct fd_given){
        .kind = POSIXFDS_FILE, .status = s & POSIXFDS_STATUS, .offset = (uint64_t)off,
        .nhandles = 2, .data = data, .data_len = (uint32_t)strlen(data) + 1,
    };
    long r = reopen(f, s, &out->handles[0], &out->handles[1]);
    if (r) {
        jam_free(data);
        memset(out, 0, sizeof(*out));
    }
    return r;
}

static void kind_close(struct fd_desc *d)
{
    struct file *f = d->priv;
    if (f->o)
        fobj_put(f->o);
    jam_free(f->path);
    jam_free(f);
}

/* fork (docs/FORK-PLAN.md section 1.6): nothing is passed, so a fork
 * costs no file service a call. The child's copy of the file's open is
 * lent (fobj_fork): its first use opens the path again, and the
 * description goes on from its offset, its own from then on (M13 Q5's A,
 * as a spawned child's). On a mount with one writer (fat) an open that
 * may write is EBUSY there while we hold ours. A file that came over a
 * socket has no path to open again: left out. */
static bool kind_fork(struct fd_desc *d, struct fd_fork *f)
{
    struct file *fl = d->priv;
    if (!fd_fork_child(f) || !fl->o)
        return true;   /* O_PATH: the path alone */
    if (!fl->path)
        return false;
    fobj_fork(f, fl->o);
    return true;
}

static const struct fd_ops file_ops = {
    .name = "file", .read = kind_read, .write = kind_write, .write_at = kind_write_at,
    .seek = kind_seek,
    .stat = kind_stat, .ioctl = kind_ioctl, .sync = kind_sync, .truncate = kind_truncate,
    .path = kind_path, .give = kind_give, .fork = kind_fork, .close = kind_close,
};

/* A description for the open o (its reference consumed) of path abs. */
static long file_new(struct fobj *o, const char *abs, uint32_t status, struct fd_desc **out)
{
    struct file *f = jam_calloc(1, sizeof(*f));
    char *path = abs ? jam_strdup(abs) : NULL;
    struct fd_desc *d = f && (path || !abs) ? fd_new(&file_ops, status, f) : NULL;
    if (!d) {
        if (o)
            fobj_put(o);
        jam_free(path);
        jam_free(f);
        return -ENOMEM;
    }
    f->o = o;
    f->path = path;
    *out = d;
    return 0;
}

bool file_desc_is(const struct fd_desc *d)
{
    return d->ops == &file_ops;
}

long file_setattr_fd(struct fd_desc *d, uint32_t mask, uint32_t mode, uint32_t uid, uint32_t gid,
                     int64_t atime_ns, int64_t mtime_ns)
{
    const struct file *f = d->priv;
    if (d->ops != &file_ops || !f->o || !f->o->attrs)
        return -ENOTSUP;
    lock_take(&f->o->lock);
    status_t st = f->o->jf.ch ? file_setattr_within(f->o->jf.ch, FILE_TIMEOUT, mask, mode, uid,
                                                    gid, atime_ns, mtime_ns)
                              : ERR_IO;
    lock_give(&f->o->lock);
    if (st == ERR_ACCESS_DENIED) {   /* not the owner, or a view that may not change it */
        struct path p;
        return f->path && !path_set(f->path, &p) && path_read_only(&p) ? -EROFS : -EPERM;
    }
    return st == OK ? 0 : file_err(f, st);
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
    struct fobj *o;
    uint32_t ff = fs_access(rec->status) | (rec->status & O_APPEND ? FS_APPEND : 0);
    long r = fobj_adopt(hs[0], hs[1], ff, path, &o);
    if (!r)
        r = file_new(o, path, rec->status, out);
    if (!r)
        (*out)->offset = (int64_t)rec->offset;
    return r;
}

/* ---- open ------------------------------------------------------------------------ */

/* The FS_* flags for open's flags (FS_APPEND and FS_NOFOLLOW are kept
 * only on the mounts where they mean something: fobj_open). */
static uint32_t open_fs_flags(long flags)
{
    uint32_t f = fs_access((uint32_t)flags);
    if (flags & O_CREAT)
        f |= FS_CREATE | FS_WRITE | ((flags & O_EXCL) ? FS_EXCL : 0);
    if (flags & O_TRUNC)
        f |= FS_TRUNCATE | FS_WRITE;   /* O_RDONLY|O_TRUNC truncates too, as Linux's */
    if ((flags & O_APPEND) && (f & FS_WRITE))
        f |= FS_APPEND;
    if (flags & O_NOFOLLOW)
        f |= FS_NOFOLLOW;
    return f;
}

/* A directory found where open was asked to open: a directory
 * description if the flags allow one (read-only, or O_PATH: musl's
 * O_ACCMODE holds O_PATH, so the mode is FD_ACCMODE's), its path physical. */
static long open_dir(struct path *p, long flags, struct fd_desc **out)
{
    if ((flags & FD_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
        return -EISDIR;
    long r = path_place(p);
    return r ? r : dir_new(p, (uint32_t)flags & (FILE_STATUS | O_DIRECTORY), out);
}

/* O_DIRECTORY, O_PATH or a trailing slash: what the path is decides. */
static long open_by_stat(struct path *p, long flags, struct fd_desc **out)
{
    struct path_info i;
    long r = path_info(p, &i);
    if (r == -ENOENT && (flags & O_CREAT) && p->must_dir)
        return -EISDIR;   /* open("new/", O_CREAT): Linux's answer */
    if (r)
        return r;
    if (i.is_dir)
        return open_dir(p, flags, out);
    if (i.is_link && !(flags & O_PATH))
        return -ELOOP;   /* O_NOFOLLOW, and the last name is a link */
    if (flags & O_DIRECTORY)
        return -ENOTDIR;
    if (i.is_svc || !(flags & O_PATH))
        return -ENXIO;
    r = file_new(NULL, p->abs, O_PATH, out);
    if (!r) {
        struct file *f = (*out)->priv;
        f->nofollow = p->nofollow;   /* O_PATH | O_NOFOLLOW of a link: the link */
    }
    return r;
}

/* A plain open of a file in a mount; mode: a new file's bits. */
static long open_file(struct path *p, long flags, uint32_t mode, struct fd_desc **out)
{
    struct fobj *o;
    long r = fobj_open(p, open_fs_flags(flags), mode, &o);
    if (write_alone_may((uint32_t)flags, r))
        r = fobj_open(p, open_fs_flags(flags) & ~(uint32_t)FS_READ, mode, &o);
    bool rdonly_create = (flags & (O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC)) == (O_RDONLY | O_CREAT);
    if ((r == -EROFS || r == -EACCES) && rdonly_create) {   /* there already, read-only? */
        r = fobj_open(p, FS_READ | (flags & O_NOFOLLOW ? FS_NOFOLLOW : 0), mode, &o);
        if (r == -ENOENT)
            r = path_read_only(p) ? -EROFS : -EACCES;   /* not there, and it can't be made */
    }
    if (r == -EINVAL && (flags & O_NOFOLLOW) && p->links)
        r = -ELOOP;   /* the service's answer to FS_NOFOLLOW on a link */
    if (r == -EISDIR)
        return open_dir(p, flags, out);
    if (r)
        return r;
    return file_new(o, p->abs, (uint32_t)flags & FILE_STATUS, out);
}

/* open's checks of flags: 0 or -errno. */
static long open_flags_ok(long flags)
{
    if (flags & ~(long)OPEN_KNOWN)
        return -EINVAL;
    if ((flags & O_TMPFILE) == O_TMPFILE)
        return -EOPNOTSUPP;   /* no unnamed files on FAT */
    if ((flags & FD_ACCMODE) == FD_ACCMODE && !(flags & O_PATH))
        return -EINVAL;   /* access mode 3 (O_PATH ignores the mode) */
    if ((flags & (O_CREAT | O_DIRECTORY)) == (O_CREAT | O_DIRECTORY))
        return -EINVAL;       /* Linux 6.4 on */
    return 0;
}

/* What open makes for p: a description, or -errno, or PATH_LINK_OUT. */
static long open_path(struct path *p, long flags, uint32_t mode, struct fd_desc **out)
{
    if (flags & O_PATH)
        flags &= O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    if (p->where != PATH_MOUNT || (flags & (O_DIRECTORY | O_PATH)) || p->must_dir) {
        long r = open_by_stat(p, flags, out);
        if (r == -ENOENT && p->where != PATH_MOUNT && (flags & O_CREAT))
            r = -EROFS;   /* nothing can be made in "/" or /svc */
        return r;
    }
    return open_file(p, flags, mode, out);
}

long lx_openat(long dirfd, long pathname, long flags, long mode, long e, long f)
{
    long r = open_flags_ok(flags);
    struct path p;
    if (!r)
        r = path_get(dirfd, (const char *)(uintptr_t)pathname, &p);
    if (r)
        return r;
    p.nofollow = (flags & O_NOFOLLOW) || (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
    uint32_t perm = (flags & O_CREAT) ? ((uint32_t)mode & 07777u) & ~lx_umask_now()
                                      : FS_MODE_FILE_DEFAULT;
    long fd = -1;
    struct fd_desc *d = NULL;
    for (;;) {   /* again from where a link out of a mount led */
        enum path_libc which = path_libc(&p);
        if (which != LIBC_NONE && fd >= 0)
            fd_free(fd);
        if (which == LIBC_DEV)
            return dev_open(p.abs, (int)flags, (mode_t)mode);
        if (which == LIBC_ETC)
            return etc_open(p.abs, (int)flags);
        if (which == LIBC_PROC)
            return proc_open(&p, flags);
        if (fd < 0 && (fd = fd_alloc(0)) < 0)   /* EMFILE before anything is made, as Linux */
            return fd;
        r = open_path(&p, flags, perm, &d);
        if (!path_again(&p, &r))
            break;
    }
    if (r) {
        fd_free(fd);
        return r;
    }
    fd_place(fd, d, flags & O_CLOEXEC);
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

/* truncate's open of p, to write. */
static long truncate_open(struct path *p, struct fd_desc **out)
{
    if (path_libc(p) != LIBC_NONE)
        return path_libc_refuse(path_libc(p));
    if (p->where != PATH_MOUNT || p->must_dir) {
        struct path_info i;
        long r = path_info(p, &i);
        return r ? r : i.is_dir ? -EISDIR : -EINVAL;
    }
    return open_file(p, O_WRONLY, FS_MODE_FILE_DEFAULT, out);
}

long lx_truncate(long pathname, long len, long c, long d, long e, long f)
{
    struct path p;
    long r = len < 0 ? -EINVAL : path_get(AT_FDCWD, (const char *)(uintptr_t)pathname, &p);
    if (r)
        return r;
    struct fd_desc *desc = NULL;
    do
        r = truncate_open(&p, &desc);
    while (path_again(&p, &r));
    if (r)
        return r;
    r = kind_truncate(desc, len);
    fd_put(desc);
    return r;
}


/* ---- mmap of a descriptor ------------------------------------------------------- */

/* Fill vmo (len bytes) from f's file at off. o->lock. */
static long fill_from_file(const struct file *f, uint64_t off, uint64_t len, handle_t vmo)
{
    const struct jfile *jf = &f->o->jf;
    if (!jf->ch)
        return -EIO;
    for (uint64_t at = 0; at < len;) {
        uint32_t want = len - at < jf->buf_size ? (uint32_t)(len - at) : jf->buf_size;
        uint32_t actual = 0;
        status_t st = file_read_within(jf->ch, FILE_TIMEOUT, off + at, want, &actual);
        if (st == OK && actual > want)
            st = ERR_IO;
        if (st == OK && actual)
            st = jam_vmo_write(vmo, at, jf->buf, actual);
        if (st != OK)
            return file_err(f, st);
        at += actual;
        if (actual < want)
            break;   /* the end of the file: the rest stays zero */
    }
    return 0;
}

/* Fill vmo (len bytes) from a memfd's VMO from (size bytes), at off. */
static long fill_from_vmo(handle_t from, uint64_t size, uint64_t off, uint64_t len, handle_t vmo)
{
    enum { CHUNK = 65536 };
    uint64_t n = off < size ? (size - off < len ? size - off : len) : 0;
    uint8_t *buf = n ? jam_malloc(CHUNK) : NULL;
    if (n && !buf)
        return -ENOMEM;
    status_t st = OK;
    for (uint64_t at = 0; st == OK && at < n; at += CHUNK) {
        uint64_t k = n - at < CHUNK ? n - at : CHUNK;
        st = jam_vmo_read(from, off + at, buf, k);
        if (st == OK)
            st = jam_vmo_write(vmo, at, buf, k);
    }
    jam_free(buf);
    return lx_err(st);
}

/* MAP_PRIVATE: a copy of d's file or memfd from off, len bytes, in *out. */
static long map_copy(struct fd_desc *d, int64_t off, uint64_t len, handle_t *out)
{
    const struct file *f = d->priv;
    handle_t from = HANDLE_INVALID, vmo = HANDLE_INVALID;
    uint64_t size = 0;
    bool file = d->ops == &file_ops;
    long r = file ? 0 : memfd_vmo(d, &from, &size);
    if (!r)
        r = lx_err(jam_vmo_create(len, 0, &vmo));
    if (!r && file) {
        lock_take(&f->o->lock);
        status_t back = fobj_back_locked(f->o, f->path);
        r = back != OK ? file_err(f, back) : fill_from_file(f, (uint64_t)off, len, vmo);
        lock_give(&f->o->lock);
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

/* The file's own pages: its one shared VMO from the service (file.map)
 * into *out. flags FILE_MAP_READ: read and map only; FILE_MAP_WRITE (ext
 * only, a file open for writing): the same VMO, which may be mapped
 * writable too. ERR_NOT_SUPPORTED: the mount's service has no pager (for
 * FILE_MAP_WRITE: none that writes pages back: fat); ERR_ACCESS_DENIED
 * (FILE_MAP_WRITE): the file or its volume can't be written through a
 * mapping; ERR_BUSY: fat maps no file open for writing (ours may be)
 * until it keeps mappings coherent with writes, and ext maps no file
 * writable that is mapped to run. */
static status_t file_pages(const struct file *f, uint32_t flags, handle_t *out)
{
    uint64_t size = 0;
    lock_take(&f->o->lock);
    status_t st = fobj_back_locked(f->o, f->path);
    if (st == OK && !f->o->jf.ch)
        st = ERR_IO;   /* the open was lost */
    if (st == OK)
        st = file_map_within(f->o->jf.ch, FILE_TIMEOUT, flags, out, &size);
    lock_give(&f->o->lock);
    return st;
}

/* A file's mapping (see files.h): on the file's pages where its service
 * has them, else MAP_PRIVATE's copy. A shared mapping is the file's VMO
 * itself; a private read-only one too (out->pager_ro: mem.c puts a child
 * in its place if mprotect adds write); a private writable one a
 * copy-on-write child of it, charged in full now.
 *
 * MAP_SHARED of a descriptor open for writing asks for the writable VMO
 * (FILE_MAP_WRITE) whatever PROT_WRITE says, as Linux decides at mmap
 * whether the mapping may ever be writable (VM_MAYWRITE: the descriptor's
 * mode then, not its mode at a later mprotect, which may be after the
 * close). Linux's price is ours: a file mapped so can't be run while the
 * mapping lives (its struct file holds the write access; here the shared
 * VMO, which extfs marks written: ETXTBSY to a run). Only an O_RDWR
 * MAP_SHARED pays it; a read-only descriptor, MAP_PRIVATE and a file with
 * no writable pager (fat) ask for FILE_MAP_READ as before. Without
 * PROT_WRITE a refusal of the writable VMO falls back to the read-only
 * one (mprotect adding write is then EACCES, as for a read-only
 * descriptor); with it the refusal is the answer: ENODEV (fat), ETXTBSY,
 * EACCES. */
static long map_file(struct fd_desc *d, int64_t off, uint64_t len, uint32_t how,
                     struct file_mapping *out)
{
    const struct file *f = d->priv;
    bool shared = how & FMAP_SHARED, write = how & FMAP_WRITE;
    if (shared && write && !fd_can_write(d))   /* Linux checks this before anything else */
        return -EACCES;
    handle_t pages = HANDLE_INVALID;
    status_t st = ERR_NOT_SUPPORTED;
    if (shared && fd_can_write(d)) {
        st = file_pages(f, FILE_MAP_WRITE, &pages);
        bool refused = st == ERR_NOT_SUPPORTED || st == ERR_ACCESS_DENIED || st == ERR_BUSY;
        if (st != OK && (write || !refused))
            return st == ERR_NOT_SUPPORTED ? -ENODEV : st == ERR_BUSY ? -ETXTBSY : file_err(f, st);
    }
    if (st != OK)   /* not asked for, or refused to a mapping that doesn't write */
        st = file_pages(f, FILE_MAP_READ, &pages);
    if (st == ERR_NOT_SUPPORTED || st == ERR_BUSY)
        return shared ? -ENODEV : map_copy(d, off, len, &out->vmo);
    if (st != OK)
        return file_err(f, st);
    if (shared || !write) {
        *out = (struct file_mapping){ pages, (uint64_t)off, !shared };
        return 0;
    }
    st = jam_vmo_create_child(pages, VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE, (uint64_t)off, len,
                              &out->vmo);
    jam_handle_close(pages);   /* the child holds the file's VMO */
    return st == OK ? 0 : st == ERR_NO_MEMORY ? -ENOMEM : lx_err(st);
}

long file_map(long fd, int64_t off, uint64_t len, uint32_t how, struct file_mapping *out)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return -EBADF;
    const struct file *f = d->priv;
    bool file = d->ops == &file_ops;
    struct file_mapping m = { HANDLE_INVALID, 0, false };
    long r;
    uint64_t size;
    if (off < 0)
        r = -EINVAL;
    else if (!fd_can_read(d) || (file && !f->o))
        r = -EACCES;   /* Linux checks the read access first */
    else if (file)
        r = map_file(d, off, len, how, &m);
    else if (how & FMAP_SHARED)
        r = memfd_vmo(d, &m.vmo, &size);
    else
        r = map_copy(d, off, len, &m.vmo);
    fd_put(d);
    if (!r && !file && (how & FMAP_SHARED))
        m.vmo_off = (uint64_t)off;   /* the memfd's own VMO, at off */
    if (!r)
        *out = m;
    return r;
}
