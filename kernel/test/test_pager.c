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
 * answered or the pager closes.
 *
 * The fault path has readers of its own: kernel threads faulting or
 * copying through an address space, and ring-3 threads (of no process)
 * whose kills are read back from the log. The page cache's section adds a
 * pager thread that answers with versions of a file (struct gpager),
 * budgets small enough to evict on every supply, and readers sampling the
 * budget as they go. The last section runs a verified program
 * (vmo_make_exec_verified) on them, its pages checked against their hashes
 * as they arrive. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/handle.h>
#include <jam/irq.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/sha256.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/usercopy.h>
#include <jam/vmo.h>
#include <jam/vmo_verify.h>

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

/* The word of page `page` in version gen (< 256) of key's file: version 0
 * is word()'s. */
static uint64_t gword(uint64_t key, uint64_t page, uint64_t gen)
{
    return word(key, page) | gen << 48;
}

/* Supply pages [first, first + n) of key with their words of version gen. */
static status_t supply_gen(struct pager *pg, uint64_t key, uint64_t first, uint64_t n,
                           uint64_t gen)
{
    uint64_t *buf = kmalloc(n * PG);
    KT_ASSERT(buf);
    for (uint64_t p = 0; p < n; p++)
        for (uint64_t i = 0; i < PG / 8; i++)
            buf[p * PG / 8 + i] = gword(key, first + p, gen);
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    status_t st = pager_supply(pg, key, first * PG, &src, n * PG);
    kfree(buf);
    return st;
}

/* Supply pages [first, first + n) of key with their words. */
static status_t supply(struct pager *pg, uint64_t key, uint64_t first, uint64_t n)
{
    return supply_gen(pg, key, first, n, 0);
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
    KT_EQ(aspace_fault(as, at + PG, ASPACE_READ), ERR_PEER_CLOSED);   /* gone: no wait */
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
    KT_EQ(sys_pager_op(&t, ro, 1, PAGER_OP_INVALIDATE, 0, PG, 0), ERR_ACCESS_DENIED);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_INVALIDATE, 0, PG, 1), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_INVALIDATE, 1, PG, 0), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_INVALIDATE, 0, 0, 0), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_INVALIDATE, PG, UINT64_MAX & ~(PG - 1), 0),
          ERR_INVALID_ARGS);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_INVALIDATE, 2 * PG, 2 * PG, 0), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pager_op(&t, pgh, 2, PAGER_OP_INVALIDATE, 0, PG, 0), ERR_NOT_FOUND);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_INVALIDATE, 0, 3 * PG, 0), OK);
    KT_EQ(sys_pager_op(&t, ro, 1, PAGER_OP_SET_SIZE, PG, 0, 0), ERR_ACCESS_DENIED);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_SET_SIZE, PG, PG, 0), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_SET_SIZE, PG, 0, 1), ERR_INVALID_ARGS);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_SET_SIZE, VMO_MAX_SIZE + 1, 0, 0), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pager_op(&t, pgh, 2, PAGER_OP_SET_SIZE, PG, 0, 0), ERR_NOT_FOUND);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_SET_SIZE, PG + 1, 0, 0), OK);   /* rounded up */
    KT_EQ(sys_vmo_get_size(&t, vh, &size), OK);
    KT_EQ(size, 2 * PG);
    KT_EQ(sys_pager_op(&t, pgh, 1, PAGER_OP_SET_SIZE, VMO_SMALL_MAX_SIZE, 0, 0), OK);
    KT_EQ(sys_vmo_get_size(&t, vh, &size), OK);
    KT_EQ(size, VMO_SMALL_MAX_SIZE);

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
    bool        done;   /* atomic: st is final */
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
    __atomic_store_n(&s->done, true, __ATOMIC_RELEASE);
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

/* ---- the fault path (PK2) ---------------------------------------------------- */

/* A fault on a pager VMO's absent page asks the pager and waits for it
 * without the region lock, then looks again from the start; it ends as
 * <jam/aspace.h> says. Three kinds of faulter: kernel threads calling
 * aspace_fault (struct kf), kernel threads copying through an address
 * space as a system call does (struct cp, with copy_from_user and
 * friends), and ring-3 threads running a three-instruction program
 * (struct ut), whose kill messages are read back from the kernel log. */

extern uint64_t user_faults;   /* uentry.c: user threads killed for a fault */

/* A kernel thread in aspace_fault, then (then_sleep) in an interruptible
 * sleep, to see whether an interrupt that came during the fault's wait is
 * still latched for it. */
struct kf {
    struct aspace *as;
    uint64_t       addr;
    bool           then_sleep;
    status_t       st, slept;   /* ERR_INTERNAL until set */
    struct thread *t;
};

static void kf_main(void *arg)
{
    struct kf *k = arg;
    status_t st = aspace_fault(k->as, k->addr, ASPACE_READ);
    if (k->then_sleep)
        k->slept = thread_sleep_until_interruptible(uptime_ns() + 10 * NS_PER_S);
    __atomic_store_n(&k->st, st, __ATOMIC_RELEASE);
}

static void kf_start(struct kf *k, struct aspace *as, uint64_t addr, bool then_sleep)
{
    *k = (struct kf){ .as = as, .addr = addr, .then_sleep = then_sleep, .st = ERR_INTERNAL,
                      .slept = ERR_INTERNAL };
    k->t = thread_create("pager-kfault", kf_main, k, PRIO_DEFAULT);
}

static bool kf_waiting(struct kf *k)
{
    return __atomic_load_n(&k->st, __ATOMIC_ACQUIRE) == ERR_INTERNAL;
}

static status_t kf_end(struct kf *k)
{
    thread_join(k->t);
    return k->st;
}

/* The address space a test maps a pager VMO into. */
#define U_CODE 0x10000000ull   /* the program (read, execute) */
#define U_DATA 0x10200000ull   /* where it stores what it read (read, write) */
#define U_FILE 0x20000000ull   /* the pager VMO (read) */

struct uas {
    struct aspace *as;
    struct vmo    *code, *data;
    uint64_t       file_len;    /* bytes of the pager VMO mapped at U_FILE */
};

/* The program each ring-3 thread runs: rsi = where to load from, rdi =
 * where to store it; then it spins until it is cancelled. */
static const uint8_t uprog[] = {
    0x48, 0x8b, 0x06,   /* mov rax, [rsi] */
    0x48, 0x89, 0x07,   /* mov [rdi], rax */
    0xeb, 0xfe,         /* jmp . */
};

static void uas_new(struct uas *u, struct vmo *file, uint64_t len)
{
    KT_EQ(aspace_create(&u->as), OK);
    KT_EQ(vmo_create(PG, 0, &u->code), OK);
    KT_EQ(vmo_write(u->code, 0, uprog, sizeof(uprog)), OK);
    uint64_t a = U_CODE;
    KT_EQ(aspace_map(u->as, u->code, 0, PG, ASPACE_READ | ASPACE_EXEC | ASPACE_FIXED, &a), OK);
    KT_EQ(vmo_create(PG, 0, &u->data), OK);
    a = U_DATA;
    KT_EQ(aspace_map(u->as, u->data, 0, PG, ASPACE_READ | ASPACE_WRITE | ASPACE_FIXED, &a), OK);
    u->file_len = len;
    a = U_FILE;
    KT_EQ(aspace_map(u->as, file, 0, len, ASPACE_READ | ASPACE_FIXED, &a), OK);
}

/* The pager VMO's mapping goes now (a dead thread's reaper may drop the
 * last address-space reference later); the rest with the references. */
static void uas_free(struct uas *u)
{
    (void)aspace_unmap(u->as, U_FILE, u->file_len);   /* (a test may have done it) */
    aspace_unref(u->as);
    put(u->code);
    put(u->data);
}

/* The word at U_DATA + off (0 until a thread stores there). */
static uint64_t stored(struct uas *u, uint64_t off)
{
    uint64_t w = 0;
    KT_EQ(vmo_read(u->data, off, &w, 8), OK);
    return w;
}

/* Wait (a broken kernel fails, not hangs) for the word at U_DATA + off. */
static void wait_stored(struct uas *u, uint64_t off, uint64_t want)
{
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (stored(u, off) != want && uptime_ns() < deadline)
        thread_sleep_ms(1);
    KT_EQ(stored(u, off), want);
}

/* A ring-3 thread of no process (its kill ends just the thread). */
struct ut {
    struct aspace *as;
    uint64_t       load, store;
    struct thread *t;
};

static void ut_main(void *arg)
{
    struct ut *u = arg;
    struct thread *t = current_thread();
    KT_ASSERT(fpu_ustate_alloc(t) == OK);
    aspace_ref(u->as);   /* the thread's, dropped when it is reaped */
    irq_disable();
    t->aspace = u->as;
    aspace_switch(NULL, u->as);
    irq_enable();
    arch_enter_user(U_CODE, U_DATA + PG, u->store, u->load);
}

static void ut_start(struct ut *u, struct uas *as, const char *name, uint64_t load, uint64_t store)
{
    u->as = as->as;
    u->load = load;
    u->store = store;
    u->t = thread_create(name, ut_main, u, PRIO_DEFAULT);
}

