/* DMA capabilities and safe rebind (kernel/object/dma_cap.c). QEMU only:
 * they need the edu device; each skips itself without it. They run in
 * both modes, each checking what its mode promises: with the IOMMU
 * translating edu (QEMU's intel-iommu and iommu=on) a closed cap's pages
 * are freed at once once its domain is gone; without it they are
 * quarantined. The IOMMU's own (edu blocked, pin cost) are in
 * test_dma_iommu.c.
 *
 *   dma_stale_write_after_rebind
 *       A driver dies while its device has a device -> RAM transfer queued
 *       (QEMU's edu starts it 100 ms after the command). Three rounds:
 *       nobody rebinds (control); the next driver quiesces the device (its
 *       DMA engine idle) before it turns bus mastering on, as drivers must;
 *       a careless next driver turns it on at once. The first two must
 *       write 0 bytes. Without the IOMMU the third's stale write lands, but
 *       in pages the quarantine still holds (never in released memory),
 *       and the quarantine's release sees the changed page; with it the
 *       third writes nothing either (the new driver's domain is empty) and
 *       every round's pages are freed at once, unchanged.
 *   dma_cap_owner_rules
 *       Only the function's current (newest) cap turns bus mastering on or
 *       pins; an older cap's close leaves Bus Master Enable alone; a new cap
 *       starts with it off.
 *   dma_quarantine_phys_and_clean_close
 *       A clean close (nothing pinned) holds nothing; pins of a physical
 *       VMO (no RAM) are released at once, the RAM one quarantined (or
 *       freed once the IOMMU took it away).
 *   dma_quarantine_stats_consistent
 *       A reader of the stats in the middle of a release (forced with the
 *       DBG_DMA_RELEASED hook) sees the batch either still held or already
 *       released (or freed), never gone from `pages` but not yet counted.
 *   dma_cap_charges_driver_job
 *       dma_cap_create charges the job it is given (the driver's, which
 *       devmgr makes first), not its caller's: the cap, and with the IOMMU
 *       its domain's table pages, the first ones and those a pin adds; the
 *       caller pays only for its handle and its own VMO. Only a job the
 *       caller manages may be named. Once the cap is gone the driver's job
 *       is charged for nothing.
 *   dma_pin_charges_pinner
 *       A pin's handle unit is the pinner's: the cap's job (the driver's),
 *       never the VMO's holder (a client that handed the driver a view of
 *       its buffer). A client with no handle units to spare is pinned all
 *       the same; a driver with none is refused (ERR_NO_RESOURCES). Each
 *       unit comes back to the driver's job at the unpin, at the cap's
 *       close (with the IOMMU) or at the quarantine's release (without),
 *       and a cap with no job charges nobody. Review M12.1 KDEV finding 4
 *       / KCORE finding 6.
 *   dma_read_only_pin_rules
 *       A read-only pin (VMO_PIN_READ_ONLY) needs only RIGHT_READ where
 *       the IOMMU makes it read only, and RIGHT_WRITE as any pin where it
 *       doesn't (the device could write through it); a sealed VMO takes a
 *       read-only pin only in the first case. Unpinning needs RIGHT_READ.
 *       An unknown flag is refused. (The device's refused write is
 *       test_dma_iommu.c's dma_iommu_read_only_pin.)
 *   dma_read_only_pin_needs_committed
 *       A read-only pin takes only pages already committed: over a sparse
 *       buffer's holes it is ERR_BAD_STATE (whichever handle), commits
 *       nothing and charges neither the VMO's job nor the pinner's; over
 *       committed pages it works as before. A writable pin still commits
 *       what it covers. Review M12.1 KDEV finding 4 / KCORE finding 6.
 *   m6r_pins_are_charged
 *       Every pin is a kernel allocation, so it is charged: otherwise a
 *       driver pins one page over and over and fills the kernel heap.
 *   m6r_unpin_by_other_holder
 *       Only the pin's own DMA capability may unpin it, not another holder
 *       of the VMO, and the pinned page can't be decommitted. */
#include <jam/aspace_vmo.h>
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
#include <jam/time.h>
#include <jam/vmo.h>

#define PG      PAGE_SIZE
#define CMD_BME 0x04

static bool bme(struct pci_dev *d)
{
    return pci_cfg_read(d, 0x04, 2) & CMD_BME;
}

/* A driver's dma_cap: a handle to a fresh cap for d (the function's new
 * current one); *cap is an extra reference to look at it afterwards. */
static struct khandle new_cap(struct pci_dev *d, struct kobject **cap)
{
    KT_EQ(dma_cap_create_for(d, NULL, cap), OK);
    kobject_ref(*cap);
    return khandle_from_new(*cap, DMA_CAP_RIGHTS);
}

