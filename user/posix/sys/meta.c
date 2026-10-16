/* What a file is and may be (docs/M13-PLAN.md section 3, and M13.6's
 * "What POSIX gains"): the stat family, statfs, access, the attribute
 * changes and the links.
 *
 * On a mount that keeps modes and links (extfs: fs.statfs's FS_FEAT_MODES
 * and FS_FEAT_LINKS) they are the inode's own:
 *   - stat is fs.getattr's (mode, links, owner, three times in ns), lstat
 *     the link itself (FS_NOFOLLOW);
 *   - chmod, chown and utimensat are fs.setattr (or file.setattr on an
 *     open file): every mode bit stored (setuid and setgid never
 *     honoured), the one user as the only owner (EPERM else, and for a
 *     file someone else owns), "now" read here;
 *   - symlink, readlink and link are the service's; link across mounts is
 *     EXDEV, two mounts of one volume too.
 * On FAT's mounts and the boot image nothing changed from B1:
 *   - stat is made up from fs.stat (path.c's path_fill_stat: the mode
 *     from the kind and the mount's read-only flag, the id as st_ino, the
 *     mount's place as st_dev, the one user as owner, the modification
 *     time as all three times);
 *   - chmod, chown and utimensat succeed and change nothing, but on a
 *     read-only mount (EROFS) or to another owner (EPERM: only the one
 *     user exists, and it isn't root);
 *   - link, symlink and mknod are EPERM (Linux's vfat answer), readlink
 *     EINVAL (nothing there is a link).
 * Everywhere: access answers from the modes stat gives (the owner's bits
 * for the one user's files, the others' else: path_perm), EROFS for W_OK
 * on a read-only mount; statfs says the mount's magic, name length and
 * read-only flag (fs.statfs); mknod is EPERM; the umask is kept (the
 * hand-over's, else 022), handed to children and applied to every create
 * (file.c, dir.c); sync and syncfs ask each mount to sync; fadvise is
 * advice, taken and ignored; fallocate is EOPNOTSUPP.
 * Paths under /dev and /etc are kinds.h's: their stat is dev_stat and
 * etc_stat, and a change there is refused (path_libc_refuse). /proc's are
 * procfs.c's, whose links readlink and lstat answer there, stat follows. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_idl.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "files.h"

#define STAT_FLAGS   (AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH)
#define ACCESS_FLAGS (AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)
#define MSDOS_MAGIC  0x4d44        /* statfs f_type: FAT (Linux's MSDOS_SUPER_MAGIC) */
#define TMPFS_MAGIC  0x01021994    /* ... "/", /svc and libc's own trees */
#define FS_BSIZE     4096u
#define ONE_USER     1000u
#define NAME_MAX_FAT 255u          /* FAT's long names, if a service doesn't say */

static lock_t   umask_lock;
static uint32_t umask_now = 022;   /* umask_lock */
static bool     umask_ready;       /* umask_lock: the hand-over's looked at */

/* ---- stat ---------------------------------------------------------------------- */

/* stat of a resolved path into *st (zeroed here); lstat's if p->nofollow.
 * A link out of a mount is followed (p becomes where it led). */
static long stat_path(struct path *p, struct stat *st)
{
    long r;
    do {
        memset(st, 0, sizeof(*st));
        enum path_libc which = path_libc(p);
        if (which == LIBC_PROC && p->nofollow)
            return proc_stat(p, false, st);   /* /proc's links are links too */
        if (which != LIBC_NONE)
            return path_libc_stat(p, which, st);
        struct path_info i;
        r = path_info(p, &i);
        if (!r)
            path_fill_stat(p, &i, st);
    } while (path_again(p, &r));
    return r;
}

/* stat of dirfd + pathname with flags (fstatat's and faccessat's AT_*):
 * the path's, or with AT_EMPTY_PATH and "" dirfd's own. The path goes
 * into *p (for the caller's next question about it); p->abs is "" for a
 * descriptor's stat without a path. */
static long stat_at(long dirfd, const char *pathname, long flags, struct path *p,
                    struct stat *st)
{
    if ((flags & AT_EMPTY_PATH) && pathname && !*pathname) {
        if (dirfd == AT_FDCWD) {
            char cwd[PATH_BYTES];
            path_cwd(cwd);
            long r = path_set(cwd, p);
            return r ? r : stat_path(p, st);
        }
        p->abs[0] = '\0';
        return lx_fstat(dirfd, (long)(uintptr_t)st, 0, 0, 0, 0);
    }
    long r = path_get(dirfd, pathname, p);
    p->nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
    return r ? r : stat_path(p, st);
}

