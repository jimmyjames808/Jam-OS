/* The stress test (kernel/debug/stress.c) run twice in one boot, as the
 * shell's `stress` command can.
 *
 *   stress_failure_does_not_stick
 *       A run that fails (here a faked stale TLB shootdown read) is
 *       FAILED, and the next run, with nothing wrong, is PASSED: each run
 *       starts from zero failures. */
#include <jam/dbghook.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/selftest.h>

/* DBG_STRESS_SHOOTDOWN: one CPU "saw" a stale mapping. */
static void fake_stale_read(void *arg)
{
    __atomic_add_fetch((uint64_t *)arg, 1, __ATOMIC_RELAXED);
}

KTEST(stress_failure_does_not_stick)
{
    KT_SKIP_LIVE("the stress test wants the machine; the shell runs it with `stress`");
    kprintf("ktest %s: the next stress run FAILS on purpose (a faked stale read)\n",
            ktest_current);
    __atomic_store_n(&dbg_hooks[DBG_STRESS_SHOOTDOWN], fake_stale_read, __ATOMIC_RELEASE);
    bool first = stress_run(1);
    __atomic_store_n(&dbg_hooks[DBG_STRESS_SHOOTDOWN], NULL, __ATOMIC_RELEASE);
    KT_ASSERT(!first);
    KT_ASSERT(stress_run(1));
}
