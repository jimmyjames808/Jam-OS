/* The soak record: what the kernel test runs of one soak added up to, and
 * the summary the shell's `soak` command ends with.
 *
 * "soak begin" (a debug command) clears the record and notes what the
 * system holds: free pages, channels, interrupt objects, and what the
 * caller's job tree is charged for. Every ktest_run_opts after it adds its
 * loops, its passed, skipped and failed tests, the failures themselves
 * (the first FAILS_KEPT of them, with loop and seed) and the slowest tests
 * (ktest.c calls the ktest_soak_note_* functions). "soak end" prints it
 * all in one box, with the counts the shell brings (utest runs, the file
 * load's cycles) and the same holdings again.
 *
 * The holdings are printed, not judged: on a live system drivers, mounts
 * and the log move them. A steady climb from one soak to the next is what
 * to look for. */
#include <jam/channel.h>
#include <jam/interrupt_test.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>

#define FAILS_KEPT 8
#define SLOW_KEPT  5

struct fail {
    const char *name;      /* the test (a KTEST name: static) */
    char        msg[200];  /* the failed check, with file and line */
    uint32_t    loop;      /* of its run; 0: not a test (the load) */
    uint64_t    seed;      /* that loop's seed */
};

struct slow {
    const char *name;      /* the test */
    uint64_t    us;        /* its longest run */
    uint32_t    loop;      /* where that was */
    uint64_t    seed;
};

/* What the system holds at one moment. */
struct holdings {
    uint64_t pages;                    /* free + thread stack cache, settled */
    uint64_t channels;                 /* channel_live_count */
    uint64_t interrupts;               /* interrupt_live_count */
    uint64_t job[JOB_LIMIT_COUNT];     /* the caller's job tree, by kind */
};

static spinlock_t lock = SPINLOCK_INIT("ktest soak");
/* All guarded by lock. */
static uint64_t loops, passed, skipped, failed;
static uint64_t seed_first, seed_last;   /* of the loops; 0: link order */
static struct fail fails[FAILS_KEPT];
static struct slow slowest[SLOW_KEPT];
static uint64_t began_ns;
static struct holdings at_begin;

static void hold(struct job *scope, struct holdings *h)
{
    h->pages = ktest_accounted_pages();
    h->channels = channel_live_count();
    h->interrupts = interrupt_live_count();
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        h->job[k] = scope ? job_used(scope, k) : 0;
}

void ktest_soak_begin(struct job *scope)
{
    struct holdings h;
    hold(scope, &h);
    uint64_t f = spin_lock_irqsave(&lock);
    loops = passed = skipped = failed = 0;
    seed_first = seed_last = 0;
    memset(fails, 0, sizeof(fails));
    memset(slowest, 0, sizeof(slowest));
    began_ns = uptime_ns();
    at_begin = h;
    spin_unlock_irqrestore(&lock, f);
    kprintf("soak: begun on %u CPUs: %lu pages free or stack-cached, %lu channels, the "
            "caller's jobs hold %lu pages, %lu handles, %lu threads\n", cpu_count, h.pages,
            h.channels, h.job[JOB_LIMIT_PAGES], h.job[JOB_LIMIT_HANDLES],
            h.job[JOB_LIMIT_THREADS]);
}

void ktest_soak_note_loop(uint64_t seed)
{
    uint64_t f = spin_lock_irqsave(&lock);
    if (!loops)
        seed_first = seed;
    seed_last = seed;
    loops++;
    spin_unlock_irqrestore(&lock, f);
}

void ktest_soak_note_pass(const char *name, uint64_t us, uint32_t loop, uint64_t seed)
{
    uint64_t f = spin_lock_irqsave(&lock);
    passed++;
    /* The test's own entry if it has one, else the quickest entry. */
    struct slow *s = &slowest[0];
    for (int i = 0; i < SLOW_KEPT; i++) {
        if (slowest[i].name == name) {
            s = &slowest[i];
            break;
        }
        if (slowest[i].us < s->us)
            s = &slowest[i];
    }
    if (us > s->us || !s->name)
        *s = (struct slow){ name, us, loop, seed };
    spin_unlock_irqrestore(&lock, f);
}

void ktest_soak_note_skip(void)
{
    uint64_t f = spin_lock_irqsave(&lock);
    skipped++;
    spin_unlock_irqrestore(&lock, f);
}

void ktest_soak_note_fail(const char *name, const char *msg, uint32_t loop, uint64_t seed)
{
    uint64_t f = spin_lock_irqsave(&lock);
    if (failed < FAILS_KEPT) {
        struct fail *x = &fails[failed];
        x->name = name;
        x->loop = loop;
        x->seed = seed;
        size_t n = strlen(msg);
        if (n >= sizeof(x->msg))
            n = sizeof(x->msg) - 1;
        memcpy(x->msg, msg, n);
        x->msg[n] = '\0';
    }
    failed++;
    spin_unlock_irqrestore(&lock, f);
}

/* ---- "soak end" ------------------------------------------------------------------ */

