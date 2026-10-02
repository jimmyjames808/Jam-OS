/* CPU features and per-CPU descriptor tables. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct cpu_features {
    bool nx;                                /* no-execute pages (EFER.NXE) */
    bool pages_1g;                          /* 1 GiB pages */
    bool pat;                               /* page attribute table (WC mappings) */
    bool pge;                               /* global pages */
    bool x2apic;                            /* x2APIC mode possible */
    bool tsc_invariant;                     /* TSC runs at a constant rate in every C-state */
    bool hybrid;                            /* Intel P-core/E-core (CPUID 7.EDX[15]) */
    bool tsc_deadline;                      /* LAPIC timer TSC-deadline mode */
    bool smep, smap, umip;                  /* CPUID 7: supervisor-mode protections, UMIP */
    bool xsave, avx;                        /* XSAVE (CPUID 1.ECX[26]); AVX usable with it */
    bool pcid, invpcid;                     /* CPUID 1.ECX[17], 7.EBX[10] */
    bool pku, pks, waitpkg;                 /* CPUID 7.ECX[3], [31], [5] */
    bool cet_ss, cet_ibt, uintr;            /* CPUID 7.ECX[7], 7.EDX[20], 7.EDX[5] */
    bool mtrr, tsc_adjust;                  /* CPUID 1.EDX[12], 7.EBX[1] (IA32_TSC_ADJUST) */
    bool rdrand, rdseed;                    /* CPUID 1.ECX[30], 7.EBX[18] (random.c) */
    uint32_t family, model, stepping;       /* CPUID 1.EAX, the extended fields folded in */
    uint32_t microcode;                     /* the running microcode revision, 0 if unknown */
    uint32_t max_leaf;                      /* highest basic CPUID leaf */
    uint32_t crystal_hz;                    /* CPUID 0x15, 0 if not reported */
    uint32_t tsc_ratio_num, tsc_ratio_den;  /* CPUID 0x15: TSC = crystal * num / den */
    char vendor[13];                        /* "GenuineIntel", NUL-terminated */
    char brand[49];                         /* CPUID brand string, NUL-terminated */
};

extern struct cpu_features cpu_features;
extern uint8_t smap_on;   /* stac/clac usable (entry_asm.h) */

void cpu_detect(void);
/* EFER.NXE, CR0.WP, CR4.PGE and the PAT layout used by the VMM. */
void cpu_enable_paging_features(void);
/* Per-CPU setup for ring 3, run by every CPU once it is on the kernel's
 * page tables and its own GDT (percpu_load): SMEP, SMAP, UMIP, FSGSBASE
 * off, the FPU/XSAVE state (fpu.c) and the syscall MSRs (uentry.c). */
void cpu_init_local(void);

/* What firmware sets up on every CPU and the kernel does not: the
 * microcode, the MTRRs (Intel SDM vol. 3A, 12.11.8: they must match on
 * every CPU) and IA32_TSC_ADJUST. cpu_snapshot_bsp records the BSP's, once,
 * before the APs start. cpu_match_bsp, on an AP with interrupts off,
 * compares, loads the BSP's MTRRs if this CPU's differ (INIT keeps the
 * MTRRs on real CPUs, QEMU's TCG resets them), and returns what differed
 * (CPU_DIFF_*) and this CPU's microcode revision. */
#define CPU_DIFF_MICROCODE  (1u << 0)
#define CPU_DIFF_MTRR       (1u << 1)   /* differ, and could not be fixed */
#define CPU_DIFF_MTRR_SET   (1u << 2)   /* differed; now the BSP's */
#define CPU_DIFF_TSC_ADJUST (1u << 3)
void     cpu_snapshot_bsp(void);
unsigned cpu_match_bsp(uint32_t *microcode);

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
#define IST_DEBUG         4
#define IST_COUNT         4

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
