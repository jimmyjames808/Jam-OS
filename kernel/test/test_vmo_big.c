/* Tests for big sparse VMOs: a plain paged VMO of up to VMO_MAX_SIZE (128
 * TiB), whose pages past the inline root's 64 GiB are found through the top
 * and upper tables (kernel/object/vmo.c's header). A reservation costs one
 * record until touched; each touch charges its page and exactly the tables
 * it needed (the job's count is checked against a count made here from the
 * page numbers); decommit gives the pages back and leaves the tables; a
 * shrink and destroy give the tables back too; a walk over an untouched TiB
 * is a few looks; a job at its limit is refused at the touch, by vmo_write
 * and by a fault; copy-on-write children of a huge parent share across
 * every level, and one of a sparse parent walks only the tables that
 * exist; and the kinds that stay within 64 GiB (kept, pager, a pager
 * VMO's child, physical) are refused past it.
 *
 * Page counts on the whole machine (free pages) are KT_GLOBAL checks; the
 * job's counts are the test's own. slab_keeper keeps the slab that holds
 * struct vmo alive so creating one costs no page of its own. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/list.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/process.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/vmo.h>

#define PG          PAGE_SIZE
#define TIB         (1ull << 40)
#define GIB         (1ull << 30)
#define SNAP        VMO_CHILD_SNAPSHOT
#define ALOW        VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE
#define ROOT_PAGES  (VMO_SMALL_MAX_SIZE / PG)   /* the inline root's reach, in pages */
#define UPPER_PAGES (512ull * GIB / PG)         /* one upper table's reach */
#define U_A         0x20000000ull               /* where the fault test maps */
#define ANSWER      (600 * NS_PER_S)

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static struct vmo *slab_keeper(void)
{
    struct vmo *k;
    KT_EQ(vmo_create(PG, 0, &k), OK);
    return k;
}

static struct vmo *big_for(struct job *j, uint64_t size)
{
    struct vmo *v;
    KT_EQ(vmo_create_for(j, size, 0, &v), OK);
    return v;
}

static uint64_t pages_of(struct job *j)
{
    return job_used(j, JOB_LIMIT_PAGES);
}

/* Write the word w at the start of page idx of v. */
static status_t poke(struct vmo *v, uint64_t idx, uint64_t w)
{
    return vmo_write(v, idx * PG, &w, sizeof(w));
}

/* The word at the start of page idx of v. */
static uint64_t peek(struct vmo *v, uint64_t idx)
{
    uint64_t w = 0xdeadull;
    KT_EQ(vmo_read(v, idx * PG, &w, sizeof(w)), OK);
    return w;
}

/* ns that decommitting all of v takes. */
static uint64_t decommit_all_ns(struct vmo *v)
{
    uint64_t t0 = uptime_ns();
    KT_EQ(vmo_decommit(v, 0, vmo_size(v)), OK);
    return uptime_ns() - t0;
}

/* ---- a reservation ------------------------------------------------------- */

/* A 1 TiB VMO costs its record (one handle unit, no page) however big it
 * is, reads as zeros anywhere without committing anything, and a decommit
 * of all of it, untouched, takes a few looks: well under a millisecond,
 * and so does one of a whole 128 TiB. */
