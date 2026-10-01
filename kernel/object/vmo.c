/* Virtual memory objects (see vmo.h).
 *
 * Page tracking (paged VMOs): a fixed three-level table indexed by page
 * number, root[64] (inline) -> mid[512] -> leaf[512] of physical addresses,
 * 0 meaning "not committed". A leaf covers 2 MiB, a mid table 1 GiB, the
 * root all 64 GiB. Table pages come straight from the buddy allocator on
 * first touch; shrink and destroy free them (decommit leaves them).
 *
 * Locking: the kobject's lock (class "vmo", taken with interrupts off like
 * every object lock) guards the size, the table, the committed count, the
 * range list and the reverse map. The buddy allocator ranks after it:
 * table pages are allocated with it held. Address spaces rank around it
 * (aspace_vmo.h): an address space's region lock (a mutex) is above it,
 * its page-table lock below it. The kernel page table lock is never taken
 * under it, and data is never copied under it. vmo_set_size is serialised
 * by a mutex ("vmo resize") above everything else.
 *
 * User mappings: each address-space mapping of a VMO is a struct
 * vmo_umap on v->umaps and holds a VMO reference. Unlike kernel mappings
 * and pins, they don't block decommit or shrink: those take the pages out
 * of the table, clear their page-table entries in every user mapping (under
 * this lock, so a racing fault either installed the old page first and
 * sees it zapped, or finds the slot empty and commits a new one), and free
 * the pages only after the TLB shootdown (a tlb_gather). mm/aspace.c's
 * header has the full argument.
 *
 * Latency: decommit and shrink work one leaf table (2 MiB,
 * at most 512 pages) per lock hold and flush/free every 512 pages or so,
 * re-checking under each re-lock: the range is clipped to the current size
 * (a racing shrink already took the rest), and a pin or kernel mapping that
 * appeared since the call started fails it with ERR_BAD_STATE, leaving the
 * pages before it decommitted. A commit racing the decommit of a slot it
 * already passed simply stays: it is ordered after the decommit. Shrink
 * lowers the size first, so nothing new can appear past the new end while
 * it works, and holds "vmo resize" so no grow can reopen that range.
 *
 * Page lifetime: a committed page's struct page refcount is 1 for the
 * table's reference. vmo_read/vmo_write take an extra reference under the
 * lock and copy with it dropped, so a racing decommit or shrink can't free
 * a page mid-copy: whoever drops the last reference frees it. Contiguous
 * and physical VMOs never give pages up before destroy, so they skip this.
 *
 * Job charges: a VMO made for a process (vmo_set_job) charges one
 * JOB_LIMIT_PAGES unit to that job for every page it owns, from the moment
 * the page is published in the table (under the lock, so a failed charge
 * just drops the fresh page: ERR_NO_MEMORY) until it leaves the table;
 * `committed` and the charge move together. Its own table pages (mid and
 * leaf, `tables`) are charged the same way, one unit each from creation to
 * free (were they free, a job allowed 0 pages could make the kernel allocate
 * 128 MiB of leaf tables for one 64 GiB VMO). The page is
 * charged BEFORE any table is made for it, so a refused commit builds
 * nothing; if a table it needs is refused, the page's charge is undone.
 * Tables stay (charged) until shrink or destroy frees them, as before
 * (decommit leaves them). Contiguous VMOs are charged whole when the job is
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
 * and unmap/unpin ignore busy ranges. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/vmo.h>

#define VMO_MAX_PAGES (VMO_MAX_SIZE >> PAGE_SHIFT)
#define TBL_ENTRIES   512u
#define LEAF_PAGES    ((uint64_t)TBL_ENTRIES)            /* pages per leaf: 2 MiB */
#define MID_PAGES     (LEAF_PAGES * TBL_ENTRIES)         /* pages per mid table: 1 GiB */
#define ROOT_ENTRIES  (VMO_MAX_PAGES / MID_PAGES)        /* 64 */
#define CONTIG_MAX    (PAGE_SIZE << MAX_ORDER)           /* 4 MiB */

