/* Shared by smp.c (the CPU list, the wait, the per-CPU bring-up every AP
 * runs), apboot.c (the kernel's own startup: INIT-SIPI-SIPI through a
 * real-mode trampoline) and trampoline.S. Also included by the assembly,
 * so the C part is behind __ASSEMBLER__.
 *
 * The trampoline page: the code from offset 0 (a SIPI starts the AP at
 * CS = vector << 8, IP = 0), then a data block at fixed offsets. The blob
 * in trampoline.S holds offsets relative to the page; apboot.c copies it
 * into the page and adds the page's address where marked. */
#pragma once

#define TR_GDT    0x100   /* 4 descriptors: null, 64-bit code, data, 32-bit code */
#define TR_GDTR   0x120   /* limit (16 bits), base (32 bits, + the page) */
#define TR_FAR32  0x128   /* far pointer to the 32-bit code: offset (+ the page), selector */
#define TR_FAR64  0x130   /* far pointer to the 64-bit code: offset (+ the page), selector */
#define TR_CR3    0x138   /* 32 bits: the transition PML4 (below 4 GiB) */
#define TR_EFER   0x13c   /* low 32 bits of EFER: LME, and NXE if the CPU has NX */
#define TR_ENTRY  0x140   /* 64 bits: ap_boot64, in the kernel's high half */
#define TR_END    0x148

/* The trampoline GDT's selectors. 0x08 and 0x10 are the kernel GDT's
 * code and data selectors too. */
#define TR_SEL_CODE64 0x08
#define TR_SEL_DATA   0x10
#define TR_SEL_CODE32 0x18

/* struct ap_slot, as ap_boot64 reads it. */
#define SLOT_APIC_ID 0
#define SLOT_STACK   8
#define SLOT_SIZE    24

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>

struct cpu;

/* One AP the trampoline may start: ap_boot64 finds its slot by APIC ID.
 * Written by the BSP before the IPIs, read-only while APs start. */
struct ap_slot {
    uint32_t    apic_id;     /* the x2APIC ID (or the 8-bit initial APIC ID) */
    uint32_t    reserved;    /* 0 */
    void       *stack_top;   /* its kernel stack, the one it keeps for idle */
    struct cpu *cpu;         /* its struct cpu */
};

_Static_assert(__builtin_offsetof(struct ap_slot, apic_id) == SLOT_APIC_ID, "ap_slot layout");
_Static_assert(__builtin_offsetof(struct ap_slot, stack_top) == SLOT_STACK, "ap_slot layout");
_Static_assert(sizeof(struct ap_slot) == SLOT_SIZE, "ap_slot layout");

/* apboot.c: the kernel's own startup. Call in this order, BSP only. */
/* Copy the trampoline into its page, build the transition page table and
 * fill a slot per AP. False (logged) if there is no trampoline page or no
 * memory for the tables: nothing was changed then. */
bool apboot_prepare(struct cpu *const *aps, uint32_t n);
/* INIT to every AP, 10 ms, SIPI to every AP, 200 us, SIPI again, 200 us.
 * `skip` (a struct cpu index, 0 = none) gets no IPIs: a test. */
void apboot_kick(struct cpu *const *aps, uint32_t n, uint32_t skip);
/* Stop a CPU the BSP has given up on: INIT leaves it waiting for a SIPI. */
void apboot_stop(const struct cpu *c);
/* After the wait: let any INIT from apboot_stop land, park the trampoline
 * (a stray SIPI only halts its CPU), free the transition tables. */
void apboot_finish(bool stopped_any);

/* smp.c: the bring-up both startups end in, on the AP's own kernel stack
 * and the kernel's page tables, with EFER.NXE, WP, PGE and the PAT set. */
_Noreturn void smp_ap_main(struct cpu *c);

#endif
