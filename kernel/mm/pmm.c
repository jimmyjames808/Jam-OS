/* Physical memory: an early bump allocator for bootstrapping, then a buddy
 * allocator (orders 0..MAX_ORDER) with a DMA32 zone below 4 GiB, and a
 * per-CPU cache ("stash") of single pages in front of it.
 *
 * struct page lives in the vmemmap array indexed by PFN. vmm_init backs it
 * in 2 MiB chunks and skips chunks that describe no RAM at all. A buddy block
 * (at most 4 MiB) never spans two chunks, so merging never touches an
 * unbacked struct page.
 *
 * Per-CPU stashes. The buddy lists sit behind one global lock, which
 * collapsed when every CPU allocated at once (BENCH.md: 53 ns on one CPU,
 * 19 us with 28 CPUs). Each CPU now keeps up to PCP_MAX free order-0 pages
 * of its own. A single-page allocation or free on that CPU touches only its
 * stash; an empty stash refills, and a full one drains, PCP_BATCH pages at a
 * time under ONE acquisition of the buddy lock.
 *   - Zone: stashes hold pages of `pcp_zone` only: the normal zone when the
 *     machine has memory above 4 GiB, else DMA32 (then it is the only
 *     zone). PMM_DMA32 requests and every order > 0 request go straight to
 *     the buddy lists, and a freed page of the other zone goes straight back
 *     to them, so memory below 4 GiB is never parked in a stash nor handed
 *     to ordinary allocations while normal memory is free (the preference
 *     the buddy allocator already applies). PMM_ZERO is applied after the
 *     page is taken, outside every lock, whichever path it came from.
 *   - Accounting: stashed pages are FREE. pmm_stats() reports the buddy
 *     count plus every stash's count, read under the buddy lock. Pages move
 *     between a stash and the buddy lists only with that lock held, so a
 *     refill or drain is never seen half done, and the total changes only
 *     through real allocations and frees (exact whenever nobody allocates,
 *     which is what the ktest leak check needs). A stashed page is tagged
 *     PG_PCP (not PG_FREE, so the buddy allocator never merges it), which
 *     also lets pmm_free_pages catch a double free of a stashed page.
 *   - Stash lock: each stash has a flag lock, taken with interrupts off by
 *     its owner on every operation and by any CPU draining it. It is not a
 *     spinlock_t, so the lock checker does not see it: that keeps the
 *     owner's path at one uncontended exchange on a line only that CPU
 *     touches, instead of the checker's bookkeeping twice per page. It is
 *     safe because it is private to this file and strictly ordered: nothing
 *     but the buddy lock "pmm" (a leaf) is ever taken under it, it is never
 *     taken with "pmm" held, and at most one stash lock is held at a time.
 *     So a CPU spinning on a stash lock waits for a holder that waits at
 *     most for "pmm", whose holders wait for nothing. Interrupts are off
 *     while it is held, so an interrupt handler can't deadlock against its
 *     own CPU's stash (none allocates today; the checker would object to
 *     "pmm" taken in one anyway).
 *   - Out of memory: before an allocation fails, every CPU's stash is
 *     drained into the buddy lists and the buddy allocation is retried.
 *     Draining steals under each stash's lock, with no IPIs, so it works
 *     from every context the allocator is called from (spinlocks held,
 *     interrupts off: e.g. page tables allocated under the page-table lock).
 *     Race: other CPUs can take the drained pages first. Using them is real
 *     use of memory, but an allocator could also park them in its stash
 *     again. Two things bound that: below PCP_LOW free pages in the stash
 *     zone, stashes stop refilling (an empty stash then allocates straight
 *     from the buddy lists) and frees bypass them, so near exhaustion the
 *     allocator behaves like the plain buddy allocator; and drain-and-retry
 *     repeats (up to PCP_DRAIN_TRIES times) while the drain still finds
 *     pages. What a stash can still hold then is a page freed on a CPU that
 *     read the low-water mark just before it was crossed: at most
 *     PCP_MAX per CPU, only while memory is being freed. */
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/x86.h>

