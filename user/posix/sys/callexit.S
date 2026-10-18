/* A thread's end that unmaps its stack for calls (callstack.c), which it
 * may be running on: vmar_unmap and thread_exit issued here with
 * registers only, as musl's __unmapself override does for the thread's
 * own stack. The kernel stores 0 at the thread's tid word, and wakes it,
 * after the thread has left user space for good. */
#include <jam/syscall_nums.h>

    .text
    .globl __jam_call_stack_exit
    .hidden __jam_call_stack_exit
    .type __jam_call_stack_exit, @function
/* _Noreturn void __jam_call_stack_exit(unsigned vmar, uintptr_t base,
 * size_t size): edi, rsi and rdx are vmar_unmap's already. */
__jam_call_stack_exit:
    mov $SYS_vmar_unmap, %eax
    syscall                         /* the stack may be gone from here on */
    mov $SYS_thread_exit, %eax
    syscall
    ud2
    .size __jam_call_stack_exit, . - __jam_call_stack_exit

    .section .note.GNU-stack, "", @progbits
