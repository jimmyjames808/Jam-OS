/* Tests for pagers (<jam/pager.h>): the pager object, its VMOs and keys,
 * the request queue (sizes, coalescing, delivered and again, deadlines),
 * supplying, failing, detaching, PAGER_REQ_GONE, the pager's close, and
 * everything a pager VMO refuses.
 *
 * The pager is the test itself or a kernel thread (struct kpager) serving
 * requests in a loop, as fat will from user space; the readers are kernel
 * threads in vmo_read, which waits for its page like a fault will. Each
 * test ends by closing the pager's handle (pg_close), which is also what
 * frees a pager whose VMOs died: a GONE link holds the pager until it is
 * answered or the pager closes. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/handle.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/vmo.h>

#define PG     PAGE_SIZE
#define ANSWER (600 * NS_PER_S)   /* long: only the deadline tests wait it out */

/* ---- helpers ------------------------------------------------------------- */

static struct pager *pg_new(struct job *j, uint64_t answer_ns)
{
    struct pager *pg;
    KT_EQ(pager_create(j, 1u << 20, answer_ns, &pg), OK);
    return pg;
}

/* Close the pager's (only) handle: every VMO is detached, every GONE
 * answered, and the reference goes with it. */
static void pg_close(struct pager *pg)
{
    struct handle_table t;
    handle_table_init(&t);
    struct khandle kh = khandle_from_new(pager_kobject(pg), PAGER_RIGHTS);
    handle_t h;
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    handle_table_destroy(&t);
}

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

/* The word every 8 bytes of page `page` of key's file hold. */
static uint64_t word(uint64_t key, uint64_t page)
{
    return key << 32 | page | 0x5a00000000000000ull;
}

/* Supply pages [first, first + n) of key with their words. */
static status_t supply(struct pager *pg, uint64_t key, uint64_t first, uint64_t n)
{
    uint64_t *buf = kmalloc(n * PG);
    KT_ASSERT(buf);
    for (uint64_t p = 0; p < n; p++)
        for (uint64_t i = 0; i < PG / 8; i++)
            buf[p * PG / 8 + i] = word(key, first + p);
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    status_t st = pager_supply(pg, key, first * PG, &src, n * PG);
    kfree(buf);
    return st;
}

/* The next request, waiting for one (a broken kernel fails, not hangs). */
static void next(struct pager *pg, uint32_t flags, struct pager_request *r)
{
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    for (;;) {
        status_t st = pager_next(pg, flags, r);
        if (st == OK)
            return;
        KT_EQ(st, ERR_SHOULD_WAIT);
        flags = 0;
        KT_EQ(object_wait_one(pager_kobject(pg), SIG_READABLE, deadline, NULL), OK);
    }
}

static void none_queued(struct pager *pg)
{
    struct pager_request r;
    KT_EQ(pager_next(pg, 0, &r), ERR_SHOULD_WAIT);
    KT_EQ(kobject_signals(pager_kobject(pg)) & SIG_READABLE, 0);
}

static void is_read(const struct pager_request *r, uint64_t key, uint64_t first, uint64_t pages)
{
    KT_EQ(r->kind, PAGER_REQ_READ);
    KT_EQ(r->key, key);
    KT_EQ(r->offset, first * PG);
    KT_EQ(r->len, pages * PG);
    KT_EQ(r->reserved, 0);
}

/* A reader: a kernel thread in vmo_read of [off, off + len). */
struct rd {
    struct vmo    *v;
    uint64_t       off, len;
    uint64_t      *buf;
    status_t       st;        /* ERR_INTERNAL until it returns */
    struct thread *t;
};

static void reader_main(void *arg)
{
    struct rd *r = arg;
    status_t st = vmo_read(r->v, r->off, r->buf, r->len);
    __atomic_store_n(&r->st, st, __ATOMIC_RELEASE);
}

