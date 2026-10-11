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
 * call, which the layer answers.
 *
 * Each call site also names its call's handler (__jam_need): a relocation
 * of type R_X86_64_NONE (it changes no byte) against the handler's symbol,
 * __jl_lx_<name> as libc.a holds it (user/posix/sys/calls.def). The table
 * in calls.c names the handlers only weakly, so a program links the
 * handlers its call sites name and no others. A site whose number isn't a
 * constant once inlined names __jam_calls_all, every handler; the calls
 * that can be cancellation points (syscall_cp's, called by number, not
 * inline) are always linked. */

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

hidden long __jam_syscall(long n, long a, long b, long c, long d, long e, long f);

#define LX(name) case SYS_##name: \
	__asm__ volatile (".reloc ., R_X86_64_NONE, __jl_lx_" #name); break;
#define LX_CP(name)
static __inline __attribute__((__always_inline__)) void __jam_need(long n)
{
	if (!__builtin_constant_p(n)) {
		__asm__ volatile (".reloc ., R_X86_64_NONE, __jam_calls_all");
		return;
	}
	switch (n) {
#include "../../../sys/calls.def"
	}
}
#undef LX
#undef LX_CP

static __inline __attribute__((__always_inline__)) long __syscall0(long n)
{
	__jam_need(n);
	return __jam_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline __attribute__((__always_inline__)) long __syscall1(long n, long a1)
{
	__jam_need(n);
	return __jam_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline __attribute__((__always_inline__)) long __syscall2(long n, long a1, long a2)
{
	__jam_need(n);
	return __jam_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline __attribute__((__always_inline__)) long __syscall3(long n, long a1, long a2, long a3)
{
	__jam_need(n);
	return __jam_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline __attribute__((__always_inline__)) long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	__jam_need(n);
	return __jam_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline __attribute__((__always_inline__)) long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	__jam_need(n);
	return __jam_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline __attribute__((__always_inline__)) long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	__jam_need(n);
	return __jam_syscall(n, a1, a2, a3, a4, a5, a6);
}

#define IPC_64 0
