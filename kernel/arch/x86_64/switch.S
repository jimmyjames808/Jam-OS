/* Thread context switch: only callee-saved registers need saving, since
 * switch_context is an ordinary function call from the C scheduler.
 *
 * void switch_context(uint64_t *save_rsp, uint64_t load_rsp) */
    .section .text
    .global switch_context
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* First code a new thread runs: switch_context "returns" here with the
 * entry function in r12 and its argument in r13. */
    .global thread_start
thread_start:
    movq %r12, %rdi
    movq %r13, %rsi
    xorq %rbp, %rbp
    call thread_entry
    ud2

    .section .note.GNU-stack, "", @progbits
