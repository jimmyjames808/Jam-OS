/* posixtest's fork section, its descriptors and mappings (docs/FORK-PLAN.md
 * section 1.6, track FL2): cases of fork.c's table, each in a job of its
 * own. A forked child reports through a MAP_SHARED page (the line of its
 * first failed check, in the kernel log too) and its exit code; parent
 * and child take turns by two words there (the child's step, the
 * parent's go).
 *
 *   fds: every kind with handles, made by the parent, used from both
 *     sides by a forked child: pipe ends (the parent's leftover read once,
 *     by the parent: the child's first read is the next message),
 *     close-on-exec ones present (FD_CLOEXEC kept), socketpair ends both
 *     ways and a memfd sent back over one (SCM_RIGHTS), a SOCK_SEQPACKET
 *     end whose peeked message stays the parent's, a memfd and its dup
 *     (the pages shared, the size the child's copy), an eventfd (the
 *     child's counter its own), /dev/null, /etc/passwd and a file at the
 *     parent's offset (each side's own offset after), a file written by
 *     the child on an ext /data (EBUSY on FAT: one writer), a directory
 *     listed, an unconnected AF_UNIX socket, a named socket's connection
 *     and its accepted end both ways (with /svc/unix); absent: an epoll
 *     set, an inotify descriptor (the child makes one of its own at its
 *     number; the parent's still works after), a netstack socket (with
 *     /svc/net) and a listener.
 *     /proc/self/fd lists exactly what the child kept. Then the child
 *     closes every descriptor above 2 by /proc/self/fd: the job lets go of
 *     at least the handles those descriptors came with (the charge counts
 *     VMO objects too, so memory a close frees adds to it), and 16 more
 *     forks leave the parent within a few units of where one left it (a
 *     handle kept per fork would be 15).
 *   chromium: Chromium's child sequence (base::LaunchProcess on Linux):
 *     close-on-exec descriptors everywhere, dup2 of the output pipe to 1
 *     and 2 and of the IPC socketpair end to 3, every other descriptor
 *     closed by reading /proc/self/fd, then only 0 to 3 left, working, and
 *     the closed ones' handles gone.
 *   ttys: a pseudo-terminal's master and slave in a child (both ways, the
 *     slave a terminal there), still the parent's after the child closed
 *     its copies; the terminal at 0 too when there is one.
 *   maps: the table of mappings' handles in a child: MADV_DONTNEED and
 *     MAP_FIXED decommit its private memory (the parent's untouched), an
 *     mprotect adding write to a file's private read-only pages, a
 *     lowered break decommitting, a memfd's MAP_SHARED pages shared, and
 *     a munmap of the last piece of a mapping letting go of exactly its
 *     handle. */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <netinet/in.h>
#include <sched.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "pt.h"
#include "ptfiles.h"

#define PG      4096ul
#define KID_NS  (20 * NS_PER_S)    /* a side's wait for the other's word */
#define FDW     (1024 / 64)        /* words of a descriptor bitmap */
#define RFILE   PT_DIR "/r"        /* read by both sides */
#define WFILE   PT_DIR "/w"        /* written by the child */

/* The shared page: the child's first failure and the two sides' words. */
struct kr {
    volatile int line;   /* the first check that failed in a child, or 0 */
    volatile int step;   /* the child's: how far it has come */
    volatile int go;     /* the parent's: how far the child may go */
};
static struct kr *kr;

static bool map_kr(void)
{
    kr = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(kr != MAP_FAILED);
    return kr != MAP_FAILED;
}

static void kid_failed(int line)
{
    if (!kr->line)
        kr->line = line;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "posixtest: fork: a child FAILED forkfd.c line %d\n", line);
    (void)jam_debug_write(buf, (size_t)n);
}
#define KID(c)                     \
    do {                           \
        if (!(c))                  \
            kid_failed(__LINE__);  \
    } while (0)

/* Spin until *w reaches v or the time is up. */
static bool reach(volatile int *w, int v)
{
    uint64_t end = now() + KID_NS;
    while (*w < v && now() < end)
        sched_yield();
    return *w >= v;
}

/* The child: at step s, then on once the parent says go s. */
static void kid_step(int s)
{
    kr->step = s;
    KID(reach(&kr->go, s));
}

