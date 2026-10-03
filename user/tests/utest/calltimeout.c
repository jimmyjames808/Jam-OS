/* utest: channel_call with CHANNEL_CALL_TIMEOUT (<jam/abi.h>): deadline_ns
 * is a timeout from the start of the call, which the kernel turns into a
 * deadline itself. A call nobody answers times out after it, not before;
 * a slow answer inside it arrives; UINT64_MAX is still forever (the sum
 * must not wrap into the past); 0 times out at once; any other flag bit
 * is refused before anything is sent. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define SLOW_NS  (30 * NS_PER_MS)   /* the slow server's delay */
#define SHORT_NS (20 * NS_PER_MS)   /* a timeout nobody beats */

/* Call on h with this timeout (flags: CHANNEL_CALL_TIMEOUT unless told). */
static status_t call_within(handle_t h, uint32_t flags, uint64_t timeout_ns)
{
    uint32_t req[2] = { 0, 77 }, rep[2] = { 0 }, ra = 0;
    struct channel_call_args a = {
        .h = h, .wn = sizeof(req), .wbytes = (uint64_t)(uintptr_t)req,
        .rcap = sizeof(rep), .rbytes = (uint64_t)(uintptr_t)rep,
        .ractual = (uint64_t)(uintptr_t)&ra, .flags = flags, .deadline_ns = timeout_ns,
    };
    status_t st = jam_channel_call(&a);
    if (st == OK && (ra != sizeof(rep) || rep[1] != 77))
        return ERR_BAD_STATE;   /* not our reply */
    return st;
}

/* Answers one request on the end it is given, SLOW_NS after it arrives. */
static void slow_server(void *arg)
{
    handle_t h = (handle_t)(uintptr_t)arg;
    signals_t seen;
    if (jam_object_wait_one(h, SIG_READABLE, now() + 10 * NS_PER_S, &seen) != OK)
        return;
    jam_nanosleep(now() + SLOW_NS);
    uint32_t req[2], n = 0;
    struct channel_read_args r = {
        .h = h, .bytes_cap = sizeof(req), .bytes = (uint64_t)(uintptr_t)req,
        .actual_bytes = (uint64_t)(uintptr_t)&n,
    };
    if (jam_channel_read(&r) == OK && n == sizeof(req))
        (void)jam_channel_write(h, req, n, NULL, 0);   /* a lost reply fails the caller */
}

/* One slow answer to a call with this timeout. */
static bool slow_answer(handle_t a, handle_t b, uint64_t timeout_ns)
{
    static uint8_t stack[16384] __attribute__((aligned(64)));
    handle_t th;
    CHECK_ST(thread_spawn("slow server", slow_server, (void *)(uintptr_t)b, stack,
                          sizeof(stack), &th),
             OK);
    status_t st = call_within(a, CHANNEL_CALL_TIMEOUT, timeout_ns);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(st, OK);
    return true;
}

/* Nothing queued on h: the refused or timed-out calls left nothing behind
 * but what they sent. Reads and drops up to `want` requests. */
static bool drain(handle_t h, unsigned want)
{
    for (unsigned i = 0; i < want; i++) {
        uint32_t req[2], n = 0;
        struct channel_read_args r = {
            .h = h, .bytes_cap = sizeof(req), .bytes = (uint64_t)(uintptr_t)req,
            .actual_bytes = (uint64_t)(uintptr_t)&n,
        };
        CHECK_ST(jam_channel_read(&r), OK);
    }
    uint32_t n = 0;
    struct channel_read_args r = { .h = h, .actual_bytes = (uint64_t)(uintptr_t)&n };
    CHECK_ST(jam_channel_read(&r), ERR_SHOULD_WAIT);
    return true;
}

bool t_call_timeout_relative(void)
{
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    /* Bad flags: refused, nothing sent. */
    CHECK_ST(call_within(a, 1u << 1, SHORT_NS), ERR_INVALID_ARGS);
    CHECK_ST(call_within(a, CHANNEL_CALL_TIMEOUT | 1u << 31, SHORT_NS), ERR_INVALID_ARGS);
    if (!drain(b, 0))
        return false;
    /* Nobody answers: it times out after the timeout, not before. */
    uint64_t t0 = now();
    CHECK_ST(call_within(a, CHANNEL_CALL_TIMEOUT, SHORT_NS), ERR_TIMED_OUT);
    uint64_t took = now() - t0;
    if (took < SHORT_NS || took > 5 * NS_PER_S)
        FAIL("a %lu ns timeout took %lu ns", (unsigned long)SHORT_NS,
             (unsigned long)took);
    CHECK_ST(call_within(a, CHANNEL_CALL_TIMEOUT, 0), ERR_TIMED_OUT);
    if (!drain(b, 2))
        return false;
    /* A slow answer inside the timeout, and with "forever". */
    if (!slow_answer(a, b, 5 * NS_PER_S) || !slow_answer(a, b, DEADLINE_NEVER))
        return false;
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
    printf("utest: call timeout: %lu ns timeout took %lu ns\n", (unsigned long)SHORT_NS,
           (unsigned long)took);
    return true;
}
