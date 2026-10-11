/* Sockets (docs/M13-PLAN.md section 8): AF_INET stream and datagram
 * sockets over netstack, as a descriptor kind (fd.h). libjam's <net.h>
 * makes them and moves their bytes through the socket rings
 * (<sockring.h>); this layer gives them POSIX's names and answers. The
 * files:
 *
 *   socket.c   the kind, socket, bind and listen, the drain at exit
 *   sockconn.c connect, accept, shutdown, the names
 *   sockgive.c a socket handed to a child, and a child's from its hand-over
 *   sockio.c   the bytes: read and write, the send and recv family
 *   sockwait.c the one way this layer sleeps on a socket (sock_wait)
 *   sockopt.c  setsockopt and getsockopt
 *   resolve.c  names through /svc/dns (the lookup_name override's call)
 *
 * **A socket's life.** socket() makes only the description (SK_NEW):
 * netstack hears of it at the first call that needs it. A datagram socket
 * is opened at bind, connect or its first send (SK_DGRAM); a stream one
 * becomes a listener at listen (SK_LISTENING) or a connection at connect
 * (SK_STREAM, whose status line says CONNECTING, OPEN or CLOSED); accept
 * makes a new SK_STREAM description. A socket handed to a child moves
 * there (its ring VMO can't be duplicated, <sockring.h>) and is SK_GONE
 * here: every call on it is ENOTCONN.
 *
 * **Openers.** Every socket is made on one of the program's kept channels
 * to netstack (svc_get): /svc/net (or net-sys) for connections, datagram
 * sockets on ports from NET_PORT_EPHEMERAL up and port 0; /svc/net-listen
 * for listeners and fixed ports from NET_PORT_LOW up; /svc/net-low below
 * that. A program whose list didn't ask for the one a call needs gets
 * EACCES. netstack ends every socket of an opener that goes, so a socket
 * lives no longer than the process that made it (a child given one keeps
 * it only while that process lives).
 *
 * **Locks**, in this order: the description's lock (d->lock: the state,
 * the options, a listener's accept), then rx_lock (the rx ring's
 * consumer side, the datagram kept by MSG_PEEK), then tx_lock (the tx
 * ring's producer side). None is held while a thread sleeps. The state
 * moves forward under d->lock and is read with an acquire load; the
 * rings are touched only under their side's lock with the state checked
 * there, so a socket handed away (which takes all three) is never
 * touched again.
 *
 * **Waits** (sockwait.c's sock_wait): a thread that must sleep binds the
 * socket's to_prog event and its channel's end on a port of its own,
 * raises its side's `waits` flag and looks again before it sleeps, so
 * several threads may wait on one socket and each is woken. A wait ends
 * with EINTR on a signal, and with a never-restarted EINTR
 * (LX_EINTR_FINAL) when the socket has a timeout for it (SO_RCVTIMEO,
 * SO_SNDTIMEO), as Linux never restarts those. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include "fd.h"

#include <net.h>

#define SOCK_LINGER_NS (10ull * NS_PER_S)   /* close's and exit's wait for unsent bytes */

/* What a socket is now (struct sock's state). */
enum sock_state {
    SK_NEW = 0,        /* socket(): nothing at netstack yet */
    SK_LISTENING,      /* a stream socket listening: l */
    SK_STREAM,         /* a connection: ns (its status line: CONNECTING, OPEN, CLOSED) */
    SK_DGRAM,          /* a datagram socket at netstack: ns */
    SK_GONE,           /* handed to a child: nothing left here */
};

/* The options a program set (sockopt.c), applied where they belong. */
struct sock_opts {
    bool     nodelay;      /* TCP_NODELAY (POSIX's default: off, Nagle's algorithm on) */
    bool     keepalive;    /* SO_KEEPALIVE */
    bool     reuseaddr;    /* SO_REUSEADDR: a listener's NET_LISTEN_REUSE */
    bool     broadcast;    /* SO_BROADCAST: kept for getsockopt (netstack sends no broadcast) */
    bool     linger_on;    /* SO_LINGER's l_onoff */
    uint32_t linger_s;     /* ... and l_linger, seconds */
    uint32_t keep_idle;    /* TCP_KEEPIDLE, s (0: netstack's default) */
    uint32_t keep_intvl;   /* TCP_KEEPINTVL, s (0: the default) */
    uint32_t keep_cnt;     /* TCP_KEEPCNT (0: the default) */
    uint32_t sndbuf;       /* SO_SNDBUF: the tx ring's bytes (0: netstack's default) */
    uint32_t rcvbuf;       /* SO_RCVBUF: the rx ring's bytes (0: the default) */
    uint64_t rcvtimeo;     /* SO_RCVTIMEO, ns (0: none) */
    uint64_t sndtimeo;     /* SO_SNDTIMEO, ns (0: none) */
};

