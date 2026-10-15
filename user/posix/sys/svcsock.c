/* AF_UNIX stream sockets: the descriptor kind, the table of service
 * sockets a connect looks a path up in, and the calls that reach them
 * (socket, connect, and the bytes, for sockio.c). svcsock.h has the model;
 * the framings are their own files (wlsock.c). */
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "kinds.h"
#include "sock.h"
#include "svcsock.h"

/* A socket's state (struct usock's), only ever moving forward but for a
 * failed connect or bind, which goes back to US_NEW. */
enum usock_state {
    US_NEW = 0,        /* socket(): not connected */
    US_CONNECTING,     /* a connect is opening the service (no lock held) */
    US_CONNECTED,      /* f and conn are set, for good */
    US_BINDING,        /* a bind is asking the broker (no lock held) */
    US_BOUND,          /* l is set, for good */
    US_LISTENING,      /* and listen has been called */
};

/* A service socket: the path a program connects to, the service behind
 * it, and how the stream is framed on the service's channel. */
struct svcsock_row {
    const char *path;                       /* a sockaddr_un's sun_path */
    const char *svc;                        /* svc_open's name */
    const struct svcsock_framing *framing;
};

static const struct svcsock_row rows[] = {
    { "/svc/wayland", SVC_WAYLAND, &wlsock_framing },   /* the compositor: libwayland's */
};

/* An AF_UNIX stream socket: a description's priv. */
struct usock {
    uint32_t state;                         /* enum usock_state; d->lock, read atomically */
    const struct svcsock_framing *f;        /* US_CONNECTED: the row's framing */
    void *conn;                             /* US_CONNECTED: the framing's state */
    struct ulisten *l;                      /* US_BOUND, US_LISTENING: the listener */
};

static uint32_t state_of(const struct usock *u)
{
    return __atomic_load_n(&u->state, __ATOMIC_ACQUIRE);
}

/* d's connection, or NULL before connect has made one; *f its framing. */
static void *conn_of(struct fd_desc *d, const struct svcsock_framing **f)
{
    struct usock *u = d->priv;
    if (state_of(u) != US_CONNECTED)
        return NULL;
    *f = u->f;
    return u->conn;
}

/* ---- the bytes ---------------------------------------------------------------------------- */

long usock_sendmsg(struct fd_desc *d, const struct msghdr *m, int flags)
{
    const struct svcsock_framing *f;
    void *c = conn_of(d, &f);
    if (!c)
        return -ENOTCONN;
    if (m->msg_name && m->msg_namelen)
        return -EISCONN;   /* a connected stream socket takes no address (Linux's answer) */
    return f->sendmsg(d, c, m, flags);
}

long usock_recvmsg(struct fd_desc *d, struct msghdr *m, int flags)
{
    const struct svcsock_framing *f;
    void *c = conn_of(d, &f);
    if (!c)
        return -ENOTCONN;
    if (f != &unixpair_framing && (flags & (MSG_PEEK | MSG_WAITALL | MSG_TRUNC)))
        return -EOPNOTSUPP;   /* not built: no service framing keeps bytes to look at twice */
    return f->recvmsg(d, c, m, flags);
}

static long usock_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    struct msghdr m = { .msg_iov = (struct iovec *)iov, .msg_iovlen = iovcnt };
    return usock_recvmsg(d, &m, 0);   /* descriptors that come are closed, as Linux's read */
}

static long usock_write(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    const struct msghdr m = { .msg_iov = (struct iovec *)iov, .msg_iovlen = iovcnt };
    return usock_sendmsg(d, &m, 0);
}

/* ---- the kind ----------------------------------------------------------------------------- */

static long usock_stat(struct fd_desc *d, struct stat *st)
{
    st->st_mode = S_IFSOCK | 0777;
    st->st_nlink = 1;
    st->st_uid = st->st_gid = 1000;   /* the one user */
    st->st_blksize = 4096;
    st->st_ino = (ino_t)d->serial;
    return 0;
}

/* d's listener once listen has been called, else NULL. */
static struct ulisten *listener_of(struct fd_desc *d)
{
    struct usock *u = d->priv;
    return state_of(u) == US_LISTENING ? u->l : NULL;
}

