/* Kernel page tables. vmm_init builds a fresh PML4 (no dependency on the
 * loader's tables), maps the kernel image with per-section permissions, the
 * HHDM (RAM write-back, framebuffer write-combining) and the vmemmap, then
 * switches CR3. */
#include <jam/cpu.h>
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
static spinlock_t vmap_lock = SPINLOCK_INIT;
static uint64_t vmap_next = VMAP_BASE;

static uint64_t alloc_table(void)
{
    if (!use_buddy)
        return pmm_early_alloc(PAGE_SIZE, PAGE_SIZE);
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    if (!pa)
        panic("vmm: out of memory for page tables");
    return pa;
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
 * intermediate tables. */
static uint64_t *walk(uint64_t pml4, uint64_t va, int level, bool create)
{
    uint64_t *t = table(pml4);
    for (int l = 4; l > level; l--) {
        uint64_t *e = &t[(va >> (12 + 9 * (l - 1))) & 511];
        if (!(*e & PTE_P)) {
            if (!create)
                return NULL;
            /* Intermediate entries are permissive; leaves decide access. */
            *e = alloc_table() | PTE_P | PTE_W | PTE_U;
        } else if (*e & PTE_PS) {
            if (!create)
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
    while (len) {
        int level = 1;
        uint64_t size = PAGE_SIZE;
        if (!(flags & VM_SMALL)) {
            if (cpu_features.pages_1g && !((va | pa) & (SIZE_1G - 1)) && len >= SIZE_1G)
                level = 3, size = SIZE_1G;
            else if (!((va | pa) & (SIZE_2M - 1)) && len >= SIZE_2M)
                level = 2, size = SIZE_2M;
        }
        uint64_t *e = walk(pml4, va, level, true);
        *e = pa | leaf_bits(flags, level > 1) | (level > 1 ? PTE_PS : 0);
        invlpg(va);
        va += size;
        pa += size;
        len -= size;
    }
}

void vmm_unmap(uint64_t pml4, uint64_t va, uint64_t len)
{
    for (uint64_t end = va + ALIGN_UP(len, PAGE_SIZE); va < end; va += PAGE_SIZE) {
        uint64_t *e = walk(pml4, va, 1, false);
        if (!e)
            continue;
        if (*e & PTE_PS)
            panic("vmm: unmap of a large page at %lx not supported", va);
        *e = 0;
        invlpg(va);   /* M3: plus a shootdown IPI to other CPUs */
    }
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

uint64_t vmm_kernel_pml4(void)
{
    return kernel_pml4;
}

static void map_kernel_section(const struct boot_info *bi, char *start, char *end, unsigned flags)
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
        if (r->type == BOOT_MEM_RESERVED || r->type == BOOT_MEM_BAD ||
            r->type == BOOT_MEM_FRAMEBUFFER)
            continue;
        uint64_t base = ALIGN_DOWN(r->base, PAGE_SIZE);
        uint64_t end = ALIGN_UP(r->base + r->length, PAGE_SIZE);
        vmm_map(kernel_pml4, base + hhdm_offset, base, end - base, VM_WRITE | VM_GLOBAL);
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

static uint64_t vmap_reserve(uint64_t len)
{
    spin_lock(&vmap_lock);
    uint64_t va = vmap_next;
    vmap_next += len;
    spin_unlock(&vmap_lock);
    if (vmap_next > VMAP_END)
        panic("vmm: vmap area exhausted");
    return va;
}

void *kstack_alloc(size_t size)
{
    size = ALIGN_UP(size, PAGE_SIZE);
    uint64_t va = vmap_reserve(size + PAGE_SIZE) + PAGE_SIZE;   /* guard below */
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
        if (!pa)
            panic("vmm: out of memory for kernel stack");
        vmm_map(kernel_pml4, va + off, pa, PAGE_SIZE, VM_WRITE | VM_GLOBAL | VM_SMALL);
    }
    return (void *)(va + size);
}

void *vmm_map_mmio(uint64_t pa, uint64_t len)
{
    uint64_t off = pa & (PAGE_SIZE - 1);
    len = ALIGN_UP(len + off, PAGE_SIZE);
    uint64_t va = vmap_reserve(len + PAGE_SIZE);   /* unmapped gap after */
    vmm_map(kernel_pml4, va, pa - off, len, VM_WRITE | VM_UC | VM_GLOBAL | VM_SMALL);
    return (void *)(va + off);
}
