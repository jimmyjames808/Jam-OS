/* linuxbench: the lines on one CPU (P): the timestamp, a lock, malloc,
 * a context switch, system calls, the clock, page faults, mmap, and how
 * late a sleep wakes.
 *
 * System calls are made with the `syscall` instruction itself (as
 * user/tests/utest/bench.c makes its unused one), so libc's wrapper and
 * errno are not in the time. */
#include "lb.h"   /* first: it defines _GNU_SOURCE before any system header */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NULL_SYSCALL 1000   /* no Linux system call has this number: ENOSYS */
#define PAGE         4096ull

/* ---- batches of BATCH operations on P ----------------------------------------- */

static void (*batch_op)(void);

static void *batch_thread(void *arg)
{
    (void)arg;
    uint64_t end;
    warm_until(&end);
    while (mono_ns() < end)
        batch_op();
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        for (unsigned k = 0; k < BATCH; k++)
            batch_op();
        samples[i] = span_ps(t0, stamp(), BATCH);
    }
    return NULL;
}

static void batch(const char *what, void (*op)(void), const char *how)
{
    batch_op = op;
    run_on(cpu_p, batch_thread, NULL);
    result(what, how, SAMPLES);
}

/* A ticket lock, as Jam OS's spinlock is, without its lock checker. */
static struct {
    uint32_t next __attribute__((aligned(64)));   /* the next ticket */
    uint32_t owner;                               /* the ticket now served */
} ticket;

static void op_lock(void)
{
    uint32_t me = __atomic_fetch_add(&ticket.next, 1, __ATOMIC_ACQUIRE);
    while (__atomic_load_n(&ticket.owner, __ATOMIC_ACQUIRE) != me)
        cpu_relax();
    __atomic_store_n(&ticket.owner, me + 1, __ATOMIC_RELEASE);
}

static void op_malloc(void)
{
    void *volatile p = malloc(64);   /* volatile: the pair can't be optimised away */
    free(p);
}

static long raw_syscall2(long nr, long a, long b)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(nr), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return r;
}

static void op_null(void)
{
    raw_syscall2(NULL_SYSCALL, 0, 0);
}

static void op_getppid(void)
{
    raw_syscall2(SYS_getppid, 0, 0);
}

static struct timespec clock_ts;

static void op_clock_sys(void)
{
    raw_syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&clock_ts);
}

static void op_clock_vdso(void)
{
    clock_gettime(CLOCK_MONOTONIC, &clock_ts);
}

/* ---- context switch ------------------------------------------------------------
 * Two SCHED_FIFO threads of the same priority on P yield to each other:
 * sched_yield puts the caller at the back of its priority's queue, so
 * every yield switches. A sample is BATCH yields of the timer, divided by
 * the switches they made (2 per yield); the switches are checked with
 * the timer thread's own count (getrusage), as bench checks the CPU's. */

static volatile bool yield_done, partner_running;
static uint64_t yield_switches;

static void *yield_partner(void *arg)
{
    (void)arg;
    partner_running = true;
    while (!yield_done)
        sched_yield();
    return NULL;
}

static uint64_t my_switches(void)
{
    struct rusage r;
    getrusage(RUSAGE_THREAD, &r);
    return (uint64_t)r.ru_nvcsw + (uint64_t)r.ru_nivcsw;
}

static void *yield_timer(void *arg)
{
    (void)arg;
    while (!partner_running)
        sched_yield();
    uint64_t end;
    warm_until(&end);
    while (mono_ns() < end)
        sched_yield();
    uint64_t sw0 = my_switches();
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        for (unsigned k = 0; k < BATCH; k++)
            sched_yield();
        samples[i] = span_ps(t0, stamp(), 2 * BATCH);
    }
    yield_switches = my_switches() - sw0;
    yield_done = true;
    return NULL;
}

