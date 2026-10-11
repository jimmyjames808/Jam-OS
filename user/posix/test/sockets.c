/* posixtest's sockets section: TCP and UDP against the network peer
 * (tools/posix-net-test.sh runs it: the guest at 10.2.21.5, the peer's
 * TCP servers and clients of tools/tcppeer.py, its DNS server at
 * 10.2.21.1). Only run when asked for by name (`posixtest sockets`).
 *
 *   - names: getaddrinfo and gethostbyname through /svc/dns (mac.jam, the
 *     CNAME www.jam, a name the peer says doesn't exist);
 *   - a connection: 1 MiB each way to the peer's server (port 5040), every
 *     byte checked, both names (getsockname, getpeername), the end of
 *     file after the peer's FIN;
 *   - a nonblocking connect (5041): EINPROGRESS, poll for POLLOUT,
 *     SO_ERROR 0, EISCONN, then 64 KiB each way in nonblocking writes (poll)
 *     and reads drained to EAGAIN between EPOLLET edges;
 *   - EINTR (5042): a recv cut by SIGALRM without SA_RESTART; SO_RCVTIMEO's
 *     EAGAIN; with SA_RESTART and a timeout still EINTR (never restarted);
 *     with SA_RESTART and no timeout the recv goes on and gets the bytes
 *     another thread's send and shutdown made the peer answer with;
 *   - EPIPE (5043): a send after shutdown(SHUT_WR) is EPIPE, with SIGPIPE
 *     unless MSG_NOSIGNAL, and write's too;
 *   - a listener (5045): the peer's two connections, poll on the listener,
 *     accept4, 64 KiB each way each;
 *   - poll on 32 connections at once (5046), 4 KiB each way each;
 *   - a connection handed to a child (5047): posix_spawn with it as
 *     descriptors 3 and 4 (one description: --child-sock writes on 4,
 *     closes it, reads on 3); here it is ENOTCONN after;
 *   - UDP both ways: a DNS query for mac.jam to the peer's server, its
 *     answer read with MSG_PEEK first, then FIONREAD and recvfrom, on a
 *     socket that connects too.
 * The peer counts every connection's bytes (tools/posix-net-test.sh wants
 * its 39 right and none wrong). A stream's byte i is
 * (i * 131 + (i >> 8) * 7 + seed) & 0xff, as tcppeer.py's. */
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define PEER       0x0a0215aeu   /* 10.2.21.174: the Mac's address on the peer's network */
#define GUEST      0x0a021505u   /* 10.2.21.5: ours (tools/posix-net-test.sh's settings) */
#define DNS_SERVER 0x0a021501u   /* 10.2.21.1 */
#define TX_SEED    0xa1          /* what the peer's servers want from us */
#define RX_SEED    0xb2          /* ... and send back */
#define MANY       32            /* connections polled at once */
#define WAIT_MS    30000         /* the most any one poll waits */

extern char **environ;

static uint8_t pat(unsigned seed, uint64_t i)
{
    return (uint8_t)((i * 131 + (i >> 8) * 7 + seed) & 0xff);
}

static void fill(unsigned seed, uint64_t off, uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        b[i] = pat(seed, off + i);
}

static bool same(unsigned seed, uint64_t off, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (b[i] != pat(seed, off + i))
            return false;
    return true;
}

static uint64_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static struct sockaddr_in addr_of(uint32_t ip, uint16_t port)
{
    return (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(port),
                                 .sin_addr = { htonl(ip) } };
}

