/* The kernel test runner. ktest_run_opts runs every KTEST (collected by the
 * linker between __ktests_start and __ktests_end) whose name starts with a
 * prefix, and checks after each one that it gave back every page it took
 * (within LEAK_SLACK_PAGES).
 *
 * At boot ("ktest") every check is strict. From the shell on a live system
 * (ktest_live) the KT_GLOBAL_* checks are relaxed, since other processes
 * change the global counts, and tests marked KT_SKIP_LIVE are skipped.
 *
 * The options (struct ktest_opts, ktest.h) repeat the set, shuffle its
 * order from a seed, run it under load, and carry on past a failure:
 *
 *   order     Loop k runs in the order shuffled from seed + k - 1, which
 *             its first line prints: "ktest seed=<that>" replays it.
 *   failure   The panic screen's note (panic_note_set) names the loop, the
 *             seed, the test's place in the order and the tests before it,
 *             whatever kind of panic it is. With `keep`, each test runs in
 *             a thread of its own; a failed check records the failure
 *             (ktest_soak.c keeps the list) and ends that thread, and the
 *             run goes on. That is only possible where a thread can end:
 *             a check that fails with interrupts or preemption off still
 *             panics, and so does a test that never ends (KEEP_STUCK_S
 *             after a helper thread failed, KEEP_LIMIT_S in any case).
 *             What a failed test left behind (objects, threads) may break
 *             later tests: the first failure of a run is the finding.
 *   load      stress_load_start's threads and processes run meanwhile;
 *             ktest_busy and ktest_live are set (KT_NEEDS_IDLE, ktest.h). */
#include <jam/cmdline.h>
#include <jam/dbghook.h>
#include <jam/interrupt.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/report.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/selftest.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/timer.h>

#define RECENT        4     /* tests before the current one the panic note names */
#define KEEP_STUCK_S  10    /* keep: a test may run this long after a helper failed */
#define KEEP_LIMIT_S  600   /* keep: no test runs longer than this */
#define LOOPS_MAX     100000u
#define SEED_MAX      0xffffffffull

extern const struct ktest __ktests_start[], __ktests_end[];

const char *ktest_current = "?";
bool ktest_live;
bool ktest_busy;
unsigned ktest_relaxed;
unsigned ktest_idle_relaxed;
const char *ktest_skip_reason;
const char *ktest_skip_kind;
const char *ktest_own_leak_check;

/* The run in progress. Written by the runner's thread; ktest_fail reads it
 * from whichever thread a check failed in. */
static struct {
    struct ktest_opts o;          /* what was asked for */
    bool           running;       /* inside ktest_run_opts */
    uint32_t       loop;          /* 1 .. o.loops */
    uint64_t       seed;          /* this loop's (0: link order) */
    uint32_t       pos, total;    /* the current test is pos of total (from 1) */
    const char    *recent[RECENT]; /* the tests before it, newest first */
    struct thread *body;          /* keep: the thread the test's function runs in */
    bool           failed;        /* keep: a check of the current test failed (atomic) */
    bool           done;          /* keep: its function returned or failed (atomic) */
} rs;

static unsigned last_failed;   /* the last run's failures (ktest_last_failed) */

/* What one run adds up to. */
struct tally {
    int      ran, skipped, failed, drifted, relaxed_tests;
    unsigned relaxed, idle_relaxed;
};

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
    if (ktest_busy)
        return;   /* the load never settles, and no page count is judged under it */
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

/* ---- options ------------------------------------------------------------------ */

/* "key=123" at w (a word ending at a space or NUL): the number in *v. */
static bool word_num(const char *w, size_t n, const char *key, uint64_t *v)
{
    size_t kl = strlen(key);
    if (n <= kl || memcmp(w, key, kl))
        return false;
    uint64_t x = 0;
    for (size_t i = kl; i < n; i++) {
        if (w[i] < '0' || w[i] > '9' || x > SEED_MAX)
            return false;
        x = x * 10 + (uint64_t)(w[i] - '0');
    }
    *v = x;
    return true;
}