long lx_newfstatat(long dirfd, long pathname, long out, long flags, long e, long f)
{
    if (flags & ~(long)STAT_FLAGS)
        return -EINVAL;
    struct path p;
    return stat_at(dirfd, (const char *)(uintptr_t)pathname, flags, &p,
                   (struct stat *)(uintptr_t)out);
}

long lx_stat(long pathname, long out, long c, long d, long e, long f)
{
    return lx_newfstatat(AT_FDCWD, pathname, out, 0, 0, 0);
}

long lx_lstat(long pathname, long out, long c, long d, long e, long f)
{
    return lx_newfstatat(AT_FDCWD, pathname, out, AT_SYMLINK_NOFOLLOW, 0, 0);
}

/* ---- statfs ----------------------------------------------------------------------- */

static long statfs_path(const struct path *p, struct statfs *s)
{
    memset(s, 0, sizeof(*s));
    s->f_bsize = s->f_frsize = FS_BSIZE;
    s->f_namelen = NAME_MAX_FAT;
    s->f_type = TMPFS_MAGIC;
    s->f_flags = ST_RDONLY;
    if (p->where != PATH_MOUNT || path_libc(p) != LIBC_NONE)
        return 0;
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    uint64_t total = 0, free_bytes = 0;
    uint8_t ro = 0, label[16];
    uint32_t magic = 0, name_max = 0, features = 0;
    status_t st = fs_statfs_within(fs, FILE_TIMEOUT, &total, &free_bytes, &ro, label, &magic,
                                   &name_max, &features);
    jam_handle_close(fs);
    if (st != OK)
        return path_err_read(p, st);
    s->f_type = magic ? magic : MSDOS_MAGIC;   /* fat 0x4d44, the boot image's, ext 0xef53 */
    if (name_max)
        s->f_namelen = name_max;
    s->f_blocks = total / FS_BSIZE;
    s->f_bfree = s->f_bavail = free_bytes / FS_BSIZE;
    s->f_flags = ro ? ST_RDONLY : 0;
    s->f_fsid.__val[0] = (int)(p->mount_len ? p->abs[1] : 0);
    return 0;
}

long lx_statfs(long pathname, long out, long c, long d, long e, long f)
{
    struct path p;
    struct stat st;
    long r = path_get(AT_FDCWD, (const char *)(uintptr_t)pathname, &p);
    if (!r)
        r = stat_path(&p, &st);   /* statfs of something that isn't there: ENOENT */
    return r ? r : statfs_path(&p, (struct statfs *)(uintptr_t)out);
}

long lx_fstatfs(long fd, long out, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    struct path p;
    const char *abs = desc->ops->path ? desc->ops->path(desc) : NULL;
    long r = abs ? path_set(abs, &p) : 0;
    if (!abs)
        p = (struct path){ .abs = "/", .where = PATH_ROOT };   /* a pipe, a device: no mount */
    if (!r)
        r = statfs_path(&p, (struct statfs *)(uintptr_t)out);
    fd_put(desc);
    return r;
}

/* ---- access ---------------------------------------------------------------------- */

long lx_faccessat2(long dirfd, long pathname, long mode, long flags, long e, long f)
{
    if ((mode & ~(long)(R_OK | W_OK | X_OK)) || (flags & ~(long)ACCESS_FLAGS))
        return -EINVAL;
    struct path p;
    struct stat st;
    long r = stat_at(dirfd, (const char *)(uintptr_t)pathname, flags, &p, &st);
    if (r)
        return r;
    enum path_libc which = p.abs[0] ? path_libc(&p) : LIBC_NONE;
    bool ro = p.abs[0] && (which == LIBC_ETC || (which == LIBC_NONE && path_read_only(&p)));
    if ((mode & W_OK) && ro && !S_ISSOCK(st.st_mode))
        return -EROFS;   /* access(2): write asked of a read-only filesystem */
    unsigned may = path_perm(st.st_mode, st.st_uid);
    if (((mode & R_OK) && !(may & 4)) || ((mode & W_OK) && !(may & 2)) ||
        ((mode & X_OK) && !(may & 1)))
        return -EACCES;
    return 0;
}

long lx_faccessat(long dirfd, long pathname, long mode, long d, long e, long f)
{
    return lx_faccessat2(dirfd, pathname, mode, 0, 0, 0);
}

