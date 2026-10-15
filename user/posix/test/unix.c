/* posixtest's unix section: named AF_UNIX sockets through the broker
 * /svc/unix (user/posix/sys/unixsock.c, svcsock.c, user/services/unix;
 * docs/CR-PLAN.md section 4, CU2), socket files in the files section's
 * directory on /data:
 *
 *   - a listener and two connectors (children started by posix_spawn),
 *     each connection answered; getsockname of the listener says its
 *     path; poll says the listener is readable while one waits;
 *   - bytes and a descriptor (SCM_RIGHTS: a pipe end) over a connection,
 *     shutdown(SHUT_RDWR) seen by the other side as the end of the file;
 *   - the backlog: listen(1) takes one connection waiting, the next is
 *     EAGAIN until accept; accept on a non-blocking listener EAGAIN;
 *   - names: a second bind of the path EADDRINUSE, a file that isn't a
 *     socket ECONNREFUSED, the socket file removed (connect ENOENT, the
 *     listener's waiting connection still accepted), a stale socket file
 *     (its listener closed: ECONNREFUSED) unlinked and bound again; a
 *     path of all 108 bytes, whose getsockname is the whole struct;
 *   - views: a child whose namespace has no /data can't reach the socket
 *     file (ENOENT), one with /data read-only can (reading the file is
 *     the test, its view decides);
 *   - abstract names: bound, reached, taken (EADDRINUSE), free again once
 *     the listener closes. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <poll.h>
#include <posixfds.h>
#include <spawn.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ptfiles.h"

#define SELF          "/boot/bin/posixtest"
#define SOCK          PT_DIR "/sock"
#define CHILD_WAIT_NS (60 * NS_PER_S)

extern char **environ;

static socklen_t path_addr(struct sockaddr_un *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strncpy(a->sun_path, path, sizeof(a->sun_path) - 1);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
}

static socklen_t abstract_addr(struct sockaddr_un *a, const char *name)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path + 1, name, strlen(name));   /* sun_path[0] is the NUL */
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
}

/* A socket connected to the path (or abstract name): its descriptor, or
 * -1 with errno. */
static int connect_to(const struct sockaddr_un *a, socklen_t len)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (const struct sockaddr *)a, len) == 0)
        return fd;
    int e = errno;
    close(fd);
    errno = e;
    return -1;
}

/* A listener at a (bound, listening with backlog): its descriptor, or -1. */
static int listen_at(const struct sockaddr_un *a, socklen_t len, int backlog)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (bind(fd, (const struct sockaddr *)a, len) == 0 && listen(fd, backlog) == 0)
        return fd;
    int e = errno;
    close(fd);
    errno = e;
    return -1;
}

/* Accept one and answer its byte with the byte + 1. */
static void serve_one(int l)
{
    int c = accept(l, NULL, NULL);
    CHECK(c >= 0);
    if (c < 0)
        return;
    unsigned char b = 0;
    CHECK(read(c, &b, 1) == 1);
    b++;
    CHECK(write(c, &b, 1) == 1);
    close(c);
}

/* ---- the children ------------------------------------------------------------------------ */

/* posixtest --child-unix connect <path> <byte>: connect, send the byte,
 * expect it + 1 back. --child-unix refused <path>: connect must be ENOENT
 * (the path isn't in our namespace). Exit 0 if all of it held. */
int pt_child_unix(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    struct sockaddr_un a;
    socklen_t len = path_addr(&a, argv[3]);
    if (!strcmp(argv[2], "refused")) {
        errno = 0;
        int fd = connect_to(&a, len);
        return fd < 0 && errno == ENOENT ? 0 : 3;
    }
    if (strcmp(argv[2], "connect") || argc < 5)
        return 2;
    int fd = connect_to(&a, len);
    if (fd < 0)
        return 4;
    unsigned char b = (unsigned char)atoi(argv[4]), back = 0;
    if (write(fd, &b, 1) != 1)
        return 5;
    if (read(fd, &back, 1) != 1 || back != (unsigned char)(b + 1))
        return 6;
    close(fd);
    return 0;
}

