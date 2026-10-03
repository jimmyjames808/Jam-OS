/* Tests for the VT-d page tables (kernel/dev/vtd_pt.c), without hardware.
 *
 * The test's ops stand in for the unit. Each table page has a second copy,
 * "what the unit sees", that changes only when the module flushes that part
 * of the page; a new page starts as garbage in both copies. Every check
 * that matters walks the unit's copy: a missing flush (a new table not
 * cleared in the unit's view, a cleared entry the unit still sees) shows up
 * as a walk into garbage or into a page that isn't a table any more. The
 * free op checks that the page is unreachable in the unit's view and that
 * an invalidation completed since the operation that unlinked it; the
 * invalidate op checks that every mapping the unit stopped seeing (and,
 * in caching mode, started seeing) is in the gather's ranges.
 *
 * The IOVAs mapped here are made-up addresses (IOVA = physical address,
 * but nothing behind them is touched): only the tables are real pages. */
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/string.h>
#include <jam/vtd.h>

#include "../dev/vtd_pt.h"

#define MAX_SHADOWS 128
#define NREG        6    /* regions of the random test */
#define RPAGES      16   /* pages per region */
#define K4          PAGE_SIZE

struct shadow {
    uint64_t  pa;    /* the table page */
    uint64_t *dev;   /* the unit's view of it (a page of its own) */
};

struct tctx {
    struct vtd_pt pt;
    struct shadow sh[MAX_SHADOWS];   /* live table pages */
    unsigned      nsh;
    unsigned      allocs, frees, flushes, invals;   /* calls of each op (invals: completed) */
    int           alloc_budget;   /* allocations left before alloc_page fails; -1: no limit */
    bool          alloc_failed;   /* alloc_page refused one */
    status_t      inval_result;   /* the next invalidate fails with this, once (OK: none) */
    unsigned      invals_at_op;   /* invals when the operation under test began */
    bool          destroying;     /* frees come from vtd_pt_destroy */
    /* The model: pin counts of NREG regions of RPAGES pages. */
    unsigned      nreg;
    uint64_t      base[NREG];
    uint16_t      pins[NREG][RPAGES];
    bool          at_inval[NREG][RPAGES];   /* the unit saw it mapped at the last invalidation */
    bool          seen[NREG][RPAGES];       /* ... or at any check since */
};

/* ---- the unit's view ---------------------------------------------------------- */

/* The unit's view of a live table page, or NULL. A coherent unit sees the
 * memory itself. */
static uint64_t *dev_of(struct tctx *t, uint64_t pa)
{
    for (unsigned i = 0; i < t->nsh; i++)
        if (t->sh[i].pa == pa)
            return t->pt.geom.coherent ? phys_to_virt(pa) : t->sh[i].dev;
    return NULL;
}

static bool on(uint64_t e)
{
    return (e & (VTD_PTE_R | VTD_PTE_W)) != 0;
}

/* An entry the unit sees: present ones exactly in the module's format. */
static void check_entry(struct tctx *t, uint64_t e, unsigned level)
{
    if (!on(e)) {
        KT_EQ(e, 0);
        return;
    }
    KT_EQ(e & (VTD_PTE_R | VTD_PTE_W), VTD_PTE_R | VTD_PTE_W);
    uint64_t snp = level == 1 && t->pt.geom.snoop ? VTD_PTE_SNP : 0;
    KT_EQ(e & ~(VTD_PTE_ADDR | VTD_PTE_SW_MASK | VTD_PTE_R | VTD_PTE_W), snp);
    KT_EQ((e & VTD_PTE_ADDR) >> t->pt.geom.addr_bits, 0);
}

/* Translate iova as the unit would: true with the address, or false. */
static bool dev_translate(struct tctx *t, uint64_t iova, uint64_t *out)
{
    uint64_t *d = dev_of(t, t->pt.root);
    KT_ASSERT(d);
    for (unsigned l = t->pt.geom.levels; l >= 1; l--) {
        uint64_t e = d[(iova >> (12 + 9 * (l - 1))) & 511];
        check_entry(t, e, l);
        if (!on(e))
            return false;
        if (l == 1) {
            *out = e & VTD_PTE_ADDR;
            return true;
        }
        d = dev_of(t, e & VTD_PTE_ADDR);
        if (!d)
            ktest_fail("the unit walks from level %u into %lx, not a table", l, (uint64_t)(
                       e & VTD_PTE_ADDR));
    }
    return false;
}

/* Every table the unit can reach, each entry checked: how many, and
 * whether `target` is one of them. Depth first, without recursion. */
