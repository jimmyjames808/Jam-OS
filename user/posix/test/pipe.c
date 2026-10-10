/* posixtest's pipe section: pipes and socketpair (docs/M13-PLAN.md
 * section 4).
 *
 *   - 1 GiB through a pipe between two threads, written and read in
 *     changing sizes, every byte checked;
 *   - PIPE_BUF atomicity: four threads each write PIPE_BUF-byte records
 *     at once, the reader reads in odd sizes, and every record arrives
 *     whole, each writer's in order;
 *   - end of file only after the last writer closes (a dup of the write
 *     end, closed by another thread later);
 *   - EPIPE once the reader is gone (SIGPIPE ignored), EAGAIN on a full pipe with
 *     O_NONBLOCK, pipe2's flags, a leftover read in pieces, writev as one
 *     message, FIONREAD, the errors (EBADF, ESPIPE, EINVAL);
 *   - socketpair: both ends both ways, a socket to fstat;
 *   - the bench: bytes a second through a pipe between two threads, 4 KiB
 *     and 64 KiB writes (BENCH.md's line). */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define BIG_BYTES   (1ull << 30)       /* the checked transfer */
#define PAT_PERIOD  ((1u << 20) + 8)   /* the pattern repeats every this many bytes */
#define CHUNK_MAX   (200u * 1024)      /* the most one write or read in the checked run */
#define WRITERS     4
#define RECORDS     2000               /* each writer's PIPE_BUF records */
#define BENCH_BYTES (256ull << 20)     /* each bench run */

static uint8_t *pattern;               /* 2 * PAT_PERIOD bytes: byte i is pattern[i % PAT_PERIOD] */

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* A size for step i of a run: from 1 byte to CHUNK_MAX, many of them
 * across the 64 KiB a message holds. */
static size_t step_size(uint64_t i)
{
    static const size_t sizes[] = { 1, 7, 4096, 65536, 65537, 100000, 3, 131072, CHUNK_MAX, 512 };
    return sizes[(i * 7 + (i >> 3)) % (sizeof(sizes) / sizeof(sizes[0]))];
}

struct big_run {
    int      fd;       /* the write end */
    uint64_t bytes;    /* to write */
    size_t   fixed;    /* 0: changing sizes, else every write this size */
    bool     ok;       /* every write went whole */
};

static void *big_writer(void *arg)
{
    struct big_run *r = arg;
    r->ok = true;
    for (uint64_t off = 0, i = 0; off < r->bytes && r->ok; i++) {
        size_t n = r->fixed ? r->fixed : step_size(i);
        if (n > r->bytes - off)
            n = (size_t)(r->bytes - off);
        r->ok = write(r->fd, pattern + off % PAT_PERIOD, n) == (ssize_t)n;
        off += n;
    }
    close(r->fd);
    return NULL;
}

/* Read the pipe to its end, checking against the pattern if check:
 * the bytes read, or 0 at a mismatch or an error. */
static uint64_t read_all(int fd, bool check)
{
    uint8_t *buf = malloc(CHUNK_MAX);
    uint64_t off = 0;
    for (uint64_t i = 3; buf; i++) {
        ssize_t n = read(fd, buf, check ? step_size(i) : CHUNK_MAX);
        if (n == 0)
            break;
        if (n < 0 || (check && memcmp(buf, pattern + off % PAT_PERIOD, (size_t)n))) {
            off = 0;
            break;
        }
        off += (uint64_t)n;
    }
    free(buf);
    return off;
}

/* bytes through a new pipe from a second thread: the bytes read (and the
 * ns it took). */
static uint64_t run_through(uint64_t bytes, size_t fixed, bool check, uint64_t *ns)
{
    int p[2];
    CHECK(pipe(p) == 0);
    struct big_run r = { .fd = p[1], .bytes = bytes, .fixed = fixed };
    pthread_t t;
    uint64_t t0 = now_ns();
    CHECK(pthread_create(&t, NULL, big_writer, &r) == 0);
    uint64_t got = read_all(p[0], check);
    CHECK(pthread_join(t, NULL) == 0);
    *ns = now_ns() - t0;
    CHECK(r.ok);
    close(p[0]);
    return got;
}

static void check_gib(void)
{
    uint64_t ns;
    CHECK(run_through(BIG_BYTES, 0, true, &ns) == BIG_BYTES);
    printf("posixtest: pipe: 1 GiB checked in %llu ms\n", (unsigned long long)(ns / 1000000));
}

