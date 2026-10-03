/* linuxbench: the method: the TSC, its rate, the timestamp's cost,
 * medians and 99th percentiles, and the lines, printed as bench prints
 * them (kernel/test/bench.c: result). */
#include "lb.h"   /* first: it defines _GNU_SOURCE before any system header */

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

uint64_t samples[SAMPLES];
uint64_t samples2[SAMPLES];
uint64_t tsc_hz;

static uint64_t ps_per_cycle_x1024;   /* picoseconds per TSC cycle, << 10 */
static uint64_t stamp_cost;           /* cycles, median of a back-to-back pair */
static FILE *out;                     /* the output file, or NULL */

static uint64_t clock_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t mono_ns(void)
{
    return clock_ns(CLOCK_MONOTONIC);
}

void warm_until(uint64_t *end)
{
    *end = mono_ns() + WARM_NS;
}

void sort_u64(uint64_t *a, unsigned n)
{
    static const unsigned gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (unsigned g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
        for (unsigned i = gaps[g]; i < n; i++) {
            uint64_t v = a[i];
            unsigned j = i;
            for (; j >= gaps[g] && a[j - gaps[g]] > v; j -= gaps[g])
                a[j] = a[j - gaps[g]];
            a[j] = v;
        }
}

/* 200 ms against CLOCK_MONOTONIC_RAW (no NTP slewing): the rate to a few
 * parts per million. Then the timestamp pair, as bench_stamp does. */
void calibrate(void)
{
    uint64_t t0 = clock_ns(CLOCK_MONOTONIC_RAW), c0 = stamp(), t1, c1;
    do {
        t1 = clock_ns(CLOCK_MONOTONIC_RAW);
        c1 = stamp();
    } while (t1 - t0 < 200000000ull);
    tsc_hz = (c1 - c0) * 1000000000ull / (t1 - t0);
    if (!tsc_hz)
        tsc_hz = 1;
    ps_per_cycle_x1024 = (1000000000000ull << 10) / tsc_hz;
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t a = stamp(), b = stamp();
        samples[i] = b - a;
    }
    sort_u64(samples, SAMPLES);
    stamp_cost = samples[SAMPLES / 2];
}

uint64_t cycles_to_ps(uint64_t c)
{
    return c * ps_per_cycle_x1024 >> 10;
}

uint64_t stamp_cost_ps(void)
{
    return cycles_to_ps(stamp_cost);
}

uint64_t span_ps(uint64_t t0, uint64_t t1, uint64_t n)
{
    uint64_t c = t1 - t0;
    c = c > stamp_cost ? c - stamp_cost : 0;
    return cycles_to_ps(c) / n;
}

bool out_open(const char *path)
{
    if (!path)
        return true;
    out = fopen(path, "w");
    return out != NULL;
}

void out_close(void)
{
    if (out)
        fclose(out);
    out = NULL;
}

void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (out) {
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
        fflush(out);
    }
}

void fmt_ps(char *buf, size_t n, uint64_t ps)
{
    if (ps < 10000000)   /* under 10 us: ns with one decimal */
        snprintf(buf, n, "%llu.%llu ns", (unsigned long long)(ps / 1000),
                 (unsigned long long)(ps / 100 % 10));
    else
        snprintf(buf, n, "%llu us", (unsigned long long)(ps / 1000000));
}

void result_of(const char *what, const char *how, uint64_t *s, unsigned n)
{
    char med[24], p99[24];
    sort_u64(s, n);
    fmt_ps(med, sizeof(med), s[(n - 1) / 2]);
    fmt_ps(p99, sizeof(p99), s[(n - 1) * 99 / 100]);
    say("bench: %-44s median %-10s p99 %-10s = %s\n", what, med, p99, how);
}

void result(const char *what, const char *how, unsigned n)
{
    result_of(what, how, samples, n);
}

void skipped(const char *what, const char *why)
{
    say("bench: %s: %s\n", what, why);
}
