/* posixtest's socket section: what sockets do without the network
 * (docs/M13-PLAN.md section 8), so it runs in every posixtest run; the
 * sockets section (sockets.c) talks to the network peer.
 *
 *   - socket's refusals: AF_INET6 (EAFNOSUPPORT), an AF_UNIX datagram
 *     socket and a raw socket (ESOCKTNOSUPPORT; AF_UNIX stream sockets are
 *     wayland.c's), a protocol that isn't the type's
 *     (EPROTONOSUPPORT);
 *   - a new stream socket: fstat says a socket; the read-only options
 *     (SO_TYPE, SO_DOMAIN, SO_PROTOCOL, SO_ACCEPTCONN, SO_ERROR); POSIX's
 *     defaults (TCP_NODELAY off); options set and read back (TCP_NODELAY,
 *     SO_SNDBUF as a ring's size, SO_RCVTIMEO, SO_LINGER); their
 *     refusals (ENOPROTOOPT, EINVAL, EDOM);
 *   - a socket that isn't connected: getpeername, shutdown and recv are
 *     ENOTCONN, send is EPIPE (with MSG_NOSIGNAL: no SIGPIPE), accept is
 *     EINVAL, listen on a datagram socket EOPNOTSUPP, connect to
 *     127.0.0.1 ECONNREFUSED (no loopback), bind's address checks; poll
 *     says it is writable at once;
 *   - socketpair ends through the socket calls: send and recv (and
 *     sendmsg/recvmsg with two pieces), MSG_DONTWAIT's EAGAIN, MSG_WAITALL
 *     across a writer's pieces, MSG_PEEK's EOPNOTSUPP, the names (AF_UNIX)
 *     and options;
 *   - names without the resolver: getaddrinfo of a numeric address and of
 *     "localhost" (libc's /etc/hosts), inet_pton and inet_ntop;
 *   - the raw DNS calls (res_query, and getnameinfo's reverse lookup)
 *     answer at once: the system's resolver takes names only, and musl's
 *     own client would wait out its timeout asking 127.0.0.1. */
#include <stdio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <resolv.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

/* Milliseconds of the monotonic clock. */
static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int int_opt(int fd, int level, int name)
{
    int v = -1;
    socklen_t len = sizeof(v);
    if (getsockopt(fd, level, name, &v, &len) != 0 || len != sizeof(v))
        return -1000;
    return v;
}

static void refusals(void)
{
    errno = 0;
    CHECK(socket(AF_INET6, SOCK_STREAM, 0) == -1 && errno == EAFNOSUPPORT);
    errno = 0;
    CHECK(socket(AF_UNIX, SOCK_DGRAM, 0) == -1 && errno == ESOCKTNOSUPPORT);   /* stream only */
    errno = 0;
    CHECK(socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) == -1 && errno == ESOCKTNOSUPPORT);
    errno = 0;
    CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_UDP) == -1 && errno == EPROTONOSUPPORT);
    errno = 0;
    CHECK(socket(AF_INET, SOCK_DGRAM, IPPROTO_TCP) == -1 && errno == EPROTONOSUPPORT);
}

static void options(int fd)
{
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode));
    CHECK(int_opt(fd, SOL_SOCKET, SO_TYPE) == SOCK_STREAM);
    CHECK(int_opt(fd, SOL_SOCKET, SO_DOMAIN) == AF_INET);
    CHECK(int_opt(fd, SOL_SOCKET, SO_PROTOCOL) == IPPROTO_TCP);
    CHECK(int_opt(fd, SOL_SOCKET, SO_ACCEPTCONN) == 0);
    CHECK(int_opt(fd, SOL_SOCKET, SO_ERROR) == 0);
    CHECK(int_opt(fd, IPPROTO_TCP, TCP_NODELAY) == 0);   /* POSIX's default: Nagle on */
    int one = 1, zero = 0, big = 10000;
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0);
    CHECK(int_opt(fd, IPPROTO_TCP, TCP_NODELAY) == 1);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big)) == 0);
    CHECK(int_opt(fd, SOL_SOCKET, SO_SNDBUF) == 16384);   /* the ring's size: a power of two */
    struct timeval tv = { 1, 500000 }, back = { 0, 0 };
    socklen_t len = sizeof(back);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    CHECK(getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &back, &len) == 0 && back.tv_sec == 1 &&
          back.tv_usec == 500000);
    struct linger l = { 1, 3 }, lb = { 0, 0 };
    len = sizeof(lb);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) == 0);
    CHECK(getsockopt(fd, SOL_SOCKET, SO_LINGER, &lb, &len) == 0 && lb.l_onoff && lb.l_linger == 3);
    errno = 0;
    tv.tv_usec = 1000000;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1 && errno == EDOM);
    errno = 0;
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &zero, sizeof(zero)) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, 9999, &one, sizeof(one)) == -1 && errno == ENOPROTOOPT);
    errno = 0;
    CHECK(setsockopt(fd, IPPROTO_IPV6, 1, &one, sizeof(one)) == -1 && errno == ENOPROTOOPT);
    errno = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, 2) == -1 && errno == EINVAL);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) == 0);   /* ignored */
}

