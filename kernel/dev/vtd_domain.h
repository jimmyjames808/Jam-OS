/* VT-d DMA remapping in legacy mode: the root and context tables, domains
 * and their ids, and which domain each PCI function's context entry names
 * (Intel VT-d specification 4.1, 3.4.2, 9.1, 9.3, 6.2.2.1). What
 * vtd_domain.c and vtd_boot.c share; the kernel's interface is
 * <jam/iommu.h>, the units' is vtd_internal.h.
 *
 * The model, per started unit (struct vtd_ctl):
 *   - one root table (a page: an entry per bus) and a context table (a
 *     page: an entry per device and function) for every bus that has a
 *     function the unit covers. Built at boot (vtd_boot.c) before the unit
 *     is pointed at it; root entries never change afterwards;
 *   - domains (struct vtd_dom): a domain id and a second-stage page table
 *     (vtd_pt) holding what the domain maps. Pass-through (TT = 10b) is
 *     never used: a function reaches what its domain maps or nothing.
 *     Every context entry naming a domain carries its id, and
 *     ids are never shared between different tables (6.2.2.1), so the
 *     unit's caches, tagged by id, can't mix two domains up. Id 0 is never
 *     used (reserved under CM = 1, 9.3; simpler to skip always);
 *   - per function (struct vtd_fn): its current domain and its home (the
 *     blocking domain, or its RMRR boot domain).
 * A context entry is always rewritten whole with one 16-byte atomic write
 * (CMPXCHG16B), so the unit never sees a new table with an old id
 * (6.2.2.1), then its line is flushed (unless ECAP.C) and, when the old
 * entry was present (or the unit caches not-present ones, CAP.CM), the
 * old entry's caches are invalidated: device-selective context cache,
 * then the old domain's IOTLB (Table 25), and waited for.
 *
 * Fault-processing disable ("mute", VT-d 9.3 FPD): a function whose DMA
 * faulted VTD_FAULT_LOGGED times gets FPD in its context entry, so a
 * device stuck retrying can't keep the fault registers full. Attaching it
 * to a new domain (a driver) clears it and starts the function's count
 * again, so a new driver is muted only for its own faults. (The log's
 * counts per requester, vtd_fault.c, run for the whole boot.)
 *
 * Locks, in order: "vtd context" (a mutex per unit: its context tables,
 * its functions' state, its domain ids) before "vtd domain" (a mutex per
 * domain: its page table) before the unit's "vtd queue" (taken inside
 * every invalidation). Both mutexes are held across invalidation waits
 * (bounded: 100 ms each). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/sched.h>

#include "vtd_internal.h"

struct pci_dev;
struct dmar_info;
struct dmar_scope;

/* ---- the entries (9.1, 9.3) ----------------------------------------------------------- */

/* A root or context entry: 128 bits, 16-byte aligned in its table. */
struct vtd_ctx {
    uint64_t lo;   /* bits 63:0 */
    uint64_t hi;   /* bits 127:64 */
} __attribute__((aligned(16)));
_Static_assert(sizeof(struct vtd_ctx) == 16, "a 128-bit entry");

#define VTD_ROOT_P        (1ull << 0)              /* root entry present */
#define VTD_ROOT_CTP      0x000ffffffffff000ull    /* the context table, 63:12 (51:12 kept) */
#define VTD_CTX_P         (1ull << 0)              /* context entry present */
#define VTD_CTX_FPD       (1ull << 1)              /* fault processing disable */
#define VTD_CTX_TT_SHIFT  2                        /* translation type, 3:2 */
#define VTD_CTX_TT_SS     0ull                     /* untranslated requests: second stage */
#define VTD_CTX_SSPTPTR   0x000ffffffffff000ull    /* the page table, 63:12 */
#define VTD_CTX_AW(hi)    ((unsigned)((hi) & 7))   /* hi 2:0 = bits 66:64 */
#define VTD_CTX_DID_SHIFT 8                        /* hi 23:8 = bits 87:72 */
#define VTD_CTX_DID(hi)   ((uint16_t)((hi) >> VTD_CTX_DID_SHIFT))
#define VTD_RTADDR_TTM(r) VTD_BITS(r, 10, 2)       /* 11.4.5: 00 legacy, 01 scalable, 11 abort */

