/* Address spaces at scale (mm/aspace.c): the mapping tree, merging and
 * the cap.
 *
 *   aspace_merge_shapes
 *       neighbours of one VMO at contiguous offsets with the same
 *       permissions and maximum become one mapping after a map (fixed or
 *       first fit), a replace and a protect; a different VMO, offset,
 *       maximum, or a kept mapping, never; a protect in the middle splits
 *       and the opposite protect merges it whole again; a decommit across
 *       a merged boundary clears the entries on both sides (the merged
 *       reverse-map entry covers them); VMO references follow the count.
 *   aspace_protect_storm
 *       50,000 protects alternating read-write and no access over pieces
 *       of one 4,096-page reservation, with a write now and then: the
 *       mapping count is always exactly the number of runs of equal
 *       permissions (checked every 64 protects), the job's charges exact,
 *       and one mapping at the end.
 *   aspace_cap_65536
 *       65,536 mappings fit; one more is ERR_NO_RESOURCES, as are a
 *       protect and an unmap that would split one more off (nothing
 *       changed); a map and a protect that merge what they make fit at the
 *       cap; the job is charged a page per 16; an unmap of 30,000 at once.
 *   aspace_unmap_many
 *       one unmap across 3,801 mappings that starts and ends inside one:
 *       the two ends trimmed, the rest gone, entries and tables with them,
 *       the pages outside untouched.
 *   aspace_replace_big
 *       VMAR_REPLACE inside a 1 GiB reservation with the same VMO and
 *       offsets leaves one mapping; with another VMO three, and back.
 *   aspace_first_fit_holes
 *       first fit among 2,048 mappings with random holes punched in them
 *       lands where a scan of the pages says (the tree's gaps lead the
 *       way), and keeps doing so as the holes fill.
 *   aspace_fault_time_flat
 *       a fault (and a fixed map and unmap) costs about the same with
 *       60,000 mappings below it as with 64: timed, with a generous bound,
 *       on an idle machine only (the numbers are printed either way). */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/vmo.h>

#define PG    PAGE_SIZE
#define R     ASPACE_READ
#define RW    (ASPACE_READ | ASPACE_WRITE)
#define CANRW (ASPACE_CAN_READ | ASPACE_CAN_WRITE)
#define FIX   ASPACE_FIXED
#define KEPT  ASPACE_KEPT_ONLY
#define REPL  (ASPACE_FIXED | ASPACE_REPLACE)

#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_ADDR 0x000ffffffffff000ull

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static struct vmo *new_vmo(uint64_t pages, uint32_t flags)
{
    struct vmo *v;
    KT_EQ(vmo_create(pages * PG, flags, &v), OK);
    return v;
}

static uint64_t pte_pa(struct aspace *as, uint64_t va)
{
    uint64_t e = aspace_pte(as, va);
    return (e & PTE_P) ? (e & PTE_ADDR) : 0;
}

/* Map pages [off, off + n) of v at a (fixed), expecting want. */
static void map_at(struct aspace *as, struct vmo *v, uint64_t off, uint64_t n, unsigned flags,
                   uint64_t a, status_t want)
{
    uint64_t at = a;
    KT_EQ(aspace_map(as, v, off * PG, n * PG, flags | FIX, &at), want);
    if (want == OK)
        KT_EQ(at, a);
}

static void refs_are(struct vmo *v, uint32_t n)
{
    KT_EQ(vmo_kobject(v)->refs, n);
}

/* ---- merging ---------------------------------------------------------------- */

