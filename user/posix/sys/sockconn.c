/* Sockets: connect, accept, shutdown and the names (getsockname,
 * getpeername). sock.h has the model.
 *
 * **A connection's local port** is netstack's to pick, so a stream
 * socket bound to a port of its own can listen but not connect
 * (EADDRNOTAVAIL); one bound to port 0 can do either. A connect to
 * 127.0.0.0/8 is netstack's own loopback interface: it needs no address
 * or link, and with nothing listening there the answer is ECONNREFUSED
 * (a reset), as anywhere.
 *
 * **POSIX's defaults** on every connection, made or accepted: Nagle's
 * algorithm on (netstack's own connections start with it off) and no
 * keep-alive, unless the options say otherwise (sockopt.c), so each
 * connection costs one net_stream.set_options call. An accepted
 * connection takes its listener's options.
 *
 * **connect** waits for the handshake (SO_SNDTIMEO: EINPROGRESS when it
 * passes, the connection going on) or, with O_NONBLOCK, is EINPROGRESS at
 * once: poll says POLLOUT when it is open, POLLERR when it failed, and
 * SO_ERROR (or connect again) says why once. A signal ends the wait with
 * EINTR, never restarted (calls.c), the connection going on.
 *
 * **accept** always has the listener's next connection asked for (one
 * net_listener.accept in flight), so the listener's channel is readable
 * whenever a connection waits: poll needs nothing else. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "kinds.h"
#include "sock.h"
#include "svcsock.h"

/* A socketpair end's name: AF_UNIX with no path. */
static long unix_out(long addr, long lenp)
{
    socklen_t len, full = sizeof(sa_family_t);   /* the caller's room, said back */
    if (!addr || !lenp || uc_get(&len, (const void *)(uintptr_t)lenp, sizeof(len)))
        return -EFAULT;
    if ((int)len < 0)
        return -EINVAL;
    sa_family_t f = AF_UNIX;
    if (uc_put((void *)(uintptr_t)addr, &f, len < full ? len : full) ||
        uc_put((void *)(uintptr_t)lenp, &full, sizeof(full)))
        return -EFAULT;
    return 0;
}

/* ---- connect ----------------------------------------------------------------------------- */

/* A second connect on a stream socket: Linux's answers. */
static long connect_again(struct sock *s)
{
    uint32_t state;
    status_t err;
    net_tcp_status(&s->ns, &state, &err);
    if (state == SOCKRING_STATE_CONNECTING)
        return -EALREADY;
    if (state == SOCKRING_STATE_CLOSED && err != OK && !s->err_said) {
        s->err_said = true;
        return -sock_closed_errno(err);
    }
    return -EISCONN;
}

static long stream_open_locked(struct sock *s, uint32_t ip, uint16_t port)
{
    if (sock_state(s) == SK_STREAM)
        return connect_again(s);
    if (sock_state(s) != SK_NEW)
        return -EINVAL;   /* a listener, or a socket handed away */
    if (s->bind_port)
        return -EADDRNOTAVAIL;   /* netstack picks a connection's port */
    handle_t net = net_svc();
    if (!net)
        return -EACCES;
    status_t st = net_tcp_open(net, ip, port, s->opts.sndbuf, s->opts.rcvbuf, &s->ns);
    if (st == ERR_INVALID_ARGS)
        return port ? -ENETUNREACH : -EINVAL;   /* an address a program can't reach */
    if (st != OK)
        return st == ERR_NO_RESOURCES ? -EAGAIN : sock_net_errno(st);
    (void)sock_apply_tcp(s);   /* fails only once netstack is gone: the status says so */
    sock_state_set(s, SK_STREAM);
    sock_streams_add(s);
    return 1;   /* made: CONNECTING */
}