KTEST(dma_stale_write_after_rebind)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    volatile uint8_t *r = kt_edu_regs(d);
    KT_ASSERT(kt_edu_idle(r));
    dma_quarantine_flush(d);
    struct dma_quarantine_stats q0, q;
    dma_quarantine_stats(d, &q0);

    /* The dead driver's buffer: page 0 the pattern, page 1 the target. */
    struct vmo *v;
    KT_EQ(vmo_create(2 * PG, VMO_CONTIGUOUS | VMO_DMA32, &v), OK);
    static uint8_t buf[PAGE_SIZE];
    for (uint32_t i = 0; i < PG; i++)
        buf[i] = (uint8_t)(0xa5 ^ i);
    KT_EQ(vmo_write(v, 0, buf, PG), OK);

    /* The pattern into the device's buffer, cleanly (unpinned before the
     * close: nothing to quarantine). */
    struct kobject *cap;
    struct khandle kh = new_cap(d, &cap);
    KT_ASSERT(!bme(d));   /* a new cap starts with bus mastering off */
    bool tr = dma_cap_translated(cap);   /* the IOMMU translates edu: the mode under test */
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t pa[2], id;
    KT_EQ(vmo_pin(v, cap, 0, 2 * PG, pa, 2, &id), OK);
    KT_ASSERT(kt_edu_dma(r, pa[0], 0, PG, false));
    KT_EQ(vmo_unpin(v, cap, id), OK);
    khandle_release(&kh);
    kobject_unref(cap);
    dma_quarantine_flush(d);   /* (with the IOMMU: its domain gone) */
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, q0.pins);
    KT_EQ(q.freed, q0.freed);   /* unpinned: nothing left to free */

    static const char *const what[3] = { "no rebind (control)", "rebind, driver quiesced first",
                                         "rebind, bus mastering on at once" };
    uint32_t hit[3];
    bool idle[3], held[3];
    uint64_t changed[3], freed[3];
    for (int round = 0; round < 3; round++) {
        /* A driver (one dma_cap handle, bus mastering on) with a transfer
         * to its page 1 queued when it dies: BME off, the pin handed to
         * the release thread (quarantined, or freed once the IOMMU has
         * taken the domain away). */
        kh = new_cap(d, &cap);
        KT_EQ(dma_cap_bus_master(cap, true), OK);
        KT_EQ(vmo_pin(v, cap, 0, 2 * PG, pa, 2, &id), OK);
        kt_edu_dma_start(r, 0, pa[1], PG, true);
        khandle_release(&kh);
        KT_ASSERT(!bme(d));
        KT_EQ(dma_cap_pin_count(cap), 0);
        KT_EQ(vmo_unpin(v, cap, id), ERR_NOT_FOUND);   /* not the cap's any more */
        kobject_unref(cap);
        dma_quarantine_stats(d, &q);
        if (!tr) {   /* (with the IOMMU the release thread may be done already) */
            KT_EQ(q.pins, q0.pins + 1);
            KT_EQ(q.pages, q0.pages + 2);
        }

        struct khandle kh2 = { 0 };
        struct kobject *cap2 = NULL;
        if (round) {
            kh2 = new_cap(d, &cap2);
            if (round == 1)
                KT_ASSERT(kt_edu_idle(r));   /* quiesce first, as a driver must */
            KT_EQ(dma_cap_bus_master(cap2, true), OK);
        }
        idle[round] = kt_edu_idle(r);
        dma_quarantine_stats(d, &q);
        held[round] = q.pins == q0.pins + 1;   /* the pages were still quarantined */
        if (round) {
            khandle_release(&kh2);   /* BME off (a clean close: no pins) */
            kobject_unref(cap2);
        }
        memset(buf, 0, sizeof(buf));
        KT_EQ(vmo_read(v, PG, buf, PG), OK);
        hit[round] = 0;
        for (uint32_t i = 0; i < PG; i++)
            hit[round] += buf[i] && buf[i] == (uint8_t)(0xa5 ^ i);   /* (16 bytes of it are 0) */
        if (round == 1 && !tr) {
            /* The reaper lets it go a grace period after bus mastering
             * went on (no flush: this is its path). */
            uint64_t until = uptime_ns() + DMA_QUARANTINE_GRACE_NS + 5 * NS_PER_S;
            do {
                thread_sleep_ms(20);
                dma_quarantine_stats(d, &q);
            } while (q.pins > q0.pins && uptime_ns() < until);
            KT_EQ(q.pins, q0.pins);
        } else {
            dma_quarantine_flush(d);
        }
        struct dma_quarantine_stats after;
        dma_quarantine_stats(d, &after);
        changed[round] = after.changed - q0.changed;
        freed[round] = after.freed - q0.freed;
        q0 = after;
        memset(buf, 0, sizeof(buf));
        KT_EQ(vmo_write(v, PG, buf, PG), OK);
    }
    for (int round = 0; round < 3; round++)
        kprintf("ktest %s: %s: the stale transfer wrote %u bytes (of %u), %s; %lu page(s) "
                "freed at once, %lu found changed\n", ktest_current, what[round], hit[round],
                (unsigned)PG - 16,
                tr ? "the IOMMU translating" : held[round] ? "the pages still quarantined"
                                                           : "RELEASED",
                freed[round], changed[round]);
    kobject_unref(vmo_kobject(v));
    KT_ASSERT(idle[0] && idle[1] && idle[2]);
    KT_EQ(hit[0], 0);          /* BME off stops it */
    KT_EQ(hit[1], 0);          /* ... and a driver that quiesces first keeps it stopped */
    KT_EQ(changed[0], 0);
    KT_EQ(changed[1], 0);
    if (tr) {
        /* Nothing quarantined: the dead driver's pages were freed at once,
         * and the careless driver's device, in its own empty domain,
         * reached none of them. */
        KT_EQ(hit[2], 0);
        KT_EQ(changed[2], 0);
        for (int round = 0; round < 3; round++)
            KT_EQ(freed[round], 2);
        return;
    }
    KT_ASSERT(held[0] && held[1] && held[2]);
    for (int round = 0; round < 3; round++)
        KT_EQ(freed[round], 0);
    KT_ASSERT(hit[2] > 0);     /* a careless driver lets it through ... */
    KT_EQ(changed[2], 1);      /* ... into its quarantined page 1, seen at release */
}

