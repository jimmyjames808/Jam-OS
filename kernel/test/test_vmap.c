/* The vmap area (kernel/mm/vmm.c): ranges given back are merged and reused,
 * so mapping and unmapping again and again makes no new page tables. */
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/vmo.h>

/* Map the whole of v, check both ends through the mapping, unmap. */
static uint64_t map_round(struct vmo *v, uint64_t len, uint64_t mark)
{
    volatile uint64_t *va;
    KT_EQ(vmo_map_kernel(v, 0, len, VM_WRITE, (void **)&va), OK);
    va[0] = mark;
    va[len / 8 - 1] = ~mark;
    uint64_t got[2];
    KT_EQ(vmo_read(v, 0, &got[0], 8), OK);
    KT_EQ(vmo_read(v, len - 8, &got[1], 8), OK);
    KT_EQ(got[0], mark);
    KT_EQ(got[1], ~mark);
    KT_EQ(vmo_unmap_kernel(v, (void *)va), OK);
    return (uint64_t)va;
}

/* A multi-MiB VMO mapped and unmapped many times: the range is reused, so
 * the kernel's page tables stay as they were after the first mapping
 * (a vmap area that never reused ranges made two or three per round). */
KTEST(vmap_reuse_keeps_tables_flat)
{
    KT_OWN_LEAK_CHECK("the first mapping may make vmap page tables, kept for reuse");
    enum { LEN = (5 << 20) + 3 * 4096, ROUNDS = 32 };
    struct vmo *v;
    KT_EQ(vmo_create(LEN, 0, &v), OK);
    uint64_t first = map_round(v, LEN, 0);   /* commits the pages, makes the tables */
    uint64_t pages = ktest_accounted_pages();
    uint64_t tables = vmm_kernel_table_pages();
    unsigned moved = 0;   /* rounds mapped somewhere else */
    for (uint64_t i = 1; i <= ROUNDS; i++)
        moved += map_round(v, LEN, i) != first;
    uint64_t grown = vmm_kernel_table_pages() - tables;
    KT_ASSERT(grown < ROUNDS);   /* holds live too: others don't map 160 MiB */
    KT_GLOBAL_EQ(grown, 0);
    KT_GLOBAL_EQ(moved, 0);   /* nothing else took vmap space in between */
    int64_t kept = (int64_t)(pages - ktest_accounted_pages());
    KT_GLOBAL_ASSERT(kept <= 2);   /* the harness's slack: a slab page or two */
    kobject_unref(vmo_kobject(v));
}

/* Ranges given back merge with free neighbours on both sides, a whole free
 * range is handed out again, and what reaches the top folds back into it. */
KTEST(vmap_ranges_merge)
{
    KT_SKIP_LIVE("its ranges must lie side by side: other threads take vmap space too");
    /* Bigger than any free range, so all four come from the top, in a row
     * (nothing is mapped: no page tables are made). */
    enum { BIG = 64 << 20 };
    const uint64_t step = BIG + PAGE_SIZE;   /* each range's guard page follows it */
    uint64_t a = vmm_reserve(BIG), b = vmm_reserve(BIG);
    uint64_t c = vmm_reserve(BIG), d = vmm_reserve(BIG);
    KT_EQ(b, a + step);
    KT_EQ(c, b + step);
    KT_EQ(d, c + step);

    vmm_release(a, BIG);   /* two free ranges with b between them */
    vmm_release(c, BIG);
    KT_EQ(vmm_reserve(BIG), a);   /* first fit: the lower one */
    vmm_release(a, BIG);
    vmm_release(b, BIG);   /* joins both: a..c is one free range */
    KT_EQ(vmm_reserve(3 * step - PAGE_SIZE), a);   /* exactly all of it */
    vmm_release(a, 3 * step - PAGE_SIZE);

    vmm_release(d, BIG);   /* joins a..c and reaches the top: all folded back */
    KT_EQ(vmm_reserve(BIG), a);
    KT_EQ(vmm_reserve(BIG), b);   /* from the top again */
    vmm_release(b, BIG);
    vmm_release(a, BIG);
}
