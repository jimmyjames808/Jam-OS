#pragma once

/* Boot-time self-tests, run when "selftest" is on the kernel command line. */
void selftest_run(void);
/* Deliberate crashes selected by name on the command line
 * ("testpf", "testro", "teststack", "testpanic", "testbp"). */
void selftest_crash(const char *cmdline);
