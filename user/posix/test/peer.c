/* posixtest's peer section (docs/CR-REVIEW-POSIX.md, "Tests"): our own
 * IPC against a misbehaving peer in another process, so that a buggy or
 * hostile local program can't crash, hang or leak the program it talks
 * to. The receiver is a child of ours (posixtest --child-peer), started
 * with libjam's spawn in a job of its own, so its job's handles are its
 * own to count.
 *
 *   - Socketpairs: PAIR_ROUNDS rounds, each a fresh socketpair end given to
 *     the child over a control socketpair (its descriptor 3), one of four
 *     SOCK_SEQPACKET. We write into the end's raw channel what the scm
 *     section's fuzz makes (scmkid.c: our own sends of random bytes and
 *     random descriptors of every kind, taken off a raw channel and
 *     mutated: bytes changed, handles dropped, added or swapped for any
 *     kind of object, cut short, junk messages between), then close it.
 *   - Named sockets: UNIX_ROUNDS connections through /svc/unix to an
 *     abstract name the child listens on, each our raw end of the
 *     connection (the broker's protocol) written the same way and closed.
 *
 * The child reads each end until its end of file, with random room for
 * bytes and descriptors and random flags (MSG_PEEK, MSG_CMSG_CLOEXEC,
 * always MSG_DONTWAIT), waiting in poll between reads. It fails (its exit
 * code, and a line in the kernel log) if a recvmsg takes more than
 * RECV_SLOW, if a poll waits PEER_HANG for an end that is already closed,
 * if it read another number of ends than we sent, or if, after the last
 * round, its job holds other handles than after the
 * first (scm_job_handles) or its lowest free descriptor moved. We wait
 * PEER_WAIT for it; its being killed is a crash. The seed is fixed and
 * printed: a failing run is repeated by its seed (PEER_SEED).
 *
 *   - Finding 2: a file whose channel never answers, received with
 *     MSG_DONTWAIT: the recvmsg returns at once with its descriptor (the
 *     file asks its server nothing until a call needs it), and closing it
 *     doesn't wait either. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <idl/unix.h>
#include <jamos/libjam.h>
#include <poll.h>
#include <posixfds.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "pt.h"
#include "ptscm.h"

#define SELF        "/boot/bin/posixtest"
#define PEER_SEED   0x7065657232ull          /* "peer2" */
#define PAIR_ROUNDS 400
#define UNIX_ROUNDS 150
#define READS_MAX   48                       /* reads of one end at most */
#define RECV_SLOW   (1 * NS_PER_S)           /* a recvmsg with MSG_DONTWAIT takes no longer */
#define PEER_HANG   10000                    /* ms: a poll for a closed end's last bytes */
#define PEER_WAIT   (60 * NS_PER_S)          /* the child, all of it */
#define CONN_WAIT   (10 * NS_PER_S)          /* a connect, while the child's backlog is full */

enum { PEER_OK, PEER_SLOW, PEER_HUNG, PEER_LEAK, PEER_FD, PEER_FEW, PEER_SETUP };

static const char *const verdicts[] = {
    [PEER_OK] = "ok",
    [PEER_SLOW] = "a recvmsg was slow",
    [PEER_HUNG] = "a closed end never ended",
    [PEER_LEAK] = "its job kept handles",
    [PEER_FD] = "its lowest free descriptor moved",
    [PEER_FEW] = "didn't read every round",
    [PEER_SETUP] = "it couldn't start its side",
};

/* ---- the sender (us) ------------------------------------------------------------------- */

/* The scm fuzz's sends on a local wrapped pair, taken off its raw end,
 * mutated and written into to (then closed). */
static void spoil_into(handle_t to, const int *pool, unsigned npool, bool seq, uint8_t *b)
{
    handle_t a, rawa;
    CHECK(jam_channel_create(&a, &rawa) == OK);
    int x = scm_wrap(a, seq);
    CHECK(x >= 0);
    scm_fuzz_send(x, pool, npool, seq, b);
    while (scm_forward(rawa, to, b))
        ;
    close(x);
    jam_handle_close(rawa);
    jam_handle_close(to);   /* the end of it: the receiver reads to its end of file */
}

/* The child in mode ("pair" or "unix"; name: the abstract name it listens
 * on), to read `rounds` ends, in a job of its own, its descriptor 3 a
 * socketpair end whose other end is *ctl. Its process, or HANDLE_INVALID. */
