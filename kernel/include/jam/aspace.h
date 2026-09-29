/* User address spaces (kernel/mm/aspace.c).
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
/* aspace_map only: permissions a later aspace_protect may grant (on top of
 * the ones mapped now). sys_vmar_map sets them from the VMO handle's
 * rights, so a mapping can be made RW, filled, then flipped to RX, but
 * never gain a permission the handle didn't carry. */
#define ASPACE_CAN_READ  (1u << 4)
#define ASPACE_CAN_WRITE (1u << 5)
#define ASPACE_CAN_EXEC  (1u << 6)

struct aspace;
struct job;
struct vmo;

/* A new, empty address space with one reference, charged to nobody. */
status_t aspace_create(struct aspace **out);
/* The same, charged to job (JOB_LIMIT_PAGES; NULL = nobody) for as long as
 * it exists: its PML4 now, and later every page table it makes and a page
 * per ASPACE_MAPPINGS_PER_PAGE mappings. The address space holds a job
 * reference. ERR_NO_MEMORY if the job refuses the PML4. With a job, map,
 * unmap and protect fail with ERR_NO_MEMORY, and faults with ERR_NO_MEMORY,
 * when the job refuses what they need. */
status_t aspace_create_charged(struct job *job, struct aspace **out);
#define ASPACE_MAPPINGS_PER_PAGE 16   /* charged: one page per this many mappings */
void     aspace_ref(struct aspace *as);
/* The last reference unmaps everything and frees the page tables. It must
 * not be dropped while a CPU still has the address space loaded (a
 * thread's reference covers its time on a CPU; checked, panics). It never
 * sleeps, so it may run from object teardown. */
void     aspace_unref(struct aspace *as);

/* Map [vmo_off, vmo_off+len) of vmo (page-aligned, len > 0) with flags.
 * *addr: in, the address for ASPACE_FIXED; out, where it was mapped. The
 * mapping holds a VMO reference. ERR_INVALID_ARGS for W+X (or W or X
 * without R) or a bad range, ERR_OUT_OF_RANGE past the VMO's end,
 * ERR_NO_RESOURCES if it doesn't fit, ERR_ALREADY_BOUND if a FIXED range
 * overlaps an existing mapping. No permissions at all is allowed (a guard:
 * every access faults with ERR_ACCESS_DENIED). */
status_t aspace_map(struct aspace *as, struct vmo *vmo, uint64_t vmo_off, uint64_t len,
                    unsigned flags, uint64_t *addr);
/* Remove every mapping page in [addr, addr+len) (splitting mappings as
 * needed) and shoot the range down on the CPUs using this address space.
 * Holes are fine; ERR_NOT_FOUND if nothing at all was mapped there. */
status_t aspace_unmap(struct aspace *as, uint64_t addr, uint64_t len);
/* Change permissions of [addr, addr+len), which must be fully mapped
 * (ERR_NOT_FOUND otherwise). ERR_ACCESS_DENIED if a mapping in the range
 * may not have them (see ASPACE_CAN_*). */
status_t aspace_protect(struct aspace *as, uint64_t addr, uint64_t len, unsigned flags);

/* Resolve a fault at addr for `access` (ASPACE_READ/WRITE/EXEC): commit
 * the VMO page if needed and install the page-table entry. OK: retry the
 * access. ERR_NOT_FOUND: nothing mapped there. ERR_ACCESS_DENIED: the
 * mapping doesn't allow that access. ERR_NO_MEMORY: couldn't commit.
 * ERR_OUT_OF_RANGE: the VMO was shrunk below the mapped page.
 * May sleep (region lock): call with interrupts on and no spinlock held. */
status_t aspace_fault(struct aspace *as, uint64_t addr, unsigned access);

/* On this CPU, with interrupts off: stop using prev (NULL = kernel page
 * tables) and start using next (NULL = kernel page tables): CR3 and the
 * address spaces' active-CPU masks. Cheap no-op when prev == next. */
void     aspace_switch(struct aspace *prev, struct aspace *next);
/* Physical address of the PML4 (for tests and the entry path). */
uint64_t aspace_pml4(struct aspace *as);
