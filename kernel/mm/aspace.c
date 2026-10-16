/* User address spaces (see aspace.h; the VMO side is in aspace_vmo.h).
 *
 * Data structures. An address space is a PML4 of its own whose kernel half
 * (entries 256-511) is copied from the kernel PML4 at creation: vmm_init
 * creates every kernel-half PDPT up front, so kernel mappings made later
 * show up here too. The user half holds page tables this file allocates
 * (intermediate entries P|W|U, so the leaf alone decides access) and the
 * mappings, in a balanced tree ordered by address (<jam/avl.h>), so the
 * fault path's lookup is O(log n) however many there are (MAX_MAPPINGS at
 * most). Each mapping also keeps its `gap`, the free bytes just below it
 * (down to the mapping before, or to USER_BASE), and the tree keeps the
 * largest gap of each subtree, so first-fit placement goes straight down
 * to the lowest gap that fits: O(log n) too. A change keeps the gaps
 * right: every mapping whose base or predecessor's end moved has its gap
 * fixed (gap_fix), which updates the summaries above it.
 *
 * Each mapping maps [vmo_off, vmo_off+len) of one VMO at [base, base+len),
 * holds a VMO reference (through its reverse-map entry, struct vmo_umap),
 * and has its current permissions plus the most aspace_protect may grant
 * (`max`, from ASPACE_CAN_*). Page-table entries are filled lazily by
 * aspace_fault. Every mapping is also in `vmaps`, by VMO and VMO page (an
 * interval tree: "Mappings over the same pages", below).
 *
 * Merging. Two neighbouring mappings of one VMO at contiguous offsets,
 * with the same permissions and the same maximum, neither kept, are one
 * mapping (can_merge): map, replace and protect merge what they leave
 * that way, so no two neighbours ever could be (the invariant), and a
 * program that commits pieces of a big reservation (protect to read-write)
 * and decommits them again (back to no access) leaves one mapping, not
 * thousands. A merge changes no entry and no gap: the two mapped the same
 * pages with the same permissions. Protect doesn't split a mapping that
 * has the permissions already. The cap counts what a call leaves, merges
 * done (maps_reserve; merges_for and protect_merges say how many there
 * will be before anything changes); the job is charged for the most
 * structs a call holds on the way.
 *
 * Locking, in order:
 *   - region lock (`lock`, a mutex, class "aspace"): the mapping tree, and
 *     creating or freeing page-table pages. map, unmap, protect and fault
 *     hold it throughout, so a fault never races an unmap in the same
 *     address space. The one exception is a fault on a pager VMO's absent
 *     page (<jam/pager.h>): it asks the pager under the lock, drops the
 *     lock to wait (seconds, maybe), holding a VMO reference of its own,
 *     and then starts again from find, trusting nothing it saw before: the
 *     mapping may be gone, moved or changed by then. FAULT_ROUNDS bounds
 *     the retries.
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
 * A protect that found no entry present in its range and let no table go
 * asks for no shootdown at all (`quiet`): V8's commit of untouched
 * reservation pages, thousands of times, would otherwise interrupt every
 * CPU running the process each time. Nothing can be stale then: under the
 * region lock only the reverse map's walks (decommit, shrink, a
 * separation) clear entries behind our back, and each shoots this
 * address space down itself before it lets the page go, so a TLB entry
 * that outlives our protect reaches only a page on its way out of the
 * VMO, with the permissions it had a moment before. The reverse map's own
 * walks always ask: one may find entries an unmap cleared and hasn't shot
 * down yet, and must not free the page before that shootdown.
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
 *     coverage only means a page is zapped twice. A merge the other way
 *     round: the survivor's entry grows over the other's pages before the
 *     other's goes.
 *   - An address space whose last reference is gone may still be on VMO
 *     reverse maps until destroy unlinks it (under each VMO lock) and only
 *     then frees the page tables, so decommit never walks freed tables. No
 *     CPU can be using it (see aspace_unref), so it needs no shootdown.
 *
 * Replacing (ASPACE_REPLACE, MAP_FIXED's call) is an unmap and a map under
 * one hold of the region lock: every check that can fail (the cap, the
 * charge, the spare struct, the new mapping's reverse-map entry, which
 * checks the VMO's size) comes first, then the range is cut exactly as an
 * unmap cuts it and the new mapping goes in. A fault on the range waits
 * on the region lock throughout, so it finds the old mapping or the new
 * one. The new entry is on the new VMO's reverse map a moment before the
 * old entries go: a decommit of the new VMO then clears entries of the
 * old mapping too, which only costs a fault.
 *
 * Copy-on-write (vmo_create_child, vmo.c's header): a page that more than
 * one VMO's table holds is never mapped writable. A child's creation takes
 * the write bit off the parent's entries (aspace_wrprotect_locked, under
 * the VMO lock) and shoots them down before it returns; a read fault
 * installs such a page read-only; a write fault copies it, and under the
 * VMO lock puts the copy in the table, clears the old page's entries in
 * every mapping of that VMO (the reverse map) and installs the copy for
 * the faulting address, then shoots the old entries down before the
 * access is retried, all while the faulting address space's region lock
 * is held. aspace_protect never adds the write bit to an entry: the next
 * write faults instead, and the fault decides.
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
 * W^X, and its one exception: JIT VMOs (docs/CR-PLAN.md section 12).
 * No user page-table entry is ever writable and executable at once unless
 * it maps a page of a JIT VMO (vmo_make_jit) through a mapping whose
 * handle had both RIGHT_WRITE and RIGHT_EXEC. The argument:
 *   1. Every user leaf entry gets its write and execute bits from
 *      leaf_bits(perms) with perms a mapping's `flags` (a fault through
 *      vmo_fault_map, a kept fill, leaves_protect, which may only take the
 *      write bit away); nothing else in the kernel writes user leaves but
 *      clearing (zap) and write-protecting (copy-on-write).
 *   2. A mapping's `flags` are set in three places only: mapping_new
 *      (aspace_map), protect_entries (aspace_protect) and split, which
 *      copies them to a piece of the same mapping (same VMO). A merge
 *      joins two mappings of one VMO with equal flags. So W+X flags exist
 *      only where aspace_map or aspace_protect allowed them.
 *   3. aspace_map allows them (perms_ok's `jit`) only when the VMO is a
 *      JIT VMO, and aspace_protect (protect_span) only when every mapping
 *      in the range maps one; each also needs ASPACE_CAN_WRITE and
 *      ASPACE_CAN_EXEC in the mapping's `max`. sys_vmar_map sets those
 *      from the handle's rights (userboot sets them itself and never maps
 *      a JIT VMO: none exists then). A mapping's VMO never changes.
 *   4. A VMO becomes a JIT VMO only through vmo_make_jit, once, for good,
 *      while nothing maps it and it holds no page (vmo.c), and only
 *      sys_vmo_make_jit calls that: with RIGHT_ROOT_JIT on the root and
 *      the VMO's only handle in hand, which comes back with RIGHT_EXEC.
 *      Every other handle that has RIGHT_EXEC (the boot image's, and
 *      vmo_make_exec's and _verified's, of sealed VMOs) lacks RIGHT_WRITE,
 *      and a JIT VMO is never sealed, so no sealed (approved) code is ever
 *      writable and no JIT memory is ever approved code.
 * So a program without RIGHT_ROOT_JIT gets W+X only through a JIT VMO
 * handle that a holder of the power chose to give it, with both rights
 * (the owner's rule: handles are delegable); a handle passed on without
 * RIGHT_WRITE maps read and execute only, and without RIGHT_EXEC never
 * executes. A JIT mapping's entries are otherwise ordinary: protect,
 * faults, decommit and shrink treat them as any other's, and a JIT VMO has
 * no copy-on-write child (its pages are never shared, so its writable
 * entries are never write-protected by one).
 *
 * Job charges. An address space made for a process
 * (aspace_create_charged) charges that job JOB_LIMIT_PAGES units for the
 * kernel memory it holds: 1 for the PML4 from creation to destroy, 1 for
 * every user page-table page from pt_prepare (charged BEFORE it is
 * allocated, so a refused one is never made, and the ones made for it
 * before are taken back) until it is detached or
 * freed, and 1 per ASPACE_MAPPINGS_PER_PAGE mapping structs (`map_pages`,
 * settled under the region lock whenever nmaps changes; a split the job
 * can't pay for fails the unmap/protect up front, before anything
 * changed). The charges move with pt_pages / nmaps under the same locks
 * and destroy credits whatever is left, so the job is exact. Charges are
 * lock-free atomics, fine under pt_lock.
 *
 * Mappings over the same pages (CR1 finding 1). A decommit batch holds the
 * VMO lock, with interrupts off, while it clears its pages from every
 * mapping over them (found through the VMO's interval tree, O(log n) each,
 * so mappings elsewhere in the VMO cost nothing), so the mappings over one
 * page are what a program could pile up to stall a CPU. Two bounds: an
 * address space holds at most VMAR_MAX_OVERLAP mappings of one VMO over
 * any one page (map and replace count them in `vmaps`, a second tree of
 * the mappings by VMO page, under the region lock only, so the count costs
 * nobody else; splits and merges never add to it), and a batch is cut
 * short so its walk stays within VMO_BATCH_BUDGET (vmo.c). What is left is
 * one page under the cap's 64 mappings from each of many address spaces:
 * a batch of its own (64 address spaces: 0.23 to 0.45 ms on the PC, where
 * one address space with 65,535 used to take 2.4 to 2.9 ms).
 *
 * Known limits: no lazy TLB (switching to a kernel thread reloads the
 * kernel CR3, with PCIDs a cheap load that keeps the user entries). The
 * region lock must never be held across a user copy: the copy's fault
 * would take it again. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/avl.h>
#include <jam/cpu.h>
#include <jam/dbghook.h>
#include <jam/ipi.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/pager.h>
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
#define MAP_FLAGS (PERMS | ASPACE_FIXED | ASPACE_KEPT_ONLY | ASPACE_REPLACE | ASPACE_CAN_READ | \
                   ASPACE_CAN_WRITE | ASPACE_CAN_EXEC)
#define MAX_MAPPINGS VMAR_MAX_MAPPINGS   /* <jam/abi.h>: 65536 */

