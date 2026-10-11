/* posixtest's wayland section: libc's Wayland socket (user/posix/sys/
 * wlsock.c, svcsock.c, fdpass.c; docs/G2-PLAN.md section 1) against a
 * fake compositor: the test publishes /svc/wayland in its own namespace
 * (ns_svc_set: a channel whose other end it keeps) and reads and writes
 * that end as the compositor would, JWL2 batches and handles (<jwl.h>).
 *
 *   - no service socket: a path not in the table, and /svc/wayland while
 *     our list hasn't asked for it, are ENOENT; bind and listen
 *     EAFNOSUPPORT; the calls before connect ENOTCONN;
 *   - sending: whole messages into batches (at most 16 KiB, never a
 *     message split), a message split over three sendmsgs, one byte with
 *     28 descriptors then the rest (libwayland's flush), 29 refused (in
 *     one call, and as one more waiting), a bad header EINVAL;
 *   - every descriptor kind of the table (memfd, shm_open, pipe ends, a
 *     socketpair end) arriving as the handle it should, with a pool's
 *     rights, and EINVAL for the rest (eventfd, /dev/null, an AF_INET
 *     socket, the Wayland socket itself);
 *   - receiving: handles as descriptors (a memfd readable and mappable,
 *     one without RIGHT_DUPLICATE too, a pipe end that writes), too little
 *     room (MSG_CTRUNC, the rest closed), read() closing them all;
 *   - a batch that breaks JWL2 (a bad magic, a count that lies, a handle
 *     of the wrong kind): ECONNRESET once, then the end, writes EPIPE;
 *   - a full channel: EAGAIN, no POLLOUT; a batch read: POLLOUT and the
 *     send goes; descriptors on a full channel take nothing;
 *   - poll: POLLIN for bytes libc holds, POLLHUP once the compositor is
 *     gone and read() is 0;
 *   - a memfd from another process (a child given ours by the hand-over)
 *     sent on as a pool: the same kept VMO arrives;
 *   - a fuzz loop: 10,000 mutated streams and batches, every batch the
 *     shim writes well formed, no handle and no descriptor leaked
 *     (wlfuzz.c).
 *
 * The rig, the fake compositor's reads and writes: wlrig.c. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pt.h"
#include "wlrig.h"

#define SELF "/boot/bin/posixtest"

extern char **environ;

/* ---- no service socket -------------------------------------------------------------- */

static void check_no_socket(void)
{
    int fd = wr_sock_new();
    CHECK(fd >= 0);
    errno = 0;
    CHECK(wr_connect_to(fd, "/svc/wayland") == -1 && errno == ENOENT);   /* not in our list */
    errno = 0;
    CHECK(wr_connect_to(fd, "/tmp/wayland-0") == -1 && errno == ENOENT);  /* not in the table */
    errno = 0;
    CHECK(wr_connect_to(fd, "") == -1 && errno == ENOENT);
    struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = "/svc/wayland" };
    errno = 0;
    CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EAFNOSUPPORT);
    errno = 0;
    CHECK(listen(fd, 1) == -1 && errno == EAFNOSUPPORT);
    errno = 0;
    CHECK(send(fd, "x", 1, MSG_NOSIGNAL) == -1 && errno == ENOTCONN);
    char c;
    errno = 0;
    CHECK(read(fd, &c, 1) == -1 && errno == ENOTCONN);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode));
    close(fd);
    errno = 0;
    CHECK(socket(AF_UNIX, SOCK_DGRAM, 0) == -1 && errno == ESOCKTNOSUPPORT);
}

/* ---- sending -------------------------------------------------------------------------- */

