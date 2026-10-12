/* Sockets' bytes: read and write, the send and recv family (sendto,
 * recvfrom, sendmsg, recvmsg, sendmmsg, recvmmsg; no ancillary data). The
 * model and the locks are sock.h's; the waits are sockwait.c's.
 *
 * **Streams.** A read takes what the rx ring holds (MSG_PEEK copies it
 * without taking it); none, and the peer's end is 0, a connection that
 * failed its error once (ECONNRESET, ETIMEDOUT, ...) and 0 after. A
 * blocking write puts every byte in the tx ring, waiting for room
 * (netstack takes them as the peer's window allows); one cut short by a
 * signal or a timeout says how many went. A write after shutdown(SHUT_WR),
 * or once the connection is gone, is EPIPE (calls.c raises SIGPIPE unless
 * MSG_NOSIGNAL); a socket never connected is EPIPE too, as Linux's.
 *
 * **Datagrams** are records in the rings: a send puts one (at most
 * NET_DGRAM_MAX bytes: EMSGSIZE past it) and returns; a recv takes one,
 * cut to the caller's buffer (MSG_TRUNC says its whole length). netstack
 * refuses a bad destination after the send has returned (its status
 * line counts it): the next send says so, as Linux reports an ICMP error
 * on a later call.
 *
 * **socketpair ends** are pipe.c's: send and recv go to its read and
 * write, MSG_DONTWAIT by a look at the channel first (another thread can
 * still take what the look saw), MSG_PEEK and MSG_TRUNC are EOPNOTSUPP.
 *
 * **AF_UNIX stream sockets** are svcsock.c's, ancillary data (SCM_RIGHTS)
 * and all: every call here hands them over once the flags are checked. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "kinds.h"
#include "sock.h"
#include "svcsock.h"

#define RECV_FLAGS (MSG_PEEK | MSG_DONTWAIT | MSG_WAITALL | MSG_TRUNC | MSG_NOSIGNAL |     \
                    MSG_CMSG_CLOEXEC)
#define SEND_FLAGS (MSG_DONTWAIT | MSG_NOSIGNAL | MSG_MORE | MSG_CONFIRM | MSG_EOR |        \
                    MSG_DONTROUTE)
#define MMSG_MAX   1024   /* sendmmsg's and recvmmsg's vlen: Linux's UIO_MAXIOV */

/* One transfer: a send's or a recv's arguments and its progress. */
struct xfer {
    const struct iovec *iov;
    int                 iovcnt;
    size_t              total;     /* iov's bytes (cut to FD_RW_MAX) */
    size_t              done;      /* moved so far (a blocking write, MSG_WAITALL) */
    int                 flags;     /* MSG_* */
    bool                to;        /* a datagram's address was given (send) */
    bool                from;      /* a datagram's sender is in addr, port (recv) */
    uint32_t            addr;      /* host order */
    uint16_t            port;
    size_t              dg_len;    /* recv: the datagram's whole length */
    const void         *buf;       /* send: a datagram's bytes, gathered */
    struct net_dgram   *dg;        /* recv: room for a datagram (the caller's stack) */
};

/* ---- streams ----------------------------------------------------------------------------- */

/* netstack is gone: the socket's channel is closed at its end. */
static bool gone(const struct sock *s)
{
    signals_t seen = 0;
    return jam_object_wait_one(s->ns.ch, SIG_PEER_CLOSED, 0, &seen) == OK;
}

/* A CLOSED connection's error, the first time it is asked; then 0. */
static long closed_once(struct sock *s, status_t err)
{
    int e = sock_closed_errno(err);
    if (!e || s->err_said)
        return 0;
    s->err_said = true;
    return -e;
}

/* The piece of iov holding byte `skip`, and where in it. false: past the end. */
static bool iov_at(const struct iovec *iov, int n, size_t skip, int *i, size_t *off)
{
    for (*i = 0; *i < n; (*i)++) {
        if (skip < iov[*i].iov_len) {
            *off = skip;
            return true;
        }
        skip -= iov[*i].iov_len;
    }
    return false;
}

/* Move up to want bytes between the ring and iov from byte skip on: rx
 * into iov (take), or iov into tx (!take). How many. */
