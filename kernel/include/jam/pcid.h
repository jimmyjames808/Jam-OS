/* PCIDs: tagged TLB entries per address space (arch/x86_64/pcid.c).
 *
 * Every user address space has a unique 64-bit id and a TLB generation,
 * bumped each time entries of it are removed or narrowed (aspace.c,
 * gather_note). Each CPU keeps a few PCID slots, each remembering which
 * address space it last held and the generation it had then. Loading an
 * address space whose slot is current keeps its TLB entries; a stale slot
 * (or a new one) is loaded with a flush of that PCID. See pcid.c for the
 * ordering argument with the shootdown. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Are PCIDs to be used on this machine? pcid_decide says, from the CPU
 * (cpu_features) and the boot words; decided once, at the first call. */
bool     pcid_usable(void);
/* What the decision looks at. */
struct pcid_cpu_info {
    bool     has_pcid, has_pge;   /* CPUID: PCIDs; global pages (the kernel's entries) */
    bool     intel;               /* vendor GenuineIntel */
    uint32_t family, model;       /* CPUID leaf 1, extended fields folded in */
    uint32_t microcode;           /* the running revision, 0 if unknown */
    bool     word_off, word_on;   /* boot words "nopcid", "forcepcid" */
};
/* The decision, and in *why a few words for the boot log. In order: no
 * PCIDs or no PGE: off. "nopcid": off. "forcepcid": on. An Intel model
 * whose INVLPG may leave global entries while PCIDs are on (Alder Lake,
 * Raptor Lake, Gracemont) with microcode older than the first fixed
 * revision: off. Otherwise on. *fixed: that revision for an affected
 * model, else 0. */
bool     pcid_decide(const struct pcid_cpu_info *c, const char **why, uint32_t *fixed);
/* One boot log line: the decision and its reason. */
void     pcid_report(void);
/* A new address-space id (never 0, never reused). */
uint64_t pcid_new_id(void);
/* Interrupts off. Load CR3 for an address space: its PML4, its id (0 for
 * the kernel's own tables) and its generation, read here AFTER the caller
 * has published this CPU in the address space's active mask. */
void     pcid_load(uint64_t pml4, uint64_t id, const uint64_t *gen);
/* The run-time switch (boot "nopcid" = never enabled). Off: every address
 * space runs as PCID 0 and each load flushes it. Changing it
 * makes every CPU forget its slots. */
void     pcid_set(bool on);
bool     pcid_is_on(void);
/* Statistics for tests: loads that flushed a PCID's entries. */
uint64_t pcid_flushed_loads(uint32_t cpu);
#ifndef JAM_NO_KTESTS
#define PCID_SLOTS_PER_CPU 8
#define PCID_TEST_KEEP 0x10000
uint32_t pcid_test_decide(uint32_t cpu, uint64_t id, uint64_t gen, bool sw);
/* The fake CPUs as new: a test starts from empty slots every time it runs. */
void     pcid_test_reset(void);
#endif
