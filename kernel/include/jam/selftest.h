/* Checks that ship in every kernel (kernel/debug/): the boot self-tests,
 * the deliberate crash tests, and the stress test. Unlike kernel/test, none
 * of this is left out by `make KTESTS=0`. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Boot-time self-tests, run when "selftest" is on the kernel command line. */
void selftest_run(void);
/* Multi-CPU tests; needs every CPU online. */
void selftest_run_smp(void);
/* Deliberate crashes (selftest.c's table): each has a name ("pf") and a
 * boot word ("testpf"). selftest_crash runs the early ones named on the
 * command line ("testbp", "testpanic", "testpf", "testro", "testrohhdm",
 * "teststack"), before the scheduler starts; selftest_crash_smp the ones
 * that need the scheduler and all CPUs ("testlockorder", "testlocknest",
 * "testlockirq", "testmutexorder", "testmutexspin", "teststuck",
 * "testwatchdog", "testsmap", "testsmep"). */
void selftest_crash(const char *cmdline);
void selftest_crash_smp(void);
/* The same tests on a running system (debug_command "crash <name>",
 * from a kernel thread). Run one by name: returns only if it didn't crash
 * (bp: 0), ERR_NOT_FOUND for an unknown name, ERR_NOT_SUPPORTED if it
 * needs 2 CPUs. List them in the log (returns how many). */
bool    selftest_crash_known(const char *name, size_t len);
int64_t selftest_crash_run(const char *name);
int64_t selftest_crash_list(void);
/* Stress test for `seconds`; returns true if every check held. */
bool stress_run(uint64_t seconds);
/* n (at least 2) of the stress test's workers as a background load, until
 * stress_load_stop, which makes the end-of-run checks and returns how many
 * checks failed (each failure is a "stress: FAILED" report line). false if
 * a load or a stress run is already going. Callers are the debug commands
 * and the boot's main thread: never two at once. */
bool     stress_load_start(uint32_t n);
uint64_t stress_load_stop(void);
