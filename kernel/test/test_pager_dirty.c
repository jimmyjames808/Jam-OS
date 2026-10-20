/* Writable pager VMOs (docs/WRITEMAP-PLAN.md, tracks WK1, WK2 and WK3;
 * <jam/pager.h>'s "Writable pager VMOs", "Write-back" and "The dirty
 * limit", vmo.c's): the page states, the write fault, the lists and
 * counts, the hold, PAGER_VMO_FIND and the GONE takeover, pager_dirty's
 * look, pager_write, the clone mode, write-back, the dirty limit's waits
 * and vmo_sync.
 *
 *   pager_dirty_store_marks
 *       a read installs a clean page read-only in a writable mapping; a
 *       store (a fault, or a kernel copy to user memory) makes it dirty and
 *       writable; protect never gives a dirty page's entry the write bit
 *       back (the next store faults and finds it dirty); the counts move,
 *       the budget's doesn't; no clean page is ever writable.
 *   pager_dirty_ring3_store
 *       a ring-3 store to a page mapped read-only because it is clean
 *       faults (present, write) and makes it dirty.
 *   pager_dirty_never_evicted
 *       supplies past the budget evict the oldest clean pages only; dirty
 *       pages stay mapped and counted; an invalidation drops clean pages
 *       only.
 *   pager_dirty_popped_then_stored
 *       a store to the page an evictor has just popped (a hook in the
 *       window): the evictor leaves it, dirty, counted again.
 *   pager_dirty_evict_storm
 *       a sweeper faulting pages in under a small budget (every supply
 *       evicts) while stores dirty pages: no store is ever lost to an
 *       eviction (an evictor that popped a page a store then dirtied
 *       leaves it).
 *   pager_dirty_hold_and_find
 *       a VMO with dirty pages outlives its handles and mappings (no
 *       GONE), FIND hands it out with its bytes; a shrink drops dirty pages
 *       past the end; once none is left the VMO goes and its GONE comes;
 *       a create of that key takes the GONE over (delivered or queued); a
 *       detach drops dirty pages (lost) and ends the hold.
 *   pager_dirty_dying_key
 *       a create of a key whose VMO is being destroyed (no reference left,
 *       its pages freed, its link not yet told: DBG_PAGER_VMO_DYING) takes
 *       the key over instead of ERR_ALREADY_EXISTS; no GONE comes for the
 *       old one, the new one serves the key and its GONE comes in turn.
 *   pager_dirty_store_waits
 *       a store to an absent page waits for it like a read (the region
 *       lock free) and dirties it when it arrives; a store cancelled while
 *       it waits leaves the page clean and every count as it was.
 *   pager_dirty_peek
 *       pager_dirty without BEGIN: the first run of dirty pages in the
 *       range, its bytes at their place, its age; nothing changes; every
 *       refusal.
 *   pager_dirty_write
 *       pager_write: bytes into present pages whatever their state (states
 *       kept), absent pages left absent, any byte range; refusals.
 *   pager_dirty_refusals
 *       a budget under PAGER_WRITABLE_MIN_PAGES makes none; a writable VMO
 *       refuses vmo_write, resize, commit, decommit, kernel mappings,
 *       seals (never executable or verified); handles: RIGHT_WRITE only on
 *       a writable VMO's (FIND's too), the calls need RIGHT_WRITE on the
 *       pager.
 *   pager_dirty_fork
 *       a writable mapping of a writable VMO is always cloned shared (an
 *       explicit VMAR_CLONE_NONE too); the child gets no entry at the
 *       clone, its faults follow the states (a clean page read-only, a
 *       dirty one writable), its store dirties the parent's page; no
 *       clean page writable in either; a write-back protects in both, and
 *       the child's store then dirties the page again.
 *   pager_dirty_begin_protects
 *       BEGIN with VMAR_MAX_OVERLAP mappings of every page in each of four
 *       address spaces: runs cut to the batch budget (vmo_batch_cost_most
 *       within VMO_BATCH_BUDGET), every entry of a begun page read-only
 *       in every mapping, the rest untouched; a store after it dirties
 *       its page again, writable in that mapping only; WRITTEN.
 *   pager_dirty_store_storm
 *       eight threads storing counters (two to a page) while a pager
 *       thread loops begin, copy, written: the last image holds every
 *       thread's last count.
 *   pager_dirty_written
 *       WRITTEN: a page not stored to since its begin clean, one stored to
 *       dirty (and writable); BEGIN skips writing pages; an error leaves
 *       the pages clean and is reported once (ERR_IO, ERR_NO_SPACE);
 *       refusals; clean again, the hold goes and the GONE comes.
 *   pager_dirty_failed_kept
 *       a write-back that fails leaves its pages clean but kept: PEEK
 *       copies them with their stores (a filesystem's read shows what the
 *       mappings show), flags 0 and BEGIN don't; a store makes one dirty
 *       and its write-back the disk's again; an invalidation drops one.
 *   pager_dirty_again
 *       a pager dies between begin and written: AGAIN makes the writing
 *       pages dirty and gives the request again, unanswered until the
 *       successor's write-back; a BEGIN whose copy fails begins nothing.
 *   pager_dirty_requests
 *       what a write-back request needs: a page writing when it was made
 *       that write-back's end, one writing and dirty a later one (the
 *       request given to the pager again), a dirty one a write-back begun
 *       after it, a page dirtied after it nothing; an error answers it;
 *       a shrink's dropped pages are done; a detach fails it.
 *   pager_dirty_background
 *       past half the dirty limit a store queues one request for the
 *       oldest dirty run, one at a time; a WRITTEN that leaves the pager
 *       past half asks again.
 *   pager_dirty_oldest
 *       OLDEST: the run around the oldest dirty page whatever its VMO, its
 *       age, a minimum age, BEGIN one run after another, how far back a
 *       run reaches.
 *   pager_dirty_limit_waits
 *       at the dirty limit a store to a clean page (a fault, a user copy)
 *       waits without the region lock, joining the background request
 *       (no second one); a load, a store to a dirty page and one to a
 *       page being written back don't; a begin doesn't end the wait, the
 *       pager's WRITTEN does, and the stores go on.
 *   pager_dirty_limit_deadline
 *       a store at the limit with no request to join (the background one
 *       past its deadline) makes its own and goes on when it is answered;
 *       a pager that serves reads but never writes back: a kernel fault
 *       ends ERR_TIMED_OUT, a user copy ERR_IO, vmo_sync ERR_TIMED_OUT, a
 *       ring-3 store kills its thread ("the file's pager didn't answer"),
 *       each after the answer time and not before, while a reader of
 *       another VMO of the pager faults its pages in at once (evicting).
 *   pager_dirty_limit_kill
 *       a kill ends every wait at the limit: a kernel fault, a user copy
 *       and vmo_sync end ERR_CANCELED, a ring-3 thread dies without a
 *       fault of its own; nothing dirtied.
 *   pager_dirty_sync
 *       vmo_sync: refusals, OK at once with nothing dirty or for another
 *       VMO; exact (a store made before it but after a write-back began
 *       keeps it waiting past that write-back's end, a store made after it
 *       doesn't); an error reported once, by the waiting call only (one
 *       kept from a write-back by age too); without the wait the request
 *       is only made; a range of two chunks asks for one at a time.
 *   pager_dirty_errseq
 *       write-back errors once to each mapper (WR2's errseq): samples
 *       (refusals, 0 with no error and for another VMO); two mappers each
 *       hear of an error once, at a sync that waits; a mapping made
 *       before anyone heard hears of it too, one made after doesn't; two
 *       errors before a look are one; a copied sample hears again; a
 *       sync whose own write-back fails hears of it.
 *   pager_dirty_closed
 *       a detach fails a waiting vmo_sync ERR_PEER_CLOSED (a later one at
 *       once); the pager's close fails a store waiting at the limit
 *       ERR_PEER_CLOSED.
 *
 * The pager is the test itself (supplying before the faults that need the
 * pages) or a kernel thread serving requests (struct kpager). Each test
 * ends with the pager's handle closed (pg_close), which detaches every
 * VMO: dirty pages go and the holds with them. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/dbghook.h>
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
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/usercopy.h>
#include <jam/vmo.h>
#include <jam/vmo_verify.h>

#define PG     PAGE_SIZE
#define ANSWER (600 * NS_PER_S)
#define RW     (ASPACE_READ | ASPACE_WRITE)
#define PTE_W  (1ull << 1)
#define DIRTY  PAGER_PAGE_DIRTY
/* The smallest budget for a writable VMO, and the VMO's two tables
 * (a mid and a leaf table), which the budget holds too: eight pages. */
#define MIN_BUDGET (PAGER_WRITABLE_MIN_PAGES + 2)

/* ---- helpers ------------------------------------------------------------- */

static struct pager *pg_new(struct job *j, uint64_t max_pages)
{
    struct pager *pg;
    KT_EQ(pager_create(j, max_pages, ANSWER, &pg), OK);
    return pg;
}

/* Close the pager's (only) handle: every VMO detached, every GONE taken. */
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

/* A writable VMO of `pages` pages under key. */
static struct vmo *wvmo(struct pager *pg, uint64_t key, uint64_t pages)
{
    struct vmo *v;
    KT_EQ(pager_create_vmo_flags(pg, key, pages * PG, PAGER_VMO_WRITABLE, &v), OK);
    KT_ASSERT(vmo_pager_writable(v));
    return v;
}

/* The word every 8 bytes of page `page` of key's file hold. */
static uint64_t word(uint64_t key, uint64_t page)
{
    return key << 32 | page | 0x5a00000000000000ull;
}

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
static void next(struct pager *pg, struct pager_request *r)
{
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    for (;;) {
        status_t st = pager_next(pg, 0, r);
        if (st == OK)
            return;
        KT_EQ(st, ERR_SHOULD_WAIT);
        KT_EQ(object_wait_one(pager_kobject(pg), SIG_READABLE, deadline, NULL), OK);
    }
}

static void none_queued(struct pager *pg)
{
    struct pager_request r;
    KT_EQ(pager_next(pg, 0, &r), ERR_SHOULD_WAIT);
}

static uint64_t map(struct aspace *as, struct vmo *v, uint64_t pages, unsigned flags)
{
    uint64_t a = 0;
    KT_EQ(aspace_map(as, v, 0, pages * PG, flags, &a), OK);
    return a;
}

/* Run the current thread in address space `as` (NULL: out of `was`), as a
 * system call's copies do. */
static void enter(struct aspace *as, struct aspace *was)
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

/* A kernel copy to user memory: a store. */
static status_t uput(struct aspace *as, uint64_t va, uint64_t w)
{
    enter(as, NULL);
    status_t st = copy_to_user(va, &w, 8);
    enter(NULL, as);
    return st;
}

static uint64_t uword(struct aspace *as, uint64_t va)
{
    uint64_t w = ~0ull;
    enter(as, NULL);
    KT_EQ(copy_from_user(&w, va, 8), OK);
    enter(NULL, as);
    return w;
}

static uint64_t vword(struct vmo *v, uint64_t off)
{
    uint64_t w = 0;
    KT_EQ(vmo_read(v, off, &w, 8), OK);
    return w;
}

static bool pte_w(struct aspace *as, uint64_t va)
{
    return (aspace_pte(as, va) & PTE_W) != 0;
}

/* No entry of v's pages [0, pages) mapped at a in as is writable unless
 * its page is dirty: the rule every way to an entry keeps. */
static void no_clean_writable(struct aspace *as, struct vmo *v, uint64_t a, uint64_t pages)
{
    for (uint64_t i = 0; i < pages; i++)
        if (pte_w(as, a + i * PG))
            KT_ASSERT(vmo_pager_page_state(v, i) & DIRTY);
}

/* A kernel thread serving every request with the file's words. */
struct kpager {
    struct pager  *pg;
    bool           stop;     /* atomic */
    uint64_t       served;   /* atomic */
    struct thread *t;
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
        if (q.kind == PAGER_REQ_WRITEBACK)
            continue;   /* (left unanswered: the background one, which these tests don't serve) */
        st = supply(k->pg, q.key, q.offset / PG, q.len / PG);
        KT_ASSERT(st == OK || st == ERR_NOT_FOUND || st == ERR_OUT_OF_RANGE);   /* (a shrink) */
        __atomic_add_fetch(&k->served, 1, __ATOMIC_RELAXED);
    }
}

static void kp_start(struct kpager *k, struct pager *pg)
{
    *k = (struct kpager){ .pg = pg };
    k->t = thread_create("wk-pager", kpager_main, k, PRIO_DEFAULT);
}

static void kp_stop(struct kpager *k)
{
    __atomic_store_n(&k->stop, true, __ATOMIC_RELEASE);
    thread_join(k->t);
}

/* A kernel thread in aspace_fault (a store or a load). */
struct kf {
    struct aspace *as;
    uint64_t       addr;
    unsigned       access;
    status_t       st;   /* ERR_INTERNAL until it returns */
    struct thread *t;
};

static void kf_main(void *arg)
{
    struct kf *k = arg;
    __atomic_store_n(&k->st, aspace_fault(k->as, k->addr, k->access), __ATOMIC_RELEASE);
}

