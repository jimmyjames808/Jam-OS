/* The kernel test runner. ktest_run runs every KTEST (collected by the
 * linker between __ktests_start and __ktests_end) whose name starts with a
 * prefix, and checks after each one that it gave back every page it took
 * (within LEAK_SLACK_PAGES).
 *
 * At boot ("ktest") every check is strict. From the shell on a live system
 * (ktest_live) the KT_GLOBAL_* checks are relaxed, since other processes
 * change the global counts, and tests marked KT_SKIP_LIVE are skipped. */
#include <jam/interrupt.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/report.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/timer.h>

extern const struct ktest __ktests_start[], __ktests_end[];

const char *ktest_current = "?";
bool ktest_live;
unsigned ktest_relaxed;
const char *ktest_skip_reason;
const char *ktest_own_leak_check;

/* A test may leave a slab partially filled (a page held until the last
 * object on that slab is freed), so allow a small slack before calling it a
 * leak. Measured 2026-09-29 over the whole suite at 4 and 8 CPUs: no test
 * drifts by more than 1 page. A leaked 64 KiB buffer is 16 pages. */
#define LEAK_SLACK_PAGES 2

/* Force one-time lazily-created singletons into existence before the baseline,
 * so their allocation isn't charged to whichever test first happens to use
 * them. The timer service thread's stack is allocated on first timer use and
 * never freed; so are the device vector tables (irq.c) on the first vector_alloc. */
static void warm_vector(void *ctx)
{
    (void)ctx;
}

static void warm_singletons(void)
{
    struct ktimer *tm;
    if (timer_create(&tm) != OK)
        return;
    timer_set(tm, uptime_ns() + 1000000000ull);
    timer_cancel(tm);
    kobject_unref(&tm->base);
    /* The per-CPU device vector tables, made on the first vector_alloc. */
    uint32_t cpu;
    uint8_t vec;
    if (vector_alloc(warm_vector, NULL, &cpu, &vec) == OK)
        vector_free(cpu, vec);
    /* The DMA quarantine's thread, started by the first bound dma_cap. */
    dma_quarantine_start();
}

static uint64_t free_pages_now(uint64_t *total)
{
    uint64_t free;
    pmm_stats(total, &free);
    return free;
}

/* Let lazily-reaped stacks (a thread reaped on its CPU's next switch, after
 * the joiner already woke) return to the cache before we measure. */
static void settle(void)
{
    for (int i = 0; i < 4; i++) {
        thread_sleep_ms(2);
        thread_yield();
    }
}

/* Free pages plus the thread stack cache, after settle(). */
static uint64_t accounted_pages(void)
{
    uint64_t total;
    settle();
    return free_pages_now(&total) + sched_stack_cache_pages();
}

/* The same, once it stops moving: keep reading until two readings 10 ms
 * apart agree (at most 1 s). Used only when a test looks like it leaked:
 * on a machine with many CPUs, work the test started elsewhere (a thread or
 * process reaped on another CPU's next switch) can finish after settle()'s
 * 8 ms. A real leak never comes back, so waiting for it can't hide one. */
uint64_t ktest_accounted_pages(void)
{
    uint64_t total;
    uint64_t prev = accounted_pages();
    for (int i = 0; i < 100; i++) {
        thread_sleep_ms(10);
        uint64_t now = free_pages_now(&total) + sched_stack_cache_pages();
        if (now == prev)
            break;
        prev = now;
    }
    return prev;
}

int ktest_run(const char *prefix)
{
    size_t pl = strlen(prefix);
    int ran = 0, skipped = 0, drifted = 0, relaxed_tests = 0;
    unsigned relaxed = 0;
    warm_singletons();
    for (const struct ktest *t = __ktests_start; t < __ktests_end; t++) {
        if (memcmp(t->name, prefix, pl))
            continue;
        /* review_* tests are repros that fail until their bug is fixed:
         * only "ktest=review..." runs them. */
        if (!memcmp(t->name, "review_", 7) && (pl < 6 || memcmp(prefix, "review", 6)))
            continue;
        ktest_current = t->name;
        ktest_relaxed = 0;
        ktest_skip_reason = NULL;
        ktest_own_leak_check = NULL;
        /* Account pages held by the reusable thread stack cache alongside free
         * pages: a stack just moves between the two, so free + cached is
         * conserved unless a test allocates a fresh stack (charged once) or
         * truly leaks. Measured after settling so lazily-reaped stacks of
         * threads the test already joined are back in the cache. */
        uint64_t accounted_before = accounted_pages();
        uint64_t t0 = uptime_ns();
        t->fn();
        uint64_t us = (uptime_ns() - t0) / 1000;
        if (ktest_skip_reason) {
            kprintf("ktest: %-32s skipped (live system: %s)\n", t->name, ktest_skip_reason);
            skipped++;
            continue;
        }
        uint64_t accounted_after = accounted_pages();
        long leaked = (long)(accounted_before - accounted_after);
        if (leaked > LEAK_SLACK_PAGES) {
            accounted_after = ktest_accounted_pages();
            leaked = (long)(accounted_before - accounted_after);
        }
        if (ktest_own_leak_check) {
            kprintf("ktest: %s: harness page check replaced by the test's own (%s); "
                    "%ld page(s) kept\n", t->name, ktest_own_leak_check, leaked);
            leaked = 0;
        }
        if (leaked > LEAK_SLACK_PAGES) {
            if (!ktest_live)
                panic("ktest %s: leaked %ld pages (accounted %lu -> %lu)", t->name, leaked,
                      accounted_before, accounted_after);
            /* Live, user space allocated at the same time: not a verdict. */
            kprintf("ktest: %s: %ld fewer free pages after it (live system: not checked)\n",
                    t->name, leaked);
            drifted++;
        }
        if (ktest_relaxed) {
            kprintf("ktest: %-32s ok  %lu.%03lu ms (%u global-count check(s) not made live)\n",
                    t->name, us / 1000, us % 1000, ktest_relaxed);
            relaxed += ktest_relaxed;
            relaxed_tests++;
        } else {
            kprintf("ktest: %-32s ok  %lu.%03lu ms\n", t->name, us / 1000, us % 1000);
        }
        ran++;
    }
    ktest_current = "?";
    if (ktest_live)
        report("ktest (live system): %d passed, %d skipped; not checked: %u global counts "
               "(%d tests), %d page drifts", ran, skipped, relaxed, relaxed_tests, drifted);
    else
        report("ktest: %d test(s) passed (%u of %u lock classes in use)", ran,
               lockdep_class_count(), LOCKDEP_MAX_CLASSES);
    return ran;
}
