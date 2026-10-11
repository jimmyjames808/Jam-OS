/* Pagers (see pager.h): the pager object, its keys, its request queue,
 * supplying pages and failing or detaching them.
 *
 * State. A pager has a table of keys (KEY_BUCKETS lists of struct
 * pager_vmo), a queue of undelivered requests and a list of delivered ones
 * still without an answer, all under the pager's object lock (class
 * "pager"), which is also its signals' lock: SIG_READABLE while the queue
 * is not empty. Each pager VMO's link (struct pager_vmo) is in one of four
 * states:
 *   NEW       made, not linked yet (pager_create_vmo's window);
 *   LINKED    in the key table: its VMO lives, requests are made and
 *             answered;
 *   DETACHED  cut off (PAGER_OP_DETACH, the pager's last handle): out of
 *             the table, every request failed, nothing published again;
 *             the link lives on with its VMO and goes when the VMO does;
 *   GONE      its VMO died while linked: still in the table (the key stays
 *             taken) with its PAGER_REQ_GONE queued, until the pager
 *             answers that with PAGER_OP_DETACH (or closes).
 *
 * Requests. A read request covers pages [first, end) of one VMO, which
 * were absent when it was made: the page a reader touched and up to
 * PAGER_REQ_PAGES - 1 absent pages after it, never a page another of the
 * VMO's requests covers, so a page is in at most one request and the
 * readers of a page all wait on one. Its references: the queue's while it
 * is unanswered, and one per waiter (and per vmo_page_request caller not
 * yet waiting). It is answered (`done`, with a status) by the end of a
 * supply that brought all its pages (`arrived`: each page counts once it
 * was published, even if the cache evicted it again since, so a budget
 * smaller than a request can't keep it from an answer; its waiters then
 * look again and ask again), by a failure, by its VMO's detach or death,
 * by a shrink past it, or at its deadline by the first waiter to wake then
 * (or by pager_next meeting it): no kernel timer. Answering takes it off
 * every list and wakes its waiters; whoever drops the last reference
 * frees it, always with no lock held (doomed requests are collected on a
 * private list, as port.c's reap does, since freeing one drops a pager
 * reference).
 *
 * Why requests live in the object: the pager process may die between
 * pager_next and its answer; the pager object (held by volumes) keeps the
 * request delivered, and the successor's PAGER_NEXT_AGAIN puts it back in
 * the queue. Supplying a present page is a no-op, so an answer given twice
 * is harmless.
 *
 * Budget: the pages of a pager's VMOs are its page cache (`cache`,
 * pager_cache.c): at most max_pages of them, the oldest evicted first. */
#include <jam/aspace_vmo.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/usercopy.h>
#include <jam/vmo.h>
#include <jam/vmo_verify.h>
#include "pager_cache.h"

#define KEY_BUCKETS 64u

enum pv_state { PV_NEW, PV_LINKED, PV_DETACHED, PV_GONE };

struct pager_req {
    struct list_node qnode;      /* the queue or delivered (pager lock); then a dead list */
    struct list_node vnode;      /* its VMO's link's reqs, while unanswered (read requests) */
    struct pager    *pg;         /* a reference (read requests; GONE's is its link's) */
    struct job      *job;        /* charged sizeof() message bytes (a reference), or NULL */
    uint64_t         key;        /* its VMO's key */
    uint64_t         first, end; /* page indices (read requests) */
    uint64_t         deadline;   /* uptime ns: failed ERR_TIMED_OUT from then on */
    uint32_t         kind;       /* PAGER_REQ_* */
    uint32_t         refs;       /* the queue's and each waiter's; pager lock */
    uint32_t         arrived;    /* bit i: page first + i was published since; pager lock */
    bool             queued;     /* on the queue or delivered; pager lock */
    bool             delivered;  /* on delivered rather than the queue; pager lock */
    bool             done;       /* answered: status says how; pager lock */
    status_t         status;     /* OK (supplied) or why it failed */
    struct waitqueue wq;         /* its waiters (class "pager request") */
};