static uint32_t usock_ready(struct fd_desc *d, uint32_t interest)
{
    const struct svcsock_framing *f;
    void *c = conn_of(d, &f);
    if (listener_of(d))
        return 0;   /* the arm's channel says when a connection waits */
    return c ? f->ready(c, interest) : NETWAIT_WRITE;   /* as Linux: a write says why at once */
}

static long usock_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out)
{
    const struct svcsock_framing *f;
    void *c = conn_of(d, &f);
    struct ulisten *l = listener_of(d);
    return l ? ulisten_arm(l, out) : c ? f->arm(c, interest, out) : sock_arm_idle(out);
}

/* A service socket's connection is one process's, and a listener the
 * broker's: a child is never given them. A named socket's connection is
 * its socketpair end's to give. */
static long usock_give(struct fd_desc *d, struct fd_given *out)
{
    const struct svcsock_framing *f;
    void *c = conn_of(d, &f);
    return c && f == &unixpair_framing ? unixpair_give(d, c, out) : -ENOTSUP;
}

static void usock_close(struct fd_desc *d)
{
    struct usock *u = d->priv;
    if (state_of(u) == US_CONNECTED)
        u->f->close(u->conn);
    if (state_of(u) == US_BOUND || state_of(u) == US_LISTENING)
        ulisten_close(u->l);
    free(u);
}

const struct fd_ops usock_ops = {
    .name = "unix socket",
    .read = usock_read,
    .write = usock_write,
    .stat = usock_stat,
    .ready = usock_ready,
    .arm = usock_arm,
    .give = usock_give,
    .close = usock_close,
};

struct fd_desc *usock_get(long fd)
{
    struct fd_desc *d = fd_get(fd);
    if (d && d->ops == &usock_ops)
        return d;
    if (d)
        fd_put(d);
    return NULL;
}

/* ---- socket and connect ------------------------------------------------------------------- */

