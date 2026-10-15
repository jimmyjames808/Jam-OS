/* AF_UNIX stream sockets and the service sockets they connect to
 * (svcsock.c; docs/G2-PLAN.md section 1). A path a program connects to is
 * first looked up in a small table of **service sockets**, each a Jam OS
 * service (/svc/<name>, svc_open) and the framing that turns the socket's
 * byte stream and its SCM_RIGHTS descriptors into that service's channel
 * messages and handles. One row today: /svc/wayland, framed as JWL2
 * batches (wlsock.c, <jwl.h>), so libwayland talks to the compositor
 * unchanged; a row whose service the program's list didn't ask for (`svc
 * wayland`) is ENOENT, as a missing socket file is. A later service socket
 * (a D-Bus broker) is a new row with a framing of its own, not a new
 * mechanism. Every other name is a **named socket** (CU2: unixsock.c,
 * below), through the broker /svc/unix (`svc unix`).
 *
 * **A socket's life.** socket(AF_UNIX, SOCK_STREAM) makes the description
 * unconnected (US_NEW); connect opens the row's service, or the named
 * socket's connection, with no lock held while it waits (US_CONNECTING:
 * another connect is EALREADY, the other calls ENOTCONN), then hands the
 * channel to the framing's open and the socket is US_CONNECTED for good:
 * the state only moves forward, published with a release store after the
 * framing's state is in place, so a call that reads US_CONNECTED (an
 * acquire load) finds it whole. bind moves US_NEW to US_BOUND (US_BINDING
 * while the broker and the socket file are asked, no lock held), listen
 * US_BOUND to US_LISTENING; a bound socket can't connect (EINVAL). A
 * service socket's connection is never handed to a child (a connection is
 * one process's: posix_spawn leaves it out, as FD_CLOEXEC would), nor a
 * listener; a named socket's connection is handed on as the socketpair
 * end it holds.
 *
 * **Locks.** The description's lock (d->lock) guards the framing's state
 * for one call's bookkeeping, never across a wait; ready (poll) reads
 * only what the framing stores atomically. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include "fd.h"

/* How a service socket's bytes and descriptors become channel messages.
 * Every hook but open and close is called with a reference to d held
 * and no lock taken, and answers Linux's way (a value, or -errno). */
struct svcsock_framing {
    const char *name;    /* for log lines: "wayland" */
    /* The framing's state for a connection on the channel end ch
     * (consumed, whatever happens); NULL if out of memory. */
    void    *(*open)(handle_t ch);
    /* sendmsg and recvmsg on the connection (flags already checked
     * against what a socket takes; MSG_DONTWAIT or O_NONBLOCK: never wait). */
    long     (*sendmsg)(struct fd_desc *d, void *conn, const struct msghdr *m, int flags);
    long     (*recvmsg)(struct fd_desc *d, void *conn, struct msghdr *m, int flags);
    /* poll's hooks, as fd.h's ready and arm. */
    uint32_t (*ready)(void *conn, uint32_t interest);
    long     (*arm)(void *conn, uint32_t interest, struct fd_wait *out);
    /* The last reference went: close the channel and every handle held. */
    void     (*close)(void *conn);
};

/* The Wayland socket's framing (wlsock.c). */
extern const struct svcsock_framing wlsock_framing;

/* ---- the calls that reach an AF_UNIX stream socket (socket.c, sockconn.c, sockio.c) ---- */

/* socket(AF_UNIX, type, proto): a new unconnected socket's descriptor, or
 * -errno (-ESOCKTNOSUPPORT for anything but SOCK_STREAM, -EPROTONOSUPPORT
 * for a protocol). */
long usock_new(long type, long proto);
/* connect of d (an AF_UNIX stream socket's, held by the caller) to the
 * sockaddr_un at addr, len bytes: 0, or -errno (ENOENT: no such service
 * socket, or the service isn't in our namespace). */
long usock_connect(struct fd_desc *d, long addr, long len);
/* Descriptor fd's description, held (the caller fd_puts it), if it is an
 * AF_UNIX stream socket; else NULL (nothing held). */
struct fd_desc *usock_get(long fd);
/* sendmsg and recvmsg on d, an AF_UNIX stream socket (held by the
 * caller), flags already checked as a socket's: Linux's answers. */
long usock_sendmsg(struct fd_desc *d, const struct msghdr *m, int flags);
long usock_recvmsg(struct fd_desc *d, struct msghdr *m, int flags);
/* bind, listen, accept4, shutdown and the names (getsockname, peer:
 * getpeername) of d, an AF_UNIX stream socket held by the caller:
 * Linux's answers (accept: the new descriptor). */
long usock_bind(struct fd_desc *d, long addr, long len);
long usock_listen(struct fd_desc *d, long backlog);
long usock_accept(struct fd_desc *d, long addr, long lenp, long flags);
long usock_shutdown(struct fd_desc *d, int how);
long usock_name(struct fd_desc *d, long addr, long lenp, bool peer);

/* ---- named sockets: socket files and abstract names (unixsock.c) ----------------------
 *
 * A socket that isn't connected to a service socket is a named one: bind
 * and listen make it a listener of the broker's (/svc/unix, unix.idl),
 * connect makes it a connection to one, framed by unixpair_framing (a
 * socketpair end inside: everything sockpair.c does, SCM_RIGHTS
 * included); accept makes plain socketpair ends. unixsock.c's header has
 * what a socket file is. These are called with no lock held (each may
 * wait for the broker). */

/* A sockaddr_un's name: a path (bytes NUL-terminated) or an abstract name. */
struct unix_name {
    bool     abstract;
    uint32_t len;          /* bytes of it, without the path's NUL */
    char     bytes[109];
};

struct ulisten;   /* a listener: its channel and its name */

extern const struct svcsock_framing unixpair_framing;

/* The name in the sockaddr_un at addr (len bytes): 0, or -errno
 * (EINVAL: not AF_UNIX, an empty path, an empty abstract name). */
long unix_name_of(long addr, long len, struct unix_name *n);
/* Write name n (NULL: none, AF_UNIX alone) at addr for getsockname and
 * accept, its length to *lenp: 0, or -errno. */
long unix_name_out(const struct unix_name *n, long addr, long lenp);
/* A new listener bound to n, not listening yet: 0, or -errno
 * (EADDRINUSE: the name is taken, or a file is at the path). */
long ulisten_bind(const struct unix_name *n, struct ulisten **out);
long ulisten_listen(struct ulisten *l, int backlog);
/* The next connection's channel end into *ch, waiting for one unless nb:
 * 0, or -errno (EAGAIN, EINTR, ECONNABORTED: the broker is gone). */
long ulisten_accept(struct ulisten *l, bool nb, handle_t *ch);
/* poll's arm for a listener: its channel, readable while one waits. */
long ulisten_arm(struct ulisten *l, struct fd_wait *out);
const struct unix_name *ulisten_name(const struct ulisten *l);
void ulisten_close(struct ulisten *l);
/* A connection to the listener named n, as unixpair_framing's state, or
 * NULL with *r its errno. */
void *unamed_connect(const struct unix_name *n, long *r);
/* accept's new descriptor on the channel end ch (consumed) with accept4's
 * flags: the descriptor, or -errno. */
long unix_accepted(handle_t ch, long flags);
/* A connection's give (posix_spawn, SCM_RIGHTS: a socketpair end's) and
 * shutdown (a socketpair end's). */
long unixpair_give(struct fd_desc *d, void *conn, struct fd_given *out);
long unixpair_shutdown(void *conn, int how);
