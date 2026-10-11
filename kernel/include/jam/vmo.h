/* Virtual memory objects: the unit of memory that is shared between
 * processes (mapped through address spaces) and handed to drivers for
 * DMA.
 *
 * Three kinds:
 *   - paged (the default): pages are committed on demand, zero-filled, and
 *     tracked in a sparse three-level table, so a mostly-empty 64 GiB VMO
 *     costs only the pages it touches plus their table pages;
 *   - CONTIGUOUS: one buddy block, committed and zeroed at creation;
 *   - physical: existing memory the kernel doesn't own (MMIO, a framebuffer);
 *     its pages are never freed.
 * A paged VMO made with VMO_KEEP_PAGES ("kept") has every page committed
 * from its creation to its destruction: decommit and shrink are refused for
 * every holder, and growing commits the new pages before it returns. So an
 * address space that maps it can't be made to fault by another holder
 * (ASPACE_KEPT_ONLY fills such a mapping's entries at map time). There is
 * no clone or copy-on-write child of any VMO; one added later must not
 * share a kept VMO's pages copy-on-write (a write would replace a page
 * under a reader), so it would copy them into a VMO of its own.
 *
 * Offsets and lengths are in bytes. Operations that act on pages (commit,
 * decommit, kernel mappings) cover every page the byte range touches.
 *
 * Kernel mappings and pins are recorded on the VMO. While a page is kernel-
 * mapped or pinned it can't be decommitted or cut off by a shrink
 * (ERR_BAD_STATE), so its physical address stays valid for the CPU or the
 * device using it. User mappings (address spaces) are recorded too, in
 * a reverse map, but don't block anything: decommit and shrink unmap the
 * pages from every address space (and shoot down their TLBs) before
 * freeing them, and the next access faults in a fresh zero page. Each
 * mapping of any kind and each pin holds a reference on the VMO: closing
 * the last handle never frees memory a device may still be writing to. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/abi.h>   /* the flags and limits below */
#include <jam/object.h>
#include <jam/status.h>

/* The flags (VMO_CONTIGUOUS, VMO_DMA32, VMO_KEEP_PAGES) and the limits
 * (VMO_MAX_SIZE, VMO_CONTIGUOUS_MAX) are in <jam/abi.h>: user code asks for
 * them and meets them. */
#define VMO_CREATE_FLAGS (VMO_CONTIGUOUS | VMO_DMA32 | VMO_KEEP_PAGES)

/* Embeds struct kobject first (type OBJ_VMO), so the casts below are safe. */
struct vmo;
struct vmo_verify;   /* <jam/vmo_verify.h> */

static inline struct kobject *vmo_kobject(struct vmo *v) { return (struct kobject *)v; }
/* NULL unless obj is a VMO. */
static inline struct vmo *vmo_from_kobject(struct kobject *obj)
{
    return obj && obj->type == OBJ_VMO ? (struct vmo *)obj : NULL;
}

/* Size is rounded up to whole pages. 0 is allowed except for CONTIGUOUS.
 * ERR_OUT_OF_RANGE past VMO_MAX_SIZE (or 4 MiB for CONTIGUOUS);
 * ERR_INVALID_ARGS for VMO_KEEP_PAGES with CONTIGUOUS or DMA32. The caller
 * gets the only reference (kobject_unref to drop it). The same as
 * vmo_create_for with no job. */
status_t vmo_create(uint64_t size, uint32_t flags, struct vmo **out);
/* vmo_create, then vmo_set_job(job) unless job is NULL; then, for
 * VMO_KEEP_PAGES, every page is committed, each charged to job before it
 * is published (so the kernel never holds more than one page the job
 * hasn't paid for). ERR_NO_MEMORY if job refuses one or memory runs out:
 * the VMO is gone again and job is charged nothing. Thread context. */
struct job;
status_t vmo_create_for(struct job *job, uint64_t size, uint32_t flags, struct vmo **out);
/* Made with VMO_KEEP_PAGES (fixed at creation, so no lock is needed). */
bool vmo_is_kept(const struct vmo *v);
/* A VMO over [phys, phys+size), which must be page-aligned. The pages are
 * never freed. cache: VM_UC, VM_WC or 0 (write-back), used for mappings.
 * Byte access (vmo_read/vmo_write) is ERR_NOT_SUPPORTED: map it instead. */
