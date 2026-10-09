/* musl's cancellation-point call on Jam OS: overrides upstream's
 * src/thread/x86_64/syscall_cp.s (user/posix/musl/OVERRIDES).
 *
 * __syscall_cp_asm(&self->cancel, nr, u, v, w, x, y, z): if the thread has
 * a cancellation pending, __cancel; else the call, as __jam_syscall(nr,
 * u, ..., z), by a tail jump (its arguments moved down one place; z goes
 * where y was on the stack, which is ours to write: the System V ABI
 * gives a callee its stack arguments). Between __cp_begin and __cp_end is
 * the window musl's cancellation handler looks for; nothing sends that
 * signal until cancellation is built on thread_interrupt (M13-PLAN
 * section 6), which replaces this file and src/thread/pthread_cancel.c. */

    .text
    .global __cp_begin
    .hidden __cp_begin
    .global __cp_end
    .hidden __cp_end
    .global __cp_cancel
    .hidden __cp_cancel
    .hidden __cancel
    .global __syscall_cp_asm
    .hidden __syscall_cp_asm
    .type __syscall_cp_asm, @function
__syscall_cp_asm:
__cp_begin:
    mov (%rdi), %eax
    test %eax, %eax
    jnz __cp_cancel
    mov %rsi, %rdi                  /* nr */
    mov %rdx, %rsi                  /* u */
    mov %rcx, %rdx                  /* v */
    mov %r8, %rcx                   /* w */
    mov %r9, %r8                    /* x */
    mov 8(%rsp), %r9                /* y */
    mov 16(%rsp), %rax              /* z, the seventh: on the stack */
    mov %rax, 8(%rsp)
    jmp __jam_syscall
__cp_end:
__cp_cancel:
    jmp __cancel
    .size __syscall_cp_asm, . - __syscall_cp_asm

    .section .note.GNU-stack, "", @progbits