#define MAX_EARLY_RANGES 128
#define FOUR_GIB         (4ull << 30)

#define PCP_MAX         PMM_PCP_MAX
#define PCP_BATCH       PMM_PCP_BATCH
#define PCP_LOW         2048   /* stash-zone free pages (8 MiB) below which
                                * stashes neither refill nor take frees */
#define PCP_DRAIN_TRIES 3

struct range {
    uint64_t base, end;   /* physical, [base, end) */
};

struct page *const vmemmap = (struct page *)VMEMMAP_BASE;
uint64_t hhdm_offset;

static struct range early[MAX_EARLY_RANGES];
static size_t early_count;
/* Every boot memory-map range that is (or was) RAM, page-rounded
 * outwards, for pmm_range_has_ram (never changes after early init). */
static struct range ram[BOOT_MAX_MEMMAP];
static size_t ram_count;
static bool early_done;
static uint64_t max_pfn;

static struct list_node free_lists[ZONE_COUNT][MAX_ORDER + 1];
/* Guarded by `lock`. free_pages counts the buddy lists only (pmm_stats adds
 * the stashes); zone_free is the same per zone, read racily by pcp_free. */
static uint64_t total_pages, free_pages;
static uint64_t zone_total[ZONE_COUNT];
static volatile uint64_t zone_free[ZONE_COUNT];
static spinlock_t lock = SPINLOCK_INIT("pmm");

struct pcp {
    volatile uint32_t busy;             /* the stash lock (see the top) */
    volatile uint32_t n;                /* pages[0..n) stashed; written with
                                         * busy held, read racily by stats */
    struct page      *pages[PCP_MAX];   /* LIFO: pages[n-1] is the hottest */
} __attribute__((aligned(64)));

static struct pcp pcps[MAX_CPUS];
static volatile unsigned pcp_zone = ZONE_DMA32;
static volatile uint64_t pcp_drains;

void pmm_early_init(const struct boot_info *bi)
{
    hhdm_offset = bi->hhdm_offset;
    for (size_t i = 0; i < bi->memmap_count; i++) {
        const struct boot_mem_region *r = &bi->memmap[i];
        if (boot_mem_is_ram(r->type)) {
            uint64_t end_pfn = (r->base + r->length) >> PAGE_SHIFT;
            if (end_pfn > max_pfn)
                max_pfn = end_pfn;
        }
        if ((boot_mem_is_ram(r->type) || r->type == BOOT_MEM_BAD) && r->length &&
            ram_count < BOOT_MAX_MEMMAP)
            ram[ram_count++] = (struct range){ ALIGN_DOWN(r->base, PAGE_SIZE),
                                               ALIGN_UP(r->base + r->length, PAGE_SIZE) };
        if (r->type != BOOT_MEM_USABLE)
            continue;
        /* Physical page 0 is never handed out: 0 means "no page" in the
         * phys-address APIs, and a NULL-ish bug must not hit real data. */
        uint64_t base = ALIGN_UP(r->base, PAGE_SIZE);
        if (base == 0)
            base = PAGE_SIZE;
        uint64_t end = ALIGN_DOWN(r->base + r->length, PAGE_SIZE);
        if (base >= end)
            continue;
        if (early_count == MAX_EARLY_RANGES)
            panic("pmm: more than %d usable ranges", MAX_EARLY_RANGES);
        early[early_count++] = (struct range){ base, end };
    }
}

uint64_t pmm_max_pfn(void)
{
    return max_pfn;
}

bool pmm_range_has_ram(uint64_t base, uint64_t len)
{
    if (len == 0)
        return false;
    uint64_t end = base + len < base ? UINT64_MAX : base + len;
    for (size_t i = 0; i < ram_count; i++)
        if (ram[i].base < end && base < ram[i].end)
            return true;
    return false;
}

/* Carves from the top of the highest range that fits, keeping low memory
 * (and DMA32) for later. Memory is zeroed. */
