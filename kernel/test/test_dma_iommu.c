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
 *   dma_iommu_read_only_pin
 *       A read-only pin (VMO_PIN_READ_ONLY), made through the system call
 *       layer with a VMO handle that can only read: the device reads the
 *       page (its bytes reach another, writable, pinned page), and its
 *       write there is refused: the page keeps its bytes and the unit
 *       records a write fault naming edu at that address. The domain's
 *       leaf for it has read and no write permission, the writable pin's
 *       both. The same page pinned writable through the cap is refused
 *       while the read-only pin holds it. The domain's table pages for
 *       the pins are charged to the driver's job (the cap's), not to the
 *       VMO's holder's.
 *   dma_iommu_kill_mid_dma
 *       A driver killed while its device has a transfer into its pinned
 *       page queued. Its pages are freed only once the unit has dropped
 *       the domain: at the moment they go back (DBG_DMA_RELEASED) the
 *       function's context entry no longer names the dead cap's domain.
 *       Afterwards a careless next driver turns bus mastering on at once
 *       and points the device at the freed page: it faults, and the page
 *       is never written.
 *   dma_iommu_unconfirmed_close_keeps_pages
 *       A cap closed while the unit confirms no invalidation (its queue
 *       off) keeps its pinned pages held, not released after a time; once
 *       the unit confirms again, the next try frees them.
 *   dma_iommu_unpin_races_device
 *       The device's queued write races the driver's unpin, at delays on
 *       both sides of it: once the unpin has returned the page never
 *       changes again.
 *   dma_iommu_failed_map_unconfirmed_keeps_pages
 *       A pin that fails after mapping part of itself, while the unit
 *       confirms nothing: the undo's invalidation failed, so the pin's
 *       pages are kept (the VMO can't decommit them), not handed back.
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

/* edu if a VT-d unit translates it, else NULL (said: the test skips).
 * Every test here makes a new cap before it provokes a fault, and the
 * cap's attach clears the mute and starts edu's DMA fault count again
 * (iommu_attach), so earlier tests' faults can't hide this one's. */
static struct pci_dev *edu_translated(void)
{
    struct pci_dev *d = kt_edu();
    struct vtd_fn *f = d ? vtd_fn_of(d) : NULL;
    if (d && !f)
        kprintf("ktest %s: edu is not translated (QEMU's intel-iommu and iommu=on), skipped\n",
                ktest_current);
    return f ? d : NULL;
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

/* ---- a read-only pin ----------------------------------------------------------------------- */

/* The domain leaf of pa in cap's domain: present, and writable or not. */
static bool leaf_writable(struct kobject *cap, uint64_t pa, bool *writable)
{
    struct vtd_dom *dom = dma_cap_from_kobject(cap)->dom->dom;
    uint32_t pins;
    mutex_lock(&dom->lock);
    status_t st = vtd_pt_lookup_perm(&dom->pt, pa, &pins, writable);
    mutex_unlock(&dom->lock);
    return st == OK;
}

KTEST(dma_iommu_read_only_pin)
{
    struct pci_dev *d = edu_translated();
    if (!d)
        return;
    volatile uint8_t *r = kt_edu_regs(d);
    KT_ASSERT(kt_edu_idle(r));
    /* devmgr's job holds the VMO, the driver's the cap (siblings: neither
     * count holds the other's). */
    struct job *mgr = kt_fresh_job(), *drv = kt_fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = mgr;
    handle_t dev, cap, vh, vro;
    struct khandle kh = khandle_from_new(kt_pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &dev), OK);
    KT_EQ(sys_dma_cap_create(&t, dev, kt_job_handle(&t, drv), &cap), OK);
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    /* Page 0: the device only reads it (0xc3). Pages 1 and 2 writable:
     * 1 gets what was read (0 before), 2 holds what the device then
     * tries to write into page 0 (0x5a). */
    KT_EQ(sys_vmo_create(&t, 3 * PG, 0, &vh), OK);
    KT_EQ(sys_vmo_commit(&t, vh, 0, 3 * PG), OK);
    struct kobject *vo, *capobj;
    KT_EQ(handle_get(&t, vh, OBJ_VMO, 0, &vo, NULL), OK);
    KT_EQ(handle_get(&t, cap, OBJ_DMA_CAP, 0, &capobj, NULL), OK);
    struct vmo *v = vmo_from_kobject(vo);
    vmo_fill(v, 0, 0xc3);
    vmo_fill(v, PG, 0);
    vmo_fill(v, 2 * PG, 0x5a);
    KT_EQ(handle_duplicate(&t, vh, RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP, &vro), OK);

    uint64_t dp0 = job_used(drv, JOB_LIMIT_PAGES), mp0 = job_used(mgr, JOB_LIMIT_PAGES);
    uint64_t pro, prw[2], idro, idrw;
    KT_EQ(sys_vmo_pin(&t, vro, cap, 0, PG, &pro, &idro), ERR_ACCESS_DENIED);   /* writable */
    KT_EQ(sys_vmo_pin_flags(&t, vro, cap, 0, PG, VMO_PIN_READ_ONLY, &pro, &idro), OK);
    uint64_t dp1 = job_used(drv, JOB_LIMIT_PAGES), mp1 = job_used(mgr, JOB_LIMIT_PAGES);
    KT_EQ(sys_vmo_pin(&t, vh, cap, PG, 2 * PG, prw, &idrw), OK);
    uint64_t again;
    status_t mixed = sys_vmo_pin(&t, vh, cap, 0, PG, &again, &again);
    bool ro_w = true, rw_w = false;
    bool ro_there = leaf_writable(capobj, pro, &ro_w), rw_there = leaf_writable(capobj, prw[0], &rw_w);

    kt_vtd_faults_watch(true);
    /* Page 2 -> edu -> page 0: the write is refused. First: QEMU's unit
     * records the fault only when it walks the tables; a translation
     * cached by a read of the page first refuses the write unrecorded. */
    bool ok = kt_edu_dma(r, prw[1], 0, LEN, false) && kt_edu_dma(r, 0, pro, LEN, true);
    bool ro_kept = vmo_is(v, 0, 0xc3), ro_fault = write_faulted(d, pro);
    kt_vtd_faults_watch(false);
    /* Page 0 -> edu -> page 1: the read works. */
    ok = ok && kt_edu_dma(r, pro, 0, LEN, false) && kt_edu_dma(r, 0, prw[0], LEN, true);
    bool read_landed = vmo_is(v, PG, 0xc3);

    KT_EQ(sys_vmo_unpin(&t, vro, cap, idro), OK);   /* RIGHT_READ is enough */
    KT_EQ(sys_vmo_unpin(&t, vh, cap, idrw), OK);
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 0), OK);
    kobject_unref(capobj);
    kobject_unref(vo);
    handle_table_destroy(&t);
    dma_quarantine_flush(d);   /* the domain gone */
    kprintf("ktest %s: read-only leaf %s, writable leaf %s; the read %s; the write to the "
            "read-only page %s, fault %s; a writable pin of it: %s; the pin's tables: %lu "
            "page(s) to the driver's job, %lu to the VMO's holder's\n", ktest_current,
            !ro_there ? "MISSING" : ro_w ? "WRITABLE" : "read only",
            !rw_there ? "MISSING" : rw_w ? "writable" : "READ ONLY",
            read_landed ? "landed" : "DID NOT LAND", ro_kept ? "refused" : "LANDED",
            ro_fault ? "recorded" : "MISSING", status_str(mixed), dp1 - dp0, mp1 - mp0);
    kt_job_is_empty(drv);
    kt_job_is_empty(mgr);
    job_unref(drv);
    job_unref(mgr);
    KT_ASSERT(ro_there && !ro_w);
    KT_ASSERT(rw_there && rw_w);
    KT_EQ(mixed, ERR_BAD_STATE);
    KT_ASSERT(ok);
    KT_ASSERT(read_landed);
    KT_ASSERT(ro_kept && ro_fault);
    KT_ASSERT(dp1 > dp0);   /* a new path of tables down to the page */
    KT_EQ(mp1, mp0);
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
    KT_EQ(sys_dma_cap_create(&t, dev, kt_job_handle(&t, j), &cap), OK);
    uint16_t dead_did = VTD_CTX_DID(vtd_fn_read(vtd_fn_of(d)).hi);   /* its domain's */
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    KT_EQ(sys_dma_vmo_create(&t, cap, 2 * PG, VMO_CONTIGUOUS | VMO_DMA32, &vh), OK);
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