static void rd_start(struct rd *r, struct vmo *v, uint64_t off, uint64_t len)
{
    r->v = v;
    r->off = off;
    r->len = len;
    r->buf = kmalloc(len);
    KT_ASSERT(r->buf);
    r->st = ERR_INTERNAL;
    r->t = thread_create("pager-reader", reader_main, r, PRIO_DEFAULT);
}

/* Join the reader: its status. With `key`, its bytes must be the words. */
static status_t rd_end(struct rd *r, uint64_t key)
{
    thread_join(r->t);
    status_t st = r->st;
    if (st == OK && key)
        for (uint64_t i = 0; i < r->len / 8; i++)
            KT_EQ(r->buf[i], word(key, (r->off + i * 8) / PG));
    kfree(r->buf);
    return st;
}

static bool rd_waiting(struct rd *r)
{
    return __atomic_load_n(&r->st, __ATOMIC_ACQUIRE) == ERR_INTERNAL;
}

/* ---- the basics ------------------------------------------------------------ */

/* A reader of an absent page waits; the pager sees one request for it and
 * the absent pages after it; its supply wakes the reader with the bytes.
 * The pages are charged to the pager's job, and everything goes back. */
KTEST(pager_supply_basic)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, ANSWER);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 1);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 7, 20 * PG, &v), OK);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 3);   /* the pager, the VMO, its link */
    KT_ASSERT(vmo_is_pager(v));
    KT_EQ(vmo_pager_koid(v), pager_kobject(pg)->koid);

    struct rd r;
    rd_start(&r, v, 3 * PG + 104, 2 * PG);
    struct pager_request q;
    next(pg, 0, &q);
    is_read(&q, 7, 3, 16);   /* page 3, then 15 more absent ones */
    thread_sleep_ms(5);
    KT_ASSERT(rd_waiting(&r));
    KT_EQ(supply(pg, 7, 3, 16), OK);
    KT_EQ(rd_end(&r, 7), OK);
    KT_EQ(vmo_committed(v), 16 * PG);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 16 + 2);   /* the pages, a mid and a leaf table */
    none_queued(pg);

    /* Present pages are read straight away; supplying them again is a
     * no-op (the first copy stays). */
    uint64_t w;
    uint64_t pa = vmo_page_phys(v, 5 * PG);
    KT_EQ(supply(pg, 7, 4, 2), OK);
    KT_EQ(vmo_page_phys(v, 5 * PG), pa);
    KT_EQ(vmo_read(v, 18 * PG + 8, &w, 8), OK);
    KT_EQ(w, word(7, 18));
    KT_EQ(vmo_read(v, 20 * PG, &w, 8), ERR_OUT_OF_RANGE);
    none_queued(pg);

    put(v);
    struct pager_request g;
    next(pg, 0, &g);   /* the VMO is gone: the pager is told */
    KT_EQ(g.kind, PAGER_REQ_GONE);
    KT_EQ(g.key, 7);
    KT_EQ(g.len, 0);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* A request covers its page and the absent ones after it: at most
 * PAGER_REQ_PAGES, never past a present page, the VMO's end or a page
 * another request covers. */
KTEST(pager_request_sizes)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 1, 64 * PG, &v), OK);
    KT_EQ(supply(pg, 1, 30, 1), OK);   /* unasked: allowed */
    struct pager_request q;
    struct rd a, b, c, d;

    rd_start(&a, v, 8 * PG, 8);
    next(pg, 0, &q);
    is_read(&q, 1, 8, 16);             /* [8, 24) */
    rd_start(&b, v, 2 * PG, 8);
    next(pg, 0, &q);
    is_read(&q, 1, 2, 6);              /* cut at 8, a's */
    rd_start(&c, v, 25 * PG, 8);
    next(pg, 0, &q);
    is_read(&q, 1, 25, 5);             /* cut at 30, present */
    rd_start(&d, v, 60 * PG, 8);
    next(pg, 0, &q);
    is_read(&q, 1, 60, 4);             /* cut at the end */

    KT_EQ(supply(pg, 1, 0, 30), OK);
    KT_EQ(supply(pg, 1, 31, 33), OK);
    KT_EQ(rd_end(&a, 1), OK);
    KT_EQ(rd_end(&b, 1), OK);
    KT_EQ(rd_end(&c, 1), OK);
    KT_EQ(rd_end(&d, 1), OK);
    none_queued(pg);
    put(v);
    pg_close(pg);
}

