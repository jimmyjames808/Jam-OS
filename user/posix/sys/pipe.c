/* Pipes and socketpair (docs/M13-PLAN.md section 4): a pipe is a channel
 * of byte messages, closed at the end.
 *
 * pipe2 makes a channel pair and narrows one end to reading and the other
 * to writing (handle_replace), so a child handed the read end can never
 * write into it. socketpair is the same channel with both ends able to
 * read and write: two pipes' worth.
 *
 * A write sends a message per PIPE_SIZE (64 KiB), so any write up to 64
 * KiB is atomic (PIPE_BUF, 4096, is far inside it) and a bigger one is
 * split. A full queue (CHANNEL_MAX_QUEUED messages) is EAGAIN with
 * O_NONBLOCK, else a wait for SIG_WRITABLE. The reader gone is EPIPE (and
 * SIGPIPE, once signals exist: not built yet). Who pays for the queue:
 * the writer's job until it closes, then the reader's (the kernel's rule).
 *
 * A read takes the next message. One that fits the caller's buffer goes
 * straight into it; a bigger one is read into the description's leftover
 * buffer (made on the first such message, PIPE_SIZE bytes), and what the
 * caller didn't take is read first next time. End of file is the peer
 * gone with nothing queued: the kernel end stays open while any handle to
 * it does, so with the write end in several processes the end comes after
 * the last writer closes. Handles someone put in a message (no pipe of
 * ours does) are closed, never kept.
 *
 * The leftover is the description's, under its lock (fd.h); a reader that
 * has to wait gives the lock up first, so a second reader of the same
 * description is never stuck behind a sleeping one. Writes take no lock:
 * a write of up to 64 KiB is one message whoever else writes, and POSIX
 * promises nothing for a bigger one.
 *
 * poll (section 7): a pipe end is ready to read when it has a leftover or
 * its channel is SIG_READABLE or SIG_PEER_CLOSED, to write when its
 * channel is SIG_WRITABLE. */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include "kinds.h"

#define PIPE_SIZE    CHANNEL_MAX_BYTES   /* the most one message (and one atomic write) holds */
#define PIPE_ATOMIC  4096                /* PIPE_BUF, as musl's <limits.h> */
#define READ_RIGHTS  (RIGHT_READ | RIGHT_WAIT | RIGHT_TRANSFER | RIGHT_DUPLICATE)
#define WRITE_RIGHTS (RIGHT_WRITE | RIGHT_WAIT | RIGHT_TRANSFER | RIGHT_DUPLICATE)

_Static_assert(PIPE_ATOMIC <= PIPE_SIZE, "an atomic write is one message");

struct pipe_end {
    handle_t ch;        /* the channel end */
    uint8_t *buf;       /* PIPE_SIZE bytes once a message didn't fit a read, else NULL */
    uint32_t at, have;  /* buf[at .. have): read from the channel, not by the program;
                         * the description's lock */
};

static struct pipe_end *end_of(struct fd_desc *d)
{
    return d->priv;
}

/* A pipe end on ch (consumed, whatever happens) with len leftover bytes. */
static struct pipe_end *end_new(handle_t ch, const void *leftover, size_t len)
{
    struct pipe_end *p = len <= PIPE_SIZE ? calloc(1, sizeof(*p)) : NULL;
    if (p && len && !(p->buf = malloc(PIPE_SIZE))) {
        free(p);
        p = NULL;
    }
    if (!p) {
        jam_handle_close(ch);
        return NULL;
    }
    p->ch = ch;
    if (len)
        memcpy(p->buf, leftover, len);
    p->have = (uint32_t)len;
    return p;
}

/* Wait for sig (or the peer's end) on the channel: 0, or -errno
 * (-EINTR when a signal cut the wait short). */
