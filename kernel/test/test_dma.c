/* DMA capabilities and safe rebind (kernel/object/dma_cap.c). QEMU only:
 * they need the edu device; each skips itself without it.
 *
 *   dma_stale_write_after_rebind
 *       A driver dies while its device has a device -> RAM transfer queued
 *       (QEMU's edu starts it 100 ms after the command). Three rounds:
 *       nobody rebinds (control); the next driver quiesces the device (its
 *       DMA engine idle) before it turns bus mastering on, as drivers must;
 *       a careless next driver turns it on at once. The first two must
 *       write 0 bytes; the third's stale write lands, but in pages the
 *       quarantine still holds (never in released memory), and the
 *       quarantine's release sees the changed page.
 *   dma_cap_owner_rules
 *       Only the function's current (newest) cap turns bus mastering on or
 *       pins; an older cap's close leaves Bus Master Enable alone; a new cap
 *       starts with it off.
 *   dma_quarantine_phys_and_clean_close
 *       A clean close (nothing pinned) quarantines nothing; pins of a
 *       physical VMO (no RAM) are released at once.
 *   dma_quarantine_stats_consistent
 *       A reader of the stats in the middle of a release (forced with the
 *       DBG_DMA_RELEASED hook) sees the batch either still held or already
 *       released, never gone from `pages` but not yet in `released`.
 *   m6r_pins_are_charged
 *       Every pin is a kernel allocation, so it is charged: otherwise a
 *       driver pins one page over and over and fills the kernel heap.
 *   m6r_unpin_by_other_holder
 *       Only the pin's own DMA capability may unpin it, not another holder
 *       of the VMO, and the pinned page can't be decommitted. */
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
    KT_EQ(dma_cap_create_for(d, cap), OK);
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
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t pa[2], id;
    KT_EQ(vmo_pin(v, cap, 0, 2 * PG, pa, 2, &id), OK);
    KT_ASSERT(kt_edu_dma(r, pa[0], 0, PG, false));
    KT_EQ(vmo_unpin(v, cap, id), OK);
    khandle_release(&kh);
    kobject_unref(cap);
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, q0.pins);

    static const char *const what[3] = { "no rebind (control)", "rebind, driver quiesced first",
                                         "rebind, bus mastering on at once" };
    uint32_t hit[3];
    bool idle[3], held[3];
    uint64_t changed[3];
    for (int round = 0; round < 3; round++) {
        /* A driver (one dma_cap handle, bus mastering on) with a transfer
         * to its page 1 queued when it dies: BME off, pin quarantined. */
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
        KT_EQ(q.pins, q0.pins + 1);
        KT_EQ(q.pages, q0.pages + 2);

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
        if (round == 1) {
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
        q0 = after;
        memset(buf, 0, sizeof(buf));
        KT_EQ(vmo_write(v, PG, buf, PG), OK);
    }
    for (int round = 0; round < 3; round++)
        kprintf("ktest %s: %s: the stale transfer wrote %u bytes (of %u), %s; quarantine saw "
                "%lu changed page(s)\n", ktest_current, what[round], hit[round],
                (unsigned)PG - 16, held[round] ? "the pages still quarantined" : "RELEASED",
                changed[round]);
    kobject_unref(vmo_kobject(v));
    KT_ASSERT(idle[0] && idle[1] && idle[2]);
    KT_ASSERT(held[0] && held[1] && held[2]);
    KT_EQ(hit[0], 0);          /* BME off stops it */
    KT_EQ(hit[1], 0);          /* ... and a driver that quiesces first keeps it stopped */
    KT_EQ(changed[0], 0);
    KT_EQ(changed[1], 0);
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
    khandle_release(&kh);
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, q0.pins + 1);   /* the RAM pin only */
    KT_EQ(q.pages, q0.pages + 1);
    KT_EQ(__atomic_load_n(&vmo_kobject(phys)->refs, __ATOMIC_RELAXED), prefs - 1);   /* released */
    KT_EQ(vmo_decommit(ram, 0, PG), ERR_BAD_STATE);   /* still held */
    dma_quarantine_flush(d);
    KT_EQ(vmo_decommit(ram, 0, PG), OK);
    kobject_unref(cap);

    /* A clean close: everything unpinned first. */
    kh = new_cap(d, &cap);
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    KT_EQ(vmo_pin(ram, cap, 0, PG, &pa, 1, &id), OK);
    KT_EQ(vmo_unpin(ram, cap, id), OK);
    khandle_release(&kh);
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, q0.pins);
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
    KT_EQ(dma_cap_bus_master(cap, true), OK);
    uint64_t pa[2], id;
    KT_EQ(vmo_pin(v, cap, 0, 2 * PG, pa, 2, &id), OK);
    khandle_release(&kh);   /* closed with the pin held: one batch of 2 pages */
    kobject_unref(cap);
    dma_quarantine_stats(d, &q1);
    KT_EQ(q1.pins, q0.pins + 1);
    KT_EQ(q1.pages, q0.pages + 2);

    mid_dev = d;
    mid_calls = 0;
    __atomic_store_n(&dbg_hooks[DBG_DMA_RELEASED], mid_release_hook, __ATOMIC_RELEASE);
    dma_quarantine_flush(d);
    __atomic_store_n(&dbg_hooks[DBG_DMA_RELEASED], NULL, __ATOMIC_RELEASE);
    dma_quarantine_stats(d, &q2);
    kobject_unref(vmo_kobject(v));
    kprintf("ktest %s: mid-release: %lu pin(s), %lu held + %lu released page(s); before: %lu + "
            "%lu\n", ktest_current, mid_seen.pins, mid_seen.pages, mid_seen.released, q1.pages,
            q1.released);
    KT_EQ(mid_calls, 1);
    /* Held pages move to `released` in one step: their sum never dips. */
    KT_EQ(mid_seen.pages + mid_seen.released, q1.pages + q1.released);
    KT_EQ(mid_seen.pins, q1.pins);   /* still counted: not all released yet */
    KT_EQ(q2.pins, q0.pins);
    KT_EQ(q2.pages, q0.pages);
    KT_EQ(q2.released, q1.released + 2);
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
    KT_EQ(sys_dma_cap_create(&t, dev, &cap), OK);
    KT_EQ(sys_vmo_create(&t, PG, 0, HANDLE_INVALID, &vh), OK);
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
    handle_table_destroy(&t);   /* closes the cap: BME off, every pin quarantined */
    KT_ASSERT(!(pci_cfg_read(d, 0x04, 2) & 0x04));
    struct dma_quarantine_stats q;
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, ok);
    dma_quarantine_flush(d);   /* ... still charged until released */
    kt_job_is_empty(j);
    job_unref(j);
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
    KT_EQ(sys_dma_cap_create(&td, dev, &cap), OK);
    KT_EQ(sys_dma_cap_bus_master(&td, cap, 1), OK);
    KT_EQ(sys_vmo_create(&tc, PG, 0, HANDLE_INVALID, &vc), OK);
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
    KT_EQ(sys_dma_cap_create(&tc, dev2, &ccap), OK);
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
