/* Descriptors 0 to 2 before the descriptor table (stdfd.c), as the start
 * (user/posix/crt/start.c) sets them up. */
#pragma once

#include "layer.h"

/* <posixfds.h> includes <os.h> itself: it comes after layer.h, which
 * includes <os.h> the way musl's headers need (<jamos/libjam.h>). */
#include <posixfds.h>

/* No hand-over message (docs/M13-PLAN.md section 5): 0 is the null
 * device; 1 and 2 are the SR_STDOUT channel if the starter gave one, else
 * the log (debug_write), as a native program's printf. A terminal
 * (SR_CONSOLE) becomes 0 to 2 once terminals have a POSIX side; until
 * then such a program writes to the log, as a native program in the
 * foreground does. */
void     stdfd_default(void);
/* A hand-over's descriptor (posixfds_sink.fd): a pipe or the null device
 * at 0, 1 or 2 is taken; anything else is closed and left out (the
 * descriptor table will take every kind). Always OK. */
status_t stdfd_take(const struct posixfds_fd *rec, const void *data, const handle_t *hs);
/* The hand-over failed after stdfd_take: close and forget all of them. */
void     stdfd_reset(void);