static void context_switch(void)
{
    const char *what = "context switch (yield, 2 threads, P)";
    yield_done = partner_running = false;
    pthread_t b = start_on(cpu_p, yield_partner, NULL);
    pthread_t a = start_on(cpu_p, yield_timer, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    uint64_t expect = (uint64_t)SAMPLES * BATCH;
    if (yield_switches < expect * 9 / 10) {
        char why[128];
        snprintf(why, sizeof(why), "INVALID, only %llu of ~%llu yields switched",
                 (unsigned long long)yield_switches, (unsigned long long)expect);
        skipped(what, why);
        return;
    }
    result(what, "sched_yield between 2 SCHED_FIFO threads pinned to P", SAMPLES);
}

void bench_local(void)
{
    char cost[24];
    fmt_ps(cost, sizeof(cost), stamp_cost_ps());
    say("bench: %-44s median %-10s p99 %-10s = the same lfence; rdtsc; lfence pair\n",
        "timestamp cost (subtracted from all below)", cost, "-");
    batch("spin_lock + spin_unlock, uncontended (P)", op_lock,
          "a ticket lock in user space: a locked add, a load, a store (no lock checker)");
    batch("kmalloc(64) + kfree (P)", op_malloc,
          "malloc(64) + free: libc's per-thread cache, not a kernel allocator");
    context_switch();
}

/* ---- page faults and mappings -------------------------------------------------- */

static void *fault_thread(void *arg)
{
    (void)arg;
    uint64_t pages = SAMPLES + 256;
    uint8_t *p = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                      -1, 0);
    if (p == MAP_FAILED) {
        memset(samples, 0, sizeof(samples));
        return NULL;
    }
    /* 4 KiB pages, one fault each, as Jam OS's VMO pages are. */
    (void)madvise(p, pages * PAGE, MADV_NOHUGEPAGE);
    for (unsigned i = 0; i < 256; i++)   /* warm-up: the last 256 pages */
        ((volatile uint8_t *)p)[(SAMPLES + i) * PAGE] = 1;
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        ((volatile uint8_t *)p)[i * PAGE] = 1;
        samples[i] = span_ps(t0, stamp(), 1);
    }
    munmap(p, pages * PAGE);
    return NULL;
}

static void *mmap_thread(void *arg)
{
    (void)arg;
    uint64_t end;
    warm_until(&end);
    for (unsigned i = 0; i < SAMPLES;) {
        uint64_t t0 = stamp();
        void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED)
            munmap(p, PAGE);
        uint64_t t1 = stamp();
        if (mono_ns() >= end)
            samples[i++] = span_ps(t0, t1, 1);
    }
    return NULL;
}

void bench_user(void)
{
    batch("user: syscall round trip (unused number, P)", op_null,
          "the syscall instruction, number 1000: ENOSYS");
    batch("user: syscall round trip (getppid, P)", op_getppid,
          "getppid: the cheapest real system call (no Jam OS twin)");
    batch("user: clock_get syscall (P)", op_clock_sys,
          "clock_gettime(CLOCK_MONOTONIC) as a system call");
    batch("user: clock_gettime through the vDSO (P)", op_clock_vdso,
          "libc's clock_gettime: the vDSO, no kernel entry (no Jam OS twin)");
    run_on(cpu_p, fault_thread, NULL);
    result("user: page fault, fresh zero page (P)",
           "first write to a page of an anonymous mmap, no huge pages", SAMPLES);
    run_on(cpu_p, mmap_thread, NULL);
    result("user: mmap + munmap of 1 page, untouched (P)",
           "anonymous, private (no Jam OS twin: vmar_map/unmap)", SAMPLES);
}

/* ---- sleep accuracy ---------------------------------------------------------------
 * As bench's: P sleeps a set time; the sample is how late it woke. A
 * SCHED_FIFO thread has no timer slack. */

static uint64_t sleep_req_ns;

static void *sleep_thread(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, (long)sleep_req_ns };
    for (unsigned i = 0; i < 5; i++)   /* warm-up */
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    for (unsigned i = 0; i < TIMER_SAMPLES; i++) {
        uint64_t t0 = stamp();
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
        uint64_t ps = span_ps(t0, stamp(), 1), req = sleep_req_ns * 1000;
        samples[i] = ps > req ? ps - req : 0;
    }
    return NULL;
}

void bench_sleep(void)
{
    static const unsigned us[] = { 100, 1000 };
    for (unsigned i = 0; i < 2; i++) {
        char what[64];
        sleep_req_ns = us[i] * 1000ull;
        run_on(cpu_p, sleep_thread, NULL);
        snprintf(what, sizeof(what), "sleep %u us (P): how late it wakes", us[i]);
        result(what, "clock_nanosleep(CLOCK_MONOTONIC), SCHED_FIFO", TIMER_SAMPLES);
    }
}