/* ---- a close the unit doesn't confirm -------------------------------------------------- */

/* A cap closed while the unit confirms nothing (its queue off: every
 * invalidation fails): its domain can't be shown gone, and the unit may
 * still hold the function's old context entry and translations (a later
 * switch invalidates by the new entry's domain id, 6.5.1.1), so its pages
 * must stay held, not be released as a timed quarantine would. Once the
 * unit confirms again, the next try frees them. Review finding 3. */
KTEST(dma_iommu_unconfirmed_close_keeps_pages)
{
    KT_SKIP_LIVE("turns the unit's queue off for a moment");
    struct pci_dev *d = edu_translated();
    if (!d)
        return;
    struct vtd_unit *u = vtd_fn_of(d)->ctl->unit;
    dma_quarantine_flush(d);
    struct dma_quarantine_stats q0, q1, q2;
    dma_quarantine_stats(d, &q0);
    struct vmo *v;
    KT_EQ(vmo_create(PG, VMO_CONTIGUOUS | VMO_DMA32, &v), OK);
    struct kobject *cap;
    KT_EQ(dma_cap_create_for(d, NULL, &cap), OK);
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t pa, id;
    KT_EQ(vmo_pin(v, cap, 0, PG, &pa, 1, &id), OK);
    KT_EQ(vtd_qi_disable(u), OK);   /* from here every invalidation fails */
    struct khandle kh = khandle_from_new(cap, DMA_CAP_RIGHTS);
    khandle_release(&kh);           /* its last handle: closed, the page still pinned */
    dma_quarantine_flush(d);
    dma_quarantine_stats(d, &q1);
    status_t on = vtd_qi_enable(u);
    dma_quarantine_flush(d);        /* confirmed now */
    dma_quarantine_stats(d, &q2);
    kobject_unref(vmo_kobject(v));
    kprintf("ktest %s: unconfirmed: %lu pin(s) held, %lu page(s) released; confirmed: %lu "
            "held, %lu freed\n", ktest_current, q1.pins - q0.pins, q1.released - q0.released,
            q2.pins - q0.pins, q2.freed - q0.freed);
    KT_EQ(on, OK);
    KT_EQ(q1.pins, q0.pins + 1);         /* still held */
    KT_EQ(q1.released, q0.released);     /* not released after a time */
    KT_EQ(q1.freed, q0.freed);
    KT_EQ(q2.pins, q0.pins);
    KT_EQ(q2.freed, q0.freed + 1);       /* freed once the domain was confirmed gone */
    KT_EQ(q2.released, q0.released);
}