uint64_t pmm_early_alloc(uint64_t size, uint64_t align)
{
    ASSERT(!early_done);
    size = ALIGN_UP(size, PAGE_SIZE);
    for (size_t i = early_count; i-- > 0;) {
        struct range *r = &early[i];
        if (r->end - r->base < size)
            continue;
        uint64_t pa = ALIGN_DOWN(r->end - size, align);
        if (pa < r->base)
            continue;
        r->end = pa;   /* anything above pa+size is alignment slack, lost */
        memset(phys_to_virt(pa), 0, size);
        return pa;
    }
    panic("pmm: early allocation of %lu bytes failed", size);
}

static unsigned zone_of(uint64_t pfn)
{
    return (pfn << PAGE_SHIFT) < FOUR_GIB ? ZONE_DMA32 : ZONE_NORMAL;
}

static void free_block_locked(uint64_t pfn, unsigned order)
{
    unsigned zone = zone_of(pfn);
    while (order < MAX_ORDER) {
        uint64_t buddy_pfn = pfn ^ (1ull << order);
        if (buddy_pfn >= max_pfn)
            break;
        struct page *b = pfn_to_page(buddy_pfn);
        if (!(b->flags & PG_FREE) || b->order != order || b->zone != zone)
            break;
        list_del(&b->node);
        b->flags &= ~PG_FREE;
        pfn &= ~(1ull << order);
        order++;
    }
    struct page *p = pfn_to_page(pfn);
    p->flags = PG_FREE;
    p->order = order;
    p->zone = zone;
    list_add(&free_lists[zone][order], &p->node);
}

/* Hand [pfn, end) to the buddy allocator. Returns the number of pages. */
static uint64_t seed_range_locked(uint64_t pfn, uint64_t end)
{
    uint64_t added = 0;
    for (uint64_t q = pfn; q < end; q++) {
        struct page *p = pfn_to_page(q);
        p->flags = 0;
        p->zone = zone_of(q);
    }
    /* Largest aligned blocks that fit and don't cross 4 GiB. */
    while (pfn < end) {
        unsigned order = MAX_ORDER;
        while (order > 0 &&
               ((pfn & ((1ull << order) - 1)) || pfn + (1ull << order) > end ||
                zone_of(pfn) != zone_of(pfn + (1ull << order) - 1)))
            order--;
        free_block_locked(pfn, order);
        zone_total[zone_of(pfn)] += 1ull << order;
        zone_free[zone_of(pfn)] += 1ull << order;
        pfn += 1ull << order;
        added += 1ull << order;
    }
    /* Stash the normal zone once there is one (see the top). Pages of the
     * old zone already stashed are fine: they drain back in time. */
    pcp_zone = zone_total[ZONE_NORMAL] ? ZONE_NORMAL : ZONE_DMA32;
    return added;
}

void pmm_init(void)
{
    for (unsigned z = 0; z < ZONE_COUNT; z++)
        for (unsigned o = 0; o <= MAX_ORDER; o++)
            list_init(&free_lists[z][o]);

    /* vmm_init marked every backed struct page PG_RESERVED; only the pages
     * handed to the buddy allocator below become allocatable. */
    early_done = true;
    for (size_t i = 0; i < early_count; i++)
        total_pages += seed_range_locked(early[i].base >> PAGE_SHIFT,
                                         early[i].end >> PAGE_SHIFT);
    free_pages = total_pages;
}

uint64_t pmm_add_range(uint64_t base, uint64_t length)
{
    uint64_t pfn = ALIGN_UP(base, PAGE_SIZE) >> PAGE_SHIFT;
    uint64_t end = ALIGN_DOWN(base + length, PAGE_SIZE) >> PAGE_SHIFT;
    if (pfn == 0)
        pfn = 1;   /* page 0 stays reserved */
    if (end > max_pfn)
        end = max_pfn;
    if (pfn >= end)
        return 0;

    spin_lock(&lock);
    uint64_t added = seed_range_locked(pfn, end);
    total_pages += added;
    free_pages += added;
    spin_unlock(&lock);
    return added << PAGE_SHIFT;
}