/* A blocking connection to the peer's port, or -1. */
static int dial(uint16_t port)
{
    struct sockaddr_in a = addr_of(PEER, port);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        printf("posixtest: sockets: connect to port %u: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Send n bytes of the seed's stream, then SHUT_WR: true if all went. */
static bool send_all(int fd, unsigned seed, size_t n)
{
    uint8_t b[8192];
    for (size_t off = 0; off < n;) {
        size_t k = n - off < sizeof(b) ? n - off : sizeof(b);
        fill(seed, off, b, k);
        ssize_t w = send(fd, b, k, MSG_NOSIGNAL);
        if (w <= 0)
            return false;
        off += (size_t)w;
    }
    return shutdown(fd, SHUT_WR) == 0;
}

/* Read the seed's stream to the end of file: true if exactly n bytes, all right. */
static bool recv_all(int fd, unsigned seed, size_t n)
{
    uint8_t b[7000];
    size_t got = 0;
    for (;;) {
        ssize_t r = recv(fd, b, sizeof(b), 0);
        if (r < 0)
            return false;
        if (r == 0)
            return got == n;
        if (got + (size_t)r > n || !same(seed, got, b, (size_t)r))
            return false;
        got += (size_t)r;
    }
}

/* ---- names ------------------------------------------------------------------------------- */

static void names(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    CHECK(getaddrinfo("mac.jam", "http", &hints, &res) == 0 && res);
    if (res) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)res->ai_addr;
        CHECK(a->sin_addr.s_addr == htonl(PEER) && a->sin_port == htons(80));
        freeaddrinfo(res);
    }
    CHECK(getaddrinfo("nothing.jam", NULL, &hints, &res) == EAI_NONAME);
    struct hostent *h = gethostbyname("www.jam");   /* a CNAME to mac.jam */
    CHECK(h && h->h_addrtype == AF_INET && h->h_length == 4);
    if (h && h->h_addr_list[0])
        CHECK(!memcmp(h->h_addr_list[0], &(uint32_t){ htonl(PEER) }, 4));
}

/* ---- one connection, blocking ------------------------------------------------------------ */

static void bulk(void)
{
    int fd = dial(5040);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    CHECK(getpeername(fd, (struct sockaddr *)&a, &len) == 0 && a.sin_addr.s_addr == htonl(PEER) &&
          a.sin_port == htons(5040));
    len = sizeof(a);
    CHECK(getsockname(fd, (struct sockaddr *)&a, &len) == 0 &&
          a.sin_addr.s_addr == htonl(GUEST) && ntohs(a.sin_port) >= 49152);
    uint64_t t0 = ms_now();
    CHECK(send_all(fd, TX_SEED, 1 << 20));
    CHECK(recv_all(fd, RX_SEED, 1 << 20));
    char b;
    CHECK(recv(fd, &b, 1, 0) == 0);   /* the end stays the end */
    printf("posixtest: sockets: 1 MiB each way in %llu ms\n", (unsigned long long)(ms_now() - t0));
    CHECK(close(fd) == 0);
}

/* ---- a nonblocking connect --------------------------------------------------------------- */

/* Wait for events on fd: the revents, or 0 at the timeout. */
static int poll_one(int fd, short events)
{
    struct pollfd p = { .fd = fd, .events = events };
    return poll(&p, 1, WAIT_MS) == 1 ? p.revents : 0;
}

static void nonblocking(void)
{
    struct sockaddr_in a = addr_of(PEER, 5041);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EINPROGRESS);
    CHECK(poll_one(fd, POLLOUT) & POLLOUT);
    int err = -1;
    socklen_t len = sizeof(err);
    CHECK(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0);
    errno = 0;
    CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EISCONN);
    uint8_t b[4096];
    size_t off = 0, got = 0, n = 65536;
    while (off < n && (poll_one(fd, POLLOUT) & POLLOUT)) {   /* each turn sends what fits */
        fill(TX_SEED, off, b, sizeof(b));
        ssize_t w = send(fd, b, sizeof(b) < n - off ? sizeof(b) : n - off, 0);
        if (w > 0)
            off += (size_t)w;
        else if (errno != EAGAIN)
            break;
    }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP | EPOLLET }, out;
    CHECK(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0);
    CHECK(off == n && shutdown(fd, SHUT_WR) == 0);
    for (;;) {   /* drained to EAGAIN, then each edge: until the end or a failure */
        ssize_t r = recv(fd, b, sizeof(b), 0);
        if (r == -1 && errno == EAGAIN && epoll_wait(ep, &out, 1, WAIT_MS) == 1)
            continue;
        if (r <= 0 || !same(RX_SEED, got, b, (size_t)r))
            break;
        got += (size_t)r;
    }
    CHECK(got == n);
    CHECK(close(ep) == 0 && close(fd) == 0);
}

