/* pthread_cancel on Jam OS: overrides upstream's src/thread/pthread_cancel.c
 * (user/posix/musl/OVERRIDES).
 *
 * Upstream sends the thread SIGCANCEL, whose handler cancels it if it is
 * inside a cancellation point's system call (or at once, for an
 * asynchronous cancel). Signals here are delivered only at call
 * boundaries, so: pthread_cancel sets the flag and interrupts the thread
 * (tkill of SIGCANCEL, which the layer takes as a wake and never as a
 * pending signal); a cancellation point (__syscall_cp_c, then the
 * layer's __jam_syscall_cp through syscall_cp's override) checks the
 * flag before its call and after an EINTR, as upstream does. What is
 * given up: an asynchronous cancel acts at the thread's next
 * cancellation point, not in the middle of its user code.
 *
 * __cancel, __syscall_cp_c and __testcancel are upstream's, unchanged. */
#define _GNU_SOURCE
#include <string.h>
#include "pthread_impl.h"
#include "syscall.h"

hidden long __cancel(), __syscall_cp_asm(), __syscall_cp_c();

long __cancel()
{
	pthread_t self = __pthread_self();
	if (self->canceldisable == PTHREAD_CANCEL_ENABLE || self->cancelasync)
		pthread_exit(PTHREAD_CANCELED);
	self->canceldisable = PTHREAD_CANCEL_DISABLE;
	return -ECANCELED;
}

long __syscall_cp_asm(volatile void *, syscall_arg_t,
                      syscall_arg_t, syscall_arg_t, syscall_arg_t,
                      syscall_arg_t, syscall_arg_t, syscall_arg_t);

long __syscall_cp_c(syscall_arg_t nr,
                    syscall_arg_t u, syscall_arg_t v, syscall_arg_t w,
                    syscall_arg_t x, syscall_arg_t y, syscall_arg_t z)
{
	pthread_t self;
	long r;
	int st;

	if ((st=(self=__pthread_self())->canceldisable)
	    && (st==PTHREAD_CANCEL_DISABLE || nr==SYS_close))
		return __syscall(nr, u, v, w, x, y, z);

	r = __syscall_cp_asm(&self->cancel, nr, u, v, w, x, y, z);
	if (r==-EINTR && nr!=SYS_close && self->cancel &&
	    self->canceldisable != PTHREAD_CANCEL_DISABLE)
		r = __cancel();
	return r;
}

void __testcancel()
{
	pthread_t self = __pthread_self();
	if (self->cancel && !self->canceldisable)
		__cancel();
}

int pthread_cancel(pthread_t t)
{
	a_store(&t->cancel, 1);
	if (t == pthread_self()) {
		if (t->canceldisable == PTHREAD_CANCEL_ENABLE && t->cancelasync)
			pthread_exit(PTHREAD_CANCELED);
		return 0;
	}
	return pthread_kill(t, SIGCANCEL);
}