KTEST(dma_cap_owner_rules)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    struct kobject *a, *b, *u;
    struct khandle ka = new_cap(d, &a);
    KT_EQ(dma_cap_bus_master(a, true), OK);
    KT_ASSERT(bme(d));
    struct khandle kb = new_cap(d, &b);   /* the function's new owner */
    KT_ASSERT(!bme(d));                   /* starts with bus mastering off */
    KT_EQ(dma_cap_bus_master(a, true), ERR_BAD_STATE);   /* no longer current */
    KT_EQ(dma_cap_bus_master(a, false), ERR_BAD_STATE);
    KT_ASSERT(!dma_cap_bus_master_on(a));
    KT_EQ(dma_cap_bus_master(b, true), OK);
    KT_ASSERT(dma_cap_bus_master_on(b));
    KT_ASSERT(!dma_cap_bus_master_on(a));   /* BME is on, but not for it: no pins */
    struct vmo *v;
    KT_EQ(vmo_create(PG, VMO_CONTIGUOUS | VMO_DMA32, &v), OK);
    uint64_t pa, id;
    KT_EQ(vmo_pin(v, a, 0, PG, &pa, 1, &id), ERR_BAD_STATE);
    khandle_release(&ka);   /* an older cap's close leaves the new owner's BME alone */
    KT_ASSERT(bme(d));
    KT_EQ(dma_cap_bus_master(b, false), OK);
    KT_ASSERT(!bme(d));
    KT_EQ(dma_cap_bus_master(b, true), OK);
    khandle_release(&kb);   /* the current cap's close turns it off */
    KT_ASSERT(!bme(d));
    KT_EQ(dma_cap_bus_master(b, true), ERR_BAD_STATE);   /* closed: nobody's owner */
    KT_EQ(dma_cap_create(&u), OK);
    KT_EQ(dma_cap_bus_master(u, true), ERR_NOT_SUPPORTED);   /* unbound */
    KT_EQ(dma_cap_bus_master(vmo_kobject(v), true), ERR_WRONG_TYPE);
    kobject_unref(u);
    kobject_unref(vmo_kobject(v));
    kobject_unref(a);
    kobject_unref(b);
}

/* dma_cap_bus_master takes on (1) or off (0): anything else is refused
 * before the cap is looked at (fail closed), here on an unbound cap, which
 * would otherwise answer ERR_NOT_SUPPORTED. */
KTEST(dma_cap_bus_master_refuses_other_values)
{
    struct kobject *u;
    KT_EQ(dma_cap_create(&u), OK);
    struct handle_table t;
    handle_table_init(&t);
    handle_t h;
    struct khandle kh = khandle_from_new(u, DMA_CAP_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    KT_EQ(sys_dma_cap_bus_master(&t, h, 2), ERR_INVALID_ARGS);
    KT_EQ(sys_dma_cap_bus_master(&t, h, 0x100), ERR_INVALID_ARGS);
    KT_EQ(sys_dma_cap_bus_master(&t, h, 1), ERR_NOT_SUPPORTED);   /* unbound */
    handle_table_destroy(&t);
}

KTEST(dma_quarantine_phys_and_clean_close)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    dma_quarantine_flush(d);
    struct dma_quarantine_stats q0, q;
    dma_quarantine_stats(d, &q0);
    /* Physical memory (the device's own BAR: peer-to-peer) and RAM, both
     * pinned when the cap closes. */
    struct vmo *phys, *ram;
    KT_EQ(vmo_create_physical(d->info.bar[0].phys, PG, VM_UC, &phys), OK);
    KT_EQ(vmo_create(PG, 0, &ram), OK);
    struct kobject *cap;
    struct khandle kh = new_cap(d, &cap);
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t pa, id;
    KT_EQ(vmo_pin(phys, cap, 0, PG, &pa, 1, &id), OK);
    KT_EQ(pa, d->info.bar[0].phys);
    KT_EQ(vmo_pin(ram, cap, 0, PG, &pa, 1, &id), OK);
    uint32_t prefs = __atomic_load_n(&vmo_kobject(phys)->refs, __ATOMIC_RELAXED);
    bool tr = dma_cap_translated(cap);
    khandle_release(&kh);
    dma_quarantine_stats(d, &q);
    KT_EQ(__atomic_load_n(&vmo_kobject(phys)->refs, __ATOMIC_RELAXED), prefs - 1);   /* released */
    if (!tr) {   /* (with the IOMMU the release thread may be done already) */
        KT_EQ(q.pins, q0.pins + 1);   /* the RAM pin only */
        KT_EQ(q.pages, q0.pages + 1);
        KT_EQ(vmo_decommit(ram, 0, PG), ERR_BAD_STATE);   /* still held */
    }
    dma_quarantine_flush(d);
    KT_EQ(vmo_decommit(ram, 0, PG), OK);
    dma_quarantine_stats(d, &q);
    KT_EQ(q.freed, q0.freed + (tr ? 1 : 0));   /* the RAM page: freed at once, or released */
    KT_EQ(q.released, q0.released + (tr ? 0 : 1));
    kobject_unref(cap);

    /* A clean close: everything unpinned first. */
    kh = new_cap(d, &cap);
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    KT_EQ(vmo_pin(ram, cap, 0, PG, &pa, 1, &id), OK);
    KT_EQ(vmo_unpin(ram, cap, id), OK);
    dma_quarantine_stats(d, &q0);
    khandle_release(&kh);
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, q0.pins);
    dma_quarantine_flush(d);   /* (with the IOMMU: its domain gone, nothing to free) */
    dma_quarantine_stats(d, &q);
    KT_EQ(q.freed, q0.freed);
    KT_EQ(q.released, q0.released);
    kobject_unref(cap);
    kobject_unref(vmo_kobject(phys));
    kobject_unref(vmo_kobject(ram));
}

/* ---- the stats while a batch is being released ------------------------------ */

static struct pci_dev *mid_dev;
static struct dma_quarantine_stats mid_seen;
static volatile int mid_calls;

/* DBG_DMA_RELEASED: the batch's pages are back with their VMO; what does
 * a reader see now? (Other functions' releases by the reaper pass by.) */
