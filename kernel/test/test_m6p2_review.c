/* Repros from the independent review of M6 phase 2 (QEMU only: they need
 * the edu device and qemu-xhci). review_* tests are skipped by the
 * default `ktest` run until their bug is fixed; run them with
 * "ktest=review_m6p2". None is left open:
 *
 *   review_m6p2_stale_dma_after_rebind (fixed in M7 Track D: dma_cap.c's
 *       per-function owner and quarantine, the driver turns bus mastering
 *       on after quiescing): now kernel/test/test_dma.c,
 *       dma_stale_write_after_rebind.
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