static void bench(void)
{
    static const size_t sizes[] = { 4096, 65536 };
    for (unsigned i = 0; i < 2; i++) {
        uint64_t ns;
        CHECK(run_through(BENCH_BYTES, sizes[i], false, &ns) == BENCH_BYTES);
        printf("posixtest: bench: pipe %zu KiB writes: %llu MB/s\n", sizes[i] / 1024,
               (unsigned long long)(BENCH_BYTES * 1000 / (ns ? ns : 1)));
    }
}

/* ---- PIPE_BUF atomicity -------------------------------------------------- */

struct atom_writer {
    int fd;      /* the shared write end */
    int id;      /* 0 .. WRITERS - 1 */
    bool ok;
};

static void fill_record(uint8_t *rec, int id, uint32_t seq)
{
    memset(rec, 'a' + id, PIPE_BUF);
    memcpy(rec, &seq, sizeof(seq));
}

static void *atom_writer(void *arg)
{
    struct atom_writer *w = arg;
    uint8_t rec[PIPE_BUF];
    w->ok = true;
    for (uint32_t seq = 0; seq < RECORDS && w->ok; seq++) {
        fill_record(rec, w->id, seq);
        w->ok = write(w->fd, rec, PIPE_BUF) == PIPE_BUF;
    }
    return NULL;
}

/* One record as read: whole (one writer's byte after the sequence
 * number) and in that writer's order. */
static bool record_ok(const uint8_t *rec, uint32_t next[WRITERS])
{
    int id = rec[PIPE_BUF - 1] - 'a';
    uint32_t seq;
    memcpy(&seq, rec, sizeof(seq));
    if (id < 0 || id >= WRITERS || seq != next[id])
        return false;
    for (size_t i = sizeof(seq); i < PIPE_BUF; i++)
        if (rec[i] != 'a' + id)
            return false;
    next[id]++;
    return true;
}

static void check_atomic(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    struct atom_writer w[WRITERS];
    pthread_t t[WRITERS];
    for (int i = 0; i < WRITERS; i++) {
        w[i] = (struct atom_writer){ .fd = p[1], .id = i };
        CHECK(pthread_create(&t[i], NULL, atom_writer, &w[i]) == 0);
    }
    uint8_t rec[PIPE_BUF];
    uint32_t next[WRITERS] = { 0 };
    size_t have = 0, odd = 1000;   /* reads of 1000 bytes: never a record's edge */
    unsigned whole = 0;
    bool ok = true;
    while (ok && whole < WRITERS * RECORDS) {
        size_t want = PIPE_BUF - have < odd ? PIPE_BUF - have : odd;
        ssize_t n = read(p[0], rec + have, want);
        ok = n > 0;
        have += ok ? (size_t)n : 0;
        if (ok && have == PIPE_BUF) {
            ok = record_ok(rec, next);
            whole++;
            have = 0;
        }
    }
    CHECK(ok && whole == WRITERS * RECORDS);
    for (int i = 0; i < WRITERS; i++) {
        CHECK(pthread_join(t[i], NULL) == 0);
        CHECK(w[i].ok && next[i] == RECORDS);
    }
    close(p[1]);
    char c;
    CHECK(read(p[0], &c, 1) == 0);
    close(p[0]);
}

/* ---- the end of the file ------------------------------------------------- */

struct late_close {
    int fd;          /* closed after a while */
    bool closed;     /* set just before */
};

static void *close_late(void *arg)
{
    struct late_close *l = arg;
    struct timespec nap = { 0, 100 * 1000 * 1000 };
    nanosleep(&nap, NULL);
    CHECK(write(l->fd, "last", 4) == 4);
    __atomic_store_n(&l->closed, true, __ATOMIC_RELEASE);
    close(l->fd);
    return NULL;
}