struct pager_vmo {
    struct list_node knode;      /* in its key bucket while LINKED or GONE; pager lock */
    struct list_node work;       /* the pager's close: its private list */
    struct pager    *pg;         /* a reference; fixed */
    struct vmo      *v;          /* LINKED: its VMO, no reference (lookups tryref it) */
    uint64_t         key;        /* fixed */
    enum pv_state    state;      /* pager lock */
    struct list_node reqs;       /* its unanswered read requests; pager lock */
    struct pager_req gone;       /* its PAGER_REQ_GONE (no references, never waited on) */
};

struct pager {
    struct kobject   base;       /* OBJ_PAGER; base.lock guards everything below */
    struct job      *job;        /* pays for it and its VMOs (a reference), or NULL */
    uint64_t         answer_ns;  /* how long a request may wait */
    struct pager_cache cache;    /* its VMOs' resident pages and the budget (own lock) */
    bool             closed;     /* the last handle is gone: no new VMOs */
    struct list_node queue;      /* struct pager_req, undelivered, oldest first */
    struct list_node delivered;  /* struct pager_req, delivered, not answered */
    struct list_node keys[KEY_BUCKETS];   /* struct pager_vmo, LINKED or GONE */
};

static uint64_t plock(struct pager *pg)
{
    return spin_lock_irqsave(&pg->base.lock);
}

static void punlock(struct pager *pg, uint64_t f)
{
    spin_unlock_irqrestore(&pg->base.lock, f);
}

/* ---- requests ------------------------------------------------------------- */

static void req_init(struct pager_req *r, uint32_t kind, uint64_t key)
{
    list_init(&r->qnode);
    list_init(&r->vnode);
    r->kind = kind;
    r->key = key;
    waitqueue_init(&r->wq, "pager request");
}

status_t pager_req_alloc(struct pager_vmo *pv, struct pager_req **out)
{
    struct job *job = job_current();
    status_t st = job_charge(job, JOB_LIMIT_MSG_BYTES, sizeof(struct pager_req));
    if (st != OK)
        return ERR_NO_MEMORY;
    struct pager_req *r = kzalloc(sizeof(*r));
    if (!r) {
        job_uncharge(job, JOB_LIMIT_MSG_BYTES, sizeof(struct pager_req));
        return ERR_NO_MEMORY;
    }
    req_init(r, PAGER_REQ_READ, pv->key);
    job_ref(job);
    r->job = job;
    kobject_ref(pager_kobject(pv->pg));
    r->pg = pv->pg;
    *out = r;
    return OK;
}

void pager_req_free(struct pager_req *r)
{
    job_uncharge(r->job, JOB_LIMIT_MSG_BYTES, sizeof(struct pager_req));
    job_unref(r->job);
    kobject_unref(pager_kobject(r->pg));   /* may be the pager's last: no lock held */
    kfree(r);
}

/* Free the requests on dead (no lock held). */
static void reap(struct list_node *dead)
{
    while (!list_empty(dead)) {
        struct pager_req *r = list_first(dead, struct pager_req, qnode);
        list_del(&r->qnode);
        pager_req_free(r);
    }
}

static void unqueue_locked(struct pager_req *r)
{
    if (r->queued) {
        list_del(&r->qnode);
        r->queued = false;
        r->delivered = false;
    }
}

/* Pager lock held: answer read request r with st, wake its waiters, drop
 * the queue's reference (onto dead if it was the last). */
static void finish_locked(struct pager_req *r, status_t st, struct list_node *dead)
{
    r->done = true;
    r->status = st;
    list_del(&r->vnode);
    unqueue_locked(r);
    waitqueue_wake_all(&r->wq);
    if (--r->refs == 0)
        list_add_tail(dead, &r->qnode);
}

