/* pagertest's own pieces: main.c (the commands on real files), self.c
 * (libos's mapped-file code on VMOs that are no pager's) and write.c (a
 * mapped file changed under its mapping). */
#pragma once

/* pagertest self: run the checks, print "pagertest: self: PASS" or the
 * failed check. 0 if all passed, else 1. */
int pagertest_self(void);
/* pagertest write <path>: make the file, map it, change it through an
 * open of its own in every way, and check the mapping after each change;
 * print "pagertest: write <path>: PASS" or the first MISMATCH. 0 if it
 * passed, 1 on a mismatch, 2 if the file couldn't be made or mapped. */
int pagertest_write(const char *path);
/* pagertest rec ...: records stored through a writable shared mapping and
 * checked (rec.c's header has the commands); argv is what follows "rec". */
int pagertest_rec(int argc, char **argv);