static bool word_is(const char *w, size_t n, const char *name)
{
    return strlen(name) == n && !memcmp(w, name, n);
}

/* A seed from the clock, for `shuffle`: never 0. */
static uint64_t clock_seed(void)
{
    uint64_t x = rdtsc() ^ (uptime_ns() << 7);
    x = (x ^ (x >> 32)) & SEED_MAX;
    return x ? x : 1;
}

/* One word into *o. false: a number out of range, or (shell) a second prefix. */
static bool parse_word(const char *w, size_t n, bool boot, struct ktest_opts *o, bool *prefixed)
{
    uint64_t v;
    if (n > 6 && !memcmp(w, "loops=", 6)) {
        if (!word_num(w, n, "loops=", &v) || v < 1 || v > LOOPS_MAX)
            return false;
        o->loops = (uint32_t)v;
    } else if (n > 5 && !memcmp(w, "seed=", 5)) {
        if (!word_num(w, n, "seed=", &v) || v < 1 || v > SEED_MAX)
            return false;
        o->seed = v;
    } else if (word_is(w, n, "shuffle")) {
        if (!o->seed)
            o->seed = clock_seed();
    } else if (word_is(w, n, "keep")) {
        o->keep = true;
    } else if (word_is(w, n, "load")) {
        o->load = cpu_count * 2;
    } else if (n > 5 && !memcmp(w, "load=", 5)) {
        if (!word_num(w, n, "load=", &v) || v < 2 || v > 1024)
            return false;
        o->load = (uint32_t)v;
    } else if (boot ? n > 6 && !memcmp(w, "ktest=", 6) : true) {
        const char *p = boot ? w + 6 : w;
        size_t pl = boot ? n - 6 : n;
        if (*prefixed || pl >= sizeof(o->prefix))
            return boot;   /* the command line: not ours to refuse */
        memcpy(o->prefix, p, pl);
        o->prefix[pl] = '\0';
        *prefixed = true;
    }
    return true;
}

bool ktest_parse_opts(const char *words, bool boot, struct ktest_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->loops = 1;
    bool prefixed = false;
    for (const char *p = words; *p;) {
        while (*p == ' ')
            p++;
        size_t n = 0;
        while (p[n] && p[n] != ' ')
            n++;
        if (n && !parse_word(p, n, boot, o, &prefixed)) {
            if (!boot)
                return false;
            /* The command line can't be refused: say so and run without. */
            kprintf("ktest: the word \"%.*s\" is not understood: left out\n", (int)n, p);
        }
        p += n;
    }
    return true;
}

/* ---- the order ------------------------------------------------------------------ */

static bool selected(const struct ktest *t, const char *prefix)
{
    size_t pl = strlen(prefix);
    if (memcmp(t->name, prefix, pl))
        return false;
    /* review_* tests are repros that fail until their bug is fixed:
     * only "ktest=review..." runs them. */
    return memcmp(t->name, "review_", 7) || (pl >= 6 && !memcmp(prefix, "review", 6));
}

/* The selected tests' indexes in link order; how many. order may be NULL. */
static uint32_t select_tests(const char *prefix, uint16_t *order)
{
    uint32_t n = 0;
    for (const struct ktest *t = __ktests_start; t < __ktests_end; t++) {
        if (!selected(t, prefix))
            continue;
        if (order)
            order[n] = (uint16_t)(t - __ktests_start);
        n++;
    }
    return n;
}

/* Fisher-Yates from seed: the same seed gives the same order for the same
 * set of tests (the same kernel and prefix). */
static void shuffle(uint16_t *order, uint32_t n, uint64_t seed)
{
    uint64_t state = (seed * 0x9e3779b97f4a7c15ull) | 1;
    for (int i = 0; i < 8; i++)
        kt_rng(&state);
    for (uint32_t i = n; i > 1; i--) {
        uint32_t j = (uint32_t)(kt_rng(&state) % i);
        uint16_t tmp = order[i - 1];
        order[i - 1] = order[j];
        order[j] = tmp;
    }
}

/* ---- failures -------------------------------------------------------------------- */