static void mid_release_hook(void *arg)
{
    if (arg != mid_dev)
        return;
    dma_quarantine_stats(mid_dev, &mid_seen);
    mid_calls++;
}

KTEST(dma_quarantine_stats_consistent)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    dma_quarantine_flush(d);
    struct dma_quarantine_stats q0, q1, q2;
    dma_quarantine_stats(d, &q0);
    struct vmo *v;
    KT_EQ(vmo_create(2 * PG, VMO_CONTIGUOUS | VMO_DMA32, &v), OK);
    struct kobject *cap;
    struct khandle kh = new_cap(d, &cap);
    bool tr = dma_cap_translated(cap);
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t pa[2], id;
    KT_EQ(vmo_pin(v, cap, 0, 2 * PG, pa, 2, &id), OK);
    /* With the IOMMU the batch is due at once and the release thread may
     * take it before this test does, so the hook goes in before the close
     * (and q1, "held, not yet released", is what it must be then). */
    mid_dev = d;
    mid_calls = 0;
    __atomic_store_n(&dbg_hooks[DBG_DMA_RELEASED], mid_release_hook, __ATOMIC_RELEASE);
    khandle_release(&kh);   /* closed with the pin held: one batch of 2 pages */
    kobject_unref(cap);
    q1 = q0;
    q1.pins++;
    q1.pages += 2;
    if (!tr) {
        struct dma_quarantine_stats now;
        dma_quarantine_stats(d, &now);
        KT_EQ(now.pins, q1.pins);
        KT_EQ(now.pages, q1.pages);
    }
    dma_quarantine_flush(d);
    __atomic_store_n(&dbg_hooks[DBG_DMA_RELEASED], NULL, __ATOMIC_RELEASE);
    dma_quarantine_stats(d, &q2);
    kobject_unref(vmo_kobject(v));
    kprintf("ktest %s: mid-release: %lu pin(s), %lu held + %lu released + %lu freed page(s); "
            "before: %lu + %lu + %lu\n", ktest_current, mid_seen.pins, mid_seen.pages,
            mid_seen.released, mid_seen.freed, q1.pages, q1.released, q1.freed);
    KT_EQ(mid_calls, 1);
    /* Held pages move to `released` (or `freed`) in one step: the sum
     * never dips. */
    KT_EQ(mid_seen.pages + mid_seen.released + mid_seen.freed,
          q1.pages + q1.released + q1.freed);
    KT_EQ(mid_seen.pins, q1.pins);   /* still counted: not all released yet */
    KT_EQ(q2.pins, q0.pins);
    KT_EQ(q2.pages, q0.pages);
    KT_EQ(q2.released, q1.released + (tr ? 0 : 2));
    KT_EQ(q2.freed, q1.freed + (tr ? 2 : 0));
    KT_EQ(q2.changed, q1.changed);
}

/* ---- pins are charged --------------------------------------------------------- */

KTEST(m6r_pins_are_charged)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    struct job *j = kt_fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = j;
    handle_t dev, cap, vh;
    struct khandle kh = khandle_from_new(kt_pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &dev), OK);
    KT_EQ(sys_dma_cap_create(&t, dev, kt_job_handle(&t, j), &cap), OK);
    KT_EQ(sys_vmo_create(&t, PG, 0, &vh), OK);
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    uint64_t used = job_used(j, JOB_LIMIT_HANDLES);
    KT_EQ(job_set_limit(j, JOB_LIMIT_HANDLES, used + 64), OK);
    /* The same page, again and again: each pin is a kmalloc'd range. */
    unsigned ok = 0;
    for (unsigned i = 0; i < 1000; i++) {
        uint64_t pa, id;
        if (sys_vmo_pin(&t, vh, cap, 0, PG, &pa, &id) != OK)
            break;
        ok++;
    }
    kprintf("ktest %s: %u pins with 64 handle units to spare\n", ktest_current, ok);
    KT_ASSERT(ok <= 64);
    struct kobject *capobj;
    KT_EQ(handle_get(&t, cap, OBJ_DMA_CAP, 0, &capobj, NULL), OK);
    bool tr = dma_cap_translated(capobj);
    kobject_unref(capobj);
    struct dma_quarantine_stats q0, q;
    dma_quarantine_stats(d, &q0);
    handle_table_destroy(&t);   /* closes the cap: BME off, every pin to the release thread */
    KT_ASSERT(!(pci_cfg_read(d, 0x04, 2) & 0x04));
    dma_quarantine_stats(d, &q);
    if (!tr)   /* (with the IOMMU the release thread may be done already) */
        KT_EQ(q.pins, q0.pins + ok);
    dma_quarantine_flush(d);   /* ... still charged until released (or freed) */
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, q0.pins);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- the cap's charges go to the driver's job ----------------------------------- */

