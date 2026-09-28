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

void gdt_init_bsp(void);
void idt_init(void);
void idt_load(void);
