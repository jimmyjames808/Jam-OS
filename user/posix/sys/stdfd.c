/* Descriptors 0, 1 and 2 before the descriptor table (docs/M13-PLAN.md
 * section 3 builds the table, in fd.c, which replaces this file): what a
 * POSIX program's standard descriptors are, read, written and closed.
 *
 * Each is one of three kinds:
 *   - the null device: reads end at once, writes vanish;
 *   - the log: writes go to debug_write, as a native program's printf
 *     does when it has no SR_STDOUT; reading is EBADF;
 *   - a pipe: a channel of byte messages (a message is up to
 *     CHANNEL_MAX_BYTES; the bytes a read doesn't take wait for the next).
 * start.c sets them up (stdfd_default, without a hand-over message; or
 * stdfd_take for each descriptor a hand-over gives). Nothing else is a
 * descriptor yet: any other number is EBADF.
 *
 * No lock: the table changes only at the start and in close, and a
 * program that closes a descriptor another of its threads is using gets
 * what it gets on Linux too. A pipe's read buffer is the reading
 * thread's while it reads (two threads reading one pipe at once is the
 * program's race). */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include "stdfd.h"

#define STDFD_N 3

enum stdfd_kind { SF_CLOSED, SF_NULL, SF_LOG, SF_PIPE };

struct stdfd {
    enum stdfd_kind kind;
    uint32_t        status;     /* O_* status flags (access mode, O_NONBLOCK, O_APPEND) */
    bool            cloexec;    /* FD_CLOEXEC */
    handle_t        ch;         /* SF_PIPE: the channel end */
    uint32_t        have, at;   /* SF_PIPE: buf[at .. have) not read yet */
    uint8_t         buf[CHANNEL_MAX_BYTES];
};

static struct stdfd fds[STDFD_N];

static struct stdfd *get(long fd)
{
    return fd >= 0 && fd < STDFD_N && fds[fd].kind != SF_CLOSED ? &fds[fd] : NULL;
}

void stdfd_default(void)
{
    handle_t out = startup_handle(SR_STDOUT);
    fds[0] = (struct stdfd){ .kind = SF_NULL, .status = O_RDONLY };
    for (int fd = 1; fd <= 2; fd++) {
        fds[fd] = (struct stdfd){ .kind = SF_LOG, .status = O_WRONLY };
        if (out != HANDLE_INVALID)
            fds[fd] = (struct stdfd){ .kind = SF_PIPE, .status = O_WRONLY, .ch = out };
    }
}

status_t stdfd_take(const struct posixfds_fd *rec, const void *data, const handle_t *hs)
{
    if (rec->fd >= STDFD_N || (rec->kind != POSIXFDS_PIPE && rec->kind != POSIXFDS_DEV) ||
        (rec->kind == POSIXFDS_DEV && strcmp(data, "null"))) {
        for (unsigned i = 0; i < rec->nhandles; i++)
            jam_handle_close(hs[i]);
        return OK;   /* kept out, not refused: the rest of the hand-over still counts */
    }
    struct stdfd *f = &fds[rec->fd];
    f->kind = rec->kind == POSIXFDS_PIPE ? SF_PIPE : SF_NULL;
    f->status = rec->status;
    f->cloexec = false;
    f->have = f->at = 0;
    if (f->kind == SF_PIPE) {
        f->ch = hs[0];
        f->have = rec->data_len < sizeof(f->buf) ? rec->data_len : sizeof(f->buf);
        if (f->have)   /* what the starter read and didn't take */
            memcpy(f->buf, data, f->have);
    }
    return OK;
}

void stdfd_reset(void)
{
    for (int fd = 0; fd < STDFD_N; fd++) {
        if (fds[fd].kind == SF_PIPE)
            jam_handle_close(fds[fd].ch);
        fds[fd].kind = SF_CLOSED;
    }
}

static bool can_read(const struct stdfd *f)
{
    return (f->status & O_ACCMODE) != O_WRONLY;
}

static bool can_write(const struct stdfd *f)
{
    return (f->status & O_ACCMODE) != O_RDONLY;
}

