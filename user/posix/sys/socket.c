/* Sockets: the descriptor kind, socket, bind and listen, and the drain
 * at exit. The model, the openers and the locks are sock.h's; connect,
 * accept, shutdown and the names are sockconn.c's, the bytes sockio.c's,
 * a socket handed to a child sockgive.c's.
 *
 * **Addresses.** AF_INET here (AF_INET6: EAFNOSUPPORT; AF_UNIX stream
 * sockets are svcsock.c's, named ones through it unixsock.c's, socketpair
 * pipe.c's). netstack serves one card
 * interface and the loopback one (127.0.0.0/8): a bind to an address that
 * isn't one of those (or INADDR_ANY) is EADDRNOTAVAIL. A socket bound to a
 * loopback address is reachable from this host only (a datagram socket
 * is made loopback-only, a listener too: net.idl's loopback_only and
 * LISTEN_LOOPBACK), so a service bound to 127.0.0.1 is not on the network.
 *
 * **Binding.** A port below NET_PORT_EPHEMERAL needs the listen
 * permission (below NET_PORT_LOW the low one): EACCES without it, at bind.
 * A datagram socket is opened at its bind (EADDRINUSE then); a stream
 * socket's bind is only remembered until listen, where netstack checks
 * the port.
 *
 * **Closing.** netstack sends a FIN after what it has taken from the tx
 * ring and drops the rest when the channel closes, so close and exit
 * first wait, up to the linger time, for netstack to take every byte
 * written (Linux sends them after close returns; a process here can only
 * wait). A listener's waiting connections are reset. */
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

#define IDLE_SIGNAL  (1u << 24)        /* the idle event's bit: never raised */

static lock_t       streams_lock;   /* streams, and a stream on it is never closed meanwhile */
static struct sock *streams;        /* every SK_STREAM socket here: the drain at exit */
static handle_t     idle_event;     /* poll's handle for a socket with nothing to wait on */

/* ---- small parts ------------------------------------------------------------------------- */

void sock_state_set(struct sock *s, enum sock_state st)
{
    __atomic_store_n(&s->state, st, __ATOMIC_RELEASE);
}

void sock_streams_add(struct sock *s)
{
    lock_take(&streams_lock);
    s->next = streams;
    streams = s;
    lock_give(&streams_lock);
}

void sock_streams_remove(struct sock *s)
{
    lock_take(&streams_lock);
    for (struct sock **p = &streams; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            break;
        }
    }
    lock_give(&streams_lock);
}

handle_t sock_opener(uint16_t port, bool listen)
{
    if (port && port < NET_PORT_LOW)
        return svc_get(SVC_NET_LISTEN_LOW);
    if (listen || (port && port < NET_PORT_EPHEMERAL))
        return svc_get(SVC_NET_LISTEN);
    return net_svc();
}

/* netstack's answer to a call that makes or binds a socket, as an errno. */
long sock_net_errno(status_t st)
{
    switch (st) {
    case ERR_ALREADY_BOUND: return -EADDRINUSE;
    case ERR_ACCESS_DENIED: return -EACCES;
    case ERR_NO_RESOURCES:  return -ENOBUFS;
    case ERR_PEER_CLOSED:   return -ENETDOWN;
    case ERR_BAD_STATE:     return -ENETUNREACH;   /* no address, no route, no link */
    }
    return lx_err(st);
}

int sock_closed_errno(status_t err)
{
    switch (err) {
    case OK:               return 0;
    case ERR_NOT_FOUND:    return ECONNREFUSED;   /* a reset for our SYN */
    case ERR_TIMED_OUT:    return ETIMEDOUT;
    case ERR_PEER_CLOSED:  return ECONNRESET;
    case ERR_BAD_STATE:    return ENETUNREACH;    /* our address went away */
    case ERR_OUT_OF_RANGE: return ECONNABORTED;   /* our own rings were out of range */
    }
    return ECONNABORTED;
}

long sock_apply_tcp(struct sock *s)
{
    uint32_t f = (s->opts.nodelay ? NET_STREAM_OPT_NODELAY : 0) |
                 (s->opts.keepalive ? NET_STREAM_OPT_KEEPALIVE : 0);
    status_t st = net_tcp_set_options(&s->ns, f, s->opts.keep_idle, s->opts.keep_intvl,
                                      s->opts.keep_cnt);
    return st == OK ? 0 : lx_err(st);
}

