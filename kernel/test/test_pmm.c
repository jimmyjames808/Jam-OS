/* The page allocator's per-CPU stashes (kernel/mm/pmm.c): refill and
 * drain in batches, frees on another CPU, and running out of memory with
 * pages parked in stashes. */
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/sched.h>

/* ---- per-CPU page stashes --------------------------------------------------- */

static struct page *stash_pages[80];

/* Single pages come from and go back to this CPU's stash, which refills and
 * drains in batches, and every page in it is still counted free. */
KTEST(pcp_refill_drain)
{
    KT_SKIP_LIVE("exact per-CPU stash and free page counts");
    uint32_t me = kt_pin_self(cpu_count > 1 ? 1 : 0);
    pmm_drain_stashes();
    KT_EQ(pmm_stash_count(me), 0);
    uint64_t free0 = kt_free_pages();

    /* One page: the empty stash refills a batch and hands out its top. */
    struct page *p = pmm_alloc_pages(0, PMM_ZERO);
    KT_ASSERT(p);
    KT_EQ(pmm_stash_count(me), PMM_PCP_BATCH - 1);
    KT_EQ(kt_free_pages(), free0 - 1);
    uint64_t *mem = page_to_virt(p);
    KT_ASSERT(mem[0] == 0 && mem[PAGE_SIZE / 8 - 1] == 0);   /* PMM_ZERO applied */
    pmm_free_pages(p, 0);
    KT_EQ(pmm_stash_count(me), PMM_PCP_BATCH);
    KT_EQ(kt_free_pages(), free0);

    /* LIFO: the page just freed is the next one handed out (cache-hot). */
    struct page *again = pmm_alloc_pages(0, 0);
    KT_ASSERT(again == p);
    pmm_free_pages(again, 0);

    /* 80 pages: 16 from the stash, then 4 refills of 16. Distinct pages. */
    for (int i = 0; i < 80; i++) {
        stash_pages[i] = pmm_alloc_pages(0, 0);
        KT_ASSERT(stash_pages[i]);
        KT_ASSERT(!(stash_pages[i]->flags & (PG_PCP | PG_FREE)));
        *(uint64_t *)page_to_virt(stash_pages[i]) = page_to_phys(stash_pages[i]);
    }
    KT_EQ(pmm_stash_count(me), 0);
    for (int i = 0; i < 80; i++)
        KT_EQ(*(uint64_t *)page_to_virt(stash_pages[i]), page_to_phys(stash_pages[i]));
    KT_EQ(kt_free_pages(), free0 - 80);

    /* Free them: the stash fills to PCP_MAX, drains a batch, fills again. */
    for (int i = 0; i < 80; i++)
        pmm_free_pages(stash_pages[i], 0);
    /* 64 at the 64th free; the 65th drains 16 (48) and adds one; 15 more. */
    KT_EQ(pmm_stash_count(me), PMM_PCP_MAX);
    KT_EQ(kt_free_pages(), free0);

    /* DMA32 requests bypass the stash. Freeing the page stashes it only if
     * DMA32 is the stash zone (no memory above 4 GiB, as in QEMU), and then
     * the full stash drains a batch first. */
    struct page *low = pmm_alloc_pages(0, PMM_DMA32);
    KT_ASSERT(low && page_to_phys(low) < (4ull << 30));
    KT_EQ(pmm_stash_count(me), PMM_PCP_MAX);
    pmm_free_pages(low, 0);
    uint32_t after = pmm_stash_count(me);
    KT_ASSERT(after == PMM_PCP_MAX || after == PMM_PCP_MAX - PMM_PCP_BATCH + 1);

    /* Draining puts everything back in the buddy lists; the count holds. */
    KT_ASSERT(pmm_drain_stashes() >= after);
    KT_EQ(pmm_stash_count(me), 0);
    KT_EQ(kt_free_pages(), free0);
    kt_unpin_self();
}

/* A page freed on another CPU lands in THAT CPU's stash, still counted. */
static struct page *cross_page;

static void cross_free(void *arg)
{
    (void)arg;
    pmm_free_pages(cross_page, 0);
}

KTEST(pcp_cross_cpu_free)
{
    KT_SKIP_LIVE("exact per-CPU stash and free page counts");
    if (cpu_count < 2)
        return;
    uint32_t me = kt_pin_self(0);
    uint32_t other = 1;
    pmm_drain_stashes();
    uint64_t free0 = kt_free_pages();
    cross_page = pmm_alloc_pages(0, 0);
    KT_ASSERT(cross_page);
    KT_EQ(pmm_stash_count(me), PMM_PCP_BATCH - 1);
    cpumask_t m;
    cpumask_one(&m, other);
    thread_join(thread_create_on("pcp-cross", cross_free, NULL, PRIO_DEFAULT, &m));
    /* thread_join returns once the thread has marked itself exited, but it
     * is reaped on its CPU's next switch, a moment later, and that can free
     * the slab page its struct lived in. Let that happen before counting:
     * on the real PC it landed between the two counts below and the free
     * count rose by one (a real free, not a drain bug). */
    thread_sleep_ms(20);
    KT_ASSERT(pmm_stash_count(other) >= 1);
    /* Creating the thread may have taken a slab page for its struct. */
    KT_ASSERT(kt_free_pages() + 1 >= free0);
    uint64_t f1 = kt_free_pages();
    pmm_drain_stashes();
    KT_EQ(kt_free_pages(), f1);
    kt_unpin_self();
}

/* Out of memory: stashes on every CPU are drained before an allocation
 * fails, and every page is accounted for. */
static void stash_some(void *arg)
{
    (void)arg;
    struct page *p[20];
    for (int i = 0; i < 20; i++)
        p[i] = pmm_alloc_pages(0, 0);
    for (int i = 0; i < 20; i++)
        if (p[i])
            pmm_free_pages(p[i], 0);
}

KTEST(pcp_oom_drains_stashes)
{
    KT_SKIP_LIVE("takes every free page");
    /* Park pages in every CPU's stash first. */
    for (uint32_t c = 0; c < cpu_count; c++) {
        cpumask_t m;
        cpumask_one(&m, c);
        thread_join(thread_create_on("pcp-stash", stash_some, NULL, PRIO_DEFAULT, &m));
    }
    thread_sleep_ms(20);   /* let the helpers be reaped first (see pcp_cross_cpu_free) */
    uint64_t parked = pmm_stash_pages();
    KT_ASSERT(parked >= cpu_count);
    uint64_t drains0 = pmm_stash_drains();
    uint64_t free0 = kt_free_pages();

    /* Take every page, chained through struct page (no memory touched).
     * Nothing else in the system may allocate meanwhile: the other CPUs
     * are idle between tests. */
    struct page *chain = NULL;
    uint64_t n = 0;
    struct page *p;
    while ((p = pmm_alloc_pages(0, 0))) {
        p->private = (uint64_t)chain;
        chain = p;
        n++;
    }
    uint64_t free_at_oom = kt_free_pages(), stash_at_oom = pmm_stash_pages();
    uint64_t drains = pmm_stash_drains() - drains0;
    while (chain) {
        struct page *next = (struct page *)chain->private;
        chain->private = 0;
        pmm_free_pages(chain, 0);
        chain = next;
    }
    kprintf("pcp: %lu pages taken (%lu were parked in stashes), %lu drains\n", n, parked,
            drains);
    KT_EQ(stash_at_oom, 0);
    KT_EQ(free_at_oom, 0);
    KT_EQ(n, free0);
    KT_ASSERT(drains >= 1);
    KT_EQ(kt_free_pages(), free0);
}
