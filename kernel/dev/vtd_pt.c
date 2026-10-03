/* VT-d second-stage page tables (see vtd_pt.h for the model): map and
 * unmap runs of 4 KiB pages at IOVA = physical address, pin counts in the
 * leaves' software bits, empty tables freed only after the caller's
 * invalidation, the per-domain cap and the job's charge.
 *
 * Shape (VT-d 4.1, 3.7.1 and 9.8): a table is one page of 512 64-bit
 * entries, the same radix tree as the CPU's page tables. Level 1 holds the
 * leaves (4 KiB pages); levels 2 to `levels` hold entries that point at the
 * next table down. An entry is present when it allows read or write. The
 * top table is level `levels`, and its address goes in the domain's
 * context entry.
 *
 * Use counts. A table entry's software bits (52-61) count the entries in
 * use in the table it points at; the top table's count is root_used.
 * Adding the first entry to a table therefore increments its parent
 * entry's count, and a table whose count drops to 0 is unlinked from its
 * parent (that entry cleared and flushed) and put on the gather, which can
 * empty the parent in turn. All counts and walks are loops over at most
 * `levels` steps: nothing here recurses.
 *
 * Ordering. A table page is cleared and flushed before the entry that
 * links it is written, so the unit never walks into stale bytes; a cleared
 * entry is flushed before its table page can go on a gather. Software bits
 * change with a plain store and no flush: the unit ignores them, and a
 * 64-bit aligned store never shows it a torn entry. */
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/string.h>
#include <jam/vtd.h>

#include "vtd_pt.h"

#define ENTRIES     512u
#define MAX_LEVELS  4

/* The tables on one IOVA's path, top down. table[l] is the level-l table;
 * ref[l] is the entry in table[l + 1] that points at it (NULL for the
 * top table, whose use count is root_used). */
struct path {
    uint64_t *table[MAX_LEVELS + 1];
    uint64_t *ref[MAX_LEVELS + 1];
};

/* ---- entries ------------------------------------------------------------------- */

static unsigned index_of(uint64_t iova, unsigned level)
{
    return (unsigned)(iova >> (PAGE_SHIFT + 9 * (level - 1))) & (ENTRIES - 1);
}

static bool present(uint64_t e)
{
    return (e & (VTD_PTE_R | VTD_PTE_W)) != 0;
}

static unsigned sw_bits(uint64_t e)
{
    return (unsigned)((e & VTD_PTE_SW_MASK) >> VTD_PTE_SW_SHIFT);
}

static uint64_t with_sw(uint64_t e, unsigned v)
{
    return (e & ~VTD_PTE_SW_MASK) | ((uint64_t)v << VTD_PTE_SW_SHIFT);
}

static uint64_t *table_at(uint64_t pa)
{
    return phys_to_virt(pa);
}

/* The unit may read the entry at any moment: one aligned 64-bit store. */
static void put(uint64_t *e, uint64_t v)
{
    *(volatile uint64_t *)e = v;
}

static void flush(const struct vtd_pt *pt, const void *va, size_t len)
{
    if (!pt->geom.coherent)
        pt->ops->flush(pt->ctx, va, len);
}

/* How many address bits a depth covers: 39 for 3 levels, 48 for 4. */
static unsigned depth_bits(unsigned levels)
{
    return PAGE_SHIFT + 9 * levels;
}

/* ---- geometry ------------------------------------------------------------------ */

static bool sagaw_has(uint64_t cap, unsigned levels)
{
    return (VTD_CAP_SAGAW(cap) >> (levels - 2)) & 1;   /* bit 1 = 3 levels, bit 2 = 4 */
}

status_t vtd_pt_geom_from_caps(uint64_t cap, uint64_t ecap, unsigned haw,
                               struct vtd_pt_geom *out)
{
    unsigned mgaw = (unsigned)VTD_CAP_MGAW(cap) + 1;
    unsigned levels = 0;
    for (unsigned l = 3; l <= MAX_LEVELS && !levels; l++)
        if (sagaw_has(cap, l) && depth_bits(l) >= mgaw)
            levels = l;
    for (unsigned l = MAX_LEVELS; l >= 3 && !levels; l--)
        if (sagaw_has(cap, l))
            levels = l;
    if (!levels)
        return ERR_NOT_SUPPORTED;   /* only 2- or 5-level tables (or none) */
    unsigned bits = mgaw < depth_bits(levels) ? mgaw : depth_bits(levels);
    if (haw && haw < bits)
        bits = haw;
    if (bits <= PAGE_SHIFT)
        return ERR_NOT_SUPPORTED;
    *out = (struct vtd_pt_geom){
        .levels = (uint8_t)levels,
        .addr_bits = (uint8_t)bits,
        .snoop = VTD_ECAP_SC(ecap) != 0,
        .coherent = VTD_ECAP_C(ecap) != 0,
        .caching_mode = VTD_CAP_CM(cap) != 0,
    };
    return OK;
}

