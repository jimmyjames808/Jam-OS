/* The process's exception entry (docs/SIGNALS-PLAN.md section 1;
 * sigfault.c says when it is set and what it does). The kernel enters
 * here on the faulting thread with RDI its struct exc_frame (<jam/abi.h>)
 * and RSP just below the frame, at a 0 return address, as just after a
 * call. sig_fault (C) does the rest and never returns.
 *
 * The unwind table says this frame's caller is the interrupted code with
 * every register as the frame holds it, so a debugger or an unwinder that
 * reads DWARF expressions (libgcc's, LLVM's libunwind) walks from a
 * handler through the fault into the code that faulted, as through
 * Linux's signal trampoline. RBX holds the frame's address from the
 * second instruction on (sig_fault keeps it, as any callee keeps RBX). It is a
 * signal frame (.cfi_signal_frame): the caller's pc is the faulting
 * instruction itself, not a return address.
 *
 * __jam_sig_moved is the same frame on another stack (SG4: a handler
 * without SA_ONSTACK runs on the interrupted stack, with a copy of the
 * frame there, so a nested fault, which the kernel serves on the alternate
 * stack again, can't touch what the handler's thread still needs);
 * __jam_sig_stack_call runs a function on another stack and comes back
 * (a handler with SA_ONSTACK for a signal delivered at a call boundary). */

/* struct exc_frame's offsets (sigfault.c checks them against <jam/abi.h>). */
#define F_R8   48
#define F_R9   56
#define F_R10  64
#define F_R11  72
#define F_R12  80
#define F_R13  88
#define F_R14  96
#define F_R15  104
#define F_RDI  112
#define F_RSI  120
#define F_RBP  128
#define F_RBX  136
#define F_RDX  144
#define F_RAX  152
#define F_RCX  160
#define F_RSP  168
#define F_RIP  176

/* DWARF's x86-64 register numbers (the psABI's): 16 is the return address. */
#define D_RAX 0
#define D_RDX 1
#define D_RCX 2
#define D_RBX 3
#define D_RSI 4
#define D_RDI 5
#define D_RBP 6
#define D_RA  16

/* Register `reg` is saved at RBX + off: DW_CFA_expression (0x10), its
 * register, an expression of 3 bytes: DW_OP_breg3 (0x73) and off as a
 * two-byte SLEB128 (any offset below 8192). */
.macro saved reg, off
    .cfi_escape 0x10, \reg, 0x03, 0x73, ((\off) & 0x7f) | 0x80, ((\off) >> 7) & 0x3f
.endm

/* A signal frame whose registers are the exc_frame at RBX's. */
.macro frame_cfi
    .cfi_signal_frame
    /* The CFA is the interrupted RSP: DW_CFA_def_cfa_expression (0x0f), 4
     * bytes: DW_OP_breg3 F_RSP, DW_OP_deref (0x06). */
    .cfi_escape 0x0f, 0x04, 0x73, (F_RSP & 0x7f) | 0x80, (F_RSP >> 7) & 0x3f, 0x06
    saved D_RAX, F_RAX
    saved D_RDX, F_RDX
    saved D_RCX, F_RCX
    saved D_RBX, F_RBX
    saved D_RSI, F_RSI
    saved D_RDI, F_RDI
    saved D_RBP, F_RBP
    saved 8, F_R8
    saved 9, F_R9
    saved 10, F_R10
    saved 11, F_R11
    saved 12, F_R12
    saved 13, F_R13
    saved 14, F_R14
    saved 15, F_R15
    saved D_RA, F_RIP
.endm

    .text
    .globl __jam_sig_entry
    .type __jam_sig_entry, @function
    .p2align 4
__jam_sig_entry:
    .cfi_startproc
    frame_cfi
    mov %rdi, %rbx
    and $-16, %rsp            /* aligned for the call (the frame is above) */
    call sig_fault
    ud2                       /* sig_fault never returns */
    .cfi_endproc
    .size __jam_sig_entry, . - __jam_sig_entry

/* void __jam_sig_moved(struct exc_frame *copy, void *run, void *top):
 * on the stack below top (16-byte aligned), sig_fault_run(run) for the
 * frame copy; never returns. */
    .globl __jam_sig_moved
    .type __jam_sig_moved, @function
    .p2align 4
__jam_sig_moved:
    .cfi_startproc
    frame_cfi
    mov %rdi, %rbx
    mov %rdx, %rsp
    mov %rsi, %rdi
    call sig_fault_run
    ud2                       /* sig_fault_run never returns */
    .cfi_endproc
    .size __jam_sig_moved, . - __jam_sig_moved

/* void __jam_sig_stack_call(void (*fn)(void *), void *arg, void *top):
 * fn(arg) on the stack below top (16-byte aligned), then back on this
 * one. RBP holds this frame's place meanwhile, so an unwinder walks from
 * fn into the caller. */
    .globl __jam_sig_stack_call
    .type __jam_sig_stack_call, @function
    .p2align 4
__jam_sig_stack_call:
    .cfi_startproc
    push %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset 6, -16
    mov %rsp, %rbp
    .cfi_def_cfa_register 6
    mov %rdx, %rsp
    mov %rdi, %rax
    mov %rsi, %rdi
    call *%rax
    mov %rbp, %rsp
    pop %rbp
    .cfi_def_cfa 7, 8
    ret
    .cfi_endproc
    .size __jam_sig_stack_call, . - __jam_sig_stack_call

    .section .note.GNU-stack, "", @progbits
