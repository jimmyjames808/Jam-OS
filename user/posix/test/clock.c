/* posixtest's clock section: the clocks read from the kernel's time page.
 *
 *   - CLOCK_MONOTONIC from 8 threads for a second, each read between two
 *     clock_get calls: never behind the first, and no more than 1 us
 *     past the second (the call's own TSC read isn't fenced, so it may be
 *     taken a little early);
 *   - CLOCK_REALTIME against wallclock_get's UTC moved on by the uptime,
 *     the same way;
 *   - clock_getres, a nanosleep measured with the clock, and time();
 *   - the bench: clock_gettime with the page against the calls it used
 *     before (clock_get, and wallclock_get plus clock_get), in ns a call
 *     (BENCH.md's lines). */
#include <stdio.h>
#include <jamos/libjam.h>
#include <pthread.h>
#include <time.h>
#include "pt.h"

#define READERS     8
#define READ_NS     1000000000ll   /* each reader's second */
#define SLACK_NS    1000           /* the unfenced call's TSC read */
#define BENCH_CALLS 200000

static int64_t ts_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * 1000000000ll + t->tv_nsec;
}

static int64_t mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ts_ns(&t);
}

static int64_t real(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return ts_ns(&t);
}

struct reader {
    long reads, behind, ahead, real_bad;
};

static void *reader(void *arg)
{
    struct reader *r = arg;
    int64_t end = (int64_t)jam_clock_get() + READ_NS;
    while ((int64_t)jam_clock_get() < end) {
        int64_t c1 = (int64_t)jam_clock_get(), v = mono(), c2 = (int64_t)jam_clock_get();
        r->behind += v < c1;
        r->ahead += v > c2 + SLACK_NS;
        struct wall_clock w;
        if (jam_wallclock_get(&w) == OK) {
            int64_t t = real(), up = (int64_t)jam_clock_get();
            r->real_bad += t < w.utc_ns || t > w.utc_ns + (up - (int64_t)w.uptime_ns) + SLACK_NS;
        }
        r->reads++;
    }
    return NULL;
}

static void check_against_calls(void)
{
    pthread_t t[READERS];
    struct reader r[READERS] = { { 0 } };
    for (int i = 0; i < READERS; i++)
        CHECK(pthread_create(&t[i], NULL, reader, &r[i]) == 0);
    long reads = 0;
    for (int i = 0; i < READERS; i++) {
        CHECK(pthread_join(t[i], NULL) == 0);
        CHECK(r[i].behind == 0 && r[i].ahead == 0 && r[i].real_bad == 0);
        CHECK(r[i].reads > 0);
        reads += r[i].reads;
    }
    printf("posixtest: clock: %ld reads from %d threads against the calls\n", reads, READERS);
}

static void check_misc(void)
{
    struct timespec res;
    CHECK(clock_getres(CLOCK_MONOTONIC, &res) == 0 && res.tv_sec == 0 && res.tv_nsec == 1);
    CHECK(clock_getres(CLOCK_REALTIME, &res) == 0 && res.tv_nsec == 1);
    int64_t t0 = mono();
    struct timespec nap = { 0, 2000000 };
    CHECK(nanosleep(&nap, NULL) == 0);
    CHECK(mono() - t0 >= 2000000);
    time_t now = time(NULL);
    CHECK(now > 946684800 && now - real() / 1000000000ll <= 1);   /* after 2000; the same clock */
}

/* Tenths of a ns a call of fn, the mean of BENCH_CALLS calls after a
 * tenth as many untimed (the page mapped, the caches warm). */
static long per_call(void (*fn)(void))
{
    for (int i = 0; i < BENCH_CALLS / 10; i++)
        fn();
    uint64_t t0 = jam_clock_get();
    for (int i = 0; i < BENCH_CALLS; i++)
        fn();
    return (long)((jam_clock_get() - t0) * 10 / BENCH_CALLS);
}

static volatile int64_t sink;
static void mono_page(void) { sink = mono(); }
static void real_page(void) { sink = real(); }
static void mono_call(void) { sink = (int64_t)jam_clock_get(); }
static void real_calls(void)
{
    struct wall_clock w;
    int64_t up = (int64_t)jam_clock_get();
    if (jam_wallclock_get(&w) == OK)
        sink = w.utc_ns + (up - (int64_t)w.uptime_ns);
}

static void bench(void)
{
    long mp = per_call(mono_page), mc = per_call(mono_call);
    long rp = per_call(real_page), rc = per_call(real_calls);
    printf("posixtest: bench: clock_gettime(CLOCK_MONOTONIC): %ld.%ld ns with the page, "
           "%ld.%ld ns as the call (clock_get)\n", mp / 10, mp % 10, mc / 10, mc % 10);
    printf("posixtest: bench: clock_gettime(CLOCK_REALTIME): %ld.%ld ns with the page, "
           "%ld.%ld ns as the calls (wallclock_get, clock_get)\n",
           rp / 10, rp % 10, rc / 10, rc % 10);
}

void pt_clock(char **argv)
{
    (void)argv;
    check_misc();
    check_against_calls();
    bench();
}
