/* Named AF_UNIX stream sockets: bind, listen, accept and connect on
 * socket files and abstract names, through the broker /svc/unix
 * (abi/idl/unix.idl has its model; docs/CR-PLAN.md section 4, track CU2).
 * svcsock.c keeps the socket's state and calls these; svcsock.h has the
 * model of an AF_UNIX socket's life.
 *
 * **A socket file** is an ordinary file of UNIX_FILE_BYTES: the magic,
 * then the key the broker answered bind with. bind makes it with O_EXCL
 * (anything at the path: EADDRINUSE), mode 0777 less the umask, as Linux
 * makes a socket inode; connect opens it for reading as any open does
 * (ENOENT, EACCES, a view that hides it: the open's answer) and gives the
 * broker the key it read. A file that isn't a socket file, or whose
 * listener is gone (a stale socket), is ECONNREFUSED, as Linux answers a
 * connect to a file that isn't a socket or one nobody listens on. Linux
 * asks for write permission on the socket file; reading it is the test
 * here (what the namespace's views decide). It is a plain file to every
 * other call: stat says S_IFREG, not S_IFSOCK.
 *
 * **An abstract name** (sun_path starting with a NUL byte) is the bytes
 * after it, up to the address's length, in the broker's one table.
 *
 * **A connection** is a channel pair the broker makes: the connector's
 * socket gets one end (a socketpair end inside it: unixpair_framing below
 * passes every call to it, SCM_RIGHTS and shutdown included), the
 * listener gets the other as a `conn` event, which accept makes into a
 * plain socketpair end's descriptor. A connect never waits for accept:
 * the bytes wait in the channel, as on Linux. A full backlog is EAGAIN
 * (Linux makes a blocking connect wait for room). getsockname and
 * getpeername of a connection say AF_UNIX with no path; of a bound socket,
 * getsockname says its name. */
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <idl/unix.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "calls.h"
#include "kinds.h"
#include "pipe.h"
#include "svcsock.h"

#define CALL_NS (10 * NS_PER_S)   /* the most a call to the broker may take */

struct ulisten {
    handle_t ch;                 /* the listener's channel (unix.idl) */
    struct unix_name name;       /* what it was bound to */
};

/* ---- names --------------------------------------------------------------------------- */

long unix_name_of(long addr, long len, struct unix_name *n)
{
    struct sockaddr_un a;   /* the caller's first len bytes */
    size_t at = offsetof(struct sockaddr_un, sun_path);
    if (!addr)
        return -EFAULT;
    if (len < (long)sizeof(sa_family_t) || len > (long)sizeof(a))
        return -EINVAL;
    if (uc_get(&a, (const void *)(uintptr_t)addr, (size_t)len))
        return -EFAULT;
    if (a.sun_family != AF_UNIX)
        return -EINVAL;
    size_t room = (size_t)len > at ? (size_t)len - at : 0;
    memset(n, 0, sizeof(*n));
    if (room && !a.sun_path[0]) {   /* abstract: every byte after the NUL */
        n->abstract = true;
        n->len = (uint32_t)(room - 1);
        memcpy(n->bytes, a.sun_path + 1, n->len);
        return n->len ? 0 : -EINVAL;   /* autobind (an empty name): not built */
    }
    n->len = (uint32_t)strnlen(a.sun_path, room);
    memcpy(n->bytes, a.sun_path, n->len);   /* bytes[len] stays NUL: a C string */
    return n->len ? 0 : -EINVAL;
}

long unix_name_out(const struct unix_name *n, long addr, long lenp)
{
    socklen_t len;   /* the caller's room, said back */
    if (!addr || !lenp || uc_get(&len, (const void *)(uintptr_t)lenp, sizeof(len)))
        return -EFAULT;
    if ((int)len < 0)
        return -EINVAL;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    size_t at = offsetof(struct sockaddr_un, sun_path), full = at;
    if (n && n->abstract) {
        memcpy(a.sun_path + 1, n->bytes, n->len);   /* sun_path[0] stays NUL */
        full = at + 1 + n->len;
    } else if (n) {
        memcpy(a.sun_path, n->bytes, n->len);
        full = at + n->len + 1;   /* with its NUL, as Linux */
        if (full > sizeof(a))
            full = sizeof(a);     /* a path of all 108 bytes has no room for one */
    }
    socklen_t said = (socklen_t)full;
    if (uc_put((void *)(uintptr_t)addr, &a, len < full ? len : full) ||
        uc_put((void *)(uintptr_t)lenp, &said, sizeof(said)))
        return -EFAULT;
    return 0;
}

/* ---- the broker ---------------------------------------------------------------------- */

/* A broker call's status as an errno (no /svc/unix in our namespace is the
 * callers' own: EACCES for bind and a socket file, ECONNREFUSED for an
 * abstract name). */
