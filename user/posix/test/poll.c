/* posixtest's poll section: poll, ppoll, select, pselect and epoll on
 * pipes.
 *
 *   - 200 pipes, each written once, in a random order, from another
 *     thread: a loop polling all 200 sees every write exactly once, and
 *     a non-blocking read after each POLLIN finds the byte;
 *   - O_NONBLOCK against poll: an empty pipe is EAGAIN and not POLLIN; a
 *     full one is EAGAIN and not POLLOUT, until a read makes room; bytes
 *     kept from a message read in part are POLLIN while the channel is
 *     empty;
 *   - hang-ups: a read end whose writer closed is POLLHUP, a write end
 *     whose reader closed POLLERR; a closed descriptor is POLLNVAL;
 *   - a poll woken by a write from another process (a child polling its
 *     descriptor 0, a pipe from the hand-over);
 *   - a poll with no descriptors as a sleep; ppoll's timeout; a signal
 *     ending a poll with EINTR (even with SA_RESTART), a bare
 *     thread_interrupt not ending it; a thread's set (its bindings) gone with
 *     the thread;
 *   - select and pselect on the same pipes;
 *   - an epoll with 256 entries (and the 257th refused), level-triggered
 *     reports in turns of 16, EPOLLET (on a socketpair too: a read edge
 *     while it stays writable), EPOLLONESHOT, a closed descriptor leaving
 *     the set, EPERM for the null device. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <poll.h>
#include <posixfds.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define PIPES      200
#define EPOLL_N    256
#define CHILD_WAIT (30 * NS_PER_S)

static int rd[EPOLL_N], wr[EPOLL_N];
static uint32_t rng = 12345;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static bool open_pipes(int n, int flags)
{
    for (int i = 0; i < n; i++) {
        int p[2];
        if (pipe2(p, flags) != 0)
            return false;
        rd[i] = p[0];
        wr[i] = p[1];
    }
    return true;
}

static void close_pipes(int n)
{
    for (int i = 0; i < n; i++) {
        close(rd[i]);
        close(wr[i]);
    }
}

/* ---- 200 pipes, written once each at random ------------------------------------------- */

static void *write_each(void *arg __attribute__((unused)))
{
    int order[PIPES];
    for (int i = 0; i < PIPES; i++)
        order[i] = i;
    uint32_t r = 777;
    for (int i = PIPES - 1; i > 0; i--) {
        r = r * 1103515245u + 12345u;
        int j = (int)(r % (uint32_t)(i + 1)), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int i = 0; i < PIPES; i++) {
        unsigned char b = (unsigned char)order[i];
        if (write(wr[order[i]], &b, 1) != 1)
            return (void *)1;
        if (i % 8 == 0) {
            struct timespec nap = { 0, 200 * 1000 };
            nanosleep(&nap, NULL);
        }
    }
    return NULL;
}

static void check_200(void)
{
    CHECK(open_pipes(PIPES, O_NONBLOCK));
    static struct pollfd fds[PIPES];
    int seen[PIPES] = { 0 }, total = 0;
    pthread_t t;
    CHECK(pthread_create(&t, NULL, write_each, NULL) == 0);
    for (int guard = 0; total < PIPES && guard < 100000; guard++) {
        for (int i = 0; i < PIPES; i++)
            fds[i] = (struct pollfd){ .fd = rd[i], .events = POLLIN };
        int n = poll(fds, PIPES, 10000);
        CHECK(n > 0);
        if (n <= 0)
            break;
        int got = 0;
        for (int i = 0; i < PIPES; i++) {
            if (!fds[i].revents)
                continue;
            got++;
            CHECK(fds[i].revents == POLLIN);
            unsigned char b = 0;
            CHECK(read(rd[i], &b, 1) == 1 && b == (unsigned char)i);
            seen[i]++;
            total++;
        }
        CHECK(got == n);
    }
    void *res = (void *)1;
    CHECK(pthread_join(t, &res) == 0 && res == NULL);
    for (int i = 0; i < PIPES; i++)
        CHECK(seen[i] == 1);
    for (int i = 0; i < PIPES; i++)
        fds[i] = (struct pollfd){ .fd = rd[i], .events = POLLIN };
    CHECK(poll(fds, PIPES, 0) == 0);   /* nothing left, nothing invented */
    close_pipes(PIPES);
}

