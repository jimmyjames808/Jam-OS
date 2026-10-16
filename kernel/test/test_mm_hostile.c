/* Hostile tests of the memory system at Chromium's scale (CR1, the kernel
 * review: docs/CR-REVIEW-KERNEL.md). Each plays a program that tries to
 * make the kernel pay for what its job can't, or to break the mapping
 * tree and the reverse map by racing them.
 *
 *   aspace_hostile_tib_reservations
 *       a job allowed 16 MiB reserves 100 TiB (a hundred 1 TiB VMOs, each
 *       mapped whole with no access) for a record each, then four threads
 *       commit pages a GiB apart by protect and touch, V8's way, until the
 *       job refuses: every refusal is ERR_NO_MEMORY, the job is never past
 *       its limit, the machine never pays more than the job (and the
 *       records), and decommit, protect back and shrink give it all back.
 *   aspace_hostile_protect_storm
 *       60,000 mappings of one reservation; eight threads protect pieces
 *       of it at random (and touch some) while one unmaps and replaces
 *       pieces and one decommits and snapshots the VMO under them, for two
 *       seconds: every call answers as documented, and afterwards the
 *       tree, its gaps, merging, the reverse map, every entry and the
 *       job's charges are exactly right (aspace_check).
 *   aspace_hostile_overlap_decommit
 *       CR1 finding 1: one address space holds VMAR_MAX_OVERLAP mappings
 *       over a page of one VMO and no more (first fit, fixed, a range; a
 *       replace in place of one fits); at the cap over 512 pages, all
 *       written, a child's write-protects come in batches within
 *       VMO_BATCH_BUDGET and the decommit in one. Then 64 address spaces
 *       map one VMO: 64 x 64 mappings of one page are one batch of their
 *       own (what the cap per address space bounds), and 64 x 4 mappings
 *       of 512 pages, all written, make a child's write-protects and a
 *       decommit batches within the budget. The longest batch of each is
 *       printed; every entry is gone after. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/vmo.h>

#define PG    PAGE_SIZE
#define GIB   (1ull << 30)
#define TIB   (1ull << 40)
#define R     ASPACE_READ
#define RW    (ASPACE_READ | ASPACE_WRITE)
#define CANRW (ASPACE_CAN_READ | ASPACE_CAN_WRITE)
#define FIX   ASPACE_FIXED
#define REPL  (ASPACE_FIXED | ASPACE_REPLACE)

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static uint64_t structs_pages(uint64_t mappings)
{
    return (mappings + ASPACE_MAPPINGS_PER_PAGE - 1) / ASPACE_MAPPINGS_PER_PAGE;
}

/* ---- a TiB of reservations from a job with 16 MiB --------------------------- */

#define TR_N       100                  /* reservations of 1 TiB each */
#define TR_LIMIT   ((16ull << 20) / PG)   /* the job's pages */
#define TR_THREADS 4
#define TR_REFUSALS 64                  /* each thread stops after this many */

static struct aspace *tr_as;
static struct job *tr_job;
static struct vmo *tr_vmo[TR_N];
static uint64_t tr_at[TR_N];
static uint64_t tr_touched[TR_THREADS], tr_refused[TR_THREADS], tr_bad, tr_most;

/* Thread i commits page k of its sequence: reservation (i + k * threads)
 * % N, a GiB further on each lap, a page inside the GiB that moves too. */