static void check_whole(void)
{
    struct rig r;
    CHECK(wr_open(&r));
    static uint8_t out[5 * 4096 + 20];
    wr_msg(out, 3, 8);
    wr_msg(out + 8, 4, 12);
    wr_msg(out + 20, 5, 4096);
    CHECK(wr_send(r.fd, out, 4116, NULL, 0) == 4116);
    static struct batch bt;
    CHECK(wr_comp_read(&r, &bt) == OK && wr_batch_ok(&bt) && bt.nh == 0);
    CHECK(bt.nb == JWL_HEADER_BYTES + 4116 && !memcmp(bt.b + JWL_HEADER_BYTES, out, 4116));
    for (int i = 0; i < 5; i++)   /* 20 KiB of 4 KiB messages: 3 to a batch, then 2 */
        wr_msg(out + i * 4096, 10 + i, 4096);
    CHECK(wr_send(r.fd, out, 5 * 4096, NULL, 0) == 5 * 4096);
    CHECK(wr_comp_read(&r, &bt) == OK && wr_batch_ok(&bt) && bt.nb == JWL_HEADER_BYTES + 3 * 4096);
    CHECK(!memcmp(bt.b + JWL_HEADER_BYTES, out, 3 * 4096));
    CHECK(wr_comp_read(&r, &bt) == OK && wr_batch_ok(&bt) && bt.nb == JWL_HEADER_BYTES + 2 * 4096);
    CHECK(!memcmp(bt.b + JWL_HEADER_BYTES, out + 3 * 4096, 2 * 4096));
    CHECK(wr_comp_read(&r, &bt) == ERR_SHOULD_WAIT);
    wr_close(&r);
}

static void check_split(void)
{
    struct rig r;
    CHECK(wr_open(&r));
    uint8_t m[100];
    wr_msg(m, 7, sizeof(m));
    static struct batch bt;
    CHECK(wr_send(r.fd, m, 5, NULL, 0) == 5);              /* inside the header */
    CHECK(wr_comp_read(&r, &bt) == ERR_SHOULD_WAIT);
    CHECK(wr_send(r.fd, m + 5, 50, NULL, 0) == 50);
    CHECK(wr_comp_read(&r, &bt) == ERR_SHOULD_WAIT);
    CHECK(wr_send(r.fd, m + 55, 45, NULL, 0) == 45);
    CHECK(wr_comp_read(&r, &bt) == OK && wr_batch_ok(&bt) && bt.nb == JWL_HEADER_BYTES + 100);
    CHECK(!memcmp(bt.b + JWL_HEADER_BYTES, m, 100));
    uint8_t bad[8];                                         /* a size of 6: no message's */
    wr_wr32(bad, 1);
    wr_wr32(bad + 4, 6u << 16);
    errno = 0;
    CHECK(wr_send(r.fd, bad, 8, NULL, 0) == -1 && errno == EINVAL);
    CHECK(wr_send(r.fd, m, 100, NULL, 0) == 100);          /* the bad bytes were never taken */
    CHECK(wr_comp_read(&r, &bt) == OK && bt.nb == JWL_HEADER_BYTES + 100);
    wr_close(&r);
}

/* libwayland's flush: one byte with the descriptors, then the rest. */
static void check_28(void)
{
    struct rig r;
    CHECK(wr_open(&r));
    int fds[29];
    for (int i = 0; i < 29; i++)
        fds[i] = wr_memfd_with("pool");
    uint8_t m[28 * 12];
    for (int i = 0; i < 28; i++)
        wr_msg(m + i * 12, 20 + i, 12);
    static struct batch bt;
    errno = 0;
    CHECK(wr_send(r.fd, m, 1, fds, 29) == -1 && errno == EINVAL);   /* 29: refused */
    CHECK(wr_comp_read(&r, &bt) == ERR_SHOULD_WAIT);
    CHECK(wr_send(r.fd, m, 1, fds, 28) == 1);
    CHECK(wr_comp_read(&r, &bt) == ERR_SHOULD_WAIT);                  /* waiting for its message */
    errno = 0;
    CHECK(wr_send(r.fd, m + 1, 1, fds + 28, 1) == -1 && errno == EINVAL);   /* a 29th waiting */
    CHECK(wr_send(r.fd, m + 1, sizeof(m) - 1, NULL, 0) == (ssize_t)sizeof(m) - 1);
    CHECK(wr_comp_read(&r, &bt) == OK && wr_batch_ok(&bt) && bt.nh == 28);
    CHECK(bt.nb == JWL_HEADER_BYTES + sizeof(m) && !memcmp(bt.b + JWL_HEADER_BYTES, m, sizeof(m)));
    for (uint32_t i = 0; i < bt.nh; i++) {
        struct handle_info hi;
        char text[4];
        CHECK(jam_handle_info(bt.h[i], &hi) == OK && hi.type == OBJ_VMO);
        CHECK(hi.rights == WR_POOL_RIGHTS);   /* read, map, inspect, transfer: no more */
        CHECK(jam_vmo_read(bt.h[i], 0, text, 4) == OK && !memcmp(text, "pool", 4));
    }
    wr_close_handles(bt.h, bt.nh);
    for (int i = 0; i < 29; i++)
        close(fds[i]);
    wr_close(&r);
}