#define SIZE_2M   (1ull << 21)
#define SIZE_1G   (1ull << 30)
#define SIZE_512G (1ull << 39)

_Static_assert(ASPACE_CAN_READ == ASPACE_READ << CAN_SHIFT &&
               ASPACE_CAN_WRITE == ASPACE_WRITE << CAN_SHIFT &&
               ASPACE_CAN_EXEC == ASPACE_EXEC << CAN_SHIFT, "CAN bits mirror the permissions");

struct mapping {
    struct avl_node  node;       /* in as->maps, ordered by base (region lock) */
    uint64_t         gap;        /* free bytes right below base: from the end of the
                                    mapping before, or from USER_BASE (region lock) */
    uint64_t         max_gap;    /* the largest gap in node's subtree (region lock) */
    struct vmo_umap  umap;       /* on the VMO's reverse map (VMO lock) */
    struct vmo      *vmo;        /* the VMO mapped (a reference) */
    uint64_t         base, len;  /* bytes, page-aligned */
    uint64_t         vmo_off;    /* VMO offset of base */
    unsigned         flags;      /* current ASPACE_READ/WRITE/EXEC */
    unsigned         max;        /* what aspace_protect may grant */
    bool             kept;       /* ASPACE_KEPT_ONLY: entries filled at map time, never changed */
    struct avl_node  vnode;      /* in as->vmaps, by VMO then VMO offset (region lock) */
    uintptr_t        vmax_vmo;   /* the highest (VMO, end page) in vnode's subtree */
    uint64_t         vmax_end;   /* (region lock) */
};
_Static_assert(sizeof(struct mapping) <= PAGE_SIZE / ASPACE_MAPPINGS_PER_PAGE,
               "the page charged per ASPACE_MAPPINGS_PER_PAGE mappings holds them");

struct aspace {
    struct mutex      lock;       /* region lock (see the file header) */
    spinlock_t        pt_lock;    /* page-table entries */
    uint64_t          pml4;       /* physical */
    uint64_t         *pml4v;      /* pml4 through the HHDM */
    struct avl_tree   maps;       /* struct mapping, by base (region lock) */
    struct avl_tree   vmaps;      /* the same, by VMO page (vnode; region lock) */
    uint32_t          nmaps;      /* mappings in maps (and in vmaps) */
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
    bool              jit;        /* has mapped a JIT VMO (atomic, set once; PROCESS_FLAG_JIT) */
};

static uint64_t mend(const struct mapping *m)
{
    return m->base + m->len;
}

/* W^X unless jit (a JIT VMO's mapping: the file header), and no write or
 * execute without read (x86 can't express either). */
static bool perms_ok(unsigned p, bool jit)
{
    if ((p & ASPACE_WRITE) && (p & ASPACE_EXEC) && !jit)
        return false;
    return !(p & (ASPACE_WRITE | ASPACE_EXEC)) || (p & ASPACE_READ);
}

static bool perms_wx(unsigned p)
{
    return (p & ASPACE_WRITE) && (p & ASPACE_EXEC);
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

void tlb_gather_shared(struct tlb_gather *g, struct page *p)
{
    ASSERT(g->nshared < TLB_GATHER_SHARED);
    g->shared[g->nshared++] = p;
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
    for (uint32_t i = 0; i < g->nshared; i++)
        page_share_drop(g->shared[i]);
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

/* pt_lock held: unlink an empty table and hand it to g. */
static void table_drop(struct aspace *as, uint64_t *entry, uint64_t *t, struct tlb_gather *g)
{
    *entry = 0;
    as->pt_pages--;
    job_uncharge(as->job, JOB_LIMIT_PAGES, 1);   /* freed once g is finished */
    tlb_gather_page(g, virt_to_page(t));
}

/* Region lock held: take back the tables pt_prepare made for va before
 * the job refused the next one (made[0..n), the entries that link them,
 * top down). Nothing is under them, but a CPU running the address space
 * may have cached a link to one already: they go through a shootdown,
 * as table_drop's do. Else nothing would ever free them before the
 * address space died (a walk lets a table go only once the one under it
 * has gone), charged all that time. */
static void pt_unprepare(struct aspace *as, uint64_t va, uint64_t **made, unsigned n)
{
    struct tlb_gather g;
    tlb_gather_init(&g);
    uint64_t f = spin_lock_irqsave(&as->pt_lock);
    while (n--)
        table_drop(as, made[n], tbl(*made[n]), &g);
    spin_unlock_irqrestore(&as->pt_lock, f);
    gather_note(&g, as, va, va + PAGE_SIZE);
    tlb_gather_finish(&g);
}

/* Region lock held: create the tables down to va's page table, each one
 * charged to the job before it is allocated. ERR_NO_MEMORY if the job
 * refuses one (or there is none): then those made here are taken back. */
static status_t pt_prepare(struct aspace *as, uint64_t va)
{
    uint64_t *made[3];
    unsigned n = 0;
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
                if (n)   /* (only region-lock holders change the tables: still there) */
                    pt_unprepare(as, va, made, n);
                return ERR_NO_MEMORY;
            }
            *e = pa | PTE_P | PTE_W | PTE_U;
            as->pt_pages++;
            made[n++] = e;
        }
        t = tbl(*e);
    }
    spin_unlock_irqrestore(&as->pt_lock, f);
    return OK;
}