static void kf_start(struct kf *k, struct aspace *as, uint64_t addr, unsigned access)
{
    *k = (struct kf){ .as = as, .addr = addr, .access = access, .st = ERR_INTERNAL };
    k->t = thread_create("wk-fault", kf_main, k, PRIO_DEFAULT);
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

/* ---- the states ----------------------------------------------------------- */

KTEST(pager_dirty_store_marks)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 1, 4);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 4, RW);
    KT_EQ(supply(pg, 1, 0, 4), OK);   /* absent -> clean */
    KT_EQ(pager_resident(pg), 4);

    /* A load installs a clean page read-only, though the mapping may write. */
    KT_EQ(aspace_fault(as, a, ASPACE_READ), OK);
    KT_ASSERT(aspace_pte(as, a) & 1);
    KT_ASSERT(!pte_w(as, a));
    KT_EQ(vmo_pager_page_state(v, 0), 0);
    KT_EQ(uword(as, a + 8), word(1, 0));

    /* A kernel copy's store faults on it and makes it dirty, then writable. */
    KT_EQ(uput(as, a + 16, 0x1234), OK);
    KT_EQ(vmo_pager_page_state(v, 0), DIRTY);
    KT_ASSERT(pte_w(as, a));
    KT_EQ(vword(v, 16), 0x1234);
    KT_EQ(vmo_pager_dirty_count(v), 1);
    KT_EQ(pager_dirty_pages(pg), 1);
    KT_EQ(pager_resident(pg), 4);   /* dirtying moves a page, it adds none */

    /* A store fault with no entry yet: dirty and writable at once. */
    KT_EQ(aspace_fault(as, a + PG, ASPACE_WRITE), OK);
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY);
    KT_ASSERT(pte_w(as, a + PG));
    /* A load of a dirty page installs it writable (the mapping allows). */
    KT_EQ(aspace_unmap(as, a, 4 * PG), OK);
    a = map(as, v, 4, RW);
    KT_EQ(aspace_fault(as, a + PG, ASPACE_READ), OK);
    KT_ASSERT(pte_w(as, a + PG));
    KT_EQ(aspace_fault(as, a + 2 * PG, ASPACE_READ), OK);
    KT_ASSERT(!pte_w(as, a + 2 * PG));

    /* Protect takes the write bit and never gives it back: the next store
     * faults, finds the page dirty, and installs it writable again. */
    KT_EQ(aspace_protect(as, a, 4 * PG, ASPACE_READ), OK);
    KT_ASSERT(!pte_w(as, a + PG));
    KT_EQ(aspace_protect(as, a, 4 * PG, RW), OK);
    KT_ASSERT(!pte_w(as, a + PG));
    KT_EQ(uput(as, a + PG, 77), OK);
    KT_ASSERT(pte_w(as, a + PG));
    KT_EQ(vmo_pager_dirty_count(v), 2);
    KT_EQ(pager_dirty_pages(pg), 2);

    /* A read-only mapping of the same VMO: never writable, a store refused. */
    uint64_t b = map(as, v, 4, ASPACE_READ);
    KT_EQ(aspace_fault(as, b, ASPACE_READ), OK);
    KT_ASSERT(!pte_w(as, b));
    KT_EQ(aspace_fault(as, b + 3 * PG, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(vmo_pager_page_state(v, 3), 0);

    no_clean_writable(as, v, a, 4);
    KT_ASSERT(aspace_check(as) >= 0);
    KT_EQ(aspace_unmap(as, a, 4 * PG), OK);
    KT_EQ(aspace_unmap(as, b, 4 * PG), OK);
    aspace_unref(as);
    put(v);
    none_queued(pg);   /* dirty: held, so no GONE */
    pg_close(pg);
    KT_EQ(pager_dirty_pages(pg), 0);
    kt_job_is_empty(j);
    job_unref(j);
}

/* The program each ring-3 thread runs: rsi = where to load from, rdi =
 * where to store it; then it spins until it is cancelled. */
static const uint8_t uprog[] = {
    0x48, 0x8b, 0x06,   /* mov rax, [rsi] */
    0x48, 0x89, 0x07,   /* mov [rdi], rax */
    0xeb, 0xfe,         /* jmp . */
};

#define U_CODE 0x10000000ull
#define U_DATA 0x10200000ull
#define U_FILE 0x20000000ull

struct ut {
    struct aspace *as;
    uint64_t       load, store;
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

KTEST(pager_dirty_ring3_store)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 2, 2);
    struct aspace *as;
    struct vmo *code, *data;
    KT_EQ(aspace_create(&as), OK);
    KT_EQ(vmo_create(PG, 0, &code), OK);
    KT_EQ(vmo_write(code, 0, uprog, sizeof(uprog)), OK);
    KT_EQ(vmo_create(PG, 0, &data), OK);
    uint64_t mark = 0xfeedfacecafe0001ull, at = U_CODE;
    KT_EQ(vmo_write(data, 0, &mark, 8), OK);
    KT_EQ(aspace_map(as, code, 0, PG, ASPACE_READ | ASPACE_EXEC | ASPACE_FIXED, &at), OK);
    at = U_DATA;
    KT_EQ(aspace_map(as, data, 0, PG, RW | ASPACE_FIXED, &at), OK);
    at = U_FILE;
    KT_EQ(aspace_map(as, v, 0, 2 * PG, RW | ASPACE_FIXED, &at), OK);
    KT_EQ(supply(pg, 2, 0, 2), OK);
    KT_EQ(aspace_fault(as, U_FILE, ASPACE_READ), OK);   /* clean: present, read-only */
    KT_ASSERT((aspace_pte(as, U_FILE) & 1) && !pte_w(as, U_FILE));

    struct ut u = { .as = as, .load = U_DATA, .store = U_FILE + 24 };
    struct thread *t = thread_create("wk-ring3", ut_main, &u, PRIO_DEFAULT);
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (vword(v, 24) != mark && uptime_ns() < deadline)
        thread_sleep_ms(1);
    KT_EQ(vword(v, 24), mark);
    KT_EQ(vmo_pager_page_state(v, 0), DIRTY);
    KT_ASSERT(pte_w(as, U_FILE));
    KT_EQ(vmo_pager_page_state(v, 1), 0);
    thread_cancel(t);
    thread_join(t);

    KT_EQ(aspace_unmap(as, U_FILE, 2 * PG), OK);   /* (the thread's reaper drops the space later) */
    aspace_unref(as);
    put(code);
    put(data);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- eviction ------------------------------------------------------------- */

KTEST(pager_dirty_never_evicted)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, MIN_BUDGET);
    struct vmo *v = wvmo(pg, 3, 16);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 16, RW);
    const uint64_t d = pager_dirty_limit(pg);   /* 2: as many as may be dirty */
    KT_EQ(d, PAGER_WRITABLE_MIN_PAGES / 4);
    KT_EQ(supply(pg, 3, 0, 8), OK);
    for (uint64_t i = 0; i < d; i++)
        KT_EQ(uput(as, a + i * PG, 100 + i), OK);
    KT_EQ(pager_dirty_pages(pg), d);
    KT_EQ(pager_resident(pg), 8);

    /* Each supply past the budget evicts the oldest clean page, never a
     * dirty one: pages d.. go, 0..d - 1 stay mapped and dirty. */
    for (uint64_t i = 8; i < 16; i++)
        KT_EQ(supply(pg, 3, i, 1), OK);
    KT_EQ(pager_resident(pg), 8);
    for (uint64_t i = 0; i < d; i++) {
        KT_EQ(vmo_pager_page_state(v, i), DIRTY);
        KT_ASSERT(pte_w(as, a + i * PG));
        KT_EQ(uword(as, a + i * PG), 100 + i);
    }
    for (uint64_t i = d; i < d + 8; i++)
        KT_EQ(vmo_page_phys(v, i * PG), 0);
    for (uint64_t i = d + 8; i < 16; i++)
        KT_ASSERT(vmo_page_phys(v, i * PG));

    /* An invalidation drops the clean pages only (dirty ones are newer
     * than the disk). */
    KT_EQ(pager_op(pg, 3, PAGER_OP_INVALIDATE, 0, 16 * PG, 0), OK);
    for (uint64_t i = 0; i < 16; i++)
        KT_EQ(vmo_page_phys(v, i * PG) != 0, i < d);
    KT_EQ(pager_resident(pg), d);
    KT_EQ(pager_dirty_pages(pg), d);
    for (uint64_t i = 0; i < d; i++)
        KT_EQ(uword(as, a + i * PG), 100 + i);
    no_clean_writable(as, v, a, 16);

    KT_EQ(aspace_unmap(as, a, 16 * PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* The popped-then-stored race, made certain: the hook stores to the page
 * the evictor just popped, before the evictor locks its VMO. */
static struct aspace *popped_as;
static uint64_t popped_va;
static int popped_hits;

static void store_popped(void *arg)
{
    (void)arg;
    if (popped_hits++ == 0)
        KT_EQ(aspace_fault(popped_as, popped_va, ASPACE_WRITE), OK);
}

KTEST(pager_dirty_popped_then_stored)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, MIN_BUDGET);
    struct vmo *v = wvmo(pg, 13, 16);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 16, RW);
    KT_EQ(supply(pg, 13, 0, 8), OK);
    popped_as = as;
    popped_va = a;   /* page 0: the oldest, the next supply's victim */
    popped_hits = 0;
    __atomic_store_n(&dbg_hooks[DBG_PAGER_EVICT], store_popped, __ATOMIC_RELEASE);
    KT_EQ(supply(pg, 13, 8, 1), OK);
    __atomic_store_n(&dbg_hooks[DBG_PAGER_EVICT], NULL, __ATOMIC_RELEASE);
    KT_EQ(popped_hits, 2);
    /* The evictor left the page the store dirtied: still there, dirty and
     * counted again, one past the budget, so the supply evicted the next
     * oldest clean page (1) before it published its own. */
    KT_EQ(vmo_pager_page_state(v, 0), DIRTY);
    KT_ASSERT(pte_w(as, a));
    KT_ASSERT(vmo_page_phys(v, 8 * PG));
    KT_EQ(vmo_page_phys(v, PG), 0);
    KT_EQ(pager_resident(pg), PAGER_WRITABLE_MIN_PAGES);
    KT_EQ(pager_dirty_pages(pg), 1);
    KT_EQ(supply(pg, 13, 9, 1), OK);   /* takes the oldest clean page's slot: page 2 */
    KT_EQ(vmo_page_phys(v, 2 * PG), 0);
    KT_EQ(pager_resident(pg), PAGER_WRITABLE_MIN_PAGES);
    KT_EQ(vmo_pager_page_state(v, 0), DIRTY);
    no_clean_writable(as, v, a, 16);

    KT_EQ(aspace_unmap(as, a, 16 * PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* Faults every page of [base, base + pages * PG) in order, again and
 * again, until stopped: with a small budget every supply evicts. */
struct sweep {
    struct aspace *as;
    uint64_t       base, pages;
    bool           stop;   /* atomic */
    struct thread *t;
};

static void sweep_main(void *arg)
{
    struct sweep *s = arg;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))
        for (uint64_t i = 0; i < s->pages; i++)
            (void)aspace_fault(s->as, s->base + i * PG, ASPACE_READ);   /* (shrunk: refused) */
}