static unsigned dev_scan(struct tctx *t, uint64_t target, bool *hit)
{
    unsigned top = t->pt.geom.levels, n = 1, l = top;
    uint64_t *tbl[5];
    unsigned idx[5];
    tbl[l] = dev_of(t, t->pt.root);
    KT_ASSERT(tbl[l]);
    idx[l] = 0;
    *hit = t->pt.root == target;
    for (;;) {
        if (idx[l] == 512) {
            if (l == top)
                return n;
            l++;
            idx[l]++;
            continue;
        }
        uint64_t e = tbl[l][idx[l]];
        if (e)
            check_entry(t, e, l);
        if (!on(e) || l == 1) {
            idx[l]++;
            continue;
        }
        uint64_t *d = dev_of(t, e & VTD_PTE_ADDR);
        if (!d)
            ktest_fail("the unit reaches %lx at level %u, not a table", (uint64_t)(e & VTD_PTE_ADDR), l);
        n++;
        *hit = *hit || (e & VTD_PTE_ADDR) == target;
        l--;
        tbl[l] = d;
        idx[l] = 0;
    }
}

/* ---- the ops ------------------------------------------------------------------ */

static uint64_t t_alloc(void *c)
{
    struct tctx *t = c;
    if (t->alloc_budget == 0) {
        t->alloc_failed = true;
        return 0;
    }
    if (t->alloc_budget > 0)
        t->alloc_budget--;
    KT_ASSERT(t->nsh < MAX_SHADOWS);
    uint64_t pa = pmm_alloc_page_phys(0), dev = pmm_alloc_page_phys(0);
    KT_ASSERT(pa && dev);
    memset(phys_to_virt(pa), 0xa5, K4);   /* garbage, R and W set in every entry */
    memset(phys_to_virt(dev), 0xa5, K4);
    t->sh[t->nsh++] = (struct shadow){ pa, phys_to_virt(dev) };
    t->allocs++;
    return pa;
}

static void t_free(void *c, uint64_t pa)
{
    struct tctx *t = c;
    unsigned i = 0;
    while (i < t->nsh && t->sh[i].pa != pa)
        i++;
    KT_ASSERT(i < t->nsh);
    if (!t->destroying) {
        bool hit;
        KT_ASSERT(t->invals > t->invals_at_op);   /* invalidated since it was unlinked */
        (void)dev_scan(t, pa, &hit);
        KT_ASSERT(!hit);
    }
    pmm_free_page_phys(virt_to_phys(t->sh[i].dev));
    pmm_free_page_phys(pa);
    t->sh[i] = t->sh[--t->nsh];
    t->frees++;
}

/* Copy the flushed lines into the unit's view. */
static void t_flush(void *c, const void *va, size_t len)
{
    struct tctx *t = c;
    uint64_t pa = virt_to_phys(va), page = ALIGN_DOWN(pa, K4);
    uint64_t *dev = dev_of(t, page);
    KT_ASSERT(dev && len && (pa - page) + len <= K4);
    uint64_t lo = ALIGN_DOWN(pa - page, 64), hi = ALIGN_UP(pa - page + len, 64);
    memcpy((char *)dev + lo, (char *)phys_to_virt(page) + lo, hi - lo);
    t->flushes++;
}

static bool covered(const struct vtd_pt_gather *g, uint64_t iova)
{
    if (g->whole)
        return true;
    for (uint32_t i = 0; i < g->nruns; i++)
        if (iova >= g->run[i].base && (iova - g->run[i].base) / K4 < g->run[i].pages)
            return true;
    return false;
}

static status_t t_inval(void *c, const struct vtd_pt_gather *g)
{
    struct tctx *t = c;
    if (t->inval_result != OK) {
        status_t st = t->inval_result;
        t->inval_result = OK;
        return st;
    }
    for (unsigned r = 0; r < t->nreg; r++)
        for (unsigned i = 0; i < RPAGES; i++) {
            uint64_t iova = t->base[r] + i * K4, pa;
            bool now = dev_translate(t, iova, &pa);
            if (t->seen[r][i] && !now)
                KT_ASSERT(covered(g, iova));
            if (t->pt.geom.caching_mode && now && !t->at_inval[r][i])
                KT_ASSERT(covered(g, iova));
            t->at_inval[r][i] = t->seen[r][i] = now;
        }
    t->invals++;
    return OK;
}

static const struct vtd_pt_ops t_ops = {
    .alloc_page = t_alloc,
    .free_page = t_free,
    .flush = t_flush,
    .invalidate = t_inval,
};

/* ---- set-up ---------------------------------------------------------------------- */

