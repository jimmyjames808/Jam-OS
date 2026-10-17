/* musl's __clone on Jam OS: overrides upstream's src/thread/x86_64/clone.s
 * (user/posix/musl/OVERRIDES), which makes Linux's clone itself.
 *
 * A thread of this process (CLONE_VM and CLONE_THREAD: pthread_create) is
 * thread_create and thread_start, its TLS as the FS base, in
 * user/posix/sys/thread.c. Anything else is a process: a fork whose child
 * runs fn (musl's clone() without CLONE_VM, gtest's death tests), in
 * user/posix/sys/fork.c, which refuses a child sharing our memory
 * (CLONE_VM alone). Both of musl's callers pass ptid, tls and ctid.
 *
 * pthread_create's thread is told by its ptid, the new thread's own tid
 * field; its stack's size goes to the layer, which runs a small stack's
 * calls on a stack of its own (user/posix/sys/callstack.c). Anyone
 * else's thread is of a size not known (0). */
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
	if ((flags & (CLONE_VM | CLONE_THREAD)) == (CLONE_VM | CLONE_THREAD)) {
		struct pthread *p = tls;
		unsigned long size = 0;
		if (p && ptid == &p->tid)
			size = (uintptr_t)stack - ((uintptr_t)p->stack - p->stack_size);
		return __jam_clone(fn, stack, flags, arg, ptid, tls, ctid, size);
	}
	return __jam_clone_process(fn, stack, flags, arg, ptid, tls, ctid);
}
