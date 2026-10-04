/* DMA through the IOMMU (kernel/object/dma_cap.c and vmo.c's pin paths
 * on <jam/iommu.h>). They need QEMU's edu under translation (its
 * intel-iommu and the boot word iommu=on) and skip themselves without it;
 * the same rules without the IOMMU are test_dma.c's.
 *
 *   dma_iommu_unpinned_page_blocked
 *       A cap's device reaches the pages its cap pinned and nothing else:
 *       its write to a pinned page lands; to a page nobody pinned, nothing
 *       changes and the unit records a write fault naming edu at that
 *       address; to a page once pinned, after its unpin (which waited for
 *       the invalidation), the same.
 *   dma_iommu_kill_mid_dma
 *       A driver killed while its device has a transfer into its pinned
 *       page queued. Its pages are freed only once the unit has dropped
 *       the domain: at the moment they go back (DBG_DMA_RELEASED) the
 *       function's context entry no longer names the dead cap's domain.
 *       Afterwards a careless next driver turns bus mastering on at once
 *       and points the device at the freed page: it faults, and the page
 *       is never written.
 *   dma_iommu_pin_cost
 *       What pins and unpins cost now, in the unit's invalidation waits
 *       and descriptors (for M11.5's numbers): a pin waits for nothing (one
 *       wait in caching mode), an unpin for one, whatever its size; a cap's
 *       creation and close, a few. Printed; the per-pin counts checked. */
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/vmo.h>
#include <jam/vtd.h>

#include "../dev/vtd_domain.h"
#include "../dev/vtd_internal.h"

#define PG  PAGE_SIZE
#define LEN 64u   /* bytes per edu transfer */

/* edu if a VT-d unit translates it, else NULL (said: the test skips). Its
 * DMA fault count starts again from 0, so the unit's mute (after
 * VTD_FAULT_LOGGED faults) can't hide this test's faults; a new cap's
 * domain clears the mute itself. */
static struct pci_dev *edu_translated(void)
{
    struct pci_dev *d = kt_edu();
    struct vtd_fn *f = d ? vtd_fn_of(d) : NULL;
    if (d && !f)
        kprintf("ktest %s: edu is not translated (QEMU's intel-iommu and iommu=on), skipped\n",
                ktest_current);
    if (!f)
        return NULL;
    mutex_lock(&f->ctl->lock);
    f->dma_faults = 0;
    mutex_unlock(&f->ctl->lock);
    return d;
}

static uint16_t rid(const struct pci_dev *d)
{
    return (uint16_t)(d->info.bus << 8 | d->info.dev << 3 | d->info.fn);
}

/* A DMA write fault (not an interrupt's) from requester sid at page. */
struct write_fault {
    uint16_t sid;
    uint64_t page;
};

static bool write_fault_is(const struct vtd_fault_rec *r, const void *arg)
{
    const struct write_fault *w = arg;
    return VTD_FRCD_REASON(r->hi) < VTD_FRCD_IR_FIRST && VTD_FRCD_SID(r->hi) == w->sid &&
           !VTD_FRCD_TYPE1(r->hi) && !VTD_FRCD_TYPE2(r->hi) && (r->lo & ~0xfffull) == w->page;
}

/* Did the unit record d's write at pa (kt_vtd_faults_watch on)? */
static bool write_faulted(const struct pci_dev *d, uint64_t pa)
{
    const struct write_fault w = { rid(d), pa };
    return kt_vtd_fault_wait(write_fault_is, &w, NULL);
}

/* LEN bytes of v at off set to byte, or checked to be byte. */
static void vmo_fill(struct vmo *v, uint64_t off, uint8_t byte)
{
    static uint8_t buf[LEN];
    memset(buf, byte, sizeof(buf));
    KT_EQ(vmo_write(v, off, buf, sizeof(buf)), OK);
}

