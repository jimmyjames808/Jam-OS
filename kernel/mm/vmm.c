/* Kernel page tables. vmm_init builds a fresh PML4 (no dependency on the
 * loader's tables), maps the kernel image with per-section permissions, the
 * HHDM (RAM write-back, framebuffer write-combining) and the vmemmap, then
 * switches CR3. */
#include <jam/cpu.h>
#include <jam/dbghook.h>
#include <jam/ipi.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/x86.h>

#define PTE_P     (1ull << 0)
#define PTE_W     (1ull << 1)
#define PTE_U     (1ull << 2)
#define PTE_PWT   (1ull << 3)
#define PTE_PCD   (1ull << 4)
#define PTE_PS    (1ull << 7)   /* PD/PDPT: large page */
#define PTE_PAT4K (1ull << 7)   /* PT: PAT bit */
#define PTE_G     (1ull << 8)
#define PTE_PATLG (1ull << 12)  /* PD/PDPT large page: PAT bit */
#define PTE_NX    (1ull << 63)
#define PTE_ADDR  0x000ffffffffff000ull

#define SIZE_2M (1ull << 21)
#define SIZE_1G (1ull << 30)

extern char __kernel_start[], __text_start[], __text_end[], __rodata_start[],
    __rodata_end[], __data_start[], __data_end[], __ksyms_start[], __kernel_end[];

static uint64_t kernel_pml4;
static bool use_buddy;
static spinlock_t vmap_lock = SPINLOCK_INIT("vmap");
static spinlock_t pt_lock = SPINLOCK_INIT("kernel page tables");
static uint64_t vmap_next = VMAP_BASE;

/* 0 only when `may_fail` and the allocator is out of memory. */
static uint64_t alloc_table_mode(bool may_fail)
{
    if (!use_buddy)
        return pmm_early_alloc(PAGE_SIZE, PAGE_SIZE);
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    if (!pa && !may_fail)
        panic("vmm: out of memory for page tables");
    return pa;
}

static uint64_t alloc_table(void)
{
    return alloc_table_mode(false);
}

static uint64_t *table(uint64_t pa)
{
    return phys_to_virt(pa & PTE_ADDR);
}

static uint64_t leaf_bits(unsigned flags, bool large)
{
    uint64_t e = PTE_P;
    if (flags & VM_WRITE)
        e |= PTE_W;
    if (flags & VM_USER)
        e |= PTE_U;
    if (!(flags & VM_EXEC) && cpu_features.nx)
        e |= PTE_NX;
    if ((flags & VM_GLOBAL) && cpu_features.pge)
        e |= PTE_G;
    if (flags & VM_WC)   /* PAT index 5: PAT=1 PCD=0 PWT=1 */
        e |= (large ? PTE_PATLG : PTE_PAT4K) | PTE_PWT;
    else if (flags & VM_UC)   /* PAT index 3 */
        e |= PTE_PCD | PTE_PWT;
    return e;
}

/* Walk to the entry for va at `level` (1 = PT ... 4 = PML4), creating
 * intermediate tables. WALK_TRY returns NULL instead of panicking when a
 * table can't be allocated (tables created before that stay: harmless, and
 * reused by the next walk). */
enum { WALK_LOOKUP, WALK_CREATE, WALK_TRY };

static uint64_t *walk(uint64_t pml4, uint64_t va, int level, int create)
{
    uint64_t *t = table(pml4);
    for (int l = 4; l > level; l--) {
        uint64_t *e = &t[(va >> (12 + 9 * (l - 1))) & 511];
        if (!(*e & PTE_P)) {
            if (create == WALK_LOOKUP)
                return NULL;
            uint64_t pa = alloc_table_mode(create == WALK_TRY);
            if (!pa)
                return NULL;
            /* Intermediate entries are permissive; leaves decide access. */
            *e = pa | PTE_P | PTE_W | PTE_U;
        } else if (*e & PTE_PS) {
            if (create == WALK_LOOKUP)
                return e;   /* caller sees a large leaf */
            panic("vmm: remapping inside a large page at %lx", va);
        }
        t = table(*e);
    }
    return &t[(va >> (12 + 9 * (level - 1))) & 511];
}