/* A 4-level domain over 48 bits (QEMU's unit), or 3 levels over 39. */
static struct vtd_pt_geom geom_of(unsigned levels)
{
    return (struct vtd_pt_geom){
        .levels = (uint8_t)levels,
        .addr_bits = (uint8_t)(levels == 4 ? 48 : 39),
    };
}

static struct tctx *new_ctx(void)
{
    struct tctx *t = kzalloc(sizeof(*t));
    KT_ASSERT(t);
    t->alloc_budget = -1;
    return t;
}

static void start(struct tctx *t, const struct vtd_pt_geom *g, struct job *job, uint32_t cap)
{
    KT_EQ(vtd_pt_init(&t->pt, g, &t_ops, t, job, cap), OK);
}

/* Destroy the domain and check nothing is left. */
static void finish(struct tctx *t, struct job *job)
{
    t->destroying = true;
    KT_EQ(vtd_pt_destroy(&t->pt), OK);
    KT_EQ(t->nsh, 0);
    KT_EQ(t->allocs, t->frees);
    if (job) {
        kt_job_is_empty(job);
        job_unref(job);
    }
    kfree(t);
}

/* Start an operation (or a batch of them) on a fresh gather. */
static void begin(struct tctx *t, struct vtd_pt_gather *g)
{
    vtd_pt_gather_init(g);
    t->invals_at_op = t->invals;
}

/* Map or unmap in a gather of its own, finished; the operation's status. */
static status_t map1(struct tctx *t, uint64_t pa, uint64_t n)
{
    struct vtd_pt_gather g;
    begin(t, &g);
    status_t st = vtd_pt_map(&t->pt, pa, n, &g);
    KT_EQ(vtd_pt_gather_finish(&t->pt, &g), OK);
    return st;
}

static status_t unmap1(struct tctx *t, uint64_t pa, uint64_t n)
{
    struct vtd_pt_gather g;
    begin(t, &g);
    status_t st = vtd_pt_unmap(&t->pt, pa, n, &g);
    KT_EQ(vtd_pt_gather_finish(&t->pt, &g), OK);
    return st;
}

static uint32_t pins_of(struct tctx *t, uint64_t iova)
{
    uint32_t pins = 0;
    return vtd_pt_lookup(&t->pt, iova, &pins) == OK ? pins : 0;
}

static bool dev_maps(struct tctx *t, uint64_t iova)
{
    uint64_t pa;
    bool yes = dev_translate(t, iova, &pa);
    KT_ASSERT(!yes || pa == iova);
    return yes;
}

/* ---- geometry and widths ------------------------------------------------------------- */

#define CAP_ADL   0xd2008c40660462ull  /* an Alder Lake client unit: 4 levels, MGAW 39 */
#define ECAP_ADL  0xf050daull
#define CAP_QEMU  0xd2008c222f0686ull  /* QEMU's: 3 and 4 levels, MGAW 48, caching mode */
#define ECAP_QEMU 0xf00f4aull

static uint64_t with_sagaw_mgaw(uint64_t cap, unsigned sagaw, unsigned mgaw)
{
    cap &= ~((0x1full << 8) | (0x3full << 16));
    return cap | ((uint64_t)sagaw << 8) | ((uint64_t)(mgaw - 1) << 16);
}

KTEST(vtd_pt_geometry_from_caps)
{
    struct vtd_pt_geom g;
    KT_EQ(vtd_pt_geom_from_caps(CAP_ADL, ECAP_ADL, 39, &g), OK);
    KT_EQ(g.levels, 4);   /* the only depth it offers, though MGAW is 39 */
    KT_EQ(g.addr_bits, 39);
    KT_ASSERT(g.snoop && !g.coherent && !g.caching_mode);
    KT_EQ(vtd_pt_geom_from_caps(CAP_QEMU, ECAP_QEMU, 0, &g), OK);
    KT_EQ(g.levels, 4);
    KT_EQ(g.addr_bits, 48);
    KT_ASSERT(!g.snoop && !g.coherent && g.caching_mode);
    KT_EQ(vtd_pt_geom_from_caps(CAP_QEMU, ECAP_QEMU, 39, &g), OK);
    KT_EQ(g.addr_bits, 39);   /* the host's width limits it */
    KT_EQ(vtd_pt_geom_from_caps(with_sagaw_mgaw(CAP_QEMU, 0x6, 39), 1, 0, &g), OK);
    KT_EQ(g.levels, 3);       /* the shallowest that covers MGAW */
    KT_EQ(g.addr_bits, 39);
    KT_ASSERT(g.coherent);
    KT_EQ(vtd_pt_geom_from_caps(with_sagaw_mgaw(CAP_QEMU, 0x2, 48), 0, 0, &g), OK);
    KT_EQ(g.levels, 3);       /* the deepest offered, limited to what it covers */
    KT_EQ(g.addr_bits, 39);
    KT_EQ(vtd_pt_geom_from_caps(with_sagaw_mgaw(CAP_QEMU, 0x4, 57), 0, 0, &g), OK);
    KT_EQ(g.levels, 4);
    KT_EQ(g.addr_bits, 48);
    g.levels = 9;
    KT_EQ(vtd_pt_geom_from_caps(with_sagaw_mgaw(CAP_QEMU, 0x8, 57), 0, 0, &g),
          ERR_NOT_SUPPORTED);   /* 5 levels only */
    KT_EQ(vtd_pt_geom_from_caps(with_sagaw_mgaw(CAP_QEMU, 0x0, 48), 0, 0, &g),
          ERR_NOT_SUPPORTED);
    KT_EQ(g.levels, 9);         /* untouched on failure */
}

