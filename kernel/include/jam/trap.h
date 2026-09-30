/* The register frame every interrupt and exception entry builds
 * (arch/x86_64/isr.S) and hands to trap_dispatch (arch/x86_64/trap.c). */
#pragma once

#include <stdint.h>

/* Layout must match isr.S: pushed GPRs, then vector + error code, then the
 * hardware frame. */
struct trap_frame {
    /* General registers, in the order isr.S saves them. */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;              /* vector; error code (0 if the CPU pushes none) */
    uint64_t rip, cs, rflags, rsp, ss;   /* pushed by the CPU */
};

void trap_dispatch(struct trap_frame *f);