/* Cancel a thread that is spinning in ring 3 (or waiting) and join it. */
static void ut_stop(struct ut *u)
{
    thread_cancel(u->t);
    thread_join(u->t);
}

static bool has(const char *s, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (!memcmp(s + i, needle, m))
            return true;
    return false;
}

/* Did the log say, since position `from`, on one line, that thread `name`
 * was killed for `why`? */
static bool killed_for(uint64_t from, const char *name, const char *why)
{
    enum { CAP = 256 * 1024 };
    char *buf = kmalloc(CAP);
    KT_ASSERT(buf);
    char quoted[40];
    ksnprintf(quoted, sizeof(quoted), "killed thread \"%s\"", name);
    uint64_t deadline = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    bool found = false;
    while (!found && uptime_ns() < deadline) {
        uint64_t first, pos = klog_head() - from > CAP ? klog_head() - CAP : from;
        size_t n = klog_read_at(pos, buf, CAP, &first);
        for (size_t i = 0; i < n && !found;) {
            size_t e = i;
            while (e < n && buf[e] != '\n')
                e++;
            found = has(buf + i, e - i, quoted) && has(buf + i, e - i, why);
            i = e + 1;
        }
        if (!found)
            thread_sleep_ms(5);
    }
    kfree(buf);
    return found;
}

/* One ring-3 thread faulting on a pager VMO: the job, the pager, the VMO
 * (key, 4 pages), the address space, the thread. */
struct ufix {
    struct job    *j;
    struct pager  *pg;
    struct vmo    *v;
    struct uas     us;
    struct ut      u;
    uint64_t       key, log0, faults0;
    const char    *name;
    bool           closed;   /* the test closed the pager itself */
};

static void ufix_start(struct ufix *f, uint64_t answer_ns, uint64_t key, const char *name,
                       uint64_t page)
{
    f->j = kt_fresh_job();
    f->pg = pg_new(f->j, answer_ns);
    f->key = key;
    f->name = name;
    f->closed = false;
    KT_EQ(pager_create_vmo(f->pg, key, 4 * PG, &f->v), OK);
    uas_new(&f->us, f->v, 4 * PG);
    f->log0 = klog_head();
    f->faults0 = __atomic_load_n(&user_faults, __ATOMIC_RELAXED);
    ut_start(&f->u, &f->us, name, U_FILE + page * PG + 8, U_DATA);
}

/* The thread was killed for `why` (a fault of its own) and stored nothing. */
static void ufix_killed(struct ufix *f, const char *why)
{
    thread_join(f->u.t);
    KT_ASSERT(killed_for(f->log0, f->name, why));
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), f->faults0 + 1);
    KT_EQ(stored(&f->us, 0), 0);
}

static void ufix_end(struct ufix *f)
{
    uas_free(&f->us);
    if (f->v)
        put(f->v);
    if (!f->closed)
        pg_close(f->pg);
    kt_job_is_empty(f->j);
    job_unref(f->j);
}

/* A kernel thread's own fault: it waits without the region lock (the
 * address space maps, unmaps and counts meanwhile), the supply ends it
 * with the page installed; a write is refused without asking, and a
 * detached VMO's absent page is ERR_PEER_CLOSED at once. */
KTEST(pager_fault_wait)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v, *other;
    KT_EQ(pager_create_vmo(pg, 15, 2 * PG, &v), OK);
    KT_EQ(vmo_create(PG, 0, &other), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0, b = 0;
    KT_EQ(aspace_map(as, v, 0, 2 * PG, ASPACE_READ, &a), OK);

    struct kf k;
    kf_start(&k, as, a + 8, false);
    struct pager_request q;
    next(pg, 0, &q);
    is_read(&q, 15, 0, 2);
    thread_sleep_ms(5);
    KT_ASSERT(kf_waiting(&k));
    /* The region lock is free while it waits. */
    KT_EQ(aspace_map(as, other, 0, PG, ASPACE_READ | ASPACE_WRITE, &b), OK);
    KT_EQ(aspace_fault(as, b, ASPACE_WRITE), OK);
    KT_EQ(aspace_mapping_count(as), 2);
    KT_EQ(aspace_unmap(as, b, PG), OK);
    KT_EQ(aspace_pte(as, a), 0);
    KT_ASSERT(kf_waiting(&k));

    KT_EQ(supply(pg, 15, 0, 2), OK);
    KT_EQ(kf_end(&k), OK);
    KT_EQ(aspace_pte(as, a) & 0x000ffffffffff000ull, vmo_page_phys(v, 0));
    KT_EQ(aspace_fault(as, a, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(pager_op(pg, 15, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(aspace_pte(as, a), 0);
    KT_EQ(aspace_fault(as, a, ASPACE_READ), ERR_PEER_CLOSED);
    none_queued(pg);
    KT_EQ(aspace_unmap(as, a, 2 * PG), OK);
    aspace_unref(as);
    put(other);
    put(v);
    pg_close(pg);
}

/* A fault's wait is not interruptible (M13's thread_interrupt): an
 * interrupt during it is a spurious wakeup, the page still arrives, and
 * the interrupt stays latched for the thread's next interruptible wait,
 * which it ends at once. */
KTEST(pager_fault_interrupt)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 16, PG, &v), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_READ, &a), OK);

    struct kf k;
    kf_start(&k, as, a, true);
    struct pager_request q;
    next(pg, 0, &q);
    thread_sleep_ms(5);
    thread_interrupt(k.t);
    thread_interrupt(k.t);   /* latched once, not counted */
    thread_sleep_ms(20);
    KT_ASSERT(kf_waiting(&k));
    KT_ASSERT(thread_interrupt_pending(k.t));
    none_queued(pg);   /* still the one request: nobody asked again */
    KT_EQ(supply(pg, 16, 0, 1), OK);
    KT_EQ(kf_end(&k), OK);
    KT_EQ(k.slept, ERR_INTERRUPTED);   /* the latched interrupt (else OK, 10 s later) */
    KT_EQ(aspace_unmap(as, a, PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);
}

/* A ring-3 thread's load from an absent page waits for the supply and then
 * completes with the right bytes; the rest of the address space works
 * meanwhile, an interrupt doesn't end the wait, and nobody is killed. */
KTEST(pager_fault_user_supplied)
{
    struct ufix f;
    ufix_start(&f, ANSWER, 20, "pk2-supplied", 1);
    struct pager_request q;
    next(f.pg, 0, &q);
    is_read(&q, 20, 1, 3);
    thread_sleep_ms(10);
    KT_EQ(stored(&f.us, 0), 0);
    KT_EQ(aspace_mapping_count(f.us.as), 3);   /* the region lock is free */
    thread_interrupt(f.u.t);
    thread_sleep_ms(10);
    KT_EQ(stored(&f.us, 0), 0);
    KT_EQ(supply(f.pg, 20, 1, 3), OK);
    wait_stored(&f.us, 0, word(20, 1));
    KT_ASSERT(thread_interrupt_pending(f.u.t));
    ut_stop(&f.u);
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), f.faults0);
    none_queued(f.pg);
    ufix_end(&f);
}

/* PAGER_OP_FAIL, with either status: the thread is killed, "couldn't
 * read it". */
KTEST(pager_fault_user_failed)
{
    static const status_t how[] = { ERR_IO, ERR_OUT_OF_RANGE };
    for (unsigned i = 0; i < 2; i++) {
        struct ufix f;
        ufix_start(&f, ANSWER, 21, i ? "pk2-failed-oor" : "pk2-failed-io", 2);
        struct pager_request q;
        next(f.pg, 0, &q);
        is_read(&q, 21, 2, 2);
        KT_EQ(pager_op(f.pg, 21, PAGER_OP_FAIL, 2 * PG, PG, how[i]), OK);
        ufix_killed(&f, "page fault: the file's pager couldn't read it");
        ufix_end(&f);
    }
}

/* The VMO detached, or the pager's last handle closed, while the thread
 * waits: killed, "is gone". */
KTEST(pager_fault_user_detached)
{
    for (unsigned i = 0; i < 2; i++) {
        struct ufix f;
        ufix_start(&f, ANSWER, 22, i ? "pk2-closed" : "pk2-detached", 0);
        struct pager_request q;
        next(f.pg, 0, &q);
        if (i) {
            pg_close(f.pg);
            f.closed = true;
        } else {
            KT_EQ(pager_op(f.pg, 22, PAGER_OP_DETACH, 0, 0, 0), OK);
        }
        ufix_killed(&f, "page fault: the file's pager is gone");
        ufix_end(&f);
    }
}

/* No answer in the pager's answer time: killed at the deadline, "didn't
 * answer", and the request is gone from the queue. */
KTEST(pager_fault_user_timed_out)
{
    struct ufix f;
    uint64_t t0 = uptime_ns();
    ufix_start(&f, 30 * NS_PER_MS, 23, "pk2-late", 3);
    ufix_killed(&f, "page fault: the file's pager didn't answer");
    KT_ASSERT(uptime_ns() - t0 >= 30 * NS_PER_MS);
    none_queued(f.pg);
    ufix_end(&f);
}

