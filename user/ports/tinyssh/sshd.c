/* sshd: the SSH server's listener (docs/M13-PLAN.md section 15, "X2
 * notes"), bin/sshd's default mode (tinysshd.c), a POSIX program that init
 * starts as a service (user/services/init/sshd.c) once /data is mounted,
 * netstack runs and /data/etc/authorized_keys holds a key.
 *
 * It listens on port 22 (its list's `svc net listen low`) and, for each
 * connection, starts one tinysshd (bin/sshd as "tinysshd") with the
 * connection as descriptors 0 and 1, as inetd would: tinyssh is written for
 * that. At most SSHD_MAX run at once; one more is closed at once. Each
 * gets:
 *   - TCP_NODELAY (an interactive session's keys go out at once: POSIX's
 *     default is Nagle's algorithm) and SO_KEEPALIVE (a peer gone without
 *     a word is found), set here before the spawn: options move with the
 *     socket;
 *   - a namespace of /data read-only (the host key, authorized_keys) and
 *     /svc/session (init's sessions: what a login gets), nothing else;
 *   - descriptor 2 the write end of one log pipe they all share, whose
 *     other end this loop reads and copies to its own 2, the kernel log (a
 *     child can't be given the log itself: the layer's log descriptor
 *     stays its process's). Each tinyssh log line is one write, so lines
 *     never mix.
 * The listener and every accepted connection are FD_CLOEXEC: a socket
 * handed to a child moves to it, and one left without would go to every
 * child.
 *
 * The loop must outlive its connections: netstack ends a process's
 * sockets when it exits, the ones its children were given among them. It
 * never ends by itself; it exits 1 when the listener fails (netstack gone)
 * and init starts it again. SIGCHLD ends poll's wait; each turn reaps the
 * children that ended and says so in the log. */
#include <stdarg.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/spawn.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "helper.h"

#define SSHD_PORT   22
#define SSHD_MAX    8                  /* connections at once (init's sessions: 8 too) */
#define SSHD_KEYDIR "/data/etc/ssh"    /* the host key (init makes it) */
#define LOG_BUF     4096               /* a log pipe read: one or more whole lines */

extern char **environ;

static pid_t kids[SSHD_MAX];   /* the tinysshd processes running (0: a free slot) */
static int   nkids;

/* One line into the log, "sshd: " first, in one write (a line of the log
 * is never split by another writer's). */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    static const char prefix[] = "sshd: ";
    char line[256];
    memcpy(line, prefix, sizeof(prefix) - 1);
    size_t at = sizeof(prefix) - 1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + at, sizeof(line) - at - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    at += (size_t)n < sizeof(line) - at - 2 ? (size_t)n : sizeof(line) - at - 2;
    line[at++] = '\n';
    (void)!write(2, line, at);
}

/* SIGCHLD's handler: only there so that poll's wait ends (the loop reaps). */
static void on_child(int sig)
{
    (void)sig;
}

