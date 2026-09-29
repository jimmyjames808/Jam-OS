/* Physical memory, kernel page tables and the kernel heap.
 *
 * Kernel virtual layout (4-level paging):
 *   ffff800000000000  HHDM: all RAM mapped at phys + hhdm_offset (WB),
 *                     framebuffer mapped the same way (WC)
 *   ffffc00000000000  vmemmap: struct page array, indexed by PFN
 *   ffffd00000000000  vmap: kernel stacks (with guard pages), MMIO
 *   ffffffff80000000  kernel image
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/boot.h>
#include <jam/list.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE  (1ull << PAGE_SHIFT)
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))

#define VMEMMAP_BASE 0xffffc00000000000ull
#define VMAP_BASE    0xffffd00000000000ull
#define VMAP_END     0xffffe00000000000ull

extern uint64_t hhdm_offset;

static inline void *phys_to_virt(uint64_t pa) { return (void *)(pa + hhdm_offset); }
static inline uint64_t virt_to_phys(const void *va) { return (uint64_t)va - hhdm_offset; }

/* ---- physical pages ---------------------------------------------------- */

#define PG_RESERVED (1u << 0)   /* not managed by the allocator */
#define PG_FREE     (1u << 1)   /* head of a free buddy block */
#define PG_SLAB     (1u << 2)   /* part of a slab; private = slab header */
#define PG_LARGE    (1u << 3)   /* head of a kmalloc multi-page block */
#define PG_PCP      (1u << 4)   /* free, parked in a per-CPU page stash */

enum { ZONE_DMA32, ZONE_NORMAL, ZONE_COUNT };   /* DMA32 = below 4 GiB */

struct page {
    struct list_node node;
    uint16_t flags;
    uint8_t  order;
    uint8_t  zone;
    uint32_t refcount;
    uint64_t private;
};
_Static_assert(sizeof(struct page) == 32, "struct page should stay 32 bytes");

#define MAX_ORDER 10   /* largest block: 4 MiB */

#define PMM_ZERO  (1u << 0)
#define PMM_DMA32 (1u << 1)   /* must be below 4 GiB */

extern struct page *const vmemmap;

static inline struct page *pfn_to_page(uint64_t pfn) { return &vmemmap[pfn]; }
static inline uint64_t page_to_pfn(const struct page *p) { return (uint64_t)(p - vmemmap); }
static inline uint64_t page_to_phys(const struct page *p) { return page_to_pfn(p) << PAGE_SHIFT; }
static inline void *page_to_virt(const struct page *p) { return phys_to_virt(page_to_phys(p)); }
static inline struct page *virt_to_page(const void *va) { return pfn_to_page(virt_to_phys(va) >> PAGE_SHIFT); }

void         pmm_early_init(const struct boot_info *bi);
/* Bump allocator used only while building the first page tables. */
uint64_t     pmm_early_alloc(uint64_t size, uint64_t align);
void         pmm_init(void);
/* Give a RAM range (e.g. loader-reclaimable) to the allocator; returns bytes. */
uint64_t     pmm_add_range(uint64_t base, uint64_t length);
struct page *pmm_alloc_pages(unsigned order, unsigned flags);
void         pmm_free_pages(struct page *p, unsigned order);
uint64_t     pmm_alloc_page_phys(unsigned flags);   /* 0 on failure */
void         pmm_free_page_phys(uint64_t pa);
/* Free pages include those parked in the per-CPU stashes (see pmm.c). It
 * drains the heap's per-CPU magazines first (heap.c), so call it from
 * thread context with no heap lock held. */
void         pmm_stats(uint64_t *total_pages, uint64_t *free_pages);
uint64_t     pmm_max_pfn(void);
/* Per-CPU page stashes: empty all of them into the buddy lists (returns the
 * pages moved; the allocator does this itself before failing), and the
 * number of pages parked in them / whole-machine drains so far (tests). */