/* ---- gathers ------------------------------------------------------------------- */

void vtd_pt_gather_init(struct vtd_pt_gather *g)
{
    memset(g, 0, sizeof(*g));
    list_init(&g->tables);
}

bool vtd_pt_gather_needed(const struct vtd_pt_gather *g)
{
    return g->whole || g->nruns != 0;
}

/* Record [iova, iova + pages * 4 KiB) for invalidation. Runs grow while
 * they stay contiguous; past VTD_PT_RUNS the gather asks for the whole
 * domain instead. */
static void gather_range(struct vtd_pt_gather *g, uint64_t iova, uint64_t pages)
{
    if (g->whole)
        return;
    if (g->nruns) {
        uint64_t *base = &g->run[g->nruns - 1].base, *n = &g->run[g->nruns - 1].pages;
        uint64_t end = *base + (*n << PAGE_SHIFT);
        if (iova >= *base && iova < end && pages <= (end - iova) >> PAGE_SHIFT)
            return;   /* already inside the last run */
        if (iova == end) {
            *n += pages;
            return;
        }
    }
    if (g->nruns == VTD_PT_RUNS) {
        g->whole = true;
        return;
    }
    g->run[g->nruns].base = iova;
    g->run[g->nruns].pages = pages;
    g->nruns++;
}

static void gather_table(struct vtd_pt *pt, struct vtd_pt_gather *g, uint64_t *table)
{
    list_add_tail(&g->tables, &virt_to_page(table)->node);
    g->ntables++;
    pt->pending++;
}

/* ---- table pages ----------------------------------------------------------------- */

/* A cleared, flushed table page, counted and charged: OK with its
 * physical address, ERR_NO_RESOURCES (the cap) or ERR_NO_MEMORY. */
static status_t table_new(struct vtd_pt *pt, uint64_t *out)
{
    if (pt->tables >= pt->max_tables)
        return ERR_NO_RESOURCES;
    status_t st = job_charge(pt->job, JOB_LIMIT_PAGES, 1);
    if (st != OK)
        return st;
    uint64_t pa = pt->ops->alloc_page(pt->ctx);
    if (!pa) {
        job_uncharge(pt->job, JOB_LIMIT_PAGES, 1);
        return ERR_NO_MEMORY;
    }
    memset(table_at(pa), 0, PAGE_SIZE);
    flush(pt, table_at(pa), PAGE_SIZE);
    pt->tables++;
    *out = pa;
    return OK;
}

static void table_free(struct vtd_pt *pt, uint64_t pa)
{
    pt->ops->free_page(pt->ctx, pa);
    job_uncharge(pt->job, JOB_LIMIT_PAGES, 1);
    pt->tables--;
}

/* ---- walks --------------------------------------------------------------------------- */

static unsigned used(const struct vtd_pt *pt, const struct path *p, unsigned level)
{
    return p->ref[level] ? sw_bits(*p->ref[level]) : pt->root_used;
}

/* The level-`level` table on p gained (+1) or lost (-1) an entry in use. */
static void used_add(struct vtd_pt *pt, struct path *p, unsigned level, int d)
{
    if (p->ref[level])
        put(p->ref[level], with_sw(*p->ref[level], sw_bits(*p->ref[level]) + (unsigned)d));
    else
        pt->root_used += (unsigned)d;
}

/* From `level` up, unlink each table on p that is empty (never the top),
 * onto g. iova is an address in all of them: recorded once anything is
 * unlinked, so the unit drops what its paging-structure caches hold. */
static void prune(struct vtd_pt *pt, struct path *p, unsigned level, uint64_t iova,
                  struct vtd_pt_gather *g)
{
    for (unsigned l = level; l < pt->geom.levels && used(pt, p, l) == 0; l++) {
        put(p->ref[l], 0);
        flush(pt, p->ref[l], sizeof(uint64_t));
        used_add(pt, p, l + 1, -1);
        gather_range(g, iova & ~(PAGE_SIZE - 1), 1);
        gather_table(pt, g, p->table[l]);
    }
}