static long broker_errno(status_t st)
{
    switch (st) {
    case ERR_NOT_FOUND:      return -ECONNREFUSED;
    case ERR_ALREADY_EXISTS: return -EADDRINUSE;
    case ERR_NO_RESOURCES:   return -ENOBUFS;
    case ERR_SHOULD_WAIT:    return -EAGAIN;
    case ERR_TIMED_OUT:
    case ERR_PEER_CLOSED:    return -ECONNREFUSED;
    }
    return lx_err(st);
}

/* /svc/unix (libos keeps it), or HANDLE_INVALID: not in our namespace. */
static handle_t broker(void)
{
    return svc_get(SVC_UNIX);
}

static status_t bind_call(const struct unix_name *n, handle_t *l, uint8_t key[UNIX_KEY_BYTES])
{
    const uint8_t *name = n->abstract ? (const uint8_t *)n->bytes : NULL;
    uint32_t len = n->abstract ? n->len : 0;
    status_t st = ERR_PEER_CLOSED;
    for (int tries = 0; tries < 2 && st == ERR_PEER_CLOSED; tries++) {   /* once more: restarted */
        handle_t b = broker();
        if (!b)
            return ERR_ACCESS_DENIED;
        st = unix_bind_within(b, CALL_NS, name, len, l, key);
    }
    return st;
}

static status_t connect_call(const struct unix_name *n, const uint8_t key[UNIX_KEY_BYTES],
                             handle_t *ch)
{
    const uint8_t *name = n && n->abstract ? (const uint8_t *)n->bytes : NULL;
    uint32_t len = n && n->abstract ? n->len : 0;
    status_t st = ERR_PEER_CLOSED;
    for (int tries = 0; tries < 2 && st == ERR_PEER_CLOSED; tries++) {
        handle_t b = broker();
        if (!b)
            return ERR_ACCESS_DENIED;
        st = unix_connect_within(b, CALL_NS, name, len, key, ch);
    }
    return st;
}

/* ---- socket files -------------------------------------------------------------------- */

static void file_bytes(uint8_t out[UNIX_FILE_BYTES], const uint8_t key[UNIX_KEY_BYTES])
{
    uint64_t magic = UNIX_FILE_MAGIC;
    memcpy(out, &magic, sizeof(magic));
    memcpy(out + sizeof(magic), key, UNIX_KEY_BYTES);
}

/* Make the socket file at path holding key: 0, or -errno. */
static long file_make(const char *path, const uint8_t key[UNIX_KEY_BYTES])
{
    long fd = lx_openat(AT_FDCWD, (long)(uintptr_t)path,
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0777, 0, 0);
    if (fd < 0)
        return fd == -EEXIST ? -EADDRINUSE : fd;
    uint8_t b[UNIX_FILE_BYTES];
    file_bytes(b, key);
    long r = lx_write(fd, (long)(uintptr_t)b, sizeof(b), 0, 0, 0);
    long c = lx_close(fd, 0, 0, 0, 0, 0);
    if (r == (long)sizeof(b) && !c)
        return 0;
    (void)lx_unlinkat(AT_FDCWD, (long)(uintptr_t)path, 0, 0, 0, 0);
    return r < 0 ? r : c < 0 ? c : -EIO;
}

/* The key in the socket file at path: 0, or -errno (ECONNREFUSED: not a
 * socket file). */
static long file_key(const char *path, uint8_t key[UNIX_KEY_BYTES])
{
    long fd = lx_openat(AT_FDCWD, (long)(uintptr_t)path, O_RDONLY | O_CLOEXEC, 0, 0, 0);
    if (fd < 0)
        return fd;
    uint8_t b[UNIX_FILE_BYTES + 1], want[UNIX_FILE_BYTES];
    long r = lx_read(fd, (long)(uintptr_t)b, sizeof(b), 0, 0, 0);
    (void)lx_close(fd, 0, 0, 0, 0, 0);   /* a read-only open: nothing to lose */
    if (r != UNIX_FILE_BYTES)
        return -ECONNREFUSED;   /* a directory, an empty file, anything else */
    memcpy(key, b + sizeof(uint64_t), UNIX_KEY_BYTES);
    file_bytes(want, key);
    return memcmp(b, want, sizeof(want)) ? -ECONNREFUSED : 0;
}

/* ---- listeners ----------------------------------------------------------------------- */

long ulisten_bind(const struct unix_name *n, struct ulisten **out)
{
    struct ulisten *l = jam_calloc(1, sizeof(*l));
    if (!l)
        return -ENOMEM;
    l->name = *n;
    uint8_t key[UNIX_KEY_BYTES];
    status_t st = bind_call(n, &l->ch, key);
    long r = st == ERR_ACCESS_DENIED ? -EACCES : st != OK ? broker_errno(st) : 0;
    if (!r && !n->abstract && (r = file_make(n->bytes, key)) != 0)
        jam_handle_close(l->ch);   /* the broker drops the listener */
    if (r) {
        jam_free(l);
        return r;
    }
    *out = l;
    return 0;
}

