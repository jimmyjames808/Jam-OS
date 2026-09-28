/* Virtual memory objects (see vmo.h).
 *
 * Page tracking (paged VMOs): a fixed three-level table indexed by page
 * number, root[64] (inline) -> mid[512] -> leaf[512] of physical addresses,
 * 0 meaning "not committed". A leaf covers 2 MiB, a mid table 1 GiB, the
 * root all 64 GiB. Table pages come straight from the buddy allocator on
 * first touch; shrink and destroy free them (decommit leaves them).
 *
 * Locking: the kobject's lock (class "vmo", taken with interrupts off like
 * every object lock) guards the size, the table, the committed count and
 * the range list. The buddy allocator ranks after it: table pages are
 * allocated, and decommitted pages freed, with it held. The kernel page
 * table lock is never taken under it, and data is never copied under it.
 *
 * Page lifetime: a committed page's struct page refcount is 1 for the
 * table's reference. vmo_read/vmo_write take an extra reference under the
 * lock and copy with it dropped, so a racing decommit or shrink can't free
 * a page mid-copy: whoever drops the last reference frees it. Contiguous
 * and physical VMOs never give pages up before destroy, so they skip this.
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
#include <jam/mm.h>
#include <jam/panic.h>
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
    struct list_node node;
    enum range_kind  kind;
    bool             busy;        /* being set up or unmapped: not findable */
    uint64_t         first, end;  /* page indices */
    uint64_t         key;         /* mapping: base va; pin: pin id */
    struct kobject  *cap;         /* pin: the DMA capability (referenced) */
};

struct vmo {
    struct kobject   base;
    enum vmo_kind    kind;
    uint32_t         flags;       /* VMO_CONTIGUOUS / VMO_DMA32 */
    unsigned         cache;       /* physical: VM_UC / VM_WC / 0 */
    uint64_t         size;        /* bytes, a page multiple */
    uint64_t         committed;   /* pages owned right now */
    uint64_t         phys;        /* contiguous / physical: first byte */
    unsigned         order;       /* contiguous: buddy order it came from */
    uint64_t         next_pin_id;
    struct list_node ranges;      /* struct vmo_range */
    uint64_t       **root[ROOT_ENTRIES];   /* paged: root[r][m][l] = phys */
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
    uint32_t left = __atomic_sub_fetch(&p->refcount, 1, __ATOMIC_ACQ_REL);
    if (left == UINT32_MAX)
        panic("vmo: page %lx released too many times", page_to_phys(p));
    if (left == 0)
        pmm_free_pages(p, 0);
}

static void *table_alloc(void)
{
    struct page *p = pmm_alloc_pages(0, PMM_ZERO);
    return p ? page_to_virt(p) : NULL;
}

static void table_free(void *t)
{
    pmm_free_pages(virt_to_page(t), 0);
}

/* With the lock held: the table slot for page idx. NULL if its tables don't
 * exist and `create` is false, or creating them failed. */
static uint64_t *slot_locked(struct vmo *v, uint64_t idx, bool create)
{
    uint64_t ***mid = &v->root[idx / MID_PAGES];
    if (!*mid && (!create || !(*mid = table_alloc())))
        return NULL;
    uint64_t **leaf = &(*mid)[(idx / LEAF_PAGES) % TBL_ENTRIES];
    if (!*leaf && (!create || !(*leaf = table_alloc())))
        return NULL;
    return &(*leaf)[idx % LEAF_PAGES];
}

/* With the lock held: physical address of page idx, 0 if not committed. */
static uint64_t phys_locked(struct vmo *v, uint64_t idx)
{
    if (v->kind != VMO_PAGED)
        return v->phys + (idx << PAGE_SHIFT);
    uint64_t *s = slot_locked(v, idx, false);
    return s ? *s : 0;
}

