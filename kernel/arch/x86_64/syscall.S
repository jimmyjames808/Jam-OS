/* Ring 3 entry and exit: the syscall instruction, and the first iretq
 * into a new user thread.
 *
 * syscall arrives with interrupts, TF, DF, AC and NT cleared (SFMASK), the
 * user's RSP and GS still loaded, rcx = return RIP, r11 = RFLAGS. Until
 * the frame is built nothing may fault or be interrupted: NMI/#MC/#DB find
 * GS themselves (isr.S). */
#include <jam/entry_asm.h>

    .section .text

/* struct syscall_frame, built by pushes in reverse: see jam/syscall.h. */
#define SF_RDI     8
#define SF_RSI     16
#define SF_RDX     24
#define SF_R10     32
#define SF_R8      40
#define SF_R9      48
#define SF_RIP     56
#define SF_RFLAGS  64
#define SF_RSP     72
#define SF_RBX     80
#define SF_RBP     88
#define SF_R12     96
#define SF_R13     104
#define SF_R14     112
#define SF_R15     120

    .global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:PERCPU_USER_RSP
    movq %gs:PERCPU_KERNEL_RSP, %rsp
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbp
    pushq %rbx
    pushq %gs:PERCPU_USER_RSP
    pushq %r11
    pushq %rcx
    pushq %r9
    pushq %r8
    pushq %r10
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rax
    /* No user values in the kernel's callee-saved registers, and a zero
     * frame pointer so backtraces stop at the entry. */
    xorl %ebx, %ebx
    xorl %ebp, %ebp
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    movq %rsp, %rdi
    call syscall_entry_c    /* interrupts on inside; back with them off */
    /* rax = result. Every other register comes back from the frame, so no
     * kernel value reaches user code; rcx/r11 carry RIP/RFLAGS, which
     * syscall_entry_c checked (RIP below USER_TOP: a non-canonical RIP
     * would make sysretq fault in ring 0 on the user's stack). */
    movq SF_RDI(%rsp), %rdi
    movq SF_RSI(%rsp), %rsi
    movq SF_RDX(%rsp), %rdx
    movq SF_R10(%rsp), %r10
    movq SF_R8(%rsp), %r8
    movq SF_R9(%rsp), %r9
    movq SF_RIP(%rsp), %rcx
    movq SF_RFLAGS(%rsp), %r11
    movq SF_RBX(%rsp), %rbx
    movq SF_RBP(%rsp), %rbp
    movq SF_R12(%rsp), %r12
    movq SF_R13(%rsp), %r13
    movq SF_R14(%rsp), %r14
    movq SF_R15(%rsp), %r15
    movq SF_RSP(%rsp), %rsp
    swapgs
    sysretq

/* _Noreturn void enter_user_iret(uint64_t entry, uint64_t stack, uint64_t arg0,
 *                                uint64_t arg1, uint64_t kstack_top)
 * Interrupts are off and the FPU state is loaded. Builds an iretq frame at
 * the top of the thread's kernel stack (whatever the thread ran before is
 * abandoned) and enters ring 3 with every other register zero. */
    .global enter_user_iret
enter_user_iret:
    movq %r8, %rsp
    pushq $(0x20 | 3)       /* ss: GDT_USER_DATA, RPL 3 */
    pushq %rsi              /* rsp */
    pushq $0x202            /* rflags: IF */
    pushq $(0x28 | 3)       /* cs: GDT_USER_CODE, RPL 3 */
    pushq %rdi              /* rip */
    movq %rdx, %rdi
    movq %rcx, %rsi
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    swapgs
    iretq

    .section .note.GNU-stack, "", @progbits