KTEST(dma_cap_charges_driver_job)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    /* devmgr's job and its driver's. Siblings here, so neither's count
     * includes the other's (devmgr's drivers are its children: there its
     * own count includes theirs, and their own limits bound them). */
    struct job *mgr = kt_fresh_job(), *drv = kt_fresh_job();
    struct handle_table t;   /* devmgr's */
    handle_table_init(&t);
    t.job = mgr;
    handle_t dev, jh, jown, cap, vh;
    struct khandle kh = khandle_from_new(kt_pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &dev), OK);
    jh = kt_job_handle(&t, drv);
    KT_EQ(handle_duplicate(&t, jh, JOB_RIGHTS_OWN, &jown), OK);
    KT_EQ(sys_vmo_create(&t, PG, 0, &vh), OK);   /* devmgr's buffer, for the pin below */
    KT_EQ(sys_vmo_commit(&t, vh, 0, PG), OK);    /* paid for now, its tables too */
    /* Only a job the caller manages pays: spending a job's limits is
     * managing it. */
    KT_EQ(sys_dma_cap_create(&t, dev, jown, &cap), ERR_ACCESS_DENIED);
    KT_EQ(sys_dma_cap_create(&t, dev, dev, &cap), ERR_WRONG_TYPE);
    KT_EQ(sys_dma_cap_create(&t, dev, HANDLE_INVALID, &cap), ERR_BAD_HANDLE);

    uint64_t mp0 = job_used(mgr, JOB_LIMIT_PAGES), mh0 = job_used(mgr, JOB_LIMIT_HANDLES);
    KT_EQ(sys_dma_cap_create(&t, dev, jh, &cap), OK);
    struct kobject *capobj;
    KT_EQ(handle_get(&t, cap, OBJ_DMA_CAP, 0, &capobj, NULL), OK);
    bool tr = dma_cap_translated(capobj);
    kobject_unref(capobj);
    uint64_t dp1 = job_used(drv, JOB_LIMIT_PAGES), dh1 = job_used(drv, JOB_LIMIT_HANDLES);
    uint64_t mp1 = job_used(mgr, JOB_LIMIT_PAGES), mh1 = job_used(mgr, JOB_LIMIT_HANDLES);
    /* A pin: its page is the VMO's (devmgr's, paid for already), its new
     * table pages the domain's (the driver's). */
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    uint64_t pa, id;
    KT_EQ(sys_vmo_pin(&t, vh, cap, 0, PG, &pa, &id), OK);
    uint64_t dp2 = job_used(drv, JOB_LIMIT_PAGES), mp2 = job_used(mgr, JOB_LIMIT_PAGES);
    KT_EQ(sys_vmo_unpin(&t, vh, cap, id), OK);
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 0), OK);
    KT_EQ(handle_close(&t, cap), OK);
    dma_quarantine_flush(d);   /* its domain gone (a clean close: nothing quarantined) */
    uint64_t dp3 = job_used(drv, JOB_LIMIT_PAGES), dh3 = job_used(drv, JOB_LIMIT_HANDLES);
    kprintf("ktest %s: %s; the driver's job: %lu page(s) and %lu handle unit(s) for the cap, "
            "%lu page(s) after a pin, %lu and %lu once it closed; the caller's: %lu page(s) "
            "and %lu handle unit(s) more for the cap\n", ktest_current,
            tr ? "translated" : "not translated", dp1, dh1, dp2, dp3, dh3, mp1 - mp0,
            mh1 - mh0);
    handle_table_destroy(&t);
    dma_quarantine_flush(d);
    kt_job_is_empty(drv);
    kt_job_is_empty(mgr);
    job_unref(drv);
    job_unref(mgr);

    KT_EQ(dh1, 1);                  /* the cap itself */
    KT_EQ(mh1, mh0 + 1);            /* the caller: its handle's slot only */
    KT_EQ(mp1, mp0);
    KT_EQ(mp2, mp1);                /* the pinned page was the VMO's already */
    if (tr) {
        KT_ASSERT(dp1 >= 1);        /* the domain's top table */
        KT_ASSERT(dp2 > dp1);       /* and the tables down to the pinned page */
    } else {
        KT_EQ(dp1, 0);              /* no domain, no tables */
        KT_EQ(dp2, 0);
    }
    KT_EQ(dp3, 0);
    KT_EQ(dh3, 0);
}

/* ---- the pinner pays for its pins ------------------------------------------------- */

