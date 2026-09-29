/* The seam between address spaces (mm/aspace.c) and VMOs (object/vmo.c).
 * Kernel-internal: nothing outside those two files (and their tests)
 * should need this header.
 *
 * Reverse map: every user mapping of a VMO is a struct vmo_umap on the
 * VMO's list, under the VMO lock. decommit and shrink walk that list with
 * the VMO lock held and clear the page-table entries of the pages they take
 * away (aspace_zap_locked), collecting the CPUs to shoot down and the pages
 * to free in a struct tlb_gather. The pages are freed only after the
 * shootdown (tlb_gather_finish), so a freed page is never still reachable
 * through any TLB.
 *
 * Lock order: aspace region lock (mutex) -> "vmo" -> "aspace page tables".
 * See the header of mm/aspace.c for the races this covers. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/list.h>
#include <jam/sched.h>   /* cpumask_t */
#include <jam/status.h>

struct aspace;
struct page;
struct vmo;

/* ---- deferred TLB flush ("gather") -------------------------------------- */

struct tlb_gather {
    cpumask_t        cpus;      /* CPUs that may hold a cleared entry */
    uint64_t         lo, hi;    /* user range covering every cleared entry */
    bool             need;      /* something was cleared: flush */
    struct list_node pages;     /* struct page (via page->node): release after the flush */
    uint64_t         npages;    /* pages on the list */
};

void tlb_gather_init(struct tlb_gather *g);
/* Drop one reference on p (freeing it at zero) only after the flush. Its
 * struct page `node` is used for the list, so p must be an allocated page
 * nobody else lists (VMO pages and page-table pages qualify). */
void tlb_gather_page(struct tlb_gather *g, struct page *p);
/* Shoot the gathered range down on the gathered CPUs, then release the
 * pages. Interrupts on, no spinlock held (it waits for other CPUs). Leaves
 * g empty and ready for reuse. */
void tlb_gather_finish(struct tlb_gather *g);

/* Drop one reference on a page, freeing it at zero (the VMO page rule). */
void page_unref(struct page *p);

/* ---- the reverse map ---------------------------------------------------- */

/* One user mapping of a VMO, embedded in the address space's mapping. Its
 * fields change only under the VMO lock (vmo_umap_set), so the VMO side can
 * trust them while it holds that lock. */
struct vmo_umap {
    struct list_node node;        /* on the VMO's list (VMO lock) */
    struct aspace   *as;          /* the address space it is in */
    uint64_t         base;        /* user address of VMO page `first` */
    uint64_t         first, end;  /* VMO page indices [first, end) */
};

/* Record u (fields filled in) on v and take a VMO reference for it. With
 * `check`, ERR_OUT_OF_RANGE if u reaches past the VMO's end (a new
 * mapping); splitting an existing mapping passes false, since a shrink may
 * already have cut the VMO below it. */
status_t vmo_umap_add(struct vmo *v, struct vmo_umap *u, bool check);
/* Change u's range under the VMO lock. */
void     vmo_umap_set(struct vmo *v, struct vmo_umap *u, uint64_t base, uint64_t first,
                      uint64_t end);
/* Unlink u and drop its VMO reference (v may be destroyed). The caller has
 * already cleared and shot down u's page-table entries. */
void     vmo_umap_remove(struct vmo *v, struct vmo_umap *u);

/* Fault: under the VMO lock, commit page idx if needed (paged VMOs) and
 * install it at va in as with `perms` (ASPACE_READ/WRITE/EXEC) and the
 * VMO's cache type, through aspace_set_pte_locked. The caller holds as's
 * region lock and has created the page tables for va. ERR_OUT_OF_RANGE if
 * idx is past the VMO's end, ERR_NO_MEMORY if it couldn't be committed. */
status_t vmo_fault_map(struct vmo *v, uint64_t idx, struct aspace *as, uint64_t va,
                       unsigned perms);

/* ---- address-space side, called by vmo.c with the VMO lock held --------- */

/* Install one leaf entry (its tables must exist). */
void aspace_set_pte_locked(struct aspace *as, uint64_t va, uint64_t pa, unsigned perms,
                           unsigned cache);
/* Clear the leaf entries in [va, va+len) and add the range and as's active
 * CPUs to g. Never frees page tables. */
void aspace_zap_locked(struct aspace *as, uint64_t va, uint64_t len, struct tlb_gather *g);

/* ---- for tests ---------------------------------------------------------- */

/* The raw leaf entry for va (0 if none). */
uint64_t aspace_pte(struct aspace *as, uint64_t va);
/* Page-table pages the address space owns below its PML4. */
uint64_t aspace_pt_pages(struct aspace *as);
/* Number of mappings. */
uint32_t aspace_mapping_count(struct aspace *as);
/* Physical address of the page holding `offset`, 0 if not committed. */
uint64_t vmo_page_phys(struct vmo *v, uint64_t offset);