KTEST(pager_dirty_evict_storm)
{
    enum { PAGES = 32, ROUNDS = 300 };
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, MIN_BUDGET);
    struct vmo *v = wvmo(pg, 4, PAGES);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, PAGES, RW);
    struct kpager k;
    kp_start(&k, pg);
    struct sweep s = { .as = as, .base = a, .pages = PAGES };
    s.t = thread_create("wk-sweep", sweep_main, &s, PRIO_DEFAULT);

    for (uint64_t r = 0; r < ROUNDS; r++) {
        uint64_t page = (r * 7) % PAGES, val = 0xd00d000000000000ull | r;
        /* The store may have to fetch its page more than once while the
         * sweeper evicts (FAULT_ROUNDS): try again. */
        status_t st = ERR_INTERNAL;
        for (int tries = 0; tries < 100 && st != OK; tries++)
            st = uput(as, a + page * PG + 8, val);
        KT_EQ(st, OK);
        KT_ASSERT(vmo_pager_page_state(v, page) & DIRTY);
        /* Let the sweeper replace every clean page (the budget's worth of
         * supplies), then the store must still be there. */
        uint64_t served = __atomic_load_n(&k.served, __ATOMIC_RELAXED);
        uint64_t deadline = uptime_ns() + 50 * NS_PER_MS;
        while (__atomic_load_n(&k.served, __ATOMIC_RELAXED) < served + PAGER_WRITABLE_MIN_PAGES &&
               uptime_ns() < deadline)
            thread_yield();
        KT_EQ(vword(v, page * PG + 8), val);
        if (r % 2 == 1) {   /* at the dirty limit (2): drop them all, then grow back */
            KT_EQ(pager_op(pg, 4, PAGER_OP_SET_SIZE, 0, 0, 0), OK);
            KT_EQ(vmo_pager_dirty_count(v), 0);
            KT_EQ(pager_op(pg, 4, PAGER_OP_SET_SIZE, PAGES * PG, 0, 0), OK);
        }
    }
    __atomic_store_n(&s.stop, true, __ATOMIC_RELEASE);
    thread_join(s.t);
    kp_stop(&k);
    no_clean_writable(as, v, a, PAGES);
    KT_EQ(pager_dirty_pages(pg), vmo_pager_dirty_count(v));
    KT_EQ(aspace_unmap(as, a, PAGES * PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- the hold, FIND and the takeover ---------------------------------------- */

KTEST(pager_dirty_hold_and_find)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 5, 8), *f;
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 8, RW);
    KT_EQ(supply(pg, 5, 0, 8), OK);
    KT_EQ(uput(as, a + 1 * PG, 11), OK);
    KT_EQ(uput(as, a + 5 * PG, 55), OK);
    KT_EQ(uput(as, a + 6 * PG, 66), OK);
    KT_EQ(pager_dirty_pages(pg), 3);

    /* A shrink drops the dirty pages past the end too. */
    KT_EQ(pager_op(pg, 5, PAGER_OP_SET_SIZE, 4 * PG, 0, 0), OK);
    KT_EQ(vmo_pager_dirty_count(v), 1);
    KT_EQ(pager_dirty_pages(pg), 1);
    KT_EQ(aspace_pte(as, a + 5 * PG), 0);

    /* No handle, no mapping: the dirty page holds it, no GONE, and FIND
     * hands it out again with the store in it. */
    KT_EQ(aspace_unmap(as, a, 8 * PG), OK);
    put(v);
    none_queued(pg);
    KT_EQ(pager_create_vmo_flags(pg, 5, 0, PAGER_VMO_FIND, &f), OK);
    KT_ASSERT(f == v);
    KT_EQ(vword(f, PG), 11);
    KT_EQ(vmo_pager_page_state(f, 1), DIRTY);
    put(f);
    none_queued(pg);

    /* The last dirty page goes: so does the hold, the VMO, and the GONE
     * comes; FIND finds nothing now. */
    KT_EQ(pager_op(pg, 5, PAGER_OP_SET_SIZE, 0, 0, 0), OK);
    struct pager_request q;
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);
    KT_EQ(q.key, 5);
    KT_EQ(pager_create_vmo_flags(pg, 5, 0, PAGER_VMO_FIND, &f), ERR_NOT_FOUND);
    KT_EQ(pager_dirty_pages(pg), 0);

    /* A create of the key takes the delivered GONE over at once. */
    KT_EQ(pager_create_vmo_flags(pg, 5, PG, PAGER_VMO_WRITABLE, &v), OK);
    KT_EQ(pager_create_vmo_flags(pg, 5, 0, PAGER_VMO_FIND, &f), OK);
    KT_ASSERT(f == v);
    put(f);
    put(v);   /* clean: it goes, a GONE queued */
    KT_EQ(pager_create_vmo_flags(pg, 5, PG, 0, &v), OK);   /* a queued GONE withdrawn */
    none_queued(pg);
    put(v);
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);
    KT_EQ(pager_op(pg, 5, PAGER_OP_DETACH, 0, 0, 0), OK);

    /* A detach drops dirty pages (lost) and ends the hold. */
    v = wvmo(pg, 6, 2);
    a = map(as, v, 2, RW);
    KT_EQ(supply(pg, 6, 0, 2), OK);
    KT_EQ(uput(as, a, 1), OK);
    KT_EQ(aspace_unmap(as, a, 2 * PG), OK);
    put(v);
    KT_EQ(pager_dirty_pages(pg), 1);
    KT_EQ(pager_op(pg, 6, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(pager_dirty_pages(pg), 0);
    KT_EQ(pager_resident(pg), 0);
    KT_EQ(pager_create_vmo_flags(pg, 6, 0, PAGER_VMO_FIND, &f), ERR_NOT_FOUND);
    none_queued(pg);

    aspace_unref(as);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* pager_dirty_dying_key's pager, and what its hook's create got. */
static struct pager *dying_pg;
static struct vmo   *dying_new;
static status_t      dying_found, dying_st;

/* DBG_PAGER_VMO_DYING: our VMO's last reference went and its pages were
 * freed, its link not told yet: the pager makes its key again (a map
 * racing another process's last unmap of the file). */
static void make_while_dying(void *arg)
{
    struct vmo *v = arg;
    if (vmo_pager_koid(v) != pager_kobject(dying_pg)->koid || vmo_pager_key(v) != 35)
        return;
    struct vmo *f = NULL;
    dying_found = pager_create_vmo_flags(dying_pg, 35, 0, PAGER_VMO_FIND, &f);
    if (dying_found == OK)
        put(f);   /* (wrong: checked after) */
    dying_st = pager_create_vmo_flags(dying_pg, 35, 2 * PG, PAGER_VMO_WRITABLE, &dying_new);
}

KTEST(pager_dirty_dying_key)
{
    struct job *j = kt_fresh_job();
    dying_pg = pg_new(j, 64);
    struct vmo *v = wvmo(dying_pg, 35, 2);
    KT_EQ(supply(dying_pg, 35, 0, 2), OK);
    dying_new = NULL;
    dying_found = dying_st = ERR_BAD_STATE;
    __atomic_store_n(&dbg_hooks[DBG_PAGER_VMO_DYING], make_while_dying, __ATOMIC_RELEASE);
    put(v);   /* the last reference: destroyed here, the hook in the middle */
    __atomic_store_n(&dbg_hooks[DBG_PAGER_VMO_DYING], NULL, __ATOMIC_RELEASE);
    KT_EQ(dying_found, ERR_NOT_FOUND);   /* (dying: no reference to be had) */
    if (dying_st != OK)
        ktest_fail("a create while the key's VMO was dying: %d (the map would fail)", dying_st);
    KT_ASSERT(dying_new);

    /* The new VMO has the key; the old one's destruction freed its link
     * and sent no GONE. */
    struct vmo *f = NULL;
    KT_EQ(pager_create_vmo_flags(dying_pg, 35, 0, PAGER_VMO_FIND, &f), OK);
    KT_ASSERT(f == dying_new);
    put(f);
    none_queued(dying_pg);
    KT_EQ(supply(dying_pg, 35, 0, 2), OK);
    KT_EQ(vword(dying_new, PG), word(35, 1));
    put(dying_new);
    struct pager_request q;
    next(dying_pg, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);
    KT_EQ(q.key, 35);
    KT_EQ(pager_op(dying_pg, 35, PAGER_OP_DETACH, 0, 0, 0), OK);
    none_queued(dying_pg);

    pg_close(dying_pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- a store's wait ---------------------------------------------------------- */

KTEST(pager_dirty_store_waits)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 7, 4), *other;
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    KT_EQ(vmo_create(PG, 0, &other), OK);
    uint64_t a = map(as, v, 4, RW);
    uint64_t held0 = __atomic_load_n(&aspace_pager_waits_held, __ATOMIC_RELAXED);

    /* A store to an absent page waits for it as a load does, without the
     * region lock; when it comes, the store makes it dirty. */
    struct kf k;
    kf_start(&k, as, a + 8, ASPACE_WRITE);
    struct pager_request q;
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_READ);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, 4 * PG);
    thread_sleep_ms(5);
    KT_ASSERT(kf_waiting(&k));
    uint64_t b = map(as, other, 1, RW);   /* the region lock is free */
    KT_EQ(aspace_unmap(as, b, PG), OK);
    KT_EQ(supply(pg, 7, 0, 4), OK);
    KT_EQ(kf_end(&k), OK);
    KT_EQ(vmo_pager_page_state(v, 0), DIRTY);
    KT_ASSERT(pte_w(as, a));
    for (uint64_t i = 1; i < 4; i++)
        KT_EQ(vmo_pager_page_state(v, i), 0);

    /* A store cancelled while it waits: nothing dirtied, the counts as
     * they were, and the request still the pager's to answer. */
    KT_EQ(pager_op(pg, 7, PAGER_OP_INVALIDATE, 2 * PG, PG, 0), OK);
    KT_EQ(vmo_page_phys(v, 2 * PG), 0);
    kf_start(&k, as, a + 2 * PG, ASPACE_WRITE);
    next(pg, &q);
    KT_EQ(q.offset, 2 * PG);
    KT_EQ(q.len, PG);
    thread_sleep_ms(5);
    KT_ASSERT(kf_waiting(&k));
    thread_cancel(k.t);
    KT_EQ(kf_end(&k), ERR_CANCELED);
    KT_EQ(vmo_page_phys(v, 2 * PG), 0);
    KT_EQ(vmo_pager_dirty_count(v), 1);
    KT_EQ(pager_dirty_pages(pg), 1);
    KT_EQ(supply(pg, 7, 2, 1), OK);
    KT_EQ(vmo_pager_page_state(v, 2), 0);   /* it came clean: no store reached it */
    KT_EQ(aspace_pte(as, a + 2 * PG), 0);
    KT_GLOBAL_EQ(__atomic_load_n(&aspace_pager_waits_held, __ATOMIC_RELAXED), held0);

    no_clean_writable(as, v, a, 4);
    KT_EQ(aspace_unmap(as, a, 4 * PG), OK);
    aspace_unref(as);
    put(other);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- pager_dirty and pager_write ----------------------------------------------- */

static status_t peek(struct pager *pg, uint64_t key, uint32_t flags, uint64_t off, uint64_t len,
                     void *buf, struct pager_run *run)
{
    *run = (struct pager_run){ .key = 99, .offset = off, .len = len, .age_ns = 99 };
    struct pager_dst dst = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    return pager_dirty(pg, key, flags, run, &dst);
}

KTEST(pager_dirty_peek)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 8, 8), *ro;
    KT_EQ(pager_create_vmo(pg, 9, 8 * PG, &ro), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 8, RW);
    KT_EQ(supply(pg, 8, 0, 8), OK);
    KT_EQ(uput(as, a + 2 * PG + 64, 222), OK);
    KT_EQ(uput(as, a + 3 * PG, 333), OK);
    KT_EQ(uput(as, a + 5 * PG, 555), OK);
    uint64_t *buf = kzalloc(8 * PG);
    KT_ASSERT(buf);
    struct pager_run run;

    /* The first run in the range, its bytes at their place in buf. */
    KT_EQ(peek(pg, 8, PAGER_DIRTY_PEEK, 0, 8 * PG, buf, &run), OK);
    KT_EQ(run.key, 8);
    KT_EQ(run.offset, 2 * PG);
    KT_EQ(run.len, 2 * PG);
    KT_ASSERT(run.age_ns < 60 * NS_PER_S);
    KT_EQ(buf[(2 * PG + 64) / 8], 222);
    KT_EQ(buf[(2 * PG + 72) / 8], word(8, 2));
    KT_EQ(buf[3 * PG / 8], 333);
    KT_EQ(buf[3 * PG / 8 + 1], word(8, 3));
    KT_EQ(buf[0], 0);
    KT_EQ(buf[4 * PG / 8], 0);
    KT_EQ(buf[5 * PG / 8], 0);

    /* The next run, asked from where this one ended; then none. */
    memset(buf, 0, 8 * PG);
    KT_EQ(peek(pg, 8, 0, 4 * PG, 4 * PG, buf, &run), OK);
    KT_EQ(run.offset, 5 * PG);
    KT_EQ(run.len, PG);
    KT_EQ(buf[PG / 8], 555);   /* page 5 is page 1 of the range asked */
    KT_EQ(peek(pg, 8, PAGER_DIRTY_PEEK, 6 * PG, 2 * PG, buf, &run), OK);
    KT_EQ(run.len, 0);
    KT_EQ(run.key, 8);
    KT_EQ(run.age_ns, 0);
    KT_EQ(peek(pg, 8, 0, 6 * PG, 4 * PG, buf, &run), OK);   /* past the size: no run there */
    KT_EQ(run.len, 0);

    /* Nothing changed. */
    KT_EQ(vmo_pager_dirty_count(v), 3);
    KT_EQ(vmo_pager_page_state(v, 2), DIRTY);
    KT_ASSERT(pte_w(as, a + 2 * PG));

    /* Refusals. */
    KT_EQ(peek(pg, 8, 0, 1, PG, buf, &run), ERR_INVALID_ARGS);
    KT_EQ(peek(pg, 8, 0, 0, PG + 1, buf, &run), ERR_INVALID_ARGS);
    KT_EQ(peek(pg, 8, 0, 0, 0, buf, &run), ERR_INVALID_ARGS);
    KT_EQ(peek(pg, 8, 0, 0, PAGER_SUPPLY_MAX + PG, buf, &run), ERR_OUT_OF_RANGE);
    KT_EQ(peek(pg, 8, PAGER_DIRTY_BEGIN | PAGER_DIRTY_PEEK, 0, PG, buf, &run), ERR_INVALID_ARGS);
    KT_EQ(peek(pg, 8, 1u << 5, 0, PG, buf, &run), ERR_INVALID_ARGS);
    KT_EQ(peek(pg, 8, PAGER_DIRTY_BEGIN | PAGER_DIRTY_PEEK | PAGER_DIRTY_OLDEST, 0, PG, buf, &run),
          ERR_INVALID_ARGS);
    KT_EQ(peek(pg, 8, PAGER_DIRTY_OLDEST, 0, 0, buf, &run), ERR_INVALID_ARGS);
    KT_EQ(peek(pg, 77, 0, 0, PG, buf, &run), ERR_NOT_FOUND);
    KT_EQ(peek(pg, 9, 0, 0, PG, buf, &run), ERR_INVALID_ARGS);   /* not writable */

    kfree(buf);
    KT_EQ(aspace_unmap(as, a, 8 * PG), OK);
    aspace_unref(as);
    put(v);
    put(ro);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

static status_t pwrite(struct pager *pg, uint64_t key, uint64_t off, const void *buf,
                       uint64_t len)
{
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    return pager_write(pg, key, off, &src, len);
}

KTEST(pager_dirty_write)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 10, 4), *ro;
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 4, RW);
    KT_EQ(supply(pg, 10, 0, 2), OK);   /* pages 2 and 3 absent */
    KT_EQ(uput(as, a + PG, 1), OK);    /* page 1 dirty, page 0 clean */
    uint8_t *bytes = kmalloc(PG + 100);
    KT_ASSERT(bytes);
    memset(bytes, 0xab, PG + 100);

    /* [PG - 50, 2 PG + 50): the tail of page 0, all of page 1, the head of
     * page 2 (absent: stays so). */
    KT_EQ(pwrite(pg, 10, PG - 50, bytes, PG + 100), OK);
    uint8_t got[64];
    KT_EQ(vmo_read(v, PG - 64, got, 64), OK);
    for (int i = 0; i < 64; i++)
        KT_EQ(got[i], i < 14 ? (uint8_t)(word(10, 0) >> (8 * ((PG - 64 + i) % 8))) : 0xab);
    KT_EQ(vword(v, PG), 0xababababababababull);
    KT_EQ(vword(v, 2 * PG - 8), 0xababababababababull);
    KT_EQ(vmo_page_phys(v, 2 * PG), 0);
    KT_EQ(vmo_pager_page_state(v, 0), 0);       /* the states stay */
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY);
    KT_EQ(vmo_pager_dirty_count(v), 1);
    KT_ASSERT(!pte_w(as, a) || !(aspace_pte(as, a) & 1));
    KT_EQ(uword(as, a + PG - 8), 0xababababababababull);   /* the mapping sees it */

    /* Refusals and edges. */
    KT_EQ(pwrite(pg, 10, 4 * PG - 10, bytes, 20), ERR_OUT_OF_RANGE);
    KT_EQ(pwrite(pg, 10, 0, bytes, PAGER_SUPPLY_MAX + 1), ERR_OUT_OF_RANGE);
    KT_EQ(pwrite(pg, 10, 3 * PG, bytes, 0), OK);
    KT_EQ(pwrite(pg, 77, 0, bytes, 8), ERR_NOT_FOUND);

    /* A read-only pager VMO takes it too; a verified one never, and a
     * writable one is never verified (unmapped, so nothing else refuses). */
    KT_EQ(pager_create_vmo(pg, 11, 2 * PG, &ro), OK);
    KT_EQ(supply(pg, 11, 0, 1), OK);
    KT_EQ(pwrite(pg, 11, 8, bytes, 8), OK);
    KT_EQ(vword(ro, 8), 0xababababababababull);
    struct vmo *table, *bare = wvmo(pg, 12, 2);
    struct vmo_verify *vf;
    KT_EQ(vmo_create(PG, 0, &table), OK);
    KT_EQ(vmo_verify_new(j, table, 2, &vf), OK);
    KT_EQ(vmo_seal_verified(bare, vf), ERR_BAD_STATE);
    KT_EQ(vmo_seal_verified(ro, vf), OK);
    KT_EQ(pwrite(pg, 11, 8, bytes, 8), ERR_BAD_STATE);
    put(table);
    put(bare);

    kfree(bytes);
    KT_EQ(aspace_unmap(as, a, 4 * PG), OK);
    aspace_unref(as);
    put(v);
    put(ro);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- refusals and handles ------------------------------------------------------ */

