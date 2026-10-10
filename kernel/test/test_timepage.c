/* The time page (struct time_page in <jam/abi.h>, dev/wallclock.c): the
 * page user code reads clock_get's uptime and wallclock_get's UTC from
 * without a system call. Read here with the same inline helpers libc uses
 * (time_page_begin, time_page_retry, time_page_uptime).
 *
 *   timepage_conversion_exact
 *       mult and shift (tsc_conversion) plus the correction give exactly
 *       floor(d * 1e9 / hz) for TSC rates from 25 MHz to 4 GHz and for
 *       deltas from 0 to 2^64 - 1; for this boot's rate, tsc_to_ns's.
 *   timepage_against_clock_get
 *       on up to 8 CPUs for 10 s: the page read between two uptime_ns()
 *       calls is never behind the first nor ahead of the second (so within
 *       their gap, and the conversion is exact), and the page's UTC is
 *       wallclock_get's at the same uptime.
 *   timepage_set_mid_read
 *       a publish between a reader's time_page_begin and time_page_retry
 *       makes it read again, and the second read sees the new values; on
 *       the live page, wallclock_set (to the time it already has) moves
 *       the word by two and leaves it even.
 *   timepage_no_torn_reads
 *       a writer publishing two pairs of values as fast as it can and
 *       readers on the other CPUs: no reader ever accepts a mixed pair. */
#include <jam/abi.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/wallclock.h>

#define AGAINST_NS   (10 * NS_PER_S)   /* timepage_against_clock_get's run */
#define TORN_NS      (300 * NS_PER_MS) /* timepage_no_torn_reads' run */
#define MAX_READERS  8
/* Each thread's least work, however late a loaded machine starts it. */
#define MIN_ROUNDS   1000

static inline uint64_t tsc_ordered(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}

static inline void lfence(void)
{
    __asm__ volatile("lfence" ::: "memory");
}

/* floor(d * 1e9 / hz), the way tsc_to_ns writes it, for any hz. */
static uint64_t ref_ns(uint64_t d, uint64_t hz)
{
    return d / hz * NS_PER_S + d % hz * NS_PER_S / hz;
}

/* One rate: a page with its conversion, checked at many deltas. */
static void conversion_at(uint64_t hz, uint64_t *seed)
{
    struct time_page p;
    memset(&p, 0, sizeof(p));
    p.tsc_hz = hz;
    p.tsc_boot = kt_rng(seed);   /* any base: the delta wraps as the kernel's does */
    tsc_conversion(hz, &p.mult, &p.shift);
    KT_ASSERT(p.shift >= 34 && p.shift <= 64);
    /* Deltas whose uptime fits 64 bits: all of them from 1 GHz up. */
    uint64_t cap = hz >= NS_PER_S ? UINT64_MAX : UINT64_MAX / NS_PER_S * hz;
    static const uint64_t edges[] = { 0, 1, 999, 1000000000, UINT64_MAX, UINT64_MAX - 1,
                                      1ull << 63, (1ull << 32) - 1 };
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        uint64_t d = edges[i] <= cap ? edges[i] : cap;
        KT_EQ(time_page_uptime(&p, p.tsc_boot + d), ref_ns(d, hz));
    }
    for (unsigned i = 0; i < 20000; i++) {
        uint64_t r = kt_rng(seed), d;
        switch (i % 4) {
        case 0: d = r % (hz * 3600); break;              /* the first hour */
        case 1: d = r >> (r % 64); break;                /* any size */
        case 2: d = (r % 100000) * hz + (r >> 40) % 3 - 1; break;   /* about whole seconds */
        default: d = r; break;
        }
        d = d <= cap ? d : d % cap;
        KT_EQ(time_page_uptime(&p, p.tsc_boot + d), ref_ns(d, hz));
    }
}

KTEST(timepage_conversion_exact)
{
    uint64_t seed = 0x7a11ce5;
    static const uint64_t rates[] = { 25000000, 100000000, 999999999, 1000000000, 1000000001,
                                      2400000000ull, 3187654321ull, 4000000000ull };
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        conversion_at(rates[i], &seed);
    conversion_at(tsc_hz, &seed);

    const struct time_page *p = wallclock_page_kernel();
    KT_ASSERT(p);
    KT_EQ(p->tsc_hz, tsc_hz);
    KT_ASSERT(p->flags & TIME_PAGE_TSC);
    for (unsigned i = 0; i < 20000; i++) {
        uint64_t d = kt_rng(&seed) >> (i % 64);
        KT_EQ(time_page_uptime(p, p->tsc_boot + d), tsc_to_ns(d));
    }
}