KTEST(vmo_big_reservation_costs_nothing)
{
    struct job *j = kt_fresh_job();
    struct vmo *keep = slab_keeper();
    uint64_t base = kt_free_pages();
    struct vmo *v = big_for(j, TIB);
    KT_EQ(vmo_size(v), TIB);
    KT_EQ(pages_of(j), 0);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 1);
    KT_GLOBAL_EQ(kt_free_pages(), base);
    KT_EQ(peek(v, 0), 0);
    KT_EQ(peek(v, 700 * GIB / PG), 0);
    KT_EQ(peek(v, TIB / PG - 1), 0);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(pages_of(j), 0);
    uint64_t ns = decommit_all_ns(v);
    kprintf("vmo_big: decommit of an untouched 1 TiB VMO took %lu ns\n", ns);
    KT_IDLE_ASSERT(ns < NS_PER_MS);
    put(v);

    struct vmo *bad;
    KT_EQ(vmo_create(VMO_MAX_SIZE + 1, 0, &bad), ERR_OUT_OF_RANGE);
    v = big_for(j, VMO_MAX_SIZE);
    KT_EQ(vmo_size(v), VMO_MAX_SIZE);
    KT_EQ(pages_of(j), 0);
    ns = decommit_all_ns(v);
    kprintf("vmo_big: decommit of an untouched 128 TiB VMO took %lu ns\n", ns);
    KT_IDLE_ASSERT(ns < NS_PER_MS);
    KT_EQ(vmo_set_size(v, VMO_MAX_SIZE + 1), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_set_size(v, 0), OK);   /* a shrink over nothing: nothing to free */
    KT_EQ(vmo_set_size(v, VMO_MAX_SIZE), OK);
    put(v);
    kt_job_is_empty(j);
    KT_GLOBAL_EQ(kt_free_pages(), base);
    put(keep);
    job_unref(j);
}

/* ---- the edges of the table ---------------------------------------------- */

/* The very last page of a 128 TiB VMO costs itself and four tables (top,
 * upper, mid, leaf); page 0 costs itself and two (the inline root's mid
 * and a leaf) as in any small VMO. A decommit leaves the tables; a shrink
 * back to 64 GiB frees the four past the inline root, and growing again
 * finds zeros there. */
KTEST(vmo_big_top_page)
{
    struct job *j = kt_fresh_job();
    struct vmo *keep = slab_keeper();
    uint64_t base = kt_free_pages();
    struct vmo *v = big_for(j, VMO_MAX_SIZE);
    uint64_t last = VMO_MAX_SIZE / PG - 1;
    uint8_t b = 0x5a;
    KT_EQ(vmo_write(v, VMO_MAX_SIZE - 1, &b, 1), OK);
    KT_EQ(pages_of(j), 1 + 4);
    KT_EQ(vmo_committed(v), PG);
    KT_GLOBAL_EQ(base - kt_free_pages(), 1 + 4);
    b = 0;
    KT_EQ(vmo_read(v, VMO_MAX_SIZE - 1, &b, 1), OK);
    KT_EQ(b, 0x5a);
    KT_EQ(vmo_read(v, VMO_MAX_SIZE, &b, 1), ERR_OUT_OF_RANGE);
    KT_EQ(peek(v, last - 1), 0);   /* its neighbour: no page, same leaf */
    KT_EQ(vmo_committed(v), PG);

    KT_EQ(poke(v, 0, 0x11), OK);
    KT_EQ(pages_of(j), 5 + 1 + 2);
    KT_EQ(vmo_decommit(v, VMO_MAX_SIZE - PG, PG), OK);   /* the tables stay */
    KT_EQ(pages_of(j), 4 + 1 + 2);
    KT_EQ(poke(v, last, 0x22), OK);   /* only the page now */
    KT_EQ(pages_of(j), 5 + 1 + 2);

    KT_EQ(vmo_set_size(v, VMO_SMALL_MAX_SIZE), OK);   /* every table past it goes */
    KT_EQ(pages_of(j), 1 + 2);
    KT_EQ(vmo_committed(v), PG);
    KT_GLOBAL_EQ(base - kt_free_pages(), 1 + 2);
    KT_EQ(vmo_set_size(v, VMO_MAX_SIZE), OK);
    KT_EQ(peek(v, last), 0);
    KT_EQ(peek(v, 0), 0x11);
    KT_EQ(pages_of(j), 1 + 2);
    put(v);
    kt_job_is_empty(j);
    KT_GLOBAL_EQ(kt_free_pages(), base);
    put(keep);
    job_unref(j);
}