struct shell_counts {
    uint64_t utest_runs, utest_failed;   /* utest between the loops */
    uint64_t io_cycles, io_failed;       /* the file load */
    bool     halt;                       /* panic if anything failed */
};

/* "123" or "123,45" at *p up to a space: the numbers; *p moves past them. */
static bool two_numbers(const char **p, uint64_t *a, uint64_t *b)
{
    uint64_t *v = a;
    bool digits = false;
    *a = *b = 0;
    for (; **p && **p != ' '; (*p)++) {
        if (**p == ',' && v == a && digits) {
            v = b;
            digits = false;
        } else if (**p >= '0' && **p <= '9' && *v < 1000000000) {
            *v = *v * 10 + (uint64_t)(**p - '0');
            digits = true;
        } else {
            return false;
        }
    }
    return digits;
}

static bool parse_counts(const char *args, struct shell_counts *c)
{
    memset(c, 0, sizeof(*c));
    for (const char *p = args; *p;) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (!memcmp(p, "u=", 2)) {
            p += 2;
            if (!two_numbers(&p, &c->utest_runs, &c->utest_failed))
                return false;
        } else if (!memcmp(p, "io=", 3)) {
            p += 3;
            if (!two_numbers(&p, &c->io_cycles, &c->io_failed))
                return false;
        } else if (!memcmp(p, "halt", 4) && (!p[4] || p[4] == ' ')) {
            c->halt = true;
            p += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool ktest_soak_args_ok(const char *args)
{
    struct shell_counts c;
    return parse_counts(args, &c);
}

static void print_tests(void)
{
    if (seed_first || seed_last)
        kprintf("  kernel tests: %lu loop(s), seeds %lu to %lu: %lu passed, %lu skipped, "
                "%lu FAILED\n", loops, seed_first, seed_last, passed, skipped, failed);
    else
        kprintf("  kernel tests: %lu loop(s) in link order: %lu passed, %lu skipped, "
                "%lu FAILED\n", loops, passed, skipped, failed);
    for (uint64_t i = 0; i < failed && i < FAILS_KEPT; i++) {
        if (fails[i].loop)
            kprintf("  FAILED %s: %s [loop %u, replay: ktest seed=%lu]\n", fails[i].name,
                    fails[i].msg, fails[i].loop, fails[i].seed);
        else
            kprintf("  FAILED %s: %s\n", fails[i].name, fails[i].msg);
    }
    if (failed > FAILS_KEPT)
        kprintf("  (%lu more failures are in the log)\n", failed - FAILS_KEPT);
    kprintf("  slowest tests:\n");
    for (int i = 0; i < SLOW_KEPT; i++)
        if (slowest[i].name)
            kprintf("    %-40s %6lu.%03lu ms  (loop %u, seed %lu)\n", slowest[i].name,
                    slowest[i].us / 1000, slowest[i].us % 1000, slowest[i].loop,
                    slowest[i].seed);
}

static void print_holdings(const struct holdings *b, const struct holdings *a)
{
    kprintf("  held before -> after (printed, not judged: drivers and mounts move them):\n");
    kprintf("    pages free or stack-cached  %lu -> %lu (%ld)\n", b->pages, a->pages,
            (long)(a->pages - b->pages));
    kprintf("    channels                    %lu -> %lu (%ld)\n", b->channels, a->channels,
            (long)(a->channels - b->channels));
    kprintf("    interrupt objects           %lu -> %lu (%ld)\n", b->interrupts, a->interrupts,
            (long)(a->interrupts - b->interrupts));
    static const char *const kinds[JOB_LIMIT_COUNT] = { "", "pages", "handles", "threads",
                                                        "message bytes" };
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        kprintf("    jobs: %-21s %lu -> %lu (%ld)\n", kinds[k], b->job[k], a->job[k],
                (long)(a->job[k] - b->job[k]));
}

int64_t ktest_soak_end(struct job *scope, const char *args)
{
    struct shell_counts c;
    struct holdings now;
    parse_counts(args, &c);
    hold(scope, &now);
    uint64_t total = failed + c.utest_failed + c.io_failed;
    kprintf("\n==================== SOAK RESULTS (read these lines out) ====================\n");
    kprintf("  soak: %s after %lu s on %u CPUs\n", total ? "FAILED" : "PASSED",
            (uptime_ns() - began_ns) / 1000000000, cpu_count);
    print_tests();
    kprintf("  utest between the loops: %lu run(s), %lu FAILED\n", c.utest_runs, c.utest_failed);
    kprintf("  file load: %lu cycle(s) written, read back and compared, %lu FAILED\n",
            c.io_cycles, c.io_failed);
    print_holdings(&at_begin, &now);
    kprintf("==============================================================================\n");
    if (total && c.halt)
        panic("soak: FAILED: %lu kernel test failure(s)%s%s%s%s, %lu utest run(s), %lu file "
              "cycle(s): the SOAK RESULTS box is in the log lines below", failed,
              failed ? ", the first: " : "", failed ? fails[0].name : "", failed ? ": " : "",
              failed ? fails[0].msg : "", c.utest_failed, c.io_failed);
    return (int64_t)total;
}
