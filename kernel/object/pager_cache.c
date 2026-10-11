/* A pager's page cache: the budget, the list of resident pages oldest
 * first, publishing a supplied page inside the budget and evicting the
 * oldest. The model, the counting rule and the lock order are in
 * pager_cache.h; the VMO side of an eviction (zap, gather, free) is
 * vmo.c's vmo_pager_evict. */
#include <jam/aspace_vmo.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/vmo.h>
#include "pager_cache.h"

/* Where a resident page is: the evictor's way back from the list to the
 * table. */
struct pager_rec {
    struct vmo *v;     /* its VMO (no reference: the record goes before the VMO can) */
    uint64_t    idx;   /* its page index there */
};
_Static_assert(sizeof(struct pager_rec) == 16, "a resident page costs its job 16 bytes");

/* A page taken off the list by an evictor. */
struct victim {
    struct page *p;     /* only compared, never touched: it may be freed meanwhile */
    struct vmo  *v;     /* a reference, or NULL: its VMO is being destroyed */
    uint64_t     idx;
};

void pager_cache_init(struct pager_cache *c, struct job *job, uint64_t max_pages)
{
    spin_init(&c->lock, "pager cache");
    c->job = job;
    c->max_pages = max_pages;
    c->resident = 0;
    list_init(&c->pages);
}

static uint64_t cache_lock(struct pager_cache *c)
{
    return spin_lock_irqsave(&c->lock);
}

static void cache_unlock(struct pager_cache *c, uint64_t f)
{
    spin_unlock_irqrestore(&c->lock, f);
}

/* A record charged to c's job, or NULL (the job refused it, or no memory). */
static struct pager_rec *rec_new(struct pager_cache *c)
{
    if (job_charge(c->job, JOB_LIMIT_MSG_BYTES, sizeof(struct pager_rec)) != OK)
        return NULL;
    struct pager_rec *r = kmalloc(sizeof(*r));
    if (!r)
        job_uncharge(c->job, JOB_LIMIT_MSG_BYTES, sizeof(struct pager_rec));
    return r;
}

static void rec_free(struct pager_cache *c, struct pager_rec *r)
{
    kfree(r);
    job_uncharge(c->job, JOB_LIMIT_MSG_BYTES, sizeof(struct pager_rec));
}

/* Lock held, the list not empty: the oldest page off the list (the
 * caller decides what becomes of its count). Its VMO's memory is valid
 * while the page is listed (the VMO's destruction takes its pages off
 * under this lock first), so trying a reference is safe here. */
static void pop_locked(struct pager_cache *c, struct victim *out)
{
    struct page *p = list_first(&c->pages, struct page, node);
    list_del(&p->node);   /* node.next is NULL now: off the list */
    const struct pager_rec *r = (const struct pager_rec *)p->private;
    out->p = p;
    out->idx = r->idx;
    out->v = kobject_tryref(vmo_kobject(r->v)) ? r->v : NULL;
}

/* Take a popped page out of its table, if it is still there. */
static void evict(struct victim *vic)
{
    if (!vic->v)
        return;   /* its VMO's destruction takes it */
    vmo_pager_evict(vic->v, vic->idx, vic->p);
    kobject_unref(vmo_kobject(vic->v));
}

/* One slot of the budget for a page about to be published. With the
 * budget full, the oldest page's slot becomes ours: it is taken out of its
 * table before we return, so it and the page we publish are never both
 * there. */
static void reserve(struct pager_cache *c)
{
    struct victim vic;
    uint64_t f = cache_lock(c);
    bool full = c->resident >= c->max_pages && !list_empty(&c->pages);
    if (full)
        pop_locked(c, &vic);
    else
        c->resident++;   /* (past the budget only if the list is all reservations) */
    cache_unlock(c, f);
    if (full)
        evict(&vic);
}

static void unreserve(struct pager_cache *c)
{
    uint64_t f = cache_lock(c);
    c->resident--;
    cache_unlock(c, f);
}

bool pager_cache_evict_oldest(struct pager_cache *c)
{
    struct victim vic;
    uint64_t f = cache_lock(c);
    bool any = !list_empty(&c->pages);
    if (any) {
        pop_locked(c, &vic);
        c->resident--;
    }
    cache_unlock(c, f);
    if (any)
        evict(&vic);
    return any;
}

status_t pager_cache_publish(struct pager_cache *c, struct vmo *v, uint64_t idx, struct page *p,
                             bool checked)
{
    if (vmo_page_phys(v, idx << PAGE_SHIFT)) {
        page_unref(p);   /* present already: a second answer evicts nothing */
        return OK;
    }
    /* Memory pressure (the job's limits, or no memory) evicts the oldest
     * pages, at most a cache's worth: the cache can hold no more, so past
     * that the room is taken by someone else and the supply gives up. */
    uint64_t presses = 0;
    struct pager_rec *rec;
    while (!(rec = rec_new(c)) && presses++ < c->max_pages && pager_cache_evict_oldest(c))
        ;
    if (!rec) {
        page_unref(p);
        return ERR_NO_MEMORY;
    }
    rec->v = v;
    rec->idx = idx;
    reserve(c);
    status_t st;
    while ((st = vmo_pager_publish(v, idx, p, checked, rec)) == ERR_NO_MEMORY &&
           presses++ < c->max_pages && pager_cache_evict_oldest(c))
        ;
    if (st == OK)
        return OK;   /* p and rec are the table's now */
    unreserve(c);
    rec_free(c, rec);
    page_unref(p);
    return st == ERR_ALREADY_EXISTS ? OK : st;   /* present after all: a racing answer */
}

void pager_cache_add_vlocked(struct pager_cache *c, struct page *p, struct pager_rec *rec)
{
    uint64_t f = cache_lock(c);
    p->private = (uint64_t)(uintptr_t)rec;
    list_add_tail(&c->pages, &p->node);   /* its count is the slot reserve() made */
    cache_unlock(c, f);
}

void pager_cache_remove_vlocked(struct pager_cache *c, struct page *p)
{
    struct pager_rec *rec = (struct pager_rec *)p->private;
    ASSERT(rec);   /* every page of a pager VMO came in through pager_cache_add_vlocked */
    uint64_t f = cache_lock(c);
    if (p->node.next) {   /* still listed: no evictor has it */
        list_del(&p->node);
        c->resident--;
    }
    p->private = 0;
    cache_unlock(c, f);
    rec_free(c, rec);
}

uint64_t pager_cache_resident(struct pager_cache *c)
{
    uint64_t f = cache_lock(c);
    uint64_t n = c->resident;
    cache_unlock(c, f);
    return n;
}