KTEST(pager_dirty_refusals)
{
    struct job *j = kt_fresh_job();
    struct pager *small = pg_new(j, PAGER_WRITABLE_MIN_PAGES - 1), *pg = pg_new(j, 64);
    struct vmo *v, *ro;
    KT_EQ(pager_create_vmo_flags(small, 1, PG, PAGER_VMO_WRITABLE, &v), ERR_INVALID_ARGS);
    KT_EQ(pager_create_vmo_flags(small, 1, PG, 0, &ro), OK);
    put(ro);
    KT_EQ(pager_create_vmo_flags(pg, 1, PG, PAGER_VMO_FIND, &v), ERR_INVALID_ARGS);
    KT_EQ(pager_create_vmo_flags(pg, 1, 0, PAGER_VMO_FIND | PAGER_VMO_WRITABLE, &v),
          ERR_INVALID_ARGS);
    KT_EQ(pager_create_vmo_flags(pg, 1, PG, 4, &v), ERR_INVALID_ARGS);
    KT_EQ(pager_create_vmo_flags(pg, 1, 0, PAGER_VMO_FIND, &v), ERR_NOT_FOUND);

    /* RIGHT_WRITE lets it be mapped writable, and nothing more. */
    v = wvmo(pg, 2, 2);
    uint64_t w = 1;
    KT_EQ(vmo_write(v, 0, &w, 8), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_set_size(v, 4 * PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_commit(v, 0, PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_decommit(v, 0, PG), ERR_NOT_SUPPORTED);
    void *kva;
    KT_EQ(vmo_map_kernel(v, 0, PG, VM_WRITE, &kva), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_seal(v), ERR_BAD_STATE);
    KT_EQ(vmo_make_jit(v), ERR_NOT_SUPPORTED);
    /* A read-only pager VMO still can't be mapped writable. */
    KT_EQ(pager_create_vmo(pg, 3, PG, &ro), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, ro, 0, PG, RW, &a), ERR_ACCESS_DENIED);
    KT_EQ(aspace_map(as, v, 0, PG, RW, &a), OK);
    KT_EQ(aspace_unmap(as, a, PG), OK);
    aspace_unref(as);

    /* Through handles: RIGHT_WRITE on a writable VMO's handle only, FIND's
     * as made; the calls need RIGHT_WRITE on the pager. */
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *root = resource_root();
    kobject_ref(root);
    struct khandle kh = khandle_from_new(root, RIGHTS_BASIC | RIGHT_ROOT_PAGER);
    handle_t rooth, pgh, rdonly, vh;
    KT_EQ(handle_insert(&t, &kh, &rooth), OK);
    KT_EQ(sys_pager_create(&t, rooth, 64, ANSWER, 0, &pgh), OK);
    struct handle_info hi;
    KT_EQ(sys_pager_create_vmo(&t, pgh, 1, PG, PAGER_VMO_WRITABLE, &vh), OK);
    KT_EQ(handle_info(&t, vh, &hi), OK);
    KT_EQ(hi.rights, PAGER_VMO_RIGHTS | RIGHT_WRITE);
    KT_EQ(sys_pager_create_vmo(&t, pgh, 1, 0, PAGER_VMO_FIND, &vh), OK);
    KT_EQ(handle_info(&t, vh, &hi), OK);
    KT_EQ(hi.rights, PAGER_VMO_RIGHTS | RIGHT_WRITE);
    KT_EQ(sys_pager_create_vmo(&t, pgh, 2, PG, 0, &vh), OK);
    KT_EQ(handle_info(&t, vh, &hi), OK);
    KT_EQ(hi.rights, PAGER_VMO_RIGHTS);
    KT_EQ(sys_pager_create_vmo(&t, pgh, 2, 0, PAGER_VMO_FIND, &vh), OK);
    KT_EQ(handle_info(&t, vh, &hi), OK);
    KT_EQ(hi.rights, PAGER_VMO_RIGHTS);
    KT_EQ(handle_duplicate(&t, pgh, RIGHTS_BASIC | RIGHT_READ, &rdonly), OK);
    struct pager_run run = { .offset = 0, .len = PG };
    uint64_t *buf = kzalloc(PG);
    KT_ASSERT(buf);
    struct pager_dst dst = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    KT_EQ(sys_pager_dirty(&t, rdonly, 1, PAGER_DIRTY_PEEK, &run, &dst), ERR_ACCESS_DENIED);
    KT_EQ(sys_pager_write(&t, rdonly, 1, 0, &src, 8), ERR_ACCESS_DENIED);
    KT_EQ(sys_pager_create_vmo(&t, rdonly, 1, 0, PAGER_VMO_FIND, &vh), ERR_ACCESS_DENIED);
    KT_EQ(sys_pager_dirty(&t, pgh, 1, PAGER_DIRTY_PEEK, &run, &dst), OK);
    KT_EQ(run.len, 0);
    kfree(buf);
    handle_table_destroy(&t);

    put(v);
    put(ro);
    pg_close(small);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- fork ------------------------------------------------------------------------ */

#define BASE  0x40000000ull
#define BASE2 0x48000000ull
#define BASE3 0x50000000ull

static struct map_info info_at(struct aspace *as, uint64_t va)
{
    struct map_info mi = { 0 };
    KT_EQ(aspace_get_maps(as, va, &mi, 1), 1);
    return mi;
}

KTEST(pager_dirty_fork)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 12, 4);
    struct aspace *src, *dst;
    KT_EQ(aspace_create_charged(j, &src), OK);
    KT_EQ(aspace_create_charged(j, &dst), OK);
    uint64_t at = BASE;
    KT_EQ(aspace_map(src, v, 0, 4 * PG, RW | ASPACE_FIXED, &at), OK);
    at = BASE2;   /* none asked: a writable mapping of one is shared all the same */
    KT_EQ(aspace_map(src, v, 0, 4 * PG, RW | ASPACE_FIXED | ASPACE_CLONE_NONE, &at), OK);
    at = BASE3;
    KT_EQ(aspace_map(src, v, 0, 4 * PG, ASPACE_READ | ASPACE_FIXED | ASPACE_CLONE_NONE, &at), OK);
    KT_EQ(info_at(src, BASE).flags, VMAR_CLONE_SHARE);
    KT_EQ(info_at(src, BASE2).flags, VMAR_CLONE_SHARE);
    KT_EQ(info_at(src, BASE3).flags, VMAR_CLONE_NONE);   /* never writable: the caller's say */
    KT_EQ(supply(pg, 12, 0, 4), OK);
    KT_EQ(uput(src, BASE, 1000), OK);                      /* page 0 dirty */
    KT_EQ(aspace_fault(src, BASE + PG, ASPACE_READ), OK);  /* page 1 clean, read-only */

    struct aspace_clone_req r = { 0 };
    KT_EQ(aspace_clone(src, dst, &r, 0), OK);
    KT_EQ(info_at(dst, BASE).flags, VMAR_CLONE_SHARE);
    KT_EQ(info_at(dst, BASE2).flags, VMAR_CLONE_SHARE);
    KT_EQ(aspace_get_maps(dst, BASE3, &(struct map_info){ 0 }, 1), 0);
    for (uint64_t i = 0; i < 4; i++)
        KT_EQ(aspace_pte(dst, BASE + i * PG), 0);   /* its faults make its entries */
    KT_EQ(vmo_rmap_check(v), 5);

    /* The child's faults follow the states: dirty writable, clean not. */
    KT_EQ(uword(dst, BASE), 1000);
    KT_ASSERT(pte_w(dst, BASE));
    KT_EQ(uword(dst, BASE + PG), word(12, 1));
    KT_ASSERT(!pte_w(dst, BASE + PG));
    /* Its stores dirty the one VMO's pages: the parent sees them. */
    KT_EQ(uput(dst, BASE + PG + 8, 2001), OK);
    KT_EQ(uput(dst, BASE2 + 2 * PG, 2002), OK);
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY);
    KT_EQ(vmo_pager_page_state(v, 2), DIRTY);
    KT_EQ(vmo_pager_page_state(v, 3), 0);
    KT_EQ(uword(src, BASE + PG + 8), 2001);
    KT_EQ(uword(src, BASE + 2 * PG), 2002);
    KT_EQ(aspace_pte(src, BASE + PG) & ~0xfffull, aspace_pte(dst, BASE + PG) & ~0xfffull);
    KT_EQ(aspace_fault(dst, BASE + 3 * PG, ASPACE_READ), OK);
    KT_ASSERT(!pte_w(dst, BASE + 3 * PG));
    KT_EQ(vmo_pager_dirty_count(v), 3);

    /* Written back: read-only in both; the child's store dirties a page
     * again, and the parent sees it. */
    uint64_t *buf = kzalloc(4 * PG);
    KT_ASSERT(buf);
    struct pager_run run;
    KT_EQ(peek(pg, 12, PAGER_DIRTY_BEGIN, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.len, 3 * PG);
    KT_EQ(buf[0], 1000);
    for (uint64_t i = 0; i < 3; i++) {
        KT_ASSERT(!pte_w(src, BASE + i * PG) && !pte_w(dst, BASE + i * PG));
        KT_ASSERT(!pte_w(src, BASE2 + i * PG) && !pte_w(dst, BASE2 + i * PG));
    }
    KT_EQ(pager_op(pg, 12, PAGER_OP_WRITTEN, 0, 4 * PG, OK), OK);
    KT_EQ(vmo_pager_dirty_count(v), 0);
    KT_EQ(uput(dst, BASE + PG + 16, 3001), OK);
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY);
    KT_EQ(uword(src, BASE + PG + 16), 3001);
    KT_ASSERT(!pte_w(src, BASE + PG));   /* (its next store faults and finds it dirty) */
    kfree(buf);

    for (uint64_t b = BASE; b <= BASE2; b += BASE2 - BASE) {
        no_clean_writable(src, v, b, 4);
        no_clean_writable(dst, v, b, 4);
    }
    KT_ASSERT(aspace_check(src) >= 0);
    KT_ASSERT(aspace_check(dst) >= 0);
    aspace_unref(dst);
    aspace_unref(src);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- write-back (WK2) ---------------------------------------------------------- */

static status_t begin(struct pager *pg, uint64_t key, uint64_t off, uint64_t len, void *buf,
                      struct pager_run *run)
{
    return peek(pg, key, PAGER_DIRTY_BEGIN, off, len, buf, run);
}

static status_t written(struct pager *pg, uint64_t key, uint64_t off, uint64_t len, status_t st)
{
    return pager_op(pg, key, PAGER_OP_WRITTEN, off, len, st);
}

/* pager_dirty with OLDEST (and `flags`): at most len bytes, a page at
 * least min_age old. */
static status_t oldest(struct pager *pg, uint32_t flags, uint64_t len, uint64_t min_age,
                       void *buf, struct pager_run *run)
{
    *run = (struct pager_run){ .key = 99, .offset = 99 * PG, .len = len, .age_ns = min_age };
    struct pager_dst dst = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    return pager_dirty(pg, 99, flags | PAGER_DIRTY_OLDEST, run, &dst);
}

#define BP_SPACES 4
#define BP_PAGES  64

