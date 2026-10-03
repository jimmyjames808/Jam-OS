/* linuxbench: one thread waking another, and P against the other CPUs:
 * block+wake round trips (a futex ping-pong), the cache line's round
 * trip (the hardware floor, which should match Jam OS's to the ns: a
 * check that the same CPUs were chosen), an IPI round trip (membarrier)
 * and a TLB shootdown (mprotect).
 *
 * A partner on another CPU blocks between rounds, so its CPU goes idle
 * as Linux idles a CPU (the cpuidle driver and governor in the header);
 * Jam OS's idle CPU polls for 10 us first (spinidle), and BENCH.md prints
 * its line with that off and on. */
#include "lb.h"   /* first: it defines _GNU_SOURCE before any system header */

#include <linux/futex.h>
#include <linux/membarrier.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

long futex_op(uint32_t *u, int op, uint32_t val, const struct timespec *ts)
{
    return syscall(SYS_futex, u, op, val, ts, NULL, 0);
}

/* ---- block+wake: a futex ping-pong between two threads ------------------------
 * As bench's wake_round: the initiator sets turn to 1 and wakes the
 * responder, then blocks until it is 0; the responder blocks until turn
 * is 1, sets it to 0 and wakes the initiator. 2 means stop. */

static struct {
    uint32_t turn __attribute__((aligned(64)));
} wpp;

static void wake_round(void)
{
    __atomic_store_n(&wpp.turn, 1, __ATOMIC_RELEASE);
    futex_op(&wpp.turn, FUTEX_WAKE_PRIVATE, 1, NULL);
    while (__atomic_load_n(&wpp.turn, __ATOMIC_ACQUIRE) == 1)
        futex_op(&wpp.turn, FUTEX_WAIT_PRIVATE, 1, NULL);
}

static void *wake_responder(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t t = __atomic_load_n(&wpp.turn, __ATOMIC_ACQUIRE);
        if (t == 2)
            return NULL;
        if (t == 0) {
            futex_op(&wpp.turn, FUTEX_WAIT_PRIVATE, 0, NULL);
            continue;
        }
        __atomic_store_n(&wpp.turn, 0, __ATOMIC_RELEASE);
        futex_op(&wpp.turn, FUTEX_WAKE_PRIVATE, 1, NULL);
    }
}

static void *wake_initiator(void *arg)
{
    (void)arg;
    uint64_t end;
    warm_until(&end);
    while (mono_ns() < end)
        wake_round();
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        wake_round();
        samples[i] = span_ps(t0, stamp(), 1);
    }
    __atomic_store_n(&wpp.turn, 2, __ATOMIC_RELEASE);
    futex_op(&wpp.turn, FUTEX_WAKE_PRIVATE, 1, NULL);
    return NULL;
}

/* The responder on `set`, the initiator on P. */
static void wake_measure(const cpu_set_t *set)
{
    wpp.turn = 0;
    pthread_t r = start_set(set, wake_responder, NULL);
    run_on(cpu_p, wake_initiator, NULL);
    pthread_join(r, NULL);
}

static void wake_on(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    wake_measure(&set);
}

void bench_wake_same(void)
{
    wake_on(cpu_p);
    result("block+wake round trip, same CPU (P)",
           "futex wait/wake ping-pong, 2 threads pinned to P", SAMPLES);
}

/* ---- the cache line's round trip ------------------------------------------------- */

static struct {
    uint64_t flag __attribute__((aligned(64)));
    uint64_t stop __attribute__((aligned(64)));
} line;

static void *line_responder(void *arg)
{
    (void)arg;
    uint64_t seen = 0;
    while (!__atomic_load_n(&line.stop, __ATOMIC_RELAXED)) {
        uint64_t v = __atomic_load_n(&line.flag, __ATOMIC_ACQUIRE);
        if (v != seen && (v & 1)) {
            seen = v + 1;
            __atomic_store_n(&line.flag, seen, __ATOMIC_RELEASE);
        }
        cpu_relax();
    }
    return NULL;
}

static void *line_initiator(void *arg)
{
    (void)arg;
    uint64_t v = 0, end;
    warm_until(&end);
    for (unsigned n = 0; n < SAMPLES;) {
        bool timed = mono_ns() >= end;
        uint64_t t0 = stamp();
        __atomic_store_n(&line.flag, ++v, __ATOMIC_RELEASE);   /* odd: ping */
        while (__atomic_load_n(&line.flag, __ATOMIC_ACQUIRE) != v + 1)
            cpu_relax();
        uint64_t t1 = stamp();
        v++;                                                    /* even: pong seen */
        if (timed)
            samples[n++] = span_ps(t0, t1, 1);
    }
    __atomic_store_n(&line.stop, 1, __ATOMIC_RELAXED);
    return NULL;
}

static void cache_line(int other)
{
    char what[64];
    line.flag = line.stop = 0;
    pthread_t r = start_on(other, line_responder, NULL);
    run_on(cpu_p, line_initiator, NULL);
    pthread_join(r, NULL);
    snprintf(what, sizeof(what), "cache-line round trip P->%s (hardware floor)", kind(other));
    result(what, "the same code: a store, then spin for the answer", SAMPLES);
}

