/* fbbench: timing and output, with bench's method (the header of
 * kernel/test/bench.c): the TSC fenced with lfence on both sides
 * (cpu_tsc), its rate measured against the clock, the timestamp's own
 * cost measured and subtracted, an untimed warm-up before every line
 * (which also brings the cores up to speed), and the median and the 99th
 * percentile of the samples, never a mean, nothing trimmed. Nothing is
 * printed while measuring: the lines are kept in a buffer and printed
 * after the screen is given back. */
#include <stdarg.h>
#include "fbbench.h"

#define WARM_NS    (20 * NS_PER_MS)
#define MAX_SAMPLE 4096
#define OUT_MAX    (24u << 10)

static uint64_t ps_per_cycle_x1024;   /* picoseconds a TSC cycle, << 10 */
static uint64_t tsc_mhz;              /* the TSC's rate, measured */
static uint64_t stamp_cost;           /* cycles: median of a back-to-back pair */
static uint64_t samples[MAX_SAMPLE];  /* one line's samples, in ps */
static char     out[OUT_MAX];         /* the kept lines */
static size_t   out_len;              /* bytes of out used */
static bool     out_lost;             /* a line didn't fit */

static void sort(uint64_t *a, unsigned n)
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

void timing_init(void)
{
    uint64_t t0 = now(), c0 = cpu_tsc(), t1, c1;
    do {
        t1 = now();
        c1 = cpu_tsc();
    } while (t1 - t0 < WARM_NS);
    uint64_t hz = (c1 - c0) * NS_PER_S / (t1 - t0);
    if (!hz)
        hz = 1;
    tsc_mhz = hz / 1000000;
    ps_per_cycle_x1024 = (1000000000000ull << 10) / hz;
    for (unsigned i = 0; i < MAX_SAMPLE; i++) {
        uint64_t a = cpu_tsc(), b = cpu_tsc();
        samples[i] = b - a;
    }
    sort(samples, MAX_SAMPLE);
    stamp_cost = samples[(MAX_SAMPLE - 1) / 2];
}

uint64_t timing_tsc_mhz(void) { return tsc_mhz; }
uint64_t timing_stamp_ps(void) { return stamp_cost * ps_per_cycle_x1024 >> 10; }

static uint64_t span_ps(uint64_t t0, uint64_t t1)
{
    uint64_t c = t1 - t0;
    c = c > stamp_cost ? c - stamp_cost : 0;
    return c * ps_per_cycle_x1024 >> 10;
}

void time_line(void (*fn)(void *), void *arg, unsigned n, uint64_t *med, uint64_t *p99)
{
    n = n < 1 ? 1 : n > MAX_SAMPLE ? MAX_SAMPLE : n;
    unsigned warm = 0;
    for (uint64_t end = now() + WARM_NS; warm < 2 || now() < end; warm++)
        fn(arg);
    for (unsigned i = 0; i < n; i++) {
        uint64_t t0 = cpu_tsc();
        fn(arg);
        samples[i] = span_ps(t0, cpu_tsc());
    }
    sort(samples, n);
    *med = samples[(n - 1) / 2];
    *p99 = samples[(n - 1) * 99 / 100];
}

/* ---- output ------------------------------------------------------------------------ */

static void keep(const char *fmt, va_list ap)
{
    if (out_len >= OUT_MAX - 1) {
        out_lost = true;
        return;
    }
    int n = vsnprintf(out + out_len, OUT_MAX - out_len, fmt, ap);
    if (n < 0 || (size_t)n >= OUT_MAX - out_len) {
        out[out_len] = 0;   /* drop the cut line whole */
        out_lost = true;
        return;
    }
    out_len += (size_t)n;
}

void out_text(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    keep(fmt, ap);
    va_end(ap);
}

/* As bench's: "123.4 ns" under 10 us, else whole us. */
static void fmt_ps(char *buf, size_t n, uint64_t ps)
{
    if (ps < 10000000)
        snprintf(buf, n, "%lu.%lu ns", (unsigned long)(ps / 1000), (unsigned long)(ps / 100 % 10));
    else
        snprintf(buf, n, "%lu us", (unsigned long)(ps / 1000000));
}

void out_line(const char *what, uint64_t med, uint64_t p99, const char *rate)
{
    char m[24], p[24];
    fmt_ps(m, sizeof(m), med);
    fmt_ps(p, sizeof(p), p99);
    out_text("fbbench: %-64s median %-10s p99 %-10s %s\n", what, m, p, rate);
}

void out_flush(void)
{
    /* printf sends at most a console write's worth at a time: line by line. */
    char *line = out;
    while (*line) {
        char *nl = line;
        while (*nl && *nl != '\n')
            nl++;
        printf("%.*s\n", (int)(nl - line), line);
        line = *nl ? nl + 1 : nl;
    }
    if (out_lost)
        printf("fbbench: (some lines were lost: the output buffer was full)\n");
    out_len = 0;
    out[0] = 0;
    out_lost = false;
}

void rate_gbs(char *buf, size_t n, uint64_t bytes, uint64_t ps)
{
    /* bytes per ns is GB/s; x100 for two decimals. */
    uint64_t x100 = ps ? bytes * 100000 / ps : 0;
    snprintf(buf, n, "%lu.%02lu GB/s", (unsigned long)(x100 / 100), (unsigned long)(x100 % 100));
}

void rate_mpx(char *buf, size_t n, uint64_t pixels, uint64_t ps)
{
    /* ps per pixel is us per megapixel; x10 for one decimal. */
    uint64_t x10 = pixels ? ps * 10 / pixels : 0;
    snprintf(buf, n, "%lu.%lu us/Mpx", (unsigned long)(x10 / 10), (unsigned long)(x10 % 10));
}
