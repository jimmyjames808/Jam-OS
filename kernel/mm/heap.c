/* Kernel heap: slab caches for small objects, whole buddy blocks for large
 * ones. Every allocation is physically contiguous and HHDM-addressed.
 *
 * A slab is a 2^order page block whose first bytes hold struct slab; each of
 * its pages is flagged PG_SLAB with private pointing at that header, so
 * kfree can find the owning cache from any object address. */
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/spinlock.h>
#include <jam/string.h>

#define MIN_OBJECTS_PER_SLAB 8
#define KMALLOC_MAX_SLAB     2048

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

static struct kmem_cache cache_pool[32];
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
    if (cache_pool_used == sizeof(cache_pool) / sizeof(cache_pool[0]))
        panic("heap: too many caches");
    struct kmem_cache *c = &cache_pool[cache_pool_used++];
    spin_unlock(&pool_lock);

    *c = (struct kmem_cache){
        .name = name, .obj_size = size, .order = order,
        .per_slab = (uint32_t)(((PAGE_SIZE << order) - SLAB_HDR) / size),
        .lock = SPINLOCK_INIT("kmem_cache"),
    };
    list_init(&c->partial);
    list_init(&c->full);
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

void *kmem_cache_alloc(struct kmem_cache *c)
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
    void **obj = s->freelist;
    s->freelist = *obj;
    if (++s->inuse == c->per_slab) {
        list_del(&s->node);
        list_add(&c->full, &s->node);
    }
    spin_unlock(&c->lock);
    return obj;
}

static void slab_free(struct slab *s, void *obj)
{
    struct kmem_cache *c = s->cache;
    spin_lock(&c->lock);
    if (s->inuse == c->per_slab) {   /* full -> partial */
        list_del(&s->node);
        list_add(&c->partial, &s->node);
    }
    *(void **)obj = s->freelist;
    s->freelist = obj;
    if (--s->inuse == 0) {
        /* Give empty slabs back; M3's per-CPU caches will keep a spare. */
        list_del(&s->node);
        struct page *p = virt_to_page(s);
        for (uint64_t i = 0; i < (1ull << c->order); i++)
            p[i].flags &= ~PG_SLAB;
        pmm_free_pages(p, c->order);
    }
    spin_unlock(&c->lock);
}

void kmem_cache_free(struct kmem_cache *c, void *obj)
{
    struct slab *s = (struct slab *)virt_to_page(obj)->private;
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

void *kmalloc(size_t size)
{
    if (size <= KMALLOC_MAX_SLAB) {
        unsigned i = 0;
        while (kmalloc_sizes[i] < size)
            i++;
        return kmem_cache_alloc(kmalloc_caches[i]);
    }
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
