/* Virtual memory objects (see vmo.h).
 *
 * Page tracking (paged VMOs): a sparse table indexed by page number,
 * root[64] (inline) -> mid[512] -> leaf[512] of physical addresses, 0
 * meaning "not committed". A leaf covers 2 MiB, a mid table 1 GiB, the
 * inline root the first 64 GiB: all of every VMO that size or smaller, so
 * the thousands of small VMOs pay nothing for the rest. A plain paged VMO
 * may be up to 128 TiB (a reservation); its pages from 64 GiB on are found
 * through two more levels, top[512] -> upper[512] -> mid -> leaf, an upper
 * table covering 512 GiB (only the top table's first 256 entries are used,
 * and upper table 0's first 64, below 64 GiB, never are). Every table page
 * comes straight from the buddy allocator on the first commit that needs
 * it; shrink and destroy free them (decommit leaves them). A walk over a
 * range (decommit, shrink, a child's creation) skips a missing table at
 * any level, and scans past the run of missing entries after it in the
 * same lock hold, so a whole untouched TiB costs a few looks, not one lock
 * hold per GiB.
 *
 * Locking: the kobject's lock (class "vmo", taken with interrupts off like
 * every object lock) guards the size, the table, the committed count, the
 * range list and the reverse map. The buddy allocator ranks after it:
 * table pages are allocated with it held. Address spaces rank around it
 * (aspace_vmo.h): an address space's region lock (a mutex) is above it,
 * its page-table lock below it. The kernel page table lock is never taken
 * under it, and data is never copied under it. vmo_set_size is serialised
 * by a mutex ("vmo resize") above everything else but a region lock; a
 * copy-on-write child's creation holds it throughout. A fault (region
 * lock held), a writer or a resize (resize held) may wait for a creation
 * or a clone to lower `protecting` (cow_wait; a kill ends that wait, so
 * a process is never held up by another's freeze); neither takes a region
 * lock other than the clone's own source's, nor "vmo resize" while it
 * keeps anyone waiting but the creation's own. A freeze itself waits only
 * for the vmo_write copies under way (freeze_drain_locked), which hold no
 * lock. Two VMO locks nest only in
 * those: the parent's, then the new child's (spin_lock_nested), which
 * nobody else can reach yet.
 *
 * User mappings: each address-space mapping of a VMO is a struct
 * vmo_umap in v->umaps and holds a VMO reference. Unlike kernel mappings
 * and pins, they don't block decommit or shrink: those take the pages out
 * of the table, clear their page-table entries in every user mapping (under
 * this lock, so a racing fault either installed the old page first and
 * sees it zapped, or finds the slot empty and commits a new one), and free
 * the pages only after the TLB shootdown (a tlb_gather). mm/aspace.c's
 * header has the full argument. v->umaps is an interval tree by the VMO
 * pages each mapping covers (<jam/itree.h>), so a batch finds the mappings
 * over its pages, and only those, in O(log n) each: a reservation split
 * into tens of thousands of mappings (V8's cage, PartitionAlloc's pools)
 * costs a one-page decommit, a separation or a child's creation no more
 * than one mapping does. Adding, moving and removing an entry is O(log n)
 * too, under this lock.
 *
 * Latency: decommit and shrink work one leaf table (2 MiB, at most 512
 * pages, or one run of missing tables: above) per lock hold, less when the
 * user mappings over those pages are many: a batch's walk over them, which
 * clears each mapping's entries with this lock held, is cut short to stay
 * within VMO_BATCH_BUDGET (<jam/aspace_vmo.h>), down to one page (a page
 * under more mappings than that is a batch of its own: VMAR_MAX_OVERLAP
 * bounds how many each address space has there). A child's creation
 * write-protects the parent's mappings in batches the same way. They
 * flush/free every 512 pages or so, re-checking under each re-lock: the
 * range is clipped to the current size
 * (a racing shrink already took the rest), and a pin or kernel mapping that
 * appeared since the call started fails it with ERR_BAD_STATE, leaving the
 * pages before it decommitted. A commit racing the decommit of a slot it
 * already passed simply stays: it is ordered after the decommit. Shrink
 * lowers the size first, so nothing new can appear past the new end while
 * it works, and holds "vmo resize" so no grow can reopen that range.
 *
 * Page lifetime: a committed page's struct page refcount is 1 for each
 * table holding it (one, unless it is shared copy-on-write, below).
 * vmo_read/vmo_write take an extra reference under the lock and copy with
 * it dropped, so a racing decommit or shrink can't free a page mid-copy:
 * whoever drops the last reference frees it. Contiguous and physical VMOs
 * never give pages up before destroy, so they skip this.
 *
 * Job charges: a VMO made for a process (vmo_set_job) charges one
 * JOB_LIMIT_PAGES unit to that job for every page it owns, from the moment
 * the page is published in the table (under the lock, so a failed charge
 * just drops the fresh page: ERR_NO_MEMORY) until it leaves the table;
 * `committed` and the charge move together. Its own table pages (every
 * level, `tables`) are charged the same way, one unit each from creation to
 * free (uncharged, a job allowed 0 pages could make the kernel allocate
 * 128 MiB of leaf tables for one 64 GiB VMO). The page is
 * charged BEFORE any table is made for it, so a refused commit builds
 * nothing; if a table it needs is refused, the page's charge is undone.
 * Tables stay (charged) until shrink or destroy frees them (decommit
 * leaves them). Contiguous VMOs are charged whole when the job is
 * set. Kernel VMOs have no job. Physical VMOs own no memory: only their
 * struct (one handle unit) is ever charged.
 *
 * Commit: a zeroed page is allocated with the lock dropped, then published
 * under the lock if its slot is still empty (the loser of a race frees its
 * page). The zeroing happens before the publishing unlock and every lookup
 * takes the lock, so nobody sees a page's previous contents.
 *
 * Ranges: each kernel mapping and pin is a struct vmo_range on v->ranges
 * and holds a VMO reference. It is recorded (with the bounds checked)
 * before its pages are committed and looked up, so no decommit or shrink
 * can slip in between; decommit and shrink refuse to touch a page any
 * range covers. A range is `busy` while it is being set up or torn down,
 * and unmap/unpin ignore busy ranges.
 *
 * Pins and the IOMMU: with a dma_cap that has an IOMMU domain, a pin's
 * pages are mapped there (read only for a VMO_PIN_READ_ONLY pin: the
 * device's writes fault) before the pin is published and unmapped (the
 * unit's invalidation waited for) before they may go; a pin whose unmap
 * the unit didn't confirm keeps its pages for good ("pins through the
 * IOMMU" below). No VMO lock is held across either: the IOMMU's locks are
 * mutexes, and the range, busy, keeps the pages in place meanwhile.
 *
 * Sealing (vmo_seal, for vmo_make_exec): a sealed VMO's bytes and size
 * never change again. Every call that could change them (vmo_write,
 * vmo_commit, vmo_decommit, vmo_set_size) enters as a writer under the
 * lock: refused once sealed, and counted in `writers` until it returns, so
 * vmo_seal refuses while one is still copying. A writable user mapping,
 * a writable kernel mapping and a pin a device may write through (any but
 * a read-only pin the IOMMU enforces) are refused under the lock when
 * they are recorded; vmo_seal refuses while any
 * mapping or pin exists. Both sides test under the one lock, so a call
 * that took its reference before the seal either finishes before it (and
 * the seal fails) or is refused after it: none lands afterwards.
 *
 * JIT VMOs (vmo_make_jit, docs/CR-PLAN.md section 12): a plain paged VMO
 * that may be mapped writable and executable at once (aspace.c's W^X
 * rule). `jit` is set once, under the lock and the "vmo resize" mutex,
 * while the VMO has no page, no mapping, pin or kernel mapping and no
 * writer, and is never cleared; so every mapping of a JIT VMO was made
 * after it became one, and nothing it held before (no page: nothing) is
 * ever executed through it. The other kinds can never become one, and a
 * JIT VMO can never be sealed (vmo_seal, vmo_seal_verified: no approved
 * code is ever writable) or give a copy-on-write child (vmo_create_child,
 * checked under "vmo resize", so a creation and the marking never
 * overlap: a child holds nothing a JIT VMO ever held), and is never
 * pinned (range_add, under the lock: a device never writes code, nor
 * keeps a page a decommit of code should take).
 *
 * Kept VMOs (VMO_KEEP_PAGES, a paged VMO): every page below the size is
 * committed from vmo_create_for to destroy. Creation commits them all
 * (charged to the job one by one, so a refused page stops it); decommit and
 * a shrinking vmo_set_size are refused up front (ERR_BAD_STATE), and
 * nothing else takes a page out of the table but destroy. A growing
 * vmo_set_size (under "vmo resize") commits the new pages while they still
 * lie past the size, where no lookup, mapping, fault or pin can reach them
 * (every one checks the size under the lock), and only then raises the
 * size; if one can't be had, the ones made so far go through shrink_pages
 * (nobody maps them: no entries to zap) and the size never moved. So a
 * reader of any page below the size always finds it committed, and an
 * address space that filled its entries for one (ASPACE_KEPT_ONLY) never
 * has them zapped. A kept VMO may also be sealed: the two don't meet.
 *
 * Pager VMOs (vmo_create_pager, <jam/pager.h>): a paged VMO whose absent
 * pages come from its pager, never zero-filled. Only the pager puts a page
 * in (vmo_pager_publish, under this lock, if the slot is still absent and
 * the VMO isn't detached) and only the pager's side takes pages out: its
 * cache's eviction (vmo_pager_evict, one page), PAGER_OP_INVALIDATE (a
 * decommit), PAGER_OP_SET_SIZE (a shrink) and a detach (a decommit of
 * everything), each through the decommit and shrink code below, which
 * tells the cache (pager_page_gone_vlocked) for every page it takes.
 * Every other call that would commit, write, decommit, resize, seal, pin
 * or kernel-map one is refused at its entry, and so is a writable user
 * mapping unless the VMO is writable (below). A reader of an absent page
 * (vmo_read here; a fault through vmo_page_request) joins or queues a
 * request under this lock and then the pager's ("vmo" -> "pager"), and
 * waits for it with no lock held. `pv` is fixed at creation, so the
 * refusals need no lock.
 *
 * Writable pager VMOs (`pwritable`, <jam/pager.h>): user mappings may
 * write them. A page's state (clean, dirty, writing) is in its cache
 * record and changes under this lock. vmo_fault_map installs a page
 * writable only while it is dirty, so the first store to a clean or
 * writing page faults, and the fault makes it dirty (store_locked) before
 * it installs the entry writable: no D bit is ever read, and every other
 * way to an entry (leaves_protect, a clone's twin, which gets none) only
 * keeps or takes the write bit. Dirty and writing pages are never taken
 * by an eviction or an invalidation (take_batch_locked's `clean_only`),
 * only by a shrink, a detach, or a write-back that makes them clean. `dirty`
 * counts them; while it isn't 0 the VMO holds a reference to itself
 * (`held`, the hold: taken by the first store, given back by hold_end once
 * they are gone), so it outlives its handles and mappings and no store is
 * lost because a program unmapped. A write-back's begin
 * (vmo_pager_dirty_run with PAGER_DIRTY_BEGIN) makes a run of dirty pages
 * writing and takes their write bit in every mapping under this lock (a
 * batch within VMO_BATCH_BUDGET, as a child's creation protects), then
 * shoots down with no lock held before anyone copies them: the same rule
 * then keeps them read-only, and a store after the shootdown faults and
 * makes its page dirty again.
 *
 * Verified pager VMOs (vmo_seal_verified, <jam/vmo_verify.h>): a pager
 * VMO sealed with a table of page hashes, for a program from /data. The
 * seal drops every page present (none, normally: nothing maps the VMO, so
 * no entry needs zapping) and sets `verify` in one hold of the lock, and
 * vmo_pager_publish, under that lock, publishes only a page its supplier
 * matched against `verify` or that matches now: so every page present
 * after the seal matched, whatever supply was under way across it.
 *
 * Copy-on-write children (vmo_create_child; the design is docs/M13-PLAN.md's
 * "F1a design"). Flat, never a chain: a child's table points at the same
 * physical pages as its parent's, and each table is complete, so nothing
 * links a child to its parent and any destruction order just drops
 * references. A page held by n tables has refcount n (plus readers' brief
 * references) and `private` n - 1, its share count (an anonymous VMO page
 * doesn't otherwise use the word; pager pages never are shared: a child of
 * a pager VMO copies them). The rules:
 *   - A page is shared when its share count isn't 0, read under the lock of
 *     the table looking. A shared page is never written in place and never
 *     mapped writable; a table that wants to write it copies it into a page
 *     of its own (separates it: cow_replace_locked) and drops its reference
 *     on the old one. A page with a share count of 0 is that table's alone
 *     and is written in place, so the survivor of a relative's death pays
 *     nothing.
 *   - The count only rises under the lock of a table that holds the page
 *     (creation shares from the parent's table, under the parent's lock), so
 *     a table that saw 0 under its own lock knows nobody can share the page
 *     while it holds that lock. A table that lets go of a shared page lowers
 *     the count only after its TLB shootdown (page_share_drop, from the
 *     gather), so a relative never sees 0 while stale entries of the old
 *     holder may still read the page. The decrement stops at 0: the last
 *     tables to drop a page may find 0 already, which is what it ends at.
 *   - A gather lists pages through their `node`, which only one gather may
 *     use; a page that may be shared goes in the gather's array instead
 *     (tlb_gather_shared). take_batch_locked stops early when the array is
 *     full, and its callers flush and go on.
 *   - Creation (vmo_create_child) holds the parent's "vmo resize" mutex: no size
 *     change meanwhile. One leaf at a time under the parent's lock and the
 *     child's (nested; the child is new, nobody else can reach it), every
 *     present page is shared and the parent's writable user entries for the
 *     leaf lose their write bit; one shootdown at the end. Until it is done
 *     the parent's `protecting` is raised, and a write that would separate
 *     one of the parent's shared pages waits for the creation (cow_wait):
 *     otherwise a thread still writing through a stale writable entry could
 *     change the page after the copy was taken, and its write would be lost.
 *   - While `protecting` is raised the parent's bytes are frozen for
 *     everyone but writes through entries not yet shot down: every write
 *     fault, vmo_write (get_page's GET_PRIVATE, and so every pin and kernel
 *     mapping), decommit and size change waits for it to fall to 0
 *     (cow_wait, on one queue for all VMOs: protect_end wakes it), and a
 *     fault installs nothing writable meanwhile, so a later write faults and
 *     waits too. A vmo_write copies its bytes after it got its page and let
 *     the lock go (GET_WRITE); the copies under way are counted
 *     (`inflight`), and a freeze waits for them right after raising
 *     `protecting` (freeze_drain_locked), so none lands in a page shared
 *     afterwards: the child's bytes never change once its copy returned.
 *     So a change a clone (below) or creation doesn't see is one
 *     that waited, and everything ordered after it waited as well: a clone
 *     of many VMOs is never torn.
 *   - A separation under the lock: the copy (made with the lock dropped,
 *     from a page held by a reference) goes in if the slot still holds the
 *     page it copied, the old page's entries are cleared in every user
 *     mapping of this VMO, and the old reference waits for the shootdown.
 *     The copy is charged as the page it replaces: nothing more.
 *   - Pins and kernel mappings separate every shared page of their range
 *     first (commit_pages, GET_PRIVATE), and a child is refused a range one
 *     covers, so a device or the kernel always reaches the VMO's own page.
 *   - A kept VMO is never shared: its child gets copies at creation.
 *   - Accounting: each table pays for every page in it, shared or not, so
 *     a child is charged at creation for every page it shares and no
 *     separation ever charges anything.
 * A child of a pager VMO (or of such a child) has `src`, the pager VMO
 * (a reference, fixed at creation), and a window [0, win) of its pages
 * that come from src while absent: a touch of an absent page there copies
 * src's page (fetching it first like a pager VMO's reader) into a page of
 * the child's own; past src's end it is zero. The window is charged whole
 * at creation (its pages are never charged one by one: charged_alone) and
 * its tables are made then, so no touch of it charges anything.
 *
 * Clones (vmar_clone, mm/aspace.c's "Clones"): a copy of an address space
 * makes one child per VMO it copies (vmo_clone_begin, as large as the VMO
 * and with the same window if it is a pager child), shares into it only
 * the ranges the address space maps (vmo_clone_share: share_range, or
 * copy_pages for a kept VMO), adds the write-protects of every VMO to one
 * gather, and lowers each VMO's `protecting` (vmo_clone_end) only after
 * that gather's one shootdown. It takes no "vmo resize": a size change
 * waits for `protecting` instead, so the size read at vmo_clone_begin
 * holds. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/dbghook.h>
#include <jam/iommu.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/report.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/vmo.h>
#include <jam/vmo_verify.h>

#define VMO_MAX_PAGES (VMO_MAX_SIZE >> PAGE_SHIFT)
#define TBL_ENTRIES   512u
#define LEAF_PAGES    ((uint64_t)TBL_ENTRIES)            /* pages per leaf: 2 MiB */
#define MID_PAGES     (LEAF_PAGES * TBL_ENTRIES)         /* pages per mid table: 1 GiB */
#define UPPER_PAGES   (MID_PAGES * TBL_ENTRIES)          /* pages per upper table: 512 GiB */
#define ROOT_ENTRIES  64u                                /* the inline root's mid tables */
#define ROOT_PAGES    (MID_PAGES * ROOT_ENTRIES)         /* pages the inline root reaches: 64 GiB */
#define TOP_ENTRIES   (VMO_MAX_PAGES / UPPER_PAGES)      /* upper tables in use: 256 */
#define CONTIG_MAX    (PAGE_SIZE << MAX_ORDER)           /* 4 MiB */
_Static_assert(CONTIG_MAX == VMO_CONTIGUOUS_MAX, "<jam/abi.h> says what one buddy block holds");

