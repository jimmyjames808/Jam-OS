/* Exception and interrupt entry stubs. Each pushes a dummy error code if the CPU did not,
 * then the vector, then jumps to the common path that saves the GPRs in
 * struct trap_frame order and calls trap_dispatch. */
    .section .text

.macro ISR_NOERR n
    .global isr\n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro ISR_ERR n
    .global isr\n
isr\n:
    pushq $\n
    jmp isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
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

isr_common:
    cld
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
    movq %rsp, %rdi
    /* Keep the ABI's 16-byte alignment for the C handler. */
    movq %rsp, %rbx
    andq $-16, %rsp
    call trap_dispatch
    movq %rbx, %rsp
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
    addq $16, %rsp          /* vector + error code */
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