/* The listener on port 22, FD_CLOEXEC and non-blocking; -1 (said) if not. */
static int listen_22(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        say("no socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    /* A restarted sshd takes the port back at once, past the old one's
     * connections in TIME_WAIT. */
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(SSHD_PORT) };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, SSHD_MAX) < 0) {
        say("can't listen on port %d: %s", SSHD_PORT, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* One tinysshd for connection c (ours, still open: the caller closes it),
 * its log on logw. 0 and *pid, or an errno value. */
static int start_tinysshd(int c, int logw, pid_t *pid)
{
    static const char *const grants[] = { "/data:r", "/svc/session", NULL };
    char *argv[] = { "tinysshd", "-v", SSHD_KEYDIR, NULL };
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    int e = posix_spawn_file_actions_init(&fa);
    if (e)
        return e;
    e = posix_spawnattr_init(&at);
    if (!e)
        e = posix_spawn_file_actions_adddup2(&fa, c, 0);
    if (!e)
        e = posix_spawn_file_actions_adddup2(&fa, c, 1);
    if (!e)
        e = posix_spawn_file_actions_adddup2(&fa, logw, 2);
    if (!e)
        e = posix_spawnattr_setgrants_np(&at, grants);
    if (!e)
        e = posix_spawn(pid, SSHD_SELF, &fa, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return e;
}

/* A connection waits on the listener: take it, and start its tinysshd. */
static void take_one(int lfd, int logw)
{
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    int c = accept4(lfd, (struct sockaddr *)&peer, &len, SOCK_CLOEXEC);
    if (c < 0) {
        if (errno != EAGAIN && errno != EINTR && errno != ECONNABORTED) {
            say("the listener failed: %s", strerror(errno));
            _exit(1);   /* netstack is gone: init starts us again */
        }
        return;
    }
    char who[INET_ADDRSTRLEN + 8];
    char ip[INET_ADDRSTRLEN] = "?";
    (void)inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    snprintf(who, sizeof(who), "%s:%u", ip, ntohs(peer.sin_port));
    if (nkids >= SSHD_MAX) {
        say("%s refused: %d connections already", who, SSHD_MAX);
        close(c);
        return;
    }
    int one = 1;
    if (setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0 ||
        setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) < 0)
        say("%s: no TCP_NODELAY or keep-alive: %s", who, strerror(errno));
    pid_t pid = 0;
    int e = start_tinysshd(c, logw, &pid);
    close(c);   /* the child has it now (or nobody: a spawn that failed) */
    if (e) {
        say("%s: tinysshd didn't start: %s", who, strerror(e));
        return;
    }
    for (int i = 0; i < SSHD_MAX; i++) {
        if (!kids[i]) {
            kids[i] = pid;
            break;
        }
    }
    nkids++;
    say("%s: connection, tinysshd %d (%d open)", who, (int)pid, nkids);
}

/* Every child that has ended, reaped and said. */
static void reap(void)
{
    for (;;) {
        int status;
        pid_t p = waitpid(-1, &status, WNOHANG);
        if (p <= 0)
            return;
        for (int i = 0; i < SSHD_MAX; i++) {
            if (kids[i] == p) {
                kids[i] = 0;
                nkids--;
                break;
            }
        }
        if (WIFEXITED(status))
            say("tinysshd %d ended with %d (%d open)", (int)p, WEXITSTATUS(status), nkids);
        else
            say("tinysshd %d was killed (%d open)", (int)p, nkids);
    }
}

/* What the children wrote to the log pipe, into our own log. */
static void copy_log(int logr)
{
    char buf[LOG_BUF];
    ssize_t n = read(logr, buf, sizeof(buf));
    if (n > 0)
        (void)!write(2, buf, (size_t)n);
}

int sshd_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct sigaction sa = { .sa_handler = on_child };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        say("no SIGCHLD handler: ended connections are seen every 5 s");
    int logp[2];
    if (pipe2(logp, O_CLOEXEC | O_NONBLOCK) < 0) {
        say("no log pipe: %s", strerror(errno));
        return 1;
    }
    int lfd = listen_22();
    if (lfd < 0)
        return 1;
    say("listening on port %d (at most %d connections)", SSHD_PORT, SSHD_MAX);
    for (;;) {
        struct pollfd p[2] = { { .fd = lfd, .events = POLLIN }, { .fd = logp[0], .events = POLLIN } };
        int n = poll(p, 2, 5000);
        int e = errno;   /* before reap's waitpid sets it (ECHILD once none is left) */
        reap();
        if (n < 0 && e != EINTR) {
            say("poll failed: %s", strerror(e));
            return 1;
        }
        if (n <= 0)
            continue;
        if (p[1].revents & POLLIN)
            copy_log(logp[0]);
        if (p[0].revents & (POLLIN | POLLERR | POLLHUP))
            take_one(lfd, logp[1]);
    }
}
