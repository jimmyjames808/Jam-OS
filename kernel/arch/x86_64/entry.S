/* Loader entry. Zero the frame pointer so backtraces stop cleanly here. */
    .section .text
    .global _start
_start:
    xor %rbp, %rbp
    call limine_entry
1:  cli
    hlt
    jmp 1b


/* _Noreturn void stack_switch_call(void *top, void (*fn)(void *), void *arg)
 * Moves onto a new stack and calls fn(arg) with a fresh frame chain. */
    .section .text
    .global stack_switch_call
stack_switch_call:
    mov %rdi, %rsp
    xor %rbp, %rbp
    mov %rdx, %rdi
    call *%rsi
    ud2

    .section .note.GNU-stack, "", @progbits