/* Killed while it waits: the thread just goes (no fault of its own: no
 * kill message, no count), and its request stays for the pager, whose
 * answer still lands. */
KTEST(pager_fault_user_killed)
{
    struct ufix f;
    ufix_start(&f, ANSWER, 24, "pk2-cancel", 0);
    struct pager_request q;
    next(f.pg, 0, &q);
    thread_sleep_ms(5);
    ut_stop(&f.u);
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), f.faults0);
    KT_EQ(supply(f.pg, 24, 0, 4), OK);
    KT_EQ(vmo_committed(f.v), 4 * PG);
    ufix_end(&f);
}

/* Unmapped by another thread while it waits, with the mapping holding the
 * VMO's last reference but the waiter's: the VMO lives on (no
 * PAGER_REQ_GONE, the supply finds it), the retry finds no mapping and
 * the thread is killed as for any unmapped address, and only then does
 * the VMO die. */
KTEST(pager_fault_user_unmapped)
{
    struct ufix f;
    ufix_start(&f, ANSWER, 25, "pk2-unmapped", 1);
    struct pager_request q;
    next(f.pg, 0, &q);
    thread_sleep_ms(5);
    put(f.v);
    f.v = NULL;
    KT_EQ(aspace_unmap(f.us.as, U_FILE, 4 * PG), OK);
    thread_sleep_ms(5);
    none_queued(f.pg);   /* no GONE: the waiter holds the VMO */
    KT_EQ(supply(f.pg, 25, 1, 3), OK);
    ufix_killed(&f, "): page fault at");
    next(f.pg, 0, &q);   /* the waiter's reference was the last */
    KT_EQ(q.kind, PAGER_REQ_GONE);
    KT_EQ(q.key, 25);
    KT_EQ(pager_op(f.pg, 25, PAGER_OP_DETACH, 0, 0, 0), OK);
    ufix_end(&f);
}

/* Eight ring-3 threads load from one absent page: one request, one supply,
 * every thread gets its bytes. */
KTEST(pager_fault_user_many)
{
    enum { N = 8 };
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 26, 4 * PG, &v), OK);
    struct uas us;
    uas_new(&us, v, 4 * PG);
    uint64_t faults0 = __atomic_load_n(&user_faults, __ATOMIC_RELAXED);
    static struct ut u[N];
    for (unsigned i = 0; i < N; i++)
        ut_start(&u[i], &us, "pk2-many", U_FILE + 2 * PG + 8 * i, U_DATA + 8 * i);
    struct pager_request q;
    next(pg, 0, &q);
    is_read(&q, 26, 2, 2);
    thread_sleep_ms(20);   /* let them all join (a late one finds the page anyway) */
    none_queued(pg);
    KT_EQ(supply(pg, 26, 2, 2), OK);
    for (unsigned i = 0; i < N; i++)
        wait_stored(&us, 8 * i, word(26, 2));
    for (unsigned i = 0; i < N; i++)
        ut_stop(&u[i]);
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), faults0);
    none_queued(pg);
    uas_free(&us);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* A kernel thread copying from (or to) user memory through an address
 * space, as a system call does, optionally holding a mutex across it. */
enum cp_how { CP_FROM, CP_STR, CP_TO };

struct cp {
    struct aspace *as;
    uint64_t       addr;
    enum cp_how    how;
    struct mutex  *hold;
    uint64_t       word;
    status_t       st;   /* ERR_INTERNAL until it returns */
    struct thread *t;
};

static void cp_main(void *arg)
{
    struct cp *c = arg;
    struct thread *t = current_thread();
    aspace_ref(c->as);
    irq_disable();
    t->aspace = c->as;
    aspace_switch(NULL, c->as);
    irq_enable();
    if (c->hold)
        mutex_lock(c->hold);
    status_t st;
    if (c->how == CP_FROM) {
        st = copy_from_user(&c->word, c->addr, sizeof(c->word));
    } else if (c->how == CP_STR) {
        char s[16];
        st = copy_str_from_user(s, c->addr, sizeof(s), NULL);
    } else {
        st = copy_to_user(c->addr, &c->word, sizeof(c->word));
    }
    if (c->hold)
        mutex_unlock(c->hold);
    irq_disable();
    t->aspace = NULL;
    aspace_switch(c->as, NULL);
    irq_enable();
    aspace_unref(c->as);
    __atomic_store_n(&c->st, st, __ATOMIC_RELEASE);
}

static void cp_start(struct cp *c, struct aspace *as, uint64_t addr, enum cp_how how,
                     struct mutex *hold)
{
    *c = (struct cp){ .as = as, .addr = addr, .how = how, .hold = hold, .st = ERR_INTERNAL };
    c->t = thread_create("pager-copy", cp_main, c, PRIO_DEFAULT);
}

static status_t cp_end(struct cp *c)
{
    thread_join(c->t);
    return c->st;
}

/* The copies: a supplied page is copied; every way a pager fails one is
 * ERR_IO for the copy (PAGER_OP_FAIL with either status, detach, no
 * answer in time), for strings too; a copy to a pager mapping is refused
 * at once without asking (it is never writable). */
KTEST(pager_fault_copies)
{
    struct pager *pg = pg_new(NULL, ANSWER), *slow = pg_new(NULL, 30 * NS_PER_MS);
    struct vmo *v, *sv;
    KT_EQ(pager_create_vmo(pg, 27, 8 * PG, &v), OK);
    KT_EQ(pager_create_vmo(slow, 28, PG, &sv), OK);
    struct uas us;
    uas_new(&us, v, 8 * PG);
    uint64_t s = U_FILE + 8 * PG;
    KT_EQ(aspace_map(us.as, sv, 0, PG, ASPACE_READ | ASPACE_FIXED, &s), OK);
    struct cp c;
    struct pager_request q;

    cp_start(&c, us.as, U_FILE + 16, CP_FROM, NULL);   /* supplied */
    next(pg, 0, &q);
    is_read(&q, 27, 0, 8);
    KT_EQ(supply(pg, 27, 0, 1), OK);   /* the request's first page only */
    thread_sleep_ms(5);
    KT_EQ(pager_op(pg, 27, PAGER_OP_FAIL, PG, PG, ERR_IO), OK);   /* the rest fails */
    KT_EQ(cp_end(&c), OK);   /* its own page came: OK (a request is answered whole) */
    KT_EQ(c.word, word(27, 0));

    static const status_t how[] = { ERR_IO, ERR_OUT_OF_RANGE };
    for (unsigned i = 0; i < 2; i++) {   /* failed */
        cp_start(&c, us.as, U_FILE + (2 + i) * PG, i ? CP_STR : CP_FROM, NULL);
        next(pg, 0, &q);
        KT_EQ(pager_op(pg, 27, PAGER_OP_FAIL, (2 + i) * PG, PG, how[i]), OK);
        KT_EQ(cp_end(&c), ERR_IO);
    }

    cp_start(&c, us.as, U_FILE + 5 * PG, CP_TO, NULL);   /* never writable: no wait */
    KT_EQ(cp_end(&c), ERR_INVALID_ARGS);
    none_queued(pg);

    cp_start(&c, us.as, s, CP_FROM, NULL);   /* no answer in time */
    KT_EQ(cp_end(&c), ERR_IO);

    cp_start(&c, us.as, U_FILE + 6 * PG, CP_FROM, NULL);   /* detached while it waits */
    next(pg, 0, &q);
    KT_EQ(pager_op(pg, 27, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(cp_end(&c), ERR_IO);
    cp_start(&c, us.as, U_FILE, CP_STR, NULL);   /* and after: at once */
    KT_EQ(cp_end(&c), ERR_IO);

    uas_free(&us);
    put(v);
    put(sv);
    pg_close(pg);
    pg_close(slow);
}

/* A copy killed while it waits returns ERR_CANCELED. A copy that waits
 * with a mutex held is reported (aspace_pager_waits_held): PK2's audit
 * found no such copy in the kernel, and this is how a new one shows. */
KTEST(pager_fault_copy_cancel_and_held)
{
    struct pager *pg = pg_new(NULL, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 29, 2 * PG, &v), OK);
    struct uas us;
    uas_new(&us, v, 2 * PG);
    struct cp c;
    struct pager_request q;

    cp_start(&c, us.as, U_FILE, CP_FROM, NULL);
    next(pg, 0, &q);
    thread_sleep_ms(5);
    thread_cancel(c.t);
    KT_EQ(cp_end(&c), ERR_CANCELED);

    static struct mutex m;
    mutex_init(&m, "kt pager copy");
    uint64_t held = __atomic_load_n(&aspace_pager_waits_held, __ATOMIC_RELAXED);
    cp_start(&c, us.as, U_FILE + PG, CP_FROM, &m);   /* joins the request above */
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (__atomic_load_n(&aspace_pager_waits_held, __ATOMIC_RELAXED) == held &&
           uptime_ns() < deadline)
        thread_sleep_ms(1);
    KT_GLOBAL_EQ(__atomic_load_n(&aspace_pager_waits_held, __ATOMIC_RELAXED), held + 1);
    KT_EQ(supply(pg, 29, 0, 2), OK);
    KT_EQ(cp_end(&c), OK);
    KT_EQ(c.word, word(29, 1));
    none_queued(pg);

    uas_free(&us);
    put(v);
    pg_close(pg);
}

/* The hostile pager: it never answers. Every thread that touches its pages
 * waits until the deadline and dies; meanwhile the same address space
 * maps, unmaps and faults its other pages, another address space reads a
 * healthy pager's VMO, and afterwards nothing is left: no request, no
 * charge. */
KTEST(pager_fault_hostile)
{
    enum { N = 3 };
    struct job *j = kt_fresh_job();
    struct pager *bad = pg_new(j, NS_PER_S), *good = pg_new(NULL, ANSWER);
    struct vmo *v, *gv, *scratch;
    KT_EQ(pager_create_vmo(bad, 30, 4 * PG, &v), OK);
    KT_EQ(pager_create_vmo(good, 31, PG, &gv), OK);
    KT_EQ(supply(good, 31, 0, 1), OK);
    KT_EQ(vmo_create(PG, 0, &scratch), OK);
    struct uas us;
    uas_new(&us, v, 4 * PG);
    uint64_t log0 = klog_head(), faults0 = __atomic_load_n(&user_faults, __ATOMIC_RELAXED);
    uint64_t t0 = uptime_ns();
    static struct ut u[N + 1];
    for (unsigned i = 0; i < N; i++)
        ut_start(&u[i], &us, "pk2-hostile", U_FILE + (i % 2) * 2 * PG, U_DATA + 8 * i);
    struct cp c;
    cp_start(&c, us.as, U_FILE + 3 * PG, CP_FROM, NULL);
    thread_sleep_ms(20);

    /* The kernel goes on, and so does the rest of the address space. */
    uint64_t a = 0;
    KT_EQ(aspace_map(us.as, scratch, 0, PG, ASPACE_READ, &a), OK);
    KT_EQ(aspace_unmap(us.as, a, PG), OK);
    KT_EQ(vmo_write(us.data, PG - 8, &t0, 8), OK);
    ut_start(&u[N], &us, "pk2-bystander", U_DATA + PG - 8, U_DATA + 64);
    wait_stored(&us, 64, t0);
    /* Before the deadline: had the waits kept the region lock, the
     * bystander's fault would have waited for them to die. */
    KT_IDLE_ASSERT(__atomic_load_n(&c.st, __ATOMIC_ACQUIRE) == ERR_INTERNAL);
    uint64_t w;
    KT_EQ(vmo_read(gv, 0, &w, 8), OK);
    KT_EQ(w, word(31, 0));
    ut_stop(&u[N]);

    for (unsigned i = 0; i < N; i++)
        thread_join(u[i].t);
    KT_EQ(cp_end(&c), ERR_IO);
    KT_ASSERT(uptime_ns() - t0 >= NS_PER_S);
    KT_ASSERT(killed_for(log0, "pk2-hostile", "page fault: the file's pager didn't answer"));
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), faults0 + N);
    for (unsigned i = 0; i < N; i++)
        KT_EQ(stored(&us, 8 * i), 0);
    none_queued(bad);   /* every request failed, none delivered */

    uas_free(&us);
    put(v);
    put(gv);
    put(scratch);
    pg_close(bad);
    pg_close(good);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- the page cache: budget, eviction, invalidate, set-size ------------------- */

