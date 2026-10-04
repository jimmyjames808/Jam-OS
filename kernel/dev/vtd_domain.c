/* VT-d DMA remapping: the root and context entries, domain ids, domains
 * and their page tables, which domain each function's context entry
 * names, the fault mute, and the domain half of <jam/iommu.h> (see
 * vtd_domain.h for the model and the lock order; Intel VT-d
 * specification 4.1).
 *
 * The tables are built at boot (vtd_boot.c) while nothing reads them:
 * entries are written and flushed, and the unit is pointed at the root
 * table afterwards. From then on (`live`) every change of a context entry
 * is one 16-byte atomic write followed by the invalidation of what the
 * unit cached for the entry it replaced (6.2.2.1, Table 25), and every
 * domain id given back is invalidated before it can be reused. */
#include <jam/iommu.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pci.h>
#include <jam/report.h>
#include <jam/string.h>
#include <jam/vtd.h>

#include "vtd_domain.h"
#include "vtd_internal.h"

static struct vtd_ctl ctls[VTD_MAX_UNITS];   /* by the DMAR's unit number */
static struct vtd_fn *fns;                   /* one per PCI function; written at boot */
static uint32_t nfns;

/* What <jam/iommu.h> hands out: a domain of the function it was made for. */
struct iommu_domain {
    struct vtd_dom *dom;   /* the domain itself */
    struct vtd_fn  *fn;    /* the function it is for (fixed) */
};

/* ---- the entries (pure) ------------------------------------------------------------------- */

struct vtd_ctx vtd_root_entry(uint64_t ctx_table)
{
    /* 9.1: CTP 63:12, P bit 0, the upper 64 bits reserved (0). */
    return (struct vtd_ctx){ .lo = (ctx_table & VTD_ROOT_CTP) | VTD_ROOT_P, .hi = 0 };
}

struct vtd_ctx vtd_ctx_entry(uint16_t did, bool pass, uint64_t table, unsigned aw, bool fpd)
{
    /* 9.3: P 0, FPD 1, TT 3:2, SSPTPTR 63:12 (ignored with TT = 10b: 0);
     * AW 66:64, DID 87:72, the rest reserved (0). */
    uint64_t lo = VTD_CTX_P | (fpd ? VTD_CTX_FPD : 0) |
                  (pass ? VTD_CTX_TT_PT : VTD_CTX_TT_SS) << VTD_CTX_TT_SHIFT;
    if (!pass)
        lo |= table & VTD_CTX_SSPTPTR;
    return (struct vtd_ctx){ lo, (uint64_t)(aw & 7) | (uint64_t)did << VTD_CTX_DID_SHIFT };
}

unsigned vtd_pass_aw(uint64_t cap)
{
    /* 9.3: with TT = 10b, AW is the largest AGAW the unit supports; AW's
     * encoding (1 = 39-bit, 2 = 48, 3 = 57) is SAGAW's bit number. */
    uint64_t sagaw = VTD_CAP_SAGAW(cap);
    for (unsigned b = 3; b >= 1; b--)
        if (sagaw & (1u << b))
            return b;
    return 0;
}

/* ---- units and functions ------------------------------------------------------------------ */

struct vtd_ctl *vtd_ctl_get(uint32_t i)
{
    return i < VTD_MAX_UNITS && ctls[i].unit ? &ctls[i] : NULL;
}

static bool live(const struct vtd_ctl *ctl)
{
    return __atomic_load_n(&ctl->live, __ATOMIC_ACQUIRE);
}

struct vtd_fn *vtd_fn_at(uint32_t pci_index)
{
    return fns && pci_index < nfns ? &fns[pci_index] : NULL;
}

struct vtd_fn *vtd_fn_of(const struct pci_dev *dev)
{
    struct vtd_fn *f = dev ? vtd_fn_at(dev->index) : NULL;
    return f && f->ctl && live(f->ctl) ? f : NULL;
}