/* Many readers of one page make one request, and one supply answers
 * them all. */
KTEST(pager_coalescing)
{
    enum { N = 8 };
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 2, 4 * PG, &v), OK);
    static struct rd r[N];
    for (unsigned i = 0; i < N; i++)
        rd_start(&r[i], v, PG + 8 * i, 8);
    struct pager_request q;
    next(pg, 0, &q);
    is_read(&q, 2, 1, 3);
    thread_sleep_ms(20);   /* let them all join (a late one finds the page anyway) */
    none_queued(pg);
    KT_EQ(supply(pg, 2, 1, 3), OK);
    for (unsigned i = 0; i < N; i++)
        KT_EQ(rd_end(&r[i], 2), OK);
    none_queued(pg);
    put(v);
    pg_close(pg);
}

/* A delivered request stays until answered: a pager that "died" after
 * pager_next (never answered) leaves it to its successor, whose
 * PAGER_NEXT_AGAIN gets it again; a second answer is harmless. */
KTEST(pager_again_after_death)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 3, 2 * PG, &v), OK);
    struct rd a, b;
    rd_start(&a, v, 0, 8);
    struct pager_request q, q2;
    next(pg, 0, &q);
    is_read(&q, 3, 0, 2);
    none_queued(pg);               /* delivered: not given again by itself */
    rd_start(&b, v, PG, 8);        /* joins the delivered request */
    thread_sleep_ms(5);
    none_queued(pg);
    KT_ASSERT(rd_waiting(&a) && rd_waiting(&b));

    next(pg, PAGER_NEXT_AGAIN, &q2);   /* the successor */
    is_read(&q2, 3, 0, 2);
    none_queued(pg);
    KT_EQ(supply(pg, 3, 0, 2), OK);
    KT_EQ(supply(pg, 3, 0, 2), OK);    /* the dead one's answer, late */
    KT_EQ(rd_end(&a, 3), OK);
    KT_EQ(rd_end(&b, 3), OK);
    KT_EQ(pager_next(pg, PAGER_NEXT_AGAIN, &q), ERR_SHOULD_WAIT);   /* nothing left */
    put(v);
    pg_close(pg);
}

/* Nobody waits past the answer time: the reader's wait ends ERR_IO (the
 * request failed ERR_TIMED_OUT for everyone); a request nobody waits on
 * any more is failed by pager_next when it meets it. */
KTEST(pager_deadline)
{
    struct pager *pg = pg_new(NULL, 30 * NS_PER_MS);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 4, 2 * PG, &v), OK);
    struct rd a;
    uint64_t t0 = uptime_ns();
    rd_start(&a, v, 0, 8);
    KT_EQ(rd_end(&a, 0), ERR_IO);
    KT_ASSERT(uptime_ns() - t0 >= 30 * NS_PER_MS);
    none_queued(pg);   /* the waiter failed it: gone from the queue */

    struct pager_req *r;
    KT_EQ(vmo_page_request(v, 1, &r), OK);
    KT_ASSERT(r);
    pager_req_put(r);   /* no waiter left: the queue still holds it */
    KT_ASSERT(kobject_signals(pager_kobject(pg)) & SIG_READABLE);
    thread_sleep_ms(40);
    none_queued(pg);   /* met past its deadline: failed, not delivered */
    put(v);
    pg_close(pg);
}

/* PAGER_OP_FAIL fails the requests it overlaps (ERR_IO for the reader),
 * leaves the others, and takes only ERR_IO or ERR_OUT_OF_RANGE. */
