/* User address spaces (see aspace.h; the VMO side is in aspace_vmo.h).
 *
 * Data structures. An address space is a PML4 of its own whose kernel half
 * (entries 256-511) is copied from the kernel PML4 at creation: vmm_init
 * creates every kernel-half PDPT up front, so kernel mappings made later
 * show up here too. The user half holds page tables this file allocates
 * (intermediate entries P|W|U, so the leaf alone decides access) and a list
 * of mappings sorted by address. A list, not a tree: lookups, first-fit
 * placement and splitting are O(mappings), which is fine for the handful of
 * mappings a process has; MAX_MAPPINGS bounds the worst case until a tree
 * replaces it.
 *
 * Each mapping maps [vmo_off, vmo_off+len) of one VMO at [base, base+len),
 * holds a VMO reference (through its reverse-map entry, struct vmo_umap),
 * and has its current permissions plus the most aspace_protect may grant
 * (`max`, from ASPACE_CAN_*). Page-table entries are filled lazily by
 * aspace_fault. Mappings are never merged.
 *
 * Locking, in order:
 *   - region lock (`lock`, a mutex, class "aspace"): the mapping list, and
 *     creating or freeing page-table pages. map, unmap, protect and fault
 *     hold it throughout, so a fault never races an unmap in the same
 *     address space.
 *   - "vmo" (the VMO object lock): the VMO's page table and reverse map. A
 *     fault looks the page up and installs its entry under it, and decommit
 *     and shrink clear entries under it, so those two are atomic with
 *     respect to each other.
 *   - page-table lock (`pt_lock`, "aspace page tables", a spinlock with
 *     interrupts off): every read or write of an entry. Needed because
 *     decommit and shrink edit entries without the region lock.
 * Only region-lock holders create or detach tables, so a fault that made
 * the tables for its address (pt_prepare) finds them still there when it
 * installs the entry, and decommit (which only clears leaves) never sees a
 * table disappear from under it: a detached table is unreachable from the
 * root for anyone walking under pt_lock, and is freed only after the
 * shootdown.
 *
 * Active CPUs and TLB shootdown. `active` has a bit for every CPU whose CR3
 * is this address space (aspace_switch). An unmap clears entries, then
 * reads the mask (after a full fence) and shoots down exactly those CPUs.
 * That is enough:
 *   - a CPU whose bit is set after our read set it with a locked RMW that
 *     comes after our entry stores, then loaded CR3 and walks the tables
 *     afresh: it can't cache the old entry;
 *   - a CPU that cleared its bit before our read did so after loading
 *     another CR3, which dropped all its non-global entries (user entries
 *     are never global) and paging-structure caches.
 * A CPU that switches away after our read is flushed anyway (harmless).
 * Without PCIDs nothing else can hold stale user entries.
 *
 * With PCIDs (arch/x86_64/pcid.c) the second point no longer holds:
 * a CR3 load keeps the entries of the PCID it leaves. So gather_note also
 * bumps the address space's TLB generation (`tlb_gen`) BEFORE it reads the
 * mask, and a CPU loading this address space again flush-loads its PCID
 * when the generation moved since that CPU last flushed it. The active mask
 * still says whom to interrupt: CPUs running the address space now. The
 * ordering argument is at the top of pcid.c.
 *
 * Freeing ("gather"). Pages that leave a user mapping (VMO pages on
 * decommit/shrink, page-table pages on unmap) go on a struct tlb_gather
 * and are released only after its shootdown has completed on every CPU in
 * the gathered mask, so a page is never freed while any TLB can still reach
 * it.
 *
 * Reverse map and races (vmo.c has the other half):
 *   - decommit/shrink, per batch under the VMO lock: take the pages out of
 *     the VMO's table onto a gather, clear their entries in every user
 *     mapping on the VMO's reverse map (aspace_zap_locked, no region lock
 *     needed), collect those address spaces' active CPUs; then drop the
 *     lock, shoot down, free.
 *   - A fault racing a decommit either ran first (it installed the old page
 *     under the VMO lock; the decommit clears that entry and shoots it down
 *     before freeing) or runs after (the slot is empty, so it commits a new
 *     zero page, which the decommit does not touch: the fault is ordered
 *     after the decommit of that page). Either way no entry points at a
 *     freed page, and a decommitted page reads as zeros afterwards.
 *   - unmap clears and shoots down its entries BEFORE it removes (or
 *     shrinks) the reverse-map entry. So a decommit that doesn't find the
 *     mapping any more knows its entries are gone from every TLB; one that
 *     still finds it includes this address space's CPUs in its own
 *     shootdown. (Decommit notes an address space's CPUs even if its
 *     entries were already clear: an unmap may have cleared them without
 *     having flushed yet.)
 *   - Splits add the new piece's reverse-map entry before shrinking the old
 *     one, so every mapped page stays covered at every moment; double
 *     coverage only means a page is zapped twice.
 *   - An address space whose last reference is gone may still be on VMO
 *     reverse maps until destroy unlinks it (under each VMO lock) and only
 *     then frees the page tables, so decommit never walks freed tables. No
 *     CPU can be using it (see aspace_unref), so it needs no shootdown.
 *
 * Faults past a shrunk VMO's end fail with ERR_OUT_OF_RANGE; the mapping
 * stays and works again if the VMO grows back.
 *
 * Kept mappings (ASPACE_KEPT_ONLY): only of a kept VMO (VMO_KEEP_PAGES),
 * read-only, and every entry is installed before aspace_map returns,
 * through the same vmo_fault_map a fault uses, with the page tables
 * charged then (a refusal takes the whole mapping back). A kept VMO never
 * gives up a page below its size and never shrinks (vmo.c), so nobody zaps
 * those entries; this file never clears them either while the mapping
 * lasts, since protect refuses any change to a kept mapping (no access
 * would clear them). So no access through it ever faults, whatever the
 * VMO's other holders do: a compositor reads a client's pixels without
 * trusting the client. `faults` counts aspace_fault calls, for the tests
 * that check that.
 *
 * Job charges. An address space made for a process
 * (aspace_create_charged) charges that job JOB_LIMIT_PAGES units for the
 * kernel memory it holds: 1 for the PML4 from creation to destroy, 1 for
 * every user page-table page from pt_prepare (charged BEFORE it is
 * allocated, so a refused one is never made) until it is detached or
 * freed, and 1 per ASPACE_MAPPINGS_PER_PAGE mapping structs (`map_pages`,
 * settled under the region lock whenever nmaps changes; a split the job
 * can't pay for fails the unmap/protect up front, before anything
 * changed). The charges move with pt_pages / nmaps under the same locks
 * and destroy credits whatever is left, so the job is exact. Charges are
 * lock-free atomics, fine under pt_lock.
 *
 * Known limits: no lazy TLB (switching to a kernel thread reloads the
 * kernel CR3, with PCIDs a cheap load that keeps the user entries); one
 * decommit batch holds the VMO lock (with interrupts off) for up to 512
 * pages times the number of mappings of the
 * VMO that overlap them, so a VMO mapped thousands of times makes that
 * latency grow. The region lock must never be held across a user copy:
 * the copy's fault would take it again. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/cpu.h>
#include <jam/dbghook.h>
#include <jam/ipi.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pcid.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/uentry.h>
#include <jam/vmo.h>
#include <jam/x86.h>

#define PTE_P     (1ull << 0)
#define PTE_W     (1ull << 1)
#define PTE_U     (1ull << 2)
#define PTE_PWT   (1ull << 3)
#define PTE_PCD   (1ull << 4)
#define PTE_PAT4K (1ull << 7)
#define PTE_NX    (1ull << 63)
#define PTE_ADDR  0x000ffffffffff000ull
#define PTE_CACHE (PTE_PWT | PTE_PCD | PTE_PAT4K)

#define PERMS     (ASPACE_READ | ASPACE_WRITE | ASPACE_EXEC)
#define CAN_SHIFT 8   /* ASPACE_CAN_x == ASPACE_x << CAN_SHIFT */
#define MAP_FLAGS (PERMS | ASPACE_FIXED | ASPACE_KEPT_ONLY | ASPACE_CAN_READ | ASPACE_CAN_WRITE | \
                   ASPACE_CAN_EXEC)
