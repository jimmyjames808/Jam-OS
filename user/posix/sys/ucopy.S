/* The checked copies (ucopy.h says what each does and how handlers use
 * them). Every load or store of the caller's memory is marked by `ex`,
 * which puts its address and its fixup's in the exception table
 * (uc_table .. uc_table_end) that uc_fault (ucopy.c) searches. No routine
 * pushes anything or moves RSP, so at any marked instruction RSP is at the
 * return address, and the one fixup, fail, returns -EFAULT to the caller.
 * Only the marked instructions: a fault on our own side of a copy (a bug
 * in the layer) is still SIGSEGV. */

#define EFAULT 14
#define PAGE   4096

/* The table's line for the instruction at `at`: it and its fixup, each
 * as an offset from its own field (no absolute address in the table). */
.macro ex at
    .pushsection .rodata.uc_table, "a"
    .long \at - .
    .long fail - .
    .popsection
.endm

    .section .rodata.uc_table, "a"
    .balign 4
    .globl uc_table
    .hidden uc_table
uc_table:

    .section .text.uc_copy, "ax", @progbits

/* long uc_get(void *dst, const void *src, size_t n), and uc_put: the
 * same copy, named for which side is the caller's. */
    .globl uc_get, uc_put
    .type uc_get, @function
    .type uc_put, @function
    .p2align 4
uc_get:
uc_put:
    .cfi_startproc
    mov %rdx, %rcx
1:  rep movsb
    ex 1b
    xor %eax, %eax
    ret
    .cfi_endproc
    .size uc_get, . - uc_get
    .size uc_put, . - uc_put

/* long uc_zero(void *dst, size_t n) */
    .globl uc_zero
    .type uc_zero, @function
    .p2align 4
uc_zero:
    .cfi_startproc
    mov %rsi, %rcx
    xor %eax, %eax
1:  rep stosb
    ex 1b
    ret
    .cfi_endproc
    .size uc_zero, . - uc_zero

/* long uc_strnlen(const char *s, size_t max): a byte at a time (paths:
 * tens of bytes). */
    .globl uc_strnlen
    .type uc_strnlen, @function
    .p2align 4
uc_strnlen:
    .cfi_startproc
    xor %eax, %eax
1:  cmp %rsi, %rax
    jae 3f
2:  cmpb $0, (%rdi,%rax)
    ex 2b
    je 3f
    inc %rax
    jmp 1b
3:  ret
    .cfi_endproc
    .size uc_strnlen, . - uc_strnlen

/* long uc_readable(const void *p, size_t n): a load from p and from the
 * start of each later page up to p + n. A range past the top of memory
 * is -EFAULT. */
    .globl uc_readable
    .type uc_readable, @function
    .p2align 4
uc_readable:
    .cfi_startproc
    xor %eax, %eax
    test %rsi, %rsi
    jz 3f
    add %rdi, %rsi            /* the end */
    jc fail
1:  movzbl (%rdi), %ecx
    ex 1b
    and $-PAGE, %rdi
    add $PAGE, %rdi           /* the next page (none past the last: done) */
    jc 3f
    cmp %rsi, %rdi
    jb 1b
3:  ret
    .cfi_endproc
    .size uc_readable, . - uc_readable

/* long uc_writable(void *p, size_t n): as uc_readable, with a locked add
 * of 0 to each page's byte (a write the CPU checks, which changes
 * nothing, even for a thread writing that byte at the same time). */
    .globl uc_writable
    .type uc_writable, @function
    .p2align 4
uc_writable:
    .cfi_startproc
    xor %eax, %eax
    test %rsi, %rsi
    jz 3f
    add %rdi, %rsi
    jc fail
1:  lock addb $0, (%rdi)
    ex 1b
    and $-PAGE, %rdi
    add $PAGE, %rdi
    jc 3f
    cmp %rsi, %rdi
    jb 1b
3:  ret
    .cfi_endproc
    .size uc_writable, . - uc_writable

/* Every marked instruction's fixup: -EFAULT to the routine's caller. */
    .type fail, @function
    .p2align 4
fail:
    .cfi_startproc
    mov $-EFAULT, %rax
    ret
    .cfi_endproc
    .size fail, . - fail

    .section .rodata.uc_table, "a"
    .globl uc_table_end
    .hidden uc_table_end
uc_table_end:

    .section .note.GNU-stack, "", @progbits
