/* System call entry contract between the entry path (syscall.S) and the
 * dispatch table (generated from abi/syscalls.def).
 *
 * Convention: `syscall` with the number in rax, arguments in rdi rsi rdx
 * r10 r8 r9, result in rax (status_t or a value; negative = error). At most
 * six register arguments: bigger calls pass a pointer to an argument
 * struct. Syscall numbers live in <jam/syscall_nums.h> (generated). */
#pragma once

#include <stdint.h>

#define SYSCALL_MAX_ARGS 6

/* Built by the entry stub on the thread's kernel stack. */
struct syscall_frame {
    uint64_t nr;                      /* rax on entry */
    uint64_t args[SYSCALL_MAX_ARGS];  /* rdi rsi rdx r10 r8 r9 */
    uint64_t user_rip;                /* rcx */
    uint64_t user_rflags;             /* r11 */
    uint64_t user_rsp;                /* rsp */
    /* The callee-saved registers, so the frame holds the whole
     * user register state (restored on sysret; rcx/r11 are clobbered). */
    uint64_t rbx, rbp, r12, r13, r14, r15;
};

/* Runs with interrupts on. The return value goes back to user rax. */
int64_t syscall_dispatch(struct syscall_frame *f);
