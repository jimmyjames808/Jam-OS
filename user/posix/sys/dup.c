/* The calls on descriptors themselves (docs/M13-PLAN.md section 3): dup,
 * dup2, dup3 and fcntl, over the descriptor table (fd.h). A dup shares
 * the description (its offset and status flags); FD_CLOEXEC is the new
 * slot's own.
 *
 * fcntl:
 *   F_DUPFD, F_DUPFD_CLOEXEC   the lowest free descriptor >= arg (EINVAL at or
 *                              past RLIMIT_NOFILE, as POSIX says)
 *   F_GETFD, F_SETFD           FD_CLOEXEC
 *   F_GETFL, F_SETFL           the status flags; F_SETFL changes only
 *                              O_APPEND and O_NONBLOCK (the access mode
 *                              and the creation flags are ignored, as
 *                              Linux does; O_DIRECT and O_NOATIME too).
 *                              O_ASYNC is EINVAL: no SIGIO is ever sent
 *   F_GETLK, F_SETLK, F_SETLKW and the F_OFD_ ones
 *                              accepted, never enforced (one user, and
 *                              no file server keeps locks): a lock is
 *                              always granted, F_GETLK says F_UNLCK. The
 *                              lock's type and the access mode are
 *                              checked as Linux checks them
 *   F_GETOWN, F_SETOWN and the _EX ones
 *                              no owner is kept (nothing signals one)
 *   F_GETPIPE_SZ, F_SETPIPE_SZ a pipe holds 64 KiB a message; the size
 *                              never changes
 *   F_ADD_SEALS, F_GET_SEALS   a memfd's (memfd.c)
 * Anything else is EINVAL. flock's locks are as fcntl's: checked and granted. */
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include "kinds.h"

#define PIPE_SZ CHANNEL_MAX_BYTES   /* F_GETPIPE_SZ: the most one message holds */

long kind_install(const struct fd_ops *ops, void *priv, uint32_t status, bool cloexec)
{
    struct fd_desc *d = fd_new(ops, status, priv);
    if (!d) {
        if (ops->close)
            ops->close(&(struct fd_desc){ .ops = ops, .priv = priv });
        return -ENOMEM;
    }
    return fd_install(d, 0, cloexec);
}

void *kind_data(const void *src, size_t n, long *err)
{
    void *p = n ? jam_malloc(n) : NULL;
    if (n && !p)
        *err = -ENOMEM;
    if (p)
        memcpy(p, src, n);
    return p;
}

long lx_dup(long fd, long b, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    return desc ? fd_install(desc, 0, false) : -EBADF;
}

static long dup_to(long old, long new, bool cloexec)
{
    struct fd_desc *desc = fd_get(old);
    return desc ? fd_replace(new, desc, cloexec) : -EBADF;
}

long lx_dup2(long old, long new, long c, long d, long e, long f)
{
    if (old != new)
        return dup_to(old, new, false);
    struct fd_desc *desc = fd_get(old);   /* the same: only check it is open */
    if (!desc)
        return -EBADF;
    fd_put(desc);
    return new;
}

long lx_dup3(long old, long new, long flags, long d, long e, long f)
{
    if (flags & ~(long)O_CLOEXEC || old == new)
        return -EINVAL;
    return dup_to(old, new, flags & O_CLOEXEC);
}

/* The record locks: checked, granted, never kept. arg is the caller's
 * struct flock. */
static long flock_cmd(struct fd_desc *d, long cmd, long arg)
{
    struct flock l;
    if (uc_get(&l, (const void *)(uintptr_t)arg, sizeof(l)))
        return -EFAULT;
    if (l.l_whence != SEEK_SET && l.l_whence != SEEK_CUR && l.l_whence != SEEK_END)
        return -EINVAL;
    if (l.l_type != F_RDLCK && l.l_type != F_WRLCK && l.l_type != F_UNLCK)
        return -EINVAL;
    if (cmd == F_GETLK || cmd == F_OFD_GETLK) {
        l.l_type = F_UNLCK;   /* nobody else holds one */
        return uc_put((void *)(uintptr_t)arg, &l, sizeof(l));
    }
    if ((l.l_type == F_RDLCK && !fd_can_read(d)) || (l.l_type == F_WRLCK && !fd_can_write(d)))
        return -EBADF;
    return 0;
}

/* flock: as fcntl's locks, checked and granted, never kept. */
long lx_flock(long fd, long op, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    fd_put(desc);
    long how = op & ~(long)LOCK_NB;
    return how == LOCK_SH || how == LOCK_EX || how == LOCK_UN ? 0 : -EINVAL;
}

static long setfl(struct fd_desc *d, long arg)
{
    if (arg & O_ASYNC)
        return -EINVAL;
    return fd_status_set(d, O_APPEND | O_NONBLOCK, (uint32_t)arg);
}

static long pipe_size(struct fd_desc *d, long cmd, long arg)
{
    if (d->ops != &pipe_ops && d->ops != &sockp_ops)
        return -EBADF;
    if (cmd == F_SETPIPE_SZ && (arg < 0 || arg > PIPE_SZ * CHANNEL_MAX_QUEUED))
        return -EINVAL;
    return PIPE_SZ;
}

/* The commands on the description (fd's, held by the caller). */
static long desc_cmd(struct fd_desc *d, long cmd, long arg)
{
    struct f_owner_ex own = { .type = F_OWNER_PID, .pid = 0 };
    switch (cmd) {
    case F_GETFL:       return fd_status(d);
    case F_SETFL:       return setfl(d, arg);
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
    case F_OFD_GETLK:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:  return flock_cmd(d, cmd, arg);
    case F_GETOWN:      return 0;
    case F_SETOWN:      return 0;
    case F_GETOWN_EX:   return uc_put((void *)(uintptr_t)arg, &own, sizeof(own));
    case F_SETOWN_EX:   return 0;
    case F_GETPIPE_SZ:
    case F_SETPIPE_SZ:  return pipe_size(d, cmd, arg);
    case F_ADD_SEALS:   return memfd_add_seals(d, (unsigned)arg);
    case F_GET_SEALS:   return memfd_get_seals(d);
    }
    return -EINVAL;
}

long lx_fcntl(long fd, long cmd, long arg, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    long r;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        if (arg < 0 || arg >= FD_MAX || (uint64_t)arg >= rlimit_cur(RLIMIT_NOFILE)) {
            r = -EINVAL;
            break;
        }
        fd_ref(desc);   /* the new slot's; fd_install consumes it */
        r = fd_install(desc, arg, cmd == F_DUPFD_CLOEXEC);
        break;
    case F_GETFD:
        r = fd_cloexec(fd);
        if (r > 0)
            r = FD_CLOEXEC;
        break;
    case F_SETFD:
        r = fd_set_cloexec(fd, arg & FD_CLOEXEC);
        break;
    default:
        r = desc_cmd(desc, cmd, arg);
    }
    fd_put(desc);
    return r;
}
