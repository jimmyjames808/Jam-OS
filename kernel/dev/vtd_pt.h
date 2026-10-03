/* VT-d second-stage page tables: one domain's map from the addresses its
 * devices use (IOVAs) to physical memory (Intel VT-d specification 4.1,
 * 3.7 and 9.8, legacy mode).
 *
 * The model:
 *   - IOVA = physical address: a page is mapped at its own address, so
 *     the numbers a driver writes into its device are the ones vmo_pin
 *     gave it, and what isn't pinned for the domain isn't in its table
 *     (docs/M11-PLAN.md, question 1). Only 4 KiB leaves, read and write,
 *     with the snoop bit where the unit has snoop control.
 *   - Pin counts: two pins of one page in one domain share the mapping.
 *     A leaf keeps its pin count in the entry's software bits (52-61,
 *     ignored by the unit), and is cleared when the count drops to 0. A
 *     table entry keeps, in the same bits, how many entries of the table
 *     it points to are in use, so a table that empties is found without
 *     scanning it.
 *   - The unit reads the tables from memory, maybe without snooping the
 *     CPU's caches (ECAP.C = 0). Every entry the unit may read is flushed
 *     (ops->flush) after it is written: a new table page in full before
 *     the entry that links it, a leaf before anything else may depend on
 *     it. Only the software bits change unflushed: the unit ignores them.
 *   - The unit caches translations. Removing a mapping records its range
 *     in a struct vtd_pt_gather, and a table page that empties goes on the
 *     gather's list instead of being freed. The caller invalidates the
 *     unit's caches for the gather and waits (vtd_pt_gather_finish calls
 *     ops->invalidate), and only then are the table pages freed: the same
 *     rule as the CPU's TLB gather (<jam/aspace_vmo.h>). A unit in caching
 *     mode (CAP.CM = 1: it may cache not-present entries) also needs new
 *     mappings invalidated, so with caching_mode they are recorded too.
 *   - Table pages are kernel memory a driver makes the kernel hold by
 *     pinning: each is charged to the domain's job (JOB_LIMIT_PAGES)
 *     before it is allocated, and a domain holds at most max_tables of
 *     them, the root and pages waiting in a gather included. Over either
 *     limit a map fails and changes nothing.
 *   - Address width: the depth (3 or 4 levels) is one CAP.SAGAW offers,
 *     and an IOVA must lie below 2^addr_bits, the smallest of MGAW, what
 *     the depth covers and the host address width.
 *
 * The module knows nothing of units, queues or domains' ids: its page
 * allocator, its cache flush and its invalidation are function pointers
 * (struct vtd_pt_ops), so it is tested without hardware
 * (kernel/test/test_vtd_pt.c).
 *
 * Locking: none inside. The caller serialises every call on one table
 * (the domain's lock); ops->invalidate may wait, so that lock is a mutex
 * or is dropped around vtd_pt_gather_finish. Tables of different domains
 * are independent. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/list.h>
#include <jam/status.h>

struct job;
struct vtd_pt_gather;

#define VTD_PT_MAX_PINS   1023u   /* a leaf's pin count: 10 software bits */
#define VTD_PT_RUNS       8       /* ranges a gather lists before it says "whole domain" */

/* What the module needs from its user. ctx is vtd_pt.ctx. */
struct vtd_pt_ops {
    /* One page for a table: its physical address, or 0 when there is
     * none. The contents don't matter (the module clears it). It must be
     * a page of the allocator's (struct page): a gather lists it through
     * its `node`. */
    uint64_t (*alloc_page)(void *ctx);
    /* Give back a page from alloc_page. */
    void (*free_page)(void *ctx, uint64_t pa);
    /* Make the CPU's writes to [va, va + len) visible to the unit's page
     * walks (CLFLUSHOPT over the lines, then SFENCE). Not called when the
     * unit's walks are coherent. */
    void (*flush)(void *ctx, const void *va, size_t len);
    /* Invalidate the unit's caches for g's ranges in this domain (all of
     * the domain's entries when g->whole) and wait for it: OK, or the
     * error (ERR_TIMED_OUT). With g->ntables != 0 tables were unlinked,
     * so the paging-structure caches go too (IH = 0); every unlinked
     * table's span contains one of g's ranges. */
    status_t (*invalidate)(void *ctx, const struct vtd_pt_gather *g);
};

/* The shape of a domain's tables, from the unit's registers. */
struct vtd_pt_geom {
    uint8_t levels;      /* 3 or 4 */
    uint8_t addr_bits;   /* IOVAs lie below 2^addr_bits */
    bool    snoop;       /* set SNP in leaves (ECAP.SC) */
    bool    coherent;    /* page walks snoop the CPU's caches (ECAP.C): no flushes */
    bool    caching_mode;/* CAP.CM: new mappings need invalidating too */
};

/* Pick the geometry for a unit: the shallowest depth CAP.SAGAW offers
 * that covers MGAW (else the deepest it offers, with addresses limited to
 * what that covers), addr_bits further limited by haw (the DMAR table's
 * host address width; 0: no limit of its own). ERR_NOT_SUPPORTED if the
 * unit offers neither 3 nor 4 levels. Pure. */
status_t vtd_pt_geom_from_caps(uint64_t cap, uint64_t ecap, unsigned haw,
                               struct vtd_pt_geom *out);

/* One domain's tables. Fields are the module's; read them through the
 * functions below. */
