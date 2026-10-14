/* Directories (docs/M13-PLAN.md section 3): the directory kind of
 * descriptor and its listing (getdents64), and the calls that change a
 * directory or the current one: mkdir, unlink, rmdir, rename, chdir,
 * fchdir and getcwd.
 *
 * A directory description is a path (files.h: no handle; the `fs`
 * protocol's own directory handles are an addition for later) and a
 * position in its listing. The listing starts with "." and "..", which
 * the services leave out, then the service's entries in its batches
 * (fs.readdir: a cookie asks for a batch, and each batch says the cookie
 * of the next). A position is where the next getdents64 starts, and is
 * what d_off, telldir and seekdir carry:
 *     0                       "."
 *     1                       ".."
 *     2 + (cookie << 16 | k)  entry k of the batch asked for with cookie
 * (a batch holds at most FS_READDIR_BYTES / (FS_DIRENT_FIXED + 1) < 2^16
 * entries). The last batch asked for is kept with the description, so a
 * listing read in order asks the service once per batch; one started
 * again (from ".", or after any lseek: rewinddir, seekdir) asks afresh,
 * so it sees the directory as it is now. "/" lists the
 * mounts and "svc", "/svc" the services (libjam's fs_readdir, by index:
 * 2 + index).
 *
 * The answers follow Linux's for the same steps where FAT allows them
 * (the errno table is posixtest's files section); rename replaces what
 * is at the new name (fs.rename's FS_RENAME_REPLACE). */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_idl.h>
#include <string.h>
#include "files.h"

#define POS_DOTDOT  1
#define POS_ENTRIES 2
#define K_BITS      16
#define K_MASK      ((1u << K_BITS) - 1)
#define DIRENT_HEAD offsetof(struct dirent, d_name)   /* 19: Linux's linux_dirent64 */
#define ID_TOP      1000u   /* "/"'s and /svc's entries: ids of the layer's own, from here */

struct dir {
    char    *path;       /* absolute and clean */
    uint8_t *batch;      /* the last batch (FS_READDIR_BYTES), made at the first listing */
    uint32_t blen;       /* its bytes */
    uint32_t bcookie;    /* the cookie it was asked with */
    uint32_t bnext;      /* the next batch's cookie; 0: it was the last */
    uint16_t bcount;     /* its entries */
    bool     bvalid;     /* batch holds bcookie's answer */
    uint16_t at_k;       /* the entry at byte at_off of batch (a cursor for reading in order) */
    uint32_t at_off;
};

/* One entry of a listing, as getdents64 writes it. */
struct entry {
    char     name[FS_NAME_MAX];
    uint64_t ino;
    uint8_t  type;     /* DT_* */
    int64_t  next;     /* the position after it */
};

static const struct fd_ops dir_ops;

bool dir_is(const struct fd_desc *d)
{
    return d->ops == &dir_ops;
}

/* ---- the listing ------------------------------------------------------------- */

/* The id of p ("." or ".."), or 1 if it can't be asked. */
static uint64_t id_of(const char *abs, const char *more)
{
    char buf[PATH_BYTES + 3];
    struct path p;
    struct path_info i;
    snprintf(buf, sizeof(buf), "%s%s", abs, more);
    return !path_set(buf, &p) && !path_info(&p, &i) && i.id ? i.id : 1;
}

/* Ask the service for cookie's batch. d->lock. */
static long fetch(const struct dir *x, struct dir *w, uint32_t cookie)
{
    struct path p;
    handle_t fs;
    long r = path_set(x->path, &p);
    if (!r)
        r = path_fs(&p, &fs);
    if (r)
        return r;
    if (!w->batch && !(w->batch = malloc(FS_READDIR_BYTES))) {
        jam_handle_close(fs);
        return -ENOMEM;
    }
    uint32_t len, next = 0, blen = 0;
    uint16_t count = 0;
    const char *rel = path_rel(&p, &len);
    w->bvalid = false;
    status_t st = fs_readdir_within(fs, FILE_TIMEOUT, rel, len, cookie, &next, &count, w->batch,
                                    &blen);
    jam_handle_close(fs);
    if (st != OK)
        return path_err_read(&p, st);
    if (!count && next)
        return -EIO;   /* an empty batch that isn't the last: a broken service */
    *w = (struct dir){ .path = w->path, .batch = w->batch, .blen = blen, .bcookie = cookie,
                       .bnext = next, .bcount = count, .bvalid = true };
    return 0;
}

