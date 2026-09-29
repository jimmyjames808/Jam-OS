/* M7 Track D: safe rebind (kernel/object/dma_cap.c). QEMU only: they need
 * the edu device; each skips itself without it.
 *
 *   dma_stale_write_after_rebind
 *       The M6 phase 2 review's CONFIRMED finding 1, as a normal test: a
 *       driver dies while its device has a device -> RAM transfer queued
 *       (QEMU's edu starts it 100 ms after the command). Three rounds:
 *       nobody rebinds (control); the next driver quiesces the device (its
 *       DMA engine idle) before it turns bus mastering on, as drivers must;
 *       a careless next driver turns it on at once. The first two must
 *       write 0 bytes; the third's stale write lands, but in pages the
 *       quarantine still holds (never in released memory), and the
 *       quarantine's release sees the changed page.
 *   dma_cap_owner_rules
 *       Only the function's current (newest) cap turns bus mastering on or
 *       pins; an older cap's close leaves Bus Master Enable alone (review
 *       finding 2); a new cap starts with it off.
 *   dma_quarantine_phys_and_clean_close
 *       A clean close (nothing pinned) quarantines nothing; pins of a
 *       physical VMO (no RAM) are released at once. */
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/vmo.h>

#define PG      PAGE_SIZE
#define S       1000000000ull
#define CMD_BME 0x04

#define EDU_DMA_SRC 0x80
#define EDU_DMA_DST 0x88
#define EDU_DMA_CNT 0x90
#define EDU_DMA_CMD 0x98
#define EDU_BUF     0x40000u
#define DMA_RUN     0x1u
#define DMA_TO_RAM  0x2u

static struct pci_dev *edu(void)
{
    struct pci_dev *d = pci_find(0x1234, 0x11e8, 0);
    if (!d)
        kprintf("ktest %s: no edu, skipped\n", ktest_current);
    return d;
}

static bool bme(struct pci_dev *d)
{
    return pci_cfg_read(d, 0x04, 2) & CMD_BME;
}

static bool edu_wait_idle(volatile uint8_t *r)
{
    uint64_t end = uptime_ns() + 2 * S;
    while (*(volatile uint64_t *)(r + EDU_DMA_CMD) & DMA_RUN) {
        if (uptime_ns() > end)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

static void edu_dma(volatile uint8_t *r, uint64_t src, uint64_t dst, uint32_t len, uint32_t dir)
{
    *(volatile uint64_t *)(r + EDU_DMA_SRC) = src;
    *(volatile uint64_t *)(r + EDU_DMA_DST) = dst;
    *(volatile uint64_t *)(r + EDU_DMA_CNT) = len;
    *(volatile uint64_t *)(r + EDU_DMA_CMD) = DMA_RUN | dir;
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
    struct pci_dev *d = edu();
    if (!d)
        return;
    uint64_t cf = pci_cmd_lock();
    KT_EQ(pci_enable_memory(d), OK);
    pci_cmd_unlock(cf);
    volatile uint8_t *r = vmm_map_mmio(d->info.bar[0].phys, PG);
    KT_ASSERT(edu_wait_idle(r));
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
    edu_dma(r, pa[0], EDU_BUF, PG, 0);
    KT_ASSERT(edu_wait_idle(r));
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
        edu_dma(r, EDU_BUF, pa[1], PG, DMA_TO_RAM);
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
                KT_ASSERT(edu_wait_idle(r));   /* quiesce first, as a driver must */
            KT_EQ(dma_cap_bus_master(cap2, true), OK);
        }
        idle[round] = edu_wait_idle(r);
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
            uint64_t until = uptime_ns() + DMA_QUARANTINE_GRACE_NS + 5 * S;
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
    struct pci_dev *d = edu();
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

KTEST(dma_quarantine_phys_and_clean_close)
{
    struct pci_dev *d = edu();
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