status_t vtd_fns_init(void)
{
    uint32_t n = pci_count();
    if (fns || !n)
        return OK;
    struct vtd_fn *a = kzalloc(n * sizeof(*a));
    if (!a)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < n; i++) {
        struct pci_dev *d = pci_get(i);
        a[i].dev = d;
        a[i].sid = (uint16_t)(d->info.bus << 8 | d->info.dev << 3 | d->info.fn);
    }
    fns = a;
    nfns = n;
    return OK;
}

status_t vtd_ctl_init(struct vtd_unit *u)
{
    struct vtd_ctl *ctl = &ctls[u->index];
    if (ctl->unit)
        return OK;
    /* CAP.ND: 2^(4 + 2 * ND) ids (11.4.2); 7 is reserved: the cap keeps
     * the bitmap to 16 bits' worth. */
    uint32_t ndid = 1u << (4 + 2 * (unsigned)VTD_CAP_ND(u->cap));
    if (ndid > VTD_TT_MAX_DIDS)
        ndid = VTD_TT_MAX_DIDS;
    uint64_t root = pmm_alloc_page_phys(PMM_ZERO);
    uint64_t *used = kzalloc(((ndid + 63) / 64) * sizeof(uint64_t));
    if (!root || !used) {
        if (root)
            pmm_free_page_phys(root);
        kfree(used);
        return ERR_NO_MEMORY;
    }
    mutex_init(&ctl->lock, "vtd context");
    ctl->root_phys = root;
    ctl->root = phys_to_virt(root);
    ctl->did_used = used;
    ctl->did_used[0] = 1;   /* id 0: reserved with CM = 1 (9.3), never used here */
    ctl->ndid = ndid;
    ctl->unit = u;
    return OK;
}

/* ---- domain ids ------------------------------------------------------------------------- */

uint16_t vtd_did_alloc(struct vtd_ctl *ctl)
{
    for (uint32_t w = 0; w < (ctl->ndid + 63) / 64; w++) {
        if (ctl->did_used[w] == ~0ull)
            continue;
        uint32_t id = w * 64 + (uint32_t)__builtin_ctzll(~ctl->did_used[w]);
        if (id >= ctl->ndid)
            return 0;
        ctl->did_used[w] |= 1ull << (id % 64);
        return (uint16_t)id;
    }
    return 0;
}

static void did_put(struct vtd_ctl *ctl, uint16_t did)
{
    ctl->did_used[did / 64] &= ~(1ull << (did % 64));
}

status_t vtd_did_free(struct vtd_ctl *ctl, uint16_t did)
{
    if (live(ctl)) {
        /* Table 25, "re-use domain-id": the context cache and the IOTLB
         * entries it tagged go, when the id stops being used. */
        status_t st = vtd_inv_context_domain(ctl->unit, did);
        if (st == OK)
            st = vtd_inv_iotlb_domain(ctl->unit, did);
        if (st != OK)
            return st;
    }
    did_put(ctl, did);
    return OK;
}

/* ---- domains ------------------------------------------------------------------------------ */

static struct vtd_ctl *ctl_of(const struct vtd_dom *d)
{
    return &ctls[d->ud.unit->index];
}

status_t vtd_dom_new(struct vtd_ctl *ctl, enum vtd_dom_kind kind, struct job *job,
                     uint32_t max_tables, const char *what, struct vtd_dom **out)
{
    struct vtd_unit *u = ctl->unit;
    if (kind == VTD_DOM_PASS && (!VTD_ECAP_PT(u->ecap) || !vtd_pass_aw(u->cap)))
        return ERR_NOT_SUPPORTED;
    struct vtd_dom *d = kzalloc(sizeof(*d));
    if (!d)
        return ERR_NO_MEMORY;
    uint16_t did = vtd_did_alloc(ctl);
    if (!did) {
        kfree(d);
        return ERR_NO_RESOURCES;
    }
    d->ud = (struct vtd_unit_domain){ .unit = u, .did = did };
    d->kind = kind;
    d->what = what;
    mutex_init(&d->lock, "vtd domain");
    if (kind == VTD_DOM_TABLE) {
        struct vtd_pt_geom g;
        status_t st = vtd_unit_pt_geom(u, &g);
        if (st == OK)
            st = vtd_pt_init(&d->pt, &g, &vtd_unit_pt_ops, &d->ud, job, max_tables);
        if (st != OK) {
            did_put(ctl, did);   /* never named by an entry: nothing cached */
            kfree(d);
            return st == ERR_NO_MEMORY ? st : ERR_NOT_SUPPORTED;
        }
    }
    *out = d;
    return OK;
}