_Static_assert(TBL_ENTRIES * sizeof(uint64_t) == PAGE_SIZE, "a table is one page");
_Static_assert(ROOT_PAGES == VMO_SMALL_MAX_SIZE >> PAGE_SHIFT,
               "every VMO but a plain paged one fits the inline root");
_Static_assert(VMO_MAX_PAGES % UPPER_PAGES == 0 && TOP_ENTRIES <= TBL_ENTRIES,
               "the top table reaches VMO_MAX_SIZE");

enum vmo_kind { VMO_PAGED, VMO_CONTIG, VMO_PHYS };
enum range_kind { RANGE_MAP, RANGE_PIN };

struct vmo_range {
    struct list_node node;        /* on the VMO's ranges list (VMO lock) */
    enum range_kind  kind;        /* a kernel mapping or a pin */
    bool             busy;        /* being set up or unmapped: not findable */
    bool             writes;      /* a pin or a writable mapping: refused on a sealed VMO */
    uint64_t         first, end;  /* page indices */
    uint64_t         key;         /* mapping: base va; pin: pin id */
    struct kobject  *cap;         /* pin: the DMA capability (referenced) */
    /* A pin is also on its cap's list (struct dma_cap.pins, under the
     * cap's lock), so closing the cap can find and release it. */
    struct vmo      *v;           /* pin: the VMO it pins */
    struct list_node cap_node;
    bool             cap_linked;  /* on the cap's list (cap's lock) */
    struct job      *charged;     /* pin: charged one JOB_LIMIT_HANDLES unit (the pinner's
                                   * job, its cap's; a reference), or NULL */
    /* A pin quarantined by its cap's unclean close (vmo_quarantine_cap_pins)
     * sits on the quarantine's list through cap_node, busy, with no cap.
     * sums: a checksum per page taken then, to see at release whether the
     * device wrote the pages meanwhile (NULL: not taken). */
    uint64_t        *sums;
};

struct vmo {
    struct kobject   base;                /* OBJ_VMO; base.lock is the VMO lock */
    enum vmo_kind    kind;                /* paged, contiguous or physical */
    uint32_t         flags;               /* VMO_CONTIGUOUS / VMO_DMA32 */
    unsigned         cache;               /* physical: VM_UC / VM_WC / 0 */
    uint64_t         size;                /* bytes, a page multiple */
    uint64_t         committed;           /* pages owned right now */
    uint64_t         tables;              /* paged: table pages (every level), charged like pages */
    struct job      *job;                 /* charged for them (a reference), or NULL */
    uint64_t         phys;                /* contiguous / physical: first byte */
    unsigned         order;               /* contiguous: buddy order it came from */
    uint64_t         next_pin_id;         /* the next pin's id */
    bool             sealed;              /* vmo_seal: bytes and size fixed for good; lock */
    bool             jit;                 /* vmo_make_jit: W+X mappings allowed; set once
                                             (lock and resize), read lock-free */
    uint32_t         writers;             /* write-path calls in progress (writer_enter); lock */
    struct list_node ranges;              /* struct vmo_range: kernel mappings and pins */
    struct itree     umaps;               /* struct vmo_umap by page range: user mappings
                                             (the reverse map) */
    struct mutex     resize;              /* serialises vmo_set_size */
    struct pager_vmo *pv;                 /* a pager VMO: its link (fixed at creation), else NULL */
    bool             pdetached;           /* a pager VMO cut off: nothing published any more; lock */
    bool             pwritable;           /* a writable pager VMO (fixed at creation) */
    bool             held;                /* the hold: a reference of its own while `dirty`; lock */
    uint64_t         dirty;               /* a writable pager VMO's dirty and writing pages; lock */
    status_t         pwb_error;           /* the error a write-back ended with, until taken; lock */
    /* A child of a pager VMO (or of such a child): the pager VMO its absent
     * pages [0, win) come from (a reference, fixed at creation), and its
     * page index of this VMO's page 0; else NULL. win is charged whole to
     * the job from creation on (lock; it only shrinks, with the size). */
    struct vmo      *src;
    uint64_t         src_first;
    uint64_t         win;
    /* Child creations sharing this VMO's pages whose shootdown of its
     * writable entries hasn't finished (lock): a separation waits. */
    uint32_t         protecting;
    /* vmo_write copies into pages they got (GET_WRITE) that haven't
     * finished (lock): a freeze waits for them (the file header's
     * "protecting"). */
    uint32_t         inflight;
    /* A verified pager VMO's table (<jam/vmo_verify.h>), owned: set once,
     * under the lock, by vmo_seal_verified (a release store; lockless
     * readers load it with acquire), never changed after. */
    struct vmo_verify *verify;
    uint64_t       **root[ROOT_ENTRIES];  /* paged, pages below ROOT_PAGES: root[r][m][l] = phys */
    /* Paged, pages from ROOT_PAGES on (only a plain paged VMO over 64 GiB
     * has them): top[t][u][m][l] = phys for page t * UPPER_PAGES + u *
     * MID_PAGES + m * LEAF_PAGES + l. The top table and each upper table
     * are made on the first commit that needs them; upper table 0's
     * entries below ROOT_PAGES stay empty. NULL: no page that far. */
    uint64_t    ****top;
};

static uint64_t vlock(struct vmo *v)
{
    return spin_lock_irqsave(&v->base.lock);
}

static void vunlock(struct vmo *v, uint64_t f)
{
    spin_unlock_irqrestore(&v->base.lock, f);
}

static bool in_bounds(uint64_t offset, uint64_t len, uint64_t size)
{
    return offset <= size && len <= size - offset;
}

/* The most v's size may grow to (<jam/abi.h>): VMO_MAX_SIZE for a plain
 * paged VMO; a kept one, a pager VMO and a pager VMO's child keep within
 * the inline root (VMO_SMALL_MAX_SIZE). All of it is fixed at creation. */
static uint64_t size_max(const struct vmo *v)
{
    bool plain = !v->pv && !v->src && !(v->flags & VMO_KEEP_PAGES);
    return plain ? VMO_MAX_SIZE : VMO_SMALL_MAX_SIZE;
}

/* Start a call that may change v's bytes or size: ERR_BAD_STATE once v is
 * sealed. Every OK is paired with writer_exit when the call is done. */
static status_t writer_enter(struct vmo *v)
{
    uint64_t f = vlock(v);
    status_t st = v->sealed ? ERR_BAD_STATE : OK;
    if (st == OK)
        v->writers++;
    vunlock(v, f);
    return st;
}

static void writer_exit(struct vmo *v)
{
    uint64_t f = vlock(v);
    v->writers--;
    vunlock(v, f);
}

/* ---- pages and tables ------------------------------------------------- */

static struct page *pa_page(uint64_t pa)
{
    return pfn_to_page(pa >> PAGE_SHIFT);
}

static void page_get(struct page *p)
{
    __atomic_add_fetch(&p->refcount, 1, __ATOMIC_RELAXED);
}

static void page_put(struct page *p)
{
    page_unref(p);
}

/* With v's lock held, p in v's table: do other tables hold it too? Never
 * for a pager VMO (its `private` is its cache's record). */
static bool page_shared(const struct vmo *v, struct page *p)
{
    return !v->pv && __atomic_load_n(&p->private, __ATOMIC_ACQUIRE) != 0;
}

/* One more table holds p (the lock of a table that holds it is held). */
static void page_share(struct page *p)
{
    page_get(p);
    __atomic_add_fetch(&p->private, 1, __ATOMIC_RELAXED);
}

void page_share_drop(struct page *p)
{
    /* Release: a relative that then sees the count at 0 writes the page in
     * place, after everything this table did with it. */
    uint64_t n = __atomic_load_n(&p->private, __ATOMIC_RELAXED);
    while (n && !__atomic_compare_exchange_n(&p->private, &n, n - 1, true, __ATOMIC_RELEASE,
                                             __ATOMIC_RELAXED))
        ;
    page_unref(p);
}

/* Pages copied because a write separated them from a relative, and
 * changes that waited for a creation or a clone (`protecting`; for tests
 * and the benchmark; relaxed). */
uint64_t vmo_cow_copies, vmo_cow_waits;

/* Everyone waiting for some VMO's `protecting` to fall to 0 (and the
 * freezes waiting for its `inflight` to): few and brief (a creation or a
 * clone; a copy of at most a page), so one queue, and each waiter looks
 * at its own VMO again when woken. */
static struct waitqueue protect_wq = {
    SPINLOCK_INIT("vmo protecting"), LIST_INIT(protect_wq.waiters)
};

/* A creation or a clone is sharing v's pages: wait for it to end (the
 * file header's "protecting"). No lock held. ERR_CANCELED if the thread
 * is killed (or its process replaced) meanwhile: someone else's freeze
 * never holds up a kill. */
static status_t cow_wait(struct vmo *v)
{
    __atomic_add_fetch(&vmo_cow_waits, 1, __ATOMIC_RELAXED);
    status_t st = OK;
    uint64_t f = vlock(v);
    while (v->protecting && st == OK)
        st = waitqueue_wait_cancellable(&protect_wq, &v->base.lock, &f, DEADLINE_NEVER);
    vunlock(v, f);
    return st;
}

/* With the lock held, `protecting` just raised: wait for the vmo_write
 * copies that got their page before (`inflight`) to finish, so none lands
 * in a page shared from here on. They copy from a kernel buffer and hold
 * nothing while they do: the wait is short and waits for nothing else. */
static void freeze_drain_locked(struct vmo *v, uint64_t *f)
{
    while (v->inflight)
        waitqueue_wait(&protect_wq, &v->base.lock, f);
}

/* A GET_WRITE page's copy is done: let a freeze that waits for it go on. */
static void write_done(struct vmo *v)
{
    uint64_t f = vlock(v);
    bool wake = --v->inflight == 0 && v->protecting;
    vunlock(v, f);
    if (wake)
        waitqueue_wake_all(&protect_wq);
}

/* One creation or clone sharing v's pages is done with them: lower
 * `protecting`, and wake the waiters when it reaches 0. */
static void protect_end(struct vmo *v)
{
    uint64_t f = vlock(v);
    bool last = --v->protecting == 0;
    vunlock(v, f);
    if (last)
        waitqueue_wake_all(&protect_wq);
}

/* With the lock held: does absent page idx come from v's pager VMO? */
static bool in_window_locked(const struct vmo *v, uint64_t idx)
{
    return v->src && idx < v->win;
}

/* With the lock held: a zeroed table page, charged to v's job. NULL if the
 * job refuses it or there is no memory. */
static void *table_alloc_locked(struct vmo *v)
{
    if (job_charge(v->job, JOB_LIMIT_PAGES, 1) != OK)
        return NULL;
    struct page *p = pmm_alloc_pages(0, PMM_ZERO);
    if (!p) {
        job_uncharge(v->job, JOB_LIMIT_PAGES, 1);
        return NULL;
    }
    v->tables++;
    return page_to_virt(p);
}

/* With the lock held (or no references left): free a table page and credit
 * v's job for it. */
static void table_free_locked(struct vmo *v, void *t)
{
    pmm_free_pages(virt_to_page(t), 0);
    v->tables--;
    job_uncharge(v->job, JOB_LIMIT_PAGES, 1);
}

/* With the lock held: where page idx's mid table pointer lives. Below
 * ROOT_PAGES (all of every VMO of 64 GiB or less) that is the inline root;
 * past it, an upper table under the top table, which are made here if
 * `create` (charged as every table page). NULL if they don't exist and
 * `create` is false, or making one failed (what was made stays, charged). */
static uint64_t ***mid_ref_locked(struct vmo *v, uint64_t idx, bool create)
{
    if (idx < ROOT_PAGES)
        return &v->root[idx / MID_PAGES];
    if (!v->top && (!create || !(v->top = table_alloc_locked(v))))
        return NULL;
    uint64_t ****upper = &v->top[idx / UPPER_PAGES];
    if (!*upper && (!create || !(*upper = table_alloc_locked(v))))
        return NULL;
    return &(*upper)[(idx / MID_PAGES) % TBL_ENTRIES];
}

/* With the lock held: the table slot for page idx. NULL if its tables don't
 * exist and `create` is false, or creating them failed (the job refused a
 * table page, or no memory; any table made before the failure stays, and
 * stays charged). */
static uint64_t *slot_locked(struct vmo *v, uint64_t idx, bool create)
{
    uint64_t ***mid = mid_ref_locked(v, idx, create);
    if (!mid || (!*mid && (!create || !(*mid = table_alloc_locked(v)))))
        return NULL;
    uint64_t **leaf = &(*mid)[(idx / LEAF_PAGES) % TBL_ENTRIES];
    if (!*leaf && (!create || !(*leaf = table_alloc_locked(v))))
        return NULL;
    return &(*leaf)[idx % LEAF_PAGES];
}

/* mid_find_locked past the inline root (idx >= ROOT_PAGES). */
static uint64_t **upper_mid_find_locked(const struct vmo *v, uint64_t idx, uint64_t *next)
{
    uint64_t t = idx / UPPER_PAGES, u = (idx / MID_PAGES) % TBL_ENTRIES;
    uint64_t ***upper = v->top ? v->top[t] : NULL;
    if (upper && upper[u])
        return upper[u];
    if (!v->top) {
        *next = VMO_MAX_PAGES;
    } else if (!upper) {
        while (++t < TOP_ENTRIES && !v->top[t])
            ;
        *next = t * UPPER_PAGES;
    } else {
        while (++u < TBL_ENTRIES && !upper[u])
            ;
        *next = t * UPPER_PAGES + u * MID_PAGES;
    }
    return NULL;
}

/* With the lock held: page idx's mid table, or NULL if it has none; then
 * *next is the first page after idx that may have one: past the missing
 * tables that follow it in the table above (the root, an upper table or
 * the top table), so a walk over an untouched range takes one look per
 * table that exists, not one per GiB. It may lie past the size: callers
 * clip it. */
static uint64_t **mid_find_locked(const struct vmo *v, uint64_t idx, uint64_t *next)
{
    if (idx >= ROOT_PAGES)
        return upper_mid_find_locked(v, idx, next);
    uint64_t r = idx / MID_PAGES;
    if (v->root[r])
        return v->root[r];
    while (++r < ROOT_ENTRIES && !v->root[r])
        ;
    *next = r * MID_PAGES;   /* ROOT_PAGES if none: on in the top table */
    return NULL;
}

/* With the lock held: page idx's leaf table, or NULL if it has none; then
 * *next is the first page after idx that may have one, past the missing
 * leaves that follow it in its mid table (or as mid_find_locked says). */
static uint64_t *leaf_find_locked(const struct vmo *v, uint64_t idx, uint64_t *next)
{
    uint64_t **mid = mid_find_locked(v, idx, next);
    if (!mid)
        return NULL;
    uint64_t m = (idx / LEAF_PAGES) % TBL_ENTRIES;
    if (mid[m])
        return mid[m];
    while (++m < TBL_ENTRIES && !mid[m])
        ;
    *next = ALIGN_DOWN(idx, MID_PAGES) + m * LEAF_PAGES;
    return NULL;
}

/* With the lock held: where page idx's leaf table pointer lives, or NULL
 * if the tables above it don't exist. */
static uint64_t **leaf_ref_locked(struct vmo *v, uint64_t idx)
{
    uint64_t ***mid = mid_ref_locked(v, idx, false);
    return mid && *mid ? &(*mid)[(idx / LEAF_PAGES) % TBL_ENTRIES] : NULL;
}

/* With the lock held: is page idx charged to the job on its own while
 * committed? Every page but those of a pager child's window, which were
 * paid for at creation. */
static bool charged_alone(const struct vmo *v, uint64_t idx)
{
    return idx >= v->win;
}

/* With the lock held and page idx not committed (*s is its slot if its
 * tables exist, else NULL): charge the page to v's job, then make its
 * tables if needed. Returns the slot to publish the page in, or NULL with
 * nothing charged for the page (ERR_NO_MEMORY). */
static uint64_t *charge_slot_locked(struct vmo *v, uint64_t idx, uint64_t *s)
{
    uint64_t n = charged_alone(v, idx) ? 1 : 0;
    if (job_charge(v->job, JOB_LIMIT_PAGES, n) != OK)
        return NULL;   /* refused: no table gets built for it */
    if (!s && !(s = slot_locked(v, idx, true))) {
        job_uncharge(v->job, JOB_LIMIT_PAGES, n);
        return NULL;
    }
    return s;
}

/* With the lock held: physical address of page idx, 0 if not committed. */
static uint64_t phys_locked(struct vmo *v, uint64_t idx)
{
    if (v->kind != VMO_PAGED)
        return v->phys + (idx << PAGE_SHIFT);
    uint64_t *s = slot_locked(v, idx, false);
    return s ? *s : 0;
}

/* With the lock held (or no reference left): page p leaves pager VMO v's
 * table, its cache's lists and, if dirty or writing, v's count. */
static void pager_page_out_locked(struct vmo *v, struct page *p)
{
    if (v->pwritable && pager_page_state_vlocked(v->pv, p))
        v->dirty--;
    pager_page_gone_vlocked(v->pv, p);
}

/* drop_from_locked's work on one leaf table, whose first entry is page
 * `base`: drop its pages at index >= first. */
static void drop_leaf_from_locked(struct vmo *v, uint64_t *leaf, uint64_t base, uint64_t first)
{
    for (uint64_t l = 0; l < LEAF_PAGES; l++) {
        if (leaf[l] && base + l >= first) {
            if (v->pv) {
                pager_page_out_locked(v, pa_page(leaf[l]));
                page_put(pa_page(leaf[l]));
            } else {
                page_share_drop(pa_page(leaf[l]));   /* no mapping: nothing to shoot down */
            }
            leaf[l] = 0;
            v->committed--;
            if (charged_alone(v, base + l))
                job_uncharge(v->job, JOB_LIMIT_PAGES, 1);
        }
    }
}