/* The panic screen's line: where the run is. */
static void note_position(const char *name)
{
    char before[160];
    size_t n = 0;
    before[0] = '\0';
    for (int i = 0; i < RECENT && rs.recent[i]; i++)
        n += (size_t)ksnprintf(before + n, sizeof(before) - n, "%s%s", i ? ", " : "",
                               rs.recent[i]);
    panic_note_set("ktest: loop %u of %u, seed %lu%s, test %u of %u: %s%s%s%s; before it: %s",
                   rs.loop, rs.o.loops, rs.seed, rs.seed ? "" : " (link order)", rs.pos,
                   rs.total, name, rs.o.load ? ", under load" : "",
                   ktest_live ? ", live" : "", rs.o.keep ? ", keep" : "",
                   before[0] ? before : "(the first)");
}

static void remember(const char *name)
{
    for (int i = RECENT - 1; i > 0; i--)
        rs.recent[i] = rs.recent[i - 1];
    rs.recent[0] = name;
}

/* keep: write the failure down. */
static void record_failure(const char *name, const char *msg)
{
    kprintf("ktest: FAILED %s: %s [loop %u, seed %lu, test %u of %u]\n", name, msg, rs.loop,
            rs.seed, rs.pos, rs.total);
    ktest_soak_note_fail(name, msg, rs.loop, rs.seed);
    __atomic_store_n(&rs.failed, true, __ATOMIC_RELEASE);
}

_Noreturn void ktest_fail(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (!rs.running || !rs.o.keep)
        panic("ktest %s: %s", ktest_current, msg);
    record_failure(ktest_current, msg);
    /* A thread can only end where it could be switched out. */
    if (!irqs_enabled() || percpu_preempt_count())
        panic("ktest %s: %s (failed with interrupts or preemption off: the run can't go on)",
              ktest_current, msg);
    if (current_thread() == rs.body)
        __atomic_store_n(&rs.done, true, __ATOMIC_RELEASE);
    thread_exit();
}

/* ---- one test --------------------------------------------------------------------- */

static void keep_body(void *arg)
{
    const struct ktest *t = arg;
    t->fn();
    __atomic_store_n(&rs.done, true, __ATOMIC_RELEASE);
}

/* keep: the test's function in a thread of its own, so a failed check can
 * end it (ktest_fail) and whatever it pinned or changed about its thread
 * goes with it. */
static void call_in_thread(const struct ktest *t)
{
    __atomic_store_n(&rs.failed, false, __ATOMIC_RELEASE);
    __atomic_store_n(&rs.done, false, __ATOMIC_RELEASE);
    uint64_t start = uptime_ns(), failed_at = 0;
    rs.body = thread_create("ktest", keep_body, (void *)t, PRIO_DEFAULT);
    while (!__atomic_load_n(&rs.done, __ATOMIC_ACQUIRE)) {
        thread_sleep_ns(200000);
        uint64_t now = uptime_ns();
        if (!failed_at && __atomic_load_n(&rs.failed, __ATOMIC_ACQUIRE))
            failed_at = now;
        if (failed_at && now - failed_at > KEEP_STUCK_S * 1000000000ull)
            panic("ktest %s: a check failed in a helper thread and the test never ended",
                  t->name);
        if (now - start > KEEP_LIMIT_S * 1000000000ull)
            panic("ktest %s: still running after %d s", t->name, KEEP_LIMIT_S);
    }
    thread_join(rs.body);
    rs.body = NULL;
    if (__atomic_load_n(&rs.failed, __ATOMIC_ACQUIRE))   /* a hook may point into the dead test */
        for (unsigned i = 0; i < DBG_N; i++)
            __atomic_store_n(&dbg_hooks[i], NULL, __ATOMIC_RELEASE);
}

