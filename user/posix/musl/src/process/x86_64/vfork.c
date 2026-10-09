/* musl's vfork on Jam OS: overrides upstream's src/process/x86_64/vfork.s
 * (user/posix/musl/OVERRIDES). Jam OS has no fork of any kind (M13-PLAN,
 * "What M13 is not"): a program starts another with posix_spawn. */
#include <errno.h>
#include <unistd.h>
#include "syscall.h"

pid_t vfork(void)
{
	return __syscall_ret(-ENOSYS);
}
