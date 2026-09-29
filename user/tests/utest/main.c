/* utest: the user-space test suite, run in ring 3 as a process under
 * init (and by the shell's `utest`).
 *
 * It checks the kernel from user space: system calls and handle
 * rights, bad user pointers (ERR_INVALID_ARGS, not a kill), a child that
 * crashes is killed and nothing else is, W^X, channel ping-pong between two
 * processes, a child killed while blocked in channel_call gives back every
 * page, handle and thread, a runaway child hits its job's limits and gets
 * ERR_NO_MEMORY / ERR_NO_RESOURCES (no panic), FPU/SSE/AVX state survives
 * preemption, and many threads come and go. Drivers: the null and
 * drvtest drivers (drivers/) run as processes and talk through
 * <idl/null.h>; with devmgr (init hands us its control channel,
 * SR_DEVMGR_CTL): the edu driver process it bound, called through
 * <idl/edu.h>, killed in the middle of a DMA, supervision bringing
 * drivers back, and what a driver's handles can't do. The devmgr tests
 * skip themselves without devmgr or the device (edu is QEMU's). The hid
 * driver process against a mock usb-bus and a mock console (hid.c).
 *
 * Children are this same program started with a mode ("utest nullderef",
 * see child.c), each in a job of its own so its usage can be read exactly.
 * One line per test ("utest: <name> ok"); the summary also goes into the
 * kernel's RESULTS box. Exit code 0 when everything passed. */
#define CHECK_PROG "utest"
#define CHECK_CUR  cur
#include <check.h>
#include <devmgr.h>
#include <idl/null.h>
#include <os.h>
#include "edu_check.h"
#include "utest.h"

static const char *cur;   /* the test running */

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
    CHECK_ST(spawn_wait(proc, 20 * NS_PER_S, info), OK);
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
    CHECK_ST(jam_nanosleep(t0 + 2 * NS_PER_MS), OK);
    CHECK(now() >= t0 + 2 * NS_PER_MS);

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
    CHECK_ST(jam_timer_set(tm, now() + 5 * NS_PER_MS), OK);
    CHECK_ST(jam_object_wait_one(tm, SIG_SIGNALED, now() + NS_PER_S, &seen), OK);

    CHECK_ST(jam_port_create(&port), OK);
    struct port_packet p = { .key = 7, .type = PORT_PACKET_USER }, out;
    p.user.data[0] = 42;
    CHECK_ST(jam_port_queue(port, &p), OK);
    CHECK_ST(jam_port_wait(port, now(), &out), OK);
    CHECK(out.key == 7 && out.type == PORT_PACKET_USER && out.user.data[0] == 42);
    CHECK_ST(jam_port_bind(port, e, 9, SIG_SIGNALED, PORT_BIND_ONCE), OK);
    CHECK_ST(jam_port_wait(port, now() + NS_PER_S, &out), OK);
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
            .ractual = (uint64_t)(uintptr_t)&ra, .deadline_ns = now() + 5 * NS_PER_S,
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
        .deadline_ns = now() + 5 * NS_PER_S,
    };
    CHECK_ST(jam_channel_call(&c), OK);
    CHECK_EQ(nh, 1);
    CHECK_ST(jam_event_signal(e, 0, SIG_SIGNALED), ERR_BAD_HANDLE);   /* it moved */
    CHECK_ST(jam_event_signal(back, 0, SIG_SIGNALED), OK);
    CHECK_ST(jam_handle_close(back), OK);

    CHECK_ST(jam_handle_close(a), OK);   /* the echo server sees PEER_CLOSED and exits 0 */
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * NS_PER_S, &info), OK);
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
    CHECK_ST(jam_object_wait_one(a, SIG_READABLE, now() + 10 * NS_PER_S, &seen), OK);
    jam_nanosleep(now() + 30 * NS_PER_MS);
    CHECK_ST(info_of(job, &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_THREADS], 2);   /* main (in the call) + the sleeper */
    CHECK(ji.used[JOB_LIMIT_PAGES] >= 16);      /* its heap */
    CHECK(ji.used[JOB_LIMIT_HANDLES] >= 5);
    CHECK(ji.used[JOB_LIMIT_MSG_BYTES] > 0);    /* its request, queued on our end */

    CHECK_ST(jam_process_kill(proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * NS_PER_S, &info), OK);
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
 * (else a program could make the kernel hold memory past its limits): a
 * VMO's struct and a process are handle units, a
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
#define FPU_RUN_NS  (400 * NS_PER_MS)
#define FPU_SPINS   4000000

static bool have_avx;
/* Counted by the workers (RELAXED adds), read once they have all ended. */
static uint32_t fpu_errors, fpu_rounds, fpu_preempted;