/* Commits and decommits that cross from the inline root into the top
 * table (64 GiB) and from one upper table into the next (512 GiB): bytes
 * across each boundary read back, every table is charged once, and a
 * decommit across them gives back exactly the pages. */
KTEST(vmo_big_across_levels)
{
    struct job *j = kt_fresh_job();
    struct vmo *v = big_for(j, TIB);
    uint64_t r = VMO_SMALL_MAX_SIZE;   /* the inline root's end, in bytes */
    KT_EQ(vmo_commit(v, r - 2 * PG, 4 * PG), OK);
    /* root: its last mid and that mid's last leaf; past it: top, upper 0,
     * upper 0's mid 64 and its first leaf. */
    KT_EQ(pages_of(j), 4 + 2 + 4);
    KT_EQ(vmo_committed(v), 4 * PG);

    static uint8_t in[2 * 4096], out[2 * 4096];
    for (unsigned i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)(i * 7 + 3);
    KT_EQ(vmo_write(v, r - PG - 100, in, sizeof(in)), OK);   /* 3 pages, across 64 GiB */
    memset(out, 0, sizeof(out));
    KT_EQ(vmo_read(v, r - PG - 100, out, sizeof(out)), OK);
    KT_ASSERT(!memcmp(in, out, sizeof(in)));
    KT_EQ(pages_of(j), 4 + 2 + 4);   /* all three were committed already */

    uint64_t u = 512 * GIB;   /* upper table 0's end */
    KT_EQ(vmo_write(v, u - 100, in, sizeof(in)), OK);   /* 3 pages, across 512 GiB */
    /* upper 0's mid 511 and its last leaf; upper 1, its mid 0, its leaf 0 */
    KT_EQ(pages_of(j), 10 + 3 + 2 + 3);
    memset(out, 0, sizeof(out));
    KT_EQ(vmo_read(v, u - 100, out, sizeof(out)), OK);
    KT_ASSERT(!memcmp(in, out, sizeof(in)));
    KT_EQ(vmo_committed(v), 7 * PG);

    KT_EQ(vmo_decommit(v, r - 2 * PG, 4 * PG), OK);
    KT_EQ(vmo_committed(v), 3 * PG);
    KT_EQ(pages_of(j), 18 - 4);
    KT_EQ(vmo_decommit(v, u - PG, 2 * PG), OK);
    KT_EQ(vmo_committed(v), PG);   /* the third page, past u + PG */
    KT_EQ(vmo_decommit(v, 0, TIB), OK);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(pages_of(j), 18 - 7);   /* every table stays */
    KT_EQ(peek(v, r / PG), 0);
    KT_EQ(vmo_committed(v), 0);

    /* A shrink that ends inside upper table 0 (at 100 GiB) keeps it, the
     * top and what lies below 100 GiB (the root's mid and leaf, upper 0's
     * mid 64 and its leaf), and frees mid 511 with its leaf, and upper 1
     * with all below it. */
    KT_EQ(vmo_set_size(v, 100 * GIB), OK);
    KT_EQ(pages_of(j), 2 + 4);
    KT_EQ(vmo_set_size(v, 0), OK);
    KT_EQ(pages_of(j), 0);
    put(v);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- many touches -------------------------------------------------------- */

static void sort_u64(uint64_t *a, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint64_t x = a[i];
        unsigned k = i;
        for (; k > 0 && a[k - 1] > x; k--)
            a[k] = a[k - 1];
        a[k] = x;
    }
}

/* Distinct values of a[i] >> shift over the sorted a[from..n). */
static uint64_t distinct(const uint64_t *a, unsigned from, unsigned n, unsigned shift)
{
    uint64_t count = 0;
    for (unsigned i = from; i < n; i++)
        count += i == from || a[i] >> shift != a[i - 1] >> shift;
    return count;
}

