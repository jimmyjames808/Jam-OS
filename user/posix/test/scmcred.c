/* The scm section's third file: what base's socket tests ask of a
 * socketpair beyond the descriptors (docs/CR-PLAN.md, "CBF2a notes";
 * user/posix/sys/sockpair.c), each as base does it:
 *
 *   - SO_PASSCRED (UnixDomainSocket::EnableReceiveProcessId): off, no
 *     SCM_CREDENTIALS; on, each message's, the writer's pid (the kernel's
 *     stamp) with getuid's and getgid's ids (UnixDomainSocketTest.RecvPid),
 *     before 16 descriptors in base's room (RecvPidWithMaxDescriptors,
 *     their sender here another end: base sends the socket over itself,
 *     which the kernel's cycle rule refuses), none at the end
 *     (RecvPidDisconnectedSocket), MSG_CTRUNC in too little room, an
 *     empty seqpacket message's, a stream's read and MSG_PEEK; a sender's
 *     own SCM_CREDENTIALS taken, another pid's EPERM. scmkid.c checks a
 *     child's pid (Mojo's pattern), unix.c a named connection's;
 *   - shutdown(SHUT_RDWR) wakes what sleeps on that end: a blocking read
 *     gets the end (SyncSocketTest.DisconnectTest,
 *     CancelableSyncSocketTest.ShutdownCancelsReceive), poll's 750 ms
 *     wait returns at once with POLLIN and FIONREAD 0
 *     (ShutdownCancelsReceiveWithTimeout), so does epoll_wait, and a send
 *     waiting for room gets EPIPE; half-closes the same: a read woken by
 *     its end's SHUT_RD, one on the peer by SHUT_WR's end part, a send
 *     waiting for room by SHUT_WR (its end part then owed, sent by the
 *     end's next read);
 *   - FIONREAD frees no room: a non-blocking writer that filled the
 *     channel to EAGAIN still gets EAGAIN after the reader's FIONREAD, and
 *     a read of one message makes room for one (SyncSocketTest.
 *     NonBlockingWriteTest); a seqpacket's next message is counted, an
 *     empty one 0, one with descriptors with its bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"
#include "ptscm.h"

#define WAKE_MS  750    /* base's kReceiveTimeout: a woken wait must end well before it */
#define SETTLE_MS 100   /* the sleeper's head start before the shutdown */
#define JOIN_S   5      /* a sleeper the shutdown never woke is given up after this */

static uint64_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void ms_sleep(unsigned ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}

struct scm_cred scm_recv_cred(int s, void *b, size_t n, int flags, size_t room)
{
    union {
        struct cmsghdr h;
        char           buf[SCM_CRED_ROOM];
    } c;
    struct iovec v = { b, n };
    struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1, .msg_control = room ? c.buf : NULL,
                        .msg_controllen = room };
    struct scm_cred g = { .pid = -1 };
    g.n = recvmsg(s, &m, flags);
    if (g.n < 0)
        return g;
    g.flags = m.msg_flags;
    g.controllen = m.msg_controllen;
    for (struct cmsghdr *h = m.msg_controllen ? CMSG_FIRSTHDR(&m) : NULL; h;
         h = CMSG_NXTHDR(&m, h)) {
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_CREDENTIALS &&
            h->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
            struct ucred u;
            memcpy(&u, CMSG_DATA(h), sizeof(u));
            g.pid = u.pid;
            g.uid = u.uid;
            g.gid = u.gid;
        }
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
            unsigned k = (unsigned)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (unsigned i = 0; i < k; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(h) + i * sizeof(int), sizeof(fd));
                close(fd);
            }
            g.nfds += k;
        }
    }
    return g;
}

/* ---- SO_PASSCRED ------------------------------------------------------------------------ */

/* A send of "cr" with an SCM_CREDENTIALS block of pid and our ids. */
static ssize_t send_cred(int s, pid_t pid)
{
    union {
        struct cmsghdr h;
        char           buf[CMSG_SPACE(sizeof(struct ucred))];
    } c;
    struct iovec v = { "cr", 2 };
    struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1, .msg_control = c.buf,
                        .msg_controllen = sizeof(c.buf) };
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_CREDENTIALS;
    h->cmsg_len = CMSG_LEN(sizeof(struct ucred));
    const struct ucred u = { .pid = pid, .uid = getuid(), .gid = getgid() };
    memcpy(CMSG_DATA(h), &u, sizeof(u));
    return sendmsg(s, &m, MSG_NOSIGNAL);
}