/* ---- a device racing an unpin -------------------------------------------------------------- */

/* The device has a write to the page queued (edu runs a transfer about
 * 100 ms after it is told) while the driver unpins it, at delays from
 * well before to well after the write: whatever the order, once vmo_unpin
 * has returned the page never changes again (the write either landed
 * before the unpin's invalidation completed, or is blocked and recorded).
 * Both outcomes are counted; the second is the one that matters. */
KTEST(dma_iommu_unpin_races_device)
{
    struct pci_dev *d = edu_translated();
    if (!d)
        return;
    volatile uint8_t *r = kt_edu_regs(d);
    KT_ASSERT(kt_edu_idle(r));
    static const unsigned delay_ms[] = { 0, 20, 50, 80, 90, 95, 98, 100, 102, 105, 110, 130, 160 };
    const unsigned n = sizeof(delay_ms) / sizeof(delay_ms[0]);
    struct vmo *src, *v;
    KT_EQ(vmo_create(PG, VMO_CONTIGUOUS | VMO_DMA32, &src), OK);
    KT_EQ(vmo_create(PG, 0, &v), OK);
    KT_EQ(vmo_commit(v, 0, PG), OK);
    vmo_fill(src, 0, 0xa5);
    struct kobject *cap;
    KT_EQ(dma_cap_create_for(d, NULL, &cap), OK);
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t spa, sid;
    KT_EQ(vmo_pin(src, cap, 0, PG, &spa, 1, &sid), OK);
    KT_ASSERT(kt_edu_dma(r, spa, 0, LEN, false));   /* 0xa5 in edu's buffer from here on */

    unsigned landed = 0, blocked = 0, changed_after = 0;
    bool idle = true;
    for (unsigned i = 0; i < n; i++) {
        vmo_fill(v, 0, 0);
        uint64_t pa, id;
        KT_EQ(vmo_pin(v, cap, 0, PG, &pa, 1, &id), OK);
        kt_edu_dma_start(r, 0, pa, LEN, true);
        if (delay_ms[i])
            thread_sleep_ms(delay_ms[i]);
        KT_EQ(vmo_unpin(v, cap, id), OK);
        bool at_unpin = vmo_is(v, 0, 0xa5);   /* landed before the unpin returned? */
        idle = kt_edu_idle(r) && idle;
        thread_sleep_ms(2);
        bool later = vmo_is(v, 0, 0xa5);
        landed += at_unpin;
        blocked += !at_unpin;
        changed_after += at_unpin != later;
    }
    KT_EQ(vmo_unpin(src, cap, sid), OK);
    KT_EQ(dma_cap_bus_master(cap, false), OK);
    kobject_unref(cap);
    dma_quarantine_flush(d);
    kobject_unref(vmo_kobject(src));
    kobject_unref(vmo_kobject(v));
    kprintf("ktest %s: %u transfer(s) landed before the unpin returned, %u blocked after it; "
            "%u page(s) CHANGED after the unpin returned\n", ktest_current, landed, blocked,
            changed_after);
    KT_ASSERT(idle);
    KT_EQ(changed_after, 0);
    KT_EQ(landed + blocked, n);
    KT_IDLE_ASSERT(blocked >= 1);   /* the 0 ms unpin comes ~100 ms before the transfer */
}

