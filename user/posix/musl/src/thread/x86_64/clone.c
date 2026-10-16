/* musl's __clone on Jam OS: overrides upstream's src/thread/x86_64/clone.s
 * (user/posix/musl/OVERRIDES), which makes Linux's clone itself.
 *
 * A thread of this process (CLONE_VM and CLONE_THREAD: pthread_create) is
 * thread_create and thread_start, its TLS as the FS base, in
 * user/posix/sys/thread.c. Anything else is a process: a fork whose child
 * runs fn (musl's clone() without CLONE_VM, gtest's death tests), in
 * user/posix/sys/fork.c, which refuses a child sharing our memory
 * (CLONE_VM alone). Both of musl's callers pass ptid, tls and ctid. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <errno.h>
#include <sched.h>
#include "glue.h"
#include "pthread_impl.h"

int __clone(int (*fn)(void *), void *stack, int flags, void *arg, ...)
{
	va_list ap;
	va_start(ap, arg);
	int *ptid = va_arg(ap, int *);
	void *tls = va_arg(ap, void *);
	int *ctid = va_arg(ap, int *);
	va_end(ap);
	if ((flags & (CLONE_VM | CLONE_THREAD)) == (CLONE_VM | CLONE_THREAD))
		return __jam_clone(fn, stack, flags, arg, ptid, tls, ctid);
	return __jam_clone_process(fn, stack, flags, arg, ptid, tls, ctid);
}