static void unconnected(int fd)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    char b[8] = "x";
    errno = 0;
    CHECK(getpeername(fd, (struct sockaddr *)&a, &len) == -1 && errno == ENOTCONN);
    len = sizeof(a);
    CHECK(getsockname(fd, (struct sockaddr *)&a, &len) == 0 && len == sizeof(a) &&
          a.sin_family == AF_INET && a.sin_port == 0 && a.sin_addr.s_addr == 0);
    errno = 0;
    CHECK(shutdown(fd, SHUT_WR) == -1 && errno == ENOTCONN);
    errno = 0;
    CHECK(recv(fd, b, sizeof(b), 0) == -1 && errno == ENOTCONN);
    errno = 0;
    CHECK(send(fd, b, 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);   /* and no SIGPIPE */
    errno = 0;
    CHECK(accept(fd, NULL, NULL) == -1 && errno == EINVAL);
    struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT };
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT));
    struct sockaddr_in lo = { .sin_family = AF_INET, .sin_port = htons(80),
                              .sin_addr = { htonl(INADDR_LOOPBACK) } };
    errno = 0;
    CHECK(connect(fd, (struct sockaddr *)&lo, sizeof(lo)) == -1 && errno == ECONNREFUSED);
    errno = 0;
    CHECK(connect(fd, (struct sockaddr *)&lo, 4) == -1 && errno == EINVAL);
    struct sockaddr_in6 six = { .sin6_family = AF_INET6 };
    errno = 0;
    CHECK(bind(fd, (struct sockaddr *)&six, sizeof(six)) == -1 && errno == EAFNOSUPPORT);
    int u = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(u >= 0 && int_opt(u, SOL_SOCKET, SO_TYPE) == SOCK_DGRAM);
    CHECK(int_opt(u, SOL_SOCKET, SO_PROTOCOL) == IPPROTO_UDP);
    errno = 0;
    CHECK(listen(u, 1) == -1 && errno == EOPNOTSUPP);
    errno = 0;
    CHECK(send(u, b, 1, 0) == -1 && errno == EDESTADDRREQ);
    errno = 0;
    CHECK(getsockopt(u, IPPROTO_TCP, TCP_NODELAY, b, &len) == -1 && errno == ENOPROTOOPT);
    CHECK(fcntl(u, F_GETFD) == FD_CLOEXEC);
    CHECK(close(u) == 0);
}

/* MSG_WAITALL's writer: four pieces, 30 ms apart. */
static void *pieces(void *arg)
{
    int fd = *(int *)arg;
    for (int i = 0; i < 4; i++) {
        usleep(30000);
        char c[5] = { 'a', 'b', 'c', 'd', 'e' };
        if (send(fd, c, sizeof(c), MSG_NOSIGNAL) != sizeof(c))
            return (void *)1;
    }
    return NULL;
}

static void pair_names(int fd)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    CHECK(getsockname(fd, (struct sockaddr *)&ss, &len) == 0 && ss.ss_family == AF_UNIX);
    len = sizeof(ss);
    CHECK(getpeername(fd, (struct sockaddr *)&ss, &len) == 0 && ss.ss_family == AF_UNIX);
    CHECK(int_opt(fd, SOL_SOCKET, SO_TYPE) == SOCK_STREAM);
    CHECK(int_opt(fd, SOL_SOCKET, SO_DOMAIN) == AF_UNIX);
    CHECK(int_opt(fd, SOL_SOCKET, SO_ERROR) == 0);
}