static handle_t spawn_receiver(const char *mode, const char *name, int rounds, int *ctl,
                               handle_t *job)
{
    static const char *const ns[] = { "/svc/unix", NULL };
    char seed[24], count[16];
    snprintf(seed, sizeof(seed), "%llx", (unsigned long long)scm_seed);
    snprintf(count, sizeof(count), "%d", rounds);
    const char *const argv[] = { "posixtest", "--child-peer", mode, seed, name, count };
    handle_t mine, theirs, proc = HANDLE_INVALID;
    *ctl = -1;
    *job = HANDLE_INVALID;
    if (jam_channel_create(&mine, &theirs) != OK)
        return HANDLE_INVALID;
    *ctl = scm_wrap(mine, false);
    const struct posixfds_desc d = { .fd = 3, .kind = POSIXFDS_PIPE, .status = O_RDWR,
                                     .handles = &theirs };
    const struct posixfds_out ho = { .state = { .umask = 022 }, .cwd = "/", .fds = &d, .nfds = 1 };
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, job);
    if (st == OK) {
        struct spawn_args a = { .path = "bin/posixtest", .argc = 6, .argv = argv, .job = *job,
                                .ns = ns, .handover = &ho };
        st = spawn(&a, &proc);   /* theirs goes with the hand-over */
    } else {
        jam_handle_close(theirs);
    }
    CHECK(st == OK && *ctl >= 0);
    return st == OK ? proc : HANDLE_INVALID;
}

/* Wait for the receiver: its verdict (PEER_*), or -1 if it was killed or
 * never ended (then its job is killed). */
static int receiver_done(handle_t proc, handle_t job)
{
    struct process_info info = { 0 };
    status_t st = proc ? spawn_wait(proc, PEER_WAIT, &info) : ERR_BAD_HANDLE;
    if (st != OK && job)
        jam_job_kill(job);
    if (proc)
        jam_handle_close(proc);
    if (job)
        jam_handle_close(job);
    return st == OK && !info.killed ? (int)info.exit_code : -1;
}

static void say_verdict(const char *what, int v)
{
    if (v != PEER_OK)
        printf("posixtest: peer: %s: the receiver %s\n", what,
               v < 0 ? "crashed or hung" : v < PEER_SETUP + 1 ? verdicts[v] : "said nonsense");
    CHECK(v == PEER_OK);
}

static void check_pairs(const int *pool, unsigned npool, uint8_t *b)
{
    int ctl;
    handle_t job, proc = spawn_receiver("pair", "-", PAIR_ROUNDS, &ctl, &job);
    int i = 0;
    for (; proc && i < PAIR_ROUNDS; i++) {
        bool seq = scm_rnd(4) == 0;
        handle_t raw, end;
        if (jam_channel_create(&raw, &end) != OK)
            break;
        int y = scm_wrap(end, seq);
        uint8_t kind = seq;
        ssize_t sent = scm_send(ctl, &kind, 1, &y, 1, MSG_NOSIGNAL);
        close(y);
        if (sent != 1) {
            jam_handle_close(raw);
            break;   /* the receiver is gone: its verdict says why */
        }
        spoil_into(raw, pool, npool, seq, b);
    }
    if (ctl >= 0)
        close(ctl);   /* the last round: the receiver counts and ends */
    say_verdict("socketpairs", receiver_done(proc, job));
    CHECK(i == PAIR_ROUNDS);
}

/* Our raw end of a connection to the abstract name, through the broker's
 * protocol; HANDLE_INVALID if none came in CONN_WAIT (the receiver's
 * backlog stays full, or it never listened). */
static handle_t connect_raw(const char *name)
{
    static const uint8_t zero_key[UNIX_KEY_BYTES];
    uint64_t end = now() + CONN_WAIT;
    for (;;) {
        handle_t b = svc_get(SVC_UNIX), c = HANDLE_INVALID;
        status_t st = b ? unix_connect_within(b, CONN_WAIT, (const uint8_t *)name,
                                              (uint32_t)strlen(name), zero_key, &c)
                        : ERR_NOT_FOUND;
        if (st == OK)
            return c;
        if ((st != ERR_SHOULD_WAIT && st != ERR_NOT_FOUND) || now() >= end)
            return HANDLE_INVALID;
        jam_sleep_until(now() + NS_PER_MS);   /* not listening yet, or its backlog full */
    }
}

