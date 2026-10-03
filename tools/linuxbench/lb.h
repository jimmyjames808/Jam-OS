/* linuxbench: docs/BENCH.md's lines measured on Linux, on the same PC,
 * for BENCH.md's Linux column (docs/M11.5-PLAN.md, "The Linux column";
 * how to build and run it: README.md here).
 *
 * The method is Jam OS's (kernel/test/bench.c's header), so the columns
 * compare:
 *   - time is the TSC, `lfence; rdtsc; lfence`, its rate measured against
 *     CLOCK_MONOTONIC_RAW; the cost of a timestamp pair is measured first
 *     and subtracted from every sample;
 *   - SAMPLES samples a line (TIMER_SAMPLES for the sleeps, DEV_SAMPLES
 *     for the lines that reach the stick), the median and the 99th
 *     percentile, nothing trimmed; operations shorter than a few hundred
 *     ns are timed in batches of BATCH;
 *   - WARM_NS of untimed warm-up first;
 *   - every benchmark thread is pinned and SCHED_FIFO at FIFO_PRIO (Jam
 *     OS's run at priority 24, above everything else); the CPUs are
 *     chosen as bench chooses them (P, P2, HT, E: pick_cpus in cpus.c);
 *     the orchestrating thread stays on CPU 0;
 *   - nothing is printed while measuring.
 * Each line is BENCH.md's name for the Jam OS line it stands beside, then
 * "=" and what Linux does for it; a line with no Jam OS twin says so.
 *
 * Files: method.c (timing, statistics, output), cpus.c (topology,
 * pinning, threads), sysinfo.c (the header: kernel, command line,
 * mitigations, governor), basic.c (lines on one CPU), cross.c (one
 * thread waking another, and P against the other CPUs), ipc.c (calls
 * between processes and threads), files.c (the per-operation lines),
 * main.c (the order). */
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAMPLES       4000
#define BATCH         64
#define WARM_NS       20000000ull   /* 20 ms */
#define TIMER_SAMPLES 200
#define DEV_SAMPLES   400
#define DEV_WARM      8
#define FIFO_PRIO     50
#define MAX_CPUS      256

/* ---- method.c ------------------------------------------------------------------ */

static inline uint64_t stamp(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return (uint64_t)hi << 32 | lo;
}

static inline void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

extern uint64_t samples[SAMPLES];    /* picoseconds per sample (per operation) */
extern uint64_t samples2[SAMPLES];   /* a second set, for lines timed in pairs */
extern uint64_t tsc_hz;              /* the TSC's rate, measured */

/* CLOCK_MONOTONIC in ns. */
uint64_t mono_ns(void);
/* The TSC's rate (against CLOCK_MONOTONIC_RAW), then a timestamp's cost:
 * call once, from the CPU the lines are timed on. */
void     calibrate(void);
/* Picoseconds for one operation of a span of n timed between t0 and t1,
 * the timestamp's cost taken off. */
uint64_t span_ps(uint64_t t0, uint64_t t1, uint64_t n);
uint64_t cycles_to_ps(uint64_t c);
uint64_t stamp_cost_ps(void);
/* *end = now + WARM_NS. */
void     warm_until(uint64_t *end);

/* Print to stdout and the output file (out_open). */
void     say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The output file; NULL path: stdout only. False if it can't be made. */
bool     out_open(const char *path);
void     out_close(void);
/* "bench: <what> median X p99 Y = <how>" for s[0..n), which it sorts. */
void     result_of(const char *what, const char *how, uint64_t *s, unsigned n);
void     result(const char *what, const char *how, unsigned n);   /* samples[] */
/* "bench: <what>: <why>" for a line that could not be measured. */
void     skipped(const char *what, const char *why);
/* Format "x.y ns" / "N us" as bench does. */
void     fmt_ps(char *buf, size_t n, uint64_t ps);
void     sort_u64(uint64_t *a, unsigned n);

/* ---- cpus.c -------------------------------------------------------------------- */

extern int cpu_p, cpu_p2, cpu_ht, cpu_e;   /* -1: none */
extern int ncpus_online;
extern bool fifo_ok;                       /* every SCHED_FIFO request so far worked */

/* Read the topology and choose P, P2, HT and E. */
void        pick_cpus(void);
const char *kind(int cpu);                 /* "P", "P2", "HT", "E" or "?" */
bool        cpu_online(int cpu);
int         core_of(int cpu);              /* the first CPU of its core */
bool        cpu_is_e(int cpu);
/* The calling thread onto set (or one CPU) at SCHED_FIFO FIFO_PRIO. */
void        pin_self_set(const cpu_set_t *set);
void        pin_self(int cpu);
/* A thread running fn(arg) pinned like pin_self; run_on waits for it. */
pthread_t   start_on(int cpu, void *(*fn)(void *), void *arg);
pthread_t   start_set(const cpu_set_t *set, void *(*fn)(void *), void *arg);
void        run_on(int cpu, void *(*fn)(void *), void *arg);

/* ---- sysinfo.c ----------------------------------------------------------------- */

/* The performance governor and energy preference on every CPU (the
 * plan's method), unless as_is; prints what it found and set. */
void sys_performance(bool as_is);
/* The header lines: CPU, kernel, command line, mitigations, idle, load. */
void sys_header(void);
/* "default" or "mitigations-off", from /proc/cmdline. */
const char *sys_boot_tag(void);
/* A file's first line into buf (newline dropped); false if unreadable. */
bool read_line(const char *path, char *buf, size_t n);

/* ---- the lines ----------------------------------------------------------------- */

void bench_local(void);      /* basic.c: the timestamp, a lock, malloc, a context switch */
void bench_user(void);       /* basic.c: system calls, the clock, page faults, mmap */
void bench_sleep(void);      /* basic.c: how late a sleep wakes */
void bench_wake_same(void);  /* cross.c: block+wake on P */
void bench_cross(void);      /* cross.c: cache lines, block+wake, IPIs: P against P2, HT, E */
void bench_tlb(void);        /* cross.c: a TLB shootdown to every other CPU */
void bench_ipc(void);        /* ipc.c: calls between processes and threads */
void bench_files(const char *dir);   /* files.c: the per-operation lines */

/* The futex system call (cross.c). */
struct timespec;
long futex_op(uint32_t *u, int op, uint32_t val, const struct timespec *ts);
