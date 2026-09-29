/* utest: the M5 test suite, run in ring 3 as a process under init.
 *
 * It checks the milestone from user space: system calls and handle
 * rights, bad user pointers (ERR_INVALID_ARGS, not a kill), a child that
 * crashes is killed and nothing else is, W^X, channel ping-pong between two
 * processes, a child killed while blocked in channel_call gives back every
 * page, handle and thread, a runaway child hits its job's limits and gets
 * ERR_NO_MEMORY / ERR_NO_RESOURCES (no panic), FPU/SSE/AVX state survives
 * preemption, and many threads come and go.
 *
 * Children are this same program started with a mode ("utest nullderef",
 * see child.c), each in a job of its own so its usage can be read exactly.
 * One line per test ("utest: <name> ok"); the summary also goes into the
 * kernel's RESULTS box. Exit code 0 when everything passed. */
#include <os.h>
#include "utest.h"

#define MS 1000000ull
#define S  1000000000ull

static const char *cur;

#define FAIL(...)                                                   \
    do {                                                            \
        printf("utest: %s: FAILED at line %d: ", cur, __LINE__);    \
        printf(__VA_ARGS__);                                        \
        printf("\n");                                               \
        return false;                                               \
    } while (0)
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c))                                                   \
            FAIL("%s", #c);                                         \
    } while (0)