static bool avx_usable(void)
{
    uint32_t r[4];
    cpu_cpuid(1, 0, r);
    if (!(r[2] & (1u << 27)) || !(r[2] & (1u << 28)))   /* OSXSAVE, AVX */
        return false;
    return (cpu_xcr0() & 6) == 6;   /* the kernel saves SSE and AVX state */
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
        uint64_t t0 = cpu_tsc();
        if (n == 512)
            avx_round(pat, got, FPU_SPINS);
        else
            sse_round(pat, got, FPU_SPINS);
        uint64_t t = cpu_tsc() - t0;
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
        CHECK_ST(jam_object_wait_one(th[i], SIG_TERMINATED, now() + 20 * NS_PER_S, &seen), OK);
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
           FPU_THREADS, __atomic_load_n(&fpu_rounds, __ATOMIC_RELAXED),
           __atomic_load_n(&fpu_preempted, __ATOMIC_RELAXED),
           have_avx ? "SSE + AVX" : "SSE only", __atomic_load_n(&fpu_errors, __ATOMIC_RELAXED));
    CHECK(__atomic_load_n(&fpu_rounds, __ATOMIC_RELAXED) >= FPU_THREADS);
    CHECK(__atomic_load_n(&fpu_preempted, __ATOMIC_RELAXED) > 0);   /* else it proved nothing */
    CHECK_EQ(__atomic_load_n(&fpu_errors, __ATOMIC_RELAXED), 0);
    return true;
}

/* ---- many threads ------------------------------------------------------------ */

#define MANY 64
static uint32_t many_count;   /* the workers' RELAXED adds; read once they have ended */

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
        __atomic_store_n(&many_count, 0, __ATOMIC_RELAXED);
        for (unsigned i = 0; i < MANY; i++)
            CHECK_ST(thread_spawn("many", many_worker, NULL, stacks[i], sizeof(stacks[i]),
                                  &th[i]),
                     OK);
        if (!wait_threads(th, MANY))
            return false;
        CHECK_EQ(__atomic_load_n(&many_count, __ATOMIC_RELAXED), MANY);
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
    jam_nanosleep(now() + 30 * NS_PER_MS);
    CHECK_ST(jam_process_kill(proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * NS_PER_S, &info), OK);
    CHECK(info.killed);
    CHECK_ST(jam_process_kill(proc), OK);   /* killing the dead is a no-op */
    CHECK_ST(jam_handle_close(proc), OK);
    /* A process that never started: killing it tears it down at once. */
    CHECK_ST(jam_process_create(job, "never", 5, 0, &p2, &v2), OK);
    CHECK_ST(jam_process_kill(p2), OK);
    CHECK_ST(spawn_wait(p2, NS_PER_S, &info), OK);
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

/* A child that leaves a grandchild behind (spinning, in a job of its own,
 * with no handle to it anywhere): job_kill on the child's job reaps it,
 * returns only once it is dead, and the job is empty and closed to new
 * processes and jobs afterwards. */
static bool t_job_kill_reaps_orphans(void)
{
    handle_t job, proc, p2, v2, j2;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("orphan", NULL, job, HANDLE_INVALID, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_THREADS], 1);   /* the orphan, still spinning */
    CHECK_ST(jam_job_kill(own_job()), ERR_ACCESS_DENIED);   /* no RIGHT_MANAGE on our own */
    CHECK_ST(jam_job_kill(job), OK);
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(ji.used[k], 0);
    CHECK_ST(jam_process_create(job, "late", 4, 0, &p2, &v2), ERR_BAD_STATE);
    CHECK_ST(jam_job_create(job, 0, &j2), ERR_BAD_STATE);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* ---- drivers as processes ------------------------------------------------------- */

#define DRVTEST_NULL 0x40   /* drvtest's role for its channel to a null server */

/* Start bootfs driver drv/<name> in job, handing it h under driver role
 * `role` (h is consumed). */
static status_t driver(const char *name, handle_t job, uint32_t role, handle_t h,
                       handle_t *proc)
{
    char path[32];
    snprintf(path, sizeof(path), "drv/%s", name);
    const char *argv[] = { path };
    struct spawn_handle x = { SR_DRIVER(role), h };
    struct spawn_args a = {
        .path = path, .argc = 1, .argv = argv, .job = job, .extra = &x, .nextra = 1,
    };
    return spawn(&a, proc);
}

static bool job_is_empty(handle_t job)
{
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k])
            FAIL("the drivers' job still has %lu units of kind %u", (unsigned long)ji.used[k], k);
    return true;
}

/* The null driver as a process, called through the generated client from
 * here, then by the drvtest driver (a process too), which checks the whole
 * driver.h surface in the process build. */
