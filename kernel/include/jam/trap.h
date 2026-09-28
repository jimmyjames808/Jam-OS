#pragma once

#include <stdint.h>

/* Layout must match isr.S: pushed GPRs, then vector + error code, then the
 * hardware frame. */
struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

void trap_dispatch(struct trap_frame *f);
