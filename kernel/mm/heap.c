/* Kernel heap: slab caches for small objects, whole buddy blocks for large
 * ones. Every allocation is physically contiguous and HHDM-addressed.
 *
 * A slab is a 2^order page block whose first bytes hold struct slab; each of
 * its pages is flagged PG_SLAB with private pointing at that header, so
 * kfree can find the owning cache from any object address.
 *
 * Per-CPU magazines, in the style of pmm.c's page stashes. Each CPU
 * keeps up to MAG_MAX free objects of each cache in front of the cache's
 * lock. kmem_cache_alloc/free (and kmalloc/kfree of slab sizes) on that CPU
 * touch only its magazine; an empty one refills, and a full one drains,
 * MAG_BATCH objects under one acquisition of the cache lock.
 *   - Refill never creates a slab while the cache has a partial one: it
 *     takes free objects from partial slabs first and makes a new slab only
 *     if there were none at all. So with the magazines drained, the slabs in
 *     use are the same as without magazines (the objects may sit in a
 *     different one of the existing slabs), which keeps page counts exact.
 *   - Accounting: an object in a magazine is FREE to its owner but still
 *     holds its slab (and so its pages) allocated. pmm_stats() therefore
 *     drains every magazine first (kmem_drain_all), so what it reports is
 *     exact whenever nobody allocates, as the ktest leak check (and the
 *     tests that count pages exactly) need. A stats call is rare; that it
 *     empties the magazines costs only their next refills.
 *   - Magazine lock: a flag lock taken with interrupts off by its owner on
 *     every operation and by any CPU draining it, like a pmm stash, and for
 *     the same reasons not a spinlock_t: the owner's path stays one
 *     uncontended exchange on a line only that CPU touches. Order: magazine
 *     lock, then the cache lock ("kmem_cache"), then pmm's locks (a new or
 *     emptied slab). Nothing takes a magazine lock with a cache lock held,
 *     and at most one magazine lock is held at a time. No interrupt handler
 *     allocates (the cache lock would already be interrupt-unsafe).
 *   - Out of memory: when a refill finds no partial slab and no page for a
 *     new one, the caller drops its magazine, drains every CPU's magazines
 *     (which can free empty slabs) and retries once through the plain path.
 *     A page allocation elsewhere that fails does not drain them: at most
 *     MAG_MAX objects per cache per CPU are parked (on the PC with the 10
 *     caches of today, well under 2 MiB in the worst case).
 *   - Switch: heap_percpu (boot "nokmcache"). Off, allocations and frees go
 *     straight to the slabs; objects already in magazines stay there until
 *     the next drain. */
#include <jam/cmdline.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/x86.h>

#define MIN_OBJECTS_PER_SLAB 8
#define KMALLOC_MAX_SLAB     2048
#define CACHE_POOL           32

struct kmem_cache {
    const char      *name;
    size_t           obj_size;   /* rounded up to align */
    unsigned         order;
    uint32_t         per_slab;
    struct list_node partial, full;
    spinlock_t       lock;
};

struct slab {
    struct list_node   node;
    struct kmem_cache *cache;
    void              *freelist;
    uint32_t           inuse;
};

#define SLAB_HDR ALIGN_UP(sizeof(struct slab), 64)

/* One CPU's magazine for one cache. */
struct kmag {
    volatile uint32_t busy;             /* the magazine lock (see the top) */
    volatile uint32_t n;                /* objs[0..n) free; written with busy held */
    void             *objs[MAG_MAX];    /* LIFO: objs[n-1] is the hottest */
} __attribute__((aligned(64)));

/* Per CPU: one kmag per cache_pool slot, allocated by heap_percpu_init. */
#define MAG_BLOCK_ORDER 1
_Static_assert(sizeof(struct kmag) * CACHE_POOL <= (PAGE_SIZE << MAG_BLOCK_ORDER),
               "a CPU's magazines fit its block");
static struct kmag *mags[MAX_CPUS];
volatile bool heap_percpu = true;

static struct kmem_cache cache_pool[CACHE_POOL];
static unsigned cache_pool_used;
static spinlock_t pool_lock = SPINLOCK_INIT("kmem cache pool");

static const size_t kmalloc_sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
#define KMALLOC_CLASSES (sizeof(kmalloc_sizes) / sizeof(kmalloc_sizes[0]))
static struct kmem_cache *kmalloc_caches[KMALLOC_CLASSES];

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align)
{
    if (align < 8)
        align = 8;
    if (size < sizeof(void *))
        size = sizeof(void *);
    size = ALIGN_UP(size, align);

    unsigned order = 0;
    while (order < MAX_ORDER &&
           ((PAGE_SIZE << order) - SLAB_HDR) / size < MIN_OBJECTS_PER_SLAB)
        order++;
    ASSERT((PAGE_SIZE << order) - SLAB_HDR >= size);

    spin_lock(&pool_lock);
    if (cache_pool_used == CACHE_POOL)
        panic("heap: too many caches");
    struct kmem_cache *c = &cache_pool[cache_pool_used];
    *c = (struct kmem_cache){
        .name = name, .obj_size = size, .order = order,
        .per_slab = (uint32_t)(((PAGE_SIZE << order) - SLAB_HDR) / size),
        .lock = SPINLOCK_INIT("kmem_cache"),
    };
    list_init(&c->partial);
    list_init(&c->full);
    /* Published once set up: kmem_drain_all walks caches [0, used). */
    __atomic_store_n(&cache_pool_used, cache_pool_used + 1, __ATOMIC_RELEASE);
    spin_unlock(&pool_lock);
    return c;
}