#define MAX_MAPPINGS 16384

#define SIZE_2M   (1ull << 21)
#define SIZE_1G   (1ull << 30)
#define SIZE_512G (1ull << 39)

_Static_assert(ASPACE_CAN_READ == ASPACE_READ << CAN_SHIFT &&
               ASPACE_CAN_WRITE == ASPACE_WRITE << CAN_SHIFT &&
               ASPACE_CAN_EXEC == ASPACE_EXEC << CAN_SHIFT, "CAN bits mirror the permissions");

struct mapping {
    struct list_node node;       /* as->maps, sorted by base (region lock) */
    struct vmo_umap  umap;       /* on the VMO's reverse map (VMO lock) */
    struct vmo      *vmo;        /* the VMO mapped (a reference) */
    uint64_t         base, len;  /* bytes, page-aligned */
    uint64_t         vmo_off;    /* VMO offset of base */
    unsigned         flags;      /* current ASPACE_READ/WRITE/EXEC */
    unsigned         max;        /* what aspace_protect may grant */
    bool             kept;       /* ASPACE_KEPT_ONLY: entries filled at map time, never changed */
};

struct aspace {
    struct mutex      lock;       /* region lock (see the file header) */
    spinlock_t        pt_lock;    /* page-table entries */
    uint64_t          pml4;       /* physical */
    uint64_t         *pml4v;      /* pml4 through the HHDM */
    struct list_node  maps;       /* struct mapping */
    uint32_t          nmaps;      /* entries on maps */
    uint64_t          map_pages;  /* pages charged for the mapping structs (region lock) */
    uint64_t          pt_pages;   /* table pages below the PML4 (pt_lock), each charged */
    struct job       *job;        /* charged for all of it (a reference), or NULL */
    uint32_t          refs;       /* references (atomic once published) */
    cpumask_t         active;     /* CPUs with this CR3 loaded (atomic bits) */
    /* PCIDs (pcid.h): a never-reused id, and the TLB generation,
     * bumped by every change that must reach TLBs (gather_note). */
    uint64_t          pcid_id;
    uint64_t          tlb_gen;
    uint64_t          faults;     /* aspace_fault calls (atomic, relaxed; for tests) */
};

