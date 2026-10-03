/* linuxbench: a call to another process (or thread) and its answer, the
 * Linux side of Jam OS's channel_call lines: a 16-byte request, a 16-byte
 * reply, as utest's bench-call sends to bench-echo. Three ways, as the
 * plan lists them:
 *   futex  the message in shared memory, a futex word for whose turn it
 *          is (the least a call can cost on Linux: two wakes and two waits);
 *   socket a socketpair(AF_UNIX, SOCK_SEQPACKET): a message with
 *          boundaries, the closest to a channel message;
 *   pipe   two pipes, one each way.
 * The server is a forked process (or a thread, for thread->thread), pinned
 * to its CPU at SCHED_FIFO; the client is a thread pinned to P. On one
 * CPU the client's wake doesn't preempt (same priority), so each call is
 * two switches, as on Jam OS. */
#include "lb.h"   /* first: it defines _GNU_SOURCE before any system header */

#include <linux/futex.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MSG 16   /* bytes each way */

enum way { WAY_FUTEX, WAY_SOCKET, WAY_PIPE };

static const char *const way_how[] = {
    "futex: 16 B each way in shared memory",
    "socketpair SOCK_SEQPACKET: 16 B each way",
    "two pipes: 16 B each way",
};

/* Shared with the server (MAP_SHARED, made before the fork). */
struct shm {
    uint32_t turn __attribute__((aligned(64)));   /* 0 server waits, 1 request, 2 stop */
    uint8_t msg[MSG];                              /* the request, then the reply */
};

struct conn {
    enum way way;
    bool process;            /* the server is a process: futexes are shared */
    bool timeout;            /* every futex wait has a 5 s timeout */
    struct shm *shm;
    int cli, srv;            /* socket ends */
    int req[2], rep[2];      /* pipes: client->server, server->client */
};

static int futex_flags(const struct conn *c, int op)
{
    return c->process ? op : op | FUTEX_PRIVATE_FLAG;
}

static void futex_call(struct conn *c, const uint8_t *req, uint8_t *rep)
{
    static const struct timespec five = { 5, 0 };
    memcpy(c->shm->msg, req, MSG);
    __atomic_store_n(&c->shm->turn, 1, __ATOMIC_RELEASE);
    futex_op(&c->shm->turn, futex_flags(c, FUTEX_WAKE), 1, NULL);
    while (__atomic_load_n(&c->shm->turn, __ATOMIC_ACQUIRE) == 1)
        futex_op(&c->shm->turn, futex_flags(c, FUTEX_WAIT), 1, c->timeout ? &five : NULL);
    memcpy(rep, c->shm->msg, MSG);
}

static void call(struct conn *c)
{
    uint8_t req[MSG] = { 42 }, rep[MSG];
    switch (c->way) {
    case WAY_FUTEX:
        futex_call(c, req, rep);
        break;
    case WAY_SOCKET:
        if (write(c->cli, req, MSG) == MSG)
            (void)!read(c->cli, rep, MSG);
        break;
    case WAY_PIPE:
        if (write(c->req[1], req, MSG) == MSG)
            (void)!read(c->rep[0], rep, MSG);
        break;
    }
}

/* The echo server: until the client stops it (turn 2) or closes its end. */
static void serve(struct conn *c)
{
    uint8_t buf[MSG];
    for (;;) {
        if (c->way == WAY_FUTEX) {
            uint32_t t = __atomic_load_n(&c->shm->turn, __ATOMIC_ACQUIRE);
            if (t == 2)
                return;
            if (t == 0) {
                futex_op(&c->shm->turn, futex_flags(c, FUTEX_WAIT), 0, NULL);
                continue;
            }
            memcpy(buf, c->shm->msg, MSG);   /* read the request, write the reply */
            memcpy(c->shm->msg, buf, MSG);
            __atomic_store_n(&c->shm->turn, 0, __ATOMIC_RELEASE);
            futex_op(&c->shm->turn, futex_flags(c, FUTEX_WAKE), 1, NULL);
        } else {
            int in = c->way == WAY_SOCKET ? c->srv : c->req[0];
            int out = c->way == WAY_SOCKET ? c->srv : c->rep[1];
            ssize_t n = read(in, buf, MSG);
            if (n <= 0 || write(out, buf, (size_t)n) != n)
                return;
        }
    }
}

