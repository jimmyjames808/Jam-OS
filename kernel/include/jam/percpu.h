/* Per-CPU state, reached through GS: gs:0 holds a pointer to the struct.
 * M5 adds swapgs on user entry; until then GS is always the kernel's. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_CPUS 256

enum core_type { CORE_UNKNOWN, CORE_PERFORMANCE, CORE_EFFICIENCY };

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
};

struct cpu {
    struct cpu    *self;          /* must stay first: this_cpu() reads gs:0 */
    uint32_t       index;         /* 0 = BSP, dense */
    uint32_t       lapic_id;
    uint32_t       acpi_uid;
    enum core_type type;
    uint32_t       core_id;       /* x2APIC id with the SMT bits dropped */
    uint32_t       smt_id;
    volatile bool  online;
    volatile uint64_t ticks;
    void          *kstack_top;

    uint64_t       gdt[9] __attribute__((aligned(16)));
    struct tss     tss;
};

extern struct cpu *cpus[MAX_CPUS];
extern uint32_t    cpu_count;

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

/* Load this CPU's GDT/TSS/IDT and point GS at it. */
void percpu_load(struct cpu *c);
