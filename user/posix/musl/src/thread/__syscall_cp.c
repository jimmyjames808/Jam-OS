/* musl's cancellation point entry on Jam OS: overrides upstream's
 * src/thread/__syscall_cp.c (user/posix/musl/OVERRIDES).
 *
 * Upstream's fallback, for a program that never links pthread_cancel
 * (sccp, behind the weak __syscall_cp_c), makes the call with __syscall
 * and a number known only when it runs, which the override of
 * syscall_arch.h answers by naming every handler of the layer, and this
 * file is in every program. The calls that come here are cancellation
 * points, whose handlers are always linked (user/posix/sys/calls.def's
 * LX_CP), so the fallback calls the layer directly. The rest is
 * upstream's. */
#include "pthread_impl.h"
#include "syscall.h"

hidden long __syscall_cp_c();

static long sccp(syscall_arg_t nr,
                 syscall_arg_t u, syscall_arg_t v, syscall_arg_t w,
                 syscall_arg_t x, syscall_arg_t y, syscall_arg_t z)
{
	return __jam_syscall(nr, u, v, w, x, y, z);
}

weak_alias(sccp, __syscall_cp_c);

long (__syscall_cp)(syscall_arg_t nr,
                    syscall_arg_t u, syscall_arg_t v, syscall_arg_t w,
                    syscall_arg_t x, syscall_arg_t y, syscall_arg_t z)
{
	return __syscall_cp_c(nr, u, v, w, x, y, z);
}