long sock_dgram_open_locked(struct sock *s)
{
    if (sock_state(s) == SK_DGRAM)
        return 0;
    if (sock_state(s) != SK_NEW)
        return -EINVAL;
    handle_t net = sock_opener(s->bind_port, false);
    if (!net)
        return -EACCES;
    status_t st = net_udp_open_rings(net, s->bind_port, s->opts.sndbuf, s->opts.rcvbuf, &s->ns);
    if (st != OK)
        return sock_net_errno(st);
    if (sock_is_loopback(s->bind_addr)) {   /* only this host's own datagrams */
        st = net_socket_loopback_only_until(s->ns.ch, now() + CALL_WAIT);
        if (st != OK) {
            net_close(&s->ns);
            return sock_net_errno(st);
        }
    }
    struct sockring_status ss;
    sockring_status_get(&s->ns.r, &ss);
    s->refused = ss.tx_refused;
    sock_state_set(s, SK_DGRAM);
    return 0;
}

/* Descriptor fd's description (held: the caller puts it) and its socket.
 * 0; -EBADF; -ENOTSOCK for another kind (*d is still set and held for a
 * socketpair end, whose calls go to pipe.c, and for an AF_UNIX stream
 * socket, svcsock.c's). */
long sock_get(long fd, struct fd_desc **d, struct sock **s)
{
    *d = fd_get(fd);
    if (!*d)
        return -EBADF;
    *s = sock_of(*d);
    if (*s)
        return 0;
    if ((*d)->ops == &sockp_ops || (*d)->ops == &usock_ops)
        return -ENOTSOCK;
    fd_put(*d);
    *d = NULL;
    return -ENOTSOCK;
}

/* An AF_INET address from the caller: its address and port, host order. */
long sock_addr_in(long addr, long len, uint32_t *ip, uint16_t *port)
{
    if (!addr)
        return -EFAULT;
    if (len < (long)sizeof(sa_family_t))
        return -EINVAL;
    const struct sockaddr_in *a = (const struct sockaddr_in *)(uintptr_t)addr;
    if (a->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    if (len < (long)sizeof(struct sockaddr_in))
        return -EINVAL;
    *ip = ntohl(a->sin_addr.s_addr);
    *port = ntohs(a->sin_port);
    return 0;
}

/* Write an AF_INET address out (cut to the caller's room, the full length
 * said back, as Linux does). Nothing to do when addr or lenp is NULL. */
long sock_addr_out(long addr, long lenp, uint32_t ip, uint16_t port)
{
    if (!addr || !lenp)
        return 0;
    socklen_t *len = (socklen_t *)(uintptr_t)lenp;
    if ((int)*len < 0)
        return -EINVAL;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr = { htonl(ip) } };
    memcpy((void *)(uintptr_t)addr, &a, *len < sizeof(a) ? *len : sizeof(a));
    *len = sizeof(a);
    return 0;
}

/* ---- the kind ---------------------------------------------------------------------------- */

static long sock_stat(struct fd_desc *d, struct stat *st)
{
    st->st_mode = S_IFSOCK | 0777;
    st->st_nlink = 1;
    st->st_uid = st->st_gid = 1000;   /* the one user */
    st->st_blksize = 4096;
    st->st_ino = (ino_t)d->serial;
    return 0;
}

/* FIONREAD: a stream's bytes waiting; a datagram socket's next datagram's
 * length (taken off the ring and kept, as MSG_PEEK does). */
static long sock_ioctl(struct fd_desc *d, unsigned long req, long arg)
{
    struct sock *s = d->priv;
    if (req == SIOCATMARK)
        return uc_put_int(arg, 0);   /* no out-of-band data, ever */
    if (req != FIONREAD)
        return -ENOTTY;
    long n = 0;
    lock_take(&s->rx_lock);
    if (sock_state(s) == SK_STREAM && !s->rd_shut) {
        n = sockring_ready(&s->ns.r.rx);
    } else if (sock_state(s) == SK_DGRAM) {
        if (!s->kept && (s->kept = malloc(sizeof(*s->kept))) &&
            net_sock_take(&s->ns, s->kept) != OK) {
            free(s->kept);
            s->kept = NULL;
        }
        n = s->kept ? s->kept->len : 0;
    }
    lock_give(&s->rx_lock);
    return uc_put_int(arg, (int)n);
}

/* poll: what libc itself knows (no lock, no call: ready.h's rule). */
static uint32_t sock_ready(struct fd_desc *d, uint32_t interest)
{
    struct sock *s = d->priv;
    switch (sock_state(s)) {
    case SK_NEW:
        return NETWAIT_WRITE;   /* as Linux: a write would say why at once */
    case SK_GONE:
        return NETWAIT_READ | NETWAIT_WRITE;   /* every call answers at once (ENOTCONN) */
    case SK_STREAM:
        return __atomic_load_n(&s->rd_shut, __ATOMIC_RELAXED) ? NETWAIT_READ : 0;
    case SK_DGRAM:
        return __atomic_load_n(&s->kept, __ATOMIC_RELAXED) ? NETWAIT_READ : 0;
    }
    return 0;
}