/* The tables the pages in sorted a[0..n) need: a leaf per 2 MiB, a mid
 * per GiB, and past the inline root an upper per 512 GiB and the top. */
static uint64_t tables_for(const uint64_t *a, unsigned n)
{
    unsigned big = 0;
    while (big < n && a[big] < ROOT_PAGES)
        big++;
    uint64_t t = distinct(a, 0, n, 9) + distinct(a, 0, n, 18);
    if (big < n)
        t += distinct(a, big, n, 27) + 1;
    return t;
}

#define TOUCHES 1000u

/* 1,000 touches scattered over a 1 TiB VMO (and its edges: the first and
 * last page, both sides of 64 GiB and of 512 GiB) charge the job exactly
 * the pages and the tables they need; every page reads back; a full
 * decommit gives every page back and keeps the tables; destroy leaves the
 * job at zero. */
KTEST(vmo_big_scattered_touches)
{
    struct job *j = kt_fresh_job();
    uint64_t *idx = kmalloc(TOUCHES * sizeof(*idx));
    KT_ASSERT(idx);
    struct vmo *keep = slab_keeper();
    uint64_t base = kt_free_pages();
    const uint64_t n_pages = TIB / PG;
    const uint64_t edges[] = { 0, ROOT_PAGES - 1, ROOT_PAGES, UPPER_PAGES - 1, UPPER_PAGES,
                               n_pages - 1 };
    unsigned n = 0;
    for (; n < sizeof(edges) / sizeof(edges[0]); n++)
        idx[n] = edges[n];
    uint64_t rng = 0x9e3779b97f4a7c15ull;
    for (; n < TOUCHES; n++)
        idx[n] = kt_rng(&rng) % n_pages;

    struct vmo *v = big_for(j, TIB);
    for (unsigned i = 0; i < TOUCHES; i++)
        KT_EQ(poke(v, idx[i], idx[i] ^ 0xb16ull), OK);
    sort_u64(idx, TOUCHES);
    uint64_t pages = distinct(idx, 0, TOUCHES, 0), tables = tables_for(idx, TOUCHES);
    kprintf("vmo_big: %u touches over 1 TiB: %lu pages, %lu table pages\n", TOUCHES, pages,
            tables);
    KT_EQ(vmo_committed(v), pages * PG);
    KT_EQ(pages_of(j), pages + tables);
    KT_GLOBAL_EQ(base - kt_free_pages(), pages + tables);
    for (unsigned i = 0; i < TOUCHES; i++)
        KT_EQ(peek(v, idx[i]), idx[i] ^ 0xb16ull);

    uint64_t ns = decommit_all_ns(v);
    kprintf("vmo_big: decommit of 1 TiB with %lu pages took %lu ns\n", pages, ns);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(pages_of(j), tables);
    for (unsigned i = 0; i < TOUCHES; i += 97)
        KT_EQ(peek(v, idx[i]), 0);
    put(v);
    kt_job_is_empty(j);
    KT_GLOBAL_EQ(kt_free_pages(), base);
    put(keep);
    kfree(idx);
    job_unref(j);
}

/* ---- the job's limit ----------------------------------------------------- */

/* Pages of a 128 TiB VMO in three upper tables far apart: 100, 10 and 50
 * TiB (upper tables 200, 20 and 100). */
#define AT_100T (100 * TIB / PG)
#define AT_10T  (10 * TIB / PG)
#define OFF_50T (50 * TIB)   /* bytes */

/* A job at its limit is refused at the touch, and a refused touch builds
 * nothing: the page is charged before any table. A touch refused for a
 * table after the page was paid for gives the page's charge back; the
 * tables made before the refusal stay, charged (the file header's rule).
 * The same through a fault: a mapping of 4 pages at 50 TiB faults
 * ERR_NO_MEMORY at the limit and commits page and tables once there is
 * room. */