static bool vmo_is(struct vmo *v, uint64_t off, uint8_t byte)
{
    static uint8_t buf[LEN];
    KT_EQ(vmo_read(v, off, buf, sizeof(buf)), OK);
    for (uint32_t i = 0; i < LEN; i++)
        if (buf[i] != byte)
            return false;
    return true;
}

static bool phys_is(uint64_t pa, uint8_t byte)
{
    const uint8_t *p = phys_to_virt(pa);
    for (uint32_t i = 0; i < LEN; i++)
        if (p[i] != byte)
            return false;
    return true;
}

/* ---- only what was pinned ---------------------------------------------------------------- */

KTEST(dma_iommu_unpinned_page_blocked)
{
    struct pci_dev *d = edu_translated();
    if (!d)
        return;
    volatile uint8_t *r = kt_edu_regs(d);
    KT_ASSERT(kt_edu_idle(r));
    struct vmo *v;
    KT_EQ(vmo_create(2 * PG, VMO_CONTIGUOUS | VMO_DMA32, &v), OK);
    vmo_fill(v, 0, 0xc3);
    vmo_fill(v, PG, 0);
    uint64_t other = pmm_alloc_page_phys(PMM_DMA32);   /* nobody pins it */
    KT_ASSERT(other);
    memset(phys_to_virt(other), 0x5a, PG);
    /* A cap with no handle: its destroy gives the domain up. */
    struct kobject *cap;
    KT_EQ(dma_cap_create_for(d, NULL, &cap), OK);
    KT_ASSERT(dma_cap_translated(cap));
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t pa[2], id;
    KT_EQ(vmo_pin(v, cap, 0, 2 * PG, pa, 2, &id), OK);
    kt_vtd_faults_watch(true);

    /* Pinned: page 0 -> edu -> page 1 lands. */
    bool ok = kt_edu_dma(r, pa[0], 0, LEN, false) && kt_edu_dma(r, 0, pa[1], LEN, true);
    bool landed = vmo_is(v, PG, 0xc3);
    /* Never pinned: blocked, recorded. */
    ok = ok && kt_edu_dma(r, 0, other, LEN, true);
    bool other_kept = phys_is(other, 0x5a), other_fault = write_faulted(d, other);
    /* Unpinned (the unit's cached translation dropped before the unpin
     * returned): blocked, recorded. */
    KT_EQ(vmo_unpin(v, cap, id), OK);
    vmo_fill(v, PG, 0x22);
    ok = ok && kt_edu_dma(r, 0, pa[1], LEN, true);
    bool unpin_kept = vmo_is(v, PG, 0x22), unpin_fault = write_faulted(d, pa[1]);

    kt_vtd_faults_watch(false);
    KT_EQ(dma_cap_bus_master(cap, false), OK);
    kobject_unref(cap);
    dma_quarantine_flush(d);   /* its domain gone */
    kobject_unref(vmo_kobject(v));
    pmm_free_page_phys(other);
    kprintf("ktest %s: pinned page %s; unpinned page %s, fault %s; after the unpin %s, fault "
            "%s\n", ktest_current, landed ? "written" : "NOT WRITTEN",
            other_kept ? "untouched" : "WRITTEN", other_fault ? "recorded" : "MISSING",
            unpin_kept ? "untouched" : "WRITTEN", unpin_fault ? "recorded" : "MISSING");
    KT_ASSERT(ok);
    KT_ASSERT(landed);
    KT_ASSERT(other_kept && other_fault);
    KT_ASSERT(unpin_kept && unpin_fault);
}

/* ---- a driver killed mid-DMA --------------------------------------------------------------- */

static struct pci_dev *kill_dev;
static uint16_t kill_did;        /* the context entry's domain id when the pages went back */
static volatile int kill_calls;

static void kill_release_hook(void *arg)
{
    if (arg != kill_dev)
        return;
    kill_did = VTD_CTX_DID(vtd_fn_read(vtd_fn_of(kill_dev)).hi);
    kill_calls++;
}