/* A connect's look: open (0), failed (its errno, said once), or not yet. */
static long connected_try(struct sock *s, void *arg)
{
    if (sock_state(s) != SK_STREAM)
        return -ENOTCONN;   /* handed away meanwhile */
    uint32_t state;
    status_t err;
    net_tcp_status(&s->ns, &state, &err);
    if (state == SOCKRING_STATE_OPEN)
        return 0;
    if (state == SOCKRING_STATE_CLOSED) {
        s->err_said = true;
        int e = sock_closed_errno(err);
        return e ? -e : -ECONNRESET;
    }
    signals_t seen;
    if (jam_object_wait_one(s->ns.ch, SIG_PEER_CLOSED, 0, &seen) == OK)
        return -ENETDOWN;   /* netstack is gone */
    return -EAGAIN;
}

static long dgram_connect_locked(struct sock *s, long addr, long len)
{
    sa_family_t family = AF_INET;   /* the caller's, read once */
    if (addr && len >= (long)sizeof(family) &&
        uc_get(&family, (const void *)(uintptr_t)addr, sizeof(family)))
        return -EFAULT;
    uint32_t ip = 0;
    uint16_t port = 0;
    bool unspec = addr && len >= (long)sizeof(sa_family_t) && family == AF_UNSPEC;
    long r = unspec ? 0 : sock_addr_in(addr, len, &ip, &port);
    if (!r)
        r = sock_dgram_open_locked(s);
    if (r)
        return r;
    status_t st = net_connect(&s->ns, ip, port);
    if (st != OK)
        return st == ERR_INVALID_ARGS ? -ENETUNREACH : sock_net_errno(st);
    s->connected = !unspec;
    s->peer_addr = ip;
    s->peer_port = port;
    return 0;
}

long lx_connect(long fd, long addr, long len, long d4, long e, long f)
{
    struct fd_desc *d;
    struct sock *s;
    long r = sock_get(fd, &d, &s);
    if (r == -ENOTSOCK && d) {   /* a socketpair end is connected already */
        r = d->ops == &usock_ops ? usock_connect(d, addr, len) : -EISCONN;
        fd_put(d);
        return r;
    }
    if (r)
        return r;
    uint32_t ip = 0;
    uint16_t port = 0;
    lock_take(&d->lock);
    if (s->type == SOCK_DGRAM)
        r = dgram_connect_locked(s, addr, len);
    else if (!(r = sock_addr_in(addr, len, &ip, &port)))
        r = stream_open_locked(s, ip, port);
    uint64_t timeout = s->opts.sndtimeo;
    lock_give(&d->lock);
    if (r == 1 && (fd_status(d) & O_NONBLOCK)) {
        r = -EINPROGRESS;
    } else if (r == 1) {
        r = sock_wait(s, SIDE_TX, connected_try, NULL, false, sock_deadline(timeout), timeout);
        if (r == -EAGAIN)
            r = -EINPROGRESS;   /* SO_SNDTIMEO passed: it goes on, as Linux's */
    }
    fd_put(d);
    return r;
}

/* ---- accept ------------------------------------------------------------------------------ */

/* One accept's try, under the description's lock: 0 with a connection
 * in *ns, -EAGAIN for none yet, or -errno. The next is asked for at
 * once, so the listener's channel is readable whenever one waits (poll). */
static long accept_take(struct fd_desc *d, struct sock *s, struct net_sock *ns, uint32_t *peer,
                        uint16_t *port)
{
    lock_take(&d->lock);
    if (sock_state(s) != SK_LISTENING) {
        lock_give(&d->lock);
        return -EINVAL;   /* not listening (or handed away) */
    }
    status_t st = net_tcp_accept_send(&s->l);
    if (st == OK)
        st = net_tcp_accept_take(&s->l, ns, peer, port);
    if (st != ERR_SHOULD_WAIT)
        (void)net_tcp_accept_send(&s->l);   /* failed: the next take asks again */
    lock_give(&d->lock);
    if (st == OK || st == ERR_SHOULD_WAIT)
        return st == OK ? 0 : -EAGAIN;
    return st == ERR_PEER_CLOSED ? -ENETDOWN : -ECONNABORTED;
}