/* One reader of timepage_against_clock_get. */
struct reader {
    uint32_t       cpu;
    uint64_t       until;
    uint64_t       reads, behind, ahead, wall_checks, wall_bad, max_gap;
    struct thread *t;
};

/* The page's wall clock against wallclock_get's at the same uptime; false
 * only for a real mismatch (a set between the two reads is skipped). */
static bool wall_agrees(const struct time_page *p, uint64_t *checked)
{
    struct wall_clock w;
    uint32_t s = time_page_begin(p);
    int64_t off = __atomic_load_n(&p->wall_offset, __ATOMIC_RELAXED);
    uint32_t flags = __atomic_load_n(&p->wall_flags, __ATOMIC_RELAXED);
    status_t st = wallclock_get(&w);
    if (time_page_retry(p, s))
        return true;   /* the clock was set meanwhile: no answer */
    (*checked)++;
    if (st != OK)
        return flags == 0;
    return flags == w.flags && (int64_t)w.uptime_ns + off == w.utc_ns;
}

static void reader_main(void *arg)
{
    struct reader *r = arg;
    kt_pin_self(r->cpu);
    const struct time_page *p = wallclock_page_kernel();
    do {
        for (unsigned i = 0; i < 4096; i++) {
            lfence();
            uint64_t c1 = uptime_ns();
            uint64_t v = time_page_uptime(p, tsc_ordered());
            lfence();
            uint64_t c2 = uptime_ns();
            r->behind += v < c1;
            r->ahead += v > c2;
            if (v >= c1 && v - c1 > r->max_gap)
                r->max_gap = v - c1;
        }
        r->reads += 4096;
        r->wall_bad += !wall_agrees(p, &r->wall_checks);
    } while (uptime_ns() < r->until);
    kt_unpin_self();
}

KTEST(timepage_against_clock_get)
{
    const struct time_page *p = wallclock_page_kernel();
    KT_ASSERT(p);
    uint32_t n = cpu_count < MAX_READERS ? cpu_count : MAX_READERS;
    static struct reader rs[MAX_READERS];
    memset(rs, 0, sizeof(rs));
    uint64_t until = uptime_ns() + AGAINST_NS;
    for (uint32_t i = 0; i < n; i++) {
        rs[i].cpu = i;
        rs[i].until = until;
        rs[i].t = thread_create("timepage-read", reader_main, &rs[i], PRIO_DEFAULT);
        KT_ASSERT(rs[i].t);
    }
    uint64_t reads = 0, gap = 0, walls = 0;
    for (uint32_t i = 0; i < n; i++) {
        thread_join(rs[i].t);
        reads += rs[i].reads;
        walls += rs[i].wall_checks;
        gap = rs[i].max_gap > gap ? rs[i].max_gap : gap;
    }
    kprintf("ktest %s: %u CPUs, %lu reads, %lu wall checks, largest gap after clock_get %lu ns\n",
            ktest_current, n, reads, walls, gap);
    for (uint32_t i = 0; i < n; i++) {
        KT_EQ(rs[i].behind, 0);
        KT_EQ(rs[i].ahead, 0);
        KT_EQ(rs[i].wall_bad, 0);
        KT_ASSERT(rs[i].reads > 0);
    }
}