/* Each descriptor alone with a message: the handle type that arrives,
 * or -1 if the send was refused (EINVAL). */
static int send_kind(struct rig *r, int fd, rights_t *rights)
{
    uint8_t m[8];
    wr_msg(m, 2, 8);
    static struct batch bt;
    errno = 0;
    if (wr_send(r->fd, m, 8, &fd, 1) != 8) {
        CHECK(errno == EINVAL && wr_comp_read(r, &bt) == ERR_SHOULD_WAIT);
        return -1;
    }
    if (wr_comp_read(r, &bt) != OK || bt.nh != 1)
        return -2;
    struct handle_info hi = { 0 };
    (void)jam_handle_info(bt.h[0], &hi);
    *rights = hi.rights;
    wr_close_handles(bt.h, bt.nh);
    return (int)hi.type;
}

static void check_kinds(void)
{
    struct rig r;
    CHECK(wr_open(&r));
    int p[2], sp[2];
    CHECK(pipe(p) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    int memfd = wr_memfd_with("m"), shm = shm_open("/wl-test", O_RDWR | O_CREAT, 0600);
    rights_t rt;
    CHECK(send_kind(&r, memfd, &rt) == OBJ_VMO && rt == WR_POOL_RIGHTS);
    CHECK(shm >= 0 && send_kind(&r, shm, &rt) == OBJ_VMO && rt == WR_POOL_RIGHTS);
    CHECK(send_kind(&r, p[0], &rt) == OBJ_CHANNEL && (rt & RIGHT_READ) && !(rt & RIGHT_WRITE));
    CHECK(send_kind(&r, p[1], &rt) == OBJ_CHANNEL && (rt & RIGHT_WRITE) && !(rt & RIGHT_READ));
    CHECK(send_kind(&r, sp[0], &rt) == OBJ_CHANNEL && (rt & RIGHT_READ) && (rt & RIGHT_WRITE));
    int ev = eventfd(0, 0), null = open("/dev/null", O_RDWR);
    int inet = socket(AF_INET, SOCK_STREAM, 0), wl = wr_sock_new();
    CHECK(send_kind(&r, ev, &rt) == -1);
    CHECK(send_kind(&r, null, &rt) == -1);
    CHECK(send_kind(&r, inet, &rt) == -1);
    CHECK(send_kind(&r, wl, &rt) == -1);
    CHECK(send_kind(&r, r.fd, &rt) == -1);
    uint8_t m[8];
    wr_msg(m, 2, 8);
    int bad = 999;
    errno = 0;
    CHECK(wr_send(r.fd, m, 8, &bad, 1) == -1 && errno == EBADF);
    int fds[] = { p[0], p[1], sp[1], memfd };   /* a mix: all four ride one batch */
    static struct batch bt;
    CHECK(wr_send(r.fd, m, 8, fds, 4) == 8 && wr_comp_read(&r, &bt) == OK && bt.nh == 4);
    wr_close_handles(bt.h, bt.nh);
    int all[] = { p[0], p[1], sp[0], sp[1], memfd, shm, ev, null, inet, wl };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        close(all[i]);
    shm_unlink("/wl-test");
    wr_close(&r);
}

/* ---- receiving ------------------------------------------------------------------------ */

static void check_received(int fd, const char *text)
{
    char buf[16] = { 0 };
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 4096);
    CHECK(pread(fd, buf, strlen(text), 0) == (ssize_t)strlen(text) && !strcmp(buf, text));
    char *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(p != MAP_FAILED && !memcmp(p, text, strlen(text)));
    if (p != MAP_FAILED)
        munmap(p, 4096);
}