/* What walk_leaves does to each page table: `fn` edits the leaves in
 * [va, stop) of one page table; with `detach` (region lock held), tables
 * left empty are unlinked onto g. */
struct leaf_walk {
    /* Edits one page table's leaves in [va, stop); returns how many of
     * them were present. */
    uint64_t (*fn)(uint64_t *pt, uint64_t va, uint64_t stop, unsigned perms);
    unsigned           perms;    /* passed to fn */
    bool               detach;   /* unlink the tables left empty */
    /* No shootdown if no entry was present and no table went (a protect:
     * the file header says why); else, and always without it, g gets the
     * range and the address space's CPUs. */
    bool               quiet;
    struct tlb_gather *g;        /* gets the unlinked tables, then the range */
};

/* pt_lock held: apply w to the page table covering va (up to end), if there
 * is one, adding the entries present and the tables let go to *seen.
 * Returns where the next page table's range starts. */
static uint64_t walk_one(struct aspace *as, uint64_t va, uint64_t end,
                         const struct leaf_walk *w, uint64_t *seen)
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
    *seen += w->fn(pt, va, next < end ? next : end, w->perms);
    if (!w->detach || !table_empty(pt))
        return next;
    table_drop(as, e2, pt, w->g);
    (*seen)++;
    if (!table_empty(pd))
        return next;
    table_drop(as, e3, pd, w->g);
    if (table_empty(pdpt))
        table_drop(as, e4, pdpt, w->g);
    return next;
}

/* Walk [va, end) one page table (2 MiB) at a time under pt_lock, skipping
 * missing tables (see struct leaf_walk). Then w->g gets the range and as's
 * active CPUs, unless w is quiet and found nothing. */
static void walk_leaves(struct aspace *as, uint64_t va, uint64_t end, const struct leaf_walk *w)
{
    uint64_t start = va, seen = 0;
    while (va < end) {
        uint64_t f = spin_lock_irqsave(&as->pt_lock);
        uint64_t next = walk_one(as, va, end, w, &seen);
        spin_unlock_irqrestore(&as->pt_lock, f);
        va = next;
    }
    if (!w->quiet || seen)
        gather_note(w->g, as, start, end);
}

static uint64_t leaves_clear(uint64_t *pt, uint64_t va, uint64_t stop, unsigned perms)
{
    (void)perms;
    uint64_t n = 0;
    for (; va < stop; va += PAGE_SIZE) {
        n += pt[ix(va, 1)] != 0;
        pt[ix(va, 1)] = 0;
    }
    return n;
}

/* The write bit is only ever kept, never added: an entry may be read-only
 * because its page is shared copy-on-write (vmo.c), and only a fault
 * (vmo_fault_map, under the VMO lock) may decide it can be written. */
static uint64_t leaves_protect(uint64_t *pt, uint64_t va, uint64_t stop, unsigned perms)
{
    uint64_t bits = leaf_bits(perms, 0), n = 0;
    for (; va < stop; va += PAGE_SIZE) {
        uint64_t *e = &pt[ix(va, 1)];
        if (*e & PTE_P) {
            *e = (*e & (PTE_ADDR | PTE_CACHE)) | (bits & ~PTE_W) | (bits & *e & PTE_W);
            n++;
        }
    }
    return n;
}

static uint64_t leaves_wrprotect(uint64_t *pt, uint64_t va, uint64_t stop, unsigned perms)
{
    (void)perms;
    uint64_t n = 0;
    for (; va < stop; va += PAGE_SIZE) {
        n += pt[ix(va, 1)] != 0;
        pt[ix(va, 1)] &= ~PTE_W;
    }
    return n;
}

static void zap(struct aspace *as, uint64_t va, uint64_t end, bool detach, struct tlb_gather *g)
{
    const struct leaf_walk w = { leaves_clear, 0, detach, false, g };
    walk_leaves(as, va, end, &w);
}

void aspace_zap_locked(struct aspace *as, uint64_t va, uint64_t len, struct tlb_gather *g)
{
    zap(as, va, va + len, false, g);
}