#define CHECK_ST(expr, want)                                        \
    do {                                                            \
        status_t _s = (expr), _w = (want);                          \
        if (_s != _w)                                               \
            FAIL("%s is %s, want %s", #expr, status_str(_s), status_str(_w)); \
    } while (0)
#define CHECK_EQ(a, b)                                              \
    do {                                                            \
        int64_t _a = (int64_t)(a), _b = (int64_t)(b);               \
        if (_a != _b)                                               \
            FAIL("%s == %s: %ld vs %ld", #a, #b, (long)_a, (long)_b); \
    } while (0)

static uint64_t now(void)
{
    return (uint64_t)jam_clock_get();
}

/* ---- helpers ------------------------------------------------------------------ */

static handle_t own_job(void)
{
    return startup_handle(SR_JOB);
}

static status_t info_of(handle_t job, struct job_info *ji)
{
    return jam_job_get_info(job, ji);
}

static status_t new_job(handle_t *out)
{
    return jam_job_create(own_job(), 0, out);
}

/* Start "utest <mode> [arg]" in job, with up to one extra handle. */
static status_t child(const char *mode, const char *arg, handle_t job, handle_t extra,
                      handle_t *proc)
{
    char name[32];
    snprintf(name, sizeof(name), "utest-%s", mode);
    const char *argv[] = { "utest", mode, arg };
    struct spawn_handle x = { SR_USER, extra };
    struct spawn_args a = {
        .path = "bin/utest", .name = name, .argc = arg ? 3 : 2, .argv = argv, .job = job,
        .extra = extra ? &x : NULL, .nextra = extra ? 1 : 0,
    };
    return spawn(&a, proc);
}

/* Run a child to the end in a fresh job; *info says how it ended, and the
 * job must be empty afterwards. limit_kind/limit (0 = none) go on the job. */
static bool run_child(const char *mode, uint32_t limit_kind, uint64_t limit,
                      struct process_info *info)
{
    handle_t job, proc;
    CHECK_ST(new_job(&job), OK);
    if (limit_kind)
        CHECK_ST(jam_job_set_limit(job, limit_kind, limit), OK);
    CHECK_ST(child(mode, NULL, job, HANDLE_INVALID, &proc), OK);
    CHECK_ST(spawn_wait(proc, 20 * S, info), OK);
    CHECK_EQ(info->state, PROCESS_DEAD);
    CHECK_ST(jam_handle_close(proc), OK);
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k])
            FAIL("child \"%s\" left %lu units of job kind %u", mode, (unsigned long)ji.used[k],
                 k);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* ---- tests -------------------------------------------------------------------- */

static bool t_basics(void)
{
    uint64_t t0 = now();
    CHECK(t0 > 0);
    CHECK_ST(jam_nanosleep(t0 + 2 * MS), OK);
    CHECK(now() >= t0 + 2 * MS);

    handle_t e, tm, port;
    signals_t seen = 0;
    CHECK_ST(jam_event_create(&e), OK);
    CHECK_ST(jam_object_wait_one(e, SIG_SIGNALED, now(), &seen), ERR_TIMED_OUT);
    CHECK_ST(jam_event_signal(e, 0, SIG_SIGNALED), OK);
    CHECK_ST(jam_object_wait_one(e, SIG_SIGNALED, DEADLINE_NEVER, &seen), OK);
    CHECK(seen & SIG_SIGNALED);
    CHECK_ST(jam_object_signal(e, 0, 1u << 24), OK);
    CHECK_ST(jam_object_signal(e, 0, SIG_SIGNALED), ERR_INVALID_ARGS);   /* kernel bits */

    CHECK_ST(jam_timer_create(&tm), OK);
    CHECK_ST(jam_timer_set(tm, now() + 5 * MS), OK);
    CHECK_ST(jam_object_wait_one(tm, SIG_SIGNALED, now() + S, &seen), OK);

    CHECK_ST(jam_port_create(&port), OK);
    struct port_packet p = { .key = 7, .type = PORT_PACKET_USER }, out;
    p.user.data[0] = 42;
    CHECK_ST(jam_port_queue(port, &p), OK);
    CHECK_ST(jam_port_wait(port, now(), &out), OK);
    CHECK(out.key == 7 && out.type == PORT_PACKET_USER && out.user.data[0] == 42);
    CHECK_ST(jam_port_bind(port, e, 9, SIG_SIGNALED, PORT_BIND_ONCE), OK);
    CHECK_ST(jam_port_wait(port, now() + S, &out), OK);
    CHECK(out.key == 9 && out.type == PORT_PACKET_SIGNAL);

    CHECK_ST(jam_handle_close(port), OK);
    CHECK_ST(jam_handle_close(tm), OK);
    CHECK_ST(jam_handle_close(e), OK);
    CHECK_ST(jam_handle_close(e), ERR_BAD_HANDLE);
    return true;
}

static bool t_rights(void)
{
    handle_t v, ro, ro2, x, vmar = startup_handle(SR_SELF_VMAR);
    uint64_t addr = 0;
    char buf[4];
    CHECK_ST(jam_vmo_create(8192, 0, HANDLE_INVALID, &v), OK);
    CHECK_ST(jam_vmo_write(v, 0, "hi", 2), OK);
    CHECK_ST(jam_handle_duplicate(v, RIGHT_READ | RIGHT_MAP | RIGHT_DUPLICATE, &ro), OK);
    CHECK_ST(jam_vmo_write(ro, 0, "no", 2), ERR_ACCESS_DENIED);   /* a read-only handle */
    CHECK_ST(jam_vmo_read(ro, 0, buf, 2), OK);
    CHECK(buf[0] == 'h' && buf[1] == 'i');
    CHECK_ST(jam_handle_duplicate(ro, RIGHT_READ | RIGHT_WRITE, &x), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmar_map(vmar, ro, 0, 4096, VMAR_READ | VMAR_WRITE, &addr), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_replace(ro, RIGHT_READ, &ro2), OK);
    CHECK_ST(jam_vmo_read(ro, 0, buf, 1), ERR_BAD_HANDLE);         /* replaced: stale */
    CHECK_ST(jam_handle_duplicate(ro2, RIGHT_SAME, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_event_signal(v, 0, SIG_SIGNALED), ERR_WRONG_TYPE);

    /* bootfs: readable and mappable, never writable */
    handle_t fs = startup_handle(SR_BOOTFS);
    CHECK(fs != HANDLE_INVALID);
    CHECK_ST(jam_vmo_write(fs, 0, "x", 1), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_set_size(fs, 0), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_map(vmar, fs, 0, 4096, VMAR_READ | VMAR_WRITE, &addr), ERR_ACCESS_DENIED);

    /* priorities: up to 24 for user threads */
    handle_t self = startup_handle(SR_SELF_THREAD);
    CHECK_ST(jam_thread_set_priority(self, 25), ERR_ACCESS_DENIED);
    CHECK_ST(jam_thread_set_priority(self, 32), ERR_INVALID_ARGS);
    CHECK_ST(jam_thread_set_priority(self, THREAD_PRIO_USER_MAX), OK);
    CHECK_ST(jam_thread_set_priority(self, THREAD_PRIO_DEFAULT), OK);

    /* a job handle without RIGHT_WRITE can't make processes or jobs */
    handle_t jro, j2, p, pv;
    CHECK_ST(jam_handle_duplicate(own_job(), RIGHT_INSPECT | RIGHT_DUPLICATE, &jro), OK);
    CHECK_ST(jam_job_create(jro, 0, &j2), ERR_ACCESS_DENIED);
    CHECK_ST(jam_process_create(jro, "x", 1, 0, &p, &pv), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(jro), OK);
    /* our own job comes without RIGHT_MANAGE: its limits are our parent's */
    CHECK_ST(jam_job_set_limit(own_job(), JOB_LIMIT_PAGES, JOB_NO_LIMIT), ERR_ACCESS_DENIED);
    handle_t jm;
    CHECK_ST(jam_handle_duplicate(own_job(), JOB_RIGHTS, &jm), ERR_INVALID_ARGS);
    /* a job we make is ours to manage */
    CHECK_ST(new_job(&j2), OK);
    CHECK_ST(jam_job_set_limit(j2, JOB_LIMIT_PAGES, 16), OK);
    CHECK_ST(jam_handle_duplicate(j2, JOB_RIGHTS_OWN, &jm), OK);
    CHECK_ST(jam_job_set_limit(jm, JOB_LIMIT_PAGES, 32), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(jm), OK);
    CHECK_ST(jam_handle_close(j2), OK);

    CHECK_ST(jam_handle_close(ro2), OK);
    CHECK_ST(jam_handle_close(v), OK);
    return true;
}

static bool t_bad_pointers(void)
{
    struct job_info before, after;
    CHECK_ST(info_of(own_job(), &before), OK);
    const uint64_t unmapped = 16, kernel = 0xffff800000001000ull,
                   noncanon = 0x0000800000000000ull, top = 0x00007fffffffe000ull - 2;
    CHECK_ST(jam_debug_write((const char *)unmapped, 4), ERR_INVALID_ARGS);
    CHECK_ST(jam_debug_write((const char *)kernel, 4), ERR_INVALID_ARGS);
    CHECK_ST(jam_debug_write((const char *)noncanon, 4), ERR_INVALID_ARGS);
    CHECK_ST(jam_debug_write((const char *)top, 8), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_create((handle_t *)unmapped, (handle_t *)unmapped), ERR_INVALID_ARGS);
    handle_t *kh = (handle_t *)kernel;
    CHECK_ST(jam_event_create(kh), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_read((const struct channel_read_args *)kernel), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_call((const struct channel_call_args *)unmapped), ERR_INVALID_ARGS);

    handle_t v;
    CHECK_ST(jam_vmo_create(4096, 0, HANDLE_INVALID, &v), OK);
    CHECK_ST(jam_vmo_read(v, 0, (void *)unmapped, 16), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmo_write(v, 0, (const void *)kernel, 16), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmo_get_size(v, NULL), ERR_INVALID_ARGS);
    CHECK_ST(jam_job_get_info(own_job(), (struct job_info *)unmapped), ERR_INVALID_ARGS);
    CHECK_ST(jam_handle_close(v), OK);

    /* A message whose buffer turns out bad is consumed, but the handle it
     * carried must not leak into (or stay in) our table. */
    handle_t a, b, e;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(jam_event_create(&e), OK);
    CHECK_ST(jam_channel_write(a, "abcdefgh", 8, &e, 1), OK);
    handle_t got[4];
    uint32_t nb, nh;
    struct channel_read_args ra = {
        .h = b, .bytes_cap = 64, .bytes = unmapped, .actual_bytes = (uint64_t)(uintptr_t)&nb,
        .handles = (uint64_t)(uintptr_t)got, .handles_cap = 4,
        .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    CHECK_ST(jam_channel_read(&ra), ERR_INVALID_ARGS);
    ra.bytes = (uint64_t)(uintptr_t)got;
    CHECK_ST(jam_channel_read(&ra), ERR_SHOULD_WAIT);   /* it was consumed */
    /* A request with a bad buffer: nothing sent, our handle still ours. */
    CHECK_ST(jam_event_create(&e), OK);
    struct channel_call_args ca = {
        .h = a, .wn = 8, .wbytes = unmapped, .wh = (uint64_t)(uintptr_t)&e, .whn = 1,
        .deadline_ns = now(),
    };
    CHECK_ST(jam_channel_call(&ca), ERR_INVALID_ARGS);
    CHECK_ST(jam_event_signal(e, 0, SIG_SIGNALED), OK);
    CHECK_ST(jam_handle_close(e), OK);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);

    CHECK_ST(info_of(own_job(), &after), OK);
    CHECK_EQ(after.used[JOB_LIMIT_HANDLES], before.used[JOB_LIMIT_HANDLES]);
    CHECK_EQ(after.used[JOB_LIMIT_MSG_BYTES], before.used[JOB_LIMIT_MSG_BYTES]);
    return true;
}

static bool t_crash_kills_only_the_child(void)
{
    struct process_info info;
    if (!run_child("nullderef", 0, 0, &info))
        return false;
    CHECK(info.killed);
    CHECK_EQ(info.exit_code, PROCESS_KILLED_CODE);
    /* ...and we are still here, and so is a normal child. */
    if (!run_child("exit7", 0, 0, &info))
        return false;
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 7);
    return true;
}

static bool t_wx(void)
{
    handle_t v, vmar = startup_handle(SR_SELF_VMAR);
    uint64_t addr = 0;
    CHECK_ST(jam_vmo_create(4096, 0, HANDLE_INVALID, &v), OK);
    CHECK_ST(jam_vmar_map(vmar, v, 0, 4096, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &addr),
             ERR_INVALID_ARGS);
    CHECK_ST(jam_vmar_map(vmar, v, 0, 4096, VMAR_READ | VMAR_EXEC, &addr), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_map(vmar, v, 0, 4096, VMAR_READ | VMAR_WRITE, &addr), OK);
    CHECK_ST(jam_vmar_protect(vmar, addr, 4096, VMAR_READ | VMAR_EXEC), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_unmap(vmar, addr, 4096), OK);
    CHECK_ST(jam_handle_close(v), OK);
    /* bootfs may be mapped executable (that's how programs load)... */
    handle_t fs = startup_handle(SR_BOOTFS);
    CHECK_ST(jam_vmar_map(vmar, fs, 0, 4096, VMAR_READ | VMAR_EXEC, &addr), OK);
    CHECK_ST(jam_vmar_unmap(vmar, addr, 4096), OK);
    /* ...but data never runs: jumping into the heap kills the child. */
    struct process_info info;
    if (!run_child("execdata", 0, 0, &info))
        return false;
    CHECK(info.killed);
    return true;
}

static bool t_ping_pong(void)
{
    handle_t a, b, job, proc;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("echo", NULL, job, b, &proc), OK);
    uint64_t t0 = now();
    enum { CALLS = 2000 };
    for (uint32_t i = 0; i < CALLS; i++) {
        uint32_t req[4] = { 0, i, ~i, 12345 }, rep[4] = { 0 }, ra = 0;
        struct channel_call_args c = {
            .h = a, .wn = sizeof(req), .wbytes = (uint64_t)(uintptr_t)req,
            .rcap = sizeof(rep), .rbytes = (uint64_t)(uintptr_t)rep,
            .ractual = (uint64_t)(uintptr_t)&ra, .deadline_ns = now() + 5 * S,
        };
        CHECK_ST(jam_channel_call(&c), OK);
        if (ra != sizeof(rep) || rep[1] != i || rep[2] != ~i || rep[3] != 12345)
            FAIL("call %u: bad reply (%u bytes, %u %u %u)", i, ra, rep[1], rep[2], rep[3]);
        CHECK(req[0] == 0);   /* the txid was stamped in the kernel's copy, not ours */
    }
    uint64_t us = (now() - t0) / 1000;
    /* A handle there and back. */
    handle_t e, back = 0;
    uint32_t nh = 0;
    CHECK_ST(jam_event_create(&e), OK);
    uint32_t req = 0, rep = 0;
    struct channel_call_args c = {
        .h = a, .wn = 4, .wbytes = (uint64_t)(uintptr_t)&req, .wh = (uint64_t)(uintptr_t)&e,
        .whn = 1, .rcap = 4, .rbytes = (uint64_t)(uintptr_t)&rep,
        .rh = (uint64_t)(uintptr_t)&back, .rhcap = 1, .rhactual = (uint64_t)(uintptr_t)&nh,
        .deadline_ns = now() + 5 * S,
    };
    CHECK_ST(jam_channel_call(&c), OK);
    CHECK_EQ(nh, 1);
    CHECK_ST(jam_event_signal(e, 0, SIG_SIGNALED), ERR_BAD_HANDLE);   /* it moved */
    CHECK_ST(jam_event_signal(back, 0, SIG_SIGNALED), OK);
    CHECK_ST(jam_handle_close(back), OK);

    CHECK_ST(jam_handle_close(a), OK);   /* the echo server sees PEER_CLOSED and exits 0 */
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    printf("utest: ping_pong: %u calls between two processes in %lu us\n", CALLS,
           (unsigned long)us);
    return true;
}