KTEST(aspace_merge_shapes)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *v = new_vmo(16, 0), *w = new_vmo(16, 0), *k = new_vmo(4, VMO_KEEP_PAGES);
    uint64_t b = 0x50000000;
    map_at(as, v, 0, 8, RW | CANRW | ASPACE_CAN_EXEC, b, OK);
    for (unsigned i = 0; i < 8; i++)
        KT_EQ(aspace_fault(as, b + i * PG, ASPACE_WRITE), OK);

    /* A protect in the middle splits; the opposite one merges it whole. */
    KT_EQ(aspace_protect(as, b + 2 * PG, 2 * PG, R), OK);
    KT_EQ(aspace_mapping_count(as), 3);
    KT_EQ(aspace_protect(as, b + 2 * PG, 2 * PG, RW), OK);
    KT_EQ(aspace_mapping_count(as), 1);
    refs_are(v, 2);
    /* Protecting to what it has already splits nothing. */
    KT_EQ(aspace_protect(as, b + 3 * PG, PG, RW), OK);
    KT_EQ(aspace_mapping_count(as), 1);
    /* Merging with one side only: [0,4) RW [4,8) R, then [4,6) RW. */
    KT_EQ(aspace_protect(as, b + 4 * PG, 4 * PG, R), OK);
    KT_EQ(aspace_mapping_count(as), 2);
    KT_EQ(aspace_protect(as, b + 4 * PG, 2 * PG, RW), OK);
    KT_EQ(aspace_mapping_count(as), 2);
    KT_EQ(aspace_fault(as, b + 5 * PG, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(as, b + 6 * PG, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(as, b, 8 * PG, RW), OK);
    KT_EQ(aspace_mapping_count(as), 1);
    for (unsigned i = 0; i < 8; i++)
        KT_EQ(pte_pa(as, b + i * PG), vmo_page_phys(v, i * PG));

    /* A map into a hole merges with both sides when it continues them. */
    KT_EQ(aspace_unmap(as, b + 3 * PG, 2 * PG), OK);
    KT_EQ(aspace_mapping_count(as), 2);
    map_at(as, v, 3, 2, RW | CANRW | ASPACE_CAN_EXEC, b + 3 * PG, OK);
    KT_EQ(aspace_mapping_count(as), 1);
    refs_are(v, 2);
    KT_EQ(aspace_fault(as, b + 4 * PG, ASPACE_READ), OK);
    KT_EQ(pte_pa(as, b + 4 * PG), vmo_page_phys(v, 4 * PG));

    /* Not when anything differs: the maximum, the VMO, the offset. */
    KT_EQ(aspace_unmap(as, b + 3 * PG, 2 * PG), OK);
    map_at(as, v, 3, 2, RW | CANRW, b + 3 * PG, OK);   /* no CAN_EXEC */
    KT_EQ(aspace_mapping_count(as), 3);
    KT_EQ(aspace_unmap(as, b + 3 * PG, 2 * PG), OK);
    map_at(as, w, 3, 2, RW | CANRW | ASPACE_CAN_EXEC, b + 3 * PG, OK);
    KT_EQ(aspace_mapping_count(as), 3);
    KT_EQ(aspace_unmap(as, b + 3 * PG, 2 * PG), OK);
    map_at(as, v, 9, 2, RW | CANRW | ASPACE_CAN_EXEC, b + 3 * PG, OK);
    KT_EQ(aspace_mapping_count(as), 3);
    KT_EQ(aspace_unmap(as, b + 3 * PG, 2 * PG), OK);
    KT_EQ(aspace_mapping_count(as), 2);

    /* A replace merges on both sides, splits them apart and back. */
    map_at(as, w, 3, 2, RW | CANRW | ASPACE_CAN_EXEC, b + 3 * PG, OK);
    map_at(as, v, 2, 4, RW | CANRW | ASPACE_CAN_EXEC | REPL, b + 2 * PG, OK);
    KT_EQ(aspace_mapping_count(as), 1);
    refs_are(w, 1);
    map_at(as, w, 2, 4, RW | CANRW | ASPACE_CAN_EXEC | REPL, b + 2 * PG, OK);
    KT_EQ(aspace_mapping_count(as), 3);
    map_at(as, v, 2, 4, RW | CANRW | ASPACE_CAN_EXEC | REPL, b + 2 * PG, OK);
    KT_EQ(aspace_mapping_count(as), 1);
    refs_are(v, 2);
    refs_are(w, 1);

    /* The merged reverse-map entry covers the whole mapping: a decommit
     * across the old boundaries clears every entry it takes a page of. */
    for (unsigned i = 0; i < 8; i++)
        KT_EQ(aspace_fault(as, b + i * PG, ASPACE_WRITE), OK);
    KT_EQ(vmo_decommit(v, 1 * PG, 6 * PG), OK);
    KT_ASSERT(pte_pa(as, b) != 0);
    KT_ASSERT(pte_pa(as, b + 7 * PG) != 0);
    for (unsigned i = 1; i < 7; i++)
        KT_EQ(pte_pa(as, b + i * PG), 0);
    KT_EQ(aspace_fault(as, b + 5 * PG, ASPACE_READ), OK);
    KT_EQ(pte_pa(as, b + 5 * PG), vmo_page_phys(v, 5 * PG));

    /* First fit puts a continuation right after its neighbour: merged. */
    KT_EQ(aspace_unmap(as, USER_BASE, USER_TOP - USER_BASE), OK);
    uint64_t a1 = 0, a2 = 0;
    KT_EQ(aspace_map(as, v, 0, 4 * PG, R | CANRW, &a1), OK);
    KT_EQ(aspace_map(as, v, 4 * PG, 4 * PG, R | CANRW, &a2), OK);
    KT_EQ(a2, a1 + 4 * PG);
    KT_EQ(aspace_mapping_count(as), 1);

    /* Kept mappings never merge, not even after a protect to the same. */
    uint64_t kb = 0x60000000;
    map_at(as, k, 0, 2, R | KEPT, kb, OK);
    map_at(as, k, 2, 2, R | KEPT, kb + 2 * PG, OK);
    KT_EQ(aspace_mapping_count(as), 3);
    KT_EQ(aspace_protect(as, kb, 4 * PG, R), OK);
    KT_EQ(aspace_mapping_count(as), 3);
    KT_EQ(aspace_protect(as, kb + PG, PG, R), OK);   /* splits nothing either */
    KT_EQ(aspace_mapping_count(as), 3);

    aspace_unref(as);
    refs_are(v, 1);
    refs_are(w, 1);
    refs_are(k, 1);
    put(v);
    put(w);
    put(k);
}

/* ---- many protects ---------------------------------------------------------- */

#define STORM_PAGES  4096u
#define STORM_ROUNDS 50000u

/* Runs of equal permissions in the model: the mappings there should be. */
static uint32_t storm_runs(const uint8_t *flags)
{
    uint32_t n = 1;
    for (unsigned i = 1; i < STORM_PAGES; i++)
        n += flags[i] != flags[i - 1];
    return n;
}

static void storm_charges(struct job *j, struct aspace *as)
{
    uint64_t maps = (aspace_mapping_count(as) + ASPACE_MAPPINGS_PER_PAGE - 1) /
                    ASPACE_MAPPINGS_PER_PAGE;
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 1 + aspace_pt_pages(as) + maps);
}