static pid_t spawn_connector(const char *path, const char *byte)
{
    char *const argv[] = { "posixtest", "--child-unix", "connect", (char *)path, (char *)byte,
                           NULL };
    pid_t pid = -1;
    CHECK(posix_spawn(&pid, SELF, NULL, NULL, argv, environ) == 0);
    return pid;
}

static int reap(pid_t pid)
{
    int status = 0;
    if (pid <= 0 || waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* This program started by libjam's spawn in namespace ns (no hand-over
 * but the current directory), in mode with the path: its process, or 0. */
static handle_t spawn_in(const char *const *ns, const char *mode, const char *path)
{
    const struct posixfds_out ho = { .state = { .umask = 022 }, .cwd = "/" };
    const char *const argv[] = { "posixtest", "--child-unix", mode, path, "7" };
    struct spawn_args a = {
        .path = "bin/posixtest", .argc = 5, .argv = argv, .job = startup_handle(SR_JOB),
        .ns = ns, .handover = &ho,
    };
    handle_t proc = HANDLE_INVALID;
    status_t st = spawn(&a, &proc);
    CHECK(st == OK);
    return st == OK ? proc : HANDLE_INVALID;
}

static int spawned_exit(handle_t proc)
{
    if (!proc)
        return -1;
    struct process_info info;
    status_t st = spawn_wait(proc, CHILD_WAIT_NS, &info);
    if (st != OK)
        (void)jam_process_kill(proc);
    jam_handle_close(proc);
    return st == OK && !info.killed ? (int)info.exit_code : -1;
}

/* ---- the checks -------------------------------------------------------------------------- */

static void listener_and_two(void)
{
    struct sockaddr_un a;
    socklen_t len = path_addr(&a, SOCK);
    int l = listen_at(&a, len, 4);
    CHECK(l >= 0);
    if (l < 0)
        return;
    struct sockaddr_un got;
    socklen_t glen = sizeof(got);
    CHECK(getsockname(l, (struct sockaddr *)&got, &glen) == 0 && got.sun_family == AF_UNIX &&
          !strcmp(got.sun_path, SOCK));
    struct stat st;
    CHECK(stat(SOCK, &st) == 0 && S_ISREG(st.st_mode));   /* a plain file here (unixsock.c) */
    pid_t one = spawn_connector(SOCK, "10"), two = spawn_connector(SOCK, "20");
    struct pollfd p = { .fd = l, .events = POLLIN };
    CHECK(poll(&p, 1, 10000) == 1 && (p.revents & POLLIN));
    serve_one(l);
    serve_one(l);
    CHECK(reap(one) == 0);
    CHECK(reap(two) == 0);
    close(l);
    CHECK(unlink(SOCK) == 0);
}

static void bytes_and_fds(void)
{
    struct sockaddr_un a;
    socklen_t len = path_addr(&a, SOCK);
    int l = listen_at(&a, len, 2);
    int c = connect_to(&a, len);
    int s = l >= 0 ? accept4(l, NULL, NULL, SOCK_CLOEXEC) : -1;
    CHECK(l >= 0 && c >= 0 && s >= 0);
    if (l < 0 || c < 0 || s < 0)
        goto out;
    struct sockaddr_un peer;
    socklen_t plen = sizeof(peer);
    CHECK(getpeername(c, (struct sockaddr *)&peer, &plen) == 0 && peer.sun_family == AF_UNIX);
    /* A pipe's write end, with one byte, from the connector to the listener's side. */
    int pp[2];
    CHECK(pipe(pp) == 0);
    char byte = 'x', cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec v = { &byte, 1 };
    struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1, .msg_control = cbuf,
                        .msg_controllen = sizeof(cbuf) };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &pp[1], sizeof(int));
    CHECK(sendmsg(c, &m, MSG_NOSIGNAL) == 1);
    close(pp[1]);
    char in = 0, rbuf[CMSG_SPACE(sizeof(int))];
    struct iovec rv = { &in, 1 };
    struct msghdr r = { .msg_iov = &rv, .msg_iovlen = 1, .msg_control = rbuf,
                        .msg_controllen = sizeof(rbuf) };
    CHECK(recvmsg(s, &r, 0) == 1 && in == 'x');
    struct cmsghdr *rc = CMSG_FIRSTHDR(&r);
    int wfd = -1;
    if (rc && rc->cmsg_type == SCM_RIGHTS)
        memcpy(&wfd, CMSG_DATA(rc), sizeof(int));
    CHECK(wfd >= 0 && write(wfd, "y", 1) == 1);
    char y = 0;
    CHECK(read(pp[0], &y, 1) == 1 && y == 'y');
    if (wfd >= 0)
        close(wfd);
    close(pp[0]);
    /* The other way, and non-blocking reads on the connector. */
    CHECK(write(s, "ab", 2) == 2);
    char two[2];
    CHECK(recv(c, two, 2, MSG_WAITALL) == 2 && !memcmp(two, "ab", 2));
    CHECK(recv(c, two, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    /* shutdown on the connector: the other side reads the end. */
    CHECK(shutdown(c, SHUT_RDWR) == 0);
    CHECK(read(s, two, 1) == 0);
out:
    if (s >= 0)
        close(s);
    if (c >= 0)
        close(c);
    if (l >= 0)
        close(l);
    (void)unlink(SOCK);
}

static void backlog(void)
{
    struct sockaddr_un a;
    socklen_t len = path_addr(&a, SOCK);
    int l = listen_at(&a, len, 1);
    CHECK(l >= 0);
    if (l < 0)
        return;
    CHECK(fcntl(l, F_SETFL, O_NONBLOCK) == 0);
    CHECK(accept(l, NULL, NULL) == -1 && errno == EAGAIN);
    int c1 = connect_to(&a, len);
    CHECK(c1 >= 0);
    errno = 0;
    CHECK(connect_to(&a, len) == -1 && errno == EAGAIN);   /* its one waits for accept */
    int s1 = accept(l, NULL, NULL);
    CHECK(s1 >= 0);
    int c2 = connect_to(&a, len);
    CHECK(c2 >= 0);
    int s2 = accept(l, NULL, NULL);
    CHECK(s2 >= 0);
    int fds[] = { c1, s1, c2, s2, l };
    for (unsigned i = 0; i < 5; i++)
        if (fds[i] >= 0)
            close(fds[i]);
    (void)unlink(SOCK);
}

static void names(void)
{
    struct sockaddr_un a;
    socklen_t len = path_addr(&a, SOCK);
    int l = listen_at(&a, len, 4);
    CHECK(l >= 0);
    errno = 0;
    CHECK(listen_at(&a, len, 4) == -1 && errno == EADDRINUSE);
    /* A file that isn't a socket file. */
    int f = open(PT_DIR "/plain", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(f >= 0 && write(f, "not a socket, not 24 bytes", 26) == 26);
    if (f >= 0)
        close(f);
    struct sockaddr_un p;
    socklen_t plen = path_addr(&p, PT_DIR "/plain");
    errno = 0;
    CHECK(connect_to(&p, plen) == -1 && errno == ECONNREFUSED);
    /* Removed: nobody new reaches it; what was waiting is still accepted. */
    int c = connect_to(&a, len);
    CHECK(c >= 0);
    CHECK(unlink(SOCK) == 0);
    errno = 0;
    CHECK(connect_to(&a, len) == -1 && errno == ENOENT);
    int s = l >= 0 ? accept(l, NULL, NULL) : -1;
    CHECK(s >= 0 && write(c, "z", 1) == 1);
    char z = 0;
    CHECK(s >= 0 && read(s, &z, 1) == 1 && z == 'z');
    if (s >= 0)
        close(s);
    if (c >= 0)
        close(c);
    if (l >= 0)
        close(l);
    /* Stale: the file left behind by a listener that closed. */
    l = listen_at(&a, len, 4);
    CHECK(l >= 0);
    if (l >= 0)
        close(l);
    errno = 0;
    CHECK(connect_to(&a, len) == -1 && errno == ECONNREFUSED);
    errno = 0;
    CHECK(listen_at(&a, len, 4) == -1 && errno == EADDRINUSE);
    CHECK(unlink(SOCK) == 0);
    l = listen_at(&a, len, 4);
    CHECK(l >= 0);
    if (l >= 0)
        close(l);
    (void)unlink(SOCK);
}

/* A path of all of sun_path's 108 bytes, with no NUL: getsockname gives
 * the whole struct, 110 bytes, as Linux does (never one past it). */
static void long_path(void)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
    size_t dir = strlen(PT_DIR "/");
    memcpy(path, PT_DIR "/", dir);
    memset(path + dir, 'l', sizeof(path) - 1 - dir);
    path[sizeof(path) - 1] = '\0';
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, path, sizeof(a.sun_path));   /* no room for the NUL */
    int l = listen_at(&a, sizeof(a), 4);
    CHECK(l >= 0);
    struct {
        struct sockaddr_un a;
        char               more[8];   /* room for a length past the struct's */
    } got;
    socklen_t glen = sizeof(got);
    CHECK(l >= 0 && getsockname(l, (struct sockaddr *)&got, &glen) == 0 &&
          glen == sizeof(struct sockaddr_un) && !memcmp(got.a.sun_path, path, sizeof(a.sun_path)));
    if (l >= 0)
        close(l);
    (void)unlink(path);
}

