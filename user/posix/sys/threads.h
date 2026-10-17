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

/* The ids of this process's threads (/proc/<pid>/task): the first thread's,
 * then those clone made. Up to cap of them, from the from'th, into out;
 * how many there are in all. */
unsigned thread_tids(uint32_t *out, unsigned from, unsigned cap);
/* Is tid exactly the id of one of our threads (a process's own id is not
 * its first thread's here, as it is for thread_apply)? */
bool thread_tid_exists(uint32_t tid);
/* The CPU time (ns) the thread with exactly this id has used, into *ns.
 * 0, or -ESRCH if it is no thread of ours. */
long thread_cpu_ns(uint32_t tid, uint64_t *ns);