static long wait_for(handle_t ch, signals_t sig)
{
    signals_t seen;
    status_t st = jam_object_wait_one(ch, sig | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
    return st == OK ? 0 : lx_err(st);
}

/* channel_read of the next message into buf (cap bytes): its size in *got.
 * Handles in it are closed. */
static status_t read_msg(handle_t ch, void *buf, uint32_t cap, uint32_t *got)
{
    handle_t hs[CHANNEL_MAX_HANDLES];
    uint32_t nh = 0;
    *got = 0;
    struct channel_read_args a = {
        .h = ch, .rd_cap = cap, .rd_bytes = (uint64_t)(uintptr_t)buf,
        .rd_handles = (uint64_t)(uintptr_t)hs, .rd_hcap = CHANNEL_MAX_HANDLES,
        .rd_actual = (uint64_t)(uintptr_t)got, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    status_t st = jam_channel_read(&a);
    for (uint32_t i = 0; st == OK && i < nh; i++)
        jam_handle_close(hs[i]);
    return st;
}

static long take_leftover(struct pipe_end *p, const struct iovec *iov, int iovcnt, size_t n)
{
    size_t k = p->have - p->at < n ? p->have - p->at : n;
    fd_iov_scatter(iov, iovcnt, 0, p->buf + p->at, k);
    p->at += (uint32_t)k;
    return (long)k;
}

/* The next message: straight into a single buffer it fits, else through
 * the leftover. Bytes, 0 at the end, -EAGAIN if none is queued, or
 * -errno. The description's lock held. */
static long next_message_locked(struct pipe_end *p, const struct iovec *iov, int iovcnt,
                                size_t n)
{
    for (;;) {
        uint32_t got;
        status_t st = ERR_BUFFER_TOO_SMALL;
        if (iovcnt == 1)
            st = read_msg(p->ch, iov[0].iov_base, n < PIPE_SIZE ? (uint32_t)n : PIPE_SIZE, &got);
        if (st == ERR_BUFFER_TOO_SMALL) {
            if (!p->buf && !(p->buf = malloc(PIPE_SIZE)))
                return -ENOMEM;
            st = read_msg(p->ch, p->buf, PIPE_SIZE, &got);
            if (st == OK && got) {
                p->at = 0;
                p->have = got;
                return take_leftover(p, iov, iovcnt, n);
            }
        }
        if (st == OK && got)
            return got;
        if (st == OK)
            continue;   /* an empty message carries nothing */
        if (st == ERR_PEER_CLOSED)
            return 0;
        return st == ERR_SHOULD_WAIT ? -EAGAIN : lx_err(st);
    }
}

static long pipe_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    long n = fd_iov_total(iov, iovcnt);
    if (n <= 0)
        return n;
    struct pipe_end *p = end_of(d);
    lock_take(&d->lock);
    for (;;) {
        long r = p->at < p->have ? take_leftover(p, iov, iovcnt, (size_t)n)
                                 : next_message_locked(p, iov, iovcnt, (size_t)n);
        if (r != -EAGAIN || (fd_status(d) & O_NONBLOCK)) {
            lock_give(&d->lock);
            return r;
        }
        lock_give(&d->lock);   /* never asleep with it: see the file's header */
        r = wait_for(p->ch, SIG_READABLE);
        if (r)
            return r;
        lock_take(&d->lock);
    }
}

/* Send len bytes from b as one message, waiting for room unless
 * nonblock: 0, or -errno. */
static long send_one(struct fd_desc *d, const void *b, uint32_t len)
{
    struct pipe_end *p = end_of(d);
    for (;;) {
        status_t st = jam_channel_write(p->ch, b, len, NULL, 0);
        if (st == OK)
            return 0;
        if (st != ERR_SHOULD_WAIT)   /* the reader gone: EPIPE */
            return st == ERR_PEER_CLOSED ? -EPIPE : lx_err(st);
        if (fd_status(d) & O_NONBLOCK)
            return -EAGAIN;
        long r = wait_for(p->ch, SIG_WRITABLE);   /* the reader is behind */
        if (r)
            return r;
    }
}

/* A write in messages of PIPE_SIZE at most: straight from a single
 * buffer, else each message gathered into a buffer of its own first. */
static long pipe_write(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    long n = fd_iov_total(iov, iovcnt);
    if (n <= 0)
        return n;
    uint8_t *gather = NULL;
    if (iovcnt > 1 && !(gather = malloc(n < PIPE_SIZE ? (size_t)n : PIPE_SIZE)))
        return -ENOMEM;
    size_t done = 0;
    long r = 0;
    while (!r && done < (size_t)n) {
        uint32_t len = (size_t)n - done > PIPE_SIZE ? PIPE_SIZE : (uint32_t)((size_t)n - done);
        const uint8_t *b = (const uint8_t *)iov[0].iov_base + done;
        if (gather) {
            fd_iov_gather(iov, iovcnt, done, gather, len);
            b = gather;
        }
        r = send_one(d, b, len);
        if (!r)
            done += len;
    }
    free(gather);
    return done ? (long)done : r;
}

/* The pipe's id for st_ino: the read end's koid, the same from both ends
 * while both are open. */
static uint64_t pipe_ino(handle_t ch)
{
    struct handle_info hi;
    if (jam_handle_info(ch, &hi) != OK)
        return 0;
    bool reader = !(hi.rights & RIGHT_WRITE);
    return reader || !hi.related_koid ? hi.koid : hi.related_koid;
}

static void stat_common(struct fd_desc *d, struct stat *st, mode_t mode)
{
    st->st_mode = mode;
    st->st_nlink = 1;
    st->st_uid = st->st_gid = 1000;   /* the one user */
    st->st_blksize = PIPE_ATOMIC;
    st->st_ino = (ino_t)pipe_ino(end_of(d)->ch);
}

static long pipe_stat(struct fd_desc *d, struct stat *st)
{
    stat_common(d, st, S_IFIFO | 0600);
    return 0;
}

static long sockp_stat(struct fd_desc *d, struct stat *st)
{
    stat_common(d, st, S_IFSOCK | 0777);
    return 0;
}

/* FIONREAD: the bytes a read would get at once: the leftover, or the next
 * message (read into the leftover to be counted). */
static long pipe_ioctl(struct fd_desc *d, unsigned long req, long arg)
{
    if (req != FIONREAD)
        return -ENOTTY;
    struct pipe_end *p = end_of(d);
    long r = 0;
    lock_take(&d->lock);
    if (p->at == p->have && !p->buf && !(p->buf = malloc(PIPE_SIZE)))
        r = -ENOMEM;
    uint32_t got;
    if (!r && p->at == p->have && read_msg(p->ch, p->buf, PIPE_SIZE, &got) == OK) {
        p->at = 0;
        p->have = got;
    }
    if (!r)
        *(int *)(uintptr_t)arg = (int)(p->have - p->at);
    lock_give(&d->lock);
    return r;
}

static void pipe_close(struct fd_desc *d)
{
    struct pipe_end *p = end_of(d);
    jam_handle_close(p->ch);
    free(p->buf);
    free(p);
}

/* A child gets a duplicate of the end, and the leftover moves to it: the
 * bytes are the description's, and the child is the next reader in the
 * usual case (a program handing its input on and waiting for it). A
 * leftover bigger than a hand-over carries refuses the spawn rather than
 * lose or reorder bytes. */
static long pipe_give(struct fd_desc *d, struct fd_given *out)
{
    struct pipe_end *p = end_of(d);
    lock_take(&d->lock);
    uint32_t left = p->have - p->at;
    long r = left > POSIXFDS_DATA_MAX ? -ENOBUFS : 0;
    void *data = r ? NULL : kind_data(p->buf + p->at, left, &r);
    status_t st = r ? OK : jam_handle_duplicate(p->ch, RIGHT_SAME, &out->handles[0]);
    if (!r && st != OK) {
        free(data);
        r = lx_err(st);
    }
    if (!r) {
        out->kind = POSIXFDS_PIPE;
        out->status = fd_status(d);
        out->offset = 0;
        out->nhandles = 1;
        out->data = data;
        out->data_len = left;
        p->at = p->have = 0;
    }
    lock_give(&d->lock);
    return r;
}

const struct fd_ops pipe_ops = {
    .name = "pipe",
    .read = pipe_read,
    .write = pipe_write,
    .stat = pipe_stat,
    .ioctl = pipe_ioctl,
    /* .ready, .arm: poll's (the header says what is ready when) */
    .give = pipe_give,
    .close = pipe_close,
};

const struct fd_ops sockp_ops = {
    .name = "socketpair",
    .read = pipe_read,
    .write = pipe_write,
    .stat = sockp_stat,
    .ioctl = pipe_ioctl,
    /* .ready, .arm: as pipe_ops' */
    .give = pipe_give,
    .close = pipe_close,
};

struct fd_desc *pipe_desc_new(handle_t ch, uint32_t status)
{
    struct pipe_end *p = end_new(ch, NULL, 0);
    struct fd_desc *d = p ? fd_new(&pipe_ops, status, p) : NULL;
    if (p && !d)
        pipe_close(&(struct fd_desc){ .priv = p });
    return d;
}

/* A socketpair end reads and writes; a pipe end does one of the two. */
long pipe_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                struct fd_desc **out)
{
    struct pipe_end *p = end_new(hs[0], data, rec->data_len);
    if (!p)
        return -ENOMEM;
    bool both = (rec->status & O_ACCMODE) == O_RDWR;
    *out = fd_new(both ? &sockp_ops : &pipe_ops, rec->status, p);
    if (!*out)
        pipe_close(&(struct fd_desc){ .priv = p });
    return *out ? 0 : -ENOMEM;
}