/* ---- a failed map the unit doesn't confirm undone ---------------------------------------- */

/* A writable pin of two pages whose second is pinned read only already
 * fails (one permission per page), after its first page was mapped: its
 * leaf was present for a moment, so the device may have cached it. With
 * the unit confirming nothing (its queue off) the undo's invalidation
 * fails, and the pin must keep its pages (as an unconfirmed unpin does),
 * not hand them back to the VMO, whose decommit or death would give them
 * to the allocator while the unit may still translate to them. Review
 * KDEV finding 1: without the fix the map's own error (ERR_BAD_STATE) came
 * back and the page was released. The page is kept for good, as such a
 * pin's pages always are: a leak on purpose. */
KTEST(dma_iommu_failed_map_unconfirmed_keeps_pages)
{
    KT_SKIP_LIVE("turns the unit's queue off for a moment");
    struct pci_dev *d = edu_translated();
    if (!d)
        return;
    KT_OWN_LEAK_CHECK("a page whose unmap the unit didn't confirm is kept for good");
    struct vtd_unit *u = vtd_fn_of(d)->ctl->unit;
    dma_quarantine_flush(d);
    struct vmo *v;
    KT_EQ(vmo_create(2 * PG, 0, &v), OK);
    KT_EQ(vmo_commit(v, 0, 2 * PG), OK);
    struct kobject *cap;
    KT_EQ(dma_cap_create_for(d, NULL, &cap), OK);
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t pa[2], ro_id, id = 0;
    KT_EQ(vmo_pin_flags(v, cap, PG, PG, VMO_PIN_READ_ONLY, &pa[1], 1, &ro_id), OK);
    KT_EQ(vtd_qi_disable(u), OK);   /* from here every invalidation fails */
    status_t pin = vmo_pin(v, cap, 0, 2 * PG, pa, 2, &id);
    status_t dc = vmo_decommit(v, 0, PG);   /* page 0: kept, or back with the VMO */
    status_t on = vtd_qi_enable(u);
    KT_EQ(vmo_unpin(v, cap, ro_id), OK);
    KT_EQ(dma_cap_bus_master(cap, false), OK);
    kobject_unref(cap);
    dma_quarantine_flush(d);   /* its domain gone */
    kobject_unref(vmo_kobject(v));   /* (kept alive by the kept pin: on purpose) */
    kprintf("ktest %s: the pin with the undo unconfirmed: %s; page 0 then %s\n", ktest_current,
            status_str(pin), dc == OK ? "DECOMMITTED (released)" : "kept");
    KT_EQ(on, OK);
    KT_ASSERT(pin == ERR_IO || pin == ERR_TIMED_OUT);
    KT_EQ(dc, ERR_BAD_STATE);
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
