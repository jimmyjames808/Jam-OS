/* utest: the unread bound of the generated servers (tools/genidl.py, "The
 * unread bound"), on the test protocol `null` (abi/idl/null.idl).
 *
 * A server pays for every answer it sends until its client reads it, so a
 * client that sends requests and reads nothing would fill the server's
 * job, and every other client's answers would be lost. A child process,
 * in a job of its own whose queued messages are limited to UNREAD_JOB
 * bytes, serves two clients: once with the generated loop
 * (<proto>_serve, a thread a channel), once with a port loop of its own on
 * one thread (<proto>_serve_one after <proto>_unread_full, a struct
 * idl_held). One client writes FLOOD pings and reads nothing: the server
 * leaves it at most the bound unread, answers the other client meanwhile,
 * stays within its job, and answers all FLOOD in order once they are
 * read. Without the bound the job fills after about a hundred answers and
 * the rest are dropped. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/null.h>
#include <os.h>
#include "utest.h"

#define FLOOD      300u                /* pings the slow client sends before it reads */
#define UNREAD_JOB (8u << 10)          /* the server's job: queued message bytes */
#define CALL_WAIT  (2 * NS_PER_S)

/* ---- the child: the server --------------------------------------------------------- */

static status_t s_ping(void *ctx, uint64_t value, uint64_t *out_value)
{
    (void)ctx;
    *out_value = value;
    return OK;
}

static const struct null_ops ops = { .ping = s_ping };