KTEST(pager_fail)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 5, 40 * PG, &v), OK);
    struct rd a, b;
    struct pager_request q;
    rd_start(&a, v, 0, 8);
    next(pg, 0, &q);
    is_read(&q, 5, 0, 16);
    rd_start(&b, v, 20 * PG, 8);
    next(pg, 0, &q);
    is_read(&q, 5, 20, 16);

    KT_EQ(pager_op(pg, 5, PAGER_OP_FAIL, 0, PG, ERR_BAD_STATE), ERR_INVALID_ARGS);
    KT_EQ(pager_op(pg, 5, PAGER_OP_FAIL, 0, 0, ERR_IO), ERR_INVALID_ARGS);
    KT_EQ(pager_op(pg, 5, PAGER_OP_FAIL, 1, PG, ERR_IO), ERR_INVALID_ARGS);
    KT_EQ(pager_op(pg, 6, PAGER_OP_FAIL, 0, PG, ERR_IO), ERR_NOT_FOUND);
    KT_EQ(pager_op(pg, 5, PAGER_OP_FAIL, 16 * PG, 4 * PG, ERR_IO), OK);   /* between them */
    thread_sleep_ms(5);
    KT_ASSERT(rd_waiting(&a) && rd_waiting(&b));
    KT_EQ(pager_op(pg, 5, PAGER_OP_FAIL, 15 * PG, PG, ERR_OUT_OF_RANGE), OK);
    KT_EQ(rd_end(&a, 0), ERR_IO);
    KT_ASSERT(rd_waiting(&b));
    KT_EQ(pager_op(pg, 5, PAGER_OP_FAIL, 0, 40 * PG, ERR_IO), OK);
    KT_EQ(rd_end(&b, 0), ERR_IO);

    /* A failed page is asked for again by the next reader. */
    rd_start(&a, v, 0, 8);
    next(pg, 0, &q);
    is_read(&q, 5, 0, 16);
    KT_EQ(supply(pg, 5, 0, 16), OK);
    KT_EQ(rd_end(&a, 5), OK);
    put(v);
    pg_close(pg);
}

/* Detach: waiting and later readers fail ERR_IO, the present pages go
 * (and their charges), supplies find no key, and the key is free. */
KTEST(pager_detach)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 8, 32 * PG, &v), OK);
    KT_EQ(supply(pg, 8, 0, 4), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 4 + 2);
    struct rd a;
    struct pager_request q;
    rd_start(&a, v, 10 * PG, 8);
    next(pg, 0, &q);
    is_read(&q, 8, 10, 16);

    KT_EQ(pager_op(pg, 8, PAGER_OP_DETACH, PG, 0, 0), ERR_INVALID_ARGS);
    KT_EQ(pager_op(pg, 8, PAGER_OP_DETACH, 0, 0, ERR_IO), ERR_INVALID_ARGS);
    KT_EQ(pager_op(pg, 8, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(rd_end(&a, 0), ERR_IO);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 2);   /* tables stay until the VMO goes */
    uint64_t w;
    KT_EQ(vmo_read(v, 0, &w, 8), ERR_IO);
    KT_EQ(supply(pg, 8, 0, 1), ERR_NOT_FOUND);
    KT_EQ(pager_op(pg, 8, PAGER_OP_DETACH, 0, 0, 0), ERR_NOT_FOUND);
    none_queued(pg);

    struct vmo *v2;
    KT_EQ(pager_create_vmo(pg, 8, PG, &v2), OK);   /* the key is free */
    put(v);   /* detached: no PAGER_REQ_GONE */
    none_queued(pg);
    put(v2);
    next(pg, 0, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* A VMO's end: PAGER_REQ_GONE, delivered and given again like any request,
 * keeps its key taken until PAGER_OP_DETACH answers it. */
KTEST(pager_gone)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v, *v2;
    KT_EQ(pager_create_vmo(pg, 9, PG, &v), OK);
    KT_EQ(pager_create_vmo(pg, 9, PG, &v2), ERR_ALREADY_EXISTS);
    KT_EQ(supply(pg, 9, 0, 1), OK);
    put(v);
    struct pager_request q;
    next(pg, 0, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);
    KT_EQ(q.key, 9);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, 0);
    none_queued(pg);
    KT_EQ(supply(pg, 9, 0, 1), ERR_NOT_FOUND);
    KT_EQ(pager_op(pg, 9, PAGER_OP_FAIL, 0, PG, ERR_IO), ERR_NOT_FOUND);
    KT_EQ(pager_create_vmo(pg, 9, PG, &v2), ERR_ALREADY_EXISTS);
    next(pg, PAGER_NEXT_AGAIN, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);
    KT_EQ(pager_op(pg, 9, PAGER_OP_DETACH, 0, 0, 0), OK);   /* the answer */
    none_queued(pg);
    KT_EQ(pager_next(pg, PAGER_NEXT_AGAIN, &q), ERR_SHOULD_WAIT);
    KT_EQ(pager_op(pg, 9, PAGER_OP_DETACH, 0, 0, 0), ERR_NOT_FOUND);
    KT_EQ(pager_create_vmo(pg, 9, PG, &v2), OK);
    put(v2);
    pg_close(pg);   /* answers the new GONE */
}