status_t vtd_dom_free(struct vtd_dom *d)
{
    if (d->users || (d->kind == VTD_DOM_TABLE && d->pt.pending))
        return ERR_BAD_STATE;
    status_t st = vtd_did_free(ctl_of(d), d->ud.did);
    if (st != OK)
        return st;
    if (d->kind == VTD_DOM_TABLE)
        (void)vtd_pt_destroy(&d->pt);   /* nothing pending (checked): OK */
    kfree(d);
    return OK;
}

/* Pages from pages[0] on that follow each other: the run's length. */
static size_t run_len(const uint64_t *pages, size_t n)
{
    size_t k = 1;
    while (k < n && pages[k] == pages[k - 1] + PAGE_SIZE)
        k++;
    return k;
}

/* Unmap the runs of pages[0..n) (all mapped by this caller just now). */
static void undo_runs(struct vtd_dom *d, const uint64_t *pages, size_t n,
                      struct vtd_pt_gather *g)
{
    for (size_t i = 0; i < n;) {
        size_t k = run_len(pages + i, n - i);
        (void)vtd_pt_unmap(&d->pt, pages[i], k, g);   /* mapped: can't fail */
        i += k;
    }
}

/* Finish g; a failure is logged (its table pages stay with the domain). */
static status_t finish(struct vtd_dom *d, struct vtd_pt_gather *g)
{
    status_t st = vtd_pt_gather_finish(&d->pt, g);
    if (st != OK)
        kprintf("vtd: unit %u domain %u (%s): an invalidation failed (%d): %u table pages "
                "kept\n", d->ud.unit->index, d->ud.did, d->what, st, d->pt.pending);
    return st;
}

static status_t check_pages(const struct vtd_dom *d, const uint64_t *pages, size_t n)
{
    if (d->kind != VTD_DOM_TABLE)
        return ERR_NOT_SUPPORTED;
    if (!n)
        return ERR_INVALID_ARGS;
    for (size_t i = 0; i < n; i++)
        if (pages[i] & (PAGE_SIZE - 1))
            return ERR_INVALID_ARGS;
    return OK;
}

status_t vtd_dom_map(struct vtd_dom *d, const uint64_t *pages, size_t n)
{
    status_t st = check_pages(d, pages, n);
    if (st != OK)
        return st;
    mutex_lock(&d->lock);
    struct vtd_pt_gather g;
    vtd_pt_gather_init(&g);
    size_t done = 0;
    while (done < n && st == OK) {
        size_t k = run_len(pages + done, n - done);
        st = vtd_pt_map(&d->pt, pages[done], k, &g);
        if (st == OK)
            done += k;
    }
    if (st != OK)
        undo_runs(d, pages, done, &g);
    status_t fst = finish(d, &g);
    if (st == OK && fst != OK) {
        /* In caching mode the unit may still hold "not present" for the
         * new mappings: all or nothing, so they go again. */
        undo_runs(d, pages, n, &g);
        (void)finish(d, &g);
        st = fst;
    }
    mutex_unlock(&d->lock);
    return st;
}

status_t vtd_dom_map_range(struct vtd_dom *d, uint64_t pa, uint64_t pages)
{
    if (d->kind != VTD_DOM_TABLE)
        return ERR_NOT_SUPPORTED;
    mutex_lock(&d->lock);
    struct vtd_pt_gather g;
    vtd_pt_gather_init(&g);
    status_t st = vtd_pt_map(&d->pt, pa, pages, &g);
    status_t fst = finish(d, &g);
    mutex_unlock(&d->lock);
    return st != OK ? st : fst;
}