KTEST(dma_pin_charges_pinner)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    dma_quarantine_flush(d);
    /* The client made and filled the buffer; the driver holds a handle to
     * it ("sent" to it), its device and its cap. */
    struct job *cli = kt_fresh_job(), *drv = kt_fresh_job();
    struct vmo *v;
    KT_EQ(vmo_create_for(cli, 2 * PG, 0, &v), OK);
    KT_EQ(vmo_commit(v, 0, 2 * PG), OK);
    struct handle_table t;   /* the driver's */
    handle_table_init(&t);
    t.job = drv;
    handle_t dev, cap, vh;
    struct khandle kh = khandle_from_new(kt_pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &dev), OK);
    kobject_ref(vmo_kobject(v));
    struct khandle kv = khandle_from_new(vmo_kobject(v), RIGHTS_BASIC | RIGHTS_IO | RIGHT_MAP);
    KT_EQ(handle_insert(&t, &kv, &vh), OK);
    handle_t jh = kt_job_handle(&t, drv);
    uint64_t dh0 = job_used(drv, JOB_LIMIT_HANDLES);
    KT_EQ(sys_dma_cap_create(&t, dev, jh, &cap), OK);
    struct kobject *capobj;
    KT_EQ(handle_get(&t, cap, OBJ_DMA_CAP, 0, &capobj, NULL), OK);
    bool tr = dma_cap_translated(capobj);
    kobject_unref(capobj);
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    uint64_t ch0 = job_used(cli, JOB_LIMIT_HANDLES), cp0 = job_used(cli, JOB_LIMIT_PAGES);
    uint64_t dh1 = job_used(drv, JOB_LIMIT_HANDLES);   /* + the cap and its handle */

    /* The client can't pay for one more handle unit: it isn't asked to. */
    struct job_info ci, di;
    job_get_info(cli, &ci);
    job_get_info(drv, &di);
    KT_EQ(job_set_limit(cli, JOB_LIMIT_HANDLES, ch0), OK);
    uint64_t pa[2], id[3];
    status_t p0 = sys_vmo_pin(&t, vh, cap, 0, PG, &pa[0], &id[0]);
    status_t p1 = sys_vmo_pin(&t, vh, cap, 0, PG, &pa[0], &id[1]);
    status_t p2 = sys_vmo_pin_flags(&t, vh, cap, PG, PG, VMO_PIN_READ_ONLY, &pa[1], &id[2]);
    uint64_t dh2 = job_used(drv, JOB_LIMIT_HANDLES), ch2 = job_used(cli, JOB_LIMIT_HANDLES);
    /* The driver can't: refused, nothing charged anywhere. */
    KT_EQ(job_set_limit(drv, JOB_LIMIT_HANDLES, dh2), OK);
    uint64_t spare;
    status_t p3 = sys_vmo_pin(&t, vh, cap, 0, PG, &pa[0], &spare);
    uint64_t dh3 = job_used(drv, JOB_LIMIT_HANDLES), ch3 = job_used(cli, JOB_LIMIT_HANDLES);
    KT_EQ(job_set_limit(drv, JOB_LIMIT_HANDLES, di.limit[JOB_LIMIT_HANDLES]), OK);
    KT_EQ(job_set_limit(cli, JOB_LIMIT_HANDLES, ci.limit[JOB_LIMIT_HANDLES]), OK);
    /* An unpin gives its unit back to the driver. */
    status_t u1 = p1 == OK ? sys_vmo_unpin(&t, vh, cap, id[1]) : p1;
    uint64_t dh4 = job_used(drv, JOB_LIMIT_HANDLES);
    /* The cap closes with two pins held (a killed driver): with the IOMMU
     * they go with its domain, without it they wait in the quarantine,
     * still the driver's, until it releases them. */
    KT_EQ(handle_close(&t, cap), OK);
    uint64_t dh5 = job_used(drv, JOB_LIMIT_HANDLES);
    dma_quarantine_flush(d);
    uint64_t dh6 = job_used(drv, JOB_LIMIT_HANDLES), ch6 = job_used(cli, JOB_LIMIT_HANDLES);
    uint64_t cp6 = job_used(cli, JOB_LIMIT_PAGES);

    /* A cap with no job (the kernel's own) charges nobody: not the VMO's
     * job either. */
    struct kobject *kcap;
    KT_EQ(dma_cap_create_for(d, NULL, &kcap), OK);
    KT_EQ(dma_cap_bus_master(kcap, true), OK);
    uint64_t kid;
    status_t pk = vmo_pin(v, kcap, 0, PG, &pa[0], 1, &kid);
    uint64_t chk = job_used(cli, JOB_LIMIT_HANDLES);
    if (pk == OK)
        KT_EQ(vmo_unpin(v, kcap, kid), OK);
    KT_EQ(dma_cap_bus_master(kcap, false), OK);
    kobject_unref(kcap);
    dma_quarantine_flush(d);

    handle_table_destroy(&t);
    kobject_unref(vmo_kobject(v));
    kprintf("ktest %s: %s: 3 pins: %s %s %s, the driver's job +%lu handle unit(s), the "
            "client's +%lu; one past the driver's limit: %s; unpin: %s, the driver's job then "
            "+%lu, after the close +%lu, after the release +%lu (the client's +%lu); a "
            "jobless cap's pin: %s, the client's +%lu\n", ktest_current,
            tr ? "translated" : "not translated", status_str(p0), status_str(p1),
            status_str(p2), dh2 - dh1, ch2 - ch0, status_str(p3), status_str(u1), dh4 - dh1,
            dh5 - dh0, dh6 - dh0, ch6 - ch0, status_str(pk), chk - ch0);
    kt_job_is_empty(drv);
    kt_job_is_empty(cli);
    job_unref(drv);
    job_unref(cli);

    KT_EQ(p0, OK);
    KT_EQ(p1, OK);
    KT_EQ(p2, OK);
    KT_EQ(dh2, dh1 + 3);   /* one unit a pin, the driver's */
    KT_EQ(ch2, ch0);       /* none the client's */
    KT_EQ(p3, ERR_NO_RESOURCES);
    KT_EQ(dh3, dh2);
    KT_EQ(ch3, ch0);
    KT_EQ(u1, OK);
    KT_EQ(dh4, dh1 + 2);
    if (!tr)                     /* the cap's own unit and its handle gone, its pins held */
        KT_EQ(dh5, dh0 + 2);
    KT_EQ(dh6, dh0);             /* every unit back */
    KT_EQ(ch6, ch0);
    KT_EQ(cp6, cp0);             /* the pages were the client's already, and stay */
    KT_EQ(pk, OK);
    KT_EQ(chk, ch0);
}

/* ---- read-only pins -------------------------------------------------------------- */