/* The event poll waits on for a socket with nothing at netstack: never
 * signalled, so its readiness is sock_ready's alone. One for the process. */
static handle_t idle(void)
{
    handle_t h = __atomic_load_n(&idle_event, __ATOMIC_ACQUIRE), mine = HANDLE_INVALID;
    if (h || jam_event_create(&mine) != OK)
        return h;
    if (__atomic_compare_exchange_n(&idle_event, &h, mine, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return mine;
    jam_handle_close(mine);   /* another thread's is the one */
    return h;
}

long sock_arm_idle(struct fd_wait *out)
{
    handle_t h = idle();
    if (!h)
        return -ENOMEM;
    out->kind = FD_WAIT_HANDLE;
    out->h = (struct netwait_handle){ .h = h, .read = IDLE_SIGNAL, .hup = IDLE_SIGNAL };
    return 0;
}

static long sock_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out)
{
    struct sock *s = d->priv;
    switch (sock_state(s)) {
    case SK_STREAM:
    case SK_DGRAM:
        out->kind = FD_WAIT_SOCK;
        net_sock_waitable(&s->ns, &out->sock);
        return 0;
    case SK_LISTENING:
        out->kind = FD_WAIT_HANDLE;
        net_listener_waitable(&s->l, &out->h);   /* READ: an accept's answer (one is always
                                                  * asked: lx_listen, lx_accept4) */
        return 0;
    }
    return sock_arm_idle(out);
}

/* A drain's look: has netstack taken every byte written (or is there
 * nothing more it will take)? */
static long drained_try(struct sock *s, void *arg)
{
    lock_take(&s->tx_lock);
    uint32_t state;
    status_t err;
    long r = 0;
    if (sock_state(s) == SK_STREAM) {
        net_tcp_status(&s->ns, &state, &err);
        if (state != SOCKRING_STATE_CLOSED && sockring_room(&s->ns.r.tx) < s->ns.r.tx.size)
            r = -EAGAIN;
    }
    lock_give(&s->tx_lock);
    return r;
}

/* Wait until netstack has taken what was written, or the deadline. A
 * signal doesn't end it (close must not fail): the wait goes on. */
static void drain(struct sock *s, uint64_t deadline)
{
    for (unsigned guard = 0; guard < 1000; guard++)   /* each turn: one wait, or done */
        if (sock_wait(s, SIDE_TX, drained_try, NULL, false, deadline, false) != -EINTR)
            return;
}

static uint64_t linger_deadline(const struct sock *s)
{
    if (!s->opts.linger_on)
        return now() + SOCK_LINGER_NS;
    return now() + (uint64_t)s->opts.linger_s * NS_PER_S;
}

static void sock_close(struct fd_desc *d)
{
    struct sock *s = d->priv;
    switch (sock_state(s)) {
    case SK_STREAM:
        sock_streams_remove(s);   /* first: the drain at exit never meets a closing socket */
        drain(s, linger_deadline(s));
        net_close(&s->ns);
        break;
    case SK_DGRAM:
        net_close(&s->ns);
        break;
    case SK_LISTENING:
        net_listener_close(&s->l);   /* its waiting connections are reset */
        break;
    }
    free(s->kept);
    free(s);
}

const struct fd_ops sock_ops = {
    .name = "socket",
    .read = sock_read,
    .write = sock_write,
    .stat = sock_stat,
    .ioctl = sock_ioctl,
    .ready = sock_ready,
    .arm = sock_arm,
    .give = sock_give,
    .close = sock_close,
};

/* ---- the drain at exit ------------------------------------------------------------------- */

void sock_exit_drain(void)
{
    if (!__atomic_load_n(&streams, __ATOMIC_ACQUIRE))
        return;   /* no stream: the common case */
    uint64_t deadline = now() + SOCK_LINGER_NS;
    lock_take(&streams_lock);   /* a stream on the list is not closed meanwhile */
    for (struct sock *s = streams; s; s = s->next)
        drain(s, deadline);
    lock_give(&streams_lock);
}

/* ---- socket, bind, listen ---------------------------------------------------------------- */

long lx_socket(long domain, long type, long proto, long d, long e, long f)
{
    if (domain == AF_UNIX)
        return usock_new(type, proto);   /* connects only to a service socket: svcsock.c */
    if (domain != AF_INET)
        return -EAFNOSUPPORT;   /* AF_INET6, the rest */
    long kind = type & ~(long)(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (kind == SOCK_STREAM && proto && proto != IPPROTO_TCP)
        return -EPROTONOSUPPORT;
    if (kind == SOCK_DGRAM && proto && proto != IPPROTO_UDP)
        return -EPROTONOSUPPORT;
    if (kind != SOCK_STREAM && kind != SOCK_DGRAM)
        return -ESOCKTNOSUPPORT;   /* raw sockets and the rest: not built */
    struct sock *s = calloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->type = (int)kind;
    return kind_install(&sock_ops, s, O_RDWR | (type & SOCK_NONBLOCK ? O_NONBLOCK : 0),
                        type & SOCK_CLOEXEC);
}

/* Is ip one this host may bind (any, a loopback one, or the interface's own)? */
static long addr_ours(uint32_t ip)
{
    if (!ip || sock_is_loopback(ip))
        return 0;
    struct net_info i;
    handle_t net = net_svc();
    if (!net)
        return -EACCES;
    status_t st = net_info(net, &i);
    if (st != OK)
        return sock_net_errno(st);
    return i.address == ip ? 0 : -EADDRNOTAVAIL;   /* one card interface */
}

static long bind_locked(struct sock *s, uint32_t ip, uint16_t port)
{
    if (s->bound || sock_state(s) != SK_NEW)
        return -EINVAL;
    long r = addr_ours(ip);
    if (r)
        return r;
    if (!sock_opener(port, false))
        return -EACCES;   /* a port below NET_PORT_EPHEMERAL needs the listen permission */
    s->bound = true;
    s->bind_addr = ip;
    s->bind_port = port;
    if (s->type == SOCK_DGRAM && (r = sock_dgram_open_locked(s)) != 0)
        s->bound = false;
    return r;
}

long lx_bind(long fd, long addr, long len, long d4, long e, long f)
{
    struct fd_desc *d;
    struct sock *s;
    long r = sock_get(fd, &d, &s);
    if (r == -ENOTSOCK && d) {
        /* a named AF_UNIX socket (svcsock.c); a socketpair end is already "bound" to its peer */
        r = d->ops == &usock_ops ? usock_bind(d, addr, len) : -EINVAL;
        fd_put(d);
        return r;
    }
    if (r)
        return r;
    uint32_t ip = 0;
    uint16_t port = 0;
    r = sock_addr_in(addr, len, &ip, &port);
    if (!r) {
        lock_take(&d->lock);
        r = bind_locked(s, ip, port);
        lock_give(&d->lock);
    }
    fd_put(d);
    return r;
}

static long listen_locked(struct sock *s, long backlog)
{
    if (sock_state(s) == SK_LISTENING)
        return 0;   /* listen again: Linux changes the backlog, ours stays */
    if (sock_state(s) != SK_NEW)
        return -EINVAL;
    uint16_t port = s->bound ? s->bind_port : 0;
    handle_t net = sock_opener(port, true);
    if (!net)
        return -EACCES;   /* the program's list didn't ask for `svc net listen` */
    uint32_t b = backlog < 1 ? 1 : backlog > NET_BACKLOG_MAX ? NET_BACKLOG_MAX : (uint32_t)backlog;
    uint32_t flags = (s->opts.reuseaddr ? NET_LISTEN_REUSE : 0) |
                     (sock_is_loopback(s->bind_addr) ? NET_LISTEN_LOOPBACK : 0);
    status_t st = net_tcp_listen_flags(net, port, b, s->opts.sndbuf, s->opts.rcvbuf, flags,
                                       &s->l);
    if (st != OK)
        return sock_net_errno(st);
    (void)net_tcp_accept_send(&s->l);   /* failed: accept asks again */
    sock_state_set(s, SK_LISTENING);
    return 0;
}

long lx_listen(long fd, long backlog, long c, long d4, long e, long f)
{
    struct fd_desc *d;
    struct sock *s;
    long r = sock_get(fd, &d, &s);
    if (r == -ENOTSOCK && d) {
        r = d->ops == &usock_ops ? usock_listen(d, backlog) : -EOPNOTSUPP;   /* svcsock.c's */
        fd_put(d);
        return r;
    }
    if (r)
        return r;
    if (s->type != SOCK_STREAM) {
        fd_put(d);
        return -EOPNOTSUPP;
    }
    lock_take(&d->lock);
    r = listen_locked(s, backlog);
    lock_give(&d->lock);
    fd_put(d);
    return r;
}
