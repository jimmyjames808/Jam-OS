/* What a file is and may be (docs/M13-PLAN.md section 3): the stat
 * family, statfs, access, and the calls FAT has no answer for.
 *
 *   - stat is made up from fs.stat (path.c's path_fill_stat: the mode
 *     from the kind and the mount's read-only flag, the id as st_ino, the
 *     mount's place as st_dev, the one user as owner, the modification
 *     time as all three times);
 *   - access answers from those modes, with EROFS for W_OK on a
 *     read-only mount;
 *   - chmod, chown and utimensat succeed and change nothing (FAT keeps no
 *     modes or owners), but on a read-only mount (EROFS) or to another
 *     owner (EPERM: only the one user exists, and it isn't root);
 *   - link, symlink and mknod are EPERM (Linux's vfat answer), readlink
 *     EINVAL (nothing is a link);
 *   - umask is kept (the hand-over's, else 022) and ignored;
 *   - sync and syncfs ask each mount to sync; fadvise is advice, taken
 *     and ignored; fallocate is EOPNOTSUPP.
 * Paths under /dev and /etc are kinds.h's: their stat is dev_stat and
 * etc_stat, and a change there is refused (path_libc_refuse). */
#include <errno.h>
#include <fcntl.h>
#include <fs_idl.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "files.h"

#define STAT_FLAGS   (AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH)
#define ACCESS_FLAGS (AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)
#define MSDOS_MAGIC  0x4d44        /* statfs f_type: FAT (Linux's MSDOS_SUPER_MAGIC) */
#define RAMFS_MAGIC  0x858458f6    /* ... the boot image, read from memory */
#define TMPFS_MAGIC  0x01021994    /* ... "/", /svc and libc's own trees */
#define FS_BSIZE     4096u
#define ONE_USER     1000u

static uint32_t umask_now = 022;   /* atomic */
static bool     umask_ready;       /* the hand-over's looked at; atomic */

/* ---- stat ---------------------------------------------------------------------- */