static bool t_driver_processes(void)
{
    handle_t job, a, b, srv, cli;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(driver("null", job, DR_SERVE, b, &srv), OK);
    uint64_t v = 0;
    uint32_t sum = 0;
    CHECK_ST(null_ping(a, 7, &v), OK);
    CHECK_EQ(v, 7);
    CHECK_ST(null_add(a, 40, 2, &sum), OK);
    CHECK_EQ(sum, 42);
    CHECK_ST(driver("drvtest", job, DRVTEST_NULL, a, &cli), OK);   /* our end goes to it */
    struct process_info info;
    CHECK_ST(spawn_wait(cli, 30 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);   /* every drvtest check passed */
    CHECK_ST(spawn_wait(srv, 10 * NS_PER_S, &info), OK);   /* its client is gone: it returns */
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(cli), OK);
    CHECK_ST(jam_handle_close(srv), OK);
    if (!job_is_empty(job))
        return false;
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* Killing a driver process that is waiting for requests. */
static bool t_driver_killed(void)
{
    handle_t job, a, b, srv;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(driver("null", job, DR_SERVE, b, &srv), OK);
    uint8_t data[16], rev[16];
    for (int i = 0; i < 16; i++)
        data[i] = (uint8_t)i;
    CHECK_ST(null_reverse(a, data, rev), OK);
    CHECK(rev[0] == 15 && rev[15] == 0);
    CHECK_ST(jam_process_kill(srv), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(srv, 10 * NS_PER_S, &info), OK);
    CHECK(info.killed);
    uint64_t v;
    CHECK_ST(null_ping(a, 1, &v), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(srv), OK);
    if (!job_is_empty(job))
        return false;
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

static bool t_startup_message(void)
{
    handle_t job, proc;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("startup", "hello", job, HANDLE_INVALID, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* ---- devmgr and the edu driver process ------------------------------------------------ */

#define EDU_VENDOR 0x1234
#define EDU_DEVICE 0x11e8
#define CMD_BME    0x04

/* devmgr's control channel (the tests kill, rebind and look at
 * drivers' handles; init and the shell's `utest` hand it to us), or 0
 * (with a line saying the test is skipped). */
static handle_t devmgr(void)
{
    handle_t dm = startup_handle(SR_DEVMGR_CTL);
    if (!dm)
        printf("utest: %s: no devmgr control channel (not started by init or the shell's "
               "utest?): skipped\n", cur);
    return dm;
}

static status_t dm_call(handle_t dm, uint32_t op, uint16_t vendor, uint16_t device,
                        struct devmgr_rep *r, handle_t *hs, uint32_t *nh)
{
    return devmgr_call(dm, op, vendor, device, 0, r, hs, hs ? DEVMGR_MAX_HANDLES : 0, nh,
                       now() + 30 * NS_PER_S);
}

/* The edu protocol end to end: utest -> devmgr's edu driver process. */
static bool t_edu_process(void)
{
    handle_t dm = devmgr(), hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    struct devmgr_rep r;
    if (!dm)
        return true;
    status_t st = dm_call(dm, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no edu device (not QEMU?): skipped\n", cur);
        return true;
    }
    CHECK_ST(st, OK);
    CHECK_EQ(nh, 1);
    struct edu_check_result res;
    CHECK_ST(edu_check(hs[0], now() + 60 * NS_PER_S, &res), OK);
    char line[160];
    int n = snprintf(line, sizeof(line),
                     "edu (process): factorial(10)=%u ok, DMA 4 KiB round trip ok in %lu us, "
                     "MSI -> driver in %lu us",
                     res.fact10, (unsigned long)(res.dma_ns / 1000),
                     (unsigned long)(res.msi_ns / 1000));
    jam_debug_report(line, (uint64_t)n);
    CHECK_ST(jam_handle_close(hs[0]), OK);
    /* What devmgr must refuse. */
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, 0x1234, 0x0bad, &r, hs, &nh), ERR_NOT_FOUND);
    CHECK_ST(dm_call(dm, 0x00030063u, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh), ERR_NOT_SUPPORTED);
    CHECK_ST(dm_call(dm, DEVMGR_STATUS, 0, 0, &r, NULL, NULL), OK);
    CHECK(r.a >= 1);   /* edu at least */
    return true;
}

/* devmgr's query channel (SR_DEVMGR) answers the queries and
 * refuses everything that changes something or hands out hardware. */
static bool t_devmgr_query_channel(void)
{
    handle_t q = startup_handle(SR_DEVMGR);
    struct devmgr_rep r;
    handle_t hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    if (!q || !devmgr())
        return true;
    CHECK_ST(dm_call(q, DEVMGR_STATUS, 0, 0, &r, NULL, NULL), OK);
    static const uint32_t refused[] = { DEVMGR_KILL, DEVMGR_REBIND, DEVMGR_DRIVER_VIEW,
                                        DEVMGR_TEST_DRIVER, DEVMGR_SET_CONSOLE, 0x00030063u };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        nh = 0;
        CHECK_ST(dm_call(q, refused[i], 0xffff, 0xffff, &r, hs, &nh), ERR_ACCESS_DENIED);
        CHECK_EQ(nh, 0);
    }
    /* SET_CONSOLE with its handle: refused, the handle closed. */
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    struct devmgr_req qr = { 0, DEVMGR_SET_CONSOLE, 0, 0, 0 };
    uint32_t n = 0, got = 0;
    struct channel_call_args ca = {
        .h = q, .wn = sizeof(qr), .wbytes = (uint64_t)(uintptr_t)&qr,
        .wh = (uint64_t)(uintptr_t)&b, .whn = 1, .rcap = sizeof(r),
        .rbytes = (uint64_t)(uintptr_t)&r, .ractual = (uint64_t)(uintptr_t)&n,
        .rhactual = (uint64_t)(uintptr_t)&got, .deadline_ns = now() + 10 * NS_PER_S,
    };
    CHECK_ST(jam_channel_call(&ca), OK);
    CHECK(n >= DEVMGR_REP_HDR);
    CHECK_ST(r.status, ERR_ACCESS_DENIED);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(a, SIG_PEER_CLOSED, now() + 5 * NS_PER_S, &seen), OK);
    jam_handle_close(a);
    return true;
}