static bool ours(const struct scm_cred *g)
{
    return g->pid == getpid() && g->uid == getuid() && g->gid == getgid();
}

static int passcred_of(int s)
{
    int v = -1;
    socklen_t len = sizeof(v);
    return getsockopt(s, SOL_SOCKET, SO_PASSCRED, &v, &len) == 0 && len == sizeof(v) ? v : -1;
}

void scm_cred(void)
{
    int s[2], on = 1, other[2];
    char b[8];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, s) == 0);   /* base's CreateSocketPair */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, other) == 0);
    CHECK(passcred_of(s[0]) == 0);
    CHECK(send(s[1], "off", 3, MSG_NOSIGNAL) == 3);
    struct scm_cred g = scm_recv_cred(s[0], b, sizeof(b), 0, SCM_CRED_ROOM);
    CHECK(g.n == 3 && g.pid == -1 && g.controllen == 0);
    CHECK(setsockopt(s[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) == 0);
    CHECK(passcred_of(s[0]) == 1 && passcred_of(s[1]) == 0);
    /* RecvPid: "hello" and its NUL, a byte more of room */
    CHECK(send(s[1], "hello", 6, MSG_NOSIGNAL) == 6);
    g = scm_recv_cred(s[0], b, 7, 0, SCM_CRED_ROOM);
    CHECK(g.n == 6 && !memcmp(b, "hello", 6) && ours(&g) && !g.nfds && !(g.flags & MSG_CTRUNC));
    /* RecvPidWithMaxDescriptors: both blocks fit base's room exactly */
    int sixteen[16];
    for (unsigned i = 0; i < 16; i++)
        sixteen[i] = other[0];
    CHECK(scm_send(s[1], "hello", 6, sixteen, 16, MSG_NOSIGNAL) == 6);
    g = scm_recv_cred(s[0], b, 7, 0, SCM_CRED_ROOM);
    CHECK(g.n == 6 && ours(&g) && g.nfds == 16 && !(g.flags & MSG_CTRUNC) &&
          g.controllen == SCM_CRED_ROOM);
    /* too little room for the credentials: cut, and said */
    CHECK(send(s[1], "x", 1, MSG_NOSIGNAL) == 1);
    g = scm_recv_cred(s[0], b, sizeof(b), 0, CMSG_LEN(0));
    CHECK(g.n == 1 && g.pid == -1 && (g.flags & MSG_CTRUNC));
    /* an empty message has them; MSG_PEEK too */
    CHECK(send(s[1], "", 0, MSG_NOSIGNAL) == 0);
    g = scm_recv_cred(s[0], b, sizeof(b), 0, SCM_CRED_ROOM);
    CHECK(g.n == 0 && ours(&g));
    CHECK(send(s[1], "pk", 2, MSG_NOSIGNAL) == 2);
    g = scm_recv_cred(s[0], b, sizeof(b), MSG_PEEK, SCM_CRED_ROOM);
    CHECK(g.n == 2 && ours(&g));
    g = scm_recv_cred(s[0], b, sizeof(b), 0, SCM_CRED_ROOM);
    CHECK(g.n == 2 && ours(&g));
    /* a sender's own credentials are taken, another pid's refused */
    CHECK(send_cred(s[1], getpid()) == 2);
    g = scm_recv_cred(s[0], b, sizeof(b), 0, SCM_CRED_ROOM);
    CHECK(g.n == 2 && !memcmp(b, "cr", 2) && ours(&g));
    errno = 0;
    CHECK(send_cred(s[1], getpid() + 1) == -1 && errno == EPERM);
    /* RecvPidDisconnectedSocket: the end has none */
    close(s[1]);
    g = scm_recv_cred(s[0], b, 1, 0, SCM_CRED_ROOM);
    CHECK(g.n == 0 && g.pid == -1 && g.controllen == 0 && !g.nfds);
    close(s[0]);
    /* a stream: a read's bytes have them, MSG_PEEK's too; read() is as ever */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    CHECK(setsockopt(s[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) == 0);
    CHECK(write(s[1], "abcd", 4) == 4);
    g = scm_recv_cred(s[0], b, 2, MSG_PEEK, SCM_CRED_ROOM);
    CHECK(g.n == 2 && ours(&g));
    CHECK(read(s[0], b, 1) == 1 && b[0] == 'a');
    g = scm_recv_cred(s[0], b, sizeof(b), 0, SCM_CRED_ROOM);
    CHECK(g.n == 3 && !memcmp(b, "bcd", 3) && ours(&g));
    int off = 0;
    CHECK(setsockopt(s[0], SOL_SOCKET, SO_PASSCRED, &off, sizeof(off)) == 0);
    CHECK(write(s[1], "e", 1) == 1);
    g = scm_recv_cred(s[0], b, sizeof(b), 0, SCM_CRED_ROOM);
    CHECK(g.n == 1 && g.pid == -1);
    int all[] = { s[0], s[1], other[0], other[1] };
    for (unsigned i = 0; i < 4; i++)
        close(all[i]);
}

/* ---- shutdown's wakes ---------------------------------------------------------------------- */

enum how { BY_READ, BY_POLL, BY_EPOLL, BY_SEND };

/* A thread asleep on s one way, and what it came back with. */
struct sleeper {
    int       s;
    enum how  how;
    long      r;
    int       err;
    short     revents;
    int       peek;      /* BY_POLL: FIONREAD after the poll (base's Peek) */
    uint64_t  ms;        /* how long the wait took */
    pthread_t t;
};

static void *sleep_on(void *arg)
{
    struct sleeper *z = arg;
    char b[64] = { 0 };
    uint64_t t0 = ms_now();
    errno = 0;
    switch (z->how) {
    case BY_READ:
        z->r = read(z->s, b, sizeof(b));
        break;
    case BY_POLL: {
        struct pollfd pf = { z->s, POLLIN, 0 };
        z->r = poll(&pf, 1, WAKE_MS);
        z->revents = pf.revents;
        break;
    }
    case BY_EPOLL: {
        int ep = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event e = { .events = EPOLLIN };
        z->r = ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, z->s, &e) ? -2 : epoll_wait(ep, &e, 1, WAKE_MS);
        z->revents = (short)e.events;
        if (ep >= 0)
            close(ep);
        break;
    }
    case BY_SEND:   /* the channel is full: the send waits for room */
        z->r = send(z->s, b, sizeof(b), MSG_NOSIGNAL);
        break;
    }
    z->err = errno;
    z->ms = ms_now() - t0;
    if (z->how == BY_POLL && ioctl(z->s, FIONREAD, &z->peek))
        z->peek = -1;
    return NULL;
}