KTEST(pager_dirty_begin_protects)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 16 * BP_PAGES);   /* (no background request: far from the limit) */
    struct vmo *v = wvmo(pg, 30, BP_PAGES);
    KT_EQ(supply(pg, 30, 0, BP_PAGES), OK);
    struct aspace *sp[BP_SPACES];
    uint64_t at[BP_SPACES][VMAR_MAX_OVERLAP];
    for (unsigned s = 0; s < BP_SPACES; s++) {
        KT_EQ(aspace_create(&sp[s]), OK);
        for (unsigned m = 0; m < VMAR_MAX_OVERLAP; m++)
            at[s][m] = map(sp[s], v, BP_PAGES, RW);   /* every page under the most there may be */
    }
    /* Every page dirty, and writable in every mapping (a load of a dirty
     * page installs it writable). */
    for (uint64_t p = 0; p < BP_PAGES; p++)
        KT_EQ(uput(sp[0], at[0][0] + p * PG, 0xb000 + p), OK);
    for (unsigned s = 0; s < BP_SPACES; s++)
        for (unsigned m = 0; m < VMAR_MAX_OVERLAP; m++)
            for (uint64_t p = 0; p < BP_PAGES; p++) {
                KT_EQ(aspace_fault(sp[s], at[s][m] + p * PG, ASPACE_READ), OK);
                KT_ASSERT(pte_w(sp[s], at[s][m] + p * PG));
            }

    /* BEGIN: a run cut to the batch budget (with this many mappings over
     * each page, `most` pages), every page of it writing and read-only in
     * every mapping before its bytes are copied; the rest untouched. */
    const uint64_t n = BP_SPACES * VMAR_MAX_OVERLAP;
    const uint64_t most = (VMO_BATCH_BUDGET / n - VMO_BATCH_MAPPING_COST) / VMO_BATCH_PROTECT_COST;
    KT_ASSERT(most > 1 && most < BP_PAGES);
    uint64_t *buf = kzalloc(BP_PAGES * PG);
    KT_ASSERT(buf);
    struct pager_run run;
    __atomic_store_n(&vmo_batch_cost_most, 0, __ATOMIC_RELAXED);
    KT_EQ(begin(pg, 30, 0, BP_PAGES * PG, buf, &run), OK);
    uint64_t cost = __atomic_load_n(&vmo_batch_cost_most, __ATOMIC_RELAXED);
    KT_GLOBAL_ASSERT(cost > VMO_BATCH_BUDGET / 2 && cost <= VMO_BATCH_BUDGET);
    KT_EQ(run.key, 30);
    KT_EQ(run.offset, 0);
    KT_EQ(run.len, most * PG);
    for (uint64_t p = 0; p < BP_PAGES; p++) {
        KT_EQ(vmo_pager_page_state(v, p), p < most ? PAGER_PAGE_WRITING : DIRTY);
        if (p < most) {
            KT_EQ(buf[p * PG / 8], 0xb000 + p);
            KT_EQ(buf[p * PG / 8 + 1], word(30, p));
        }
        for (unsigned s = 0; s < BP_SPACES; s++)
            for (unsigned m = 0; m < VMAR_MAX_OVERLAP; m++)
                KT_EQ(pte_w(sp[s], at[s][m] + p * PG), p >= most);
    }
    KT_EQ(pager_writing_pages(pg), most);

    /* The rest, a run at a time, each batch within the budget. */
    __atomic_store_n(&vmo_batch_cost_most, 0, __ATOMIC_RELAXED);
    uint64_t runs = 1;
    while (begin(pg, 30, 0, BP_PAGES * PG, buf, &run) == OK && run.len) {
        KT_ASSERT(run.len <= most * PG);
        runs++;
    }
    KT_EQ(run.len, 0);
    KT_EQ(runs, (BP_PAGES + most - 1) / most);
    KT_GLOBAL_ASSERT(__atomic_load_n(&vmo_batch_cost_most, __ATOMIC_RELAXED) <= VMO_BATCH_BUDGET);
    KT_EQ(pager_writing_pages(pg), BP_PAGES);
    KT_EQ(pager_dirty_pages(pg), BP_PAGES);

    /* A store after the begin: dirty again, writable in that mapping only. */
    KT_EQ(uput(sp[2], at[2][7] + 5 * PG + 8, 77), OK);
    KT_EQ(vmo_pager_page_state(v, 5), DIRTY | PAGER_PAGE_WRITING);
    KT_ASSERT(pte_w(sp[2], at[2][7] + 5 * PG));
    KT_ASSERT(!pte_w(sp[2], at[2][6] + 5 * PG));
    KT_ASSERT(!pte_w(sp[1], at[1][7] + 5 * PG));

    /* Written: clean but for that one, which stays dirty. */
    KT_EQ(written(pg, 30, 0, BP_PAGES * PG, OK), OK);
    for (uint64_t p = 0; p < BP_PAGES; p++)
        KT_EQ(vmo_pager_page_state(v, p), p == 5 ? DIRTY : 0);
    KT_EQ(pager_writing_pages(pg), 0);
    KT_EQ(vmo_pager_dirty_count(v), 1);
    KT_EQ(vword(v, 5 * PG + 8), 77);
    for (unsigned s = 0; s < BP_SPACES; s++) {
        for (unsigned m = 0; m < VMAR_MAX_OVERLAP; m++)
            no_clean_writable(sp[s], v, at[s][m], BP_PAGES);
        KT_EQ(aspace_check(sp[s]), VMAR_MAX_OVERLAP);
    }

    kfree(buf);
    for (unsigned s = 0; s < BP_SPACES; s++)
        aspace_unref(sp[s]);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* A kernel thread storing a counter to its word of a mapping until
 * stopped: `last` is the last count it stored. */
struct storer {
    struct aspace *as;
    uint64_t       va;
    uint64_t       last;
    bool          *stop;   /* atomic */
    struct thread *t;
};

static void storer_main(void *arg)
{
    struct storer *s = arg;
    enter(s->as, NULL);
    for (uint64_t c = 1; !__atomic_load_n(s->stop, __ATOMIC_ACQUIRE); c++) {
        KT_EQ(copy_to_user(s->va, &c, 8), OK);
        __atomic_store_n(&s->last, c, __ATOMIC_RELAXED);   /* the test polls the first */
    }
    enter(NULL, s->as);
}

/* One write-back of key's first dirty run in [0, pages): begun, its
 * bytes into the image (the disk), written. False if there was none. */
static bool wb_once(struct pager *pg, uint64_t key, uint64_t pages, uint64_t *image, uint64_t *buf)
{
    struct pager_run run;
    KT_EQ(begin(pg, key, 0, pages * PG, buf, &run), OK);
    if (!run.len)
        return false;
    memcpy((char *)image + run.offset, (char *)buf + run.offset, run.len);
    KT_EQ(written(pg, key, run.offset, run.len, OK), OK);
    return true;
}

/* A kernel thread writing key's dirty runs back again and again. */
struct wbloop {
    struct pager  *pg;
    uint64_t       key, pages;
    uint64_t      *image, *buf;
    bool           stop;   /* atomic */
    uint64_t       rounds;
    struct thread *t;
};

static void wbloop_main(void *arg)
{
    struct wbloop *w = arg;
    while (!__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) {
        if (wb_once(w->pg, w->key, w->pages, w->image, w->buf))
            w->rounds++;
        else
            thread_yield();
    }
}

#define ST_THREADS 8
#define ST_PAGES   4

