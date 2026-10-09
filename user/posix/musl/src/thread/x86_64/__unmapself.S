/* musl's __unmapself on Jam OS: overrides upstream's
 * src/thread/x86_64/__unmapself.s (user/posix/musl/OVERRIDES).
 *
 * A detached thread's last steps: unmap its own stack, then end, without
 * touching the stack in between. Its own handle is closed first, in C,
 * while the stack is still there (__jam_unmapself_prepare, which also
 * gives the address space's handle); then vmar_unmap and thread_exit are
 * issued here with registers only. No C runs after the `syscall`s, so the
 * register rule (the vector registers are the caller's) has nothing to
 * protect. The kernel stores 0 at the thread's tid word, and wakes it,
 * after this thread has left user space for good. */
#include <jam/syscall_nums.h>

    .text
    .global __unmapself
    .hidden __unmapself
    .type __unmapself, @function
__unmapself:                        /* rdi = base, rsi = size */
    mov %rdi, %rbx                  /* callee-saved; this never returns */
    mov %rsi, %r12
    sub $8, %rsp                    /* 16-aligned at the call */
    call __jam_unmapself_prepare    /* eax = the address space's handle */
    mov %eax, %edi
    mov %rbx, %rsi
    mov %r12, %rdx
    mov $SYS_vmar_unmap, %eax
    syscall                         /* the stack is gone from here on */
    mov $SYS_thread_exit, %eax
    syscall
    ud2
    .size __unmapself, . - __unmapself

    .section .note.GNU-stack, "", @progbits
