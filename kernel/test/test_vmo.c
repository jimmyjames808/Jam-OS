/* Tests for VMOs, DMA pinning and the VMO handle layer. Page accounting is
 * checked against pmm_stats; tests that count pages exactly create no
 * threads (the scheduler caches exited threads' stacks). */
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/vmo.h>

#define PG PAGE_SIZE
#define FOUR_GIB (4ull << 30)

static uint64_t free_now(void)
{
    uint64_t total, free;
    pmm_stats(&total, &free);
    return free;
}

static void fill(uint8_t *buf, uint64_t len, uint32_t seed)
{
    for (uint64_t i = 0; i < len; i++)
        buf[i] = (uint8_t)((i * 131 + seed * 7 + (i >> 12)) ^ seed);
}

static bool all_zero(const uint8_t *buf, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++)
        if (buf[i])
            return false;
    return true;
}

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

/* A VMO kept alive across a measurement, so the kmalloc slab holding
 * struct vmo is already there and doesn't show up in the counts. */
static struct vmo *slab_keeper(void)
{
    struct vmo *k;
    KT_EQ(vmo_create(PG, 0, &k), OK);
    return k;
}

KTEST(vmo_rw_basic)
{
    uint64_t base = free_now();
    struct vmo *v;
    KT_EQ(vmo_create(3 * PG + 10, 0, &v), OK);
    KT_EQ(vmo_size(v), 4 * PG);   /* rounded up */
    KT_EQ(vmo_committed(v), 0);

    /* Across two page boundaries. */
    enum { LEN = 5000 };
    static uint8_t in[LEN], out[LEN];
    fill(in, LEN, 1);
    KT_EQ(vmo_write(v, 4000, in, LEN), OK);
    KT_EQ(vmo_committed(v), 3 * PG);   /* pages 0-2 (bytes 4000..8999) */
    memset(out, 0xaa, LEN);
    KT_EQ(vmo_read(v, 4000, out, LEN), OK);
    KT_ASSERT(!memcmp(in, out, LEN));
    /* Bytes around it are still zero. */
    KT_EQ(vmo_read(v, 0, out, 4000), OK);
    KT_ASSERT(all_zero(out, 4000));
    KT_EQ(vmo_read(v, 9000, out, 4 * PG - 9000), OK);
    KT_ASSERT(all_zero(out, 4 * PG - 9000));

    /* Bounds. */
    KT_EQ(vmo_read(v, 4 * PG - 1, out, 2), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_write(v, 4 * PG, in, 1), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_read(v, 4 * PG, out, 0), OK);
    KT_EQ(vmo_read(v, UINT64_MAX, out, 2), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_read(v, 8, out, UINT64_MAX - 4), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_commit(v, 0, 5 * PG), ERR_OUT_OF_RANGE);

    /* Creation arguments. */
    struct vmo *bad;
    KT_EQ(vmo_create(VMO_MAX_SIZE + 1, 0, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_create(PG, 1u << 7, &bad), ERR_INVALID_ARGS);
    KT_EQ(vmo_create(0, VMO_CONTIGUOUS, &bad), ERR_INVALID_ARGS);
    KT_EQ(vmo_create(8ull << 20, VMO_CONTIGUOUS, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_create(0, 0, &bad), OK);   /* empty is fine */
    KT_EQ(vmo_size(bad), 0);
    KT_EQ(vmo_read(bad, 0, out, 1), ERR_OUT_OF_RANGE);
    put(bad);

    put(v);
    KT_GLOBAL_EQ(free_now(), base);
}

KTEST(vmo_uncommitted_reads_zero)
{
    struct vmo *v;
    KT_EQ(vmo_create(1 << 20, 0, &v), OK);
    uint64_t before = free_now();
    static uint8_t buf[3 * 4096];
    memset(buf, 0x5a, sizeof(buf));
    KT_EQ(vmo_read(v, 12345, buf, sizeof(buf)), OK);
    KT_ASSERT(all_zero(buf, sizeof(buf)));
    KT_EQ(vmo_read(v, (1 << 20) - 1, buf, 1), OK);
    KT_EQ(buf[0], 0);
    KT_GLOBAL_EQ(free_now(), before);   /* no pages, not even tables */
    KT_EQ(vmo_committed(v), 0);
    put(v);
}

KTEST(vmo_sparse_16g)
{
    struct vmo *keep = slab_keeper();
    uint64_t base = free_now();
    struct vmo *v;
    KT_EQ(vmo_create(16ull << 30, 0, &v), OK);
    KT_GLOBAL_EQ(free_now(), base);   /* creating it costs no pages */

    static const uint64_t offs[] = { 0, (5ull << 30) + 123, (10ull << 30) + 7,
                                     (16ull << 30) - 1 };
    for (unsigned i = 0; i < 4; i++) {
        uint8_t b = (uint8_t)(0x11 * (i + 1));
        KT_EQ(vmo_write(v, offs[i], &b, 1), OK);
    }
    /* Four data pages, each in its own 1 GiB: a mid table and a leaf each. */
    KT_EQ(vmo_committed(v), 4 * PG);
    KT_GLOBAL_EQ(base - free_now(), 4 + 4 + 4);
    for (unsigned i = 0; i < 4; i++) {
        uint8_t b = 0;
        KT_EQ(vmo_read(v, offs[i], &b, 1), OK);
        KT_EQ(b, 0x11 * (i + 1));
    }
    uint8_t z = 1;
    KT_EQ(vmo_read(v, 7ull << 30, &z, 1), OK);
    KT_EQ(z, 0);
    KT_GLOBAL_EQ(base - free_now(), 12);

    put(v);
    KT_GLOBAL_EQ(free_now(), base);
    put(keep);
}

KTEST(vmo_commit_accounting)
{
    uint64_t base = free_now();
    struct vmo *v;
    KT_EQ(vmo_create(8 << 20, 0, &v), OK);   /* 8 MiB: 4 leaves' worth */
    uint64_t created = free_now();

    /* 100 pages straddling the first 2 MiB boundary: 1 mid + 2 leaves. */
    uint64_t off = (2 << 20) - 50 * PG;
    KT_EQ(vmo_commit(v, off, 100 * PG), OK);
    KT_GLOBAL_EQ(created - free_now(), 100 + 3);
    KT_EQ(vmo_committed(v), 100 * PG);
    KT_EQ(vmo_commit(v, off, 100 * PG), OK);   /* again: nothing new */
    KT_GLOBAL_EQ(created - free_now(), 100 + 3);

    /* Byte ranges cover every page they touch. */
    KT_EQ(vmo_commit(v, 1, 1), OK);
    KT_EQ(vmo_committed(v), 101 * PG);
    KT_GLOBAL_EQ(created - free_now(), 101 + 3);

    KT_EQ(vmo_decommit(v, off, 40 * PG), OK);
    KT_EQ(vmo_committed(v), 61 * PG);
    KT_GLOBAL_EQ(created - free_now(), 61 + 3);
    KT_EQ(vmo_decommit(v, 0, 8 << 20), OK);   /* all of it; tables stay */
    KT_EQ(vmo_committed(v), 0);
    KT_GLOBAL_EQ(created - free_now(), 3);
    KT_EQ(vmo_decommit(v, 0, (8 << 20) + 1), ERR_OUT_OF_RANGE);

    /* Decommitted pages read as zero again. */
    uint8_t b = 0x77;
    KT_EQ(vmo_write(v, off, &b, 1), OK);
    KT_EQ(vmo_decommit(v, off, 1), OK);
    KT_EQ(vmo_read(v, off, &b, 1), OK);
    KT_EQ(b, 0);

    KT_EQ(vmo_commit(v, 0, 8 << 20), OK);
    KT_GLOBAL_EQ(created - free_now(), 2048 + 1 + 4);
    put(v);
    KT_GLOBAL_EQ(free_now(), base);
}

KTEST(vmo_contiguous)
{
    struct vmo *keep = slab_keeper();
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    enum { PAGES = 769 };
    uint64_t *phys = kmalloc(PAGES * sizeof(uint64_t));
    uint64_t base = free_now();

    /* 769 pages: one 4 MiB buddy block with the 255-page tail given back. */
    struct vmo *v;
    KT_EQ(vmo_create(3 * (1 << 20) + 1, VMO_CONTIGUOUS, &v), OK);
    KT_EQ(vmo_size(v), PAGES * PG);
    KT_EQ(vmo_committed(v), PAGES * PG);
    KT_GLOBAL_EQ(base - free_now(), PAGES);

    uint64_t pin;
    KT_EQ(vmo_pin(v, cap, 0, PAGES * PG, phys, PAGES, &pin), OK);
    KT_EQ(phys[0] & ((4ull << 20) - 1), 0);
    for (unsigned i = 1; i < PAGES; i++)
        KT_EQ(phys[i], phys[0] + i * PG);

    /* Zeroed at creation; ordinary byte access works. */
    uint8_t buf[64];
    KT_EQ(vmo_read(v, PAGES * PG - 64, buf, 64), OK);
    KT_ASSERT(all_zero(buf, 64));
    fill(buf, 64, 3);
    KT_EQ(vmo_write(v, 2 * PG - 32, buf, 64), OK);
    KT_ASSERT(!memcmp((uint8_t *)phys_to_virt(phys[1]) + PG - 32, buf, 64));

    KT_EQ(vmo_decommit(v, 0, PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_set_size(v, PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_commit(v, 0, PG), OK);
    KT_EQ(vmo_unpin(v, cap, pin), OK);
    put(v);
    KT_GLOBAL_EQ(free_now(), base);

    /* Small ones too: 5 pages from an order-3 block. */
    KT_EQ(vmo_create(5 * PG, VMO_CONTIGUOUS, &v), OK);
    KT_GLOBAL_EQ(base - free_now(), 5);
    put(v);
    KT_GLOBAL_EQ(free_now(), base);

    kfree(phys);
    kobject_unref(cap);
    put(keep);
}

KTEST(vmo_dma32)
{
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    enum { PAGES = 64 };
    static uint64_t phys[PAGES];
    uint64_t pin;
    bool high_mem = pmm_max_pfn() > (FOUR_GIB >> PAGE_SHIFT) + 65536;

    /* Without the flag, pages come from above 4 GiB when there is any. */
    struct vmo *v;
    KT_EQ(vmo_create(PAGES * PG, 0, &v), OK);
    KT_EQ(vmo_pin(v, cap, 0, PAGES * PG, phys, PAGES, &pin), OK);
    unsigned above = 0;
    for (unsigned i = 0; i < PAGES; i++)
        above += phys[i] >= FOUR_GIB;
    if (high_mem)
        KT_EQ(above, PAGES);
    KT_EQ(vmo_unpin(v, cap, pin), OK);
    put(v);

    KT_EQ(vmo_create(PAGES * PG, VMO_DMA32, &v), OK);
    KT_EQ(vmo_pin(v, cap, 0, PAGES * PG, phys, PAGES, &pin), OK);
    for (unsigned i = 0; i < PAGES; i++)
        KT_ASSERT(phys[i] + PG <= FOUR_GIB);
    KT_EQ(vmo_unpin(v, cap, pin), OK);
    put(v);

    KT_EQ(vmo_create(PAGES * PG, VMO_DMA32 | VMO_CONTIGUOUS, &v), OK);
    KT_EQ(vmo_pin(v, cap, 0, PAGES * PG, phys, PAGES, &pin), OK);
    KT_ASSERT(phys[PAGES - 1] + PG <= FOUR_GIB);
    KT_EQ(vmo_unpin(v, cap, pin), OK);
    put(v);

    kprintf("ktest: vmo_dma32: memory above 4 GiB %s (%u/%u plain pages above)\n",
            high_mem ? "yes" : "no", above, PAGES);
    kobject_unref(cap);
}

KTEST(vmo_map_kernel)
{
    uint64_t base = free_now();
    uint64_t pml4 = vmm_kernel_pml4();
    struct vmo *v;
    KT_EQ(vmo_create(5 * PG, 0, &v), OK);

    enum { LEN = 3 * 4096 };
    static uint8_t in[LEN], out[LEN];
    fill(in, LEN, 5);
    KT_EQ(vmo_write(v, 100, in, LEN), OK);   /* pages 0..3 */
    KT_EQ(vmo_committed(v), 4 * PG);

    /* Maps pages 0..3; *va points at offset 100 itself. */
    void *va;
    KT_EQ(vmo_map_kernel(v, 100, LEN, VM_WRITE, &va), OK);
    KT_EQ((uint64_t)va & (PG - 1), 100);
    KT_ASSERT(!memcmp(va, in, LEN));
    KT_EQ(vmo_kobject(v)->refs, 2);   /* the mapping holds one */

    /* Writes through the mapping show up in vmo_read. */
    fill((uint8_t *)va + 50, 5000, 9);
    fill(in, 5000, 9);
    KT_EQ(vmo_read(v, 150, out, 5000), OK);
    KT_ASSERT(!memcmp(in, out, 5000));

    /* Mapping commits what it covers. */
    void *va2;
    KT_EQ(vmo_map_kernel(v, 4 * PG, PG, 0, &va2), OK);
    KT_EQ(vmo_committed(v), 5 * PG);
    KT_ASSERT(all_zero(va2, PG));
    uint8_t b = 0xc3;
    KT_EQ(vmo_write(v, 4 * PG + 17, &b, 1), OK);
    KT_EQ(((volatile uint8_t *)va2)[17], 0xc3);

    /* Mapped pages can't go away. */
    KT_EQ(vmo_decommit(v, 3 * PG, 1), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, 2 * PG), ERR_BAD_STATE);
    KT_EQ(vmo_unmap_kernel(v, va2), OK);
    KT_EQ(vmm_translate(pml4, (uint64_t)va2), UINT64_MAX);
    KT_EQ(vmo_unmap_kernel(v, va2), ERR_NOT_FOUND);
    KT_EQ(vmo_decommit(v, 4 * PG, PG), OK);

    /* Arguments. */
    void *bad;
    KT_EQ(vmo_map_kernel(v, 0, PG, VM_EXEC, &bad), ERR_INVALID_ARGS);
    KT_EQ(vmo_map_kernel(v, 0, PG, VM_WRITE | VM_UC, &bad), ERR_INVALID_ARGS);
    KT_EQ(vmo_map_kernel(v, 0, 0, 0, &bad), ERR_INVALID_ARGS);
    KT_EQ(vmo_map_kernel(v, 4 * PG, PG + 1, 0, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_map_kernel(v, UINT64_MAX - 10, 20, 0, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_unmap_kernel(v, NULL), ERR_NOT_FOUND);

    uint64_t pa = vmm_translate(pml4, (uint64_t)va);
    KT_ASSERT(pa != UINT64_MAX);
    KT_EQ(vmo_unmap_kernel(v, va), OK);
    for (unsigned i = 0; i < 4; i++)
        KT_EQ(vmm_translate(pml4, ((uint64_t)va & ~(PG - 1)) + i * PG), UINT64_MAX);
    KT_EQ(vmo_kobject(v)->refs, 1);
    KT_EQ(vmo_decommit(v, 0, 5 * PG), OK);
    KT_EQ(vmo_set_size(v, 0), OK);
    put(v);
    /* The vmap area never reuses addresses, but page-table pages created
     * for it stay: allow those. */
    KT_GLOBAL_ASSERT(base - free_now() <= 4);
}

KTEST(vmo_pin)
{
    struct vmo *keep = slab_keeper();
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    uint64_t pml4 = vmm_kernel_pml4();
    uint64_t base = free_now();

    struct vmo *v;
    KT_EQ(vmo_create(16 * PG, 0, &v), OK);
    uint64_t phys[8], pin, pin2;

    /* Argument checks come before anything is recorded. */
    KT_EQ(vmo_pin(v, NULL, 0, PG, phys, 8, &pin), ERR_INVALID_ARGS);
    KT_EQ(vmo_pin(v, vmo_kobject(keep), 0, PG, phys, 8, &pin), ERR_WRONG_TYPE);
    KT_EQ(vmo_pin(v, cap, 1, PG, phys, 8, &pin), ERR_INVALID_ARGS);
    KT_EQ(vmo_pin(v, cap, 0, 0, phys, 8, &pin), ERR_INVALID_ARGS);
    KT_EQ(vmo_pin(v, cap, 0, 9 * PG, phys, 8, &pin), ERR_BUFFER_TOO_SMALL);
    KT_EQ(vmo_pin(v, cap, 12 * PG, 8 * PG, phys, 8, &pin), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_committed(v), 0);

    /* Pin pages 4..11: commits them and returns their addresses. */
    KT_EQ(vmo_pin(v, cap, 4 * PG, 8 * PG, phys, 8, &pin), OK);
    KT_EQ(vmo_committed(v), 8 * PG);
    KT_EQ(vmo_kobject(v)->refs, 2);
    KT_EQ(cap->refs, 2);

    /* A second, overlapping pin is fine. */
    uint64_t phys2[1];
    KT_EQ(vmo_pin(v, cap, 11 * PG, PG, phys2, 1, &pin2), OK);
    KT_EQ(phys2[0], phys[7]);
    KT_ASSERT(pin2 != pin);

    /* Compare with what a kernel mapping of the same pages translates to. */
    void *va;
    KT_EQ(vmo_map_kernel(v, 0, 16 * PG, VM_WRITE, &va), OK);
    for (unsigned i = 0; i < 8; i++)
        KT_EQ(vmm_translate(pml4, (uint64_t)va + (4 + i) * PG), phys[i]);
    KT_EQ(vmo_unmap_kernel(v, va), OK);

    /* Pinned pages can't be decommitted or cut off; others can. */
    KT_EQ(vmo_decommit(v, 5 * PG, PG), ERR_BAD_STATE);
    KT_EQ(vmo_decommit(v, 0, 16 * PG), ERR_BAD_STATE);
    KT_EQ(vmo_decommit(v, 0, 4 * PG), OK);
    KT_EQ(vmo_decommit(v, 12 * PG, 4 * PG), OK);
    KT_EQ(vmo_set_size(v, 8 * PG), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, 12 * PG), OK);
    KT_EQ(vmo_committed(v), 8 * PG);

    struct kobject *other;   /* only the pin's own cap may undo it */
    KT_EQ(dma_cap_create(&other), OK);
    KT_EQ(vmo_unpin(v, other, pin2), ERR_ACCESS_DENIED);
    KT_EQ(vmo_unpin(v, NULL, pin2), ERR_ACCESS_DENIED);
    kobject_unref(other);
    KT_EQ(vmo_unpin(v, cap, pin2), OK);
    KT_EQ(vmo_unpin(v, cap, pin2), ERR_NOT_FOUND);
    KT_EQ(vmo_unpin(v, cap, 12345), ERR_NOT_FOUND);
    KT_EQ(vmo_decommit(v, 11 * PG, PG), ERR_BAD_STATE);   /* the first pin remains */

    /* The pin keeps the VMO (and its pages) alive after its creator lets go;
     * unpinning the last pin destroys it. */
    put(v);
    KT_EQ(vmo_kobject(v)->refs, 1);
    KT_EQ(((uint64_t *)phys_to_virt(phys[0]))[0], 0);   /* still ours, still zero */
    KT_EQ(vmo_unpin(v, cap, pin), OK);   /* last reference: v is gone */
    KT_EQ(cap->refs, 1);
    KT_GLOBAL_ASSERT(base - free_now() <= 4);   /* vmap page tables may remain */

    kobject_unref(cap);
    put(keep);
}

KTEST(vmo_set_size)
{
    uint64_t base = free_now();
    struct vmo *v;
    KT_EQ(vmo_create(8 * PG, 0, &v), OK);
    uint64_t created = free_now();
    static uint8_t buf[8 * 4096];
    fill(buf, sizeof(buf), 11);
    KT_EQ(vmo_write(v, 0, buf, sizeof(buf)), OK);
    KT_EQ(vmo_committed(v), 8 * PG);
    KT_GLOBAL_EQ(created - free_now(), 8 + 2);

    /* Shrink to 3 pages + 1 byte = 4 pages: frees 4 (the leaf stays). */
    KT_EQ(vmo_set_size(v, 3 * PG + 1), OK);
    KT_EQ(vmo_size(v), 4 * PG);
    KT_EQ(vmo_committed(v), 4 * PG);
    KT_GLOBAL_EQ(created - free_now(), 4 + 2);
    uint8_t b;
    KT_EQ(vmo_read(v, 4 * PG, &b, 1), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_write(v, 4 * PG, &b, 1), ERR_OUT_OF_RANGE);

    /* Grow back: the cut-off part reads as zeros, the rest is intact. */
    KT_EQ(vmo_set_size(v, 8 * PG), OK);
    static uint8_t out[8 * 4096];
    KT_EQ(vmo_read(v, 0, out, sizeof(out)), OK);
    KT_ASSERT(!memcmp(out, buf, 4 * PG));
    KT_ASSERT(all_zero(out + 4 * PG, 4 * PG));

    /* Shrink to nothing: every page and table goes. */
    KT_EQ(vmo_set_size(v, 0), OK);
    KT_EQ(vmo_committed(v), 0);
    KT_GLOBAL_EQ(free_now(), created);

    /* Grow far and touch the end; shrinking frees its tables too. */
    KT_EQ(vmo_set_size(v, VMO_MAX_SIZE), OK);
    KT_EQ(vmo_set_size(v, VMO_MAX_SIZE + 1), ERR_OUT_OF_RANGE);
    b = 0x42;
    KT_EQ(vmo_write(v, VMO_MAX_SIZE - 1, &b, 1), OK);
    KT_GLOBAL_EQ(created - free_now(), 3);
    KT_EQ(vmo_set_size(v, 1ull << 30), OK);
    KT_GLOBAL_EQ(free_now(), created);
    put(v);
    KT_GLOBAL_EQ(free_now(), base);
}

KTEST(vmo_physical)
{
    struct vmo *keep = slab_keeper();
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    struct page *pg = pmm_alloc_pages(2, 0);   /* 4 pages that we own */
    KT_ASSERT(pg != NULL);
    uint64_t pa = page_to_phys(pg);
    uint8_t *direct = page_to_virt(pg);
    fill(direct, 4 * PG, 21);
    uint64_t base = free_now();

    struct vmo *v, *bad;
    KT_EQ(vmo_create_physical(pa + 1, PG, 0, &bad), ERR_INVALID_ARGS);
    KT_EQ(vmo_create_physical(pa, 0, 0, &bad), ERR_INVALID_ARGS);
    KT_EQ(vmo_create_physical(pa, PG, VM_WRITE, &bad), ERR_INVALID_ARGS);
    KT_EQ(vmo_create_physical(1ull << 52, PG, 0, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_create_physical(0ull - PG, 2 * PG, 0, &bad), ERR_OUT_OF_RANGE);   /* wraps */
    KT_EQ(vmo_create_physical(pa, 4 * PG, 0, &v), OK);
    KT_EQ(vmo_size(v), 4 * PG);
    KT_EQ(vmo_committed(v), 0);   /* it owns nothing */

    uint8_t b;
    KT_EQ(vmo_read(v, 0, &b, 1), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_write(v, 0, &b, 1), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_decommit(v, 0, PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_set_size(v, PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_commit(v, 0, 4 * PG), OK);

    /* Mapping shows the memory, both ways. */
    void *va;
    KT_EQ(vmo_map_kernel(v, 0, 4 * PG, VM_WRITE, &va), OK);
    KT_ASSERT(!memcmp(va, direct, 4 * PG));
    KT_EQ(vmm_translate(vmm_kernel_pml4(), (uint64_t)va + 3 * PG), pa + 3 * PG);
    ((volatile uint8_t *)va)[2 * PG + 5] = 0x99;
    KT_EQ(direct[2 * PG + 5], 0x99);

    uint64_t phys[4], pin;
    KT_EQ(vmo_pin(v, cap, PG, 3 * PG, phys, 4, &pin), OK);
    for (unsigned i = 0; i < 3; i++)
        KT_EQ(phys[i], pa + (i + 1) * PG);
    KT_EQ(vmo_unpin(v, cap, pin), OK);
    KT_EQ(vmo_unmap_kernel(v, va), OK);

    /* Destroying it gives nothing back to the allocator: the pages are
     * still ours. (base only bounds vmap page tables made by the mapping.) */
    uint64_t before_destroy = free_now();
    put(v);
    KT_GLOBAL_EQ(free_now(), before_destroy);
    KT_GLOBAL_ASSERT(base - free_now() <= 4);
    KT_EQ(direct[2 * PG + 5], 0x99);
    pmm_free_pages(pg, 2);
    kobject_unref(cap);
    put(keep);
}

/* ---- concurrency ----------------------------------------------------------- */

enum { CW_THREADS = 12, CW_OWN = 4, CW_SHARED = 4, CW_SLOT = 256 };
#define CW_PAGES (CW_THREADS * CW_OWN + CW_SHARED)

static struct vmo *cw_vmo;
static volatile int cw_ready, cw_go;

static void cw_wait_start(void)
{
    __atomic_add_fetch(&cw_ready, 1, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&cw_go, __ATOMIC_ACQUIRE))
        thread_yield();
}

/* Thread i writes its own four pages in odd-sized chunks that cross page
 * boundaries, and its own 256-byte slot in each of the shared pages, which
 * every thread races to commit at the same moment. */
static void cw_worker(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    static uint8_t bufs[CW_THREADS][CW_OWN * 4096];
    uint8_t *mine = bufs[i];
    uint8_t slot[CW_SLOT];
    fill(mine, CW_OWN * PG, 100 + i);
    fill(slot, CW_SLOT, 200 + i);
    cw_wait_start();

    for (unsigned s = 0; s < CW_SHARED; s++)
        KT_EQ(vmo_write(cw_vmo, (CW_THREADS * CW_OWN + s) * PG + i * CW_SLOT, slot, CW_SLOT), OK);
    uint64_t own = (uint64_t)i * CW_OWN * PG;
    for (uint64_t off = 0; off < CW_OWN * PG;) {
        uint64_t n = CW_OWN * PG - off < 1000 ? CW_OWN * PG - off : 1000;
        KT_EQ(vmo_write(cw_vmo, own + off, mine + off, n), OK);
        off += n;
    }
}

KTEST(vmo_concurrent_writers)
{
    static uint8_t got[CW_OWN * 4096], want[CW_OWN * 4096];
    uint64_t warm = 0;
    for (int round = 0; round < 8; round++) {
        KT_EQ(vmo_create(CW_PAGES * PG, 0, &cw_vmo), OK);
        cw_ready = 0;
        cw_go = 0;
        struct thread *ts[CW_THREADS];
        for (uint32_t i = 0; i < CW_THREADS; i++) {
            cpumask_t m;
            cpumask_one(&m, i % cpu_count);
            ts[i] = thread_create_on("vmo-writer", cw_worker, (void *)(uintptr_t)i,
                                     PRIO_DEFAULT, &m);
        }
        while (__atomic_load_n(&cw_ready, __ATOMIC_ACQUIRE) < CW_THREADS)
            thread_yield();
        __atomic_store_n(&cw_go, 1, __ATOMIC_RELEASE);
        for (uint32_t i = 0; i < CW_THREADS; i++)
            thread_join(ts[i]);

        /* Exactly one page per index: racing commits didn't double up. */
        KT_EQ(vmo_committed(cw_vmo), CW_PAGES * PG);
        for (uint32_t i = 0; i < CW_THREADS; i++) {
            fill(want, CW_OWN * PG, 100 + i);
            KT_EQ(vmo_read(cw_vmo, (uint64_t)i * CW_OWN * PG, got, CW_OWN * PG), OK);
            KT_ASSERT(!memcmp(got, want, CW_OWN * PG));
        }
        for (unsigned s = 0; s < CW_SHARED; s++) {
            KT_EQ(vmo_read(cw_vmo, (CW_THREADS * CW_OWN + s) * PG, got, PG), OK);
            for (uint32_t i = 0; i < CW_THREADS; i++) {
                fill(want, CW_SLOT, 200 + i);
                KT_ASSERT(!memcmp(got + i * CW_SLOT, want, CW_SLOT));
            }
            KT_ASSERT(all_zero(got + CW_THREADS * CW_SLOT, PG - CW_THREADS * CW_SLOT));
        }
        put(cw_vmo);
        /* Round 0 fills the scheduler's stack cache; after that, every
         * page (including racing commits' losers) must come back. */
        if (round == 0)
            warm = free_now();
        else
            KT_GLOBAL_EQ(free_now(), warm);
    }
}

/* Readers and writers against a thread that keeps decommitting the same
 * pages. What this actually checks: each write-then-read of a thread's own
 * slot reads back that exact value or zero (its page was decommitted in
 * between), never a torn or foreign value; and the committed page count never
 * exceeds the VMO size. It does not directly observe writes to freed pages --
 * the write/read pairing is the proxy: a write that landed on a page freed
 * under it would, once that page is reused, read back as neither the value
 * nor zero and trip dc_bad. */
enum { DC_THREADS = 6, DC_ITERS = 3000 };
static volatile int dc_done;
static volatile int dc_bad, dc_decommits;

static void dc_worker(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    uint64_t off = (i % 2) * PG + (i / 2) * 64 + 8;   /* spread over both pages */
    cw_wait_start();
    for (uint64_t k = 1; k <= DC_ITERS; k++) {
        uint64_t val = ((uint64_t)i << 32) | k, seen;
        if (vmo_write(cw_vmo, off, &val, 8) != OK || vmo_read(cw_vmo, off, &seen, 8) != OK ||
            (seen != val && seen != 0))
            __atomic_add_fetch(&dc_bad, 1, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&dc_done, 1, __ATOMIC_RELEASE);
}

static void dc_decommitter(void *arg)
{
    (void)arg;
    cw_wait_start();
    while (__atomic_load_n(&dc_done, __ATOMIC_ACQUIRE) < DC_THREADS) {
        KT_EQ(vmo_decommit(cw_vmo, 0, 2 * PG), OK);
        dc_decommits++;
        thread_yield();
    }
}

static void dc_round(void)
{
    KT_EQ(vmo_create(2 * PG, 0, &cw_vmo), OK);
    cw_ready = cw_go = 0;
    dc_done = dc_bad = dc_decommits = 0;
    struct thread *ts[DC_THREADS + 1];
    for (uint32_t i = 0; i <= DC_THREADS; i++) {
        cpumask_t m;
        cpumask_one(&m, i % cpu_count);
        ts[i] = i < DC_THREADS
                    ? thread_create_on("vmo-dc", dc_worker, (void *)(uintptr_t)i, PRIO_DEFAULT, &m)
                    : thread_create_on("vmo-decommit", dc_decommitter, NULL, PRIO_DEFAULT, &m);
    }
    while (__atomic_load_n(&cw_ready, __ATOMIC_ACQUIRE) < DC_THREADS + 1)
        thread_yield();
    __atomic_store_n(&cw_go, 1, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i <= DC_THREADS; i++)
        thread_join(ts[i]);
    KT_EQ(dc_bad, 0);
    KT_ASSERT(dc_decommits > 10);   /* the race really ran */
    KT_ASSERT(vmo_committed(cw_vmo) <= 2 * PG);
    put(cw_vmo);
}

KTEST(vmo_concurrent_decommit)
{
    dc_round();
    uint64_t warm = free_now();   /* thread stacks are cached now */
    dc_round();
    KT_GLOBAL_EQ(free_now(), warm);      /* pages freed by whoever let go last */
}

/* ---- handle layer ---------------------------------------------------------- */

KTEST(vmo_sys_rights)
{
    uint64_t base = free_now();
    struct handle_table t;
    handle_table_init(&t);
    handle_t h, ro, wo, dh;
    KT_EQ(sys_vmo_create(&t, 3 * PG, 1u << 9, HANDLE_INVALID, &h), ERR_INVALID_ARGS);
    KT_EQ(sys_vmo_create(&t, 3 * PG, 0, HANDLE_INVALID, &h), OK);

    /* VMO_CONTIGUOUS / VMO_DMA32 need a DMA capability at the sys layer. (O3b) */
    handle_t dch;
    KT_EQ(sys_vmo_create(&t, PG, VMO_CONTIGUOUS, HANDLE_INVALID, &dch), ERR_ACCESS_DENIED);
    struct kobject *dcap;
    KT_EQ(dma_cap_create(&dcap), OK);
    struct khandle dcapk = khandle_from_new(dcap, RIGHTS_BASIC);
    handle_t dcaph;
    KT_EQ(handle_insert(&t, &dcapk, &dcaph), OK);
    KT_EQ(sys_vmo_create(&t, PG, VMO_CONTIGUOUS, dcaph, &dch), OK);
    KT_EQ(handle_close(&t, dch), OK);
    KT_EQ(handle_close(&t, dcaph), OK);

    struct kobject *obj;
    rights_t r;
    KT_EQ(handle_get(&t, h, OBJ_VMO, 0, &obj, &r), OK);
    KT_EQ(r, RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    kobject_unref(obj);

    uint8_t in[16], out[16];
    fill(in, 16, 31);
    KT_EQ(sys_vmo_write(&t, h, PG - 8, in, 16), OK);
    KT_EQ(sys_vmo_read(&t, h, PG - 8, out, 16), OK);
    KT_ASSERT(!memcmp(in, out, 16));

    /* Read-only handle. */
    KT_EQ(handle_duplicate(&t, h, RIGHT_READ | RIGHTS_BASIC, &ro), OK);
    memset(out, 0, 16);
    KT_EQ(sys_vmo_read(&t, ro, PG - 8, out, 16), OK);
    KT_ASSERT(!memcmp(in, out, 16));
    KT_EQ(sys_vmo_write(&t, ro, 0, in, 1), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_set_size(&t, ro, PG), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_commit(&t, ro, 0, PG), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_decommit(&t, ro, 0, PG), ERR_ACCESS_DENIED);
    uint64_t size = 0;
    KT_EQ(sys_vmo_get_size(&t, ro, &size), OK);
    KT_EQ(size, 3 * PG);

    /* Write-only handle. */
    KT_EQ(handle_duplicate(&t, h, RIGHT_WRITE, &wo), OK);
    KT_EQ(sys_vmo_read(&t, wo, 0, out, 1), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_write(&t, wo, 0, in, 1), OK);
    KT_EQ(sys_vmo_commit(&t, wo, 0, 3 * PG), OK);
    KT_EQ(sys_vmo_decommit(&t, wo, 2 * PG, PG), OK);
    KT_EQ(sys_vmo_set_size(&t, wo, 5 * PG), OK);
    KT_EQ(sys_vmo_get_size(&t, ro, &size), OK);
    KT_EQ(size, 5 * PG);
    KT_EQ(sys_vmo_read(&t, h, 5 * PG, out, 1), ERR_OUT_OF_RANGE);   /* errors pass through */

    /* Wrong type, bad handles. */
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    struct khandle kh = khandle_from_new(cap, RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(handle_insert(&t, &kh, &dh), OK);
    KT_EQ(sys_vmo_read(&t, dh, 0, out, 1), ERR_WRONG_TYPE);
    KT_EQ(sys_vmo_get_size(&t, dh, &size), ERR_WRONG_TYPE);
    KT_EQ(sys_vmo_read(&t, HANDLE_INVALID, 0, out, 1), ERR_BAD_HANDLE);
    KT_EQ(handle_close(&t, wo), OK);
    KT_EQ(sys_vmo_write(&t, wo, 0, in, 1), ERR_BAD_HANDLE);

    handle_table_destroy(&t);   /* closes the rest: the VMO goes with it */
    KT_GLOBAL_EQ(free_now(), base);
}
