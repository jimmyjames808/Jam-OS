/* User-side benchmarks for the kernel's "bench" entry (kernel/test/bench.c
 * starts these pinned to chosen CPUs and turns the results into the
 * RESULTS box lines; its header explains the method).
 *
 * "utest bench-<what>": time SAMPLES samples of <what> in ring 3 with the
 * TSC (lfence; rdtsc; lfence), after WARM_NS of untimed warm-up, and send
 * the raw cycle counts to the kernel on the SR_USER channel as one message
 * (struct ubench_result). Short operations are timed in batches of BATCH.
 *
 *   bench-null   a system call with an unused number: entry, the dispatch
 *                bounds check, exit (the floor under every call)
 *   bench-clock  clock_get
 *   bench-fault  a write to a fresh page of a mapped VMO: the page fault,
 *                committing a zeroed page, installing it, returning
 *   bench-call   channel_call to a bench-echo server on SR_USER + 1
 *   bench-echo   the server: read, write the same bytes back
 *   bench-rwecho the same server on channel_reply_wait: one system call
 *                per request (the reply to the last and the wait for the
 *                next), where bench-echo makes four
 *   bench-dcall  bench-call with a deadline on every call, made as libos
 *                makes one for a file call (now() + FS_CALL_TIMEOUT): the
 *                clock read and the kernel's sleeper queue are in the time
 *   bench-tcall  channel_call to an echo THREAD of this process on a
 *                channel of its own: the same work as bench-call without
 *                the address-space switches (both threads share our CR3),
 *                for the process->process breakdown in BENCH.md */
#include <os.h>
#include "utest.h"

#define SAMPLES 4000
#define BATCH   64
#define WARM_NS 20000000ull
#define NULL_SYSCALL 7   /* no call has this number (abi/syscalls.def) */

struct ubench_result {
    uint32_t txid;     /* 0 */
    uint32_t n;        /* samples */
    uint32_t batch;    /* operations per sample */
    uint32_t reserved;   /* 0 */
    uint64_t cycles[SAMPLES];   /* TSC cycles per sample */
};

static struct ubench_result res;

static int64_t null_syscall(void)
{
    int64_t r;
    /* The system call rule (<jam_syscalls.h>): the vector registers are
     * clobbered, as across any call. */
    __asm__ volatile("syscall" : "=a"(r) : "a"((uint64_t)NULL_SYSCALL)
                     : "rcx", "r11", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
                       "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                       "xmm14", "xmm15");
    return r;
}

static int send(uint32_t batch)
{
    res.n = SAMPLES;
    res.batch = batch;
    status_t st = jam_channel_write(startup_handle(SR_USER), &res, sizeof(res), NULL, 0);
    return st == OK ? 0 : 2;
}

static int b_null(void)
{
    if (null_syscall() != ERR_NOT_SUPPORTED)
        return 3;
    for (uint64_t end = now() + WARM_NS; now() < end;)
        null_syscall();
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = cpu_tsc();
        for (unsigned k = 0; k < BATCH; k++)
            null_syscall();
        res.cycles[i] = cpu_tsc() - t0;
    }
    return send(BATCH);
}

static int b_clock(void)
{
    for (uint64_t end = now() + WARM_NS; now() < end;)
        ;
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = cpu_tsc();
        for (unsigned k = 0; k < BATCH; k++)
            jam_clock_get();
        res.cycles[i] = cpu_tsc() - t0;
    }
    return send(BATCH);
}

static int b_fault(void)
{
    handle_t v, vmar = startup_handle(SR_SELF_VMAR);
    uint64_t pages = SAMPLES + 256, addr = 0;
    if (jam_vmo_create(pages * 4096, 0, HANDLE_INVALID, &v) != OK ||
        jam_vmar_map(vmar, v, 0, pages * 4096, VMAR_READ | VMAR_WRITE, &addr) != OK)
        return 3;
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)addr;
    for (unsigned i = 0; i < 256; i++)   /* warm-up: the last 256 pages */
        p[(uint64_t)(SAMPLES + i) * 4096] = 1;
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = cpu_tsc();
        p[(uint64_t)i * 4096] = 1;
        res.cycles[i] = cpu_tsc() - t0;
    }
    return send(1);
}

static bool call_deadline;   /* bench-dcall: every call gets a deadline */

