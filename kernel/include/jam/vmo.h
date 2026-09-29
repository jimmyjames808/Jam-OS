/* Virtual memory objects: the unit of memory that is shared between
 * processes (mapped through address spaces in M5) and handed to drivers for
 * DMA (M6).
 *
 * Three kinds:
 *   - paged (the default): pages are committed on demand, zero-filled, and
 *     tracked in a sparse three-level table, so a mostly-empty 64 GiB VMO
 *     costs only the pages it touches plus their table pages;
 *   - CONTIGUOUS: one buddy block, committed and zeroed at creation;
 *   - physical: existing memory the kernel doesn't own (MMIO, a framebuffer);
 *     its pages are never freed.
 *
 * Offsets and lengths are in bytes. Operations that act on pages (commit,
 * decommit, kernel mappings) cover every page the byte range touches.
 *
 * Kernel mappings and pins are recorded on the VMO. While a page is kernel-
 * mapped or pinned it can't be decommitted or cut off by a shrink
 * (ERR_BAD_STATE), so its physical address stays valid for the CPU or the
 * device using it. User mappings (address spaces, M5) are recorded too, in
 * a reverse map, but don't block anything: decommit and shrink unmap the
 * pages from every address space (and shoot down their TLBs) before
 * freeing them, and the next access faults in a fresh zero page. Each
 * mapping of any kind and each pin holds a reference on the VMO: closing
 * the last handle never frees memory a device may still be writing to. */
#pragma once

#include <stdint.h>
#include <jam/object.h>
#include <jam/status.h>

#define VMO_MAX_SIZE   (1ull << 36)   /* 64 GiB of address space; pages committed on demand */
#define VMO_CONTIGUOUS (1u << 0)      /* physically contiguous, committed at creation (<= 4 MiB) */
#define VMO_DMA32      (1u << 1)      /* every page below 4 GiB */
#define VMO_CREATE_FLAGS (VMO_CONTIGUOUS | VMO_DMA32)

/* Embeds struct kobject first (type OBJ_VMO), so the casts below are safe. */
struct vmo;

static inline struct kobject *vmo_kobject(struct vmo *v) { return (struct kobject *)v; }
/* NULL unless obj is a VMO. */
static inline struct vmo *vmo_from_kobject(struct kobject *obj)
{
    return obj && obj->type == OBJ_VMO ? (struct vmo *)obj : NULL;
}

/* Size is rounded up to whole pages. 0 is allowed except for CONTIGUOUS.
 * ERR_OUT_OF_RANGE past VMO_MAX_SIZE (or 4 MiB for CONTIGUOUS). The caller
 * gets the only reference (kobject_unref to drop it). */
status_t vmo_create(uint64_t size, uint32_t flags, struct vmo **out);
/* A VMO over [phys, phys+size), which must be page-aligned. The pages are
 * never freed. cache: VM_UC, VM_WC or 0 (write-back), used for mappings.
 * Byte access (vmo_read/vmo_write) is ERR_NOT_SUPPORTED: map it instead. */
status_t vmo_create_physical(uint64_t phys, uint64_t size, unsigned cache, struct vmo **out);

/* M5: charge the VMO's pages to job (JOB_LIMIT_PAGES; see process.h), now
 * and whenever it commits more; a commit or fault the job can't afford
 * fails with ERR_NO_MEMORY. Once, before the VMO is shared: ERR_BAD_STATE
 * if it already has a job or is physical, ERR_NO_MEMORY if its current
 * pages (a contiguous VMO) don't fit. A NULL job is allowed and charges
 * nothing. */
struct job;
status_t vmo_set_job(struct vmo *v, struct job *job);

/* Uncommitted pages read as zeros and stay uncommitted. */
status_t vmo_read(struct vmo *v, uint64_t offset, void *buf, uint64_t len);
/* Commits pages as needed. */
status_t vmo_write(struct vmo *v, uint64_t offset, const void *buf, uint64_t len);
uint64_t vmo_size(struct vmo *v);
/* Bytes of memory the VMO owns right now (committed pages). */
uint64_t vmo_committed(struct vmo *v);
/* Grow or shrink (rounded up to pages). Shrinking frees the pages past the
 * new end, unmapping them from address spaces first; ERR_BAD_STATE if any
 * of them is pinned or kernel-mapped. Paged VMOs only (ERR_NOT_SUPPORTED
 * otherwise). Growing adds zero pages. May sleep (interrupts on, no
 * spinlock held): it shoots down TLBs. */
status_t vmo_set_size(struct vmo *v, uint64_t size);
/* Allocate (zeroed) pages now. A no-op for contiguous and physical VMOs. */
status_t vmo_commit(struct vmo *v, uint64_t offset, uint64_t len);
/* Free pages; they read as zeros again, and are unmapped from address
 * spaces first. ERR_BAD_STATE if any is pinned or kernel-mapped (checked up
 * front; a pin that appears while a long decommit runs stops it there, with
 * the pages before it already decommitted). Paged VMOs only. Interrupts on,
 * no spinlock held: it shoots down TLBs. */
status_t vmo_decommit(struct vmo *v, uint64_t offset, uint64_t len);

/* Map the pages holding [offset, offset+len) into the kernel's vmap area,
 * committing them first. vm_flags: 0 or VM_WRITE (the cache type is the
 * VMO's own: write-back, or what vmo_create_physical was given). *va points
 * at `offset` itself, not the start of its page. The mapping holds a VMO
 * reference until vmo_unmap_kernel, which needs interrupts on (it shoots
 * down other CPUs' TLBs) and no spinlock held. Kernel virtual space is not
 * reused after an unmap yet (the vmap area has no free list). */
status_t vmo_map_kernel(struct vmo *v, uint64_t offset, uint64_t len, unsigned vm_flags,
                        void **va);
status_t vmo_unmap_kernel(struct vmo *v, void *va);

/* DMA. dma_cap_create is a kernel-only constructor for now (OBJ_DMA_CAP;
 * drivers get one from devmgr in M6). */
status_t dma_cap_create(struct kobject **out);
/* Pin [offset, offset+len) (both page-aligned, len > 0) for device DMA:
 * commits the pages, forbids decommitting or shrinking them away, and
 * writes each page's physical address to phys_out[0..len/PAGE_SIZE).
 * ERR_WRONG_TYPE if dma_cap isn't a DMA capability, ERR_BUFFER_TOO_SMALL
 * if phys_cap (entries) is too small. The pin holds references on the VMO
 * and the capability until vmo_unpin. */
status_t vmo_pin(struct vmo *v, struct kobject *dma_cap, uint64_t offset, uint64_t len,
                 uint64_t *phys_out, uint64_t phys_cap, uint64_t *pin_id);
/* ERR_NOT_FOUND if pin_id isn't a live pin of v. */
status_t vmo_unpin(struct vmo *v, uint64_t pin_id);
