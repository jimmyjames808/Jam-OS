/* Pagers (see pager.h): the pager object, its keys, its request queue,
 * supplying pages, failing or detaching them, and write-back.
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
 *   DETACHED  cut off (PAGER_OP_DETACH, the pager's last handle, or its
 *             key made again while its VMO was being destroyed): out of
 *             the table, every request failed, nothing published again;
 *             the link lives on with its VMO and goes when the VMO does;
 *   GONE      its VMO died while linked: still in the table (the key stays
 *             taken) with its PAGER_REQ_GONE queued, until the pager
 *             answers that with PAGER_OP_DETACH (or closes), or makes the
 *             key again, which takes it over (the GONE withdrawn, the
 *             link freed). A writable VMO dies only once its dirty pages
 *             are gone (the hold, vmo.c), so a GONE always means clean.
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
 * Write-back requests (struct pager_wb) cover at most PAGER_SUPPLY_MAX of
 * one writable VMO's pages, which may be in other write-back requests too
 * (they are on the link's `wbs`, not `reqs`). What one needs (the rule
 * that makes vmo_sync exact) is kept page by page in two bits, set when
 * it is made: `need`, the page must still be written back (it was dirty
 * or writing), and `begin`, by a write-back that begins later (it was
 * dirty: stored to since its last write-back began). A write-back's
 * begin clears `begin` (pager_begun_vlocked), its end clears `need` where
 * `begin` is clear (pager_written_vlocked): so a page writing when the
 * request was made needs that write-back's end, a dirty one a whole
 * write-back after it, and a page dirtied later nothing. Only one
 * write-back of a page runs at a time (BEGIN takes dirty pages that are
 * not writing; one stored to while writing waits for that write-back's
 * end), so the end of a page's write-back is the end of the one its
 * `begin` was cleared by; a write-back cut off by a death (or a copy that
 * failed) leaves its pages dirty, and the next end of one of them is then
 * of a write-back begun later still: the bits need no change. The request
 * is answered when no bit is left (with the first error a write-back of
 * one of its pages ended with), by a shrink's dropping its pages (done),
 * or failed with its VMO's detach. One left needing pages nobody is
 * writing back after a PAGER_OP_WRITTEN goes back in the queue, so a pager
 * that answered it sees it again (a page that was being written when it
 * was made, and stored to since).
 *
 * The request for the oldest run (`bg`), one at a time per pager: when a
 * store takes a pager's dirty and writing pages past half its dirty limit,
 * the run of dirty pages around its oldest one (pager_writeback_kick, the
 * background request, charged to the pager's job). A store at the limit
 * (pager_throttle, the throttle) joins it and waits, or makes it if there
 * is none (charged to the storer's job; around the first page being
 * written back if none is dirty, so its end is that write-back's). One
 * nobody waits on ends at its deadline when the next kick, throttle or
 * pager_next meets it.
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
    struct list_node vnode;      /* its VMO's link's reqs (read) or wbs (write-back), while
                                    unanswered */
    struct pager    *pg;         /* a reference (GONE's is its link's) */
    struct job      *job;        /* charged req_bytes() message bytes (a reference), or NULL */
    uint64_t         key;        /* its VMO's key */
    uint64_t         first, end; /* page indices (read and write-back requests) */
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

/* A write-back request: its pages' bits (the file header's "Write-back
 * requests"), page first + i at bit i; pager lock. */
struct pager_wb {
    struct pager_req r;
    uint64_t         need[PAGER_WB_WORDS];    /* must still be written back */
    uint64_t         begin[PAGER_WB_WORDS];   /* by a write-back that begins later */
    status_t         err;                     /* the first error one of them ended with */
};

#define RUN_PAGES (PAGER_SUPPLY_MAX / PAGE_SIZE)
_Static_assert(PAGER_WB_WORDS * 64 == RUN_PAGES, "a write-back request's bits");

static struct pager_wb *wb_of(struct pager_req *r)
{
    return container_of(r, struct pager_wb, r);
}

