/* posixtest's loopback section: 127.0.0.0/8 and localhost reach sockets of
 * the same netstack, TCP and UDP, with no network peer and no card (the
 * socket section runs without netstack; this one needs it, so it is asked
 * for by name: `posixtest loopback`, tools/shell-tests/posix-loop.txt).
 *
 *   - a TCP echo: a listener bound to 127.0.0.1 (port 0), getsockname, a
 *     client connecting by address, accept (its peer is 127.0.0.1 and the
 *     client's port), getpeername, 100 KiB each way at once through the
 *     rings and windows (a thread writes each direction) checked byte by
 *     byte, the end of file after shutdown;
 *   - by name ("localhost"), and by another address of the block
 *     (127.0.0.7: the same interface, so the listener answers);
 *   - a connect to a port nobody listens on: ECONNREFUSED (a reset);
 *   - UDP: two sockets bound to 127.0.0.1, a datagram each way with the
 *     sender's address (127.0.0.1) and port, poll, MSG_PEEK, and a
 *     connected socket;
 *   - the listen permission still applies: this program's list has it, and
 *     the section says so, instead of failing, when listen is EACCES. */
#include <stdio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "pt.h"

#define LOOP   0x7f000001u
#define LOOP7  0x7f000007u
#define STREAM (100 * 1024)

static struct sockaddr_in addr_of(uint32_t ip, uint16_t port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr = { htonl(ip) } };
    return a;
}

static uint8_t pat(uint32_t i, uint8_t seed)
{
    return (uint8_t)(i * 13 + (i >> 9) * 5 + seed);
}

struct stream {
    int     fd;
    uint8_t seed;
};

/* STREAM bytes of the pattern to fd, then the end of its direction. */
static void *writer(void *arg)
{
    const struct stream *w = arg;
    uint8_t buf[4096];
    uint32_t sent = 0;
    while (sent < STREAM) {
        uint32_t n = STREAM - sent < sizeof(buf) ? STREAM - sent : (uint32_t)sizeof(buf);
        for (uint32_t i = 0; i < n; i++)
            buf[i] = pat(sent + i, w->seed);
        ssize_t k = send(w->fd, buf, n, MSG_NOSIGNAL);
        if (k <= 0)
            return (void *)1;
        sent += (uint32_t)k;
    }
    return shutdown(w->fd, SHUT_WR) == 0 ? NULL : (void *)1;
}

/* STREAM bytes of the pattern from fd, then the end of file. */
static bool read_all(int fd, uint8_t seed)
{
    static uint8_t buf[4096];
    uint32_t got = 0;
    for (;;) {
        ssize_t k = recv(fd, buf, sizeof(buf), 0);
        if (k < 0)
            return false;
        if (k == 0)
            return got == STREAM;
        for (ssize_t i = 0; i < k; i++)
            if (buf[i] != pat(got + (uint32_t)i, seed))
                return false;
        got += (uint32_t)k;
    }
}

/* A client connected to ls (by sa) and the connection accepted: *c and *s. */
static bool pair(int ls, const struct sockaddr_in *sa, int *c, int *s)
{
    *c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(*c >= 0);
    CHECK(connect(*c, (const struct sockaddr *)sa, sizeof(*sa)) == 0);
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    *s = accept(ls, (struct sockaddr *)&from, &len);
    CHECK(*s >= 0);
    CHECK(from.sin_family == AF_INET && from.sin_addr.s_addr == htonl(LOOP));
    return *c >= 0 && *s >= 0;
}

static void echo(int c, int s)
{
    struct stream wc = { c, 0x42 }, ws = { s, 0x31 };
    pthread_t tc, ts;
    void *rc = (void *)1, *rs = (void *)1;
    CHECK(pthread_create(&tc, NULL, writer, &wc) == 0);
    CHECK(pthread_create(&ts, NULL, writer, &ws) == 0);
    CHECK(read_all(c, 0x31));   /* the server's stream, then its end */
    CHECK(read_all(s, 0x42));
    CHECK(pthread_join(tc, &rc) == 0 && rc == NULL);
    CHECK(pthread_join(ts, &rs) == 0 && rs == NULL);
}