/* ---- buddy lists (lock held) ------------------------------------------------ */

/* A 2^order block from zone z's lists, or NULL. */
static struct page *buddy_take_locked(unsigned order, unsigned z)
{
    for (unsigned o = order; o <= MAX_ORDER; o++) {
        if (list_empty(&free_lists[z][o]))
            continue;
        struct page *p = list_first(&free_lists[z][o], struct page, node);
        list_del(&p->node);
        p->flags &= ~PG_FREE;
        /* Split, returning upper halves to the free lists. */
        while (o > order) {
            o--;
            struct page *half = p + (1ull << o);
            half->flags = PG_FREE;
            half->order = o;
            half->zone = z;
            list_add(&free_lists[z][o], &half->node);
        }
        free_pages -= 1ull << order;
        zone_free[z] -= 1ull << order;
        return p;
    }
    return NULL;
}

static void buddy_put_locked(struct page *p, unsigned order)
{
    unsigned z = p->zone;
    for (uint64_t i = 0; i < (1ull << order); i++)
        p[i].flags = 0;
    free_block_locked(page_to_pfn(p), order);
    free_pages += 1ull << order;
    zone_free[z] += 1ull << order;
}

static struct page *buddy_alloc(unsigned order, unsigned flags)
{
    spin_lock(&lock);
    struct page *p = NULL;
    /* Prefer NORMAL so DMA32 stays available for devices that need it. */
    for (int z = (flags & PMM_DMA32) ? ZONE_DMA32 : ZONE_NORMAL; z >= 0 && !p; z--)
        p = buddy_take_locked(order, (unsigned)z);
    spin_unlock(&lock);
    return p;
}

/* ---- per-CPU stashes --------------------------------------------------------- */

static inline void pcp_lock(struct pcp *s)
{
    while (__atomic_exchange_n(&s->busy, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&s->busy, __ATOMIC_RELAXED))
            cpu_relax();
}

static inline void pcp_unlock(struct pcp *s)
{
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
}

/* With s locked: move up to `count` of its COLDEST pages (the bottom of the
 * stack) back to the buddy lists under one acquisition of the lock. */
static uint32_t pcp_drain_locked(struct pcp *s, uint32_t count)
{
    uint32_t n = s->n;
    if (count > n)
        count = n;
    if (!count)
        return 0;
    spin_lock(&lock);
    for (uint32_t i = 0; i < count; i++)
        buddy_put_locked(s->pages[i], 0);
    for (uint32_t i = count; i < n; i++)
        s->pages[i - count] = s->pages[i];
    __atomic_store_n(&s->n, n - count, __ATOMIC_RELAXED);
    spin_unlock(&lock);
    return count;
}

/* With s locked and empty: take up to PCP_BATCH pages of the stash zone
 * under one acquisition of the lock, or none below the low-water mark. */
static void pcp_refill_locked(struct pcp *s)
{
    spin_lock(&lock);
    unsigned z = pcp_zone;
    uint32_t n = 0;
    if (zone_free[z] >= PCP_LOW + PCP_BATCH) {
        for (; n < PCP_BATCH; n++) {
            struct page *p = buddy_take_locked(0, z);
            if (!p)
                break;
            p->flags = PG_PCP;
            s->pages[n] = p;
        }
    }
    __atomic_store_n(&s->n, n, __ATOMIC_RELAXED);
    spin_unlock(&lock);
}

static struct page *pcp_alloc(void)
{
    uint64_t f = irq_save();   /* pins us to this CPU's stash (see the top) */
    struct pcp *s = &pcps[percpu_index()];
    pcp_lock(s);
    if (!s->n)
        pcp_refill_locked(s);
    struct page *p = NULL;
    uint32_t n = s->n;
    if (n) {
        p = s->pages[n - 1];
        __atomic_store_n(&s->n, n - 1, __ATOMIC_RELAXED);
        p->flags = 0;
    }
    pcp_unlock(s);
    irq_restore(f);
    return p;
}

