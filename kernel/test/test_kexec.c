/* kexec: the checksum, memory-map splitting, the handoff's check (every
 * field a corrupted handoff could get wrong), the region unmapped from
 * every kernel page table, the loaded crash kernel intact, and a refused
 * image changing nothing. */
#include <stddef.h>
#include <jam/kexec.h>
#include <jam/kexec_handoff.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/vmo.h>

/* x86-64 page-table entry bits, for the walk below. */
#define PTE_P    (1ull << 0)
#define PTE_PS   (1ull << 7)
#define PTE_ADDR 0x000ffffffffff000ull

KTEST(kexec_checksum_words)
{
    uint64_t buf[64];
    for (unsigned i = 0; i < 64; i++)
        buf[i] = i * 0x9e3779b97f4a7c15ull;
    uint64_t sum = kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED);
    KT_EQ(kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED), sum);
    for (unsigned i = 0; i < 64; i += 7) {
        uint8_t *b = (uint8_t *)&buf[i] + (i % 8);
        *b ^= 1;   /* one bit of one word */
        KT_ASSERT(kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED) != sum);
        *b ^= 1;
    }
    KT_EQ(kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED), sum);
    /* Two halves chained are the whole. */
    uint64_t half = kexec_checksum(buf, sizeof(buf) / 2, KEXEC_SUM_SEED);
    KT_EQ(kexec_checksum(buf + 32, sizeof(buf) / 2, half), sum);
    /* A struct's own checksum field counts as 0. */
    uint64_t at = 8 * 5, keep = buf[5];
    uint64_t s1 = kexec_struct_sum(buf, sizeof(buf), at);
    buf[5] = 12345;
    KT_EQ(kexec_struct_sum(buf, sizeof(buf), at), s1);
    buf[5] = 0;
    KT_EQ(kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED), s1);
    buf[5] = keep;
}

static bool map_is(const struct boot_mem_region *m, size_t n, const uint64_t (*want)[3],
                   size_t wn)
{
    if (n != wn)
        return false;
    for (size_t i = 0; i < n; i++)
        if (m[i].base != want[i][0] || m[i].length != want[i][1] || m[i].type != want[i][2])
            return false;
    return true;
}

KTEST(kexec_memmap_overlay)
{
#define U BOOT_MEM_USABLE
#define R BOOT_MEM_RESERVED
#define F BOOT_MEM_FOREIGN
    struct boot_mem_region m[8] = {
        { 0x0000, 0x1000, R }, { 0x1000, 0x9000, U }, { 0xa000, 0x2000, R },
    };
    size_t n = 3;
    /* Inside one entry: split into three. */
    KT_EQ(kexec_memmap_overlay(m, &n, 8, 0x3000, 0x2000, F), OK);
    static const uint64_t a[][3] = { { 0, 0x1000, R }, { 0x1000, 0x2000, U },
                                     { 0x3000, 0x2000, F }, { 0x5000, 0x5000, U },
                                     { 0xa000, 0x2000, R } };
    KT_ASSERT(map_is(m, n, a, 5));
    /* Across two entries, to the end of the second's. */
    KT_EQ(kexec_memmap_overlay(m, &n, 8, 0x8000, 0x4000, F), OK);
    static const uint64_t b[][3] = { { 0, 0x1000, R }, { 0x1000, 0x2000, U },
                                     { 0x3000, 0x2000, F }, { 0x5000, 0x3000, U },
                                     { 0x8000, 0x2000, F }, { 0xa000, 0x2000, F } };
    KT_ASSERT(map_is(m, n, b, 6));
    kexec_memmap_merge(m, &n);
    static const uint64_t c[][3] = { { 0, 0x1000, R }, { 0x1000, 0x2000, U },
                                     { 0x3000, 0x2000, F }, { 0x5000, 0x3000, U },
                                     { 0x8000, 0x4000, F } };
    KT_ASSERT(map_is(m, n, c, 5));
    /* Outside every entry: nothing changes. */
    KT_EQ(kexec_memmap_overlay(m, &n, 8, 0x100000, 0x1000, F), OK);
    KT_ASSERT(map_is(m, n, c, 5));
    /* Over the cap: refused, unchanged. */
    KT_EQ(kexec_memmap_overlay(m, &n, 6, 0x1800, 0x5000, R), ERR_NO_RESOURCES);
    KT_ASSERT(map_is(m, n, c, 5));
    /* A range that wraps: refused. */
    KT_EQ(kexec_memmap_overlay(m, &n, 8, ~0ull - 10, 0x1000, F), ERR_INVALID_ARGS);
#undef U
#undef R
#undef F
}