struct vtd_pt {
    struct vtd_pt_geom       geom;        /* from vtd_pt_init */
    const struct vtd_pt_ops *ops;         /* the user's functions */
    void                    *ctx;         /* their argument */
    struct job              *job;         /* charged for table pages (a reference; NULL: nobody) */
    uint64_t                 root;        /* the top table's physical address */
    uint32_t                 root_used;   /* entries in use in the top table */
    uint32_t                 tables;      /* table pages held: linked, plus waiting in gathers */
    uint32_t                 pending;     /* of those, waiting in gathers */
    uint32_t                 max_tables;  /* the cap on `tables` */
    uint64_t                 mapped;      /* leaves present (pages mapped) */
};

/* Mappings removed (or, in caching mode, added) and table pages to free
 * once the unit has been told. On the caller's stack; start each with
 * vtd_pt_gather_init. */
struct vtd_pt_gather {
    struct {
        uint64_t base;       /* first IOVA */
        uint64_t pages;      /* 4 KiB pages from it */
    } run[VTD_PT_RUNS];
    uint32_t         nruns;  /* runs in run[] */
    bool             whole;  /* more ranges than run[] holds: the whole domain */
    struct list_node tables; /* table pages (struct page node) to free afterwards */
    uint32_t         ntables;/* pages on that list */
};

void vtd_pt_gather_init(struct vtd_pt_gather *g);
/* Does g need ops->invalidate (a range recorded)? */
bool vtd_pt_gather_needed(const struct vtd_pt_gather *g);

/* An empty domain: allocates and clears the top table (charged to job,
 * which may be NULL and gets a reference otherwise). max_tables >= 1.
 * ERR_INVALID_ARGS for a geometry the module can't build, ERR_NO_MEMORY
 * (job's page limit, or no page). */
status_t vtd_pt_init(struct vtd_pt *pt, const struct vtd_pt_geom *geom,
                     const struct vtd_pt_ops *ops, void *ctx, struct job *job,
                     uint32_t max_tables);

/* Free every table page and drop the job's reference. Only once the unit
 * can no longer reach the tables: no context entry names the domain any
 * more and that change's invalidation completed. The mappings' pages are
 * not the module's: their pins are the caller's to release.
 * ERR_BAD_STATE (nothing freed) while a gather still holds table pages. */
status_t vtd_pt_destroy(struct vtd_pt *pt);

/* Map n pages at IOVA = pa (page-aligned): a page not mapped yet gets a
 * leaf with pin count 1, a mapped one its count + 1. All or nothing: on
 * failure no count has changed, and table pages that the attempt left
 * empty are on g. ERR_INVALID_ARGS (unaligned pa, n = 0), ERR_OUT_OF_RANGE
 * (past addr_bits), ERR_NO_RESOURCES (the max_tables cap, or a page
 * already pinned VTD_PT_MAX_PINS times), ERR_NO_MEMORY (the job's limit,
 * or no page). In caching mode the new leaves are recorded on g. Finish g
 * whatever the result. */
status_t vtd_pt_map(struct vtd_pt *pt, uint64_t pa, uint64_t n, struct vtd_pt_gather *g);

/* Drop one pin of each of n pages at IOVA = pa: a leaf whose count drops
 * to 0 is cleared and recorded on g, and a table page left empty goes on
 * g's list. All or nothing: ERR_NOT_FOUND (a page not mapped) changes
 * nothing; ERR_INVALID_ARGS, ERR_OUT_OF_RANGE as for map. The pages may be
 * reused only after vtd_pt_gather_finish returned OK. */
status_t vtd_pt_unmap(struct vtd_pt *pt, uint64_t pa, uint64_t n, struct vtd_pt_gather *g);

/* Invalidate (ops->invalidate, when g recorded a range), then free g's
 * table pages. On an invalidation error nothing is freed and g is kept as
 * it was: the caller may try again, or keep the pages forever (they are
 * still counted in `tables`); never free them otherwise. On OK, g is empty
 * again. */
status_t vtd_pt_gather_finish(struct vtd_pt *pt, struct vtd_pt_gather *g);

/* What the CPU's copy of the tables says for iova: OK with its pin count,
 * or ERR_NOT_FOUND. For tests and the debug command. */
status_t vtd_pt_lookup(const struct vtd_pt *pt, uint64_t iova, uint32_t *out_pins);

/* For the context entry: the top table's physical address, and the
 * address-width field (AW: 1 for 3 levels, 2 for 4). */
static inline uint64_t vtd_pt_root(const struct vtd_pt *pt) { return pt->root; }
static inline unsigned vtd_pt_aw(const struct vtd_pt *pt) { return pt->geom.levels - 2u; }

/* ---- the entry format (VT-d 9.8), for the tests ---------------------------- */

#define VTD_PTE_R        (1ull << 0)     /* read allowed */
#define VTD_PTE_W        (1ull << 1)     /* write allowed */
#define VTD_PTE_PS       (1ull << 7)     /* a superpage (never set here) */
#define VTD_PTE_SNP      (1ull << 11)    /* snoop, whatever the request says */
#define VTD_PTE_ADDR     0x000ffffffffff000ull   /* bits 51:12 */
#define VTD_PTE_SW_SHIFT 52              /* software bits 61:52 */
#define VTD_PTE_SW_MASK  (0x3ffull << VTD_PTE_SW_SHIFT)
