/* Program entry. The kernel (userboot or process_start) starts the first
 * thread here with rdi = the startup channel handle, rsp = the top of a
 * fresh stack and every other register zero. */
    .section .text.start, "ax"
    .globl _start
    .type _start, @function
_start:
    xor %ebp, %ebp          /* end of the frame-pointer chain for backtraces */
    and $-16, %rsp          /* the ABI wants rsp 16-aligned at the call */
    call libos_start        /* rdi passes straight through; never returns */
    ud2
    .size _start, . - _start

    .section .note.GNU-stack, "", @progbits