/* drop_from_locked's work on one mid table, *ref, whose first entry is page
 * `base`: drop its pages at index >= first, and free its tables that lie
 * wholly past first (itself too: *ref = NULL). */
static void drop_mid_from_locked(struct vmo *v, uint64_t ***ref, uint64_t base, uint64_t first)
{
    uint64_t **mid = *ref;
    if (!mid || base + MID_PAGES <= first)
        return;
    for (uint64_t m = 0; m < TBL_ENTRIES; m++) {
        uint64_t *leaf = mid[m];
        uint64_t lbase = base + m * LEAF_PAGES;
        if (!leaf || lbase + LEAF_PAGES <= first)
            continue;
        drop_leaf_from_locked(v, leaf, lbase, first);
        if (lbase >= first) {
            table_free_locked(v, leaf);
            mid[m] = NULL;
        }
    }
    if (base >= first) {
        table_free_locked(v, mid);
        *ref = NULL;
    }
}

/* drop_from_locked's work past the inline root: the upper tables and the
 * top table. Upper table t holds pages from t * UPPER_PAGES on, but never
 * below ROOT_PAGES; the top table, none below it either. */
static void drop_top_from_locked(struct vmo *v, uint64_t first)
{
    for (uint64_t t = 0; t < TOP_ENTRIES; t++) {
        uint64_t ***upper = v->top[t];
        uint64_t base = t * UPPER_PAGES;
        if (!upper || base + UPPER_PAGES <= first)
            continue;
        for (uint64_t u = 0; u < TBL_ENTRIES; u++)
            drop_mid_from_locked(v, &upper[u], base + u * MID_PAGES, first);
        if ((base > ROOT_PAGES ? base : ROOT_PAGES) >= first) {
            table_free_locked(v, upper);
            v->top[t] = NULL;
        }
    }
    if (ROOT_PAGES >= first) {
        table_free_locked(v, v->top);
        v->top = NULL;
    }
}

/* Drop every page at index >= first, and free the table pages that lie
 * wholly past it, with no TLB care: for vmo_destroy (no references, so no
 * mappings and no lock needed), the tail of a shrink (which has already
 * taken every page past `first` out through a gather, so only tables are
 * left to free) and vmo_seal_verified (the lock held and no mapping). */
static void drop_from_locked(struct vmo *v, uint64_t first)
{
    for (uint64_t r = 0; r < ROOT_ENTRIES; r++)
        drop_mid_from_locked(v, &v->root[r], r * MID_PAGES, first);
    if (v->top)
        drop_top_from_locked(v, first);
}

static uint64_t zap_umaps_locked(const struct vmo *v, uint64_t first, uint64_t end,
                                 struct tlb_gather *g);

/* What get_page makes sure of. */
enum get_mode {
    GET_PEEK,      /* nothing: an absent page is *out = NULL */
    GET_COMMIT,    /* committed (a shared page will do) */
    GET_PRIVATE,   /* committed and this table's alone: a shared page is separated */
    GET_WRITE,     /* as GET_PRIVATE, for bytes copied in once the lock is dropped:
                      counted in `inflight` until write_done */
};

/* A page made with no lock held for one slot, and what it holds: a copy of
 * `from` (a reference: the shared page it separates), the pager VMO's
 * bytes (`filled`: a pager child's window), or zeros. */
struct fresh {
    struct page *page;
    struct page *from;
    bool         filled;
};

static void fresh_drop(struct fresh *fr)
{
    if (fr->page)
        page_put(fr->page);
    if (fr->from)
        page_put(fr->from);
    *fr = (struct fresh){ 0 };
}

/* With the lock held (f its saved flags) and p shared in v's table: drop
 * the lock and copy p into fr (whatever fr held goes). OK: look again;
 * ERR_NO_MEMORY: no page for the copy (fr is empty). */
static status_t copy_unlocked(struct vmo *v, uint64_t f, struct page *p, struct fresh *fr)
{
    page_get(p);
    vunlock(v, f);
    fresh_drop(fr);
    fr->from = p;
    fr->page = pmm_alloc_pages(0, (v->flags & VMO_DMA32) ? PMM_DMA32 : 0);
    if (!fr->page) {
        fresh_drop(fr);
        return ERR_NO_MEMORY;
    }
    memcpy(page_to_virt(fr->page), page_to_virt(p), PAGE_SIZE);
    return OK;
}

/* With the lock held, slot s of page idx holding fr->from: separate it.
 * fr->page goes in (charged as the page it replaces) and is the table's
 * from now on, the old page's entries are cleared in every user mapping of
 * v, and the table's reference on the old page goes to g, released after
 * g's shootdown (the caller's, with the lock dropped); fr->from keeps its
 * own reference for fresh_drop. g is empty. */
static void cow_replace_locked(struct vmo *v, uint64_t idx, uint64_t *s, struct fresh *fr,
                               struct tlb_gather *g)
{
    *s = page_to_phys(fr->page);
    fr->page = NULL;
    zap_umaps_locked(v, idx, idx + 1, g);
    tlb_gather_shared(g, fr->from);
    __atomic_add_fetch(&vmo_cow_copies, 1, __ATOMIC_RELAXED);
}

/* Fill fr with v's pager VMO's page for idx (in v's window), with no lock
 * held: it waits as a pager VMO's reader does (vmo_read's errors). Past
 * the pager VMO's end: zeros. */
static status_t source_fill(struct vmo *v, uint64_t idx, struct fresh *fr)
{
    fresh_drop(fr);
    struct page *p = pmm_alloc_pages(0, 0);
    if (!p)
        return ERR_NO_MEMORY;
    status_t st = vmo_read(v->src, (v->src_first + idx) << PAGE_SHIFT, page_to_virt(p), PAGE_SIZE);
    if (st == ERR_OUT_OF_RANGE) {
        memset(page_to_virt(p), 0, PAGE_SIZE);
        st = OK;
    }
    if (st != OK) {
        page_put(p);
        return st;
    }
    fr->page = p;
    fr->filled = true;
    return OK;
}

/* Paged VMOs: page idx with an extra reference (page_put it), made sure
 * of as `mode` says. *out = NULL for an absent page with GET_PEEK (a pager
 * child's window page too: vmo_read reads its pager VMO's).
 * ERR_OUT_OF_RANGE if idx is past the end (a racing shrink); a pager VMO's
 * absent page is ERR_NOT_SUPPORTED (its pager's to commit); a pager
 * child's window page may wait for the pager (vmo_read's errors). */
static status_t get_page(struct vmo *v, uint64_t idx, enum get_mode mode, struct page **out)
{
    struct fresh fr = { 0 };
    bool priv = mode == GET_PRIVATE || mode == GET_WRITE;
    for (;;) {
        uint64_t f = vlock(v);
        if (idx >= v->size >> PAGE_SHIFT) {
            vunlock(v, f);
            fresh_drop(&fr);
            return ERR_OUT_OF_RANGE;
        }
        if (priv && v->protecting) {   /* for a write: frozen meanwhile */
            vunlock(v, f);
            fresh_drop(&fr);
            status_t st = cow_wait(v);
            if (st != OK)
                return st;
            continue;
        }
        /* Each OK below comes in this hold, with v not frozen: a
         * GET_WRITE counts itself in `inflight` before the lock goes. */
        uint64_t *s = slot_locked(v, idx, false);
        struct page *p = s && *s ? pa_page(*s) : NULL;
        if (p && (!priv || !page_shared(v, p))) {
            page_get(p);
            v->inflight += mode == GET_WRITE;
            vunlock(v, f);
            fresh_drop(&fr);   /* another CPU committed or separated it first */
            *out = p;
            return OK;
        }
        if (p && fr.page && fr.from == p) {
            struct tlb_gather g;
            tlb_gather_init(&g);
            struct page *mine = fr.page;
            cow_replace_locked(v, idx, s, &fr, &g);
            page_get(mine);   /* the table's reference plus the caller's */
            v->inflight += mode == GET_WRITE;
            vunlock(v, f);
            tlb_gather_finish(&g);
            fresh_drop(&fr);
            *out = mine;
            return OK;
        }
        if (p) {
            status_t st = copy_unlocked(v, f, p, &fr);
            if (st != OK)
                return st;
            continue;
        }
        if (mode == GET_PEEK || v->pv) {   /* a pager VMO's page is its pager's to commit */
            vunlock(v, f);
            fresh_drop(&fr);
            *out = NULL;
            return mode == GET_PEEK ? OK : ERR_NOT_SUPPORTED;
        }
        bool src = in_window_locked(v, idx);
        if (fr.page && !fr.from && fr.filled == src) {
            if (!(s = charge_slot_locked(v, idx, s))) {
                vunlock(v, f);
                fresh_drop(&fr);
                return ERR_NO_MEMORY;
            }
            *s = page_to_phys(fr.page);
            v->committed++;
            page_get(fr.page);   /* the table's reference plus the caller's */
            v->inflight += mode == GET_WRITE;
            vunlock(v, f);
            *out = fr.page;
            return OK;
        }
        vunlock(v, f);
        status_t st = OK;
        if (src) {
            st = source_fill(v, idx, &fr);
        } else {
            fresh_drop(&fr);
            fr.page = pmm_alloc_pages(0, PMM_ZERO | ((v->flags & VMO_DMA32) ? PMM_DMA32 : 0));
            st = fr.page ? OK : ERR_NO_MEMORY;
        }
        if (st != OK)
            return st;
    }
}

/* Make sure of pages [first, end) of a paged VMO, as `mode` says. */
static status_t commit_pages(struct vmo *v, uint64_t first, uint64_t end, enum get_mode mode)
{
    for (uint64_t idx = first; idx < end; idx++) {
        struct page *p;
        status_t st = get_page(v, idx, mode, &p);
        if (st != OK)
            return st;
        page_put(p);
    }
    return OK;
}

/* ---- contiguous blocks ------------------------------------------------- */

/* A contiguous VMO keeps the first `pages` pages of its 2^order buddy block
 * and gives the tail back at creation. Both parts are freed as the aligned
 * power-of-two pieces the buddy allocator can take back. */
static void contig_free_tail(uint64_t pfn, uint64_t pages, unsigned order)
{
    for (uint64_t pos = pages; pos < (1ull << order);) {
        unsigned k = (unsigned)__builtin_ctzll(pos);
        pmm_free_pages(pfn_to_page(pfn + pos), k);
        pos += 1ull << k;
    }
}

static void contig_free_head(uint64_t pfn, uint64_t pages)
{
    uint64_t pos = 0;
    for (int k = MAX_ORDER; k >= 0; k--) {
        if (pages & (1ull << k)) {
            pmm_free_pages(pfn_to_page(pfn + pos), (unsigned)k);
            pos += 1ull << k;
        }
    }
}

/* ---- creation and destruction ----------------------------------------- */

static void vmo_destroy(struct kobject *obj)
{
    struct vmo *v = (struct vmo *)obj;
    /* Every mapping and pin holds a reference, so none can be left. */
    if (!list_empty(&v->ranges) || !itree_empty(&v->umaps))
        panic("vmo: koid %lu destroyed while mapped or pinned", obj->koid);
    if (v->kind == VMO_PAGED) {
        ASSERT(!v->held && v->dirty == 0);   /* the hold is a reference: no dirty page here */
        drop_from_locked(v, 0);
        ASSERT(v->committed == 0 && v->tables == 0);
        if (v->pv)
            pager_vmo_gone(v->pv);   /* PAGER_REQ_GONE, or the link goes */
        if (v->verify)
            vmo_verify_free(v->verify);
        if (v->src) {
            job_uncharge(v->job, JOB_LIMIT_PAGES, v->win);   /* the window, paid at creation */
            kobject_unref(&v->src->base);
        }
    } else if (v->kind == VMO_CONTIG) {
        contig_free_head(v->phys >> PAGE_SHIFT, v->size >> PAGE_SHIFT);
        if (v->job)
            job_uncharge(v->job, JOB_LIMIT_PAGES, v->committed);
    }
    job_uncharge(v->job, JOB_LIMIT_HANDLES, 1);   /* the struct (see vmo_set_job) */
    job_unref(v->job);
    kfree(v);
}

static const struct kobject_ops vmo_ops = {
    .name = "vmo",
    .destroy = vmo_destroy,
};

static struct vmo *vmo_alloc(enum vmo_kind kind, uint64_t size)
{
    struct vmo *v = kzalloc(sizeof(*v));
    if (!v)
        return NULL;
    kobject_init(&v->base, OBJ_VMO, &vmo_ops, "vmo", 0);
    v->kind = kind;
    v->size = size;
    v->next_pin_id = 1;
    list_init(&v->ranges);
    itree_init(&v->umaps);
    mutex_init(&v->resize, "vmo resize");
    return v;
}

static status_t vmo_new(uint64_t size, uint32_t flags, struct vmo **out)
{
    if (flags & ~VMO_CREATE_FLAGS)
        return ERR_INVALID_ARGS;
    if ((flags & VMO_KEEP_PAGES) && (flags & (VMO_CONTIGUOUS | VMO_DMA32)))
        return ERR_INVALID_ARGS;   /* contiguous memory stays anyway; DMA32 pages are scarce */
    if (size > ((flags & VMO_KEEP_PAGES) ? VMO_SMALL_MAX_SIZE : VMO_MAX_SIZE))
        return ERR_OUT_OF_RANGE;
    size = ALIGN_UP(size, PAGE_SIZE);

    struct page *block = NULL;
    unsigned order = 0;
    if (flags & VMO_CONTIGUOUS) {
        if (size == 0)
            return ERR_INVALID_ARGS;
        if (size > CONTIG_MAX)
            return ERR_OUT_OF_RANGE;
        while ((PAGE_SIZE << order) < size)
            order++;
        block = pmm_alloc_pages(order, PMM_ZERO | ((flags & VMO_DMA32) ? PMM_DMA32 : 0));
        if (!block)
            return ERR_NO_MEMORY;
    }

    struct vmo *v = vmo_alloc(block ? VMO_CONTIG : VMO_PAGED, size);
    if (!v) {
        if (block)
            pmm_free_pages(block, order);
        return ERR_NO_MEMORY;
    }
    v->flags = flags;
    if (block) {
        v->phys = page_to_phys(block);
        v->order = order;
        v->committed = size >> PAGE_SHIFT;
        contig_free_tail(page_to_pfn(block), size >> PAGE_SHIFT, order);
    }
    *out = v;
    return OK;
}

status_t vmo_create_for(struct job *job, uint64_t size, uint32_t flags, struct vmo **out)
{
    struct vmo *v;
    status_t st = vmo_new(size, flags, &v);
    if (st != OK)
        return st;
    if (job)
        st = vmo_set_job(v, job);
    if (st == OK && (flags & VMO_KEEP_PAGES))
        st = commit_pages(v, 0, v->size >> PAGE_SHIFT, GET_COMMIT);
    if (st != OK) {
        kobject_unref(&v->base);   /* destroy gives back every page and its charge */
        return st;
    }
    *out = v;
    return OK;
}

status_t vmo_create(uint64_t size, uint32_t flags, struct vmo **out)
{
    return vmo_create_for(NULL, size, flags, out);
}

bool vmo_is_kept(const struct vmo *v)
{
    return (v->flags & VMO_KEEP_PAGES) != 0;
}

bool vmo_is_paged(const struct vmo *v)
{
    return v->kind == VMO_PAGED;
}

