/* Stand-in kinds (stdfd.h says until when), over the descriptor table
 * (fd.h):
 *   - the null device: reads end at once, writes vanish;
 *   - the log: writes go to debug_write, as a native program's printf
 *     does when it has no SR_STDOUT; it can't be read;
 *   - a pipe end: a channel of byte messages (a message is up to
 *     CHANNEL_MAX_BYTES; the bytes a read doesn't take wait in the
 *     description's buffer for the next). Its read and write hold
 *     nothing of the table's while they wait; two threads reading one
 *     pipe at once is the program's race, as stdio would make it;
 * and fcntl's descriptor flags and status flags. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include "stdfd.h"

/* A pipe end's state. */
struct spipe {
    handle_t ch;                     /* the channel end */
    uint32_t have, at;               /* buf[at .. have) not read yet */
    uint8_t  buf[CHANNEL_MAX_BYTES];
};

/* ---- the null device and the log ---------------------------------------------- */

static long null_read(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return 0;
}

static long null_write(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return fd_iov_total(iov, n);
}

static long char_stat(struct fd_desc *d, struct stat *st)
{
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    return 0;
}

static long log_write(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    long done = 0;
    for (int i = 0; i < n; i++)
        for (size_t at = 0; at < iov[i].iov_len;) {
            size_t k = iov[i].iov_len - at > DEBUG_WRITE_MAX ? DEBUG_WRITE_MAX
                                                             : iov[i].iov_len - at;
            if (jam_debug_write((const char *)iov[i].iov_base + at, k) != OK)
                return done ? done : -EIO;
            at += k;
            done += (long)k;
        }
    return done;
}

static const struct fd_ops null_ops = {
    .name = "null", .read = null_read, .write = null_write, .stat = char_stat,
};

static const struct fd_ops log_ops = {
    .name = "log", .write = log_write, .stat = char_stat,
};

/* ---- a pipe end ----------------------------------------------------------------- */