KTEST(aspace_protect_storm)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct vmo *v = new_vmo(STORM_PAGES, 0);   /* charged to nobody: the job holds the rest */
    uint8_t *flags = kzalloc(STORM_PAGES);
    KT_ASSERT(flags);
    uint64_t b = 1ull << 40, rng = 0x5eed;
    map_at(as, v, 0, STORM_PAGES, CANRW, b, OK);   /* a reservation: no access yet */
    uint32_t most = 1;
    for (unsigned r = 0; r < STORM_ROUNDS; r++) {
        uint64_t x = kt_rng(&rng);
        unsigned p = (unsigned)(x % STORM_PAGES), n = 1 + (unsigned)((x >> 20) % 16);
        if (p + n > STORM_PAGES)
            n = STORM_PAGES - p;
        unsigned f = r % 2 ? 0 : RW;   /* commit, decommit, commit, ... */
        KT_EQ(aspace_protect(as, b + p * PG, n * PG, f), OK);
        for (unsigned i = p; i < p + n; i++)
            flags[i] = (uint8_t)f;
        if (f && r % 16 == 0)
            KT_EQ(aspace_fault(as, b + p * PG, ASPACE_WRITE), OK);
        if (r % 64 == 0) {
            uint32_t want = storm_runs(flags), got = aspace_mapping_count(as);
            KT_EQ(got, want);
            most = got > most ? got : most;
            storm_charges(j, as);
        }
    }
    KT_EQ(aspace_mapping_count(as), storm_runs(flags));
    KT_EQ(aspace_protect(as, b, STORM_PAGES * PG, 0), OK);
    KT_EQ(aspace_mapping_count(as), 1);
    storm_charges(j, as);
    kprintf("ktest %s: %u protects, at most %u mappings, 1 at the end\n", ktest_current,
            STORM_ROUNDS, most);
    KT_EQ(aspace_fault(as, b, ASPACE_READ), ERR_ACCESS_DENIED);
    refs_are(v, 2);
    kfree(flags);
    aspace_unref(as);
    kt_job_is_empty(j);
    job_unref(j);
    put(v);
}