/* The pager's last handle: every VMO detached (waiting readers fail, the
 * pages go), unanswered GONEs dropped, no new VMO; the pager itself goes
 * once its VMOs have. */
KTEST(pager_close_detaches)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, ANSWER);
    struct vmo *v, *gone;
    KT_EQ(pager_create_vmo(pg, 10, 8 * PG, &v), OK);
    KT_EQ(pager_create_vmo(pg, 11, 8 * PG, &gone), OK);
    put(gone);
    KT_EQ(supply(pg, 10, 0, 2), OK);
    struct rd a;
    struct pager_request q;
    rd_start(&a, v, 4 * PG, 8);
    next(pg, 0, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);   /* first in the queue; left unanswered */
    KT_EQ(q.key, 11);
    next(pg, 0, &q);
    is_read(&q, 10, 4, 4);

    struct aspace *as;   /* a page mapped: the close zaps it from the teardown */
    KT_EQ(aspace_create(&as), OK);
    uint64_t at = 0;
    KT_EQ(aspace_map(as, v, 0, 8 * PG, ASPACE_READ, &at), OK);
    KT_EQ(aspace_fault(as, at + PG, ASPACE_READ), OK);
    KT_ASSERT(aspace_pte(as, at + PG) != 0);

    kobject_ref(pager_kobject(pg));   /* to try a VMO after the close */
    pg_close(pg);
    KT_EQ(rd_end(&a, 0), ERR_IO);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(aspace_pte(as, at + PG), 0);
    KT_EQ(aspace_fault(as, at + PG, ASPACE_READ), ERR_SHOULD_WAIT);
    KT_EQ(aspace_unmap(as, at, 8 * PG), OK);
    aspace_unref(as);
    struct vmo *v3;
    KT_EQ(pager_create_vmo(pg, 12, PG, &v3), ERR_BAD_STATE);
    KT_EQ(pager_next(pg, 0, &q), ERR_SHOULD_WAIT);
    kobject_unref(pager_kobject(pg));
    put(v);
    kt_job_is_empty(j);
    job_unref(j);
}

/* A reader killed while it waits returns ERR_CANCELED at once; its request
 * stays for the pager (and is answered as usual). */
KTEST(pager_cancel)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 13, PG, &v), OK);
    struct rd a;
    rd_start(&a, v, 0, 8);
    struct pager_request q;
    next(pg, 0, &q);
    thread_sleep_ms(5);
    thread_cancel(a.t);
    KT_EQ(rd_end(&a, 0), ERR_CANCELED);
    KT_EQ(supply(pg, 13, 0, 1), OK);
    uint64_t w;
    KT_EQ(vmo_read(v, 0, &w, 8), OK);
    KT_EQ(w, word(13, 0));
    put(v);
    pg_close(pg);
}

/* ---- refusals -------------------------------------------------------------- */