static void check_named(const int *pool, unsigned npool, uint8_t *b)
{
    if (!svc_get(SVC_UNIX)) {
        printf("posixtest: peer: named sockets skipped: no /svc/unix\n");
        return;
    }
    char name[32];
    snprintf(name, sizeof(name), "pt-peer-%d", (int)getpid());
    int ctl;
    handle_t job, proc = spawn_receiver("unix", name, UNIX_ROUNDS, &ctl, &job);
    for (int i = 0; proc && i < UNIX_ROUNDS; i++) {
        handle_t raw = connect_raw(name);
        CHECK(raw != HANDLE_INVALID);
        if (!raw)
            break;
        spoil_into(raw, pool, npool, false, b);
    }
    if (ctl >= 0)
        close(ctl);
    say_verdict("named sockets", receiver_done(proc, job));
}

/* Finding 2: one file record whose channel is one of ours that never
 * answers, received with MSG_DONTWAIT. */
static void check_silent_file(uint8_t *b)
{
    handle_t a, rawa, c, e, quiet, silent;
    CHECK(jam_channel_create(&a, &rawa) == OK && jam_channel_create(&c, &e) == OK &&
          jam_channel_create(&quiet, &silent) == OK);
    int x = scm_wrap(a, false), y = scm_wrap(e, false), f = open(SELF, O_RDONLY);
    CHECK(x >= 0 && y >= 0 && f >= 0);
    CHECK(scm_send(x, "f", 1, &f, 1, 0) == 1);
    close(f);
    /* The file's channel swapped for `silent`, whose peer we hold and
     * never read. */
    handle_t hs[CHANNEL_MAX_HANDLES];
    uint32_t nb = 0, nh = 0;
    struct channel_read_args r = {
        .h = rawa, .rd_cap = CHANNEL_MAX_BYTES, .rd_bytes = (uint64_t)(uintptr_t)b,
        .rd_handles = (uint64_t)(uintptr_t)hs, .rd_hcap = CHANNEL_MAX_HANDLES,
        .rd_actual = (uint64_t)(uintptr_t)&nb, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    CHECK(jam_channel_read(&r) == OK);
    unsigned swapped = 0;
    for (uint32_t i = 0; i < nh && !swapped; i++) {
        struct handle_info hi;
        if (jam_handle_info(hs[i], &hi) == OK && hi.type == OBJ_CHANNEL) {
            jam_handle_close(hs[i]);
            hs[i] = silent;
            swapped = 1;
        }
    }
    CHECK(swapped);
    if (!swapped)
        jam_handle_close(silent);
    CHECK(jam_channel_write(c, b, nb, hs, nh) == OK);
    struct scm_got g = { .n = 0 };
    uint64_t t0 = now();
    ssize_t n = scm_recv(y, b, 16, 4, MSG_DONTWAIT, &g);
    uint64_t took = now() - t0;
    if (took > RECV_SLOW)
        printf("posixtest: peer: a silent file's recvmsg took %llu ms\n",
               (unsigned long long)(took / NS_PER_MS));
    CHECK(n == 1 && g.n == 1 && took <= RECV_SLOW);
    t0 = now();
    scm_close_got(&g);
    CHECK(now() - t0 <= RECV_SLOW);
    close(x);
    close(y);
    jam_handle_close(rawa);
    jam_handle_close(c);
    jam_handle_close(quiet);
}

void pt_peer(char **argv __attribute__((unused)))
{
    int p[2], q[2], k[2];
    CHECK(pipe(p) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, q) == 0 &&
          socketpair(AF_UNIX, SOCK_SEQPACKET, 0, k) == 0);
    int pool[] = { memfd_create("peer", 0), p[0], p[1], q[1], k[1],
                   open("/dev/null", O_RDWR), open(SELF, O_RDONLY) };
    const unsigned npool = sizeof(pool) / sizeof(pool[0]);
    uint8_t *b = malloc(CHANNEL_MAX_BYTES);
    CHECK(b != NULL);
    if (!b)
        return;
    scm_seed = PEER_SEED;
    printf("posixtest: peer: seed %#llx, %d socketpair rounds, %d connections\n",
           (unsigned long long)scm_seed, PAIR_ROUNDS, UNIX_ROUNDS);
    check_pairs(pool, npool, b);
    check_named(pool, npool, b);
    check_silent_file(b);
    for (unsigned i = 0; i < npool; i++)
        close(pool[i]);
    int rest[] = { p[0], p[1], q[0], q[1], k[0], k[1] };
    for (unsigned i = 0; i < 6; i++)
        close(rest[i]);
    free(b);
}

/* ---- the receiver (the child) ---------------------------------------------------------- */

/* "posixtest: peer child: <what>" in the kernel log (the child has no
 * descriptor 1). */
