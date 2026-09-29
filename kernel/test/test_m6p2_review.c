/* Repros from the independent review of M6 phase 2 (QEMU only: they need
 * the edu device, and qemu-xhci for the second). review_* tests are
 * skipped by the default `ktest` run until their bug is fixed; run them
 * with "ktest=review_m6p2".
 *
 *   review_m6p2_stale_dma_after_rebind
 *       A driver dies while its device has a device -> RAM transfer queued.
 *       The dma_cap's close turns Bus Master Enable off and releases the
 *       pin, but the device keeps the transfer: when devmgr binds the
 *       function again (REBIND: a new dma_cap, BME on) before the device
 *       got to it, the write lands in the page whose pin was released (a
 *       killed driver's pages go back to the allocator). QEMU's edu starts
 *       a transfer 100 ms after the command, so a quick REBIND hits it
 *       every time; an xHCI left running (DCBAA, rings, scratchpads) does
 *       the same the moment BME is back.
 *
 *   m6p2_bar_overlaps_live_function (fixed: resource.c)
 *       resource_pci_bar checked only a sub-page BAR's rounding slack
 *       against other functions (9d4719a): a function whose recorded BAR
 *       (stale, unassigned, or firmware-overlapping) covers another live
 *       function's registers got a RES_MMIO over them, and
 *       pci_bar_resource then turned its memory decode on. */
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
#define CMD_BME 0x04

#define EDU_DMA_SRC 0x80
#define EDU_DMA_DST 0x88
#define EDU_DMA_CNT 0x90
#define EDU_DMA_CMD 0x98
#define EDU_BUF     0x40000u
#define DMA_RUN     0x1u
#define DMA_TO_RAM  0x2u