static bool t_kill_in_channel_call(void)
{
    struct job_info before, ji;
    CHECK_ST(info_of(own_job(), &before), OK);
    handle_t a, b, job, proc;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("caller", NULL, job, b, &proc), OK);
    signals_t seen;
    /* The request arriving means the child is (about to be) blocked. */
    CHECK_ST(jam_object_wait_one(a, SIG_READABLE, now() + 10 * S, &seen), OK);
    jam_nanosleep(now() + 30 * MS);
    CHECK_ST(info_of(job, &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_THREADS], 2);   /* main (in the call) + the sleeper */
    CHECK(ji.used[JOB_LIMIT_PAGES] >= 16);      /* its heap */
    CHECK(ji.used[JOB_LIMIT_HANDLES] >= 5);
    CHECK(ji.used[JOB_LIMIT_MSG_BYTES] > 0);    /* its request, queued on our end */

    CHECK_ST(jam_process_kill(proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * S, &info), OK);
    CHECK(info.killed);
    CHECK_EQ(info.threads, 0);
    CHECK_ST(info_of(job, &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_THREADS], 0);
    CHECK_EQ(ji.used[JOB_LIMIT_PAGES], 0);
    CHECK_EQ(ji.used[JOB_LIMIT_HANDLES], 0);
    /* The request still sits on our end, charged to the dead child's job
     * until we read it. */
    CHECK(ji.used[JOB_LIMIT_MSG_BYTES] > 0);
    uint8_t buf[64];
    uint32_t nb = 0;
    struct channel_read_args r = {
        .h = a, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
        .actual_bytes = (uint64_t)(uintptr_t)&nb,
    };
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK_EQ(nb, 16);
    CHECK_ST(info_of(job, &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_MSG_BYTES], 0);
    CHECK_ST(jam_channel_read(&r), ERR_PEER_CLOSED);   /* its end died with it */

    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    /* And our own job is back where it was: the child's stack and data
     * (charged to us, since we made them) are gone too. */
    CHECK_ST(info_of(own_job(), &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k] != before.used[k])
            FAIL("our job kind %u: %lu before, %lu after", k, (unsigned long)before.used[k],
                 (unsigned long)ji.used[k]);
    return true;
}

