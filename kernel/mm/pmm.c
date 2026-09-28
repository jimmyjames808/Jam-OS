/* Physical memory: an early bump allocator for bootstrapping, then a buddy
 * allocator (orders 0..MAX_ORDER) with a DMA32 zone below 4 GiB.
 *
 * struct page lives in the vmemmap array indexed by PFN. vmm_init backs it
 * in 2 MiB chunks and skips chunks that describe no RAM at all. A buddy block
 * (at most 4 MiB) never spans two chunks, so merging never touches an
 * unbacked struct page. */
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/spinlock.h>
#include <jam/string.h>

#define MAX_EARLY_RANGES 128
#define FOUR_GIB         (4ull << 30)

struct range {
    uint64_t base, end;
};

struct page *const vmemmap = (struct page *)VMEMMAP_BASE;
uint64_t hhdm_offset;

static struct range early[MAX_EARLY_RANGES];
static size_t early_count;
static bool early_done;
static uint64_t max_pfn;

static struct list_node free_lists[ZONE_COUNT][MAX_ORDER + 1];
static uint64_t total_pages, free_pages;
static spinlock_t lock = SPINLOCK_INIT;

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
        pfn += 1ull << order;
        added += 1ull << order;
    }
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

struct page *pmm_alloc_pages(unsigned order, unsigned flags)
{
    ASSERT(order <= MAX_ORDER);
    spin_lock(&lock);
    struct page *p = NULL;
    /* Prefer NORMAL so DMA32 stays available for devices that need it. */
    for (int z = (flags & PMM_DMA32) ? ZONE_DMA32 : ZONE_NORMAL; z >= 0 && !p; z--) {
        for (unsigned o = order; o <= MAX_ORDER; o++) {
            if (list_empty(&free_lists[z][o]))
                continue;
            p = list_first(&free_lists[z][o], struct page, node);
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
            break;
        }
    }
    if (p) {
        free_pages -= 1ull << order;
        p->order = order;
        p->refcount = 1;
        p->private = 0;
    }
    spin_unlock(&lock);

    if (p && (flags & PMM_ZERO))
        memset(page_to_virt(p), 0, PAGE_SIZE << order);
    return p;
}

void pmm_free_pages(struct page *p, unsigned order)
{
    ASSERT(!(p->flags & (PG_FREE | PG_RESERVED)));
    spin_lock(&lock);
    for (uint64_t i = 0; i < (1ull << order); i++)
        p[i].flags = 0;
    free_block_locked(page_to_pfn(p), order);
    free_pages += 1ull << order;
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
    spin_lock(&lock);
    *total = total_pages;
    *free = free_pages;
    spin_unlock(&lock);
}
