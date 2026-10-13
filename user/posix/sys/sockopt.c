/* Sockets' options: setsockopt and getsockopt (docs/M13-PLAN.md section
 * 8). They are kept in the socket (sock.h's struct sock_opts, under the
 * description's lock) and applied where they belong:
 *
 *   TCP_NODELAY, SO_KEEPALIVE, TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT
 *       net_stream.set_options on a connection now, and on every one made
 *       or accepted later (a listener's go to the connections it accepts);
 *   SO_REUSEADDR
 *       a listener's NET_LISTEN_REUSE, at listen;
 *   SO_SNDBUF, SO_RCVBUF
 *       the rings' sizes, a power of two from SOCKRING_MIN to
 *       SOCKRING_MAX, when given before connect, listen or (a datagram
 *       socket) bind; getsockopt says the rings' real sizes;
 *   SO_RCVTIMEO, SO_SNDTIMEO
 *       the waits' timeouts (sockio.c): EAGAIN when they pass;
 *   SO_LINGER
 *       how long close waits for netstack to take what was written
 *       (socket.c); l_linger 0: not at all;
 *   SO_ERROR, SO_TYPE, SO_DOMAIN, SO_PROTOCOL, SO_ACCEPTCONN
 *       read only.
 *
 * Accepted and ignored (nothing here could do them; getsockopt says the
 * default): SO_BROADCAST (kept, but netstack sends no broadcast),
 * SO_REUSEPORT, SO_OOBINLINE, SO_DONTROUTE, SO_RCVLOWAT, SO_SNDLOWAT,
 * SO_PRIORITY, SO_TIMESTAMP, TCP_CORK, TCP_QUICKACK, TCP_DEFER_ACCEPT,
 * TCP_USER_TIMEOUT, TCP_MAXSEG, IP_TOS, IP_TTL, IP_MTU_DISCOVER. Any other
 * option, or another level, is ENOPROTOOPT; a value out of range EINVAL
 * (a time's microseconds EDOM). A socketpair end answers SO_TYPE,
 * SO_DOMAIN, SO_PROTOCOL and SO_ERROR, and takes the buffer sizes. */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/time.h>
#include "kinds.h"
#include "sock.h"

#define INT_SZ ((socklen_t)sizeof(int))

uint32_t sock_ring_size(uint64_t asked)
{
    if (asked <= SOCKRING_MIN)
        return SOCKRING_MIN;
    if (asked >= SOCKRING_MAX)
        return SOCKRING_MAX;
    uint32_t size = SOCKRING_MIN;
    while (size < asked)
        size <<= 1;
    return size;
}

/* The int at val (len bytes at least): 0, or -EINVAL. */
static long int_in(long val, long len, int *out)
{
    if (!val)
        return -EFAULT;
    if (len < INT_SZ)
        return -EINVAL;
    memcpy(out, (const void *)(uintptr_t)val, sizeof(*out));
    return 0;
}

/* A struct timeval as ns (0: no timeout). */
static long time_in(long val, long len, uint64_t *out)
{
    struct timeval tv;
    if (!val)
        return -EFAULT;
    if (len < (long)sizeof(tv))
        return -EINVAL;
    memcpy(&tv, (const void *)(uintptr_t)val, sizeof(tv));
    if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
        return -EDOM;
    uint64_t ns;
    if (__builtin_mul_overflow((uint64_t)tv.tv_sec, NS_PER_S, &ns) ||
        __builtin_add_overflow(ns, (uint64_t)tv.tv_usec * 1000, &ns))
        ns = DEADLINE_NEVER;   /* forever, in effect */
    *out = ns;
    return 0;
}

/* Copy n bytes of v out to val, at most *lenp (said back): Linux's rule. */
static long out(long val, long lenp, const void *v, socklen_t n)
{
    if (!val || !lenp)
        return -EFAULT;
    socklen_t *len = (socklen_t *)(uintptr_t)lenp;
    if ((int)*len < 0)
        return -EINVAL;
    if (*len < n)
        n = *len;
    memcpy((void *)(uintptr_t)val, v, n);
    *len = n;
    return 0;
}

static long int_out(long val, long lenp, int v)
{
    return out(val, lenp, &v, INT_SZ);
}

/* ---- setsockopt -------------------------------------------------------------------------- */

/* An option of the connection's (net_stream.set_options) changed: apply
 * the whole set to a connection now; later ones get it as they are made. */
static long tcp_changed(struct sock *s)
{
    return sock_state(s) == SK_STREAM ? sock_apply_tcp(s) : 0;
}