KTEST(pager_dirty_store_storm)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 31, ST_PAGES);
    KT_EQ(supply(pg, 31, 0, ST_PAGES), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, ST_PAGES, RW);
    struct wbloop w = { .pg = pg, .key = 31, .pages = ST_PAGES };
    w.image = kmalloc(ST_PAGES * PG);
    w.buf = kmalloc(ST_PAGES * PG);
    KT_ASSERT(w.image && w.buf);
    for (uint64_t p = 0; p < ST_PAGES; p++)
        for (uint64_t i = 0; i < PG / 8; i++)
            w.image[p * PG / 8 + i] = word(31, p);

    /* Eight storers, two to a page, while a pager writes back. */
    bool stop = false;
    struct storer s[ST_THREADS];
    w.t = thread_create("wk-wback", wbloop_main, &w, PRIO_DEFAULT);
    for (unsigned t = 0; t < ST_THREADS; t++) {
        s[t] = (struct storer){ .as = as, .stop = &stop,
                                .va = a + (t % ST_PAGES) * PG + (t / ST_PAGES + 1) * 64 };
        s[t].t = thread_create("wk-store", storer_main, &s[t], PRIO_DEFAULT);
    }
    /* Every storer has stored before the 300 ms start: under the soak's
     * load a stress worker of a higher priority can keep a new thread
     * off its CPU until the starvation boost (STARVE_TICKS, 1 s), and one
     * that first ran after the stop never stored (soak 2026-10-17). */
    uint64_t until = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    for (unsigned t = 0; t < ST_THREADS; t++)
        while (!__atomic_load_n(&s[t].last, __ATOMIC_RELAXED) && uptime_ns() < until)
            thread_sleep_ns(NS_PER_MS);
    thread_sleep_ns(300 * NS_PER_MS);
    __atomic_store_n(&stop, true, __ATOMIC_RELEASE);
    for (unsigned t = 0; t < ST_THREADS; t++)
        thread_join(s[t].t);
    __atomic_store_n(&w.stop, true, __ATOMIC_RELEASE);
    thread_join(w.t);
    while (wb_once(pg, 31, ST_PAGES, w.image, w.buf))   /* what was stored since */
        ;

    /* The last image holds every storer's last count: no store was lost
     * between a write-back's copy and its end. */
    KT_EQ(pager_dirty_pages(pg), 0);
    KT_EQ(vmo_pager_dirty_count(v), 0);
    for (unsigned t = 0; t < ST_THREADS; t++) {
        KT_ASSERT(s[t].last > 0);
        KT_EQ(w.image[(s[t].va - a) / 8], s[t].last);
        KT_EQ(vword(v, s[t].va - a), s[t].last);
    }
    KT_IDLE_ASSERT(w.rounds > 10);
    kprintf("ktest %s: %lu write-backs while %u threads stored\n", ktest_current, w.rounds,
            ST_THREADS);
    no_clean_writable(as, v, a, ST_PAGES);

    kfree(w.image);
    kfree(w.buf);
    KT_EQ(aspace_unmap(as, a, ST_PAGES * PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_written)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 32, 4), *ro;
    KT_EQ(pager_create_vmo(pg, 33, 4 * PG, &ro), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 4, RW);
    KT_EQ(supply(pg, 32, 0, 4), OK);
    KT_EQ(uput(as, a, 10), OK);
    KT_EQ(uput(as, a + PG, 11), OK);
    uint64_t *buf = kzalloc(4 * PG);
    KT_ASSERT(buf);
    struct pager_run run;

    KT_EQ(begin(pg, 32, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.offset, 0);
    KT_EQ(run.len, 2 * PG);
    KT_EQ(buf[0], 10);
    KT_EQ(buf[PG / 8], 11);
    KT_EQ(vmo_pager_page_state(v, 0), PAGER_PAGE_WRITING);
    KT_EQ(vmo_pager_page_state(v, 1), PAGER_PAGE_WRITING);
    KT_ASSERT(!pte_w(as, a) && !pte_w(as, a + PG));
    KT_EQ(pager_writing_pages(pg), 2);
    KT_EQ(pager_dirty_pages(pg), 2);
    KT_EQ(vmo_pager_dirty_count(v), 2);

    /* A store after the begin: writing and dirty, writable again. BEGIN
     * takes no writing page (one write-back of a page at a time); PEEK
     * sees both, flags 0 the one stored to since. */
    KT_EQ(uput(as, a + PG + 8, 12), OK);
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY | PAGER_PAGE_WRITING);
    KT_ASSERT(pte_w(as, a + PG));
    KT_EQ(begin(pg, 32, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.len, 0);
    KT_EQ(peek(pg, 32, PAGER_DIRTY_PEEK, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.len, 2 * PG);
    KT_EQ(peek(pg, 32, 0, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.offset, PG);
    KT_EQ(run.len, PG);

    /* Written: the one not stored to since clean, the other dirty. */
    KT_EQ(written(pg, 32, 0, 4 * PG, OK), OK);
    KT_EQ(vmo_pager_page_state(v, 0), 0);
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY);
    KT_ASSERT(!pte_w(as, a));
    KT_ASSERT(pte_w(as, a + PG));
    KT_EQ(pager_writing_pages(pg), 0);
    KT_EQ(pager_dirty_pages(pg), 1);
    KT_EQ(vmo_pager_dirty_count(v), 1);
    KT_EQ(vword(v, PG + 8), 12);
    KT_EQ(vmo_pager_take_error(v, NULL), OK);
    KT_EQ(written(pg, 32, 0, 4 * PG, OK), OK);   /* no page writing: nothing changes */
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY);

    /* A write-back that fails: clean all the same, the error kept and
     * reported once. */
    KT_EQ(begin(pg, 32, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.offset, PG);
    KT_EQ(written(pg, 32, PG, PG, ERR_IO), OK);
    KT_EQ(vmo_pager_page_state(v, 1), 0);
    KT_EQ(vmo_pager_take_error(v, NULL), ERR_IO);
    KT_EQ(vmo_pager_take_error(v, NULL), OK);
    KT_EQ(uput(as, a + 2 * PG, 20), OK);
    KT_EQ(begin(pg, 32, 0, 4 * PG, buf, &run), OK);
    KT_EQ(written(pg, 32, 0, 4 * PG, ERR_NO_SPACE), OK);
    KT_EQ(vmo_pager_take_error(v, NULL), ERR_NO_SPACE);
    KT_EQ(vmo_pager_dirty_count(v), 0);
    KT_EQ(vword(v, 2 * PG), 20);   /* (still here until evicted) */

    /* Refusals. */
    KT_EQ(written(pg, 32, 0, PG, ERR_BAD_STATE), ERR_INVALID_ARGS);
    KT_EQ(written(pg, 32, 1, PG, OK), ERR_INVALID_ARGS);
    KT_EQ(written(pg, 32, 0, 0, OK), ERR_INVALID_ARGS);
    KT_EQ(written(pg, 32, PG, UINT64_MAX & ~(PG - 1), OK), ERR_INVALID_ARGS);
    KT_EQ(written(pg, 77, 0, PG, OK), ERR_NOT_FOUND);
    KT_EQ(written(pg, 33, 0, PG, OK), ERR_INVALID_ARGS);   /* not writable */
    KT_EQ(written(pg, 32, 8 * PG, PG, OK), OK);   /* past the size: no page there */

    /* Clean again: the hold went, so unmapped and put the VMO goes and
     * its GONE comes. */
    KT_EQ(aspace_unmap(as, a, 4 * PG), OK);
    put(v);
    struct pager_request q;
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);
    KT_EQ(q.key, 32);
    KT_EQ(pager_op(pg, 32, PAGER_OP_DETACH, 0, 0, 0), OK);

    kfree(buf);
    aspace_unref(as);
    put(ro);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_failed_kept)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 34, 4);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 4, RW);
    KT_EQ(supply(pg, 34, 0, 4), OK);
    KT_EQ(uput(as, a + PG + 8, 40), OK);
    KT_EQ(uput(as, a + 2 * PG + 8, 41), OK);
    uint64_t *buf = kzalloc(4 * PG);
    KT_ASSERT(buf);
    struct pager_run run;

    /* The write-back fails: clean (nothing dirty, the hold gone), the
     * error kept once. */
    KT_EQ(begin(pg, 34, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.offset, PG);
    KT_EQ(run.len, 2 * PG);
    KT_EQ(written(pg, 34, PG, 2 * PG, ERR_IO), OK);
    KT_EQ(vmo_pager_page_state(v, 1), 0);
    KT_EQ(vmo_pager_page_state(v, 2), 0);
    KT_EQ(pager_dirty_pages(pg), 0);
    KT_EQ(vmo_pager_dirty_count(v), 0);
    KT_EQ(vmo_pager_take_error(v, NULL), ERR_IO);
    KT_ASSERT(!pte_w(as, a + PG) && !pte_w(as, a + 2 * PG));

    /* But kept: PEEK copies both, their stores in them; flags 0 (dirty)
     * and BEGIN take neither. */
    memset(buf, 0, 4 * PG);
    KT_EQ(peek(pg, 34, PAGER_DIRTY_PEEK, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.offset, PG);
    KT_EQ(run.len, 2 * PG);
    KT_EQ(buf[PG / 8], word(34, 1));
    KT_EQ(buf[PG / 8 + 1], 40);
    KT_EQ(buf[2 * PG / 8 + 1], 41);
    KT_EQ(peek(pg, 34, 0, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.len, 0);
    KT_EQ(begin(pg, 34, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.len, 0);

    /* A store makes page 1 dirty (kept no more); its write-back, done,
     * makes it the disk's again: PEEK leaves it out. */
    KT_EQ(uput(as, a + PG + 16, 42), OK);
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY);
    KT_EQ(begin(pg, 34, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.offset, PG);
    KT_EQ(run.len, PG);
    KT_EQ(written(pg, 34, PG, PG, OK), OK);
    KT_EQ(vmo_pager_page_state(v, 1), 0);
    memset(buf, 0, 4 * PG);
    KT_EQ(peek(pg, 34, PAGER_DIRTY_PEEK, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.offset, 2 * PG);
    KT_EQ(run.len, PG);
    KT_EQ(buf[2 * PG / 8 + 1], 41);

    /* An invalidation drops the kept page as any clean one: the next touch
     * fetches the disk's bytes, and nothing is left to PEEK. */
    KT_EQ(pager_op(pg, 34, PAGER_OP_INVALIDATE, 2 * PG, PG, 0), OK);
    KT_EQ(peek(pg, 34, PAGER_DIRTY_PEEK, 0, 4 * PG, buf, &run), OK);
    KT_EQ(run.len, 0);
    KT_EQ(pager_dirty_pages(pg), 0);

    KT_EQ(aspace_unmap(as, a, 4 * PG), OK);
    put(v);
    struct pager_request q;
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_GONE);
    KT_EQ(q.key, 34);
    KT_EQ(pager_op(pg, 34, PAGER_OP_DETACH, 0, 0, 0), OK);

    kfree(buf);
    aspace_unref(as);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_again)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 34, 8);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 8, RW);
    KT_EQ(supply(pg, 34, 0, 8), OK);
    for (uint64_t p = 0; p < 4; p++)
        KT_EQ(uput(as, a + p * PG, 40 + p), OK);
    uint64_t *buf = kzalloc(8 * PG);
    KT_ASSERT(buf);
    struct pager_run run;

    /* A request delivered, its pages begun: then the pager dies. */
    struct pager_req *r;
    KT_EQ(vmo_pager_writeback(v, 0, 8, &r), OK);
    KT_ASSERT(r);
    struct pager_request q;
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(q.key, 34);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, 8 * PG);
    KT_EQ(begin(pg, 34, 0, 8 * PG, buf, &run), OK);
    KT_EQ(run.len, 4 * PG);
    KT_EQ(pager_writing_pages(pg), 4);
    KT_EQ(uput(as, a + 2 * PG + 8, 42), OK);   /* writing and dirty */

    /* Its successor's AGAIN: every writing page dirty again, the request
     * given again; nothing answered meanwhile. */
    KT_EQ(pager_next(pg, PAGER_NEXT_AGAIN, &q), OK);
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, 8 * PG);
    for (uint64_t p = 0; p < 8; p++)
        KT_EQ(vmo_pager_page_state(v, p), p < 4 ? DIRTY : 0);
    KT_EQ(pager_writing_pages(pg), 0);
    KT_EQ(pager_dirty_pages(pg), 4);
    KT_ASSERT(!pager_req_answered(r));
    for (uint64_t p = 0; p < 4; p++)   /* still read-only: the next store faults, finds it dirty */
        KT_ASSERT(!pte_w(as, a + p * PG) || p == 2);

    /* The successor writes them back: the request is answered. */
    KT_EQ(begin(pg, 34, 0, 8 * PG, buf, &run), OK);
    KT_EQ(run.len, 4 * PG);
    KT_EQ(buf[2 * PG / 8 + 1], 42);
    KT_ASSERT(!pager_req_answered(r));
    KT_EQ(written(pg, 34, 0, 8 * PG, OK), OK);
    KT_ASSERT(pager_req_answered(r));
    KT_EQ(pager_req_wait(r), OK);
    pager_req_put(r);
    none_queued(pg);
    KT_EQ(pager_dirty_pages(pg), 0);

    /* A BEGIN whose copy fails begins nothing: the page stays dirty. */
    KT_EQ(uput(as, a + 6 * PG, 46), OK);
    run = (struct pager_run){ .offset = 0, .len = 8 * PG };
    struct pager_dst bad = { .addr = 0x10000, .user = true };   /* (nothing mapped there) */
    KT_EQ(pager_dirty(pg, 34, PAGER_DIRTY_BEGIN, &run, &bad), ERR_INVALID_ARGS);
    KT_EQ(vmo_pager_page_state(v, 6), DIRTY);
    KT_EQ(pager_writing_pages(pg), 0);

    kfree(buf);
    KT_EQ(aspace_unmap(as, a, 8 * PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_requests)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 35, 8);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 8, RW);
    KT_EQ(supply(pg, 35, 0, 8), OK);
    uint64_t *buf = kzalloc(8 * PG);
    KT_ASSERT(buf);
    struct pager_run run;
    struct pager_request q;
    struct pager_req *r;

    /* Nothing dirty: no request. Refusals. */
    KT_EQ(vmo_pager_writeback(v, 0, 8, &r), OK);
    KT_ASSERT(!r);
    KT_EQ(vmo_pager_writeback(v, 0, 9, &r), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_pager_writeback(v, 4, 4, &r), ERR_OUT_OF_RANGE);

    /* Writing when it was made: that write-back's end is enough, though
     * the page was stored to after. */
    KT_EQ(uput(as, a, 1), OK);
    KT_EQ(begin(pg, 35, 0, PG, buf, &run), OK);
    KT_EQ(vmo_pager_writeback(v, 0, 1, &r), OK);
    KT_EQ(uput(as, a, 2), OK);
    KT_ASSERT(!pager_req_answered(r));
    KT_EQ(written(pg, 35, 0, PG, OK), OK);
    KT_EQ(vmo_pager_page_state(v, 0), DIRTY);
    KT_EQ(pager_req_wait(r), OK);
    pager_req_put(r);

    /* Writing and dirty when made: that end isn't enough, a write-back
     * that begins later is. The pager's answer leaves it needing a page
     * nobody writes back, so the pager gets it again. */
    KT_EQ(begin(pg, 35, 0, PG, buf, &run), OK);
    KT_EQ(uput(as, a, 3), OK);
    KT_EQ(vmo_pager_writeback(v, 0, 1, &r), OK);
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, PG);
    KT_EQ(written(pg, 35, 0, PG, OK), OK);
    KT_EQ(vmo_pager_page_state(v, 0), DIRTY);
    KT_ASSERT(!pager_req_answered(r));
    next(pg, &q);   /* given again */
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(q.offset, 0);
    KT_EQ(begin(pg, 35, 0, PG, buf, &run), OK);
    KT_EQ(buf[0], 3);
    KT_EQ(written(pg, 35, 0, PG, OK), OK);
    KT_EQ(pager_req_wait(r), OK);
    pager_req_put(r);
    KT_EQ(vmo_pager_page_state(v, 0), 0);

    /* Dirty when made: a write-back that began after it; a page dirtied
     * after it doesn't hold it up. */
    KT_EQ(uput(as, a + PG, 4), OK);
    KT_EQ(vmo_pager_writeback(v, 1, 3, &r), OK);
    next(pg, &q);
    KT_EQ(q.offset, PG);
    KT_EQ(q.len, 2 * PG);
    KT_EQ(uput(as, a + 2 * PG, 5), OK);
    KT_EQ(begin(pg, 35, PG, PG, buf, &run), OK);   /* page 1 only */
    KT_EQ(written(pg, 35, PG, 2 * PG, OK), OK);
    KT_EQ(pager_req_wait(r), OK);
    pager_req_put(r);
    KT_EQ(vmo_pager_page_state(v, 2), DIRTY);

    /* A failed write-back's error is the request's answer too. */
    KT_EQ(vmo_pager_writeback(v, 2, 3, &r), OK);
    KT_EQ(begin(pg, 35, 0, 8 * PG, buf, &run), OK);
    KT_EQ(run.offset, 2 * PG);
    KT_EQ(written(pg, 35, 2 * PG, PG, ERR_NO_SPACE), OK);
    KT_EQ(pager_req_wait(r), ERR_NO_SPACE);
    pager_req_put(r);
    KT_EQ(vmo_pager_take_error(v, NULL), ERR_NO_SPACE);

    /* A shrink: a request's pages past the end are done (dropped), one
     * wholly past it is answered. */
    struct pager_req *r2;
    KT_EQ(uput(as, a + 6 * PG, 6), OK);
    KT_EQ(uput(as, a + 3 * PG, 7), OK);
    KT_EQ(vmo_pager_writeback(v, 4, 8, &r), OK);
    KT_EQ(vmo_pager_writeback(v, 3, 7, &r2), OK);
    KT_EQ(pager_op(pg, 35, PAGER_OP_SET_SIZE, 5 * PG, 0, 0), OK);
    KT_EQ(pager_req_wait(r), OK);
    pager_req_put(r);
    KT_ASSERT(!pager_req_answered(r2));
    KT_EQ(vmo_pager_dirty_count(v), 1);

    /* A detach fails the rest. */
    KT_EQ(pager_op(pg, 35, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(pager_req_wait(r2), ERR_PEER_CLOSED);
    pager_req_put(r2);
    none_queued(pg);

    kfree(buf);
    KT_EQ(aspace_unmap(as, a, 8 * PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_background)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    KT_EQ(pager_dirty_limit(pg), 16);
    struct vmo *v = wvmo(pg, 36, 32);
    KT_EQ(supply(pg, 36, 0, 32), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 32, RW);
    uint64_t *buf = kzalloc(32 * PG);
    KT_ASSERT(buf);
    struct pager_run run;
    struct pager_request q;

    /* Half the limit: nothing; past it, one request for the oldest run. */
    for (uint64_t p = 0; p < 8; p++)
        KT_EQ(uput(as, a + p * PG, p), OK);
    none_queued(pg);
    KT_EQ(uput(as, a + 8 * PG, 8), OK);
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(q.key, 36);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, 9 * PG);
    KT_EQ(uput(as, a + 10 * PG, 10), OK);
    none_queued(pg);   /* one at a time */

    /* Served: under half again, nothing more. */
    KT_EQ(begin(pg, 36, q.offset, q.len, buf, &run), OK);
    KT_EQ(run.len, 9 * PG);
    KT_EQ(written(pg, 36, q.offset, q.len, OK), OK);
    KT_EQ(pager_dirty_pages(pg), 1);
    none_queued(pg);

    /* Past half again: the oldest dirty run is page 10's now. */
    for (uint64_t p = 12; p < 21; p++)
        KT_EQ(uput(as, a + p * PG, p), OK);
    next(pg, &q);
    KT_EQ(q.offset, 10 * PG);
    KT_EQ(q.len, PG);
    none_queued(pg);

    /* Answered and still past half: the WRITTEN asks again, for the next
     * oldest run. */
    KT_EQ(begin(pg, 36, q.offset, q.len, buf, &run), OK);
    KT_EQ(written(pg, 36, q.offset, q.len, OK), OK);
    KT_EQ(pager_dirty_pages(pg), 9);
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(q.offset, 12 * PG);
    KT_EQ(q.len, 9 * PG);
    none_queued(pg);

    kfree(buf);
    KT_EQ(aspace_unmap(as, a, 32 * PG), OK);
    aspace_unref(as);
    put(v);
    pg_close(pg);   /* (the delivered request fails with the detach) */
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_oldest)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v1 = wvmo(pg, 37, 8), *v2 = wvmo(pg, 38, 8);
    KT_EQ(supply(pg, 37, 0, 8), OK);
    KT_EQ(supply(pg, 38, 0, 8), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a1 = map(as, v1, 8, RW), a2 = map(as, v2, 8, RW);
    uint64_t *buf = kzalloc(8 * PG);
    KT_ASSERT(buf);
    struct pager_run run;

    KT_EQ(oldest(pg, 0, 8 * PG, 0, buf, &run), ERR_NOT_FOUND);   /* nothing dirty */
    KT_EQ(uput(as, a2 + 3 * PG, 3), OK);
    KT_EQ(uput(as, a1 + PG, 1), OK);
    KT_EQ(uput(as, a1 + 2 * PG, 2), OK);
    KT_EQ(uput(as, a2 + 4 * PG, 4), OK);

    /* The run around the oldest dirty page, whatever its VMO, at buf's
     * start. */
    KT_EQ(oldest(pg, PAGER_DIRTY_PEEK, 8 * PG, 0, buf, &run), OK);
    KT_EQ(run.key, 38);
    KT_EQ(run.offset, 3 * PG);
    KT_EQ(run.len, 2 * PG);
    KT_EQ(buf[0], 3);
    KT_EQ(buf[PG / 8], 4);
    KT_ASSERT(run.age_ns > 0 && run.age_ns < 60 * NS_PER_S);
    /* Younger than asked: no run, and how old it is. */
    KT_EQ(oldest(pg, 0, 8 * PG, 60 * NS_PER_S, buf, &run), OK);
    KT_EQ(run.len, 0);
    KT_EQ(run.key, 38);
    KT_ASSERT(run.age_ns > 0 && run.age_ns < 60 * NS_PER_S);

    /* BEGIN takes the oldest runs one after another, then none is left. */
    KT_EQ(oldest(pg, PAGER_DIRTY_BEGIN, 8 * PG, 0, buf, &run), OK);
    KT_EQ(run.key, 38);
    KT_EQ(run.offset, 3 * PG);
    KT_EQ(run.len, 2 * PG);
    KT_EQ(oldest(pg, PAGER_DIRTY_BEGIN, 8 * PG, 0, buf, &run), OK);
    KT_EQ(run.key, 37);
    KT_EQ(run.offset, PG);
    KT_EQ(run.len, 2 * PG);
    KT_EQ(buf[0], 1);
    KT_EQ(oldest(pg, PAGER_DIRTY_BEGIN, 8 * PG, 0, buf, &run), ERR_NOT_FOUND);
    KT_EQ(written(pg, 37, 0, 8 * PG, OK), OK);
    KT_EQ(written(pg, 38, 0, 8 * PG, OK), OK);

    /* Around it: from as far back as the length allows. */
    KT_EQ(uput(as, a1 + 6 * PG, 6), OK);
    KT_EQ(uput(as, a1 + 5 * PG, 5), OK);
    KT_EQ(uput(as, a1 + 7 * PG, 7), OK);
    KT_EQ(oldest(pg, PAGER_DIRTY_PEEK, 8 * PG, 0, buf, &run), OK);
    KT_EQ(run.offset, 5 * PG);
    KT_EQ(run.len, 3 * PG);
    KT_EQ(buf[0], 5);
    KT_EQ(oldest(pg, PAGER_DIRTY_PEEK, 2 * PG, 0, buf, &run), OK);
    KT_EQ(run.offset, 5 * PG);
    KT_EQ(run.len, 2 * PG);
    KT_EQ(oldest(pg, 0, PG, 0, buf, &run), OK);
    KT_EQ(run.offset, 6 * PG);
    KT_EQ(run.len, PG);
    KT_EQ(buf[0], 6);

    kfree(buf);
    KT_EQ(aspace_unmap(as, a1, 8 * PG), OK);
    KT_EQ(aspace_unmap(as, a2, 8 * PG), OK);
    aspace_unref(as);
    put(v1);
    put(v2);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- the dirty limit and vmo_sync (WK3) ------------------------------------------ */

#define SHORT (1000 * NS_PER_MS)   /* the deadline test's answer time */

extern uint64_t user_faults;   /* uentry.c: user threads killed for a fault */

/* An address space for ring-3 threads (uprog at U_CODE, its data at
 * U_DATA) with `file` mapped read-write at U_FILE, `pages` long. */
struct uspace {
    struct aspace *as;
    struct vmo    *code, *data;
    uint64_t       pages;
};

static void us_new(struct uspace *u, struct vmo *file, uint64_t pages)
{
    KT_EQ(aspace_create(&u->as), OK);
    KT_EQ(vmo_create(PG, 0, &u->code), OK);
    KT_EQ(vmo_write(u->code, 0, uprog, sizeof(uprog)), OK);
    KT_EQ(vmo_create(PG, 0, &u->data), OK);
    uint64_t at = U_CODE;
    KT_EQ(aspace_map(u->as, u->code, 0, PG, ASPACE_READ | ASPACE_EXEC | ASPACE_FIXED, &at), OK);
    at = U_DATA;
    KT_EQ(aspace_map(u->as, u->data, 0, PG, RW | ASPACE_FIXED, &at), OK);
    at = U_FILE;
    KT_EQ(aspace_map(u->as, file, 0, pages * PG, RW | ASPACE_FIXED, &at), OK);
    u->pages = pages;
}

static void us_free(struct uspace *u)
{
    KT_EQ(aspace_unmap(u->as, U_FILE, u->pages * PG), OK);
    aspace_unref(u->as);   /* (a ring-3 thread's reaper drops its own later) */
    put(u->code);
    put(u->data);
}

/* A ring-3 thread of u storing U_DATA's word to U_FILE's page `page`. */
static struct thread *ustore(struct uspace *u, struct ut *t, uint64_t page, const char *name)
{
    *t = (struct ut){ .as = u->as, .load = U_DATA, .store = U_FILE + page * PG };
    return thread_create(name, ut_main, t, PRIO_DEFAULT);
}

/* A kernel thread in a user copy (a store into as at va), or in vmo_sync
 * over [0, len) of v. */
struct kw {
    struct aspace *as;
    uint64_t       va;
    struct vmo    *v;
    uint64_t       len;
    uint32_t       flags;
    uint32_t      *since;   /* vmo_sync's sample (NULL: none) */
    status_t       st;   /* ERR_INTERNAL until it returns */
    struct thread *t;
};

#define KW_WORD 0xc0b1c0b1ull   /* what a copy stores */

static void kc_main(void *arg)
{
    struct kw *k = arg;
    __atomic_store_n(&k->st, uput(k->as, k->va, KW_WORD), __ATOMIC_RELEASE);
}

static void ks_main(void *arg)
{
    struct kw *k = arg;
    __atomic_store_n(&k->st, vmo_pager_sync(k->v, 0, k->len, k->flags, k->since), __ATOMIC_RELEASE);
}

static void kc_start(struct kw *k, struct aspace *as, uint64_t va)
{
    *k = (struct kw){ .as = as, .va = va, .st = ERR_INTERNAL };
    k->t = thread_create("wk-copy", kc_main, k, PRIO_DEFAULT);
}

static void ks_start_since(struct kw *k, struct vmo *v, uint64_t len, uint32_t flags,
                           uint32_t *since)
{
    *k = (struct kw){ .v = v, .len = len, .flags = flags, .since = since, .st = ERR_INTERNAL };
    k->t = thread_create("wk-sync", ks_main, k, PRIO_DEFAULT);
}

static void ks_start(struct kw *k, struct vmo *v, uint64_t len, uint32_t flags)
{
    ks_start_since(k, v, len, flags, NULL);
}

static bool kw_waiting(struct kw *k)
{
    return __atomic_load_n(&k->st, __ATOMIC_ACQUIRE) == ERR_INTERNAL;
}

static status_t kw_end(struct kw *k)
{
    thread_join(k->t);
    return k->st;
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
 * was killed for `why` (test_pager.c's check)? */
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

static uint64_t waits_held(void)
{
    return __atomic_load_n(&aspace_pager_waits_held, __ATOMIC_RELAXED);
}

KTEST(pager_dirty_limit_waits)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    const uint64_t lim = pager_dirty_limit(pg);
    KT_EQ(lim, 16);
    struct vmo *v = wvmo(pg, 40, 32), *other;
    KT_EQ(supply(pg, 40, 0, 32), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    KT_EQ(vmo_create(PG, 0, &other), OK);
    uint64_t a = map(as, v, 32, RW);
    uint64_t *buf = kzalloc(32 * PG);
    KT_ASSERT(buf);
    uint64_t held0 = waits_held();
    struct pager_run run;
    struct pager_request q;

    /* Up to the limit nothing waits; past half of it came the background
     * request, for the oldest run. */
    for (uint64_t p = 0; p < lim; p++)
        KT_EQ(uput(as, a + p * PG, p), OK);
    KT_EQ(pager_dirty_pages(pg), lim);
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, (lim / 2 + 1) * PG);

    /* At it, a load and a store to a dirty page go on. */
    KT_EQ(uword(as, a + 20 * PG), word(40, 20));
    KT_EQ(uput(as, a + 3 * PG, 33), OK);

    /* A store to a clean page waits (a fault, a user copy) without the
     * region lock, on the background request: no second one is made. */
    struct kf k;
    struct kw c;
    kf_start(&k, as, a + 20 * PG, ASPACE_WRITE);
    kc_start(&c, as, a + 21 * PG + 8);
    thread_sleep_ms(10);
    KT_ASSERT(kf_waiting(&k) && kw_waiting(&c));
    uint64_t b = map(as, other, 1, RW);   /* the region lock is free */
    KT_EQ(aspace_unmap(as, b, PG), OK);
    none_queued(pg);
    KT_EQ(vmo_pager_page_state(v, 20), 0);
    KT_EQ(vmo_pager_page_state(v, 21), 0);
    KT_EQ(pager_dirty_pages(pg), lim);

    /* Begun: writing pages still count, so they wait on. A store to a
     * page being written back doesn't wait (it is counted already). */
    KT_EQ(begin(pg, 40, q.offset, q.len, buf, &run), OK);
    KT_EQ(run.len, q.len);
    KT_EQ(buf[3 * PG / 8], 33);
    KT_EQ(uput(as, a + PG, 11), OK);
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY | PAGER_PAGE_WRITING);
    thread_sleep_ms(10);
    KT_ASSERT(kf_waiting(&k) && kw_waiting(&c));

    /* Written: the count falls, and both stores go on. */
    KT_EQ(written(pg, 40, q.offset, q.len, OK), OK);
    KT_EQ(kf_end(&k), OK);
    KT_EQ(kw_end(&c), OK);
    KT_EQ(vmo_pager_page_state(v, 20), DIRTY);
    KT_ASSERT(pte_w(as, a + 20 * PG));
    KT_EQ(vword(v, 21 * PG + 8), KW_WORD);
    KT_EQ(pager_dirty_pages(pg), lim - q.len / PG + 1 + 2);   /* page 1 again, 20, 21 */
    KT_GLOBAL_EQ(waits_held(), held0);

    no_clean_writable(as, v, a, 32);
    kfree(buf);
    KT_EQ(aspace_unmap(as, a, 32 * PG), OK);
    aspace_unref(as);
    put(other);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_limit_deadline)
{
    struct job *j = kt_fresh_job();
    struct pager *pg;
    KT_EQ(pager_create(j, 64, SHORT, &pg), OK);
    const uint64_t lim = pager_dirty_limit(pg);
    struct vmo *v = wvmo(pg, 44, 32), *w;
    KT_EQ(pager_create_vmo(pg, 45, 64 * PG, &w), OK);
    KT_EQ(supply(pg, 44, 0, 32), OK);
    struct uspace u;
    us_new(&u, v, 32);
    uint64_t a2 = map(u.as, w, 64, ASPACE_READ);
    uint64_t *buf = kzalloc(lim * PG);
    KT_ASSERT(buf);
    uint64_t held0 = waits_held(), log0 = klog_head();
    uint64_t faults0 = __atomic_load_n(&user_faults, __ATOMIC_RELAXED);
    struct pager_run run;
    struct pager_request q;

    /* The background request goes past its deadline unanswered: a store
     * at the limit has none to join, makes its own (the run around the
     * oldest dirty page) and goes on once that is answered. */
    for (uint64_t p = 0; p < lim; p++)
        KT_EQ(uput(u.as, U_FILE + p * PG, p), OK);
    next(pg, &q);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, (lim / 2 + 1) * PG);
    thread_sleep_ms(SHORT / NS_PER_MS + 50);
    struct kf k;
    kf_start(&k, u.as, U_FILE + 20 * PG, ASPACE_WRITE);
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(q.key, 44);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, lim * PG);
    KT_ASSERT(kf_waiting(&k));
    KT_EQ(begin(pg, 44, 0, lim * PG, buf, &run), OK);
    KT_EQ(written(pg, 44, 0, lim * PG, OK), OK);
    KT_EQ(kf_end(&k), OK);
    KT_EQ(pager_dirty_pages(pg), 1);

    /* A pager that serves reads and never writes back. At the limit again
     * a kernel fault, a user copy, vmo_sync and a ring-3 store wait. */
    for (uint64_t p = 0; p < lim - 1; p++)
        KT_EQ(uput(u.as, U_FILE + p * PG, p), OK);
    KT_EQ(pager_dirty_pages(pg), lim);
    struct kpager kp;
    kp_start(&kp, pg);
    struct kw c, s;
    struct ut t3;
    uint64_t t0 = uptime_ns();
    kf_start(&k, u.as, U_FILE + 21 * PG, ASPACE_WRITE);
    kc_start(&c, u.as, U_FILE + 22 * PG);
    ks_start(&s, v, 32 * PG, VMO_SYNC_WAIT);
    struct thread *t = ustore(&u, &t3, 23, "wk3-late");

    /* A reader of another of its VMOs is never held up: each supply finds
     * a clean page to evict, long before the writers' time is up. */
    for (uint64_t i = 0; i < 64; i++)
        KT_EQ(uword(u.as, a2 + i * PG), word(45, i));
    KT_ASSERT(kf_waiting(&k) && kw_waiting(&c) && kw_waiting(&s));

    /* At the answer time and not before: the fault ERR_TIMED_OUT, the
     * copy ERR_IO, vmo_sync ERR_TIMED_OUT, the ring-3 thread killed. */
    KT_EQ(kf_end(&k), ERR_TIMED_OUT);
    KT_ASSERT(uptime_ns() - t0 >= SHORT);
    KT_EQ(kw_end(&c), ERR_IO);
    KT_EQ(kw_end(&s), ERR_TIMED_OUT);
    thread_join(t);
    KT_ASSERT(killed_for(log0, "wk3-late", "page fault: the file's pager didn't answer"));
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), faults0 + 1);
    for (uint64_t p = 21; p < 24; p++)
        KT_EQ(vmo_pager_page_state(v, p), 0);
    KT_EQ(pager_dirty_pages(pg), lim);
    KT_GLOBAL_EQ(waits_held(), held0);
    kp_stop(&kp);

    kfree(buf);
    KT_EQ(aspace_unmap(u.as, a2, 64 * PG), OK);
    us_free(&u);
    put(w);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_limit_kill)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    const uint64_t lim = pager_dirty_limit(pg);
    struct vmo *v = wvmo(pg, 46, 32);
    KT_EQ(supply(pg, 46, 0, 32), OK);
    struct uspace u;
    us_new(&u, v, 32);
    uint64_t held0 = waits_held();
    uint64_t faults0 = __atomic_load_n(&user_faults, __ATOMIC_RELAXED);
    for (uint64_t p = 0; p < lim; p++)
        KT_EQ(uput(u.as, U_FILE + p * PG, p), OK);

    /* Every wait at the limit, and vmo_sync's: only a kill can end them
     * (the answer time is ten minutes). */
    struct kf k;
    struct kw c, s;
    struct ut t3;
    kf_start(&k, u.as, U_FILE + 20 * PG, ASPACE_WRITE);
    kc_start(&c, u.as, U_FILE + 21 * PG);
    ks_start(&s, v, 32 * PG, VMO_SYNC_WAIT);
    struct thread *t = ustore(&u, &t3, 22, "wk3-kill");
    thread_sleep_ms(10);
    KT_ASSERT(kf_waiting(&k) && kw_waiting(&c) && kw_waiting(&s));
    thread_cancel(k.t);
    KT_EQ(kf_end(&k), ERR_CANCELED);
    thread_cancel(c.t);
    KT_EQ(kw_end(&c), ERR_CANCELED);
    thread_cancel(s.t);
    KT_EQ(kw_end(&s), ERR_CANCELED);
    thread_cancel(t);
    thread_join(t);   /* killed, not for a fault of its own */
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), faults0);
    for (uint64_t p = 20; p < 23; p++)
        KT_EQ(vmo_pager_page_state(v, p), 0);
    KT_EQ(pager_dirty_pages(pg), lim);
    KT_GLOBAL_EQ(waits_held(), held0);

    us_free(&u);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_sync)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 1024);   /* (a 2 MiB VMO: two chunks) */
    struct vmo *v = wvmo(pg, 41, 512), *anon;
    KT_EQ(vmo_create(PG, 0, &anon), OK);
    KT_EQ(supply(pg, 41, 0, 8), OK);
    KT_EQ(supply(pg, 41, 300, 1), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 512, RW);
    uint64_t *buf = kzalloc(8 * PG);
    KT_ASSERT(buf);
    struct pager_run run;
    struct pager_request q;
    struct kw s;

    /* Refusals; OK at once with nothing dirty (no request), and for a VMO
     * that isn't a writable pager VMO. */
    KT_EQ(vmo_pager_sync(v, 0, PG, 4, NULL), ERR_INVALID_ARGS);
    KT_EQ(vmo_pager_sync(v, 0, 513 * PG, 0, NULL), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_pager_sync(v, 512 * PG, 1, 0, NULL), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_pager_sync(v, 0, 512 * PG, VMO_SYNC_WAIT, NULL), OK);
    KT_EQ(vmo_pager_sync(v, 0, 0, VMO_SYNC_WAIT, NULL), OK);
    KT_EQ(vmo_pager_sync(anon, 0, PG, VMO_SYNC_WAIT, NULL), OK);
    none_queued(pg);

    /* Exact: page 1's write-back began after its first store, and it was
     * stored to again; page 2 is dirty. The sync needs both written by
     * write-backs that begin after it: the end of page 1's first one
     * doesn't do (the request goes back to the pager). */
    KT_EQ(uput(as, a + PG, 1), OK);
    KT_EQ(begin(pg, 41, PG, PG, buf, &run), OK);
    KT_EQ(run.len, PG);
    KT_EQ(uput(as, a + PG, 2), OK);
    KT_EQ(uput(as, a + 2 * PG, 3), OK);
    ks_start(&s, v, 8 * PG, VMO_SYNC_WAIT);
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, 8 * PG);
    KT_EQ(uput(as, a + 3 * PG, 4), OK);   /* after it: not needed */
    KT_EQ(written(pg, 41, PG, PG, OK), OK);
    KT_EQ(vmo_pager_page_state(v, 1), DIRTY);
    thread_sleep_ms(10);
    KT_ASSERT(kw_waiting(&s));
    next(pg, &q);   /* asked again */
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, 8 * PG);
    /* Pages 1 and 2 begun and written: done, though page 3 (stored to
     * after it) is still dirty. */
    KT_EQ(begin(pg, 41, PG, 2 * PG, buf, &run), OK);
    KT_EQ(run.offset, PG);
    KT_EQ(run.len, 2 * PG);
    KT_EQ(buf[0], 2);
    KT_EQ(written(pg, 41, PG, 2 * PG, OK), OK);
    KT_EQ(kw_end(&s), OK);
    KT_EQ(vmo_pager_page_state(v, 3), DIRTY);

    /* A write-back's error: the sync waiting on it reports it, once. */
    ks_start(&s, v, 8 * PG, VMO_SYNC_WAIT);
    next(pg, &q);
    KT_EQ(begin(pg, 41, 0, 8 * PG, buf, &run), OK);
    KT_EQ(run.offset, 3 * PG);
    KT_EQ(written(pg, 41, 0, 8 * PG, ERR_IO), OK);
    KT_EQ(kw_end(&s), ERR_IO);
    KT_EQ(vmo_pager_page_state(v, 3), 0);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, NULL), OK);

    /* One kept from a write-back by age: reported by the next sync that
     * waits, with nothing dirty (not by one that doesn't). */
    KT_EQ(uput(as, a + 4 * PG, 5), OK);
    KT_EQ(begin(pg, 41, 4 * PG, PG, buf, &run), OK);
    KT_EQ(written(pg, 41, 4 * PG, PG, ERR_NO_SPACE), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, 0, NULL), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, NULL), ERR_NO_SPACE);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, NULL), OK);
    none_queued(pg);

    /* Without the wait: the request is made, and answered later. */
    KT_EQ(uput(as, a + 5 * PG, 6), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, 0, NULL), OK);
    next(pg, &q);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, 8 * PG);
    KT_EQ(begin(pg, 41, 0, 8 * PG, buf, &run), OK);
    KT_EQ(written(pg, 41, 0, 8 * PG, OK), OK);
    KT_EQ(vmo_pager_dirty_count(v), 0);
    none_queued(pg);

    /* Two chunks: the second's request is made once the first is
     * answered. */
    KT_EQ(uput(as, a + 7 * PG, 8), OK);
    KT_EQ(uput(as, a + 300 * PG, 9), OK);
    ks_start(&s, v, 512 * PG, VMO_SYNC_WAIT);
    next(pg, &q);
    KT_EQ(q.offset, 0);
    KT_EQ(q.len, 256 * PG);
    none_queued(pg);
    KT_EQ(begin(pg, 41, 7 * PG, PG, buf, &run), OK);
    KT_EQ(written(pg, 41, 7 * PG, PG, OK), OK);
    next(pg, &q);
    KT_EQ(q.offset, 256 * PG);
    KT_EQ(q.len, 256 * PG);
    KT_ASSERT(kw_waiting(&s));
    KT_EQ(begin(pg, 41, 300 * PG, PG, buf, &run), OK);
    KT_EQ(written(pg, 41, 300 * PG, PG, OK), OK);
    KT_EQ(kw_end(&s), OK);
    KT_EQ(vmo_pager_dirty_count(v), 0);

    kfree(buf);
    KT_EQ(aspace_unmap(as, a, 512 * PG), OK);
    aspace_unref(as);
    put(anon);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_errseq)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    struct vmo *v = wvmo(pg, 42, 8), *anon;
    KT_EQ(vmo_create(PG, 0, &anon), OK);
    KT_EQ(supply(pg, 42, 0, 8), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 8, RW);
    uint64_t *buf = kzalloc(8 * PG);
    KT_ASSERT(buf);
    struct pager_run run;
    struct pager_request q;
    struct kw s;
    uint32_t ma = 7, mb = 7, mc = 7, md = 7, other = 7;   /* four mappers' samples */

    /* Samples: refusals; 0 with no error yet, and for a VMO that isn't a
     * writable pager VMO. */
    KT_EQ(vmo_pager_sync(v, 0, 0, VMO_SYNC_SAMPLE, NULL), ERR_INVALID_ARGS);
    KT_EQ(vmo_pager_sync(v, 0, 0, VMO_SYNC_SAMPLE | VMO_SYNC_WAIT, &ma), ERR_INVALID_ARGS);
    KT_EQ(ma, 7);
    KT_EQ(vmo_pager_sync(anon, 0, 0, VMO_SYNC_SAMPLE, &other), OK);
    KT_EQ(other, 0);
    KT_EQ(vmo_pager_sync(v, 0, 0, VMO_SYNC_SAMPLE, &ma), OK);
    KT_EQ(vmo_pager_sync(v, 0, 0, VMO_SYNC_SAMPLE, &mb), OK);
    KT_EQ(ma, 0);
    KT_EQ(mb, 0);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &ma), OK);
    KT_EQ(ma, 0);

    /* A write-back by age fails. Mappers a and b each hear of it once, at
     * their next sync that waits (not at one that doesn't); c, made
     * before anyone heard, hears of it too (Linux's open after an error);
     * d, made after, doesn't. */
    KT_EQ(uput(as, a, 1), OK);
    KT_EQ(begin(pg, 42, 0, 8 * PG, buf, &run), OK);
    KT_EQ(written(pg, 42, 0, 8 * PG, ERR_IO), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, 0, &ma), OK);
    KT_EQ(ma, 0);
    KT_EQ(vmo_pager_sync(v, 0, 0, VMO_SYNC_SAMPLE, &mc), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &ma), ERR_IO);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &ma), OK);
    KT_EQ(vmo_pager_sync(v, 0, 0, VMO_SYNC_SAMPLE, &md), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &md), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &mb), ERR_IO);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &mb), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &mc), ERR_IO);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &mc), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, NULL), OK);   /* heard of: not new */
    KT_ASSERT(ma != 0 && ma == mb && mb == mc && mc == md);

    /* Two errors before anyone looks are one to the mappers (the first
     * kept); a caller with no sample hears of it if nobody did; a copy of
     * a sample (a forked child's) hears of it again. */
    uint32_t copy = ma;
    KT_EQ(uput(as, a + PG, 2), OK);
    KT_EQ(begin(pg, 42, 0, 8 * PG, buf, &run), OK);
    KT_EQ(written(pg, 42, 0, 8 * PG, ERR_NO_SPACE), OK);
    KT_EQ(uput(as, a + 2 * PG, 3), OK);
    KT_EQ(begin(pg, 42, 0, 8 * PG, buf, &run), OK);
    KT_EQ(written(pg, 42, 0, 8 * PG, ERR_IO), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, NULL), ERR_NO_SPACE);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, NULL), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &ma), ERR_NO_SPACE);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &ma), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &copy), ERR_NO_SPACE);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &mb), ERR_NO_SPACE);

    /* A sync whose own write-back fails hears of it (b's sample is the
     * count: the error moves it); c and d, two errors behind, hear of the
     * newest once. */
    KT_EQ(uput(as, a + 3 * PG, 4), OK);
    ks_start_since(&s, v, 8 * PG, VMO_SYNC_WAIT, &mb);
    next(pg, &q);
    KT_EQ(q.kind, PAGER_REQ_WRITEBACK);
    KT_EQ(begin(pg, 42, 0, 8 * PG, buf, &run), OK);
    KT_EQ(run.offset, 3 * PG);
    KT_EQ(written(pg, 42, 0, 8 * PG, ERR_IO), OK);
    KT_EQ(kw_end(&s), ERR_IO);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &mb), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &mc), ERR_IO);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &mc), OK);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &md), ERR_IO);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &ma), ERR_IO);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, &copy), ERR_IO);
    KT_EQ(vmo_pager_sync(v, 0, 8 * PG, VMO_SYNC_WAIT, NULL), OK);
    KT_ASSERT(ma == mb && mb == mc && mc == md && md == copy);
    none_queued(pg);

    kfree(buf);
    KT_EQ(aspace_unmap(as, a, 8 * PG), OK);
    aspace_unref(as);
    put(anon);
    put(v);
    pg_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(pager_dirty_closed)
{
    struct job *j = kt_fresh_job();
    struct pager *pg = pg_new(j, 64);
    const uint64_t lim = pager_dirty_limit(pg);
    struct vmo *v = wvmo(pg, 47, 4), *w = wvmo(pg, 48, 32);
    KT_EQ(supply(pg, 47, 0, 4), OK);
    KT_EQ(supply(pg, 48, 0, 32), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 4, RW), b = map(as, w, 32, RW);
    struct pager_request q;
    struct kw s;
    struct kf k;

    /* A detach fails a waiting sync, and a later one at once. */
    KT_EQ(uput(as, a, 1), OK);
    ks_start(&s, v, 4 * PG, VMO_SYNC_WAIT);
    next(pg, &q);
    KT_EQ(q.key, 47);
    KT_EQ(pager_op(pg, 47, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(kw_end(&s), ERR_PEER_CLOSED);
    KT_EQ(vmo_pager_sync(v, 0, 4 * PG, VMO_SYNC_WAIT, NULL), ERR_PEER_CLOSED);
    KT_EQ(vmo_pager_dirty_count(v), 0);

    /* The pager's close fails a store waiting at the limit at once: the
     * file's pager is gone. */
    for (uint64_t p = 0; p < lim; p++)
        KT_EQ(uput(as, b + p * PG, p), OK);
    kf_start(&k, as, b + 20 * PG, ASPACE_WRITE);
    thread_sleep_ms(10);
    KT_ASSERT(kf_waiting(&k));
    pg_close(pg);
    KT_EQ(kf_end(&k), ERR_PEER_CLOSED);
    KT_EQ(vmo_pager_dirty_count(w), 0);

    KT_EQ(aspace_unmap(as, a, 4 * PG), OK);
    KT_EQ(aspace_unmap(as, b, 32 * PG), OK);
    aspace_unref(as);
    put(v);
    put(w);
    kt_job_is_empty(j);
    job_unref(j);
}
