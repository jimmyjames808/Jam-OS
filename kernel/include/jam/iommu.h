/* The IOMMU as the rest of the kernel sees it: DMA remapping (Intel VT-d,
 * kernel/dev/vtd_*.c) without its formats. The design is
 * docs/M11-PLAN.md; the hardware's model is in kernel/dev/vtd_domain.h.
 *
 * Everything here is behind the boot word `iommu=on` (off by default).
 * Without it, or on a machine with no VT-d unit, no function changes
 * anything: iommu_translating() is false, the boot calls return at once,
 * iommu_domain_create says ERR_NOT_SUPPORTED (the caller keeps today's
 * behaviour: physical addresses, the pin quarantine) and the jump call
 * finds no unit to turn off.
 *
 * With it, translation is on from the boot (iommu_boot): every PCI
 * function a unit covers has a context entry from the start, naming
 *   - the BLOCKING domain (an empty page table shared by the unit's
 *     functions with nothing of their own): any DMA is blocked and logged;
 *   - or, for a function the DMAR table lists in a reserved memory region
 *     (RMRR), its BOOT domain: that region mapped, nothing else;
 *   - or, while a driver holds a dma_cap for it, that cap's own domain
 *     (iommu_domain_create, iommu_attach: kernel/object/dma_cap.c): what
 *     the cap pinned (vmo_pin's iommu_map) and the function's RMRRs.
 * A function's "home" is the blocking domain or its boot domain: where
 * iommu_detach puts it back. Each unit also has a PASS-THROUGH domain
 * (all of RAM, as without an IOMMU: iommu_device_driven); no driver's
 * function is put there any more, only the tests use it.
 *
 * IOVA = physical address: iommu_map maps each page at its own address
 * (docs/M11-PLAN.md, question 1), so a driver's numbers don't change.
 *
 * Context: every call but iommu_reserve_early and iommu_jump_off is
 * thread context, interrupts on, no spinlock held (they wait for the
 * unit's invalidations, at most 100 ms each). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/status.h>

struct boot_info;
struct job;
struct pci_dev;
struct iommu_domain;

/* ---- the boot and the jump ---------------------------------------------------------- */

/* Before the memory managers (kmain, next to kexec_reserve), with
 * `iommu=on` only: each RMRR's pages that the boot memory map calls
 * usable (or loader-reclaimable) RAM become reserved, so the allocator
 * never hands out memory a device keeps using (VT-d 8.4 says firmware
 * must report them reserved already: this catches firmware that doesn't).
 * The DMAR table is read through the loader's direct map only where it is
 * sure to be mapped; otherwise it is said, and iommu_boot reports any
 * RMRR in RAM. Takes no lock, allocates nothing. */
void iommu_reserve_early(struct boot_info *bi);

/* With `iommu=on`, after the units are started (vtd_units_start): every
 * started unit gets its tables (root, context entries for each function
 * it covers, the blocking, pass-through and boot domains) and translation
 * on. A unit whose firmware (or a kexec'd kernel) left translation on is
 * taken over without turning it off. Problems go to the RESULTS box; a
 * unit that can't be set up is left as it was. Once, at boot, before user
 * space. */
void iommu_boot(void);

/* Before a jump into another kernel (kexec, the panic path) and before a
 * firmware reset: interrupt remapping off (irq_remap_off: the one place
 * that does it), then per started unit the fault event masked,
 * translation off, queued invalidation off (VT-d 11.4.4.1, 6.5.2), each
 * with a short bounded wait; a unit that doesn't answer is skipped. Bus mastering must be off on every function first
 * (pci_panic_bus_master_off): then no device can use the window with
 * translation off. Takes no lock (other CPUs may be halted holding any),
 * allocates nothing; interrupts may be off. */
void iommu_jump_off(void);

/* ---- what the rest of the kernel asks ----------------------------------------------- */

/* Does any unit translate (iommu_boot turned it on)? Lock-free. */
bool iommu_translating(void);

/* dev's context entry to its unit's pass-through domain (all of RAM). For
 * the tests only: a driver's function gets its dma_cap's own domain. OK at
 * once when dev isn't translated. ERR_TIMED_OUT, ERR_IO (the invalidation
 * failed: dev stays where it was). */
status_t iommu_device_driven(struct pci_dev *dev);

/* ---- domains (for dma_cap and vmo_pin) ---------------------------------------------- */

/* A new, empty domain for dev on dev's unit (its own domain id and page
 * table, the table pages charged to job, which may be NULL), with dev's
 * RMRRs already mapped. Not attached. ERR_NOT_SUPPORTED (dev isn't
 * translated: no unit covers it, or iommu=off), ERR_NO_RESOURCES (no
 * domain id left), ERR_NO_MEMORY. */
status_t iommu_domain_create(struct pci_dev *dev, struct job *job, struct iommu_domain **out);

/* Free a domain no context entry names (detached, or never attached):
 * its domain id's caches are invalidated, then its tables freed. The
 * mapped pages are the caller's: their pins are released by the caller
 * after this returns OK. ERR_BAD_STATE (still attached), ERR_TIMED_OUT
 * (the invalidation failed: nothing is freed, the domain stays valid). */
status_t iommu_domain_destroy(struct iommu_domain *d);

/* Point d's function's context entry at d (a 16-byte atomic write that
 * changes the domain id with the table, VT-d 6.2.2.1), then invalidate
 * what the unit cached for the entry it replaced and wait. The faults
 * muted on the function (its home domain's storm) are heard again.
 * ERR_TIMED_OUT, ERR_IO. */
status_t iommu_attach(struct iommu_domain *d);

/* Point d's function back at its home (blocking or boot domain), with the
 * same invalidation. After OK the function reaches nothing of d's.
 * ERR_BAD_STATE (d isn't attached), ERR_TIMED_OUT, ERR_IO. */
status_t iommu_detach(struct iommu_domain *d);

/* Map pages[0..n) (page-aligned physical addresses, in any order) each at
 * its own address, read and write; a page mapped twice holds two pins of
 * its mapping. Consecutive addresses are mapped as one run. With CAP.CM
 * the new mappings are invalidated (one batch, one wait). All or nothing.
 * ERR_INVALID_ARGS (n = 0, an unaligned page), ERR_OUT_OF_RANGE (past the
 * unit's address width), ERR_NO_RESOURCES (the domain's table cap),
 * ERR_NO_MEMORY, ERR_TIMED_OUT. */
status_t iommu_map(struct iommu_domain *d, const uint64_t *pages, size_t n);

/* Drop one pin of each of pages[0..n); mappings whose pins reach 0 are
 * removed, the unit's caches invalidated (one batch) and waited for. Only
 * after OK may the pages be reused. ERR_NOT_FOUND (a page not mapped:
 * nothing changed), ERR_TIMED_OUT (the invalidation failed: the pages
 * must NOT be reused; the emptied table pages are kept). */
status_t iommu_unmap(struct iommu_domain *d, const uint64_t *pages, size_t n);