KTEST(vtd_pt_width_checks)
{
    for (unsigned levels = 3; levels <= 4; levels++) {
        struct tctx *t = new_ctx();
        struct vtd_pt_geom geom = { .levels = (uint8_t)levels, .addr_bits = 39 };
        start(t, &geom, NULL, 64);
        uint64_t top = 1ull << 39;
        KT_EQ(map1(t, top - K4, 1), OK);   /* the last page there is */
        KT_EQ(pins_of(t, top - K4), 1);
        KT_ASSERT(dev_maps(t, top - K4));
        KT_EQ(map1(t, top, 1), ERR_OUT_OF_RANGE);
        KT_EQ(map1(t, top - K4, 2), ERR_OUT_OF_RANGE);
        KT_EQ(map1(t, 0, UINT64_MAX), ERR_OUT_OF_RANGE);
        KT_EQ(map1(t, K4, top >> 12), ERR_OUT_OF_RANGE);
        KT_EQ(map1(t, ~0ull - K4 + 1, 1), ERR_OUT_OF_RANGE);
        KT_EQ(map1(t, 0x1001, 1), ERR_INVALID_ARGS);
        KT_EQ(map1(t, 0x1000, 0), ERR_INVALID_ARGS);
        KT_EQ(unmap1(t, top, 1), ERR_OUT_OF_RANGE);
        KT_EQ(pins_of(t, top - K4), 1);   /* nothing changed */
        KT_EQ(t->pt.tables, levels);
        KT_EQ(unmap1(t, top - K4, 1), OK);
        KT_EQ(t->pt.tables, 1);
        finish(t, NULL);
    }
    /* Geometries the module can't build. */
    struct tctx *t = new_ctx();
    static const struct vtd_pt_geom bad[] = {
        { .levels = 5, .addr_bits = 48 }, { .levels = 2, .addr_bits = 30 },
        { .levels = 4, .addr_bits = 49 }, { .levels = 3, .addr_bits = 40 },
        { .levels = 4, .addr_bits = 12 },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        KT_EQ(vtd_pt_init(&t->pt, &bad[i], &t_ops, t, NULL, 64), ERR_INVALID_ARGS);
    struct vtd_pt_geom geom = geom_of(4);
    KT_EQ(vtd_pt_init(&t->pt, &geom, &t_ops, t, NULL, 0), ERR_INVALID_ARGS);
    KT_EQ(t->allocs, 0);
    kfree(t);
}

/* ---- map, unmap, remap ------------------------------------------------------------- */

KTEST(vtd_pt_map_unmap_remap)
{
    struct job *job = kt_fresh_job();
    struct tctx *t = new_ctx();
    struct vtd_pt_geom geom = geom_of(4);
    struct vtd_pt_gather g;
    start(t, &geom, job, 64);
    KT_EQ(t->pt.tables, 1);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 1);
    uint64_t a = 0x1fe000;   /* 3 pages across a 2 MiB line: two leaf tables */
    begin(t, &g);
    KT_EQ(vtd_pt_map(&t->pt, a, 3, &g), OK);
    KT_ASSERT(!vtd_pt_gather_needed(&g));   /* caching mode off: nothing to invalidate */
    KT_EQ(vtd_pt_gather_finish(&t->pt, &g), OK);
    KT_EQ(t->invals, 0);
    for (unsigned i = 0; i < 3; i++) {
        KT_EQ(pins_of(t, a + i * K4), 1);
        KT_ASSERT(dev_maps(t, a + i * K4));
    }
    KT_ASSERT(!dev_maps(t, a - K4) && !dev_maps(t, a + 3 * K4));
    KT_EQ(t->pt.tables, 5);   /* root, one level 3, one level 2, two leaf tables */
    KT_EQ(t->pt.mapped, 3);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 5);
    /* The last page: its leaf table empties, and waits for the invalidation. */
    begin(t, &g);
    KT_EQ(vtd_pt_unmap(&t->pt, a + 2 * K4, 1, &g), OK);
    KT_EQ(g.nruns, 1);
    KT_EQ(g.run[0].base, a + 2 * K4);
    KT_EQ(g.run[0].pages, 1);
    KT_EQ(g.ntables, 1);
    KT_EQ(t->frees, 0);
    KT_EQ(t->pt.pending, 1);
    KT_ASSERT(!dev_maps(t, a + 2 * K4));
    KT_EQ(vtd_pt_gather_finish(&t->pt, &g), OK);
    KT_EQ(t->invals, 1);
    KT_EQ(t->frees, 1);
    KT_EQ(t->pt.pending, 0);
    KT_EQ(t->pt.tables, 4);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 4);
    /* Remap it with a page already mapped: one table back, one count up. */
    KT_EQ(map1(t, a + K4, 2), OK);
    KT_EQ(pins_of(t, a + K4), 2);
    KT_EQ(pins_of(t, a + 2 * K4), 1);
    KT_EQ(t->pt.tables, 5);
    KT_EQ(t->pt.mapped, 3);
    /* Unmapping a run with a page not mapped changes nothing. */
    KT_EQ(unmap1(t, a + 2 * K4, 2), ERR_NOT_FOUND);
    KT_EQ(pins_of(t, a + 2 * K4), 1);
    /* All of it, in one gather: back to the root alone. */
    begin(t, &g);
    KT_EQ(vtd_pt_unmap(&t->pt, a, 3, &g), OK);
    KT_EQ(pins_of(t, a + K4), 1);
    KT_EQ(vtd_pt_unmap(&t->pt, a + K4, 1, &g), OK);
    KT_EQ(g.nruns, 3);   /* a, a + 2, then a + 1: not contiguous in that order */
    KT_EQ(g.ntables, 4);
    KT_EQ(vtd_pt_gather_finish(&t->pt, &g), OK);
    KT_EQ(t->pt.tables, 1);
    KT_EQ(t->pt.mapped, 0);
    KT_EQ(t->pt.root_used, 0);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 1);
    finish(t, job);
}

