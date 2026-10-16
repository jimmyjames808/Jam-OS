/* utest: message budgets. What a program writes that nobody has read is
 * charged to its job and every job above it, up to the root's message
 * budget, which the whole system writes from; a flood must stop at the
 * program's own connection or job, never there (the compositor review's
 * D1). The other halves: cmds.txt's `run utest msgflood` (a program the
 * shell started stops at its own job's budget, <dataprog.h>'s
 * DP_JOB_MSG_BYTES) and execsvc.c's t_exec_sealed_runs (a /svc/exec
 * child's job has that budget too).
 *
 * t_msgbudget_comp_flood: a client writes valid batches flat out at the
 * compositor (bin/compositor headless, comptest.h), each 16 KiB of
 * wl_surface.damage requests, where the compositor takes 64 requests a
 * turn. Its end of the connection is CHANNEL_BYTE_BOUND, so the write that
 * finds 1 MiB waiting is ERR_SHOULD_WAIT, with at most that and one batch
 * waiting (never ERR_NO_MEMORY: the flood waits on its own connection).
 * Without the bound 1024 batches (16 MiB) wait, and four such clients fill
 * the root's budget. Meanwhile another client is served; then the
 * compositor works through the flood and the connection still round-trips. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <os.h>
#include "comptest.h"
#include "utest.h"

#define FLOOD_MAX CHANNEL_MAX_QUEUED   /* batches written at most: a channel's whole queue */

bool t_msgbudget_comp_flood(void)
{
    struct ct_comp p;
    CHECK(ct_start(&p, 320, 200));
    static struct ct_client good, k;
    CHECK(ct_open(&p, &good));
    CHECK(ct_bind_all(&good));
    CHECK(ct_open(&p, &k));
    CHECK(ct_bind_all(&k));
    uint32_t s = ct_new(&k, &jwl_wl_surface_interface, 4);
    CHECK(s);
    CHECK_ST(jwl_wl_compositor_create_surface(k.c, k.compositor, s), OK);
    CHECK_ST(ct_roundtrip(&k), OK);
    /* One batch: its header, then as many damage requests as fit. */
    static uint32_t b[JWL_BATCH_MAX / 4];
    unsigned n = 4;
    b[0] = JWL_MAGIC;
    b[1] = b[2] = b[3] = 0;   /* no handles */
    while (n + 6 <= JWL_BATCH_MAX / 4) {
        const uint32_t m[6] = { s, 24u << 16 | JWL_WL_SURFACE_REQ_DAMAGE, 0, 0, 1, 1 };
        for (unsigned i = 0; i < 6; i++)
            b[n++] = m[i];
    }
    unsigned written = 0;
    uint64_t peak = 0;
    status_t st = OK;
    while (written < FLOOD_MAX && (st = jam_channel_write(k.c->ch, b, n * 4, NULL, 0)) == OK) {
        written++;
        struct handle_info hi;
        if (jam_handle_info(k.c->ch, &hi) == OK && hi.peer_queued_bytes > peak)
            peak = hi.peer_queued_bytes;
    }
    printf("utest: %s: %u batches of %u bytes written, then %s; at most %lu bytes waiting\n",
           utest_cur, written, n * 4, written == FLOOD_MAX ? "no more" : status_str(st),
           (unsigned long)peak);
    CHECK_ST(st, ERR_SHOULD_WAIT);   /* full at the bound, before 1024 batches */
    CHECK(written < FLOOD_MAX);
    CHECK(peak <= CHANNEL_BOUND_BYTES + JWL_BATCH_MAX);
    CHECK_ST(ct_roundtrip(&good), OK);   /* served meanwhile */
    /* Room again once the compositor has read below the bound; then our
     * own sync is answered after everything before it. */
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(k.c->ch, SIG_WRITABLE, now() + CT_WAIT, &seen), OK);
    CHECK_ST(ct_roundtrip(&k), OK);
    CHECK(!k.errored);
    ct_close(&k);
    ct_close(&good);
    CHECK(ct_stop(&p));
    return true;
}