status_t vtd_dom_unmap(struct vtd_dom *d, const uint64_t *pages, size_t n)
{
    status_t st = check_pages(d, pages, n);
    if (st != OK)
        return st;
    mutex_lock(&d->lock);
    for (size_t i = 0; i < n && st == OK; i++) {
        uint32_t pins;
        st = vtd_pt_lookup(&d->pt, pages[i], &pins);
    }
    struct vtd_pt_gather g;
    vtd_pt_gather_init(&g);
    size_t done = 0;
    while (done < n && st == OK) {
        size_t k = run_len(pages + done, n - done);
        st = vtd_pt_unmap(&d->pt, pages[done], k, &g);
        if (st == OK)
            done += k;
    }
    if (st != OK && done) {
        /* A page listed twice with one pin: put the earlier runs back. */
        for (size_t i = 0; i < done;) {
            size_t k = run_len(pages + i, done - i);
            (void)vtd_pt_map(&d->pt, pages[i], k, &g);
            i += k;
        }
    }
    status_t fst = finish(d, &g);
    mutex_unlock(&d->lock);
    return st != OK ? st : fst;
}

/* ---- context entries ------------------------------------------------------------------------ */

static void flush(const struct vtd_ctl *ctl, const void *va, size_t len)
{
    if (!VTD_ECAP_C(ctl->unit->ecap))
        vtd_flush_lines(va, len);
}

static struct vtd_ctx *entry_of(const struct vtd_fn *f)
{
    struct vtd_ctx *t = f->ctl->ctx[f->sid >> 8];
    return t ? &t[f->sid & 0xff] : NULL;
}

/* One 16-byte atomic store of v into *e (6.2.2.1): CMPXCHG16B with the
 * value last read as the comparand. Only this file writes entries, under
 * "vtd context", so the exchange succeeds on the first try, or on the
 * second when the first read was torn (a failed exchange loads the
 * current value, which the second matches). */
static void write_entry(struct vtd_ctx *e, struct vtd_ctx v)
{
    uint64_t lo = __atomic_load_n(&e->lo, __ATOMIC_RELAXED);
    uint64_t hi = __atomic_load_n(&e->hi, __ATOMIC_RELAXED);
    for (int tries = 0; tries < 4; tries++) {
        bool ok;
        __asm__ volatile("lock cmpxchg16b %1"
                         : "=@ccz"(ok), "+m"(*e), "+a"(lo), "+d"(hi)
                         : "b"(v.lo), "c"(v.hi)
                         : "memory");
        if (ok)
            return;
    }
    panic("vtd: a context entry changed under its lock");
}

struct vtd_ctx vtd_fn_entry(const struct vtd_fn *f, const struct vtd_dom *d)
{
    const struct vtd_unit *u = f->ctl->unit;
    bool pass = d->kind == VTD_DOM_PASS;
    unsigned aw = pass ? vtd_pass_aw(u->cap) : vtd_pt_aw(&d->pt);
    return vtd_ctx_entry(d->ud.did, pass, pass ? 0 : vtd_pt_root(&d->pt), aw, f->muted);
}

struct vtd_ctx vtd_fn_read(const struct vtd_fn *f)
{
    const struct vtd_ctx *e = entry_of(f);
    if (!e)
        return (struct vtd_ctx){ 0, 0 };
    return (struct vtd_ctx){ __atomic_load_n(&e->lo, __ATOMIC_RELAXED),
                             __atomic_load_n(&e->hi, __ATOMIC_RELAXED) };
}

static void set_cur(struct vtd_fn *f, struct vtd_dom *d)
{
    if (f->cur)
        f->cur->users--;
    d->users++;
    f->cur = d;
}

/* The context table for bus, made (and linked from the root table) if
 * there is none. Before the unit is live only: root entries never change
 * under a running unit. */