/* With the lock held (or from destroy): drop every page at index >= first,
 * and free the table pages that lie wholly past it. */
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
            for (uint64_t l = 0; l < LEAF_PAGES; l++) {
                if (leaf[l] && base + l >= first) {
                    page_put(pa_page(leaf[l]));
                    leaf[l] = 0;
                    v->committed--;
                }
            }
            if (base >= first) {
                table_free(leaf);
                mid[m] = NULL;
            }
        }
        if (r * MID_PAGES >= first) {
            table_free(mid);
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
        uint64_t *s = slot_locked(v, idx, fresh != NULL);
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
            if (!s) {
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
    if (!list_empty(&v->ranges))
        panic("vmo: koid %lu destroyed while mapped or pinned", obj->koid);
    if (v->kind == VMO_PAGED) {
        drop_from_locked(v, 0);
        ASSERT(v->committed == 0);
    } else if (v->kind == VMO_CONTIG) {
        contig_free_head(v->phys >> PAGE_SHIFT, v->size >> PAGE_SHIFT);
    }
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
static bool ranges_overlap_locked(struct vmo *v, uint64_t first, uint64_t end)
{
    for (struct list_node *n = v->ranges.next; n != &v->ranges; n = n->next) {
        struct vmo_range *r = container_of(n, struct vmo_range, node);
        if (r->first < end && first < r->end)
            return true;
    }
    return false;
}

status_t vmo_set_size(struct vmo *v, uint64_t size)
{
    if (v->kind != VMO_PAGED)
        return ERR_NOT_SUPPORTED;
    if (size > VMO_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    size = ALIGN_UP(size, PAGE_SIZE);
    uint64_t f = vlock(v);
    uint64_t first = size >> PAGE_SHIFT, end = v->size >> PAGE_SHIFT;
    if (first < end) {
        if (ranges_overlap_locked(v, first, end)) {
            vunlock(v, f);
            return ERR_BAD_STATE;
        }
        drop_from_locked(v, first);
    }
    __atomic_store_n(&v->size, size, __ATOMIC_RELAXED);
    vunlock(v, f);
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
    if (ranges_overlap_locked(v, first, end)) {
        vunlock(v, f);
        return ERR_BAD_STATE;
    }
    for (uint64_t idx = first; idx < end;) {
        uint64_t *s = slot_locked(v, idx, false);
        if (!s) {
            idx = ALIGN_UP(idx + 1, LEAF_PAGES);   /* no leaf: skip its 2 MiB */
            continue;
        }
        if (*s) {
            page_put(pa_page(*s));   /* frees it unless a reader holds it */
            *s = 0;
            v->committed--;
        }
        idx++;
    }
    vunlock(v, f);
    return OK;
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
    if (r->cap)
        kobject_unref(r->cap);
    kfree(r);
    kobject_unref(&v->base);
}

/* With the lock held: the non-busy range of `kind` with this key. */
static struct vmo_range *range_find_locked(struct vmo *v, enum range_kind kind, uint64_t key)
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

    struct vmo_range *r = kzalloc(sizeof(*r));
    if (!r)
        return ERR_NO_MEMORY;
    r->kind = RANGE_PIN;
    r->first = first;
    r->end = end;
    status_t st = range_add(v, r);
    if (st != OK) {
        kfree(r);
        return st;
    }
    kobject_ref(dma_cap);
    r->cap = dma_cap;
    if (v->kind == VMO_PAGED && (st = commit_pages(v, first, end)) != OK) {
        range_remove(v, r);
        return st;
    }

    uint64_t f = vlock(v);
    for (uint64_t idx = first; idx < end; idx++) {
        phys_out[idx - first] = phys_locked(v, idx);
        ASSERT(phys_out[idx - first] != 0 || v->kind != VMO_PAGED);
    }
    r->busy = false;
    *pin_id = r->key;
    vunlock(v, f);
    return OK;
}

status_t vmo_unpin(struct vmo *v, uint64_t pin_id)
{
    uint64_t f = vlock(v);
    struct vmo_range *r = range_find_locked(v, RANGE_PIN, pin_id);
    if (r)
        r->busy = true;
    vunlock(v, f);
    if (!r)
        return ERR_NOT_FOUND;
    range_remove(v, r);
    return OK;
}