/* Two descriptors on a new channel pair, end i narrowed to rights[i]
 * (RIGHT_SAME: as made) with status[i], into fds. 0 or -errno. */
static long make_pair(const struct fd_ops *ops, const rights_t rights[2],
                      const uint32_t status[2], bool cloexec, int *fds)
{
    handle_t h[2];
    status_t st = jam_channel_create(&h[0], &h[1]);
    if (st != OK)
        return lx_err(st);
    for (int i = 0; i < 2 && st == OK; i++)
        if (rights[i] != RIGHT_SAME && (st = jam_handle_replace(h[i], rights[i], &h[i])) != OK)
            jam_handle_close(h[!i]);   /* the replace closed h[i] already */
    if (st != OK)
        return lx_err(st);
    long fd[2] = { fd_alloc(0), -1 };
    fd[1] = fd[0] < 0 ? fd[0] : fd_alloc(0);
    struct pipe_end *p[2] = { end_new(h[0], NULL, 0), end_new(h[1], NULL, 0) };
    struct fd_desc *d[2] = { p[0] ? fd_new(ops, status[0], p[0]) : NULL,
                             p[1] ? fd_new(ops, status[1], p[1]) : NULL };
    long r = fd[1] < 0 ? fd[1] : d[0] && d[1] ? 0 : -ENOMEM;
    for (int i = 0; i < 2; i++) {
        if (!r) {
            fd_set(fd[i], d[i], cloexec);
            fds[i] = (int)fd[i];
            continue;
        }
        if (fd[i] >= 0)
            fd_free(fd[i]);
        if (d[i])
            fd_put(d[i]);
        else if (p[i])
            pipe_close(&(struct fd_desc){ .priv = p[i] });
    }
    return r;
}