_Static_assert(TBL_ENTRIES * sizeof(uint64_t) == PAGE_SIZE, "a table is one page");

enum vmo_kind { VMO_PAGED, VMO_CONTIG, VMO_PHYS };
enum range_kind { RANGE_MAP, RANGE_PIN };

struct vmo_range {
    struct list_node node;        /* on the VMO's ranges list (VMO lock) */
    enum range_kind  kind;        /* a kernel mapping or a pin */
    bool             busy;        /* being set up or unmapped: not findable */
    uint64_t         first, end;  /* page indices */
    uint64_t         key;         /* mapping: base va; pin: pin id */
    struct kobject  *cap;         /* pin: the DMA capability (referenced) */
    /* A pin is also on its cap's list (struct dma_cap.pins, under the
     * cap's lock), so closing the cap can find and release it. */
    struct vmo      *v;           /* pin: the VMO it pins */
    struct list_node cap_node;
    bool             cap_linked;  /* on the cap's list (cap's lock) */
    struct job      *charged;     /* pin: charged one JOB_LIMIT_HANDLES unit (the VMO's job) */
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
    uint64_t         tables;              /* paged: table pages (mid + leaf), charged like pages */
    struct job      *job;                 /* charged for them (a reference), or NULL */
    uint64_t         phys;                /* contiguous / physical: first byte */
    unsigned         order;               /* contiguous: buddy order it came from */
    uint64_t         next_pin_id;         /* the next pin's id */
    struct list_node ranges;              /* struct vmo_range: kernel mappings and pins */
    struct list_node umaps;               /* struct vmo_umap: user mappings (reverse map) */
    struct mutex     resize;              /* serialises vmo_set_size */
    uint64_t       **root[ROOT_ENTRIES];  /* paged: root[r][m][l] = phys */
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

/* With the lock held: the table slot for page idx. NULL if its tables don't
 * exist and `create` is false, or creating them failed (the job refused a
 * table page, or no memory; any table made before the failure stays, and
 * stays charged). */
static uint64_t *slot_locked(struct vmo *v, uint64_t idx, bool create)
{
    uint64_t ***mid = &v->root[idx / MID_PAGES];
    if (!*mid && (!create || !(*mid = table_alloc_locked(v))))
        return NULL;
    uint64_t **leaf = &(*mid)[(idx / LEAF_PAGES) % TBL_ENTRIES];
    if (!*leaf && (!create || !(*leaf = table_alloc_locked(v))))
        return NULL;
    return &(*leaf)[idx % LEAF_PAGES];
}

/* With the lock held and page idx not committed (*s is its slot if its
 * tables exist, else NULL): charge the page to v's job, then make its
 * tables if needed. Returns the slot to publish the page in, or NULL with
 * nothing charged for the page (ERR_NO_MEMORY). */
static uint64_t *charge_slot_locked(struct vmo *v, uint64_t idx, uint64_t *s)
{
    if (job_charge(v->job, JOB_LIMIT_PAGES, 1) != OK)
        return NULL;   /* refused: no table gets built for it */
    if (!s && !(s = slot_locked(v, idx, true))) {
        job_uncharge(v->job, JOB_LIMIT_PAGES, 1);
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

/* drop_from_locked's work on one leaf table, whose first entry is page
 * `base`: drop its pages at index >= first. */
static void drop_leaf_from_locked(struct vmo *v, uint64_t *leaf, uint64_t base, uint64_t first)
{
    for (uint64_t l = 0; l < LEAF_PAGES; l++) {
        if (leaf[l] && base + l >= first) {
            page_put(pa_page(leaf[l]));
            leaf[l] = 0;
            v->committed--;
            job_uncharge(v->job, JOB_LIMIT_PAGES, 1);
        }
    }
}

/* Drop every page at index >= first, and free the table pages that lie
 * wholly past it, with no TLB care: for vmo_destroy (no references, so no
 * mappings and no lock needed) and the tail of a shrink (which has already
 * taken every page past `first` out through a gather, so only tables are
 * left to free). */
static void drop_from_locked(struct vmo *v, uint64_t first)
{
    for (uint64_t r = first / MID_PAGES; r < ROOT_ENTRIES; r++) {
        uint64_t **mid = v->root[r];
        if (!mid)
            continue;
        for (uint64_t m = 0; m < TBL_ENTRIES; m++) {
            uint64_t *leaf = mid[m];
            uint64_t base = r * MID_PAGES + m * LEAF_PAGES;
            if (!leaf || base + LEAF_PAGES <= first)
                continue;
            drop_leaf_from_locked(v, leaf, base, first);
            if (base >= first) {
                table_free_locked(v, leaf);
                mid[m] = NULL;
            }
        }
        if (r * MID_PAGES >= first) {
            table_free_locked(v, mid);
            v->root[r] = NULL;
        }
    }
}

/* Paged VMOs: page idx with an extra reference (page_put it), committing
 * it first if `commit`. *out = NULL if it isn't committed and commit is
 * false. ERR_OUT_OF_RANGE if idx is past the end (a racing shrink). */
static status_t get_page(struct vmo *v, uint64_t idx, bool commit, struct page **out)
{
    struct page *fresh = NULL;
    for (;;) {
        uint64_t f = vlock(v);
        if (idx >= v->size >> PAGE_SHIFT) {
            vunlock(v, f);
            if (fresh)
                page_put(fresh);
            return ERR_OUT_OF_RANGE;
        }
        uint64_t *s = slot_locked(v, idx, false);
        if (s && *s) {
            struct page *p = pa_page(*s);
            page_get(p);
            vunlock(v, f);
            if (fresh)
                page_put(fresh);   /* another CPU committed it first */
            *out = p;
            return OK;
        }
        if (fresh) {
            if (!(s = charge_slot_locked(v, idx, s))) {
                vunlock(v, f);
                page_put(fresh);
                return ERR_NO_MEMORY;
            }
            *s = page_to_phys(fresh);
            v->committed++;
            page_get(fresh);   /* the table's reference plus the caller's */
            vunlock(v, f);
            *out = fresh;
            return OK;
        }
        vunlock(v, f);
        if (!commit) {
            *out = NULL;
            return OK;
        }
        fresh = pmm_alloc_pages(0, PMM_ZERO | ((v->flags & VMO_DMA32) ? PMM_DMA32 : 0));
        if (!fresh)
            return ERR_NO_MEMORY;
    }
}

/* Commit pages [first, end) of a paged VMO. */
static status_t commit_pages(struct vmo *v, uint64_t first, uint64_t end)
{
    for (uint64_t idx = first; idx < end; idx++) {
        struct page *p;
        status_t st = get_page(v, idx, true, &p);
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
    if (!list_empty(&v->ranges) || !list_empty(&v->umaps))
        panic("vmo: koid %lu destroyed while mapped or pinned", obj->koid);
    if (v->kind == VMO_PAGED) {
        drop_from_locked(v, 0);
        ASSERT(v->committed == 0 && v->tables == 0);
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
    list_init(&v->umaps);
    mutex_init(&v->resize, "vmo resize");
    return v;
}

status_t vmo_create(uint64_t size, uint32_t flags, struct vmo **out)
{
    if (flags & ~VMO_CREATE_FLAGS)
        return ERR_INVALID_ARGS;
    if (size > VMO_MAX_SIZE)
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

status_t vmo_create_physical(uint64_t phys, uint64_t size, unsigned cache, struct vmo **out)
{
    if ((phys & (PAGE_SIZE - 1)) || size == 0 ||
        (cache != 0 && cache != VM_UC && cache != VM_WC))
        return ERR_INVALID_ARGS;
    if (size > VMO_MAX_SIZE)
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
    while (len) {
        uint64_t in = offset & (PAGE_SIZE - 1);
        uint64_t n = PAGE_SIZE - in < len ? PAGE_SIZE - in : len;
        struct page *p;
        status_t st = get_page(v, offset >> PAGE_SHIFT, false, &p);
        if (st != OK)
            return st;
        if (p) {
            memcpy(dst, (char *)page_to_virt(p) + in, n);
            page_put(p);
        } else {
            memset(dst, 0, n);
        }
        dst += n;
        offset += n;
        len -= n;
    }
    return OK;
}

status_t vmo_write(struct vmo *v, uint64_t offset, const void *buf, uint64_t len)
{
    if (v->kind == VMO_PHYS)
        return ERR_NOT_SUPPORTED;
    if (!in_bounds(offset, len, vmo_size(v)))
        return ERR_OUT_OF_RANGE;
    if (v->kind == VMO_CONTIG) {
        memcpy((char *)phys_to_virt(v->phys) + offset, buf, len);
        return OK;
    }
    const char *src = buf;
    while (len) {
        uint64_t in = offset & (PAGE_SIZE - 1);
        uint64_t n = PAGE_SIZE - in < len ? PAGE_SIZE - in : len;
        struct page *p;
        status_t st = get_page(v, offset >> PAGE_SHIFT, true, &p);
        if (st != OK)
            return st;
        memcpy((char *)page_to_virt(p) + in, src, n);
        page_put(p);
        src += n;
        offset += n;
        len -= n;
    }
    return OK;
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

/* With the lock held: zap pages [first, end) from every user mapping. */
static void zap_umaps_locked(const struct vmo *v, uint64_t first, uint64_t end,
                             struct tlb_gather *g)
{
    for (struct list_node *n = v->umaps.next; n != &v->umaps; n = n->next) {
        struct vmo_umap *u = container_of(n, struct vmo_umap, node);
        uint64_t lo = u->first > first ? u->first : first;
        uint64_t hi = u->end < end ? u->end : end;
        if (lo < hi)
            aspace_zap_locked(u->as, u->base + ((lo - u->first) << PAGE_SHIFT),
                              (hi - lo) << PAGE_SHIFT, g);
    }
}

/* With the lock held: take the committed pages in [idx, end), at most up to
 * the end of idx's leaf, out of the table onto g and out of every user
 * mapping. Returns where to continue (past a missing leaf or mid table). */
static uint64_t take_batch_locked(struct vmo *v, uint64_t idx, uint64_t end, struct tlb_gather *g)
{
    uint64_t **mid = v->root[idx / MID_PAGES];
    if (!mid) {
        uint64_t next = ALIGN_UP(idx + 1, MID_PAGES);
        return next < end ? next : end;
    }
    uint64_t stop = ALIGN_UP(idx + 1, LEAF_PAGES);
    if (stop > end)
        stop = end;
    uint64_t *leaf = mid[(idx / LEAF_PAGES) % TBL_ENTRIES];
    if (!leaf)
        return stop;
    uint64_t taken = 0;
    for (uint64_t i = idx; i < stop; i++) {
        uint64_t *s = &leaf[i % LEAF_PAGES];
        if (*s) {
            tlb_gather_page(g, pa_page(*s));   /* the table's reference, released after the flush */
            *s = 0;
            v->committed--;
            taken++;
        }
    }
    job_uncharge(v->job, JOB_LIMIT_PAGES, taken);
    bool any = taken != 0;
    /* Entries exist only for committed pages (a fault installs under this
     * lock what the table holds), so nothing taken means nothing to zap. */
    if (any)
        zap_umaps_locked(v, idx, stop, g);
    return stop;
}

/* Flush and free once a batch's worth of pages is waiting. */
static void gather_maybe_finish(struct tlb_gather *g)
{
    if (g->npages >= LEAF_PAGES)
        tlb_gather_finish(g);
}

/* Shrink from old_end pages to first pages; v->size is already lowered and
 * v->resize held, so nothing can appear at or past `first` meanwhile. */
static void shrink_pages(struct vmo *v, uint64_t first, uint64_t old_end)
{
    struct tlb_gather g;
    tlb_gather_init(&g);
    for (uint64_t idx = first; idx < old_end;) {
        uint64_t f = vlock(v);
        uint64_t next = take_batch_locked(v, idx, old_end, &g);
        /* A leaf wholly past the new end goes too (its pages just did). */
        uint64_t lbase = ALIGN_DOWN(idx, LEAF_PAGES);
        uint64_t **mid = v->root[idx / MID_PAGES];
        if (lbase >= first && mid && mid[(idx / LEAF_PAGES) % TBL_ENTRIES]) {
            table_free_locked(v, mid[(idx / LEAF_PAGES) % TBL_ENTRIES]);
            mid[(idx / LEAF_PAGES) % TBL_ENTRIES] = NULL;
        }
        vunlock(v, f);
        gather_maybe_finish(&g);
        idx = next;
    }
    uint64_t f = vlock(v);
    drop_from_locked(v, first);   /* only mid tables are left to free */
    vunlock(v, f);
    tlb_gather_finish(&g);
}

status_t vmo_set_size(struct vmo *v, uint64_t size)
{
    if (v->kind != VMO_PAGED)
        return ERR_NOT_SUPPORTED;
    if (size > VMO_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    size = ALIGN_UP(size, PAGE_SIZE);
    mutex_lock(&v->resize);
    uint64_t f = vlock(v);
    uint64_t first = size >> PAGE_SHIFT, end = v->size >> PAGE_SHIFT;
    if (first < end && ranges_overlap_locked(v, first, end)) {
        vunlock(v, f);
        mutex_unlock(&v->resize);
        return ERR_BAD_STATE;
    }
    __atomic_store_n(&v->size, size, __ATOMIC_RELAXED);
    vunlock(v, f);
    if (first < end)
        shrink_pages(v, first, end);
    mutex_unlock(&v->resize);
    return OK;
}

status_t vmo_commit(struct vmo *v, uint64_t offset, uint64_t len)
{
    if (!in_bounds(offset, len, vmo_size(v)))
        return ERR_OUT_OF_RANGE;
    if (v->kind != VMO_PAGED || len == 0)
        return OK;   /* contiguous and physical VMOs are always committed */
    return commit_pages(v, offset >> PAGE_SHIFT, ALIGN_UP(offset + len, PAGE_SIZE) >> PAGE_SHIFT);
}

status_t vmo_decommit(struct vmo *v, uint64_t offset, uint64_t len)
{
    if (v->kind != VMO_PAGED)
        return ERR_NOT_SUPPORTED;
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
        idx = take_batch_locked(v, idx, stop, &g);
        vunlock(v, f);
        gather_maybe_finish(&g);
    }
    tlb_gather_finish(&g);
    return st;
}

bool vmo_unmapped_paged(struct vmo *v)
{
    uint64_t f = vlock(v);
    bool ok = v->kind == VMO_PAGED && list_empty(&v->umaps) && list_empty(&v->ranges);
    vunlock(v, f);
    return ok;
}

/* ---- user mappings (reverse map, see aspace_vmo.h) ------------------------ */

status_t vmo_umap_add(struct vmo *v, struct vmo_umap *u, bool check)
{
    uint64_t f = vlock(v);
    if (check && u->end > v->size >> PAGE_SHIFT) {
        vunlock(v, f);
        return ERR_OUT_OF_RANGE;
    }
    list_add_tail(&v->umaps, &u->node);
    vunlock(v, f);
    kobject_ref(&v->base);
    return OK;
}

void vmo_umap_set(struct vmo *v, struct vmo_umap *u, uint64_t base, uint64_t first, uint64_t end)
{
    uint64_t f = vlock(v);
    u->base = base;
    u->first = first;
    u->end = end;
    vunlock(v, f);
}

void vmo_umap_remove(struct vmo *v, struct vmo_umap *u)
{
    uint64_t f = vlock(v);
    list_del(&u->node);
    vunlock(v, f);
    kobject_unref(&v->base);
}

/* VMO lock held, a paged VMO: *pa gets page idx's physical address. If the
 * page isn't committed, *fresh (if any) is committed there and taken
 * (*fresh = NULL); with no fresh page *pa is 0. ERR_NO_MEMORY if the page's
 * table can't be charged (*fresh is still the caller's). */
static status_t paged_pa_locked(struct vmo *v, uint64_t idx, struct page **fresh, uint64_t *pa)
{
    uint64_t *s = slot_locked(v, idx, false);
    if (s && *s) {
        *pa = *s;   /* committed already (maybe by a racing fault: drop ours) */
        return OK;
    }
    if (!*fresh) {
        *pa = 0;
        return OK;
    }
    if (!(s = charge_slot_locked(v, idx, s)))
        return ERR_NO_MEMORY;
    *pa = *s = page_to_phys(*fresh);   /* the table takes our reference */
    v->committed++;
    *fresh = NULL;
    return OK;
}

status_t vmo_fault_map(struct vmo *v, uint64_t idx, struct aspace *as, uint64_t va,
                       unsigned perms)
{
    struct page *fresh = NULL;
    for (;;) {
        uint64_t f = vlock(v);
        if (idx >= v->size >> PAGE_SHIFT) {
            vunlock(v, f);
            if (fresh)
                page_put(fresh);
            return ERR_OUT_OF_RANGE;
        }
        uint64_t pa;
        if (v->kind != VMO_PAGED) {
            pa = v->phys + (idx << PAGE_SHIFT);
        } else {
            status_t st = paged_pa_locked(v, idx, &fresh, &pa);
            if (st != OK) {
                vunlock(v, f);
                page_put(fresh);
                return st;
            }
        }
        if (v->kind == VMO_PAGED && !pa) {
            /* Not committed and no page of ours yet: get one, then retry. */
            vunlock(v, f);
            fresh = pmm_alloc_pages(0, PMM_ZERO | ((v->flags & VMO_DMA32) ? PMM_DMA32 : 0));
            if (!fresh)
                return ERR_NO_MEMORY;
            continue;
        }
        /* Installed under the VMO lock: a decommit that takes this page
         * later zaps this entry too (see the file header). */
        aspace_set_pte_locked(as, va, pa, perms, v->cache);
        vunlock(v, f);
        if (fresh)
            page_put(fresh);
        return OK;
    }
}

uint64_t vmo_page_phys(struct vmo *v, uint64_t offset)
{
    uint64_t f = vlock(v);
    uint64_t idx = offset >> PAGE_SHIFT;
    uint64_t pa = idx < v->size >> PAGE_SHIFT ? phys_locked(v, idx) : 0;
    vunlock(v, f);
    return pa;
}

/* ---- mapping and pinning ranges ----------------------------------------- */

/* Record r (busy) if it lies inside the VMO. On success it holds a VMO
 * reference and, for pins, gets its id. */
static status_t range_add(struct vmo *v, struct vmo_range *r)
{
    uint64_t f = vlock(v);
    if (r->end > v->size >> PAGE_SHIFT) {
        vunlock(v, f);
        return ERR_OUT_OF_RANGE;
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
    job_uncharge(r->charged, JOB_LIMIT_HANDLES, 1);   /* (the VMO still holds the job) */
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
    if (len == 0 || (vm_flags & ~VM_WRITE))
        return ERR_INVALID_ARGS;
    if (offset > VMO_MAX_SIZE || len > VMO_MAX_SIZE)
        return ERR_OUT_OF_RANGE;   /* (and offset + len can't overflow below) */
    struct vmo_range *r = kzalloc(sizeof(*r));
    if (!r)
        return ERR_NO_MEMORY;
    r->kind = RANGE_MAP;
    r->first = offset >> PAGE_SHIFT;
    r->end = ALIGN_UP(offset + len, PAGE_SIZE) >> PAGE_SHIFT;
    status_t st = range_add(v, r);
    if (st != OK) {
        kfree(r);
        return st;
    }
    if (v->kind == VMO_PAGED && (st = commit_pages(v, r->first, r->end)) != OK) {
        range_remove(v, r);
        return st;
    }

    /* The range keeps every page in place, so the addresses looked up here
     * stay valid; map them in physically contiguous runs. */
    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t base = vmm_reserve((r->end - r->first) << PAGE_SHIFT);
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
    vmm_unmap(vmm_kernel_pml4(), base, (r->end - r->first) << PAGE_SHIFT);
    range_remove(v, r);
    return OK;
}

/* A pin is a kernel allocation that lives until unpin or the cap's
 * close: one handle unit of the VMO's job, like any small object (a
 * driver could otherwise pin one page forever and fill the kernel
 * heap; test: m6r_pins_are_charged). A new busy pin range [first, end)
 * on v, charged and listed. */
static status_t pin_range_new(struct vmo *v, uint64_t first, uint64_t end,
                              struct vmo_range **out)
{
    uint64_t jf = vlock(v);
    struct job *job = v->job;
    vunlock(v, jf);
    status_t st = job_charge(job, JOB_LIMIT_HANDLES, 1);
    if (st != OK)
        return st;
    struct vmo_range *r = kzalloc(sizeof(*r));
    if (!r) {
        job_uncharge(job, JOB_LIMIT_HANDLES, 1);
        return ERR_NO_MEMORY;
    }
    r->kind = RANGE_PIN;
    r->first = first;
    r->end = end;
    r->v = v;
    r->charged = job;
    st = range_add(v, r);
    if (st != OK) {
        job_uncharge(job, JOB_LIMIT_HANDLES, 1);
        kfree(r);
        return st;
    }
    *out = r;
    return OK;
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
 * (or is running) and skipped this busy pin, so the pin goes here. */
static status_t pin_publish(struct vmo *v, struct vmo_range *r, struct kobject *dma_cap,
                            uint64_t *phys_out, uint64_t *pin_id)
{
    struct dma_cap *c = dma_cap_from_kobject(dma_cap);
    uint64_t cf = spin_lock_irqsave(&dma_cap->lock);
    if (c->closed) {
        spin_unlock_irqrestore(&dma_cap->lock, cf);
        range_remove(v, r);
        return ERR_BAD_STATE;
    }
    uint64_t f = vlock(v);
    for (uint64_t idx = r->first; idx < r->end; idx++) {
        phys_out[idx - r->first] = phys_locked(v, idx);
        ASSERT(phys_out[idx - r->first] != 0 || v->kind != VMO_PAGED);
    }
    r->busy = false;
    *pin_id = r->key;
    vunlock(v, f);
    spin_unlock_irqrestore(&dma_cap->lock, cf);
    return OK;
}

status_t vmo_pin(struct vmo *v, struct kobject *dma_cap, uint64_t offset, uint64_t len,
                 uint64_t *phys_out, uint64_t phys_cap, uint64_t *pin_id)
{
    if (!dma_cap || !pin_id)
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

    struct vmo_range *r;
    status_t st = pin_range_new(v, first, end, &r);
    if (st != OK)
        return st;
    st = pin_link_cap(r, dma_cap);
    if (st == OK && v->kind == VMO_PAGED)
        st = commit_pages(v, first, end);
    if (st != OK) {
        range_remove(v, r);
        return st;
    }
    return pin_publish(v, r, dma_cap, phys_out, pin_id);
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
    range_remove(v, r);
    return OK;
}