/* Entry k of the kept batch into *e (name, ino, type). */
static long batch_entry(struct dir *x, uint16_t k, struct entry *e)
{
    if (k < x->at_k)
        x->at_k = 0, x->at_off = 0;
    struct fs_dirent de;
    for (;;) {
        if (!fs_dirent_get(x->batch, x->blen, &x->at_off, &de))
            return -EIO;
        if (x->at_k++ == k)
            break;
    }
    memcpy(e->name, de.name, de.name_len);
    e->name[de.name_len] = '\0';
    e->ino = de.id ? de.id : 1;
    e->type = de.is_dir ? DT_DIR : DT_REG;
    return 0;
}

/* The entry at pos of a mount's directory: 1 and *e, 0 at the end, or -errno. */
static long mount_entry(struct fd_desc *d, int64_t pos, struct entry *e)
{
    struct dir *x = d->priv;
    uint64_t v = (uint64_t)(pos - POS_ENTRIES);
    uint32_t cookie = (uint32_t)(v >> K_BITS);
    uint16_t k = (uint16_t)(v & K_MASK);
    for (;;) {
        if (!x->bvalid || x->bcookie != cookie) {
            long r = fetch(x, x, cookie);
            if (r)
                return r;
        }
        if (k < x->bcount)
            break;
        if (!x->bnext)
            return 0;
        cookie = x->bnext;
        k = 0;
    }
    long r = batch_entry(x, k, e);
    if (r)
        return r;
    bool last = k + 1 == x->bcount && x->bnext;
    uint64_t at = last ? (uint64_t)x->bnext << K_BITS : ((uint64_t)cookie << K_BITS) | (k + 1u);
    e->next = POS_ENTRIES + (int64_t)at;
    return 1;
}

/* The entry at pos of "/" or /svc (libjam's own listing of them). */
static long top_entry(const struct dir *x, int64_t pos, struct entry *e)
{
    struct fs_entry fe;
    uint32_t index = (uint32_t)(pos - POS_ENTRIES);
    status_t st = fs_readdir(x->path, index, &fe);
    if (st == ERR_NOT_FOUND)
        return 0;
    if (st != OK)
        return lx_err(st);
    memcpy(e->name, fe.name, strlen(fe.name) + 1);
    e->ino = ID_TOP + index;
    e->type = fe.is_dir ? DT_DIR : DT_SOCK;
    e->next = pos + 1;
    return 1;
}

static long entry_at(struct fd_desc *d, int64_t pos, struct entry *e)
{
    const struct dir *x = d->priv;
    if (pos < POS_ENTRIES) {
        memcpy(e->name, pos ? ".." : ".", pos ? 3 : 2);
        e->ino = id_of(x->path, pos ? "/.." : "");
        e->type = DT_DIR;
        e->next = pos + 1;
        return 1;
    }
    struct path p;
    long r = path_set(x->path, &p);
    if (r)
        return r;
    return p.where == PATH_MOUNT ? mount_entry(d, pos, e) : top_entry(x, pos, e);
}

size_t dir_put(void *buf, size_t room, const char *name, uint64_t ino, uint8_t type,
               int64_t next)
{
    size_t nlen = strlen(name);
    size_t reclen = (DIRENT_HEAD + nlen + 1 + 7) & ~(size_t)7;
    if (reclen > room)
        return 0;
    struct dirent *de = buf;
    de->d_ino = ino;
    de->d_off = next;
    de->d_reclen = (unsigned short)reclen;
    de->d_type = type;
    memcpy(de->d_name, name, nlen);
    memset(de->d_name + nlen, 0, reclen - DIRENT_HEAD - nlen);
    return reclen;
}

