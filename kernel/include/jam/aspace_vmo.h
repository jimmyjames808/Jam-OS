/* The seam between address spaces (mm/aspace.c) and VMOs (object/vmo.c).
 * Kernel-internal: nothing outside those two files (and their tests)
 * should need this header.
 *
 * Reverse map: every user mapping of a VMO is a struct vmo_umap in the
 * VMO's interval tree (<jam/itree.h>), by the range of VMO pages it maps,
 * under the VMO lock. decommit and shrink look up the mappings over the
 * pages they take away with the VMO lock held (O(log n) each, however many
 * mappings the VMO has elsewhere), clear those mappings' page-table entries
 * for the pages (aspace_zap_locked), and collect the CPUs to shoot down
 * and the pages to free in a struct tlb_gather. The pages are freed only after the
 * shootdown (tlb_gather_finish), so a freed page is never still reachable
 * through any TLB. What one batch of pages may spend on that walk is
 * bounded (VMO_BATCH_BUDGET, below), and so is how many mappings of a VMO
 * one address space holds over a page (VMAR_MAX_OVERLAP, <jam/abi.h>).
 *
 * Lock order: aspace region lock (mutex) -> "vmo" -> "aspace page tables".
 * See the header of mm/aspace.c for the races this covers. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/itree.h>
#include <jam/list.h>
#include <jam/sched.h>   /* cpumask_t */
#include <jam/status.h>

struct aspace;
struct job;
struct page;
struct vmo;

/* ---- deferred TLB flush ("gather") -------------------------------------- */

#define TLB_GATHER_SHARED 32   /* shared pages one gather holds (tlb_gather_shared) */

struct tlb_gather {
    cpumask_t        cpus;      /* CPUs that may hold a cleared entry */
    uint64_t         lo, hi;    /* user range covering every cleared entry */
    bool             need;      /* something was cleared: flush */
    struct list_node pages;     /* struct page (via page->node): release after the flush */
    uint64_t         npages;    /* pages on the list */
    uint32_t         nshared;   /* entries in shared[] */
    struct page     *shared[TLB_GATHER_SHARED];   /* tlb_gather_shared's pages */
};

void tlb_gather_init(struct tlb_gather *g);
/* Drop one reference on p (freeing it at zero) only after the flush. Its
 * struct page `node` is used for the list, so p must be an allocated page
 * nobody else lists (VMO pages and page-table pages qualify; a page other
 * VMOs share copy-on-write does not: tlb_gather_shared). */
void tlb_gather_page(struct tlb_gather *g, struct page *p);
/* The same for a VMO table's reference on a page that other tables may
 * hold too (a copy-on-write page, vmo.c): another gather may be listing
 * it, so it goes in shared[] instead, and after the flush its share count
 * is lowered (page_share_drop). The caller checks tlb_gather_full first. */
void tlb_gather_shared(struct tlb_gather *g, struct page *p);
static inline bool tlb_gather_full(const struct tlb_gather *g)
{
    return g->nshared == TLB_GATHER_SHARED;
}
/* Shoot the gathered range down on the gathered CPUs, then release the
 * pages. Interrupts on, no spinlock held (it waits for other CPUs). Leaves
 * g empty and ready for reuse. */
void tlb_gather_finish(struct tlb_gather *g);

/* Drop one reference on a page, freeing it at zero (the VMO page rule). */
void page_unref(struct page *p);
/* A VMO table lets go of p: its share count (how many other tables hold
 * it, vmo.c) goes down by one unless it is 0 already, then the table's
 * reference is dropped (page_unref). */
void page_share_drop(struct page *p);

/* ---- the reverse map ---------------------------------------------------- */

/* One user mapping of a VMO, embedded in the address space's mapping. Its
 * fields change only under the VMO lock (vmo_umap_set), so the VMO side can
 * trust them while it holds that lock. */
struct vmo_umap {
    struct itree_node pages;      /* VMO page indices [pages.first, pages.end), in the
                                     VMO's tree (VMO lock) */
    struct aspace    *as;         /* the address space it is in */
    uint64_t          base;       /* user address of VMO page pages.first */
    bool              writable;   /* may be (or become) writable: refused on a sealed VMO */
};

/* Record u (fields filled in, pages.first < pages.end) on v and take a VMO
 * reference for it: O(log n). With `check`, ERR_OUT_OF_RANGE if u reaches
 * past the VMO's end (a new mapping), ERR_BAD_STATE if u is writable and v
 * sealed (vmo_seal), ERR_ACCESS_DENIED if u is writable and v a pager VMO;
 * splitting an existing mapping passes false, since a shrink may already
 * have cut the VMO below it. */