static long set_socket_level(struct sock *s, int name, long val, long len)
{
    int v = 0;
    long r = name == SO_RCVTIMEO || name == SO_SNDTIMEO || name == SO_LINGER ? 0
                                                                             : int_in(val, len, &v);
    if (r)
        return r;
    switch (name) {
    case SO_KEEPALIVE:
        s->opts.keepalive = v != 0;
        return s->type == SOCK_STREAM ? tcp_changed(s) : 0;
    case SO_REUSEADDR:
        s->opts.reuseaddr = v != 0;
        return 0;
    case SO_BROADCAST:
        s->opts.broadcast = v != 0;
        return 0;
    case SO_SNDBUF:
        s->opts.sndbuf = sock_ring_size(v < 0 ? 0 : (uint64_t)v);
        return 0;
    case SO_RCVBUF:
        s->opts.rcvbuf = sock_ring_size(v < 0 ? 0 : (uint64_t)v);
        return 0;
    case SO_RCVTIMEO:
        return time_in(val, len, &s->opts.rcvtimeo);
    case SO_SNDTIMEO:
        return time_in(val, len, &s->opts.sndtimeo);
    case SO_LINGER: {
        struct linger l;
        if (!val)
            return -EFAULT;
        if (len < (long)sizeof(l))
            return -EINVAL;
        memcpy(&l, (const void *)(uintptr_t)val, sizeof(l));
        s->opts.linger_on = l.l_onoff != 0;
        s->opts.linger_s = l.l_linger < 0 ? 0 : (uint32_t)l.l_linger;
        return 0;
    }
    case SO_REUSEPORT: case SO_OOBINLINE: case SO_DONTROUTE: case SO_RCVLOWAT:
    case SO_SNDLOWAT: case SO_PRIORITY: case SO_TIMESTAMP:
        return 0;   /* accepted and ignored (the file's header) */
    }
    return -ENOPROTOOPT;
}

static long set_tcp_level(struct sock *s, int name, long val, long len)
{
    int v = 0;
    long r = s->type == SOCK_STREAM ? int_in(val, len, &v) : -ENOPROTOOPT;
    if (r)
        return r;
    switch (name) {
    case TCP_NODELAY:
        s->opts.nodelay = v != 0;
        return tcp_changed(s);
    case TCP_KEEPIDLE:
    case TCP_KEEPINTVL:
        if (v < 1 || v > (int)NET_STREAM_KEEP_MAX)
            return -EINVAL;
        *(name == TCP_KEEPIDLE ? &s->opts.keep_idle : &s->opts.keep_intvl) = (uint32_t)v;
        return tcp_changed(s);
    case TCP_KEEPCNT:
        if (v < 1 || v > (int)NET_STREAM_KEEP_CNT_MAX)
            return -EINVAL;
        s->opts.keep_cnt = (uint32_t)v;
        return tcp_changed(s);
    case TCP_CORK: case TCP_QUICKACK: case TCP_DEFER_ACCEPT: case TCP_USER_TIMEOUT:
    case TCP_MAXSEG:
        return 0;   /* accepted and ignored */
    }
    return -ENOPROTOOPT;
}

static long set_on_sock(struct fd_desc *d, struct sock *s, long level, long name, long val,
                        long len)
{
    long r = -ENOPROTOOPT;
    lock_take(&d->lock);
    if (level == SOL_SOCKET)
        r = set_socket_level(s, (int)name, val, len);
    else if (level == IPPROTO_TCP)
        r = set_tcp_level(s, (int)name, val, len);
    else if (level == IPPROTO_IP && (name == IP_TOS || name == IP_TTL || name == IP_MTU_DISCOVER))
        r = 0;   /* accepted and ignored */
    lock_give(&d->lock);
    return r;
}

long lx_setsockopt(long fd, long level, long name, long val, long len, long f)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return -EBADF;
    long r = -ENOTSOCK;
    if (sock_of(d))
        r = set_on_sock(d, d->priv, level, name, val, len);
    else if (d->ops == &sockp_ops)
        r = level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF) ? 0 : -ENOPROTOOPT;
    fd_put(d);
    return r;
}

/* ---- getsockopt -------------------------------------------------------------------------- */

/* SO_ERROR: an error waiting (a failed connection's, said once), then 0. */
static int take_error(struct sock *s)
{
    int e = s->error;
    s->error = 0;
    if (e || sock_state(s) != SK_STREAM || s->err_said)
        return e;
    uint32_t state;
    status_t err;
    net_tcp_status(&s->ns, &state, &err);
    if (state != SOCKRING_STATE_CLOSED || !(e = sock_closed_errno(err)))
        return 0;
    s->err_said = true;
    return e;
}

/* The rings' size on side tx (or rx) now, or what a new one would get. */
static int ring_bytes(const struct sock *s, bool tx)
{
    uint32_t st = sock_state(s), asked = tx ? s->opts.sndbuf : s->opts.rcvbuf;
    if (st == SK_STREAM || st == SK_DGRAM)
        return (int)(tx ? s->ns.r.tx.size : s->ns.r.rx.size);
    if (asked)
        return (int)asked;
    if (s->type == SOCK_STREAM)
        return (int)(tx ? NET_TCP_TX : NET_TCP_RX);
    return (int)(tx ? SOCKRING_UDP_TX : SOCKRING_UDP_RX);
}