/* Fill p with iova's path down to its leaf table, linking new tables on
 * the way when g is given; without g, ERR_NOT_FOUND where the path stops.
 * On a failure to make a table, tables the walk left empty go on g. */
static status_t walk(struct vtd_pt *pt, uint64_t iova, struct path *p, struct vtd_pt_gather *g)
{
    unsigned top = pt->geom.levels;
    p->table[top] = table_at(pt->root);
    p->ref[top] = NULL;
    for (unsigned l = top; l > 1; l--) {
        uint64_t *e = &p->table[l][index_of(iova, l)];
        if (!present(*e)) {
            if (!g)
                return ERR_NOT_FOUND;
            uint64_t pa;
            status_t st = table_new(pt, &pa);
            if (st != OK) {
                prune(pt, p, l, iova, g);
                return st;
            }
            put(e, pa | VTD_PTE_R | VTD_PTE_W);
            flush(pt, e, sizeof(*e));
            used_add(pt, p, l, +1);
        }
        p->ref[l - 1] = e;
        p->table[l - 1] = table_at(*e & VTD_PTE_ADDR);
    }
    return OK;
}

/* ---- one page ---------------------------------------------------------------------- */

static status_t map_one(struct vtd_pt *pt, uint64_t iova, struct vtd_pt_gather *g)
{
    struct path p;
    status_t st = walk(pt, iova, &p, g);
    if (st != OK)
        return st;
    uint64_t *e = &p.table[1][index_of(iova, 1)];
    if (present(*e)) {
        unsigned pins = sw_bits(*e);
        if (pins >= VTD_PT_MAX_PINS)
            return ERR_NO_RESOURCES;
        put(e, with_sw(*e, pins + 1));
        return OK;
    }
    uint64_t leaf = iova | VTD_PTE_R | VTD_PTE_W | (pt->geom.snoop ? VTD_PTE_SNP : 0);
    put(e, with_sw(leaf, 1));
    flush(pt, e, sizeof(*e));
    used_add(pt, &p, 1, +1);
    pt->mapped++;
    if (pt->geom.caching_mode)
        gather_range(g, iova, 1);
    return OK;
}

/* Drop one pin of a page that is mapped (the caller checked). */
static void unmap_one(struct vtd_pt *pt, uint64_t iova, struct vtd_pt_gather *g)
{
    struct path p;
    if (walk(pt, iova, &p, NULL) != OK)
        panic("vtd_pt: unmap of %lx, checked mapped, has no path", iova);
    uint64_t *e = &p.table[1][index_of(iova, 1)];
    unsigned pins = sw_bits(*e);
    if (!present(*e) || pins == 0)
        panic("vtd_pt: unmap of %lx, checked mapped, found entry %lx", iova, *e);
    if (pins > 1) {
        put(e, with_sw(*e, pins - 1));
        return;
    }
    put(e, 0);
    flush(pt, e, sizeof(*e));
    used_add(pt, &p, 1, -1);
    pt->mapped--;
    gather_range(g, iova, 1);
    prune(pt, &p, 1, iova, g);
}

/* ---- the interface ------------------------------------------------------------------- */

/* Is [pa, pa + n pages) a run the domain can map? */
static status_t check_run(const struct vtd_pt *pt, uint64_t pa, uint64_t n)
{
    if ((pa & (PAGE_SIZE - 1)) || n == 0)
        return ERR_INVALID_ARGS;
    uint64_t limit = 1ull << pt->geom.addr_bits;
    if (pa >= limit || n > (limit - pa) >> PAGE_SHIFT)
        return ERR_OUT_OF_RANGE;
    return OK;
}