status_t vmo_umap_add(struct vmo *v, struct vmo_umap *u, bool check);
/* Change u's range under the VMO lock (first < end): O(log n). */
void     vmo_umap_set(struct vmo *v, struct vmo_umap *u, uint64_t base, uint64_t first,
                      uint64_t end);
/* Unlink u and drop its VMO reference (v may be destroyed): O(log n). The
 * caller has already cleared and shot down u's page-table entries. */
void     vmo_umap_remove(struct vmo *v, struct vmo_umap *u);

/* What one batch (a decommit's, a shrink's, a child's creation's) may
 * spend clearing or write-protecting its pages in the user mappings over
 * them, with the VMO lock held and interrupts off, in units of under a
 * nanosecond on the PC: each mapping costs VMO_BATCH_MAPPING_COST (finding
 * it, its page-table lock, the walk down to its entries: 60 to 110 ns
 * when the mappings are in many address spaces), and each page of the
 * batch it maps VMO_BATCH_CLEAR_COST to clear or VMO_BATCH_PROTECT_COST
 * to write-protect. A batch is cut short to stay within it (vmo.c's
 * "Latency"), down to one page: a whole budget took 12 to 61 us on the PC
 * (ktest aspace_hostile_overlap_decommit, 4 and 8 CPUs). One page under
 * more mappings than that is a batch of its own, whatever it costs:
 * VMAR_MAX_OVERLAP mappings from each address space that maps it at most
 * (4,096 from 64 address spaces: 0.23 to 0.45 ms). */
#define VMO_BATCH_BUDGET       65536
#define VMO_BATCH_MAPPING_COST 128
#define VMO_BATCH_CLEAR_COST   1
#define VMO_BATCH_PROTECT_COST 4

/* Fault: under the VMO lock, commit page idx if needed (paged VMOs) and
 * install it at va in as with `perms` (the mapping's ASPACE_READ/WRITE/EXEC)
 * and the VMO's cache type, through aspace_set_pte_locked. `write`: the
 * access was a write. A page shared copy-on-write with another VMO is
 * installed without ASPACE_WRITE for any other access, and copied first
 * for a write (the copy goes in every mapping of v: their entries for the
 * old page are cleared and shot down before this returns). The caller
 * holds as's region lock and has created the page tables for va.
 * ERR_OUT_OF_RANGE if idx is past the VMO's end, ERR_NO_MEMORY if it
 * couldn't be committed or copied, ERR_SHOULD_WAIT if idx is an absent
 * page of a pager VMO (or one that a child of a pager VMO takes from it,
 * absent there): nothing is installed or asked for, and aspace_fault asks
 * the pager (vmo_page_request) and waits without the region lock.
 * ERR_NO_RESOURCES if `write` found a clean page of a writable pager VMO
 * whose pager is at its dirty limit: nothing changed, and aspace_fault
 * waits for a write-back (vmo_pager_throttle) without the region lock. */
status_t vmo_fault_map(struct vmo *v, uint64_t idx, struct aspace *as, uint64_t va,
                       unsigned perms, bool write);
/* After a pager's answer: is page idx of v there now (in v, or for a
 * child of a pager VMO in the pager VMO, or past its end)? A request is
 * answered whole, so one that failed may still have brought it. */
bool     vmo_page_ready(struct vmo *v, uint64_t idx);
/* mincore's look (aspace_resident; abi/syscalls.def's vmar_resident has
 * the rule): out[i] = 1 if v holds page first + i, i < n, else 0 (past
 * the size too). A contiguous or physical VMO holds every page below its
 * size. With `cache` false a pager VMO's cache isn't told: its pages below
 * the size read 1, and so do the window pages a child of one doesn't hold
 * itself (they would come from the pager VMO). Committed means in the
 * table: a page shared copy-on-write is there for each table holding it,
 * and a pager VMO's clean, dirty and writing pages alike. One leaf table
 * (or one run of missing tables) per lock hold. */
void     vmo_resident(struct vmo *v, uint64_t first, uint64_t n, bool cache, uint8_t *out);

/* ---- vmar_clone's copies (aspace.c's clone; vmo.c's "Clones") ----------- */