/* ---- O_NONBLOCK against poll ----------------------------------------------------------- */

static short one(int fd, short events)
{
    struct pollfd p = { .fd = fd, .events = events };
    return poll(&p, 1, 0) == 1 ? p.revents : 0;
}

static void check_nonblock(void)
{
    int p[2];
    char buf[64];
    CHECK(pipe2(p, O_NONBLOCK) == 0);
    errno = 0;
    CHECK(read(p[0], buf, 1) == -1 && errno == EAGAIN);
    CHECK(one(p[0], POLLIN) == 0);
    CHECK(one(p[1], POLLOUT) == POLLOUT);
    int n = 0;
    while (write(p[1], "x", 1) == 1 && n < 100000)
        n++;
    CHECK(errno == EAGAIN && n > 0 && n < 100000);
    CHECK(one(p[1], POLLOUT) == 0);   /* full: no POLLOUT, as the write said */
    CHECK(one(p[0], POLLIN) == POLLIN);
    CHECK(read(p[0], buf, 1) == 1);
    CHECK(one(p[1], POLLOUT) == POLLOUT);
    CHECK(write(p[1], "y", 1) == 1);
    close(p[0]);
    close(p[1]);

    CHECK(pipe2(p, O_NONBLOCK) == 0);
    CHECK(write(p[1], "hello", 5) == 5);   /* one message */
    CHECK(read(p[0], buf, 2) == 2);        /* the rest is kept by libc */
    CHECK(one(p[0], POLLIN) == POLLIN);    /* readable though the channel is empty */
    CHECK(read(p[0], buf, sizeof(buf)) == 3 && !memcmp(buf, "llo", 3));
    CHECK(one(p[0], POLLIN) == 0);
    close(p[0]);
    close(p[1]);
}

static void check_hangups(void)
{
    int p[2];
    CHECK(pipe2(p, 0) == 0);
    CHECK(write(p[1], "z", 1) == 1);
    close(p[1]);
    CHECK(one(p[0], POLLIN) == (POLLIN | POLLHUP));   /* the byte is still there */
    char c;
    CHECK(read(p[0], &c, 1) == 1 && read(p[0], &c, 1) == 0);
    CHECK(one(p[0], POLLIN) == POLLHUP);
    close(p[0]);
    CHECK(pipe2(p, 0) == 0);
    close(p[0]);
    CHECK(one(p[1], POLLOUT) & POLLERR);
    close(p[1]);
    CHECK(one(p[1], POLLIN) == POLLNVAL);
    struct pollfd neg = { .fd = -1, .events = POLLIN, .revents = 7 };
    CHECK(poll(&neg, 1, 0) == 0 && neg.revents == 0);
}

/* ---- another process --------------------------------------------------------------- */

/* The child: its 0 is a pipe from us, empty until it says it is waiting
 * (through 1), then written by us. */
int pt_child_poll(void)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 0);
    CHECK(write(1, "w", 1) == 1);
    uint64_t t0 = mono_ns();
    CHECK(poll(&p, 1, 20000) == 1 && p.revents == POLLIN);
    CHECK(mono_ns() - t0 < 20000000000ull);
    char c = 0;
    CHECK(read(0, &c, 1) == 1 && c == 'g');
    return pt_failures() ? 1 : 0;
}

static bool channel_byte(handle_t h, char *c)
{
    signals_t seen;
    if (jam_object_wait_one(h, SIG_READABLE, now() + CHILD_WAIT, &seen) != OK)
        return false;
    uint32_t got = 0, nh = 0;
    struct channel_read_args a = { .h = h, .rd_cap = 1, .rd_bytes = (uint64_t)(uintptr_t)c,
                                   .rd_actual = (uint64_t)(uintptr_t)&got,
                                   .rd_hactual = (uint64_t)(uintptr_t)&nh };
    return jam_channel_read(&a) == OK && got == 1;
}

