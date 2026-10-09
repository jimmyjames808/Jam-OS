/* The only code that touches user memory. SMAP is opened (stac) only
 * around the access itself. A fault on the user side lands in
 * trap_page_fault, which first asks the address space to resolve it
 * (demand paging, or waiting for a pager; the copy is retried) and
 * otherwise resumes at the fixup named in __ex_table for the faulting
 * instruction, with the fault's status (a status_t, sign-extended; never
 * -1) in %rax: each fixup returns it. Nothing here keeps %rax live across
 * a faulting instruction except copy_str_user_raw's index, which the fault
 * ends anyway. */
#include <jam/entry_asm.h>

.macro EX_ENTRY insn, fixup
    .pushsection __ex_table, "a"
    .balign 8
    .quad \insn, \fixup
    .popsection
.endm

    .section .text

/* status_t copy_user_raw(void *dst, const void *src, size_t n)
 * Returns 0 (OK) once all n bytes are copied, else the fault's status
 * (ERR_INVALID_ARGS, ERR_IO or ERR_CANCELED); part of the bytes may have
 * been copied by then. */
    .global copy_user_raw
copy_user_raw:
    movq %rdx, %rcx
    STAC_IF_SMAP
1:  rep movsb
    CLAC_IF_SMAP
    xorl %eax, %eax
    ret
2:  CLAC_IF_SMAP            /* rax = the fault's status (trap_page_fault) */
    ret
    EX_ENTRY 1b, 2b

/* long copy_str_user_raw(char *dst, const char *usrc, size_t n)
 * Copies up to n bytes, stopping after a NUL. Returns the string length
 * (the NUL was copied), -1 if n bytes held no NUL, or on a fault its
 * status (negative, never -1). */
    .global copy_str_user_raw
copy_str_user_raw:
    xorl %eax, %eax
    STAC_IF_SMAP
1:  cmpq %rdx, %rax
    jae 3f
2:  movb (%rsi,%rax), %cl
    movb %cl, (%rdi,%rax)
    testb %cl, %cl
    jz 4f
    incq %rax
    jmp 1b
3:  CLAC_IF_SMAP
    movq $-1, %rax
    ret
4:  CLAC_IF_SMAP
    ret
5:  CLAC_IF_SMAP            /* rax = the fault's status (trap_page_fault) */
    ret
    EX_ENTRY 2b, 5b

    .section .note.GNU-stack, "", @progbits
