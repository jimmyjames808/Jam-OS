/* The scm section's second file: Mojo's pattern between two processes,
 * and the fuzz loop.
 *
 * **Mojo's pattern** (scm_mojo and the child, posixtest --child-scm): a
 * non-blocking socketpair, one end the child's descriptor 3 (posix_spawn's
 * adddup2, as Chromium names it on the command line). The parent sends
 * one byte with six descriptors (a pipe's write end, a memfd, a file at
 * offset 1, /dev/zero, a socketpair end, and the child's own
 * /proc/<pid>/statm opened by the parent, as Chromium's browser gives each
 * renderer: the child's link names it, it reads), then 3 MB with a memfd on the
 * first byte, each with MSG_DONTWAIT | MSG_NOSIGNAL, partial writes and
 * EAGAIN met by poll; the child uses each descriptor (writes the pipe and
 * the socketpair, writes into the memfd's mapping, reads the file and
 * /dev/zero), checks every byte, answers "OK", and reads on until the
 * parent's shutdown(SHUT_RDWR) ends the stream.
 *
 * **The fuzz loop** (FUZZ_ROUNDS rounds, a fixed seed): each round wraps
 * two raw channel pairs' ends as socketpair ends (the layer's adopters, as
 * a hand-over makes them), sends random bytes with random descriptors
 * through the first, takes the parts off the raw channel, mutates them
 * (bytes changed, handles dropped or added of any kind, cut short, random
 * messages between) and writes them into the second, whose reader takes
 * them with random room and flags. Nothing may crash, and at the end the
 * job holds the handles it held before (scm_job_handles: net of libc's
 * own mappings) and the lowest free descriptor is the one it was. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../sys/kinds.h"
#include "pt.h"
#include "ptscm.h"

#define SELF        "/boot/bin/posixtest"
#define BIG         3000000u   /* the big send's bytes: past the channel's bound */
#define WAIT_MS     10000
#define FUZZ_ROUNDS 10000

extern char **environ;

static uint8_t pattern(size_t i)
{
    return (uint8_t)((i * 2654435761u) >> 13);
}

/* ---- Mojo's pattern ------------------------------------------------------------------- */

/* All n bytes (fds with the first), non-blocking, waiting in poll. */
static bool send_all(int s, const uint8_t *b, size_t n, const int *fds, unsigned nf)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = done ? send(s, b + done, n - done, MSG_DONTWAIT | MSG_NOSIGNAL)
                         : scm_send(s, b, n, fds, nf, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        struct pollfd pf = { s, POLLOUT, 0 };
        if (r == 0 || errno != EAGAIN || poll(&pf, 1, WAIT_MS) != 1)
            return false;
    }
    return true;
}

/* Some bytes (at most n), non-blocking, waiting in poll: the count, 0 at
 * the end, -1 on an error or a timeout. */
static ssize_t recv_some(int s, void *b, size_t n, struct scm_got *g)
{
    for (;;) {
        ssize_t r = scm_recv(s, b, n, 8, MSG_DONTWAIT | MSG_CMSG_CLOEXEC, g);
        if (r >= 0 || errno != EAGAIN)
            return r;
        struct pollfd pf = { s, POLLIN, 0 };
        if (poll(&pf, 1, WAIT_MS) != 1)
            return -1;
    }
}

/* The child's use of the five: 0 if each did what it should. */
/* The sixth: the parent's open of /proc/<our pid>/statm (Chromium's browser
 * gives each renderer its own so): ours, read live. */
static int use_proc(int f)
{
    char want[48], link[64], text[128], path[32];
    snprintf(want, sizeof(want), "/proc/%d/statm", (int)getpid());
    snprintf(path, sizeof(path), "/proc/self/fd/%d", f);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n != (ssize_t)strlen(want) || memcmp(link, want, (size_t)n))
        return 1;
    unsigned long size = 0;
    n = pread(f, text, sizeof(text) - 1, 0);
    if (n <= 0)
        return 2;
    text[n] = '\0';
    return sscanf(text, "%lu", &size) == 1 && size > 0 ? 0 : 3;
}