/* The parent: wait for the child's step s (false: it never came). */
static bool par_wait(int s)
{
    bool ok = reach(&kr->step, s);
    CHECK(ok);
    return ok;
}

static void par_go(int s)
{
    kr->go = s;
}

static _Noreturn void kid_end(void)
{
    _exit(kr->line ? 1 : 0);
}

/* Reap pid, which should have exited 0. */
static void reap0(pid_t pid)
{
    int st = 0;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r == -1 && errno == EINTR);
    CHECK(r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    if (kr->line)
        printf("posixtest: fork: a child failed at forkfd.c line %d\n", kr->line);
}

/* Handles our job holds: us and our children (each case's job is its own). */
static uint64_t job_handles(void)
{
    struct job_info ji;
    return jam_job_get_info(startup_handle(SR_JOB), &ji) == OK ? ji.used[JOB_LIMIT_HANDLES]
                                                                 : 0;
}

/* The descriptors /proc/self/fd lists, but the listing's own, into bits;
 * false if it can't be read. */
static bool fd_list(uint64_t bits[FDW])
{
    memset(bits, 0, FDW * sizeof(bits[0]));
    DIR *dir = opendir("/proc/self/fd");
    if (!dir)
        return false;
    int own = dirfd(dir);
    for (struct dirent *e; (e = readdir(dir));) {
        char *end;
        long fd = strtol(e->d_name, &end, 10);
        if (*end || e->d_name[0] == '.' || fd == own || fd < 0 || fd >= 64 * FDW)
            continue;
        bits[fd / 64] |= 1ull << (fd % 64);
    }
    closedir(dir);
    return true;
}

static void bit_set(uint64_t bits[FDW], int fd)
{
    if (fd >= 0 && fd < 64 * FDW)
        bits[fd / 64] |= 1ull << (fd % 64);
}

/* Chromium's way (base's CloseSuperfluousFds): every descriptor
 * /proc/self/fd lists closed but those in keep and the listing's own. */
static void close_listed(const uint64_t keep[FDW])
{
    DIR *dir = opendir("/proc/self/fd");
    KID(dir != NULL);
    if (!dir)
        return;
    int own = dirfd(dir);
    for (struct dirent *e; (e = readdir(dir));) {
        char *end;
        long fd = strtol(e->d_name, &end, 10);
        if (*end || e->d_name[0] == '.' || fd == own)
            continue;
        if (fd < 64 * FDW && (keep[fd / 64] >> (fd % 64) & 1))
            continue;
        KID(close((int)fd) == 0);
    }
    closedir(dir);
}

/* n bytes of c, written as one message. */
static bool write_run(int fd, char c, size_t n)
{
    char buf[128];
    memset(buf, c, n);
    return write(fd, buf, n) == (ssize_t)n;
}

/* Read up to cap bytes: are they all c (and as many as want)? */
static bool read_run(int fd, char c, size_t want)
{
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n != (ssize_t)want)
        return false;
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] != c)
            return false;
    return true;
}

static bool read_is(int fd, const char *s)
{
    char buf[128];
    size_t len = strlen(s);
    ssize_t n = read(fd, buf, sizeof(buf));
    return n == (ssize_t)len && !memcmp(buf, s, len);
}

/* A new file at path holding s: true if made. */
static bool make_file(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0 && write(fd, s, strlen(s)) == (ssize_t)strlen(s);
    if (fd >= 0)
        close(fd);
    return ok;
}

/* A memfd's descriptor sent over sock (SCM_RIGHTS) with one byte. */
static bool send_fd(int sock, int fd)
{
    char byte = 'm';
    struct iovec iov = { &byte, 1 };
    union {
        struct cmsghdr h;
        char room[CMSG_SPACE(sizeof(int))];
    } u = { 0 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.room,
                        .msg_controllen = sizeof(u.room) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &m, 0) == 1;
}