static status_t table_for(struct vtd_ctl *ctl, uint8_t bus)
{
    if (ctl->ctx[bus])
        return OK;
    if (live(ctl))
        return ERR_BAD_STATE;
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    if (!pa)
        return ERR_NO_MEMORY;
    ctl->ctx[bus] = phys_to_virt(pa);
    flush(ctl, ctl->ctx[bus], PAGE_SIZE);   /* the zeroes: not-present entries */
    ctl->root[bus] = vtd_root_entry(pa);
    flush(ctl, &ctl->root[bus], sizeof(struct vtd_ctx));
    return OK;
}

status_t vtd_fn_place(struct vtd_fn *f, struct vtd_dom *d)
{
    status_t st = table_for(f->ctl, (uint8_t)(f->sid >> 8));
    if (st != OK)
        return st;
    struct vtd_ctx *e = entry_of(f);
    write_entry(e, vtd_fn_entry(f, d));
    flush(f->ctl, e, sizeof(*e));
    set_cur(f, d);
    return OK;
}

/* What the unit may have cached for the entry `old` of f goes (Table 25,
 * "changes to a context-table entry"): device-selective context cache,
 * then the old domain's IOTLB, one after the other (6.2.2.1: serially).
 * A not-present entry is cached only in caching mode, tagged with domain
 * id 0 (6.2.2); otherwise a write-buffer flush is all (6.8). */
static status_t invalidate_old(const struct vtd_fn *f, struct vtd_ctx old)
{
    struct vtd_unit *u = f->ctl->unit;
    if (old.lo & VTD_CTX_P) {
        uint16_t did = VTD_CTX_DID(old.hi);
        status_t st = vtd_inv_context_device(u, did, f->sid, 0);
        return st != OK ? st : vtd_inv_iotlb_domain(u, did);
    }
    if (VTD_CAP_CM(u->cap))
        return vtd_inv_context_device(u, 0, f->sid, 0);
    return vtd_unit_flush_write_buffer(u);
}

status_t vtd_fn_switch_locked(struct vtd_fn *f, struct vtd_dom *d)
{
    struct vtd_ctx *e = entry_of(f);
    if (!e)
        return ERR_BAD_STATE;
    struct vtd_ctx old = vtd_fn_read(f);
    write_entry(e, vtd_fn_entry(f, d));
    flush(f->ctl, e, sizeof(*e));
    set_cur(f, d);
    status_t st = invalidate_old(f, old);
    if (st != OK)
        kprintf("vtd: unit %u: %02x:%02x.%x to domain %u (%s): the invalidation failed (%d)\n",
                f->ctl->unit->index, f->sid >> 8, (f->sid >> 3) & 0x1f, f->sid & 7, d->ud.did,
                d->what, st);
    return st;
}

status_t vtd_fn_switch(struct vtd_fn *f, struct vtd_dom *d)
{
    mutex_lock(&f->ctl->lock);
    status_t st = vtd_fn_switch_locked(f, d);
    mutex_unlock(&f->ctl->lock);
    return st;
}

/* ---- the fault mute ------------------------------------------------------------------------- */

static struct vtd_fn *fn_by_sid(const struct vtd_ctl *ctl, uint16_t sid)
{
    for (uint32_t i = 0; i < nfns; i++)
        if (fns[i].ctl == ctl && fns[i].sid == sid)
            return &fns[i];
    return NULL;
}