status_t vmo_create_physical(uint64_t phys, uint64_t size, unsigned cache, struct vmo **out);

/* Charge the VMO's pages to job (JOB_LIMIT_PAGES; see process.h), now
 * and whenever it commits more; a commit or fault the job can't afford
 * fails with ERR_NO_MEMORY. Once, before the VMO is shared: ERR_BAD_STATE
 * if it already has a job, ERR_NO_MEMORY if its current pages (a
 * contiguous VMO) don't fit. A physical VMO owns no pages: only its struct
 * (one JOB_LIMIT_HANDLES unit) is charged. A NULL job is allowed and
 * charges nothing. */
status_t vmo_set_job(struct vmo *v, struct job *job);

/* Uncommitted pages read as zeros and stay uncommitted; a pager VMO's
 * absent page is waited for (<jam/pager.h>: ERR_IO if its pager fails it,
 * ERR_CANCELED if the thread is killed), so call it with no lock held. */
status_t vmo_read(struct vmo *v, uint64_t offset, void *buf, uint64_t len);
/* Commits pages as needed. ERR_BAD_STATE if v is sealed (vmo_seal), as
 * for vmo_set_size, vmo_commit and vmo_decommit. */
status_t vmo_write(struct vmo *v, uint64_t offset, const void *buf, uint64_t len);
uint64_t vmo_size(struct vmo *v);
/* Seal v for good (vmo_make_exec): from now on its bytes and size can't
 * change. Every write path is refused with ERR_BAD_STATE (vmo_write,
 * vmo_set_size, vmo_commit, vmo_decommit), and so are a writable user or
 * kernel mapping and a pin; reading and read-only or executable mappings
 * still work. ERR_BAD_STATE (and v unchanged) unless v is a paged VMO that
 * nothing maps or pins and no write path is running on. Sealing a sealed
 * VMO again is OK under the same conditions. */
status_t vmo_seal(struct vmo *v);
/* Seal a pager VMO with its table of page hashes (vmo_make_exec_verified,
 * <jam/vmo_verify.h>): every page present is dropped, and from then on a
 * page is published only if it matches vf, which v owns from then on
 * (freed with it). ERR_BAD_STATE (vf still the caller's) unless v is a
 * pager VMO, not detached, not verified already, and nothing maps or pins
 * it. */
status_t vmo_seal_verified(struct vmo *v, struct vmo_verify *vf);
/* v's table (vmo_seal_verified), or NULL. Lock-free. */
struct vmo_verify *vmo_verifier(struct vmo *v);
/* Bytes of memory the VMO owns right now (committed pages). */
uint64_t vmo_committed(struct vmo *v);
/* Grow or shrink (rounded up to pages). Shrinking frees the pages past the
 * new end, unmapping them from address spaces first; ERR_BAD_STATE if any
 * of them is pinned or kernel-mapped, or v is kept (VMO_KEEP_PAGES). Paged
 * VMOs only (ERR_NOT_SUPPORTED otherwise). Growing adds zero pages; a kept
 * VMO commits them (charged to its job) before the new size shows, and on
 * ERR_NO_MEMORY gives them back and keeps its size. May sleep (interrupts
 * on, no spinlock held): it shoots down TLBs. */
status_t vmo_set_size(struct vmo *v, uint64_t size);
/* Allocate (zeroed) pages now. A no-op for contiguous and physical VMOs. */
status_t vmo_commit(struct vmo *v, uint64_t offset, uint64_t len);
/* Free pages; they read as zeros again, and are unmapped from address
 * spaces first. ERR_BAD_STATE if v is kept (VMO_KEEP_PAGES: never, for any
 * range), or if any page is pinned or kernel-mapped (checked up
 * front; a pin that appears while a long decommit runs stops it there, with
 * the pages before it already decommitted). Paged VMOs only. Interrupts on,
 * no spinlock held: it shoots down TLBs. */
status_t vmo_decommit(struct vmo *v, uint64_t offset, uint64_t len);

/* Map the pages holding [offset, offset+len) into the kernel's vmap area,
 * committing them first. vm_flags: 0 or VM_WRITE (the cache type is the
 * VMO's own: write-back, or what vmo_create_physical was given). *va points
 * at `offset` itself, not the start of its page. The mapping holds a VMO
 * reference until vmo_unmap_kernel, which needs interrupts on (it shoots
 * down other CPUs' TLBs) and no spinlock held, and gives the virtual range
 * back for reuse (page tables included). ERR_NO_MEMORY if the pages or the
 * page tables can't be had (nothing is left mapped then). */