/* ---- IPI: membarrier ---------------------------------------------------------------
 * MEMBARRIER_CMD_PRIVATE_EXPEDITED interrupts every other CPU running a
 * thread of this process and waits until each has run its handler: with
 * one thread spinning on the target, one IPI and its answer. The target
 * is busy in user space, not idle (Jam OS's target idles, polling). */

static volatile bool ipi_stop;

static void *ipi_spinner(void *arg)
{
    (void)arg;
    while (!ipi_stop)
        cpu_relax();
    return NULL;
}

static void *ipi_initiator(void *arg)
{
    (void)arg;
    uint64_t end;
    warm_until(&end);
    while (mono_ns() < end)
        syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0);
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0);
        samples[i] = span_ps(t0, stamp(), 1);
    }
    ipi_stop = true;
    return NULL;
}

static void ipi(int other)
{
    char what[64];
    snprintf(what, sizeof(what), "IPI function call round trip P->%s", kind(other));
    if (syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) != 0) {
        skipped(what, "membarrier's expedited command refused");
        return;
    }
    ipi_stop = false;
    pthread_t s = start_on(other, ipi_spinner, NULL);
    run_on(cpu_p, ipi_initiator, NULL);
    pthread_join(s, NULL);
    result(what, "membarrier(PRIVATE_EXPEDITED), the target spinning in user space", SAMPLES);
}

void bench_cross(void)
{
    int others[] = { cpu_p2, cpu_ht, cpu_e };
    for (unsigned i = 0; i < 3; i++)
        if (others[i] >= 0)
            cache_line(others[i]);
    for (unsigned i = 0; i < 3; i++) {
        if (others[i] < 0)
            continue;
        char what[64];
        wake_on(others[i]);
        snprintf(what, sizeof(what), "block+wake round trip P->%s (idle CPU)", kind(others[i]));
        result(what, "futex ping-pong; the partner's CPU idles as Linux idles it", SAMPLES);
    }
    if (cpu_ht >= 0) {
        /* Unpinned, but kept off CPU 0 and P, as bench's partner is. */
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c = 1; c < MAX_CPUS; c++)
            if (cpu_online(c) && c != cpu_p)
                CPU_SET(c, &set);
        wake_measure(&set);
        result("block+wake round trip P->unpinned partner",
               "futex ping-pong; Linux's scheduler places the partner", SAMPLES);
    }
    for (unsigned i = 0; i < 3; i++)
        if (others[i] >= 0)
            ipi(others[i]);
}

/* ---- TLB shootdown -------------------------------------------------------------------
 * A thread of ours spins on every other online CPU, so each has our
 * address space loaded; P takes write permission off one touched page
 * (mprotect RW->R): Linux must flush it on all of them, by IPI, and waits.
 * Giving it back (R->RW) is timed too: it should need no flush. The
 * spinner on P's sibling shares P's core (it spins with `pause`, which
 * leaves the core to P as far as it can); on Jam OS the other CPUs idle. */

static struct {
    volatile bool stop;    /* the spinners end */
    uint32_t ready;        /* spinners running */
    unsigned others;       /* spinners started */
} shoot;

static void *shoot_spinner(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&shoot.ready, 1, __ATOMIC_RELEASE);
    while (!shoot.stop)
        cpu_relax();
    return NULL;
}

/* On P: start the spinners (one may be on CPU 0, where main waits, so
 * main must not be the one that waits for them), time, stop them. */
static void *shoot_initiator(void *arg)
{
    static pthread_t spin[MAX_CPUS];
    volatile uint8_t *page = arg;
    unsigned n = 0;
    for (int c = 0; c < MAX_CPUS; c++)
        if (cpu_online(c) && c != cpu_p)
            spin[n++] = start_on(c, shoot_spinner, NULL);
    while (__atomic_load_n(&shoot.ready, __ATOMIC_ACQUIRE) < n)
        sched_yield();
    page[0] = 1;
    uint64_t end;
    warm_until(&end);
    while (mono_ns() < end) {
        mprotect((void *)page, 4096, PROT_READ);
        mprotect((void *)page, 4096, PROT_READ | PROT_WRITE);
    }
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t t0 = stamp();
        mprotect((void *)page, 4096, PROT_READ);
        uint64_t t1 = stamp();
        mprotect((void *)page, 4096, PROT_READ | PROT_WRITE);
        samples[i] = span_ps(t0, t1, 1);
        samples2[i] = span_ps(t1, stamp(), 1);
        page[0] = (uint8_t)i;   /* written again: its TLB entry is back each round */
    }
    shoot.stop = true;
    for (unsigned i = 0; i < n; i++)
        pthread_join(spin[i], NULL);
    shoot.others = n;
    return NULL;
}

void bench_tlb(void)
{
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED || ncpus_online < 2)
        return;
    memset(&shoot, 0, sizeof(shoot));
    run_on(cpu_p, shoot_initiator, page);
    munmap(page, 4096);
    char what[64], how[96];
    snprintf(what, sizeof(what), "TLB shootdown, 1 page, %u other CPUs", shoot.others);
    snprintf(how, sizeof(how), "mprotect RW->R of 1 touched page, our threads on all %u",
             shoot.others);
    result(what, how, SAMPLES);
    result_of("mprotect R->RW of the same page",
              "write given back: no flush expected (no Jam OS twin)", samples2, SAMPLES);
}
