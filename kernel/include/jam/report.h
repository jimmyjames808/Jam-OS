/* Result lines worth reading off the real PC's screen. report() prints a
 * line like kprintf and also keeps it; report_print() repeats every kept
 * line in one box at the end of the boot, so the user only has to read the
 * bottom of the screen. */
#pragma once

#include <stdint.h>

void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The same for one line a process reported (debug_report): marked in the
 * log as `writer`'s (klog_write_from). */
void report_from(uint64_t writer, const char *line);
/* Print the box; `version` goes in its first line. */
void report_print(const char *version);