void aspace_wrprotect_locked(struct aspace *as, uint64_t va, uint64_t len, struct tlb_gather *g)
{
    const struct leaf_walk w = { leaves_wrprotect, 0, false, false, g };
    walk_leaves(as, va, va + len, &w);
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

/* ---- the mapping tree --------------------------------------------------- */

static struct mapping *map_of(const struct avl_node *n)
{
    return n ? container_of(n, struct mapping, node) : NULL;
}

static uint64_t max_gap_of(const struct avl_node *n)
{
    return n ? map_of(n)->max_gap : 0;
}

/* The tree's summary (<jam/avl.h>): the largest gap in n's subtree. */
static void gap_update(struct avl_node *n)
{
    struct mapping *m = map_of(n);
    uint64_t l = max_gap_of(n->left), r = max_gap_of(n->right);
    uint64_t g = l > r ? l : r;
    m->max_gap = m->gap > g ? m->gap : g;
}

static struct mapping *first_map(const struct aspace *as)
{
    return map_of(avl_first(&as->maps));
}

static struct mapping *next_of(const struct mapping *m)
{
    return map_of(avl_next(&m->node));
}

static struct mapping *prev_of(const struct mapping *m)
{
    return map_of(avl_prev(&m->node));
}

/* Region lock held: the last mapping with base <= addr, or NULL. */
static struct mapping *floor_of(const struct aspace *as, uint64_t addr)
{
    struct mapping *best = NULL;
    for (const struct avl_node *n = as->maps.root; n;) {
        struct mapping *m = map_of(n);
        if (m->base <= addr) {
            best = m;
            n = n->right;
        } else {
            n = n->left;
        }
    }
    return best;
}

/* Region lock held: the first mapping with base >= addr, or NULL. */
static struct mapping *ceil_of(const struct aspace *as, uint64_t addr)
{
    struct mapping *best = NULL;
    for (const struct avl_node *n = as->maps.root; n;) {
        struct mapping *m = map_of(n);
        if (m->base >= addr) {
            best = m;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return best;
}

/* Region lock held: m's gap from the mapping before it, and the summaries
 * above it (nothing for NULL). */
static void gap_fix(struct aspace *as, struct mapping *m)
{
    if (!m)
        return;
    struct mapping *p = prev_of(m);
    m->gap = m->base - (p ? mend(p) : USER_BASE);
    avl_propagate(&as->maps, &m->node);
}

static void vmap_insert(struct aspace *as, struct mapping *m);

/* Region lock held: put m (its range set, not in the trees) right after
 * prev (NULL: first); the gap after it shrinks. */
static void map_link(struct aspace *as, struct mapping *m, struct mapping *prev)
{
    m->gap = m->base - (prev ? mend(prev) : USER_BASE);
    avl_insert_after(&as->maps, &m->node, prev ? &prev->node : NULL);
    vmap_insert(as, m);
    as->nmaps++;
    gap_fix(as, next_of(m));
}

/* Region lock held: take m out of the trees; the gap after it grows. */
static void map_unlink(struct aspace *as, struct mapping *m)
{
    struct mapping *n = next_of(m);
    avl_erase(&as->maps, &m->node);
    avl_erase(&as->vmaps, &m->vnode);
    as->nmaps--;
    gap_fix(as, n);
}

/* Region lock held: the lowest address from USER_BASE up with len free
 * bytes (*base), and the mapping that will be right before it (*prev, NULL
 * for none). ERR_NO_RESOURCES if no gap is that big. The gaps the tree
 * keeps lead the way down: a subtree whose largest gap is too small is
 * never entered, so this is O(log n). */
static status_t first_fit(const struct aspace *as, uint64_t len, uint64_t *base,
                          struct mapping **prev)
{
    struct mapping *at = NULL;   /* the lowest mapping with a gap of len below it */
    for (const struct avl_node *n = as->maps.root; n && !at;) {
        if (max_gap_of(n->left) >= len)
            n = n->left;
        else if (map_of(n)->gap >= len)
            at = map_of(n);
        else
            n = max_gap_of(n->right) >= len ? n->right : NULL;
    }
    struct mapping *p = at ? prev_of(at) : map_of(avl_last(&as->maps));
    uint64_t b = p ? mend(p) : USER_BASE;
    if (!at && USER_TOP - b < len)
        return ERR_NO_RESOURCES;   /* not after the last one either */
    *base = b;
    *prev = p;
    return OK;
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

/* ---- mappings by VMO page (VMAR_MAX_OVERLAP) ------------------------------ */

/* `vmaps` holds every mapping a second time, by what it maps: it is
 * <jam/itree.h>'s interval tree on a line where each VMO has a stretch of
 * its own (positions are pairs, the VMO's address and then a page of it,
 * compared in that order), each node keeping the highest (VMO, end page)
 * of its subtree. So itree's search, with pairs for numbers, finds this
 * address space's mappings over some pages of one VMO, and only those, in
 * O(log n) each, however many it maps of other VMOs or elsewhere in this
 * one. map counts them before it adds one (overlap_room). */

static struct mapping *vmap_of(const struct avl_node *n)
{
    return n ? container_of(n, struct mapping, vnode) : NULL;
}

static uint64_t vfirst(const struct mapping *m)
{
    return m->vmo_off >> PAGE_SHIFT;
}

static uint64_t vend(const struct mapping *m)
{
    return (m->vmo_off + m->len) >> PAGE_SHIFT;
}

/* Whether page pa of the VMO at a lies before page pb of the VMO at b. */
static bool vpos_before(uintptr_t a, uint64_t pa, uintptr_t b, uint64_t pb)
{
    return a < b || (a == b && pa < pb);
}

/* What m's summary should be: the highest (VMO, end page) of its own and
 * its children's. */
static void vmax_of(const struct mapping *m, uintptr_t *v, uint64_t *end)
{
    *v = (uintptr_t)m->vmo;
    *end = vend(m);
    const struct mapping *c[2] = { vmap_of(m->vnode.left), vmap_of(m->vnode.right) };
    for (unsigned i = 0; i < 2; i++) {
        if (c[i] && vpos_before(*v, *end, c[i]->vmax_vmo, c[i]->vmax_end)) {
            *v = c[i]->vmax_vmo;
            *end = c[i]->vmax_end;
        }
    }
}

/* The tree's summary (<jam/avl.h>). */
static void vmax_update(struct avl_node *n)
{
    struct mapping *m = vmap_of(n);
    vmax_of(m, &m->vmax_vmo, &m->vmax_end);
}

/* Region lock held: m (its VMO and range set) into vmaps. */
static void vmap_insert(struct aspace *as, struct mapping *m)
{
    struct avl_node *parent = NULL;
    bool right = false;
    for (struct avl_node *n = as->vmaps.root; n;) {
        const struct mapping *o = vmap_of(n);
        parent = n;
        right = !vpos_before((uintptr_t)m->vmo, vfirst(m), (uintptr_t)o->vmo, vfirst(o));
        n = right ? n->right : n->left;
    }
    avl_link(&as->vmaps, &m->vnode, parent, right);
}

/* A search of vmaps: the mappings over pages [lo, hi) of the VMO at v. */
struct vsearch {
    uintptr_t v;
    uint64_t  lo, hi;
};

/* Does something in m's subtree end past the search's first page? */
static bool vs_reaches(const struct vsearch *s, const struct mapping *m)
{
    return m && vpos_before(s->v, s->lo, m->vmax_vmo, m->vmax_end);
}

/* Does m start at or past the search's end (and so everything after it)? */
static bool vs_past(const struct vsearch *s, const struct mapping *m)
{
    return !vpos_before((uintptr_t)m->vmo, vfirst(m), s->v, s->hi);
}

/* Does m itself end past the search's first page? */
static bool vs_ends_past(const struct vsearch *s, const struct mapping *m)
{
    return vpos_before(s->v, s->lo, (uintptr_t)m->vmo, vend(m));
}

/* itree.c's subtree_search: the first match in m's subtree (which reaches
 * past s->lo), or NULL if there is none from there on. */
static struct mapping *vs_subtree(const struct vsearch *s, struct mapping *m)
{
    for (;;) {
        struct mapping *l = vmap_of(m->vnode.left);
        if (vs_reaches(s, l)) {
            m = l;
            continue;
        }
        if (vs_past(s, m))
            return NULL;
        if (vs_ends_past(s, m))
            return m;
        struct mapping *r = vmap_of(m->vnode.right);
        if (!vs_reaches(s, r))
            return NULL;
        m = r;
    }
}

/* Region lock held: the first mapping in vmaps order over s's pages, or
 * NULL; then each next one (vs_next with the same search, nothing changed
 * meanwhile) until NULL. */
static struct mapping *vs_first(const struct aspace *as, const struct vsearch *s)
{
    struct mapping *root = vmap_of(as->vmaps.root);
    if (s->lo >= s->hi || !vs_reaches(s, root))
        return NULL;
    return vs_subtree(s, root);
}

static struct mapping *vs_next(const struct vsearch *s, const struct mapping *m)
{
    const struct avl_node *a = &m->vnode;
    for (;;) {
        struct mapping *r = vmap_of(a->right);
        if (vs_reaches(s, r))
            return vs_subtree(s, r);
        const struct avl_node *from;
        do {
            from = a;
            a = a->parent;
            if (!a)
                return NULL;
        } while (a->right == from);
        struct mapping *p = vmap_of(a);
        if (vs_past(s, p))
            return NULL;
        if (vs_ends_past(s, p))
            return p;
    }
}

/* overlap_room's heap: the n smallest-first page numbers in h. */
static void heap_push(uint64_t *h, unsigned *n, uint64_t x)
{
    unsigned i = (*n)++;
    for (; i && h[(i - 1) / 2] > x; i = (i - 1) / 2)
        h[i] = h[(i - 1) / 2];
    h[i] = x;
}

static void heap_pop(uint64_t *h, unsigned *n)
{
    uint64_t x = h[--*n];
    unsigned i = 0;
    for (unsigned c; (c = 2 * i + 1) < *n; i = c) {
        if (c + 1 < *n && h[c + 1] < h[c])
            c++;
        if (h[c] >= x)
            break;
        h[i] = h[c];
    }
    h[i] = x;
}

/* Region lock held: are fewer than `most` (at most VMAR_MAX_OVERLAP + 1)
 * of as's mappings of v over each page of [first, end)? A mapping wholly
 * inside the addresses [skip, skip_end) doesn't count (a replace unmaps it
 * first); one the replace only cuts counts whole. The mappings come by
 * first page, so the count is highest at the first page of one of them: a
 * sweep keeps where the ones still over the page end, in a heap of fewer
 * than `most`. O(log most) for each mapping of v over [first, end), and
 * only as's own mappings: the region lock is all it holds. */
static bool overlap_room(const struct aspace *as, const struct vmo *v, uint64_t first,
                         uint64_t end, uint64_t skip, uint64_t skip_end, unsigned most)
{
    uint64_t ends[VMAR_MAX_OVERLAP];
    unsigned n = 0;
    struct vsearch s = { (uintptr_t)v, first, end };
    for (struct mapping *m = vs_first(as, &s); m; m = vs_next(&s, m)) {
        if (m->base >= skip && mend(m) <= skip_end)
            continue;
        uint64_t lo = vfirst(m) > first ? vfirst(m) : first;
        while (n && ends[0] <= lo)
            heap_pop(ends, &n);
        if (n + 1 >= most)   /* m and the n still open: over page lo */
            return false;
        heap_push(ends, &n, vend(m) < end ? vend(m) : end);
    }
    return true;
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
    avl_init(&as->maps, gap_update);
    avl_init(&as->vmaps, vmax_update);
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
    for (struct mapping *m; (m = first_map(as));) {
        avl_erase(&as->maps, &m->node);
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
    struct mapping *m = floor_of(as, addr);
    return m && addr < mend(m) ? m : NULL;
}

/* Region lock held: the first mapping that overlaps [addr, end), or NULL. */
static struct mapping *first_in(const struct aspace *as, uint64_t addr, uint64_t end)
{
    struct mapping *m = floor_of(as, addr);
    if (!m || mend(m) <= addr)
        m = m ? next_of(m) : first_map(as);
    return m && m->base < end ? m : NULL;
}

/* Region lock held: publish m's current range to its reverse-map entry,
 * and move it to its place in vmaps. */
static void umap_sync(struct aspace *as, struct mapping *m)
{
    vmo_umap_set(m->vmo, &m->umap, m->base, m->vmo_off >> PAGE_SHIFT,
                 (m->vmo_off + m->len) >> PAGE_SHIFT);
    avl_erase(&as->vmaps, &m->vnode);
    vmap_insert(as, m);
}

static void umap_fill(struct aspace *as, struct mapping *m)
{
    m->umap.as = as;
    m->umap.base = m->base;
    m->umap.pages.first = m->vmo_off >> PAGE_SHIFT;
    m->umap.pages.end = (m->vmo_off + m->len) >> PAGE_SHIFT;
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
    m->len = addr - m->base;
    umap_sync(as, m);
    map_link(as, r, m);
    return r;
}

/* ASPACE_KEPT_ONLY's own checks: read-only (a bad combination of flags),
 * then a kept VMO (ERR_NOT_SUPPORTED for another: it is a VMO, the right
 * type, of a kind this mapping can't be made of). */
static status_t kept_check(const struct vmo *vmo, unsigned flags)
{
    if (!(flags & ASPACE_KEPT_ONLY))
        return OK;
    if ((flags & PERMS) != ASPACE_READ)
        return ERR_INVALID_ARGS;
    return vmo_is_kept(vmo) ? OK : ERR_NOT_SUPPORTED;
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
            st = vmo_fault_map(m->vmo, (m->vmo_off + off) >> PAGE_SHIFT, as, va, m->flags,
                               false);
        if (st != OK)
            return st;
    }
    return OK;
}

/* Region lock held: take back m, just added, whose fill failed: its
 * entries and the tables left empty (shot down), then m from the tree and
 * the reverse map. The caller frees m. */
static void map_undo(struct aspace *as, struct mapping *m)
{
    struct tlb_gather g;
    tlb_gather_init(&g);
    zap(as, m->base, mend(m), true, &g);
    tlb_gather_finish(&g);
    map_unlink(as, m);
    vmo_umap_remove(m->vmo, &m->umap);
    (void)maps_account(as, as->nmaps);   /* shrinking: can't fail */
}

/* A new mapping struct for aspace_map (not placed yet), or NULL. */
static struct mapping *mapping_new(struct vmo *vmo, uint64_t vmo_off, uint64_t len,
                                   unsigned flags)
{
    struct mapping *m = kzalloc(sizeof(*m));
    if (!m)
        return NULL;
    unsigned perms = flags & PERMS;
    m->vmo = vmo;
    m->vmo_off = vmo_off;
    m->len = len;
    m->flags = perms;
    m->kept = (flags & ASPACE_KEPT_ONLY) != 0;
    /* A kept mapping stays as it is made: no CAN bits. */
    m->max = m->kept ? perms : perms | ((flags >> CAN_SHIFT) & PERMS);
    return m;
}

/* ---- merging -------------------------------------------------------------- */

/* Whether a (with permissions fa) and b right after it (with fb) can be one
 * mapping: the same VMO at contiguous addresses and offsets, the same
 * permissions and maximum, and neither kept (a kept mapping stays as it was
 * made). The cache mode is the VMO's, so it matches too. */
static bool joinable(const struct mapping *a, unsigned fa, const struct mapping *b, unsigned fb)
{
    return mend(a) == b->base && a->vmo == b->vmo && a->vmo_off + a->len == b->vmo_off &&
           fa == fb && a->max == b->max && !a->kept && !b->kept;
}

static bool can_merge(const struct mapping *a, const struct mapping *b)
{
    return joinable(a, a->flags, b, b->flags);
}

/* Region lock held: if m and the mapping after it can be one, make them
 * one: m grows over it and it goes. m's reverse-map entry grows before the
 * other's goes, so its pages never drop out of the reverse map. No entry
 * changes (both mapped those pages with the same permissions), and no gap:
 * the one below the mapping that goes was 0, the one after it stays. True
 * if they merged. */
static bool merge_next(struct aspace *as, struct mapping *m)
{
    struct mapping *n = next_of(m);
    if (!n || !can_merge(m, n))
        return false;
    m->len += n->len;
    umap_sync(as, m);
    avl_erase(&as->maps, &n->node);
    avl_erase(&as->vmaps, &n->vnode);
    as->nmaps--;
    vmo_umap_remove(n->vmo, &n->umap);   /* never the last reference: m holds one */
    kfree(n);
    return true;
}

/* Region lock held: merge m, just placed, with the mappings on either side
 * where they can be one. m may be freed. */
static void merge_around(struct aspace *as, struct mapping *m)
{
    struct mapping *p = prev_of(m);
    if (p && can_merge(p, m)) {
        merge_next(as, p);
        m = p;
    }
    merge_next(as, m);
}

/* Region lock held: merge every pair of neighbours that can be one, from m
 * on, up to the pair whose second mapping starts at or past end. */
static void merge_range(struct aspace *as, struct mapping *m, uint64_t end)
{
    while (m) {
        struct mapping *n = next_of(m);
        if (!n)
            return;
        if (merge_next(as, m))
            continue;   /* m grew over n: look at its new neighbour */
        if (n->base >= end)
            return;
        m = n;
    }
}

/* Region lock held: how many neighbours m (its range set, not in the tree)
 * merges with once [m->base, mend(m)) is cleared for it, 0 to 2: on each
 * side, the piece a mapping it cuts keeps, or the mapping right beside the
 * range. */
static uint32_t merges_for(const struct aspace *as, const struct mapping *m)
{
    uint32_t n = 0;
    struct mapping *lo = floor_of(as, m->base - 1);   /* m->base >= USER_BASE: no wrap */
    if (lo) {
        struct mapping l = *lo;   /* a copy to measure, never linked */
        if (mend(lo) > m->base)
            l.len = m->base - lo->base;   /* the piece kept below the range */
        n += can_merge(&l, m);
    }
    struct mapping *hi = find(as, mend(m));
    if (hi) {
        struct mapping r = *hi;
        uint64_t cut = mend(m) - hi->base;   /* > 0: the piece kept above the range */
        r.base += cut;
        r.vmo_off += cut;
        r.len -= cut;
        n += can_merge(m, &r);
    }
    return n;
}

/* ---- map, unmap, protect -------------------------------------------------- */

/* Region lock held: room for a change that leaves `final` mappings and has
 * at most `peak` on the way (its splits come before its merges): the cap,
 * on final (ERR_NO_RESOURCES past MAX_MAPPINGS; so a change that merges
 * back what it splits fits even at the cap), the charge for the structs
 * at the peak (ERR_NO_MEMORY; the caller settles it with maps_account when
 * done), and nspare spare structs in spare[] for the splits
 * (ERR_NO_MEMORY). Nothing changed on an error. */
static status_t maps_reserve(struct aspace *as, uint32_t final, uint32_t peak, unsigned nspare,
                             struct mapping **spare)
{
    if (final > MAX_MAPPINGS)
        return ERR_NO_RESOURCES;
    status_t st = maps_account(as, peak > as->nmaps ? peak : as->nmaps);
    if (st != OK)
        return st;
    for (unsigned i = 0; i < nspare; i++) {
        spare[i] = kzalloc(sizeof(struct mapping));
        if (spare[i])
            continue;
        for (unsigned j = 0; j < i; j++) {
            kfree(spare[j]);
            spare[j] = NULL;
        }
        (void)maps_account(as, as->nmaps);   /* shrinking: can't fail */
        return ERR_NO_MEMORY;
    }
    return OK;
}

/* Region lock held: take [addr, end) out of every mapping from first (the
 * first that overlaps it) on. Entries first, shot down and their empty
 * tables freed, while the reverse map still covers them (see the file
 * header); then the mappings are trimmed, freed, or split with spare
 * (needed when first reaches past both ends). The caller settles the
 * charge. Can't fail. Only one gap changes in the end, the one below the
 * first mapping from end on: it is fixed last, once. */
static void cut_locked(struct aspace *as, struct mapping *first, uint64_t addr, uint64_t end,
                       struct mapping *spare)
{
    struct tlb_gather g;
    tlb_gather_init(&g);
    for (struct mapping *m = first; m && m->base < end; m = next_of(m)) {
        uint64_t s = m->base > addr ? m->base : addr;
        uint64_t e = mend(m) < end ? mend(m) : end;
        zap(as, s, e, true, &g);
    }
    tlb_gather_finish(&g);

    for (struct mapping *m = first, *next; m && m->base < end; m = next) {
        if (m->base < addr && mend(m) > end)
            split(as, m, end, spare);   /* m becomes [base, end): a tail cut below */
        next = next_of(m);
        if (m->base >= addr && mend(m) <= end) {
            avl_erase(&as->maps, &m->node);
            avl_erase(&as->vmaps, &m->vnode);
            as->nmaps--;
            vmo_umap_remove(m->vmo, &m->umap);
            kfree(m);
        } else if (m->base < addr) {
            m->len = addr - m->base;
            umap_sync(as, m);
        } else {
            uint64_t cut = end - m->base;
            m->base = end;
            m->vmo_off += cut;
            m->len -= cut;
            umap_sync(as, m);
        }
    }
    gap_fix(as, ceil_of(as, end));
}

/* Region lock held: put m (not in the tree yet) at *addr (ASPACE_FIXED) or
 * the first gap that fits, fill it if it is kept, and merge it with its
 * neighbours. *addr: where it went. m may be freed by then (merged). */
static status_t map_place_locked(struct aspace *as, struct mapping *m, unsigned flags,
                                 uint64_t *addr)
{
    struct mapping *prev = NULL;   /* the mapping m goes after */
    status_t st = OK;
    m->base = *addr;
    if (flags & ASPACE_FIXED) {
        if (first_in(as, m->base, mend(m)))
            st = ERR_ALREADY_BOUND;
        else
            prev = floor_of(as, m->base);
    } else {
        st = first_fit(as, m->len, &m->base, &prev);
    }
    if (st == OK && !overlap_room(as, m->vmo, vfirst(m), vend(m), 0, 0, VMAR_MAX_OVERLAP))
        st = ERR_NO_RESOURCES;
    if (st == OK)
        st = maps_reserve(as, as->nmaps + 1 - merges_for(as, m), as->nmaps + 1, 0, NULL);
    if (st == OK) {
        umap_fill(as, m);
        st = vmo_umap_add(m->vmo, &m->umap, true);   /* bounds against the VMO size */
        if (st != OK)
            (void)maps_account(as, as->nmaps);   /* give the charge back: shrinking can't fail */
    }
    if (st != OK)
        return st;
    map_link(as, m, prev);
    if (m->kept && (st = fill_kept(as, m)) != OK) {
        map_undo(as, m);
        return st;
    }
    *addr = m->base;
    merge_around(as, m);
    (void)maps_account(as, as->nmaps);   /* down to what is left: can't fail */
    return OK;
}

/* Region lock held: m (not in the tree yet) at *addr, in place of whatever
 * [*addr, *addr + m->len) holds (ASPACE_REPLACE; see the file header),
 * then merged with its neighbours (m may be freed). */
static status_t map_replace_locked(struct aspace *as, struct mapping *m, const uint64_t *addr)
{
    m->base = *addr;
    uint64_t base = m->base, end = mend(m);
    struct mapping *first = first_in(as, base, end), *spare = NULL;
    uint32_t inside = 0;   /* mappings the range holds whole: they go */
    for (struct mapping *o = first; o && o->base < end; o = next_of(o))
        inside += o->base >= base && mend(o) <= end;
    bool split = first && first->base < base && mend(first) > end;
    uint32_t peak = as->nmaps - inside + split + 1;
    if (!overlap_room(as, m->vmo, vfirst(m), vend(m), base, end, VMAR_MAX_OVERLAP))
        return ERR_NO_RESOURCES;   /* what the range holds whole doesn't count */
    status_t st = maps_reserve(as, peak - merges_for(as, m), peak, split, &spare);
    if (st != OK)
        return st;
    umap_fill(as, m);
    st = vmo_umap_add(m->vmo, &m->umap, true);   /* bounds against the VMO size */
    if (st != OK) {
        kfree(spare);
        (void)maps_account(as, as->nmaps);   /* back to what we have: can't fail */
        return st;
    }
    if (first)
        cut_locked(as, first, base, end, spare);
    map_link(as, m, floor_of(as, base));   /* nothing overlaps the range now */
    merge_around(as, m);
    (void)maps_account(as, as->nmaps);   /* at most the reservation: can't fail */
    return OK;
}

status_t aspace_map(struct aspace *as, struct vmo *vmo, uint64_t vmo_off, uint64_t len,
                    unsigned flags, uint64_t *addr)
{
    if (!as || !vmo || !addr || (flags & ~MAP_FLAGS))
        return ERR_INVALID_ARGS;
    unsigned perms = flags & PERMS;
    bool jit = vmo_is_jit(vmo);   /* for good once true; never true for a kept VMO */
    if (!perms_ok(perms, jit) || len == 0 || ((vmo_off | len) & (PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    if ((flags & ASPACE_FIXED) && !range_ok(*addr, len))
        return ERR_INVALID_ARGS;
    if ((flags & ASPACE_REPLACE) && (!(flags & ASPACE_FIXED) || (flags & ASPACE_KEPT_ONLY)))
        return ERR_INVALID_ARGS;   /* a kept fill that failed couldn't put back what it replaced */
    if (len > USER_TOP - USER_BASE)
        return ERR_NO_RESOURCES;
    if (vmo_off > VMO_MAX_SIZE || len > VMO_MAX_SIZE)
        return ERR_OUT_OF_RANGE;   /* (so vmo_off + len can't overflow) */
    status_t st = kept_check(vmo, flags);
    if (st != OK)
        return st;

    struct mapping *m = mapping_new(vmo, vmo_off, len, flags);
    if (!m)
        return ERR_NO_MEMORY;
    uint64_t at = *addr;
    mutex_lock(&as->lock);
    if (flags & ASPACE_REPLACE)
        st = map_replace_locked(as, m, &at);
    else
        st = map_place_locked(as, m, flags, &at);
    mutex_unlock(&as->lock);
    if (st != OK) {
        kfree(m);   /* never placed (or taken back): still ours */
        return st;
    }
    if (jit)
        __atomic_store_n(&as->jit, true, __ATOMIC_RELAXED);
    *addr = at;   /* m itself may have merged into a neighbour by now */
    return OK;
}

status_t aspace_unmap(struct aspace *as, uint64_t addr, uint64_t len)
{
    if (!range_ok(addr, len))
        return ERR_INVALID_ARGS;
    uint64_t end = addr + len;
    mutex_lock(&as->lock);
    struct mapping *first = first_in(as, addr, end), *spare = NULL;
    status_t st = first ? OK : ERR_NOT_FOUND;
    if (st == OK && first->base < addr && mend(first) > end)   /* a hole in the middle */
        st = maps_reserve(as, as->nmaps + 1, as->nmaps + 1, 1, &spare);
    if (st == OK) {
        cut_locked(as, first, addr, end, spare);
        (void)maps_account(as, as->nmaps);   /* only ever down here: can't fail */
    }
    mutex_unlock(&as->lock);
    return st;
}

/* Region lock held: the mappings covering [addr, end), *first to *last,
 * for aspace_protect. ERR_INVALID_ARGS if flags are W+X and a mapping met
 * isn't of a JIT VMO (W^X: the file header), else ERR_NOT_FOUND if any of
 * the range isn't mapped, else ERR_ACCESS_DENIED if a mapping may not have
 * flags (past its maximum, or a kept one changed: its entries stay as they
 * were filled). */
static status_t protect_span(const struct aspace *as, uint64_t addr, uint64_t end,
                             unsigned flags, struct mapping **first, struct mapping **last)
{
    struct mapping *f = find(as, addr), *l = NULL;
    bool wx = perms_wx(flags);
    if (!f)
        return wx ? ERR_INVALID_ARGS : ERR_NOT_FOUND;   /* W+X needs a JIT VMO there */
    bool denied = false;
    for (struct mapping *m = f;; m = next_of(m)) {
        if (!m || (l && m->base != mend(l)))
            return ERR_NOT_FOUND;   /* ran out, or a gap */
        if (wx && !vmo_is_jit(m->vmo))
            return ERR_INVALID_ARGS;
        denied |= (flags & ~m->max) != 0;
        denied |= m->kept && flags != m->flags;
        l = m;
        if (mend(m) >= end)
            break;
    }
    if (denied)
        return ERR_ACCESS_DENIED;
    *first = f;
    *last = l;
    return OK;
}

/* How many merges protecting first..last (all of [addr, end), see
 * protect_span) to flags makes (merge_range does them): between each
 * neighbouring pair inside, and with the mapping on either side unless
 * that end is split (lsplit, rsplit), whose piece outside keeps its other
 * permissions and so merges with nothing new. */
static uint32_t protect_merges(const struct mapping *first, const struct mapping *last,
                               unsigned flags, bool lsplit, bool rsplit)
{
    uint32_t n = 0;
    const struct mapping *p = prev_of(first), *q = next_of(last);
    if (!lsplit && p)
        n += joinable(p, p->flags, first, flags);
    for (const struct mapping *m = first; m != last; m = next_of(m))
        n += joinable(m, flags, next_of(m), flags);
    if (!rsplit && q)
        n += joinable(last, flags, q, q->flags);
    return n;
}

/* Region lock held: flags for every mapping from first up to end (each
 * inside the range, or one that has flags already), and their entries
 * rewritten (or cleared, for no access: x86 can't say "present, no
 * access"), shot down before this returns if there were any (quiet: the
 * file header). */
static void protect_entries(struct aspace *as, struct mapping *first, uint64_t end,
                            unsigned flags)
{
    struct tlb_gather g;
    tlb_gather_init(&g);
    for (struct mapping *m = first; m && m->base < end; m = next_of(m)) {
        if (m->flags == flags)
            continue;   /* its entries are what they would be rewritten to */
        m->flags = flags;
        const struct leaf_walk w = { (flags & ASPACE_READ) ? leaves_protect : leaves_clear, flags,
                                     !(flags & ASPACE_READ), true, &g };
        walk_leaves(as, m->base, mend(m), &w);
    }
    tlb_gather_finish(&g);
}

status_t aspace_protect(struct aspace *as, uint64_t addr, uint64_t len, unsigned flags)
{
    /* W+X passes here; protect_span allows it only over JIT VMOs. */
    if ((flags & ~PERMS) || !perms_ok(flags, true) || !range_ok(addr, len))
        return ERR_INVALID_ARGS;
    uint64_t end = addr + len;
    mutex_lock(&as->lock);
    struct mapping *first = NULL, *last = NULL, *spare[2] = { NULL, NULL };
    status_t st = protect_span(as, addr, end, flags, &first, &last);
    /* An end mapping that has flags already isn't split: its two pieces
     * would only merge again. */
    bool lsplit = st == OK && first->base < addr && first->flags != flags;
    bool rsplit = st == OK && mend(last) > end && last->flags != flags;
    if (st == OK) {
        uint32_t peak = as->nmaps + lsplit + rsplit;
        st = maps_reserve(as, peak - protect_merges(first, last, flags, lsplit, rsplit), peak,
                          lsplit + rsplit, spare);
    }
    if (st != OK) {
        mutex_unlock(&as->lock);
        return st;
    }
    unsigned used = 0;
    if (lsplit) {
        bool same = first == last;
        first = split(as, first, addr, spare[used++]);
        if (same)
            last = first;
    }
    if (rsplit)
        split(as, last, end, spare[used++]);
    protect_entries(as, first, end, flags);
    struct mapping *before = prev_of(first);
    merge_range(as, before ? before : first, end);
    (void)maps_account(as, as->nmaps);   /* at most the reservation: can't fail */
    mutex_unlock(&as->lock);
    return OK;
}

/* A fault waits for a pager this many times at most before it gives up
 * (ERR_NO_MEMORY): its page was taken away again between each supply and
 * the retry's look (vmo.c's FETCH_ROUNDS, the same rule for vmo_read). */
#define FAULT_ROUNDS 8

/* Faults that waited for a pager with a mutex held (the caller's: the
 * region lock is dropped by then). PK2's audit found no copy that does;
 * one appearing later is reported here, since it could make another
 * process wait for this pager too. */
uint64_t aspace_pager_waits_held;

static void wait_held_check(void)
{
    struct thread *t = current_thread();
    if (!t->sleep_depth)
        return;
    __atomic_add_fetch(&aspace_pager_waits_held, 1, __ATOMIC_RELAXED);
    kprintf("aspace: thread \"%s\" waits for a pager holding %u mutex(es)\n", t->name,
            t->sleep_depth);
    preempt_disable();
    lockdep_print_held();
    preempt_enable();
}

/* The region lock is held and page idx of v (a pager VMO, through a
 * mapping) was absent: ask the pager for it, drop the region lock, wait,
 * and say how it went. OK: look again (from find: the mapping may have
 * changed meanwhile). The region lock is not held on return. */
static status_t fault_wait(struct aspace *as, struct vmo *v, uint64_t idx)
{
    /* Our own reference: an unmap while we wait may drop the mapping's,
     * and the VMO's death fails its requests; a waiter must not be the
     * one that lets it die (it outlives our wait instead). */
    kobject_ref(vmo_kobject(v));
    struct pager_req *r = NULL;
    status_t st = vmo_page_request(v, idx, &r);   /* spinlocks only: fine here */
    mutex_unlock(&as->lock);
    if (st == OK && r) {
        wait_held_check();
        st = pager_req_wait(r);   /* cancellable, never interruptible: a load can't restart */
        pager_req_put(r);
        if (st == ERR_OUT_OF_RANGE)
            st = ERR_IO;   /* PAGER_OP_FAIL's "past the file's end": it couldn't read it */
        /* A request is answered whole: one failed for its other pages may
         * still have brought ours. */
        if (st != OK && st != ERR_CANCELED && vmo_page_ready(v, idx))
            st = OK;
    }
    kobject_unref(vmo_kobject(v));
    return st;
}

status_t aspace_fault(struct aspace *as, uint64_t addr, unsigned access)
{
    if (addr < USER_BASE || addr >= USER_TOP)
        return ERR_NOT_FOUND;
    unsigned need = access & PERMS;
    if (!need)
        need = ASPACE_READ;
    __atomic_add_fetch(&as->faults, 1, __ATOMIC_RELAXED);
    for (unsigned rounds = 0;;) {
        mutex_lock(&as->lock);
        struct mapping *m = find(as, addr);
        status_t st;
        uint64_t idx = 0;
        if (!m) {
            st = ERR_NOT_FOUND;
        } else if (need & ~m->flags) {
            st = ERR_ACCESS_DENIED;
        } else {
            uint64_t va = ALIGN_DOWN(addr, PAGE_SIZE);
            idx = (m->vmo_off + (va - m->base)) >> PAGE_SHIFT;
            st = pt_prepare(as, va);
            if (st == OK)
                st = vmo_fault_map(m->vmo, idx, as, va, m->flags, (need & ASPACE_WRITE) != 0);
        }
        if (st != ERR_SHOULD_WAIT) {
            mutex_unlock(&as->lock);
            return st;
        }
        /* A pager VMO's absent page: wait for it without the region lock
         * (it guards every other fault, map and unmap of the process),
         * then start again from find. */
        if (++rounds > FAULT_ROUNDS) {
            mutex_unlock(&as->lock);
            return ERR_NO_MEMORY;
        }
        st = fault_wait(as, m->vmo, idx);
        if (st != OK)
            return st;
    }
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

bool aspace_maps_jit(struct aspace *as)
{
    return __atomic_load_n(&as->jit, __ATOMIC_RELAXED);
}

uint32_t aspace_mapping_count(struct aspace *as)
{
    mutex_lock(&as->lock);
    uint32_t n = as->nmaps;
    mutex_unlock(&as->lock);
    return n;
}

static bool map_before(const struct avl_node *a, const struct avl_node *b)
{
    return map_of(a)->base < map_of(b)->base;
}

static bool vmap_before(const struct avl_node *a, const struct avl_node *b)
{
    const struct mapping *x = vmap_of(a), *y = vmap_of(b);
    return !vpos_before((uintptr_t)y->vmo, vfirst(y), (uintptr_t)x->vmo, vfirst(x));
}

/* aspace_check's look at m in vmaps, region lock held: its summary, and no
 * page of it under more than VMAR_MAX_OVERLAP mappings. */
static const char *vmap_check(const struct aspace *as, const struct mapping *m)
{
    uintptr_t v;
    uint64_t end;
    vmax_of(m, &v, &end);
    if (v != m->vmax_vmo || end != m->vmax_end)
        return "a summary in the tree by VMO page";
    if (!overlap_room(as, m->vmo, vfirst(m), vend(m), 0, 0, VMAR_MAX_OVERLAP + 1))
        return "a page under more than VMAR_MAX_OVERLAP mappings";
    return NULL;
}

/* aspace_check's look at m's entries, region lock held: each present one
 * names the VMO's page for its address now and allows nothing m's flags
 * don't. buf holds a page table's worth of entries, copied under pt_lock
 * and checked after it (the VMO lock ranks above it). NULL if all is well,
 * else what is wrong. */
static const char *entries_check(struct aspace *as, const struct mapping *m, uint64_t *buf)
{
    for (uint64_t va = m->base; va < mend(m);) {
        uint64_t f = spin_lock_irqsave(&as->pt_lock);
        const uint64_t *pt = NULL;
        uint64_t next, n = 0, e4 = as->pml4v[ix(va, 4)];
        if (!(e4 & PTE_P)) {
            next = ALIGN_UP(va + 1, SIZE_512G);
        } else {
            uint64_t e3 = tbl(e4)[ix(va, 3)];
            if (!(e3 & PTE_P)) {
                next = ALIGN_UP(va + 1, SIZE_1G);
            } else {
                uint64_t e2 = tbl(e3)[ix(va, 2)];
                next = ALIGN_UP(va + 1, SIZE_2M);
                pt = (e2 & PTE_P) ? tbl(e2) : NULL;
            }
        }
        for (uint64_t a = va; pt && a < next && a < mend(m); a += PAGE_SIZE)
            buf[n++] = pt[ix(a, 1)];
        spin_unlock_irqrestore(&as->pt_lock, f);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t e = buf[i], a = va + i * PAGE_SIZE;
            if (!(e & PTE_P))
                continue;
            if (!(m->flags & ASPACE_READ))
                return "an entry under no access";
            if ((e & PTE_W) && !(m->flags & ASPACE_WRITE))
                return "a writable entry in a mapping that may not write";
            if (cpu_features.nx && !(e & PTE_NX) && !(m->flags & ASPACE_EXEC))
                return "an executable entry in a mapping that may not execute";
            if ((e & PTE_ADDR) != vmo_page_phys(m->vmo, m->vmo_off + (a - m->base)))
                return "an entry for a page the VMO doesn't hold there";
        }
        va = next;
    }
    return NULL;
}

int64_t aspace_check(struct aspace *as)
{
    uint64_t *buf = kmalloc(PAGE_SIZE);
    if (!buf)
        return -1;
    const char *why = NULL;
    mutex_lock(&as->lock);
    int64_t n = avl_check(&as->maps, map_before);
    if (n != as->nmaps)
        why = "the tree";
    else if (avl_check(&as->vmaps, vmap_before) != n)
        why = "the tree by VMO page";
    const struct mapping *prev = NULL;
    for (struct mapping *m = first_map(as); m && !why; prev = m, m = next_of(m)) {
        uint64_t below = prev ? mend(prev) : USER_BASE;
        uint64_t l = max_gap_of(m->node.left), r = max_gap_of(m->node.right);
        uint64_t most = m->gap > l ? m->gap : l;
        most = most > r ? most : r;
        if (!m->len || ((m->base | m->len | m->vmo_off) & (PAGE_SIZE - 1)) || m->base < below ||
            mend(m) > USER_TOP)
            why = "a mapping out of order, overlapping or unaligned";
        else if (m->gap != m->base - below || m->max_gap != most)
            why = "a gap";
        else if (prev && can_merge(prev, m))
            why = "two neighbours that could be one";
        else if (m->umap.as != as || m->umap.base != m->base ||
                 m->umap.pages.first != m->vmo_off >> PAGE_SHIFT ||
                 m->umap.pages.end != (m->vmo_off + m->len) >> PAGE_SHIFT ||
                 m->umap.writable != ((m->max & ASPACE_WRITE) != 0))
            why = "a reverse-map entry";
        else if ((m->flags & ~m->max) || !perms_ok(m->flags, vmo_is_jit(m->vmo)) ||
                 (m->kept && m->max != m->flags))
            why = "a mapping's permissions";
        else if (!(why = vmap_check(as, m)))
            why = entries_check(as, m, buf);
    }
    if (!why && as->map_pages != map_pages_for(as->nmaps))
        why = "the charge for the mapping structs";
    mutex_unlock(&as->lock);
    kfree(buf);
    if (why) {
        kprintf("aspace_check: %s\n", why);
        return -1;
    }
    return n;
}