long lx_access(long pathname, long mode, long c, long d, long e, long f)
{
    return lx_faccessat2(AT_FDCWD, pathname, mode, 0, 0, 0);
}

/* ---- chmod, chown, utimensat ------------------------------------------------------- */

/* A change of attributes: fs.setattr's mask and fields; owner_ok false
 * for a chown to anyone but the one user (EPERM). */
struct change {
    uint32_t mask, mode, uid, gid;
    int64_t  atime_ns, mtime_ns;
    bool     owner_ok;
};

/* errno for a refused setattr: EROFS on a read-only mount, else EPERM
 * (not the owner, or a view that may not change it). */
static long change_err(const struct path *p, status_t st)
{
    if (st == ERR_ACCESS_DENIED)
        return path_read_only(p) ? -EROFS : -EPERM;
    return path_err(p, st);
}

/* The change c of p (p->nofollow: lchown's, AT_SYMLINK_NOFOLLOW's). */
static long change_path(struct path *p, const struct change *c)
{
    struct stat st;
    long r = stat_path(p, &st);   /* there? (links out followed: p is where they led) */
    if (r)
        return r;
    enum path_libc which = path_libc(p);
    if (which != LIBC_NONE)
        return path_libc_refuse(which);
    if (!(path_features(p) & FS_FEAT_MODES))   /* FAT keeps nothing */
        return path_read_only(p) ? -EROFS : c->owner_ok ? 0 : -EPERM;
    if (!c->owner_ok)
        return -EPERM;
    handle_t fs;
    if ((r = path_fs(p, &fs)) != 0)
        return r;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    status_t s = fs_setattr_within(fs, FILE_TIMEOUT, rel, len, p->nofollow ? FS_NOFOLLOW : 0,
                                   c->mask, c->mode, c->uid, c->gid, c->atime_ns, c->mtime_ns);
    jam_handle_close(fs);
    if (s == OK)
        return 0;
    r = change_err(p, s);
    return r == PATH_LINK_OUT ? -EIO : r;   /* stat_path followed every link out just now */
}

/* The change c of dirfd + pathname with flags (AT_SYMLINK_NOFOLLOW,
 * AT_EMPTY_PATH): a descriptor's own is made on its open where it has
 * one (file.setattr), else by its path on a mount that keeps modes; any
 * other descriptor keeps nothing, and changes nothing (B1's answer). */
static long change_at(long dirfd, const char *pathname, long flags, const struct change *c)
{
    enum { BY_PATH = 1 };
    struct path *p = malloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    long r;
    if ((flags & AT_EMPTY_PATH) && pathname && !*pathname && dirfd != AT_FDCWD) {
        struct fd_desc *d = fd_get(dirfd);
        if (!d) {
            free(p);
            return -EBADF;
        }
        long done = c->owner_ok ? file_setattr_fd(d, c->mask, c->mode, c->uid, c->gid,
                                                  c->atime_ns, c->mtime_ns)
                                : -ENOTSUP;
        const char *abs = d->ops->path ? d->ops->path(d) : NULL;
        if (done != -ENOTSUP)
            r = done;
        else if (abs && !path_set(abs, p) && (path_features(p) & FS_FEAT_MODES))
            r = BY_PATH;   /* a directory, an O_PATH file */
        else
            r = c->owner_ok ? 0 : -EPERM;
        fd_put(d);
        if (r == BY_PATH)
            r = change_path(p, c);
    } else if ((flags & AT_EMPTY_PATH) && pathname && !*pathname) {   /* the current directory */
        char *cwd = malloc(PATH_BYTES);
        r = cwd ? 0 : -ENOMEM;
        if (cwd)
            path_cwd(cwd);
        if (!r && !(r = path_set(cwd, p)))
            r = change_path(p, c);
        free(cwd);
    } else if (!(r = path_get(dirfd, pathname, p))) {
        p->nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
        r = change_path(p, c);
    }
    free(p);
    return r;
}

/* chown's owner: -1 (unchanged) or the one user; any other is EPERM. */
static bool owner_ok(long uid, long gid)
{
    return ((uint32_t)uid == (uint32_t)-1 || uid == ONE_USER) &&
           ((uint32_t)gid == (uint32_t)-1 || gid == ONE_USER);
}

static long chmod_at(long dirfd, const char *pathname, long flags, long mode)
{
    struct change c = { .mask = FS_ATTR_MODE, .mode = (uint32_t)mode & 07777u, .owner_ok = true };
    return change_at(dirfd, pathname, flags, &c);
}

