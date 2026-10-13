/* The VMO reverse map (object/vmo.c, an interval tree of a VMO's user
 * mappings by page range) at scale and against a model.
 *
 *   vmo_rmap_model
 *       three address spaces map, unmap, protect and fault pieces of one
 *       VMO at random (many over the same pages, so ranges pile up and
 *       share starts); after every step the reverse map is a sound tree
 *       holding exactly the mappings there are. Every user of the reverse
 *       map is checked against a page-by-page model of the windows: a
 *       decommit (across leaf tables too), a shrink, a copy-on-write
 *       child's creation and a write that separates a shared page each
 *       change exactly the entries of the pages they touch, in every
 *       address space, and leave every other entry as it was.
 *   vmo_rmap_decommit_flat
 *       a 10,240-page reservation split into 10,240 mappings (alternating
 *       read-write and read-only pages): a one-page decommit, and a
 *       copy-on-write child of one page, cost about the same as with the
 *       reservation in 64 mappings: timed, with a generous bound, on an
 *       idle machine only (the numbers are printed either way). Each
 *       one-page decommit clears that page's entry and no other.
 * (The pager's invalidate at scale: pager_invalidate_many_mappings, in
 * test_pager.c.) */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/time.h>
#include <jam/vmo.h>

#define PG    PAGE_SIZE
#define R     ASPACE_READ
#define RW    (ASPACE_READ | ASPACE_WRITE)
#define CANRW (ASPACE_CAN_READ | ASPACE_CAN_WRITE)
#define FIX   ASPACE_FIXED
#define REPL  (ASPACE_FIXED | ASPACE_REPLACE)

#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_ADDR 0x000ffffffffff000ull

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static uint64_t pte_pa(struct aspace *as, uint64_t va)
{
    uint64_t e = aspace_pte(as, va);
    return (e & PTE_P) ? (e & PTE_ADDR) : 0;
}

/* ---- against a model ----------------------------------------------------- */

#define MD_AS    3
#define MD_SLOTS 256    /* pages in each address space's window */
#define MD_PAGES 1200   /* the VMO: past two leaf tables (512 pages each) */
#define MD_STEPS 3000
#define MD_NONE  (-1)

/* The model: which VMO page each slot of each window maps (MD_NONE: no
 * mapping) and with what permissions; the entries as they were before the
 * VMO operation under test. */
struct md {
    struct aspace *as[MD_AS];
    struct vmo    *v;
    struct vmo    *child;                  /* a copy-on-write child, or NULL */
    uint64_t       child_lo, child_hi;     /* the pages of v it was made of */
    int32_t        page[MD_AS][MD_SLOTS];
    uint8_t        perm[MD_AS][MD_SLOTS];
    uint64_t       pte[MD_AS][MD_SLOTS];
    uint64_t       rng;
};

static struct md md;

static uint64_t md_va(unsigned i, unsigned s)
{
    return 0x40000000ull + (uint64_t)i * 0x10000000ull + (uint64_t)s * PG;
}

static uint64_t md_rand(uint64_t n)
{
    return kt_rng(&md.rng) % n;
}

static unsigned md_perm(void)
{
    static const unsigned perms[3] = { 0, R, RW };
    return perms[md_rand(3)];
}

/* A random run of slots [*s, *s + *n) of one window. */
static void md_slots(unsigned *s, unsigned *n)
{
    *s = (unsigned)md_rand(MD_SLOTS);
    unsigned room = MD_SLOTS - *s;
    *n = 1 + (unsigned)md_rand(room < 16 ? room : 16);
}

/* Whether every slot of the run is mapped (all), or any is (!all). */
static bool md_mapped(unsigned i, unsigned s, unsigned n, bool all)
{
    for (unsigned k = s; k < s + n; k++)
        if ((md.page[i][k] != MD_NONE) != all)
            return !all;
    return all;
}

static void md_map(void)
{
    unsigned i = (unsigned)md_rand(MD_AS), s, n;
    md_slots(&s, &n);
    /* Often among the first pages, so many mappings cover the same ones. */
    uint64_t off = md_rand(2) ? md_rand(64) : md_rand(MD_PAGES - n + 1);
    unsigned perm = md_perm();
    uint64_t a = md_va(i, s);
    KT_EQ(aspace_map(md.as[i], md.v, off * PG, n * PG, perm | CANRW | REPL, &a), OK);
    for (unsigned k = 0; k < n; k++) {
        md.page[i][s + k] = (int32_t)(off + k);
        md.perm[i][s + k] = (uint8_t)perm;
    }
}

