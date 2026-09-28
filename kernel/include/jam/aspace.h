/* User address spaces (Track B of M5-PLAN.md owns the implementation).
 *
 * One flat address space per process: a PML4 whose kernel half (entries
 * 256-511) is shared with every other address space, plus a set of
 * mappings of VMOs in the user half. Page-table entries are filled on
 * demand by aspace_fault. Lock order: the region lock (a sleeping mutex,
 * class "aspace") -> "vmo" -> "aspace page tables" (spinlock). */
#pragma once

#include <stdint.h>
#include <jam/status.h>

#define ASPACE_READ  (1u << 0)
#define ASPACE_WRITE (1u << 1)
#define ASPACE_EXEC  (1u << 2)   /* never together with ASPACE_WRITE */
#define ASPACE_FIXED (1u << 3)   /* map at *addr exactly (else first fit) */

struct aspace;
struct vmo;

/* A new, empty address space with one reference. */
status_t aspace_create(struct aspace **out);
void     aspace_ref(struct aspace *as);
/* The last reference unmaps everything and frees the page tables. */
void     aspace_unref(struct aspace *as);

/* Map [vmo_off, vmo_off+len) of vmo (page-aligned, len > 0) with flags.
 * *addr: in, the address for ASPACE_FIXED; out, where it was mapped. The
 * mapping holds a VMO reference. ERR_INVALID_ARGS for W+X or a bad range,
 * ERR_NO_RESOURCES if it doesn't fit, ERR_ALREADY_BOUND if a FIXED range
 * overlaps an existing mapping. */
status_t aspace_map(struct aspace *as, struct vmo *vmo, uint64_t vmo_off, uint64_t len,
                    unsigned flags, uint64_t *addr);
/* Remove every mapping page in [addr, addr+len) (splitting mappings as
 * needed) and shoot the range down on the CPUs using this address space. */
status_t aspace_unmap(struct aspace *as, uint64_t addr, uint64_t len);
/* Change permissions of [addr, addr+len), which must be fully mapped. */
status_t aspace_protect(struct aspace *as, uint64_t addr, uint64_t len, unsigned flags);

/* Resolve a fault at addr for `access` (ASPACE_READ/WRITE/EXEC): commit
 * the VMO page if needed and install the page-table entry. OK: retry the
 * access. ERR_NOT_FOUND: nothing mapped there. ERR_ACCESS_DENIED: the
 * mapping doesn't allow that access. ERR_NO_MEMORY: couldn't commit.
 * May sleep (region lock): call with interrupts on and no spinlock held. */
status_t aspace_fault(struct aspace *as, uint64_t addr, unsigned access);

/* On this CPU, with interrupts off: stop using prev (NULL = kernel page
 * tables) and start using next (NULL = kernel page tables): CR3 and the
 * address spaces' active-CPU masks. Cheap no-op when prev == next. */
void     aspace_switch(struct aspace *prev, struct aspace *next);
/* Physical address of the PML4 (for tests and the entry path). */
uint64_t aspace_pml4(struct aspace *as);