/* stat of a resolved path into *st (zeroed here). */
static long stat_path(const struct path *p, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    enum path_libc which = path_libc(p);
    if (which != LIBC_NONE)
        return path_libc_stat(p, which, st);
    struct path_info i;
    long r = path_info(p, &i);
    if (!r)
        path_fill_stat(p, &i, st);
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
    s->f_namelen = 255;   /* FAT's long names */
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
    status_t st = fs_statfs_within(fs, FILE_TIMEOUT, &total, &free_bytes, &ro, label);
    jam_handle_close(fs);
    if (st != OK)
        return path_err(p, st);
    s->f_type = !memcmp(label, "BOOTFS", 7) ? RAMFS_MAGIC : MSDOS_MAGIC;
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
    if (((mode & R_OK) && !(st.st_mode & S_IRUSR)) || ((mode & W_OK) && !(st.st_mode & S_IWUSR)) ||
        ((mode & X_OK) && !(st.st_mode & S_IXUSR)))
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

/* ---- changes FAT keeps nothing of ------------------------------------------------ */

/* chmod, chown or utimensat of dirfd + pathname (flags: the AT_* allowed)
 * that would change nothing: 0 if p exists and may change, else -errno. */
static long no_change_at(long dirfd, const char *pathname, long flags)
{
    struct path p;
    struct stat st;
    long r = stat_at(dirfd, pathname, flags, &p, &st);
    if (r || !p.abs[0])
        return r;
    enum path_libc which = path_libc(&p);
    if (which != LIBC_NONE)
        return path_libc_refuse(which);
    return path_read_only(&p) ? -EROFS : 0;
}

/* chown's owner: -1 (unchanged) or the one user; any other is EPERM. */
static bool owner_ok(long uid, long gid)
{
    return ((uint32_t)uid == (uint32_t)-1 || uid == ONE_USER) &&
           ((uint32_t)gid == (uint32_t)-1 || gid == ONE_USER);
}

long lx_fchmodat(long dirfd, long pathname, long mode, long d, long e, long f)
{
    return no_change_at(dirfd, (const char *)(uintptr_t)pathname, 0);
}

long lx_chmod(long pathname, long mode, long c, long d, long e, long f)
{
    return no_change_at(AT_FDCWD, (const char *)(uintptr_t)pathname, 0);
}

long lx_fchmod(long fd, long mode, long c, long d, long e, long f)
{
    return no_change_at(fd, "", AT_EMPTY_PATH);
}

long lx_fchownat(long dirfd, long pathname, long uid, long gid, long flags, long f)
{
    if (flags & ~(long)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    long r = no_change_at(dirfd, (const char *)(uintptr_t)pathname, flags);
    return r ? r : owner_ok(uid, gid) ? 0 : -EPERM;
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

/* utimensat: a NULL path is the descriptor's own (futimens). */
long lx_utimensat(long dirfd, long pathname, long times, long flags, long e, long f)
{
    if (flags & ~(long)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    const char *path = (const char *)(uintptr_t)pathname;
    return path ? no_change_at(dirfd, path, flags) : no_change_at(dirfd, "", AT_EMPTY_PATH);
}

/* ---- links and nodes: none on FAT --------------------------------------------- */

/* A new name at dirfd + pathname that FAT can't make: EEXIST if
 * something is there, the read-only or /dev, /etc refusal, else EPERM. */
static long cant_make(long dirfd, const char *pathname)
{
    struct path p;
    struct stat st;
    long r = stat_at(dirfd, pathname, 0, &p, &st);
    if (!r)
        return -EEXIST;
    if (r != -ENOENT)
        return r;
    enum path_libc which = path_libc(&p);
    if (which != LIBC_NONE)
        return path_libc_refuse(which);
    return path_read_only(&p) ? -EROFS : -EPERM;
}

long lx_linkat(long olddirfd, long oldpath, long newdirfd, long newpath, long flags, long f)
{
    if (flags & ~(long)(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    struct path p;
    struct stat st;
    long r = stat_at(olddirfd, (const char *)(uintptr_t)oldpath, flags, &p, &st);
    return r ? r : cant_make(newdirfd, (const char *)(uintptr_t)newpath);
}

long lx_link(long oldpath, long newpath, long c, long d, long e, long f)
{
    return lx_linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0, 0);
}

long lx_symlinkat(long target, long newdirfd, long newpath, long d, long e, long f)
{
    if (!target)
        return -EFAULT;
    return cant_make(newdirfd, (const char *)(uintptr_t)newpath);
}

long lx_symlink(long target, long newpath, long c, long d, long e, long f)
{
    return lx_symlinkat(target, AT_FDCWD, newpath, 0, 0, 0);
}

long lx_mknodat(long dirfd, long pathname, long mode, long dev, long e, long f)
{
    return cant_make(dirfd, (const char *)(uintptr_t)pathname);
}

long lx_mknod(long pathname, long mode, long dev, long d, long e, long f)
{
    return cant_make(AT_FDCWD, pathname ? (const char *)(uintptr_t)pathname : NULL);
}

long lx_readlinkat(long dirfd, long pathname, long buf, long size, long e, long f)
{
    if (size <= 0)
        return -EINVAL;
    struct path p;
    struct stat st;
    long r = stat_at(dirfd, (const char *)(uintptr_t)pathname, 0, &p, &st);
    return r ? r : -EINVAL;   /* there, and not a link */
}

long lx_readlink(long pathname, long buf, long size, long d, long e, long f)
{
    return lx_readlinkat(AT_FDCWD, pathname, buf, size, 0, 0);
}

/* ---- umask, sync, advice ------------------------------------------------------- */

long lx_umask(long mask, long b, long c, long d, long e, long f)
{
    if (!__atomic_load_n(&umask_ready, __ATOMIC_ACQUIRE)) {
        const char *cwd;
        const struct posixfds_state *s = lx_handover(&cwd);
        uint32_t want = 022;   /* the hand-over's, if the program had one */
        if (s)
            want = s->umask & 0777;
        bool no = false;
        if (__atomic_compare_exchange_n(&umask_ready, &no, true, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            __atomic_store_n(&umask_now, want, __ATOMIC_RELEASE);
    }
    return __atomic_exchange_n(&umask_now, (uint32_t)mask & 0777, __ATOMIC_ACQ_REL);
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
    return -EOPNOTSUPP;   /* FAT can't reserve without writing */
}