KTEST(dma_read_only_pin_rules)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    struct job *j = kt_fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = j;
    handle_t dev, cap, vh, vro;
    struct khandle kh = khandle_from_new(kt_pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &dev), OK);
    KT_EQ(sys_dma_cap_create(&t, dev, kt_job_handle(&t, j), &cap), OK);
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    KT_EQ(sys_vmo_create(&t, PG, 0, &vh), OK);
    KT_EQ(sys_vmo_commit(&t, vh, 0, PG), OK);   /* a read-only pin takes only committed pages */
    KT_EQ(handle_duplicate(&t, vh, RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP, &vro), OK);
    struct kobject *capobj;
    KT_EQ(handle_get(&t, cap, OBJ_DMA_CAP, 0, &capobj, NULL), OK);
    bool tr = dma_cap_translated(capobj);

    uint64_t pa, id, id2;
    KT_EQ(sys_vmo_pin_flags(&t, vh, cap, 0, PG, 2, &pa, &id), ERR_INVALID_ARGS);
    KT_EQ(sys_vmo_pin(&t, vro, cap, 0, PG, &pa, &id), ERR_ACCESS_DENIED);
    status_t ro_read = sys_vmo_pin_flags(&t, vro, cap, 0, PG, VMO_PIN_READ_ONLY, &pa, &id);
    if (ro_read == OK)
        KT_EQ(sys_vmo_unpin(&t, vro, cap, id), OK);
    /* With RIGHT_WRITE a read-only pin works either way, and RIGHT_READ
     * undoes it. */
    KT_EQ(sys_vmo_pin_flags(&t, vh, cap, 0, PG, VMO_PIN_READ_ONLY, &pa, &id2), OK);
    KT_EQ(sys_vmo_unpin(&t, vro, cap, id2), OK);

    /* A sealed VMO: nothing may change its bytes, a device included. */
    struct vmo *sv;
    KT_EQ(vmo_create(PG, 0, &sv), OK);
    KT_EQ(vmo_commit(sv, 0, PG), OK);
    KT_EQ(vmo_seal(sv), OK);
    status_t sealed_rw = vmo_pin(sv, capobj, 0, PG, &pa, 1, &id);
    status_t sealed_ro = vmo_pin_flags(sv, capobj, 0, PG, VMO_PIN_READ_ONLY, &pa, 1, &id);
    if (sealed_ro == OK)
        KT_EQ(vmo_unpin(sv, capobj, id), OK);
    kobject_unref(vmo_kobject(sv));

    KT_EQ(sys_dma_cap_bus_master(&t, cap, 0), OK);
    kobject_unref(capobj);
    handle_table_destroy(&t);
    dma_quarantine_flush(d);
    kt_job_is_empty(j);
    job_unref(j);
    kprintf("ktest %s: %s: a read-only pin with RIGHT_READ: %s; of a sealed VMO: %s (a "
            "writable one: %s)\n", ktest_current, tr ? "translated" : "not translated",
            status_str(ro_read), status_str(sealed_ro), status_str(sealed_rw));
    KT_EQ(ro_read, tr ? OK : ERR_ACCESS_DENIED);
    KT_EQ(sealed_ro, tr ? OK : ERR_BAD_STATE);
    KT_EQ(sealed_rw, ERR_BAD_STATE);
}

/* A client's sparse buffer, pages 0 and 2 of 4 committed, handed to a
 * driver (its read-only view, and a writable one). */
KTEST(dma_read_only_pin_needs_committed)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    dma_quarantine_flush(d);
    struct job *cli = kt_fresh_job(), *drv = kt_fresh_job();
    struct vmo *v;
    KT_EQ(vmo_create_for(cli, 4 * PG, 0, &v), OK);
    KT_EQ(vmo_commit(v, 0, PG), OK);
    KT_EQ(vmo_commit(v, 2 * PG, PG), OK);
    struct handle_table t;   /* the driver's */
    handle_table_init(&t);
    t.job = drv;
    handle_t dev, cap, vh, vro;
    struct khandle kh = khandle_from_new(kt_pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &dev), OK);
    kobject_ref(vmo_kobject(v));
    struct khandle kv = khandle_from_new(vmo_kobject(v), RIGHTS_BASIC | RIGHTS_IO | RIGHT_MAP);
    KT_EQ(handle_insert(&t, &kv, &vh), OK);
    KT_EQ(handle_duplicate(&t, vh, RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP, &vro), OK);
    KT_EQ(sys_dma_cap_create(&t, dev, kt_job_handle(&t, drv), &cap), OK);
    struct kobject *capobj;
    KT_EQ(handle_get(&t, cap, OBJ_DMA_CAP, 0, &capobj, NULL), OK);
    bool tr = dma_cap_translated(capobj);
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    uint64_t c0 = vmo_committed(v) / PG;
    uint64_t cp0 = job_used(cli, JOB_LIMIT_PAGES), ch0 = job_used(cli, JOB_LIMIT_HANDLES);
    uint64_t dp0 = job_used(drv, JOB_LIMIT_PAGES), dh0 = job_used(drv, JOB_LIMIT_HANDLES);

    /* Over the holes: refused, whichever handle, nothing committed or
     * charged anywhere, no pin left. */
    uint64_t pa[4], id[4];
    status_t all = sys_vmo_pin_flags(&t, vh, cap, 0, 4 * PG, VMO_PIN_READ_ONLY, pa, &id[0]);
    status_t hole = sys_vmo_pin_flags(&t, vh, cap, PG, PG, VMO_PIN_READ_ONLY, pa, &id[0]);
    status_t tail = sys_vmo_pin_flags(&t, vh, cap, 2 * PG, 2 * PG, VMO_PIN_READ_ONLY, pa, &id[0]);
    status_t viaro = sys_vmo_pin_flags(&t, vro, cap, 0, 2 * PG, VMO_PIN_READ_ONLY, pa, &id[0]);
    uint64_t c1 = vmo_committed(v) / PG, pins1 = dma_cap_pin_count(capobj);
    bool holes = !vmo_page_phys(v, PG) && !vmo_page_phys(v, 3 * PG);
    bool same = job_used(cli, JOB_LIMIT_PAGES) == cp0 && job_used(cli, JOB_LIMIT_HANDLES) == ch0 &&
                job_used(drv, JOB_LIMIT_PAGES) == dp0 && job_used(drv, JOB_LIMIT_HANDLES) == dh0;

    /* Over committed pages: as before (with only RIGHT_READ where the
     * IOMMU translates), each at its page's own address. */
    status_t p0 = sys_vmo_pin_flags(&t, tr ? vro : vh, cap, 0, PG, VMO_PIN_READ_ONLY, &pa[0],
                                    &id[0]);
    status_t p2 = sys_vmo_pin_flags(&t, vh, cap, 2 * PG, PG, VMO_PIN_READ_ONLY, &pa[2], &id[2]);
    bool at = p0 == OK && p2 == OK && pa[0] == vmo_page_phys(v, 0) &&
              pa[2] == vmo_page_phys(v, 2 * PG);
    uint64_t pins2 = dma_cap_pin_count(capobj), c2 = vmo_committed(v) / PG;
    if (p0 == OK)
        KT_EQ(sys_vmo_unpin(&t, vro, cap, id[0]), OK);
    if (p2 == OK)
        KT_EQ(sys_vmo_unpin(&t, vro, cap, id[2]), OK);
    /* A writable pin still commits what it covers; then the read-only pin
     * of the whole buffer works. */
    status_t w = sys_vmo_pin(&t, vh, cap, 0, 4 * PG, pa, &id[1]);
    uint64_t c3 = vmo_committed(v) / PG;
    if (w == OK)
        KT_EQ(sys_vmo_unpin(&t, vh, cap, id[1]), OK);
    status_t after = sys_vmo_pin_flags(&t, vh, cap, 0, 4 * PG, VMO_PIN_READ_ONLY, pa, &id[3]);
    if (after == OK)
        KT_EQ(sys_vmo_unpin(&t, vh, cap, id[3]), OK);

    KT_EQ(sys_dma_cap_bus_master(&t, cap, 0), OK);
    kobject_unref(capobj);
    handle_table_destroy(&t);
    dma_quarantine_flush(d);
    kobject_unref(vmo_kobject(v));
    kt_job_is_empty(drv);
    kt_job_is_empty(cli);
    job_unref(drv);
    job_unref(cli);
    kprintf("ktest %s: %s: read-only pins over holes: %s %s %s, with RIGHT_READ %s; %lu page(s) "
            "committed before, %lu after, %lu pin(s) left, jobs %s; over committed pages: %s %s "
            "(%s); a writable pin: %s, %lu page(s) committed; the read-only pin then: %s\n",
            ktest_current, tr ? "translated" : "not translated", status_str(all),
            status_str(hole), status_str(tail), status_str(viaro), c0, c1, pins1,
            same ? "unchanged" : "CHANGED", status_str(p0), status_str(p2),
            at ? "at the pages' addresses" : "WRONG addresses", status_str(w), c3,
            status_str(after));
    KT_EQ(c0, 2);
    KT_EQ(all, ERR_BAD_STATE);
    KT_EQ(hole, ERR_BAD_STATE);
    KT_EQ(tail, ERR_BAD_STATE);
    KT_EQ(viaro, tr ? ERR_BAD_STATE : ERR_ACCESS_DENIED);   /* (the rights come first) */
    KT_EQ(c1, c0);
    KT_ASSERT(holes);
    KT_EQ(pins1, 0);
    KT_ASSERT(same);
    KT_EQ(p0, OK);
    KT_EQ(p2, OK);
    KT_ASSERT(at);
    KT_EQ(pins2, 2);
    KT_EQ(c2, c0);
    KT_EQ(w, OK);
    KT_EQ(c3, 4);
    KT_EQ(after, OK);
}