static void check_receive(void)
{
    struct rig r;
    CHECK(wr_open(&r));
    handle_t ch[2];
    CHECK(jam_channel_create(&ch[0], &ch[1]) == OK);
    handle_t h[3] = { wr_vmo_with("plain", RIGHT_SAME),
                      wr_vmo_with("keymap", RIGHT_READ | RIGHT_MAP | RIGHT_TRANSFER), ch[0] };
    uint8_t m[8];
    wr_msg(m, 0xff000001u, 8);
    CHECK(wr_comp_write(&r, m, 8, h, 3) == OK);
    struct rx got;
    uint8_t in[8];
    CHECK(wr_recv(r.fd, in, 8, 8, &got) == 8 && got.n == 3 && !(got.flags & MSG_CTRUNC));
    CHECK(!memcmp(in, m, 8));
    if (got.n == 3) {
        int *fds = got.fds;
        check_received(fds[0], "plain");
        check_received(fds[1], "keymap");   /* no RIGHT_DUPLICATE: copied, still mappable */
        CHECK(fcntl(fds[2], F_GETFD) == FD_CLOEXEC);
        CHECK(write(fds[2], "x", 1) == 1);   /* a channel end: a socketpair end that writes */
        char c = 0;
        uint32_t nb = 0;
        struct channel_read_args a = { .h = ch[1], .rd_cap = 1, .rd_bytes = (uintptr_t)&c,
                                       .rd_actual = (uintptr_t)&nb };
        CHECK(jam_channel_read(&a) == OK && nb == 1 && c == 'x');
        for (unsigned i = 0; i < got.n; i++)
            close(fds[i]);
    }
    jam_handle_close(ch[1]);
    wr_close(&r);
}

/* Too little room: what fits, the rest closed, MSG_CTRUNC; read() closes
 * them all. Counted by the job's handles. */
static void check_ctrunc(void)
{
    struct rig r;
    CHECK(wr_open(&r));
    uint64_t before = wr_job_handles();
    handle_t h[3] = { wr_vmo_with("a", RIGHT_SAME), wr_vmo_with("b", RIGHT_SAME),
                      wr_vmo_with("c", RIGHT_SAME) };
    uint8_t m[16];
    wr_msg(m, 0xff000001u, 8);
    wr_msg(m + 8, 0xff000002u, 8);
    CHECK(wr_comp_write(&r, m, 16, h, 3) == OK);
    struct rx got, more;
    uint8_t in[16];
    /* Room for 2 (CMSG_SPACE of 2 is that of 1: Linux fills what the
     * room holds, and so do we). */
    CHECK(wr_recv(r.fd, in, 8, 2, &got) == 8 && got.n == 2 && (got.flags & MSG_CTRUNC));
    CHECK(wr_job_handles() == before + 2);   /* two installed; the third closed */
    CHECK(wr_recv(r.fd, in + 8, 8, 8, &more) == 8 && more.n == 0 && !more.flags);
    CHECK(!memcmp(in, m, 16));
    for (unsigned i = 0; i < got.n; i++)
        close(got.fds[i]);
    h[0] = wr_vmo_with("d", RIGHT_SAME);
    h[1] = wr_vmo_with("e", RIGHT_SAME);
    CHECK(wr_comp_write(&r, m, 8, h, 2) == OK);
    CHECK(read(r.fd, in, 8) == 8);   /* read: no room for any */
    CHECK(wr_job_handles() == before);
    wr_close(&r);
}

/* A batch that breaks JWL2: ECONNRESET once, then the end; writes EPIPE. */
static void check_broken_one(uint32_t magic, uint32_t count, bool event)
{
    struct rig r;
    CHECK(wr_open(&r));
    uint64_t before = wr_job_handles();
    handle_t h = HANDLE_INVALID;
    if (event)
        CHECK(jam_event_create(&h) == OK);
    else
        h = wr_vmo_with("v", RIGHT_SAME);
    uint8_t m[8];
    wr_msg(m, 0xff000001u, 8);
    const uint32_t hdr[2] = { magic, count };
    CHECK(wr_comp_write_hdr(&r, hdr, m, 8, &h, 1) == OK);
    uint8_t in[8];
    errno = 0;
    CHECK(read(r.fd, in, 8) == -1 && errno == ECONNRESET);
    CHECK(read(r.fd, in, 8) == 0);
    errno = 0;
    CHECK(send(r.fd, m, 8, MSG_NOSIGNAL) == -1 && errno == EPIPE);
    struct pollfd p = { .fd = r.fd, .events = POLLIN | POLLOUT };
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN) && (p.revents & POLLOUT));
    CHECK(wr_job_handles() == before);   /* the batch's handle closed */
    wr_close(&r);
}

static void check_broken(void)
{
    check_broken_one(0x12345678u, 1, false);   /* a bad magic */
    check_broken_one(JWL_MAGIC, 2, false);     /* a count that lies */
    check_broken_one(JWL_MAGIC, 1, true);      /* an event: no kind the compositor sends */
}

/* ---- waiting ---------------------------------------------------------------------------- */