KTEST(vmo_big_job_limit)
{
    struct job *j = kt_fresh_job();
    struct vmo *keep = slab_keeper();
    uint64_t base = kt_free_pages();
    struct vmo *v = big_for(j, VMO_MAX_SIZE);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 0), OK);
    KT_EQ(poke(v, AT_100T, 1), ERR_NO_MEMORY);
    KT_EQ(pages_of(j), 0);
    KT_EQ(vmo_committed(v), 0);
    KT_GLOBAL_EQ(kt_free_pages(), base);

    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 5), OK);
    KT_EQ(poke(v, AT_100T, 1), OK);   /* page, leaf, mid, upper, top */
    KT_EQ(pages_of(j), 5);
    KT_EQ(poke(v, AT_10T, 2), ERR_NO_MEMORY);   /* refused at the page */
    KT_EQ(pages_of(j), 5);
    KT_GLOBAL_EQ(base - kt_free_pages(), 5);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 7), OK);
    KT_EQ(poke(v, AT_10T, 2), ERR_NO_MEMORY);   /* page and upper paid, the mid refused */
    KT_EQ(pages_of(j), 6);                      /* the upper stays */
    KT_EQ(vmo_committed(v), PG);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 9), OK);
    KT_EQ(poke(v, AT_10T, 2), OK);   /* page, mid, leaf */
    KT_EQ(pages_of(j), 9);
    KT_EQ(peek(v, AT_10T), 2);
    KT_EQ(peek(v, AT_100T), 1);

    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);   /* its own tables charged to nobody */
    uint64_t a = U_A;
    KT_EQ(aspace_map(as, v, OFF_50T, 4 * PG, ASPACE_READ | ASPACE_WRITE | ASPACE_FIXED, &a), OK);
    KT_EQ(aspace_fault(as, U_A + PG, ASPACE_WRITE), ERR_NO_MEMORY);
    KT_EQ(pages_of(j), 9);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 13), OK);
    KT_EQ(aspace_fault(as, U_A + PG, ASPACE_WRITE), OK);   /* page, leaf, mid, upper */
    KT_EQ(pages_of(j), 13);
    KT_EQ(vmo_committed(v), 3 * PG);
    KT_ASSERT(vmo_page_phys(v, OFF_50T + PG) != 0);
    KT_EQ(vmo_page_phys(v, OFF_50T), 0);
    KT_EQ(aspace_fault(as, U_A, ASPACE_READ), ERR_NO_MEMORY);   /* a page more: no room */
    KT_EQ(vmo_decommit(v, OFF_50T, 4 * PG), OK);   /* the mapping's entry goes with it */
    KT_EQ(pages_of(j), 12);
    KT_EQ(aspace_fault(as, U_A, ASPACE_READ), OK);
    KT_EQ(pages_of(j), 13);
    aspace_unref(as);
    put(v);
    kt_job_is_empty(j);   /* (the address space's own pages: the harness's leak check) */
    put(keep);
    job_unref(j);
}

/* ---- copy-on-write children ---------------------------------------------- */

/* A child of a whole 1 TiB parent shares its pages on every level (the
 * inline root, upper 0 past 64 GiB, upper 1) and pays for each page it
 * shares and the tables it needs for them; a write separates one page;
 * a child at an offset finds the parent's page past 512 GiB below its own
 * 64 GiB; a 128 TiB child of a 4-page VMO is zeros past them, and its last
 * page can be written. */