static size_t stream_move(struct sock *s, const struct xfer *x, bool take)
{
    size_t want = x->total - x->done, moved = 0, off;
    int i;
    if (!iov_at(x->iov, x->iovcnt, x->done, &i, &off))
        return 0;
    for (; i < x->iovcnt && moved < want; i++, off = 0) {
        uint8_t *b = (uint8_t *)x->iov[i].iov_base + off;
        size_t len = x->iov[i].iov_len - off;
        if (len > want - moved)
            len = want - moved;
        size_t k = take ? net_read_some(&s->ns, b, len) : net_write_some(&s->ns, b, len);
        moved += k;
        if (k < len)
            break;
    }
    return moved;
}

/* MSG_PEEK: the rx ring's bytes copied into iov, none taken. */
static size_t stream_peek(struct sock *s, const struct xfer *x)
{
    struct sockring_end *e = &s->ns.r.rx;
    size_t n = sockring_ready(e);
    if (n > x->total)
        n = x->total;
    uint32_t off = (uint32_t)(e->count & (e->size - 1));
    size_t first = n < e->size - off ? n : e->size - off;
    fd_iov_scatter(x->iov, x->iovcnt, 0, e->data + off, first);
    fd_iov_scatter(x->iov, x->iovcnt, first, e->data, n - first);
    return n;
}

static long stream_read_locked(struct sock *s, struct xfer *x)
{
    if (sock_state(s) != SK_STREAM)
        return -ENOTCONN;   /* handed away meanwhile */
    if (s->rd_shut || x->done == x->total)
        return 0;
    size_t n = x->flags & MSG_PEEK ? stream_peek(s, x) : stream_move(s, x, true);
    if (n)
        return (long)n;
    if (sockring_at_end(&s->ns.r.rx))
        return 0;   /* the peer's FIN, after its last byte */
    uint32_t state;
    status_t err;
    net_tcp_status(&s->ns, &state, &err);
    if (state == SOCKRING_STATE_CLOSED)
        return closed_once(s, err);
    if (gone(s))
        return closed_once(s, ERR_PEER_CLOSED);
    return -EAGAIN;   /* CONNECTING, or nothing yet */
}

static long stream_read_try(struct sock *s, void *arg)
{
    lock_take(&s->rx_lock);
    long r = stream_read_locked(s, arg);
    lock_give(&s->rx_lock);
    return r;
}

static long stream_write_locked(struct sock *s, struct xfer *x)
{
    if (sock_state(s) != SK_STREAM)
        return -ENOTCONN;   /* handed away meanwhile */
    if (s->wr_shut)
        return -EPIPE;
    uint32_t state;
    status_t err;
    net_tcp_status(&s->ns, &state, &err);
    if (state == SOCKRING_STATE_CLOSED) {
        long r = closed_once(s, err);
        return r ? r : -EPIPE;
    }
    if (state == SOCKRING_STATE_CONNECTING)
        return -EAGAIN;
    size_t n = stream_move(s, x, false);
    if (n)
        return (long)n;
    return gone(s) ? -EPIPE : -EAGAIN;
}

static long stream_write_try(struct sock *s, void *arg)
{
    lock_take(&s->tx_lock);
    long r = stream_write_locked(s, arg);
    lock_give(&s->tx_lock);
    return r;
}

/* ---- datagrams --------------------------------------------------------------------------- */

/* A refused datagram's reason (the status line's error) as an errno. */
static long dgram_errno(status_t err)
{
    switch (err) {
    case ERR_BAD_STATE:    return -ENETUNREACH;   /* no address, no route, the link down */
    case ERR_NOT_FOUND:    return -ECONNREFUSED;
    case ERR_INVALID_ARGS: return -EINVAL;        /* an address a program can't send to */
    }
    return err < 0 ? lx_err(err) : -EIO;
}