/* Nothing but the pager puts a page in or takes one out, and nothing that
 * can't wait for it (a device, the kernel, a kept mapping) may hold it. */
KTEST(pager_refusals)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 14, 4 * PG, &v), OK);
    KT_EQ(supply(pg, 14, 0, 1), OK);
    uint64_t w = 1;
    KT_EQ(vmo_write(v, 0, &w, 8), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_write(v, PG, &w, 8), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_commit(v, PG, PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_decommit(v, 0, PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_set_size(v, 8 * PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_set_size(v, 0), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_seal(v), ERR_BAD_STATE);
    void *va;
    KT_EQ(vmo_map_kernel(v, 0, PG, 0, &va), ERR_NOT_SUPPORTED);
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    uint64_t addr, pin;
    KT_EQ(vmo_pin_flags(v, cap, 0, PG, VMO_PIN_READ_ONLY, &addr, 1, &pin), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_pin(v, cap, 0, PG, &addr, 1, &pin), ERR_NOT_SUPPORTED);
    kobject_unref(cap);
    KT_EQ(vmo_committed(v), PG);
    KT_EQ(vmo_read(v, 0, &w, 8), OK);
    KT_EQ(w, word(14, 0));

    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_READ | ASPACE_KEPT_ONLY, &a), ERR_NOT_SUPPORTED);
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_READ | ASPACE_WRITE, &a), ERR_ACCESS_DENIED);
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_READ | ASPACE_CAN_WRITE, &a), ERR_ACCESS_DENIED);
    KT_EQ(aspace_mapping_count(as), 0);
    aspace_unref(as);
    put(v);
    pg_close(pg);
}

/* A fault on an absent page of a pager VMO installs nothing and says
 * ERR_SHOULD_WAIT (the fault path's wait is not built yet); a present
 * page maps as usual, read-only, and a detach takes it out of the
 * mapping. */
