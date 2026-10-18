/* posixtest's scm section: socketpairs that carry descriptors (SCM_RIGHTS;
 * docs/CR-PLAN.md section 4, CU1), against the layer's sockpair.c:
 *
 *   - every kind that crosses, passed and used on the other side: a pipe
 *     end written, a memfd mapped and written (the sender sees it), a file
 *     read at the sender's offset, /dev/zero read, a socketpair end and a
 *     SOCK_SEQPACKET end talked through, a directory (by its path: CB5) a
 *     start for fstatat and fsync'd, a file on /data unlinked while open
 *     (our own open crosses: CB5); the kinds that don't (an eventfd, an
 *     epoll set) refused with EINVAL;
 *   - a memfd's read-only twin passed on stays read-only (no write, no
 *     writable mapping, no writable reopen), and a write-sealed memfd's
 *     seals arrive and hold;
 *   - one byte with 64, 128 and 253 descriptors (several parts), 254
 *     EINVAL; too little room (MSG_CTRUNC, the rest closed: the job's
 *     handles as before), MSG_CMSG_CLOEXEC, MSG_PEEK (bytes only), read()
 *     dropping descriptors, a stream's zero-byte send;
 *   - SOCK_SEQPACKET's message boundaries, MSG_TRUNC, EMSGSIZE;
 *   - shutdown(SHUT_RDWR) on either side, poll's hang-up; half-closes:
 *     SHUT_WR's writes EPIPE and the peer reading what came before, then
 *     the end for good; SHUT_RD reading what was queued, then the end;
 *     both halves a SHUT_RDWR; SandboxHostLinux's seqpacket pair (one end
 *     SHUT_RD, the other SHUT_WR, a request with a descriptor still
 *     crossing); no handle kept;
 *   - MSG_DONTWAIT on a full channel with descriptors (EAGAIN, no handle
 *     kept), a peer gone (EPIPE);
 *   - scmcred.c: SO_PASSCRED, shutdown waking what sleeps on the end, and
 *     FIONREAD leaving the channel's room alone (base's socket tests);
 *   - scmkid.c: Mojo's pattern between two processes (the child's pid in
 *     SCM_CREDENTIALS), and the fuzz loop. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../sys/maptable.h"
#include "pt.h"
#include "ptscm.h"

#define SELF "/boot/bin/posixtest"

uint64_t scm_job_handles(void)
{
    const struct mt_entry *e = mt_entries();
    unsigned n = e ? mt_count() : 0, vmos = 0;
    for (unsigned i = 0; i < n; i++) {
        bool again = false;
        for (unsigned j = 0; j < i && !again; j++)
            again = e[j].vmo == e[i].vmo;
        vmos += !again;
    }
    struct job_info ji;
    uint64_t used = jam_job_get_info(startup_handle(SR_JOB), &ji) == OK
                        ? ji.used[JOB_LIMIT_HANDLES] : 0;
    return used - 2ull * vmos;
}

int scm_lowest_fd(void)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
        close(fd);
    return fd;
}

ssize_t scm_send(int s, const void *b, size_t n, const int *fds, unsigned nf, int flags)
{
    struct iovec v = { (void *)b, n };
    static union {
        struct cmsghdr h;
        char           buf[CMSG_SPACE(sizeof(int) * SCM_ROOM_MAX)];
    } c;
    struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1 };
    if (nf) {
        m.msg_control = c.buf;
        m.msg_controllen = CMSG_SPACE(nf * sizeof(int));
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(nf * sizeof(int));
        memcpy(CMSG_DATA(h), fds, nf * sizeof(int));
    }
    return sendmsg(s, &m, flags);
}

ssize_t scm_recv(int s, void *b, size_t n, unsigned room, int flags, struct scm_got *g)
{
    struct iovec v = { b, n };
    static union {
        struct cmsghdr h;
        char           buf[CMSG_SPACE(sizeof(int) * SCM_ROOM_MAX)];
    } c;
    struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1, .msg_control = room ? c.buf : NULL,
                        .msg_controllen = room ? CMSG_SPACE(room * sizeof(int)) : 0 };
    ssize_t r = recvmsg(s, &m, flags);
    g->n = 0;
    g->flags = r >= 0 ? m.msg_flags : 0;
    for (struct cmsghdr *h = r >= 0 ? CMSG_FIRSTHDR(&m) : NULL; h; h = CMSG_NXTHDR(&m, h)) {
        unsigned k = (unsigned)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS || g->n + k > SCM_ROOM_MAX)
            continue;
        memcpy(g->fds + g->n, CMSG_DATA(h), k * sizeof(int));
        g->n += k;
    }
    return r;
}

void scm_close_got(struct scm_got *g)
{
    for (unsigned i = 0; i < g->n; i++)
        close(g->fds[i]);
    g->n = 0;
}

/* One descriptor across s[0] -> s[1] with one byte: the one received, or -1. */
static int pass_one(const int *s, int fd)
{
    struct scm_got g = { .n = 0 };
    char b = 0;
    if (scm_send(s[0], "x", 1, &fd, 1, 0) != 1 || scm_recv(s[1], &b, 1, 4, 0, &g) != 1 ||
        b != 'x' || g.n != 1 || g.flags) {
        scm_close_got(&g);
        return -1;
    }
    return g.fds[0];
}

