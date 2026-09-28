#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Boot-time self-tests, run when "selftest" is on the kernel command line. */
void selftest_run(void);
/* Multi-CPU tests; needs every CPU online. */
void selftest_run_smp(void);
/* Deliberate crashes selected by name on the command line
 * ("testpf", "testro", "teststack", "testpanic", "testbp"). */
void selftest_crash(const char *cmdline);
/* Deliberate crashes that need the scheduler and all CPUs
 * ("testlockorder", "teststuck", "testwatchdog"). */
void selftest_crash_smp(void);
/* Stress test for `seconds`; returns true if every check held. */
bool stress_run(uint64_t seconds);