/* The page check after a test that ended: true if it may count as passed. */
static bool check_pages(const struct ktest *t, uint64_t before, struct tally *ty)
{
    uint64_t after = accounted_pages();
    long leaked = (long)(before - after);
    if (leaked > LEAK_SLACK_PAGES && ktest_busy) {
        ty->drifted++;   /* the load takes and frees pages all the time: nothing to read */
        return true;
    }
    if (leaked > LEAK_SLACK_PAGES) {
        after = ktest_accounted_pages();
        leaked = (long)(before - after);
    }
    if (ktest_own_leak_check) {
        kprintf("ktest: %s: harness page check replaced by the test's own (%s); "
                "%ld page(s) kept\n", t->name, ktest_own_leak_check, leaked);
        leaked = 0;
    }
    if (leaked <= LEAK_SLACK_PAGES)
        return true;
    if (ktest_live) {
        /* Live, user space allocated at the same time: not a verdict. */
        kprintf("ktest: %s: %ld fewer free pages after it (live system: not checked)\n",
                t->name, leaked);
        ty->drifted++;
        return true;
    }
    if (!rs.o.keep)
        panic("ktest %s: leaked %ld pages (accounted %lu -> %lu)", t->name, leaked, before,
              after);
    char msg[96];
    ksnprintf(msg, sizeof(msg), "leaked %ld pages (accounted %lu -> %lu)", leaked, before, after);
    record_failure(t->name, msg);
    return false;
}

static void print_ok(const struct ktest *t, uint64_t us, struct tally *ty)
{
    if (ktest_relaxed || ktest_idle_relaxed) {
        if (ktest_idle_relaxed)
            kprintf("ktest: %-32s ok  %lu.%03lu ms (not made: %u global-count check(s) live, "
                    "%u idle-machine check(s) busy)\n", t->name, us / 1000, us % 1000,
                    ktest_relaxed, ktest_idle_relaxed);
        else
            kprintf("ktest: %-32s ok  %lu.%03lu ms (%u global-count check(s) not made live)\n",
                    t->name, us / 1000, us % 1000, ktest_relaxed);
        ty->relaxed += ktest_relaxed;
        ty->idle_relaxed += ktest_idle_relaxed;
        ty->relaxed_tests++;
    } else {
        kprintf("ktest: %-32s ok  %lu.%03lu ms\n", t->name, us / 1000, us % 1000);
    }
}

static void run_one(const struct ktest *t, struct tally *ty)
{
    ktest_current = t->name;
    ktest_relaxed = ktest_idle_relaxed = 0;
    ktest_skip_reason = NULL;
    ktest_own_leak_check = NULL;
    note_position(t->name);
    /* Account pages held by the reusable thread stack cache alongside free
     * pages: a stack just moves between the two, so free + cached is
     * conserved unless a test allocates a fresh stack (charged once) or
     * truly leaks. Measured after settling so lazily-reaped stacks of
     * threads the test already joined are back in the cache. */
    uint64_t accounted_before = accounted_pages();
    uint64_t t0 = uptime_ns();
    if (rs.o.keep)
        call_in_thread(t);
    else
        t->fn();
    uint64_t us = (uptime_ns() - t0) / 1000;
    remember(t->name);
    if (rs.o.keep && __atomic_load_n(&rs.failed, __ATOMIC_ACQUIRE)) {
        ty->failed++;
        return;
    }
    if (ktest_skip_reason) {
        kprintf("ktest: %-32s skipped (%s: %s)\n", t->name, ktest_skip_kind, ktest_skip_reason);
        ty->skipped++;
        ktest_soak_note_skip();
        return;
    }
    if (!check_pages(t, accounted_before, ty)) {
        ty->failed++;
        return;
    }
    print_ok(t, us, ty);
    ktest_soak_note_pass(t->name, us, rs.loop, rs.seed);
    ty->ran++;
}

/* ---- a run ------------------------------------------------------------------------ */

