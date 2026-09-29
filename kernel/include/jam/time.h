#pragma once

#include <stdint.h>

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