static void md_unmap(void)
{
    unsigned i = (unsigned)md_rand(MD_AS), s, n;
    md_slots(&s, &n);
    status_t want = md_mapped(i, s, n, false) ? OK : ERR_NOT_FOUND;
    KT_EQ(aspace_unmap(md.as[i], md_va(i, s), n * PG), want);
    for (unsigned k = s; k < s + n; k++)
        md.page[i][k] = MD_NONE;
}

static void md_protect(void)
{
    unsigned i = (unsigned)md_rand(MD_AS), s, n;
    md_slots(&s, &n);
    unsigned perm = md_perm();
    bool all = md_mapped(i, s, n, true);
    KT_EQ(aspace_protect(md.as[i], md_va(i, s), n * PG, perm), all ? OK : ERR_NOT_FOUND);
    for (unsigned k = s; all && k < s + n; k++)
        md.perm[i][k] = (uint8_t)perm;
}

static void md_fault(void)
{
    unsigned i = (unsigned)md_rand(MD_AS), s = (unsigned)md_rand(MD_SLOTS);
    unsigned perm = md.perm[i][s];
    unsigned access = (perm & ASPACE_WRITE) && md_rand(2) ? ASPACE_WRITE : ASPACE_READ;
    status_t want = md.page[i][s] == MD_NONE ? ERR_NOT_FOUND
                    : perm == 0              ? ERR_ACCESS_DENIED
                                             : OK;
    KT_EQ(aspace_fault(md.as[i], md_va(i, s), access), want);
}

static void md_snapshot(void)
{
    for (unsigned i = 0; i < MD_AS; i++)
        for (unsigned s = 0; s < MD_SLOTS; s++)
            md.pte[i][s] = aspace_pte(md.as[i], md_va(i, s));
}

static bool md_in(unsigned i, unsigned s, uint64_t lo, uint64_t hi)
{
    int32_t p = md.page[i][s];
    return p != MD_NONE && (uint64_t)p >= lo && (uint64_t)p < hi;
}

/* After a decommit or shrink of pages [lo, hi): their entries are gone in
 * every window, every other entry is as it was. */
static void md_cleared(uint64_t lo, uint64_t hi)
{
    for (unsigned i = 0; i < MD_AS; i++)
        for (unsigned s = 0; s < MD_SLOTS; s++)
            KT_EQ(aspace_pte(md.as[i], md_va(i, s)), md_in(i, s, lo, hi) ? 0 : md.pte[i][s]);
}

/* A random run of VMO pages [*lo, *hi): mostly short, sometimes long
 * enough to cross a leaf table. */
static void md_pages(uint64_t *lo, uint64_t *hi)
{
    *lo = md_rand(MD_PAGES);
    uint64_t n = 1 + md_rand(md_rand(4) ? 8 : 600);
    *hi = *lo + n < MD_PAGES ? *lo + n : MD_PAGES;
}

static void md_decommit(void)
{
    uint64_t lo, hi;
    md_pages(&lo, &hi);
    md_snapshot();
    KT_EQ(vmo_decommit(md.v, lo * PG, (hi - lo) * PG), OK);
    md_cleared(lo, hi);
}

static void md_shrink(void)
{
    uint64_t end = md_rand(MD_PAGES);
    md_snapshot();
    KT_EQ(vmo_set_size(md.v, end * PG), OK);
    md_cleared(end, MD_PAGES);
    KT_EQ(vmo_set_size(md.v, MD_PAGES * PG), OK);
}

/* A new child of pages [lo, hi) (the old one goes): the entries of those
 * pages lose their write bit and nothing else changes; every other entry
 * is as it was. */
static void md_child(void)
{
    if (md.child)
        put(md.child);
    md_pages(&md.child_lo, &md.child_hi);
    md_snapshot();
    KT_EQ(vmo_create_child(NULL, md.v, VMO_CHILD_SNAPSHOT, md.child_lo * PG,
                           (md.child_hi - md.child_lo) * PG, &md.child),
          OK);
    for (unsigned i = 0; i < MD_AS; i++)
        for (unsigned s = 0; s < MD_SLOTS; s++) {
            uint64_t was = md.pte[i][s];
            bool in = md_in(i, s, md.child_lo, md.child_hi);
            KT_EQ(aspace_pte(md.as[i], md_va(i, s)), in ? was & ~PTE_W : was);
        }
}