/* Pager lock held: fail every request of pv. */
static void fail_all_locked(struct pager_vmo *pv, status_t st, struct list_node *dead)
{
    while (!list_empty(&pv->reqs))
        finish_locked(list_first(&pv->reqs, struct pager_req, vnode), st, dead);
}

/* Pager lock held: SIG_READABLE exactly while a request waits undelivered. */
static void sync_readable_locked(struct pager *pg)
{
    bool any = !list_empty(&pg->queue);
    kobject_signal_locked(&pg->base, any ? 0 : SIG_READABLE, any ? SIG_READABLE : 0);
}

status_t pager_req_wait(struct pager_req *r)
{
    struct pager *pg = r->pg;
    struct list_node dead;
    list_init(&dead);
    status_t st = OK;
    uint64_t f = plock(pg);
    while (!r->done) {
        if (uptime_ns() >= r->deadline) {
            finish_locked(r, ERR_TIMED_OUT, &dead);
            sync_readable_locked(pg);
            break;
        }
        if (waitqueue_wait_cancellable(&r->wq, &pg->base.lock, &f, r->deadline) != OK &&
            !r->done) {
            st = ERR_CANCELED;
            break;
        }
    }
    if (r->done)
        st = r->status;
    punlock(pg, f);
    reap(&dead);
    return st;
}

void pager_req_put(struct pager_req *r)
{
    uint64_t f = plock(r->pg);
    uint32_t left = --r->refs;
    punlock(r->pg, f);
    if (left == 0)
        pager_req_free(r);
}

/* ---- the VMO side's calls, under the VMO lock ------------------------------ */

status_t pager_join_vlocked(struct pager_vmo *pv, uint64_t idx, uint64_t end,
                            struct pager_req **spare, struct pager_req **out)
{
    struct pager *pg = pv->pg;
    uint64_t f = plock(pg);
    if (pv->state != PV_LINKED) {
        punlock(pg, f);
        return ERR_PEER_CLOSED;
    }
    for (struct list_node *n = pv->reqs.next; n != &pv->reqs; n = n->next) {
        struct pager_req *r = container_of(n, struct pager_req, vnode);
        if (r->first <= idx && idx < r->end) {
            r->refs++;
            punlock(pg, f);
            *out = r;
            return OK;
        }
        if (r->first > idx && r->first < end)
            end = r->first;   /* a page is in one request at most */
    }
    struct pager_req *r = *spare;
    *spare = NULL;
    r->first = idx;
    r->end = end;
    r->deadline = uptime_ns() + pg->answer_ns;
    r->refs = 2;   /* the queue's and the caller's */
    r->queued = true;
    list_add_tail(&pv->reqs, &r->vnode);
    list_add_tail(&pg->queue, &r->qnode);
    sync_readable_locked(pg);
    punlock(pg, f);
    *out = r;
    return OK;
}

_Static_assert(PAGER_REQ_PAGES <= 32, "a request's arrived pages are a 32-bit mask");

/* Every page of r arrived. */
static bool arrived_all(const struct pager_req *r)
{
    return r->arrived == (uint32_t)((1ull << (r->end - r->first)) - 1);
}

void pager_published_vlocked(struct pager_vmo *pv, uint64_t idx, struct page *p,
                             struct pager_rec *rec)
{
    struct pager *pg = pv->pg;
    uint64_t f = plock(pg);
    for (struct list_node *n = pv->reqs.next; n != &pv->reqs; n = n->next) {
        struct pager_req *r = container_of(n, struct pager_req, vnode);
        if (r->first <= idx && idx < r->end) {
            r->arrived |= 1u << (idx - r->first);
            break;   /* a page is in one request at most */
        }
    }
    punlock(pg, f);
    pager_cache_add_vlocked(&pg->cache, p, rec);
}

void pager_page_gone_vlocked(struct pager_vmo *pv, struct page *p)
{
    pager_cache_remove_vlocked(&pv->pg->cache, p);
}