static struct slab *slab_new(struct kmem_cache *c)
{
    struct page *p = pmm_alloc_pages(c->order, 0);
    if (!p)
        return NULL;
    struct slab *s = page_to_virt(p);
    for (uint64_t i = 0; i < (1ull << c->order); i++) {
        p[i].flags |= PG_SLAB;
        p[i].private = (uint64_t)s;
    }
    s->cache = c;
    s->inuse = 0;
    s->freelist = NULL;
    char *base = (char *)s + SLAB_HDR;
    for (uint32_t i = c->per_slab; i-- > 0;) {
        void **obj = (void **)(base + i * c->obj_size);
        *obj = s->freelist;
        s->freelist = obj;
    }
    return s;
}

/* c->lock held: one object from slab s (on the partial list). */
static void *slab_take_locked(struct kmem_cache *c, struct slab *s)
{
    void **obj = s->freelist;
    s->freelist = *obj;
    if (++s->inuse == c->per_slab) {
        list_del(&s->node);
        list_add(&c->full, &s->node);
    }
    return obj;
}

/* c->lock held. */
static void slab_free_locked(struct kmem_cache *c, struct slab *s, void *obj)
{
    if (s->inuse == c->per_slab) {   /* full -> partial */
        list_del(&s->node);
        list_add(&c->partial, &s->node);
    }
    *(void **)obj = s->freelist;
    s->freelist = obj;
    if (--s->inuse == 0) {
        /* Give empty slabs back (the magazines keep the hot objects). */
        list_del(&s->node);
        struct page *p = virt_to_page(s);
        for (uint64_t i = 0; i < (1ull << c->order); i++)
            p[i].flags &= ~PG_SLAB;
        pmm_free_pages(p, c->order);
    }
}

static struct slab *slab_of(void *obj)
{
    return (struct slab *)virt_to_page(obj)->private;
}

/* The plain path: straight from the slabs under the cache lock. */
static void *slab_alloc(struct kmem_cache *c)
{
    spin_lock(&c->lock);
    struct slab *s;
    if (list_empty(&c->partial)) {
        s = slab_new(c);
        if (!s) {
            spin_unlock(&c->lock);
            return NULL;
        }
        list_add(&c->partial, &s->node);
    } else {
        s = list_first(&c->partial, struct slab, node);
    }
    void *obj = slab_take_locked(c, s);
    spin_unlock(&c->lock);
    return obj;
}

/* ---- per-CPU magazines ------------------------------------------------------ */

static inline void mag_lock(struct kmag *m)
{
    while (__atomic_exchange_n(&m->busy, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&m->busy, __ATOMIC_RELAXED))
            cpu_relax();
}

static inline void mag_unlock(struct kmag *m)
{
    __atomic_store_n(&m->busy, 0, __ATOMIC_RELEASE);
}

/* With interrupts off: this CPU's magazine for c, or NULL (none yet). */
static struct kmag *mag_here(const struct kmem_cache *c)
{
    struct kmag *b = mags[percpu_index()];
    return b ? &b[c - cache_pool] : NULL;
}

/* m locked and empty: up to MAG_BATCH objects from c's partial slabs, or
 * from ONE new slab if there is no partial slab at all (see the top). */
static void mag_refill_locked(struct kmem_cache *c, struct kmag *m)
{
    uint32_t n = 0;
    spin_lock(&c->lock);
    while (n < MAG_BATCH && !list_empty(&c->partial))
        m->objs[n++] = slab_take_locked(c, list_first(&c->partial, struct slab, node));
    if (!n) {
        struct slab *s = slab_new(c);
        if (s) {
            list_add(&c->partial, &s->node);
            while (n < MAG_BATCH && s->freelist)
                m->objs[n++] = slab_take_locked(c, s);
        }
    }
    __atomic_store_n(&m->n, n, __ATOMIC_RELAXED);
    spin_unlock(&c->lock);
}

/* m locked: return its `count` COLDEST objects (the bottom) to their slabs
 * under one acquisition of the cache lock. */
static uint32_t mag_drain_locked(struct kmem_cache *c, struct kmag *m, uint32_t count)
{
    uint32_t n = m->n;
    if (count > n)
        count = n;
    if (!count)
        return 0;
    spin_lock(&c->lock);
    for (uint32_t i = 0; i < count; i++)
        slab_free_locked(c, slab_of(m->objs[i]), m->objs[i]);
    for (uint32_t i = count; i < n; i++)
        m->objs[i - count] = m->objs[i];
    __atomic_store_n(&m->n, n - count, __ATOMIC_RELAXED);
    spin_unlock(&c->lock);
    return count;
}