KTEST(vmo_big_child)
{
    struct vmo *p;
    KT_EQ(vmo_create(TIB, 0, &p), OK);
    const uint64_t at[] = { 1, ROOT_PAGES + 5, UPPER_PAGES + 7, TIB / PG - 1 };
    for (unsigned i = 0; i < 4; i++)
        KT_EQ(poke(p, at[i], 0x100 + i), OK);

    struct job *j = kt_fresh_job();
    struct vmo *c;
    uint64_t t0 = uptime_ns();
    KT_EQ(vmo_create_child(j, p, SNAP, 0, TIB, &c), OK);
    kprintf("vmo_big: a 1 TiB child of 4 pages made in %lu ns\n", uptime_ns() - t0);
    KT_EQ(vmo_committed(c), 4 * PG);
    /* tables: root mid + leaf; top, upper 0, mid, leaf; upper 1, mid,
     * leaf; another mid and leaf in upper 1 */
    KT_EQ(pages_of(j), 4 + 2 + 4 + 3 + 2);
    for (unsigned i = 0; i < 4; i++) {
        KT_EQ(peek(c, at[i]), 0x100 + i);
        KT_EQ(vmo_page_phys(c, at[i] * PG), vmo_page_phys(p, at[i] * PG));
    }
    KT_EQ(poke(c, at[1], 0x777), OK);   /* separated: the parent keeps its page */
    KT_EQ(peek(p, at[1]), 0x101);
    KT_EQ(peek(c, at[1]), 0x777);
    KT_ASSERT(vmo_page_phys(c, at[1] * PG) != vmo_page_phys(p, at[1] * PG));
    KT_EQ(poke(p, at[2], 0x888), OK);   /* and the other way */
    KT_EQ(peek(c, at[2]), 0x102);
    KT_EQ(pages_of(j), 15);   /* a separation charges nothing more */
    put(c);
    kt_job_is_empty(j);

    struct vmo *c2;
    KT_EQ(vmo_create_child(j, p, ALOW, 500 * GIB, 100 * GIB, &c2), OK);
    uint64_t in_c2 = UPPER_PAGES + 7 - 500 * GIB / PG;   /* 12 GiB + 7 pages: the root */
    KT_EQ(vmo_committed(c2), PG);
    KT_EQ(pages_of(j), 1 + 2);
    KT_EQ(peek(c2, in_c2), 0x888);
    put(c2);
    kt_job_is_empty(j);

    struct vmo *small, *c3;
    KT_EQ(vmo_create(4 * PG, 0, &small), OK);
    KT_EQ(poke(small, 3, 0x333), OK);
    KT_EQ(vmo_create_child(j, small, SNAP, 0, VMO_MAX_SIZE, &c3), OK);
    KT_EQ(vmo_size(c3), VMO_MAX_SIZE);
    KT_EQ(vmo_committed(c3), PG);
    KT_EQ(peek(c3, 3), 0x333);
    KT_EQ(peek(c3, 4), 0);
    KT_EQ(poke(c3, VMO_MAX_SIZE / PG - 1, 0x444), OK);
    KT_EQ(peek(c3, VMO_MAX_SIZE / PG - 1), 0x444);
    KT_EQ(pages_of(j), 1 + 2 + 1 + 4);
    put(c3);
    put(small);
    kt_job_is_empty(j);
    job_unref(j);
    put(p);
}

/* A child of a sparse reservation (V8's cage: a page in each GiB of a TiB)
 * walks only the tables that exist: one look past a run of missing leaves,
 * as decommit does, not a lock hold per 2 MiB under each mid table (512 K
 * of them here: CR1 finding 5). Timed, on an idle machine only. */