static bool t_runaway_hits_job_limits(void)
{
    struct process_info info;
    if (!run_child("hog", JOB_LIMIT_PAGES, 256, &info))   /* 1 MiB */
        return false;
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 42);   /* it saw ERR_NO_MEMORY and exited */
    if (!run_child("hogfault", JOB_LIMIT_PAGES, 256, &info))
        return false;
    CHECK(info.killed);             /* a fault can't be refused: killed, not a panic */
    if (!run_child("threads2", JOB_LIMIT_THREADS, 1, &info))
        return false;
    CHECK_EQ(info.exit_code, 44);   /* ERR_NO_RESOURCES */
    if (!run_child("handles", JOB_LIMIT_HANDLES, 16, &info))
        return false;
    CHECK_EQ(info.exit_code, 45);
    if (!run_child("msgs", JOB_LIMIT_MSG_BYTES, 4096, &info))
        return false;
    CHECK_EQ(info.exit_code, 46);
    if (!run_child("ports", JOB_LIMIT_MSG_BYTES, 4096, &info))
        return false;
    CHECK_EQ(info.exit_code, 47);   /* port packets and bindings are charged too */
    return true;
}

static bool wait_threads(handle_t *th, unsigned n);

static void waiter(void *arg)
{
    signals_t seen;
    jam_object_wait_one((handle_t)(uintptr_t)arg, SIG_SIGNALED, DEADLINE_NEVER, &seen);
}