/* NULL: no magazine here, or no object and no page for a new slab. */
static void *mag_alloc(struct kmem_cache *c)
{
    void *obj = NULL;
    uint64_t f = irq_save();   /* pins us to this CPU's magazine */
    struct kmag *m = mag_here(c);
    if (m) {
        mag_lock(m);
        if (!m->n)
            mag_refill_locked(c, m);
        uint32_t n = m->n;
        if (n) {
            obj = m->objs[n - 1];
            __atomic_store_n(&m->n, n - 1, __ATOMIC_RELAXED);
        }
        mag_unlock(m);
    }
    irq_restore(f);
    return obj;
}

/* False if there is no magazine here: free to the slab instead. */
static bool mag_free(struct kmem_cache *c, void *obj)
{
    uint64_t f = irq_save();
    struct kmag *m = mag_here(c);
    if (m) {
        mag_lock(m);
        if (m->n == MAG_MAX)
            mag_drain_locked(c, m, MAG_BATCH);
        uint32_t n = m->n;
        m->objs[n] = obj;
        __atomic_store_n(&m->n, n + 1, __ATOMIC_RELAXED);
        mag_unlock(m);
    }
    irq_restore(f);
    return m != NULL;
}

uint64_t kmem_drain_all(void)
{
    uint64_t moved = 0;
    unsigned used = __atomic_load_n(&cache_pool_used, __ATOMIC_ACQUIRE);
    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        struct kmag *b = mags[cpu];
        if (!b)
            continue;
        for (unsigned i = 0; i < used; i++) {
            struct kmag *m = &b[i];
            if (!__atomic_load_n(&m->n, __ATOMIC_RELAXED))
                continue;   /* a racy peek; the count is re-read under the lock */
            uint64_t f = irq_save();
            mag_lock(m);
            moved += mag_drain_locked(&cache_pool[i], m, m->n);
            mag_unlock(m);
            irq_restore(f);
        }
    }
    return moved;
}

uint64_t kmem_cached_objects(uint32_t cpu, struct kmem_cache *c)
{
    if (cpu >= MAX_CPUS || !mags[cpu])
        return 0;
    return __atomic_load_n(&mags[cpu][c - cache_pool].n, __ATOMIC_RELAXED);
}

void heap_percpu_init(void)
{
    heap_percpu = !cmdline_has("nokmcache");
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (mags[i])
            continue;
        struct page *p = pmm_alloc_pages(MAG_BLOCK_ORDER, PMM_ZERO);
        if (!p)
            panic("heap: out of memory for the per-CPU magazines");
        __atomic_store_n(&mags[i], (struct kmag *)page_to_virt(p), __ATOMIC_RELEASE);
    }
}

/* ---- interface ------------------------------------------------------------ */

void *kmem_cache_alloc(struct kmem_cache *c)
{
    if (heap_percpu) {
        void *obj = mag_alloc(c);
        if (obj)
            return obj;
    }
    void *obj = slab_alloc(c);
    if (!obj && kmem_drain_all())
        obj = slab_alloc(c);   /* parked objects may have emptied a slab */
    return obj;
}

static void slab_free(struct slab *s, void *obj)
{
    struct kmem_cache *c = s->cache;
    if (heap_percpu && mag_free(c, obj))
        return;
    spin_lock(&c->lock);
    slab_free_locked(c, s, obj);
    spin_unlock(&c->lock);
}

void kmem_cache_free(struct kmem_cache *c, void *obj)
{
    struct slab *s = slab_of(obj);
    ASSERT(s->cache == c);
    slab_free(s, obj);
}

void heap_init(void)
{
    static const char *names[KMALLOC_CLASSES] = {
        "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-128",
        "kmalloc-256", "kmalloc-512", "kmalloc-1024", "kmalloc-2048",
    };
    for (unsigned i = 0; i < KMALLOC_CLASSES; i++)
        kmalloc_caches[i] = kmem_cache_create(names[i], kmalloc_sizes[i], 16);
}

struct kmem_cache *kmalloc_cache_for(size_t size)
{
    if (size > KMALLOC_MAX_SLAB)
        return NULL;
    unsigned i = 0;
    while (kmalloc_sizes[i] < size)
        i++;
    return kmalloc_caches[i];
}

void *kmalloc(size_t size)
{
    if (size <= KMALLOC_MAX_SLAB)
        return kmem_cache_alloc(kmalloc_cache_for(size));
    unsigned order = 0;
    while ((PAGE_SIZE << order) < size) {
        if (++order > MAX_ORDER)
            return NULL;
    }
    struct page *p = pmm_alloc_pages(order, 0);
    if (!p)
        return NULL;
    p->flags |= PG_LARGE;
    return page_to_virt(p);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct page *p = virt_to_page(ptr);
    if (p->flags & PG_SLAB) {
        slab_free((struct slab *)p->private, ptr);
    } else if (p->flags & PG_LARGE) {
        p->flags &= ~PG_LARGE;
        pmm_free_pages(p, p->order);
    } else {
        panic("kfree: %p was not allocated by kmalloc", ptr);
    }
}
