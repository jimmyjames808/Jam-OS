/* User entry and exit (arch/x86_64/uentry.c, syscall.S).
 *
 * The user half of every address space is [USER_BASE, USER_TOP). Page 0
 * and the last page below the canonical hole are never mapped. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USER_BASE 0x0000000000001000ull
#define USER_TOP  0x00007fffffffe000ull

/* True if [addr, addr+len) lies inside the user range (len 0 allowed). */
static inline bool user_range_ok(uint64_t addr, uint64_t len)
{
    return addr >= USER_BASE && addr <= USER_TOP && len <= USER_TOP - addr;
}

struct thread;
struct trap_frame;

/* First entry of the current thread into ring 3 (iretq), at `entry` with
 * stack `stack`, rdi = arg0, rsi = arg1, every other register zero and the
 * default FPU state. The thread must already have an address space
 * (current_thread()->aspace) and a user FPU area. */
_Noreturn void arch_enter_user(uint64_t entry, uint64_t stack, uint64_t arg0, uint64_t arg1);

/* Called by schedule() with interrupts off and the run queue lock held,
 * just before switching from prev to next: TSS rsp0 and the per-CPU
 * syscall stack for next, the user FPU state (save prev's, load next's),
 * and the address space (aspace_switch). */
void arch_thread_switch(struct thread *prev, struct thread *next);

/* Page fault hook, called by trap_dispatch for vector 14 before it
 * panics. Returns true if the fault was handled (resolved by aspace_fault,
 * redirected to a user-copy fixup, or the faulting user thread was
 * killed), false for a genuine kernel fault. */
bool trap_page_fault(struct trap_frame *f);

/* A user thread did something fatal (unresolvable fault, #GP, #UD...):
 * its process is killed (logged; process_kill) and the thread leaves. A
 * ring-3 thread with no process (the kernel's own entry-path tests) just
 * exits. */
_Noreturn void user_fault_kill(struct trap_frame *f, const char *why);

/* Per-thread user FPU/SSE/AVX state (XSAVE area). 0 on success. */
int  fpu_ustate_alloc(struct thread *t);
void fpu_ustate_free(struct thread *t);
/* Interrupts off (fpu.c): load t's state (skipped when this CPU's registers
 * still hold it); start t from the default state; forget which thread's
 * state the registers hold (after anything else loaded them). */
void fpu_load(struct thread *t);
void fpu_reset_and_load(struct thread *t);
void fpu_clobbered(void);
void fpu_save(void *area);
void fpu_restore(const void *area);
/* Run-time switch: XSAVEOPT and the skipped restore (boot "nofpuopt"). */
extern bool fpu_opt;
uint32_t fpu_area_size(void);