long usock_new(long type, long proto)
{
    long kind = type & ~(long)(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (kind != SOCK_STREAM)
        return -ESOCKTNOSUPPORT;   /* datagrams and SOCK_SEQPACKET: not built */
    if (proto && proto != PF_UNIX)
        return -EPROTONOSUPPORT;
    struct usock *u = calloc(1, sizeof(*u));
    if (!u)
        return -ENOMEM;
    return kind_install(&usock_ops, u, O_RDWR | (type & SOCK_NONBLOCK ? O_NONBLOCK : 0),
                        type & SOCK_CLOEXEC);
}

/* The row for the path in a sockaddr_un's sun_path (len bytes, a NUL
 * ending it early), or NULL. An abstract name (a NUL first) is none. */
static const struct svcsock_row *row_for(const char *path, size_t len)
{
    size_t n = strnlen(path, len);
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        if (strlen(rows[i].path) == n && !memcmp(rows[i].path, path, n))
            return &rows[i];
    return NULL;
}

/* svc_open's answer as connect's errno. */
static long open_errno(status_t st)
{
    switch (st) {
    case ERR_NOT_FOUND:   return -ENOENT;         /* not in our namespace: no socket file */
    case ERR_PEER_CLOSED:
    case ERR_TIMED_OUT:   return -ECONNREFUSED;   /* the service is gone, or never answered */
    }
    return lx_err(st);
}

/* Open row's service and frame it: the framing's state, or NULL with *r. */
static void *open_row(const struct svcsock_row *row, long *r)
{
    handle_t ch = HANDLE_INVALID;
    status_t st = svc_open(row->svc, &ch);   /* may wait: no lock held */
    void *conn = st == OK ? row->framing->open(ch) : NULL;
    *r = st != OK ? open_errno(st) : conn ? 0 : -ENOMEM;
    return conn;
}

long usock_connect(struct fd_desc *d, long addr, long len)
{
    const struct sockaddr_un *a = (const struct sockaddr_un *)(uintptr_t)addr;
    if (!a)
        return -EFAULT;
    if (len < (long)sizeof(sa_family_t) || len > (long)sizeof(*a) || a->sun_family != AF_UNIX)
        return -EINVAL;
    size_t at = offsetof(struct sockaddr_un, sun_path);
    const struct svcsock_row *row =
        (size_t)len > at ? row_for(a->sun_path, (size_t)len - at) : NULL;
    struct unix_name n;
    long r = row ? 0 : unix_name_of(addr, len, &n);   /* not a service socket: a named one */
    if (r)
        return r;
    struct usock *u = d->priv;
    lock_take(&d->lock);
    uint32_t was = state_of(u);
    if (was == US_NEW)
        __atomic_store_n(&u->state, US_CONNECTING, __ATOMIC_RELAXED);
    lock_give(&d->lock);
    if (was != US_NEW)
        return was == US_CONNECTING ? -EALREADY : was == US_CONNECTED ? -EISCONN : -EINVAL;
    void *conn = row ? open_row(row, &r) : unamed_connect(&n, &r);
    lock_take(&d->lock);
    if (conn) {
        u->f = row ? row->framing : &unixpair_framing;
        u->conn = conn;
    }
    __atomic_store_n(&u->state, conn ? US_CONNECTED : US_NEW, __ATOMIC_RELEASE);
    lock_give(&d->lock);
    return r;
}

/* ---- named sockets: bind, listen, accept (unixsock.c's) ----------------------------------- */

long usock_bind(struct fd_desc *d, long addr, long len)
{
    struct unix_name n;
    long r = unix_name_of(addr, len, &n);
    if (r)
        return r;
    if (!n.abstract && row_for(n.bytes, n.len))
        return -EADDRINUSE;   /* a service socket's path: its service has it */
    struct usock *u = d->priv;
    lock_take(&d->lock);
    uint32_t was = state_of(u);
    if (was == US_NEW)
        __atomic_store_n(&u->state, US_BINDING, __ATOMIC_RELAXED);
    lock_give(&d->lock);
    if (was != US_NEW)
        return -EINVAL;   /* bound already, binding, or connected */
    struct ulisten *l = NULL;
    r = ulisten_bind(&n, &l);
    lock_take(&d->lock);
    u->l = l;
    __atomic_store_n(&u->state, r ? US_NEW : US_BOUND, __ATOMIC_RELEASE);
    lock_give(&d->lock);
    return r;
}

long usock_listen(struct fd_desc *d, long backlog)
{
    struct usock *u = d->priv;
    uint32_t st = state_of(u);
    if (st == US_LISTENING)
        return 0;   /* listen again: Linux changes the backlog, ours stays */
    if (st != US_BOUND)
        return -EINVAL;   /* not bound (Linux would autobind a name: not built), or connected */
    long r = ulisten_listen(u->l, backlog > INT32_MAX ? INT32_MAX : (int)backlog);
    if (!r)
        __atomic_store_n(&u->state, US_LISTENING, __ATOMIC_RELEASE);
    return r;
}

long usock_accept(struct fd_desc *d, long addr, long lenp, long flags)
{
    struct ulisten *l = listener_of(d);
    if (!l)
        return -EINVAL;   /* not listening */
    handle_t ch;
    long r = ulisten_accept(l, fd_status(d) & O_NONBLOCK, &ch);
    if (!r)
        r = unix_accepted(ch, flags);
    if (r >= 0 && addr)
        (void)unix_name_out(NULL, addr, lenp);   /* the connector has no name */
    return r;
}

long usock_shutdown(struct fd_desc *d, int how)
{
    const struct svcsock_framing *f;
    void *c = conn_of(d, &f);
    if (!c)
        return -ENOTCONN;
    return f == &unixpair_framing ? unixpair_shutdown(c, how) : -EOPNOTSUPP;
}

long usock_name(struct fd_desc *d, long addr, long lenp, bool peer)
{
    struct usock *u = d->priv;
    uint32_t st = state_of(u);
    bool bound = st == US_BOUND || st == US_LISTENING;
    if (peer && st != US_CONNECTED)
        return -ENOTCONN;
    return unix_name_out(!peer && bound ? ulisten_name(u->l) : NULL, addr, lenp);
}