/* Pin counts: shared mappings, and the count's ceiling. */
KTEST(vtd_pt_pin_counts)
{
    struct tctx *t = new_ctx();
    struct vtd_pt_geom geom = geom_of(3);
    start(t, &geom, NULL, 64);
    uint64_t a = 0x40000000;
    KT_EQ(map1(t, a, 4), OK);
    KT_EQ(map1(t, a + 2 * K4, 4), OK);   /* overlaps two pages */
    KT_EQ(pins_of(t, a + K4), 1);
    KT_EQ(pins_of(t, a + 2 * K4), 2);
    KT_EQ(pins_of(t, a + 5 * K4), 1);
    KT_EQ(t->pt.mapped, 6);
    KT_EQ(unmap1(t, a, 4), OK);
    KT_EQ(pins_of(t, a + K4), 0);
    KT_EQ(pins_of(t, a + 2 * K4), 1);
    KT_ASSERT(!dev_maps(t, a + K4) && dev_maps(t, a + 2 * K4));
    KT_EQ(t->pt.mapped, 4);
    /* Up to the ceiling: one more is refused, and a run that would pass
     * it leaves the page before it as it was. */
    for (unsigned i = 1; i < VTD_PT_MAX_PINS; i++)
        KT_EQ(map1(t, a + 3 * K4, 1), OK);
    KT_EQ(pins_of(t, a + 3 * K4), VTD_PT_MAX_PINS);
    KT_EQ(map1(t, a + 3 * K4, 1), ERR_NO_RESOURCES);
    KT_EQ(map1(t, a + 2 * K4, 3), ERR_NO_RESOURCES);
    KT_EQ(pins_of(t, a + 2 * K4), 1);
    KT_EQ(pins_of(t, a + 4 * K4), 1);
    KT_EQ(pins_of(t, a + 3 * K4), VTD_PT_MAX_PINS);
    for (unsigned i = 1; i < VTD_PT_MAX_PINS; i++)
        KT_EQ(unmap1(t, a + 3 * K4, 1), OK);
    KT_EQ(unmap1(t, a + 2 * K4, 4), OK);
    KT_EQ(t->pt.mapped, 0);
    KT_EQ(t->pt.tables, 1);
    KT_EQ(unmap1(t, a + 2 * K4, 1), ERR_NOT_FOUND);
    finish(t, NULL);
}