/* devmgr's supervision view of a device. */
static bool supervision(handle_t dm, uint16_t vendor, uint16_t device, struct devmgr_rep *r)
{
    CHECK_ST(dm_call(dm, DEVMGR_SUPERVISION, vendor, device, r, NULL, NULL), OK);
    return true;
}

/* Killing the edu driver process while its DMA runs, and supervision
 * bringing it back: Bus Master Enable goes off, its pinned buffer is
 * quarantined (still charged to its job), its MSI vector is free; devmgr
 * restarts it at once (a KILL is a death like a crash) with a new vector
 * and dma_cap; the client reconnects through GET_SERVICE and factorial and
 * DMA work; once the new driver has turned bus mastering on, the
 * quarantine lets go (a grace period later) with no page written while it
 * held them, and the dead driver's job is empty. */
static bool t_edu_killed_mid_dma(void)
{
    handle_t dm = devmgr(), hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    struct devmgr_rep r, sup;
    if (!dm)
        return true;
    status_t st = dm_call(dm, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no edu device (not QEMU?): skipped\n", cur);
        return true;
    }
    CHECK_ST(st, OK);
    handle_t ch = hs[0];
    CHECK_ST(dm_call(dm, DEVMGR_GET_DRIVER, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh), OK);
    CHECK_EQ(nh, 3);
    handle_t proc = hs[0], job = hs[1], dev = hs[2];
    if (!supervision(dm, EDU_VENDOR, EDU_DEVICE, &sup))
        return false;
    CHECK_EQ(sup.a, DEVMGR_SUP_RUNNING);
    uint32_t restarts0 = sup.b, changed0 = sup.e;

    uint64_t addr = 0;
    uint32_t cmd = 0, f = 0;
    CHECK_ST(edu_dma_start_until(ch, now() + 10 * NS_PER_S, 4096, &addr), OK);   /* running now */
    CHECK(addr && addr < (1ull << 32));
    CHECK_ST(jam_pci_config_read(dev, 0x04, 2, &cmd), OK);
    CHECK(cmd & CMD_BME);
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    CHECK(ji.used[JOB_LIMIT_PAGES] > 0);
    uint64_t t0 = now();
    CHECK_ST(dm_call(dm, DEVMGR_KILL, EDU_VENDOR, EDU_DEVICE, &r, NULL, NULL), OK);
    printf("utest: %s: killed mid-DMA, dead %lu us later\n", cur,
           (unsigned long)((now() - t0) / 1000));
    struct process_info info;
    CHECK_ST(jam_process_get_info(proc, &info), OK);
    CHECK_EQ(info.state, PROCESS_DEAD);
    CHECK(info.killed);
    /* Its pinned buffer is quarantined, still charged to its job. */
    if (!supervision(dm, EDU_VENDOR, EDU_DEVICE, &sup))
        return false;
    CHECK(sup.a == DEVMGR_SUP_RESTARTING || sup.a == DEVMGR_SUP_RUNNING);
    CHECK(sup.d >= 2);
    CHECK_ST(info_of(job, &ji), OK);
    CHECK(ji.used[JOB_LIMIT_PAGES] > 0);
    CHECK_ST(edu_factorial_until(ch, now() + 5 * NS_PER_S, 3, &f), ERR_PEER_CLOSED);
    CHECK_ST(jam_pci_config_write(dev, 0x3c, 1, 0), ERR_ACCESS_DENIED);   /* a read-only view */
    CHECK_ST(jam_process_kill(proc), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);

    /* The reconnect rule: ask devmgr again; the call waits for the restart. */
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh), OK);
    CHECK_ST(edu_factorial_until(hs[0], now() + 10 * NS_PER_S, 10, &f), OK);
    CHECK_EQ(f, 3628800);
    printf("utest: %s: restarted and answering %lu ms after the kill\n", cur,
           (unsigned long)((now() - t0) / NS_PER_MS));
    CHECK_ST(edu_dma_roundtrip_until(hs[0], now() + 10 * NS_PER_S, 4096), OK);
    CHECK_ST(jam_handle_close(hs[0]), OK);
    CHECK_ST(jam_pci_config_read(dev, 0x04, 2, &cmd), OK);
    CHECK(cmd & CMD_BME);
    /* The quarantine lets the dead driver's pages go a grace period (1 s)
     * after the new driver turned bus mastering on. Its count drops only
     * once the pages are back, so then the job is empty (pins, VMOs,
     * threads: gone), and nothing wrote them meanwhile. */
    uint64_t until = now() + 10 * NS_PER_S;
    for (;;) {
        if (!supervision(dm, EDU_VENDOR, EDU_DEVICE, &sup))
            return false;
        if (!sup.d || now() > until)
            break;
        jam_nanosleep(now() + 20 * NS_PER_MS);
    }
    if (!job_is_empty(job))
        return false;
    printf("utest: %s: restarts %u, quarantine %u page(s) left, %u stale page(s)\n", cur,
           sup.b - restarts0, sup.d, sup.e - changed0);
    CHECK_EQ(sup.a, DEVMGR_SUP_RUNNING);
    CHECK_EQ(sup.b, restarts0 + 1);
    CHECK_EQ(sup.d, 0);
    CHECK_EQ(sup.e, changed0);   /* 0 stale bytes */
    CHECK_ST(jam_handle_close(job), OK);
    CHECK_ST(jam_handle_close(dev), OK);
    return true;
}