static void check_other_process(void)
{
    handle_t in_r, in_w, out_r, out_w;
    CHECK(jam_channel_create(&in_r, &in_w) == OK && jam_channel_create(&out_r, &out_w) == OK);
    const struct posixfds_desc fds[] = {
        { .fd = 0, .kind = POSIXFDS_PIPE, .status = O_RDONLY, .handles = &in_r },
        { .fd = 1, .kind = POSIXFDS_PIPE, .status = O_WRONLY, .handles = &out_w },
    };
    const struct posixfds_out ho = { .state = { .umask = 022 }, .cwd = "/", .fds = fds, .nfds = 2 };
    const char *const argv[] = { "posixtest", "--child-poll", NULL };
    const char *const envp[] = { NULL };
    struct spawn_args a = { .path = "bin/posixtest", .argc = 2, .argv = argv,
                            .job = startup_handle(SR_JOB), .envp = envp, .handover = &ho };
    handle_t proc;
    status_t st = spawn(&a, &proc);
    CHECK(st == OK);
    char c = 0;
    CHECK(st == OK && channel_byte(out_r, &c) && c == 'w');   /* it is polling now, or about to */
    struct timespec nap = { 0, 30 * 1000 * 1000 };
    nanosleep(&nap, NULL);
    CHECK(jam_channel_write(in_w, "g", 1, NULL, 0) == OK);
    struct process_info info;
    CHECK(st == OK && spawn_wait(proc, CHILD_WAIT, &info) == OK && !info.killed &&
          info.exit_code == 0);
    if (st == OK)
        jam_handle_close(proc);
    jam_handle_close(in_w);
    jam_handle_close(out_r);
}

/* ---- sleeps, timeouts and interrupts ---------------------------------------------- */

static handle_t  main_thread;
static pthread_t main_pthread;

/* After 50 ms, a signal for the main thread (arg non-NULL), or a bare
 * interrupt with no signal behind it (thread_interrupt: the layer takes it
 * for a leftover and goes on waiting). */
static void *interrupt_soon(void *arg)
{
    struct timespec nap = { 0, 50 * 1000 * 1000 };
    nanosleep(&nap, NULL);
    if (arg)
        return (void *)(uintptr_t)(pthread_kill(main_pthread, SIGUSR1) != 0);
    return (void *)(uintptr_t)(jam_thread_interrupt(main_thread) != OK);
}

static void on_usr1(int sig)
{
    (void)sig;
}

