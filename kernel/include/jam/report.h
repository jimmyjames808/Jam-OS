/* Result lines worth reading off the real PC's screen. report() prints a
 * line like kprintf and also keeps it; report_print() repeats every kept
 * line in one box at the end of the boot, so the user only has to read the
 * bottom of the screen. */
#pragma once

void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Print the box; `version` goes in its first line. */
void report_print(const char *version);