/* A pager with a budget of its own. */
static struct pager *pg_budget(struct job *j, uint64_t max_pages, uint64_t answer_ns)
{
    struct pager *pg;
    KT_EQ(pager_create(j, max_pages, answer_ns, &pg), OK);
    return pg;
}

/* A pager thread answering every read request with its pages (or, with
 * `whole`, with pages [0, whole) of the VMO, whatever was asked) as
 * version `gen` of the file, and every GONE with a detach. */
struct gpager {
    struct pager  *pg;
    uint64_t       whole;    /* 0: the request's own pages */
    uint64_t       gen;      /* atomic: the file's version now */
    bool           stop;     /* atomic: the test is done */
    uint64_t       served;   /* atomic: read requests answered */
    status_t       st;       /* the first answer that went wrong, else OK */
    struct thread *t;
};

static void gpager_main(void *arg)
{
    struct gpager *g = arg;
    while (!__atomic_load_n(&g->stop, __ATOMIC_ACQUIRE)) {
        struct pager_request q;
        status_t st = pager_next(g->pg, 0, &q);
        if (st == ERR_SHOULD_WAIT) {
            (void)object_wait_one(pager_kobject(g->pg), SIG_READABLE,
                                  uptime_ns() + 10 * NS_PER_MS, NULL);   /* re-checks stop */
            continue;
        }
        if (st == OK && q.kind == PAGER_REQ_GONE) {
            st = pager_op(g->pg, q.key, PAGER_OP_DETACH, 0, 0, 0);
        } else if (st == OK) {
            uint64_t first = g->whole ? 0 : q.offset / PG, n = g->whole ? g->whole : q.len / PG;
            st = supply_gen(g->pg, q.key, first, n, __atomic_load_n(&g->gen, __ATOMIC_ACQUIRE));
            if (st == ERR_NOT_FOUND)
                st = OK;   /* (a detach may race it) */
            __atomic_add_fetch(&g->served, 1, __ATOMIC_RELEASE);
        }
        if (st != OK && g->st == OK)
            g->st = st;
    }
}

static void gp_start(struct gpager *g, struct pager *pg, uint64_t whole)
{
    *g = (struct gpager){ .pg = pg, .whole = whole, .st = OK };
    g->t = thread_create("pager-gpager", gpager_main, g, PRIO_DEFAULT);
}

static uint64_t gp_served(struct gpager *g)
{
    return __atomic_load_n(&g->served, __ATOMIC_ACQUIRE);
}

/* Stop the pager thread: how its answers went. */
static status_t gp_stop(struct gpager *g)
{
    __atomic_store_n(&g->stop, true, __ATOMIC_RELEASE);
    thread_join(g->t);
    return g->st;
}

/* Move the current kernel thread from address space `was` into `as`
 * (either may be NULL), as a system call's copies run. */
static void enter_aspace(struct aspace *as, struct aspace *was)
{
    struct thread *t = current_thread();
    if (as)
        aspace_ref(as);
    irq_disable();
    t->aspace = as;
    aspace_switch(was, as);
    irq_enable();
    if (was)
        aspace_unref(was);
}

/* The budget: a supply bigger than it keeps its newest pages, and the
 * oldest page goes first; a second answer evicts nothing. The job pays
 * the pages, their tables and 16 bytes a resident page, exactly: a VMO's
 * death and a detach give it all back. */