/* A socket: an open file description's state (d->priv). */
struct sock {
    int                 type;        /* SOCK_STREAM or SOCK_DGRAM */
    uint32_t            state;       /* enum sock_state; d->lock, read atomically */
    bool                bound;       /* bind() was called (bind_addr, bind_port) */
    uint32_t            bind_addr;   /* host order; 0: any */
    uint16_t            bind_port;   /* 0: netstack picks */
    struct net_sock     ns;          /* SK_STREAM, SK_DGRAM: the socket at netstack */
    struct net_listener l;           /* SK_LISTENING */
    struct sock_opts    opts;        /* d->lock */
    lock_t              rx_lock;     /* below, the rx side */
    lock_t              tx_lock;     /* below, the tx side */
    uint32_t            sleepers[2]; /* threads asleep on the rx [0] and tx [1] side; that
                                      * side's lock: its `waits` flag is up while any is */
    bool                rd_shut;     /* shutdown(SHUT_RD): reads see the end; rx_lock */
    bool                wr_shut;     /* shutdown(SHUT_WR), or a connection that ended:
                                      * tx_lock */
    bool                err_said;    /* a CLOSED connection's error was returned once */
    int                 error;       /* SO_ERROR waiting: an errno, 0 none; d->lock */
    bool                connected;   /* a datagram socket's peer is set (peer_*) */
    uint32_t            peer_addr;   /* ... host order */
    uint16_t            peer_port;
    uint64_t            refused;     /* a datagram socket's tx records refused, as seen; tx_lock */
    struct net_dgram   *kept;        /* a datagram MSG_PEEK took off the ring; rx_lock */
    struct sock        *next;        /* every stream: socket.c's list, for the drain at exit */
};

/* The kind (socket.c). */
extern const struct fd_ops sock_ops;

/* d's socket if d is one of ours, else NULL. */
static inline struct sock *sock_of(struct fd_desc *d)
{
    return d->ops == &sock_ops ? d->priv : NULL;
}

/* The state now (an acquire load). */
static inline uint32_t sock_state(const struct sock *s)
{
    return __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
}

/* The hand-over (sockgive.c): the kind's give (a socket moves to the
 * child), and its kinds as fd.c's fd_adopters take them, a child's
 * POSIXFDS_SOCKET and POSIXFDS_LISTENER descriptors. */
long        sock_give(struct fd_desc *d, struct fd_given *out);
fd_adopt_fn sock_adopt, sock_listener_adopt;

/* ---- sockio.c, sockwait.c ---------------------------------------------------------------- */

/* The kind's read and write (fd.h's): recv and send with no flags. */
long sock_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos);
long sock_write(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos);

/* Which side of a socket a wait is for. */
enum sock_side { SIDE_RX = 0, SIDE_TX = 1 };

/* One try of an operation: its result, or -EAGAIN when it can't go on yet. */
typedef long sock_try_fn(struct sock *s, void *arg);

/* Run try until it returns anything but -EAGAIN, sleeping on side between
 * tries until netstack signals it (bytes, room, a change of state),
 * netstack ends, or the deadline. -EAGAIN at once if nonblock, or at the
 * deadline; -EINTR on a signal (LX_EINTR_FINAL with timed: a wait with a
 * socket timeout is never restarted). */
long sock_wait(struct sock *s, enum sock_side side, sock_try_fn *try, void *arg, bool nonblock,
               uint64_t deadline, bool timed);

/* A deadline of timeout ns from now (0: DEADLINE_NEVER). */
uint64_t sock_deadline(uint64_t timeout);

/* A CLOSED connection's error as an errno (0: it closed cleanly). */
int sock_closed_errno(status_t err);

/* ---- socket.c ---------------------------------------------------------------------------- */

/* Move the state on (a release store: sock_state's readers see what came
 * before it). d->lock held, or a socket nobody else sees yet. */
void sock_state_set(struct sock *s, enum sock_state st);
/* An SK_STREAM socket on the list the drain at exit walks, and off it
 * (before it is closed or handed away; never under its locks). */
void sock_streams_add(struct sock *s);
void sock_streams_remove(struct sock *s);
/* netstack's answer to a call that makes or binds a socket, as -errno. */
long sock_net_errno(status_t st);
/* Descriptor fd's description (held: the caller puts it) and its socket:
 * 0; -EBADF; -ENOTSOCK for another kind, with *d still set and held for a
 * socketpair end (the caller answers for it and puts it), else NULL. */
long sock_get(long fd, struct fd_desc **d, struct sock **s);

/* The opener a call on port needs (NET_* rules above): the kept channel,
 * or HANDLE_INVALID (the program's list didn't ask for it). listen: the
 * listen permission whatever the port. */
handle_t sock_opener(uint16_t port, bool listen);

/* An AF_INET address from the caller (addr, len bytes) as host-order
 * numbers: 0, -EFAULT (NULL), -EAFNOSUPPORT (another family), -EINVAL
 * (too short). */
long sock_addr_in(long addr, long len, uint32_t *ip, uint16_t *port);
/* Write one out to addr, *lenp bytes at most (the whole length said back
 * in *lenp, as Linux does): 0, or -EINVAL for a negative *lenp. Nothing
 * to do when addr or lenp is NULL. */
long sock_addr_out(long addr, long lenp, uint32_t ip, uint16_t port);

/* Apply the options to a connection netstack made (net_tcp_set_options):
 * 0 or -errno. */
long sock_apply_tcp(struct sock *s);

/* A datagram socket at netstack now (opened on its bound port, or one
 * netstack picks): 0 or -errno. d->lock held. */
long sock_dgram_open_locked(struct sock *s);

/* ---- sockopt.c --------------------------------------------------------------------------- */

/* A power-of-two ring size for a SO_SNDBUF or SO_RCVBUF value, inside
 * SOCKRING_MIN..SOCKRING_MAX (0 stays 0: the default). */
uint32_t sock_ring_size(uint64_t asked);
