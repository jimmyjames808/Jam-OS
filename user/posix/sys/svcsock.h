/* AF_UNIX stream sockets and the service sockets they connect to
 * (svcsock.c; docs/G2-PLAN.md section 1). Jam OS has no named Unix
 * sockets: a path a program connects to is looked up in a small table of
 * **service sockets**, each a Jam OS service (/svc/<name>, svc_open) and
 * the framing that turns the socket's byte stream and its SCM_RIGHTS
 * descriptors into that service's channel messages and handles. One row
 * today: /svc/wayland, framed as JWL2 batches (wlsock.c, <jwl.h>), so
 * libwayland talks to the compositor unchanged. Any other path is ENOENT,
 * as a missing socket file is; so is a row whose service the program's
 * list didn't ask for (`svc wayland`). bind and listen are EAFNOSUPPORT.
 * A later service socket (a D-Bus broker) is a new row with a framing of
 * its own, not a new mechanism.
 *
 * **A socket's life.** socket(AF_UNIX, SOCK_STREAM) makes the description
 * unconnected (US_NEW); connect opens the row's service, with no lock held
 * while svc_open waits (US_CONNECTING: another connect is EALREADY, the
 * other calls ENOTCONN), then hands the channel to the framing's open
 * and the socket is US_CONNECTED for good: the state only moves forward,
 * published with a release store after the framing's state is in place,
 * so a call that reads US_CONNECTED (an acquire load) finds it whole. A
 * connected socket is never handed to a child (a connection is one
 * process's: posix_spawn leaves it out, as FD_CLOEXEC would).
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