/* The two channels to serve: the one message on the startup channel. */
static bool take_two(handle_t hs[2])
{
    handle_t boot = startup_handle(SR_USER);
    uint8_t b[4];
    uint32_t n = 0, nh = 0;
    signals_t seen;
    if (jam_object_wait_one(boot, SIG_READABLE, now() + 10 * NS_PER_S, &seen) != OK)
        return false;
    struct channel_read_args a = {
        .h = boot, .rd_cap = sizeof(b), .rd_bytes = (uint64_t)(uintptr_t)b,
        .rd_actual = (uint64_t)(uintptr_t)&n, .rd_handles = (uint64_t)(uintptr_t)hs,
        .rd_hcap = 2, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    return jam_channel_read(&a) == OK && nh == 2;
}

static void serve_main(void *arg)
{
    (void)null_serve(*(handle_t *)arg, &ops, NULL);
}

/* The generated loop: a thread for each channel. */
static int serve_two(handle_t hs[2])
{
    static uint8_t stack[32768] __attribute__((aligned(16)));
    handle_t th;
    if (thread_spawn("unread-serve", serve_main, &hs[1], stack, sizeof(stack), &th) != OK)
        return 2;
    status_t st = null_serve(hs[0], &ops, NULL);
    if (!wait_threads(&th, 1))
        return 3;
    return st == OK ? 0 : 4;
}

static status_t arm(handle_t port, handle_t ch, uint64_t key)
{
    return jam_port_bind(port, ch, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
}

/* A loop of the server's own, as a service writes it: one thread, one port,
 * each channel armed once and served a request a packet; a channel whose
 * client is behind is left unarmed and held, and looked at again when
 * idl_held_due says so. */
static int loop_two(handle_t hs[2])
{
    handle_t port;
    bool open[2] = { true, true }, held[2] = { false, false };
    struct idl_held h = { 0 };
    if (jam_port_create(&port) != OK || arm(port, hs[0], 0) != OK || arm(port, hs[1], 1) != OK)
        return 2;
    while (open[0] || open[1]) {
        if (idl_held_due(&h)) {
            for (unsigned i = 0; i < 2; i++) {
                if (!held[i])
                    continue;
                if (null_unread_full(hs[i])) {
                    idl_hold(&h);   /* still behind */
                    continue;
                }
                held[i] = false;
                if (arm(port, hs[i], i) != OK)
                    return 3;
            }
        }
        struct port_packet pkt;
        status_t st = jam_port_wait(port, idl_held_deadline(&h, DEADLINE_NEVER), &pkt);
        if (st == ERR_TIMED_OUT)
            continue;
        if (st != OK || pkt.key > 1)
            return 4;
        unsigned i = (unsigned)pkt.key;
        if (null_unread_full(hs[i])) {
            held[i] = true;   /* its requests wait in the channel */
            idl_hold(&h);
            continue;
        }
        st = null_serve_one(hs[i], &ops, NULL);
        if (st == OK || st == ERR_SHOULD_WAIT)
            st = arm(port, hs[i], i);   /* fires at once if more is queued */
        if (st != OK)
            open[i] = false;   /* ERR_PEER_CLOSED: the client is gone */
    }
    return 0;
}

int unread_child(int argc, char **argv)
{
    (void)argc;
    handle_t hs[2];
    if (!take_two(hs))
        return 1;
    return !strcmp(argv[1], "unread-serve") ? serve_two(hs) : loop_two(hs);
}

/* ---- the test: the clients ---------------------------------------------------------- */

static bool flood_one(const char *mode)
{
    handle_t slow, slow_sv, quick, quick_sv, boot, boot_sv, job, proc;
    CHECK_ST(jam_channel_create(&slow, &slow_sv), OK);
    CHECK_ST(jam_channel_create(&quick, &quick_sv), OK);
    CHECK_ST(jam_channel_create(&boot, &boot_sv), OK);
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_job_set_limit(job, JOB_LIMIT_MSG_BYTES, UNREAD_JOB), OK);
    CHECK_ST(child(mode, NULL, job, boot_sv, &proc), OK);
    handle_t two[2] = { slow_sv, quick_sv };
    uint8_t b[4] = { 0 };
    CHECK_ST(jam_channel_write(boot, b, sizeof(b), two, 2), OK);

    for (uint32_t i = 0; i < FLOOD; i++)
        CHECK_ST(null_ping_send(slow, 0x100 + i, 1000 + i), OK);
    jam_sleep_until(now() + 300 * NS_PER_MS);   /* the server does what it will of them */
    struct handle_info hi;
    CHECK_ST(jam_handle_info(slow, &hi), OK);
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    printf("utest: idl_unread_bound: %s: %u of %u answers sent before any was read, %u "
           "requests waiting, %lu bytes of the server's job queued\n", mode + 7, hi.queued,
           FLOOD, hi.peer_queued, (unsigned long)ji.used[JOB_LIMIT_MSG_BYTES]);
    CHECK(hi.queued <= NULL_UNREAD_MSGS);
    CHECK(hi.queued + hi.peer_queued == FLOOD);   /* nothing dropped: answered or waiting */

    /* The other client is answered meanwhile, again and again. */
    for (uint64_t k = 1; k <= 50; k++) {
        uint64_t v = 0;
        CHECK_ST(null_ping_within(quick, CALL_WAIT, k, &v), OK);
        CHECK_EQ(v, k);
    }

    /* Reading the answers lets the rest through, each one right. */
    _Alignas(8) uint8_t rep[NULL_REP_MAX];
    uint32_t got = 0;
    uint64_t until = now() + 10 * NS_PER_S;
    while (got < FLOOD) {
        struct idl_msg m;
        status_t st = idl_reply_read(slow, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen;
            CHECK(now() < until);
            (void)jam_object_wait_one(slow, SIG_READABLE, until, &seen);
            continue;
        }
        CHECK_ST(st, OK);
        CHECK_EQ(m.txid, 0x100 + got);   /* in order */
        uint64_t v = 0;
        CHECK_ST(null_ping_result(rep, &m, &v), OK);
        CHECK_EQ(v, 1000 + got);
        got++;
    }
    CHECK_ST(jam_handle_close(slow), OK);
    CHECK_ST(jam_handle_close(quick), OK);
    CHECK_ST(jam_handle_close(boot), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &info), OK);
    CHECK_EQ(info.exit_code, 0);
    CHECK(!info.killed);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

bool t_idl_unread_bound(void)
{
    return flood_one("unread-serve") && flood_one("unread-loop");
}