/* ---- supervision with the crash-test driver ----------------------------------------- */

#define TV DEVMGR_TEST_VENDOR
#define TD DEVMGR_TEST_DEVICE

/* PING: when this instance of the crash-test driver started. */
static status_t crasher_ping(handle_t ch, uint64_t deadline, uint64_t *started)
{
    uint32_t q[2] = { 0, CRASHER_PING };
    struct {
        uint32_t txid;       /* ours */
        int32_t  status;     /* OK */
        uint64_t started;    /* when this instance of the driver started */
    } __attribute__((packed)) rep;
    uint32_t n = 0;
    struct channel_call_args a = {
        .h = ch, .wn = sizeof(q), .wbytes = (uint64_t)(uintptr_t)q, .rcap = sizeof(rep),
        .rbytes = (uint64_t)(uintptr_t)&rep, .ractual = (uint64_t)(uintptr_t)&n,
        .deadline_ns = deadline,
    };
    status_t st = jam_channel_call(&a);
    if (st != OK)
        return st;
    if (n < 8 || (rep.status == OK && n < sizeof(rep)))
        return ERR_INTERNAL;
    *started = rep.started;
    return rep.status;
}

/* The crash-test driver's channel and process, or false (with a skip or a
 * failure line). */
static bool crasher(handle_t dm, handle_t *ch, handle_t *proc, bool *skip)
{
    struct devmgr_rep r;
    handle_t hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    *skip = false;
    status_t st = dm_call(dm, DEVMGR_TEST_DRIVER, 0, 0, &r, NULL, NULL);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no drv/crasher: skipped\n", cur);
        *skip = true;
        return false;
    }
    CHECK_ST(st, OK);
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, TV, TD, &r, hs, &nh), OK);
    *ch = hs[0];
    uint64_t started = 0;
    /* up, whenever it started */
    CHECK_ST(crasher_ping(*ch, now() + 10 * NS_PER_S, &started), OK);
    CHECK_ST(dm_call(dm, DEVMGR_GET_DRIVER, TV, TD, &r, hs, &nh), OK);
    CHECK_EQ(nh, 2);   /* process and job: no hardware */
    *proc = hs[0];
    CHECK_ST(jam_handle_close(hs[1]), OK);
    /* No function behind it: DRIVER_VIEW must not open PCI function 0. */
    uint32_t vh = 0;
    CHECK_ST(dm_call(dm, DEVMGR_DRIVER_VIEW, TV, TD, &r, hs, &vh), ERR_NOT_FOUND);
    CHECK_EQ(vh, 0);
    return true;
}

/* Crash it through ch (it answers nothing), and see it die. *t: when the
 * crash was asked for. */
static bool crash_it(handle_t ch, handle_t proc, uint64_t *t)
{
    uint32_t q[2] = { 0x77, CRASHER_CRASH };
    *t = now();
    CHECK_ST(jam_channel_write(ch, q, sizeof(q), NULL, 0), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &info), OK);
    CHECK(info.killed);   /* a crash: the kernel killed it */
    uint64_t started;
    CHECK_ST(crasher_ping(ch, now() + 5 * NS_PER_S, &started), ERR_PEER_CLOSED);
    return true;
}

/* Crash once, reconnect: *delay_ms from the crash to the new instance's
 * start (the backoff, and a start). */
