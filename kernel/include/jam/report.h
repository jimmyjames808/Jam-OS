/* Result lines worth reading off the real PC's screen. report() prints a
 * line like kprintf and also keeps it; report_print() repeats every kept
 * line in one box at the end of the boot, so the user only has to read the
 * bottom of the screen.
 *
 * The box holds 256 lines: the first 192 and the newest 64. A boot that
 * reports more loses the ones in between and the box says how many
 * ("N result lines didn't fit"), so a test's summary and the verdict, which
 * come last, are never the lines lost. */
#pragma once

#include <stddef.h>
#include <stdint.h>

void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The same for one line a process reported (debug_report): marked in the
 * log as `writer`'s (klog_write_from). */
void report_from(uint64_t writer, const char *line);
/* A program's debug_report line went past the cap on those (the line is in
 * the log only): counted, and the box says how many. */
void report_refused(void);
/* Print the box; `version` goes in its first line. */
void report_print(const char *version);

/* A box of its own, for the ktest of the keeping rules: report_box_keep
 * adds a line, report_box_shown says how many lines print (the note about
 * the lost ones is one of them), report_box_line copies line i of those. */
struct report_box;
struct report_box *report_box_new(void);
void report_box_free(struct report_box *b);
void report_box_keep(struct report_box *b, const char *line);
unsigned report_box_shown(const struct report_box *b);
void report_box_line(const struct report_box *b, unsigned i, char *out, size_t cap);