static void *client_thread(void *arg)
{
    struct conn *c = arg;
    uint64_t end;
    warm_until(&end);
    while (mono_ns() < end)
        call(c);
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        call(c);
        samples[i] = span_ps(t0, stamp(), 1);
    }
    return NULL;
}

static void *server_thread(void *arg)
{
    serve(arg);
    return NULL;
}

static bool open_conn(struct conn *c)
{
    c->shm = mmap(NULL, sizeof(*c->shm), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1,
                  0);
    if (c->shm == MAP_FAILED)
        return false;
    memset(c->shm, 0, sizeof(*c->shm));
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
        return false;
    c->cli = sv[0];
    c->srv = sv[1];
    return pipe(c->req) == 0 && pipe(c->rep) == 0;
}

/* The client's ends closed (the server sees the end), or turn 2. */
static void stop_conn(struct conn *c)
{
    __atomic_store_n(&c->shm->turn, 2, __ATOMIC_RELEASE);
    futex_op(&c->shm->turn, futex_flags(c, FUTEX_WAKE), 1, NULL);
    close(c->cli);
    close(c->req[1]);
}

static void close_conn(struct conn *c)
{
    close(c->srv);
    close(c->req[0]);
    close(c->rep[0]);
    close(c->rep[1]);
    munmap(c->shm, sizeof(*c->shm));
}

/* One line: the server on server_cpu (a process, or a thread), the
 * client on P. */
static void measure(enum way way, bool process, bool timeout, int server_cpu, const char *what)
{
    struct conn c = { .way = way, .process = process, .timeout = timeout };
    if (!open_conn(&c)) {
        skipped(what, "can't make the shared memory, socketpair or pipes");
        return;
    }
    pid_t pid = -1;
    pthread_t st = 0;
    if (process) {
        fflush(NULL);
        pid = fork();
        if (pid == 0) {
            /* Only the client may hold the client's ends: closing them
             * is how the server learns the run is over. */
            close(c.cli);
            close(c.req[1]);
            close(c.rep[0]);
            pin_self(server_cpu);
            serve(&c);
            _exit(0);
        }
    } else {
        st = start_on(server_cpu, server_thread, &c);
    }
    if (process && pid < 0) {
        skipped(what, "fork failed");
    } else {
        run_on(cpu_p, client_thread, &c);
        stop_conn(&c);
        if (process)
            waitpid(pid, NULL, 0);
        else
            pthread_join(st, NULL);
        char how[96];
        snprintf(how, sizeof(how), "%s%s", way_how[way],
                 timeout ? ", every wait with a 5 s timeout" : "");
        result(what, how, SAMPLES);
    }
    close_conn(&c);
}

void bench_ipc(void)
{
    for (int w = WAY_FUTEX; w <= WAY_PIPE; w++)
        measure((enum way)w, true, false, cpu_p,
                "user: process->process channel_call, same CPU (P)");
    measure(WAY_FUTEX, true, true, cpu_p, "user: the same with a 5 s deadline per call (P)");
    measure(WAY_SOCKET, false, false, cpu_p, "user: thread->thread channel_call, 1 process (P)");
    measure(WAY_FUTEX, false, false, cpu_p, "user: thread->thread channel_call, 1 process (P)");
    int others[] = { cpu_p2, cpu_ht, cpu_e };
    for (unsigned i = 0; i < 3; i++) {
        if (others[i] < 0)
            continue;
        char what[64];
        snprintf(what, sizeof(what), "user: process->process channel_call P->%s",
                 kind(others[i]));
        for (int w = WAY_FUTEX; w <= WAY_PIPE; w++)
            measure((enum way)w, true, false, others[i], what);
    }
}