/* z asleep on its end, then fd shut down (how) from here: false if it
 * didn't come back within JOIN_S seconds (it is left to the process's
 * end). */
static bool shut_under(struct sleeper *z, int fd, int how)
{
    z->r = -3;
    if (pthread_create(&z->t, NULL, sleep_on, z))
        return false;
    ms_sleep(SETTLE_MS);   /* most likely asleep by now; either order must work */
    CHECK(shutdown(fd, how) == 0);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += JOIN_S;
    if (pthread_timedjoin_np(z->t, NULL, &until)) {
        printf("posixtest: scm: a sleeper (how %d) not woken by shutdown\n", (int)z->how);
        return false;
    }
    return true;
}

void scm_wake(void)
{
    /* one each, kept: a sleeper never woken still writes into its own */
    static struct sleeper rd, pl, ep, sd, hr, hw, hs;
    static char drain[65536];
    int s[2];
    /* DisconnectTest and ShutdownCancelsReceive: the reader's own end shut */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    rd = (struct sleeper){ .s = s[0], .how = BY_READ };
    CHECK(shut_under(&rd, rd.s, SHUT_RDWR) && rd.r == 0);
    close(s[0]);
    close(s[1]);
    /* ShutdownCancelsReceiveWithTimeout: poll ends at once, nothing to read */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    pl = (struct sleeper){ .s = s[1], .how = BY_POLL };
    CHECK(shut_under(&pl, pl.s, SHUT_RDWR) && pl.r == 1 && (pl.revents & POLLIN) && pl.peek == 0 &&
          pl.ms < WAKE_MS - SETTLE_MS);
    close(s[0]);
    close(s[1]);
    /* epoll_wait the same */
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, s) == 0);
    ep = (struct sleeper){ .s = s[0], .how = BY_EPOLL };
    CHECK(shut_under(&ep, ep.s, SHUT_RDWR) && ep.r == 1 && (ep.revents & EPOLLIN) && ep.ms < WAKE_MS - SETTLE_MS);
    close(s[0]);
    close(s[1]);
    /* a send waiting for room: EPIPE */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    char *chunk = calloc(1, 65536);
    for (int i = 0; chunk && i < 1024; i++)
        if (send(s[0], chunk, 65536, MSG_DONTWAIT | MSG_NOSIGNAL) <= 0)
            break;
    free(chunk);
    sd = (struct sleeper){ .s = s[0], .how = BY_SEND };
    CHECK(shut_under(&sd, sd.s, SHUT_RDWR) && sd.r == -1 && sd.err == EPIPE);
    close(s[0]);
    close(s[1]);
    /* half-closes: a read woken by its own end's SHUT_RD, one on the peer by SHUT_WR's end
     * part */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    hr = (struct sleeper){ .s = s[0], .how = BY_READ };
    CHECK(shut_under(&hr, s[0], SHUT_RD) && hr.r == 0);
    close(s[0]);
    close(s[1]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    hw = (struct sleeper){ .s = s[1], .how = BY_READ };
    CHECK(shut_under(&hw, s[0], SHUT_WR) && hw.r == 0);
    close(s[0]);
    close(s[1]);
    /* a send waiting for room, woken by SHUT_WR: EPIPE; the end part found no room, so the
     * end's next read sends it */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    for (int i = 0; i < 1024; i++)
        if (send(s[0], drain, sizeof(drain), MSG_DONTWAIT | MSG_NOSIGNAL) <= 0)
            break;
    hs = (struct sleeper){ .s = s[0], .how = BY_SEND };
    CHECK(shut_under(&hs, s[0], SHUT_WR) && hs.r == -1 && hs.err == EPIPE);
    long n;
    do
        n = recv(s[1], drain, sizeof(drain), MSG_DONTWAIT);
    while (n > 0);
    CHECK(n == -1 && errno == EAGAIN);
    errno = 0;
    CHECK(recv(s[0], drain, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    CHECK(recv(s[1], drain, 1, MSG_DONTWAIT) == 0);
    close(s[0]);
    close(s[1]);
}

/* ---- FIONREAD ------------------------------------------------------------------------------- */

static int fionread(int s)
{
    int n = -1;
    return ioctl(s, FIONREAD, &n) ? -1 : n;
}

void scm_fionread(void)
{
    static const char hello[] = "Hello, SyncSocket Client";   /* base's kHelloString */
    int s[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    /* CancelableSyncSocket::Send: O_NONBLOCK for the send, put back after */
    int fl = fcntl(s[0], F_GETFL);
    CHECK(fl >= 0 && fcntl(s[0], F_SETFL, fl | O_NONBLOCK) == 0);
    unsigned sent = 0;
    while (sent < 100000 && write(s[0], hello, sizeof(hello)) == (ssize_t)sizeof(hello))
        sent++;
    CHECK(sent > 0 && errno == EAGAIN);
    int n = fionread(s[1]);
    CHECK(n == (int)sizeof(hello));
    errno = 0;
    CHECK(write(s[0], hello, sizeof(hello)) == -1 && errno == EAGAIN);   /* FIONREAD freed nothing */
    CHECK(fionread(s[1]) == n);
    char b[sizeof(hello)];
    CHECK(read(s[1], b, sizeof(b)) == (ssize_t)sizeof(b) && !memcmp(b, hello, sizeof(b)));
    CHECK(write(s[0], hello, sizeof(hello)) == (ssize_t)sizeof(hello));   /* room for one */
    CHECK(fcntl(s[0], F_SETFL, fl) == 0);
    close(s[0]);
    close(s[1]);
    /* a seqpacket's next message, an empty one, one with a descriptor */
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, s) == 0);
    CHECK(fionread(s[0]) == 0);
    CHECK(send(s[1], "abc", 3, 0) == 3 && send(s[1], "", 0, 0) == 0);
    CHECK(fionread(s[0]) == 3 && fionread(s[0]) == 3);
    CHECK(recv(s[0], b, sizeof(b), 0) == 3);
    CHECK(fionread(s[0]) == 0 && recv(s[0], b, sizeof(b), MSG_DONTWAIT) == 0);   /* the empty one */
    errno = 0;
    CHECK(recv(s[0], b, sizeof(b), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    int p[2];
    CHECK(pipe(p) == 0 && scm_send(s[1], "zz", 2, &p[0], 1, 0) == 2);
    CHECK(fionread(s[0]) == 2);
    struct scm_got g = { .n = 0 };
    CHECK(scm_recv(s[0], b, sizeof(b), 4, 0, &g) == 2 && g.n == 1);
    scm_close_got(&g);
    int all[] = { s[0], s[1], p[0], p[1] };
    for (unsigned i = 0; i < 4; i++)
        close(all[i]);
}