KTEST(pager_cache_budget)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_budget(j, 4, ANSWER);
    struct vmo *v, *v2;
    KT_EQ(pager_create_vmo(pg, 40, 32 * PG, &v), OK);
    KT_EQ(supply(pg, 40, 0, 16), OK);
    KT_EQ(pager_resident(pg), 4);
    KT_EQ(vmo_committed(v), 4 * PG);
    for (uint64_t i = 0; i < 16; i++)
        KT_EQ(vmo_page_phys(v, i * PG) != 0, i >= 12);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 4 + 2);   /* the pages, a mid and a leaf table */
    KT_EQ(job_used(j, JOB_LIMIT_MSG_BYTES), 4 * 16);

    uint64_t pa = vmo_page_phys(v, 12 * PG);
    KT_EQ(supply(pg, 40, 12, 4), OK);   /* present: the first copy stays, nothing goes */
    KT_EQ(vmo_page_phys(v, 12 * PG), pa);
    KT_EQ(vmo_committed(v), 4 * PG);
    KT_EQ(supply(pg, 40, 20, 1), OK);   /* the oldest, 12, makes room */
    KT_EQ(vmo_page_phys(v, 12 * PG), 0);
    KT_ASSERT(vmo_page_phys(v, 13 * PG) && vmo_page_phys(v, 20 * PG));
    KT_EQ(pager_resident(pg), 4);
    uint64_t w;
    KT_EQ(vmo_read(v, 13 * PG + 8, &w, 8), OK);
    KT_EQ(w, word(40, 13));
    none_queued(pg);

    /* The budget is the pager's, across its VMOs; a VMO's death frees its
     * share. */
    KT_EQ(pager_create_vmo(pg, 41, 2 * PG, &v2), OK);
    KT_EQ(supply(pg, 41, 0, 2), OK);
    KT_EQ(pager_resident(pg), 4);
    KT_EQ(vmo_committed(v), 2 * PG);
    put(v2);
    KT_EQ(pager_resident(pg), 2);
    struct pager_request q;
    next(pg, 0, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);
    KT_EQ(pager_op(pg, 41, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 2 + 2);
    KT_EQ(job_used(j, JOB_LIMIT_MSG_BYTES), 2 * 16);

    KT_EQ(pager_op(pg, 40, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(pager_resident(pg), 0);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 2);   /* the tables stay until the VMO goes */
    KT_EQ(job_used(j, JOB_LIMIT_MSG_BYTES), 0);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* A kernel thread reading a mapping of a pager VMO through copy_from_user,
 * as a system call does, page by page in order and then at random, with
 * the cache's count and the job's pages sampled after every page. */
struct mapper {
    struct aspace *as;
    struct pager  *pg;
    struct job    *j;
    uint64_t       key, pages, seed;
    uint64_t       max_resident, max_job;   /* the most seen */
    status_t       st;   /* ERR_INTERNAL until it ends; ERR_BAD_STATE: a wrong word */
    struct thread *t;
};

static void mapper_main(void *arg)
{
    struct mapper *m = arg;
    enter_aspace(m->as, NULL);
    uint64_t rng = m->seed;
    status_t st = OK;
    for (uint64_t i = 0; i < 2 * m->pages && st == OK; i++) {
        uint64_t p = i < m->pages ? i : kt_rng(&rng) % m->pages, w = 0;
        st = copy_from_user(&w, U_FILE + p * PG + 8 * (kt_rng(&rng) % 512), 8);
        if (st == OK && w != word(m->key, p))
            st = ERR_BAD_STATE;
        uint64_t r = pager_resident(m->pg), jp = job_used(m->j, JOB_LIMIT_PAGES);
        m->max_resident = r > m->max_resident ? r : m->max_resident;
        m->max_job = jp > m->max_job ? jp : m->max_job;
    }
    enter_aspace(NULL, m->as);
    __atomic_store_n(&m->st, st, __ATOMIC_RELEASE);
}

/* A mapper reading four times the budget sees every byte right, and the
 * pager's job never holds more than the budget (and two tables): the
 * evicted pages are zapped from the mapping and asked for again. */
KTEST(pager_cache_mapper_4x)
{
    enum { BUDGET = 16, PAGES = 4 * BUDGET };
    struct job *j = kt_fresh_job();
    struct gpager g;
    gp_start(&g, pg_budget(j, BUDGET, ANSWER), 0);
    struct vmo *v;
    KT_EQ(pager_create_vmo(g.pg, 42, PAGES * PG, &v), OK);
    struct uas us;
    uas_new(&us, v, PAGES * PG);
    static struct mapper m;
    m = (struct mapper){ .as = us.as, .pg = g.pg, .j = j, .key = 42, .pages = PAGES,
                         .seed = 0x51ce, .st = ERR_INTERNAL };
    m.t = thread_create("pager-mapper", mapper_main, &m, PRIO_DEFAULT);
    thread_join(m.t);
    KT_EQ(m.st, OK);
    KT_EQ(m.max_resident, BUDGET);
    KT_ASSERT(m.max_job <= BUDGET + 2);
    KT_ASSERT(gp_served(&g) > PAGES / PAGER_REQ_PAGES);   /* evicted pages came again */
    KT_ASSERT(vmo_committed(v) <= BUDGET * PG);
    uas_free(&us);
    put(v);
    KT_EQ(gp_stop(&g), OK);
    pg_close(g.pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* Wait (a broken kernel fails, not hangs) until the pager thread has
 * answered n requests, then a little longer: it must not answer more. */
static void served_exactly(struct gpager *g, uint64_t n)
{
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (gp_served(g) < n && uptime_ns() < deadline)
        thread_sleep_ms(1);
    thread_sleep_ms(10);
    KT_EQ(gp_served(g), n);
}

/* Eviction on every supply: a budget of one page and a pager that answers
 * every request with both of the VMO's pages, so the page a reader asked
 * for is evicted by the page after it before the request is answered.
 * The reader asks eight times and gives up: vmo_read ERR_NO_MEMORY, a
 * ring-3 load killed for "out of memory"; never a hang or a ninth
 * request. */
KTEST(pager_cache_evict_every_supply)
{
    struct job *j = kt_fresh_job();
    struct gpager g;
    gp_start(&g, pg_budget(j, 1, ANSWER), 2);
    struct vmo *v;
    KT_EQ(pager_create_vmo(g.pg, 43, 2 * PG, &v), OK);
    uint64_t w;
    KT_EQ(vmo_read(v, 8, &w, 8), ERR_NO_MEMORY);
    served_exactly(&g, 8);
    KT_EQ(vmo_page_phys(v, 0), 0);
    KT_ASSERT(vmo_page_phys(v, PG) != 0);
    KT_EQ(pager_resident(g.pg), 1);

    struct uas us;
    uas_new(&us, v, 2 * PG);
    uint64_t log0 = klog_head(), faults0 = __atomic_load_n(&user_faults, __ATOMIC_RELAXED);
    struct ut u;
    ut_start(&u, &us, "cache-thrash", U_FILE + 8, U_DATA);
    thread_join(u.t);
    KT_ASSERT(killed_for(log0, "cache-thrash", "page fault: out of memory"));
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), faults0 + 1);
    KT_EQ(stored(&us, 0), 0);
    served_exactly(&g, 16);
    uas_free(&us);
    put(v);
    KT_EQ(gp_stop(&g), OK);
    pg_close(g.pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* A reader looping over a pager VMO until told to stop, by vmo_read or
 * (with an address space) through a mapping: every word must be its
 * page's, of a version no newer than the pager's newest. */
struct looper {
    struct vmo    *v;
    struct aspace *as;       /* NULL: vmo_read */
    struct gpager *g;
    uint64_t       key, pages, reads;
    bool           stop;     /* atomic */
    status_t       st;       /* ERR_INTERNAL until it ends; ERR_BAD_STATE: a wrong word */
    struct thread *t;
};

static void looper_main(void *arg)
{
    struct looper *l = arg;
    if (l->as)
        enter_aspace(l->as, NULL);
    uint64_t rng = l->key * 77 + (l->as ? 1 : 2);
    status_t st = OK;
    while (st == OK && !__atomic_load_n(&l->stop, __ATOMIC_ACQUIRE)) {
        uint64_t p = kt_rng(&rng) % l->pages, off = p * PG + 8 * (kt_rng(&rng) % 512), w = 0;
        st = l->as ? copy_from_user(&w, U_FILE + off, 8) : vmo_read(l->v, off, &w, 8);
        uint64_t newest = __atomic_load_n(&l->g->gen, __ATOMIC_ACQUIRE);
        if (st == OK && ((w & ~(0xffull << 48)) != word(l->key, p) || (w >> 48 & 0xff) > newest))
            st = ERR_BAD_STATE;
        l->reads++;
    }
    if (l->as)
        enter_aspace(NULL, l->as);
    __atomic_store_n(&l->st, st, __ATOMIC_RELEASE);
}

/* PAGER_OP_INVALIDATE drops the range's pages from the VMO and its
 * mappings (and their charges); the next access asks again and sees the
 * new version. Under two readers (one through a mapping) and a pager
 * thread, fifty invalidations: no reader sees an error or a wrong word. */
KTEST(pager_cache_invalidate)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_budget(j, 64, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 44, 4 * PG, &v), OK);
    struct uas us;
    uas_new(&us, v, 4 * PG);
    KT_EQ(supply(pg, 44, 0, 4), OK);
    KT_EQ(aspace_fault(us.as, U_FILE + PG, ASPACE_READ), OK);
    KT_ASSERT(aspace_pte(us.as, U_FILE + PG) != 0);
    KT_EQ(pager_op(pg, 44, PAGER_OP_INVALIDATE, PG, 2 * PG, 0), OK);
    KT_EQ(aspace_pte(us.as, U_FILE + PG), 0);
    KT_EQ(vmo_committed(v), 2 * PG);
    KT_EQ(pager_resident(pg), 2);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 2 + 2);
    KT_EQ(job_used(j, JOB_LIMIT_MSG_BYTES), 2 * 16);
    struct rd r;
    rd_start(&r, v, PG, 2 * PG);
    struct pager_request q;
    next(pg, 0, &q);
    is_read(&q, 44, 1, 2);   /* asked again (page 3 is still there) */
    KT_EQ(supply_gen(pg, 44, 1, 2, 1), OK);
    KT_EQ(rd_end(&r, 0), OK);
    uint64_t w;
    KT_EQ(vmo_read(v, 2 * PG + 16, &w, 8), OK);
    KT_EQ(w, gword(44, 2, 1));
    KT_EQ(vmo_read(v, 0, &w, 8), OK);
    KT_EQ(w, word(44, 0));

    struct gpager g;
    gp_start(&g, pg, 0);
    __atomic_store_n(&g.gen, 1, __ATOMIC_RELEASE);
    static struct looper l[2];
    for (unsigned i = 0; i < 2; i++) {
        l[i] = (struct looper){ .v = v, .as = i ? us.as : NULL, .g = &g, .key = 44, .pages = 4,
                                .st = ERR_INTERNAL };
        l[i].t = thread_create("pager-looper", looper_main, &l[i], PRIO_DEFAULT);
    }
    for (uint64_t gen = 2; gen < 52; gen++) {
        __atomic_store_n(&g.gen, gen, __ATOMIC_RELEASE);
        KT_EQ(pager_op(pg, 44, PAGER_OP_INVALIDATE, 0, 4 * PG, 0), OK);
        thread_sleep_ms(1);
    }
    for (unsigned i = 0; i < 2; i++) {
        __atomic_store_n(&l[i].stop, true, __ATOMIC_RELEASE);
        thread_join(l[i].t);
        KT_EQ(l[i].st, OK);
        KT_ASSERT(l[i].reads > 0);
    }
    KT_EQ(gp_stop(&g), OK);

    /* The last version is what the next access sees. */
    KT_EQ(pager_op(pg, 44, PAGER_OP_INVALIDATE, 0, 4 * PG, 0), OK);
    KT_EQ(supply_gen(pg, 44, 0, 4, 99), OK);
    for (uint64_t p = 0; p < 4; p++) {
        KT_EQ(vmo_read(v, p * PG + 8, &w, 8), OK);
        KT_EQ(w, gword(44, p, 99));
    }
    KT_EQ(pager_resident(pg), 4);
    uas_free(&us);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* PAGER_OP_SET_SIZE: a shrink drops the pages past the end (from the
 * mappings too) and a fault, read or supply there meets the end; growing
 * adds absent pages; a reader waiting past the new end is answered and
 * meets it, one whose request crosses it waits on for the pages below; a
 * ring-3 thread waiting past it dies of a bus error. */
KTEST(pager_cache_shrink)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_budget(j, 64, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 45, 4 * PG, &v), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0, w;
    KT_EQ(aspace_map(as, v, 0, 4 * PG, ASPACE_READ, &a), OK);
    KT_EQ(supply(pg, 45, 0, 4), OK);
    KT_EQ(aspace_fault(as, a + 3 * PG, ASPACE_READ), OK);
    KT_EQ(pager_op(pg, 45, PAGER_OP_SET_SIZE, 2 * PG, 0, 0), OK);
    KT_EQ(vmo_size(v), 2 * PG);
    KT_EQ(aspace_pte(as, a + 3 * PG), 0);
    KT_EQ(vmo_committed(v), 2 * PG);
    KT_EQ(pager_resident(pg), 2);
    KT_EQ(job_used(j, JOB_LIMIT_MSG_BYTES), 2 * 16);
    KT_EQ(aspace_fault(as, a + 3 * PG, ASPACE_READ), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_read(v, 3 * PG, &w, 8), ERR_OUT_OF_RANGE);
    KT_EQ(supply(pg, 45, 3, 1), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_read(v, PG, &w, 8), OK);
    KT_EQ(w, word(45, 1));

    /* Growing: the new pages are absent, asked for when read; a shrink
     * then answers the reader, who meets the end. */
    KT_EQ(pager_op(pg, 45, PAGER_OP_SET_SIZE, 4 * PG, 0, 0), OK);
    struct rd r;
    struct pager_request q;
    rd_start(&r, v, 3 * PG, 8);
    next(pg, 0, &q);
    is_read(&q, 45, 3, 1);
    KT_EQ(pager_op(pg, 45, PAGER_OP_SET_SIZE, 2 * PG, 0, 0), OK);
    KT_EQ(rd_end(&r, 0), ERR_OUT_OF_RANGE);
    none_queued(pg);

    /* A request crossing the new end is cut there and waits for the rest. */
    KT_EQ(pager_op(pg, 45, PAGER_OP_SET_SIZE, 4 * PG, 0, 0), OK);
    KT_EQ(pager_op(pg, 45, PAGER_OP_INVALIDATE, PG, PG, 0), OK);
    rd_start(&r, v, PG, 8);
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    KT_EQ(object_wait_one(pager_kobject(pg), SIG_READABLE, deadline, NULL), OK);
    KT_EQ(pager_op(pg, 45, PAGER_OP_SET_SIZE, 2 * PG, 0, 0), OK);
    next(pg, 0, &q);
    is_read(&q, 45, 1, 1);
    thread_sleep_ms(5);
    KT_ASSERT(rd_waiting(&r));
    KT_EQ(supply(pg, 45, 1, 1), OK);
    KT_EQ(rd_end(&r, 45), OK);
    none_queued(pg);
    KT_EQ(aspace_unmap(as, a, 4 * PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);

    struct ufix f;
    ufix_start(&f, ANSWER, 46, "cache-shrunk", 3);
    next(f.pg, 0, &q);
    is_read(&q, 46, 3, 1);
    KT_EQ(pager_op(f.pg, 46, PAGER_OP_SET_SIZE, 2 * PG, 0, 0), OK);
    ufix_killed(&f, "bus error: page past the end of a shrunk VMO");
    ufix_end(&f);
}

/* Memory pressure: a job that can't pay for another page, or another
 * record, makes the supply evict the pager's oldest pages rather than
 * fail; with nothing left to evict it fails ERR_NO_MEMORY. */
KTEST(pager_cache_pressure)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_budget(j, 1u << 20, ANSWER);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 47, 16 * PG, &v), OK);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 2 + 4), OK);   /* the tables and 4 pages */
    KT_EQ(supply(pg, 47, 0, 16), OK);
    KT_EQ(pager_resident(pg), 4);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 2 + 4);
    for (uint64_t i = 0; i < 16; i++)
        KT_EQ(vmo_page_phys(v, i * PG) != 0, i >= 12);

    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, JOB_NO_LIMIT), OK);
    KT_EQ(job_set_limit(j, JOB_LIMIT_MSG_BYTES, 6 * 16), OK);   /* 6 records */
    KT_EQ(supply(pg, 47, 0, 8), OK);
    KT_EQ(pager_resident(pg), 6);
    for (uint64_t i = 0; i < 16; i++)
        KT_EQ(vmo_page_phys(v, i * PG) != 0, i >= 2 && i < 8);
    KT_EQ(job_set_limit(j, JOB_LIMIT_MSG_BYTES, JOB_NO_LIMIT), OK);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);

    j = kt_fresh_job();
    pg = pg_budget(j, 1u << 20, ANSWER);
    KT_EQ(pager_create_vmo(pg, 48, PG, &v), OK);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 0), OK);
    KT_EQ(supply(pg, 48, 0, 1), ERR_NO_MEMORY);
    KT_EQ(pager_resident(pg), 0);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, JOB_NO_LIMIT), OK);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* Four readers sweep two VMOs at random, against a pager thread, with
 * the budget a third of what they read: every byte right, the cache and
 * the job never past the budget. */