/* ---- EINTR ------------------------------------------------------------------------------- */

static volatile int alarms;

static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
}

static void alarm_in(int ms, int flags)
{
    struct sigaction sa = { .sa_handler = on_alarm, .sa_flags = flags };
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = { .it_value = { 0, ms * 1000 } };
    setitimer(ITIMER_REAL, &it, NULL);
}

static void set_rcvtimeo(int fd, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
}

/* The other thread: after a pause, our 100 bytes and the FIN (the peer
 * then answers), while the main thread is in recv. */
static void *late_sender(void *arg)
{
    usleep(600000);
    return send_all(*(int *)arg, TX_SEED, 100) ? NULL : (void *)1;
}

static void interrupted(void)
{
    int fd = dial(5042);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    char b[200];
    uint64_t t0 = ms_now();
    alarm_in(200, 0);
    errno = 0;
    CHECK(recv(fd, b, sizeof(b), 0) == -1 && errno == EINTR && alarms == 1);
    set_rcvtimeo(fd, 300);
    t0 = ms_now();
    errno = 0;
    CHECK(recv(fd, b, sizeof(b), 0) == -1 && errno == EAGAIN && ms_now() - t0 >= 250);
    set_rcvtimeo(fd, 3000);
    alarm_in(200, SA_RESTART);
    t0 = ms_now();
    errno = 0;
    CHECK(recv(fd, b, sizeof(b), 0) == -1 && errno == EINTR && alarms == 2);   /* never restarted */
    CHECK(ms_now() - t0 < 2000);
    set_rcvtimeo(fd, 0);
    pthread_t t;
    void *res = (void *)1;
    CHECK(pthread_create(&t, NULL, late_sender, &fd) == 0);
    alarm_in(200, SA_RESTART);
    CHECK(recv(fd, b, 1, MSG_WAITALL) == 1 && same(RX_SEED, 0, (uint8_t *)b, 1));   /* restarted */
    CHECK(alarms == 3);
    CHECK(pthread_join(t, &res) == 0 && res == NULL);
    CHECK(recv(fd, b, 99, MSG_WAITALL) == 99 && same(RX_SEED, 1, (uint8_t *)b, 99));
    CHECK(recv(fd, b, sizeof(b), 0) == 0);
    signal(SIGALRM, SIG_DFL);
    CHECK(close(fd) == 0);
}

/* ---- EPIPE ------------------------------------------------------------------------------- */

static volatile int pipes;

static void on_pipe(int sig)
{
    (void)sig;
    pipes++;
}