/* ---- every kind ------------------------------------------------------------------------- */

static void check_kinds(const int *s)
{
    char buf[16];
    int p[2], q[2], k[2];
    CHECK(pipe(p) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, q) == 0 &&
          socketpair(AF_UNIX, SOCK_SEQPACKET, 0, k) == 0);
    int w = pass_one(s, p[1]);   /* a pipe's write end */
    CHECK(w >= 0 && write(w, "pipe", 4) == 4 && read(p[0], buf, sizeof(buf)) == 4 &&
          !memcmp(buf, "pipe", 4));
    CHECK(fcntl(w, F_GETFL) == O_WRONLY);
    close(w);
    int m = memfd_create("scm", 0);   /* a memfd: mapped on both sides, one page */
    CHECK(m >= 0 && ftruncate(m, 4096) == 0 && write(m, "memfd", 5) == 5);
    int r = pass_one(s, m);
    char *mine = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0);
    char *theirs = r >= 0 ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, r, 0)
                          : MAP_FAILED;
    CHECK(mine != MAP_FAILED && theirs != MAP_FAILED && !memcmp(theirs, "memfd", 5));
    if (mine != MAP_FAILED && theirs != MAP_FAILED) {
        memcpy(theirs + 100, "back", 4);
        CHECK(!memcmp(mine + 100, "back", 4));
        munmap(theirs, 4096);
    }
    if (mine != MAP_FAILED)
        munmap(mine, 4096);
    CHECK(r >= 0 && lseek(r, 0, SEEK_CUR) == 5 && lseek(r, 0, SEEK_END) == 4096);
    close(r);
    close(m);
    int f = open(SELF, O_RDONLY | O_CLOEXEC);   /* a file, at its offset */
    CHECK(f >= 0 && lseek(f, 1, SEEK_SET) == 1);
    r = pass_one(s, f);
    CHECK(r >= 0 && read(r, buf, 3) == 3 && !memcmp(buf, "ELF", 3));
    CHECK(lseek(f, 0, SEEK_CUR) == 1);   /* the sender's own offset didn't move */
    close(r);
    close(f);
    int z = open("/dev/zero", O_RDONLY);
    r = pass_one(s, z);
    memset(buf, 1, sizeof(buf));
    CHECK(r >= 0 && read(r, buf, 4) == 4 && !buf[0] && !buf[3]);
    close(r);
    close(z);
    r = pass_one(s, q[1]);   /* a socketpair end: talked through */
    CHECK(r >= 0 && write(r, "sock", 4) == 4 && read(q[0], buf, sizeof(buf)) == 4);
    close(r);
    r = pass_one(s, k[1]);   /* a seqpacket end keeps its kind and its boundaries */
    int type = 0;
    socklen_t len = sizeof(type);
    CHECK(r >= 0 && getsockopt(r, SOL_SOCKET, SO_TYPE, &type, &len) == 0 &&
          type == SOCK_SEQPACKET);
    CHECK(write(r, "ab", 2) == 2 && write(r, "c", 1) == 1 &&
          read(k[0], buf, sizeof(buf)) == 2 && read(k[0], buf, sizeof(buf)) == 1);
    close(r);
    errno = 0;   /* the kernel's cycle rule: an end into its own channel (Linux allows it) */
    CHECK(scm_send(s[0], "x", 1, &s[0], 1, 0) == -1 && errno == ETOOMANYREFS);
    /* A directory, by its path (until directory handles): a directory there, a start for
     * the *at calls, fsync'd (Chromium's storage service syncs a leveldb directory so). */
    int dir = open("/boot", O_RDONLY | O_DIRECTORY);
    r = pass_one(s, dir);
    struct stat st;
    CHECK(r >= 0 && fstat(r, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(r >= 0 && fstatat(r, "bin", &st, 0) == 0 && S_ISDIR(st.st_mode));
    CHECK(r >= 0 && fsync(r) == 0);
    close(r);
    close(dir);
    /* A file unlinked while open (base's FLAG_DELETE_ON_CLOSE temp files: Chromium's browser
     * gives each renderer one for its swap): no path to open again, so our own open crosses. */
    if (access("/data", W_OK) == 0) {
        int u = open("/data/pt-scm-gone", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        CHECK(u >= 0 && unlink("/data/pt-scm-gone") == 0 && write(u, "gone", 4) == 4);
        r = pass_one(s, u);
        CHECK(r >= 0 && pread(r, buf, 4, 0) == 4 && !memcmp(buf, "gone", 4));
        CHECK(r >= 0 && pwrite(r, "back", 4, 4) == 4 && pread(r, buf, 8, 0) == 8 &&
              !memcmp(buf, "goneback", 8));
        close(r);
        close(u);
    }
    int ev = eventfd(0, 0), ep = epoll_create1(0);
    int refused[] = { ev, ep };
    for (unsigned i = 0; i < 2; i++) {
        errno = 0;
        CHECK(refused[i] >= 0 && scm_send(s[0], "x", 1, &refused[i], 1, 0) == -1 &&
              errno == EINVAL);
        close(refused[i]);
    }
    int fds[] = { p[0], p[1], q[0], q[1], k[0], k[1] };
    for (unsigned i = 0; i < 6; i++)
        close(fds[i]);
}

/* The read-only twin, and seals. */
static void check_memfd_rules(const int *s)
{
    char path[32];
    int m = memfd_create("scmtwin", MFD_ALLOW_SEALING);
    CHECK(m >= 0 && ftruncate(m, 4096) == 0);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", m);
    int ro = open(path, O_RDONLY | O_CLOEXEC);
    int r = pass_one(s, ro);
    errno = 0;
    CHECK(r >= 0 && write(r, "x", 1) == -1 && errno == EBADF);
    errno = 0;
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, r, 0) == MAP_FAILED &&
          errno == EACCES);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", r);
    errno = 0;
    CHECK(open(path, O_RDWR) == -1 && errno == EACCES);
    close(r);
    close(ro);
    const int seals = F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW;
    CHECK(fcntl(m, F_ADD_SEALS, seals) == 0);
    r = pass_one(s, m);   /* an O_RDWR description, write-sealed */
    CHECK(r >= 0 && fcntl(r, F_GET_SEALS) == seals);
    errno = 0;
    CHECK(write(r, "x", 1) == -1 && errno == EPERM);
    errno = 0;
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, r, 0) == MAP_FAILED);
    void *a = mmap(NULL, 4096, PROT_READ, MAP_SHARED, r, 0);
    CHECK(a != MAP_FAILED);
    if (a != MAP_FAILED)
        munmap(a, 4096);
    errno = 0;
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0) == MAP_FAILED);
    close(r);
    close(m);
}

