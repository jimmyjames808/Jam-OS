/* Stand-in kinds for the standard descriptors (stdfd.c) until their own
 * tracks build them: a pipe end and the null device (B2's pipe.c and
 * dev.c replace these, and their lines in fd.c's fd_adopters), the log
 * (descriptors 1 and 2 when the starter gave no channel), and fcntl's
 * commands that need nothing but the table (B2's fcntl replaces them). */
#pragma once

#include "fd.h"

/* No hand-over (docs/M13-PLAN.md section 5): a console channel
 * (SR_CONSOLE) makes its terminal 0 to 2 (tty.c), but 1 when the starter
 * gave SR_STDOUT; without one 0 is the null device. 1 and 2 not a
 * terminal share one description, the SR_STDOUT channel if the starter
 * gave one, else the log (debug_write), as a native program's printf.
 * Called by fd.c's start, after musl is up. */
void stdfd_default(void);

/* The hand-over's pipes and null devices (fd_adopt_fn): another /dev
 * node is left out (-ENODEV) until /dev is built. */
fd_adopt_fn stdfd_adopt_pipe, stdfd_adopt_dev;