static int use_five(const int *f)
{
    char buf[8];
    int bad = write(f[0], "pipe!", 5) != 5;
    char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f[1], 0);
    bad |= (m == MAP_FAILED || memcmp(m, "memfd!", 6)) << 1;
    if (m != MAP_FAILED) {
        memcpy(m + 100, "child", 5);
        munmap(m, 4096);
    }
    bad |= (read(f[2], buf, 3) != 3 || memcmp(buf, "ELF", 3)) << 2;
    memset(buf, 1, sizeof(buf));
    bad |= (read(f[3], buf, 4) != 4 || buf[0] || buf[3]) << 3;
    bad |= (write(f[4], "sock!", 5) != 5) << 4;
    return bad;
}

int pt_child_scm(void)
{
    const int s = 3;
    struct scm_got g = { .n = 0 };
    char k = 0;
    if (recv_some(s, &k, 1, &g) != 1 || k != 'K' || g.n != 6 || g.flags)
        return 10;
    int bad = use_five(g.fds);
    if (bad)
        return 20 + bad;
    bad = use_proc(g.fds[5]);
    if (bad)
        return 60 + bad;
    scm_close_got(&g);
    uint8_t *b = malloc(65536);
    size_t got = 0;
    while (b && got < BIG) {   /* the big one: a memfd with its first byte only */
        ssize_t n = recv_some(s, b, 65536, &g);
        if (n <= 0 || g.flags || g.n != (got ? 0u : 1u))
            return 11;
        char m[3];
        if (!got && (pread(g.fds[0], m, 3, 0) != 3 || memcmp(m, "big", 3)))
            return 12;
        scm_close_got(&g);
        for (ssize_t i = 0; i < n; i++)
            if (b[i] != pattern(got + (size_t)i))
                return 13;
        got += (size_t)n;
    }
    if (!b || send(s, "OK", 2, MSG_NOSIGNAL) != 2)
        return 14;
    ssize_t n = recv_some(s, b, 64, &g);   /* the parent's shutdown: the end */
    return n == 0 ? 0 : 15;
}

/* The child on s's other end as descriptor 3: its pid, or -1. */
static pid_t spawn_kid(int give)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, give, 3);
    char *argv[] = { SELF, "--child-scm", NULL };
    pid_t pid = -1;
    int e = posix_spawn(&pid, SELF, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    return e ? -1 : pid;
}