/* Wait for a connection: 0 with *ns, or -errno. */
static long accept_wait(struct fd_desc *d, struct sock *s, bool nonblock, struct net_sock *ns,
                        uint32_t *peer, uint16_t *port)
{
    uint64_t timeout = s->opts.rcvtimeo, deadline = sock_deadline(timeout);
    for (;;) {   /* each turn takes one, fails, or waits for the listener's channel */
        long r = accept_take(d, s, ns, peer, port);
        if (r != -EAGAIN)
            return r;
        if (nonblock || (deadline != DEADLINE_NEVER && now() >= deadline))
            return -EAGAIN;
        signals_t seen = 0;
        status_t st = jam_object_wait_one(s->l.ch, SIG_READABLE | SIG_PEER_CLOSED, deadline,
                                          &seen);
        if (st == ERR_INTERRUPTED)
            return timeout ? -LX_EINTR_FINAL : -EINTR;
        if (st != OK && st != ERR_TIMED_OUT)
            return lx_err(st);
    }
}

/* The accepted connection as a new descriptor, with the listener's
 * options (Linux's accepted sockets inherit them). */
static long accept_install(const struct sock_opts *opts, struct net_sock *ns, long flags)
{
    struct sock *n = calloc(1, sizeof(*n));
    if (!n) {
        net_close(ns);
        return -ENOMEM;
    }
    n->type = SOCK_STREAM;
    n->ns = *ns;
    n->opts = *opts;
    n->opts.reuseaddr = false;
    (void)sock_apply_tcp(n);   /* fails only once netstack is gone: the status says so */
    sock_state_set(n, SK_STREAM);
    sock_streams_add(n);
    return kind_install(&sock_ops, n, O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0),
                        flags & SOCK_CLOEXEC);
}

