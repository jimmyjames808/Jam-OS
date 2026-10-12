/* The threads of this process by id (thread.c), for the calls that name
 * one: setpriority, sched_setaffinity and their kin. A tid is Linux's: 0
 * is the caller, and the process's own id (getpid) is its first thread,
 * as on Linux. Another process's id is no thread of ours: ESRCH. */
#pragma once

#include "layer.h"

/* What to do to a thread, given its handle. */
typedef status_t thread_op(handle_t thread, void *arg);

/* Run op on the thread tid of this process, with its handle held so the
 * thread cannot close it meanwhile (so op is short and takes no lock of
 * thread.c's). 0, -ESRCH if there is no such thread, or the errno of the
 * status op returned. */
long thread_apply(long tid, thread_op *op, void *arg);