/* A writable slot mapping one of the child's pages, from a random start:
 * false if there is none. */
static bool md_cow_slot(unsigned *ri, unsigned *rs)
{
    unsigned i0 = (unsigned)md_rand(MD_AS), s0 = (unsigned)md_rand(MD_SLOTS);
    for (unsigned k = 0; k < MD_AS * MD_SLOTS; k++) {
        unsigned i = (i0 + (s0 + k) / MD_SLOTS) % MD_AS, s = (s0 + k) % MD_SLOTS;
        if ((md.perm[i][s] & ASPACE_WRITE) && md_in(i, s, md.child_lo, md.child_hi)) {
            *ri = i;
            *rs = s;
            return true;
        }
    }
    return false;
}

/* A write through a writable slot to one of the child's pages: if v still
 * shares it with the child, the write gives v a copy, and every other
 * entry for that page, in every window, goes; otherwise nothing but the
 * written slot changes. */
static void md_cow(void)
{
    unsigned wi, ws;
    if (!md.child || !md_cow_slot(&wi, &ws))
        return;
    uint64_t p = (uint64_t)md.page[wi][ws];
    uint64_t old = vmo_page_phys(md.v, p * PG);
    bool shared = old && old == vmo_page_phys(md.child, (p - md.child_lo) * PG);
    md_snapshot();
    KT_EQ(aspace_fault(md.as[wi], md_va(wi, ws), ASPACE_WRITE), OK);
    uint64_t now = vmo_page_phys(md.v, p * PG);
    KT_ASSERT(now && (!shared || now != old));
    for (unsigned i = 0; i < MD_AS; i++)
        for (unsigned s = 0; s < MD_SLOTS; s++) {
            uint64_t e = aspace_pte(md.as[i], md_va(i, s));
            if (i == wi && s == ws)
                KT_EQ(e & (PTE_ADDR | PTE_W | PTE_P), now | PTE_W | PTE_P);
            else if (shared && md_in(i, s, p, p + 1))
                KT_EQ(e, 0);
            else
                KT_EQ(e, md.pte[i][s]);
        }
}

/* The reverse map is a sound tree with one entry per mapping. */
static void md_rmap_ok(void)
{
    int64_t maps = 0;
    for (unsigned i = 0; i < MD_AS; i++)
        maps += aspace_mapping_count(md.as[i]);
    KT_EQ(vmo_rmap_check(md.v), maps);
}

static void md_step(void)
{
    uint64_t r = md_rand(100);
    if (r < 20)
        md_map();
    else if (r < 30)
        md_unmap();
    else if (r < 55)
        md_protect();
    else if (r < 75)
        md_fault();
    else if (r < 85)
        md_decommit();
    else if (r < 89)
        md_child();
    else if (r < 95)
        md_cow();
    else
        md_shrink();
    md_rmap_ok();
}

KTEST(vmo_rmap_model)
{
    md.rng = 0x6a09e667f3bcc909ull;
    KT_EQ(vmo_create(MD_PAGES * PG, 0, &md.v), OK);
    md.child = NULL;
    for (unsigned i = 0; i < MD_AS; i++) {
        KT_EQ(aspace_create(&md.as[i]), OK);
        for (unsigned s = 0; s < MD_SLOTS; s++)
            md.page[i][s] = MD_NONE;
    }
    for (unsigned k = 0; k < MD_STEPS; k++)
        md_step();
    int64_t left = vmo_rmap_check(md.v);
    kprintf("ktest %s: %u steps, %ld mappings at the end\n", ktest_current, MD_STEPS, left);
    KT_ASSERT(left > 0);
    for (unsigned i = 0; i < MD_AS; i++)
        aspace_unref(md.as[i]);
    KT_EQ(vmo_rmap_check(md.v), 0);
    if (md.child)
        put(md.child);
    KT_EQ(vmo_kobject(md.v)->refs, 1);
    put(md.v);
}

/* ---- the cost stays flat ----------------------------------------------------- */

#define FLAT_PAGES  10240u   /* the reservation, and the most mappings it is split into */
#define FLAT_ROUNDS 2000u    /* one-page decommits timed */
#define FLAT_KIDS   500u     /* one-page children timed */
#define FLAT_BASE   0x100000000ull
#define FLAT_PAGE   5120u    /* the page timed: read-write in both splits */

struct cost {
    uint64_t decommit_ns, decommit_max_ns, child_ns;
};