/* Wait for sig on a pipe (unless it is non-blocking): 0, or -errno. */
static long pipe_wait(const struct stdfd *f, signals_t sig)
{
    if (f->status & O_NONBLOCK)
        return -EAGAIN;
    signals_t seen;
    status_t st = jam_object_wait_one(f->ch, sig | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
    return st == OK ? 0 : lx_err(st);
}

static long pipe_write(struct stdfd *f, const uint8_t *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        uint32_t k = n - done > CHANNEL_MAX_BYTES ? CHANNEL_MAX_BYTES : (uint32_t)(n - done);
        status_t st = jam_channel_write(f->ch, p + done, k, NULL, 0);
        if (st == ERR_SHOULD_WAIT) {   /* the reader is behind */
            long r = pipe_wait(f, SIG_WRITABLE);
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

static long log_write(const uint8_t *p, size_t n)
{
    for (size_t done = 0; done < n;) {
        size_t k = n - done > DEBUG_WRITE_MAX ? DEBUG_WRITE_MAX : n - done;
        if (jam_debug_write((const char *)p + done, k) != OK)
            return done ? (long)done : -EIO;
        done += k;
    }
    return (long)n;
}

/* Fill a pipe's buffer with its next message: 1, 0 at the end, or -errno. */
static long pipe_fill(struct stdfd *f)
{
    for (;;) {
        uint32_t got = 0, nh = 0;
        struct channel_read_args a = {
            .h = f->ch,
            .rd_cap = sizeof(f->buf),
            .rd_bytes = (uint64_t)(uintptr_t)f->buf,
            .rd_actual = (uint64_t)(uintptr_t)&got,
            .rd_hactual = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == OK) {
            f->at = 0;
            f->have = got;
            if (got)
                return 1;
            continue;   /* an empty message carries nothing */
        }
        if (st == ERR_PEER_CLOSED)
            return 0;
        if (st != ERR_SHOULD_WAIT)
            return lx_err(st);
        long r = pipe_wait(f, SIG_READABLE);
        if (r)
            return r;
    }
}

static long pipe_read(struct stdfd *f, uint8_t *p, size_t n)
{
    if (f->at == f->have) {
        long r = pipe_fill(f);
        if (r <= 0)
            return r;
    }
    size_t k = f->have - f->at < n ? f->have - f->at : n;
    memcpy(p, f->buf + f->at, k);
    f->at += (uint32_t)k;
    return (long)k;
}

long lx_read(long fd, long buf, long len, long d, long e, long f)
{
    struct stdfd *s = get(fd);
    if (!s || !can_read(s) || s->kind == SF_LOG)
        return -EBADF;
    if (len < 0)
        return -EINVAL;
    if (s->kind == SF_NULL || len == 0)
        return 0;
    return pipe_read(s, (uint8_t *)(uintptr_t)buf, (size_t)len);
}

long lx_write(long fd, long buf, long len, long d, long e, long f)
{
    struct stdfd *s = get(fd);
    if (!s || !can_write(s))
        return -EBADF;
    if (len < 0)
        return -EINVAL;
    const uint8_t *p = (const uint8_t *)(uintptr_t)buf;
    switch (s->kind) {
    case SF_LOG:  return log_write(p, (size_t)len);
    case SF_PIPE: return pipe_write(s, p, (size_t)len);
    default:      return len;
    }
}

/* readv and writev: each piece in turn, stopping at a short one. */
static long vector_io(long fd, const struct iovec *iov, long n, bool write)
{
    if (n < 0 || n > IOV_MAX)
        return -EINVAL;
    long total = 0;
    for (long i = 0; i < n; i++) {
        long r = write ? lx_write(fd, (long)iov[i].iov_base, (long)iov[i].iov_len, 0, 0, 0)
                       : lx_read(fd, (long)iov[i].iov_base, (long)iov[i].iov_len, 0, 0, 0);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((size_t)r < iov[i].iov_len)
            break;
    }
    return total;
}

long lx_readv(long fd, long iov, long n, long d, long e, long f)
{
    return vector_io(fd, (const struct iovec *)(uintptr_t)iov, n, false);
}

long lx_writev(long fd, long iov, long n, long d, long e, long f)
{
    return vector_io(fd, (const struct iovec *)(uintptr_t)iov, n, true);
}

long lx_close(long fd, long b, long c, long d, long e, long f)
{
    struct stdfd *s = get(fd);
    if (!s)
        return -EBADF;
    if (s->kind == SF_PIPE)
        jam_handle_close(s->ch);
    s->kind = SF_CLOSED;
    return 0;
}

long lx_fstat(long fd, long out, long c, long d, long e, long f)
{
    struct stdfd *s = get(fd);
    if (!s)
        return -EBADF;
    struct stat *st = (struct stat *)(uintptr_t)out;
    memset(st, 0, sizeof(*st));
    st->st_mode = (s->kind == SF_PIPE ? S_IFIFO : S_IFCHR) | 0600;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_ino = (ino_t)fd + 1;
    return 0;
}

long lx_ioctl(long fd, long req, long arg, long d, long e, long f)
{
    struct stdfd *s = get(fd);
    if (!s)
        return -EBADF;
    if (req == FIONBIO) {
        if (*(const int *)(uintptr_t)arg)
            s->status |= O_NONBLOCK;
        else
            s->status &= ~(uint32_t)O_NONBLOCK;
        return 0;
    }
    return -ENOTTY;   /* none of them is a terminal yet */
}

long lx_lseek(long fd, long off, long whence, long d, long e, long f)
{
    return get(fd) ? -ESPIPE : -EBADF;
}

long lx_fcntl(long fd, long cmd, long arg, long d, long e, long f)
{
    struct stdfd *s = get(fd);
    if (!s)
        return -EBADF;
    switch (cmd) {
    case F_GETFD:
        return s->cloexec ? FD_CLOEXEC : 0;
    case F_SETFD:
        s->cloexec = arg & FD_CLOEXEC;
        return 0;
    case F_GETFL:
        return s->status;
    case F_SETFL:
        s->status = (s->status & ~(uint32_t)(O_NONBLOCK | O_APPEND)) |
                    ((uint32_t)arg & (O_NONBLOCK | O_APPEND));
        return 0;
    }
    return -EINVAL;   /* F_DUPFD and the rest: the descriptor table's */
}