static void views(void)
{
    struct sockaddr_un a;
    socklen_t len = path_addr(&a, SOCK);
    int l = listen_at(&a, len, 4);
    CHECK(l >= 0);
    if (l < 0)
        return;
    const char *const none[] = { "/svc/" SVC_UNIX, "/boot", NULL };
    CHECK(spawned_exit(spawn_in(none, "refused", SOCK)) == 0);   /* no /data: no file */
    const char *const ro[] = { "/data:r", "/svc/" SVC_UNIX, "/boot", NULL };
    handle_t proc = spawn_in(ro, "connect", SOCK);
    if (proc)
        serve_one(l);
    CHECK(spawned_exit(proc) == 0);
    close(l);
    (void)unlink(SOCK);
}

static void abstract(void)
{
    struct sockaddr_un a;
    socklen_t len = abstract_addr(&a, "posixtest-unix");
    int l = listen_at(&a, len, 4);
    CHECK(l >= 0);
    errno = 0;
    CHECK(listen_at(&a, len, 4) == -1 && errno == EADDRINUSE);
    struct sockaddr_un got;
    socklen_t glen = sizeof(got);
    CHECK(getsockname(l, (struct sockaddr *)&got, &glen) == 0 && glen == len &&
          !got.sun_path[0] && !memcmp(got.sun_path + 1, "posixtest-unix", 14));
    int c = connect_to(&a, len);
    int s = l >= 0 ? accept(l, NULL, NULL) : -1;
    CHECK(c >= 0 && s >= 0 && write(c, "q", 1) == 1);
    char q = 0;
    CHECK(s >= 0 && read(s, &q, 1) == 1 && q == 'q');
    if (s >= 0)
        close(s);
    if (c >= 0)
        close(c);
    if (l >= 0)
        close(l);
    errno = 0;
    CHECK(connect_to(&a, len) == -1 && errno == ECONNREFUSED);   /* gone with its listener */
    l = listen_at(&a, len, 4);   /* and free again */
    CHECK(l >= 0);
    if (l >= 0)
        close(l);
    struct sockaddr_un other;
    socklen_t olen = abstract_addr(&other, "posixtest-none");
    errno = 0;
    CHECK(connect_to(&other, olen) == -1 && errno == ECONNREFUSED);
}

void pt_unix(char **argv)
{
    (void)argv;
    if (!svc_get(SVC_UNIX)) {   /* libc's own kept channel: opened here first */
        printf("posixtest: unix: skipped: no /svc/unix in this namespace\n");
        return;
    }
    if (!pt_files_start()) {
        CHECK(!"no /data to test on");
        return;
    }
    listener_and_two();
    bytes_and_fds();
    backlog();
    names();
    long_path();
    views();
    abstract();
    pt_files_end();
}
