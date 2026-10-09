/* musl's system calls on Jam OS: overrides upstream's
 * arch/x86_64/syscall_arch.h (user/posix/musl/OVERRIDES).
 *
 * Every wrapper in musl ends in __syscallN(SYS_x, ...) with Linux's call
 * numbers and argument shapes. Upstream's turns that into the `syscall`
 * instruction; ours calls __jam_syscall (user/posix/sys/calls.c), which
 * switches on the Linux number into C code over Jam OS's own calls. So
 * no musl code issues the instruction, and the register rule (the vector
 * registers are the caller's across a call) holds by itself: this is an
 * ordinary C call. An unknown number is -ENOSYS.
 *
 * No vDSO (VDSO_USEFUL is left undefined): musl's clock_gettime makes the
 * call, which the layer answers. */

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

hidden long __jam_syscall(long n, long a, long b, long c, long d, long e, long f);

static __inline long __syscall0(long n)
{
	return __jam_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long n, long a1)
{
	return __jam_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long n, long a1, long a2)
{
	return __jam_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	return __jam_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	return __jam_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return __jam_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return __jam_syscall(n, a1, a2, a3, a4, a5, a6);
}

#define IPC_64 0