status_t vtd_pt_init(struct vtd_pt *pt, const struct vtd_pt_geom *geom,
                     const struct vtd_pt_ops *ops, void *ctx, struct job *job,
                     uint32_t max_tables)
{
    if (geom->levels < 3 || geom->levels > MAX_LEVELS)
        return ERR_INVALID_ARGS;
    if (geom->addr_bits <= PAGE_SHIFT || geom->addr_bits > depth_bits(geom->levels))
        return ERR_INVALID_ARGS;
    if (!max_tables || !ops->alloc_page || !ops->free_page || !ops->invalidate)
        return ERR_INVALID_ARGS;
    if (!geom->coherent && !ops->flush)
        return ERR_INVALID_ARGS;
    memset(pt, 0, sizeof(*pt));
    pt->geom = *geom;
    pt->ops = ops;
    pt->ctx = ctx;
    pt->job = job;
    pt->max_tables = max_tables;
    uint64_t root;
    status_t st = table_new(pt, &root);
    if (st != OK)
        return st;
    pt->root = root;
    job_ref(job);
    return OK;
}

status_t vtd_pt_destroy(struct vtd_pt *pt)
{
    if (pt->pending)
        return ERR_BAD_STATE;
    /* Depth first with an explicit stack: idx[l] is the next entry of
     * table[l] to look at. A level-2 entry's table holds only leaves, so
     * it is freed without being walked. */
    unsigned top = pt->geom.levels;
    uint64_t *table[MAX_LEVELS + 1];
    unsigned idx[MAX_LEVELS + 1];
    unsigned l = top;
    table[l] = table_at(pt->root);
    idx[l] = 0;
    for (;;) {
        if (idx[l] == ENTRIES) {
            table_free(pt, virt_to_phys(table[l]));
            if (l == top)
                break;
            l++;
            idx[l]++;
            continue;
        }
        uint64_t e = table[l][idx[l]];
        if (present(e) && l == 2)
            table_free(pt, e & VTD_PTE_ADDR);
        if (present(e) && l > 2) {
            l--;
            table[l] = table_at(e & VTD_PTE_ADDR);
            idx[l] = 0;
            continue;
        }
        idx[l]++;
    }
    if (pt->tables != 0)
        panic("vtd_pt: destroy left %u table pages counted", pt->tables);
    job_unref(pt->job);
    pt->job = NULL;
    pt->root = 0;
    pt->mapped = 0;
    pt->root_used = 0;
    return OK;
}

status_t vtd_pt_map(struct vtd_pt *pt, uint64_t pa, uint64_t n, struct vtd_pt_gather *g)
{
    status_t st = check_run(pt, pa, n);
    if (st != OK)
        return st;
    for (uint64_t i = 0; i < n; i++) {
        st = map_one(pt, pa + (i << PAGE_SHIFT), g);
        if (st == OK)
            continue;
        /* All or nothing: each page before i got exactly one pin here. */
        for (uint64_t j = 0; j < i; j++)
            unmap_one(pt, pa + (j << PAGE_SHIFT), g);
        return st;
    }
    return OK;
}

status_t vtd_pt_unmap(struct vtd_pt *pt, uint64_t pa, uint64_t n, struct vtd_pt_gather *g)
{
    status_t st = check_run(pt, pa, n);
    if (st != OK)
        return st;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t pins;
        if (vtd_pt_lookup(pt, pa + (i << PAGE_SHIFT), &pins) != OK)
            return ERR_NOT_FOUND;
    }
    for (uint64_t i = 0; i < n; i++)
        unmap_one(pt, pa + (i << PAGE_SHIFT), g);
    return OK;
}

status_t vtd_pt_gather_finish(struct vtd_pt *pt, struct vtd_pt_gather *g)
{
    if (vtd_pt_gather_needed(g)) {
        status_t st = pt->ops->invalidate(pt->ctx, g);
        if (st != OK)
            return st;
    }
    while (!list_empty(&g->tables)) {
        struct page *pg = list_first(&g->tables, struct page, node);
        list_del(&pg->node);
        table_free(pt, page_to_phys(pg));
        pt->pending--;
    }
    vtd_pt_gather_init(g);
    return OK;
}

status_t vtd_pt_lookup(const struct vtd_pt *pt, uint64_t iova, uint32_t *out_pins)
{
    if (iova >= 1ull << pt->geom.addr_bits)
        return ERR_NOT_FOUND;
    const uint64_t *table = table_at(pt->root);
    for (unsigned l = pt->geom.levels; l > 1; l--) {
        uint64_t e = table[index_of(iova, l)];
        if (!present(e))
            return ERR_NOT_FOUND;
        table = table_at(e & VTD_PTE_ADDR);
    }
    uint64_t e = table[index_of(iova, 1)];
    if (!present(e))
        return ERR_NOT_FOUND;
    *out_pins = sw_bits(e);
    return OK;
}