void pager_cut_vlocked(struct pager_vmo *pv, uint64_t end, struct list_node *dead)
{
    struct pager *pg = pv->pg;
    uint64_t f = plock(pg);
    for (struct list_node *n = pv->reqs.next; n != &pv->reqs;) {
        struct pager_req *r = container_of(n, struct pager_req, vnode);
        n = n->next;   /* finish_locked unlinks r */
        if (r->end <= end)
            continue;
        if (r->first < end) {
            r->end = end;
            r->arrived &= (uint32_t)((1ull << (end - r->first)) - 1);
        }
        if (r->first >= end || arrived_all(r))
            finish_locked(r, OK, dead);   /* the waiters look again */
    }
    sync_readable_locked(pg);
    punlock(pg, f);
}

/* ---- keys -------------------------------------------------------------------- */

static struct list_node *bucket(struct pager *pg, uint64_t key)
{
    return &pg->keys[(key * 0x9e3779b97f4a7c15ull) >> 58];   /* 64 buckets: the top 6 bits */
}
_Static_assert(KEY_BUCKETS == 64, "bucket() takes 6 bits");

/* Pager lock held: key's link (LINKED or GONE), or NULL. */
static struct pager_vmo *find_locked(struct pager *pg, uint64_t key)
{
    struct list_node *b = bucket(pg, key);
    for (struct list_node *n = b->next; n != b; n = n->next) {
        struct pager_vmo *pv = container_of(n, struct pager_vmo, knode);
        if (pv->key == key)
            return pv;
    }
    return NULL;
}

/* Key's live VMO with a reference, or NULL. */
static struct vmo *lookup_vmo(struct pager *pg, uint64_t key)
{
    uint64_t f = plock(pg);
    struct pager_vmo *pv = find_locked(pg, key);
    struct vmo *v = pv && pv->state == PV_LINKED && kobject_tryref(vmo_kobject(pv->v)) ? pv->v
                                                                                      : NULL;
    punlock(pg, f);
    return v;
}

/* A link nothing else refers to any more. */
static void pv_free(struct pager_vmo *pv)
{
    struct pager *pg = pv->pg;
    job_uncharge(pg->job, JOB_LIMIT_HANDLES, 1);
    kfree(pv);
    kobject_unref(pager_kobject(pg));
}

/* Pager lock held: pv (LINKED) cut off: out of the table, its requests
 * failed. Returns its VMO with a reference for the caller to drop the
 * pages of (vmo_pager_detach), or NULL if the VMO is dying (its
 * destruction frees pv). */
static struct vmo *detach_locked(struct pager_vmo *pv, struct list_node *dead)
{
    list_del(&pv->knode);
    pv->state = PV_DETACHED;
    fail_all_locked(pv, ERR_PEER_CLOSED, dead);
    return kobject_tryref(vmo_kobject(pv->v)) ? pv->v : NULL;
}

static void drop_vmo(struct vmo *v)
{
    if (v) {
        vmo_pager_detach(v);
        kobject_unref(vmo_kobject(v));   /* may destroy it, and free its link */
    }
}

void pager_vmo_gone(struct pager_vmo *pv)
{
    struct pager *pg = pv->pg;
    struct list_node dead;
    list_init(&dead);
    uint64_t f = plock(pg);
    bool linked = pv->state == PV_LINKED;
    if (linked) {
        pv->state = PV_GONE;
        pv->v = NULL;
        fail_all_locked(pv, ERR_PEER_CLOSED, &dead);   /* none has a waiter: each holds the VMO */
        pv->gone.queued = true;
        list_add_tail(&pg->queue, &pv->gone.qnode);
        sync_readable_locked(pg);
    }
    punlock(pg, f);
    reap(&dead);
    if (!linked)
        pv_free(pv);
}

uint64_t pager_vmo_pager_koid(const struct pager_vmo *pv)
{
    return pv->pg->base.koid;
}

/* ---- the object ------------------------------------------------------------ */