static long dir_getdents(struct fd_desc *d, void *buf, size_t len)
{
    struct entry *e = malloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    lock_take(&d->lock);
    if (d->offset < POS_ENTRIES) {   /* a listing from the start: asked afresh */
        struct dir *x = d->priv;
        x->bvalid = false;
    }
    size_t used = 0;
    long r;
    while ((r = entry_at(d, d->offset, e)) > 0) {
        size_t k = dir_put((uint8_t *)buf + used, len - used, e->name, e->ino, e->type, e->next);
        if (!k) {
            r = used ? 0 : -EINVAL;   /* the buffer can't hold the next one */
            break;
        }
        used += k;
        d->offset = e->next;
    }
    lock_give(&d->lock);
    free(e);
    return r < 0 && !used ? r : (long)used;
}

/* ---- the kind -------------------------------------------------------------------- */

static long dir_read(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return -EISDIR;
}

static long dir_seek(struct fd_desc *d, int64_t off, int whence)
{
    lock_take(&d->lock);
    int64_t to = -1;
    if (whence == SEEK_SET)
        to = off;
    else if (whence == SEEK_CUR && __builtin_add_overflow(d->offset, off, &to))
        to = -1;
    if (to >= 0) {
        struct dir *x = d->priv;
        d->offset = to;
        x->bvalid = false;   /* asked afresh (rewinddir after a change sees it) */
    }
    lock_give(&d->lock);
    return to >= 0 ? to : -EINVAL;
}

static long dir_stat(struct fd_desc *d, struct stat *st)
{
    const struct dir *x = d->priv;
    struct path p;
    struct path_info i;
    long r = path_set(x->path, &p);
    if (!r)
        r = path_info(&p, &i);
    if (!r)
        path_fill_stat(&p, &i, st);
    return r;
}

static long dir_sync(struct fd_desc *d)
{
    const struct dir *x = d->priv;
    struct path p;
    handle_t fs;
    long r = path_set(x->path, &p);
    if (r || p.where != PATH_MOUNT)
        return r;
    if ((r = path_fs(&p, &fs)) != 0)
        return r;
    status_t st = fs_sync_within(fs, FILE_TIMEOUT);
    jam_handle_close(fs);
    return st == OK ? 0 : path_err(&p, st);
}

static const char *dir_path(struct fd_desc *d)
{
    const struct dir *x = d->priv;
    return x->path;
}

static long dir_give(struct fd_desc *d, struct fd_given *out)
{
    const struct dir *x = d->priv;
    char *data = strdup(x->path);
    if (!data)
        return -ENOMEM;
    lock_take(&d->lock);
    int64_t off = d->offset;
    lock_give(&d->lock);
    *out = (struct fd_given){
        .kind = POSIXFDS_DIR, .status = fd_status(d) & POSIXFDS_STATUS, .offset = (uint64_t)off,
        .data = data, .data_len = (uint32_t)strlen(data) + 1,
    };
    return 0;
}

static void dir_close(struct fd_desc *d)
{
    struct dir *x = d->priv;
    free(x->batch);
    free(x->path);
    free(x);
}

static const struct fd_ops dir_ops = {
    .name = "directory", .read = dir_read, .seek = dir_seek, .stat = dir_stat,
    .sync = dir_sync, .getdents = dir_getdents, .path = dir_path, .give = dir_give,
    .close = dir_close,
};

long dir_new(const struct path *p, uint32_t status, struct fd_desc **out)
{
    struct dir *x = calloc(1, sizeof(*x));
    char *path = strdup(p->abs);
    struct fd_desc *d = x && path ? fd_new(&dir_ops, (status & ~O_ACCMODE) | O_RDONLY, x) : NULL;
    if (!d) {
        free(path);
        free(x);
        return -ENOMEM;
    }
    x->path = path;
    *out = d;
    return 0;
}

long dir_adopt_fd(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                  struct fd_desc **out)
{
    struct path p;
    if (rec->offset > INT64_MAX || path_set(data, &p))   /* data: a string (the reader's check) */
        return -EINVAL;
    long r = dir_new(&p, rec->status, out);
    if (!r)
        (*out)->offset = (int64_t)rec->offset;
    return r;
}

