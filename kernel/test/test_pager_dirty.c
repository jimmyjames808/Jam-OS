/* Writable pager VMOs (docs/WRITEMAP-PLAN.md, track WK1; <jam/pager.h>'s
 * "Writable pager VMOs", vmo.c's): the page states, the write fault, the
 * lists and counts, the hold, PAGER_VMO_FIND and the GONE takeover,
 * pager_dirty's look, pager_write, and the clone mode.
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
 *       clean page writable in either. (Write-back, then the child's store
 *       dirtying the page again, is WK2's to add.)
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
    struct pager *pg = pg_new(j, PAGER_WRITABLE_MIN_PAGES);
    struct vmo *v = wvmo(pg, 3, 16);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = map(as, v, 16, RW);
    KT_EQ(supply(pg, 3, 0, 8), OK);
    for (uint64_t i = 0; i < 4; i++)
        KT_EQ(uput(as, a + i * PG, 100 + i), OK);
    KT_EQ(pager_dirty_pages(pg), 4);
    KT_EQ(pager_resident(pg), 8);

    /* Each supply past the budget evicts the oldest clean page, never a
     * dirty one: pages 4.. go, 0..3 stay mapped and dirty. */
    for (uint64_t i = 8; i < 16; i++)
        KT_EQ(supply(pg, 3, i, 1), OK);
    KT_EQ(pager_resident(pg), 8);
    for (uint64_t i = 0; i < 4; i++) {
        KT_EQ(vmo_pager_page_state(v, i), DIRTY);
        KT_ASSERT(pte_w(as, a + i * PG));
        KT_EQ(uword(as, a + i * PG), 100 + i);
    }
    for (uint64_t i = 4; i < 12; i++)
        KT_EQ(vmo_page_phys(v, i * PG), 0);
    for (uint64_t i = 12; i < 16; i++)
        KT_ASSERT(vmo_page_phys(v, i * PG));

    /* An invalidation drops the clean pages only (dirty ones are newer
     * than the disk). */
    KT_EQ(pager_op(pg, 3, PAGER_OP_INVALIDATE, 0, 16 * PG, 0), OK);
    for (uint64_t i = 0; i < 16; i++)
        KT_EQ(vmo_page_phys(v, i * PG) != 0, i < 4);
    KT_EQ(pager_resident(pg), 4);
    KT_EQ(pager_dirty_pages(pg), 4);
    for (uint64_t i = 0; i < 4; i++)
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
    struct pager *pg = pg_new(j, PAGER_WRITABLE_MIN_PAGES);
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
    KT_EQ(popped_hits, 1);
    /* The evictor left the page the store dirtied: still there, dirty and
     * counted again (one past the budget until a clean page goes). */
    KT_EQ(vmo_pager_page_state(v, 0), DIRTY);
    KT_ASSERT(pte_w(as, a));
    KT_ASSERT(vmo_page_phys(v, 8 * PG));
    KT_EQ(pager_resident(pg), PAGER_WRITABLE_MIN_PAGES + 1);
    KT_EQ(pager_dirty_pages(pg), 1);
    KT_EQ(supply(pg, 13, 9, 1), OK);   /* takes the oldest clean page's slot: page 1 */
    KT_EQ(vmo_page_phys(v, PG), 0);
    KT_EQ(pager_resident(pg), PAGER_WRITABLE_MIN_PAGES + 1);
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
    struct pager *pg = pg_new(j, PAGER_WRITABLE_MIN_PAGES);
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
        if (r % 6 == 5) {   /* dirty pages pile up: drop them all, then grow back */
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
    KT_EQ(peek(pg, 8, PAGER_DIRTY_BEGIN, 0, PG, buf, &run), ERR_NOT_SUPPORTED);   /* WK2 */
    KT_EQ(peek(pg, 8, PAGER_DIRTY_OLDEST, 0, PG, buf, &run), ERR_NOT_SUPPORTED);
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
