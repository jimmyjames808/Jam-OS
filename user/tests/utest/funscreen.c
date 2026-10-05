/* utest: libfun's borrowed screen (user/apps/fun/gfx.c) against a fake
 * console of the test's own (gfx_console_with): it lends a 320 x 200
 * screen VMO and a lease, as the console's lend_screen does.
 *   fun_screen_close_frees  gfx_open_screen then gfx_close, three times:
 *                           the job's pages, handles and message bytes
 *                           are back where they began each time (the
 *                           back buffers and the screen's mapping go with
 *                           the close), and the lease's peer closed. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <idl/console.h>
#include <os.h>
#include "utest.h"

#define SCR_W 320u
#define SCR_H 200u
#define BG    0x204060u

/* The fake console: its server end, and its end of the last lease. */
struct fakecon {
    handle_t serve;
    handle_t lease;
};

static status_t fc_lend_screen(void *ctx, uint32_t *w, uint32_t *h, uint32_t *pitch, uint8_t *rs,
                               uint8_t *gs, uint8_t *bs, uint64_t *size, handle_t *screen,
                               handle_t *lease)
{
    struct fakecon *fc = ctx;
    uint64_t bytes = (uint64_t)SCR_W * SCR_H * 4;
    handle_t vmo, theirs;
    status_t st = jam_vmo_create(bytes, 0, HANDLE_INVALID, &vmo);
    if (st != OK)
        return st;
    st = jam_channel_create(&fc->lease, &theirs);
    if (st != OK) {
        jam_handle_close(vmo);
        return st;
    }
    *w = SCR_W;
    *h = SCR_H;
    *pitch = SCR_W * 4;
    *rs = 16;
    *gs = 8;
    *bs = 0;
    *size = bytes;
    *screen = vmo;
    *lease = theirs;
    return OK;
}

static const struct console_ops fc_ops = { .lend_screen = fc_lend_screen };

/* The fake's thread: one request, waited for at most 5 s. */
static void fc_serve_one(void *arg)
{
    struct fakecon *fc = arg;
    signals_t seen;
    if (jam_object_wait_one(fc->serve, SIG_READABLE, now() + 5 * NS_PER_S, &seen) == OK)
        (void)console_serve_one(fc->serve, &fc_ops, fc);   /* the client checks the answer */
}

static uint8_t fc_stack[16384] __attribute__((aligned(16)));

/* The job's pages, handles and message bytes now. */
static void held(uint64_t *pages, uint64_t *handles, uint64_t *bytes)
{
    struct job_info ji;
    *pages = *handles = *bytes = 0;
    if (info_of(own_job(), &ji) == OK) {
        *pages = ji.used[JOB_LIMIT_PAGES];
        *handles = ji.used[JOB_LIMIT_HANDLES];
        *bytes = ji.used[JOB_LIMIT_MSG_BYTES];
    }
}

/* One gfx_open_screen on the fake (served on a thread meanwhile), a
 * picture drawn and presented, and gfx_close. */
static bool open_draw_close(struct fakecon *fc)
{
    handle_t th;
    CHECK_ST(thread_spawn("fake console", fc_serve_one, fc, fc_stack, sizeof(fc_stack), &th), OK);
    status_t st = gfx_open_screen(BG);
    signals_t seen;
    (void)jam_object_wait_one(th, SIG_TERMINATED, now() + 10 * NS_PER_S, &seen);
    jam_handle_close(th);
    CHECK_ST(st, OK);
    CHECK(scr.open && !scr.windowed && scr.w == (int)SCR_W && scr.h == (int)SCR_H);
    fill(&scr.s, 10, 10, 100, 50, 0xff0000u);
    gfx_present();
    gfx_close();
    CHECK(!scr.open);
    /* The client's end of the lease went with the close. */
    CHECK_ST(jam_object_wait_one(fc->lease, SIG_PEER_CLOSED, now() + NS_PER_S, &seen), OK);
    jam_handle_close(fc->lease);
    fc->lease = HANDLE_INVALID;
    return true;
}

bool t_fun_screen_close_frees(void)
{
    pool_start(2);
    struct fakecon fc = { HANDLE_INVALID, HANDLE_INVALID };
    handle_t client;
    CHECK_ST(jam_channel_create(&client, &fc.serve), OK);
    gfx_console_with(client);
    /* The first round may grow what stays (the pool's, the heap's): the
     * later ones must leave nothing. */
    bool ok = open_draw_close(&fc);
    uint64_t p0, h0, b0;
    held(&p0, &h0, &b0);
    for (int i = 0; i < 3 && ok; i++) {
        ok = open_draw_close(&fc);
        uint64_t p1, h1, b1;
        held(&p1, &h1, &b1);
        if (ok && (p1 != p0 || h1 != h0 || b1 != b0)) {
            printf("utest %s: round %d: pages %lu -> %lu, handles %lu -> %lu, message bytes "
                   "%lu -> %lu\n", utest_cur, i + 2, p0, p1, h0, h1, b0, b1);
            ok = false;
        }
    }
    gfx_console_with(HANDLE_INVALID);
    jam_handle_close(client);
    jam_handle_close(fc.serve);
    return ok;
}
