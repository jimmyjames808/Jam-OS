/* The kernel's entry, from Limine or from another Jam OS kernel (kexec).
 * Limine enters with every general register 0 (its PROTOCOL.md); kexec
 * enters with rdi = KEXEC_ENTRY_MAGIC and rsi = the handoff
 * (<jam/kexec_handoff.h>), both on a stack of their own. Zero the frame
 * pointer so backtraces stop cleanly here. */
#include <jam/kexec_handoff.h>

    .section .text
    .global _start
_start:
    xor %rbp, %rbp
    movabs $KEXEC_ENTRY_MAGIC, %rax
    cmp %rax, %rdi
    je 2f
    call limine_entry
1:  cli
    hlt
    jmp 1b
2:  mov %rsi, %rdi
    call kexec_entry
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