long lx_accept4(long fd, long addr, long lenp, long flags, long e, long f)
{
    if (flags & ~(long)(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    socklen_t room = 0;   /* the caller's, checked before the wait */
    if (addr && lenp && uc_get(&room, (const void *)(uintptr_t)lenp, sizeof(room)))
        return -EFAULT;
    if ((int)room < 0)
        return -EINVAL;
    struct fd_desc *d;
    struct sock *s;
    long r = sock_get(fd, &d, &s);
    if (r == -ENOTSOCK && d) {   /* a named AF_UNIX listener's (svcsock.c), or a socketpair end */
        r = d->ops == &usock_ops ? usock_accept(d, addr, lenp, flags) : -EOPNOTSUPP;
        fd_put(d);
        return r;
    }
    if (r)
        return r;
    struct net_sock ns;
    uint32_t peer = 0;
    uint16_t port = 0;
    r = s->type != SOCK_STREAM ? -EOPNOTSUPP
                               : accept_wait(d, s, fd_status(d) & O_NONBLOCK, &ns, &peer, &port);
    if (!r) {
        lock_take(&d->lock);
        struct sock_opts opts = s->opts;
        lock_give(&d->lock);
        r = accept_install(&opts, &ns, flags);
    }
    if (r >= 0 && sock_addr_out(addr, lenp, peer, port) == -EFAULT) {   /* length checked above */
        (void)fd_close(r);   /* Linux's: the connection goes with the bad address */
        r = -EFAULT;
    }
    fd_put(d);
    return r;
}

long lx_accept(long fd, long addr, long lenp, long d, long e, long f)
{
    return lx_accept4(fd, addr, lenp, 0, 0, 0);
}

/* ---- shutdown and the names -------------------------------------------------------------- */

long lx_shutdown(long fd, long how, long c, long d4, long e, long f)
{
    struct fd_desc *d;
    struct sock *s;
    long r = sock_get(fd, &d, &s);
    if (r == -ENOTSOCK && d) {   /* a socketpair end (sockpair.c's), or an AF_UNIX stream */
        r = d->ops == &sockp_ops ? sockp_shutdown(d, (int)how) : usock_shutdown(d, (int)how);
        fd_put(d);
        return r;
    }
    if (r)
        return r;
    uint32_t state = sock_state(s), st = 0;
    status_t err;
    if (state == SK_STREAM)
        net_tcp_status(&s->ns, &st, &err);
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        r = -EINVAL;
    else if (!(state == SK_STREAM && st != SOCKRING_STATE_CONNECTING) &&
             !(state == SK_DGRAM && s->connected))
        r = -ENOTCONN;
    if (!r && how != SHUT_WR) {
        lock_take(&s->rx_lock);
        __atomic_store_n(&s->rd_shut, true, __ATOMIC_RELAXED);
        lock_give(&s->rx_lock);
    }
    if (!r && how != SHUT_RD) {
        lock_take(&s->tx_lock);
        if (!s->wr_shut && sock_state(s) == SK_STREAM)
            (void)net_shutdown(&s->ns);   /* the rings are there: can't fail */
        s->wr_shut = true;
        lock_give(&s->tx_lock);
    }
    fd_put(d);
    return r;
}

/* A connection's or a datagram socket's own ends, from netstack. */
static long ends_of(struct sock *s, struct net_tcp_ends *e)
{
    *e = (struct net_tcp_ends){ 0 };
    status_t st;
    if (sock_state(s) == SK_STREAM) {
        st = net_tcp_ends(&s->ns, e);
    } else {
        uint32_t dropped;
        st = net_socket_state_until(s->ns.ch, now() + CALL_WAIT, &e->local, &e->local_port,
                                    &e->peer, &e->peer_port, &e->queued, &dropped);
    }
    return st == OK ? 0 : st == ERR_PEER_CLOSED ? -ENETDOWN : lx_err(st);
}

static long name_of(struct sock *s, bool peer, uint32_t *ip, uint16_t *port)
{
    struct net_tcp_ends e;
    uint32_t state = sock_state(s);
    *ip = s->bind_addr;
    *port = s->bind_port;
    if (state == SK_LISTENING && !peer)
        *port = s->l.port;
    if (state == SK_DGRAM && peer) {
        *ip = s->peer_addr;
        *port = s->peer_port;
        return s->connected ? 0 : -ENOTCONN;
    }
    if (state != SK_STREAM && state != SK_DGRAM)
        return peer ? -ENOTCONN : 0;
    if (state == SK_STREAM && peer) {
        uint32_t st;
        status_t err;
        net_tcp_status(&s->ns, &st, &err);
        if (st != SOCKRING_STATE_OPEN)
            return -ENOTCONN;
    }
    long r = ends_of(s, &e);
    if (!r) {
        *ip = peer ? e.peer : s->bind_addr ? s->bind_addr : e.local;
        *port = peer ? e.peer_port : e.local_port;
    }
    return r;
}

static long names(long fd, long addr, long lenp, bool peer)
{
    struct fd_desc *d;
    struct sock *s;
    long r = sock_get(fd, &d, &s);
    if (r == -ENOTSOCK && d) {
        r = d->ops == &usock_ops ? usock_name(d, addr, lenp, peer) : unix_out(addr, lenp);
        fd_put(d);
        return r;
    }
    if (r)
        return r;
    uint32_t ip = 0;
    uint16_t port = 0;
    r = name_of(s, peer, &ip, &port);
    if (!r)
        r = addr && lenp ? sock_addr_out(addr, lenp, ip, port) : -EFAULT;
    fd_put(d);
    return r;
}

long lx_getsockname(long fd, long addr, long lenp, long d, long e, long f)
{
    return names(fd, addr, lenp, false);
}

long lx_getpeername(long fd, long addr, long lenp, long d, long e, long f)
{
    return names(fd, addr, lenp, true);
}
