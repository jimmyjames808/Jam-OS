/* fork's two ends in assembly (fork.c has the rest, <os.h> jam_fork the
 * contract; docs/FORK-PLAN.md section 1.2).
 *
 * jam_fork saves what the caller expects a call to keep (the callee-saved
 * registers, its stack pointer and return address, MXCSR and the x87
 * control word: a setjmp's) in a block on the calling thread's stack and
 * hands it to jam_fork_parent, which copies the address space with the block
 * in it and starts the child's thread at jam_fork_trampoline with the
 * block's address. The caller-saved registers are dead across a call
 * anyway, and so are the vector registers (ARCHITECTURE: the system call
 * ABI). */
#include "fork.h"

    .text
    .globl jam_fork
    .type jam_fork, @function
jam_fork:
    sub $FORK_BLOCK, %rsp           /* 16-aligned now: the call below is as the ABI wants */
    mov %rbx, FORK_B_RBX(%rsp)
    mov %rbp, FORK_B_RBP(%rsp)
    mov %r12, FORK_B_R12(%rsp)
    mov %r13, FORK_B_R13(%rsp)
    mov %r14, FORK_B_R14(%rsp)
    mov %r15, FORK_B_R15(%rsp)
    lea FORK_BLOCK+8(%rsp), %rax    /* the caller's stack pointer once we return */
    mov %rax, FORK_B_RSP(%rsp)
    mov FORK_BLOCK(%rsp), %rax      /* our return address */
    mov %rax, FORK_B_RIP(%rsp)
    stmxcsr FORK_B_MXCSR(%rsp)
    fnstcw FORK_B_FPUCW(%rsp)
    mov %rdi, %rsi                  /* the arguments */
    mov %rsp, %rdi                  /* the block */
    call jam_fork_parent            /* the child's handle, or a status */
    add $FORK_BLOCK, %rsp
    ret
    .size jam_fork, . - jam_fork

/* The child's one thread starts here (process_start): rdi = its startup
 * channel, rsi = the block in its copy of the forking thread's stack, rsp
 * a little below the block, every other register zero and the FPU state
 * clean. jam_fork_child sets the FS base and makes libjam's and the C
 * library's state the child's; then the block's registers go back and the
 * call returns 0 where it was made. */
    .globl jam_fork_trampoline
    .type jam_fork_trampoline, @function
jam_fork_trampoline:
    xor %ebp, %ebp
    and $-16, %rsp
    mov %rsi, %rbx                  /* the block: callee-saved, kept across the call */
    call jam_fork_child
    ldmxcsr FORK_B_MXCSR(%rbx)
    fldcw FORK_B_FPUCW(%rbx)
    mov FORK_B_RBP(%rbx), %rbp
    mov FORK_B_R12(%rbx), %r12
    mov FORK_B_R13(%rbx), %r13
    mov FORK_B_R14(%rbx), %r14
    mov FORK_B_R15(%rbx), %r15
    mov FORK_B_RIP(%rbx), %rcx
    mov FORK_B_RSP(%rbx), %rdx
    mov FORK_B_RBX(%rbx), %rbx      /* the block's last use */
    mov %rdx, %rsp                  /* the block is below us from here on */
    xor %eax, %eax                  /* jam_fork returns 0 */
    jmp *%rcx
    .size jam_fork_trampoline, . - jam_fork_trampoline

    .section .note.GNU-stack, "", @progbits
