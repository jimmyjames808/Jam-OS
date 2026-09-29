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

/* The CPU has PCIDs (and PGE: the kernel's entries must be global) and
 * "nopcid" is not on the command line. Decided once, at the first call. */
bool     pcid_usable(void);
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
/* Statistics for tests: loads that kept a PCID's entries / flushed them. */
uint64_t pcid_kept_loads(uint32_t cpu);
uint64_t pcid_flushed_loads(uint32_t cpu);
#ifndef JAM_NO_KTESTS
#define PCID_SLOTS_PER_CPU 8
#define PCID_TEST_KEEP 0x10000
uint32_t pcid_test_decide(uint32_t cpu, uint64_t id, uint64_t gen, bool sw);
#endif