long ulisten_listen(struct ulisten *l, int backlog)
{
    uint16_t b = backlog < 1                     ? 1
                 : backlog > (int)UNIX_BACKLOG_MAX ? UNIX_BACKLOG_MAX
                                                   : (uint16_t)backlog;
    status_t st = unix_listen_within(l->ch, CALL_NS, b);
    return st == OK ? 0 : st == ERR_PEER_CLOSED ? -ECONNABORTED : broker_errno(st);
}

static void got_conn(void *ctx, handle_t end)
{
    *(handle_t *)ctx = end;
}

long ulisten_accept(struct ulisten *l, bool nb, handle_t *ch)
{
    static const struct unix_events ev = { .conn = got_conn };
    for (;;) {
        *ch = HANDLE_INVALID;
        status_t st = unix_read_event(l->ch, &ev, ch);
        if (st == OK && *ch)
            return 0;
        if (st == ERR_PEER_CLOSED)
            return -ECONNABORTED;   /* the broker is gone: so is the listener */
        if (st != ERR_SHOULD_WAIT)
            continue;   /* something that isn't a connection: dropped */
        if (nb)
            return -EAGAIN;
        long r = pipe_wait(l->ch, SIG_READABLE);
        if (r)
            return r;
    }
}

long ulisten_arm(struct ulisten *l, struct fd_wait *out)
{
    out->kind = FD_WAIT_HANDLE;
    out->h = (struct netwait_handle){ .h = l->ch, .read = SIG_READABLE, .hup = SIG_PEER_CLOSED };
    return 0;
}

const struct unix_name *ulisten_name(const struct ulisten *l)
{
    return &l->name;
}

void ulisten_close(struct ulisten *l)
{
    jam_handle_close(l->ch);   /* the broker drops it; the socket file stays, as on Linux */
    jam_free(l);
}

/* ---- connections --------------------------------------------------------------------- */

void *unamed_connect(const struct unix_name *n, long *r)
{
    uint8_t key[UNIX_KEY_BYTES] = { 0 };
    *r = n->abstract ? 0 : file_key(n->bytes, key);
    if (*r)
        return NULL;
    handle_t ch = HANDLE_INVALID;
    status_t st = connect_call(n->abstract ? n : NULL, key, &ch);
    if (st != OK) {
        *r = st == ERR_ACCESS_DENIED ? (n->abstract ? -ECONNREFUSED : -EACCES)
                                     : broker_errno(st);
        return NULL;
    }
    void *conn = unixpair_framing.open(ch);
    *r = conn ? 0 : -ENOMEM;
    return conn;
}

long unix_accepted(handle_t ch, long flags, bool passcred)
{
    struct pipe_end *p = pipe_end_new(ch, NULL, 0);
    struct fd_desc *d = p ? sockp_desc_new(p, O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0),
                                           false)
                          : NULL;
    if (!d)
        return -ENOMEM;
    sockp_set_passcred(d, passcred);
    return fd_install(d, 0, flags & SOCK_CLOEXEC);
}

/* The framing of a connection made by connect: a socketpair end's
 * description inside the socket's, every call passed to it (with
 * MSG_DONTWAIT when the socket's own description is O_NONBLOCK). */

static void *pair_open(handle_t ch)
{
    struct pipe_end *p = pipe_end_new(ch, NULL, 0);   /* consumes ch */
    return p ? sockp_desc_new(p, O_RDWR, false) : NULL;
}

static int nb_of(struct fd_desc *d, int flags)
{
    return flags | (fd_status(d) & O_NONBLOCK ? MSG_DONTWAIT : 0);
}

static long pair_sendmsg(struct fd_desc *d, void *conn, const struct msghdr *m, int flags)
{
    return sockp_sendmsg(conn, m, nb_of(d, flags));
}

static long pair_recvmsg(struct fd_desc *d, void *conn, struct msghdr *m, int flags)
{
    return sockp_recvmsg(conn, m, nb_of(d, flags));
}

static uint32_t pair_ready(void *conn, uint32_t interest)
{
    struct fd_desc *in = conn;
    return in->ops->ready(in, interest);
}

static long pair_arm(void *conn, uint32_t interest, struct fd_wait *out)
{
    struct fd_desc *in = conn;
    return in->ops->arm(in, interest, out);
}

static void pair_close(void *conn)
{
    fd_put(conn);
}

const struct svcsock_framing unixpair_framing = {
    .name = "unix",
    .open = pair_open,
    .sendmsg = pair_sendmsg,
    .recvmsg = pair_recvmsg,
    .ready = pair_ready,
    .arm = pair_arm,
    .close = pair_close,
};

long unixpair_give(struct fd_desc *d, void *conn, struct fd_given *out)
{
    struct fd_desc *in = conn;
    long r = in->ops->give(in, out);
    if (!r)
        out->status = (out->status & ~(uint32_t)O_NONBLOCK) | (fd_status(d) & O_NONBLOCK);
    return r;
}

long unixpair_shutdown(void *conn, int how)
{
    return sockp_shutdown(conn, how);
}