static int recv_fd(int sock)
{
    char byte;
    struct iovec iov = { &byte, 1 };
    union {
        struct cmsghdr h;
        char room[CMSG_SPACE(sizeof(int))];
    } u = { 0 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.room,
                        .msg_controllen = sizeof(u.room) };
    if (recvmsg(sock, &m, 0) != 1)
        return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    int fd = -1;
    if (c && c->cmsg_type == SCM_RIGHTS)
        memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

/* ---- fds ------------------------------------------------------------------------- */

struct fds {
    int pl[2], pc[2], ss[2], sq[2], mf, mf2, ef, dn, ep, fl, fw, dd, us;
    int ee, in, net;           /* absent in the child (-1: not made) */
    int ul, uc, ua;            /* a named socket's listener, connection, accepted end (-1) */
    bool ext;                  /* /data is ext: the child may write fw */
    unsigned handles;          /* what the child's copies of these hold (closed at its step 3) */
};

/* A listener on an abstract name and a connection to it, accepted:
 * false (all -1) without /svc/unix. */
static bool unix_trio(struct fds *f)
{
    f->ul = f->uc = f->ua = -1;
    if (!svc_get(SVC_UNIX)) {
        printf("posixtest: fork: fds: no /svc/unix here: its sockets left out\n");
        return false;
    }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    static const char name[] = "fl2-fork";
    memcpy(a.sun_path + 1, name, sizeof(name) - 1);   /* abstract: sun_path[0] is the NUL */
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof(name));
    f->ul = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(f->ul >= 0 && bind(f->ul, (struct sockaddr *)&a, len) == 0 && listen(f->ul, 4) == 0);
    f->uc = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(f->uc >= 0 && connect(f->uc, (struct sockaddr *)&a, len) == 0);
    f->ua = accept(f->ul, NULL, NULL);
    CHECK(f->ua >= 0);
    return f->ua >= 0;
}

static void fds_make(struct fds *f)
{
    char buf[16];
    CHECK(pipe(f->pl) == 0);
    CHECK(write_run(f->pl[1], 'A', 100));   /* one message: 90 bytes stay our leftover */
    CHECK(read(f->pl[0], buf, 10) == 10 && buf[0] == 'A');
    CHECK(write_run(f->pl[1], 'B', 50));
    CHECK(pipe2(f->pc, O_CLOEXEC) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, f->ss) == 0);
    CHECK(write(f->ss[0], "ping", 4) == 4);
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, f->sq) == 0);
    CHECK(send(f->sq[0], "first", 5, 0) == 5 && send(f->sq[0], "second", 6, 0) == 6);
    CHECK(recv(f->sq[1], buf, sizeof(buf), MSG_PEEK) == 5);   /* kept: our description's */
    f->mf = memfd_create("fl2", 0);
    CHECK(f->mf >= 0 && write(f->mf, "memfd-data", 10) == 10);
    f->mf2 = dup(f->mf);
    f->ef = eventfd(5, 0);
    f->dn = open("/dev/null", O_RDWR);
    f->ep = open("/etc/passwd", O_RDONLY);
    CHECK(f->mf2 >= 0 && f->ef >= 0 && f->dn >= 0 && f->ep >= 0 && read(f->ep, buf, 4) == 4);
    CHECK(make_file(RFILE, "0123456789") && make_file(WFILE, "abcdefghij"));
    f->fl = open(RFILE, O_RDONLY);
    CHECK(f->fl >= 0 && read(f->fl, buf, 3) == 3);
    f->fw = open(WFILE, O_RDWR);
    f->dd = open("/data", O_RDONLY | O_DIRECTORY);
    f->us = socket(AF_UNIX, SOCK_STREAM, 0);
    f->ee = epoll_create1(EPOLL_CLOEXEC);
    f->in = inotify_init1(IN_NONBLOCK);   /* no FD_CLOEXEC: absent all the same */
    CHECK(f->fw >= 0 && f->dd >= 0 && f->us >= 0 && f->ee >= 0 && f->in >= 0);
    CHECK(inotify_add_watch(f->in, "/etc", IN_ALL_EVENTS) > 0);
    f->net = socket(AF_INET, SOCK_STREAM, 0);   /* with /svc/net only */
    if (f->net < 0)
        printf("posixtest: fork: fds: no netstack socket here (errno %d): left out\n", errno);
    f->ext = pt_data_ext();
    bool u = unix_trio(f);
    /* Each pipe and socketpair end 1, the memfd's object 1 (two descriptors), the
     * eventfd's event 1; a named connection's inner end 1 and the accepted end 1. */
    f->handles = 2 + 2 + 2 + 2 + 1 + 1 + (u ? 2 : 0);
}

