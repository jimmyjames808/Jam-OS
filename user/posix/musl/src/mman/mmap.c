/* mmap on Jam OS: overrides upstream's src/mman/mmap.c
 * (user/posix/musl/OVERRIDES).
 *
 * Upstream turns an EPERM for an anonymous mapping into ENOMEM (Linux
 * gave EPERM where it meant "no memory"). Here EPERM for MAP_JIT is the
 * answer itself: the program doesn't hold the JIT power (its list's
 * `right jit`, docs/CR-PLAN.md section 12), which no retry with less
 * memory changes, so it is kept. Otherwise upstream's file as it is. */
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>
#include "syscall.h"

static void dummy(void) { }
weak_alias(dummy, __vm_wait);

#define UNIT SYSCALL_MMAP2_UNIT
#define OFF_MASK ((-0x2000ULL << (8*sizeof(syscall_arg_t)-1)) | (UNIT-1))

void *__mmap(void *start, size_t len, int prot, int flags, int fd, off_t off)
{
	long ret;
	if (off & OFF_MASK) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (len >= PTRDIFF_MAX) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	if (flags & MAP_FIXED) {
		__vm_wait();
	}
#ifdef SYS_mmap2
	ret = __syscall(SYS_mmap2, start, len, prot, flags, fd, off/UNIT);
#else
	ret = __syscall(SYS_mmap, start, len, prot, flags, fd, off);
#endif
	/* Fixup incorrect EPERM from kernel (but MAP_JIT's: no power). */
	if (ret == -EPERM && !start && (flags&MAP_ANON) && !(flags&MAP_FIXED) &&
	    !(flags&MAP_JIT))
		ret = -ENOMEM;
	return (void *)__syscall_ret(ret);
}

weak_alias(__mmap, mmap);