static uint64_t mend(const struct mapping *m)
{
    return m->base + m->len;
}

/* W^X, and no write or execute without read (x86 can't express either). */
static bool perms_ok(unsigned p)
{
    if ((p & ASPACE_WRITE) && (p & ASPACE_EXEC))
        return false;
    return !(p & (ASPACE_WRITE | ASPACE_EXEC)) || (p & ASPACE_READ);
}

static bool range_ok(uint64_t addr, uint64_t len)
{
    return len && !((addr | len) & (PAGE_SIZE - 1)) && user_range_ok(addr, len);
}

/* ---- pages and gathers --------------------------------------------------- */

void page_unref(struct page *p)
{
    uint32_t left = __atomic_sub_fetch(&p->refcount, 1, __ATOMIC_ACQ_REL);
    if (left == UINT32_MAX)
        panic("page %lx released too many times", page_to_phys(p));
    if (left == 0)
        pmm_free_pages(p, 0);
}

void tlb_gather_init(struct tlb_gather *g)
{
    memset(g, 0, sizeof(*g));
    list_init(&g->pages);
}

void tlb_gather_page(struct tlb_gather *g, struct page *p)
{
    list_add_tail(&g->pages, &p->node);
    g->npages++;
}

/* Add as's active CPUs and [lo, hi) to g. Called after the entry stores. */
static void gather_note(struct tlb_gather *g, struct aspace *as, uint64_t lo, uint64_t hi)
{
    /* Order the entry stores before the mask read (see the file header):
     * a CPU whose bit we miss sets it after this fence, so it loads CR3
     * after our stores are visible. The generation bump (PCIDs) comes
     * first: a CPU that switches in after it flushes its PCID. */
    __atomic_add_fetch(&as->tlb_gen, 1, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (unsigned w = 0; w < MAX_CPUS / 64; w++)
        g->cpus.bits[w] |= __atomic_load_n(&as->active.bits[w], __ATOMIC_RELAXED);
    if (!g->need || lo < g->lo)
        g->lo = lo;
    if (!g->need || hi > g->hi)
        g->hi = hi;
    g->need = true;
}

void tlb_gather_finish(struct tlb_gather *g)
{
    if (g->need) {
        bool any = false;
        for (unsigned w = 0; w < MAX_CPUS / 64; w++)
            any |= g->cpus.bits[w] != 0;
        if (any)
            tlb_shootdown_mask(&g->cpus, g->lo, g->hi - g->lo);
    }
    if (g->npages)
        DBG_HOOK(DBG_GATHER_PRE_FREE, g);
    while (!list_empty(&g->pages)) {
        struct page *p = list_first(&g->pages, struct page, node);
        list_del(&p->node);
        page_unref(p);
    }
    tlb_gather_init(g);
}

/* ---- page tables ------------------------------------------------------- */

static uint64_t *tbl(uint64_t e)
{
    return phys_to_virt(e & PTE_ADDR);
}

static unsigned ix(uint64_t va, int level)
{
    return (va >> (12 + 9 * (level - 1))) & 511;
}

static bool table_empty(const uint64_t *t)
{
    for (unsigned i = 0; i < 512; i++)
        if (t[i])
            return false;
    return true;
}

static uint64_t leaf_bits(unsigned perms, unsigned cache)
{
    uint64_t e = PTE_P | PTE_U;
    if (perms & ASPACE_WRITE)
        e |= PTE_W;
    if (!(perms & ASPACE_EXEC) && cpu_features.nx)
        e |= PTE_NX;
    if (cache & VM_WC)          /* PAT index 5, as vmm.c sets it up */
        e |= PTE_PAT4K | PTE_PWT;
    else if (cache & VM_UC)     /* PAT index 3 */
        e |= PTE_PCD | PTE_PWT;
    return e;
}

/* pt_lock held: the leaf entry for va, NULL if a table is missing. */
static uint64_t *pte_find(const struct aspace *as, uint64_t va)
{
    uint64_t *t = as->pml4v;
    for (int l = 4; l > 1; l--) {
        uint64_t e = t[ix(va, l)];
        if (!(e & PTE_P))
            return NULL;
        t = tbl(e);
    }
    return &t[ix(va, 1)];
}

/* Region lock held: create the tables down to va's page table, each one
 * charged to the job before it is allocated. */
static status_t pt_prepare(struct aspace *as, uint64_t va)
{
    uint64_t f = spin_lock_irqsave(&as->pt_lock);
    uint64_t *t = as->pml4v;
    for (int l = 4; l > 1; l--) {
        uint64_t *e = &t[ix(va, l)];
        if (!(*e & PTE_P)) {
            uint64_t pa = 0;
            if (job_charge(as->job, JOB_LIMIT_PAGES, 1) == OK &&
                !(pa = pmm_alloc_page_phys(PMM_ZERO)))
                job_uncharge(as->job, JOB_LIMIT_PAGES, 1);
            if (!pa) {
                spin_unlock_irqrestore(&as->pt_lock, f);
                return ERR_NO_MEMORY;   /* tables made so far are freed by unmap/destroy */
            }
            *e = pa | PTE_P | PTE_W | PTE_U;
            as->pt_pages++;
        }
        t = tbl(*e);
    }
    spin_unlock_irqrestore(&as->pt_lock, f);
    return OK;
}

/* pt_lock held: unlink an empty table and hand it to g. */
static void table_drop(struct aspace *as, uint64_t *entry, uint64_t *t, struct tlb_gather *g)
{
    *entry = 0;
    as->pt_pages--;
    job_uncharge(as->job, JOB_LIMIT_PAGES, 1);   /* freed once g is finished */
    tlb_gather_page(g, virt_to_page(t));
}

/* What walk_leaves does to each page table: `fn` edits the leaves in
 * [va, stop) of one page table; with `detach` (region lock held), tables
 * left empty are unlinked onto g. */
struct leaf_walk {
    /* Edits one page table's leaves in [va, stop). */
    void (*fn)(uint64_t *pt, uint64_t va, uint64_t stop, unsigned perms);
    unsigned           perms;    /* passed to fn */
    bool               detach;   /* unlink the tables left empty */
    struct tlb_gather *g;        /* gets the unlinked tables, then the range */
};

/* pt_lock held: apply w to the page table covering va (up to end), if there
 * is one. Returns where the next page table's range starts. */
static uint64_t walk_one(struct aspace *as, uint64_t va, uint64_t end,
                         const struct leaf_walk *w)
{
    uint64_t *e4 = &as->pml4v[ix(va, 4)];
    if (!(*e4 & PTE_P))
        return ALIGN_UP(va + 1, SIZE_512G);
    uint64_t *pdpt = tbl(*e4), *e3 = &pdpt[ix(va, 3)];
    if (!(*e3 & PTE_P))
        return ALIGN_UP(va + 1, SIZE_1G);
    uint64_t *pd = tbl(*e3), *e2 = &pd[ix(va, 2)];
    uint64_t next = ALIGN_UP(va + 1, SIZE_2M);
    if (!(*e2 & PTE_P))
        return next;
    uint64_t *pt = tbl(*e2);
    w->fn(pt, va, next < end ? next : end, w->perms);
    if (!w->detach || !table_empty(pt))
        return next;
    table_drop(as, e2, pt, w->g);
    if (!table_empty(pd))
        return next;
    table_drop(as, e3, pd, w->g);
    if (table_empty(pdpt))
        table_drop(as, e4, pdpt, w->g);
    return next;
}

/* Walk [va, end) one page table (2 MiB) at a time under pt_lock, skipping
 * missing tables (see struct leaf_walk). Then w->g gets the range and as's
 * active CPUs. */
static void walk_leaves(struct aspace *as, uint64_t va, uint64_t end, const struct leaf_walk *w)
{
    uint64_t start = va;
    while (va < end) {
        uint64_t f = spin_lock_irqsave(&as->pt_lock);
        uint64_t next = walk_one(as, va, end, w);
        spin_unlock_irqrestore(&as->pt_lock, f);
        va = next;
    }
    gather_note(w->g, as, start, end);
}

static void leaves_clear(uint64_t *pt, uint64_t va, uint64_t stop, unsigned perms)
{
    (void)perms;
    for (; va < stop; va += PAGE_SIZE)
        pt[ix(va, 1)] = 0;
}

static void leaves_protect(uint64_t *pt, uint64_t va, uint64_t stop, unsigned perms)
{
    uint64_t bits = leaf_bits(perms, 0);
    for (; va < stop; va += PAGE_SIZE) {
        uint64_t *e = &pt[ix(va, 1)];
        if (*e & PTE_P)
            *e = (*e & (PTE_ADDR | PTE_CACHE)) | bits;
    }
}

static void zap(struct aspace *as, uint64_t va, uint64_t end, bool detach, struct tlb_gather *g)
{
    const struct leaf_walk w = { leaves_clear, 0, detach, g };
    walk_leaves(as, va, end, &w);
}

void aspace_zap_locked(struct aspace *as, uint64_t va, uint64_t len, struct tlb_gather *g)
{
    zap(as, va, va + len, false, g);
}

void aspace_set_pte_locked(struct aspace *as, uint64_t va, uint64_t pa, unsigned perms,
                           unsigned cache)
{
    uint64_t f = spin_lock_irqsave(&as->pt_lock);
    uint64_t *e = pte_find(as, va);
    if (!e)
        panic("aspace: no page table for %lx", va);
    /* No flush: the entry was either empty (never cached) or held this same
     * page, and a stale narrower entry only costs a spurious fault. */
    *e = pa | leaf_bits(perms, cache);
    spin_unlock_irqrestore(&as->pt_lock, f);
}

/* Free the page tables a PDPT points to (not the PDPT itself); returns how
 * many pages that was. */
static uint64_t free_under_pdpt(const uint64_t *pdpt)
{
    uint64_t freed = 0;
    for (unsigned j = 0; j < 512; j++) {
        if (!(pdpt[j] & PTE_P))
            continue;
        const uint64_t *pd = tbl(pdpt[j]);
        for (unsigned k = 0; k < 512; k++) {
            if (pd[k] & PTE_P) {
                pmm_free_page_phys(pd[k] & PTE_ADDR);
                freed++;
            }
        }
        pmm_free_page_phys(pdpt[j] & PTE_ADDR);
        freed++;
    }
    return freed;
}

/* Last reference: free every user table. Nothing else can reach them. */
static uint64_t free_tables(struct aspace *as)
{
    uint64_t freed = 0;
    for (unsigned i = 0; i < 256; i++) {
        uint64_t e4 = as->pml4v[i];
        if (!(e4 & PTE_P))
            continue;
        freed += free_under_pdpt(tbl(e4));
        pmm_free_page_phys(e4 & PTE_ADDR);
        freed++;
        as->pml4v[i] = 0;
    }
    return freed;
}

/* ---- lifetime ----------------------------------------------------------- */

static uint64_t map_pages_for(uint32_t nmaps)
{
    return (nmaps + ASPACE_MAPPINGS_PER_PAGE - 1) / ASPACE_MAPPINGS_PER_PAGE;
}

/* Region lock held: make the charge for mapping structs cover n mappings.
 * Only growing can fail (ERR_NO_MEMORY, nothing changed). */
static status_t maps_account(struct aspace *as, uint32_t n)
{
    uint64_t want = map_pages_for(n);
    if (want > as->map_pages) {
        status_t st = job_charge(as->job, JOB_LIMIT_PAGES, want - as->map_pages);
        if (st != OK)
            return st;
    } else if (want < as->map_pages) {
        job_uncharge(as->job, JOB_LIMIT_PAGES, as->map_pages - want);
    }
    as->map_pages = want;
    return OK;
}

status_t aspace_create(struct aspace **out)
{
    return aspace_create_charged(NULL, out);
}

status_t aspace_create_charged(struct job *job, struct aspace **out)
{
    status_t st = job_charge(job, JOB_LIMIT_PAGES, 1);   /* the PML4 */
    if (st != OK)
        return st;
    struct aspace *as = kzalloc(sizeof(*as));
    uint64_t pa = as ? pmm_alloc_page_phys(PMM_ZERO) : 0;
    if (!pa) {
        kfree(as);
        job_uncharge(job, JOB_LIMIT_PAGES, 1);
        return ERR_NO_MEMORY;
    }
    job_ref(job);
    as->job = job;
    as->pml4 = pa;
    as->pml4v = phys_to_virt(pa);
    /* The kernel half is shared: its PDPTs all exist from vmm_init on. */
    const uint64_t *k = phys_to_virt(vmm_kernel_pml4());
    for (unsigned i = 256; i < 512; i++)
        as->pml4v[i] = k[i];
    mutex_init(&as->lock, "aspace");
    spin_init(&as->pt_lock, "aspace page tables");
    list_init(&as->maps);
    as->refs = 1;
    as->pcid_id = pcid_new_id();
    *out = as;
    return OK;
}

void aspace_ref(struct aspace *as)
{
    uint32_t old = __atomic_fetch_add(&as->refs, 1, __ATOMIC_RELAXED);
    if (old == 0)
        panic("aspace: ref of a dead address space");
}

/* Nobody else has a reference, so the region lock isn't needed: only VMO
 * decommits can still see this address space, through the reverse map, and
 * unlinking each entry under its VMO lock shuts them out before the tables
 * go. No shootdown: no CPU may be using it (checked). */
static void aspace_destroy(struct aspace *as)
{
    for (unsigned w = 0; w < MAX_CPUS / 64; w++)
        if (__atomic_load_n(&as->active.bits[w], __ATOMIC_ACQUIRE))
            panic("aspace: last reference dropped while a CPU still uses it "
                  "(mask word %u = %lx)", w, as->active.bits[w]);
    while (!list_empty(&as->maps)) {
        struct mapping *m = list_first(&as->maps, struct mapping, node);
        list_del(&m->node);
        vmo_umap_remove(m->vmo, &m->umap);   /* drops the mapping's VMO reference */
        kfree(m);
    }
    uint64_t freed = free_tables(as);
    if (freed != as->pt_pages)
        panic("aspace: freed %lu table pages, owned %lu", freed, as->pt_pages);
    pmm_free_page_phys(as->pml4);
    /* Tables, mapping structs and the PML4 (the mappings are gone). */
    job_uncharge(as->job, JOB_LIMIT_PAGES, freed + as->map_pages + 1);
    job_unref(as->job);
    kfree(as);
}

void aspace_unref(struct aspace *as)
{
    uint32_t left = __atomic_sub_fetch(&as->refs, 1, __ATOMIC_ACQ_REL);
    if (left == UINT32_MAX)
        panic("aspace: unreferenced too many times");
    if (left == 0)
        aspace_destroy(as);
}

/* ---- mappings ----------------------------------------------------------- */

/* Region lock held: the mapping containing addr, or NULL. */
static struct mapping *find(const struct aspace *as, uint64_t addr)
{
    for (struct list_node *n = as->maps.next; n != &as->maps; n = n->next) {
        struct mapping *m = container_of(n, struct mapping, node);
        if (addr < m->base)
            return NULL;
        if (addr < mend(m))
            return m;
    }
    return NULL;
}

static struct mapping *next_of(const struct aspace *as, const struct mapping *m)
{
    return m->node.next == &as->maps ? NULL : container_of(m->node.next, struct mapping, node);
}

/* Publish m's current range to its reverse-map entry. */
static void umap_sync(struct mapping *m)
{
    vmo_umap_set(m->vmo, &m->umap, m->base, m->vmo_off >> PAGE_SHIFT,
                 (m->vmo_off + m->len) >> PAGE_SHIFT);
}

static void umap_fill(struct aspace *as, struct mapping *m)
{
    m->umap.as = as;
    m->umap.base = m->base;
    m->umap.first = m->vmo_off >> PAGE_SHIFT;
    m->umap.end = (m->vmo_off + m->len) >> PAGE_SHIFT;
    m->umap.writable = (m->max & ASPACE_WRITE) != 0;   /* what vmar_protect may grant */
}

/* Region lock held: split m at addr (strictly inside it). m keeps
 * [base, addr); r (preallocated) takes [addr, end) and is returned. r's
 * reverse-map entry goes in before m's shrinks, so its pages never drop
 * out of the reverse map. */
static struct mapping *split(struct aspace *as, struct mapping *m, uint64_t addr,
                             struct mapping *r)
{
    r->vmo = m->vmo;
    r->base = addr;
    r->len = mend(m) - addr;
    r->vmo_off = m->vmo_off + (addr - m->base);
    r->flags = m->flags;
    r->max = m->max;
    r->kept = m->kept;
    umap_fill(as, r);
    vmo_umap_add(m->vmo, &r->umap, false);   /* can't fail unchecked */
    list_add(&m->node, &r->node);
    as->nmaps++;
    m->len = addr - m->base;
    umap_sync(m);
    return r;
}

/* ASPACE_KEPT_ONLY's own checks: read-only (a bad combination of flags),
 * then a kept VMO. */
static status_t kept_check(const struct vmo *vmo, unsigned flags)
{
    if (!(flags & ASPACE_KEPT_ONLY))
        return OK;
    if ((flags & PERMS) != ASPACE_READ)
        return ERR_INVALID_ARGS;
    return vmo_is_kept(vmo) ? OK : ERR_WRONG_TYPE;
}

/* Region lock held: install every entry of m, a new kept mapping, as a
 * fault would (its pages are all committed and stay). ERR_NO_MEMORY if
 * the job refuses a page table; the entries made so far stay for the
 * caller to take back. */
static status_t fill_kept(struct aspace *as, const struct mapping *m)
{
    for (uint64_t off = 0; off < m->len; off += PAGE_SIZE) {
        uint64_t va = m->base + off;
        status_t st = pt_prepare(as, va);
        if (st == OK)
            st = vmo_fault_map(m->vmo, (m->vmo_off + off) >> PAGE_SHIFT, as, va, m->flags);
        if (st != OK)
            return st;
    }
    return OK;
}

/* Region lock held: take back m, just added, whose fill failed: its
 * entries and the tables left empty (shot down), then m from the list and
 * the reverse map. The caller frees m. */
static void map_undo(struct aspace *as, struct mapping *m)
{
    struct tlb_gather g;
    tlb_gather_init(&g);
    zap(as, m->base, mend(m), true, &g);
    tlb_gather_finish(&g);
    list_del(&m->node);
    as->nmaps--;
    vmo_umap_remove(m->vmo, &m->umap);
    (void)maps_account(as, as->nmaps);   /* shrinking: can't fail */
}

status_t aspace_map(struct aspace *as, struct vmo *vmo, uint64_t vmo_off, uint64_t len,
                    unsigned flags, uint64_t *addr)
{
    if (!as || !vmo || !addr || (flags & ~MAP_FLAGS))
        return ERR_INVALID_ARGS;
    unsigned perms = flags & PERMS;
    if (!perms_ok(perms) || len == 0 || ((vmo_off | len) & (PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    if ((flags & ASPACE_FIXED) && !range_ok(*addr, len))
        return ERR_INVALID_ARGS;
    if (len > USER_TOP - USER_BASE)
        return ERR_NO_RESOURCES;
    if (vmo_off > VMO_MAX_SIZE || len > VMO_MAX_SIZE)
        return ERR_OUT_OF_RANGE;   /* (so vmo_off + len can't overflow) */
    status_t st = kept_check(vmo, flags);
    if (st != OK)
        return st;

    struct mapping *m = kzalloc(sizeof(*m));
    if (!m)
        return ERR_NO_MEMORY;
    m->vmo = vmo;
    m->vmo_off = vmo_off;
    m->len = len;
    m->flags = perms;
    m->kept = (flags & ASPACE_KEPT_ONLY) != 0;
    /* A kept mapping stays as it is made: no CAN bits. */
    m->max = m->kept ? perms : perms | ((flags >> CAN_SHIFT) & PERMS);

    mutex_lock(&as->lock);
    struct list_node *pos = &as->maps;   /* insert after this */
    uint64_t base;
    if (as->nmaps >= MAX_MAPPINGS) {
        st = ERR_NO_RESOURCES;
    } else if (flags & ASPACE_FIXED) {
        base = *addr;
        for (struct list_node *n = as->maps.next; n != &as->maps; n = n->next) {
            struct mapping *o = container_of(n, struct mapping, node);
            if (mend(o) <= base) {
                pos = n;
                continue;
            }
            if (o->base < base + len)
                st = ERR_ALREADY_BOUND;
            break;
        }
    } else {
        base = USER_BASE;   /* first fit */
        for (struct list_node *n = as->maps.next; n != &as->maps; n = n->next) {
            struct mapping *o = container_of(n, struct mapping, node);
            if (o->base >= base && o->base - base >= len)
                break;
            base = mend(o);
            pos = n;
        }
        if (USER_TOP - base < len)
            st = ERR_NO_RESOURCES;
    }
    if (st == OK)
        st = maps_account(as, as->nmaps + 1);
    if (st == OK) {
        m->base = base;
        umap_fill(as, m);
        st = vmo_umap_add(vmo, &m->umap, true);   /* bounds against the VMO size */
        if (st != OK)
            (void)maps_account(as, as->nmaps);   /* give the charge back: shrinking can't fail */
    }
    if (st == OK) {
        list_add(pos, &m->node);
        as->nmaps++;
        if (m->kept && (st = fill_kept(as, m)) != OK)
            map_undo(as, m);
    }
    mutex_unlock(&as->lock);
    if (st != OK) {
        kfree(m);
        return st;
    }
    *addr = base;
    return OK;
}

status_t aspace_unmap(struct aspace *as, uint64_t addr, uint64_t len)
{
    if (!range_ok(addr, len))
        return ERR_INVALID_ARGS;
    uint64_t end = addr + len;
    mutex_lock(&as->lock);
    struct mapping *first = NULL;
    for (struct list_node *n = as->maps.next; n != &as->maps; n = n->next) {
        struct mapping *m = container_of(n, struct mapping, node);
        if (mend(m) > addr) {
            if (m->base < end)
                first = m;
            break;
        }
    }
    if (!first) {
        mutex_unlock(&as->lock);
        return ERR_NOT_FOUND;
    }
    struct mapping *spare = NULL;
    if (first->base < addr && mend(first) > end) {   /* a hole in the middle */
        status_t st = as->nmaps >= MAX_MAPPINGS ? ERR_NO_RESOURCES
                                                : maps_account(as, as->nmaps + 1);
        if (st == OK && !(spare = kzalloc(sizeof(*spare)))) {
            (void)maps_account(as, as->nmaps);   /* shrinking: can't fail */
            st = ERR_NO_MEMORY;
        }
        if (st != OK) {
            mutex_unlock(&as->lock);
            return st;
        }
    }

    /* Entries first, shot down and their empty tables freed, while the
     * reverse map still covers them (see the file header). */
    struct tlb_gather g;
    tlb_gather_init(&g);
    for (struct mapping *m = first; m && m->base < end; m = next_of(as, m)) {
        uint64_t s = m->base > addr ? m->base : addr;
        uint64_t e = mend(m) < end ? mend(m) : end;
        zap(as, s, e, true, &g);
    }
    tlb_gather_finish(&g);

    /* Then the mappings themselves. */
    for (struct mapping *m = first, *next; m && m->base < end; m = next) {
        if (m->base < addr && mend(m) > end)
            split(as, m, end, spare);   /* m becomes [base, end): a tail cut below */
        next = next_of(as, m);
        if (m->base >= addr && mend(m) <= end) {
            list_del(&m->node);
            as->nmaps--;
            vmo_umap_remove(m->vmo, &m->umap);
            kfree(m);
        } else if (m->base < addr) {
            m->len = addr - m->base;
            umap_sync(m);
        } else {
            uint64_t cut = end - m->base;
            m->base = end;
            m->vmo_off += cut;
            m->len -= cut;
            umap_sync(m);
        }
    }
    (void)maps_account(as, as->nmaps);   /* only ever down here: can't fail */
    mutex_unlock(&as->lock);
    return OK;
}

status_t aspace_protect(struct aspace *as, uint64_t addr, uint64_t len, unsigned flags)
{
    if ((flags & ~PERMS) || !perms_ok(flags) || !range_ok(addr, len))
        return ERR_INVALID_ARGS;
    uint64_t end = addr + len;
    mutex_lock(&as->lock);

    /* The whole range must be mapped, and every mapping must allow flags. */
    struct mapping *first = find(as, addr), *last = NULL;
    status_t st = first ? OK : ERR_NOT_FOUND;
    bool denied = false;
    for (struct mapping *m = first; st == OK; m = next_of(as, m)) {
        if (!m || m->base != (last ? mend(last) : m->base)) {
            st = ERR_NOT_FOUND;   /* ran out, or a gap */
            break;
        }
        denied |= (flags & ~m->max) != 0;
        denied |= m->kept && flags != m->flags;   /* its entries stay as they were filled */
        last = m;
        if (mend(m) >= end)
            break;
    }
    if (st == OK && denied)
        st = ERR_ACCESS_DENIED;
    struct mapping *spare[2] = { NULL, NULL };
    if (st == OK) {
        unsigned need = (first->base < addr) + (mend(last) > end);
        if (as->nmaps + need > MAX_MAPPINGS)
            st = ERR_NO_RESOURCES;
        else
            st = maps_account(as, as->nmaps + need);
        for (unsigned i = 0; i < need && st == OK; i++)
            if (!(spare[i] = kzalloc(sizeof(struct mapping))))
                st = ERR_NO_MEMORY;
        if (st == ERR_NO_MEMORY)
            (void)maps_account(as, as->nmaps);   /* back to what we have: can't fail */
    }
    if (st != OK) {
        mutex_unlock(&as->lock);
        kfree(spare[0]);
        kfree(spare[1]);
        return st;
    }

    unsigned used = 0;
    if (first->base < addr) {
        bool same = first == last;
        first = split(as, first, addr, spare[used++]);
        if (same)
            last = first;
    }
    if (mend(last) > end)
        split(as, last, end, spare[used++]);

    struct tlb_gather g;
    tlb_gather_init(&g);
    for (struct mapping *m = first; m && m->base < end; m = next_of(as, m)) {
        m->flags = flags;
        if (flags & ASPACE_READ) {
            const struct leaf_walk w = { leaves_protect, flags, false, &g };
            walk_leaves(as, m->base, mend(m), &w);
        } else {
            zap(as, m->base, mend(m), true, &g);   /* no access: x86 can't say "present" */
        }
    }
    tlb_gather_finish(&g);
    mutex_unlock(&as->lock);
    return OK;
}

status_t aspace_fault(struct aspace *as, uint64_t addr, unsigned access)
{
    if (addr < USER_BASE || addr >= USER_TOP)
        return ERR_NOT_FOUND;
    unsigned need = access & PERMS;
    if (!need)
        need = ASPACE_READ;
    __atomic_add_fetch(&as->faults, 1, __ATOMIC_RELAXED);
    mutex_lock(&as->lock);
    struct mapping *m = find(as, addr);
    status_t st;
    if (!m) {
        st = ERR_NOT_FOUND;
    } else if (need & ~m->flags) {
        st = ERR_ACCESS_DENIED;
    } else {
        uint64_t va = ALIGN_DOWN(addr, PAGE_SIZE);
        st = pt_prepare(as, va);
        if (st == OK)
            st = vmo_fault_map(m->vmo, (m->vmo_off + (va - m->base)) >> PAGE_SHIFT, as, va,
                               m->flags);
    }
    mutex_unlock(&as->lock);
    return st;
}

/* ---- switching ---------------------------------------------------------- */

void aspace_switch(struct aspace *prev, struct aspace *next)
{
    if (prev == next)
        return;
    ASSERT(!irqs_enabled());
    uint32_t cpu = this_cpu()->index;
    uint64_t bit = 1ull << (cpu % 64);
    if (next) {
        /* Locked RMW before the CR3 load (and before pcid_load reads the
         * generation): see the file header. */
        __atomic_fetch_or(&next->active.bits[cpu / 64], bit, __ATOMIC_SEQ_CST);
        pcid_load(next->pml4, next->pcid_id, &next->tlb_gen);
    } else {
        pcid_load(vmm_kernel_pml4(), 0, NULL);
    }
    /* Only after the new CR3 has dropped prev's entries from this CPU. */
    if (prev)
        __atomic_fetch_and(&prev->active.bits[cpu / 64], ~bit, __ATOMIC_SEQ_CST);
}

uint64_t aspace_pml4(struct aspace *as)
{
    return as ? as->pml4 : vmm_kernel_pml4();
}

/* ---- for tests ---------------------------------------------------------- */

uint64_t aspace_pte(struct aspace *as, uint64_t va)
{
    uint64_t f = spin_lock_irqsave(&as->pt_lock);
    uint64_t *e = pte_find(as, va);
    uint64_t v = e ? *e : 0;
    spin_unlock_irqrestore(&as->pt_lock, f);
    return v;
}

uint64_t aspace_pt_pages(struct aspace *as)
{
    uint64_t f = spin_lock_irqsave(&as->pt_lock);
    uint64_t n = as->pt_pages;
    spin_unlock_irqrestore(&as->pt_lock, f);
    return n;
}

uint64_t aspace_fault_count(struct aspace *as)
{
    return __atomic_load_n(&as->faults, __ATOMIC_RELAXED);
}

uint32_t aspace_mapping_count(struct aspace *as)
{
    mutex_lock(&as->lock);
    uint32_t n = as->nmaps;
    mutex_unlock(&as->lock);
    return n;
}