static long dgram_recv_locked(struct sock *s, struct xfer *x)
{
    if (sock_state(s) != SK_DGRAM)
        return -ENOTCONN;
    if (s->rd_shut)
        return 0;
    struct net_dgram *g = s->kept;
    if (!g) {
        g = x->flags & MSG_PEEK ? malloc(sizeof(*g)) : x->dg;
        if (!g)
            return -ENOMEM;
        status_t st = net_sock_take(&s->ns, g);
        if (st != OK && g != x->dg)
            free(g);
        if (st == ERR_SHOULD_WAIT)
            return -EAGAIN;
        if (st != OK)
            return st == ERR_PEER_CLOSED ? -ENETDOWN : lx_err(st);
        if (g != x->dg)
            s->kept = g;
    }
    size_t n = g->len < x->total ? g->len : x->total;
    fd_iov_scatter(x->iov, x->iovcnt, 0, g->data, n);
    x->from = true;
    x->addr = g->addr;
    x->port = g->port;
    x->dg_len = g->len;
    if (!(x->flags & MSG_PEEK) && s->kept) {
        free(s->kept);
        s->kept = NULL;
    }
    return (long)n;
}

static long dgram_recv_try(struct sock *s, void *arg)
{
    lock_take(&s->rx_lock);
    long r = dgram_recv_locked(s, arg);
    lock_give(&s->rx_lock);
    return r;
}

static long dgram_send_locked(struct sock *s, struct xfer *x)
{
    if (sock_state(s) != SK_DGRAM)
        return -ENOTCONN;
    if (s->wr_shut)
        return -EPIPE;
    struct sockring_status ss;
    sockring_status_get(&s->ns.r, &ss);
    if (ss.tx_refused > s->refused) {   /* an earlier send was refused: say so now */
        s->refused = ss.tx_refused;
        return dgram_errno(ss.error);
    }
    status_t st = net_sendto_async(&s->ns, x->to ? x->addr : 0, x->to ? x->port : 0, x->buf,
                                   x->total);
    if (st == OK)
        return (long)x->total;
    if (st == ERR_SHOULD_WAIT)
        return gone(s) ? -ENETDOWN : -EAGAIN;
    return lx_err(st);
}

static long dgram_send_try(struct sock *s, void *arg)
{
    lock_take(&s->tx_lock);
    long r = dgram_send_locked(s, arg);
    lock_give(&s->tx_lock);
    return r;
}

/* A datagram socket at netstack, opened now if it wasn't. */
static long dgram_ready(struct fd_desc *d, struct sock *s)
{
    if (sock_state(s) == SK_DGRAM)
        return 0;
    lock_take(&d->lock);
    long r = sock_dgram_open_locked(s);
    lock_give(&d->lock);
    return r == -EINVAL ? -ENOTCONN : r;   /* handed away */
}

/* ---- recv and send ----------------------------------------------------------------------- */

static bool nonblocking(struct fd_desc *d, int flags)
{
    return (fd_status(d) & O_NONBLOCK) || (flags & MSG_DONTWAIT);
}

static long sock_recv(struct fd_desc *d, struct sock *s, struct xfer *x)
{
    bool nb = nonblocking(d, x->flags);
    uint64_t timeout = s->opts.rcvtimeo, deadline = sock_deadline(timeout);
    if (s->type == SOCK_DGRAM) {
        struct net_dgram g;
        long r = dgram_ready(d, s);
        x->dg = &g;
        if (!r)
            r = sock_wait(s, SIDE_RX, dgram_recv_try, x, nb, deadline, timeout);
        x->dg = NULL;   /* g ends here */
        return r;
    }
    if (sock_state(s) != SK_STREAM)
        return -ENOTCONN;
    if (x->flags & MSG_TRUNC)
        return -EOPNOTSUPP;   /* Linux's "discard the bytes" for TCP: not built */
    if (!(x->flags & MSG_WAITALL) || (x->flags & MSG_PEEK) || nb)
        return sock_wait(s, SIDE_RX, stream_read_try, x, nb, deadline, timeout);
    for (;;) {   /* MSG_WAITALL: each turn moves bytes, or ends */
        long r = sock_wait(s, SIDE_RX, stream_read_try, x, false, deadline, timeout);
        if (r <= 0)
            return x->done ? (long)x->done : r;
        x->done += (size_t)r;
        if (x->done == x->total)
            return (long)x->done;
    }
}

