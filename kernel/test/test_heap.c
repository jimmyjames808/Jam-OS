/* The kernel heap's per-CPU magazines (kernel/mm/heap.c): refills and
 * drains in batches with exact page accounting, frees on another CPU, and
 * running out of memory with objects parked in magazines. */
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/sched.h>

/* ---- per-CPU kmalloc magazines ---------------------------------------------- */

/* Private caches, so nothing else touches their slabs: 248-byte objects
 * make exactly 16 per one-page slab ((4096 - 64) / 248). */
static struct kmem_cache *mag_cache, *oom_cache;

static struct kmem_cache *private_cache(struct kmem_cache **c, const char *name)
{
    if (!*c)
        *c = kmem_cache_create(name, 248, 8);   /* caches are never destroyed */
    return *c;
}

static void *xfer_obj;
static struct kmem_cache *xfer_cache;

static void free_it(void *arg)
{
    (void)arg;
    kmem_cache_free(xfer_cache, xfer_obj);
}

KTEST(kmag_refill_drain_accounting)
{
    if (!heap_percpu)
        return;
    struct kmem_cache *c = private_cache(&mag_cache, "kt magazine");
    uint32_t me = kt_pin_self(cpu_count > 1 ? 1 : 0);
    uint64_t free0 = kt_free_and_cached_pages();
    KT_EQ(kmem_cached_objects(me, c), 0);
    void *o[17];
    /* Empty magazine, no slab: one new slab, a batch into the magazine, and
     * its top handed out. */
    o[0] = kmem_cache_alloc(c);
    KT_ASSERT(o[0]);
    KT_EQ(kmem_cached_objects(me, c), MAG_BATCH - 1);
    for (int i = 1; i < 16; i++) {
        o[i] = kmem_cache_alloc(c);
        KT_ASSERT(o[i]);
    }
    /* Two refills emptied the slab into the magazine and out again. */
    KT_EQ(kmem_cached_objects(me, c), 0);
    o[16] = kmem_cache_alloc(c);   /* no partial slab left: a second one */
    KT_EQ(kmem_cached_objects(me, c), MAG_BATCH - 1);
    /* Exact accounting: the stats drain the 7 parked objects, and the two
     * slabs with live objects are what is used. */
    KT_GLOBAL_EQ(kt_free_and_cached_pages(), free0 - 2);
    KT_EQ(kmem_cached_objects(me, c), 0);
    /* Frees fill the magazine; the one that finds it full drains a batch. */
    for (int i = 0; i < 17; i++)
        kmem_cache_free(c, o[i]);
    KT_EQ(kmem_cached_objects(me, c), MAG_MAX - MAG_BATCH + 1);
    /* Drained: both slabs empty and given back. */
    KT_GLOBAL_EQ(kt_free_and_cached_pages(), free0);

    /* An object freed on another CPU goes into that CPU's magazine. */
    if (cpu_count > 2) {
        xfer_cache = c;
        xfer_obj = kmem_cache_alloc(c);
        cpumask_t m;
        cpumask_one(&m, 2);
        uint64_t there = kmem_cached_objects(2, c);
        thread_join(thread_create_on("kt-free", free_it, NULL, PRIO_DEFAULT, &m));
        KT_EQ(kmem_cached_objects(2, c), there + 1);
        KT_GLOBAL_EQ(kt_free_and_cached_pages(), free0);
        KT_EQ(kmem_cached_objects(2, c), 0);
    }
    kt_unpin_self();
}

static void park_16(void *arg)
{
    (void)arg;
    void *o[16];
    for (int i = 0; i < 16; i++)
        o[i] = kmem_cache_alloc(oom_cache);
    for (int i = 0; i < 16; i++)
        kmem_cache_free(oom_cache, o[i]);
}

/* A whole slab's objects parked in another CPU's magazine, and no free page
 * for a new slab: the allocation drains every magazine (emptying that slab)
 * and succeeds. */
KTEST(kmag_oom_drains_magazines)
{
    KT_SKIP_LIVE("takes every free page");
    if (!heap_percpu || cpu_count < 3)
        return;
    struct kmem_cache *c = private_cache(&oom_cache, "kt magazine oom");
    uint64_t free0 = kt_free_and_cached_pages();
    cpumask_t m;
    cpumask_one(&m, 2);
    thread_join(thread_create_on("kt-park", park_16, NULL, PRIO_DEFAULT, &m));
    thread_sleep_ms(20);   /* let the helper be reaped (its stack is cached) */
    KT_EQ(kmem_cached_objects(2, c), 16);
    kt_pin_self(1);
    /* Take every page (chained through struct page, no memory touched).
     * The other CPUs are idle between tests. */
    struct page *chain = NULL, *p;
    while ((p = pmm_alloc_pages(0, 0))) {
        p->private = (uint64_t)chain;
        chain = p;
    }
    void *obj = kmem_cache_alloc(c);
    uint64_t left = kmem_cached_objects(2, c);
    while (chain) {
        struct page *next = (struct page *)chain->private;
        chain->private = 0;
        pmm_free_pages(chain, 0);
        chain = next;
    }
    kt_unpin_self();
    KT_ASSERT(obj);
    KT_EQ(left, 0);
    kmem_cache_free(c, obj);
    KT_EQ(kt_free_and_cached_pages(), free0);
}