KTEST(dma_iommu_kill_mid_dma)
{
    struct pci_dev *d = edu_translated();
    if (!d)
        return;
    volatile uint8_t *r = kt_edu_regs(d);
    KT_ASSERT(kt_edu_idle(r));
    dma_quarantine_flush(d);
    struct dma_quarantine_stats q0, q;
    dma_quarantine_stats(d, &q0);

    /* The driver: its own job and handle table, as devmgr's drivers have. */
    struct job *j = kt_fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = j;
    handle_t dev, cap, vh;
    struct khandle kh = khandle_from_new(kt_pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &dev), OK);
    KT_EQ(sys_dma_cap_create(&t, dev, &cap), OK);
    uint16_t dead_did = VTD_CTX_DID(vtd_fn_read(vtd_fn_of(d)).hi);   /* its domain's */
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    KT_EQ(sys_vmo_create(&t, 2 * PG, VMO_CONTIGUOUS | VMO_DMA32, cap, &vh), OK);
    struct kobject *vo;
    KT_EQ(handle_get(&t, vh, OBJ_VMO, 0, &vo, NULL), OK);   /* ours, to look afterwards */
    struct vmo *v = vmo_from_kobject(vo);
    vmo_fill(v, 0, 0x3c);
    vmo_fill(v, PG, 0x6b);
    uint64_t pa[2], id;
    KT_EQ(sys_vmo_pin(&t, vh, cap, 0, 2 * PG, pa, &id), OK);
    KT_ASSERT(kt_edu_dma(r, pa[0], 0, LEN, false));   /* 0x3c into edu's buffer */

    /* Killed with a transfer to page 1 queued (edu starts it ~100 ms on). */
    kill_dev = d;
    kill_calls = 0;
    __atomic_store_n(&dbg_hooks[DBG_DMA_RELEASED], kill_release_hook, __ATOMIC_RELEASE);
    kt_edu_dma_start(r, 0, pa[1], LEN, true);
    handle_table_destroy(&t);   /* the kill: every handle closes */
    dma_quarantine_flush(d);    /* the release done (by the release thread, or here) */
    __atomic_store_n(&dbg_hooks[DBG_DMA_RELEASED], NULL, __ATOMIC_RELEASE);
    dma_quarantine_stats(d, &q);
    bool stale_idle = kt_edu_idle(r);

    /* A careless next driver: bus mastering on at once, and its device
     * told to write the dead driver's freed page. */
    struct kobject *cap2;
    KT_EQ(dma_cap_create_for(d, NULL, &cap2), OK);
    KT_EQ(dma_cap_bus_master(cap2, true), OK);
    kt_vtd_faults_watch(true);
    bool ok = kt_edu_dma(r, 0, pa[1], LEN, true);
    bool fault = write_faulted(d, pa[1]);
    kt_vtd_faults_watch(false);
    bool kept = vmo_is(v, PG, 0x6b);
    KT_EQ(dma_cap_bus_master(cap2, false), OK);
    kobject_unref(cap2);
    dma_quarantine_flush(d);
    kobject_unref(vo);
    kt_job_is_empty(j);
    job_unref(j);
    kprintf("ktest %s: pages freed with the entry naming domain %u (the dead driver's: %u); "
            "%lu page(s) freed; a write to the freed page: %s, fault %s\n", ktest_current,
            kill_did, dead_did, q.freed - q0.freed, kept ? "blocked" : "LANDED",
            fault ? "recorded" : "MISSING");
    KT_EQ(kill_calls, 1);
    KT_ASSERT(kill_did != dead_did);   /* the domain was gone first */
    KT_EQ(q.pins, q0.pins);
    KT_EQ(q.freed, q0.freed + 2);
    KT_EQ(q.released, q0.released);   /* nothing quarantined */
    KT_EQ(q.changed, q0.changed);     /* and nothing wrote them meanwhile */
    KT_ASSERT(stale_idle && ok);
    KT_ASSERT(kept && fault);
}

/* ---- what a pin costs --------------------------------------------------------------------- */