/* ---- the cap ---------------------------------------------------------------- */

KTEST(aspace_cap_65536)
{
    KT_EQ(VMAR_MAX_MAPPINGS, 65536);
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct vmo *v = new_vmo(3, 0), *w = new_vmo(3, 0);
    /* Two neighbours that don't merge (other permissions), high up:
     * P = w[0,1) R, Q = w[1,3) RW, both may be read-write. */
    uint64_t y = 1ull << 44;
    map_at(as, w, 0, 1, R | CANRW, y, OK);
    map_at(as, w, 1, 2, RW | CANRW, y + PG, OK);
    /* The rest by first fit, each page 0 of v: none merge. */
    uint64_t a = 0, last = 0;
    for (uint32_t i = 2; i < VMAR_MAX_MAPPINGS; i++) {
        KT_EQ(aspace_map(as, v, 0, PG, R | CANRW, &a), OK);
        last = a;
    }
    KT_EQ(last, USER_BASE + (VMAR_MAX_MAPPINGS - 3) * PG);
    KT_EQ(aspace_mapping_count(as), VMAR_MAX_MAPPINGS);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 1 + VMAR_MAX_MAPPINGS / ASPACE_MAPPINGS_PER_PAGE);

    /* One more is refused; so is a split, by protect or unmap. */
    KT_EQ(aspace_map(as, v, 0, PG, R | CANRW, &a), ERR_NO_RESOURCES);
    KT_EQ(aspace_protect(as, y + 2 * PG, PG, 0), ERR_NO_RESOURCES);
    map_at(as, v, 1, 2, R | CANRW, last + PG, OK);   /* v[1,3) after last's v[0,1): merges */
    KT_EQ(aspace_mapping_count(as), VMAR_MAX_MAPPINGS);
    KT_EQ(aspace_unmap(as, last + PG, PG), ERR_NO_RESOURCES);   /* a hole in [last, +3) */
    KT_EQ(aspace_mapping_count(as), VMAR_MAX_MAPPINGS);
    /* A protect that splits Q and merges its first page into P: no more
     * mappings than before, so it fits at the cap. */
    KT_EQ(aspace_protect(as, y + PG, PG, R), OK);
    KT_EQ(aspace_mapping_count(as), VMAR_MAX_MAPPINGS);
    KT_EQ(aspace_fault(as, y + PG, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, y + 2 * PG, ASPACE_WRITE), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES),
          1 + aspace_pt_pages(as) + VMAR_MAX_MAPPINGS / ASPACE_MAPPINGS_PER_PAGE);

    /* 30,000 at once, then all. */
    KT_EQ(aspace_unmap(as, USER_BASE + 1000 * PG, 30000 * PG), OK);
    KT_EQ(aspace_mapping_count(as), VMAR_MAX_MAPPINGS - 30000);
    KT_EQ(aspace_fault(as, USER_BASE + 1000 * PG, ASPACE_READ), ERR_NOT_FOUND);
    KT_EQ(aspace_fault(as, USER_BASE + 31000 * PG, ASPACE_READ), OK);
    refs_are(v, 1 + VMAR_MAX_MAPPINGS - 30000 - 2);
    KT_EQ(aspace_map(as, v, 0, PG, R | CANRW, &a), OK);   /* room again: the lowest hole */
    KT_EQ(a, USER_BASE + 1000 * PG);
    KT_EQ(aspace_unmap(as, USER_BASE, USER_TOP - USER_BASE), OK);
    KT_EQ(aspace_mapping_count(as), 0);
    refs_are(v, 1);
    refs_are(w, 1);
    aspace_unref(as);
    kt_job_is_empty(j);
    job_unref(j);
    put(v);
    put(w);
}

/* ---- one unmap across many -------------------------------------------------- */