/* Empty tables wait for the invalidation, through a failed one too; the
 * domain can't be destroyed while they wait. */
KTEST(vtd_pt_frees_only_after_invalidation)
{
    struct job *job = kt_fresh_job();
    struct tctx *t = new_ctx();
    struct vtd_pt_geom geom = geom_of(4);
    struct vtd_pt_gather g;
    start(t, &geom, job, 64);
    uint64_t a = 0x8000000000ull;   /* the second top-level entry */
    KT_EQ(map1(t, 0, 1), OK);
    KT_EQ(map1(t, a, 1), OK);
    KT_EQ(t->pt.tables, 7);
    begin(t, &g);
    KT_EQ(vtd_pt_unmap(&t->pt, a, 1, &g), OK);
    KT_EQ(g.ntables, 3);
    KT_EQ(t->pt.root_used, 1);
    KT_EQ(t->frees, 0);
    KT_EQ(vtd_pt_destroy(&t->pt), ERR_BAD_STATE);
    t->inval_result = ERR_TIMED_OUT;
    KT_EQ(vtd_pt_gather_finish(&t->pt, &g), ERR_TIMED_OUT);
    KT_EQ(t->frees, 0);
    KT_EQ(g.ntables, 3);
    KT_EQ(t->pt.tables, 7);   /* still held, still charged */
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 7);
    KT_EQ(vtd_pt_gather_finish(&t->pt, &g), OK);
    KT_EQ(t->frees, 3);
    KT_EQ(t->pt.tables, 4);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 4);
    KT_ASSERT(dev_maps(t, 0));
    /* Destroyed with a mapping left: every table goes. */
    finish(t, job);
}

/* The cap on table pages, the job's page limit, and running out of pages:
 * each refuses the map and leaves the tables as they were. */
KTEST(vtd_pt_cap_and_charge)
{
    struct job *job = kt_fresh_job();
    struct tctx *t = new_ctx();
    struct vtd_pt_geom geom = geom_of(4);
    start(t, &geom, job, 5);
    KT_EQ(map1(t, 0, 2), OK);   /* root + 3 */
    KT_EQ(t->pt.tables, 4);
    /* 1 GiB up needs a level 2 and a leaf table: the cap allows one. */
    KT_EQ(map1(t, 0x40000000, 1), ERR_NO_RESOURCES);
    KT_EQ(t->allocs, 5);        /* the level 2 table was made, then pruned */
    KT_EQ(t->frees, 1);
    KT_EQ(t->pt.tables, 4);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 4);
    KT_EQ(pins_of(t, 0x40000000), 0);
    /* A run that needs a table halfway refuses as a whole. */
    t->pt.max_tables = 4;
    KT_EQ(map1(t, 0x1ff000, 2), ERR_NO_RESOURCES);
    KT_EQ(pins_of(t, 0x1ff000), 0);
    KT_EQ(t->pt.mapped, 2);
    KT_EQ(t->pt.tables, 4);
    /* The job's limit. */
    t->pt.max_tables = 64;
    KT_EQ(job_set_limit(job, JOB_LIMIT_PAGES, 5), OK);
    KT_EQ(map1(t, 0x40000000, 1), ERR_NO_MEMORY);
    KT_EQ(t->pt.tables, 4);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 4);
    KT_EQ(job_set_limit(job, JOB_LIMIT_PAGES, JOB_NO_LIMIT), OK);
    /* No page from the allocator. */
    t->alloc_budget = 1;
    KT_EQ(map1(t, 0x40000000, 1), ERR_NO_MEMORY);
    KT_ASSERT(t->alloc_failed);
    KT_EQ(t->pt.tables, 4);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 4);
    t->alloc_budget = -1;
    KT_EQ(map1(t, 0x40000000, 1), OK);
    KT_EQ(t->pt.tables, 6);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 6);
    /* No job at all: nothing charged. */
    struct tctx *u = new_ctx();
    start(u, &geom, NULL, 8);
    KT_EQ(map1(u, 0, 1), OK);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), 6);
    finish(u, NULL);
    finish(t, job);
}

/* A coherent unit gets no flushes; snoop control sets SNP in the leaves;
 * caching mode records new mappings for invalidation. */