KTEST(pager_cache_sweep)
{
    enum { READERS = 4, PAGES = 192, BUDGET = 128 };
    struct job *j = kt_fresh_job();
    struct gpager g;
    gp_start(&g, pg_budget(j, BUDGET, ANSWER), 0);
    struct vmo *v[2];
    KT_EQ(pager_create_vmo(g.pg, 100, PAGES * PG, &v[0]), OK);
    KT_EQ(pager_create_vmo(g.pg, 101, PAGES * PG, &v[1]), OK);
    static struct sweeper s[READERS];
    struct thread *rt[READERS];
    for (unsigned i = 0; i < READERS; i++) {
        s[i] = (struct sweeper){ .v = v[i % 2], .key = 100 + i % 2, .pages = PAGES,
                                 .seed = 0x7a3 + i };
        rt[i] = thread_create("pager-sweep", sweeper_main, &s[i], PRIO_DEFAULT);
    }
    uint64_t max_resident = 0, max_job = 0;
    for (unsigned i = 0; i < READERS; i++) {
        while (!__atomic_load_n(&s[i].done, __ATOMIC_ACQUIRE)) {
            uint64_t r = pager_resident(g.pg), jp = job_used(j, JOB_LIMIT_PAGES);
            max_resident = r > max_resident ? r : max_resident;
            max_job = jp > max_job ? jp : max_job;
            thread_sleep_ms(1);
        }
        thread_join(rt[i]);
        KT_EQ(s[i].st, OK);
    }
    KT_ASSERT(max_resident <= BUDGET);
    KT_ASSERT(max_job <= BUDGET + 2 * 2);   /* and each VMO's mid and leaf table */
    put(v[0]);
    put(v[1]);
    KT_EQ(gp_stop(&g), OK);
    pg_close(g.pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- verified pages (PK4) ----------------------------------------------------- */

/* A program from /data: a pager VMO sealed with its pages' hashes
 * (vmo_make_exec_verified, <jam/vmo_verify.h>). Its page 0 is code (uprog
 * at the start), page 1 data (its words); the table is computed from
 * those approved bytes, and a test changes a byte of `file` afterwards to
 * play a file changed since its approval. A ring-3 thread runs the code
 * from the VMO itself, mapped executable at V_CODE. */

#define VF_PAGES 2
#define V_CODE   0x30000000ull   /* a verified VMO, read and execute */

struct vfix {
    struct job          *j;
    struct pager        *pg;
    struct vmo          *v;       /* our own reference to the program's VMO */
    struct handle_table  t;       /* job j's: the VMO's handle, the root's, the table's */
    handle_t             h, root, table, x;   /* x: the VMO's handle after the call */
    uint8_t             *file;    /* what the pager supplies, VF_PAGES pages */
    uint64_t             key;
};

/* A table VMO for `pages` pages of bytes. */
static struct vmo *table_of(const uint8_t *bytes, uint64_t pages)
{
    struct vmo *tv;
    KT_EQ(vmo_create(pages * VMO_VERIFIED_HASH_BYTES, 0, &tv), OK);
    for (uint64_t i = 0; i < pages; i++) {
        uint8_t d[SHA256_BYTES];
        sha256(bytes + i * PG, PG, d);
        KT_EQ(vmo_write(tv, i * VMO_VERIFIED_HASH_BYTES, d, sizeof(d)), OK);
    }
    return tv;
}

/* A handle in t to obj (the caller's reference). */
static handle_t insert(struct handle_table *t, struct kobject *obj, rights_t rights)
{
    struct khandle kh = khandle_from_new(obj, rights);
    handle_t h;
    KT_EQ(handle_insert(t, &kh, &h), OK);
    return h;
}

/* Everything but the call: the program's VMO (handle h), the root with
 * RIGHT_ROOT_VMEX, the table of the approved bytes. */
static void vfix_new(struct vfix *f, uint64_t key)
{
    f->j = kt_fresh_job();
    f->pg = pg_new(f->j, ANSWER);
    f->key = key;
    KT_EQ(pager_create_vmo(f->pg, key, VF_PAGES * PG, &f->v), OK);
    f->file = kzalloc(VF_PAGES * PG);
    KT_ASSERT(f->file);
    memcpy(f->file, uprog, sizeof(uprog));
    for (uint64_t i = 0; i < PG / 8; i++)
        ((uint64_t *)(f->file + PG))[i] = word(key, 1);
    handle_table_init(&f->t);
    f->t.job = f->j;
    kobject_ref(vmo_kobject(f->v));
    f->h = insert(&f->t, vmo_kobject(f->v), PAGER_VMO_RIGHTS);
    struct kobject *root = resource_root();
    kobject_ref(root);
    f->root = insert(&f->t, root, RIGHTS_BASIC | RIGHT_ROOT_VMEX);
    f->table = insert(&f->t, vmo_kobject(table_of(f->file, VF_PAGES)), RIGHTS_BASIC | RIGHT_READ);
}

static void vfix_start(struct vfix *f, uint64_t key)
{
    vfix_new(f, key);
    KT_EQ(sys_vmo_make_exec_verified(&f->t, f->h, f->root, f->table, 0, &f->x), OK);
}

static void vfix_end(struct vfix *f)
{
    handle_table_destroy(&f->t);
    put(f->v);
    pg_close(f->pg);
    kfree(f->file);
    kt_job_is_empty(f->j);   /* the table's pages went with the VMO */
    job_unref(f->j);
}

/* Supply pages [first, first + n) of the file as it is now. */
static status_t supply_file(struct vfix *f, uint64_t first, uint64_t n)
{
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)(f->file + first * PG), .user = false };
    return pager_supply(f->pg, f->key, first * PG, &src, n * PG);
}