static bool crash_and_reconnect(handle_t dm, handle_t *ch, handle_t *proc, uint64_t *delay_ms)
{
    struct devmgr_rep r;
    handle_t hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    uint64_t t = 0, started = 0;
    if (!crash_it(*ch, *proc, &t))
        return false;
    CHECK_ST(jam_handle_close(*ch), OK);
    CHECK_ST(jam_handle_close(*proc), OK);
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, TV, TD, &r, hs, &nh), OK);   /* the new channel */
    *ch = hs[0];
    CHECK_ST(crasher_ping(*ch, now() + 15 * NS_PER_S, &started), OK);   /* waits for the restart */
    CHECK(started > t);
    *delay_ms = (started - t) / NS_PER_MS;
    CHECK_ST(dm_call(dm, DEVMGR_GET_DRIVER, TV, TD, &r, hs, &nh), OK);
    *proc = hs[0];
    CHECK_ST(jam_handle_close(hs[1]), OK);
    return true;
}

static uint32_t sup_restarts0;   /* the crash-test driver's restarts before these tests */

/* A driver that crashes comes back by itself; its clients reconnect. */
static bool t_supervised_restart(void)
{
    handle_t dm = devmgr(), ch, proc;
    struct devmgr_rep sup;
    bool skip;
    if (!dm)
        return true;
    if (!crasher(dm, &ch, &proc, &skip))
        return skip;
    if (!supervision(dm, TV, TD, &sup))
        return false;
    CHECK_EQ(sup.a, DEVMGR_SUP_RUNNING);
    sup_restarts0 = sup.b;
    uint64_t ms = 0;
    if (!crash_and_reconnect(dm, &ch, &proc, &ms))
        return false;
    printf("utest: %s: crashed, restarted and answering %lu ms later\n", cur, (unsigned long)ms);
    CHECK(ms >= 100 && ms < 3000);   /* the first backoff is 100 ms */
    if (!supervision(dm, TV, TD, &sup))
        return false;
    CHECK_EQ(sup.a, DEVMGR_SUP_RUNNING);
    CHECK_EQ(sup.b, sup_restarts0 + 1);
    CHECK_EQ(sup.c, 100);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    return true;
}

/* Each restart within a minute doubles the backoff: 200, 400, 800, 1600 ms. */
static bool t_supervised_backoff(void)
{
    handle_t dm = devmgr(), ch, proc;
    struct devmgr_rep sup;
    bool skip;
    if (!dm)
        return true;
    if (!crasher(dm, &ch, &proc, &skip))
        return skip;
    for (uint32_t k = 1; k <= 4; k++) {
        uint64_t ms = 0, want = 100ull << k;
        if (!crash_and_reconnect(dm, &ch, &proc, &ms))
            return false;
        if (!supervision(dm, TV, TD, &sup))
            return false;
        printf("utest: %s: restart %u after %lu ms (backoff %u ms)\n", cur, k + 1,
               (unsigned long)ms, sup.c);
        CHECK_EQ(sup.c, want);
        CHECK(ms >= want && ms < want + 3000);
        CHECK_EQ(sup.b, sup_restarts0 + 1 + k);
    }
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    return true;
}

/* The 6th death within a minute: devmgr gives up (GET_SERVICE says
 * ERR_BAD_STATE); TEST_DRIVER starts it afresh; a driver that exits 0 by
 * itself is finished, not restarted. */
static bool t_supervised_give_up(void)
{
    handle_t dm = devmgr(), ch, proc, hs[DEVMGR_MAX_HANDLES];
    struct devmgr_rep sup, r;
    uint32_t nh = 0;
    bool skip;
    if (!dm)
        return true;
    if (!crasher(dm, &ch, &proc, &skip))
        return skip;
    uint64_t t = 0;
    if (!crash_it(ch, proc, &t))
        return false;
    uint64_t until = now() + 5 * NS_PER_S;
    for (;;) {
        if (!supervision(dm, TV, TD, &sup))
            return false;
        if (sup.a == DEVMGR_SUP_GAVE_UP || now() > until)
            break;
        jam_nanosleep(now() + 10 * NS_PER_MS);
    }
    printf("utest: %s: after %u restarts: state %u (3 = gave up)\n", cur,
           sup.b - sup_restarts0, sup.a);
    CHECK_EQ(sup.a, DEVMGR_SUP_GAVE_UP);
    CHECK_EQ(sup.b, sup_restarts0 + 5);
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, TV, TD, &r, hs, &nh), ERR_BAD_STATE);
    jam_nanosleep(now() + 300 * NS_PER_MS);   /* no restart comes */
    if (!supervision(dm, TV, TD, &sup))
        return false;
    CHECK_EQ(sup.a, DEVMGR_SUP_GAVE_UP);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);

    /* Started again on request, with a fresh history; exit 0 = finished. */
    if (!crasher(dm, &ch, &proc, &skip))
        return false;
    uint32_t q[3] = { 0x78, CRASHER_EXIT, 0 };
    CHECK_ST(jam_channel_write(ch, q, sizeof(q), NULL, 0), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &info), OK);
    CHECK(!info.killed && info.exit_code == 0);
    until = now() + 5 * NS_PER_S;
    for (;;) {
        if (!supervision(dm, TV, TD, &sup))
            return false;
        if (sup.a == DEVMGR_SUP_FINISHED || now() > until)
            break;
        jam_nanosleep(now() + 10 * NS_PER_MS);
    }
    CHECK_EQ(sup.a, DEVMGR_SUP_FINISHED);
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, TV, TD, &r, hs, &nh), ERR_BAD_STATE);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    return true;
}