/* Kernel memory a program makes the kernel hold is charged to its job
 * (the review's R6): a VMO's struct and a process are handle units, a
 * thread's kernel stack is pages, port packets and bindings and each
 * handle a message carries are message bytes, and all of it comes back. */
static bool t_kernel_objects_are_charged(void)
{
    struct job_info a, b;
    CHECK_ST(info_of(own_job(), &a), OK);
#define USED(k) (info_of(own_job(), &b) == OK ? b.used[k] - a.used[k] : 999999)

    handle_t v;
    CHECK_ST(jam_vmo_create(4096, 0, HANDLE_INVALID, &v), OK);
    CHECK_EQ(USED(JOB_LIMIT_HANDLES), 2);   /* its slot and the VMO itself */
    CHECK_ST(jam_handle_close(v), OK);
    CHECK_EQ(USED(JOB_LIMIT_HANDLES), 0);

    handle_t port, ev, x, y;
    CHECK_ST(jam_port_create(&port), OK);
    CHECK_ST(jam_event_create(&ev), OK);
    struct port_packet pk = { .key = 3, .type = PORT_PACKET_USER }, out;
    CHECK_ST(jam_port_queue(port, &pk), OK);
    CHECK(USED(JOB_LIMIT_MSG_BYTES) > 0);
    CHECK_ST(jam_port_wait(port, 0, &out), OK);
    CHECK_EQ(USED(JOB_LIMIT_MSG_BYTES), 0);
    CHECK_ST(jam_port_bind(port, ev, 4, SIG_SIGNALED, PORT_BIND_PERSISTENT), OK);
    CHECK(USED(JOB_LIMIT_MSG_BYTES) > 1024);   /* the binding keeps ev alive */
    CHECK_ST(jam_port_unbind(port, ev, 4), OK);
    CHECK_EQ(USED(JOB_LIMIT_MSG_BYTES), 0);

    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(jam_channel_write(x, "hi", 2, &ev, 1), OK);   /* ev moves into the message */
    CHECK(USED(JOB_LIMIT_MSG_BYTES) > 1024);
    uint8_t buf[8];
    uint32_t nb = 0, nh = 0;
    struct channel_read_args r = {
        .h = y, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
        .actual_bytes = (uint64_t)(uintptr_t)&nb, .handles = (uint64_t)(uintptr_t)&ev,
        .handles_cap = 1, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK_EQ(nh, 1);
    CHECK_EQ(USED(JOB_LIMIT_MSG_BYTES), 0);

    /* a thread: its kernel stack (64 KiB) and more, while it runs */
    static uint8_t stack[8192] __attribute__((aligned(16)));
    handle_t th;
    CHECK_ST(thread_spawn("waiter", waiter, (void *)(uintptr_t)ev, stack, sizeof(stack), &th),
             OK);
    CHECK(USED(JOB_LIMIT_PAGES) >= 16);
    CHECK_ST(jam_event_signal(ev, 0, SIG_SIGNALED), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_EQ(USED(JOB_LIMIT_PAGES), 0);

    /* a process (never started): a handle unit and its PML4, in its job */
    handle_t job, p, pv;
    struct job_info ji;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_process_create(job, "idle", 4, 0, &p, &pv), OK);
    CHECK_ST(info_of(job, &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_HANDLES], 1);
    CHECK_EQ(ji.used[JOB_LIMIT_PAGES], 1);
    CHECK_ST(jam_handle_close(pv), OK);
    CHECK_ST(jam_handle_close(p), OK);
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(ji.used[k], 0);
    CHECK_ST(jam_handle_close(job), OK);

    CHECK_ST(jam_handle_close(y), OK);
    CHECK_ST(jam_handle_close(x), OK);
    CHECK_ST(jam_handle_close(ev), OK);
    CHECK_ST(jam_handle_close(port), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(USED(k), 0);
#undef USED
    return true;
}

/* ---- FPU state across preemption ---------------------------------------------- */

/* More threads than the PC has CPUs (28), so they must take turns: a
 * round that takes far longer than the fastest one was preempted (or
 * interrupted) with the registers loaded, which is the case under test. */
#define FPU_THREADS 48
#define FPU_RUN_NS  (400 * MS)
#define FPU_SPINS   4000000

static bool have_avx;
static volatile uint32_t fpu_errors, fpu_rounds, fpu_preempted;

static inline uint64_t tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return (uint64_t)hi << 32 | lo;
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static bool avx_usable(void)
{
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(c & (1u << 27)) || !(c & (1u << 28)))   /* OSXSAVE, AVX */
        return false;
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;   /* the kernel saves SSE and AVX state */
}

/* Load 16 registers from pat, spin (preemption happens here), store them
 * to got: all in one asm block, so the compiler can't touch them between. */
static void sse_round(const uint8_t *pat, uint8_t *got, uint64_t spins)
{
    __asm__ volatile(
        "movdqu 0(%0), %%xmm0\n movdqu 16(%0), %%xmm1\n movdqu 32(%0), %%xmm2\n"
        "movdqu 48(%0), %%xmm3\n movdqu 64(%0), %%xmm4\n movdqu 80(%0), %%xmm5\n"
        "movdqu 96(%0), %%xmm6\n movdqu 112(%0), %%xmm7\n movdqu 128(%0), %%xmm8\n"
        "movdqu 144(%0), %%xmm9\n movdqu 160(%0), %%xmm10\n movdqu 176(%0), %%xmm11\n"
        "movdqu 192(%0), %%xmm12\n movdqu 208(%0), %%xmm13\n movdqu 224(%0), %%xmm14\n"
        "movdqu 240(%0), %%xmm15\n"
        "1: dec %2\n jnz 1b\n"
        "movdqu %%xmm0, 0(%1)\n movdqu %%xmm1, 16(%1)\n movdqu %%xmm2, 32(%1)\n"
        "movdqu %%xmm3, 48(%1)\n movdqu %%xmm4, 64(%1)\n movdqu %%xmm5, 80(%1)\n"
        "movdqu %%xmm6, 96(%1)\n movdqu %%xmm7, 112(%1)\n movdqu %%xmm8, 128(%1)\n"
        "movdqu %%xmm9, 144(%1)\n movdqu %%xmm10, 160(%1)\n movdqu %%xmm11, 176(%1)\n"
        "movdqu %%xmm12, 192(%1)\n movdqu %%xmm13, 208(%1)\n movdqu %%xmm14, 224(%1)\n"
        "movdqu %%xmm15, 240(%1)\n"
        : "+r"(pat), "+r"(got), "+r"(spins)
        :
        : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

/* The same with the full 256-bit ymm registers (upper halves are AVX
 * state, saved only by XSAVE). */
__attribute__((target("avx"))) static void avx_round(const uint8_t *pat, uint8_t *got,
                                                     uint64_t spins)
{
    __asm__ volatile(
        "vmovdqu 0(%0), %%ymm0\n vmovdqu 32(%0), %%ymm1\n vmovdqu 64(%0), %%ymm2\n"
        "vmovdqu 96(%0), %%ymm3\n vmovdqu 128(%0), %%ymm4\n vmovdqu 160(%0), %%ymm5\n"
        "vmovdqu 192(%0), %%ymm6\n vmovdqu 224(%0), %%ymm7\n vmovdqu 256(%0), %%ymm8\n"
        "vmovdqu 288(%0), %%ymm9\n vmovdqu 320(%0), %%ymm10\n vmovdqu 352(%0), %%ymm11\n"
        "vmovdqu 384(%0), %%ymm12\n vmovdqu 416(%0), %%ymm13\n vmovdqu 448(%0), %%ymm14\n"
        "vmovdqu 480(%0), %%ymm15\n"
        "1: dec %2\n jnz 1b\n"
        "vmovdqu %%ymm0, 0(%1)\n vmovdqu %%ymm1, 32(%1)\n vmovdqu %%ymm2, 64(%1)\n"
        "vmovdqu %%ymm3, 96(%1)\n vmovdqu %%ymm4, 128(%1)\n vmovdqu %%ymm5, 160(%1)\n"
        "vmovdqu %%ymm6, 192(%1)\n vmovdqu %%ymm7, 224(%1)\n vmovdqu %%ymm8, 256(%1)\n"
        "vmovdqu %%ymm9, 288(%1)\n vmovdqu %%ymm10, 320(%1)\n vmovdqu %%ymm11, 352(%1)\n"
        "vmovdqu %%ymm12, 384(%1)\n vmovdqu %%ymm13, 416(%1)\n vmovdqu %%ymm14, 448(%1)\n"
        "vmovdqu %%ymm15, 480(%1)\n vzeroupper\n"
        : "+r"(pat), "+r"(got), "+r"(spins)
        :
        : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

static void fpu_worker(void *arg)
{
    uint64_t seed = (uint64_t)(uintptr_t)arg;
    _Alignas(32) uint8_t pat[512], got[512];
    for (unsigned k = 0; k < sizeof(pat); k++)
        pat[k] = (uint8_t)(seed * 131 + k * 7 + (k >> 5));
    uint64_t end = now() + FPU_RUN_NS, fastest = UINT64_MAX;
    uint64_t took[64];
    unsigned rounds = 0;
    while (now() < end) {
        size_t n = have_avx && (seed & 1) ? 512 : 256;   /* a mix of SSE and AVX threads */
        memset(got, 0, n);
        uint64_t t0 = tsc();
        if (n == 512)
            avx_round(pat, got, FPU_SPINS);
        else
            sse_round(pat, got, FPU_SPINS);
        uint64_t t = tsc() - t0;
        if (t < fastest)
            fastest = t;
        took[rounds++ % 64] = t;
        if (memcmp(pat, got, n))
            __atomic_add_fetch(&fpu_errors, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&fpu_rounds, 1, __ATOMIC_RELAXED);
    }
    for (unsigned i = 0; i < rounds && i < 64; i++)
        if (took[i] > 3 * fastest)
            __atomic_add_fetch(&fpu_preempted, 1, __ATOMIC_RELAXED);
}

static bool wait_threads(handle_t *th, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        signals_t seen;
        CHECK_ST(jam_object_wait_one(th[i], SIG_TERMINATED, now() + 20 * S, &seen), OK);
        CHECK_ST(jam_handle_close(th[i]), OK);
    }
    return true;
}

static bool t_fpu_state_survives_preemption(void)
{
    static uint8_t stacks[FPU_THREADS][16384] __attribute__((aligned(64)));
    have_avx = avx_usable();
    handle_t th[FPU_THREADS];
    for (unsigned i = 0; i < FPU_THREADS; i++)
        CHECK_ST(thread_spawn("fpu", fpu_worker, (void *)(uintptr_t)(i + 1), stacks[i],
                              sizeof(stacks[i]), &th[i]),
                 OK);
    if (!wait_threads(th, FPU_THREADS))
        return false;
    printf("utest: fpu: %u threads, %u rounds (%u preempted mid-round), %s, %u mismatches\n",
           FPU_THREADS, fpu_rounds, fpu_preempted, have_avx ? "SSE + AVX" : "SSE only",
           fpu_errors);
    CHECK(fpu_rounds >= FPU_THREADS);
    CHECK(fpu_preempted > 0);   /* else the test proved nothing */
    CHECK_EQ(fpu_errors, 0);
    return true;
}

/* ---- many threads ------------------------------------------------------------ */

#define MANY 64
static volatile uint32_t many_count;

static void many_worker(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&many_count, 1, __ATOMIC_RELAXED);
}

static bool t_many_threads(void)
{
    static uint8_t stacks[MANY][8192] __attribute__((aligned(64)));
    handle_t th[MANY];
    for (unsigned round = 0; round < 3; round++) {
        many_count = 0;
        for (unsigned i = 0; i < MANY; i++)
            CHECK_ST(thread_spawn("many", many_worker, NULL, stacks[i], sizeof(stacks[i]),
                                  &th[i]),
                     OK);
        if (!wait_threads(th, MANY))
            return false;
        CHECK_EQ(many_count, MANY);
        struct job_info ji;
        CHECK_ST(info_of(own_job(), &ji), OK);
        CHECK_EQ(ji.used[JOB_LIMIT_THREADS], 1);   /* just us again */
    }
    /* A process outlives its first thread. */
    struct process_info info;
    if (!run_child("main-exits", 0, 0, &info))
        return false;
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 11);
    return true;
}

static bool t_kill_spinning_and_unstarted(void)
{
    handle_t job, proc, p2, v2;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("spin", NULL, job, HANDLE_INVALID, &proc), OK);
    jam_nanosleep(now() + 30 * MS);
    CHECK_ST(jam_process_kill(proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * S, &info), OK);
    CHECK(info.killed);
    CHECK_ST(jam_process_kill(proc), OK);   /* killing the dead is a no-op */
    CHECK_ST(jam_handle_close(proc), OK);
    /* A process that never started: killing it tears it down at once. */
    CHECK_ST(jam_process_create(job, "never", 5, 0, &p2, &v2), OK);
    CHECK_ST(jam_process_kill(p2), OK);
    CHECK_ST(spawn_wait(p2, S, &info), OK);
    CHECK(info.killed);
    CHECK_ST(jam_handle_close(v2), OK);
    CHECK_ST(jam_handle_close(p2), OK);
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(ji.used[k], 0);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

static bool t_startup_message(void)
{
    handle_t job, proc;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("startup", "hello", job, HANDLE_INVALID, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

static const struct {
    const char *name;
    bool (*fn)(void);
} tests[] = {
    { "basics", t_basics },
    { "rights", t_rights },
    { "bad_pointers", t_bad_pointers },
    { "startup_message", t_startup_message },
    { "crash_kills_only_the_child", t_crash_kills_only_the_child },
    { "wx", t_wx },
    { "ping_pong", t_ping_pong },
    { "kill_in_channel_call", t_kill_in_channel_call },
    { "runaway_hits_job_limits", t_runaway_hits_job_limits },
    { "kernel_objects_are_charged", t_kernel_objects_are_charged },
    { "kill_spinning_and_unstarted", t_kill_spinning_and_unstarted },
    { "fpu_state_survives_preemption", t_fpu_state_survives_preemption },
    { "many_threads", t_many_threads },
};

int main(int argc, char **argv)
{
    if (argc >= 2)
        return child_main(argc, argv);
    unsigned n = sizeof(tests) / sizeof(tests[0]), passed = 0;
    for (unsigned i = 0; i < n; i++) {
        cur = tests[i].name;
        uint64_t t0 = now();
        if (tests[i].fn()) {
            passed++;
            printf("utest: %s ok (%lu ms)\n", cur, (unsigned long)((now() - t0) / MS));
        }
    }
    char line[96];
    int len = passed == n ? snprintf(line, sizeof(line), "utest: %u passed", passed)
                          : snprintf(line, sizeof(line), "utest: %u passed, %u FAILED", passed,
                                     n - passed);
    jam_debug_report(line, (uint64_t)len);
    return passed == n ? 0 : 1;
}