/* The address space: the VMO at U_FILE (read) as uas_new maps it, and at
 * V_CODE (read, execute). */
static void vas_new(struct uas *u, struct vmo *prog)
{
    uas_new(u, prog, VF_PAGES * PG);
    uint64_t a = V_CODE;
    KT_EQ(aspace_map(u->as, prog, 0, VF_PAGES * PG, ASPACE_READ | ASPACE_EXEC | ASPACE_FIXED, &a),
          OK);
}

static void vas_free(struct uas *u)
{
    KT_EQ(aspace_unmap(u->as, V_CODE, VF_PAGES * PG), OK);
    uas_free(u);
}

/* ut_main, but the code is the verified VMO's (at V_CODE). */
static void vt_main(void *arg)
{
    struct ut *u = arg;
    struct thread *t = current_thread();
    KT_ASSERT(fpu_ustate_alloc(t) == OK);
    aspace_ref(u->as);   /* the thread's, dropped when it is reaped */
    irq_disable();
    t->aspace = u->as;
    aspace_switch(NULL, u->as);
    irq_enable();
    arch_enter_user(V_CODE, U_DATA + PG, u->store, u->load);
}

static void vt_start(struct ut *u, struct uas *as, const char *name, uint64_t load)
{
    u->as = as->as;
    u->load = load;
    u->store = U_DATA;
    u->t = thread_create(name, vt_main, u, PRIO_DEFAULT);
}

/* A program whose pages match runs: its code page and its data page come
 * in one request, both pass, it stores its data word, nobody is killed.
 * The new handle may execute and nothing more; the old one is gone. */
KTEST(pager_verified_runs)
{
    struct vfix f;
    vfix_start(&f, 40);
    struct handle_info hi;
    KT_EQ(handle_info(&f.t, f.h, &hi), ERR_BAD_HANDLE);
    KT_EQ(handle_info(&f.t, f.x, &hi), OK);
    KT_EQ(hi.rights, PAGER_VMO_RIGHTS | RIGHT_EXEC);
    struct uas us;
    vas_new(&us, f.v);
    uint64_t faults0 = __atomic_load_n(&user_faults, __ATOMIC_RELAXED);
    struct ut u;
    vt_start(&u, &us, "pk4-runs", V_CODE + PG + 8);
    struct pager_request q;
    next(f.pg, 0, &q);
    is_read(&q, 40, 0, VF_PAGES);   /* the code fetch, and the data after it */
    KT_EQ(supply_file(&f, 0, VF_PAGES), OK);
    wait_stored(&us, 0, word(40, 1));
    ut_stop(&u);
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), faults0);
    KT_EQ(vmo_committed(f.v), VF_PAGES * PG);
    none_queued(f.pg);
    vas_free(&us);
    vfix_end(&f);
}

/* The file changed after its approval (one bit of the data page): the
 * code page still comes and runs, the data page never does, and the
 * thread that loads it is killed with the reason. A supply of the
 * approved bytes is taken after all. */
KTEST(pager_verified_bad_page)
{
    struct vfix f;
    vfix_start(&f, 41);
    f.file[PG + 100] ^= 1;
    struct uas us;
    vas_new(&us, f.v);
    uint64_t log0 = klog_head(), faults0 = __atomic_load_n(&user_faults, __ATOMIC_RELAXED);
    struct ut u;
    vt_start(&u, &us, "pk4-changed", V_CODE + PG + 8);
    struct pager_request q;
    next(f.pg, 0, &q);
    is_read(&q, 41, 0, VF_PAGES);
    KT_EQ(supply_file(&f, 0, VF_PAGES), ERR_IO);   /* page 0 in, page 1 refused */
    KT_ASSERT(vmo_page_phys(f.v, 0));
    KT_EQ(vmo_page_phys(f.v, PG), 0);
    next(f.pg, 0, &q);   /* its code ran: now the load */
    is_read(&q, 41, 1, 1);
    KT_EQ(supply_file(&f, 1, 1), ERR_IO);
    thread_join(u.t);
    KT_ASSERT(killed_for(log0, "pk4-changed",
                         "page fault: a page doesn't match the approved program"));
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), faults0 + 1);
    KT_EQ(stored(&us, 0), 0);
    KT_EQ(vmo_page_phys(f.v, PG), 0);
    none_queued(f.pg);
    f.file[PG + 100] ^= 1;
    KT_EQ(supply_file(&f, 1, 1), OK);
    KT_ASSERT(vmo_page_phys(f.v, PG));
    vas_free(&us);
    vfix_end(&f);
}

/* A copy from a page that doesn't match is ERR_IO, and so is vmo_read of
 * it; a copy from a good page of the same request goes on (its page is
 * there when the request fails). */
KTEST(pager_verified_copies)
{
    struct vfix f;
    vfix_start(&f, 42);
    f.file[PG] ^= 0x80;
    struct uas us;
    vas_new(&us, f.v);
    struct cp c;
    struct pager_request q;

    cp_start(&c, us.as, V_CODE + PG + 16, CP_FROM, NULL);
    next(f.pg, 0, &q);
    is_read(&q, 42, 1, 1);
    KT_EQ(supply_file(&f, 1, 1), ERR_IO);
    KT_EQ(cp_end(&c), ERR_IO);

    struct rd r;
    rd_start(&r, f.v, PG, 8);
    next(f.pg, 0, &q);
    is_read(&q, 42, 1, 1);
    KT_EQ(supply_file(&f, 1, 1), ERR_IO);
    KT_EQ(rd_end(&r, 0), ERR_IO);

    cp_start(&c, us.as, V_CODE, CP_FROM, NULL);
    next(f.pg, 0, &q);
    is_read(&q, 42, 0, VF_PAGES);
    KT_EQ(supply_file(&f, 0, VF_PAGES), ERR_IO);   /* page 1 still bad */
    KT_EQ(cp_end(&c), OK);
    KT_EQ(memcmp(&c.word, uprog, sizeof(uprog)), 0);
    none_queued(f.pg);
    vas_free(&us);
    vfix_end(&f);
}

