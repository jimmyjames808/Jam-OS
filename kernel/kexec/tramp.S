/* kexec's trampoline: the last instructions of one kernel and the first
 * a new one runs before its own entry.
 *
 * This page is mapped read-execute at its link address, at T in the vmap
 * area (region.c), and at T in the new kernel's page tables (image.c), so
 * it goes on running across the CR3 switch. It is entered through T,
 * with interrupts off, from jump.c:
 *
 *   void kexec_tramp(uint64_t cr3, uint64_t entry, uint64_t handoff, uint64_t stack)
 *                    rdi           rsi             rdx               rcx
 *
 * It touches no memory but the new stack, so it is position-independent
 * and needs nothing of the old kernel once CR3 is loaded. The new kernel
 * gets what Limine would give it (PROTOCOL.md): interrupts off, an empty
 * IDT (any fault from here on is a triple fault, a firmware reset, never
 * a hang), a 16-byte aligned stack with a 0 return address pushed, and
 * the TLB empty: CR4.PGE and CR4.PCIDE are turned off after the switch,
 * which drops every entry of every PCID, global ones included (Intel SDM
 * vol. 3 4.10.4.1), so no translation of the old kernel survives into the
 * new one. The new kernel turns both on again itself. */

#include <jam/kexec_handoff.h>

#define CR4_PGE   (1 << 7)
#define CR4_PCIDE (1 << 17)

    .section .text.kexec_tramp, "ax"
    .balign 4096
    .global kexec_tramp
kexec_tramp:
    cli
    mov %rsi, %r8              /* entry */
    mov %rdi, %cr3             /* the new tables: PCID 0, so PCIDE may go off below */
    mov %cr4, %rax
    and $~(CR4_PGE | CR4_PCIDE), %rax
    mov %rax, %cr4             /* every TLB entry gone */
    mov %rcx, %rsp
    pushq $0
    pushq $0
    lidt (%rsp)                /* limit 0, base 0: no IDT */
    mov %rcx, %rsp
    pushq $0                   /* the return address Limine pushes too */
    xor %ebp, %ebp
    movabs $KEXEC_ENTRY_MAGIC, %rdi
    mov %rdx, %rsi             /* the handoff */
    jmp *%r8
    ud2
    .global kexec_tramp_end
kexec_tramp_end:
    .balign 4096

    .section .note.GNU-stack, "", @progbits