static void pair(void)
{
    int sv[2];
    char b[32];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(send(sv[0], "hello", 5, MSG_NOSIGNAL) == 5);
    CHECK(recv(sv[1], b, sizeof(b), 0) == 5 && !memcmp(b, "hello", 5));
    errno = 0;
    CHECK(recv(sv[1], b, sizeof(b), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    errno = 0;
    CHECK(recv(sv[1], b, sizeof(b), MSG_PEEK) == -1 && errno == EOPNOTSUPP);
    struct iovec out[2] = { { "two ", 4 }, { "pieces", 6 } };
    struct msghdr m = { .msg_iov = out, .msg_iovlen = 2 };
    CHECK(sendmsg(sv[1], &m, 0) == 10);
    char x[3], y[16];
    struct iovec in[2] = { { x, sizeof(x) }, { y, sizeof(y) } };
    struct msghdr r = { .msg_iov = in, .msg_iovlen = 2 };
    CHECK(recvmsg(sv[0], &r, 0) == 10 && !memcmp(x, "two", 3) && !memcmp(y, " pieces", 7) &&
          r.msg_flags == 0);
    pthread_t t;
    void *res = (void *)1;
    CHECK(pthread_create(&t, NULL, pieces, &sv[1]) == 0);
    CHECK(recv(sv[0], b, 20, MSG_WAITALL) == 20 && !memcmp(b + 15, "abcde", 5));
    CHECK(pthread_join(t, &res) == 0 && res == NULL);
    errno = 0;
    CHECK(shutdown(sv[0], SHUT_WR) == -1 && errno == EOPNOTSUPP);
    pair_names(sv[0]);
    CHECK(close(sv[0]) == 0 && close(sv[1]) == 0);
}

static void names(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM,
                              .ai_flags = AI_NUMERICHOST };
    struct addrinfo *res = NULL;
    CHECK(getaddrinfo("10.2.21.174", "5040", &hints, &res) == 0 && res &&
          res->ai_family == AF_INET);
    if (res) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)res->ai_addr;
        CHECK(a->sin_addr.s_addr == htonl(0x0a0215aeu) && a->sin_port == htons(5040));
        freeaddrinfo(res);
    }
    hints.ai_flags = 0;
    res = NULL;
    CHECK(getaddrinfo("localhost", NULL, &hints, &res) == 0 && res);   /* libc's /etc/hosts */
    if (res) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)res->ai_addr;
        CHECK(a->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        freeaddrinfo(res);
    }
    hints.ai_family = AF_INET6;
    hints.ai_flags = AI_NUMERICHOST;
    CHECK(getaddrinfo("10.2.21.174", NULL, &hints, &res) != 0);
    struct in_addr in;
    char text[INET_ADDRSTRLEN];
    CHECK(inet_pton(AF_INET, "192.168.1.2", &in) == 1 && in.s_addr == htonl(0xc0a80102u));
    CHECK(inet_ntop(AF_INET, &in, text, sizeof(text)) && !strcmp(text, "192.168.1.2"));
}

static void raw_dns(void)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(5040),
                             .sin_addr = { htonl(0xc0000201u) } };   /* 192.0.2.1 */
    char host[64], serv[16];
    long t0 = now_ms();
    CHECK(getnameinfo((const struct sockaddr *)&a, sizeof(a), host, sizeof(host), serv,
                      sizeof(serv), NI_NUMERICSERV) == 0);
    CHECK(!strcmp(host, "192.0.2.1") && !strcmp(serv, "5040"));   /* no name: the number */
    unsigned char answer[512];
    CHECK(res_query("example.invalid", 1, 1, answer, sizeof(answer)) == -1);   /* C_IN, T_A */
    CHECK(now_ms() - t0 < 2000);   /* at once, not musl's 10 s */
}

void pt_socket(char **argv)
{
    (void)argv;
    refusals();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    if (fd >= 0) {
        options(fd);
        unconnected(fd);
        CHECK(close(fd) == 0);
    }
    pair();
    names();
    raw_dns();
}