/* The entries, pure. A context entry: present, domain did, second-stage
 * translation through the table at `table` with address width aw (9.3's
 * encoding: 1 = 39-bit, 2 = 48-bit, 3 = 57-bit), fpd. */
struct vtd_ctx vtd_root_entry(uint64_t ctx_table);
struct vtd_ctx vtd_ctx_entry(uint16_t did, uint64_t table, unsigned aw, bool fpd);

/* ---- domains ----------------------------------------------------------------------------- */

struct vtd_dom {
    struct vtd_unit_domain ud;     /* the unit and the domain id (vtd_pt's ctx) */
    const char            *what;   /* for the log: "blocking", "driver", ... */
    struct vtd_pt          pt;     /* its tables; "vtd domain" */
    struct mutex           lock;   /* "vtd domain": pt */
    uint32_t               users;  /* context entries naming it; "vtd context" */
};

/* ---- a unit's tables ---------------------------------------------------------------------- */

#define VTD_TT_MAX_DIDS 65536u   /* CAP.ND's largest */

struct vtd_ctl {
    struct vtd_unit *unit;            /* the started unit; NULL: no tables */
    bool             live;            /* tables in use, translation on (release/acquire) */
    struct mutex     lock;            /* "vtd context": everything below */
    uint64_t         root_phys;       /* the root table */
    struct vtd_ctx  *root;
    struct vtd_ctx  *ctx[256];        /* the context table per bus, NULL: none */
    uint64_t        *did_used;        /* a bit per domain id; id 0 always set */
    uint32_t         ndid;            /* domain ids the unit has (CAP.ND) */
    struct vtd_dom  *blocking;        /* empty table: every driverless function */
    uint32_t         nfn, nboot;      /* functions covered; boot domains made */
};

/* Per PCI function (pci_dev.index). */
struct vtd_fn {
    struct pci_dev  *dev;          /* the function */
    struct vtd_ctl  *ctl;          /* its unit's tables; NULL: not translated */
    uint16_t         sid;          /* requester id: bus 15:8, device 7:3, function 2:0 */
    bool             muted;        /* FPD set in its entry; "vtd context" */
    uint32_t         dma_faults;   /* DMA faults seen since its last attach (log thread);
                                    * "vtd context" */
    struct vtd_dom  *home;         /* blocking, or its boot domain */
    struct vtd_dom  *cur;          /* what its entry names; "vtd context" */
};

/* Does a started unit cover dev (its context entry is that unit's)?
 * True before iommu_boot has looked (nothing known yet). Lock-free:
 * written at boot. */
bool vtd_fn_covered(const struct pci_dev *dev);

/* The unit number i's tables (live or being built), or NULL. */
struct vtd_ctl *vtd_ctl_get(uint32_t i);
/* The function's state, or NULL (iommu=off, or no unit covers it). */
struct vtd_fn *vtd_fn_of(const struct pci_dev *dev);

/* A cleared root table page for u (9.1: every entry not present), flushed
 * from the CPU's caches unless u's walks snoop (ECAP.C): its physical
 * address, or 0 (no memory). */
uint64_t vtd_root_table_new(const struct vtd_unit *u);

/* Set up u's tables (vtd_ctl_get(u->index) afterwards: an empty root
 * table, the domain-id map) and fns[] for every PCI function (vtd_boot.c
 * fills in which unit covers which). ERR_NO_MEMORY. Boot only. */
status_t vtd_ctl_init(struct vtd_unit *u);
status_t vtd_fns_init(void);
struct vtd_fn *vtd_fn_at(uint32_t pci_index);

/* A domain id, or 0 when the unit has none left. "vtd context" held (or
 * the boot, before the unit is live). */
uint16_t vtd_did_alloc(struct vtd_ctl *ctl);
/* Give one back: invalidate the caches it tagged (domain-selective
 * context cache and IOTLB, Table 25's "re-use") if the unit is live, then
 * free it. ERR_TIMED_OUT, ERR_IO: kept (never reused). */
status_t vtd_did_free(struct vtd_ctl *ctl, uint16_t did);

/* A new domain with an empty page table (max_tables table pages at most,
 * charged to job). ERR_NO_RESOURCES (no id), ERR_NO_MEMORY,
 * ERR_NOT_SUPPORTED (a geometry vtd_pt can't build). "vtd context" held
 * (or the boot). */
