/* Exception and interrupt entry stubs. Each pushes a dummy error code if the CPU did not,
 * then the vector, then jumps to the common path that saves the GPRs in
 * struct trap_frame order and calls trap_dispatch.
 *
 * GS: ring 3 runs with the user's GS base. isr_common swaps when the
 * saved CS is a user selector, on the way in and out; nothing that uses
 * isr_common can arrive in a swapgs window, because every window runs
 * with interrupts off and can't fault. NMI, #MC and #DB can arrive there,
 * so they come in on IST stacks through isr_ist: from ring 3 they move to
 * the thread's kernel stack and join isr_common (so a #DB from user code
 * can kill the thread like any other fault); from ring 0 they take
 * isr_paranoid, which asks the GS base MSR whose GS is loaded. #DF always
 * takes isr_paranoid: its saved CS is undefined. */
#include <jam/entry_asm.h>

    .section .text

.macro ISR_NOERR n, target=isr_common
    .global isr\n
isr\n:
    pushq $0
    pushq $\n
    jmp \target
.endm

.macro ISR_ERR n, target=isr_common
    .global isr\n
isr\n:
    pushq $\n
    jmp \target
.endm

ISR_NOERR 0
ISR_NOERR 1, isr_ist
ISR_NOERR 2, isr_ist
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8, isr_paranoid
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18, isr_ist
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* Vectors 32-255: interrupts, never an error code. */
.altmacro
.set vec, 32
.rept 224
    ISR_NOERR %vec
    .set vec, vec + 1
.endr
.noaltmacro

.macro PUSH_GPRS
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro POP_GPRS
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
.endm

/* Stack on arrival: vector, error, rip, cs, rflags, rsp, ss. */
isr_common:
    testb $3, 24(%rsp)
    jz isr_common_gs_ok
    swapgs
isr_common_gs_ok:
    cld
    /* Interrupt gates don't clear AC: an interrupt inside a user copy, or
     * from user code that set AC itself, would otherwise run with SMAP
     * off. iretq restores the interrupted AC. */
    CLAC_IF_SMAP
    PUSH_GPRS
    movq %rsp, %rdi
    /* Keep the ABI's 16-byte alignment for the C handler. */
    movq %rsp, %rbx
    andq $-16, %rsp
    call trap_dispatch
    /* Off from here to iretq: an interrupt between swapgs and iretq would
     * see a kernel CS and run on the user's GS. */
    cli
    movq %rbx, %rsp
    POP_GPRS
    addq $16, %rsp          /* vector + error code */
    testb $3, 8(%rsp)
    jz 1f
    swapgs
1:  iretq

/* NMI, #MC, #DB on their IST stacks. */
isr_ist:
    testb $3, 24(%rsp)
    jz isr_paranoid
    /* From ring 3: the GS base is the user's for certain, and the thread's
     * kernel stack is empty. Copy the hardware frame there, so the IST
     * stack is free again for the next NMI and the handler may sleep. */
    swapgs
    pushq %rax
    pushq %rcx
    movq %rsp, %rcx         /* rcx, rax, vector, error, rip, cs, rflags, rsp, ss */
    movq %gs:PERCPU_KERNEL_RSP, %rsp
    pushq 64(%rcx)
    pushq 56(%rcx)
    pushq 48(%rcx)
    pushq 40(%rcx)
    pushq 32(%rcx)
    pushq 24(%rcx)
    pushq 16(%rcx)
    movq 8(%rcx), %rax
    movq (%rcx), %rcx
    jmp isr_common_gs_ok

/* From ring 0, maybe inside a swapgs window: the kernel's GS base is a
 * higher-half address, the user's never is (FSGSBASE is off, so user code
 * can't set one). ebx remembers whether to swap back. */
isr_paranoid:
    cld
    PUSH_GPRS
    CLAC_IF_SMAP
    movl $ENTRY_MSR_GS_BASE, %ecx
    rdmsr
    xorl %ebx, %ebx
    testl %edx, %edx
    js 1f
    swapgs
    movl $1, %ebx
1:  movq %rsp, %rdi
    movq %rsp, %r12
    andq $-16, %rsp
    call trap_dispatch
    cli
    movq %r12, %rsp
    testl %ebx, %ebx
    jz 2f
    swapgs
2:  POP_GPRS
    addq $16, %rsp
    iretq

    .section .rodata
    .global isr_table
isr_table:
.macro ISR_ADDR n
    .quad isr\n
.endm
.altmacro
.set vec, 0
.rept 256
    ISR_ADDR %vec
    .set vec, vec + 1
.endr
.noaltmacro

    .section .note.GNU-stack, "", @progbits