status_t vmo_create_physical(uint64_t phys, uint64_t size, unsigned cache, struct vmo **out)
{
    if ((phys & (PAGE_SIZE - 1)) || size == 0 ||
        (cache != 0 && cache != VM_UC && cache != VM_WC))
        return ERR_INVALID_ARGS;
    if (size > VMO_SMALL_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    size = ALIGN_UP(size, PAGE_SIZE);
    if (phys > (1ull << 52) || phys + size > (1ull << 52))   /* architectural limit */
        return ERR_OUT_OF_RANGE;

    struct vmo *v = vmo_alloc(VMO_PHYS, size);
    if (!v)
        return ERR_NO_MEMORY;
    v->phys = phys;
    v->cache = cache;
    *out = v;
    return OK;
}

/* ---- byte access -------------------------------------------------------- */

uint64_t vmo_size(struct vmo *v)
{
    return __atomic_load_n(&v->size, __ATOMIC_RELAXED);
}

status_t vmo_set_job(struct vmo *v, struct job *job)
{
    uint64_t f = vlock(v);
    status_t st = OK;
    if (v->job) {
        st = ERR_BAD_STATE;
    } else {
        /* The struct itself is one handle unit (a mapping or a message can
         * keep it alive after its last handle closes); its pages: all of a
         * contiguous one, what a paged one has so far, tables too. */
        st = job_charge(job, JOB_LIMIT_HANDLES, 1);
        if (st == OK && (st = job_charge(job, JOB_LIMIT_PAGES, v->committed + v->tables)) != OK)
            job_uncharge(job, JOB_LIMIT_HANDLES, 1);
        if (st == OK) {
            job_ref(job);
            v->job = job;
        }
    }
    vunlock(v, f);
    return st;
}

uint64_t vmo_committed(struct vmo *v)
{
    uint64_t f = vlock(v);
    uint64_t pages = v->committed;
    vunlock(v, f);
    return pages << PAGE_SHIFT;
}

/* A pager VMO's reader found page idx absent: wait for its pager to
 * supply it (OK: look again). ERR_OUT_OF_RANGE past the size,
 * ERR_CANCELED if the thread is killed, ERR_NO_MEMORY if its job can't pay
 * for the request; ERR_IO for every way the pager can fail it (a read
 * error, detached or gone, no answer in time). */
static status_t pager_fetch(struct vmo *v, uint64_t idx)
{
    struct pager_req *r;
    status_t st = vmo_page_request(v, idx, &r);
    if (st == ERR_OUT_OF_RANGE || st == ERR_NO_MEMORY)
        return st;   /* past the size (as for any VMO), or no request */
    if (st == OK && r) {
        st = pager_req_wait(r);
        pager_req_put(r);
    }
    /* A request is answered whole: one failed for its other pages may
     * still have brought this one. */
    if (st != OK && st != ERR_CANCELED && vmo_page_phys(v, idx << PAGE_SHIFT))
        st = OK;
    return st == OK || st == ERR_CANCELED ? st : ERR_IO;
}

/* An absent page of a pager child, n bytes at offset (within one page):
 * its pager VMO's bytes if the page is in the window (read through it,
 * waiting as its reader does; zeros past its end), else zeros. A page the
 * child commits meanwhile is read as it was before: a read racing a
 * write. */
static status_t source_read(struct vmo *v, uint64_t offset, void *dst, uint64_t n)
{
    uint64_t idx = offset >> PAGE_SHIFT;
    uint64_t f = vlock(v);
    bool src = in_window_locked(v, idx);
    vunlock(v, f);
    status_t st = src ? vmo_read(v->src, ((v->src_first + idx) << PAGE_SHIFT) |
                                             (offset & (PAGE_SIZE - 1)), dst, n)
                      : ERR_OUT_OF_RANGE;
    if (st == ERR_OUT_OF_RANGE) {
        memset(dst, 0, n);
        st = OK;
    }
    return st;
}

/* A pager VMO's page is fetched again at most this many times for one
 * read before the read gives up (ERR_NO_MEMORY): a page dropped again
 * between its supply and the read's next look, each time. */
#define FETCH_ROUNDS 8

status_t vmo_read(struct vmo *v, uint64_t offset, void *buf, uint64_t len)
{
    if (v->kind == VMO_PHYS)
        return ERR_NOT_SUPPORTED;
    if (!in_bounds(offset, len, vmo_size(v)))
        return ERR_OUT_OF_RANGE;
    if (v->kind == VMO_CONTIG) {
        memcpy(buf, (char *)phys_to_virt(v->phys) + offset, len);
        return OK;
    }
    char *dst = buf;
    unsigned rounds = 0;
    while (len) {
        uint64_t in = offset & (PAGE_SIZE - 1);
        uint64_t n = PAGE_SIZE - in < len ? PAGE_SIZE - in : len;
        struct page *p;
        status_t st = get_page(v, offset >> PAGE_SHIFT, GET_PEEK, &p);
        if (st == OK && !p && v->pv)
            st = ++rounds > FETCH_ROUNDS ? ERR_NO_MEMORY : pager_fetch(v, offset >> PAGE_SHIFT);
        if (st == OK && !p && v->src)
            st = source_read(v, offset, dst, n);
        if (st != OK)
            return st;
        if (!p && v->pv)
            continue;   /* fetched: look again */
        if (p) {
            memcpy(dst, (char *)page_to_virt(p) + in, n);
            page_put(p);
        } else if (!v->src) {
            memset(dst, 0, n);
        }
        rounds = 0;
        dst += n;
        offset += n;
        len -= n;
    }
    return OK;
}

/* vmo_write's copy, as a writer (writer_enter). */
static status_t write_bytes(struct vmo *v, uint64_t offset, const void *buf, uint64_t len)
{
    if (v->kind == VMO_CONTIG) {
        memcpy((char *)phys_to_virt(v->phys) + offset, buf, len);
        return OK;
    }
    const char *src = buf;
    while (len) {
        uint64_t in = offset & (PAGE_SIZE - 1);
        uint64_t n = PAGE_SIZE - in < len ? PAGE_SIZE - in : len;
        struct page *p;
        status_t st = get_page(v, offset >> PAGE_SHIFT, GET_WRITE, &p);
        if (st != OK)
            return st;
        DBG_HOOK(DBG_VMO_WRITE_COPY, v);
        memcpy((char *)page_to_virt(p) + in, src, n);
        page_put(p);
        write_done(v);
        src += n;
        offset += n;
        len -= n;
    }
    return OK;
}

status_t vmo_write(struct vmo *v, uint64_t offset, const void *buf, uint64_t len)
{
    if (v->kind == VMO_PHYS || v->pv)
        return ERR_NOT_SUPPORTED;   /* a pager VMO's bytes are its pager's */
    if (!in_bounds(offset, len, vmo_size(v)))
        return ERR_OUT_OF_RANGE;
    status_t st = writer_enter(v);
    if (st != OK)
        return st;
    st = write_bytes(v, offset, buf, len);
    writer_exit(v);
    return st;
}

/* ---- size, commit, decommit -------------------------------------------- */

/* With the lock held: does any mapping or pin cover a page in [first, end)? */
static bool ranges_overlap_locked(const struct vmo *v, uint64_t first, uint64_t end)
{
    for (struct list_node *n = v->ranges.next; n != &v->ranges; n = n->next) {
        struct vmo_range *r = container_of(n, struct vmo_range, node);
        if (r->first < end && first < r->end)
            return true;
    }
    return false;
}

/* With the lock held: the user mappings over pages [first, end), one by
 * one (umap_first, then umap_next with the same range until NULL). Each
 * u's pages [*lo, *hi) are the part of [first, end) it maps, at user
 * address umap_va(u, *lo). */
static struct vmo_umap *umap_of(struct itree_node *n, uint64_t first, uint64_t end, uint64_t *lo,
                                uint64_t *hi)
{
    if (!n)
        return NULL;
    *lo = n->first > first ? n->first : first;
    *hi = n->end < end ? n->end : end;
    return container_of(n, struct vmo_umap, pages);
}

static struct vmo_umap *umap_first(const struct vmo *v, uint64_t first, uint64_t end,
                                   uint64_t *lo, uint64_t *hi)
{
    return umap_of(itree_first(&v->umaps, first, end), first, end, lo, hi);
}

static struct vmo_umap *umap_next(const struct vmo_umap *u, uint64_t first, uint64_t end,
                                  uint64_t *lo, uint64_t *hi)
{
    return umap_of(itree_next(&u->pages, first, end), first, end, lo, hi);
}

static uint64_t umap_va(const struct vmo_umap *u, uint64_t idx)
{
    return u->base + ((idx - u->pages.first) << PAGE_SHIFT);
}

/* What walking the user mappings over [lo, hi) of one costs a batch, each
 * page costing `page` (VMO_BATCH_BUDGET's units). */
static uint64_t umap_cost(uint64_t lo, uint64_t hi, uint64_t page)
{
    return VMO_BATCH_MAPPING_COST + page * (hi - lo);
}

/* With the lock held: zap pages [first, end) from every user mapping.
 * Returns what that cost (VMO_BATCH_BUDGET's units). */
static uint64_t zap_umaps_locked(const struct vmo *v, uint64_t first, uint64_t end,
                                 struct tlb_gather *g)
{
    uint64_t lo, hi, cost = 0;
    for (struct vmo_umap *u = umap_first(v, first, end, &lo, &hi); u;
         u = umap_next(u, first, end, &lo, &hi)) {
        aspace_zap_locked(u->as, umap_va(u, lo), (hi - lo) << PAGE_SHIFT, g);
        cost += umap_cost(lo, hi, VMO_BATCH_CLEAR_COST);
    }
    return cost;
}

uint64_t vmo_batch_ns_most, vmo_batch_cost_most;

/* With the lock held: the first mapping in order over [idx, stop) at which
 * the cost of those so far (each page `page`) passes VMO_BATCH_BUDGET (*lo:
 * where it starts in the range), or NULL if they all fit. */
static struct vmo_umap *batch_over_locked(const struct vmo *v, uint64_t idx, uint64_t stop,
                                          uint64_t page, uint64_t *lo)
{
    uint64_t cost = 0, hi;
    for (struct vmo_umap *u = umap_first(v, idx, stop, lo, &hi); u;
         u = umap_next(u, idx, stop, lo, &hi)) {
        cost += umap_cost(*lo, hi, page);
        if (cost > VMO_BATCH_BUDGET)
            return u;
    }
    return NULL;
}

/* With the lock held: where a batch of pages [idx, stop) must stop so that
 * its walk over the user mappings (each page costing `page`: clearing or
 * write-protecting) stays within VMO_BATCH_BUDGET, down to one page. The
 * mappings come by first page, so the batch can end where the one that
 * passes the budget starts. If that is idx itself (every mapping so far
 * covers page idx), the n mappings over idx allow (VMO_BATCH_BUDGET / n -
 * VMO_BATCH_MAPPING_COST) / page pages, and it ends there or where a later
 * one passes. A page under more mappings than one batch allows is a batch
 * of its own, whatever it costs: VMAR_MAX_OVERLAP bounds the mappings each
 * address space has over it. */
static uint64_t batch_end_locked(const struct vmo *v, uint64_t idx, uint64_t stop,
                                 uint64_t page)
{
    uint64_t lo, hi, n = 0;
    if (!batch_over_locked(v, idx, stop, page, &lo))
        return stop;
    if (lo > idx)
        return lo;
    for (struct vmo_umap *u = umap_first(v, idx, idx + 1, &lo, &hi); u;
         u = umap_next(u, idx, idx + 1, &lo, &hi))
        n++;
    if (n * umap_cost(0, 1, page) >= VMO_BATCH_BUDGET)
        return idx + 1;
    /* >= 1, by the test above */
    uint64_t most = (VMO_BATCH_BUDGET / n - VMO_BATCH_MAPPING_COST) / page;
    if (stop > idx + most)
        stop = idx + most;
    if (!batch_over_locked(v, idx, stop, page, &lo))
        return stop;
    return lo > idx ? lo : idx + 1;   /* (the n over idx fit: lo > idx) */
}

static void note_most(uint64_t *most, uint64_t x)
{
    uint64_t old = __atomic_load_n(most, __ATOMIC_RELAXED);
    while (x > old &&
           !__atomic_compare_exchange_n(most, &old, x, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

/* A batch that started at t0 (uptime_ns) has ended, its walk over the
 * mappings having cost `cost`: the most so far go in vmo_batch_ns_most and
 * vmo_batch_cost_most. */
static void batch_note(uint64_t t0, uint64_t cost)
{
    note_most(&vmo_batch_ns_most, uptime_ns() - t0);
    note_most(&vmo_batch_cost_most, cost);
}

/* With the lock held: take the committed pages in [idx, end), at most up to
 * the end of idx's leaf and within VMO_BATCH_BUDGET, out of the table onto g
 * and out of every user mapping. Returns where to continue (past the
 * missing tables that follow a missing leaf: leaf_find_locked; where the
 * budget ends the batch; or at a shared page when g's array is full:
 * finish g first). `clean_only`: a writable pager VMO's dirty and writing
 * pages stay (eviction, invalidation: they are newer than the disk). */
static uint64_t take_batch_locked(struct vmo *v, uint64_t idx, uint64_t end, struct tlb_gather *g,
                                  bool clean_only)
{
    bool keep = clean_only && v->pwritable;
    uint64_t next;
    uint64_t *leaf = leaf_find_locked(v, idx, &next);
    if (!leaf)
        return next < end ? next : end;
    uint64_t stop = ALIGN_UP(idx + 1, LEAF_PAGES);
    if (stop > end)
        stop = end;
    while (idx < stop && !leaf[idx % LEAF_PAGES])
        idx++;   /* no page, so no entry anywhere: nothing to take or clear */
    if (idx == stop)
        return stop;
    uint64_t t0 = uptime_ns();
    stop = batch_end_locked(v, idx, stop, VMO_BATCH_CLEAR_COST);
    uint64_t taken = 0, charged = 0;
    for (uint64_t i = idx; i < stop; i++) {
        uint64_t *s = &leaf[i % LEAF_PAGES];
        if (!*s)
            continue;
        struct page *p = pa_page(*s);
        if (keep && pager_page_state_vlocked(v->pv, p))
            continue;   /* (its entries may be zapped below: the next touch puts them back) */
        /* The table's reference, released after the flush. A page other
         * tables may hold can't use its list node (another gather's). */
        if (page_shared(v, p)) {
            if (tlb_gather_full(g)) {
                stop = i;
                break;
            }
            tlb_gather_shared(g, p);
        } else {
            if (v->pv)   /* off its pager's cache first: the gather takes its list node */
                pager_page_out_locked(v, p);
            tlb_gather_page(g, p);
        }
        *s = 0;
        v->committed--;
        taken++;
        charged += charged_alone(v, i);
    }
    job_uncharge(v->job, JOB_LIMIT_PAGES, charged);
    bool any = taken != 0;
    /* Entries exist only for committed pages (a fault installs under this
     * lock what the table holds), so nothing taken means nothing to zap. */
    batch_note(t0, any ? zap_umaps_locked(v, idx, stop, g) : 0);
    return stop;
}

/* Flush and free once a batch's worth of pages is waiting (or as many
 * shared pages as g holds). */
static void gather_maybe_finish(struct tlb_gather *g)
{
    if (g->npages >= LEAF_PAGES || tlb_gather_full(g))
        tlb_gather_finish(g);
}

/* shrink_pages: does the table holding pages [base, base + span) lie
 * wholly past the new end `first`, with the walk past its last page (or
 * past old_end: no table lies wholly past the size, so nothing is left in
 * it beyond)? */
static bool table_done(uint64_t base, uint64_t span, uint64_t first, uint64_t next,
                       uint64_t old_end)
{
    uint64_t end = base + span < old_end ? base + span : old_end;
    return base >= first && next >= end;
}

/* shrink_pages, with the lock held, after the batch at idx: the walk took
 * every page of [first, next), visiting every table that exists there
 * (take_batch_locked skips only missing ones), so idx's leaf, mid and
 * upper tables are empty once table_done says so: they go, the leaf first.
 * Not if the batch stopped early (next is inside the leaf then). */
static void free_passed_locked(struct vmo *v, uint64_t idx, uint64_t next, uint64_t first,
                               uint64_t old_end)
{
    uint64_t ***mid = mid_ref_locked(v, idx, false);
    uint64_t **leaf = leaf_ref_locked(v, idx);
    if (leaf && *leaf &&
        table_done(ALIGN_DOWN(idx, LEAF_PAGES), LEAF_PAGES, first, next, old_end)) {
        table_free_locked(v, *leaf);
        *leaf = NULL;
    }
    if (mid && *mid && table_done(ALIGN_DOWN(idx, MID_PAGES), MID_PAGES, first, next, old_end)) {
        table_free_locked(v, *mid);
        *mid = NULL;
    }
    if (idx < ROOT_PAGES || !v->top)
        return;
    uint64_t ****upper = &v->top[idx / UPPER_PAGES];
    uint64_t ubase = ALIGN_DOWN(idx, UPPER_PAGES);
    uint64_t lo = ubase > ROOT_PAGES ? ubase : ROOT_PAGES;   /* upper table 0 holds none below */
    if (*upper && table_done(lo, ubase + UPPER_PAGES - lo, first, next, old_end)) {
        table_free_locked(v, *upper);
        *upper = NULL;
    }
}

/* Shrink from old_end pages to first pages; v->size is already lowered and
 * v->resize held, so nothing can appear at or past `first` meanwhile.
 * Tables wholly past first go as the walk passes them, a lock hold each;
 * the last hold frees what straddles first and the top table. */
static void shrink_pages(struct vmo *v, uint64_t first, uint64_t old_end)
{
    struct tlb_gather g;
    tlb_gather_init(&g);
    for (uint64_t idx = first; idx < old_end;) {
        uint64_t f = vlock(v);
        uint64_t next = take_batch_locked(v, idx, old_end, &g, false);
        free_passed_locked(v, idx, next, first, old_end);
        vunlock(v, f);
        gather_maybe_finish(&g);
        idx = next;
    }
    uint64_t f = vlock(v);
    drop_from_locked(v, first);   /* only tables straddling first, and the top table, are left */
    vunlock(v, f);
    tlb_gather_finish(&g);
}

/* A kept VMO growing, "vmo resize" held: commit page idx, which lies past
 * the size (so its slot is empty and nobody else can reach it), charged
 * to v's job before it is published. */
static status_t commit_past_end(struct vmo *v, uint64_t idx)
{
    struct page *p = pmm_alloc_pages(0, PMM_ZERO);
    if (!p)
        return ERR_NO_MEMORY;
    uint64_t f = vlock(v);
    uint64_t *s = slot_locked(v, idx, false);
    ASSERT(!s || !*s);
    s = charge_slot_locked(v, idx, s);
    if (s) {
        *s = page_to_phys(p);   /* the table takes our reference */
        v->committed++;
    }
    vunlock(v, f);
    if (!s) {
        page_put(p);
        return ERR_NO_MEMORY;
    }
    return OK;
}

/* A kept VMO growing from `first` pages to `end` (bytes: size), "vmo
 * resize" held: every new page committed first, then the size raised. On a
 * failure the pages made here go again and the size stays. */
static status_t grow_kept(struct vmo *v, uint64_t first, uint64_t end, uint64_t size)
{
    status_t st = OK;
    uint64_t idx = first;
    for (; idx < end && st == OK; idx++)
        st = commit_past_end(v, idx);
    if (st != OK) {
        shrink_pages(v, first, idx);   /* past the size: no mapping has entries for them */
        return st;
    }
    uint64_t f = vlock(v);
    __atomic_store_n(&v->size, size, __ATOMIC_RELAXED);
    vunlock(v, f);
    return OK;
}

/* vmo_set_size for a kept VMO, as a writer with "vmo resize" held: never
 * smaller; bigger only with every new page committed. */
static status_t set_size_kept(struct vmo *v, uint64_t size)
{
    uint64_t old = vmo_size(v);   /* only we change it (resize is held) */
    if (size < old)
        return ERR_BAD_STATE;
    if (size == old)
        return OK;
    return grow_kept(v, old >> PAGE_SHIFT, size >> PAGE_SHIFT, size);
}

status_t vmo_set_size(struct vmo *v, uint64_t size)
{
    if (v->kind != VMO_PAGED || v->pv)
        return ERR_NOT_SUPPORTED;   /* a pager VMO's size is its pager's */
    if (size > size_max(v))
        return ERR_OUT_OF_RANGE;
    size = ALIGN_UP(size, PAGE_SIZE);
    status_t st = writer_enter(v);
    if (st != OK)
        return st;
    mutex_lock(&v->resize);
    if (vmo_is_kept(v)) {
        st = set_size_kept(v, size);
        mutex_unlock(&v->resize);
        writer_exit(v);
        return st;
    }
    uint64_t f = vlock(v);
    /* Frozen while a clone shares its pages (a creation holds resize);
     * a kill ends the wait (cow_wait's rule). */
    while (v->protecting && st == OK)
        st = waitqueue_wait_cancellable(&protect_wq, &v->base.lock, &f, DEADLINE_NEVER);
    uint64_t first = size >> PAGE_SHIFT, end = v->size >> PAGE_SHIFT;
    if (st == OK && first < end && ranges_overlap_locked(v, first, end))
        st = ERR_BAD_STATE;
    else if (st == OK)
        __atomic_store_n(&v->size, size, __ATOMIC_RELAXED);
    vunlock(v, f);
    if (st == OK && first < end) {
        shrink_pages(v, first, end);
        /* A pager child's window shrinks with it, after the pages: until
         * then they were the window's (paid at creation), so the window's
         * charge for them goes now, once. Growing never reopens it. */
        f = vlock(v);
        if (v->win > first) {
            job_uncharge(v->job, JOB_LIMIT_PAGES, v->win - first);
            v->win = first;
        }
        vunlock(v, f);
    }
    mutex_unlock(&v->resize);
    writer_exit(v);
    return st;
}

status_t vmo_commit(struct vmo *v, uint64_t offset, uint64_t len)
{
    if (v->pv)
        return ERR_NOT_SUPPORTED;   /* only its pager puts a page in */
    if (!in_bounds(offset, len, vmo_size(v)))
        return ERR_OUT_OF_RANGE;
    if (v->kind != VMO_PAGED || len == 0)
        return OK;   /* contiguous and physical VMOs are always committed */
    status_t st = writer_enter(v);
    if (st != OK)
        return st;
    st = commit_pages(v, offset >> PAGE_SHIFT, ALIGN_UP(offset + len, PAGE_SIZE) >> PAGE_SHIFT,
                      GET_COMMIT);
    writer_exit(v);
    return st;
}

/* vmo_decommit's work, as a writer (writer_enter); `clean_only` as for
 * take_batch_locked. */
static status_t decommit_pages(struct vmo *v, uint64_t offset, uint64_t len, bool clean_only)
{
    uint64_t f = vlock(v);
    if (!in_bounds(offset, len, v->size)) {
        vunlock(v, f);
        return ERR_OUT_OF_RANGE;
    }
    uint64_t first = offset >> PAGE_SHIFT;
    uint64_t end = ALIGN_UP(offset + len, PAGE_SIZE) >> PAGE_SHIFT;
    /* Refuse up front if a pin or kernel mapping covers any of it, so the
     * common failure changes nothing. */
    bool busy = ranges_overlap_locked(v, first, end);
    vunlock(v, f);
    if (busy)
        return ERR_BAD_STATE;

    struct tlb_gather g;
    tlb_gather_init(&g);
    status_t st = OK;
    for (uint64_t idx = first; idx < end;) {
        f = vlock(v);
        if (v->protecting) {   /* a decommit changes the bytes: frozen meanwhile */
            vunlock(v, f);
            tlb_gather_finish(&g);   /* what it took so far goes first */
            if ((st = cow_wait(v)) != OK)
                break;
            continue;
        }
        uint64_t stop = v->size >> PAGE_SHIFT;   /* a racing shrink took the rest */
        if (stop > end)
            stop = end;
        if (idx >= stop) {
            vunlock(v, f);
            break;
        }
        uint64_t leaf_end = ALIGN_UP(idx + 1, LEAF_PAGES) < stop ? ALIGN_UP(idx + 1, LEAF_PAGES)
                                                                 : stop;
        if (ranges_overlap_locked(v, idx, leaf_end)) {   /* pinned since we started */
            vunlock(v, f);
            st = ERR_BAD_STATE;
            break;
        }
        idx = take_batch_locked(v, idx, stop, &g, clean_only);
        vunlock(v, f);
        gather_maybe_finish(&g);
    }
    tlb_gather_finish(&g);
    return st;
}

status_t vmo_decommit(struct vmo *v, uint64_t offset, uint64_t len)
{
    if (v->kind != VMO_PAGED || v->pv)
        return ERR_NOT_SUPPORTED;   /* only its pager takes a page out */
    if (vmo_is_kept(v))
        return ERR_BAD_STATE;   /* its pages never leave (the file header) */
    status_t st = writer_enter(v);
    if (st != OK)
        return st;
    st = decommit_pages(v, offset, len, false);
    writer_exit(v);
    return st;
}

status_t vmo_seal(struct vmo *v)
{
    uint64_t f = vlock(v);
    /* A pager VMO's pages arrive after the seal: never sealed unchecked;
     * nor a pager child, whose untouched pages may still change. */
    bool ok = v->kind == VMO_PAGED && !v->pv && !v->src && !v->jit && itree_empty(&v->umaps) &&
              list_empty(&v->ranges) && v->writers == 0;
    if (ok)
        v->sealed = true;
    vunlock(v, f);
    return ok ? OK : ERR_BAD_STATE;
}

status_t vmo_seal_verified(struct vmo *v, struct vmo_verify *vf)
{
    uint64_t f = vlock(v);
    /* Code never comes from a writable page: never a writable one. */
    bool ok = v->pv && !v->pwritable && !v->pdetached && !v->verify && !v->jit &&
              itree_empty(&v->umaps) && list_empty(&v->ranges) && v->writers == 0;
    if (ok) {
        /* Pages that came before the table were never checked. Nothing
         * maps them (no umap) and the table holds the only reference
         * besides a reader's own, so they go here, with no shootdown. */
        drop_from_locked(v, 0);
        v->sealed = true;
        __atomic_store_n(&v->verify, vf, __ATOMIC_RELEASE);   /* vmo_verifier's acquire */
    }
    vunlock(v, f);
    return ok ? OK : ERR_BAD_STATE;
}

struct vmo_verify *vmo_verifier(struct vmo *v)
{
    return __atomic_load_n(&v->verify, __ATOMIC_ACQUIRE);
}

status_t vmo_make_jit(struct vmo *v)
{
    /* Fixed at creation: no lock needed to refuse the other kinds. */
    if (v->kind != VMO_PAGED || v->pv || v->src || vmo_is_kept(v))
        return ERR_NOT_SUPPORTED;
    mutex_lock(&v->resize);   /* no child creation is sharing its pages meanwhile */
    uint64_t f = vlock(v);
    bool ok = !v->jit && !v->sealed && v->committed == 0 && v->writers == 0 &&
              itree_empty(&v->umaps) && list_empty(&v->ranges);
    if (ok)
        __atomic_store_n(&v->jit, true, __ATOMIC_RELEASE);   /* vmo_is_jit's acquire */
    vunlock(v, f);
    mutex_unlock(&v->resize);
    return ok ? OK : ERR_BAD_STATE;
}

bool vmo_is_jit(const struct vmo *v)
{
    return __atomic_load_n(&v->jit, __ATOMIC_ACQUIRE);
}

/* ---- user mappings (reverse map, see aspace_vmo.h) ------------------------ */

status_t vmo_umap_add(struct vmo *v, struct vmo_umap *u, bool check)
{
    uint64_t f = vlock(v);
    /* (A split, unchecked, copies a mapping that exists already: on a
     * sealed VMO that is never a writable one.) */
    status_t st = !check                                  ? OK
                  : u->pages.end > v->size >> PAGE_SHIFT  ? ERR_OUT_OF_RANGE
                  : u->writable && v->sealed              ? ERR_BAD_STATE
                  : u->writable && v->pv && !v->pwritable ? ERR_ACCESS_DENIED   /* pager's pages */
                                                          : OK;
    if (st != OK) {
        vunlock(v, f);
        return st;
    }
    itree_insert(&v->umaps, &u->pages);
    vunlock(v, f);
    kobject_ref(&v->base);
    return OK;
}

void vmo_umap_set(struct vmo *v, struct vmo_umap *u, uint64_t base, uint64_t first, uint64_t end)
{
    uint64_t f = vlock(v);
    u->base = base;
    itree_move(&v->umaps, &u->pages, first, end);
    vunlock(v, f);
}

void vmo_umap_remove(struct vmo *v, struct vmo_umap *u)
{
    uint64_t f = vlock(v);
    itree_erase(&v->umaps, &u->pages);
    vunlock(v, f);
    kobject_unref(&v->base);
}

int64_t vmo_rmap_check(struct vmo *v)
{
    uint64_t f = vlock(v);
    int64_t n = itree_check(&v->umaps);
    vunlock(v, f);
    return n;
}

/* A reference on the pager VMO's page for page idx of v (a pager child,
 * idx in its window), with no lock held: *out = NULL if it is absent there
 * (the fault waits for the pager), ERR_OUT_OF_RANGE past the pager VMO's
 * end (the page reads as zeros). */
static status_t source_peek(struct vmo *v, uint64_t idx, struct page **out)
{
    struct vmo *pv = v->src;
    uint64_t sidx = v->src_first + idx;
    uint64_t f = vlock(pv);
    status_t st = sidx >= pv->size >> PAGE_SHIFT ? ERR_OUT_OF_RANGE : OK;
    uint64_t *s = st == OK ? slot_locked(pv, sidx, false) : NULL;
    *out = s && *s ? pa_page(*s) : NULL;
    if (*out)
        page_get(*out);
    vunlock(pv, f);
    return st;
}

/* vmo_fault_map's absent page idx (not a pager VMO's), with no lock held:
 * fr gets the page it should hold, zeros or (src: a pager child's window)
 * the pager VMO's bytes. ERR_SHOULD_WAIT: that page is absent there too;
 * ERR_NO_MEMORY: no page. fr is empty on an error. */
static status_t fault_fill(struct vmo *v, uint64_t idx, bool src, struct fresh *fr)
{
    fresh_drop(fr);
    struct page *sp = NULL;
    status_t st = src ? source_peek(v, idx, &sp) : OK;
    if (src && st == OK && !sp)
        return ERR_SHOULD_WAIT;
    bool zero = !src || st == ERR_OUT_OF_RANGE;
    fr->page = pmm_alloc_pages(0, (zero ? PMM_ZERO : 0) | ((v->flags & VMO_DMA32) ? PMM_DMA32 : 0));
    if (fr->page && sp)
        memcpy(page_to_virt(fr->page), page_to_virt(sp), PAGE_SIZE);
    if (sp)
        page_put(sp);
    fr->filled = src;
    return fr->page ? OK : ERR_NO_MEMORY;
}

/* With the lock held: a store to page p (present) of v, a writable pager
 * VMO. A clean page becomes dirty, and v's first dirty page takes the
 * hold: a reference of v's own (the caller has one, so v lives), kept
 * until none is left (hold_end). *added: the pager has one more dirty page
 * (the caller then kicks its write-back, with no lock held).
 * ERR_NO_RESOURCES: p is clean and the pager is at its dirty limit;
 * nothing changed (the store waits: vmo_pager_throttle). */
static status_t store_locked(struct vmo *v, struct page *p, bool *added)
{
    *added = !pager_page_state_vlocked(v->pv, p);
    status_t st = pager_page_store_vlocked(v->pv, p);
    if (st != OK) {
        *added = false;
        return st;
    }
    if (*added) {
        v->dirty++;
        if (!v->held) {
            v->held = true;
            kobject_ref(&v->base);
        }
    }
    return OK;
}

/* No lock held, a reference of the caller's: if v's dirty and writing
 * pages are all gone (dropped, or written back), its hold goes. */
static void hold_end(struct vmo *v)
{
    uint64_t f = vlock(v);
    bool drop = v->held && v->dirty == 0;
    if (drop)
        v->held = false;
    vunlock(v, f);
    if (drop)
        kobject_unref(&v->base);   /* never the last: the caller's */
}

status_t vmo_fault_map(struct vmo *v, uint64_t idx, struct aspace *as, uint64_t va,
                       unsigned perms, bool write)
{
    struct fresh fr = { 0 };
    for (;;) {
        uint64_t f = vlock(v);
        if (idx >= v->size >> PAGE_SHIFT) {
            vunlock(v, f);
            fresh_drop(&fr);
            return ERR_OUT_OF_RANGE;
        }
        if (v->kind != VMO_PAGED) {
            aspace_set_pte_locked(as, va, v->phys + (idx << PAGE_SHIFT), perms, v->cache);
            vunlock(v, f);
            return OK;
        }
        if (write && v->protecting) {   /* frozen while a creation or a clone shares its pages */
            vunlock(v, f);
            fresh_drop(&fr);
            status_t st = cow_wait(v);
            if (st != OK)
                return st;   /* ERR_CANCELED: the fault path ends the thread */
            continue;
        }
        /* Nothing is mapped writable while frozen, so a later write faults
         * and waits above. */
        unsigned may = v->protecting ? perms & ~ASPACE_WRITE : perms;
        uint64_t *s = slot_locked(v, idx, false);
        struct page *p = s && *s ? pa_page(*s) : NULL;
        bool kick = false;
        if (p && v->pwritable) {
            /* Writable only while dirty: a store to a clean or writing
             * page makes it dirty here (the file header's "Writable pager
             * VMOs"), unless the pager is at its dirty limit: then the
             * store waits for a write-back, as a read waits for its page
             * (aspace_fault, without the region lock). */
            if (write && store_locked(v, p, &kick) != OK) {
                vunlock(v, f);
                fresh_drop(&fr);
                return ERR_NO_RESOURCES;
            }
            if (!(pager_page_state_vlocked(v->pv, p) & PAGER_PAGE_DIRTY))
                may &= ~ASPACE_WRITE;
        }
        bool shared = p && (perms & ASPACE_WRITE) && page_shared(v, p);
        if (p && (!shared || !write)) {
            /* Installed under the VMO lock: a decommit or a separation that
             * takes this page later zaps this entry too (see the file
             * header). A shared page is never mapped writable. */
            aspace_set_pte_locked(as, va, *s, shared ? perms & ~ASPACE_WRITE : may, v->cache);
            vunlock(v, f);
            fresh_drop(&fr);
            if (kick)   /* one more dirty page: past half the limit, write-back's request */
                pager_writeback_kick(v->pv);
            return OK;
        }
        if (p && fr.page && fr.from == p) {   /* separate it: our copy goes in */
            struct tlb_gather g;
            tlb_gather_init(&g);
            uint64_t pa = page_to_phys(fr.page);
            cow_replace_locked(v, idx, s, &fr, &g);
            aspace_set_pte_locked(as, va, pa, perms, v->cache);
            vunlock(v, f);
            tlb_gather_finish(&g);   /* the old page's entries, before the access is retried */
            fresh_drop(&fr);
            return OK;
        }
        if (p) {
            status_t st = copy_unlocked(v, f, p, &fr);
            if (st != OK)
                return st;
            continue;
        }
        if (v->pv) {
            /* A pager VMO's absent page: the caller asks the pager for it
             * (vmo_page_request) and waits, with no lock held. */
            vunlock(v, f);
            fresh_drop(&fr);
            return ERR_SHOULD_WAIT;
        }
        bool src = in_window_locked(v, idx);
        if (fr.page && !fr.from && fr.filled == src) {
            if (!(s = charge_slot_locked(v, idx, s))) {
                vunlock(v, f);
                fresh_drop(&fr);
                return ERR_NO_MEMORY;
            }
            *s = page_to_phys(fr.page);   /* the table takes our reference */
            fr.page = NULL;
            v->committed++;
            aspace_set_pte_locked(as, va, *s, may, v->cache);
            vunlock(v, f);
            return OK;
        }
        /* Not committed and no page of ours yet: get one, then retry. */
        vunlock(v, f);
        status_t st = fault_fill(v, idx, src, &fr);
        if (st != OK)
            return st;
    }
}

/* ---- copy-on-write children (see the file header) ------------------------- */

/* With the lock held: take the write bit off pages [first, end) in every
 * user mapping of v that may be writable (the others never have it).
 * Returns what the walk cost (VMO_BATCH_BUDGET's units: every mapping
 * over the pages counts). */
static uint64_t wrprotect_umaps_locked(const struct vmo *v, uint64_t first, uint64_t end,
                                       struct tlb_gather *g)
{
    uint64_t lo, hi, cost = 0;
    for (struct vmo_umap *u = umap_first(v, first, end, &lo, &hi); u;
         u = umap_next(u, first, end, &lo, &hi)) {
        if (u->writable)
            aspace_wrprotect_locked(u->as, umap_va(u, lo), (hi - lo) << PAGE_SHIFT, g);
        cost += umap_cost(lo, hi, VMO_BATCH_PROTECT_COST);
    }
    return cost;
}

/* p's lock held and c's (nested): share p's present pages [idx, stop) (in
 * one leaf) into c, p's page i as c's page i - offp. The pages c pays for
 * alone (outside its window) are charged first, in one charge; a table c
 * can't have gives back what is left of it (ERR_NO_MEMORY: the pages
 * shared so far stay, for c's destroy to give back). *n: pages shared. */
static status_t share_leaf_locked(struct vmo *p, struct vmo *c, uint64_t idx, uint64_t stop,
                                  uint64_t offp, uint64_t *n)
{
    uint64_t **ref = leaf_ref_locked(p, idx);
    uint64_t *leaf = ref ? *ref : NULL;
    *n = 0;
    if (!leaf)
        return OK;
    uint64_t pay = 0;
    for (uint64_t i = idx; i < stop; i++)
        pay += leaf[i % LEAF_PAGES] && charged_alone(c, i - offp);
    status_t st = job_charge(c->job, JOB_LIMIT_PAGES, pay);
    for (uint64_t i = idx; i < stop && st == OK; i++) {
        uint64_t pa = leaf[i % LEAF_PAGES];
        if (!pa)
            continue;
        uint64_t *s = slot_locked(c, i - offp, true);
        if (!s) {
            job_uncharge(c->job, JOB_LIMIT_PAGES, pay);
            st = ERR_NO_MEMORY;
            break;
        }
        page_share(pa_page(pa));
        *s = pa;
        c->committed++;
        pay -= charged_alone(c, i - offp);
        (*n)++;
    }
    return st;
}

/* p's `protecting` raised by the caller: share p's present pages [first,
 * end) into c, p's page i as c's page i - offp, a leaf at a time (a run of
 * missing tables in one look), and take the write bit off p's entries for
 * them, adding their ranges and CPUs to g (the caller shoots them down).
 * ERR_BAD_STATE if a pin or a kernel mapping covers a page there. */
static status_t share_range(struct vmo *p, struct vmo *c, uint64_t offp, uint64_t first,
                            uint64_t end, struct tlb_gather *g)
{
    status_t st = OK;
    for (uint64_t idx = first; idx < end && st == OK;) {
        uint64_t stop = ALIGN_UP(idx + 1, LEAF_PAGES) < end ? ALIGN_UP(idx + 1, LEAF_PAGES) : end;
        uint64_t n = 0, next;
        uint64_t f = vlock(p);
        spin_lock_nested(&c->base.lock, 1);   /* parent, then the new child: nobody else has it */
        uint64_t t0 = uptime_ns();
        if (!leaf_find_locked(p, idx, &next))   /* past the missing tables, as decommit */
            stop = next < end ? next : end;
        else if (ranges_overlap_locked(p, idx, stop))
            st = ERR_BAD_STATE;   /* a device or the kernel may write these behind the sharing */
        else {
            stop = batch_end_locked(p, idx, stop, VMO_BATCH_PROTECT_COST);   /* in budget */
            st = share_leaf_locked(p, c, idx, stop, offp, &n);
        }
        batch_note(t0, n ? wrprotect_umaps_locked(p, idx, stop, g) : 0);
        spin_unlock(&c->base.lock);
        vunlock(p, f);
        idx = stop;
    }
    return st;
}

/* p's "vmo resize" held (its size can't change): share its pages [offp,
 * offp + c's size) into c (share_range); one shootdown at the end.
 * Meanwhile p's `protecting` makes a separation in p wait. ERR_BAD_STATE
 * if a pin or a kernel mapping covers a page there. */
static status_t share_pages(struct vmo *p, struct vmo *c, uint64_t offp)
{
    uint64_t f = vlock(p);
    uint64_t end = offp + (c->size >> PAGE_SHIFT);   /* (no overflow: both at most 2^35) */
    if (end > p->size >> PAGE_SHIFT)
        end = p->size >> PAGE_SHIFT;   /* past it the child's pages are zero */
    p->protecting++;
    freeze_drain_locked(p, &f);
    vunlock(p, f);
    struct tlb_gather g;
    tlb_gather_init(&g);
    status_t st = share_range(p, c, offp, offp, end, &g);
    DBG_HOOK(DBG_VMO_CHILD_SHOOT, p);
    tlb_gather_finish(&g);
    protect_end(p);
    return st;
}

/* c, new, takes its absent pages [0, win) from pager VMO src's pages from
 * `first` on: the window is charged whole now and its tables made, a leaf
 * per lock hold, so no touch of it ever charges anything. On an error what
 * was set up stays, for c's destroy to give back. */
static status_t window_set(struct vmo *c, struct vmo *src, uint64_t first, uint64_t win)
{
    status_t st = job_charge(c->job, JOB_LIMIT_PAGES, win);
    if (st != OK)
        return st;
    kobject_ref(&src->base);
    uint64_t f = vlock(c);
    c->src = src;   /* from here on c's destroy gives the window and the reference back */
    c->src_first = first;
    c->win = win;
    vunlock(c, f);
    for (uint64_t i = 0; i < win && st == OK; i += LEAF_PAGES) {
        f = vlock(c);
        st = slot_locked(c, i, true) ? OK : ERR_NO_MEMORY;
        vunlock(c, f);
    }
    return st;
}

/* A kept p's pages copied into c, p's page offp + i as c's page i for i
 * in [first, end), one at a time (a kept VMO is never shared: a separation
 * would swap a page under its kept readers). Each is charged to c as it
 * goes in. */
static status_t copy_pages(struct vmo *p, struct vmo *c, uint64_t offp, uint64_t first,
                           uint64_t end)
{
    for (uint64_t i = first; i < end; i++) {
        struct page *from, *pg;
        status_t st = get_page(p, offp + i, GET_PEEK, &from);
        if (st == ERR_OUT_OF_RANGE)
            return OK;   /* past p's end: zeros */
        if (st != OK)
            return st;
        if (!from)
            continue;
        pg = pmm_alloc_pages(0, 0);
        if (pg)
            memcpy(page_to_virt(pg), page_to_virt(from), PAGE_SIZE);
        page_put(from);
        uint64_t f = vlock(c);
        uint64_t *s = pg ? charge_slot_locked(c, i, slot_locked(c, i, false)) : NULL;
        if (s) {
            *s = page_to_phys(pg);   /* the table takes our reference */
            c->committed++;
        }
        vunlock(c, f);
        if (!s) {
            if (pg)
                page_put(pg);
            return ERR_NO_MEMORY;
        }
    }
    return OK;
}

status_t vmo_create_child(struct job *job, struct vmo *p, uint32_t flags, uint64_t offset,
                          uint64_t size, struct vmo **out)
{
    if (flags != VMO_CHILD_SNAPSHOT && flags != VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE)
        return ERR_INVALID_ARGS;
    if (offset & (PAGE_SIZE - 1))
        return ERR_INVALID_ARGS;
    if (offset > VMO_MAX_SIZE || size > VMO_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    if (p->kind != VMO_PAGED)
        return ERR_NOT_SUPPORTED;
    /* A pager VMO's untouched pages follow the file: no true snapshot. */
    if ((p->pv || p->src) && flags == VMO_CHILD_SNAPSHOT)
        return ERR_NOT_SUPPORTED;
    /* Its window is charged and given its tables whole at creation. */
    if ((p->pv || p->src) && size > VMO_SMALL_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    struct vmo *c;
    status_t st = vmo_create_for(job, size, 0, &c);
    if (st != OK)
        return st;
    uint64_t offp = offset >> PAGE_SHIFT, pages = c->size >> PAGE_SHIFT;
    if (p->pv) {
        st = window_set(c, p, offp, pages);
    } else if (vmo_is_kept(p)) {
        st = copy_pages(p, c, offp, 0, pages);
    } else {
        mutex_lock(&p->resize);
        /* A JIT VMO shares nothing (the file header): checked under "vmo
         * resize", which vmo_make_jit holds while it marks one. */
        if (vmo_is_jit(p))
            st = ERR_NOT_SUPPORTED;
        if (st == OK && p->src) {   /* a pager child's child: its pager VMO, the window's rest */
            uint64_t f = vlock(p);
            uint64_t win = p->win > offp ? p->win - offp : 0;
            vunlock(p, f);
            st = window_set(c, p->src, p->src_first + offp, win < pages ? win : pages);
        }
        if (st == OK)
            st = share_pages(p, c, offp);
        mutex_unlock(&p->resize);
    }
    if (st != OK) {
        kobject_unref(&c->base);   /* destroy gives back every page, reference and charge */
        return st;
    }
    *out = c;
    return OK;
}

/* ---- vmar_clone's copies (aspace_vmo.h) ------------------------------------- */

status_t vmo_clone_begin(struct job *job, struct vmo *v, struct vmo **out)
{
    /* aspace.c copies only what its clone_ok allows: never these. */
    if (v->kind != VMO_PAGED || v->pv || vmo_is_jit(v))
        return ERR_NOT_SUPPORTED;
    uint64_t f = vlock(v);
    v->protecting++;   /* from here on v's bytes and size wait for vmo_clone_end */
    freeze_drain_locked(v, &f);
    uint64_t size = v->size, win = v->win;
    vunlock(v, f);
    struct vmo *c;
    status_t st = vmo_create_for(job, size, 0, &c);
    if (st == OK && v->src) {   /* a pager child's: its pager VMO, the same window */
        uint64_t pages = size >> PAGE_SHIFT;
        st = window_set(c, v->src, v->src_first, win < pages ? win : pages);
        if (st != OK)
            kobject_unref(&c->base);   /* destroy gives the window back */
    }
    if (st != OK) {
        protect_end(v);
        return st;
    }
    *out = c;
    return OK;
}

status_t vmo_clone_share(struct vmo *v, struct vmo *c, uint64_t first, uint64_t end,
                         struct tlb_gather *g)
{
    uint64_t pages = c->size >> PAGE_SHIFT;   /* fixed: nobody else has c yet */
    if (end > pages)
        end = pages;   /* v was smaller at vmo_clone_begin: zeros there, as in v */
    if (first >= end)
        return OK;
    if (!vmo_is_kept(v))
        return share_range(v, c, 0, first, end, g);
    uint64_t f = vlock(v);
    bool busy = ranges_overlap_locked(v, first, end);
    vunlock(v, f);
    /* Copied, not shared; a pin or kernel mapping appearing now waits for
     * vmo_clone_end before it can commit (GET_PRIVATE) and so be written. */
    return busy ? ERR_BAD_STATE : copy_pages(v, c, 0, first, end);
}

void vmo_clone_end(struct vmo *v)
{
    protect_end(v);
}

/* ---- pager VMOs (<jam/pager.h>) ------------------------------------------- */

status_t vmo_create_pager(struct job *job, uint64_t size, struct pager_vmo *pv, bool writable,
                          struct vmo **out)
{
    if (size > VMO_SMALL_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    struct vmo *v;
    status_t st = vmo_new(size, 0, &v);
    if (st != OK)
        return st;
    if (job && (st = vmo_set_job(v, job)) != OK) {
        kobject_unref(&v->base);   /* (no link yet: pv stays the caller's) */
        return st;
    }
    v->pv = pv;
    v->pwritable = writable;
    *out = v;
    return OK;
}

bool vmo_is_pager(const struct vmo *v)
{
    return v->pv != NULL;
}

bool vmo_pager_writable(const struct vmo *v)
{
    return v->pwritable;
}

/* With the lock held: page idx of v (a writable pager VMO) if present,
 * else NULL. */
static struct page *ppage_locked(struct vmo *v, uint64_t idx)
{
    uint64_t *s = slot_locked(v, idx, false);
    return s && *s ? pa_page(*s) : NULL;
}

/* With the lock held: is page idx in the state pager_dirty's `flags` look
 * for (vmo_pager_dirty_run)? */
static bool run_page_locked(struct vmo *v, uint64_t idx, uint32_t flags)
{
    struct page *p = ppage_locked(v, idx);
    unsigned st = p ? pager_page_state_vlocked(v->pv, p) : 0;
    if (flags & PAGER_DIRTY_BEGIN)
        return st == PAGER_PAGE_DIRTY;   /* one write-back of a page at a time */
    if (flags & PAGER_DIRTY_PEEK)
        return st != 0;
    return st & PAGER_PAGE_DIRTY;
}

void vmo_pager_dirty_run(struct vmo *v, uint64_t first, uint64_t end, uint64_t around,
                         uint32_t flags, uint64_t max, struct page **pages, uint64_t *at,
                         uint64_t *n, uint64_t *dirtied)
{
    struct tlb_gather g;
    tlb_gather_init(&g);
    uint64_t oldest = UINT64_MAX;
    *at = first;
    *n = 0;
    uint64_t f = vlock(v);
    if (end > v->size >> PAGE_SHIFT)
        end = v->size >> PAGE_SHIFT;
    if (v->pdetached)
        end = first;   /* its pages are going: none */
    if (around != UINT64_MAX) {   /* the run holding page `around`, from as far back as it goes */
        uint64_t lo = first;
        if (around < lo || around >= end || !run_page_locked(v, around, flags))
            end = first;
        else
            for (first = around;
                 first > lo && around - first + 1 < max && run_page_locked(v, first - 1, flags);)
                first--;
    }
    uint64_t i = first;
    while (i < end && !run_page_locked(v, i, flags))
        i++;
    *at = i;
    while (i < end && i - *at < max && run_page_locked(v, i, flags))
        i++;
    uint64_t stop = i;
    uint64_t t0 = uptime_ns();
    if (stop > *at && (flags & PAGER_DIRTY_BEGIN))
        stop = batch_end_locked(v, *at, stop, VMO_BATCH_PROTECT_COST);   /* in budget */
    for (i = *at; i < stop; i++) {
        struct page *p = ppage_locked(v, i);
        page_get(p);
        pages[(*n)++] = p;
        uint64_t d = pager_page_dirtied_vlocked(v->pv, p);
        oldest = d < oldest ? d : oldest;
        if (flags & PAGER_DIRTY_BEGIN)
            pager_page_begin_vlocked(v->pv, p);
    }
    if (*n && (flags & PAGER_DIRTY_BEGIN)) {
        pager_begun_vlocked(v->pv, *at, stop);
        batch_note(t0, wrprotect_umaps_locked(v, *at, stop, &g));
    }
    vunlock(v, f);
    tlb_gather_finish(&g);   /* the one shootdown: no stale writable entry once we return */
    *dirtied = *n ? oldest : 0;
}

void vmo_pager_unbegin(struct vmo *v, uint64_t first, uint64_t end)
{
    uint64_t f = vlock(v);
    if (end > v->size >> PAGE_SHIFT)
        end = v->size >> PAGE_SHIFT;
    for (uint64_t i = first; i < end; i++) {
        struct page *p = ppage_locked(v, i);
        if (p && (pager_page_state_vlocked(v->pv, p) & PAGER_PAGE_WRITING))
            pager_page_unbegin_vlocked(v->pv, p);
    }
    vunlock(v, f);
}

void vmo_pager_written(struct vmo *v, uint64_t first, uint64_t end, status_t st,
                       struct list_node *dead)
{
    const uint64_t batch = PAGER_WB_WORDS * 64;   /* a write-back request's pages at most */
    for (uint64_t idx = first; idx < end;) {
        uint64_t ended[PAGER_WB_WORDS] = { 0 };
        uint64_t f = vlock(v);
        uint64_t stop = end - idx > batch ? idx + batch : end;
        if (stop > v->size >> PAGE_SHIFT)
            stop = v->size >> PAGE_SHIFT;   /* (a shrink took the rest, with its requests) */
        if (idx >= stop) {
            vunlock(v, f);
            break;
        }
        for (uint64_t i = idx; i < stop; i++) {
            struct page *p = ppage_locked(v, i);
            if (!p || !(pager_page_state_vlocked(v->pv, p) & PAGER_PAGE_WRITING))
                continue;
            if (pager_page_written_vlocked(v->pv, p))
                v->dirty--;   /* clean: its entries are read-only already */
            ended[(i - idx) / 64] |= 1ull << ((i - idx) % 64);
        }
        if (st != OK && v->pwb_error == OK)
            v->pwb_error = st;   /* the first since vmo_sync last reported one */
        pager_written_vlocked(v->pv, v, idx, stop, ended, st, dead);
        vunlock(v, f);
        idx = stop;
    }
    hold_end(v);
}

status_t vmo_pager_take_error(struct vmo *v)
{
    uint64_t f = vlock(v);
    status_t st = v->pwb_error;
    v->pwb_error = OK;
    vunlock(v, f);
    return st;
}

/* With the lock held: the pages of [first, end) (end - first at most
 * PAGER_WB_WORDS * 64) a write-back request made now needs (pager.c's
 * "Write-back requests"): dirty or writing pages in need[], those dirty
 * (stored to since their write-back began, if one did) in begin[] too.
 * False if none. */
static bool wb_needs_locked(struct vmo *v, uint64_t first, uint64_t end, uint64_t *need,
                            uint64_t *begin)
{
    bool any = false;
    memset(need, 0, PAGER_WB_WORDS * sizeof(*need));
    memset(begin, 0, PAGER_WB_WORDS * sizeof(*begin));
    for (uint64_t i = first; i < end; i++) {
        struct page *p = ppage_locked(v, i);
        unsigned st = p ? pager_page_state_vlocked(v->pv, p) : 0;
        uint64_t b = 1ull << ((i - first) % 64);
        if (st)
            need[(i - first) / 64] |= b;
        if (st & PAGER_PAGE_DIRTY)
            begin[(i - first) / 64] |= b;
        any |= st != 0;
    }
    return any;
}

status_t vmo_pager_writeback(struct vmo *v, uint64_t first, uint64_t end, struct pager_req **out)
{
    *out = NULL;
    if (!v->pwritable)
        return ERR_NOT_SUPPORTED;
    if (end <= first || end - first > PAGER_WB_WORDS * 64)
        return ERR_OUT_OF_RANGE;
    struct pager_req *r;
    status_t st = pager_wb_alloc(v->pv, job_current(), &r);   /* before the locks: it allocates */
    if (st != OK)
        return st;
    uint64_t need[PAGER_WB_WORDS], begin[PAGER_WB_WORDS];
    uint64_t f = vlock(v);
    st = end > v->size >> PAGE_SHIFT ? ERR_OUT_OF_RANGE : v->pdetached ? ERR_PEER_CLOSED : OK;
    bool any = st == OK && wb_needs_locked(v, first, end, need, begin);
    if (any)
        st = pager_wb_queue_vlocked(v->pv, r, first, end, need, begin, true, false);
    vunlock(v, f);
    if (st == OK && any)
        *out = r;
    else
        pager_req_free(r);
    return st;
}

status_t vmo_pager_writeback_bg(struct vmo *v, uint64_t idx, bool writing, struct pager_req *r,
                                bool caller)
{
    uint64_t need[PAGER_WB_WORDS], begin[PAGER_WB_WORDS];
    const uint64_t max = PAGER_WB_WORDS * 64;
    /* Dirty pages not being written back (BEGIN's), or any dirty or writing
     * one (PEEK's). */
    uint32_t sel = writing ? PAGER_DIRTY_PEEK : PAGER_DIRTY_BEGIN;
    uint64_t f = vlock(v);
    status_t st = v->pdetached ? ERR_PEER_CLOSED : OK;
    uint64_t end = v->size >> PAGE_SHIFT, first = idx;
    if (st == OK && (idx >= end || !run_page_locked(v, idx, sel)))
        st = ERR_NOT_FOUND;
    if (st == OK) {
        while (first > 0 && idx - first + 1 < max && run_page_locked(v, first - 1, sel))
            first--;
        uint64_t stop = idx + 1;
        while (stop < end && stop - first < max && run_page_locked(v, stop, sel))
            stop++;
        (void)wb_needs_locked(v, first, stop, need, begin);
        st = pager_wb_queue_vlocked(v->pv, r, first, stop, need, begin, caller, true);
    }
    vunlock(v, f);
    return st;
}

status_t vmo_pager_throttle(struct vmo *v, uint64_t *until, struct pager_req **out)
{
    *out = NULL;
    if (!v->pwritable)
        return ERR_NOT_SUPPORTED;
    return pager_throttle(v->pv, until, out);
}

/* vmo_pager_sync's step: a write-back request for pages [first, end) of v
 * (one chunk), waited for if `wait`. OK also if nothing there needs
 * writing back, or the chunk is past the size now (a shrink: its stores
 * went with the file's end); the request's own failure otherwise. */
static status_t sync_chunk(struct vmo *v, uint64_t first, uint64_t end, bool wait)
{
    uint64_t pages = vmo_size(v) >> PAGE_SHIFT;
    if (end > pages)
        end = pages;
    if (first >= end)
        return OK;
    struct pager_req *r;
    status_t st = vmo_pager_writeback(v, first, end, &r);
    if (st == ERR_OUT_OF_RANGE)
        return OK;   /* (shrunk since the look above) */
    if (st != OK || !r)
        return st;
    if (wait)
        st = pager_req_wait(r);
    pager_req_put(r);
    return st;
}

status_t vmo_pager_sync(struct vmo *v, uint64_t offset, uint64_t len, uint32_t flags)
{
    if (flags & ~VMO_SYNC_WAIT)
        return ERR_INVALID_ARGS;
    if (!v->pwritable)
        return OK;
    bool wait = flags & VMO_SYNC_WAIT;
    uint64_t f = vlock(v);
    status_t st = offset > v->size || len > v->size - offset ? ERR_OUT_OF_RANGE
                : v->pdetached                             ? ERR_PEER_CLOSED
                                                           : OK;
    bool any = v->dirty != 0;
    vunlock(v, f);
    if (st != OK)
        return st;
    /* One request a chunk, each made only once the one before it is done
     * (with `wait`): each needs the pages dirty or writing when it is
     * made, so a store made before this call is written by a write-back
     * that began after it, whichever chunk it is in. Without `wait` the
     * requests are only queued; one the job can't pay for is left to the
     * pager's own write-back by age, and the call still succeeds. */
    const uint64_t chunk = PAGER_WB_WORDS * 64;
    uint64_t first = offset >> PAGE_SHIFT;
    uint64_t end = (offset + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
    for (uint64_t i = first; any && i < end && st == OK; i += chunk)
        st = sync_chunk(v, i, end - i > chunk ? i + chunk : end, wait);
    if (!wait)
        return st == ERR_PEER_CLOSED || st == ERR_CANCELED ? st : OK;
    if (st == OK || st == ERR_IO || st == ERR_NO_SPACE) {
        /* Reported once: the kept error is taken whenever one is
         * reported (the requests' errors are kept as well). */
        status_t kept = vmo_pager_take_error(v);
        if (kept != OK)
            st = kept;
    }
    return st;
}

bool vmo_pager_writing_locked(struct vmo *v, uint64_t idx)
{
    struct page *p = idx < v->size >> PAGE_SHIFT ? ppage_locked(v, idx) : NULL;
    return p && (pager_page_state_vlocked(v->pv, p) & PAGER_PAGE_WRITING);
}

uint64_t vmo_pager_key(struct vmo *v)
{
    return pager_vmo_key(v->pv);
}

status_t vmo_pager_present(struct vmo *v, uint64_t first, uint64_t end, uint64_t byte_end,
                           struct page **pages)
{
    uint64_t f = vlock(v);
    status_t st = v->verify ? ERR_BAD_STATE : byte_end > v->size ? ERR_OUT_OF_RANGE : OK;
    for (uint64_t i = first; i < end && st == OK; i++) {
        uint64_t *s = slot_locked(v, i, false);
        pages[i - first] = s && *s ? pa_page(*s) : NULL;
        if (pages[i - first])
            page_get(pages[i - first]);
    }
    vunlock(v, f);
    return st;
}

unsigned vmo_pager_page_state(struct vmo *v, uint64_t idx)
{
    uint64_t f = vlock(v);
    uint64_t pa = v->pwritable && idx < v->size >> PAGE_SHIFT ? phys_locked(v, idx) : 0;
    unsigned st = pa ? pager_page_state_vlocked(v->pv, pa_page(pa)) : 0;
    vunlock(v, f);
    return st;
}

uint64_t vmo_pager_dirty_count(struct vmo *v)
{
    uint64_t f = vlock(v);
    uint64_t n = v->dirty;
    vunlock(v, f);
    return n;
}

uint64_t vmo_pager_koid(struct vmo *v)
{
    return v->pv ? pager_vmo_pager_koid(v->pv) : 0;
}

bool vmo_page_present_locked(struct vmo *v, uint64_t idx)
{
    uint64_t *s = slot_locked(v, idx, false);
    return s && *s;
}

/* With the lock held, page idx absent: where a request for it ends, past
 * the absent pages after it (PAGER_REQ_PAGES in all at most, not past the
 * size). */
static uint64_t absent_end_locked(struct vmo *v, uint64_t idx)
{
    uint64_t last = v->size >> PAGE_SHIFT, end = idx + 1;
    while (end < last && end - idx < PAGER_REQ_PAGES && !vmo_page_present_locked(v, end))
        end++;
    return end;
}

/* vmo_page_request for a pager child v: the request is its pager VMO's,
 * for the page idx's window page is there. r = NULL (look again) if idx is
 * present in v, past the window, or past the pager VMO's end (it reads as
 * zeros). */
static status_t child_page_request(struct vmo *v, uint64_t idx, struct pager_req **out)
{
    uint64_t f = vlock(v);
    status_t st = idx >= v->size >> PAGE_SHIFT ? ERR_OUT_OF_RANGE : OK;
    bool ask = st == OK && !vmo_page_present_locked(v, idx) && in_window_locked(v, idx);
    vunlock(v, f);
    *out = NULL;
    if (ask)
        st = vmo_page_request(v->src, v->src_first + idx, out);
    if (st == ERR_OUT_OF_RANGE && ask) {
        *out = NULL;   /* (past the pager VMO's end, not ours) */
        st = OK;
    }
    return st;
}

status_t vmo_page_request(struct vmo *v, uint64_t idx, struct pager_req **out)
{
    if (!v->pv && v->src)
        return child_page_request(v, idx, out);
    if (!v->pv)
        return ERR_NOT_SUPPORTED;
    struct pager_req *spare, *r = NULL;
    status_t st = pager_req_alloc(v->pv, &spare);   /* before the locks: it allocates */
    if (st != OK)
        return st;
    uint64_t f = vlock(v);
    if (idx >= v->size >> PAGE_SHIFT)
        st = ERR_OUT_OF_RANGE;
    else if (v->pdetached)
        st = ERR_PEER_CLOSED;
    else if (!vmo_page_present_locked(v, idx))
        st = pager_join_vlocked(v->pv, idx, absent_end_locked(v, idx), &spare, &r);
    vunlock(v, f);
    if (spare)
        pager_req_free(spare);
    if (st == OK)
        *out = r;
    return st;
}

/* With the lock held: may p be page idx of v? Always unless v is
 * verified; then if its supplier matched it already (`checked`: v was
 * verified when it looked, and `verify` never changes), else if it
 * matches now: a supply that copied it before the seal (rare). */
static bool approved_locked(struct vmo *v, uint64_t idx, struct page *p, bool checked)
{
    return !v->verify || checked || vmo_verify_match(v->verify, idx, page_to_virt(p));
}

status_t vmo_pager_publish(struct vmo *v, uint64_t idx, struct page *p, bool checked,
                           struct pager_rec *rec)
{
    uint64_t f = vlock(v);
    status_t st = v->pdetached ? ERR_PEER_CLOSED
                  : idx >= v->size >> PAGE_SHIFT ? ERR_OUT_OF_RANGE
                  : !approved_locked(v, idx, p, checked) ? ERR_IO
                                                         : OK;
    uint64_t *s = st == OK ? slot_locked(v, idx, false) : NULL;
    if (st == OK && s && *s)
        st = ERR_ALREADY_EXISTS;   /* a second answer */
    if (st == OK && !(s = charge_slot_locked(v, idx, s)))
        st = ERR_NO_MEMORY;
    if (st == OK) {
        *s = page_to_phys(p);   /* the table takes the caller's reference */
        v->committed++;
        pager_published_vlocked(v->pv, idx, p, rec);
    }
    vunlock(v, f);
    return st;
}

void vmo_pager_evict(struct vmo *v, uint64_t idx, const struct page *p)
{
    struct tlb_gather g;
    tlb_gather_init(&g);
    uint64_t f = vlock(v);
    /* (Else it went already, or, dirtied since it was popped, it stays.) */
    if (idx < v->size >> PAGE_SHIFT && phys_locked(v, idx) == page_to_phys(p))
        (void)take_batch_locked(v, idx, idx + 1, &g, true);
    vunlock(v, f);
    tlb_gather_finish(&g);
}

status_t vmo_pager_invalidate(struct vmo *v, uint64_t offset, uint64_t len)
{
    /* Not a writer: invalidating changes no byte (the pager supplies the
     * same file's bytes again), so a sealed VMO takes it as it takes an
     * eviction. No range can exist (pins and kernel mappings are refused).
     * A writable VMO's dirty and writing pages stay: newer than the disk. */
    return decommit_pages(v, offset, len, true);
}

status_t vmo_pager_set_size(struct vmo *v, uint64_t size, struct list_node *dead)
{
    if (size > VMO_SMALL_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    status_t st = writer_enter(v);   /* a sealed VMO's size is fixed */
    if (st != OK)
        return st;
    mutex_lock(&v->resize);
    uint64_t f = vlock(v);
    uint64_t first = size >> PAGE_SHIFT, end = v->size >> PAGE_SHIFT;
    st = v->pdetached ? ERR_PEER_CLOSED : OK;
    if (st == OK) {
        __atomic_store_n(&v->size, size, __ATOMIC_RELAXED);
        /* Under the lock with the size: a request is made only below the
         * size, so none can cross the new end after this. */
        if (first < end)
            pager_cut_vlocked(v->pv, first, dead);
    }
    vunlock(v, f);
    if (st == OK && first < end)
        shrink_pages(v, first, end);   /* nothing reappears past the end: resize is held */
    mutex_unlock(&v->resize);
    writer_exit(v);
    hold_end(v);   /* (the dirty pages past the end went with the rest) */
    return st;
}

void vmo_pager_detach(struct vmo *v)
{
    uint64_t f = vlock(v);
    v->pdetached = true;
    uint64_t size = v->size;
    vunlock(v, f);
    /* No range can exist (pins and kernel mappings are refused), so this
     * takes every page, dirty ones too (lost, as with a pulled disk). */
    (void)decommit_pages(v, 0, size, false);
    hold_end(v);
}

bool vmo_page_ready(struct vmo *v, uint64_t idx)
{
    uint64_t f = vlock(v);
    bool in = idx < v->size >> PAGE_SHIFT;
    bool ready = in && vmo_page_present_locked(v, idx);
    bool src = in && !ready && in_window_locked(v, idx);
    vunlock(v, f);
    if (src) {
        struct vmo *pv = v->src;
        uint64_t sidx = v->src_first + idx;
        f = vlock(pv);
        ready = sidx >= pv->size >> PAGE_SHIFT || vmo_page_present_locked(pv, sidx);
        vunlock(pv, f);
    }
    return ready;
}

uint64_t vmo_page_phys(struct vmo *v, uint64_t offset)
{
    uint64_t f = vlock(v);
    uint64_t idx = offset >> PAGE_SHIFT;
    uint64_t pa = idx < v->size >> PAGE_SHIFT ? phys_locked(v, idx) : 0;
    vunlock(v, f);
    return pa;
}

void vmo_resident(struct vmo *v, uint64_t first, uint64_t n, bool cache, uint8_t *out)
{
    uint64_t idx = first, end = first + n;
    while (idx < end) {
        uint64_t f = vlock(v);
        uint64_t size = v->size >> PAGE_SHIFT, stop = end < size ? end : size;
        if (idx >= stop) {
            vunlock(v, f);
            break;
        }
        if (v->kind != VMO_PAGED || (v->pv && !cache)) {
            memset(out + (idx - first), 1, stop - idx);   /* all there, or not told */
            idx = stop;
        } else {
            /* One leaf, or the run of missing tables from idx (next). */
            uint64_t next = stop, win = cache ? 0 : v->win;
            const uint64_t *leaf = leaf_find_locked(v, idx, &next);
            uint64_t lim = leaf ? ALIGN_DOWN(idx, LEAF_PAGES) + LEAF_PAGES : next;
            if (lim > stop)
                lim = stop;
            for (; idx < lim; idx++)
                out[idx - first] = (leaf && leaf[idx % LEAF_PAGES]) || idx < win;
        }
        vunlock(v, f);
    }
    if (idx < end)
        memset(out + (idx - first), 0, end - idx);   /* past the size */
}

/* ---- mapping and pinning ranges ----------------------------------------- */

/* Record r (busy) if it lies inside the VMO. On success it holds a VMO
 * reference and, for pins, gets its id. A JIT VMO is never pinned: the
 * test is here, under the lock vmo_make_jit marks it under (and where it
 * looks for pins), so a pin and the marking never pass each other. */
static status_t range_add(struct vmo *v, struct vmo_range *r)
{
    uint64_t f = vlock(v);
    status_t st = r->kind == RANGE_PIN && v->jit   ? ERR_NOT_SUPPORTED
                  : r->end > v->size >> PAGE_SHIFT ? ERR_OUT_OF_RANGE
                  : r->writes && v->sealed         ? ERR_BAD_STATE
                                                   : OK;
    if (st != OK) {
        vunlock(v, f);
        return st;
    }
    if (r->kind == RANGE_PIN)
        r->key = v->next_pin_id++;
    r->busy = true;
    list_add_tail(&v->ranges, &r->node);
    vunlock(v, f);
    kobject_ref(&v->base);
    return OK;
}

/* Unlink r (busy, so nobody else can find it) and release what it holds.
 * Drops a VMO reference: v may be gone afterwards. */
static void range_remove(struct vmo *v, struct vmo_range *r)
{
    uint64_t f = vlock(v);
    list_del(&r->node);
    vunlock(v, f);
    if (r->cap) {
        f = spin_lock_irqsave(&r->cap->lock);
        if (r->cap_linked) {
            list_del(&r->cap_node);
            r->cap_linked = false;
        }
        spin_unlock_irqrestore(&r->cap->lock, f);
        kobject_unref(r->cap);
    }
    job_uncharge(r->charged, JOB_LIMIT_HANDLES, 1);   /* the pinner's (pin_range_new) */
    job_unref(r->charged);
    kfree(r);
    kobject_unref(&v->base);
}

/* With the lock held: the non-busy range of `kind` with this key. */
static struct vmo_range *range_find_locked(const struct vmo *v, enum range_kind kind, uint64_t key)
{
    for (struct list_node *n = v->ranges.next; n != &v->ranges; n = n->next) {
        struct vmo_range *r = container_of(n, struct vmo_range, node);
        if (r->kind == kind && r->key == key && !r->busy)
            return r;
    }
    return NULL;
}

status_t vmo_map_kernel(struct vmo *v, uint64_t offset, uint64_t len, unsigned vm_flags,
                        void **va)
{
    if (v->pv || v->src)
        return ERR_NOT_SUPPORTED;   /* the kernel never waits for a pager */
    if (len == 0 || (vm_flags & ~VM_WRITE))
        return ERR_INVALID_ARGS;
    if (offset > VMO_MAX_SIZE || len > VMO_MAX_SIZE)
        return ERR_OUT_OF_RANGE;   /* (and offset + len can't overflow below) */
    struct vmo_range *r = kzalloc(sizeof(*r));
    if (!r)
        return ERR_NO_MEMORY;
    r->kind = RANGE_MAP;
    r->writes = (vm_flags & VM_WRITE) != 0;
    r->first = offset >> PAGE_SHIFT;
    r->end = ALIGN_UP(offset + len, PAGE_SIZE) >> PAGE_SHIFT;
    status_t st = range_add(v, r);
    if (st != OK) {
        kfree(r);
        return st;
    }
    /* Its own pages: a shared page is separated first, so a write through
     * the VMO never moves a page from under the mapping. */
    if (v->kind == VMO_PAGED && (st = commit_pages(v, r->first, r->end, GET_PRIVATE)) != OK) {
        range_remove(v, r);
        return st;
    }

    /* The range keeps every page in place, so the addresses looked up here
     * stay valid; map them in physically contiguous runs. */
    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t size = (r->end - r->first) << PAGE_SHIFT;
    uint64_t base = vmm_reserve(size);
    if (!vmm_prepare_kernel(base, size)) {   /* nothing mapped yet: give it all back */
        vmm_release(base, size);
        range_remove(v, r);
        return ERR_NO_MEMORY;
    }
    unsigned flags = vm_flags | v->cache | VM_GLOBAL | VM_SMALL;
    uint64_t run_va = base, run_pa = 0, run_len = 0;
    for (uint64_t idx = r->first; idx < r->end; idx++) {
        uint64_t f = vlock(v);
        uint64_t pa = phys_locked(v, idx);
        vunlock(v, f);
        ASSERT(pa != 0 || v->kind != VMO_PAGED);
        if (run_len && pa == run_pa + run_len) {
            run_len += PAGE_SIZE;
            continue;
        }
        if (run_len)
            vmm_map(pml4, run_va, run_pa, run_len, flags);
        run_va += run_len;
        run_pa = pa;
        run_len = PAGE_SIZE;
    }
    vmm_map(pml4, run_va, run_pa, run_len, flags);

    uint64_t f = vlock(v);
    r->key = base;
    r->busy = false;
    vunlock(v, f);
    *va = (void *)(base + (offset & (PAGE_SIZE - 1)));
    return OK;
}

status_t vmo_unmap_kernel(struct vmo *v, void *va)
{
    if (!irqs_enabled())
        return ERR_BAD_STATE;   /* the TLB shootdown needs other CPUs' answers */
    uint64_t base = ALIGN_DOWN((uint64_t)va, PAGE_SIZE);
    uint64_t f = vlock(v);
    struct vmo_range *r = range_find_locked(v, RANGE_MAP, base);
    if (r)
        r->busy = true;   /* still blocks decommit until it is really gone */
    vunlock(v, f);
    if (!r)
        return ERR_NOT_FOUND;
    uint64_t len = (r->end - r->first) << PAGE_SHIFT;
    vmm_unmap(vmm_kernel_pml4(), base, len);
    vmm_release(base, len);   /* shot down: the range and its page tables can be reused */
    range_remove(v, r);
    return OK;
}

/* A pin is a kernel allocation that lives until unpin or the cap's
 * close: one handle unit, like any small object (a driver could otherwise
 * pin one page forever and fill the kernel heap; test:
 * m6r_pins_are_charged). The PINNER pays: the unit is its cap's job's
 * (the driver's, which also pays for the cap and its domain's tables),
 * never the VMO's, whose holder may have handed the driver no more than a
 * read-only view (review M12.1 KDEV finding 4, KCORE finding 6; test:
 * dma_pin_charges_pinner). The range keeps a reference on that job and
 * gives the unit back to it wherever the range goes (range_remove: an
 * unpin, the cap's close, the quarantine's release); a pin kept for good
 * (pin_keep) keeps its unit. A cap made with no job (kernel tests) charges
 * nobody. A new busy pin range [first, end) on v, charged to `job` and
 * listed; `writes`: the device may write through it. */
static status_t pin_range_new(struct vmo *v, struct job *job, uint64_t first, uint64_t end,
                              bool writes, struct vmo_range **out)
{
    status_t st = job_charge(job, JOB_LIMIT_HANDLES, 1);
    if (st != OK)
        return st;
    struct vmo_range *r = kzalloc(sizeof(*r));
    if (!r) {
        job_uncharge(job, JOB_LIMIT_HANDLES, 1);
        return ERR_NO_MEMORY;
    }
    r->kind = RANGE_PIN;
    r->writes = writes;
    r->first = first;
    r->end = end;
    r->v = v;
    job_ref(job);
    r->charged = job;
    st = range_add(v, r);
    if (st != OK) {
        job_uncharge(job, JOB_LIMIT_HANDLES, 1);
        job_unref(job);
        kfree(r);
        return st;
    }
    *out = r;
    return OK;
}

/* ---- pins through the IOMMU ------------------------------------------------
 * A pin made with a cap that has an IOMMU domain (dma_cap.c) is mapped
 * there, each page at its own address, before the pin is published, and
 * unmapped before its pages may go: iommu_unmap returns only once the unit
 * has dropped what it cached. If the unit doesn't confirm that (an
 * invalidation failed), the device may still reach the pages, so they are
 * never given back (pin_keep). A closed cap's pins are not unmapped one by
 * one: dma_cap.c takes the whole domain away first. */

#define UNMAP_STACK_PAGES 32   /* addresses per iommu_unmap without a buffer from the heap */

/* The physical addresses of pages [first, first + n) of v into pa[]. A
 * range covers them and they are committed: they can't move. */
static void range_addrs(struct vmo *v, uint64_t first, uint64_t n, uint64_t *pa)
{
    uint64_t f = vlock(v);
    for (uint64_t i = 0; i < n; i++) {
        pa[i] = phys_locked(v, first + i);
        ASSERT(pa[i] != 0 || v->kind != VMO_PAGED);
    }
    vunlock(v, f);
}

/* r's pages out of dom: in one call (one invalidation) when the heap
 * gives a buffer for their addresses, else UNMAP_STACK_PAGES at a time. */
static status_t pin_unmap(struct vmo *v, const struct vmo_range *r, struct iommu_domain *dom)
{
    uint64_t n = r->end - r->first;
    uint64_t small[UNMAP_STACK_PAGES];
    uint64_t *pa = n > UNMAP_STACK_PAGES ? kmalloc(n * sizeof(uint64_t)) : NULL;
    uint64_t chunk = pa ? n : UNMAP_STACK_PAGES;
    if (!pa)
        pa = small;
    status_t st = OK;
    for (uint64_t i = 0; i < n && st == OK; i += chunk) {
        uint64_t k = n - i < chunk ? n - i : chunk;
        range_addrs(v, r->first + i, k, pa);
        st = iommu_unmap(dom, pa, k);
    }
    if (pa != small)
        kfree(pa);
    return st;
}

/* The unit didn't confirm that the device lost r's pages (why): keep them
 * for good. r stays on v, busy (nobody finds it; decommit and shrink
 * refuse its pages), holding v and its charge; only its cap lets go of
 * it, so the cap can still close. */
static void pin_keep(struct vmo *v, struct vmo_range *r, status_t why)
{
    uint64_t f = spin_lock_irqsave(&r->cap->lock);
    if (r->cap_linked) {
        list_del(&r->cap_node);
        r->cap_linked = false;
    }
    spin_unlock_irqrestore(&r->cap->lock, f);
    kobject_unref(r->cap);
    r->cap = NULL;
    report("vmo: koid %lu: pages %lu-%lu stay pinned for good: the IOMMU did not confirm that "
           "the device lost them (%d)", v->base.koid, r->first, r->end - 1, why);
}

/* Undo a pin (busy, so nobody else can find it): out of dom (if any),
 * then its pages may go. Returns the unmap's result: on an error the
 * pages are kept (pin_keep). */
static status_t pin_drop(struct vmo *v, struct vmo_range *r, struct iommu_domain *dom)
{
    status_t st = dom ? pin_unmap(v, r, dom) : OK;
    if (st != OK)
        pin_keep(v, r, st);
    else
        range_remove(v, r);
    return st;
}

/* The pin takes a reference on the cap and goes on the cap's list from
 * the start (busy until published), unless its last handle is already
 * gone (ERR_BAD_STATE). */
static status_t pin_link_cap(struct vmo_range *r, struct kobject *dma_cap)
{
    struct dma_cap *c = dma_cap_from_kobject(dma_cap);
    status_t st = OK;
    kobject_ref(dma_cap);
    r->cap = dma_cap;
    uint64_t cf = spin_lock_irqsave(&dma_cap->lock);
    if (c->closed) {
        st = ERR_BAD_STATE;
    } else {
        list_add_tail(&c->pins, &r->cap_node);
        r->cap_linked = true;
    }
    spin_unlock_irqrestore(&dma_cap->lock, cf);
    return st;
}

/* Publish the pin, unless the cap was closed meanwhile: its close path ran
 * (or is running) and skipped this busy pin, so the pin goes here (out of
 * dom first: the domain lives until every such pin is gone). */
static status_t pin_publish(struct vmo *v, struct vmo_range *r, struct kobject *dma_cap,
                            struct iommu_domain *dom, uint64_t *pin_id)
{
    struct dma_cap *c = dma_cap_from_kobject(dma_cap);
    uint64_t cf = spin_lock_irqsave(&dma_cap->lock);
    if (c->closed) {
        spin_unlock_irqrestore(&dma_cap->lock, cf);
        (void)pin_drop(v, r, dom);   /* on an error its pages are kept: nothing more to do */
        return ERR_BAD_STATE;
    }
    uint64_t f = vlock(v);
    r->busy = false;
    *pin_id = r->key;
    vunlock(v, f);
    spin_unlock_irqrestore(&dma_cap->lock, cf);
    return OK;
}

/* A read-only pin takes only pages already committed, and commits none
 * (ERR_BAD_STATE): it may be made with no more than RIGHT_READ, which
 * never allowed a commit, and a driver handed a read-only view of a large
 * sparse VMO must not fill it at its holder's expense (review M12.1 KDEV
 * finding 4, KCORE finding 6; test: dma_read_only_pin_needs_committed).
 * Its range, listed, keeps them committed from here on (decommit and
 * shrink refuse its pages). */
static status_t pin_committed(struct vmo *v, uint64_t first, uint64_t end)
{
    status_t st = OK;
    uint64_t f = vlock(v);
    for (uint64_t idx = first; idx < end && st == OK; idx++)
        if (!phys_locked(v, idx))
            st = ERR_BAD_STATE;
    vunlock(v, f);
    return st;
}

status_t vmo_pin_flags(struct vmo *v, struct kobject *dma_cap, uint64_t offset, uint64_t len,
                       uint32_t flags, uint64_t *phys_out, uint64_t phys_cap, uint64_t *pin_id)
{
    if (v->pv || v->src)
        return ERR_NOT_SUPPORTED;   /* a device can't wait for a pager */
    if (!dma_cap || !pin_id || (flags & ~VMO_PIN_READ_ONLY))
        return ERR_INVALID_ARGS;
    if (dma_cap->type != OBJ_DMA_CAP)
        return ERR_WRONG_TYPE;
    if (len == 0 || ((offset | len) & (PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    if (offset > VMO_MAX_SIZE || len > VMO_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    uint64_t first = offset >> PAGE_SHIFT, end = (offset + len) >> PAGE_SHIFT;
    if (!phys_out || phys_cap < end - first)
        return ERR_BUFFER_TOO_SMALL;
    /* A cap bound to a function pins only while its Bus Master Enable is
     * on, and while it is the function's current cap (its driver turned
     * bus mastering on with it: dma_cap_bus_master). */
    if (!dma_cap_bus_master_on(dma_cap))
        return ERR_BAD_STATE;

    /* The cap's domain, if it has one, is fixed from its creation, and
     * stays until this pin (on the cap's list) is gone. A read-only pin
     * is read only only where the domain makes it so: without one the
     * device can write whatever it reaches. */
    struct dma_cap *c = dma_cap_from_kobject(dma_cap);
    struct iommu_domain *dom = c->dom;
    bool ro = (flags & VMO_PIN_READ_ONLY) != 0;
    struct vmo_range *r;
    status_t st = pin_range_new(v, c->job, first, end, !(ro && dom), &r);
    if (st != OK)
        return st;
    st = pin_link_cap(r, dma_cap);
    /* Either kind separates every shared page of the range (a read-only
     * pin commits none: they are all there): the device must reach the
     * VMO's own page, which a later write would otherwise move. */
    if (st == OK && v->kind == VMO_PAGED && ro)
        st = pin_committed(v, first, end);
    if (st == OK && v->kind == VMO_PAGED)
        st = commit_pages(v, first, end, GET_PRIVATE);
    if (st != OK) {
        range_remove(v, r);
        return st;
    }
    range_addrs(v, first, end - first, phys_out);
    if (dom && (st = ro ? iommu_map_ro(dom, phys_out, end - first)
                        : iommu_map(dom, phys_out, end - first)) != OK) {
        /* All or nothing; but a failed invalidation leaves the unit's
         * caches unknown: then the pages are kept. */
        if (st == ERR_TIMED_OUT || st == ERR_IO)
            pin_keep(v, r, st);
        else
            range_remove(v, r);
        return st;
    }
    return pin_publish(v, r, dma_cap, dom, pin_id);
}

/* The last pin may hold the cap's last reference; this runs from the cap's
 * on_zero_handles (inside the object teardown drainer), so its destroy is
 * queued behind us and c stays valid until we return. (It may already have
 * no references at all: taking one here would be a bug.) */
void vmo_release_cap_pins(struct dma_cap *c)
{
    for (;;) {
        struct vmo_range *r = NULL;
        uint64_t cf = spin_lock_irqsave(&c->base.lock);
        for (struct list_node *n = c->pins.next; n != &c->pins && !r; n = n->next) {
            struct vmo_range *x = container_of(n, struct vmo_range, cap_node);
            /* x is on the list, so it (and its VMO reference) is alive. */
            uint64_t f = vlock(x->v);
            if (!x->busy) {
                x->busy = true;   /* ours now: vmo_unpin can't find it */
                r = x;
            }
            vunlock(x->v, f);
        }
        spin_unlock_irqrestore(&c->base.lock, cf);
        if (!r)
            break;
        range_remove(r->v, r);
    }
}

/* ---- DMA quarantine -----------------------------------------------------------
 * A bound dma_cap whose last handle closes while pins are still held (its
 * driver died, or quit without unpinning) doesn't give the pages back:
 * the device may still hold their addresses in a queued transfer, and a
 * later owner turning Bus Master Enable on would let it write into pages
 * that belong to someone else by then. dma_cap.c keeps them on the
 * function's quarantine (Fuchsia's BTI quarantine) and releases them once
 * the next owner has turned bus mastering on and a grace period passed,
 * or after a timeout. The pages stay charged to their VMO's job until
 * then: they are not free. */

#define SUM_PAGES_MAX 1024   /* pages checksummed per quarantined pin (4 MiB) */

static uint64_t page_sum(uint64_t pa)
{
    const uint64_t *w = phys_to_virt(pa);
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < PAGE_SIZE / 8; i++)
        h = (h ^ w[i]) * 0x100000001b3ull;
    return h;
}

void vmo_quarantine_cap_pins(struct dma_cap *c, struct list_node *out, uint64_t *pins,
                             uint64_t *pages)
{
    for (;;) {
        struct vmo_range *r = NULL;
        uint64_t cf = spin_lock_irqsave(&c->base.lock);
        for (struct list_node *n = c->pins.next; n != &c->pins && !r; n = n->next) {
            struct vmo_range *x = container_of(n, struct vmo_range, cap_node);
            uint64_t f = vlock(x->v);
            if (!x->busy) {
                x->busy = true;   /* ours now: vmo_unpin can't find it */
                r = x;
            }
            vunlock(x->v, f);
        }
        if (r) {
            list_del(&r->cap_node);
            r->cap_linked = false;
        }
        spin_unlock_irqrestore(&c->base.lock, cf);
        if (!r)
            break;
        struct vmo *v = r->v;
        if (v->kind == VMO_PHYS) {
            range_remove(v, r);   /* no RAM behind it: nothing to protect */
            continue;
        }
        /* The cap may go now (its destroy is queued behind us, see
         * vmo_release_cap_pins); the range needs nothing more of it. */
        kobject_unref(r->cap);
        r->cap = NULL;
        uint64_t n = r->end - r->first;
        if (n <= SUM_PAGES_MAX && (r->sums = kmalloc(n * sizeof(uint64_t))))
            for (uint64_t i = 0; i < n; i++) {
                uint64_t f = vlock(v);
                uint64_t pa = phys_locked(v, r->first + i);
                vunlock(v, f);
                r->sums[i] = page_sum(pa);   /* pinned: the page can't move */
            }
        list_add_tail(out, &r->cap_node);
        (*pins)++;
        *pages += n;
    }
}

void vmo_release_quarantined(struct list_node *list, uint64_t *pages, uint64_t *changed)
{
    while (!list_empty(list)) {
        struct vmo_range *r = container_of(list->next, struct vmo_range, cap_node);
        list_del(&r->cap_node);
        struct vmo *v = r->v;
        uint64_t n = r->end - r->first;
        if (r->sums) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t f = vlock(v);
                uint64_t pa = phys_locked(v, r->first + i);
                vunlock(v, f);
                *changed += page_sum(pa) != r->sums[i];
            }
            kfree(r->sums);
            r->sums = NULL;
        }
        *pages += n;
        range_remove(v, r);   /* like an unpin: the pages may go back now */
    }
}

status_t vmo_unpin(struct vmo *v, struct kobject *dma_cap, uint64_t pin_id)
{
    uint64_t f = vlock(v);
    struct vmo_range *r = range_find_locked(v, RANGE_PIN, pin_id);
    status_t st = !r ? ERR_NOT_FOUND : r->cap != dma_cap ? ERR_ACCESS_DENIED : OK;
    if (st == OK)
        r->busy = true;
    vunlock(v, f);
    if (st != OK)
        return st;
    return pin_drop(v, r, dma_cap_from_kobject(dma_cap)->dom);
}