KTEST(vtd_pt_unit_flags)
{
    struct tctx *t = new_ctx();
    struct vtd_pt_geom geom = geom_of(4);
    geom.snoop = true;
    geom.caching_mode = true;
    struct vtd_pt_gather g;
    start(t, &geom, NULL, 64);
    begin(t, &g);
    KT_EQ(vtd_pt_map(&t->pt, 0x5000, 2, &g), OK);
    KT_ASSERT(vtd_pt_gather_needed(&g));
    KT_EQ(g.run[0].base, 0x5000);
    KT_EQ(g.run[0].pages, 2);
    KT_EQ(vtd_pt_gather_finish(&t->pt, &g), OK);
    KT_EQ(t->invals, 1);
    KT_ASSERT(dev_maps(t, 0x5000));   /* check_entry wants SNP set */
    finish(t, NULL);

    static const struct vtd_pt_ops no_flush = {
        .alloc_page = t_alloc, .free_page = t_free, .invalidate = t_inval,
    };
    t = new_ctx();
    geom = geom_of(4);
    geom.coherent = true;
    KT_EQ(vtd_pt_init(&t->pt, &geom, &no_flush, t, NULL, 64), OK);
    KT_EQ(map1(t, 0x5000, 2), OK);
    KT_ASSERT(dev_maps(t, 0x6000));
    KT_EQ(unmap1(t, 0x5000, 2), OK);
    KT_EQ(t->flushes, 0);
    KT_EQ(t->pt.tables, 1);
    finish(t, NULL);
    /* Not coherent and no flush: refused. */
    t = new_ctx();
    geom.coherent = false;
    KT_EQ(vtd_pt_init(&t->pt, &geom, &no_flush, t, NULL, 64), ERR_INVALID_ARGS);
    kfree(t);
}

/* Separate ranges fill the gather's runs, then it asks for the whole
 * domain. */
KTEST(vtd_pt_gather_runs)
{
    struct tctx *t = new_ctx();
    struct vtd_pt_geom geom = geom_of(3);
    struct vtd_pt_gather g;
    start(t, &geom, NULL, 64);
    KT_EQ(map1(t, 0, 2 * VTD_PT_RUNS + 2), OK);
    begin(t, &g);
    for (unsigned i = 0; i < VTD_PT_RUNS; i++)
        KT_EQ(vtd_pt_unmap(&t->pt, 2 * i * K4, 1, &g), OK);
    KT_EQ(g.nruns, VTD_PT_RUNS);
    KT_ASSERT(!g.whole);
    KT_EQ(vtd_pt_unmap(&t->pt, 2 * VTD_PT_RUNS * K4, 2, &g), OK);
    KT_ASSERT(g.whole);
    KT_EQ(vtd_pt_gather_finish(&t->pt, &g), OK);
    KT_ASSERT(!g.whole && g.nruns == 0);
    finish(t, NULL);
}

/* ---- random sequences against a model ------------------------------------------------ */

/* Tables a set of mapped pages needs: the root, and per level below it one
 * table per distinct prefix. */
static unsigned tables_needed(struct tctx *t)
{
    uint64_t pages[NREG * RPAGES];
    unsigned n = 0, total = 1;
    for (unsigned r = 0; r < t->nreg; r++)
        for (unsigned i = 0; i < RPAGES; i++)
            if (t->pins[r][i])
                pages[n++] = t->base[r] + i * K4;
    for (unsigned l = 1; l < t->pt.geom.levels; l++) {
        unsigned shift = 12 + 9 * l;
        for (unsigned i = 0; i < n; i++) {
            bool first = true;
            for (unsigned j = 0; j < i && first; j++)
                first = (pages[j] >> shift) != (pages[i] >> shift);
            total += first;
        }
    }
    return total;
}

/* Both views against the model; with settled (the gather finished), the
 * table count against what the mappings need. */
static void verify(struct tctx *t, struct job *job, bool settled)
{
    uint64_t mapped = 0;
    for (unsigned r = 0; r < t->nreg; r++)
        for (unsigned i = 0; i < RPAGES; i++) {
            uint64_t iova = t->base[r] + i * K4;
            if (pins_of(t, iova) != t->pins[r][i])
                ktest_fail("%lx (region %u page %u): %u pins, the model says %u", iova, r, i,
                           pins_of(t, iova), t->pins[r][i]);
            bool dev = dev_maps(t, iova);
            KT_EQ(dev, t->pins[r][i] != 0);
            t->seen[r][i] = t->seen[r][i] || dev;
            mapped += t->pins[r][i] != 0;
        }
    KT_EQ(t->pt.mapped, mapped);
    bool hit;
    KT_EQ(dev_scan(t, 0, &hit), t->nsh - t->pt.pending);
    KT_EQ(t->pt.tables, t->nsh);
    KT_EQ(job_used(job, JOB_LIMIT_PAGES), t->nsh);
    if (settled) {
        KT_EQ(t->pt.pending, 0);
        KT_EQ(t->pt.tables, tables_needed(t));
    }
}