static long dgram_send(struct fd_desc *d, struct sock *s, struct xfer *x)
{
    uint8_t buf[NET_DGRAM_MAX];
    if (x->total > NET_DGRAM_MAX)
        return -EMSGSIZE;   /* one frame, never fragmented */
    if (x->to && !x->port)
        return -EINVAL;
    if (!x->to && !s->connected)
        return -EDESTADDRREQ;
    long r = dgram_ready(d, s);
    if (r)
        return r;
    fd_iov_gather(x->iov, x->iovcnt, 0, buf, x->total);
    x->buf = buf;
    uint64_t timeout = s->opts.sndtimeo;
    return sock_wait(s, SIDE_TX, dgram_send_try, x, nonblocking(d, x->flags),
                     sock_deadline(timeout), timeout);
}

static long sock_send(struct fd_desc *d, struct sock *s, struct xfer *x)
{
    if (s->type == SOCK_DGRAM)
        return dgram_send(d, s, x);
    uint32_t st = sock_state(s);
    if (st == SK_GONE)
        return -ENOTCONN;
    if (st != SK_STREAM)
        return -EPIPE;   /* never connected: Linux's answer (and SIGPIPE) */
    bool nb = nonblocking(d, x->flags);
    uint64_t timeout = s->opts.sndtimeo, deadline = sock_deadline(timeout);
    if (nb || !x->total)
        return sock_wait(s, SIDE_TX, stream_write_try, x, nb, deadline, timeout);
    while (x->done < x->total) {   /* each turn moves bytes, or ends */
        long r = sock_wait(s, SIDE_TX, stream_write_try, x, false, deadline, timeout);
        if (r < 0)
            return x->done ? (long)x->done : r;
        x->done += (size_t)r;
    }
    return (long)x->done;
}

long sock_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    long n = fd_iov_total(iov, iovcnt);
    struct xfer x = { .iov = iov, .iovcnt = iovcnt, .total = n > 0 ? (size_t)n : 0 };
    return n < 0 ? n : sock_recv(d, d->priv, &x);
}

long sock_write(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    long n = fd_iov_total(iov, iovcnt);
    struct xfer x = { .iov = iov, .iovcnt = iovcnt, .total = n > 0 ? (size_t)n : 0 };
    return n < 0 ? n : sock_send(d, d->priv, &x);
}

/* ---- socketpair ends --------------------------------------------------------------------- */

/* Would a read (or a write) of a socketpair end go on now? A look at its
 * channel, and at the bytes pipe.c keeps. */
static bool sockp_ready_now(struct fd_desc *d, bool write)
{
    if (!write && d->ops->ready && d->ops->ready(d, NETWAIT_READ))
        return true;
    struct fd_wait w;
    memset(&w, 0, sizeof(w));
    if (d->ops->arm(d, write ? NETWAIT_WRITE : NETWAIT_READ, &w) || w.kind != FD_WAIT_HANDLE)
        return true;   /* the call itself will say */
    signals_t seen = 0;
    signals_t want = (write ? w.h.write : w.h.read) | w.h.hup;
    return jam_object_wait_one(w.h.h, want, 0, &seen) == OK;
}

static long sockp_recv(struct fd_desc *d, struct xfer *x)
{
    if (x->flags & (MSG_PEEK | MSG_TRUNC))
        return -EOPNOTSUPP;   /* pipe.c's bytes can't be looked at without taking them */
    bool nb = nonblocking(d, x->flags);
    if (nb && !(fd_status(d) & O_NONBLOCK) && !sockp_ready_now(d, false))
        return -EAGAIN;
    if (!(x->flags & MSG_WAITALL) || nb || x->iovcnt != 1)
        return d->ops->read(d, x->iov, x->iovcnt, FD_AT_OFFSET);
    for (;;) {   /* MSG_WAITALL, one buffer: each turn reads, or ends */
        struct iovec v = { (uint8_t *)x->iov[0].iov_base + x->done, x->total - x->done };
        long r = d->ops->read(d, &v, 1, FD_AT_OFFSET);
        if (r <= 0)
            return x->done ? (long)x->done : r;
        x->done += (size_t)r;
        if (x->done == x->total)
            return (long)x->done;
    }
}

