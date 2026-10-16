/* User entry and exit (arch/x86_64/uentry.c, syscall.S).
 *
 * The user half of every address space is [USER_BASE, USER_TOP). Page 0
 * and the last page below the canonical hole are never mapped. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/status.h>

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

/* The current thread's FS base in ring 3 (thread-local storage), from now
 * on: ERR_INVALID_ARGS at or past USER_TOP, ERR_BAD_STATE for a thread
 * without user state. Its first entry to ring 3 loads current_thread()->
 * fs_base, so a thread's first value is set there before it starts. */
status_t thread_set_fs_base(uint64_t base);

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

/* A fault of user code that would kill its process (an unresolvable page
 * fault, #GP, #UD...), `why` its kill line's reason (a static string),
 * addr and status a page fault's address and aspace_fault status (0 for
 * the other vectors). With an exception entry that may take it (upcall.c,
 * <jam/abi.h>) the fault is delivered: f now enters the entry, and the
 * caller returns to ring 3 through user_trap_return. Otherwise the
 * process is killed (logged; process_kill) and the thread leaves; a
 * ring-3 thread with no process (the kernel's own entry-path tests) just
 * exits. Interrupts in either state; returns with them off. */
void user_fault(struct trap_frame *f, const char *why, uint64_t addr, status_t status);
/* For upcall.c (uentry.c, isr.S): the kill itself, its line with `more`
 * appended (NULL: none), the process's end naming the fault (vector, a
 * page fault's status: process_kill_fault); the work before any return to
 * ring 3 (interrupts off: a pending reschedule, a kill); and the return to
 * ring 3 with every register from f (interrupts off, f checked). */
_Noreturn void user_kill(const char *why, uint64_t rip, uint64_t addr, const char *more,
                         uint32_t vector, status_t status);
void return_to_user_work(void);
void user_trap_return(struct trap_frame *f);
_Noreturn void user_resume_iret(struct trap_frame *f);
/* A raise latched on the current thread (thread_raise), from
 * user_trap_return: enter its exception entry with a frame of kind
 * EXC_RAISED and f's registers, or drop it (no entry, or a frame that
 * can't be written); outside the window only (there it stays latched).
 * Interrupts off on entry and return; return_to_user_work is done again
 * if it turned them on. */
void user_raise(struct trap_frame *f);

/* RFLAGS bits user code may carry back into ring 3 (sysret, an
 * exception_done EXC_RESUME). IOPL, NT, RF, VM and friends are never the
 * user's to set; IF is always on. */
#define USER_RFLAGS_OK (RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | \
                        RFLAGS_TF | RFLAGS_DF | RFLAGS_OF | RFLAGS_AC | RFLAGS_ID)

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
/* Interrupts off, t switching out from inside a system call: keep only its
 * MXCSR and x87 control word, so it comes back with clean registers
 * (fpu.c, "The system call rule"). */
void fpu_save_called(struct thread *t);
/* Is the system call rule in force (fpu_call on, and nothing in XCR0 that
 * must never be dropped)? */
bool fpu_call_drop(void);
/* Run-time switches: XSAVEOPT and the skipped restore (boot "nofpuopt");
 * the system call rule (boot "nofpucall"). */
extern bool fpu_opt;
extern bool fpu_call;
uint32_t fpu_area_size(void);
/* A fault's frame (upcall.c; fpu.c says the form): the XSAVE area's size
 * in a frame (0 if bigger than FPU_FRAME_MAX, the buffer the kernel builds
 * a frame in: no upcalls then); t's registers into a zeroed area
 * (interrupts off, t current from ring 3); whether a frame's area may be
 * restored; and t continuing with one (interrupts off). */
#define FPU_FRAME_MAX 1024u
uint32_t fpu_frame_size(void);
void     fpu_frame_save(struct thread *t, uint8_t *out);
bool     fpu_frame_ok(const uint8_t *area);
void     fpu_frame_load(struct thread *t, const uint8_t *area);