long lx_fchmodat(long dirfd, long pathname, long mode, long d, long e, long f)
{
    return chmod_at(dirfd, (const char *)(uintptr_t)pathname, 0, mode);
}

long lx_chmod(long pathname, long mode, long c, long d, long e, long f)
{
    return chmod_at(AT_FDCWD, (const char *)(uintptr_t)pathname, 0, mode);
}

long lx_fchmod(long fd, long mode, long c, long d, long e, long f)
{
    return chmod_at(fd, "", AT_EMPTY_PATH, mode);
}

long lx_fchownat(long dirfd, long pathname, long uid, long gid, long flags, long f)
{
    if (flags & ~(long)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    struct change c = { .owner_ok = owner_ok(uid, gid), .uid = ONE_USER, .gid = ONE_USER };
    if ((uint32_t)uid != (uint32_t)-1)
        c.mask |= FS_ATTR_UID;
    if ((uint32_t)gid != (uint32_t)-1)
        c.mask |= FS_ATTR_GID;
    return change_at(dirfd, (const char *)(uintptr_t)pathname, flags, &c);
}

long lx_chown(long pathname, long uid, long gid, long d, long e, long f)
{
    return lx_fchownat(AT_FDCWD, pathname, uid, gid, 0, 0);
}

long lx_lchown(long pathname, long uid, long gid, long d, long e, long f)
{
    return lx_fchownat(AT_FDCWD, pathname, uid, gid, AT_SYMLINK_NOFOLLOW, 0);
}

long lx_fchown(long fd, long uid, long gid, long d, long e, long f)
{
    return lx_fchownat(fd, (long)(uintptr_t)"", uid, gid, AT_EMPTY_PATH, 0);
}

/* One of utimensat's times into the change (bit: FS_ATTR_ATIME or _MTIME):
 * false for a bad one. */
static bool time_of(const struct timespec *t, int64_t now_ns, uint32_t bit, int64_t *ns,
                    uint32_t *mask)
{
    if (t && t->tv_nsec == UTIME_OMIT)
        return true;
    if (t && t->tv_nsec != UTIME_NOW) {
        if (t->tv_nsec < 0 || t->tv_nsec >= (long)NS_PER_S ||
            t->tv_sec > INT64_MAX / (int64_t)NS_PER_S - 1 ||
            t->tv_sec < INT64_MIN / (int64_t)NS_PER_S + 1)
            return false;
        *ns = (int64_t)t->tv_sec * (int64_t)NS_PER_S + t->tv_nsec;
    } else {
        *ns = now_ns;
    }
    *mask |= bit;
    return true;
}

/* utimensat: a NULL path is the descriptor's own (futimens); NULL times
 * are both now. */
long lx_utimensat(long dirfd, long pathname, long times, long flags, long e, long f)
{
    if (flags & ~(long)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    const struct timespec *t = (const struct timespec *)(uintptr_t)times;
    struct timespec wall = { 0 };
    (void)lx_clock_gettime(CLOCK_REALTIME, (long)(uintptr_t)&wall, 0, 0, 0, 0);
    int64_t now_ns = (int64_t)wall.tv_sec * (int64_t)NS_PER_S + wall.tv_nsec;
    struct change c = { .owner_ok = true };
    if (!time_of(t, now_ns, FS_ATTR_ATIME, &c.atime_ns, &c.mask) ||
        !time_of(t ? t + 1 : NULL, now_ns, FS_ATTR_MTIME, &c.mtime_ns, &c.mask))
        return -EINVAL;
    const char *path = (const char *)(uintptr_t)pathname;
    return path ? change_at(dirfd, path, flags, &c) : change_at(dirfd, "", AT_EMPTY_PATH, &c);
}

/* ---- links and nodes ------------------------------------------------------------ */

/* A new name at p that can't be made there (FAT's link and symlink, any
 * mknod): EEXIST if something is there, the read-only or /dev, /etc
 * refusal, else EPERM. */
static long cant_make(struct path *p)
{
    struct stat st;
    p->nofollow = true;
    long r = stat_path(p, &st);
    if (!r)
        return -EEXIST;
    if (r != -ENOENT)
        return r;
    enum path_libc which = path_libc(p);
    if (which != LIBC_NONE)
        return path_libc_refuse(which);
    return path_read_only(p) ? -EROFS : -EPERM;
}

static long cant_make_at(long dirfd, const char *pathname)
{
    struct path p;
    long r = path_get(dirfd, pathname, &p);
    return r ? r : cant_make(&p);
}

/* Do two paths (physical) lie on one mount of the namespace? */
static bool same_mount(const struct path *a, const struct path *b)
{
    return a->where == PATH_MOUNT && b->where == PATH_MOUNT && a->mount_len == b->mount_len &&
           !memcmp(a->abs, b->abs, a->mount_len);
}

/* link of a (p->nofollow unless AT_SYMLINK_FOLLOW) to the new name b. */
static long link_paths(struct path *a, struct path *b)
{
    struct stat st;
    long r = stat_path(a, &st);   /* it must be there */
    if (!r)
        r = path_place(a);
    b->nofollow = true;
    if (!r)
        r = path_place(b);
    if (r)
        return r;
    if ((a->links || b->links) && path_libc(a) == LIBC_NONE && path_libc(b) == LIBC_NONE &&
        a->where == PATH_MOUNT && b->where == PATH_MOUNT && !same_mount(a, b))
        return -EXDEV;   /* two mounts, even of one volume */
    if (!a->links || !b->links || path_libc(a) != LIBC_NONE || path_libc(b) != LIBC_NONE)
        return cant_make(b);
    if (S_ISDIR(st.st_mode))
        return -EPERM;   /* link(2): no hard link to a directory */
    handle_t fs;
    if ((r = path_fs(a, &fs)) != 0)
        return r;
    uint32_t alen, blen;
    const char *arel = path_rel(a, &alen), *brel = path_rel(b, &blen);
    status_t s = fs_link_within(fs, FILE_TIMEOUT, arel, alen, brel, blen);
    jam_handle_close(fs);
    if (s == OK)
        return 0;
    if (s == ERR_IS_DIR)
        return -EPERM;
    r = path_err(b, s);
    return r == PATH_LINK_OUT ? -EIO : r;   /* both were placed just now */
}

long lx_linkat(long olddirfd, long oldpath, long newdirfd, long newpath, long flags, long f)
{
    if (flags & ~(long)(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    const char *from = (const char *)(uintptr_t)oldpath;
    if ((flags & AT_EMPTY_PATH) && from && !*from) {   /* a descriptor's file: as before */
        struct path p;
        struct stat st;
        long r = stat_at(olddirfd, from, AT_EMPTY_PATH, &p, &st);
        return r ? r : cant_make_at(newdirfd, (const char *)(uintptr_t)newpath);
    }
    struct path *two = malloc(2 * sizeof(*two));
    if (!two)
        return -ENOMEM;
    long r = path_get(olddirfd, from, &two[0]);
    if (!r)
        r = path_get(newdirfd, (const char *)(uintptr_t)newpath, &two[1]);
    if (!r) {
        two[0].nofollow = !(flags & AT_SYMLINK_FOLLOW);   /* Linux's linkat follows no link */
        r = link_paths(&two[0], &two[1]);
    }
    free(two);
    return r;
}

long lx_link(long oldpath, long newpath, long c, long d, long e, long f)
{
    return lx_linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0, 0);
}

/* A symbolic link at p (its last name never followed) to target. */
static long symlink_path(struct path *p, const char *target, size_t tlen)
{
    if (path_libc(p) != LIBC_NONE || !p->links)
        return cant_make(p);
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    status_t s = fs_symlink_within(fs, FILE_TIMEOUT, target, (uint32_t)tlen, rel, len);
    jam_handle_close(fs);
    return s == OK ? 0 : path_err(p, s);
}

long lx_symlinkat(long target, long newdirfd, long newpath, long d, long e, long f)
{
    const char *t = (const char *)(uintptr_t)target;
    if (!t)
        return -EFAULT;
    size_t tlen = strnlen(t, PATH_BYTES);
    if (!tlen)
        return -ENOENT;   /* symlink(2): an empty target */
    if (tlen == PATH_BYTES)
        return -ENAMETOOLONG;
    struct path p;
    long r = path_get(newdirfd, (const char *)(uintptr_t)newpath, &p);
    p.nofollow = true;
    if (!r)
        do
            r = symlink_path(&p, t, tlen);
        while (path_again(&p, &r));
    return r;
}

long lx_symlink(long target, long newpath, long c, long d, long e, long f)
{
    return lx_symlinkat(target, AT_FDCWD, newpath, 0, 0, 0);
}

long lx_mknodat(long dirfd, long pathname, long mode, long dev, long e, long f)
{
    return cant_make_at(dirfd, (const char *)(uintptr_t)pathname);
}

long lx_mknod(long pathname, long mode, long dev, long d, long e, long f)
{
    return cant_make_at(AT_FDCWD, pathname ? (const char *)(uintptr_t)pathname : NULL);
}

/* readlink of p (its last name not followed): the target's bytes copied
 * into buf (at most size, no NUL), EINVAL for anything there that isn't
 * a link. */
static long readlink_path(struct path *p, char *buf, size_t size)
{
    enum path_libc which = path_libc(p);
    if (which == LIBC_PROC)
        return proc_readlink(p, buf, size);
    struct stat st;
    if (which != LIBC_NONE || !p->links) {
        long r = stat_path(p, &st);
        return r ? r : -EINVAL;   /* there, and not a link */
    }
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    char *target = malloc(PATH_BYTES);
    uint32_t len, n = 0;
    const char *rel = path_rel(p, &len);
    status_t s = target ? fs_readlink_within(fs, FILE_TIMEOUT, rel, len, target, &n) : ERR_NO_MEMORY;
    jam_handle_close(fs);
    if (s == OK) {
        r = (long)(n < size ? n : size);
        memcpy(buf, target, (size_t)r);
    } else {
        r = path_err_read(p, s);   /* ERR_INVALID_ARGS: not a link, EINVAL */
    }
    free(target);
    return r;
}

long lx_readlinkat(long dirfd, long pathname, long buf, long size, long e, long f)
{
    if (size <= 0)
        return -EINVAL;
    struct path p;
    long r = path_get(dirfd, (const char *)(uintptr_t)pathname, &p);
    p.nofollow = true;
    if (!r)
        do
            r = readlink_path(&p, (char *)(uintptr_t)buf, (size_t)size);
        while (path_again(&p, &r));
    return r;
}

long lx_readlink(long pathname, long buf, long size, long d, long e, long f)
{
    return lx_readlinkat(AT_FDCWD, pathname, buf, size, 0, 0);
}

/* ---- umask, sync, advice ------------------------------------------------------- */

/* The first look at the umask takes the hand-over's, if the program had
 * one. umask_lock. */
static void umask_start_locked(void)
{
    if (umask_ready)
        return;
    umask_ready = true;
    const char *cwd;
    const struct posixfds_state *s = lx_handover(&cwd);
    if (s)
        umask_now = s->umask & 0777;
}

uint32_t lx_umask_now(void)
{
    lock_take(&umask_lock);
    umask_start_locked();
    uint32_t m = umask_now;
    lock_give(&umask_lock);
    return m;
}

long lx_umask(long mask, long b, long c, long d, long e, long f)
{
    lock_take(&umask_lock);
    umask_start_locked();
    uint32_t old = umask_now;
    umask_now = (uint32_t)mask & 0777;
    lock_give(&umask_lock);
    return old;
}

void umask_fork(int step)
{
    if (step == LX_FORK_TAKE)
        lock_take(&umask_lock);
    else if (step == LX_FORK_GIVE)
        lock_give(&umask_lock);
    else
        umask_lock = (lock_t){ 0 };   /* the child: the umask is ours */
}

long lx_sync(long a, long b, long c, long d, long e, long f)
{
    char m[NS_NAME_MAX];
    for (unsigned i = 0; ns_mount_at(i, m); i++)
        (void)fs_sync(m);   /* sync(2) can't fail: a mount that couldn't is its own */
    return 0;
}

long lx_syncfs(long fd, long b, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    struct path p;
    const char *abs = desc->ops->path ? desc->ops->path(desc) : NULL;
    long r = 0;
    if (abs && !path_set(abs, &p) && p.where == PATH_MOUNT && path_libc(&p) == LIBC_NONE) {
        char m[NS_NAME_MAX];
        if (p.mount_len < NS_NAME_MAX) {
            memcpy(m, p.abs, p.mount_len);
            m[p.mount_len] = '\0';
            status_t st = fs_sync(m);
            r = st == OK ? 0 : path_err(&p, st);
        }
    }
    fd_put(desc);
    return r;
}

long lx_fadvise64(long fd, long off, long len, long advice, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    long r = !desc->ops->seek ? -ESPIPE : (advice < 0 || advice > 5 || len < 0) ? -EINVAL : 0;
    fd_put(desc);
    return r;
}

long lx_fallocate(long fd, long mode, long off, long len, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    fd_put(desc);
    return -EOPNOTSUPP;   /* FAT can't reserve without writing; extfs: not yet */
}