/* The child's use of each: step 2 once all is said. */
static void fds_kid(struct fds *f)
{
    char buf[64];
    kid_step(1);   /* the parent counts the job's handles */
    KID(read_run(f->pl[0], 'B', 50));   /* the next message, never our leftover */
    KID(write(f->pl[1], "C", 1) == 1);
    KID(fcntl(f->pc[0], F_GETFD) == FD_CLOEXEC && fcntl(f->pc[1], F_GETFD) == FD_CLOEXEC);
    KID(write(f->pc[1], "cloexec", 7) == 7);
    KID(read_is(f->ss[1], "ping") && write(f->ss[1], "pong", 4) == 4);
    KID(send_fd(f->ss[1], f->mf));
    KID(recv(f->sq[1], buf, sizeof(buf), 0) == 6 && !memcmp(buf, "second", 6));
    struct stat st;
    KID(fstat(f->mf, &st) == 0 && st.st_size == 10);
    KID(pread(f->mf2, buf, 10, 0) == 10 && !memcmp(buf, "memfd-data", 10));
    KID(pwrite(f->mf, "CHILD", 5, 0) == 5);
    uint64_t v = 0;
    KID(read(f->ef, &v, 8) == 8 && v == 5);   /* our counter's copy */
    v = 7;
    KID(write(f->ef, &v, 8) == 8);
    KID(write(f->dn, "x", 1) == 1);
    KID(read(f->ep, buf, 4) == 4 && !memcmp(buf, "x:10", 4));   /* "jam:x:1000:..." from 4 */
    KID(read(f->fl, buf, 2) == 2 && !memcmp(buf, "34", 2));    /* from our offset, 3 */
    if (f->ext)
        KID(pwrite(f->fw, "W", 1, 9) == 1);
    else
        KID(pwrite(f->fw, "W", 1, 9) == -1 && errno == EBUSY);   /* fat: we hold its writer */
    char d[512];
    long n = syscall(SYS_getdents64, f->dd, d, sizeof(d));
    KID(n > 0);
    KID(fcntl(f->us, F_GETFD) == 0);
    struct sockaddr_un sa;
    socklen_t sl = sizeof(sa);
    KID(getsockname(f->us, (struct sockaddr *)&sa, &sl) == 0 && sa.sun_family == AF_UNIX);
    errno = 0;
    KID(fcntl(f->ee, F_GETFD) == -1 && errno == EBADF);   /* absent */
    errno = 0;
    KID(fcntl(f->in, F_GETFD) == -1 && errno == EBADF);   /* inotify: absent too (Q3) */
    errno = 0;
    KID(read(f->in, buf, sizeof(buf)) == -1 && errno == EBADF);
    int own = inotify_init1(IN_NONBLOCK);   /* the child's own (the parent's number is free) */
    KID(own >= 0 && inotify_add_watch(own, "/etc", IN_CREATE) > 0);
    KID(read(own, buf, sizeof(buf)) == -1 && errno == EAGAIN);
    close(own);
    if (f->net >= 0)
        KID(fcntl(f->net, F_GETFD) == -1);
    if (f->ua >= 0) {
        KID(fcntl(f->ul, F_GETFD) == -1);   /* a listener: absent */
        KID(write(f->uc, "u-kid", 5) == 5);
    }
    kid_step(2);   /* the parent reads what we wrote, and answers */
    if (f->ua >= 0)
        KID(read_is(f->uc, "u-par"));

    uint64_t want[FDW] = { 0 }, got[FDW];
    const int kept[] = { 0, 1, 2, f->pl[0], f->pl[1], f->pc[0], f->pc[1], f->ss[0], f->ss[1],
                         f->sq[0], f->sq[1], f->mf, f->mf2, f->ef, f->dn, f->ep, f->fl, f->fw,
                         f->dd, f->us, f->uc, f->ua };
    for (unsigned i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        bit_set(want, kept[i]);
    KID(fd_list(got) && !memcmp(got, want, sizeof(got)));
    kid_step(3);   /* the parent counts the job's handles */
    uint64_t keep[FDW] = { 0x7 };
    close_listed(keep);
    KID(fd_list(got) && got[0] == 0x7);
    kid_step(4);   /* and again */
    kid_end();
}

void pt_fork_fds(void)
{
    struct fds f;
    if (!map_kr() || !pt_files_start())
        return;
    fds_make(&f);
    pid_t pid = fork();
    if (pid == 0)
        fds_kid(&f);
    CHECK(pid > 0);
    if (pid < 0)
        return;
    par_wait(1);
    par_go(1);
    char buf[16];
    if (par_wait(2)) {
        CHECK(read_run(f.pl[0], 'A', 90));   /* our leftover, once */
        CHECK(read_is(f.pl[0], "C"));
        CHECK(read_is(f.pc[0], "cloexec"));
        CHECK(read_is(f.ss[0], "pong"));
        int got = recv_fd(f.ss[0]);
        CHECK(got >= 0 && pread(got, buf, 10, 0) == 10 && !memcmp(buf, "CHILD-data", 10));
        if (got >= 0)
            close(got);
        CHECK(recv(f.sq[1], buf, sizeof(buf), 0) == 5 && !memcmp(buf, "first", 5));
        uint64_t v = 0;
        CHECK(read(f.ef, &v, 8) == 8 && v == 5);   /* ours: the child's 7 isn't here */
        CHECK(read(f.fl, buf, 2) == 2 && !memcmp(buf, "34", 2));   /* our own offset */
        if (f.ua >= 0)
            CHECK(read_is(f.ua, "u-kid") && write(f.ua, "u-par", 5) == 5);
    }
    par_go(2);
    uint64_t h2 = par_wait(3) ? job_handles() : 0;
    par_go(3);
    if (par_wait(4) && h2) {
        /* At least the handles the child's descriptors came with went with
         * them (the charge counts VMO objects too, so memory its closes
         * freed adds to the drop). */
        int64_t went = (int64_t)(h2 - job_handles());
        if (went < (int64_t)f.handles)
            printf("posixtest: fork: fds: the child's closes let go of %lld handles, under %u\n",
                   (long long)went, f.handles);
        CHECK(went >= (int64_t)f.handles);
    }
    par_go(4);
    reap0(pid);
    CHECK(pread(f.fw, buf, 1, 9) == 1 && buf[0] == (f.ext ? 'W' : 'j'));
    CHECK(fcntl(f.in, F_GETFD) == 0);   /* ours still: a watch of it still works */
    CHECK(inotify_add_watch(f.in, "/etc", IN_ALL_EVENTS) > 0);
    CHECK(inotify_add_watch(f.in, "/dev", IN_CREATE) > 0);
    errno = 0;
    CHECK(read(f.in, buf, sizeof(buf)) == -1 && errno == EAGAIN);
    /* Nothing of a fork stays with us: every handle made for a child goes
     * to it or is closed. The charge moves with our memory too, so the test
     * is that 16 more forks (each child gone at once) leave us within a
     * few units of where one left us: a handle kept per fork is 15. */
    uint64_t h[2] = { 0 };
    for (int i = 0; i < 17; i++) {
        pid_t k = fork();
        if (k == 0)
            _exit(0);
        reap0(k);
        h[i > 0] = job_handles();
    }
    if (h[1] > h[0] + 4)
        printf("posixtest: fork: fds: 16 forks left us %llu handles more\n",
               (unsigned long long)(h[1] - h[0]));
    CHECK(h[1] <= h[0] + 4);
    close(f.fl);
    close(f.fw);
    close(f.in);
    pt_files_end();
}

/* ---- chromium -------------------------------------------------------------------- */

/* The descriptors the parent makes, at FIRST and on (out's two ends, the
 * IPC pair's, two pipes', a memfd, /dev/null), all close-on-exec; 3 is
 * free, as Chromium's child finds the IPC descriptor's place. */
#define FIRST 20
enum { OUT_R = FIRST, OUT_W, IPC_P, IPC_K, P0, P1, Q0, Q1, MEM, DEVNULL, NFDS };

static void chromium_kid(void)
{
    kid_step(1);   /* the parent has closed its copies of the child's ends */
    KID(dup2(OUT_W, 1) == 1 && dup2(OUT_W, 2) == 2 && dup2(IPC_K, 3) == 3);
    KID(fcntl(1, F_GETFD) == 0 && fcntl(3, F_GETFD) == 0);   /* dup2 clears FD_CLOEXEC */
    uint64_t keep[FDW] = { 0xf }, got[FDW];
    KID(fd_list(got));   /* the listing's memory made once, before the count */
    uint64_t h1 = job_handles();
    close_listed(keep);
    KID(fd_list(got) && got[0] == 0xf);
    for (unsigned i = 1; i < FDW; i++)
        KID(got[i] == 0);
    /* What went with them: out's read end, the parent's IPC end, the two
     * pipes and the memfd; out's write end and ours of the pair live on as
     * 1, 2 and 3, and /dev/null holds nothing. */
    KID((int64_t)(h1 - job_handles()) >= 1 + 1 + 4 + 1);   /* and memory freed: fds says why */
    char buf[16];
    KID(write(1, "hello from the child", 20) == 20);
    KID(send(3, "ipc", 3, 0) == 3 && recv(3, buf, sizeof(buf), 0) == 3 && !memcmp(buf, "ack", 3));
    kid_end();
}

/* fd moved to want (close-on-exec), the old one closed. */
static void move_to(int fd, int want)
{
    CHECK(fd >= 0 && dup3(fd, want, O_CLOEXEC) == want && close(fd) == 0);
}

void pt_fork_chromium(void)
{
    if (!map_kr())
        return;
    int out[2], ipc[2], p[2], q[2];
    CHECK(pipe(out) == 0 && socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ipc) == 0);
    CHECK(pipe(p) == 0 && pipe(q) == 0);
    const int made[] = { out[0], out[1], ipc[0], ipc[1], p[0], p[1], q[0], q[1],
                         memfd_create("noise", 0), open("/dev/null", O_RDONLY) };
    for (int i = 0; i < NFDS - FIRST; i++)
        move_to(made[i], FIRST + i);
    pid_t pid = fork();
    if (pid == 0)
        chromium_kid();
    CHECK(pid > 0);
    if (pid < 0)
        return;
    par_wait(1);
    close(OUT_W);
    close(IPC_K);
    par_go(1);
    CHECK(read_is(OUT_R, "hello from the child"));
    char buf[8];
    CHECK(recv(IPC_P, buf, sizeof(buf), 0) == 3 && !memcmp(buf, "ipc", 3));
    CHECK(send(IPC_P, "ack", 3, 0) == 3);
    reap0(pid);
    char c;
    CHECK(read(OUT_R, &c, 1) == 0);   /* every writer gone: the child's 1 and 2 too */
    for (int fd = FIRST; fd < NFDS; fd++)
        if (fd != OUT_W && fd != IPC_K)
            close(fd);
}