/* v's FLAT_PAGES pages mapped at FLAT_BASE in `pieces` mappings, runs of
 * equal length alternately read-write and read-only. */
static struct aspace *split_reservation(struct vmo *v, unsigned pieces)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = FLAT_BASE;
    KT_EQ(aspace_map(as, v, 0, FLAT_PAGES * PG, RW | CANRW | FIX, &a), OK);
    uint64_t run = FLAT_PAGES / pieces;
    for (unsigned i = 1; i < pieces; i += 2)
        KT_EQ(aspace_protect(as, FLAT_BASE + i * run * PG, run * PG, R), OK);
    KT_EQ(aspace_mapping_count(as), pieces);
    return as;
}

/* Write-fault FLAT_PAGE in and decommit it, FLAT_ROUNDS times, timing the
 * decommits (and the slowest: what the VMO lock, interrupts off, is held
 * for at most); then make a child of that page FLAT_KIDS times. The
 * neighbours' entries stay. */
static struct cost measure(struct aspace *as, struct vmo *v)
{
    struct cost c = { 0 };
    uint64_t va = FLAT_BASE + FLAT_PAGE * PG;
    KT_EQ(aspace_fault(as, va - PG, ASPACE_READ), OK);
    KT_EQ(aspace_fault(as, va + PG, ASPACE_READ), OK);
    uint64_t before = aspace_pte(as, va - PG), after = aspace_pte(as, va + PG);
    for (unsigned i = 0; i < FLAT_ROUNDS; i++) {
        KT_EQ(aspace_fault(as, va, ASPACE_WRITE), OK);
        KT_ASSERT(pte_pa(as, va) != 0);
        uint64_t t0 = uptime_ns();
        KT_EQ(vmo_decommit(v, FLAT_PAGE * PG, PG), OK);
        uint64_t dt = uptime_ns() - t0;
        c.decommit_ns += dt;
        if (dt > c.decommit_max_ns)
            c.decommit_max_ns = dt;
        KT_EQ(aspace_pte(as, va), 0);
    }
    KT_EQ(aspace_pte(as, va - PG), before);
    KT_EQ(aspace_pte(as, va + PG), after);
    KT_EQ(aspace_fault(as, va, ASPACE_WRITE), OK);
    for (unsigned i = 0; i < FLAT_KIDS; i++) {
        struct vmo *kid;
        uint64_t t0 = uptime_ns();
        KT_EQ(vmo_create_child(NULL, v, VMO_CHILD_SNAPSHOT, FLAT_PAGE * PG, PG, &kid), OK);
        c.child_ns += uptime_ns() - t0;
        KT_EQ(aspace_pte(as, va) & PTE_W, 0);   /* shared now: read-only */
        put(kid);
        KT_EQ(aspace_fault(as, va, ASPACE_WRITE), OK);
    }
    return c;
}

static uint64_t us(uint64_t ns)
{
    return ns / NS_PER_US;
}

KTEST(vmo_rmap_decommit_flat)
{
    struct vmo *v;
    KT_EQ(vmo_create(FLAT_PAGES * PG, 0, &v), OK);
    struct aspace *few = split_reservation(v, 64);
    (void)measure(few, v);   /* warm: caches, the heap's magazines */
    struct cost small = measure(few, v);
    aspace_unref(few);
    struct aspace *many = split_reservation(v, FLAT_PAGES);
    KT_EQ(vmo_rmap_check(v), FLAT_PAGES);
    struct cost big = measure(many, v);
    kprintf("ktest %s: %u one-page decommits %lu us with 64 mappings (slowest %lu ns), "
            "%lu us with %u (slowest %lu ns); %u one-page children %lu us, %lu us\n",
            ktest_current, FLAT_ROUNDS, us(small.decommit_ns), small.decommit_max_ns,
            us(big.decommit_ns), FLAT_PAGES, big.decommit_max_ns, FLAT_KIDS, us(small.child_ns),
            us(big.child_ns));
    /* O(log n): a list walked 10,240 entries each time, about a hundred
     * times the work here; 3 times plus 3 ms allows for an idle VM's
     * noise. */
    KT_IDLE_ASSERT(big.decommit_ns <= 3 * small.decommit_ns + 3 * NS_PER_MS);
    KT_IDLE_ASSERT(big.child_ns <= 3 * small.child_ns + 3 * NS_PER_MS);
    aspace_unref(many);
    KT_EQ(vmo_rmap_check(v), 0);
    KT_EQ(vmo_kobject(v)->refs, 1);
    put(v);
}
