/* musl's __set_thread_area on Jam OS: overrides upstream's
 * src/thread/x86_64/__set_thread_area.s (user/posix/musl/OVERRIDES), which
 * makes Linux's arch_prctl(ARCH_SET_FS) itself. The thread pointer is the
 * FS base, set by thread_set_fs_base (user/posix/sys/thread.c). 0 tells
 * musl's __init_tp the process can run threads. */
#include "glue.h"
#include "pthread_impl.h"

int __set_thread_area(void *p)
{
	return __jam_set_thread_area(p);
}