/* ---- ttys ------------------------------------------------------------------------ */

/* Read from fd until s is seen (a terminal may cut it, and adds \r). */
static bool read_until(int fd, const char *s)
{
    char buf[256];
    size_t have = 0;
    uint64_t end = now() + KID_NS;
    while (have < sizeof(buf) - 1 && now() < end) {
        ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0)
            return false;
        have += (size_t)n;
        buf[have] = '\0';
        if (strstr(buf, s))
            return true;
    }
    return false;
}

void pt_fork_ttys(void)
{
    if (!map_kr())
        return;
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0);
    char *name = m >= 0 ? ptsname(m) : NULL;
    int s = name ? open(name, O_RDWR | O_NOCTTY) : -1;
    CHECK(s >= 0);
    if (s < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        KID(isatty(s));
        char tn[32];
        KID(ttyname_r(s, tn, sizeof(tn)) == 0 && !strncmp(tn, "/dev/pts/", 9));
        if (isatty(0)) {
            struct termios t;
            KID(tcgetattr(0, &t) == 0);
        }
        KID(write(s, "kid-out\n", 8) == 8);
        KID(read_until(s, "par-in"));
        KID(write(m, "kid-in\n", 7) == 7);   /* the master both ways too */
        kid_step(1);
        KID(close(m) == 0 && close(s) == 0);
        kid_end();
    }
    CHECK(pid > 0);
    if (pid < 0)
        return;
    CHECK(read_until(m, "kid-out"));
    CHECK(write(m, "par-in\n", 7) == 7);
    par_wait(1);
    CHECK(read_until(s, "kid-in"));
    par_go(1);
    reap0(pid);
    CHECK(write(s, "after\n", 6) == 6 && read_until(m, "after"));   /* ours still */
    close(s);
    close(m);
}