static status_t call_once(handle_t ch)
{
    uint64_t req[2] = { 0, 42 }, rep[2];
    uint32_t n = 0;
    struct channel_call_args a = {
        .h = ch, .wn = sizeof(req), .wbytes = (uint64_t)(uintptr_t)req,
        .rcap = sizeof(rep), .rbytes = (uint64_t)(uintptr_t)rep,
        .ractual = (uint64_t)(uintptr_t)&n,
        .deadline_ns = call_deadline ? now() + FS_CALL_TIMEOUT : DEADLINE_NEVER,
    };
    return jam_channel_call(&a);
}

/* Calls on ch for WARM_NS, untimed. The clock is read once every 64
 * calls, so a warm-up call looks like a timed one to the kernel's path
 * trace (kernel/test/bench_path.c), which counts some of them. */
static void warm_calls(handle_t ch)
{
    uint64_t end = now() + WARM_NS;
    for (unsigned k = 0;; k++) {
        call_once(ch);
        if (k % 64 == 0 && now() >= end)
            return;
    }
}

static int b_call(void)
{
    handle_t ch = startup_handle(SR_USER + 1);
    if (call_once(ch) != OK)
        return 3;
    warm_calls(ch);
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = cpu_tsc();
        call_once(ch);
        res.cycles[i] = cpu_tsc() - t0;
    }
    return send(1);
}

/* Like child.c's echo, with a small buffer (the kernel's fast path). */
static int echo_on(handle_t ch)
{
    uint8_t buf[256];
    for (;;) {
        uint32_t nb = 0;
        struct channel_read_args a = {
            .h = ch, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&nb,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen;
            if (jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER,
                                    &seen) != OK)
                return 2;
            continue;
        }
        if (st != OK)
            return st == ERR_PEER_CLOSED ? 0 : 3;
        jam_channel_write(ch, buf, nb, NULL, 0);
    }
}

static int b_echo(void)
{
    return echo_on(startup_handle(SR_USER));
}

/* bench-echo's server on channel_reply_wait: each request is read straight
 * into buf, and goes back from buf in the next call (the reply's bytes are
 * copied in before the next request is copied out). */
static int b_rwecho(void)
{
    handle_t ch = startup_handle(SR_USER);
    uint8_t buf[256];
    uint32_t nb = 0;
    struct channel_reply_wait_args a = {
        .h = HANDLE_INVALID, .wait = ch, .bytes = (uint64_t)(uintptr_t)buf,
        .bytes_cap = sizeof(buf), .actual_bytes = (uint64_t)(uintptr_t)&nb,
        .deadline_ns = DEADLINE_NEVER,
    };
    for (;;) {
        status_t st = jam_channel_reply_wait(&a);
        if (st != OK)
            return st == ERR_PEER_CLOSED ? 0 : 3;
        a.h = ch;   /* from now on, answer the request just taken */
        a.rbytes = (uint64_t)(uintptr_t)buf;
        a.rn = nb;
    }
}

static uint8_t echo_stack[16384] __attribute__((aligned(16)));

static void echo_thread(void *arg)
{
    echo_on((handle_t)(uintptr_t)arg);
}

static int b_tcall(void)
{
    handle_t mine, theirs, t;
    if (jam_channel_create(&mine, &theirs) != OK)
        return 3;
    if (thread_spawn("bench-echo", echo_thread, (void *)(uintptr_t)theirs, echo_stack,
                     sizeof(echo_stack), &t) != OK)
        return 3;
    jam_thread_set_priority(t, THREAD_PRIO_USER_MAX);
    if (call_once(mine) != OK)
        return 3;
    warm_calls(mine);
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = cpu_tsc();
        call_once(mine);
        res.cycles[i] = cpu_tsc() - t0;
    }
    jam_handle_close(mine);   /* the echo thread sees PEER_CLOSED and returns */
    return send(1);
}

int bench_child(int argc, char **argv)
{
    (void)argc;
    /* The kernel's own benchmark threads run at 24: so do we. */
    jam_thread_set_priority(startup_handle(SR_SELF_THREAD), THREAD_PRIO_USER_MAX);
    const char *w = argv[1] + 6;
    if (!strcmp(w, "null"))  return b_null();
    if (!strcmp(w, "clock")) return b_clock();
    if (!strcmp(w, "fault")) return b_fault();
    if (!strcmp(w, "call"))  return b_call();
    if (!strcmp(w, "dcall")) {
        call_deadline = true;
        return b_call();
    }
    if (!strcmp(w, "echo"))  return b_echo();
    if (!strcmp(w, "rwecho")) return b_rwecho();
    if (!strcmp(w, "tcall")) return b_tcall();
    return 127;
}