static uint64_t req_bytes(uint32_t kind)
{
    return kind == PAGER_REQ_WRITEBACK ? sizeof(struct pager_wb) : sizeof(struct pager_req);
}

struct pager_vmo {
    struct list_node knode;      /* in its key bucket while LINKED or GONE; pager lock */
    struct list_node work;       /* the pager's close: its private list */
    struct pager    *pg;         /* a reference; fixed */
    struct vmo      *v;          /* LINKED: its VMO, no reference (lookups tryref it) */
    uint64_t         key;        /* fixed */
    enum pv_state    state;      /* pager lock */
    struct list_node reqs;       /* its unanswered read requests; pager lock */
    struct list_node wbs;        /* its unanswered write-back requests; pager lock */
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
    struct pager_req *bg;        /* the background write-back request, unanswered, or NULL */
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

/* A request of `kind` for key's VMO of pg, charged to job. */
static status_t req_new(struct pager *pg, uint64_t key, struct job *job, uint32_t kind,
                        struct pager_req **out)
{
    uint64_t bytes = req_bytes(kind);
    status_t st = job_charge(job, JOB_LIMIT_MSG_BYTES, bytes);
    if (st != OK)
        return ERR_NO_MEMORY;
    struct pager_req *r = kzalloc(bytes);   /* (a write-back request: r is its first member) */
    if (!r) {
        job_uncharge(job, JOB_LIMIT_MSG_BYTES, bytes);
        return ERR_NO_MEMORY;
    }
    req_init(r, kind, key);
    job_ref(job);
    r->job = job;
    kobject_ref(pager_kobject(pg));
    r->pg = pg;
    *out = r;
    return OK;
}

status_t pager_req_alloc(struct pager_vmo *pv, struct pager_req **out)
{
    return req_new(pv->pg, pv->key, job_current(), PAGER_REQ_READ, out);
}

status_t pager_wb_alloc(struct pager_vmo *pv, struct job *job, struct pager_req **out)
{
    return req_new(pv->pg, pv->key, job, PAGER_REQ_WRITEBACK, out);
}

void pager_req_free(struct pager_req *r)
{
    job_uncharge(r->job, JOB_LIMIT_MSG_BYTES, req_bytes(r->kind));
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

/* Pager lock held: answer read or write-back request r with st, wake its
 * waiters, drop the queue's reference (onto dead if it was the last). */
static void finish_locked(struct pager_req *r, status_t st, struct list_node *dead)
{
    r->done = true;
    r->status = st;
    list_del(&r->vnode);
    unqueue_locked(r);
    if (r->pg->bg == r)
        r->pg->bg = NULL;
    waitqueue_wake_all(&r->wq);
    if (--r->refs == 0)
        list_add_tail(dead, &r->qnode);
}

/* Pager lock held: fail every request of pv. */
static void fail_all_locked(struct pager_vmo *pv, status_t st, struct list_node *dead)
{
    while (!list_empty(&pv->reqs))
        finish_locked(list_first(&pv->reqs, struct pager_req, vnode), st, dead);
    while (!list_empty(&pv->wbs))
        finish_locked(list_first(&pv->wbs, struct pager_req, vnode), st, dead);
}

/* Pager lock held: SIG_READABLE exactly while a request waits undelivered. */
static void sync_readable_locked(struct pager *pg)
{
    bool any = !list_empty(&pg->queue);
    kobject_signal_locked(&pg->base, any ? 0 : SIG_READABLE, any ? SIG_READABLE : 0);
}

status_t pager_req_wait(struct pager_req *r)
{
    return pager_req_wait_until(r, DEADLINE_NEVER);
}

status_t pager_req_wait_until(struct pager_req *r, uint64_t until)
{
    struct pager *pg = r->pg;
    struct list_node dead;
    list_init(&dead);
    status_t st = OK;
    uint64_t f = plock(pg);
    while (!r->done) {
        uint64_t now = uptime_ns();
        if (now >= r->deadline) {
            finish_locked(r, ERR_TIMED_OUT, &dead);
            sync_readable_locked(pg);
            break;
        }
        if (now >= until) {
            st = ERR_TIMED_OUT;   /* the caller's time, not r's: r stays */
            break;
        }
        uint64_t d = r->deadline < until ? r->deadline : until;
        if (waitqueue_wait_cancellable(&r->wq, &pg->base.lock, &f, d) != OK && !r->done) {
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

bool pager_req_answered(struct pager_req *r)
{
    uint64_t f = plock(r->pg);
    bool done = r->done;
    punlock(r->pg, f);
    return done;
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

unsigned pager_page_state_vlocked(struct pager_vmo *pv, const struct page *p)
{
    (void)pv;
    return pager_cache_state_vlocked(p);
}

uint64_t pager_page_dirtied_vlocked(struct pager_vmo *pv, const struct page *p)
{
    (void)pv;
    return pager_cache_dirtied_vlocked(p);
}

status_t pager_page_store_vlocked(struct pager_vmo *pv, struct page *p)
{
    return pager_cache_store_vlocked(&pv->pg->cache, p, pager_dirty_limit(pv->pg));
}

void pager_page_begin_vlocked(struct pager_vmo *pv, struct page *p)
{
    pager_cache_begin_vlocked(&pv->pg->cache, p);
}

bool pager_page_written_vlocked(struct pager_vmo *pv, struct page *p, bool failed)
{
    return pager_cache_written_vlocked(&pv->pg->cache, p, failed);
}

bool pager_page_kept_vlocked(struct pager_vmo *pv, const struct page *p)
{
    (void)pv;
    return pager_cache_kept_vlocked(p);
}

void pager_page_unbegin_vlocked(struct pager_vmo *pv, struct page *p)
{
    pager_cache_unbegin_vlocked(&pv->pg->cache, p);
}

/* ---- write-back requests (the file header's "Write-back requests") ------- */

static bool bit(const uint64_t *b, uint64_t i)
{
    return (b[i / 64] >> (i % 64)) & 1;
}

static void bit_clear(uint64_t *b, uint64_t i)
{
    b[i / 64] &= ~(1ull << (i % 64));
}

static bool bits_none(const uint64_t *b)
{
    for (unsigned w = 0; w < PAGER_WB_WORDS; w++)
        if (b[w])
            return false;
    return true;
}

/* Pager lock held: the overlap of write-back request r with pages [first,
 * end), as r's bit numbers [*lo, *hi); false if none. */
static bool wb_overlap(const struct pager_req *r, uint64_t first, uint64_t end, uint64_t *lo,
                       uint64_t *hi)
{
    uint64_t a = r->first > first ? r->first : first, b = r->end < end ? r->end : end;
    *lo = a - r->first;
    *hi = b - r->first;
    return a < b;
}

status_t pager_wb_queue_vlocked(struct pager_vmo *pv, struct pager_req *r, uint64_t first,
                                uint64_t end, const uint64_t *need, const uint64_t *begin,
                                bool caller, bool bg)
{
    struct pager *pg = pv->pg;
    struct pager_wb *w = wb_of(r);
    uint64_t f = plock(pg);
    status_t st = pv->state != PV_LINKED ? ERR_PEER_CLOSED : bg && pg->bg ? ERR_ALREADY_EXISTS : OK;
    if (st == OK) {
        r->key = pv->key;   /* (the background one's is known only now) */
        r->first = first;
        r->end = end;
        memcpy(w->need, need, sizeof(w->need));
        memcpy(w->begin, begin, sizeof(w->begin));
        w->err = OK;
        r->deadline = uptime_ns() + pg->answer_ns;
        r->refs = caller ? 2 : 1;   /* the queue's, and the caller's */
        r->queued = true;
        list_add_tail(&pv->wbs, &r->vnode);
        list_add_tail(&pg->queue, &r->qnode);
        if (bg)
            pg->bg = r;
        sync_readable_locked(pg);
    }
    punlock(pg, f);
    return st;
}

void pager_begun_vlocked(struct pager_vmo *pv, uint64_t first, uint64_t end)
{
    struct pager *pg = pv->pg;
    uint64_t f = plock(pg);
    for (struct list_node *n = pv->wbs.next; n != &pv->wbs; n = n->next) {
        struct pager_req *r = container_of(n, struct pager_req, vnode);
        uint64_t lo, hi;
        if (wb_overlap(r, first, end, &lo, &hi))
            for (uint64_t i = lo; i < hi; i++)
                bit_clear(wb_of(r)->begin, i);
    }
    punlock(pg, f);
}

/* Pager lock held (and v's): does write-back request r still need a page
 * that is being written back? */
static bool wb_waits_on_writing(struct pager_req *r, struct vmo *v)
{
    for (uint64_t i = 0; i < r->end - r->first; i++)
        if (bit(wb_of(r)->need, i) && vmo_pager_writing_locked(v, r->first + i))
            return true;
    return false;
}

void pager_written_vlocked(struct pager_vmo *pv, struct vmo *v, uint64_t first, uint64_t end,
                           const uint64_t *ended, status_t st, struct list_node *dead)
{
    struct pager *pg = pv->pg;
    uint64_t f = plock(pg);
    for (struct list_node *n = pv->wbs.next; n != &pv->wbs;) {
        struct pager_req *r = container_of(n, struct pager_req, vnode);
        struct pager_wb *w = wb_of(r);
        n = n->next;   /* finish_locked unlinks r */
        uint64_t lo, hi;
        if (!wb_overlap(r, first, end, &lo, &hi))
            continue;
        for (uint64_t i = lo; i < hi; i++) {
            if (!bit(ended, r->first + i - first) || !bit(w->need, i) || bit(w->begin, i))
                continue;
            bit_clear(w->need, i);
            if (st != OK && w->err == OK)
                w->err = st;
        }
        if (bits_none(w->need)) {
            finish_locked(r, w->err, dead);
        } else if (r->delivered && !wb_waits_on_writing(r, v)) {
            list_del(&r->qnode);   /* the pager's answer left pages it must begin again */
            list_add_tail(&pg->queue, &r->qnode);
            r->delivered = false;
        }
    }
    sync_readable_locked(pg);
    punlock(pg, f);
}

uint64_t pager_dirty_limit(const struct pager *pg)
{
    return pg->cache.max_pages / 4;
}

/* No lock held: has pg its one request for the oldest run (`bg`)? One past
 * its deadline fails first (nobody may be waiting on it to end it). OK:
 * none; ERR_ALREADY_EXISTS: there is one, and with `join` the caller gets
 * a reference to it; ERR_PEER_CLOSED: pg is closed, or pv (if not NULL)
 * cut off. */
static status_t oldest_req(struct pager *pg, struct pager_vmo *pv, struct pager_req **join)
{
    struct list_node dead;
    list_init(&dead);
    uint64_t f = plock(pg);
    if (pg->bg && uptime_ns() >= pg->bg->deadline)
        finish_locked(pg->bg, ERR_TIMED_OUT, &dead);
    status_t st = pg->closed || (pv && pv->state != PV_LINKED) ? ERR_PEER_CLOSED
                : pg->bg                                         ? ERR_ALREADY_EXISTS
                                                                 : OK;
    if (st == ERR_ALREADY_EXISTS && join) {
        pg->bg->refs++;
        *join = pg->bg;
    }
    sync_readable_locked(pg);
    punlock(pg, f);
    reap(&dead);
    return st;
}

/* No lock held: queue r (a write-back request, never queued) as pg's one
 * request for the oldest run: the run of dirty pages around its oldest
 * dirty page, or with `writing`, if none is dirty, the run of dirty and
 * writing pages around the first being written back; with a reference for
 * the caller if `caller`. ERR_NOT_FOUND: none (or it moved on meanwhile);
 * ERR_ALREADY_EXISTS: pg has one; ERR_PEER_CLOSED: its VMO is cut off. r
 * is the caller's to free unless OK. */
static status_t queue_oldest(struct pager *pg, struct pager_req *r, bool writing, bool caller)
{
    struct vmo *v;
    uint64_t idx, dirtied;
    bool w = false;
    if (!pager_cache_first(&pg->cache, false, &v, &idx, &dirtied)) {
        if (!writing || !pager_cache_first(&pg->cache, true, &v, &idx, &dirtied))
            return ERR_NOT_FOUND;
        w = true;
    }
    status_t st = vmo_pager_writeback_bg(v, idx, w, r, caller);
    kobject_unref(vmo_kobject(v));   /* (the last if written back meanwhile: no lock held) */
    return st;
}

/* No lock held: past half the dirty limit, pg's background request for its
 * oldest dirty run, unless it has one. */
static void kick(struct pager *pg)
{
    if (pager_cache_ndirty(&pg->cache) <= pager_dirty_limit(pg) / 2)
        return;
    struct pager_req *r;
    if (oldest_req(pg, NULL, NULL) != OK || req_new(pg, 0, pg->job, PAGER_REQ_WRITEBACK, &r) != OK)
        return;   /* (one there, closed, or no memory: the next store tries again) */
    if (queue_oldest(pg, r, false, false) != OK)
        pager_req_free(r);   /* (written back meanwhile, or another store made it) */
}

/* How many times pager_throttle looks for the oldest run while its pages
 * move on under it, before it lets the store look again. */
#define THROTTLE_TRIES 4

status_t pager_throttle(struct pager_vmo *pv, uint64_t *until, struct pager_req **out)
{
    struct pager *pg = pv->pg;
    *out = NULL;
    if (!*until)
        *until = uptime_ns() + pg->answer_ns;
    for (unsigned tries = 0; tries < THROTTLE_TRIES; tries++) {
        if (uptime_ns() >= *until)
            return ERR_TIMED_OUT;
        if (pager_cache_ndirty(&pg->cache) < pager_dirty_limit(pg))
            return OK;   /* under it now: the store looks again */
        status_t st = oldest_req(pg, pv, out);
        if (st != OK)
            return st == ERR_ALREADY_EXISTS ? OK : st;   /* (joined) */
        struct pager_req *r;
        st = req_new(pg, 0, job_current(), PAGER_REQ_WRITEBACK, &r);
        if (st != OK)
            return st;
        st = queue_oldest(pg, r, true, true);
        if (st == OK) {
            *out = r;
            return OK;
        }
        pager_req_free(r);   /* (written back meanwhile, or another store made one) */
    }
    return OK;
}

void pager_writeback_kick(struct pager_vmo *pv)
{
    kick(pv->pg);
}

uint64_t pager_vmo_key(const struct pager_vmo *pv)
{
    return pv->key;
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
    /* A write-back request's pages past the end are done: dropped with
     * the file's end (their stores go with it, as Linux's truncate). */
    for (struct list_node *n = pv->wbs.next; n != &pv->wbs;) {
        struct pager_req *r = container_of(n, struct pager_req, vnode);
        struct pager_wb *w = wb_of(r);
        n = n->next;   /* finish_locked unlinks r */
        if (r->end <= end)
            continue;
        for (uint64_t i = r->first > end ? 0 : end - r->first; i < r->end - r->first; i++) {
            bit_clear(w->need, i);
            bit_clear(w->begin, i);
        }
        if (r->first < end)
            r->end = end;
        if (bits_none(w->need))
            finish_locked(r, w->err, dead);
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
    list_init(&pv->wbs);
    req_init(&pv->gone, PAGER_REQ_GONE, key);
    *out = pv;
    return OK;
}

status_t pager_create_vmo(struct pager *pg, uint64_t key, uint64_t size, struct vmo **out)
{
    return pager_create_vmo_flags(pg, key, size, 0, out);
}

status_t pager_create_vmo_flags(struct pager *pg, uint64_t key, uint64_t size, uint32_t flags,
                                struct vmo **out)
{
    if (flags & PAGER_VMO_FIND) {
        if (flags != PAGER_VMO_FIND || size)
            return ERR_INVALID_ARGS;
        return (*out = lookup_vmo(pg, key)) ? OK : ERR_NOT_FOUND;
    }
    if (flags & ~PAGER_VMO_WRITABLE)
        return ERR_INVALID_ARGS;
    bool writable = flags & PAGER_VMO_WRITABLE;
    if (writable && pg->cache.max_pages < PAGER_WRITABLE_MIN_PAGES)
        return ERR_INVALID_ARGS;   /* (its dirty limit would be under 2 pages) */
    if (size > VMO_SMALL_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    struct pager_vmo *pv;
    status_t st = pv_new(pg, key, &pv);
    if (st != OK)
        return st;
    struct vmo *v;
    if ((st = vmo_create_pager(pg->job, size, pv, writable, &v)) != OK) {
        pv_free(pv);
        return st;
    }
    struct list_node dead;
    list_init(&dead);
    uint64_t f = plock(pg);
    struct pager_vmo *old = pg->closed ? NULL : find_locked(pg, key);
    /* A LINKED link whose VMO has no reference left is dying: its
     * destruction frees every page and only then says so (pager_vmo_gone),
     * and a map meanwhile must not fail (one key per file: a map can race
     * another process's last unmap). Taken over like a GONE one: cut off
     * here, freed by that destruction (not linked any more). */
    struct vmo *alive = NULL;
    bool dying = false;
    if (old && old->state == PV_LINKED) {
        if (kobject_tryref(vmo_kobject(old->v)))
            alive = old->v;
        else
            dying = true;
    }
    st = pg->closed ? ERR_BAD_STATE : alive ? ERR_ALREADY_EXISTS : OK;
    if (st == OK && old) {   /* GONE, not answered yet, or dying: taken over, so a map never waits */
        list_del(&old->knode);
        if (dying) {
            old->state = PV_DETACHED;
            fail_all_locked(old, ERR_PEER_CLOSED, &dead);   /* (none has a waiter: each holds the VMO) */
        } else {
            unqueue_locked(&old->gone);
        }
        sync_readable_locked(pg);
    }
    if (st == OK) {
        pv->v = v;
        pv->state = PV_LINKED;
        list_add_tail(bucket(pg, key), &pv->knode);
    }
    punlock(pg, f);
    reap(&dead);
    if (alive)
        kobject_unref(vmo_kobject(alive));   /* (may be its last: no lock held) */
    if (st != OK) {
        kobject_unref(vmo_kobject(v));   /* a NEW link: its destruction frees pv */
        return st;
    }
    if (old && !dying)
        pv_free(old);
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

/* No lock held: every page of pg's VMOs being written back dirty again
 * (a successor starts: the write-backs its predecessor began won't end).
 * The first writing page at a time, each under its VMO's lock; as many as
 * were writing when it started, so pages a live writer begins meanwhile
 * (at the list's tail) can't keep it going. */
static void again_writing(struct pager *pg)
{
    for (uint64_t n = pager_cache_nwriting(&pg->cache); n; n--) {
        struct vmo *v;
        uint64_t idx, dirtied;
        if (!pager_cache_first(&pg->cache, true, &v, &idx, &dirtied))
            break;
        vmo_pager_unbegin(v, idx, idx + 1);
        kobject_unref(vmo_kobject(v));   /* (the last if written meanwhile: no lock held) */
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
    if (flags & PAGER_NEXT_AGAIN)
        again_writing(pg);
    uint64_t f = plock(pg);
    if (flags & PAGER_NEXT_AGAIN)
        again_locked(pg);
    while (st == ERR_SHOULD_WAIT && !list_empty(&pg->queue)) {
        struct pager_req *r = list_first(&pg->queue, struct pager_req, qnode);
        if (r->kind != PAGER_REQ_GONE && uptime_ns() >= r->deadline) {
            finish_locked(r, ERR_TIMED_OUT, &dead);
            continue;
        }
        list_del(&r->qnode);
        list_add_tail(&pg->delivered, &r->qnode);
        r->delivered = true;
        out->key = r->key;
        out->kind = r->kind;
        if (r->kind != PAGER_REQ_GONE) {
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

uint64_t pager_dirty_pages(struct pager *pg)
{
    return pager_cache_ndirty(&pg->cache);
}

uint64_t pager_writing_pages(struct pager *pg)
{
    return pager_cache_nwriting(&pg->cache);
}

/* ---- writable VMOs' pages: copied out and in --------------------------------- */

/* How many times OLDEST looks for the oldest dirty page again when the one
 * it found moved on before it locked its VMO (begun, written, dropped). */
#define OLDEST_TRIES 8

/* pager_dirty's OLDEST: the run around the oldest dirty page, into *v (a
 * reference), *at and *n; none (n = 0) if that page is younger than
 * min_age, *age then its age. ERR_NOT_FOUND if no page is dirty. */
static status_t oldest_run(struct pager *pg, uint32_t flags, uint64_t max, uint64_t min_age,
                           struct page **pages, struct vmo **v, uint64_t *at, uint64_t *n,
                           uint64_t *dirtied, uint64_t *age)
{
    for (unsigned tries = 0; tries < OLDEST_TRIES; tries++) {
        uint64_t idx, d;
        *v = NULL;
        if (!pager_cache_first(&pg->cache, false, v, &idx, &d))
            return ERR_NOT_FOUND;
        uint64_t now = uptime_ns();
        *age = now > d ? now - d : 0;
        if (*age < min_age)
            return OK;   /* (n is 0) */
        vmo_pager_dirty_run(*v, 0, UINT64_MAX, idx, flags, max, pages, at, n, dirtied);
        if (*n)
            return OK;
        kobject_unref(vmo_kobject(*v));
    }
    *v = NULL;
    return OK;   /* moving faster than we look: none this time */
}

status_t pager_dirty(struct pager *pg, uint64_t key, uint32_t flags, struct pager_run *run,
                     const struct pager_dst *dst)
{
    const uint32_t all = PAGER_DIRTY_BEGIN | PAGER_DIRTY_PEEK | PAGER_DIRTY_OLDEST;
    if ((flags & ~all) || ((flags & PAGER_DIRTY_BEGIN) && (flags & PAGER_DIRTY_PEEK)))
        return ERR_INVALID_ARGS;
    bool oldest = flags & PAGER_DIRTY_OLDEST;
    uint64_t offset = oldest ? 0 : run->offset, len = run->len;
    if (((offset | len) & (PAGE_SIZE - 1)) || len == 0 || len > UINT64_MAX - offset)
        return ERR_INVALID_ARGS;
    if (len > PAGER_SUPPLY_MAX)
        return ERR_OUT_OF_RANGE;
    struct page **pages = kmalloc(RUN_PAGES * sizeof(*pages));
    if (!pages)
        return ERR_NO_MEMORY;
    uint32_t look = flags & (PAGER_DIRTY_BEGIN | PAGER_DIRTY_PEEK);
    uint64_t first = offset >> PAGE_SHIFT, at = first, n = 0, dirtied = 0, age = 0;
    struct vmo *v = NULL;
    status_t st;
    if (oldest) {
        st = oldest_run(pg, look, len >> PAGE_SHIFT, run->age_ns, pages, &v, &at, &n, &dirtied,
                        &age);
        first = at;   /* buf stands for the run itself */
    } else {
        v = lookup_vmo(pg, key);
        st = !v ? ERR_NOT_FOUND : !vmo_pager_writable(v) ? ERR_INVALID_ARGS : OK;
        if (st == OK)
            vmo_pager_dirty_run(v, first, first + (len >> PAGE_SHIFT), UINT64_MAX, look,
                                RUN_PAGES, pages, &at, &n, &dirtied);
    }
    /* The bytes, from the page references, with no lock held: a store
     * meanwhile may show in them (in a dirty page all the same; after a
     * BEGIN's shootdown, one that made its page dirty again). */
    for (uint64_t i = 0; i < n; i++) {
        uint64_t to = dst->addr + ((at - first + i) << PAGE_SHIFT);
        const void *from = page_to_virt(pages[i]);
        if (st == OK && dst->user)
            st = copy_to_user(to, from, PAGE_SIZE) == OK ? OK : ERR_INVALID_ARGS;
        else if (st == OK)
            memcpy((void *)(uintptr_t)to, from, PAGE_SIZE);
        page_unref(pages[i]);
    }
    kfree(pages);
    if (st != OK && n && (flags & PAGER_DIRTY_BEGIN))
        vmo_pager_unbegin(v, at, at + n);   /* never told where: no write-back began */
    if (st == OK) {
        run->key = v ? vmo_pager_key(v) : key;
        run->offset = at << PAGE_SHIFT;
        run->len = n << PAGE_SHIFT;
        run->age_ns = n ? uptime_ns() - dirtied : age;
    }
    if (v)
        kobject_unref(vmo_kobject(v));
    return st;
}

status_t pager_write(struct pager *pg, uint64_t key, uint64_t offset, const struct pager_src *src,
                     uint64_t len)
{
    if (len > PAGER_SUPPLY_MAX || offset > VMO_SMALL_MAX_SIZE)
        return ERR_OUT_OF_RANGE;   /* (so offset + len can't overflow below) */
    struct vmo *v = lookup_vmo(pg, key);
    if (!v)
        return ERR_NOT_FOUND;
    uint64_t first = offset >> PAGE_SHIFT, end = ALIGN_UP(offset + len, PAGE_SIZE) >> PAGE_SHIFT;
    struct page **pages = kmalloc((RUN_PAGES + 1) * sizeof(*pages));   /* (a range may straddle) */
    status_t st = !pages ? ERR_NO_MEMORY : len ? vmo_pager_present(v, first, end, offset + len, pages)
                                               : OK;
    kobject_unref(vmo_kobject(v));
    if (st != OK || !len) {
        kfree(pages);
        return st;
    }
    /* Into the pages present when we looked, with no lock held: one evicted
     * or dropped meanwhile takes our bytes with it (the disk has them). */
    for (uint64_t i = first; i < end; i++) {
        struct page *p = pages[i - first];
        if (!p)
            continue;
        uint64_t lo = i << PAGE_SHIFT > offset ? i << PAGE_SHIFT : offset;
        uint64_t hi = (i + 1) << PAGE_SHIFT < offset + len ? (i + 1) << PAGE_SHIFT : offset + len;
        char *to = (char *)page_to_virt(p) + (lo & (PAGE_SIZE - 1));
        if (st == OK && src->user)
            st = copy_from_user(to, src->addr + (lo - offset), hi - lo) == OK ? OK
                                                                              : ERR_INVALID_ARGS;
        else if (st == OK)
            memcpy(to, (const char *)(uintptr_t)src->addr + (lo - offset), hi - lo);
        page_unref(p);
    }
    kfree(pages);
    return st;
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

/* PAGER_OP_WRITTEN: the writing pages of key's VMO in [first, end) end
 * their write-back with st. Then, if it still has too many dirty pages,
 * the background request again (the one it may just have answered). */
static status_t written(struct pager *pg, uint64_t key, uint64_t first, uint64_t end,
                        status_t st)
{
    struct vmo *v = lookup_vmo(pg, key);
    if (!v)
        return ERR_NOT_FOUND;
    if (!vmo_pager_writable(v)) {
        kobject_unref(vmo_kobject(v));
        return ERR_INVALID_ARGS;
    }
    struct list_node dead;
    list_init(&dead);
    vmo_pager_written(v, first, end, st, &dead);
    kobject_unref(vmo_kobject(v));
    reap(&dead);
    kick(pg);
    return OK;
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
        if (offset > VMO_SMALL_MAX_SIZE)
            return ERR_OUT_OF_RANGE;
        return set_size(pg, key, ALIGN_UP(offset, PAGE_SIZE));
    case PAGER_OP_WRITTEN:
        if (arg != OK && arg != ERR_IO && arg != ERR_NO_SPACE)
            return ERR_INVALID_ARGS;
        if (((offset | len) & (PAGE_SIZE - 1)) || len == 0 || len > UINT64_MAX - offset)
            return ERR_INVALID_ARGS;
        return written(pg, key, offset >> PAGE_SHIFT, (offset + len) >> PAGE_SHIFT, (status_t)arg);
    default:
        return ERR_INVALID_ARGS;
    }
}