void vtd_domain_fault_seen(uint32_t unit, uint16_t sid, uint32_t reason)
{
    if (reason >= VTD_FRCD_IR_FIRST)
        return;   /* an interrupt's: FPD in the IRTE would mute those, not here */
    struct vtd_ctl *ctl = vtd_ctl_get(unit);
    if (!ctl || !live(ctl))
        return;
    struct vtd_fn *f = fn_by_sid(ctl, sid);
    if (!f)
        return;
    mutex_lock(&ctl->lock);
    f->dma_faults++;
    bool mute = f->dma_faults >= VTD_FAULT_LOGGED && !f->muted && f->cur;
    if (mute) {
        f->muted = true;
        (void)vtd_fn_switch_locked(f, f->cur);   /* the same domain, FPD set; failure logged */
    }
    mutex_unlock(&ctl->lock);
    if (mute)
        kprintf("vtd: fault: unit %u: %02x:%02x.%x muted after %u DMA faults: its faults are no "
                "longer recorded (fault processing disabled in its context entry)\n", unit, sid >> 8,
                (sid >> 3) & 0x1f, sid & 7, VTD_FAULT_LOGGED);
}

/* ---- <jam/iommu.h>: domains --------------------------------------------------------------- */

bool iommu_translating(void)
{
    for (uint32_t i = 0; i < VTD_MAX_UNITS; i++)
        if (ctls[i].unit && live(&ctls[i]))
            return true;
    return false;
}

status_t iommu_device_driven(struct pci_dev *dev)
{
    struct vtd_fn *f = vtd_fn_of(dev);
    if (!f || !f->ctl->pass)
        return OK;
    mutex_lock(&f->ctl->lock);
    status_t st = OK;
    if (f->cur != f->ctl->pass) {
        f->muted = false;
        st = vtd_fn_switch_locked(f, f->ctl->pass);
    }
    mutex_unlock(&f->ctl->lock);
    return st;
}

/* The domain cap for one driver's domain: 512 table pages, 1 GiB of
 * scattered pins (docs/M11-PLAN.md). */
#define DRIVER_MAX_TABLES 512

status_t iommu_domain_create(struct pci_dev *dev, struct job *job, struct iommu_domain **out)
{
    struct vtd_fn *f = vtd_fn_of(dev);
    if (!f)
        return ERR_NOT_SUPPORTED;
    struct iommu_domain *id = kzalloc(sizeof(*id));
    if (!id)
        return ERR_NO_MEMORY;
    mutex_lock(&f->ctl->lock);
    struct vtd_dom *d = NULL;
    status_t st = vtd_dom_new(f->ctl, VTD_DOM_TABLE, job, DRIVER_MAX_TABLES, "driver", &d);
    mutex_unlock(&f->ctl->lock);
    if (st == OK) {
        st = vtd_boot_map_rmrrs(f, d);   /* the function keeps its RMRRs in every domain */
        if (st != OK) {
            mutex_lock(&f->ctl->lock);
            (void)vtd_dom_free(d);   /* never attached: nothing to wait for but its id */
            mutex_unlock(&f->ctl->lock);
        }
    }
    if (st != OK) {
        kfree(id);
        return st;
    }
    id->dom = d;
    id->fn = f;
    *out = id;
    return OK;
}

status_t iommu_domain_destroy(struct iommu_domain *id)
{
    mutex_lock(&id->fn->ctl->lock);
    status_t st = vtd_dom_free(id->dom);
    mutex_unlock(&id->fn->ctl->lock);
    if (st == OK)
        kfree(id);
    return st;
}

status_t iommu_attach(struct iommu_domain *id)
{
    struct vtd_fn *f = id->fn;
    mutex_lock(&f->ctl->lock);
    f->muted = false;
    status_t st = vtd_fn_switch_locked(f, id->dom);
    mutex_unlock(&f->ctl->lock);
    return st;
}

status_t iommu_detach(struct iommu_domain *id)
{
    struct vtd_fn *f = id->fn;
    mutex_lock(&f->ctl->lock);
    status_t st = f->cur == id->dom ? vtd_fn_switch_locked(f, f->home) : ERR_BAD_STATE;
    mutex_unlock(&f->ctl->lock);
    return st;
}

status_t iommu_map(struct iommu_domain *id, const uint64_t *pages, size_t n)
{
    return vtd_dom_map(id->dom, pages, n);
}

status_t iommu_unmap(struct iommu_domain *id, const uint64_t *pages, size_t n)
{
    return vtd_dom_unmap(id->dom, pages, n);
}
