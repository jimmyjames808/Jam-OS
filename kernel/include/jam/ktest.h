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
 * name, expression and values.
 *
 * A test must pass on any run of a boot and in any order (struct
 * ktest_opts: loops, a shuffled order): it starts from state it sets up
 * itself and leaves nothing behind (no static left changed, no thread,
 * timer, hook or pin). The soak run (docs/TESTING.md) checks that. */
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

/* A check failed: panic "ktest <name>: <message>" (the panic screen adds
 * the loop, seed and position: panic_note), or, in a keep-going run
 * (ktest_opts.keep), record it and end the test there. */
_Noreturn void ktest_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define KT_ASSERT(cond)                                                          \
    do {                                                                         \
        if (!(cond))                                                             \
            ktest_fail("%s failed (%s:%d)", #cond, __FILE__, __LINE__);          \
    } while (0)

#define KT_EQ(a, b)                                                              \
    do {                                                                         \
        int64_t _a = (int64_t)(a), _b = (int64_t)(b);                            \
        if (_a != _b)                                                            \
            ktest_fail("%s == %s failed: %ld vs %ld (%s:%d)", #a, #b, _a, _b,    \
                       __FILE__, __LINE__);                                      \
    } while (0)

/* ktest from the shell (debug_command) runs while user space is live:
 * devmgr, USB enumeration, hot-plug and driver restarts allocate pages and
 * make channels and interrupt objects at the same time. So system-wide
 * counts can't be checked exactly there. The boot menu's ktest (nothing
 * else running) keeps every check strict.
 *
 *   KT_GLOBAL_EQ(a, b), KT_GLOBAL_ASSERT(cond)
 *       a check on a system-wide count (channel_live_count(),
 *       interrupt_live_count(), port_get_stats(), free pages, ...): made when
 *       ktest_live is false, else counted as "not checked live". The operands are
 *       evaluated either way (free_now() drains the per-CPU caches, and
 *       tests rely on that). Checks on the test's own objects stay KT_EQ.
 *   KT_SKIP_LIVE("why")
 *       at the top of a test whose whole point is a system-wide count, or
 *       that runs the machine out of memory: live, it returns at once and
 *       ktest_run prints "skipped (live system: why)".
 *
 * ktest_run's per-test page leak check works the same way: a panic at the
 * boot menu, a logged drift live.
 *
 * Under load (ktest_opts.load: the stress test's threads and processes run
 * next to the tests, and whatever user space does) the machine is busy as
 * well as live. A test about correctness must still pass then. One that
 * asserts exact timing, exact placement (which CPU a thread runs on next,
 * that a CPU is idle) or an exact system-wide count needs an idle machine:
 *
 *   KT_NEEDS_IDLE("why")
 *       at the top of such a test: busy, it returns at once and ktest_run
 *       prints "skipped (busy machine: why)". Never for a test that is
 *       merely slow under load.
 *   KT_IDLE_EQ(a, b), KT_IDLE_ASSERT(cond)
 *       one such check inside a test whose other checks hold under load:
 *       made when the machine is idle, else counted as not checked. */
extern bool ktest_live;
extern bool ktest_busy;                 /* background load is running (implies live) */
extern unsigned ktest_relaxed;          /* global checks not made, this test */
extern unsigned ktest_idle_relaxed;     /* idle-machine checks not made, this test */
extern const char *ktest_skip_reason;   /* set by KT_SKIP_LIVE and KT_NEEDS_IDLE */
extern const char *ktest_skip_kind;     /* "live system" or "busy machine" */
extern const char *ktest_own_leak_check; /* set by KT_OWN_LEAK_CHECK */
/* Free plus stack-cached pages once they stop changing (settled; at most
 * about 1 s): what the leak check compares. */
uint64_t ktest_accounted_pages(void);

#define KT_GLOBAL_EQ(a, b)                                                       \
    do {                                                                         \
        int64_t _ga = (int64_t)(a), _gb = (int64_t)(b);                          \
        if (ktest_live)                                                          \
            ktest_relaxed++;                                                     \
        else if (_ga != _gb)                                                     \
            ktest_fail("%s == %s failed: %ld vs %ld (%s:%d)", #a, #b, _ga, _gb,  \
                       __FILE__, __LINE__);                                      \
    } while (0)

#define KT_GLOBAL_ASSERT(cond)                                                   \
    do {                                                                         \
        bool _gc = (cond);                                                       \
        if (ktest_live)                                                          \
            ktest_relaxed++;                                                     \
        else if (!_gc)                                                           \
            ktest_fail("%s failed (%s:%d)", #cond, __FILE__, __LINE__);          \
    } while (0)

#define KT_IDLE_EQ(a, b)                                                         \
    do {                                                                         \
        int64_t _ia = (int64_t)(a), _ib = (int64_t)(b);                          \
        if (ktest_busy)                                                          \
            ktest_idle_relaxed++;                                                \
        else if (_ia != _ib)                                                     \
            ktest_fail("%s == %s failed: %ld vs %ld (%s:%d)", #a, #b, _ia, _ib,  \
                       __FILE__, __LINE__);                                      \
    } while (0)

#define KT_IDLE_ASSERT(cond)                                                     \
    do {                                                                         \
        bool _ic = (cond);                                                       \
        if (ktest_busy)                                                          \
            ktest_idle_relaxed++;                                                \
        else if (!_ic)                                                           \
            ktest_fail("%s failed (%s:%d)", #cond, __FILE__, __LINE__);          \
    } while (0)

/* KT_OWN_LEAK_CHECK("why"): the harness's per-test page check is replaced
 * by the test's own. For a test whose first run legitimately grows caches
 * to a high-water mark (one stress run touches every CPU's caches): the
 * test measures a later, identical round instead, which must give every
 * page back. The harness logs the reason and the unchecked difference. */
#define KT_OWN_LEAK_CHECK(why)                                                   \
    do {                                                                         \
        ktest_own_leak_check = (why);                                            \
    } while (0)

#define KT_SKIP_LIVE(why)                                                        \
    do {                                                                         \
        if (ktest_live) {                                                        \
            ktest_skip_reason = (why);                                           \
            ktest_skip_kind = "live system";                                     \
            return;                                                              \
        }                                                                        \
    } while (0)

#define KT_NEEDS_IDLE(why)                                                       \
    do {                                                                         \
        if (ktest_busy) {                                                        \
            ktest_skip_reason = (why);                                           \
            ktest_skip_kind = "busy machine";                                    \
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
/* The same once it stops changing (two readings 5 ms apart agree; at most
 * 0.5 s): for a test that compares exact free pages and must not count
 * what an earlier test is still giving back. */
uint64_t kt_free_pages_settled(void);
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

/* How to run the tests. The words are the same on the kernel command line
 * and after the shell's `ktest`:
 *     loops=N    the whole set N times in this boot (1..100000)
 *     seed=S     in an order shuffled from S (1..4294967295); loop k of a
 *                run uses S + k - 1, so "seed=<a loop's seed>" replays
 *                that loop alone
 *     shuffle    a seed picked from the clock (and printed)
 *     keep       a failed test is recorded and the run carries on, ending
 *                with a summary; without it the first failure panics
 *     load       with the stress test's threads and processes running
 *                (kernel/debug/stress.c: stress_load_start) */
struct ktest_opts {
    char     prefix[32];   /* run the tests whose names start with it */
    uint32_t loops;        /* at least 1 */
    uint64_t seed;         /* 0: link order, every loop */
    bool     keep;         /* record failures instead of panicking */
    bool     load;         /* run under background load */
};

/* Words into *o. boot: the kernel command line, where "ktest=<prefix>"
 * names the prefix and other words are not ours. Else the shell's
 * arguments: one word that is none of the above is the prefix; false if
 * there is a second one or a number is out of range. */
bool ktest_parse_opts(const char *words, bool boot, struct ktest_opts *o);
/* Run the tests o selects. Returns how many passed, over all loops. */
int ktest_run_opts(const struct ktest_opts *o);
/* How many tests of the last run failed (keep; without it the first
 * failure panicked), plus one if its load failed its own checks. */
unsigned ktest_last_failed(void);
/* The same with only a prefix ("" = all): once, in link order, a failure
 * panics. Returns how many ran (live: passed, not counting the skipped). */
int ktest_run(const char *prefix);

/* ---- the soak record (kernel/test/ktest_soak.c) --------------------------
 * What every ktest_run_opts since ktest_soak_begin added up to: the shell's
 * `soak` command (debug commands "soak begin" and "soak end ...") prints
 * it as one summary at the end. */
void ktest_soak_begin(struct job *scope);
/* Print the summary. args: "u=<runs>,<failed> io=<ops>,<failed>
 * [halt]": what the shell counted (utest runs; the file load's cycles).
 * Returns the number of failures of every kind; with `halt`, panics
 * instead if there is one. */
int64_t ktest_soak_end(struct job *scope, const char *args);
bool ktest_soak_args_ok(const char *args);
/* The runner's notes into the record. */
void ktest_soak_note_loop(uint64_t seed);
void ktest_soak_note_pass(const char *name, uint64_t us, uint32_t loop, uint64_t seed);
void ktest_soak_note_skip(void);
void ktest_soak_note_fail(const char *name, const char *msg, uint32_t loop, uint64_t seed);