void vmm_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t len, unsigned flags)
{
    ASSERT(!(va & (PAGE_SIZE - 1)) && !(pa & (PAGE_SIZE - 1)));
    len = ALIGN_UP(len, PAGE_SIZE);
    uint64_t f = spin_lock_irqsave(&pt_lock);
    while (len) {
        int level = 1;
        uint64_t size = PAGE_SIZE;
        if (!(flags & VM_SMALL)) {
            if (cpu_features.pages_1g && !((va | pa) & (SIZE_1G - 1)) && len >= SIZE_1G)
                level = 3, size = SIZE_1G;
            else if (!((va | pa) & (SIZE_2M - 1)) && len >= SIZE_2M)
                level = 2, size = SIZE_2M;
        }
        uint64_t *e = walk(pml4, va, level, WALK_CREATE);
        *e = pa | leaf_bits(flags, level > 1) | (level > 1 ? PTE_PS : 0);
        invlpg(va);
        va += size;
        pa += size;
        len -= size;
    }
    spin_unlock_irqrestore(&pt_lock, f);
}

void vmm_unmap(uint64_t pml4, uint64_t va, uint64_t len)
{
    uint64_t start = va, end = va + ALIGN_UP(len, PAGE_SIZE);
    bool kernel = start >= 0xffff800000000000ull && __atomic_load_n(&ipi_ready, __ATOMIC_ACQUIRE);
    if (kernel && !irqs_enabled())
        panic("vmm: kernel unmap with interrupts off can't shoot down TLBs");

    uint64_t f = spin_lock_irqsave(&pt_lock);
    for (; va < end; va += PAGE_SIZE) {
        uint64_t *e = walk(pml4, va, 1, WALK_LOOKUP);
        if (!e)
            continue;
        if (*e & PTE_PS)
            panic("vmm: unmap of a large page at %lx not supported", va);
        *e = 0;
    }
    spin_unlock_irqrestore(&pt_lock, f);

    /* Flush the removed range from every CPU's TLB, INCLUDING this one, with
     * preemption disabled across the whole decision. Doing the local flush
     * and then shooting down only the OTHER CPUs would let a migration in
     * between move us to a CPU that never got flushed, so a use after unmap
     * would silently keep working. Keeping preemption off pins "this CPU" so
     * the local flush plus the remote shootdown together cover everyone
     * (test: repro_unmap_migrate_stale_tlb). For a non-kernel or pre-SMP
     * unmap only the local flush is needed. */
    preempt_disable();
    tlb_flush_local(start, end - start);
    if (kernel) {
        DBG_HOOK(DBG_UNMAP_PRE_SHOOT, NULL);
        tlb_shootdown(start, end - start);
    }
    preempt_enable();
}

uint64_t vmm_translate(uint64_t pml4, uint64_t va)
{
    uint64_t *t = table(pml4);
    for (int l = 4; l >= 1; l--) {
        uint64_t e = t[(va >> (12 + 9 * (l - 1))) & 511];
        if (!(e & PTE_P))
            return UINT64_MAX;
        if (l == 1 || (e & PTE_PS)) {
            uint64_t size = 1ull << (12 + 9 * (l - 1));
            uint64_t base = e & PTE_ADDR & ~(size - 1);
            if (l > 1)
                base &= ~PTE_PATLG;   /* bit 12 is PAT, not address, in large pages */
            return base + (va & (size - 1));
        }
        t = table(e);
    }
    return UINT64_MAX;
}

int vmm_access(uint64_t pml4, uint64_t va)
{
    uint64_t *t = table(pml4);
    bool w = true, x = true;
    for (int l = 4; l >= 1; l--) {
        uint64_t e = t[(va >> (12 + 9 * (l - 1))) & 511];
        if (!(e & PTE_P))
            return -1;
        w &= !!(e & PTE_W);
        x &= !(e & PTE_NX);
        if (l == 1 || (e & PTE_PS))
            return (w ? VM_WRITE : 0) | (x ? VM_EXEC : 0);
        t = table(e);
    }
    return -1;
}

const char *vmm_cache_type(uint64_t pml4, uint64_t va)
{
    static const char *const names[8] = { "UC", "WC", "?", "?", "WT", "WP", "WB", "UC-" };
    uint64_t *t = table(pml4);
    for (int l = 4; l >= 1; l--) {
        uint64_t e = t[(va >> (12 + 9 * (l - 1))) & 511];
        if (!(e & PTE_P))
            return "unmapped";
        if (l == 1 || (e & PTE_PS)) {
            bool pat = l == 1 ? (e & PTE_PAT4K) : (e & PTE_PATLG);
            unsigned idx = (pat ? 4 : 0) | ((e & PTE_PCD) ? 2 : 0) | ((e & PTE_PWT) ? 1 : 0);
            uint8_t type = (rdmsr(MSR_PAT) >> (idx * 8)) & 7;
            return names[type];
        }
        t = table(e);
    }
    return "unmapped";
}