KTEST(timepage_set_mid_read)
{
    /* A page of the test's own, published as wallclock_set publishes. */
    static struct time_page own;
    memset(&own, 0, sizeof(own));
    time_page_publish(&own, 1000, WALLCLOCK_RTC);
    uint32_t s = time_page_begin(&own);
    KT_EQ(s, 2);
    KT_EQ(own.wall_offset, 1000);
    time_page_publish(&own, -5, WALLCLOCK_SET | WALLCLOCK_NET);   /* mid-read */
    KT_ASSERT(time_page_retry(&own, s));
    s = time_page_begin(&own);
    KT_EQ(own.wall_offset, -5);
    KT_EQ(own.wall_flags, WALLCLOCK_SET | WALLCLOCK_NET);
    KT_ASSERT(!time_page_retry(&own, s));

    /* The live page: a set to the time the clock has already (only its
     * RTC flag, if nobody set it yet, becomes SET). */
    const struct time_page *p = wallclock_page_kernel();
    KT_ASSERT(p);
    struct wall_clock w;
    if (wallclock_get(&w) != OK)
        return;   /* a machine without an RTC that nobody set */
    w.flags &= WALLCLOCK_NET;
    memset(w.zone, 0, sizeof(w.zone));   /* "": the zone stays */
    s = time_page_begin(p);
    KT_EQ(wallclock_set(&w), OK);
    KT_ASSERT(time_page_retry(p, s));
    uint32_t now = time_page_begin(p);
    KT_ASSERT(now - s >= 2 && !(now & 1));   /* (2 unless sntp set it too) */
    struct wall_clock after;
    KT_EQ(wallclock_get(&after), OK);
    KT_EQ(after.utc_ns - (int64_t)after.uptime_ns, w.utc_ns - (int64_t)w.uptime_ns);
}

/* timepage_no_torn_reads' page and its two pairs. */
static struct time_page torn_page;
#define PAIR_A_OFF   0x0123456789abcdefll
#define PAIR_A_FLAGS WALLCLOCK_SET
#define PAIR_B_OFF   (-0x7edcba9876543210ll)
#define PAIR_B_FLAGS (WALLCLOCK_SET | WALLCLOCK_NET)

struct torn {
    uint64_t       until;
    uint32_t       cpu;
    uint64_t       reads, retries, torn, writes;
    struct thread *t;
};

static void torn_writer(void *arg)
{
    struct torn *w = arg;
    kt_pin_self(w->cpu);
    while (uptime_ns() < w->until || w->writes < MIN_ROUNDS) {
        bool a = w->writes++ & 1;
        time_page_publish(&torn_page, a ? PAIR_A_OFF : PAIR_B_OFF, a ? PAIR_A_FLAGS : PAIR_B_FLAGS);
    }
    kt_unpin_self();
}

static void torn_reader(void *arg)
{
    struct torn *r = arg;
    kt_pin_self(r->cpu);
    while (uptime_ns() < r->until || r->reads < MIN_ROUNDS) {
        int64_t off;
        uint32_t s, flags;
        for (;;) {
            s = time_page_begin(&torn_page);
            off = __atomic_load_n(&torn_page.wall_offset, __ATOMIC_RELAXED);
            flags = __atomic_load_n(&torn_page.wall_flags, __ATOMIC_RELAXED);
            if (!time_page_retry(&torn_page, s))
                break;
            r->retries++;
        }
        r->reads++;
        r->torn += !((off == PAIR_A_OFF && flags == PAIR_A_FLAGS) ||
                     (off == PAIR_B_OFF && flags == PAIR_B_FLAGS));
    }
    kt_unpin_self();
}

KTEST(timepage_no_torn_reads)
{
    memset(&torn_page, 0, sizeof(torn_page));
    time_page_publish(&torn_page, PAIR_A_OFF, PAIR_A_FLAGS);
    uint32_t n = cpu_count < MAX_READERS ? cpu_count : MAX_READERS;
    n = n < 2 ? 2 : n;   /* a writer and a reader, sharing a CPU if they must */
    static struct torn ts[MAX_READERS];
    memset(ts, 0, sizeof(ts));
    uint64_t until = uptime_ns() + TORN_NS;
    for (uint32_t i = 0; i < n; i++) {
        ts[i].until = until;
        ts[i].cpu = i % cpu_count;
        ts[i].t = thread_create("timepage-torn", i ? torn_reader : torn_writer, &ts[i],
                                PRIO_DEFAULT);
        KT_ASSERT(ts[i].t);
    }
    uint64_t reads = 0, retries = 0;
    for (uint32_t i = 0; i < n; i++) {
        thread_join(ts[i].t);
        reads += ts[i].reads;
        retries += ts[i].retries;
        KT_EQ(ts[i].torn, 0);
    }
    kprintf("ktest %s: %lu writes, %lu reads, %lu read again\n", ktest_current, ts[0].writes,
            reads, retries);
    KT_ASSERT(ts[0].writes > 0 && reads > 0);
    KT_IDLE_ASSERT(retries > 0);   /* readers on other CPUs do meet the writer */
}