status_t vmo_map_kernel(struct vmo *v, uint64_t offset, uint64_t len, unsigned vm_flags,
                        void **va);
status_t vmo_unmap_kernel(struct vmo *v, void *va);

/* DMA. dma_cap_create makes an unbound capability (kernel tests); drivers
 * get bound ones (dma_cap_create_for in <jam/resource.h>) from devmgr. */
status_t dma_cap_create(struct kobject **out);
/* Pin [offset, offset+len) (both page-aligned, len > 0) for device DMA:
 * commits the pages (a writable pin; a read-only one takes only pages
 * committed already, below), forbids decommitting or shrinking them away, and
 * writes each page's physical address to phys_out[0..len/PAGE_SIZE) (on
 * a failure too: its contents mean nothing then). With a cap that has an
 * IOMMU domain (dma_cap_translated) each page is also mapped there at
 * that same address, so the device can reach it.
 * ERR_WRONG_TYPE if dma_cap isn't a DMA capability, ERR_BUFFER_TOO_SMALL
 * if phys_cap (entries) is too small, ERR_BAD_STATE if the cap is bound to
 * a function whose Bus Master Enable is off, or that has a newer cap,
 * or its last handle is gone, or if the pin writes (no VMO_PIN_READ_ONLY,
 * or no domain) and v is sealed. From the domain: ERR_OUT_OF_RANGE (a page
 * past the unit's address width), ERR_NO_RESOURCES (the domain's table
 * cap, or a page pinned 1023 times), ERR_NO_MEMORY, and ERR_TIMED_OUT /
 * ERR_IO (the unit didn't confirm an invalidation: the pages stay pinned
 * for good). The pin costs the capability's job (the pinner's, never v's:
 * the VMO's holder may have given the driver only a read-only view) one
 * JOB_LIMIT_HANDLES unit (ERR_NO_RESOURCES if it can't pay) until it goes;
 * a cap with no job (kernel tests) charges nobody. The pin holds
 * references on the VMO and the capability until
 * vmo_unpin or until the cap's last handle closes (which releases every
 * pin made with it, or for a cap bound to a function hands them to
 * dma_cap.c: freed once the IOMMU took them away, else quarantined).
 * Thread context, no spinlock held.
 * flags: VMO_PIN_READ_ONLY (<jam/abi.h>), the device only reads the
 * pages: with a domain they are mapped there read only (its writes
 * fault), ERR_BAD_STATE if one of them is mapped there writable by
 * another pin already (and a writable pin of a page mapped read only is
 * refused the same way: one permission per page and domain); the pin
 * doesn't count as a write (a sealed VMO takes it). Without a domain the
 * device can write anyway. With or without one, a read-only pin commits
 * nothing: ERR_BAD_STATE if any page of the range isn't committed (it
 * may be made with only RIGHT_READ, which never commits; review M12.1
 * KDEV finding 4). Any other bit is ERR_INVALID_ARGS. vmo_pin is
 * vmo_pin_flags with flags 0. */
status_t vmo_pin_flags(struct vmo *v, struct kobject *dma_cap, uint64_t offset, uint64_t len,
                       uint32_t flags, uint64_t *phys_out, uint64_t phys_cap, uint64_t *pin_id);
static inline status_t vmo_pin(struct vmo *v, struct kobject *dma_cap, uint64_t offset,
                               uint64_t len, uint64_t *phys_out, uint64_t phys_cap,
                               uint64_t *pin_id)
{
    return vmo_pin_flags(v, dma_cap, offset, len, 0, phys_out, phys_cap, pin_id);
}
/* Undo a pin. Only the capability it was made with may: ERR_NOT_FOUND if
 * pin_id isn't a live pin of v, ERR_ACCESS_DENIED if it was made with
 * another dma_cap (a client sharing the VMO must not free a page a device
 * still writes to). With an IOMMU domain the pages are unmapped and the
 * unit's invalidation waited for before they may go; ERR_TIMED_OUT /
 * ERR_IO: it didn't confirm, so the pin is gone but its pages stay pinned
 * for good. Thread context, no spinlock held. */
status_t vmo_unpin(struct vmo *v, struct kobject *dma_cap, uint64_t pin_id);