KTEST(aspace_unmap_many)
{
    enum { N = 4000 };
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *v = new_vmo(2 * N, 0);
    uint64_t b = 1ull << 36;
    /* N two-page mappings of one VMO at contiguous offsets; alternate
     * permissions keep them apart. Every page touched. */
    for (unsigned i = 0; i < N; i++)
        map_at(as, v, 2 * i, 2, (i % 2 ? RW : R) | CANRW, b + 2 * i * PG, OK);
    KT_EQ(aspace_mapping_count(as), N);
    for (unsigned i = 0; i < 2 * N; i++)
        KT_EQ(aspace_fault(as, b + i * PG, ASPACE_READ), OK);
    uint64_t tables = aspace_pt_pages(as);

    /* From the second page of mapping 100 to the first of mapping 3900. */
    uint64_t from = b + (2 * 100 + 1) * PG, to = b + (2 * 3900 + 1) * PG;
    KT_EQ(aspace_unmap(as, from, to - from), OK);
    KT_EQ(aspace_mapping_count(as), N - 3799);   /* 101..3899 gone, 100 and 3900 trimmed */
    refs_are(v, 1 + N - 3799);
    for (uint64_t va = b; va < b + 2 * N * PG; va += PG) {
        bool inside = va >= from && va < to;
        KT_EQ(pte_pa(as, va) == 0, inside);
        KT_EQ(aspace_fault(as, va, ASPACE_READ), inside ? ERR_NOT_FOUND : OK);
    }
    KT_EQ(pte_pa(as, from - PG), vmo_page_phys(v, 200 * PG));
    KT_EQ(pte_pa(as, to), vmo_page_phys(v, 7801 * PG));
    KT_ASSERT(aspace_pt_pages(as) < tables);   /* the emptied page tables went */
    KT_EQ(aspace_unmap(as, from, to - from), ERR_NOT_FOUND);
    aspace_unref(as);
    refs_are(v, 1);
    put(v);
}

/* ---- replace inside a big mapping ------------------------------------------- */

KTEST(aspace_replace_big)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t pages = (1ull << 30) / PG;
    struct vmo *v = new_vmo(pages, 0), *w = new_vmo(16, 0);
    uint64_t b = 1ull << 42, mid = b + (pages / 2) * PG;
    map_at(as, v, 0, pages, R | CANRW, b, OK);
    KT_EQ(aspace_fault(as, mid, ASPACE_READ), OK);
    /* MAP_FIXED's decommit pattern: the same VMO, offsets and permissions. */
    map_at(as, v, pages / 2, 16, R | CANRW | REPL, mid, OK);
    KT_EQ(aspace_mapping_count(as), 1);
    KT_EQ(pte_pa(as, mid), 0);   /* the replace cleared it; the next fault brings it back */
    KT_EQ(aspace_fault(as, mid, ASPACE_READ), OK);
    KT_EQ(pte_pa(as, mid), vmo_page_phys(v, (pages / 2) * PG));
    /* Another VMO in the middle: three; then back to one. */
    map_at(as, w, 0, 16, R | CANRW | REPL, mid, OK);
    KT_EQ(aspace_mapping_count(as), 3);
    map_at(as, v, pages / 2, 16, R | CANRW | REPL, mid, OK);
    KT_EQ(aspace_mapping_count(as), 1);
    refs_are(w, 1);
    refs_are(v, 2);
    /* Other permissions: three, and a protect back makes one again. */
    map_at(as, v, pages / 2, 16, RW | CANRW | REPL, mid, OK);
    KT_EQ(aspace_mapping_count(as), 3);
    KT_EQ(aspace_protect(as, mid, 16 * PG, R), OK);
    KT_EQ(aspace_mapping_count(as), 1);
    aspace_unref(as);
    refs_are(v, 1);
    put(v);
    put(w);
}

/* ---- first fit ---------------------------------------------------------------- */

#define FIT_PAGES 2048u

/* The lowest page index from which n pages are free (pages past the
 * window are free). */
static unsigned fit_expect(const uint8_t *used, unsigned n)
{
    for (unsigned i = 0;; i++) {
        unsigned k = 0;
        while (k < n && i + k < FIT_PAGES && !used[i + k])
            k++;
        if (k == n || i + k == FIT_PAGES)
            return i;
        i += k;
    }
}