/* The last handle is gone: every VMO is detached, every GONE answered. */
static void pager_closed(struct kobject *obj)
{
    struct pager *pg = (struct pager *)obj;
    struct list_node dead, work;
    list_init(&dead);
    list_init(&work);
    uint64_t f = plock(pg);
    pg->closed = true;
    for (unsigned b = 0; b < KEY_BUCKETS; b++) {
        while (!list_empty(&pg->keys[b])) {
            struct pager_vmo *pv = list_first(&pg->keys[b], struct pager_vmo, knode);
            if (pv->state == PV_GONE) {
                list_del(&pv->knode);
                unqueue_locked(&pv->gone);
                list_add_tail(&work, &pv->work);
            } else if (detach_locked(pv, &dead)) {
                list_add_tail(&work, &pv->work);
            }
        }
    }
    sync_readable_locked(pg);
    punlock(pg, f);
    reap(&dead);
    while (!list_empty(&work)) {
        struct pager_vmo *pv = list_first(&work, struct pager_vmo, work);
        list_del(&pv->work);
        if (pv->state == PV_GONE)
            pv_free(pv);
        else
            drop_vmo(pv->v);   /* (pv may be freed by it) */
    }
}

static void pager_destroy(struct kobject *obj)
{
    struct pager *pg = (struct pager *)obj;
    /* Every link and request holds a reference: none can be left. */
    ASSERT(list_empty(&pg->queue) && list_empty(&pg->delivered));
    ASSERT(pager_cache_resident(&pg->cache) == 0);   /* a page's VMO's link holds the pager */
    job_uncharge(pg->job, JOB_LIMIT_HANDLES, 1);
    job_unref(pg->job);
    kfree(pg);
}

static const struct kobject_ops pager_ops = {
    .name = "pager",
    .destroy = pager_destroy,
    .on_zero_handles = pager_closed,
};

status_t pager_create(struct job *job, uint64_t max_pages, uint64_t answer_ns,
                      struct pager **out)
{
    if (max_pages == 0)
        return ERR_INVALID_ARGS;
    if (answer_ns < PAGER_ANSWER_MIN_NS || answer_ns > PAGER_ANSWER_MAX_NS)
        return ERR_OUT_OF_RANGE;
    status_t st = job_charge(job, JOB_LIMIT_HANDLES, 1);
    if (st != OK)
        return st;
    struct pager *pg = kzalloc(sizeof(*pg));
    if (!pg) {
        job_uncharge(job, JOB_LIMIT_HANDLES, 1);
        return ERR_NO_MEMORY;
    }
    kobject_init(&pg->base, OBJ_PAGER, &pager_ops, "pager", 0);
    job_ref(job);
    pg->job = job;
    pg->answer_ns = answer_ns;
    pager_cache_init(&pg->cache, job, max_pages);
    list_init(&pg->queue);
    list_init(&pg->delivered);
    for (unsigned b = 0; b < KEY_BUCKETS; b++)
        list_init(&pg->keys[b]);
    *out = pg;
    return OK;
}

/* A NEW link for key, charged to pg's job. */
static status_t pv_new(struct pager *pg, uint64_t key, struct pager_vmo **out)
{
    status_t st = job_charge(pg->job, JOB_LIMIT_HANDLES, 1);
    if (st != OK)
        return st;
    struct pager_vmo *pv = kzalloc(sizeof(*pv));
    if (!pv) {
        job_uncharge(pg->job, JOB_LIMIT_HANDLES, 1);
        return ERR_NO_MEMORY;
    }
    kobject_ref(pager_kobject(pg));
    pv->pg = pg;
    pv->key = key;
    pv->state = PV_NEW;
    list_init(&pv->reqs);
    req_init(&pv->gone, PAGER_REQ_GONE, key);
    *out = pv;
    return OK;
}

