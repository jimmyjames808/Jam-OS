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

#include <stdbool.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/panic.h>

struct ktest {
    const char *name;   /* the KTEST name; ktest=<prefix> selects by it */
    void (*fn)(void);   /* the test body */
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

/* ktest from the shell (debug_command) runs while user space is live:
 * devmgr, USB enumeration, hot-plug and driver restarts allocate pages and
 * make channels and interrupt objects at the same time. So system-wide
 * counts can't be checked exactly there. The boot menu's ktest (nothing
 * else running) keeps every check strict.
 *
 *   KT_GLOBAL_EQ(a, b), KT_GLOBAL_ASSERT(cond)
 *       a check on a system-wide count (channel_live_count(),
 *       interrupt_live_count(), free pages, ...): made when ktest_live is
 *       false, else counted as "not checked live". The operands are
 *       evaluated either way (free_now() drains the per-CPU caches, and
 *       tests rely on that). Checks on the test's own objects stay KT_EQ.
 *   KT_SKIP_LIVE("why")
 *       at the top of a test whose whole point is a system-wide count, or
 *       that runs the machine out of memory: live, it returns at once and
 *       ktest_run prints "skipped (live system: why)".
 *
 * ktest_run's per-test page leak check works the same way: a panic at the
 * boot menu, a logged drift live. */
extern bool ktest_live;
extern unsigned ktest_relaxed;          /* global checks not made, this test */
extern const char *ktest_skip_reason;   /* set by KT_SKIP_LIVE */

#define KT_GLOBAL_EQ(a, b)                                                       \
    do {                                                                         \
        int64_t _ga = (int64_t)(a), _gb = (int64_t)(b);                          \
        if (ktest_live)                                                          \
            ktest_relaxed++;                                                     \
        else if (_ga != _gb)                                                     \
            panic("ktest %s: %s == %s failed: %ld vs %ld (%s:%d)", ktest_current, \
                  #a, #b, _ga, _gb, __FILE__, __LINE__);                         \
    } while (0)

#define KT_GLOBAL_ASSERT(cond)                                                   \
    do {                                                                         \
        bool _gc = (cond);                                                       \
        if (ktest_live)                                                          \
            ktest_relaxed++;                                                     \
        else if (!_gc)                                                           \
            panic("ktest %s: %s failed (%s:%d)", ktest_current, #cond, __FILE__, \
                  __LINE__);                                                     \
    } while (0)

#define KT_SKIP_LIVE(why)                                                        \
    do {                                                                         \
        if (ktest_live) {                                                        \
            ktest_skip_reason = (why);                                           \
            return;                                                              \
        }                                                                        \
    } while (0)

/* ---- helpers shared by the tests (kernel/test/ktest_util.c) ------------- */

struct handle_table;
struct job;
struct kobject;
struct pci_dev;

/* Free pages now (pmm_stats: drains the per-CPU caches first). */
uint64_t kt_free_pages(void);
/* Free pages plus the pages parked in the thread stack cache, as the
 * per-test leak check counts them: a helper thread's stack just moves
 * between the two. */
uint64_t kt_free_and_cached_pages(void);
/* The CPU the caller runs on (it may move right after). */
uint32_t kt_cur_cpu(void);
/* Pin the current thread to `cpu` (it moves there before this returns);
 * returns cpu. kt_unpin_self lets it run anywhere again: every test that
 * pins must unpin, or whatever runs next stays pinned. */
uint32_t kt_pin_self(uint32_t cpu);
void kt_unpin_self(void);
/* The tests' only direct access to a user address: read va in the address
 * space loaded on this CPU (the caller loaded it and knows the page is
 * mapped), with stac/clac around it when the CPU has SMAP. */
uint64_t kt_user_peek(uint64_t va);
/* xorshift64: the next number of the sequence *state (never 0) holds. */
uint64_t kt_rng(uint64_t *state);
/* A new job under the root job; the caller holds the only reference. */
struct job *kt_fresh_job(void);
/* Panic unless j is charged for nothing at all. */
void kt_job_is_empty(struct job *j);
/* The signals of the object behind handle h. */
signals_t kt_signals_of(struct handle_table *t, handle_t h);
/* A RES_PCI_DEV resource for d (a new reference). */
struct kobject *kt_pci_dev_res(struct pci_dev *d);

/* Benchmarks ("bench" on the command line); results go to the RESULTS box. */
void bench_run(void);

/* Run registered tests whose name starts with `prefix` ("" = all).
 * Returns how many ran (live: passed, not counting the skipped ones). */
int ktest_run(const char *prefix);