/* ---- maps ------------------------------------------------------------------------ */

void pt_fork_maps(void)
{
    if (!map_kr() || !pt_files_start())
        return;
    const int rw = PROT_READ | PROT_WRITE, anon = MAP_PRIVATE | MAP_ANONYMOUS;
    uint8_t *a = mmap(NULL, PG, rw, anon, -1, 0);           /* MADV_DONTNEED */
    uint8_t *b = mmap(NULL, 4 * PG, rw, anon, -1, 0);       /* MAP_FIXED over a part */
    uint8_t *c = mmap(NULL, 3 * PG, rw, anon, -1, 0);       /* unmapped in pieces */
    int mf = memfd_create("fl2-maps", 0);
    CHECK(mf >= 0 && ftruncate(mf, PG) == 0);
    uint8_t *s = mmap(NULL, PG, rw, MAP_SHARED, mf, 0);     /* shared both ways */
    CHECK(make_file(PT_DIR "/pages", "file-pages"));
    int fd = open(PT_DIR "/pages", O_RDONLY);
    uint8_t *f = fd >= 0 ? mmap(NULL, PG, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    CHECK(a != MAP_FAILED && b != MAP_FAILED && c != MAP_FAILED && s != MAP_FAILED &&
          f != MAP_FAILED);
    if (a == MAP_FAILED || b == MAP_FAILED || c == MAP_FAILED || s == MAP_FAILED ||
        f == MAP_FAILED)
        return;
    memset(a, 0x11, PG);
    memset(b, 0x22, 4 * PG);
    memset(c, 0x33, 3 * PG);
    CHECK(syscall(SYS_brk, 0) > 0);   /* the break's reservation made before the fork */
    pid_t pid = fork();
    if (pid == 0) {
        KID(madvise(a, PG, MADV_DONTNEED) == 0 && a[0] == 0);
        KID(mmap(b + PG, PG, rw, anon | MAP_FIXED, -1, 0) == b + PG);
        KID(b[PG] == 0 && b[0] == 0x22 && b[2 * PG] == 0x22);
        KID(mprotect(f, PG, rw) == 0);
        f[0] = 'Z';
        s[0] = 'K';
        uint64_t h = job_handles();
        KID(munmap(c + PG, PG) == 0 && munmap(c, PG) == 0);
        KID(job_handles() == h);   /* the VMO still mapped at c + 2 pages */
        /* Its handle, once, and its VMO: the child's own copy of ours. */
        KID(munmap(c + 2 * PG, PG) == 0 && job_handles() == h - 2);
        /* The break: pages 16 MiB above it, far from where musl's malloc
         * grows it a page at a time (another thread of ours may, at any
         * moment), written, then the break lowered to them and raised
         * again. Not put back: we end next. */
        long cur = syscall(SYS_brk, 0), at = ((cur + (long)PG - 1) & -(long)PG) + (16l << 20);
        long up = at + 2 * (long)PG;
        KID(cur > 0 && syscall(SYS_brk, up) == up);
        memset((void *)at, 0x77, 2 * PG);
        KID(syscall(SYS_brk, at) == at && syscall(SYS_brk, up) == up);
        KID(((volatile uint8_t *)at)[0] == 0);   /* decommitted by the lowering */
        kid_step(1);
        kid_end();
    }
    CHECK(pid > 0);
    if (pid < 0)
        return;
    par_wait(1);
    par_go(1);
    reap0(pid);
    CHECK(a[0] == 0x11 && b[PG] == 0x22 && c[PG] == 0x33);
    CHECK(f[0] == 'f' && s[0] == 'K');
    munmap(a, PG);
    munmap(b, 4 * PG);
    munmap(c, 3 * PG);
    munmap(s, PG);
    munmap(f, PG);
    close(mf);
    close(fd);
    pt_files_end();
}