struct cost {
    uint64_t waits;   /* the unit's submissions: each ends in one invalidation wait */
    uint64_t descs;   /* descriptors, the waits' own included */
};

static struct cost cost_now(const struct vtd_unit *u)
{
    return (struct cost){ __atomic_load_n(&u->stats.submissions, __ATOMIC_RELAXED),
                          __atomic_load_n(&u->stats.descriptors, __ATOMIC_RELAXED) };
}

/* The cost since *mark; *mark moves to now. */
static struct cost cost_step(const struct vtd_unit *u, struct cost *mark)
{
    struct cost now = cost_now(u);
    struct cost c = { now.waits - mark->waits, now.descs - mark->descs };
    *mark = now;
    return c;
}

/* Pin all of v (n pages) and unpin it; their costs into pin[i], unpin[i]. */
static void pin_and_unpin(const struct vtd_unit *u, struct kobject *cap, struct vmo *v,
                          uint64_t n, struct cost *pin, struct cost *unpin)
{
    static uint64_t pa[64];
    uint64_t id;
    struct cost mark = cost_now(u);
    KT_EQ(vmo_pin(v, cap, 0, n * PG, pa, n, &id), OK);
    *pin = cost_step(u, &mark);
    KT_EQ(vmo_unpin(v, cap, id), OK);
    *unpin = cost_step(u, &mark);
}

KTEST(dma_iommu_pin_cost)
{
    struct pci_dev *d = edu_translated();
    if (!d)
        return;
    const struct vtd_unit *u = vtd_fn_of(d)->ctl->unit;
    bool cm = VTD_CAP_CM(u->cap);
    struct vmo *v[3];
    static const uint64_t pages[3] = { 1, 64, 64 };
    static const char *const what[3] = { "1 page", "64 pages in one run",
                                         "64 pages as a paged VMO has them" };
    KT_EQ(vmo_create(PG, VMO_CONTIGUOUS | VMO_DMA32, &v[0]), OK);
    KT_EQ(vmo_create(64 * PG, VMO_CONTIGUOUS | VMO_DMA32, &v[1]), OK);
    KT_EQ(vmo_create(64 * PG, 0, &v[2]), OK);

    struct cost mark = cost_now(u), pin[3], unpin[3];
    struct kobject *cap;
    KT_EQ(dma_cap_create_for(d, NULL, &cap), OK);
    struct cost create = cost_step(u, &mark);
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    for (int i = 0; i < 3; i++)
        pin_and_unpin(u, cap, v[i], pages[i], &pin[i], &unpin[i]);
    /* The close with 64 pages still pinned: the domain goes as a whole. */
    static uint64_t pa[64];
    uint64_t id;
    KT_EQ(vmo_pin(v[2], cap, 0, 64 * PG, pa, 64, &id), OK);
    mark = cost_now(u);
    struct khandle kh = khandle_from_new(cap, DMA_CAP_RIGHTS);
    khandle_release(&kh);   /* its last handle: closed */
    dma_quarantine_flush(d);
    struct cost close = cost_step(u, &mark);
    for (int i = 0; i < 3; i++)
        kobject_unref(vmo_kobject(v[i]));

    kprintf("ktest %s: caching mode %d; a cap's creation: %lu invalidation wait(s), %lu "
            "descriptor(s); its close with 64 pages pinned: %lu, %lu\n", ktest_current, cm,
            create.waits, create.descs, close.waits, close.descs);
    for (int i = 0; i < 3; i++)
        kprintf("ktest %s: %s: pin %lu wait(s), %lu descriptor(s); unpin %lu, %lu (one map "
                "and one unmap call)\n", ktest_current, what[i], pin[i].waits, pin[i].descs,
                unpin[i].waits, unpin[i].descs);
    /* Another device's invalidations would land in these counts too. */
    for (int i = 0; i < 3; i++) {
        KT_IDLE_EQ(pin[i].waits, cm ? 1u : 0u);
        KT_IDLE_EQ(unpin[i].waits, 1);
    }
}
