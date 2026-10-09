/* Jam OS's status codes as Linux's errno values: the default answer for
 * each ERR_* (the kernel review's mapping, M12-REVIEW-KERNEL.md's errno
 * row), one to one where it can be. A handler whose call means something
 * else by a status says so itself before falling back on this (a write's
 * ERR_PEER_CLOSED is EPIPE, a read's is the end of the file; a full
 * descriptor table is EMFILE, not EAGAIN). */
#include <errno.h>
#include "layer.h"

long lx_err(status_t st)
{
    switch (st) {
    case OK:                   return 0;
    case ERR_NOT_SUPPORTED:    return -ENOTSUP;
    case ERR_NO_MEMORY:        return -ENOMEM;
    case ERR_INVALID_ARGS:     return -EINVAL;   /* a bad pointer too: no EFAULT */
    case ERR_BAD_HANDLE:       return -EBADF;
    case ERR_WRONG_TYPE:       return -EINVAL;
    case ERR_ACCESS_DENIED:    return -EACCES;
    case ERR_BAD_STATE:        return -EINVAL;
    case ERR_OUT_OF_RANGE:     return -EINVAL;
    case ERR_BUFFER_TOO_SMALL: return -ERANGE;
    case ERR_SHOULD_WAIT:      return -EAGAIN;
    case ERR_TIMED_OUT:        return -ETIMEDOUT;
    case ERR_PEER_CLOSED:      return -EPIPE;
    case ERR_CANCELED:         return -ECANCELED;
    case ERR_ALREADY_BOUND:    return -EEXIST;
    case ERR_NOT_FOUND:        return -ENOENT;
    case ERR_NO_RESOURCES:     return -EAGAIN;
    case ERR_IO:               return -EIO;
    case ERR_ALREADY_EXISTS:   return -EEXIST;
    case ERR_NO_SPACE:         return -ENOSPC;
    case ERR_NOT_DIR:          return -ENOTDIR;
    case ERR_IS_DIR:           return -EISDIR;
    case ERR_NOT_EMPTY:        return -ENOTEMPTY;
    case ERR_NAME_TOO_LONG:    return -ENAMETOOLONG;
    case ERR_BUSY:             return -EBUSY;
    case ERR_INTERRUPTED:      return -EINTR;
    }
    return -EIO;   /* ERR_INTERNAL, and any code this table doesn't know yet */
}