/* ---- counts, room and flags --------------------------------------------------------------- */

/* n descriptors (pipe ends: a handle each) with one byte, all arriving. */
static void check_many(const int *s, unsigned n)
{
    int p[2];
    CHECK(pipe(p) == 0);
    int *fds = malloc(n * sizeof(int));
    for (unsigned i = 0; fds && i < n; i++)
        fds[i] = (n - 1 - i) % 2 ? p[0] : p[1];   /* the last a write end */
    struct scm_got g = { .n = 0 };
    char b = 0;
    CHECK(fds && scm_send(s[0], "m", 1, fds, n, 0) == 1);
    CHECK(scm_recv(s[1], &b, 1, n, 0, &g) == 1 && b == 'm' && g.n == n && !g.flags);
    if (g.n == n) {   /* the last one still works: the parts arrived in order */
        CHECK(write(g.fds[n - 1], "z", 1) == 1 && read(p[0], &b, 1) == 1 && b == 'z');
    }
    scm_close_got(&g);
    free(fds);
    close(p[0]);
    close(p[1]);
}

static void check_counts(const int *s)
{
    /* One round first: its malloc grows this program's own allocator,
     * which keeps one unit more for good (seen at the first round's malloc
     * of the fds; most likely the break, whose VMO mem.c holds a handle of
     * its own to). Before the layer had a heap of its own (LH), the
     * layer's allocations had grown it long before. */
    check_many(s, 64);
    uint64_t before = scm_job_handles();
    int lowest = scm_lowest_fd();
    check_many(s, 64);
    check_many(s, 128);
    check_many(s, 253);
    int fds[254];
    for (unsigned i = 0; i < 254; i++)
        fds[i] = 0;
    errno = 0;
    CHECK(scm_send(s[0], "x", 1, fds, 254, 0) == -1 && errno == EINVAL);
    errno = 0;
    int bad = 1000;
    CHECK(scm_send(s[0], "x", 1, &bad, 1, 0) == -1 && errno == EBADF);
    CHECK(scm_lowest_fd() == lowest);
    CHECK(scm_job_handles() == before);
}

