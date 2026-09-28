/* Loader entry. Zero the frame pointer so backtraces stop cleanly here. */
    .section .text
    .global _start
_start:
    xor %rbp, %rbp
    call limine_entry
1:  cli
    hlt
    jmp 1b

    .section .note.GNU-stack, "", @progbits