long lx_pipe2(long fds, long flags, long c, long d, long e, long f)
{
    if (flags & ~(long)(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;   /* O_DIRECT (packet mode) and the rest: not ours */
    const rights_t rights[2] = { READ_RIGHTS, WRITE_RIGHTS };
    uint32_t nb = flags & O_NONBLOCK ? O_NONBLOCK : 0;
    const uint32_t status[2] = { O_RDONLY | nb, O_WRONLY | nb };
    return make_pair(&pipe_ops, rights, status, flags & O_CLOEXEC, (int *)(uintptr_t)fds);
}

long lx_pipe(long fds, long b, long c, long d, long e, long f)
{
    return lx_pipe2(fds, 0, 0, 0, 0, 0);
}

long lx_socketpair(long domain, long type, long proto, long fds, long e, long f)
{
    if (domain != AF_UNIX)
        return -EAFNOSUPPORT;
    if ((type & ~(long)(SOCK_CLOEXEC | SOCK_NONBLOCK)) != SOCK_STREAM)
        return -EOPNOTSUPP;   /* datagrams and SOCK_SEQPACKET: not built */
    if (proto != 0)
        return -EPROTONOSUPPORT;
    const rights_t rights[2] = { RIGHT_SAME, RIGHT_SAME };
    uint32_t nb = type & SOCK_NONBLOCK ? O_NONBLOCK : 0;
    const uint32_t status[2] = { O_RDWR | nb, O_RDWR | nb };
    return make_pair(&sockp_ops, rights, status, type & SOCK_CLOEXEC, (int *)(uintptr_t)fds);
}