/* ---- unpin needs nothing but a writable VMO handle ------------------------------- */

/* A client hands its buffer VMO (RIGHT_WRITE, as a block or network client
 * would) to a driver, which pins it for DMA. The client, not the driver,
 * then unpins the driver's pin (ids count up from 1 per VMO) and
 * decommits the page: it goes back to the page allocator while the device
 * still has it as a DMA target and Bus Master Enable is on. */
KTEST(m6r_unpin_by_other_holder)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    struct job *j = kt_fresh_job();
    struct handle_table td, tc;
    handle_table_init(&td);
    handle_table_init(&tc);
    td.job = tc.job = j;
    handle_t dev, cap, vc, vd;
    struct khandle kh = khandle_from_new(kt_pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&td, &kh, &dev), OK);
    KT_EQ(sys_dma_cap_create(&td, dev, kt_job_handle(&td, j), &cap), OK);
    KT_EQ(sys_dma_cap_bus_master(&td, cap, 1), OK);
    KT_EQ(sys_vmo_create(&tc, PG, 0, &vc), OK);
    struct kobject *vo;
    KT_EQ(handle_get(&tc, vc, OBJ_VMO, 0, &vo, NULL), OK);
    struct khandle kv = khandle_from_new(vo, RIGHTS_BASIC | RIGHTS_IO | RIGHT_MAP);
    KT_EQ(handle_insert(&td, &kv, &vd), OK);   /* "sent" to the driver */
    uint64_t pa, id;
    KT_EQ(sys_vmo_pin(&td, vd, cap, 0, PG, &pa, &id), OK);
    /* The client, with a guessed id: without a cap, and with a cap of its
     * own (bound to the same function, so it's a valid one). */
    handle_t ccap, dev2;
    struct khandle kd = khandle_from_new(kt_pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&tc, &kd, &dev2), OK);
    KT_EQ(sys_dma_cap_create(&tc, dev2, kt_job_handle(&tc, j), &ccap), OK);
    status_t un0 = sys_vmo_unpin(&tc, vc, HANDLE_INVALID, id);
    status_t un = sys_vmo_unpin(&tc, vc, ccap, id);
    status_t dc = sys_vmo_decommit(&tc, vc, 0, PG);
    kprintf("ktest %s: client unpin of the driver's pin %lu: %s / %s, then decommit: %s "
            "(device still has 0x%lx, BME %s)\n", ktest_current, id, status_str(un0),
            status_str(un), status_str(dc), pa, (pci_cfg_read(d, 0x04, 2) & 0x04) ? "on" : "off");
    handle_table_destroy(&tc);
    handle_table_destroy(&td);   /* the driver's cap: its pin is quarantined */
    dma_quarantine_flush(d);
    kt_job_is_empty(j);
    job_unref(j);
    /* Only the pin's DMA capability may undo it (vmo_unpin takes the
     * dma_cap), and the page stays pinned. */
    KT_EQ(un0, ERR_BAD_HANDLE);
    KT_EQ(un, ERR_ACCESS_DENIED);
    KT_EQ(dc, ERR_BAD_STATE);
}