KTEST(vmo_big_child_sparse)
{
    struct vmo *p;
    KT_EQ(vmo_create(TIB, 0, &p), OK);
    for (uint64_t g = 0; g < TIB / GIB; g++)
        KT_EQ(poke(p, g * (GIB / PG), 0x5000 + g), OK);
    struct job *j = kt_fresh_job();
    struct vmo *c;
    uint64_t t0 = uptime_ns();
    KT_EQ(vmo_create_child(j, p, SNAP, 0, TIB, &c), OK);
    uint64_t ns = uptime_ns() - t0;
    kprintf("vmo_big: a 1 TiB child of a page in each GiB made in %lu us\n", (uint64_t)(ns / NS_PER_US));
    KT_EQ(vmo_committed(c), (TIB / GIB) * PG);
    /* each GiB's page, mid and leaf; the top table and uppers 0 and 1 */
    KT_EQ(pages_of(j), 3 * (TIB / GIB) + 3);
    KT_EQ(peek(c, 0), 0x5000);
    KT_EQ(peek(c, 700 * (GIB / PG)), 0x5000 + 700);
    KT_EQ(peek(c, TIB / PG - GIB / PG), 0x5000 + TIB / GIB - 1);
    KT_EQ(peek(c, 700 * (GIB / PG) + 1), 0);
    /* 2,048 lock holds and the child's 3,075 tables and pages: 2 ms on the
     * PC's QEMU; a hold per 2 MiB, half a million more: 18 ms there. */
    KT_IDLE_ASSERT(ns < 8 * NS_PER_MS);
    put(c);
    kt_job_is_empty(j);
    job_unref(j);
    put(p);
}

/* ---- the kinds that stay within 64 GiB ----------------------------------- */

/* Kept, pager and physical VMOs, and a pager VMO's child, keep to
 * VMO_SMALL_MAX_SIZE: past it ERR_OUT_OF_RANGE, with nothing changed. */
KTEST(vmo_big_small_kinds)
{
    struct vmo *v, *k;
    KT_EQ(vmo_create(VMO_SMALL_MAX_SIZE + PG, VMO_KEEP_PAGES, &v), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_create(2 * PG, VMO_KEEP_PAGES, &k), OK);
    KT_EQ(vmo_set_size(k, VMO_SMALL_MAX_SIZE + PG), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_size(k), 2 * PG);
    KT_EQ(vmo_committed(k), 2 * PG);
    struct vmo *kc;   /* a plain child of a kept VMO may be big */
    KT_EQ(vmo_create_child(NULL, k, SNAP, 0, TIB, &kc), OK);
    KT_EQ(vmo_committed(kc), 2 * PG);   /* copies of the kept VMO's two pages, nothing more */
    put(kc);
    put(k);

    KT_EQ(vmo_create_physical(0x100000, VMO_SMALL_MAX_SIZE + PG, 0, &v), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_create_physical(0x100000, VMO_SMALL_MAX_SIZE, 0, &v), OK);   /* a record only */
    put(v);

    struct pager *pg;
    struct vmo *f, *c;
    KT_EQ(pager_create(NULL, 16, ANSWER, &pg), OK);
    KT_EQ(pager_create_vmo(pg, 1, VMO_SMALL_MAX_SIZE + PG, &f), ERR_OUT_OF_RANGE);
    KT_EQ(pager_create_vmo(pg, 1, 4 * PG, &f), OK);
    KT_EQ(pager_op(pg, 1, PAGER_OP_SET_SIZE, VMO_SMALL_MAX_SIZE + PG, 0, 0), ERR_OUT_OF_RANGE);
    struct list_node dead;
    list_init(&dead);
    KT_EQ(vmo_pager_set_size(f, VMO_SMALL_MAX_SIZE + PG, &dead), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_size(f), 4 * PG);
    KT_EQ(vmo_create_child(NULL, f, ALOW, 0, VMO_SMALL_MAX_SIZE + PG, &c), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_create_child(NULL, f, ALOW, 0, 4 * PG, &c), OK);
    KT_EQ(vmo_set_size(c, VMO_SMALL_MAX_SIZE + PG), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_size(c), 4 * PG);
    struct vmo *cc;   /* a child of a pager VMO's child: the same rule */
    KT_EQ(vmo_create_child(NULL, c, ALOW, 0, VMO_SMALL_MAX_SIZE + PG, &cc), ERR_OUT_OF_RANGE);
    put(c);
    put(f);
    struct handle_table t;   /* the pager's handle, closed: its VMO detached */
    handle_table_init(&t);
    struct khandle kh = khandle_from_new(pager_kobject(pg), PAGER_RIGHTS);
    handle_t h;
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    handle_table_destroy(&t);
}