/* A handoff kexec_handoff_check accepts. */
static void valid_handoff(struct kexec_handoff *h)
{
    memset(h, 0, sizeof(*h));
    h->magic = KEXEC_HANDOFF_MAGIC;
    h->version = KEXEC_HANDOFF_VERSION;
    h->size = sizeof(*h);
    h->hhdm_offset = 0xffff800000000000ull;
    h->kernel_virt_base = 0xffffffff80000000ull;
    h->cpu_count = 1;
    h->cpus[0] = (struct kexec_cpu){ 0, 0 };
    h->memmap_count = 1;
    h->memmap[0] = (struct kexec_mem){ 0x100000, 0x1000000, KEXEC_MEM_USABLE, 0 };
    h->module_count = 1;
    h->modules[0] = (struct kexec_module){ .phys = 0x200000, .size = 0x1000 };
    memcpy(h->loader_name, "test", 5);
    memcpy(h->cmdline, "shell", 6);
}

static void reseal(struct kexec_handoff *h)
{
    h->checksum = kexec_struct_sum(h, sizeof(*h), offsetof(struct kexec_handoff, checksum));
}

KTEST(kexec_handoff_check)
{
    struct kexec_handoff *h = kzalloc(sizeof(*h));
    KT_ASSERT(h);
    valid_handoff(h);
    reseal(h);
    KT_ASSERT(kexec_handoff_check(h, sizeof(*h)) == NULL);
    KT_ASSERT(kexec_handoff_check(h, sizeof(*h) - 8) != NULL);   /* too short */
    h->cmdline[0] = 'S';   /* changed after the checksum */
    KT_ASSERT(kexec_handoff_check(h, sizeof(*h)) != NULL);

    /* Each one wrong, with a checksum that matches: the field checks. */
    for (int k = 0; k < 14; k++) {
        valid_handoff(h);
        switch (k) {
        case 0:  h->magic ^= 1; break;
        case 1:  h->version = 2; break;
        case 2:  h->size -= 8; break;
        case 3:  h->flags = 1u << 5; break;
        case 4:  h->x2apic = 2; break;
        case 5:  h->cpu_count = 0; break;
        case 6:  h->memmap_count = KEXEC_MAX_MEMMAP + 1; break;
        case 7:  h->memmap[0].type = 99; break;
        case 8:  h->memmap[0].base = ~0ull - 4; break;   /* wraps */
        case 9:  memset(h->cmdline, 'a', KEXEC_CMDLINE); break;
        case 10: memset(h->modules[0].path, 'a', KEXEC_STR); break;
        case 11: h->fb = (struct kexec_fb){ .phys = 0x80000000, .width = 0, .height = 600 };
                 break;
        case 12: h->hhdm_offset = 0x1000; break;
        case 13: h->module_count = KEXEC_MAX_MODULES + 1; break;
        }
        reseal(h);
        if (kexec_handoff_check(h, sizeof(*h)) == NULL)
            kprintf("kexec_handoff_check: corruption %d was accepted\n", k);
        KT_ASSERT(kexec_handoff_check(h, sizeof(*h)) != NULL);
    }
    kfree(h);
}

/* Does any leaf of the kernel half of the kernel's tables map a page of
 * [base, base + size)? Walks every present entry (no recursion: four
 * nested loops, one per level). */
