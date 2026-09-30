/* Time: the TSC, its calibration, and uptime in nanoseconds.
 *
 * Every time in the kernel is a uint64_t in ns of uptime unless its name
 * says otherwise (`_ms`, `_tsc`). uptime_ns() comes from the invariant TSC,
 * calibrated once at boot (arch/x86_64/tsc.c) against the HPET or the ACPI
 * PM timer. NS_PER_* are the shared unit constants: use them, don't redefine
 * them. */
#pragma once

#include <stdint.h>

#define NS_PER_US 1000ull
#define NS_PER_MS 1000000ull
#define NS_PER_S  1000000000ull

extern uint64_t tsc_hz;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* Measure the TSC against the HPET or ACPI PM timer, cross-checked with
 * CPUID 0x15 and the loader's estimate. Needs acpi_init first. */
void tsc_calibrate(void);
/* Same, also printing the loader's estimate (0 if none) for comparison. */
void tsc_calibrate_with_loader(uint64_t loader_hz);
void udelay(uint64_t us);
uint64_t uptime_ns(void);
/* A TSC cycle count as nanoseconds. */
uint64_t tsc_to_ns(uint64_t cycles);
/* The TSC value at which uptime_ns() reaches `ns` (UINT64_MAX for
 * UINT64_MAX): for arming timers. */
uint64_t uptime_to_tsc(uint64_t ns);
