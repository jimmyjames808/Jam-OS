/* A pager's page cache: the budget, the lists of clean pages oldest first,
 * of dirty ones and of those being written back, publishing a supplied
 * page inside the budget, evicting the oldest clean one, and the moves
 * from list to list (a store, a write-back's begin and end). The model, the counting rule and the lock order are in
 * pager_cache.h; the VMO side of an eviction (zap, gather, free) is
 * vmo.c's vmo_pager_evict. */
#include <jam/aspace_vmo.h>
#include <jam/dbghook.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/time.h>
#include <jam/vmo.h>
#include "pager_cache.h"

/* Where a resident page is (the evictor's way back from the list to the
 * table), and what state it is in. */
struct pager_rec {
    struct vmo *v;         /* its VMO (no reference: the record goes before the VMO can) */
    uint32_t    idx;       /* its page index there (a pager VMO has fewer than 2^32) */
    uint32_t    state;     /* PAGER_PAGE_* (0: clean); the VMO lock and the cache's */
    uint64_t    dirtied;   /* uptime ns: its age (pager_cache.h's "Age"); 0 while clean */
};
_Static_assert(sizeof(struct pager_rec) == PAGER_RECORD_BYTES, "a resident page's cost to its job");
_Static_assert((VMO_SMALL_MAX_SIZE >> PAGE_SHIFT) <= UINT32_MAX, "a page index fits in idx");

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
    c->ndirty = 0;
    c->nwriting = 0;
    list_init(&c->pages);
    list_init(&c->dirty);
    list_init(&c->writing);
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

/* Take a popped page out of its table, if it is still there and still
 * clean (a store may have dirtied it since the pop: it stays, counted
 * again by the store). */
static void evict(struct victim *vic)
{
    if (!vic->v)
        return;   /* its VMO's destruction takes it */
    DBG_HOOK(DBG_PAGER_EVICT, vic->p);
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
    rec->idx = (uint32_t)idx;   /* (past the size, vmo_pager_publish refuses it) */
    rec->state = 0;
    rec->dirtied = 0;
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
    if (p->node.next) {   /* still listed: no evictor has it (none ever has a dirty one) */
        list_del(&p->node);
        c->resident--;
        if (rec->state)
            c->ndirty--;
        if (rec->state & PAGER_PAGE_WRITING)
            c->nwriting--;
    }
    p->private = 0;
    cache_unlock(c, f);
    rec_free(c, rec);
}

unsigned pager_cache_state_vlocked(const struct page *p)
{
    return ((const struct pager_rec *)p->private)->state;
}

uint64_t pager_cache_dirtied_vlocked(const struct page *p)
{
    return ((const struct pager_rec *)p->private)->dirtied;
}

status_t pager_cache_store_vlocked(struct pager_cache *c, struct page *p, uint64_t limit)
{
    struct pager_rec *rec = (struct pager_rec *)p->private;
    uint64_t f = cache_lock(c);
    bool was_clean = !rec->state;
    if (was_clean && c->ndirty >= limit) {
        cache_unlock(c, f);
        return ERR_NO_RESOURCES;   /* the store waits for a write-back (pager.c) */
    }
    if (was_clean) {
        if (p->node.next)
            list_del(&p->node);   /* off the clean list: its count goes with it */
        else
            c->resident++;   /* popped: its count became the evictor's slot, which it keeps */
        list_add_tail(&c->dirty, &p->node);
        c->ndirty++;
        rec->dirtied = uptime_ns();
    }
    rec->state |= PAGER_PAGE_DIRTY;   /* (a writing page stays on `writing`) */
    cache_unlock(c, f);
    return OK;
}

void pager_cache_begin_vlocked(struct pager_cache *c, struct page *p)
{
    struct pager_rec *rec = (struct pager_rec *)p->private;
    ASSERT(rec->state == PAGER_PAGE_DIRTY);
    uint64_t f = cache_lock(c);
    list_del(&p->node);
    list_add_tail(&c->writing, &p->node);
    c->nwriting++;
    rec->state = PAGER_PAGE_WRITING;   /* (its age stays: a cut-off write-back keeps it) */
    cache_unlock(c, f);
}

bool pager_cache_written_vlocked(struct pager_cache *c, struct page *p)
{
    struct pager_rec *rec = (struct pager_rec *)p->private;
    ASSERT(rec->state & PAGER_PAGE_WRITING);
    uint64_t f = cache_lock(c);
    bool clean = !(rec->state & PAGER_PAGE_DIRTY);
    list_del(&p->node);
    c->nwriting--;
    if (clean) {
        list_add_tail(&c->pages, &p->node);
        c->ndirty--;
        rec->state = 0;
        rec->dirtied = 0;
    } else {
        list_add_tail(&c->dirty, &p->node);
        rec->state = PAGER_PAGE_DIRTY;
        rec->dirtied = uptime_ns();   /* (dirty since its write-back's copy, at the latest) */
    }
    cache_unlock(c, f);
    return clean;
}

void pager_cache_unbegin_vlocked(struct pager_cache *c, struct page *p)
{
    struct pager_rec *rec = (struct pager_rec *)p->private;
    ASSERT(rec->state & PAGER_PAGE_WRITING);
    uint64_t f = cache_lock(c);
    list_del(&p->node);
    c->nwriting--;
    if (!list_empty(&c->dirty)) {   /* the head's age at most: `dirty` stays in order */
        const struct page *head = list_first(&c->dirty, struct page, node);
        uint64_t oldest = ((const struct pager_rec *)head->private)->dirtied;
        if (rec->dirtied > oldest)
            rec->dirtied = oldest;
    }
    list_add(&c->dirty, &p->node);
    rec->state = PAGER_PAGE_DIRTY;
    cache_unlock(c, f);
}

bool pager_cache_first(struct pager_cache *c, bool writing, struct vmo **v, uint64_t *idx,
                       uint64_t *dirtied)
{
    struct list_node *l = writing ? &c->writing : &c->dirty;
    bool any = false;
    uint64_t f = cache_lock(c);
    if (!list_empty(l)) {
        const struct page *p = list_first(l, struct page, node);
        const struct pager_rec *r = (const struct pager_rec *)p->private;
        /* Its VMO's memory is valid while the page is listed (pop_locked);
         * a dirty or writing page's VMO is held, so the reference comes. */
        any = kobject_tryref(vmo_kobject(r->v));
        if (any) {
            *v = r->v;
            *idx = r->idx;
            *dirtied = r->dirtied;
        }
    }
    cache_unlock(c, f);
    return any;
}

uint64_t pager_cache_resident(struct pager_cache *c)
{
    uint64_t f = cache_lock(c);
    uint64_t n = c->resident;
    cache_unlock(c, f);
    return n;
}

uint64_t pager_cache_ndirty(struct pager_cache *c)
{
    uint64_t f = cache_lock(c);
    uint64_t n = c->ndirty;
    cache_unlock(c, f);
    return n;
}

uint64_t pager_cache_nwriting(struct pager_cache *c)
{
    uint64_t f = cache_lock(c);
    uint64_t n = c->nwriting;
    cache_unlock(c, f);
    return n;
}