uint64_t vmm_kernel_pml4(void)
{
    return kernel_pml4;
}

uint64_t *vmm_kernel_ptes(uint64_t va)
{
    ASSERT(!(va & (SIZE_2M - 1)) && va >= VMAP_BASE && va < VMAP_END);
    uint64_t f = spin_lock_irqsave(&pt_lock);
    uint64_t *e = walk(kernel_pml4, va, 1, WALK_CREATE);
    spin_unlock_irqrestore(&pt_lock, f);
    return e;   /* entry 0 of its table: the table is 2 MiB of va */
}

uint64_t vmm_pte(uint64_t pa, unsigned flags, bool large)
{
    return pa | leaf_bits(flags, large) | (large ? PTE_PS : 0);
}

uint64_t vmm_pte_table(uint64_t pa)
{
    return pa | PTE_P | PTE_W;
}

static void map_kernel_section(const struct boot_info *bi, const char *start, const char *end,
                               unsigned flags)
{
    uint64_t va = ALIGN_DOWN((uint64_t)start, PAGE_SIZE);
    uint64_t pa = va - bi->kernel_virt_base + bi->kernel_phys_base;
    vmm_map(kernel_pml4, va, pa, ALIGN_UP((uint64_t)end, PAGE_SIZE) - va,
            flags | VM_GLOBAL | VM_SMALL);
}

static bool chunk_has_ram(const struct boot_info *bi, uint64_t first_pfn, uint64_t end_pfn)
{
    for (size_t i = 0; i < bi->memmap_count; i++) {
        const struct boot_mem_region *r = &bi->memmap[i];
        if (!boot_mem_is_ram(r->type))
            continue;
        uint64_t rs = r->base >> PAGE_SHIFT;
        uint64_t re = (r->base + r->length + PAGE_SIZE - 1) >> PAGE_SHIFT;
        if (rs < end_pfn && re > first_pfn)
            return true;
    }
    return false;
}

/* Each 2 MiB chunk of vmemmap describes 256 MiB of physical memory. Chunks
 * over pure holes stay unmapped; buddy blocks (<= 4 MiB, aligned) never
 * straddle chunks, so merging never reads an unbacked struct page. */
static void map_vmemmap(const struct boot_info *bi)
{
    uint64_t bytes = ALIGN_UP(pmm_max_pfn() * sizeof(struct page), SIZE_2M);
    uint64_t pages_per_chunk = SIZE_2M / sizeof(struct page);
    uint64_t backed = 0;

    for (uint64_t off = 0; off < bytes; off += SIZE_2M) {
        uint64_t first_pfn = off / sizeof(struct page);
        if (!chunk_has_ram(bi, first_pfn, first_pfn + pages_per_chunk))
            continue;
        uint64_t pa = pmm_early_alloc(SIZE_2M, SIZE_2M);
        vmm_map(kernel_pml4, VMEMMAP_BASE + off, pa, SIZE_2M, VM_WRITE | VM_GLOBAL);
        struct page *pages = phys_to_virt(pa);
        for (uint64_t i = 0; i < pages_per_chunk; i++)
            pages[i].flags = PG_RESERVED;
        backed += SIZE_2M;
    }
    kprintf("vmm: vmemmap %lu MiB for %lu pages\n", backed >> 20, pmm_max_pfn());
}

