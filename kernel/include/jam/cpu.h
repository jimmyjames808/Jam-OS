/* CPU features and per-CPU descriptor tables. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct cpu_features {
    bool nx;
    bool pages_1g;
    bool pat;
    bool pge;
    bool x2apic;
    bool tsc_invariant;
    bool hybrid;          /* Intel P-core/E-core (CPUID 7.EDX[15]) */
    bool tsc_deadline;    /* LAPIC timer TSC-deadline mode */
    uint32_t max_leaf;
    uint32_t crystal_hz;  /* CPUID 0x15, 0 if not reported */
    uint32_t tsc_ratio_num, tsc_ratio_den;
    char vendor[13];
    char brand[49];
};

extern struct cpu_features cpu_features;

void cpu_detect(void);
/* EFER.NXE, CR0.WP, CR4.PGE and the PAT layout used by the VMM. */
void cpu_enable_paging_features(void);

/* Segment selectors. The user selectors are laid out for SYSRET:
 * STAR[63:48] = GDT_USER_BASE, so SS = +8 and CS = +16. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_BASE   0x18
#define GDT_USER_DATA   0x20
#define GDT_USER_CODE   0x28
#define GDT_TSS         0x30

/* IST slots (1-based as the hardware sees them). */
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_MACHINE_CHECK 3

/* Early BSP-only GDT/TSS with static IST stacks, used until the per-CPU
 * structures exist. */
void gdt_init_bsp(void);
struct cpu;
/* Per-CPU GDT and TSS with guard-paged IST stacks (needs the heap). */
void gdt_init_cpu(struct cpu *c);
/* Core type and SMT/core ids, from CPUID on the calling CPU. */
void cpu_detect_topology(struct cpu *c);
void idt_init(void);
void idt_load(void);
