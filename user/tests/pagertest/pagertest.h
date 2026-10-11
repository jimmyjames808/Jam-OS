/* pagertest's own pieces: main.c (the commands on real files) and self.c
 * (libos's mapped-file code on VMOs that are no pager's). */
#pragma once

/* pagertest self: run the checks, print "pagertest: self: PASS" or the
 * failed check. 0 if all passed, else 1. */
int pagertest_self(void);