static void broken(void)
{
    int fd = dial(5043);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    struct sigaction sa = { .sa_handler = on_pipe }, old;
    sigaction(SIGPIPE, &sa, &old);
    CHECK(send_all(fd, TX_SEED, 100));
    errno = 0;
    CHECK(send(fd, "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE && pipes == 0);
    errno = 0;
    CHECK(send(fd, "x", 1, 0) == -1 && errno == EPIPE && pipes == 1);
    errno = 0;
    CHECK(write(fd, "x", 1) == -1 && errno == EPIPE && pipes == 2);
    CHECK(recv_all(fd, RX_SEED, 100));
    sigaction(SIGPIPE, &old, NULL);
    CHECK(close(fd) == 0);
}

/* ---- a listener -------------------------------------------------------------------------- */

/* One of the peer's connections, accepted: its 64 KiB to the end, ours back. */
static void serve_one(int fd, uint16_t port)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    CHECK(getsockname(fd, (struct sockaddr *)&a, &len) == 0 && a.sin_port == htons(5045) &&
          a.sin_addr.s_addr == htonl(GUEST));
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK(recv_all(fd, port & 0xff, 65536));
    CHECK(send_all(fd, (port + 1) & 0xff, 65536));
    CHECK(close(fd) == 0);
}

static void listener(void)
{
    struct sockaddr_in a = addr_of(INADDR_ANY, 5045);
    int one = 1, l = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(l >= 0 && setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    CHECK(bind(l, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(l, 4) == 0);
    int acc = 0;
    socklen_t len = sizeof(acc);
    CHECK(getsockopt(l, SOL_SOCKET, SO_ACCEPTCONN, &acc, &len) == 0 && acc == 1);
    for (int i = 0; i < 2; i++) {   /* the peer's two, one after the other */
        CHECK(poll_one(l, POLLIN) & POLLIN);
        struct sockaddr_in peer;
        len = sizeof(peer);
        int fd = accept4(l, (struct sockaddr *)&peer, &len, SOCK_CLOEXEC);
        CHECK(fd >= 0 && len == sizeof(peer) && peer.sin_addr.s_addr == htonl(PEER));
        if (fd >= 0)
            serve_one(fd, ntohs(peer.sin_port));
    }
    CHECK(close(l) == 0);
}

/* ---- many at once ------------------------------------------------------------------------ */

struct conn {
    int    fd;
    size_t got;    /* bytes read */
    bool   sent;   /* our 4 KiB and the FIN went */
    bool   done;   /* the end read, closed */
};

/* One poll's events for c: write when it can, read when there is. */
static void step(struct conn *c, short revents)
{
    uint8_t b[4096];
    if (!c->sent && (revents & POLLOUT)) {
        fill(TX_SEED, 0, b, sizeof(b));
        CHECK(send(c->fd, b, sizeof(b), 0) == sizeof(b) && shutdown(c->fd, SHUT_WR) == 0);
        c->sent = true;   /* a failure is counted; it waits for the end either way */
    }
    if (!(revents & (POLLIN | POLLHUP)))
        return;
    ssize_t r = recv(c->fd, b, sizeof(b), 0);
    if (r == -1 && errno == EAGAIN)
        return;
    CHECK(r >= 0 && c->got + (size_t)r <= sizeof(b) && same(RX_SEED, c->got, b, (size_t)r));
    if (r > 0) {
        c->got += (size_t)r;
        return;
    }
    CHECK(c->got == sizeof(b));
    c->done = true;
    close(c->fd);
}

static void many(void)
{
    struct conn cs[MANY];
    struct pollfd ps[MANY];
    struct sockaddr_in a = addr_of(PEER, 5046);
    for (int i = 0; i < MANY; i++) {
        cs[i] = (struct conn){ .fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0) };
        CHECK(cs[i].fd >= 0);
        errno = 0;
        CHECK(connect(cs[i].fd, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EINPROGRESS);
    }
    int left = MANY;
    while (left) {   /* each turn handles what one poll said */
        int n = 0;
        for (int i = 0; i < MANY; i++)
            if (!cs[i].done)
                ps[n++] = (struct pollfd){ .fd = cs[i].fd, .events = cs[i].sent ? POLLIN
                                                                              : POLLOUT };
        int r = poll(ps, (nfds_t)n, WAIT_MS);
        CHECK(r > 0);
        if (r <= 0)
            break;
        for (int k = 0, i = 0; i < MANY; i++) {
            if (cs[i].done)
                continue;
            if (ps[k].revents)
                step(&cs[i], ps[k].revents);
            left -= cs[i].done;
            k++;
        }
    }
    CHECK(left == 0);
}

/* ---- a connection for a child ------------------------------------------------------------ */

static void handed(void)
{
    int fd = dial(5047);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, 3);
    posix_spawn_file_actions_adddup2(&fa, fd, 4);   /* the same description twice: a DUP */
    CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);   /* only 3 and 4 go */
    char *const args[] = { "posixtest", "--child-sock", NULL };
    pid_t pid = -1;
    int st = -1;
    CHECK(posix_spawn(&pid, "/boot/bin/posixtest", &fa, NULL, args, environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    char b;
    errno = 0;
    CHECK(recv(fd, &b, 1, MSG_DONTWAIT) == -1 && errno == ENOTCONN);   /* it moved */
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(close(fd) == 0);
}

int pt_child_sock(void)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    bool ok = getpeername(3, (struct sockaddr *)&a, &len) == 0 && a.sin_port == htons(5047);
    len = sizeof(a);
    ok = ok && getpeername(4, (struct sockaddr *)&a, &len) == 0 && a.sin_port == htons(5047);
    ok = ok && send_all(4, TX_SEED, 4096) && close(4) == 0;   /* 3 still holds it */
    ok = ok && recv_all(3, RX_SEED, 4096);
    return ok && close(3) == 0 ? 0 : 1;
}

/* ---- UDP --------------------------------------------------------------------------------- */

/* A DNS query for mac.jam's A record, id 0x4a4d: its length. */
static size_t query(uint8_t *q)
{
    static const uint8_t head[] = { 0x4a, 0x4d, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                                    3, 'm', 'a', 'c', 3, 'j', 'a', 'm', 0, 0, 1, 0, 1 };
    memcpy(q, head, sizeof(head));
    return sizeof(head);
}

/* Is the answer to it there, with 10.2.21.174 in it? */
static bool answer(const uint8_t *r, ssize_t n)
{
    const uint8_t want[4] = { 10, 2, 21, 174 };
    if (n < 16 || r[0] != 0x4a || r[1] != 0x4d || !(r[2] & 0x80))
        return false;
    for (ssize_t i = 12; i + 4 <= n; i++)
        if (!memcmp(r + i, want, 4))
            return true;
    return false;
}

static void udp(void)
{
    uint8_t q[64], r[512];
    struct sockaddr_in dns = addr_of(DNS_SERVER, 53), from;
    socklen_t len = sizeof(from);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    set_rcvtimeo(fd, 5000);
    CHECK(sendto(fd, q, query(q), 0, (struct sockaddr *)&dns, sizeof(dns)) > 0);
    ssize_t n = recv(fd, r, sizeof(r), MSG_PEEK);
    CHECK(answer(r, n));
    int avail = 0;
    CHECK(ioctl(fd, FIONREAD, &avail) == 0 && avail == n);
    memset(r, 0, sizeof(r));
    CHECK(recvfrom(fd, r, sizeof(r), 0, (struct sockaddr *)&from, &len) == n && answer(r, n));
    CHECK(len == sizeof(from) && from.sin_addr.s_addr == htonl(DNS_SERVER) &&
          from.sin_port == htons(53));
    CHECK(recvfrom(fd, r, 4, MSG_TRUNC | MSG_DONTWAIT, NULL, NULL) == -1 && errno == EAGAIN);
    CHECK(connect(fd, (struct sockaddr *)&dns, sizeof(dns)) == 0);
    CHECK(send(fd, q, query(q), 0) > 0);
    CHECK(recv(fd, r, 4, MSG_TRUNC) == n);   /* cut to 4 bytes, its whole length said */
    struct sockaddr_in me;
    len = sizeof(me);
    CHECK(getsockname(fd, (struct sockaddr *)&me, &len) == 0 && me.sin_addr.s_addr == htonl(GUEST));
    len = sizeof(me);
    CHECK(getpeername(fd, (struct sockaddr *)&me, &len) == 0 && me.sin_port == htons(53));
    CHECK(close(fd) == 0);
}

void pt_sockets(char **argv)
{
    (void)argv;
    names();
    bulk();
    nonblocking();
    interrupted();
    broken();
    many();
    handed();
    udp();
    listener();
}