static long time_out(long val, long lenp, uint64_t ns)
{
    struct timeval tv = { .tv_sec = (time_t)(ns / NS_PER_S),
                          .tv_usec = (suseconds_t)(ns % NS_PER_S / 1000) };
    return out(val, lenp, &tv, sizeof(tv));
}

static long get_socket_level(struct sock *s, int name, long val, long lenp)
{
    switch (name) {
    case SO_TYPE:       return int_out(val, lenp, s->type);
    case SO_DOMAIN:     return int_out(val, lenp, AF_INET);
    case SO_PROTOCOL:   return int_out(val, lenp, s->type == SOCK_STREAM ? IPPROTO_TCP
                                                                         : IPPROTO_UDP);
    case SO_ACCEPTCONN: return int_out(val, lenp, sock_state(s) == SK_LISTENING);
    case SO_ERROR:      return int_out(val, lenp, take_error(s));
    case SO_KEEPALIVE:  return int_out(val, lenp, s->opts.keepalive);
    case SO_REUSEADDR:  return int_out(val, lenp, s->opts.reuseaddr);
    case SO_BROADCAST:  return int_out(val, lenp, s->opts.broadcast);
    case SO_SNDBUF:     return int_out(val, lenp, ring_bytes(s, true));
    case SO_RCVBUF:     return int_out(val, lenp, ring_bytes(s, false));
    case SO_RCVTIMEO:   return time_out(val, lenp, s->opts.rcvtimeo);
    case SO_SNDTIMEO:   return time_out(val, lenp, s->opts.sndtimeo);
    case SO_RCVLOWAT:
    case SO_SNDLOWAT:   return int_out(val, lenp, 1);
    case SO_LINGER: {
        struct linger l = { .l_onoff = s->opts.linger_on, .l_linger = (int)s->opts.linger_s };
        return out(val, lenp, &l, sizeof(l));
    }
    case SO_REUSEPORT: case SO_OOBINLINE: case SO_DONTROUTE: case SO_PRIORITY:
    case SO_TIMESTAMP:
        return int_out(val, lenp, 0);
    }
    return -ENOPROTOOPT;
}

static long get_tcp_level(struct sock *s, int name, long val, long lenp)
{
    if (s->type != SOCK_STREAM)
        return -ENOPROTOOPT;
    switch (name) {
    case TCP_NODELAY:   return int_out(val, lenp, s->opts.nodelay);
    case TCP_KEEPIDLE:  return int_out(val, lenp, s->opts.keep_idle ? (int)s->opts.keep_idle
                                                                    : 7200);
    case TCP_KEEPINTVL: return int_out(val, lenp, s->opts.keep_intvl ? (int)s->opts.keep_intvl
                                                                     : 75);
    case TCP_KEEPCNT:   return int_out(val, lenp, s->opts.keep_cnt ? (int)s->opts.keep_cnt : 9);
    case TCP_MAXSEG:    return int_out(val, lenp, 1460);
    case TCP_CORK: case TCP_QUICKACK: case TCP_DEFER_ACCEPT: case TCP_USER_TIMEOUT:
        return int_out(val, lenp, 0);
    }
    return -ENOPROTOOPT;
}

static long get_on_sock(struct fd_desc *d, struct sock *s, long level, long name, long val,
                        long lenp)
{
    long r = -ENOPROTOOPT;
    lock_take(&d->lock);
    if (level == SOL_SOCKET)
        r = get_socket_level(s, (int)name, val, lenp);
    else if (level == IPPROTO_TCP)
        r = get_tcp_level(s, (int)name, val, lenp);
    else if (level == IPPROTO_IP && name == IP_TOS)
        r = int_out(val, lenp, 0);
    else if (level == IPPROTO_IP && name == IP_TTL)
        r = int_out(val, lenp, 64);
    lock_give(&d->lock);
    return r;
}

static long get_on_pair(struct fd_desc *d, long level, long name, long val, long lenp)
{
    if (level != SOL_SOCKET)
        return -ENOPROTOOPT;
    switch (name) {
    case SO_TYPE:     return int_out(val, lenp, sockp_type(d));
    case SO_DOMAIN:   return int_out(val, lenp, AF_UNIX);
    case SO_PROTOCOL:
    case SO_ERROR:    return int_out(val, lenp, 0);
    case SO_SNDBUF:
    case SO_RCVBUF:   return int_out(val, lenp, (int)CHANNEL_MAX_BYTES);
    }
    return -ENOPROTOOPT;
}

long lx_getsockopt(long fd, long level, long name, long val, long lenp, long f)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return -EBADF;
    long r = -ENOTSOCK;
    if (sock_of(d))
        r = get_on_sock(d, d->priv, level, name, val, lenp);
    else if (d->ops == &sockp_ops)
        r = get_on_pair(d, level, name, val, lenp);
    fd_put(d);
    return r;
}