static void check_sleeps(void)
{
    uint64_t t0 = mono_ns();
    CHECK(poll(NULL, 0, 50) == 0);
    CHECK(mono_ns() - t0 >= 50000000ull);
    struct timespec ts = { 0, 30 * 1000 * 1000 };
    t0 = mono_ns();
    CHECK(ppoll(NULL, 0, &ts, NULL) == 0);
    CHECK(mono_ns() - t0 >= 30000000ull);
    struct timespec bad = { 0, 1000000000L };
    CHECK(ppoll(NULL, 0, &bad, NULL) == -1 && errno == EINVAL);
    struct pollfd many = { .fd = 0 };
    CHECK(poll(&many, 1025, 0) == -1 && errno == EINVAL);

    main_thread = startup_handle(SR_SELF_THREAD);
    main_pthread = pthread_self();
    struct sigaction sa = { .sa_handler = on_usr1, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    pthread_t t;
    void *res = (void *)1;
    CHECK(pthread_create(&t, NULL, interrupt_soon, (void *)1) == 0);
    t0 = mono_ns();
    errno = 0;
    CHECK(poll(NULL, 0, 10000) == -1 && errno == EINTR);   /* never restarted */
    CHECK(mono_ns() - t0 < 5000000000ull);
    CHECK(pthread_join(t, &res) == 0 && res == NULL);
    signal(SIGUSR1, SIG_DFL);
    CHECK(pthread_create(&t, NULL, interrupt_soon, NULL) == 0);
    t0 = mono_ns();
    CHECK(poll(NULL, 0, 200) == 0);   /* no signal: no EINTR */
    CHECK(mono_ns() - t0 >= 200000000ull);
    CHECK(pthread_join(t, &res) == 0 && res == NULL);
}

static uint64_t msg_bytes(void)
{
    struct job_info ji;
    return jam_job_get_info(startup_handle(SR_JOB), &ji) == OK ? ji.used[JOB_LIMIT_MSG_BYTES] : 0;
}

static int thread_pipe;

static void *poll_once(void *arg __attribute__((unused)))
{
    struct pollfd p = { .fd = thread_pipe, .events = POLLIN };
    return (void *)(uintptr_t)(poll(&p, 1, 0) != 0);   /* binds it in this thread's set */
}

/* Each thread's set, and the bindings it keeps, go with the thread. */
static void check_thread_sets(void)
{
    int p[2];
    CHECK(pipe2(p, 0) == 0);
    thread_pipe = p[0];
    uint64_t before = msg_bytes();
    for (int i = 0; i < 8; i++) {
        pthread_t t;
        void *res = (void *)1;
        CHECK(pthread_create(&t, NULL, poll_once, NULL) == 0);
        CHECK(pthread_join(t, &res) == 0 && res == NULL);
    }
    CHECK(msg_bytes() == before);
    close(p[0]);
    close(p[1]);
}

/* ---- select and pselect ------------------------------------------------------------- */

static void check_select(void)
{
    int p[2], q[2];
    CHECK(pipe2(p, 0) == 0 && pipe2(q, 0) == 0);
    fd_set r, w, e;
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_SET(p[0], &r);
    FD_SET(q[0], &r);
    struct timeval tv = { 0, 20000 };
    CHECK(select(q[0] + 1, &r, NULL, NULL, &tv) == 0);   /* nothing: a timeout */
    CHECK(write(q[1], "s", 1) == 1);
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    FD_SET(q[0], &r);
    FD_SET(p[1], &w);
    CHECK(select(q[1] + 1, &r, &w, NULL, NULL) == 2);
    CHECK(!FD_ISSET(p[0], &r) && FD_ISSET(q[0], &r) && FD_ISSET(p[1], &w));
    FD_ZERO(&e);
    FD_SET(p[0], &e);
    struct timespec ts = { 0, 10 * 1000 * 1000 };
    CHECK(pselect(p[0] + 1, NULL, NULL, &e, &ts, NULL) == 0 && !FD_ISSET(p[0], &e));
    int dead = q[0];
    close(q[0]);
    FD_ZERO(&r);
    FD_SET(dead, &r);
    errno = 0;
    CHECK(select(dead + 1, &r, NULL, NULL, &tv) == -1 && errno == EBADF);
    close(p[0]);
    close(p[1]);
    close(q[1]);
}

/* ---- epoll --------------------------------------------------------------------------- */

/* Wait with a past deadline: the events in out (up to max). */
static int ep_now(int ep, struct epoll_event *out, int max)
{
    return epoll_wait(ep, out, max, 0);
}

static void check_epoll_256(int ep)
{
    CHECK(open_pipes(EPOLL_N, O_NONBLOCK));
    for (int i = 0; i < EPOLL_N; i++) {
        struct epoll_event ev = { .events = EPOLLIN, .data.u32 = (uint32_t)i };
        CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, rd[i], &ev) == 0);
    }
    struct epoll_event ev = { .events = EPOLLOUT };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, wr[0], &ev) == -1 && errno == ENOSPC);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, rd[3], &ev) == -1 && errno == EEXIST);
    struct epoll_event out[16];
    CHECK(ep_now(ep, out, 16) == 0);
    bool written[EPOLL_N] = { false };
    int nw = 0;
    for (int k = 0; k < 100; k++) {
        int i = (int)(rnd() % EPOLL_N);
        if (!written[i] && write(wr[i], "e", 1) == 1) {
            written[i] = true;
            nw++;
        }
    }
    int seen[EPOLL_N] = { 0 };
    for (int turn = 0; turn < 40; turn++) {   /* level: unread ones come back */
        int n = epoll_wait(ep, out, 16, 1000);
        CHECK(n > 0);
        for (int j = 0; j < n; j++) {
            uint32_t i = out[j].data.u32;
            CHECK(i < EPOLL_N && written[i] && out[j].events == EPOLLIN);
            if (i < EPOLL_N)
                seen[i]++;
        }
    }
    int distinct = 0;
    for (int i = 0; i < EPOLL_N; i++)
        distinct += seen[i] > 0;
    CHECK(distinct == nw);   /* turns of 16 reach every ready one: none starves */
    char c;
    for (int i = 0; i < EPOLL_N; i++)
        if (written[i])
            CHECK(read(rd[i], &c, 1) == 1);
    CHECK(ep_now(ep, out, 16) == 0);
    close_pipes(EPOLL_N);   /* every item leaves with its description */
}

