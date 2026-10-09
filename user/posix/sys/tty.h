/* The terminal descriptor (tty.c): a kind of the descriptor table (fd.h)
 * over the console's POSIX side (console.open_tty: the `tty` channel and
 * its two pipes; docs/M13-PLAN.md, "The terminal layer"). */
#pragma once

#include "fd.h"

/* The name ttyname gives a console's terminal: /dev/tty, the process's
 * terminal (B2's /dev opens it again as tty_controlling). */
#define TTY_NAME "/dev/tty"

/* A terminal of the console behind client channel con (console.open_tty;
 * con stays the caller's): 0 and *out (one reference), or -errno. */
long tty_open(handle_t con, uint32_t status, struct fd_desc **out);

/* No hand-over, and the starter gave a console channel (SR_CONSOLE): its
 * terminal at 0 and 2, and at 1 too unless the starter gave SR_STDOUT
 * (stdfd.c's default takes 1 then). Returns the descriptors set, as bits
 * (1 << fd): 0 when there is no console or no terminal could be opened
 * (said in the log). */
unsigned tty_stdfds(void);

/* A hand-over's terminal (POSIXFDS_TTY: the tty channel, the input pipe,
 * the output pipe): fd.c's fd_adopters. */
fd_adopt_fn tty_adopt;

/* The process's terminal: the first terminal descriptor it got (the one
 * /dev/tty names), with a reference the caller fd_puts, or NULL. */
struct fd_desc *tty_controlling(void);

/* ---- SIGWINCH's and ISIG's source, for the signal thread (B4) ----------------------
 * The console tells a terminal's client of a resize (the `tty` protocol's
 * size event) and of ISIG's keys when no shell is below it to take them
 * (its signal event) on the tty channel. Nobody reads them until the
 * signal thread does: it waits on tty_event_handle(d) for SIG_READABLE
 * and calls tty_take_signals(d), which reads every event queued and
 * returns the signals they raise. */
handle_t tty_event_handle(struct fd_desc *d);
/* The signals d's queued events raise, as bits (1ull << signo): SIGWINCH,
 * SIGINT, SIGQUIT. 0 if d is not a terminal. */
uint64_t tty_take_signals(struct fd_desc *d);