static void tcp(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = addr_of(LOOP, 0), b;
    socklen_t len = sizeof(b);
    CHECK(ls >= 0);
    CHECK(bind(ls, (struct sockaddr *)&a, sizeof(a)) == 0);
    if (listen(ls, 4) != 0) {
        printf("posixtest: loopback: listen is %s: the TCP checks are skipped\n", strerror(errno));
        CHECK(errno == EACCES);
        close(ls);
        return;
    }
    CHECK(getsockname(ls, (struct sockaddr *)&b, &len) == 0 && b.sin_family == AF_INET &&
          b.sin_addr.s_addr == htonl(LOOP) && ntohs(b.sin_port) >= 1024);
    uint16_t port = ntohs(b.sin_port);
    int c, s;
    a = addr_of(LOOP, port);
    if (pair(ls, &a, &c, &s)) {
        struct sockaddr_in me, peer, theirs;
        socklen_t l1 = sizeof(me), l2 = sizeof(peer), l3 = sizeof(theirs);
        CHECK(getsockname(c, (struct sockaddr *)&me, &l1) == 0 &&
              me.sin_addr.s_addr == htonl(LOOP));
        CHECK(getpeername(c, (struct sockaddr *)&peer, &l2) == 0 &&
              peer.sin_addr.s_addr == htonl(LOOP) && ntohs(peer.sin_port) == port);
        CHECK(getpeername(s, (struct sockaddr *)&theirs, &l3) == 0 &&
              theirs.sin_addr.s_addr == htonl(LOOP) && theirs.sin_port == me.sin_port);
        echo(c, s);
        close(c);
        close(s);
    }
    /* By name, and by another address of the block. */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    CHECK(getaddrinfo("localhost", NULL, &hints, &res) == 0 && res);
    if (res) {
        struct sockaddr_in n = *(const struct sockaddr_in *)res->ai_addr;
        n.sin_port = htons(port);
        if (pair(ls, &n, &c, &s)) {
            close(c);
            close(s);
        }
        freeaddrinfo(res);
    }
    a = addr_of(LOOP7, port);
    if (pair(ls, &a, &c, &s)) {
        close(c);
        close(s);
    }
    /* Nothing listens there. */
    close(ls);
    c = socket(AF_INET, SOCK_STREAM, 0);
    a = addr_of(LOOP, port);
    errno = 0;
    CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == ECONNREFUSED);
    close(c);
}

static int udp_bound(uint16_t *port)
{
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = addr_of(LOOP, 0), b;
    socklen_t len = sizeof(b);
    CHECK(u >= 0);
    CHECK(bind(u, (struct sockaddr *)&a, sizeof(a)) == 0);
    CHECK(getsockname(u, (struct sockaddr *)&b, &len) == 0 && b.sin_addr.s_addr == htonl(LOOP) &&
          ntohs(b.sin_port) >= 1024);
    *port = ntohs(b.sin_port);
    return u;
}

static void udp(void)
{
    uint16_t pa, pb;
    int a = udp_bound(&pa), b = udp_bound(&pb);
    struct sockaddr_in to = addr_of(LOOP, pa), from;
    socklen_t len = sizeof(from);
    char buf[32];
    struct pollfd p = { .fd = a, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 0);
    CHECK(sendto(b, "ping", 4, 0, (struct sockaddr *)&to, sizeof(to)) == 4);
    CHECK(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN));
    CHECK(recvfrom(a, buf, sizeof(buf), MSG_PEEK, NULL, NULL) == 4);
    CHECK(recvfrom(a, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len) == 4 &&
          !memcmp(buf, "ping", 4));
    CHECK(from.sin_addr.s_addr == htonl(LOOP) && ntohs(from.sin_port) == pb);
    /* The answer, to the sender's address and port. */
    CHECK(sendto(a, "pong", 4, 0, (struct sockaddr *)&from, sizeof(from)) == 4);
    len = sizeof(from);
    CHECK(recvfrom(b, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len) == 4 &&
          !memcmp(buf, "pong", 4));
    CHECK(from.sin_addr.s_addr == htonl(LOOP) && ntohs(from.sin_port) == pa);
    /* A connected socket, to another address of the block. */
    struct sockaddr_in far = addr_of(LOOP7, pa);
    CHECK(connect(b, (struct sockaddr *)&far, sizeof(far)) == 0);
    CHECK(send(b, "conn", 4, 0) == 4);
    CHECK(recv(a, buf, sizeof(buf), 0) == 4 && !memcmp(buf, "conn", 4));
    CHECK(send(a, "back", 4, 0) == -1 && errno == EDESTADDRREQ);
    struct sockaddr_in back = addr_of(LOOP, pb);
    CHECK(sendto(a, "back", 4, 0, (struct sockaddr *)&back, sizeof(back)) == 4);
    CHECK(recv(b, buf, sizeof(buf), 0) == 4 && !memcmp(buf, "back", 4));
    close(a);
    close(b);
}

void pt_loopback(char **argv)
{
    (void)argv;
    tcp();
    udp();
}