KTEST(pager_fault_should_wait)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 15, 2 * PG, &v), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, v, 0, 2 * PG, ASPACE_READ, &a), OK);
    KT_EQ(aspace_fault(as, a, ASPACE_READ), ERR_SHOULD_WAIT);
    KT_EQ(aspace_pte(as, a), 0);
    none_queued(pg);   /* a fault doesn't ask yet: the waiting fault path will */
    KT_EQ(supply(pg, 15, 0, 1), OK);
    KT_EQ(aspace_fault(as, a, ASPACE_READ), OK);
    KT_EQ(aspace_pte(as, a) & 0x000ffffffffff000ull, vmo_page_phys(v, 0));
    KT_EQ(aspace_fault(as, a, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, a + PG, ASPACE_READ), ERR_SHOULD_WAIT);
    KT_EQ(pager_op(pg, 15, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(aspace_pte(as, a), 0);
    KT_EQ(aspace_fault(as, a, ASPACE_READ), ERR_SHOULD_WAIT);
    KT_EQ(aspace_unmap(as, a, 2 * PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);
}

/* The handle layer: the power, the rights each call needs, the handles'
 * rights and types, handle_info, and every argument check. */
KTEST(pager_handles)
{
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *root = resource_root();
    kobject_ref(root);
    struct khandle kh = khandle_from_new(root, RIGHTS_BASIC | RIGHT_ROOT_PAGER);
    handle_t with, without, pgh, ro, vh, ev;
    KT_EQ(handle_insert(&t, &kh, &with), OK);
    kh = khandle_from_new(root, ROOT_RIGHTS & ~RIGHT_ROOT_PAGER);
    KT_EQ(handle_insert(&t, &kh, &without), OK);
    KT_EQ(sys_event_create(&t, &ev), OK);

    KT_EQ(sys_pager_create(&t, without, 16, ANSWER, 0, &pgh), ERR_ACCESS_DENIED);
    KT_EQ(sys_pager_create(&t, ev, 16, ANSWER, 0, &pgh), ERR_WRONG_TYPE);
    KT_EQ(sys_pager_create(&t, with, 16, ANSWER, 1, &pgh), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_create(&t, with, 0, ANSWER, 0, &pgh), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_create(&t, with, 16, PAGER_ANSWER_MIN_NS - 1, 0, &pgh), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pager_create(&t, with, 16, PAGER_ANSWER_MAX_NS + 1, 0, &pgh), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pager_create(&t, with, 16, ANSWER, 0, &pgh), OK);
    struct handle_info hi;
    KT_EQ(handle_info(&t, pgh, &hi), OK);
    KT_EQ(hi.type, OBJ_PAGER);
    KT_EQ(hi.rights, PAGER_RIGHTS);
    uint64_t pager_koid = hi.koid;

    KT_EQ(sys_pager_create_vmo(&t, pgh, 1, PG, 1, &vh), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_create_vmo(&t, pgh, 1, VMO_MAX_SIZE + 1, 0, &vh), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pager_create_vmo(&t, ev, 1, PG, 0, &vh), ERR_WRONG_TYPE);
    KT_EQ(handle_duplicate(&t, pgh, RIGHTS_BASIC | RIGHT_READ, &ro), OK);
    KT_EQ(sys_pager_create_vmo(&t, ro, 1, PG, 0, &vh), ERR_ACCESS_DENIED);
    KT_EQ(sys_pager_create_vmo(&t, pgh, 1, 3 * PG - 5, 0, &vh), OK);
    KT_EQ(handle_info(&t, vh, &hi), OK);
    KT_EQ(hi.type, OBJ_VMO);
    KT_EQ(hi.rights, PAGER_VMO_RIGHTS);
    KT_EQ(hi.related_koid, pager_koid);
    uint64_t size;
    KT_EQ(sys_vmo_get_size(&t, vh, &size), OK);
    KT_EQ(size, 3 * PG);
    uint64_t w = 0;
    KT_EQ(sys_vmo_write(&t, vh, 0, &w, 8), ERR_ACCESS_DENIED);

    struct pager_request q;
    KT_EQ(sys_pager_next(&t, pgh, 2, &q), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_next(&t, ev, 0, &q), ERR_WRONG_TYPE);
    KT_EQ(sys_pager_next(&t, ro, 0, &q), ERR_SHOULD_WAIT);   /* READ is enough */
    static uint8_t buf[2 * PAGE_SIZE] __attribute__((aligned(4096)));
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    KT_EQ(sys_pager_supply(&t, ro, 1, 0, &src, PG), ERR_ACCESS_DENIED);
    KT_EQ(sys_pager_supply(&t, pgh, 1, 1, &src, PG), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_supply(&t, pgh, 1, 0, &src, 100), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_supply(&t, pgh, 1, 0, &src, 0), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_supply(&t, pgh, 1, 0, &src, PAGER_SUPPLY_MAX + PG), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pager_supply(&t, pgh, 1, 2 * PG, &src, 2 * PG), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pager_supply(&t, pgh, 2, 0, &src, PG), ERR_NOT_FOUND);
    KT_EQ(sys_pager_supply(&t, pgh, 1, PG, &src, 2 * PG), OK);
    KT_EQ(sys_pager_op(&t, ro, 1, PAGER_OP_DETACH, 0, 0, 0), ERR_ACCESS_DENIED);
    KT_EQ(sys_pager_op(&t, pgh, 1, 0, 0, 0, 0), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_op(&t, pgh, 1, 99, 0, 0, 0), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_INVALIDATE, 0, PG, 0), ERR_NOT_SUPPORTED);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_SET_SIZE, PG, 0, 0), ERR_NOT_SUPPORTED);

    /* vmo_make_exec takes no pager VMO, even with the power and the only
     * handle: its pages would arrive unchecked. The handle is gone. */
    handle_t vmex, ex;
    kobject_ref(root);
    kh = khandle_from_new(root, RIGHTS_BASIC | RIGHT_ROOT_VMEX);
    KT_EQ(handle_insert(&t, &kh, &vmex), OK);
    KT_EQ(sys_vmo_make_exec(&t, vh, vmex, &ex), ERR_BAD_STATE);
    KT_EQ(handle_info(&t, vh, &hi), ERR_BAD_HANDLE);
    handle_table_destroy(&t);   /* closes the pager: the GONE goes with it */
}