status_t vtd_dom_new(struct vtd_ctl *ctl, struct job *job, uint32_t max_tables, const char *what,
                     struct vtd_dom **out);
/* Free a domain no entry names (users 0): vtd_did_free, then its tables.
 * ERR_BAD_STATE, or vtd_did_free's error (nothing freed). */
status_t vtd_dom_free(struct vtd_dom *d);
/* Map/unmap pages[0..n) at their own addresses (iommu_map's contract).
 * Takes "vtd domain". */
status_t vtd_dom_map(struct vtd_dom *d, const uint64_t *pages, size_t n);
status_t vtd_dom_unmap(struct vtd_dom *d, const uint64_t *pages, size_t n);
/* Map [pa, pa + pages * 4 KiB) (an RMRR). */
status_t vtd_dom_map_range(struct vtd_dom *d, uint64_t pa, uint64_t pages);

/* ---- context entries ---------------------------------------------------------------------- */

/* The entry for f naming d, with f's mute. Pure but for reading f. */
struct vtd_ctx vtd_fn_entry(const struct vtd_fn *f, const struct vtd_dom *d);
/* Before the unit is live (the boot): write f's entry naming d (its
 * context table made if needed). ERR_NO_MEMORY. "vtd context" held. */
status_t vtd_fn_place(struct vtd_fn *f, struct vtd_dom *d);
/* Live: switch f's entry to d (16-byte atomic), invalidate the old one's
 * caches and wait. "vtd context" held. ERR_TIMED_OUT, ERR_IO: the entry
 * is d's all the same (the hardware may still use the old one until a
 * later invalidation completes). */
status_t vtd_fn_switch_locked(struct vtd_fn *f, struct vtd_dom *d);
/* The same, taking "vtd context". */
status_t vtd_fn_switch(struct vtd_fn *f, struct vtd_dom *d);
/* f's context entry as the unit reads it (a copy). "vtd context" held or
 * not: for the tests and the log. */
struct vtd_ctx vtd_fn_read(const struct vtd_fn *f);

/* A DMA fault (reason below 20h) from (unit, sid), seen by the fault log
 * thread (vtd_fault.c): counted per function; at VTD_FAULT_LOGGED the
 * function is muted. Thread context. */
void vtd_domain_fault_seen(uint32_t unit, uint16_t sid, uint32_t reason);

/* ---- the boot (vtd_boot.c) ---------------------------------------------------------------- */

/* Point u at the root table root_phys and make translation on: Set Root
 * Table Pointer (with the global context-cache then IOTLB invalidations
 * unless CAP.ESRTPS, 6.6), then TE if it is off, then the protected memory
 * regions off if on. With translation already on (the firmware's, or a
 * kexec'd kernel's) it is never turned off: the unit switches tables in
 * flight (6.6). ERR_TIMED_OUT, ERR_IO, ERR_NOT_SUPPORTED (on, in a mode
 * other than legacy, and the unit can't change mode while on: !ESRTPS).
 * Boot, and the takeover test. */
status_t vtd_boot_handover(struct vtd_unit *u, uint64_t root_phys);

/* The early RMRR reservation's pure part: overlay [base, limit] (limit
 * inclusive, both page-aligned outwards) as BOOT_MEM_RESERVED onto the
 * usable and loader-reclaimable parts of map[0..*n) (capacity cap). The
 * bytes taken in *out_bytes. ERR_NO_RESOURCES (the map is full: unchanged
 * from there on). */
struct boot_mem_region;
status_t vtd_rmrr_carve(struct boot_mem_region *map, size_t *n, size_t cap, uint64_t base,
                        uint64_t limit, uint64_t *out_bytes);

/* Map every RMRR the DMAR table lists for f's function into d (its boot
 * domain at boot, and every domain made for it later: 3.16). OK when it
 * has none. vtd_dom_map_range's errors. */
status_t vtd_boot_map_rmrrs(const struct vtd_fn *f, struct vtd_dom *d);

/* The probe's parsed table, and its scope follower (vtd_probe.c). */
const struct dmar_info *vtd_dmar_info(void);
struct pci_dev *vtd_scope_fn(uint16_t seg, const struct dmar_scope *s, int *bus);