/* One loop over the tests in order[0..n). */
static void run_loop(uint16_t *order, uint32_t n, struct tally *ty)
{
    rs.seed = rs.o.seed ? rs.o.seed + rs.loop - 1 : 0;
    rs.total = n;
    select_tests(rs.o.prefix, order);
    if (rs.seed)
        shuffle(order, n, rs.seed);
    bool plain = rs.o.loops == 1 && !rs.seed && !rs.o.keep && !rs.o.load;
    if (rs.seed)
        kprintf("ktest: loop %u of %u: %u tests in the order of seed %lu (replay: ktest%s%s "
                "seed=%lu%s)\n", rs.loop, rs.o.loops, n, rs.seed, rs.o.prefix[0] ? " " : "",
                rs.o.prefix, rs.seed, rs.o.load ? " load" : "");
    else if (!plain)
        kprintf("ktest: loop %u of %u: %u tests in link order\n", rs.loop, rs.o.loops, n);
    struct tally before = *ty;
    uint64_t t0 = uptime_ns();
    for (uint32_t i = 0; i < n; i++) {
        rs.pos = i + 1;
        run_one(&__ktests_start[order[i]], ty);
    }
    ktest_soak_note_loop(rs.seed);
    if (!plain)
        kprintf("ktest: loop %u of %u done in %lu ms: %d passed, %d skipped, %d FAILED\n",
                rs.loop, rs.o.loops, (uptime_ns() - t0) / 1000000, ty->ran - before.ran,
                ty->skipped - before.skipped, ty->failed - before.failed);
}

static void report_run(const struct tally *ty, uint64_t load_failures)
{
    bool plain = rs.o.loops == 1 && !rs.o.seed && !rs.o.keep && !rs.o.load;
    if (!plain)
        report("ktest: %u loop(s)%s%s, seed %lu%s: %d passed, %d skipped, %d FAILED%s",
               rs.o.loops, rs.o.load ? " under load" : "", rs.o.keep ? ", keep" : "", rs.o.seed,
               rs.o.seed ? "" : " (link order)", ty->ran, ty->skipped, ty->failed,
               load_failures ? "; the load FAILED its own checks" : "");
    if (ktest_live)
        report("ktest (live system): %d passed, %d skipped; not checked: %u global counts "
               "(%d tests), %d page drifts%s", ty->ran, ty->skipped, ty->relaxed,
               ty->relaxed_tests, ty->drifted, ktest_busy ? " (busy: see the skips)" : "");
    else
        report("ktest: %d test(s) passed (%u of %u lock classes in use)", ty->ran,
               lockdep_class_count(), LOCKDEP_MAX_CLASSES);
}

int ktest_run_opts(const struct ktest_opts *o)
{
    struct tally ty = { 0 };
    uint32_t n = select_tests(o->prefix, NULL);
    uint16_t *order = kmalloc(sizeof(*order) * (n ? n : 1));
    if (!order) {
        report("ktest: no memory for the order of %u tests", n);
        return 0;
    }
    bool was_live = ktest_live;
    uint64_t load_failures = 0;
    rs.o = *o;
    memset(rs.recent, 0, sizeof(rs.recent));
    warm_singletons();
    if (o->load) {
        if (stress_load_start(o->load)) {
            /* The load makes channels, processes and pages of its own. */
            ktest_live = ktest_busy = true;
        } else {
            report("ktest: the load could not start: running without it");
            rs.o.load = 0;
        }
    }
    rs.running = true;
    for (rs.loop = 1; rs.loop <= rs.o.loops; rs.loop++)
        run_loop(order, n, &ty);
    rs.running = false;
    ktest_current = "?";
    panic_note_set("%s", "");
    if (rs.o.load) {
        load_failures = stress_load_stop();
        if (load_failures)
            ktest_soak_note_fail("(the load)", "stress workers failed their own checks: see "
                                 "the \"stress: FAILED\" lines", 0, rs.o.seed);
    }
    report_run(&ty, load_failures);
    last_failed = (unsigned)ty.failed + (load_failures ? 1 : 0);
    ktest_live = was_live;
    ktest_busy = false;
    kfree(order);
    return ty.ran;
}

unsigned ktest_last_failed(void)
{
    return last_failed;
}

int ktest_run(const char *prefix)
{
    struct ktest_opts o = { .loops = 1 };
    size_t n = strlen(prefix);
    if (n >= sizeof(o.prefix))
        n = sizeof(o.prefix) - 1;
    memcpy(o.prefix, prefix, n);
    return ktest_run_opts(&o);
}