/* Wait for sig on a pipe (unless it is non-blocking): 0, or -errno. */
static long pipe_wait(struct fd_desc *d, signals_t sig)
{
    if (fd_status(d) & O_NONBLOCK)
        return -EAGAIN;
    const struct spipe *p = d->priv;
    signals_t seen;
    status_t st = jam_object_wait_one(p->ch, sig | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
    return st == OK ? 0 : lx_err(st);
}

static long pipe_write_one(struct fd_desc *d, const uint8_t *src, size_t n)
{
    const struct spipe *p = d->priv;
    size_t done = 0;
    while (done < n) {
        uint32_t k = n - done > CHANNEL_MAX_BYTES ? CHANNEL_MAX_BYTES : (uint32_t)(n - done);
        status_t st = jam_channel_write(p->ch, src + done, k, NULL, 0);
        if (st == ERR_SHOULD_WAIT) {   /* the reader is behind */
            long r = pipe_wait(d, SIG_WRITABLE);
            if (r)
                return done ? (long)done : r;
            continue;
        }
        if (st != OK)
            return done ? (long)done : st == ERR_PEER_CLOSED ? -EPIPE : lx_err(st);
        done += k;
    }
    return (long)done;
}

static long pipe_write(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    long total = 0;
    for (int i = 0; i < n; i++) {
        long r = pipe_write_one(d, iov[i].iov_base, iov[i].iov_len);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((size_t)r < iov[i].iov_len)
            break;
    }
    return total;
}

/* Fill a pipe's buffer with its next message: 1, 0 at the end, or -errno. */
static long pipe_fill(struct fd_desc *d)
{
    struct spipe *p = d->priv;
    for (;;) {
        uint32_t got = 0, nh = 0;
        struct channel_read_args a = {
            .h = p->ch,
            .rd_cap = sizeof(p->buf),
            .rd_bytes = (uint64_t)(uintptr_t)p->buf,
            .rd_actual = (uint64_t)(uintptr_t)&got,
            .rd_hactual = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == OK) {
            p->at = 0;
            p->have = got;
            if (got)
                return 1;
            continue;   /* an empty message carries nothing */
        }
        if (st == ERR_PEER_CLOSED)
            return 0;
        if (st != ERR_SHOULD_WAIT)
            return lx_err(st);
        long r = pipe_wait(d, SIG_READABLE);
        if (r)
            return r;
    }
}

static long pipe_read(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    struct spipe *p = d->priv;
    long want = fd_iov_total(iov, n);
    if (want <= 0)
        return want;
    if (p->at == p->have) {
        long r = pipe_fill(d);
        if (r <= 0)
            return r;
    }
    size_t k = p->have - p->at < (size_t)want ? p->have - p->at : (size_t)want;
    fd_iov_scatter(iov, n, 0, p->buf + p->at, k);
    p->at += (uint32_t)k;
    return (long)k;
}

static long pipe_stat(struct fd_desc *d, struct stat *st)
{
    st->st_mode = S_IFIFO | 0600;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    return 0;
}

static void pipe_close(struct fd_desc *d)
{
    struct spipe *p = d->priv;
    jam_handle_close(p->ch);
    free(p);
}

static const struct fd_ops pipe_ops = {
    .name = "pipe", .read = pipe_read, .write = pipe_write, .stat = pipe_stat,
    .close = pipe_close,
};

/* A pipe end's description over ch (consumed), with the bytes a starter
 * read and didn't take. */
static long pipe_new(handle_t ch, uint32_t status, const void *left, uint32_t nleft,
                     struct fd_desc **out)
{
    struct spipe *p = calloc(1, sizeof(*p));
    struct fd_desc *d = p ? fd_new(&pipe_ops, status, p) : NULL;
    if (!d) {
        free(p);
        jam_handle_close(ch);
        return -ENOMEM;
    }
    p->ch = ch;
    p->have = nleft < sizeof(p->buf) ? nleft : sizeof(p->buf);
    if (p->have)
        memcpy(p->buf, left, p->have);
    *out = d;
    return 0;
}

/* ---- the start ------------------------------------------------------------------ */

long stdfd_adopt_pipe(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                      struct fd_desc **out)
{
    return pipe_new(hs[0], rec->status, data, rec->data_len, out);
}

long stdfd_adopt_dev(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                     struct fd_desc **out)
{
    if (strcmp(data, "null"))
        return -ENODEV;
    struct fd_desc *d = fd_new(&null_ops, rec->status, NULL);
    if (!d)
        return -ENOMEM;
    *out = d;
    return 0;
}

void stdfd_default(void)
{
    struct fd_desc *in = fd_new(&null_ops, O_RDONLY, NULL), *out = NULL;
    if (in)
        (void)fd_replace(0, in, false);   /* 0 is in range and free at the start */
    handle_t ch = startup_handle(SR_STDOUT);
    if (ch == HANDLE_INVALID)
        out = fd_new(&log_ops, O_WRONLY, NULL);
    else if (pipe_new(ch, O_WRONLY, NULL, 0, &out) < 0)
        out = NULL;
    if (!out)
        return;
    fd_ref(out);   /* 1 and 2 share it, as a shell's 2>&1 */
    (void)fd_replace(1, out, false);
    (void)fd_replace(2, out, false);
}

/* ---- fcntl ----------------------------------------------------------------------- */

#define SETFL_BITS (O_APPEND | O_NONBLOCK | O_ASYNC | O_DIRECT | O_NOATIME)

long lx_fcntl(long fd, long cmd, long arg, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    long r;
    switch (cmd) {
    case F_GETFD:
        r = fd_cloexec(fd);
        break;
    case F_SETFD:
        r = fd_set_cloexec(fd, arg & FD_CLOEXEC);
        break;
    case F_GETFL:
        r = fd_status(desc);
        break;
    case F_SETFL:   /* the access mode and the creation flags are ignored, as Linux does */
        r = fd_status_set(desc, SETFL_BITS, (uint32_t)arg);
        break;
    default:
        r = -EINVAL;   /* F_DUPFD and the rest: B2's */
    }
    fd_put(desc);
    return r;
}
