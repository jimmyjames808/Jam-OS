/* Constants shared by the entry assembly and C (uentry.c checks them
 * against the C structures with _Static_assert). */
#pragma once

/* struct cpu */
#define PERCPU_USER_RSP   8
#define PERCPU_KERNEL_RSP 16

#define ENTRY_MSR_GS_BASE 0xc0000101

#ifdef __ASSEMBLER__
/* stac/clac are #UD on CPUs without SMAP, so every use is guarded by this
 * byte (set once at boot from CPUID; the same on every CPU). */
.macro STAC_IF_SMAP
    testb $1, smap_on(%rip)
    jz 7771f
    stac
7771:
.endm
.macro CLAC_IF_SMAP
    testb $1, smap_on(%rip)
    jz 7772f
    clac
7772:
.endm
#endif