static bool kernel_maps(uint64_t base, uint64_t size)
{
    const uint64_t *l4 = phys_to_virt(vmm_kernel_pml4());
    for (unsigned a = 256; a < 512; a++) {
        if (!(l4[a] & PTE_P))
            continue;
        const uint64_t *l3 = phys_to_virt(l4[a] & PTE_ADDR);
        for (unsigned b = 0; b < 512; b++) {
            if (!(l3[b] & PTE_P))
                continue;
            uint64_t pa = l3[b] & PTE_ADDR & ~((1ull << 30) - 1);
            if (l3[b] & PTE_PS) {
                if (pa < base + size && base < pa + (1ull << 30))
                    return true;
                continue;
            }
            const uint64_t *l2 = phys_to_virt(l3[b] & PTE_ADDR);
            for (unsigned c = 0; c < 512; c++) {
                if (!(l2[c] & PTE_P))
                    continue;
                pa = l2[c] & PTE_ADDR & ~((1ull << 21) - 1);
                if (l2[c] & PTE_PS) {
                    if (pa < base + size && base < pa + (1ull << 21))
                        return true;
                    continue;
                }
                const uint64_t *l1 = phys_to_virt(l2[c] & PTE_ADDR);
                for (unsigned d = 0; d < 512; d++)
                    if ((l1[d] & PTE_P) && (l1[d] & PTE_ADDR) >= base &&
                        (l1[d] & PTE_ADDR) < base + size)
                        return true;
            }
        }
    }
    return false;
}

KTEST(kexec_region_unmapped)
{
    uint64_t base, size;
    if (!kexec_region(&base, &size)) {
        kprintf("kexec_region_unmapped: no region on this boot (crashkernel=0): nothing to check\n");
        return;
    }
    KT_ASSERT(!(base & ((2ull << 20) - 1)) && size >= (32ull << 20));
    KT_ASSERT(base + size <= (4ull << 30));
    /* RAM to the rules that keep MMIO off it, not RAM the PMM manages. */
    KT_ASSERT(pmm_range_has_ram(base, size));
    /* Not in the HHDM: every 2 MiB, and the first and last page. */
    for (uint64_t off = 0; off < size; off += 2ull << 20)
        KT_EQ(vmm_translate(vmm_kernel_pml4(), hhdm_offset + base + off), UINT64_MAX);
    KT_EQ(vmm_translate(vmm_kernel_pml4(), hhdm_offset + base + size - PAGE_SIZE), UINT64_MAX);
    /* Nor anywhere else in the kernel's half (which every address space
     * shares). The walk does find what is mapped: this function's page. */
    KT_ASSERT(!kernel_maps(base, size));
    uint64_t self = vmm_translate(vmm_kernel_pml4(), (uint64_t)(uintptr_t)&kernel_maps);
    KT_ASSERT(self != UINT64_MAX && kernel_maps(ALIGN_DOWN(self, PAGE_SIZE), PAGE_SIZE));
}

KTEST(kexec_crash_kernel_intact)
{
    if (!kexec_crash_armed()) {
        kprintf("kexec_crash_kernel_intact: no crash kernel armed on this boot: nothing to check\n");
        return;
    }
    KT_ASSERT(kexec_verify());
}

KTEST(kexec_load_refuses_garbage)
{
    bool armed = kexec_crash_armed();
    struct vmo *k, *b;
    KT_EQ(vmo_create(8192, 0, &k), OK);
    KT_EQ(vmo_create(8192, 0, &b), OK);
    static const char junk[] = "\x7f" "ELF but not really";
    KT_EQ(vmo_write(k, 0, junk, sizeof(junk)), OK);
    status_t st = kexec_load_image(k, b, "");
    uint64_t base, size;
    KT_EQ(st, kexec_region(&base, &size) ? ERR_INVALID_ARGS : ERR_NOT_SUPPORTED);
    /* A refused image changes nothing. */
    KT_EQ(kexec_crash_armed(), armed);
    if (armed)
        KT_ASSERT(kexec_verify());
    kobject_unref(vmo_kobject(b));
    kobject_unref(vmo_kobject(k));
}
