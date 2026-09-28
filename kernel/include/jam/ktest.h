/* In-kernel test registry. Any file can declare tests:
 *
 *     KTEST(channel_basic) {
 *         KT_ASSERT(x == 1);
 *         KT_EQ(status, OK);
 *     }
 *
 * Tests land in the .ktests section and all run, in link order, when
 * "ktest" is on the kernel command line ("ktest=chan" runs only tests whose
 * name starts with "chan"). They run in thread "main" after every CPU is
 * online and the scheduler is up. A failed assertion panics with the test
 * name, expression and values. */
#pragma once

#include <stdint.h>
#include <jam/panic.h>

struct ktest {
    const char *name;
    void (*fn)(void);
};

#define KTEST(name)                                                              \
    static void ktest_fn_##name(void);                                           \
    __attribute__((used, section(".ktests"), aligned(8)))                        \
    static const struct ktest ktest_entry_##name = { #name, ktest_fn_##name };   \
    static void ktest_fn_##name(void)

extern const char *ktest_current;

#define KT_ASSERT(cond)                                                          \
    do {                                                                         \
        if (!(cond))                                                             \
            panic("ktest %s: %s failed (%s:%d)", ktest_current, #cond, __FILE__, \
                  __LINE__);                                                     \
    } while (0)

#define KT_EQ(a, b)                                                              \
    do {                                                                         \
        int64_t _a = (int64_t)(a), _b = (int64_t)(b);                            \
        if (_a != _b)                                                            \
            panic("ktest %s: %s == %s failed: %ld vs %ld (%s:%d)", ktest_current, \
                  #a, #b, _a, _b, __FILE__, __LINE__);                           \
    } while (0)

/* Benchmarks ("bench" on the command line); results go to the RESULTS box. */
void bench_run(void);

/* Run registered tests whose name starts with `prefix` ("" = all).
 * Returns how many ran. */
int ktest_run(const char *prefix);