static void tr_worker(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    for (uint64_t k = 0; tr_refused[i] < TR_REFUSALS && k < 100000; k++) {
        uint64_t n = k * TR_THREADS + i;
        uint64_t r = n % TR_N, g = (n / TR_N) % (TIB / GIB), p = (n * 7) % 512;
        uint64_t va = tr_at[r] + g * GIB + p * PG;
        status_t st = aspace_protect(tr_as, va, PG, RW);
        if (st == OK)
            st = aspace_fault(tr_as, va, ASPACE_WRITE);
        if (st == OK)
            tr_touched[i]++;
        else if (st == ERR_NO_MEMORY)
            tr_refused[i]++;
        else
            __atomic_add_fetch(&tr_bad, 1, __ATOMIC_RELAXED);
        uint64_t used = job_used(tr_job, JOB_LIMIT_PAGES), most = tr_most;
        while (used > most && !__atomic_compare_exchange_n(&tr_most, &most, used, false,
                                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            ;
    }
}

KTEST(aspace_hostile_tib_reservations)
{
    tr_job = kt_fresh_job();
    KT_EQ(job_set_limit(tr_job, JOB_LIMIT_PAGES, TR_LIMIT), OK);
    uint64_t free0 = kt_free_and_cached_pages();
    KT_EQ(aspace_create_charged(tr_job, &tr_as), OK);
    for (unsigned r = 0; r < TR_N; r++) {
        KT_EQ(vmo_create_for(tr_job, TIB, 0, &tr_vmo[r]), OK);
        tr_at[r] = 0;
        KT_EQ(aspace_map(tr_as, tr_vmo[r], 0, TIB, CANRW, &tr_at[r]), OK);   /* no access yet */
    }
    /* 100 TiB reserved: the PML4 and the mapping structs, nothing else. */
    KT_EQ(aspace_mapping_count(tr_as), TR_N);
    KT_EQ(job_used(tr_job, JOB_LIMIT_PAGES), 1 + structs_pages(TR_N));
    KT_EQ(aspace_pt_pages(tr_as), 0);

    tr_bad = tr_most = 0;
    struct thread *t[TR_THREADS];
    for (uint32_t i = 0; i < TR_THREADS; i++) {
        tr_touched[i] = tr_refused[i] = 0;
        cpumask_t m;
        cpumask_one(&m, i % cpu_count);
        t[i] = thread_create_on("tib-touch", tr_worker, (void *)(uintptr_t)i, PRIO_DEFAULT, &m);
    }
    uint64_t touched = 0;
    for (uint32_t i = 0; i < TR_THREADS; i++) {
        thread_join(t[i]);
        touched += tr_touched[i];
        KT_EQ(tr_refused[i], TR_REFUSALS);
    }
    kprintf("ktest %s: %lu pages a GiB apart touched before the job refused, %lu pages "
            "charged at most (limit %lu)\n",
            ktest_current, touched, tr_most, (uint64_t)TR_LIMIT);
    KT_EQ(tr_bad, 0);
    KT_ASSERT(tr_most <= TR_LIMIT);
    KT_ASSERT(touched > 100);   /* (about 5 pages each: page, leaf, mid, PT, PD) */
    /* The machine paid the job's pages and the records (VMO and mapping
     * structs on the heap, a few pages), nothing more. */
    KT_GLOBAL_ASSERT((int64_t)(free0 - kt_free_and_cached_pages()) <= (int64_t)TR_LIMIT + 128);
    int64_t maps = aspace_check(tr_as);
    KT_ASSERT(maps >= TR_N);

    /* Decommit everything: the pages go, quickly (the tables stay). */
    uint64_t slowest = 0;
    for (unsigned r = 0; r < TR_N; r++) {
        uint64_t t0 = uptime_ns();
        KT_EQ(vmo_decommit(tr_vmo[r], 0, TIB), OK);
        uint64_t ns = uptime_ns() - t0;
        slowest = ns > slowest ? ns : slowest;
        KT_EQ(vmo_committed(tr_vmo[r]), 0);
    }
    kprintf("ktest %s: the slowest 1 TiB decommit took %lu us\n", ktest_current,
            (uint64_t)(slowest / NS_PER_US));
    KT_EQ(aspace_check(tr_as), maps);   /* no entry left names a page */
    /* No access again: one mapping each, the page tables let go. */
    for (unsigned r = 0; r < TR_N; r++)
        KT_EQ(aspace_protect(tr_as, tr_at[r], TIB, 0), OK);
    KT_EQ(aspace_mapping_count(tr_as), TR_N);
    KT_EQ(aspace_pt_pages(tr_as), 0);
    /* A shrink to nothing takes the VMOs' tables: back to the start. */
    for (unsigned r = 0; r < TR_N; r++) {
        KT_EQ(vmo_set_size(tr_vmo[r], 0), OK);
        KT_EQ(vmo_set_size(tr_vmo[r], TIB), OK);
    }
    KT_EQ(job_used(tr_job, JOB_LIMIT_PAGES), 1 + structs_pages(TR_N));
    aspace_unref(tr_as);
    for (unsigned r = 0; r < TR_N; r++)
        put(tr_vmo[r]);
    kt_job_is_empty(tr_job);
    job_unref(tr_job);
}

/* ---- a storm of protects over 60,000 mappings --------------------------------- */

#define PS_PAGES   (1u << 17)        /* the reservation: 512 MiB */
#define PS_SPLIT   60000u            /* pages alternating read and no access */
#define PS_THREADS 8                 /* protectors; one unmapper and one decommitter more */
#define PS_BASE    (1ull << 40)

static struct aspace *ps_as;
static struct vmo *ps_vmo;
static volatile int ps_stop;
static uint64_t ps_ops[PS_THREADS + 2], ps_bad, ps_bad_st;

static void ps_bad_status(status_t st)
{
    __atomic_store_n(&ps_bad_st, (uint64_t)(int64_t)st, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ps_bad, 1, __ATOMIC_RELAXED);
}

static uint64_t ps_range(uint64_t *rng, uint64_t most, uint64_t *n)
{
    uint64_t x = kt_rng(rng);
    *n = 1 + (x >> 40) % most;
    uint64_t p = x % (PS_SPLIT + 4096);   /* mostly in the split part */
    if (p + *n > PS_PAGES)
        *n = PS_PAGES - p;
    return p;
}

static void ps_protector(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    uint64_t rng = 0x9e3779b97f4a7c15ull * (id + 1);
    static const unsigned perms[3] = { 0, R, RW };
    while (!__atomic_load_n(&ps_stop, __ATOMIC_ACQUIRE)) {
        uint64_t n, p = ps_range(&rng, 8, &n);
        status_t st = aspace_protect(ps_as, PS_BASE + p * PG, n * PG, perms[kt_rng(&rng) % 3]);
        if (st != OK && st != ERR_NOT_FOUND && st != ERR_NO_RESOURCES)
            ps_bad_status(st);
        if (ps_ops[id]++ % 8 == 0) {
            st = aspace_fault(ps_as, PS_BASE + p * PG, kt_rng(&rng) % 2 ? ASPACE_WRITE : R);
            if (st != OK && st != ERR_ACCESS_DENIED && st != ERR_NOT_FOUND)
                ps_bad_status(st);
        }
    }
}

/* Unmaps a piece, then maps it again (or replaces a piece whole). */
static void ps_unmapper(void *arg)
{
    (void)arg;
    uint64_t rng = 0x51ed;
    while (!__atomic_load_n(&ps_stop, __ATOMIC_ACQUIRE)) {
        uint64_t n, p = ps_range(&rng, 64, &n);
        uint64_t a = PS_BASE + p * PG;
        status_t st = OK;
        if (kt_rng(&rng) % 2) {
            st = aspace_unmap(ps_as, a, n * PG);
            if (st != OK && st != ERR_NOT_FOUND && st != ERR_NO_RESOURCES)
                ps_bad_status(st);
            if (st == ERR_NO_RESOURCES)
                continue;   /* nothing changed: the cap */
        }
        st = aspace_map(ps_as, ps_vmo, p * PG, n * PG, REPL | CANRW | (kt_rng(&rng) % 2 ? R : 0),
                        &a);
        if (st != OK && st != ERR_NO_RESOURCES)
            ps_bad_status(st);
        ps_ops[PS_THREADS]++;
    }
}

/* Takes pages out from under the mappings, and snapshots pieces of the VMO
 * (a child's creation write-protects the entries over what it shares). */
static void ps_decommitter(void *arg)
{
    (void)arg;
    uint64_t rng = 0xdec0;
    while (!__atomic_load_n(&ps_stop, __ATOMIC_ACQUIRE)) {
        uint64_t n, p = ps_range(&rng, 512, &n);
        status_t st = vmo_decommit(ps_vmo, p * PG, n * PG);
        if (st != OK)
            ps_bad_status(st);
        if (ps_ops[PS_THREADS + 1]++ % 16 == 0) {
            struct vmo *c;
            st = vmo_create_child(NULL, ps_vmo, VMO_CHILD_SNAPSHOT, p * PG, n * PG, &c);
            if (st == OK)
                put(c);
            else
                ps_bad_status(st);
        }
    }
}

KTEST(aspace_hostile_protect_storm)
{
    struct job *j = kt_fresh_job();
    KT_EQ(aspace_create_charged(j, &ps_as), OK);
    KT_EQ(vmo_create(PS_PAGES * PG, 0, &ps_vmo), OK);   /* charged to nobody: j holds the rest */
    uint64_t b = PS_BASE;
    KT_EQ(aspace_map(ps_as, ps_vmo, 0, PS_PAGES * PG, FIX | CANRW, &b), OK);
    for (uint64_t p = 0; p < PS_SPLIT; p += 2)
        KT_EQ(aspace_protect(ps_as, PS_BASE + p * PG, PG, R), OK);
    KT_EQ(aspace_mapping_count(ps_as), PS_SPLIT);
    KT_EQ(aspace_check(ps_as), PS_SPLIT);

    ps_stop = 0;
    ps_bad = ps_bad_st = 0;
    struct thread *t[PS_THREADS + 2];
    for (uint32_t i = 0; i < PS_THREADS + 2; i++) {
        ps_ops[i] = 0;
        cpumask_t m;
        cpumask_one(&m, i % cpu_count);
        void (*fn)(void *) = i < PS_THREADS ? ps_protector
                             : i == PS_THREADS ? ps_unmapper
                                               : ps_decommitter;
        t[i] = thread_create_on("as-storm", fn, (void *)(uintptr_t)i, PRIO_DEFAULT, &m);
    }
    thread_sleep_ms(2000);
    __atomic_store_n(&ps_stop, 1, __ATOMIC_RELEASE);
    uint64_t protects = 0;
    for (uint32_t i = 0; i < PS_THREADS + 2; i++) {
        thread_join(t[i]);
        if (i < PS_THREADS)
            protects += ps_ops[i];
    }
    int64_t n = aspace_check(ps_as);
    kprintf("ktest %s: %lu protects, %lu unmaps and replaces, %lu decommits; %ld mappings "
            "at the end\n",
            ktest_current, protects, ps_ops[PS_THREADS], ps_ops[PS_THREADS + 1], n);
    if (ps_bad)
        kprintf("ktest %s: %lu calls answered %ld\n", ktest_current, ps_bad, (int64_t)ps_bad_st);
    KT_EQ(ps_bad, 0);
    KT_ASSERT(n > 0 && n <= VMAR_MAX_MAPPINGS);
    KT_EQ(vmo_rmap_check(ps_vmo), n);   /* every mapping is of this VMO */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 1 + aspace_pt_pages(ps_as) + structs_pages((uint64_t)n));
    /* (At 4 CPUs the decommitter, never sleeping, preempts whoever holds
     * the region lock, and the protectors queue behind it: a few thousand
     * in the 2 s; at 8, a hundred thousand.) */
    KT_IDLE_ASSERT(protects > 500);

    KT_EQ(aspace_unmap(ps_as, USER_BASE, USER_TOP - USER_BASE), OK);
    KT_EQ(aspace_mapping_count(ps_as), 0);
    KT_EQ(vmo_rmap_check(ps_vmo), 0);
    aspace_unref(ps_as);
    kt_job_is_empty(j);
    job_unref(j);
    KT_EQ(vmo_kobject(ps_vmo)->refs, 1);
    put(ps_vmo);
}

/* ---- many mappings over the same pages (CR1 finding 1) ------------------------ */

#define OV_SPACES 64   /* address spaces mapping one VMO, as 64 processes would */
#define OV_WIDE   4    /* mappings of the whole 2 MiB in each: 256 in all */

static void batches_reset(void)
{
    __atomic_store_n(&vmo_batch_ns_most, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&vmo_batch_cost_most, 0, __ATOMIC_RELAXED);
}

static uint64_t batch_us(void)
{
    return __atomic_load_n(&vmo_batch_ns_most, __ATOMIC_RELAXED) / NS_PER_US;
}

static uint64_t batch_cost(void)
{
    return __atomic_load_n(&vmo_batch_cost_most, __ATOMIC_RELAXED);
}

/* n mappings of v's pages [0, pages) in as by first fit, every page
 * touched (written if rw); *last: the last one's address. */
static void ov_map(struct aspace *as, struct vmo *v, uint64_t pages, unsigned n, bool rw,
                   uint64_t *last)
{
    for (unsigned i = 0; i < n; i++) {
        uint64_t a = 0;
        KT_EQ(aspace_map(as, v, 0, pages * PG, rw ? RW | CANRW : R | ASPACE_CAN_READ, &a), OK);
        for (uint64_t p = 0; p < pages; p++)
            KT_EQ(aspace_fault(as, a + p * PG, rw ? ASPACE_WRITE : ASPACE_READ), OK);
        *last = a;
    }
}

KTEST(aspace_hostile_overlap_decommit)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *v;
    KT_EQ(vmo_create(512 * PG, 0, &v), OK);

    /* One address space holds VMAR_MAX_OVERLAP mappings of page 0 and no
     * more, by first fit, fixed or a range over it. A replace of one by
     * the same page fits (the one it unmaps doesn't count), page 1 fits,
     * and page 0 again once one is gone. */
    uint64_t first = 0, a = 0, b = 0;
    ov_map(as, v, 1, 1, false, &first);
    ov_map(as, v, 1, VMAR_MAX_OVERLAP - 1, false, &a);
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_CAN_READ, &b), ERR_NO_RESOURCES);
    KT_EQ(aspace_map(as, v, 0, 512 * PG, R | ASPACE_CAN_READ, &b), ERR_NO_RESOURCES);
    b = 1ull << 40;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_CAN_READ | FIX, &b), ERR_NO_RESOURCES);
    KT_EQ(aspace_map(as, v, PG, PG, R | ASPACE_CAN_READ | FIX, &b), OK);
    b = first + PG;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_CAN_READ | REPL, &b), OK);
    KT_EQ(aspace_check(as), VMAR_MAX_OVERLAP + 1);
    KT_EQ(aspace_unmap(as, first, PG), OK);
    b = 0;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_CAN_READ, &b), OK);
    KT_EQ(b, first);   /* the lowest hole */
    batches_reset();
    KT_EQ(vmo_decommit(v, 0, PG), OK);
    uint64_t one_us = batch_us();
    KT_EQ(aspace_pte(as, a), 0);
    KT_EQ(aspace_unmap(as, USER_BASE, USER_TOP - USER_BASE), OK);

    /* At the cap over all 512 pages, every page written: a child's
     * write-protects in batches within the budget, then the decommit in
     * one batch. */
    ov_map(as, v, 512, VMAR_MAX_OVERLAP, true, &a);
    struct vmo *c;
    batches_reset();
    KT_EQ(vmo_create_child(NULL, v, VMO_CHILD_SNAPSHOT, 0, 512 * PG, &c), OK);
    uint64_t wchild_us = batch_us(), wchild_cost = batch_cost();
    KT_GLOBAL_ASSERT(wchild_cost > VMO_BATCH_BUDGET / 2 && wchild_cost <= VMO_BATCH_BUDGET);
    put(c);
    batches_reset();
    KT_EQ(vmo_decommit(v, 0, 512 * PG), OK);
    uint64_t wide_us = batch_us(), wide_cost = batch_cost();
    KT_GLOBAL_EQ(wide_cost,
                 VMAR_MAX_OVERLAP * (VMO_BATCH_MAPPING_COST + 512 * VMO_BATCH_CLEAR_COST));
    KT_EQ(aspace_check(as), VMAR_MAX_OVERLAP);   /* (and no entry is left) */
    KT_EQ(vmo_committed(v), 0);
    aspace_unref(as);

    /* OV_SPACES address spaces, each with VMAR_MAX_OVERLAP mappings of one
     * page: one batch over all of them, whatever it costs (the cap per
     * address space is what bounds it). Then OV_WIDE mappings of v's 2 MiB
     * in each, all written: a child's write-protects and a decommit, each
     * in batches within the budget. */
    struct aspace *sp[OV_SPACES];
    struct vmo *p;
    KT_EQ(vmo_create(PG, 0, &p), OK);
    for (unsigned i = 0; i < OV_SPACES; i++) {
        KT_EQ(aspace_create(&sp[i]), OK);
        ov_map(sp[i], p, 1, VMAR_MAX_OVERLAP, false, &a);
        ov_map(sp[i], v, 512, OV_WIDE, true, &b);
    }
    batches_reset();
    KT_EQ(vmo_decommit(p, 0, PG), OK);
    uint64_t many_us = batch_us(), many_cost = batch_cost();
    KT_GLOBAL_EQ(many_cost,
                 OV_SPACES * VMAR_MAX_OVERLAP * (VMO_BATCH_MAPPING_COST + VMO_BATCH_CLEAR_COST));
    KT_EQ(aspace_pte(sp[OV_SPACES - 1], a), 0);
    batches_reset();
    KT_EQ(vmo_create_child(NULL, v, VMO_CHILD_SNAPSHOT, 0, 512 * PG, &c), OK);
    uint64_t child_us = batch_us(), child_cost = batch_cost();
    KT_GLOBAL_ASSERT(child_cost > VMO_BATCH_BUDGET / 2 && child_cost <= VMO_BATCH_BUDGET);
    put(c);
    batches_reset();
    KT_EQ(vmo_decommit(v, 0, 512 * PG), OK);
    uint64_t spaces_us = batch_us(), spaces_cost = batch_cost();
    KT_GLOBAL_ASSERT(spaces_cost > VMO_BATCH_BUDGET / 2 && spaces_cost <= VMO_BATCH_BUDGET);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(aspace_pte(sp[0], b + 511 * PG), 0);
    KT_EQ(aspace_check(sp[0]), VMAR_MAX_OVERLAP + OV_WIDE);
    kprintf("ktest %s: the longest batch (VMO lock held, interrupts off) over %u mappings of one "
            "page %lu us, of 512 pages %lu us (a child), %lu us (decommit); %u address spaces: "
            "%u mappings of one page %lu us (no smaller batch), %u of 512 pages %lu us (a "
            "child), %lu us (decommit)\n",
            ktest_current, VMAR_MAX_OVERLAP, one_us, wchild_us, wide_us, (unsigned)OV_SPACES,
            OV_SPACES * VMAR_MAX_OVERLAP, many_us, (unsigned)(OV_SPACES * OV_WIDE), child_us,
            spaces_us);
    for (unsigned i = 0; i < OV_SPACES; i++)
        aspace_unref(sp[i]);
    put(p);
    put(v);
}