void vmm_init(const struct boot_info *bi)
{
    kernel_pml4 = alloc_table();

    /* Pre-create every kernel-half PDPT so all future address spaces can
     * share PML4 entries 256..511 by copying them once. */
    uint64_t *pml4 = table(kernel_pml4);
    for (int i = 256; i < 512; i++)
        pml4[i] = alloc_table() | PTE_P | PTE_W;

    map_kernel_section(bi, __text_start, __text_end, VM_EXEC);
    map_kernel_section(bi, __kernel_start, __text_start, 0);   /* limine requests */
    map_kernel_section(bi, __rodata_start, __rodata_end, 0);
    map_kernel_section(bi, __data_start, __data_end, VM_WRITE);
    map_kernel_section(bi, __ksyms_start, __kernel_end, 0);

    for (size_t i = 0; i < bi->memmap_count; i++) {
        const struct boot_mem_region *r = &bi->memmap[i];
        /* Foreign RAM (the crash kernel's region, or a crashed kernel's
         * memory) gets no mapping at all, so no wild write can reach it. */
        if (r->type == BOOT_MEM_RESERVED || r->type == BOOT_MEM_BAD ||
            r->type == BOOT_MEM_FRAMEBUFFER || r->type == BOOT_MEM_FOREIGN)
            continue;
        uint64_t base = ALIGN_DOWN(r->base, PAGE_SIZE);
        uint64_t end = ALIGN_UP(r->base + r->length, PAGE_SIZE);
        /* The kernel image and boot modules are read-only through the HHDM:
         * otherwise kernel text would be writable through this alias, and
         * W^X only holds if it holds for every mapping of a page. A crashed
         * kernel's log is only read. */
        unsigned flags = r->type == BOOT_MEM_KERNEL_AND_MODULES ||
                         r->type == BOOT_MEM_CRASH_LOG ? 0 : VM_WRITE;
        vmm_map(kernel_pml4, base + hhdm_offset, base, end - base, flags | VM_GLOBAL);
    }

    if (bi->fb.virt) {
        uint64_t base = ALIGN_DOWN(bi->fb.phys, PAGE_SIZE);
        uint64_t end = ALIGN_UP(bi->fb.phys + (uint64_t)bi->fb.pitch * bi->fb.height, PAGE_SIZE);
        vmm_map(kernel_pml4, base + hhdm_offset, base, end - base,
                VM_WRITE | VM_WC | VM_GLOBAL);
    }

    map_vmemmap(bi);

    cpu_enable_paging_features();
    write_cr3(kernel_pml4);
    kprintf("vmm: switched to kernel page tables (pml4 %lx)\n", kernel_pml4);
}

/* Called once the buddy allocator is up. */
void vmm_use_buddy(void)
{
    use_buddy = true;
}

static uint64_t vmap_reserve_raw(uint64_t len);

uint64_t vmm_reserve(uint64_t len)
{
    return vmap_reserve_raw(ALIGN_UP(len, PAGE_SIZE) + PAGE_SIZE);   /* + guard gap */
}

static uint64_t vmap_reserve_raw(uint64_t len)
{
    uint64_t f = spin_lock_irqsave(&vmap_lock);
    uint64_t va = vmap_next;
    vmap_next += len;
    spin_unlock_irqrestore(&vmap_lock, f);
    if (vmap_next > VMAP_END)
        panic("vmm: vmap area exhausted");
    return va;
}

/* ---- kernel stacks ----------------------------------------------------------
 *
 * The vmap area is a bump allocator, so a freed stack's virtual range is kept
 * on this list and handed to the next stack of the same size: without that,
 * thread churn beyond the scheduler's stack cache would walk the bump pointer
 * forward for ever and strand a page table per 2 MiB of it. A range on the
 * list is unmapped (its TLB entries were shot down when it was freed) but its
 * page tables stay, so reusing it needs no new tables. Guarded by vmap_lock. */
struct vslot {
    struct vslot *next;   /* next free slot */
    uint64_t      va;     /* lowest mapped byte (the guard page is below) */
    uint64_t      size;   /* bytes */
};
static struct vslot *free_slots;

/* A free range of exactly `size` bytes (plus its guard page), or NULL. */
static struct vslot *slot_take(uint64_t size)
{
    uint64_t f = spin_lock_irqsave(&vmap_lock);
    struct vslot **pp = &free_slots;
    while (*pp && (*pp)->size != size)
        pp = &(*pp)->next;
    struct vslot *s = *pp;
    if (s)
        *pp = s->next;
    spin_unlock_irqrestore(&vmap_lock, f);
    return s;
}

static void slot_put(struct vslot *s)
{
    uint64_t f = spin_lock_irqsave(&vmap_lock);
    s->next = free_slots;
    free_slots = s;
    spin_unlock_irqrestore(&vmap_lock, f);
}

/* Map the chain of pages (linked through page->private, `n` of them) at va.
 * Every page table is created BEFORE any leaf is written, so a failure
 * leaves no mapping behind to undo (and nothing another CPU could have
 * cached). */