#define PMM_PCP_MAX   64   /* pages one CPU's stash holds at most */
#define PMM_PCP_BATCH 16   /* pages moved per refill / drain */
uint64_t     pmm_drain_stashes(void);
uint64_t     pmm_stash_pages(void);
uint32_t     pmm_stash_count(uint32_t cpu);
uint64_t     pmm_stash_drains(void);

/* ---- page tables ------------------------------------------------------- */

#define VM_WRITE  (1u << 0)
#define VM_EXEC   (1u << 1)
#define VM_USER   (1u << 2)
#define VM_WC     (1u << 3)   /* write-combining (framebuffers) */
#define VM_UC     (1u << 4)   /* uncached (MMIO) */
#define VM_GLOBAL (1u << 5)
#define VM_SMALL  (1u << 6)   /* force 4 KiB pages */

void     vmm_init(const struct boot_info *bi);
uint64_t vmm_kernel_pml4(void);
/* Page tables come from the early allocator until this is called. */
void     vmm_use_buddy(void);
/* Map [va, va+len) to [pa, pa+len), using 2 MiB / 1 GiB pages when possible. */
void     vmm_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t len, unsigned flags);
/* Unmap 4 KiB-mapped pages; does not free the physical pages. */
void     vmm_unmap(uint64_t pml4, uint64_t va, uint64_t len);
/* Physical address for va, or UINT64_MAX if unmapped. */
uint64_t vmm_translate(uint64_t pml4, uint64_t va);
/* Effective PAT memory type of a mapping ("WB", "WC", "UC", ...). */
const char *vmm_cache_type(uint64_t pml4, uint64_t va);
/* Effective access to va through every level: VM_WRITE and/or VM_EXEC, or
 * -1 if it is not mapped. */
int      vmm_access(uint64_t pml4, uint64_t va);

/* Kernel stack in the vmap area with an unmapped guard page below it.
 * Returns the TOP of the stack. Panics when out of memory (boot-time and
 * per-CPU stacks, which can't do without). */
void    *kstack_alloc(size_t size);
/* The same, but returns NULL when out of memory (nothing is left behind). */
void    *kstack_alloc_try(size_t size);
/* Unmap and free a stack from kstack_alloc*: the pages go back to the
 * allocator after a TLB shootdown, and the virtual range is kept for the next
 * stack of the same size. Needs interrupts on and no spinlock held (the
 * shootdown waits for other CPUs); nothing may still run on the stack. */
void     kstack_free(void *top, size_t size);
/* Reserve (but do not map) kernel virtual space in the vmap area. */
uint64_t vmm_reserve(uint64_t len);
/* Map a physical MMIO range uncached into the vmap area. */
void    *vmm_map_mmio(uint64_t pa, uint64_t len);

/* ---- kernel heap ------------------------------------------------------- */

struct kmem_cache;

void               heap_init(void);
void              *kmalloc(size_t size);
void              *kzalloc(size_t size);
void               kfree(void *p);
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align);
void              *kmem_cache_alloc(struct kmem_cache *c);
void               kmem_cache_free(struct kmem_cache *c, void *obj);

/* Per-CPU magazines (M5.5, heap.c): each CPU keeps up to MAG_MAX free
 * objects per cache, refilled and drained MAG_BATCH at a time. */
#define MAG_MAX   16
#define MAG_BATCH 8
/* Once cpu_count is final (smp_start_aps): give every CPU its magazines.
 * Until then every allocation takes the cache lock. */
void               heap_percpu_init(void);
/* The switch (boot "nokmcache"); the benchmark flips it. */
extern volatile bool heap_percpu;
/* Return every CPU's magazined objects to their slabs (pmm_stats does this
 * first, so its count is exact). Returns how many moved. */
uint64_t           kmem_drain_all(void);
/* Tests: objects in cpu's magazine for c; the cache kmalloc(size) uses
 * (NULL above the slab sizes). */
uint64_t           kmem_cached_objects(uint32_t cpu, struct kmem_cache *c);
struct kmem_cache *kmalloc_cache_for(size_t size);