/* Pages present before the call came unchecked: the call drops them. A
 * supply that copied its page before the seal and publishes after it
 * (unchecked) is matched under the VMO lock: a bad page is refused, a
 * good one taken. A page past the table never matches. */
KTEST(pager_verified_seal_and_late_publish)
{
    struct vfix f;
    vfix_new(&f, 43);
    KT_EQ(supply_file(&f, 0, VF_PAGES), OK);   /* before the call: not checked */
    KT_EQ(vmo_committed(f.v), VF_PAGES * PG);
    KT_EQ(sys_vmo_make_exec_verified(&f.t, f.h, f.root, f.table, 0, &f.x), OK);
    KT_EQ(vmo_committed(f.v), 0);
    struct vmo_verify *vf = vmo_verifier(f.v);
    KT_ASSERT(vf);

    struct page *p = pmm_alloc_pages(0, 0);
    KT_ASSERT(p);
    memcpy(page_to_virt(p), f.file + PG, PG);
    ((uint8_t *)page_to_virt(p))[7] ^= 1;
    KT_EQ(pager_publish_page(f.pg, f.v, 1, p, false), ERR_IO);
    KT_EQ(vmo_page_phys(f.v, PG), 0);
    p = pmm_alloc_pages(0, 0);
    KT_ASSERT(p);
    memcpy(page_to_virt(p), f.file + PG, PG);
    KT_EQ(pager_publish_page(f.pg, f.v, 1, p, false), OK);
    KT_ASSERT(vmo_page_phys(f.v, PG));

    KT_ASSERT(vmo_verify_match(vf, 0, f.file));
    KT_ASSERT(!vmo_verify_match(vf, VF_PAGES, f.file));   /* no entry for it */
    vfix_end(&f);
}

/* A pager VMO of the refusals test, its handle in t (key: the next one). */
static handle_t refused_vmo(struct vfix *f, uint64_t key, uint64_t size, struct vmo **v)
{
    KT_EQ(pager_create_vmo(f->pg, key, size, v), OK);
    return insert(&f->t, vmo_kobject(*v), PAGER_VMO_RIGHTS);
}

/* Is h gone from t? */
static bool gone(struct handle_table *t, handle_t h)
{
    struct handle_info hi;
    return handle_info(t, h, &hi) == ERR_BAD_HANDLE;
}

/* Each refusal, with the VMO's handle gone whatever the status and
 * nothing left charged: flags, no power, not a pager VMO, the table's
 * type, rights and length, a VMO too big, a second handle, a mapping, a
 * detached VMO, and a VMO verified already. */
KTEST(pager_verified_refusals)
{
    struct vfix f;
    vfix_new(&f, 44);
    struct handle_table *t = &f.t;
    handle_t x, h, ev, ro, dup;
    struct vmo *v;
    KT_EQ(sys_event_create(t, &ev), OK);
    KT_EQ(handle_duplicate(t, f.table, RIGHTS_BASIC, &ro), OK);
    struct kobject *root = resource_root();
    kobject_ref(root);
    handle_t noroot = insert(t, root, ROOT_RIGHTS & ~RIGHT_ROOT_VMEX);

    h = refused_vmo(&f, 100, PG, &v);
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, f.table, 1, &x), ERR_INVALID_ARGS);
    KT_ASSERT(gone(t, h));
    h = refused_vmo(&f, 101, PG, &v);
    KT_EQ(sys_vmo_make_exec_verified(t, h, noroot, f.table, 0, &x), ERR_ACCESS_DENIED);
    KT_ASSERT(gone(t, h));
    KT_EQ(sys_vmo_create(t, PG, 0, &h), OK);
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, f.table, 0, &x), ERR_NOT_SUPPORTED);
    KT_ASSERT(gone(t, h));
    h = refused_vmo(&f, 102, PG, &v);
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, ev, 0, &x), ERR_WRONG_TYPE);
    KT_ASSERT(gone(t, h));
    h = refused_vmo(&f, 103, PG, &v);
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, ro, 0, &x), ERR_ACCESS_DENIED);
    KT_ASSERT(gone(t, h));
    h = refused_vmo(&f, 104, (PG / VMO_VERIFIED_HASH_BYTES + 1) * PG, &v);   /* 129 pages */
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, f.table, 0, &x), ERR_INVALID_ARGS);
    KT_ASSERT(gone(t, h));
    h = refused_vmo(&f, 105, VMO_VERIFIED_MAX + PG, &v);
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, f.table, 0, &x), ERR_OUT_OF_RANGE);
    KT_ASSERT(gone(t, h));

    h = refused_vmo(&f, 106, PG, &v);   /* a second handle */
    KT_EQ(handle_duplicate(t, h, RIGHT_SAME, &dup), OK);
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, f.table, 0, &x), ERR_BAD_STATE);
    KT_ASSERT(gone(t, h));
    KT_EQ(handle_close(t, dup), OK);

    h = refused_vmo(&f, 107, PG, &v);   /* mapped */
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_READ, &a), OK);
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, f.table, 0, &x), ERR_BAD_STATE);
    KT_ASSERT(gone(t, h));
    KT_EQ(aspace_unmap(as, a, PG), OK);
    aspace_unref(as);

    h = refused_vmo(&f, 108, PG, &v);   /* detached */
    KT_EQ(pager_op(f.pg, 108, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, f.table, 0, &x), ERR_BAD_STATE);
    KT_ASSERT(gone(t, h));

    KT_EQ(sys_vmo_make_exec_verified(t, f.h, f.root, f.table, 0, &h), OK);
    KT_EQ(sys_vmo_make_exec_verified(t, h, f.root, f.table, 0, &x), ERR_BAD_STATE);
    KT_ASSERT(gone(t, h));
    vfix_end(&f);   /* every table freed, every page uncharged */
}

/* Read page `page` of the verified VMO with a reader thread, answering its
 * request with the file as it is now: the supply's status, and the
 * reader's. */
static void vf_refetch(struct vfix *f, uint64_t page, status_t supplied, status_t read)
{
    struct rd r;
    rd_start(&r, f->v, page * PG, 8);
    struct pager_request q;
    next(f->pg, 0, &q);
    is_read(&q, f->key, page, 1);
    KT_EQ(supply_file(f, page, 1), supplied);
    KT_EQ(rd_end(&r, 0), read);
}

/* A verified VMO's pages stay checked across the cache: a page evicted
 * (by the cache, here under the job's pressure, or by
 * PAGER_OP_INVALIDATE) and fetched again is matched again, so a file
 * changed since its approval is refused at the refetch and the page stays
 * absent; the approved bytes are taken again. */
KTEST(pager_cache_verified_refetch)
{
    struct vfix f;
    vfix_start(&f, 49);
    KT_EQ(supply_file(&f, 0, VF_PAGES), OK);
    KT_EQ(vmo_committed(f.v), VF_PAGES * PG);

    /* Another VMO of the pager needs a page the job can't pay: the oldest,
     * the verified VMO's page 0, is evicted for it. */
    struct vmo *other;
    KT_EQ(pager_create_vmo(f.pg, 50, PG, &other), OK);
    KT_EQ(job_set_limit(f.j, JOB_LIMIT_PAGES, job_used(f.j, JOB_LIMIT_PAGES) + 2), OK);
    KT_EQ(supply(f.pg, 50, 0, 1), OK);   /* its mid and leaf tables fit, its page didn't */
    KT_EQ(job_set_limit(f.j, JOB_LIMIT_PAGES, JOB_NO_LIMIT), OK);
    KT_EQ(vmo_page_phys(f.v, 0), 0);
    KT_ASSERT(vmo_page_phys(f.v, PG) != 0);

    f.file[100] ^= 1;   /* the file changed since its approval */
    vf_refetch(&f, 0, ERR_IO, ERR_IO);
    KT_EQ(vmo_page_phys(f.v, 0), 0);
    f.file[100] ^= 1;
    vf_refetch(&f, 0, OK, OK);
    uint8_t b[8];
    KT_EQ(vmo_read(f.v, 96, b, 8), OK);
    KT_EQ(memcmp(b, f.file + 96, 8), 0);

    /* The same after PAGER_OP_INVALIDATE. */
    KT_EQ(pager_op(f.pg, f.key, PAGER_OP_INVALIDATE, PG, PG, 0), OK);
    KT_EQ(vmo_page_phys(f.v, PG), 0);
    f.file[PG + 8] ^= 1;
    vf_refetch(&f, 1, ERR_IO, ERR_IO);
    KT_EQ(vmo_page_phys(f.v, PG), 0);
    f.file[PG + 8] ^= 1;
    vf_refetch(&f, 1, OK, OK);
    uint64_t w;
    KT_EQ(vmo_read(f.v, PG + 8, &w, 8), OK);
    KT_EQ(w, word(49, 1));

    /* Its size is sealed: SET_SIZE is refused. */
    KT_EQ(pager_op(f.pg, f.key, PAGER_OP_SET_SIZE, PG, 0, 0), ERR_BAD_STATE);
    put(other);
    vfix_end(&f);
}