/* Begin a clone's copy of v, a paged VMO mapped as a copy (never a pager
 * or JIT VMO: ERR_NOT_SUPPORTED): v's `protecting` is raised, so every
 * change to its bytes or size waits for vmo_clone_end, and *out is a new
 * VMO as large as v (a pager child's gets the same window), charged to
 * job (ERR_NO_MEMORY, and nothing raised, if it refuses).
 * ERR_SHOULD_WAIT, nothing raised: a writer of v waits for its freezes to
 * end, or another thread has the freeze turn; the clone lets go of every
 * freeze and lock it holds and calls vmo_freeze_backoff (vmo.c's "Freezes
 * and writers"). With the turn it waits for v's writers instead
 * (ERR_CANCELED at a kill). */
status_t vmo_clone_begin(struct job *job, struct vmo *v, struct vmo **out);
/* After a freeze of v backed off for the `tries`th time, with nothing
 * held: wait for v's writers to go on, or (from FREEZE_TRIES on, or if
 * another thread has it) take the freeze turn (*turn: give it back with
 * vmo_freeze_turn_end once the try with it is over), then try again.
 * ERR_CANCELED if the thread is killed meanwhile. */
status_t vmo_freeze_backoff(struct vmo *v, unsigned tries, bool *turn);
void     vmo_freeze_turn_end(void);
/* Give c, v's copy, v's pages [first, end) (up to c's size): shared
 * copy-on-write with v's writable entries for them write-protected into g,
 * which the caller shoots down before vmo_clone_end; a kept VMO's are
 * copied. ERR_BAD_STATE if a pin or a kernel mapping covers one,
 * ERR_NO_MEMORY if c's job refuses one; what was given stays in c. */
status_t vmo_clone_share(struct vmo *v, struct vmo *c, uint64_t first, uint64_t end,
                         struct tlb_gather *g);
/* v's copy is over and g shot down: v may change again. */
void     vmo_clone_end(struct vmo *v);

/* ---- address-space side, called by vmo.c with the VMO lock held --------- */

/* Install one leaf entry (its tables must exist). */
void aspace_set_pte_locked(struct aspace *as, uint64_t va, uint64_t pa, unsigned perms,
                           unsigned cache);
/* Clear the leaf entries in [va, va+len) and add the range and as's active
 * CPUs to g. Never frees page tables. */
void aspace_zap_locked(struct aspace *as, uint64_t va, uint64_t len, struct tlb_gather *g);
/* Take the write bit off every leaf entry in [va, va+len) (they stay
 * present) and add the range and as's active CPUs to g: pages a new
 * copy-on-write child shares. */
void aspace_wrprotect_locked(struct aspace *as, uint64_t va, uint64_t len, struct tlb_gather *g);

/* ---- for tests ---------------------------------------------------------- */

/* The raw leaf entry for va (0 if none). */
uint64_t aspace_pte(struct aspace *as, uint64_t va);
/* Page-table pages the address space owns below its PML4. */
uint64_t aspace_pt_pages(struct aspace *as);
/* Page-table pages a clone paid for that it hasn't made yet (pt_reserve). */
uint64_t aspace_pt_reserve(struct aspace *as);
/* Faults that waited for a pager while their thread held a mutex (atomic;
 * each one is also reported on the console). Must stay 0: see PK2's audit
 * in docs/M12.5-PLAN.md. */
extern uint64_t aspace_pager_waits_held;
/* The number of user mappings on v's reverse map, or -1 if its tree is
 * not sound (itree_check). Takes the VMO lock: O(n). */
int64_t vmo_rmap_check(struct vmo *v);
/* The most a decommit or shrink batch, or a child's creation's batch, has
 * taken so far with the VMO lock held: ns (from the walk's start), and its
 * walk over the user mappings in VMO_BATCH_BUDGET's units (atomic,
 * relaxed: a test zeroes them, makes batches, reads them). */
extern uint64_t vmo_batch_ns_most, vmo_batch_cost_most;
/* The number of mappings in as, or -1 (and a line on the console saying
 * what) if anything about them is wrong: the tree (avl_check), the order
 * and overlaps, every gap and largest gap, two neighbours that could be
 * one, the tree by VMO page and VMAR_MAX_OVERLAP on every page, a
 * reverse-map entry that doesn't say what its mapping maps, flags
 * past the maximum, W+X on anything but a JIT VMO, the charge for the
 * structs, and every entry present: it must name the VMO's page there
 * now, with no permission the mapping lacks. Takes the region lock;
 * O(n + entries). For tests, on an address space nothing else changes
 * meanwhile (a racing decommit could clear an entry it is comparing). */
int64_t aspace_check(struct aspace *as);
/* Physical address of the page holding `offset`, 0 if not committed. */
uint64_t vmo_page_phys(struct vmo *v, uint64_t offset);