status_t pager_create_vmo(struct pager *pg, uint64_t key, uint64_t size, struct vmo **out)
{
    if (size > VMO_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    struct pager_vmo *pv;
    status_t st = pv_new(pg, key, &pv);
    if (st != OK)
        return st;
    struct vmo *v;
    if ((st = vmo_create_pager(pg->job, size, pv, &v)) != OK) {
        pv_free(pv);
        return st;
    }
    uint64_t f = plock(pg);
    st = pg->closed ? ERR_BAD_STATE : find_locked(pg, key) ? ERR_ALREADY_EXISTS : OK;
    if (st == OK) {
        pv->v = v;
        pv->state = PV_LINKED;
        list_add_tail(bucket(pg, key), &pv->knode);
    }
    punlock(pg, f);
    if (st != OK) {
        kobject_unref(vmo_kobject(v));   /* a NEW link: its destruction frees pv */
        return st;
    }
    *out = v;
    return OK;
}

/* Pager lock held: every delivered request back at the queue's head, in
 * the order they were delivered. */
static void again_locked(struct pager *pg)
{
    while (!list_empty(&pg->delivered)) {
        struct pager_req *r = container_of(pg->delivered.prev, struct pager_req, qnode);
        list_del(&r->qnode);
        list_add(&pg->queue, &r->qnode);
        r->delivered = false;
    }
}

status_t pager_next(struct pager *pg, uint32_t flags, struct pager_request *out)
{
    if (flags & ~PAGER_NEXT_AGAIN)
        return ERR_INVALID_ARGS;
    memset(out, 0, sizeof(*out));
    struct list_node dead;
    list_init(&dead);
    status_t st = ERR_SHOULD_WAIT;
    uint64_t f = plock(pg);
    if (flags & PAGER_NEXT_AGAIN)
        again_locked(pg);
    while (st == ERR_SHOULD_WAIT && !list_empty(&pg->queue)) {
        struct pager_req *r = list_first(&pg->queue, struct pager_req, qnode);
        if (r->kind == PAGER_REQ_READ && uptime_ns() >= r->deadline) {
            finish_locked(r, ERR_TIMED_OUT, &dead);
            continue;
        }
        list_del(&r->qnode);
        list_add_tail(&pg->delivered, &r->qnode);
        r->delivered = true;
        out->key = r->key;
        out->kind = r->kind;
        if (r->kind == PAGER_REQ_READ) {
            out->offset = r->first << PAGE_SHIFT;
            out->len = (r->end - r->first) << PAGE_SHIFT;
        }
        st = OK;
    }
    sync_readable_locked(pg);
    punlock(pg, f);
    reap(&dead);
    return st;
}

/* ---- answers ------------------------------------------------------------------ */

/* A supply ended: key's requests all of whose pages arrived are answered.
 * At the end, not page by page, so that a request is answered once,
 * after the eviction a supply bigger than the budget makes of its own
 * pages (the ktests that evict on every supply rely on it). */
static void answer_arrived(struct pager *pg, uint64_t key)
{
    struct list_node dead;
    list_init(&dead);
    uint64_t f = plock(pg);
    struct pager_vmo *pv = find_locked(pg, key);
    if (pv && pv->state == PV_LINKED) {
        for (struct list_node *n = pv->reqs.next; n != &pv->reqs;) {
            struct pager_req *r = container_of(n, struct pager_req, vnode);
            n = n->next;   /* finish_locked unlinks r */
            if (arrived_all(r))
                finish_locked(r, OK, &dead);
        }
        sync_readable_locked(pg);
    }
    punlock(pg, f);
    reap(&dead);
}

/* Page idx of v from src at byte `at`: copied into a fresh page with no
 * lock held, checked if v is verified, then published inside the cache's
 * budget. ERR_IO: the bytes aren't the approved ones (<jam/vmo_verify.h>),
 * and nothing changed. */
static status_t supply_page(struct pager *pg, struct vmo *v, uint64_t idx,
                            const struct pager_src *src, uint64_t at)
{
    struct page *p = pmm_alloc_pages(0, 0);   /* every byte is written below */
    while (!p && pager_cache_evict_oldest(&pg->cache))   /* memory runs short: our own pages go */
        p = pmm_alloc_pages(0, 0);
    if (!p)
        return ERR_NO_MEMORY;
    void *dst = page_to_virt(p);
    status_t st = OK;
    if (src->user)
        st = copy_from_user(dst, src->addr + at, PAGE_SIZE) == OK ? OK : ERR_INVALID_ARGS;
    else
        memcpy(dst, (const char *)(uintptr_t)src->addr + at, PAGE_SIZE);
    bool checked = false;
    if (st == OK && !vmo_verify_supply(v, idx, dst, &checked))
        st = ERR_IO;
    if (st != OK) {
        page_unref(p);
        return st;
    }
    st = pager_publish_page(pg, v, idx, p, checked);
    return st == ERR_PEER_CLOSED ? ERR_NOT_FOUND : st;   /* detached meanwhile */
}

status_t pager_publish_page(struct pager *pg, struct vmo *v, uint64_t idx, struct page *p,
                            bool checked)
{
    return pager_cache_publish(&pg->cache, v, idx, p, checked);
}

/* The pages of one pager_supply call that didn't match (bit i: its i-th). */
struct unapproved {
    uint64_t bits[PAGER_SUPPLY_MAX / PAGE_SIZE / 64];
    bool     any;
};

static void fail_unapproved(struct pager *pg, uint64_t key, uint64_t first,
                            const struct unapproved *u);

status_t pager_supply(struct pager *pg, uint64_t key, uint64_t offset, const struct pager_src *src,
                      uint64_t len)
{
    if (((offset | len) & (PAGE_SIZE - 1)) || len == 0)
        return ERR_INVALID_ARGS;
    if (len > PAGER_SUPPLY_MAX)
        return ERR_OUT_OF_RANGE;
    struct vmo *v = lookup_vmo(pg, key);
    if (!v)
        return ERR_NOT_FOUND;
    uint64_t size = vmo_size(v), first = offset >> PAGE_SHIFT;
    status_t st = offset <= size && len <= size - offset ? OK : ERR_OUT_OF_RANGE;
    struct unapproved bad = { { 0 }, false };
    for (uint64_t i = 0; i < len >> PAGE_SHIFT && st == OK; i++) {
        status_t one = supply_page(pg, v, first + i, src, i << PAGE_SHIFT);
        if (one == ERR_IO) {   /* a page that doesn't match: the rest still come */
            bad.bits[i / 64] |= 1ull << (i % 64);
            bad.any = true;
        } else {
            st = one;
        }
    }
    kobject_unref(vmo_kobject(v));
    answer_arrived(pg, key);   /* (also after a failure: the pages before it stay) */
    /* Only now, with the good pages of the range published: a waiter on
     * one of them sees it there and goes on. */
    if (bad.any) {
        fail_unapproved(pg, key, first, &bad);
        if (st == OK)
            st = ERR_IO;
    }
    return st;
}

uint64_t pager_resident(struct pager *pg)
{
    return pager_cache_resident(&pg->cache);
}

/* PAGER_OP_FAIL: key's requests overlapping pages [first, end) fail st. */
static status_t fail_range(struct pager *pg, uint64_t key, uint64_t first, uint64_t end,
                           status_t st)
{
    struct list_node dead;
    list_init(&dead);
    uint64_t f = plock(pg);
    struct pager_vmo *pv = find_locked(pg, key);
    if (!pv || pv->state != PV_LINKED) {
        punlock(pg, f);
        return ERR_NOT_FOUND;
    }
    for (struct list_node *n = pv->reqs.next; n != &pv->reqs;) {
        struct pager_req *r = container_of(n, struct pager_req, vnode);
        n = n->next;   /* finish_locked unlinks r */
        if (r->first < end && first < r->end)
            finish_locked(r, st, &dead);
    }
    sync_readable_locked(pg);
    punlock(pg, f);
    reap(&dead);
    return OK;
}

/* The requests waiting on pages of a verified VMO that didn't match fail
 * ERR_BAD_STATE (the faulters die with "a page doesn't match the approved
 * program"); pages from first, bit by bit. */
static void fail_unapproved(struct pager *pg, uint64_t key, uint64_t first,
                            const struct unapproved *u)
{
    for (uint64_t i = 0; i < PAGER_SUPPLY_MAX / PAGE_SIZE; i++)
        if (u->bits[i / 64] & (1ull << (i % 64)))
            (void)fail_range(pg, key, first + i, first + i + 1, ERR_BAD_STATE);   /* (gone: none) */
}

/* PAGER_OP_INVALIDATE: drop key's present pages in [offset, offset + len). */
static status_t invalidate(struct pager *pg, uint64_t key, uint64_t offset, uint64_t len)
{
    struct vmo *v = lookup_vmo(pg, key);
    if (!v)
        return ERR_NOT_FOUND;
    status_t st = vmo_pager_invalidate(v, offset, len);
    kobject_unref(vmo_kobject(v));
    return st;
}

/* PAGER_OP_SET_SIZE: key's VMO grows or shrinks to size (page-aligned). */
static status_t set_size(struct pager *pg, uint64_t key, uint64_t size)
{
    struct vmo *v = lookup_vmo(pg, key);
    if (!v)
        return ERR_NOT_FOUND;
    struct list_node dead;
    list_init(&dead);
    status_t st = vmo_pager_set_size(v, size, &dead);
    reap(&dead);
    kobject_unref(vmo_kobject(v));
    return st == ERR_PEER_CLOSED ? ERR_NOT_FOUND : st;   /* detached meanwhile */
}

/* PAGER_OP_DETACH: cut key's VMO off, or answer its PAGER_REQ_GONE. */
static status_t detach_key(struct pager *pg, uint64_t key)
{
    struct list_node dead;
    list_init(&dead);
    uint64_t f = plock(pg);
    struct pager_vmo *pv = find_locked(pg, key);
    struct pager_vmo *freed = NULL;
    struct vmo *v = NULL;
    if (pv && pv->state == PV_GONE) {
        list_del(&pv->knode);
        unqueue_locked(&pv->gone);
        freed = pv;
    } else if (pv) {
        v = detach_locked(pv, &dead);
    }
    sync_readable_locked(pg);
    punlock(pg, f);
    reap(&dead);
    if (freed)
        pv_free(freed);
    drop_vmo(v);
    return pv ? OK : ERR_NOT_FOUND;
}

status_t pager_op(struct pager *pg, uint64_t key, uint32_t op, uint64_t offset, uint64_t len,
                  int64_t arg)
{
    switch (op) {
    case PAGER_OP_FAIL:
        if (arg != ERR_IO && arg != ERR_OUT_OF_RANGE)
            return ERR_INVALID_ARGS;
        if (((offset | len) & (PAGE_SIZE - 1)) || len == 0 || len > UINT64_MAX - offset)
            return ERR_INVALID_ARGS;
        return fail_range(pg, key, offset >> PAGE_SHIFT, (offset + len) >> PAGE_SHIFT,
                          (status_t)arg);
    case PAGER_OP_DETACH:
        if (offset || len || arg)
            return ERR_INVALID_ARGS;
        return detach_key(pg, key);
    case PAGER_OP_INVALIDATE:
        if (arg || ((offset | len) & (PAGE_SIZE - 1)) || len == 0 || len > UINT64_MAX - offset)
            return ERR_INVALID_ARGS;
        return invalidate(pg, key, offset, len);
    case PAGER_OP_SET_SIZE:
        if (len || arg)
            return ERR_INVALID_ARGS;
        if (offset > VMO_MAX_SIZE)
            return ERR_OUT_OF_RANGE;
        return set_size(pg, key, ALIGN_UP(offset, PAGE_SIZE));
    default:
        return ERR_INVALID_ARGS;
    }
}