/* ---- calls that change directories ------------------------------------------- */

/* p is "/", /svc, a service, or a mount's root: nothing there can be made
 * or removed. */
static bool top_level(const struct path *p)
{
    return p->where != PATH_MOUNT || !p->abs[p->mount_len];
}

/* Does something exist at p? */
static bool exists(const struct path *p)
{
    struct path_info i;
    struct stat st;
    struct path q = *p;
    q.must_dir = false;
    enum path_libc which = path_libc(&q);
    return which != LIBC_NONE ? !path_libc_stat(&q, which, &st) : !path_info(&q, &i);
}

long lx_mkdirat(long dirfd, long pathname, long mode, long d, long e, long f)
{
    struct path p;
    long r = path_get(dirfd, (const char *)(uintptr_t)pathname, &p);
    if (r)
        return r;
    enum path_libc which = path_libc(&p);
    if (which != LIBC_NONE || top_level(&p)) {
        if (exists(&p))
            return -EEXIST;
        return which != LIBC_NONE ? path_libc_refuse(which) : -EROFS;
    }
    handle_t fs;
    if ((r = path_fs(&p, &fs)) != 0)
        return r;
    uint32_t len;
    const char *rel = path_rel(&p, &len);
    status_t st = fs_mkdir_within(fs, FILE_TIMEOUT, rel, len, (uint32_t)mode & 07777u);
    jam_handle_close(fs);
    if (st == ERR_ACCESS_DENIED && exists(&p))
        return -EEXIST;   /* Linux looks for it before it asks for write access */
    return st == OK ? 0 : path_err(&p, st);
}

long lx_mkdir(long pathname, long mode, long c, long d, long e, long f)
{
    return lx_mkdirat(AT_FDCWD, pathname, mode, 0, 0, 0);
}

/* unlink (flags 0) or rmdir (FS_UNLINK_DIR) of p. */
static long remove_path(const struct path *p, uint32_t flags)
{
    bool dir = flags & FS_UNLINK_DIR;
    if (dir && p->last_dot)
        return -EINVAL;   /* rmdir("x/."): rmdir(2) */
    if (dir && p->last_dotdot)
        return -ENOTEMPTY;
    enum path_libc which = path_libc(p);
    if (which == LIBC_DEV && !dir)
        return dev_unlink(p->abs);
    if (which != LIBC_NONE)
        return path_libc_refuse(which);
    struct path_info i;
    if (top_level(p)) {
        long r = path_info(p, &i);
        if (r)
            return r;
        return !i.is_dir ? (dir ? -ENOTDIR : -EROFS) : dir ? -EBUSY : -EISDIR;
    }
    if (p->must_dir && !dir) {   /* unlink("x/"): a file is ENOTDIR, a directory EISDIR */
        long r = path_info(p, &i);
        return r ? r : -EISDIR;
    }
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    status_t st = fs_unlink_within(fs, FILE_TIMEOUT, rel, len, flags);
    jam_handle_close(fs);
    return st == OK ? 0 : path_err(p, st);
}

long lx_unlinkat(long dirfd, long pathname, long flags, long d, long e, long f)
{
    if (flags & ~(long)AT_REMOVEDIR)
        return -EINVAL;
    struct path p;
    long r = path_get(dirfd, (const char *)(uintptr_t)pathname, &p);
    return r ? r : remove_path(&p, flags & AT_REMOVEDIR ? FS_UNLINK_DIR : 0);
}

long lx_unlink(long pathname, long b, long c, long d, long e, long f)
{
    return lx_unlinkat(AT_FDCWD, pathname, 0, 0, 0, 0);
}

long lx_rmdir(long pathname, long b, long c, long d, long e, long f)
{
    return lx_unlinkat(AT_FDCWD, pathname, AT_REMOVEDIR, 0, 0, 0);
}

