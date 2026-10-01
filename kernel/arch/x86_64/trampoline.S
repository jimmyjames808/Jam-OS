/* The application processors' way in: the real-mode trampoline that a
 * SIPI starts (copied by apboot.c into a page below 640 KiB), and
 * ap_boot64, the first kernel code an AP runs.
 *
 * The trampoline goes 16 -> 32 -> 64-bit (Intel SDM vol. 3A, 10.8.5
 * "Initializing IA-32e Mode": PAE, CR3, EFER.LME, then paging). It writes
 * nothing in its page, so any number of APs can run it at once. EBX holds
 * the page's physical address throughout, because the code doesn't know
 * where it was copied. Its layout and the data block's offsets are in
 * smp_internal.h. After the startup apboot.c overwrites it with
 * ap_park_start: a CPU that a stray SIPI wakes just halts. */
#include "smp_internal.h"

#define CR0_PE   0x00000001
#define CR0_WP   0x00010000
#define CR0_PG   0x80000000
#define CR4_PAE  0x00000020
#define MSR_EFER 0xc0000080

    .section .rodata.ap_tramp, "a"
    .global ap_tramp_start, ap_tramp_end, ap_park_start, ap_park_end

    .code16
ap_tramp_start:
    cli
    /* INIT leaves the caches and CR0.CD/NW as they were (SDM vol. 3A,
     * 10.1); from a reset CD and NW are set. Write the caches back before
     * CR0 is rewritten below, whatever mode they were in (as Linux's
     * trampoline does). */
    wbinvd
    mov     %cs, %ax
    mov     %ax, %ds
    movzwl  %ax, %ebx
    shll    $4, %ebx                     /* EBX = the page's physical address */
    lgdtl   TR_GDTR
    movl    $CR0_PE, %eax                /* protected mode, CD and NW clear: caches on */
    movl    %eax, %cr0
    ljmpl   *TR_FAR32

    .code32
tramp32:
    movw    $TR_SEL_DATA, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    xorw    %ax, %ax
    movw    %ax, %fs
    movw    %ax, %gs
    movl    $CR4_PAE, %eax
    movl    %eax, %cr4
    movl    TR_CR3(%ebx), %eax           /* the transition PML4 */
    movl    %eax, %cr3
    movl    $MSR_EFER, %ecx
    movl    TR_EFER(%ebx), %eax          /* LME (+ NXE: the kernel's leaves use NX) */
    xorl    %edx, %edx
    wrmsr
    /* Paging on activates long mode. The page is identity-mapped, so the
     * next instruction is found at the same address. */
    movl    $(CR0_PG | CR0_WP | CR0_PE), %eax
    movl    %eax, %cr0
    ljmpl   *TR_FAR64(%ebx)

    .code64
tramp64:
    movl    %ebx, %ebx                   /* the upper half is undefined after the switch */
    movq    TR_ENTRY(%rbx), %rax
    jmpq    *%rax

    .org    TR_GDT
    .quad   0
    .quad   0x00af9a000000ffff           /* 64-bit code: present, exec/read, L, G */
    .quad   0x00cf92000000ffff           /* data: present, read/write, 4 GiB */
    .quad   0x00cf9a000000ffff           /* 32-bit code: present, exec/read, D, 4 GiB */
    .org    TR_GDTR
    .word   4 * 8 - 1
    .long   TR_GDT                       /* + the page */
    .org    TR_FAR32
    .long   tramp32 - ap_tramp_start     /* + the page */
    .word   TR_SEL_CODE32
    .org    TR_FAR64
    .long   tramp64 - ap_tramp_start     /* + the page */
    .word   TR_SEL_CODE64
    .org    TR_CR3
    .long   0
    .org    TR_EFER
    .long   0
    .org    TR_ENTRY
    .quad   0
    .org    TR_END
ap_tramp_end:

    /* What the page holds after the startup. */
    .code16
ap_park_start:
    cli
1:  hlt
    jmp     1b
ap_park_end:


/* ap_boot64: on the transition page table, in 64-bit mode, interrupts off,
 * no stack. Load the kernel's page table (EFER.NXE is already on), find
 * this CPU's slot by its APIC ID, move onto its kernel stack and call
 * ap_boot_main(slot). CPUID gives the ID in either APIC mode: leaf 0Bh's
 * x2APIC ID where the CPU has the leaf, else leaf 1's 8-bit initial ID.
 * A CPU without a slot halts for good. */
    .section .text
    .code64
    .global ap_boot64
ap_boot64:
    movq    ap_boot_cr3(%rip), %rax
    movq    %rax, %cr3
    xorl    %eax, %eax
    cpuid
    cmpl    $0xb, %eax
    jb      1f
    movl    $0xb, %eax
    xorl    %ecx, %ecx
    cpuid
    testl   %ebx, %ebx                   /* leaf 0Bh not implemented: EBX = 0 */
    jz      1f
    movl    %edx, %r8d
    jmp     2f
1:  movl    $1, %eax
    cpuid
    shrl    $24, %ebx
    movl    %ebx, %r8d
2:  leaq    ap_slots(%rip), %rsi
    movl    ap_slot_count(%rip), %ecx
3:  testl   %ecx, %ecx
    jz      5f
    cmpl    %r8d, SLOT_APIC_ID(%rsi)
    je      4f
    addq    $SLOT_SIZE, %rsi
    decl    %ecx
    jmp     3b
4:  movq    SLOT_STACK(%rsi), %rsp
    movq    %rsi, %rdi
    xorl    %ebp, %ebp
    call    ap_boot_main
    ud2
5:  cli
    hlt
    jmp     5b

    .section .note.GNU-stack, "", @progbits