static void check_eof(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    int w2 = dup(p[1]);
    CHECK(w2 > 2);
    close(p[1]);   /* one writer left */
    CHECK(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
    char buf[8];
    errno = 0;
    CHECK(read(p[0], buf, sizeof(buf)) == -1 && errno == EAGAIN);   /* not the end */
    CHECK(fcntl(p[0], F_SETFL, 0) == 0);
    struct late_close l = { .fd = w2 };
    pthread_t t;
    CHECK(pthread_create(&t, NULL, close_late, &l) == 0);
    CHECK(read(p[0], buf, sizeof(buf)) == 4 && !memcmp(buf, "last", 4));
    CHECK(read(p[0], buf, sizeof(buf)) == 0);   /* waited for the last writer */
    CHECK(__atomic_load_n(&l.closed, __ATOMIC_ACQUIRE));
    CHECK(pthread_join(t, NULL) == 0);
    close(p[0]);
}

/* ---- EPIPE, EAGAIN, the flags, the leftover ----------------------------- */

static void check_epipe_full(void)
{
    int p[2];
    CHECK(pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
    CHECK(fcntl(p[0], F_GETFD) == FD_CLOEXEC && fcntl(p[1], F_GETFD) == FD_CLOEXEC);
    CHECK((fcntl(p[1], F_GETFL) & (O_NONBLOCK | O_ACCMODE)) == (O_NONBLOCK | O_WRONLY));
    CHECK((fcntl(p[0], F_GETFL) & O_ACCMODE) == O_RDONLY);
    int writes = 0;
    while (write(p[1], "x", 1) == 1 && writes < 100000)
        writes++;
    CHECK(errno == EAGAIN && writes == 1024);   /* the queue: CHANNEL_MAX_QUEUED messages */
    char c;
    CHECK(read(p[0], &c, 1) == 1 && c == 'x');
    CHECK(write(p[1], "y", 1) == 1);   /* room again */
    close(p[0]);
    signal(SIGPIPE, SIG_IGN);   /* its default would end us: the signal section checks it */
    errno = 0;
    CHECK(write(p[1], "z", 1) == -1 && errno == EPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(p[1]);
    errno = 0;
    CHECK(pipe2(p, O_DIRECT) == -1 && errno == EINVAL);
}

static void check_shapes(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    char buf[16] = { 0 };
    errno = 0;
    CHECK(write(p[0], "x", 1) == -1 && errno == EBADF);
    CHECK(read(p[1], buf, 1) == -1 && errno == EBADF);
    CHECK(lseek(p[0], 0, SEEK_SET) == -1 && errno == ESPIPE);
    struct stat st, st2;
    CHECK(fstat(p[0], &st) == 0 && S_ISFIFO(st.st_mode) && fstat(p[1], &st2) == 0 &&
          st.st_ino == st2.st_ino);
    CHECK(write(p[1], "0123456789", 10) == 10);
    int avail = 0;
    CHECK(ioctl(p[0], FIONREAD, &avail) == 0 && avail == 10);
    CHECK(read(p[0], buf, 3) == 3 && !memcmp(buf, "012", 3));   /* the rest left over */
    CHECK(ioctl(p[0], FIONREAD, &avail) == 0 && avail == 7);
    CHECK(read(p[0], buf, sizeof(buf)) == 7 && !memcmp(buf, "3456789", 7));
    struct iovec v[3] = { { "ab", 2 }, { "cde", 3 }, { "f", 1 } };
    CHECK(writev(p[1], v, 3) == 6);
    CHECK(write(p[1], "g", 1) == 1);
    CHECK(read(p[0], buf, sizeof(buf)) == 6 && !memcmp(buf, "abcdef", 6));   /* one message */
    CHECK(read(p[0], buf, sizeof(buf)) == 1 && buf[0] == 'g');
    CHECK(fcntl(p[0], F_GETPIPE_SZ) == 65536);
    close(p[0]);
    close(p[1]);
}

static void check_socketpair(void)
{
    int s[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s) == 0);
    char buf[8];
    CHECK(write(s[0], "ping", 4) == 4 && read(s[1], buf, sizeof(buf)) == 4 &&
          !memcmp(buf, "ping", 4));
    CHECK(write(s[1], "pong", 4) == 4 && read(s[0], buf, sizeof(buf)) == 4 &&
          !memcmp(buf, "pong", 4));
    struct stat st;
    CHECK(fstat(s[0], &st) == 0 && S_ISSOCK(st.st_mode));
    CHECK(fcntl(s[1], F_GETFD) == FD_CLOEXEC);
    close(s[1]);
    CHECK(read(s[0], buf, sizeof(buf)) == 0);
    close(s[0]);
    errno = 0;
    CHECK(socketpair(AF_INET, SOCK_STREAM, 0, s) == -1 && errno == EAFNOSUPPORT);
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, s) == -1 && errno == EOPNOTSUPP);
}

void pt_pipe(char **argv __attribute__((unused)))
{
    pattern = malloc(2 * PAT_PERIOD);
    CHECK(pattern != NULL);
    if (!pattern)
        return;
    for (size_t i = 0; i < PAT_PERIOD; i++)
        pattern[i] = (uint8_t)((i * 2654435761u) >> 13);
    memcpy(pattern + PAT_PERIOD, pattern, PAT_PERIOD);
    check_shapes();
    check_epipe_full();
    check_eof();
    check_atomic();
    check_socketpair();
    check_gib();
    bench();
    free(pattern);
}