static void check_full(void)
{
    struct rig r;
    CHECK(wr_open(&r));
    uint8_t m[8];
    wr_msg(m, 2, 8);
    int sent = 0;
    while (sent < 5000 && wr_send(r.fd, m, 8, NULL, 0) == 8)
        sent++;
    CHECK(sent == CHANNEL_MAX_QUEUED && errno == EAGAIN);   /* a batch each: the queue full */
    struct pollfd p = { .fd = r.fd, .events = POLLOUT };
    CHECK(poll(&p, 1, 0) == 0);
    int memfd = wr_memfd_with("x");
    errno = 0;
    CHECK(wr_send(r.fd, m, 1, &memfd, 1) == -1 && errno == EAGAIN);   /* no room: not taken */
    static struct batch bt;
    CHECK(wr_comp_read(&r, &bt) == OK);
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT));
    CHECK(wr_send(r.fd, m, 8, &memfd, 1) == 8);
    int n = 0, with = 0;
    while (wr_comp_read(&r, &bt) == OK) {
        n++;
        with += bt.nh == 1 && n == CHANNEL_MAX_QUEUED;   /* the last carried the memfd */
        wr_close_handles(bt.h, bt.nh);
    }
    CHECK(n == CHANNEL_MAX_QUEUED && with == 1);
    close(memfd);
    wr_close(&r);
}

static void check_poll(void)
{
    struct rig r;
    CHECK(wr_open(&r));
    struct pollfd p = { .fd = r.fd, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 0);
    uint8_t m[16], in[16];
    wr_msg(m, 0xff000001u, 16);
    CHECK(wr_comp_write(&r, m, 16, NULL, 0) == OK);
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
    CHECK(read(r.fd, in, 4) == 4);
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));   /* libc holds 12 more */
    CHECK(read(r.fd, in + 4, 16) == 12 && !memcmp(in, m, 16));
    CHECK(poll(&p, 1, 0) == 0);
    errno = 0;
    CHECK(read(r.fd, in, 16) == -1 && errno == EAGAIN);
    jam_handle_close(r.comp);   /* the compositor goes */
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLHUP));
    CHECK(read(r.fd, in, 16) == 0);
    errno = 0;
    CHECK(send(r.fd, m, 16, MSG_NOSIGNAL) == -1 && errno == EPIPE);
    close(r.fd);
}

/* ---- a memfd from another process ------------------------------------------------------- */

static void check_from_child(void)
{
    int memfd = wr_memfd_with("from the parent");
    struct stat st;
    CHECK(fstat(memfd, &st) == 0);
    char koid[24];
    snprintf(koid, sizeof(koid), "%llu", (unsigned long long)st.st_ino);   /* the VMO's */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, memfd, 3);
    char *argv[] = { "posixtest", "--child-wl", koid, NULL };
    pid_t pid = -1;
    CHECK(posix_spawn(&pid, SELF, &fa, NULL, argv, environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    int s = -1;
    CHECK(pid > 0 && waitpid(pid, &s, 0) == pid && WIFEXITED(s) && WEXITSTATUS(s) == 0);
    close(memfd);
}

/* The child: descriptor 3 is its parent's memfd; sent on, the same VMO
 * arrives, and the compositor's kept-only map takes it. */
int pt_child_wl(int argc, char **argv)
{
    struct rig r;
    if (argc < 3 || !wr_open(&r))
        return 1;
    uint8_t m[8];
    wr_msg(m, 2, 8);
    int fd = 3;
    static struct batch bt;
    struct handle_info hi;
    char text[16];
    uint64_t at = 0;
    bool ok = wr_send(r.fd, m, 8, &fd, 1) == 8 && wr_comp_read(&r, &bt) == OK && bt.nh == 1 &&
              jam_handle_info(bt.h[0], &hi) == OK &&
              hi.koid == strtoull(argv[2], NULL, 10) &&
              jam_vmo_read(bt.h[0], 0, text, 15) == OK && !memcmp(text, "from the parent", 15) &&
              jam_vmar_map(startup_handle(SR_SELF_VMAR), bt.h[0], 0, 4096,
                           VMAR_READ | VMAR_KEPT_ONLY, &at) == OK;
    if (at)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), at, 4096);
    wr_close_handles(bt.h, bt.nh);
    wr_close(&r);
    return ok ? 0 : 1;
}

void pt_wayland(char **argv)
{
    (void)argv;
    check_no_socket();
    check_whole();
    check_split();
    check_28();
    check_kinds();
    check_receive();
    check_ctrunc();
    check_broken();
    check_full();
    check_poll();
    check_from_child();
    wr_fuzz();
}
