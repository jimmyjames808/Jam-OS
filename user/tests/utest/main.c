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
 * driver process against a mock usb-bus and a mock console (hid.c). The
 * fat service process over a RAM disk (fat.c, fat_names.c).
 *
 * This file has the helpers, the kernel tests and the table; threads.c,
 * drivers.c, supervise.c, hid.c and the fat files the rest (utest.h lists
 * them).
 * Children are this same program started with a mode ("utest nullderef",
 * see child.c), each in a job of its own so its usage can be read exactly.
 * One line per test ("utest: <name> ok"); the summary also goes into the
 * kernel's RESULTS box. Exit code 0 when everything passed. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <wants.h>
#include "utest.h"

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc devmgr\n"
          "svc devmgr-ctl\n"
          "mount * rw\n"
          "right klog\n"
          "right sysinfo\n");

const char *utest_cur;   /* the test running */

/* ---- helpers ------------------------------------------------------------------ */

handle_t own_job(void)
{
    return startup_handle(SR_JOB);
}

status_t info_of(handle_t job, struct job_info *ji)
{
    return jam_job_get_info(job, ji);
}

status_t new_job(handle_t *out)
{
    return jam_job_create(own_job(), 0, out);
}

/* Start "utest <mode> [arg]" in job, with up to one extra handle. */
status_t child(const char *mode, const char *arg, handle_t job, handle_t extra,
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
bool run_child(const char *mode, uint32_t limit_kind, uint64_t limit,
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

/* kexec_load, kexec_reboot and klog_name refuse what they must (the rights
 * on the root resource only where we were given one: init's run gives it,
 * and the shell gives it for the list's `right klog`, without
 * RIGHT_TRANSFER). Nobody gives us RIGHT_ROOT_KEXEC (init alone holds it),
 * so its argument checks run only where a starter does. Nothing here loads
 * an image or names the log: either would change this boot (the stored
 * kernel replaced, the crash log's name). */
static bool t_kexec_refusals(void)
{
    handle_t root = startup_handle(SR_RESOURCE), v, rd, mapo, klog, m;
    CHECK_ST(jam_vmo_create(8192, 0, HANDLE_INVALID, &v), OK);
    CHECK_ST(jam_kexec_load(v, v, v, NULL, 0, 0), ERR_WRONG_TYPE);   /* not a resource */
    CHECK_ST(jam_kexec_load(0x7fff0000u, v, v, NULL, 0, 0), ERR_BAD_HANDLE);
    CHECK_ST(jam_kexec_reboot(v), ERR_WRONG_TYPE);
    if (!root) {   /* started by a program that gave it no root resource */
        CHECK_ST(jam_handle_close(v), OK);
        return true;
    }
    CHECK_ST(jam_handle_duplicate(root, RIGHT_WAIT | RIGHT_INSPECT | RIGHT_ROOT_KLOG, &rd), OK);
    CHECK_ST(jam_kexec_load(rd, v, v, NULL, 0, 0), ERR_ACCESS_DENIED);   /* no KEXEC */
    CHECK_ST(jam_kexec_reboot(rd), ERR_ACCESS_DENIED);                  /* no REBOOT */

    CHECK_ST(jam_klog_open(rd, &klog), OK);
    CHECK_ST(jam_klog_name(v, "boot-0001", 9), ERR_WRONG_TYPE);
    CHECK_ST(jam_klog_name(klog, "a b", 3), ERR_INVALID_ARGS);
    CHECK_ST(jam_klog_name(klog, "", 0), ERR_INVALID_ARGS);
    CHECK_ST(jam_klog_name(klog, "boot-0001-and-a-name-too-long-to-keep", 37), ERR_INVALID_ARGS);
    CHECK_ST(jam_klog_name(klog, (const char *)8, 4), ERR_INVALID_ARGS);   /* a bad pointer */
    CHECK_ST(jam_handle_close(klog), OK);
    CHECK_ST(jam_handle_close(rd), OK);

    /* With the root's RIGHT_ROOT_KEXEC, the arguments too. */
    if (jam_handle_duplicate(root, RIGHT_WAIT | RIGHT_ROOT_KEXEC, &m) == OK) {
        CHECK_ST(jam_handle_duplicate(v, RIGHTS_BASIC | RIGHT_MAP, &mapo), OK);
        CHECK_ST(jam_kexec_load(m, v, v, NULL, 0, 1), ERR_INVALID_ARGS);            /* flags */
        CHECK_ST(jam_kexec_load(m, v, m, NULL, 0, 0), ERR_WRONG_TYPE);              /* bootfs */
        CHECK_ST(jam_kexec_load(m, mapo, v, NULL, 0, 0), ERR_ACCESS_DENIED);        /* no READ */
        CHECK_ST(jam_kexec_load(m, v, v, (const char *)8, 4, 0), ERR_INVALID_ARGS); /* pointer */
        CHECK_ST(jam_kexec_load(m, v, v, "a\nb", 3, 0), ERR_INVALID_ARGS);          /* not text */
        status_t st = jam_kexec_load(m, v, v, NULL, 0, 0);   /* zeros: no ELF */
        CHECK(st == ERR_INVALID_ARGS || st == ERR_NOT_SUPPORTED);
        CHECK_ST(jam_handle_close(mapo), OK);
        CHECK_ST(jam_handle_close(m), OK);
    }
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

static const struct {
    const char *name;    /* in the "utest: <name> ok" lines */
    bool (*fn)(void);    /* true: passed */
} tests[] = {
    { "basics", t_basics },
    { "rights", t_rights },
    { "kexec_refusals", t_kexec_refusals },
    { "root_powers", t_root_powers },
    { "vmo_make_exec", t_vmo_make_exec },
    { "kept_vmo_refusals", t_kept_vmo_refusals },
    { "kept_vmo_client_and_compositor", t_kept_vmo_client_and_compositor },
    { "bad_pointers", t_bad_pointers },
    { "startup_message", t_startup_message },
    { "crash_kills_only_the_child", t_crash_kills_only_the_child },
    { "wx", t_wx },
    { "ping_pong", t_ping_pong },
    { "kill_in_channel_call", t_kill_in_channel_call },
    { "call_timeout_relative", t_call_timeout_relative },
    { "reply_wait_mark_before_wait", t_reply_wait_mark_before_wait },
    { "reply_wait_gone_caller", t_reply_wait_gone_caller },
    { "reply_wait_second_process", t_reply_wait_second_process },
    { "reply_wait_port", t_reply_wait_port },
    { "reply_wait_refusals", t_reply_wait_refusals },
    { "runaway_hits_job_limits", t_runaway_hits_job_limits },
    { "kernel_objects_are_charged", t_kernel_objects_are_charged },
    { "kill_spinning_and_unstarted", t_kill_spinning_and_unstarted },
    { "job_kill_reaps_orphans", t_job_kill_reaps_orphans },
    { "fpu_state_survives_preemption", t_fpu_state_survives_preemption },
    { "fpu_call_keeps_control_words", t_fpu_call_keeps_control_words },
    { "fpu_ring3_switch_keeps_all", t_fpu_ring3_switch_keeps_all },
    { "many_threads", t_many_threads },
    { "lock_take", t_lock_take },
    { "driver_processes", t_driver_processes },
    { "driver_killed", t_driver_killed },
    { "edu_process", t_edu_process },
    { "devmgr_query_channel", t_devmgr_query_channel },
    { "devmgr_query_refuses_hda", t_devmgr_query_refuses_hda },
    { "devmgr_device_channel", t_devmgr_device_channel },
    { "devmgr_openers", t_devmgr_openers },

    { "edu_killed_mid_dma", t_edu_killed_mid_dma },
    { "driver_handle_limits", t_driver_handle_limits },
    { "hid_typing", t_hid_typing },
    { "hid_modifiers", t_hid_modifiers },
    { "hid_rollover", t_hid_rollover },
    { "hid_repeat", t_hid_repeat },
    { "hid_mouse", t_hid_mouse },
    { "hid_composite", t_hid_composite },
    { "hid_unplug_and_console_gone", t_hid_unplug_and_console_gone },
    { "hid_report_parser", t_hid_report_parser },
    { "hid_mouse_report_protocol", t_hid_mouse_report_protocol },
    { "hid_mouse_report_ids", t_hid_mouse_report_ids },
    { "hid_mouse_boot_kept", t_hid_mouse_boot_kept },
    { "keymap_hid_codes", t_keymap_hid_codes },
    { "keymap_us_keys", t_keymap_us_keys },
    { "keymap_typing", t_keymap_typing },
    { "keymap_xkb_text", t_keymap_xkb_text },
    { "keymap_matches_hid", t_keymap_matches_hid },
    { "supervised_restart", t_supervised_restart },
    { "supervised_backoff", t_supervised_backoff },
    { "supervised_give_up", t_supervised_give_up },
    { "disk_mounts", t_disk_mounts },
    { "disk_not_boot", t_disk_not_boot },
    { "disk_other", t_disk_other },
    { "disk_fs_restart", t_disk_fs_restart },
    { "disk_vanishes", t_disk_vanishes },
    { "logd_writes_the_log", t_logd_writes_the_log },
    { "logd_without_data", t_logd_without_data },
    { "logd_data_goes_away", t_logd_data_goes_away },
    { "logd_kernel_log", t_logd_kernel_log },
    { "logd_openers", t_logd_openers },
    { "klog_lines", t_klog_lines },
    { "fat_format", t_fat_format },
    { "fat_files", t_fat_files },
    { "fat_dirs", t_fat_dirs },
    { "fat_names", t_fat_names },
    { "fat_names_shown", t_fat_names_shown },
    { "fat_full_disk", t_fat_full_disk },
    { "fat_read_only", t_fat_read_only },
    { "fat_not_formatted", t_fat_not_formatted },
    { "fat_format_off", t_fat_format_off },
    { "fat_dirty_volume", t_fat_dirty_volume },
    { "fat_disk_gone", t_fat_disk_gone },
    { "fat_gone_mounting", t_fat_gone_mounting },
    { "fat_dir_linear", t_fat_dir_linear },
    { "fat_dir_cursors", t_fat_dir_cursors },
    { "fat_cache", t_fat_cache },
    { "fat_gather", t_fat_gather },
    { "fat_truncate_steps", t_fat_truncate_steps },
    { "fat_ctl_queued", t_fat_ctl_queued },
    { "fat_restart_steps", t_fat_restart_steps },
    { "fat_restart_handles", t_fat_restart_handles },
    { "fat_restart_bad_request", t_fat_restart_bad_request },
    { "fat_layout", t_fat_layout },
    { "ns_boot_mount", t_ns_boot_mount },
    { "ns_boot_read_only", t_ns_boot_read_only },
    { "ns_path_rules", t_ns_path_rules },
    { "ns_mount_point_names", t_ns_mount_point_names },
    { "ns_read_write", t_ns_read_write },
    { "ns_server_dies", t_ns_server_dies },
    { "ns_child_sees_only_its_mounts", t_ns_child_sees_only_its_mounts },
    { "ns_mounts_reach_a_running_child", t_ns_mounts_reach_a_running_child },
    { "ns_malformed_messages", t_ns_malformed_messages },
    { "svc_publish_and_open", t_svc_publish_and_open },
    { "svc_connect", t_svc_connect },
    { "svc_child_gets_its_grants", t_svc_child_gets_its_grants },
    { "svc_child_gets_views", t_svc_child_gets_views },
    { "view_etc_names", t_view_etc_names },
    { "fat_views", t_fat_views },
    { "fat_view_limits", t_fat_view_limits },
    { "ns_changes_stay_bounded", t_ns_changes_stay_bounded },
    { "heap_reuses_freed_space", t_heap_reuses_freed_space },
    { "ns_fat_mount", t_ns_fat_mount },
    { "spawn_from_vmo", t_spawn_from_vmo },
    { "audio_formats", t_audio_formats },
    { "audio_resample", t_audio_resample },
    { "wav_parse", t_wav_parse },
    { "music_scan", t_music_scan },
    { "music_order", t_music_order },
    { "music_spectrum", t_music_spectrum },
    { "music_stereo", t_music_stereo },
    { "mp3_header", t_mp3_header },
    { "mp3_sniff", t_mp3_sniff },
    { "mp3_decode", t_mp3_decode },
    { "mix_gains", t_mix_gains },
    { "mix_unity_is_exact", t_mix_unity_is_exact },
    { "mix_volume_and_master", t_mix_volume_and_master },
    { "mix_dither", t_mix_dither },
    { "mix_limits", t_mix_limits },
    { "netframe_classify", t_netframe_classify },
    { "netframe_short_frames", t_netframe_short_frames },
    { "netframe_tag", t_netframe_tag },
    { "netframe_tag_refuses_tagged", t_netframe_tag_refuses_tagged },
    { "netframe_tx_check", t_netframe_tx_check },
    { "netframe_tag_copy_is_the_frame", t_netframe_tag_copy_is_the_frame },
    { "netframe_rx", t_netframe_rx },
    { "netframe_plain", t_netframe_plain },
    { "netframe_plain_refuses_tagged", t_netframe_plain_refuses_tagged },
    { "netframe_tx_check_plain", t_netframe_tx_check_plain },
    { "netframe_tx_modes", t_netframe_tx_modes },
    { "netframe_rx_plain", t_netframe_rx_plain },
    { "rtl8125_write_guard", t_rtl8125_write_guard },
    { "rtl8125_txq_guard", t_rtl8125_txq_guard },
    { "rtl8125_tx_gate", t_rtl8125_tx_gate },
    { "rtl8125_args", t_rtl8125_args },
    { "rtl8125_arp", t_rtl8125_arp },
    { "rtl8125_stays_off", t_rtl8125_stays_off },
    { "rtl8125_txdesc", t_rtl8125_txdesc },
    { "rtl8125_kick", t_rtl8125_kick },
    { "rtl8125_tx_verdict", t_rtl8125_tx_verdict },
    { "rtl8125_guard", t_rtl8125_guard },
    { "rtl8125_dump", t_rtl8125_dump },
    { "rtl8125_guard_fake", t_rtl8125_guard_fake },
    { "rtl8125_stats_nowait", t_rtl8125_stats_nowait },
    { "rtl8125_rxdesc", t_rtl8125_rxdesc },
    { "rtl8125_rxdesc_laps", t_rtl8125_rxdesc_laps },
    { "dhcp_build", t_dhcp_build },
    { "dhcp_parse_sample", t_dhcp_parse_sample },
    { "dhcp_parse_truncated", t_dhcp_parse_truncated },
    { "dhcp_parse_options", t_dhcp_parse_options },
    { "dhcp_parse_addresses", t_dhcp_parse_addresses },
    { "dhcp_overload", t_dhcp_overload },
    { "dhcp_fuzz", t_dhcp_fuzz },
    { "dhcpc_lease_cycle", t_dhcpc_lease_cycle },
    { "dhcpc_retransmit", t_dhcpc_retransmit },
    { "dhcpc_nak", t_dhcpc_nak },
    { "dhcpc_wrong_replies", t_dhcpc_wrong_replies },
    { "dhcpc_probe", t_dhcpc_probe },
    { "dhcpc_reboot", t_dhcpc_reboot },
    { "dhcpc_times", t_dhcpc_times },
    { "dhcpc_stop", t_dhcpc_stop },
    { "dhcpc_hostile", t_dhcpc_hostile },
    { "dns_names", t_dns_names },
    { "dns_parse_samples", t_dns_parse_samples },
    { "dns_parse_answers", t_dns_parse_answers },
    { "dns_names_hostile", t_dns_names_hostile },
    { "dns_parse_hostile", t_dns_parse_hostile },
    { "dns_fuzz", t_dns_fuzz },
    { "dnsres_basic", t_dnsres_basic },
    { "dnsres_retries", t_dnsres_retries },
    { "dnsres_slow_peer", t_dnsres_slow_peer },
    { "dnsres_cname", t_dnsres_cname },
    { "dnsres_failures", t_dnsres_failures },
    { "dnsres_ports", t_dnsres_ports },
    { "dnsres_cache", t_dnsres_cache },
    { "dnsres_hostile", t_dnsres_hostile },
    { "dnsres_shares", t_dnsres_shares },
    { "dnsd_sockets", t_dnsd_sockets },
    { "dnsd_shares", t_dnsd_shares },
    { "update_manifest", t_update_manifest },
    { "update_manifest_refusals", t_update_manifest_refusals },
    { "update_manifest_damage", t_update_manifest_damage },
    { "update_build_net", t_update_build_net },
    { "update_signature", t_update_signature },
    { "updwire_golden", t_updwire_golden },
    { "updwire_hostile", t_updwire_hostile },
    { "updfetch_clean", t_updfetch_clean },
    { "updfetch_lossy", t_updfetch_lossy },
    { "updfetch_snapshot_gone", t_updfetch_snapshot_gone },
    { "updfetch_failures", t_updfetch_failures },
    { "netlog_golden", t_netlog_golden },
    { "netlog_hostile", t_netlog_hostile },
    { "netlog_whole_log", t_netlog_whole_log },
    { "netlog_mac_away", t_netlog_mac_away },
    { "netlog_ring_dropped", t_netlog_ring_dropped },
    { "netlog_forged_acks", t_netlog_forged_acks },
    { "netlog_sender_restarted", t_netlog_sender_restarted },
    { "netlog_crash_stream", t_netlog_crash_stream },
    { "netlog_klog_source", t_netlog_klog_source },
    { "netserver_session", t_netserver_session },
    { "netserver_tx", t_netserver_tx },
    { "netserver_rx", t_netserver_rx },
    { "utf8_well_formed", t_utf8_well_formed },
    { "utf8_bad_pieces", t_utf8_bad_pieces },
    { "time_calendar", t_time_calendar },
    { "time_zones_switch", t_time_zones_switch },
    { "time_zones_local", t_time_zones_local },
    { "time_wallclock_calls", t_time_wallclock_calls },
    { "random_get", t_random_get },
    { "os_random", t_os_random },
    { "netdev_ring_counts", t_netdev_ring_counts },
    { "netdev_ring_one_thread", t_netdev_ring_one_thread },
    { "netdev_ring_exchange", t_netdev_ring_exchange },
    { "netdev_vlan_word", t_netdev_vlan_word },
    { "netdev_mode_words", t_netdev_mode_words },
    { "sockring_counts", t_sockring_counts },
    { "sockring_dgram", t_sockring_dgram },
    { "sockring_stream", t_sockring_stream },
    { "sockring_wake", t_sockring_wake },
    { "sockring_hostile", t_sockring_hostile },
    { "sockring_exchange", t_sockring_exchange },
    { "netwait_api", t_netwait_api },
    { "netwait_states", t_netwait_states },
    { "netwait_level", t_netwait_level },
    { "netwait_stress", t_netwait_stress },
    { "settings_parse", t_settings_parse },
    { "settings_edit", t_settings_edit },
    { "settings_file", t_settings_file },
    { "tasks_yield_and_wait", t_tasks_yield_and_wait },
    { "tasks_start_slots_cap", t_tasks_start_slots_cap },
    { "idl_answer_later", t_idl_answer_later },
    { "idl_later_blocking_clients", t_idl_later_blocking_clients },
    { "idl_later_handles", t_idl_later_handles },
    { "idl_async_through_port", t_idl_async_through_port },
    { "idl_serve_reply_wait", t_idl_serve_reply_wait },
    { "idl_serve_gone_client", t_idl_serve_gone_client },
    { "idl_within_times_out", t_idl_within_times_out },
    { "idl_slot_take_run", t_idl_slot_take_run },
    { "idl_slot_idempotent", t_idl_slot_idempotent },
    { "idl_slot_reply_after", t_idl_slot_reply_after },
    { "keep_put_drop_restore", t_keep_put_drop_restore },
    { "keep_restore_batches", t_keep_restore_batches },
    { "keep_unknown_slots", t_keep_unknown_slots },
    { "keep_refusals", t_keep_refusals },
    { "keep_restore_refusals", t_keep_restore_refusals },
    { "svcstate_fresh_and_adopted", t_svcstate_fresh_and_adopted },
    { "svcstate_slots", t_svcstate_slots },
    { "svcstate_answer_mark", t_svcstate_answer_mark },
    { "svcstate_refused", t_svcstate_refused },
    { "svcstate_standby", t_svcstate_standby },
    { "netstack_arp", t_netstack_arp },
    { "netstack_ping", t_netstack_ping },
    { "netstack_udp_unreachable", t_netstack_udp_unreachable },
    { "netstack_malformed", t_netstack_malformed },
    { "netstack_fuzz", t_netstack_fuzz },
    { "netstack_cleared", t_netstack_cleared },
    { "nettcp_connect", t_nettcp_connect },
    { "nettcp_listen", t_nettcp_listen },
    { "nettcp_slow_reader", t_nettcp_slow_reader },
    { "nettcp_reset", t_nettcp_reset },
    { "nettcp_syn_flood", t_nettcp_syn_flood },
    { "nettcp_pool_full", t_nettcp_pool_full },
    { "nettcp_malformed", t_nettcp_malformed },
    { "nettcp_fuzz", t_nettcp_fuzz },
    { "nettcp_hostile_ring", t_nettcp_hostile_ring },
    { "nettcp_card_full", t_nettcp_card_full },
    { "nettcp_window_scale", t_nettcp_window_scale },
    { "nettcp_listen_scale", t_nettcp_listen_scale },
    { "nettcp_heap_shares", t_nettcp_heap_shares },
    { "nettcp_ooseq_sack", t_nettcp_ooseq_sack },
    { "nettcp_ooseq_bounds", t_nettcp_ooseq_bounds },
    { "nettcp_limits", t_nettcp_limits },
    { "nettcp_bulk_rings", t_nettcp_bulk_rings },
    { "netctl_set_and_clear", t_netctl_set_and_clear },
    { "netctl_process", t_netctl_process },
    { "ipv4_text", t_ipv4_text },
    { "netdrv_ping_and_link", t_netdrv_ping_and_link },
    { "netdrv_link_flap", t_netdrv_link_flap },
    { "netdrv_restart", t_netdrv_restart },
    { "netdrv_hostile_driver", t_netdrv_hostile_driver },
    { "netsock_udp", t_netsock_udp },
    { "netsock_ping", t_netsock_ping },
    { "netsock_iface", t_netsock_iface },
    { "netsock_limits", t_netsock_limits },
    { "netsock_hostile", t_netsock_hostile },
    { "netsock_slow_reader", t_netsock_slow_reader },
    { "netsock_busy_client", t_netsock_busy_client },
    { "netsock_len_lies", t_netsock_len_lies },
    { "netsock_dhcp", t_netsock_dhcp },
    { "netsock_shares", t_netsock_shares },
    { "netsock_hostile_rings", t_netsock_hostile_rings },
    { "netwait_udp", t_netwait_udp },
    { "netsock_bench", t_netsock_bench },
    { "netlisten_udp", t_netlisten_udp },
    { "netlisten_low", t_netlisten_low },
    { "netlisten_wants", t_netlisten_wants },
    { "sntp_request_and_reply", t_sntp_request_and_reply },
    { "sntp_checks", t_sntp_checks },
    { "sntp_times", t_sntp_times },
    { "sntp_fuzz", t_sntp_fuzz },
    { "http_url", t_http_url },
    { "http_response", t_http_response },
    { "http_chunks", t_http_chunks },
    { "http_request", t_http_request },
    { "http_fuzz", t_http_fuzz },
    { "jwl_tables", t_jwl_tables },
    { "jwl_tables_generated", t_jwl_tables_generated },
    { "jwl_roundtrip", t_jwl_roundtrip },
    { "jwl_roundtrip_events", t_jwl_roundtrip_events },
    { "jwl_encode_limits", t_jwl_encode_limits },
    { "jwl_map_client_ids", t_jwl_map_client_ids },
    { "jwl_map_server_ids", t_jwl_map_server_ids },
    { "jwl_bad_framing", t_jwl_bad_framing },
    { "jwl_bad_strings", t_jwl_bad_strings },
    { "jwl_bad_objects", t_jwl_bad_objects },
    { "jwl_bad_bind", t_jwl_bad_bind },
    { "jwl_bad_events", t_jwl_bad_events },
    { "jwl_bad_batches", t_jwl_bad_batches },
    { "jwl_bad_compositor", t_jwl_bad_compositor },
    { "jwl_conn_messages", t_jwl_conn_messages },
    { "jwl_conn_batches", t_jwl_conn_batches },
    { "jwl_conn_window", t_jwl_conn_window },
    { "jwl_conn_never_reads", t_jwl_conn_never_reads },
    { "jwl_conn_send_refusals", t_jwl_conn_send_refusals },
    { "jwl_conn_new_ids", t_jwl_conn_new_ids },
    { "jwl_fuzz_decode", t_jwl_fuzz_decode },
    { "jwl_fuzz_conn", t_jwl_fuzz_conn },
    { "jwlc_setup", t_jwlc_setup },
    { "jwlc_window", t_jwlc_window },
    { "jwlc_window_sizes", t_jwlc_window_sizes },
    { "jwlc_keyboard", t_jwlc_keyboard },
    { "jwlc_pointer", t_jwlc_pointer },
    { "jwlc_reconnect", t_jwlc_reconnect },
    { "jwlc_protocol_error", t_jwlc_protocol_error },
    { "jwlc_blocking", t_jwlc_blocking },
    { "comp_globals", t_comp_globals },
    { "comp_surface", t_comp_surface },
    { "comp_bad_requests", t_comp_bad_requests },
    { "comp_caps", t_comp_caps },
    { "comp_connections", t_comp_connections },
    { "comp_client_crash", t_comp_client_crash },
    { "comp_never_reads", t_comp_never_reads },
    { "comp_regions", t_comp_regions },
    { "jwlc_real_compositor", t_jwlc_real_compositor },
    { "comp_paint_overlap", t_comp_paint_overlap },
    { "comp_paint_cull", t_comp_paint_cull },
    { "comp_paint_damage", t_comp_paint_damage },
    { "comp_paint_fullscreen", t_comp_paint_fullscreen },
    { "comp_paint_title", t_comp_paint_title },
    { "comp_paint_blank", t_comp_paint_blank },
};

int main(int argc, char **argv)
{
    if (argc >= 2)
        return child_main(argc, argv);
    unsigned n = sizeof(tests) / sizeof(tests[0]), passed = 0;
    for (unsigned i = 0; i < n; i++) {
        utest_cur = tests[i].name;
        uint64_t t0 = now();
        if (tests[i].fn()) {
            passed++;
            printf("utest: %s ok (%lu ms)\n", utest_cur, (unsigned long)((now() - t0) / NS_PER_MS));
        }
    }
    char line[96];
    int len = passed == n ? snprintf(line, sizeof(line), "utest: %u passed", passed)
                          : snprintf(line, sizeof(line), "utest: %u passed, %u FAILED", passed,
                                     n - passed);
    jam_debug_report(line, (uint64_t)len);
    return passed == n ? 0 : 1;
}