static void random_map(struct tctx *t, unsigned r, unsigned off, unsigned len,
                       struct vtd_pt_gather *g, uint64_t *rng)
{
    t->alloc_budget = kt_rng(rng) % 8 ? -1 : (int)(kt_rng(rng) % 3);
    t->alloc_failed = false;
    status_t st = vtd_pt_map(&t->pt, t->base[r] + off * K4, len, g);
    t->alloc_budget = -1;
    if (st != OK) {
        KT_EQ(st, ERR_NO_MEMORY);
        KT_ASSERT(t->alloc_failed);
        return;
    }
    for (unsigned i = 0; i < len; i++)
        t->pins[r][off + i]++;
}

/* Mostly unmap what is mapped: the run is cut to its mapped start. */
static void random_unmap(struct tctx *t, unsigned r, unsigned off, unsigned len,
                         struct vtd_pt_gather *g, uint64_t *rng)
{
    unsigned mapped = 0;
    while (mapped < len && t->pins[r][off + mapped])
        mapped++;
    if (kt_rng(rng) % 4 && mapped)
        len = mapped;
    status_t st = vtd_pt_unmap(&t->pt, t->base[r] + off * K4, len, g);
    KT_EQ(st, mapped >= len ? OK : ERR_NOT_FOUND);
    if (st == OK)
        for (unsigned i = 0; i < len; i++)
            t->pins[r][off + i]--;
}

static void random_run(unsigned levels, const uint64_t *bases, uint64_t seed)
{
    struct job *job = kt_fresh_job();
    struct tctx *t = new_ctx();
    struct vtd_pt_geom geom = geom_of(levels);
    geom.caching_mode = seed & 1;
    geom.snoop = seed & 2;
    struct vtd_pt_gather g;
    t->nreg = NREG;
    memcpy(t->base, bases, sizeof(t->base));
    start(t, &geom, job, MAX_SHADOWS / 2);
    uint64_t rng = seed;
    for (unsigned op = 0; op < 800; op++) {
        begin(t, &g);
        unsigned batch = 1 + (unsigned)(kt_rng(&rng) % 3);
        for (unsigned b = 0; b < batch; b++) {
            unsigned r = (unsigned)(kt_rng(&rng) % NREG);
            unsigned off = (unsigned)(kt_rng(&rng) % RPAGES);
            unsigned len = 1 + (unsigned)(kt_rng(&rng) % (RPAGES - off));
            if (kt_rng(&rng) % 2)
                random_map(t, r, off, len, &g, &rng);
            else
                random_unmap(t, r, off, len, &g, &rng);
            verify(t, job, false);
        }
        if (kt_rng(&rng) % 16 == 0) {
            unsigned frees = t->frees;
            t->inval_result = ERR_TIMED_OUT;
            status_t st = vtd_pt_gather_finish(&t->pt, &g);
            KT_ASSERT(st == ERR_TIMED_OUT || (st == OK && t->frees == frees));
            t->inval_result = OK;
            KT_EQ(t->frees, frees);
        }
        KT_EQ(vtd_pt_gather_finish(&t->pt, &g), OK);
        verify(t, job, true);
    }
    /* Everything away: the root alone is left. */
    for (unsigned r = 0; r < NREG; r++)
        for (unsigned i = 0; i < RPAGES; i++)
            while (t->pins[r][i]) {
                KT_EQ(unmap1(t, t->base[r] + i * K4, 1), OK);
                t->pins[r][i]--;
            }
    verify(t, job, true);
    KT_EQ(t->pt.tables, 1);
    finish(t, job);
}

KTEST(vtd_pt_random_against_model)
{
    /* Runs across a leaf table's 2 MiB, a level-2 table's 1 GiB and a top
     * entry's 512 GiB, at the top of the space, and somewhere else. */
    static const uint64_t four[NREG] = {
        0, 0x1f8000, 0x3fff8000, 0x7ffffff8000ull, 0xffffffff0000ull, 0x123456789000ull,
    };
    static const uint64_t three[NREG] = {
        0, 0x1f8000, 0x3fff8000, 0x7fffff0000ull, 0x1234567000ull, 0x5432100000ull,
    };
    for (uint64_t seed = 1; seed <= 4; seed++) {
        random_run(4, four, 0x5eed0000 + seed);
        random_run(3, three, 0x7ab1e000 + seed);
    }
}