static bool map_stack_pages(uint64_t va, const struct page *chain, uint64_t n, bool may_fail)
{
    uint64_t f = spin_lock_irqsave(&pt_lock);
    for (uint64_t i = 0; i < n; i++)
        if (!walk(kernel_pml4, va + i * PAGE_SIZE, 1, may_fail ? WALK_TRY : WALK_CREATE)) {
            spin_unlock_irqrestore(&pt_lock, f);
            return false;
        }
    uint64_t bits = leaf_bits(VM_WRITE | VM_GLOBAL, false);
    for (uint64_t i = 0; i < n; i++, chain = (struct page *)chain->private) {
        uint64_t a = va + i * PAGE_SIZE;
        *walk(kernel_pml4, a, 1, WALK_LOOKUP) = page_to_phys(chain) | bits;
        invlpg(a);
    }
    spin_unlock_irqrestore(&pt_lock, f);
    return true;
}

static void free_chain(struct page *chain)
{
    while (chain) {
        struct page *next = (struct page *)chain->private;
        pmm_free_pages(chain, 0);
        chain = next;
    }
}

static void *stack_alloc(size_t size, bool may_fail)
{
    size = ALIGN_UP(size, PAGE_SIZE);
    uint64_t n = size / PAGE_SIZE;
    /* Pages first: that is where running out of memory is likely, and
     * nothing is mapped yet to undo. */
    struct page *chain = NULL;
    for (uint64_t i = 0; i < n; i++) {
        struct page *p = pmm_alloc_pages(0, PMM_ZERO);
        if (!p) {
            if (!may_fail)
                panic("vmm: out of memory for kernel stack");
            free_chain(chain);
            return NULL;
        }
        p->private = (uint64_t)chain;
        chain = p;
    }
    struct vslot *slot = slot_take(size);
    uint64_t va = slot ? slot->va : vmap_reserve_raw(size + PAGE_SIZE) + PAGE_SIZE;
    if (!map_stack_pages(va, chain, n, may_fail)) {
        free_chain(chain);
        if (!slot)
            slot = kmalloc(sizeof(*slot));   /* keep the range; if this fails
                                              * too, the range is just lost */
        if (slot) {
            slot->va = va;
            slot->size = size;
            slot_put(slot);
        }
        return NULL;
    }
    for (struct page *p = chain; p;) {   /* the pages are ours: clear the links */
        struct page *next = (struct page *)p->private;
        p->private = 0;
        p = next;
    }
    kfree(slot);
    return (void *)(va + size);
}

void *kstack_alloc(size_t size)
{
    return stack_alloc(size, false);
}

void *kstack_alloc_try(size_t size)
{
    return stack_alloc(size, true);
}

void kstack_free(void *top, size_t size)
{
    size = ALIGN_UP(size, PAGE_SIZE);
    uint64_t va = (uint64_t)top - size;
    /* The node first: if it can't be had, the range is lost but the pages
     * still go back. */
    struct vslot *slot = kmalloc(sizeof(*slot));
    /* Read the physical pages before the unmap, free them only after it:
     * vmm_unmap has shot the range down on every CPU by then, so no stale
     * TLB entry can reach a page that is being reused. Chunks keep the
     * list on this stack small. */
    enum { CHUNK = 32 };
    for (uint64_t off = 0; off < size; off += CHUNK * PAGE_SIZE) {
        uint64_t pas[CHUNK], len = size - off < CHUNK * PAGE_SIZE ? size - off : CHUNK * PAGE_SIZE;
        for (uint64_t i = 0; i < len / PAGE_SIZE; i++) {
            pas[i] = vmm_translate(kernel_pml4, va + off + i * PAGE_SIZE);
            ASSERT(pas[i] != UINT64_MAX);
        }
        vmm_unmap(kernel_pml4, va + off, len);
        for (uint64_t i = 0; i < len / PAGE_SIZE; i++)
            pmm_free_page_phys(pas[i]);
    }
    if (slot) {
        slot->va = va;
        slot->size = size;
        slot_put(slot);
    }
}

void *vmm_map_mmio(uint64_t pa, uint64_t len)
{
    uint64_t off = pa & (PAGE_SIZE - 1);
    len = ALIGN_UP(len + off, PAGE_SIZE);
    uint64_t va = vmap_reserve_raw(len + PAGE_SIZE);   /* unmapped gap after */
    vmm_map(kernel_pml4, va, pa - off, len, VM_WRITE | VM_UC | VM_GLOBAL | VM_SMALL);
    return (void *)(va + off);
}