static void check_room(const int *s)
{
    int m = memfd_create("scmroom", 0), p[2];
    CHECK(m >= 0 && ftruncate(m, 4096) == 0 && pipe(p) == 0);
    int lowest = scm_lowest_fd();
    uint64_t before = scm_job_handles();
    int six[] = { m, p[0], p[1], m, p[0], p[1] };
    struct scm_got g = { .n = 0 };
    char b[4];
    CHECK(scm_send(s[0], "r", 1, six, 6, 0) == 1);   /* room for 2 */
    CHECK(scm_recv(s[1], b, sizeof(b), 2, 0, &g) == 1 && g.n == 2 && (g.flags & MSG_CTRUNC));
    scm_close_got(&g);
    CHECK(scm_send(s[0], "n", 1, six, 6, 0) == 1);   /* no room at all */
    CHECK(scm_recv(s[1], b, sizeof(b), 0, 0, &g) == 1 && !g.n && (g.flags & MSG_CTRUNC));
    CHECK(scm_send(s[0], "rd", 2, six, 6, 0) == 2);   /* read(): dropped */
    CHECK(read(s[1], b, sizeof(b)) == 2 && !memcmp(b, "rd", 2));
    CHECK(scm_send(s[0], "", 0, six, 6, 0) == 0);   /* a stream's zero bytes: nothing goes */
    CHECK(scm_send(s[0], "c", 1, six, 1, 0) == 1);   /* MSG_CMSG_CLOEXEC */
    CHECK(scm_recv(s[1], b, sizeof(b), 4, MSG_CMSG_CLOEXEC, &g) == 1 && g.n == 1 &&
          fcntl(g.fds[0], F_GETFD) == FD_CLOEXEC);
    scm_close_got(&g);
    CHECK(scm_send(s[0], "pk", 2, six + 1, 1, 0) == 2);   /* MSG_PEEK: the bytes, no descriptor */
    CHECK(scm_recv(s[1], b, sizeof(b), 4, MSG_PEEK, &g) == 2 && !g.n && !memcmp(b, "pk", 2));
    CHECK(scm_recv(s[1], b, 1, 4, 0, &g) == 1 && g.n == 1 && b[0] == 'p');
    scm_close_got(&g);
    CHECK(scm_recv(s[1], b, sizeof(b), 4, 0, &g) == 1 && !g.n && b[0] == 'k');
    CHECK(scm_lowest_fd() == lowest);
    CHECK(scm_job_handles() == before);
    close(m);
    close(p[0]);
    close(p[1]);
}