static long sockp_send(struct fd_desc *d, struct xfer *x)
{
    if (nonblocking(d, x->flags) && !(fd_status(d) & O_NONBLOCK) && !sockp_ready_now(d, true))
        return -EAGAIN;
    return d->ops->write(d, x->iov, x->iovcnt, FD_AT_OFFSET);
}

/* ---- the calls --------------------------------------------------------------------------- */

static long recv_flags_check(long flags)
{
    if (flags & MSG_OOB)
        return -EOPNOTSUPP;   /* no out-of-band data */
    if (flags & MSG_ERRQUEUE)
        return -EAGAIN;       /* no error queue: never anything in it */
    return flags & ~(long)RECV_FLAGS ? -EINVAL : 0;
}

static long send_flags_check(long flags)
{
    if (flags & (MSG_OOB | MSG_FASTOPEN))
        return -EOPNOTSUPP;
    return flags & ~(long)SEND_FLAGS ? -EINVAL : 0;
}

/* recv on descriptor fd: a socket's, or a socketpair end's. */
static long recv_on(long fd, struct xfer *x)
{
    long r = recv_flags_check(x->flags);
    struct fd_desc *d = r ? NULL : fd_get(fd);
    if (r || !d)
        return r ? r : -EBADF;
    if (sock_of(d))
        r = sock_recv(d, d->priv, x);
    else if (d->ops == &sockp_ops)
        r = sockp_recv(d, x);
    else if (d->ops == &usock_ops)
        r = usock_recvmsg(d, &(struct msghdr){ .msg_iov = (struct iovec *)x->iov,
                                               .msg_iovlen = x->iovcnt }, x->flags);
    else
        r = -ENOTSOCK;
    fd_put(d);
    return r;
}

static long send_on(long fd, struct xfer *x)
{
    long r = send_flags_check(x->flags);
    struct fd_desc *d = r ? NULL : fd_get(fd);
    if (r || !d)
        return r ? r : -EBADF;
    if (sock_of(d))
        r = sock_send(d, d->priv, x);
    else if (d->ops == &sockp_ops)
        r = sockp_send(d, x);   /* an address given is ignored: Linux's EISCONN not worth it */
    else if (d->ops == &usock_ops)
        r = usock_sendmsg(d, &(struct msghdr){ .msg_iov = (struct iovec *)x->iov,
                                               .msg_iovlen = x->iovcnt }, x->flags);
    else
        r = -ENOTSOCK;
    fd_put(d);
    return r;
}

/* One buffer as an iovec, its length cut to FD_RW_MAX. */
static struct iovec one(long buf, long len)
{
    size_t n = (size_t)len > FD_RW_MAX ? FD_RW_MAX : (size_t)len;
    return (struct iovec){ (void *)(uintptr_t)buf, n };
}

long lx_recvfrom(long fd, long buf, long len, long flags, long addr, long lenp)
{
    struct iovec v = one(buf, len);
    struct xfer x = { .iov = &v, .iovcnt = 1, .total = v.iov_len, .flags = (int)flags };
    long r = recv_on(fd, &x);
    if (r >= 0 && x.from)
        (void)sock_addr_out(addr, lenp, x.addr, x.port);   /* a bad length: nothing written */
    else if (r >= 0 && lenp)
        *(socklen_t *)(uintptr_t)lenp = 0;   /* a stream has no sender's address */
    return r >= 0 && x.from && (flags & MSG_TRUNC) ? (long)x.dg_len : r;
}

long lx_sendto(long fd, long buf, long len, long flags, long addr, long alen)
{
    struct iovec v = one(buf, len);
    struct xfer x = { .iov = &v, .iovcnt = 1, .total = v.iov_len, .flags = (int)flags };
    if (addr) {
        long r = sock_addr_in(addr, alen, &x.addr, &x.port);
        if (r)
            return r;
        x.to = true;
    }
    return send_on(fd, &x);
}