/* What a driver's handles can't do, with a function that has MSI-X (the
 * same handles devmgr gives its driver, minus the interrupt and the
 * dma_cap): map its MSI-X table or PBA page, turn on bus mastering, make a
 * dma_cap or an interrupt object, write its MSI-X capability, reach any
 * other function (no RES_PCI, no slicing), get DMA memory or pin without
 * a dma_cap. */
static bool t_driver_handle_limits(void)
{
    handle_t dm = devmgr(), hs[DEVMGR_MAX_HANDLES], x, v;
    uint32_t nh = 0;
    struct devmgr_rep r;
    if (!dm)
        return true;
    /* The first function with MSI-X whose table and PBA BARs a driver
     * could get (on the PC some BARs stay unsized and are never handed
     * out, e.g. the VMD controller's). */
    status_t st = ERR_NOT_FOUND;
    uint32_t cap = 0;
    for (uint32_t inst = 0; inst < 32; inst++) {
        nh = 0;
        st = devmgr_call(dm, DEVMGR_DRIVER_VIEW, 0xffff, 0xffff, inst, &r, hs, DEVMGR_MAX_HANDLES,
                         &nh, now() + 30 * NS_PER_S);
        if (st == ERR_NOT_FOUND)
            break;
        uint32_t t = 0, p = 0;
        if (st == OK && nh >= 2 && (cap = pci_find_cap(hs[0], 0x11)) &&
            jam_pci_config_read(hs[0], cap + 4, 4, &t) == OK &&
            jam_pci_config_read(hs[0], cap + 8, 4, &p) == OK &&
            (r.a & (1u << (t & 7))) && (r.a & (1u << (p & 7))))
            break;
        printf("utest: %s: MSI-X function #%u not usable (%s), trying the next\n", cur, inst,
               st == OK ? "table BAR not handed out" : status_str(st));
        for (uint32_t k = 0; k < nh; k++)
            jam_handle_close(hs[k]);
        st = ERR_NOT_FOUND;
    }
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no usable function with MSI-X: skipped\n", cur);
        return true;
    }
    CHECK_ST(st, OK);
    handle_t dev = hs[0];
    uint32_t mask = r.a, tab = 0, pba = 0, ctl = 0, cmd = 0, id = 0;
    CHECK_ST(jam_pci_config_read(dev, 0, 4, &id), OK);
    printf("utest: %s: using %04x:%04x\n", cur, id & 0xffff, id >> 16);
    CHECK_ST(jam_pci_config_read(dev, cap + 4, 4, &tab), OK);
    CHECK_ST(jam_pci_config_read(dev, cap + 8, 4, &pba), OK);
    CHECK_ST(jam_pci_config_read(dev, cap + 2, 2, &ctl), OK);
    CHECK_ST(jam_pci_config_read(dev, 0x04, 2, &cmd), OK);
    /* The MSI-X table and PBA pages, in whichever BAR each lives. */
    const uint32_t where[2] = { tab, pba };
    for (int i = 0; i < 2; i++) {
        uint32_t bir = where[i] & 7, page = where[i] & ~0xfffu;
        CHECK(bir < 6 && (mask & (1u << bir)));
        handle_t bar = hs[1 + __builtin_popcount(mask & ((1u << bir) - 1))];
        CHECK_ST(jam_vmo_create_physical(bar, page, 4096, VMO_CACHE_UC, &x), ERR_ACCESS_DENIED);
        /* The rest of the BAR is the driver's: page 0, unless it holds the
         * table or the PBA itself. */
        bool zero_protected = page == 0 || ((where[1 - i] & 7) == bir &&
                                            (where[1 - i] & ~0xfffu) == 0);
        if (!zero_protected) {
            CHECK_ST(jam_vmo_create_physical(bar, 0, 4096, VMO_CACHE_UC, &x), OK);
            CHECK_ST(jam_handle_close(x), OK);
        }
        CHECK_ST(jam_resource_create(bar, RES_MMIO, 0, 4096, &x), ERR_ACCESS_DENIED);
    }
    /* Config: the kernel's bits and capabilities are read-only. */
    CHECK_ST(jam_pci_config_write(dev, 0x04, 2, cmd ^ CMD_BME), ERR_ACCESS_DENIED);
    CHECK_ST(jam_pci_config_write(dev, cap + 2, 2, ctl), ERR_ACCESS_DENIED);
    CHECK_ST(jam_pci_config_write(dev, 0x10, 4, 0xffffffffu), ERR_ACCESS_DENIED);   /* BAR 0 */
    /* No RIGHT_MANAGE. */
    CHECK_ST(jam_pci_bus_master(dev, 1), ERR_ACCESS_DENIED);
    CHECK_ST(jam_dma_cap_create(dev, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_interrupt_create_msi(dev, 0, IRQ_MSIX, &x), ERR_ACCESS_DENIED);
    /* No other function: its handle is one function, and can't be sliced. */
    struct pci_dev_info info;
    CHECK_ST(jam_pci_enum(dev, 0, &info), ERR_WRONG_TYPE);
    CHECK_ST(jam_pci_device_open(dev, 0, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_resource_create(dev, RES_PCI, 0, 0, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_pci_bar_resource(dev, tab & 7, &x), ERR_ACCESS_DENIED);
    /* Nothing to pass on: the function and its BARs can't be
     * duplicated or sent (DR_SERVE is a channel like this one), nor can
     * the registers as a VMO. */
    handle_t ca, cb;
    CHECK_ST(jam_channel_create(&ca, &cb), OK);
    CHECK_ST(jam_handle_duplicate(dev, RIGHT_SAME, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_channel_write(ca, "x", 1, &dev, 1), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_duplicate(hs[1], RIGHT_SAME, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_channel_write(ca, "x", 1, &hs[1], 1), ERR_ACCESS_DENIED);
    /* A register page of the lowest BAR (hs[1]) that isn't the table or
     * the PBA. */
    uint32_t low = (uint32_t)__builtin_ctz(mask), pg = 0;
    while (((tab & 7) == low && (tab & ~0xfffu) == pg) ||
           ((pba & 7) == low && (pba & ~0xfffu) == pg))
        pg += 4096;
    if (jam_vmo_create_physical(hs[1], pg, 4096, VMO_CACHE_UC, &x) == OK) {
        CHECK_ST(jam_handle_duplicate(x, RIGHT_SAME, &v), ERR_ACCESS_DENIED);
        CHECK_ST(jam_channel_write(ca, "x", 1, &x, 1), ERR_ACCESS_DENIED);
        CHECK_ST(jam_handle_close(x), OK);
    }
    CHECK_ST(jam_handle_close(ca), OK);
    CHECK_ST(jam_handle_close(cb), OK);
    /* No dma_cap: no DMA memory, no pins. */
    CHECK_ST(jam_vmo_create(4096, DRV_VMO_CONTIGUOUS | DRV_VMO_DMA32, HANDLE_INVALID, &x),
             ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_create(4096, DRV_VMO_DMA32, dev, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_create(4096, 0, HANDLE_INVALID, &v), OK);
    uint64_t addr = 0, pin = 0;
    CHECK_ST(jam_vmo_pin(v, HANDLE_INVALID, 0, 4096, &addr, &pin), ERR_BAD_HANDLE);
    CHECK_ST(jam_vmo_pin(v, dev, 0, 4096, &addr, &pin), ERR_WRONG_TYPE);
    CHECK_ST(jam_handle_close(v), OK);
    for (uint32_t i = 0; i < nh; i++)
        CHECK_ST(jam_handle_close(hs[i]), OK);
    return true;
}

static const struct {
    const char *name;    /* in the "utest: <name> ok" lines */
    bool (*fn)(void);    /* true: passed */
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
    { "job_kill_reaps_orphans", t_job_kill_reaps_orphans },
    { "fpu_state_survives_preemption", t_fpu_state_survives_preemption },
    { "many_threads", t_many_threads },
    { "driver_processes", t_driver_processes },
    { "driver_killed", t_driver_killed },
    { "edu_process", t_edu_process },
    { "devmgr_query_channel", t_devmgr_query_channel },

    { "edu_killed_mid_dma", t_edu_killed_mid_dma },
    { "driver_handle_limits", t_driver_handle_limits },
    { "hid_typing", t_hid_typing },
    { "hid_modifiers", t_hid_modifiers },
    { "hid_rollover", t_hid_rollover },
    { "hid_repeat", t_hid_repeat },
    { "hid_mouse", t_hid_mouse },
    { "hid_composite", t_hid_composite },
    { "hid_unplug_and_console_gone", t_hid_unplug_and_console_gone },
    { "supervised_restart", t_supervised_restart },
    { "supervised_backoff", t_supervised_backoff },
    { "supervised_give_up", t_supervised_give_up },
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
            printf("utest: %s ok (%lu ms)\n", cur, (unsigned long)((now() - t0) / NS_PER_MS));
        }
    }
    char line[96];
    int len = passed == n ? snprintf(line, sizeof(line), "utest: %u passed", passed)
                          : snprintf(line, sizeof(line), "utest: %u passed, %u FAILED", passed,
                                     n - passed);
    jam_debug_report(line, (uint64_t)len);
    return passed == n ? 0 : 1;
}
