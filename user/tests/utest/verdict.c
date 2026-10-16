/* utest: the canary for the shell tests' verdicts (tools/verdict-test.sh,
 * tools/shell-tests/verdict.txt). A shell script's `utest only <name>` is
 * judged by tools/serial-feed.py, which fails the script on a utest that
 * reports a failure or never reports at all; this is a test that can be
 * made to do either, so the script can show that it does.
 *
 * t_verdict_canary: passes. If the shell exported UTEST_CANARY it does
 * what that says instead, once:
 *     fail     a check fails (the summary says "1 FAILED")
 *     crash    a NULL write: the kernel kills utest, no summary comes
 * Without it (every normal run) it only reads the environment. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

static const char *canary(void)
{
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, "UTEST_CANARY=", 13))
            return *e + 13;
    return NULL;
}

bool t_verdict_canary(void)
{
    const char *what = canary();
    if (!what)
        return true;
    if (!strcmp(what, "crash"))
        *(volatile int *)0 = 1;
    CHECK(strcmp(what, "fail") != 0);   /* the failure the script asked for */
    return true;
}