void scm_mojo(void)
{
    int s[2], p[2], q[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, s) == 0);
    pid_t pid = spawn_kid(s[1]);
    CHECK(pid > 0);
    close(s[1]);
    CHECK(pipe2(p, O_CLOEXEC) == 0 && socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, q) == 0);
    int m = memfd_create("mojo", MFD_CLOEXEC), m2 = memfd_create("mojobig", MFD_CLOEXEC);
    CHECK(m >= 0 && ftruncate(m, 4096) == 0 && pwrite(m, "memfd!", 6, 0) == 6);
    CHECK(m2 >= 0 && write(m2, "big", 3) == 3);
    int f = open(SELF, O_RDONLY | O_CLOEXEC), z = open("/dev/zero", O_RDONLY | O_CLOEXEC);
    CHECK(f >= 0 && lseek(f, 1, SEEK_SET) == 1 && z >= 0);
    char statm[32];   /* the child's own, opened here: Chromium's browser and renderer */
    snprintf(statm, sizeof(statm), "/proc/%d/statm", (int)pid);
    int sm = open(statm, O_RDONLY | O_CLOEXEC);
    CHECK(sm >= 0);
    const int six[] = { p[1], m, f, z, q[1], sm };
    CHECK(send_all(s[0], (const uint8_t *)"K", 1, six, 6));
    close(sm);
    uint8_t *big = malloc(BIG);
    for (size_t i = 0; big && i < BIG; i++)
        big[i] = pattern(i);
    CHECK(big && send_all(s[0], big, BIG, &m2, 1));
    free(big);
    char ok[2] = { 0 };
    int on = 1;   /* the answer's writer, as the kernel stamped it: the child */
    CHECK(setsockopt(s[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) == 0);
    struct pollfd pf = { s[0], POLLIN, 0 };
    CHECK(poll(&pf, 1, WAIT_MS) == 1);
    struct scm_cred c = scm_recv_cred(s[0], ok, 2, MSG_DONTWAIT, SCM_CRED_ROOM);
    CHECK(c.n == 2 && !memcmp(ok, "OK", 2) && c.pid == pid && c.pid != getpid());
    char buf[8];
    CHECK(read(p[0], buf, sizeof(buf)) == 5 && !memcmp(buf, "pipe!", 5));
    CHECK(read(q[0], buf, sizeof(buf)) == 5 && !memcmp(buf, "sock!", 5));
    CHECK(pread(m, buf, 5, 100) == 5 && !memcmp(buf, "child", 5));
    CHECK(shutdown(s[0], SHUT_RDWR) == 0);
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        printf("posixtest: scm: mojo child status %#x\n", status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    int all[] = { s[0], p[0], p[1], q[0], q[1], m, m2, f, z };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        close(all[i]);
}

/* ---- the fuzz loop ---------------------------------------------------------------------- */

uint64_t scm_seed = 0x6a616d4353ull;
static unsigned fuzz_fds, fuzz_cut, fuzz_bytes;   /* what the fuzz's reader got, for its line */

uint32_t scm_rnd(uint32_t n)
{
    scm_seed = scm_seed * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(scm_seed >> 33) % n;
}

handle_t scm_any_handle(void)
{
    handle_t h = HANDLE_INVALID, spare;
    uint32_t kind = scm_rnd(3);
    if (kind == 0 && jam_vmo_create(4096, VMO_KEEP_PAGES, &h) == OK)
        return h;
    if (kind == 1 && jam_channel_create(&h, &spare) == OK) {
        jam_handle_close(spare);
        return h;
    }
    CHECK(jam_event_create(&h) == OK);
    return h;
}

int scm_wrap(handle_t h, bool seq)
{
    struct fd_desc *d = NULL;
    const struct posixfds_fd rec = { .kind = seq ? POSIXFDS_SEQPACKET : POSIXFDS_PIPE,
                                     .nhandles = 1, .status = O_RDWR };
    long r = seq ? seqp_adopt(&rec, NULL, &h, &d) : pipe_adopt(&rec, NULL, &h, &d);
    return r ? -1 : (int)fd_install(d, 0, true);
}

bool scm_forward(handle_t from, handle_t to, uint8_t *b)
{
    handle_t hs[CHANNEL_MAX_HANDLES];
    uint32_t nb = 0, nh = 0;
    struct channel_read_args a = {
        .h = from, .rd_cap = CHANNEL_MAX_BYTES, .rd_bytes = (uint64_t)(uintptr_t)b,
        .rd_handles = (uint64_t)(uintptr_t)hs, .rd_hcap = CHANNEL_MAX_HANDLES,
        .rd_actual = (uint64_t)(uintptr_t)&nb, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    if (jam_channel_read(&a) != OK)
        return false;
    switch (scm_rnd(8)) {
    case 0:   /* bytes of the header and the first records changed */
        for (unsigned k = 1 + scm_rnd(4); nb && k; k--)
            b[scm_rnd(nb < 96 ? nb : 96)] = (uint8_t)scm_rnd(256);
        break;
    case 1:   /* a handle dropped */
        if (nh)
            jam_handle_close(hs[--nh]);
        break;
    case 2:   /* a handle of any kind added, or put in another's place */
        if (nh && scm_rnd(2)) {
            uint32_t i = scm_rnd(nh);
            jam_handle_close(hs[i]);
            hs[i] = scm_any_handle();
        } else if (nh < CHANNEL_MAX_HANDLES) {
            hs[nh++] = scm_any_handle();
        }
        break;
    case 3:   /* cut short */
        nb = scm_rnd(nb + 1);
        break;
    case 4: {   /* a random message first */
        uint8_t junk[64];
        handle_t jh[2];
        uint32_t jn = scm_rnd(3), jl = scm_rnd(sizeof(junk));
        for (uint32_t i = 0; i < jl; i++)
            junk[i] = (uint8_t)scm_rnd(256);
        if (jl >= 4 && scm_rnd(2))
            memcpy(junk, "JSCM", 4);
        for (uint32_t i = 0; i < jn; i++)
            jh[i] = scm_any_handle();
        (void)jam_channel_write(to, junk, jl, jh, jn);   /* the handles go, whatever */
        break;
    }
    default:
        break;
    }
    (void)jam_channel_write(to, b, nb, hs, nh);
    return true;
}

void scm_fuzz_send(int x, const int *pool, unsigned npool, bool seq, uint8_t *b)
{
    for (unsigned k = 1 + scm_rnd(3); k; k--) {
        int fds[80];
        unsigned nf = scm_rnd(3) ? 0 : 1 + scm_rnd(scm_rnd(4) ? 4 : 80);
        for (unsigned i = 0; i < nf; i++)
            fds[i] = pool[scm_rnd(npool)];
        size_t n = (seq ? 0 : 1) + scm_rnd(scm_rnd(4) ? 64 : 3000);
        for (size_t i = 0; i < n; i++)
            b[i] = (uint8_t)scm_rnd(256);
        if (scm_send(x, b, n, fds, nf, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
            break;
    }
}

static void fuzz_round(const int *pool, unsigned npool, uint8_t *b)
{
    handle_t a, rawa, c = HANDLE_INVALID, e;
    bool seq = scm_rnd(4) == 0;
    CHECK(jam_channel_create(&a, &rawa) == OK && jam_channel_create(&c, &e) == OK);
    int x = scm_wrap(a, seq), y = scm_wrap(e, seq);
    CHECK(x >= 0 && y >= 0);
    scm_fuzz_send(x, pool, npool, seq, b);
    while (scm_forward(rawa, c, b))
        ;
    if (scm_rnd(3) == 0) {
        jam_handle_close(c);   /* the sender gone: the end after what came */
        c = HANDLE_INVALID;
    }
    for (int i = 0; i < 10; i++) {
        struct scm_got g = { .n = 0 };
        int flags = MSG_DONTWAIT | (scm_rnd(4) ? 0 : MSG_PEEK) |
                    (scm_rnd(2) ? MSG_CMSG_CLOEXEC : 0);
        ssize_t n = scm_recv(y, b, 1 + scm_rnd(scm_rnd(4) ? 200 : CHANNEL_MAX_BYTES),
                             scm_rnd(5) ? scm_rnd(90) : 0, flags, &g);
        fuzz_fds += g.n;
        fuzz_cut += !!(g.flags & MSG_CTRUNC);
        fuzz_bytes += n > 0 ? (unsigned)n : 0;
        scm_close_got(&g);
        if (n < 0)
            break;
    }
    close(x);
    close(y);
    jam_handle_close(rawa);
    if (c != HANDLE_INVALID)
        jam_handle_close(c);
}

void scm_fuzz(void)
{
    int p[2], q[2], k[2];
    CHECK(pipe(p) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, q) == 0 &&
          socketpair(AF_UNIX, SOCK_SEQPACKET, 0, k) == 0);
    int pool[] = { memfd_create("fuzz", 0), p[0], p[1], q[1], k[1],
                   open("/dev/null", O_RDWR), open(SELF, O_RDONLY) };
    const unsigned npool = sizeof(pool) / sizeof(pool[0]);
    uint8_t *b = malloc(CHANNEL_MAX_BYTES);
    CHECK(b != NULL);
    if (!b)
        return;
    fuzz_round(pool, npool, b);   /* the first round's one-time allocations, before the count */
    int lowest = scm_lowest_fd();
    uint64_t before = scm_job_handles();
    int rounds = 0, said = 0;
    uint64_t was = before;
    for (int i = 0; i < FUZZ_ROUNDS; i++, rounds++) {
        uint64_t seed0 = scm_seed;
        fuzz_round(pool, npool, b);
        uint64_t now = scm_job_handles();
        if (now != was && said++ < 4)   /* which rounds keep handles: their seeds repeat them */
            printf("posixtest: scm: fuzz: round %d (seed %#llx): %llu handles, then %llu\n", i,
                   (unsigned long long)seed0, (unsigned long long)was, (unsigned long long)now);
        was = now;
    }
    CHECK(rounds == FUZZ_ROUNDS);
    uint64_t after = scm_job_handles();
    if (after != before)
        printf("posixtest: scm: fuzz: %llu handles before, %llu after\n",
               (unsigned long long)before, (unsigned long long)after);
    CHECK(after == before);
    CHECK(scm_lowest_fd() == lowest);
    for (unsigned i = 0; i < npool; i++)
        close(pool[i]);
    int rest[] = { p[0], p[1], q[0], q[1], k[0], k[1] };
    for (unsigned i = 0; i < 6; i++)
        close(rest[i]);
    free(b);
    printf("posixtest: scm: fuzz: %d rounds, %u bytes and %u descriptors received, %u cut\n",
           rounds, fuzz_bytes, fuzz_fds, fuzz_cut);
}