/* ---- a kernel-thread pager under load -------------------------------------- */

struct kpager {
    struct pager *pg;
    bool          stop;     /* atomic: the test is done */
    uint64_t      served;   /* read requests answered */
};

static void kpager_main(void *arg)
{
    struct kpager *k = arg;
    while (!__atomic_load_n(&k->stop, __ATOMIC_ACQUIRE)) {
        struct pager_request q;
        status_t st = pager_next(k->pg, 0, &q);
        if (st == ERR_SHOULD_WAIT) {
            (void)object_wait_one(pager_kobject(k->pg), SIG_READABLE,
                                  uptime_ns() + 10 * NS_PER_MS, NULL);   /* re-checks stop */
            continue;
        }
        KT_EQ(st, OK);
        if (q.kind == PAGER_REQ_GONE) {
            KT_EQ(pager_op(k->pg, q.key, PAGER_OP_DETACH, 0, 0, 0), OK);
            continue;
        }
        st = supply(k->pg, q.key, q.offset / PG, q.len / PG);
        KT_ASSERT(st == OK || st == ERR_NOT_FOUND);   /* (a detach may race it) */
        k->served++;
    }
}

struct sweeper {
    struct vmo *v;
    uint64_t    key, pages, seed;
    status_t    st;
};

/* Read every page of the VMO once, in a random order, checking each. */
static void sweeper_main(void *arg)
{
    struct sweeper *s = arg;
    uint64_t rng = s->seed;
    s->st = OK;
    for (uint64_t i = 0; i < s->pages && s->st == OK; i++) {
        uint64_t p = kt_rng(&rng) % s->pages, w = 0;
        s->st = vmo_read(s->v, p * PG + 8 * (kt_rng(&rng) % 512), &w, 8);
        if (s->st == OK && w != word(s->key, p))
            s->st = ERR_INTERNAL;
    }
}

/* Four readers sweep two VMOs at random against a pager thread: every
 * byte right, every request answered, nothing left queued. */
KTEST(pager_kthread_sweep)
{
    enum { READERS = 4, PAGES = 256 };
    struct job *j = kt_fresh_job();
    struct kpager k = { .pg = pg_new(j, ANSWER) };
    struct vmo *v[2];
    KT_EQ(pager_create_vmo(k.pg, 100, PAGES * PG, &v[0]), OK);
    KT_EQ(pager_create_vmo(k.pg, 101, PAGES * PG, &v[1]), OK);
    struct thread *pt = thread_create("kpager", kpager_main, &k, PRIO_DEFAULT);
    static struct sweeper s[READERS];
    struct thread *rt[READERS];
    for (unsigned i = 0; i < READERS; i++) {
        s[i] = (struct sweeper){ .v = v[i % 2], .key = 100 + i % 2, .pages = PAGES,
                                 .seed = 0x9e37 + i };
        rt[i] = thread_create("pager-sweep", sweeper_main, &s[i], PRIO_DEFAULT);
    }
    for (unsigned i = 0; i < READERS; i++) {
        thread_join(rt[i]);
        KT_EQ(s[i].st, OK);
    }
    put(v[0]);
    put(v[1]);   /* the pager thread answers both GONEs */
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (job_used(j, JOB_LIMIT_HANDLES) > 1 && uptime_ns() < deadline)
        thread_sleep_ms(1);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 1);   /* the pager alone: links answered */
    __atomic_store_n(&k.stop, true, __ATOMIC_RELEASE);
    thread_join(pt);
    KT_ASSERT(k.served > 0);
    none_queued(k.pg);
    pg_close(k.pg);
    kt_job_is_empty(j);
    job_unref(j);
}
