/* utest: libfun without a compositor (user/apps/fun/gfx.c): no window,
 * and no screen borrowed instead (M12's Q14: the console lends none).
 *   fun_no_window  gfx_open_on through a connect that never reaches a
 *                  compositor (libjwl tries again until its deadline):
 *                  it fails with the window's error (ERR_TIMED_OUT), not
 *                  with a screen from the console; scr stays closed, the
 *                  job's handles and message bytes are where they began,
 *                  and gfx_present and gfx_close after it do nothing. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <os.h>
#include "utest.h"

/* A libjwl connect function that never finds the compositor. */
static status_t refuse(void *ctx, handle_t *out)
{
    (void)ctx;
    *out = HANDLE_INVALID;
    return ERR_NOT_FOUND;
}

/* The job's handles and message bytes now. */
static void held(uint64_t *handles, uint64_t *bytes)
{
    struct job_info ji;
    *handles = *bytes = 0;
    if (info_of(own_job(), &ji) == OK) {
        *handles = ji.used[JOB_LIMIT_HANDLES];
        *bytes = ji.used[JOB_LIMIT_MSG_BYTES];
    }
}

bool t_fun_no_window(void)
{
    uint64_t h0, b0, h1, b1;
    held(&h0, &b0);
    gfx_connect_with(refuse, NULL);
    status_t st = gfx_open_on(0x204060u);
    gfx_connect_with(NULL, NULL);
    CHECK_ST(st, ERR_TIMED_OUT);
    CHECK(!scr.open);
    gfx_present();   /* nothing to present to */
    gfx_close();     /* nothing to close */
    CHECK(!scr.open);
    held(&h1, &b1);
    if (h1 != h0 || b1 != b0) {
        printf("utest %s: handles %lu -> %lu, message bytes %lu -> %lu\n", utest_cur, h0, h1,
               b0, b1);
        return false;
    }
    return true;
}