/* rename's checks that the service can't make: -errno, or 1 to go on. */
static long rename_check(const struct path *a, const struct path *b)
{
    bool same_mount = a->mount_len == b->mount_len && !memcmp(a->abs, b->abs, a->mount_len);
    if (!same_mount || a->where != PATH_MOUNT)
        return same_mount ? -EBUSY : -EXDEV;
    if (a->last_dot || a->last_dotdot || b->last_dot || b->last_dotdot || top_level(a) ||
        top_level(b))
        return -EBUSY;
    if (!strcmp(a->abs, b->abs) || a->must_dir || b->must_dir) {
        struct path_info i;
        struct path q = *a;
        q.must_dir = false;
        long r = path_info(&q, &i);
        if (r)
            return r;
        if (!i.is_dir && (a->must_dir || b->must_dir))
            return -ENOTDIR;
        if (!strcmp(a->abs, b->abs))
            return 0;   /* the same name: nothing to do (rename(2)) */
    }
    return 1;
}

static long rename_paths(const struct path *a, const struct path *b, uint32_t flags)
{
    enum path_libc wa = path_libc(a), wb = path_libc(b);
    if (wa != LIBC_NONE || wb != LIBC_NONE)
        return path_libc_refuse(wa != LIBC_NONE ? wa : wb);
    long r = rename_check(a, b);
    if (r <= 0)
        return r;
    handle_t fs;
    if ((r = path_fs(a, &fs)) != 0)
        return r;
    uint32_t alen, blen;
    const char *arel = path_rel(a, &alen), *brel = path_rel(b, &blen);
    status_t st = fs_rename_within(fs, FILE_TIMEOUT, arel, alen, brel, blen, flags);
    jam_handle_close(fs);
    return st == OK ? 0 : path_err(a, st);
}

long lx_renameat2(long olddirfd, long oldpath, long newdirfd, long newpath, long flags, long f)
{
    if (flags & ~(long)RENAME_NOREPLACE)
        return -EINVAL;   /* RENAME_EXCHANGE and RENAME_WHITEOUT: FAT has neither */
    struct path *two = malloc(2 * sizeof(*two));
    if (!two)
        return -ENOMEM;
    long r = path_get(olddirfd, (const char *)(uintptr_t)oldpath, &two[0]);
    if (!r)
        r = path_get(newdirfd, (const char *)(uintptr_t)newpath, &two[1]);
    if (!r)
        r = rename_paths(&two[0], &two[1], flags & RENAME_NOREPLACE ? 0 : FS_RENAME_REPLACE);
    free(two);
    return r;
}

long lx_renameat(long olddirfd, long oldpath, long newdirfd, long newpath, long e, long f)
{
    return lx_renameat2(olddirfd, oldpath, newdirfd, newpath, 0, 0);
}

long lx_rename(long oldpath, long newpath, long c, long d, long e, long f)
{
    return lx_renameat2(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0, 0);
}

/* ---- the current directory ------------------------------------------------------ */

long lx_chdir(long pathname, long b, long c, long d, long e, long f)
{
    struct path p;
    struct path_info i;
    struct stat st;
    long r = path_get(AT_FDCWD, (const char *)(uintptr_t)pathname, &p);
    enum path_libc which = r ? LIBC_NONE : path_libc(&p);
    if (!r && which != LIBC_NONE) {
        r = path_libc_stat(&p, which, &st);
        i.is_dir = !r && S_ISDIR(st.st_mode);
    } else if (!r) {
        r = path_info(&p, &i);
    }
    if (!r && !i.is_dir)
        r = -ENOTDIR;
    if (!r)
        path_cwd_set(p.abs);
    return r;
}

long lx_fchdir(long fd, long b, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    long r = dir_is(desc) ? 0 : -ENOTDIR;
    if (!r)
        path_cwd_set(dir_path(desc));
    fd_put(desc);
    return r;
}

long lx_getcwd(long buf, long size, long c, long d, long e, long f)
{
    char cwd[PATH_BYTES];
    path_cwd(cwd);
    size_t n = strlen(cwd) + 1;
    if ((size_t)size < n)
        return -ERANGE;
    memcpy((char *)(uintptr_t)buf, cwd, n);
    return (long)n;
}