static void child_say(const char *what)
{
    char line[160];
    int n = snprintf(line, sizeof(line), "posixtest: peer child: %s\n", what);
    jam_debug_write(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

/* Read s until its end of file: PEER_OK, PEER_SLOW or PEER_HUNG. */
static int drain(int s, uint8_t *b)
{
    for (int i = 0; i < READS_MAX; i++) {
        struct pollfd pf = { .fd = s, .events = POLLIN };
        if (poll(&pf, 1, PEER_HANG) == 0) {
            child_say("a closed end never ended");
            return PEER_HUNG;
        }
        struct scm_got g = { .n = 0 };
        int flags = MSG_DONTWAIT | (scm_rnd(4) ? 0 : MSG_PEEK) |
                    (scm_rnd(2) ? MSG_CMSG_CLOEXEC : 0);
        size_t room = 1 + scm_rnd(scm_rnd(4) ? 200 : CHANNEL_MAX_BYTES);
        unsigned fds = scm_rnd(5) ? scm_rnd(90) : 0;
        uint64_t t0 = now();
        ssize_t n = scm_recv(s, b, room, fds, flags, &g);
        uint64_t took = now() - t0;
        scm_close_got(&g);
        if (took > RECV_SLOW) {
            child_say("a recvmsg was slow");
            return PEER_SLOW;
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
            break;   /* its end, or a stream it gave up on */
    }
    return PEER_OK;
}

/* The next socketpair end on the control socket, or -1 at its end. */
static int next_pair(int ctl, uint8_t *b)
{
    struct scm_got g = { .n = 0 };
    ssize_t n = scm_recv(ctl, b, 1, 1, MSG_CMSG_CLOEXEC, &g);
    if (n == 1 && g.n == 1)
        return g.fds[0];
    scm_close_got(&g);
    return -1;
}

/* The next connection to listener l, or -1 once the control socket has
 * ended and none waits. */
static int next_conn(int l, int ctl, bool *ctl_done)
{
    for (;;) {
        int c = accept4(l, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c >= 0)
            return c;
        if (*ctl_done)
            return -1;   /* every connect of the sender's returned before its end */
        struct pollfd pf[2] = { { .fd = l, .events = POLLIN }, { .fd = ctl, .events = POLLIN } };
        if (poll(pf, 2, (int)(PEER_WAIT / NS_PER_MS)) <= 0)
            return -1;
        uint8_t byte;
        if ((pf[1].revents & (POLLIN | POLLHUP)) && read(ctl, &byte, 1) <= 0)
            *ctl_done = true;
    }
}

int pt_child_peer(int argc, char **argv)
{
    if (argc < 6)
        return PEER_SETUP;
    scm_seed = strtoull(argv[3], NULL, 16) ^ 0x5a5a;   /* the receiver's own choices */
    int want = atoi(argv[5]), rounds = 0;
    bool unix_mode = !strcmp(argv[2], "unix");
    uint8_t *b = malloc(CHANNEL_MAX_BYTES);
    int ctl = 3, l = -1;
    if (unix_mode) {
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        size_t len = strlen(argv[4]);
        memcpy(a.sun_path + 1, argv[4], len);   /* an abstract name */
        l = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (l < 0 || bind(l, (struct sockaddr *)&a, (socklen_t)(sizeof(sa_family_t) + 1 + len)) ||
            listen(l, 8)) {
            child_say("no listener");
            return PEER_SETUP;
        }
    }
    if (!b)
        return PEER_SETUP;
    int verdict = PEER_OK, lowest = -1;
    uint64_t before = 0;
    bool ctl_done = false;
    for (; verdict == PEER_OK; rounds++) {
        int s = unix_mode ? next_conn(l, ctl, &ctl_done) : next_pair(ctl, b);
        if (s < 0)
            break;
        verdict = drain(s, b);
        close(s);
        if (rounds == 0) {   /* the first round's one-time allocations, before the count */
            lowest = scm_lowest_fd();
            before = scm_job_handles();
        }
    }
    if (verdict == PEER_OK && rounds != want) {
        char what[64];
        snprintf(what, sizeof(what), "%d rounds read, not %d", rounds, want);
        child_say(what);
        verdict = PEER_FEW;
    }
    if (verdict == PEER_OK && lowest >= 0 && scm_lowest_fd() != lowest) {
        child_say("its lowest free descriptor moved");
        verdict = PEER_FD;
    }
    uint64_t after = scm_job_handles();
    if (verdict == PEER_OK && lowest >= 0 && after != before) {
        char what[96];
        snprintf(what, sizeof(what), "%llu handles after the first round, %llu after the last",
                 (unsigned long long)before, (unsigned long long)after);
        child_say(what);
        verdict = PEER_LEAK;
    }
    free(b);
    return verdict;
}
