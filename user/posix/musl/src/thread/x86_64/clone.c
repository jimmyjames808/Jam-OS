/* musl's __clone on Jam OS: overrides upstream's src/thread/x86_64/clone.s
 * (user/posix/musl/OVERRIDES), which makes Linux's clone itself.
 *
 * Only a thread of this process can be made (CLONE_VM and CLONE_THREAD):
 * thread_create and thread_start, its TLS as the FS base, in
 * user/posix/sys/thread.c. Anything else (musl's posix_spawn and fork
 * clone a child process) is -ENOSYS: Jam OS has no fork of any kind.
 * Those callers pass no ptid, tls or ctid, so the variable arguments are
 * read only for a thread. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <errno.h>
#include <sched.h>
#include "glue.h"
#include "pthread_impl.h"

int __clone(int (*fn)(void *), void *stack, int flags, void *arg, ...)
{
	if ((flags & (CLONE_VM | CLONE_THREAD)) != (CLONE_VM | CLONE_THREAD))
		return -ENOSYS;
	va_list ap;
	va_start(ap, arg);
	int *ptid = va_arg(ap, int *);
	void *tls = va_arg(ap, void *);
	int *ctid = va_arg(ap, int *);
	va_end(ap);
	return __jam_clone(fn, stack, flags, arg, ptid, tls, ctid);
}