static void check_epoll_modes(int ep)
{
    int p[2];
    char c;
    struct epoll_event out[4];
    CHECK(pipe2(p, O_NONBLOCK) == 0);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.u32 = 7 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(write(p[1], "a", 1) == 1);
    CHECK(ep_now(ep, out, 4) == 1 && out[0].data.u32 == 7);
    CHECK(ep_now(ep, out, 4) == 0);   /* edge: once */
    CHECK(read(p[0], &c, 1) == 1 && read(p[0], &c, 1) == -1 && errno == EAGAIN);
    CHECK(write(p[1], "b", 1) == 1);
    CHECK(ep_now(ep, out, 4) == 1);   /* drained, then written: an edge */
    CHECK(read(p[0], &c, 1) == 1);
    ev.events = EPOLLIN | EPOLLONESHOT;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0);
    CHECK(write(p[1], "c", 1) == 1);
    CHECK(ep_now(ep, out, 4) == 1);
    CHECK(ep_now(ep, out, 4) == 0);   /* one shot, though still readable */
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0);
    CHECK(ep_now(ep, out, 4) == 1);   /* armed again */
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == -1 && errno == ENOENT);
    CHECK(ep_now(ep, out, 4) == 0);
    ev.events = EPOLLIN;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(ep_now(ep, out, 4) == 1);
    close(p[0]);   /* the set forgets it */
    CHECK(ep_now(ep, out, 4) == 0);
    close(p[1]);
    int sp[2];   /* both ways on one channel: a read edge while it stays writable */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sp) == 0);
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, sp[0], &ev) == 0);
    CHECK(ep_now(ep, out, 4) == 1 && out[0].events == EPOLLOUT);
    CHECK(ep_now(ep, out, 4) == 0);
    CHECK(write(sp[1], "s", 1) == 1);
    CHECK(ep_now(ep, out, 4) == 1 && out[0].events == (EPOLLIN | EPOLLOUT));
    CHECK(ep_now(ep, out, 4) == 0);
    close(sp[0]);
    close(sp[1]);
    ev.events = EPOLLIN;
    /* The null device, opened here: descriptor 0 is it only in init's run (from the shell it is
     * the terminal, which epoll takes). */
    int nul = open("/dev/null", O_RDWR);
    CHECK(nul >= 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, nul, &ev) == -1 && errno == EPERM);
    close(nul);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev) == -1 && errno == EINVAL);
    ev.events = 1u << 20;
    CHECK(pipe2(p, 0) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == -1 && errno == EINVAL);
    struct pollfd pf = { .fd = ep, .events = POLLIN };
    CHECK(poll(&pf, 1, 0) == -1 && errno == EINVAL);
    close(p[0]);
    close(p[1]);
}

/* A waiter in another thread sees an item added after it went to sleep. */
static int wake_ep;

static void *epoll_waiter(void *arg __attribute__((unused)))
{
    struct epoll_event out[2];
    int n = epoll_wait(wake_ep, out, 2, 10000);
    return (void *)(uintptr_t)(n == 1 && out[0].data.u32 == 99 ? 0 : 1);
}

static void check_epoll_threads(int ep)
{
    int p[2];
    CHECK(pipe2(p, 0) == 0 && write(p[1], "r", 1) == 1);   /* ready before it is added */
    wake_ep = ep;
    pthread_t t;
    void *res = (void *)1;
    CHECK(pthread_create(&t, NULL, epoll_waiter, NULL) == 0);
    struct timespec nap = { 0, 30 * 1000 * 1000 };
    nanosleep(&nap, NULL);
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 99 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    CHECK(pthread_join(t, &res) == 0 && res == NULL);
    close(p[0]);
    close(p[1]);
}

static void check_epoll(void)
{
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    CHECK(epoll_create(0) == -1 && errno == EINVAL);
    CHECK(epoll_create1(1) == -1 && errno == EINVAL);
    check_epoll_256(ep);
    check_epoll_modes(ep);
    check_epoll_threads(ep);
    close(ep);
}

void pt_poll(char **argv __attribute__((unused)))
{
    check_200();
    check_nonblock();
    check_hangups();
    check_other_process();
    check_sleeps();
    check_thread_sets();
    check_select();
    check_epoll();
}