static bool edu_wait_idle(volatile uint8_t *r)
{
    uint64_t end = uptime_ns() + 2000000000ull;
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

static void bme(struct pci_dev *d, bool on)
{
    uint64_t f = pci_cmd_lock();
    KT_EQ(pci_set_bus_master(d, on), OK);
    pci_cmd_unlock(f);
}

KTEST(review_m6p2_stale_dma_after_rebind)
{
    struct pci_dev *d = pci_find(0x1234, 0x11e8, 0);
    if (!d) {
        kprintf("ktest %s: no edu, skipped\n", ktest_current);
        return;
    }
    uint64_t cf = pci_cmd_lock();
    KT_EQ(pci_enable_memory(d), OK);
    pci_cmd_unlock(cf);
    volatile uint8_t *r = vmm_map_mmio(d->info.bar[0].phys, PG);
    KT_ASSERT(edu_wait_idle(r));

    /* The dead driver's buffer: page 0 the pattern, page 1 the target. */
    struct vmo *v;
    KT_EQ(vmo_create(2 * PG, VMO_CONTIGUOUS | VMO_DMA32, &v), OK);
    static uint8_t buf[PAGE_SIZE];
    for (uint32_t i = 0; i < PG; i++)
        buf[i] = (uint8_t)(0xa5 ^ i);
    KT_EQ(vmo_write(v, 0, buf, PG), OK);

    /* The pattern into the device's buffer, with a cap of its own. */
    struct kobject *cap;
    KT_EQ(dma_cap_create_for(d, &cap), OK);
    struct khandle kh = khandle_from_new(cap, DMA_CAP_RIGHTS);
    bme(d, true);
    uint64_t pa[2], id;
    KT_EQ(vmo_pin(v, cap, 0, 2 * PG, pa, 2, &id), OK);
    edu_dma(r, pa[0], EDU_BUF, PG, 0);
    KT_ASSERT(edu_wait_idle(r));
    khandle_release(&kh);

    uint32_t hit[2];
    bool idle[2];
    for (int rebind = 0; rebind < 2; rebind++) {
        /* A driver (one dma_cap handle, BME on) with a transfer to its
         * page 1 queued when it dies: BME off, pins released. */
        KT_EQ(dma_cap_create_for(d, &cap), OK);
        kobject_ref(cap);   /* ours, to look at it afterwards */
        kh = khandle_from_new(cap, DMA_CAP_RIGHTS);
        bme(d, true);
        KT_EQ(vmo_pin(v, cap, 0, 2 * PG, pa, 2, &id), OK);
        edu_dma(r, EDU_BUF, pa[1], PG, DMA_TO_RAM);
        khandle_release(&kh);
        KT_ASSERT(!(pci_cfg_read(d, 0x04, 2) & CMD_BME));
        KT_EQ(dma_cap_pin_count(cap), 0);
        kobject_unref(cap);

        /* Control (rebind 0): nobody turns BME on while the device works.
         * rebind 1: devmgr REBINDs at once (a new dma_cap, BME on). */
        struct khandle kh2 = { 0 };
        if (rebind) {
            struct kobject *cap2;
            KT_EQ(dma_cap_create_for(d, &cap2), OK);
            kh2 = khandle_from_new(cap2, DMA_CAP_RIGHTS);
            bme(d, true);
        }
        idle[rebind] = edu_wait_idle(r);
        if (rebind)
            khandle_release(&kh2);   /* BME off */
        memset(buf, 0, sizeof(buf));
        KT_EQ(vmo_read(v, PG, buf, PG), OK);
        hit[rebind] = 0;
        for (uint32_t i = 0; i < PG; i++)
            hit[rebind] += buf[i] && buf[i] == (uint8_t)(0xa5 ^ i);   /* (16 bytes of it are 0) */
        memset(buf, 0, sizeof(buf));
        KT_EQ(vmo_write(v, PG, buf, PG), OK);
    }
    kprintf("ktest %s: bytes the device wrote into the page after its pin was released: "
            "%u without a rebind (control), %u with an immediate rebind (of %u)\n",
            ktest_current, hit[0], hit[1], (unsigned)PG - 16);
    kobject_unref(vmo_kobject(v));
    KT_ASSERT(idle[0] && idle[1]);
    KT_EQ(hit[0], 0);   /* BME off stops it */
    KT_EQ(hit[1], 0);   /* ... and must keep stopping it across a rebind */
}

KTEST(m6p2_bar_overlaps_live_function)
{
    struct pci_dev *e = pci_find(0x1234, 0x11e8, 0), *x = pci_find(0x1b36, 0x000d, 0);
    if (!e || !x) {
        kprintf("ktest %s: no edu + qemu-xhci, skipped\n", ktest_current);
        return;
    }
    KT_ASSERT(pci_cfg_read(x, 0x04, 2) & 0x2);   /* the xHCI decodes its BAR 0 */
    KT_ASSERT(e->info.bar[0].size >= x->info.bar[0].size);
    struct kobject *root = resource_root(), *pci, *dev, *res = NULL;
    KT_ASSERT(root);
    KT_EQ(resource_create(root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(resource_pci_device(pci, e->index, &dev), OK);
    /* edu's BAR 0 as firmware might have left a disabled function's: over
     * the xHCI's registers (page-aligned and bigger than a page, so it has
     * no rounding slack). */
    uint64_t saved = e->info.bar[0].phys;
    e->info.bar[0].phys = x->info.bar[0].phys;
    status_t st = resource_pci_bar(dev, 0, &res);
    e->info.bar[0].phys = saved;
    uint64_t base = 0, size = 0;
    if (st == OK)
        resource_range(res, &base, &size);
    kprintf("ktest %s: a BAR over 00:%02x.%u's registers [%lx, +%lx): %s%s\n", ktest_current,
            x->info.dev, x->info.fn, x->info.bar[0].phys, x->info.bar[0].size, status_str(st),
            st == OK ? " (a RES_MMIO over another function's registers)" : "");
    if (res)
        kobject_unref(res);
    kobject_unref(dev);
    kobject_unref(pci);
    kobject_unref(root);
    KT_EQ(st, ERR_ACCESS_DENIED);
}