static long recvmsg_one(long fd, struct msghdr *m, long flags)
{
    struct fd_desc *u = usock_get(fd);
    if (u) {   /* an AF_UNIX stream socket: svcsock.c's, SCM_RIGHTS and all */
        long r = recv_flags_check(flags);
        r = r ? r : usock_recvmsg(u, m, (int)flags);
        fd_put(u);
        return r;
    }
    long n = fd_iov_total(m->msg_iov, (int)(m->msg_iovlen > INT_MAX ? -1 : m->msg_iovlen));
    if (n < 0)
        return n;
    struct xfer x = { .iov = m->msg_iov, .iovcnt = (int)m->msg_iovlen, .total = (size_t)n,
                      .flags = (int)flags };
    long r = recv_on(fd, &x);
    if (r < 0)
        return r;
    m->msg_controllen = 0;   /* no ancillary data, ever */
    m->msg_flags = x.from && x.dg_len > x.total ? MSG_TRUNC : 0;
    if (m->msg_name && x.from)
        (void)sock_addr_out((long)(uintptr_t)m->msg_name, (long)(uintptr_t)&m->msg_namelen, x.addr,
                            x.port);
    else
        m->msg_namelen = 0;
    return x.from && (flags & MSG_TRUNC) ? (long)x.dg_len : r;
}

static long sendmsg_one(long fd, const struct msghdr *m, long flags)
{
    struct fd_desc *u = usock_get(fd);
    if (u) {   /* an AF_UNIX stream socket: svcsock.c's, SCM_RIGHTS and all */
        long r = send_flags_check(flags);
        r = r ? r : usock_sendmsg(u, m, (int)flags);
        fd_put(u);
        return r;
    }
    if (m->msg_controllen)
        return -EINVAL;   /* no ancillary data here (SCM_RIGHTS on socketpairs): not built */
    long n = fd_iov_total(m->msg_iov, (int)(m->msg_iovlen > INT_MAX ? -1 : m->msg_iovlen));
    if (n < 0)
        return n;
    struct xfer x = { .iov = m->msg_iov, .iovcnt = (int)m->msg_iovlen, .total = (size_t)n,
                      .flags = (int)flags };
    if (m->msg_name) {
        long r = sock_addr_in((long)(uintptr_t)m->msg_name, m->msg_namelen, &x.addr, &x.port);
        if (r)
            return r;
        x.to = true;
    }
    return send_on(fd, &x);
}

long lx_recvmsg(long fd, long msg, long flags, long d, long e, long f)
{
    return msg ? recvmsg_one(fd, (struct msghdr *)(uintptr_t)msg, flags) : -EFAULT;
}

long lx_sendmsg(long fd, long msg, long flags, long d, long e, long f)
{
    return msg ? sendmsg_one(fd, (const struct msghdr *)(uintptr_t)msg, flags) : -EFAULT;
}

/* sendmmsg: each message in turn; the count sent, or the first's error. */
long lx_sendmmsg(long fd, long vec, long vlen, long flags, long e, long f)
{
    struct mmsghdr *mm = (struct mmsghdr *)(uintptr_t)vec;
    if (vlen > MMSG_MAX)
        vlen = MMSG_MAX;
    for (long i = 0; i < vlen; i++) {
        long r = sendmsg_one(fd, &mm[i].msg_hdr, flags);
        if (r < 0)
            return i ? i : r;
        mm[i].msg_len = (unsigned)r;
    }
    return vlen;
}

/* recvmmsg: each message in turn, the ones after the first without
 * waiting with MSG_WAITFORONE; the count, or the first's error. Its
 * timeout is not built (Linux's checks it only between messages): EINVAL. */
long lx_recvmmsg(long fd, long vec, long vlen, long flags, long timeout, long f)
{
    struct mmsghdr *mm = (struct mmsghdr *)(uintptr_t)vec;
    if (timeout)
        return -EINVAL;
    if (vlen > MMSG_MAX)
        vlen = MMSG_MAX;
    long fl = flags & ~(long)MSG_WAITFORONE;
    for (long i = 0; i < vlen; i++) {
        long r = recvmsg_one(fd, &mm[i].msg_hdr, fl);
        if (r < 0)
            return i ? i : r;
        mm[i].msg_len = (unsigned)r;
        if (flags & MSG_WAITFORONE)
            fl |= MSG_DONTWAIT;
    }
    return vlen;
}