/* ---- seqpacket and shutdown -------------------------------------------------------------- */

static void check_seqpacket(void)
{
    int k[2];
    char buf[200];
    struct scm_got g = { .n = 0 };
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, k) == 0);
    memset(buf, 'q', sizeof(buf));
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(write(k[0], buf, 10) == 10 && send(k[0], buf, 0, 0) == 0 &&
          scm_send(k[0], buf, 100, &null, 1, 0) == 100 && write(k[0], buf, 7) == 7);
    close(null);
    CHECK(read(k[1], buf, sizeof(buf)) == 10);
    CHECK(recv(k[1], buf, sizeof(buf), 0) == 0);   /* the empty message */
    CHECK(scm_recv(k[1], buf, 50, 4, 0, &g) == 50 && g.n == 1 && (g.flags & MSG_TRUNC));
    scm_close_got(&g);
    CHECK(recv(k[1], buf, 3, MSG_PEEK | MSG_TRUNC) == 7);   /* peeked: still there */
    CHECK(recv(k[1], buf, sizeof(buf), MSG_DONTWAIT) == 7);
    errno = 0;
    CHECK(recv(k[1], buf, sizeof(buf), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    char *big = calloc(1, 70000);
    errno = 0;
    CHECK(big && write(k[0], big, 70000) == -1 && errno == EMSGSIZE);
    CHECK(big && write(k[0], big, 65536) == 65536 && read(k[1], big, 70000) == 65536);
    free(big);
    close(k[0]);
    close(k[1]);
}

static void check_shutdown(void)
{
    int s[2];
    char b[4];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    errno = 0;
    CHECK(shutdown(s[0], 7) == -1 && errno == EINVAL);
    CHECK(write(s[0], "x", 1) == 1);
    int d = dup(s[0]);
    CHECK(shutdown(s[0], SHUT_RDWR) == 0 && shutdown(s[0], SHUT_RDWR) == 0);
    CHECK(read(s[1], b, sizeof(b)) == 1 && read(s[1], b, sizeof(b)) == 0);   /* the end */
    errno = 0;
    CHECK(send(s[1], "y", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
    CHECK(read(d, b, sizeof(b)) == 0);   /* every descriptor of the description */
    errno = 0;
    CHECK(send(d, "y", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
    struct pollfd pf[2] = { { s[0], POLLIN | POLLOUT, 0 }, { s[1], POLLIN, 0 } };
    CHECK(poll(pf, 2, 1000) == 2 && (pf[0].revents & POLLIN) && (pf[1].revents & POLLHUP));
    close(d);
    close(s[0]);
    close(s[1]);
}

static bool polls(int fd, short ev)
{
    struct pollfd pf = { fd, ev, 0 };
    return poll(&pf, 1, 0) == 1 && (pf.revents & ev);
}

static void check_half(void)
{
    uint64_t before = scm_job_handles();
    int s[2];
    char b[8];
    /* SHUT_WR: writes EPIPE; the peer reads what came before, then the end for good */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    CHECK(write(s[0], "ab", 2) == 2 && shutdown(s[0], SHUT_WR) == 0 &&
          shutdown(s[0], SHUT_WR) == 0);
    errno = 0;
    CHECK(send(s[0], "c", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
    errno = 0;
    CHECK(write(s[0], "c", 1) == -1 && errno == EPIPE);   /* SIGPIPE ignored (pt_scm) */
    CHECK(polls(s[0], POLLOUT) && polls(s[1], POLLIN));
    CHECK(read(s[1], b, sizeof(b)) == 2 && !memcmp(b, "ab", 2));
    CHECK(read(s[1], b, sizeof(b)) == 0 && recv(s[1], b, sizeof(b), MSG_DONTWAIT) == 0);
    CHECK(polls(s[1], POLLIN));
    CHECK(write(s[1], "de", 2) == 2 && read(s[0], b, sizeof(b)) == 2);   /* the other way goes */
    close(s[0]);
    close(s[1]);
    /* SHUT_RD: what was queued, then the end instead of a wait; writes go; then SHUT_WR is
     * a SHUT_RDWR: the peer's writes EPIPE */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    CHECK(write(s[1], "fg", 2) == 2 && shutdown(s[0], SHUT_RD) == 0 && polls(s[0], POLLIN));
    CHECK(read(s[0], b, sizeof(b)) == 2 && read(s[0], b, sizeof(b)) == 0);
    CHECK(write(s[0], "h", 1) == 1 && read(s[1], b, sizeof(b)) == 1 && b[0] == 'h');
    CHECK(shutdown(s[0], SHUT_WR) == 0);
    errno = 0;
    CHECK(send(s[1], "i", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
    CHECK(read(s[1], b, sizeof(b)) == 0);
    close(s[0]);
    close(s[1]);
    /* SandboxHostLinux::Init: the child's end SHUT_RD, the browser's SHUT_WR; a request with
     * a descriptor still crosses to the browser, whose reads wait for more */
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, s) == 0);
    CHECK(shutdown(s[0], SHUT_RD) == 0 && shutdown(s[1], SHUT_WR) == 0);
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    struct scm_got g = { .n = 0 };
    CHECK(scm_send(s[0], "req", 3, &null, 1, 0) == 3 && send(s[0], b, 0, 0) == 0);
    CHECK(scm_recv(s[1], b, sizeof(b), 4, 0, &g) == 3 && g.n == 1 && !memcmp(b, "req", 3));
    scm_close_got(&g);
    CHECK(recv(s[1], b, sizeof(b), MSG_DONTWAIT) == 0);   /* the empty message */
    errno = 0;
    CHECK(recv(s[1], b, sizeof(b), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    CHECK(read(s[0], b, sizeof(b)) == 0);   /* the browser's end part */
    close(null);
    close(s[0]);
    close(s[1]);
    CHECK(scm_job_handles() == before);
}

/* ---- full, gone -------------------------------------------------------------------------- */

static void check_full_gone(void)
{
    uint64_t before = scm_job_handles();
    int s[2], m = memfd_create("scmfull", 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, s) == 0 && m >= 0);
    char *chunk = calloc(1, 65536);
    long sent = 0;
    while (chunk && sent < (64l << 20)) {   /* fill it: the channel's bound */
        ssize_t n = send(s[0], chunk, 65536, MSG_DONTWAIT);
        if (n <= 0) {
            CHECK(n == -1 && errno == EAGAIN);
            break;
        }
        sent += n;
    }
    errno = 0;
    CHECK(scm_send(s[0], "f", 1, &m, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    struct scm_got g = { .n = 0 };
    long got = 0;
    for (;;) {   /* drain: no descriptor came with the refused send */
        ssize_t n = scm_recv(s[1], chunk, 65536, 4, MSG_DONTWAIT, &g);
        CHECK(!g.n);
        scm_close_got(&g);
        if (n <= 0)
            break;
        got += n;
    }
    CHECK(got == sent);
    close(s[1]);
    errno = 0;
    CHECK(scm_send(s[0], "g", 1, &m, 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);   /* gone */
    close(s[0]);
    close(m);
    free(chunk);
    CHECK(scm_job_handles() == before);
}

void pt_scm(char **argv __attribute__((unused)))
{
    signal(SIGPIPE, SIG_IGN);
    int s[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s) == 0);
    check_kinds(s);
    check_memfd_rules(s);
    check_counts(s);
    check_room(s);
    close(s[0]);
    close(s[1]);
    check_seqpacket();
    check_shutdown();
    check_half();
    check_full_gone();
    scm_cred();
    scm_wake();
    scm_fionread();
    scm_mojo();
    scm_fuzz();
}