KTEST(aspace_first_fit_holes)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *v = new_vmo(16, 0);
    uint8_t *used = kzalloc(FIT_PAGES + 16);
    KT_ASSERT(used);
    uint64_t rng = 0xf17;
    /* Every page its own mapping (page 0 of v each time: none merge). */
    for (unsigned i = 0; i < FIT_PAGES; i++) {
        map_at(as, v, 0, 1, R, USER_BASE + i * PG, OK);
        used[i] = 1;
    }
    for (unsigned h = 0; h < 300; h++) {
        uint64_t x = kt_rng(&rng);
        unsigned p = (unsigned)(x % FIT_PAGES), n = 1 + (unsigned)((x >> 16) % 8);
        if (p + n > FIT_PAGES)
            n = FIT_PAGES - p;
        bool any = false;
        for (unsigned i = p; i < p + n; i++) {
            any |= used[i];
            used[i] = 0;
        }
        KT_EQ(aspace_unmap(as, USER_BASE + p * PG, n * PG), any ? OK : ERR_NOT_FOUND);
    }
    for (unsigned t = 0; t < 600; t++) {
        unsigned n = 1 + (unsigned)(kt_rng(&rng) % 10);
        unsigned want = fit_expect(used, n);
        uint64_t a = 0;
        KT_EQ(aspace_map(as, v, 0, n * PG, R, &a), OK);
        KT_EQ(a, USER_BASE + want * PG);
        if (want + n > FIT_PAGES || t % 2) {
            KT_EQ(aspace_unmap(as, a, n * PG), OK);   /* keep the window's model whole */
            continue;
        }
        for (unsigned i = want; i < want + n; i++)
            used[i] = 1;
    }
    kfree(used);
    aspace_unref(as);
    refs_are(v, 1);
    put(v);
}

/* ---- the cost stays flat ---------------------------------------------------- */

#define TIME_FAULTS 20000u
#define TIME_MAPS   2000u

struct cost {
    uint64_t fault_ns, map_ns;
};

static uint64_t us(uint64_t ns)
{
    return ns / NS_PER_US;
}

/* Faults on t's 16 present pages, then fixed maps and unmaps just above
 * it: both look the mapping up where the old sorted list was slowest. */
static struct cost measure(struct aspace *as, struct vmo *v, uint64_t t)
{
    struct cost c;
    uint64_t t0 = uptime_ns();
    for (unsigned i = 0; i < TIME_FAULTS; i++)
        KT_EQ(aspace_fault(as, t + (i % 16) * PG, ASPACE_READ), OK);
    c.fault_ns = uptime_ns() - t0;
    t0 = uptime_ns();
    for (unsigned i = 0; i < TIME_MAPS; i++) {
        map_at(as, v, 0, 1, R, t + 64 * PG, OK);
        KT_EQ(aspace_unmap(as, t + 64 * PG, PG), OK);
    }
    c.map_ns = uptime_ns() - t0;
    return c;
}

KTEST(aspace_fault_time_flat)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *v = new_vmo(16, 0), *f = new_vmo(1, 0);
    uint64_t t = 0x7f0000000000ull, a = 0;
    map_at(as, v, 0, 16, RW, t, OK);
    for (unsigned i = 0; i < 16; i++)
        KT_EQ(aspace_fault(as, t + i * PG, ASPACE_WRITE), OK);
    for (unsigned i = 0; i < 63; i++)
        KT_EQ(aspace_map(as, f, 0, PG, R, &a), OK);
    (void)measure(as, v, t);   /* warm: caches, the heap's magazines */
    struct cost small = measure(as, v, t);
    for (unsigned i = 63; i < 59999; i++)
        KT_EQ(aspace_map(as, f, 0, PG, R, &a), OK);
    KT_EQ(aspace_mapping_count(as), 60000);
    struct cost big = measure(as, v, t);
    kprintf("ktest %s: %u faults %lu us with 64 mappings, %lu us with 60,000; %u maps and "
            "unmaps %lu us, %lu us\n", ktest_current, TIME_FAULTS, us(small.fault_ns),
            us(big.fault_ns), TIME_MAPS, us(small.map_ns), us(big.map_ns));
    /* O(log n): 16 levels against 6. A sorted list would be ~1000 times
     * slower here; 4 times plus 5 ms allows for any noise of an idle VM. */
    KT_IDLE_ASSERT(big.fault_ns <= 4 * small.fault_ns + 5 * NS_PER_MS);
    KT_IDLE_ASSERT(big.map_ns <= 4 * small.map_ns + 5 * NS_PER_MS);
    aspace_unref(as);
    refs_are(v, 1);
    refs_are(f, 1);
    put(v);
    put(f);
}
