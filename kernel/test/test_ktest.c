/* The test runner's own failure paths (kernel/test/ktest.c), failing on
 * purpose. Named review_... so that only "ktest=review_ktest" runs them:
 *
 *   ktest=review_ktest keep    two FAILED lines (one from the test's own
 *                              thread, one from a helper thread), the third
 *                              test still runs and passes, and the run's
 *                              report says "1 passed, 0 skipped, 2 FAILED"
 *   ktest=review_ktest         the first failure panics, and the panic
 *                              screen's note names loop, seed and test
 *
 * tools/ktest-keep-test.sh checks both. */
#include <jam/ktest.h>
#include <jam/sched.h>

KTEST(review_ktest_fails_in_its_thread)
{
    KT_EQ(1 + 1, 3);
}

static void failing_helper(void *arg)
{
    KT_ASSERT(arg != NULL);
}

KTEST(review_ktest_fails_in_a_helper)
{
    struct thread *t = thread_create("kt-fails", failing_helper, NULL, PRIO_DEFAULT);
    thread_join(t);
}

KTEST(review_ktest_passes_after_them)
{
    KT_EQ(1 + 1, 2);
}
