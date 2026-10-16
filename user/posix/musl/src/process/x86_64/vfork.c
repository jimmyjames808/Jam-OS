/* musl's vfork on Jam OS: overrides upstream's src/process/x86_64/vfork.s
 * (user/posix/musl/OVERRIDES), which makes Linux's vfork in assembly
 * because its child borrows the parent's memory and stack. Jam OS's vfork
 * is a fork (docs/FORK-PLAN.md section 3, the owner's Q3 (A)): the child
 * has memory of its own, so an ordinary call returns in both; the layer's
 * lx_vfork (user/posix/sys/fork.c) is fork's core, where the parent will
 * wait for the child's exec or exit (FL4). Like upstream's, it runs no
 * pthread_atfork handlers and takes none of musl's locks. */
#include <unistd.h>
#include "syscall.h"

pid_t vfork(void)
{
	return __syscall_ret(__syscall(SYS_vfork));
}