/* False if the page must go to the buddy lists instead. */
static bool pcp_free(struct page *p)
{
    unsigned z = pcp_zone;
    if (p->zone != z || zone_free[z] < PCP_LOW)
        return false;
    uint64_t f = irq_save();
    struct pcp *s = &pcps[percpu_index()];
    pcp_lock(s);
    if (s->n == PCP_MAX)
        pcp_drain_locked(s, PCP_BATCH);
    uint32_t n = s->n;
    p->flags = PG_PCP;
    s->pages[n] = p;
    __atomic_store_n(&s->n, n + 1, __ATOMIC_RELAXED);
    pcp_unlock(s);
    irq_restore(f);
    return true;
}

uint64_t pmm_drain_stashes(void)
{
    uint64_t moved = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct pcp *s = &pcps[i];
        if (!__atomic_load_n(&s->n, __ATOMIC_RELAXED))
            continue;   /* a racy peek; the count is re-read under the lock */
        uint64_t f = irq_save();
        pcp_lock(s);
        moved += pcp_drain_locked(s, s->n);
        pcp_unlock(s);
        irq_restore(f);
    }
    __atomic_add_fetch(&pcp_drains, 1, __ATOMIC_RELAXED);
    return moved;
}

uint64_t pmm_stash_pages(void)
{
    uint64_t n = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        n += __atomic_load_n(&pcps[i].n, __ATOMIC_RELAXED);
    return n;
}

uint32_t pmm_stash_count(uint32_t cpu)
{
    return cpu < MAX_CPUS ? __atomic_load_n(&pcps[cpu].n, __ATOMIC_RELAXED) : 0;
}

uint64_t pmm_stash_drains(void)
{
    return __atomic_load_n(&pcp_drains, __ATOMIC_RELAXED);
}

/* ---- interface ------------------------------------------------------------ */

struct page *pmm_alloc_pages(unsigned order, unsigned flags)
{
    ASSERT(order <= MAX_ORDER);
    struct page *p = NULL;
    if (order == 0 && !(flags & PMM_DMA32))
        p = pcp_alloc();
    if (!p)
        p = buddy_alloc(order, flags);
    /* The buddy lists can't serve it: pull every stash back and retry,
     * again while the drain still finds pages (see the top). */
    for (int tries = 0; !p && tries < PCP_DRAIN_TRIES; tries++) {
        if (!pmm_drain_stashes() && tries > 0)
            break;
        p = buddy_alloc(order, flags);
    }
    if (!p)
        return NULL;
    p->order = order;
    p->refcount = 1;
    p->private = 0;
    if (flags & PMM_ZERO)
        memset(page_to_virt(p), 0, PAGE_SIZE << order);
    return p;
}

void pmm_free_pages(struct page *p, unsigned order)
{
    ASSERT(!(p->flags & (PG_FREE | PG_RESERVED | PG_PCP)));
    if (order == 0 && pcp_free(p))
        return;
    spin_lock(&lock);
    buddy_put_locked(p, order);
    spin_unlock(&lock);
}

uint64_t pmm_alloc_page_phys(unsigned flags)
{
    struct page *p = pmm_alloc_pages(0, flags);
    return p ? page_to_phys(p) : 0;
}

void pmm_free_page_phys(uint64_t pa)
{
    pmm_free_pages(pfn_to_page(pa >> PAGE_SHIFT), 0);
}

void pmm_stats(uint64_t *total, uint64_t *free)
{
    /* Objects parked in the heap's per-CPU magazines hold slab pages that
     * a drained heap would give back: return them first, so the count is
     * exact (heap.c). Takes no lock of ours. */
    kmem_drain_all();
    spin_lock(&lock);
    *total = total_pages;
    /* Under the lock, so no refill or drain is seen half done (see the top). */
    *free = free_pages + pmm_stash_pages();
    spin_unlock(&lock);
}
