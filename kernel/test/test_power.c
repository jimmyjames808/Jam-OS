/* M7 Track D, review of M6 phase 2 finding 7: devmgr (RIGHT_MANAGE on the
 * function) may change the PM power state, to wake a function it finds in
 * D1-D3 before binding a driver; a driver may not. The system call waits
 * out the transition and puts back what a D3hot -> D0 reset lost. Needs a
 * function with a PM capability that is neither a bridge nor the display
 * (QEMU's e1000e); skipped without one. */
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/pci.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/time.h>

#define PM_CAP 0x01

static struct pci_dev *pm_function(uint16_t *pm)
{
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (!d || (d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY)) ||
            (d->info.header_type & 0x7f) || !d->info.bar[0].size ||
            !(d->info.bar[0].flags & PCI_BAR_MMIO))
            continue;
        if ((*pm = pci_find_cap(d, PM_CAP)))
            return d;
    }
    return NULL;
}

KTEST(pci_power_wake_needs_manage)
{
    uint16_t pm = 0;
    struct pci_dev *d = pm_function(&pm);
    if (!d) {
        kprintf("ktest %s: no function with a PM capability, skipped\n", ktest_current);
        return;
    }
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *root = resource_root(), *pci, *dev;
    KT_EQ(resource_create(root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(resource_pci_device(pci, d->index, &dev), OK);
    kobject_ref(dev);   /* one reference per handle */
    struct khandle km = khandle_from_new(dev, RES_RIGHTS);   /* devmgr's */
    struct khandle kd = khandle_from_new(dev, RIGHT_READ | RIGHT_WRITE);   /* a driver's */
    handle_t hm, hd;
    KT_EQ(handle_insert(&t, &km, &hm), OK);
    KT_EQ(handle_insert(&t, &kd, &hd), OK);

    uint32_t pmcsr = 0, bar0 = pci_cfg_read(d, 0x10, 4), cmd = pci_cfg_read(d, 0x04, 2);
    KT_EQ(sys_pci_config_read(&t, hm, pm + 4, 2, &pmcsr), OK);
    KT_EQ(pmcsr & 3, 0);   /* firmware left it in D0 */
    uint32_t d3 = (pmcsr & ~0x8003u) | 3, d0 = pmcsr & ~0x8003u;   /* (bit 15: PME status, W1C) */
    KT_EQ(sys_pci_config_write(&t, hd, pm + 4, 2, d3), ERR_ACCESS_DENIED);
    uint64_t t0 = uptime_ns();
    KT_EQ(sys_pci_config_write(&t, hm, pm + 4, 2, d3), OK);
    KT_ASSERT(uptime_ns() - t0 >= 10000000ull);   /* the transition is waited out */
    KT_EQ(sys_pci_config_read(&t, hm, pm + 4, 2, &pmcsr), OK);
    uint32_t in_d3 = pmcsr & 3;
    KT_EQ(sys_pci_config_write(&t, hd, pm + 4, 2, d0), ERR_ACCESS_DENIED);   /* nor wake it */
    KT_EQ(sys_pci_config_write(&t, hm, pm + 4, 2, d0), OK);
    KT_EQ(sys_pci_config_read(&t, hm, pm + 4, 2, &pmcsr), OK);
    kprintf("ktest %s: %02x:%02x.%x %04x:%04x: D%u after the D3hot write, D%u after waking; "
            "BAR 0 %08x -> %08x, command %04x -> %04x\n", ktest_current, d->info.bus,
            d->info.dev, d->info.fn, d->info.vendor, d->info.device, in_d3, pmcsr & 3, bar0,
            pci_cfg_read(d, 0x10, 4), cmd, pci_cfg_read(d, 0x04, 2));
    KT_EQ(in_d3, 3);
    KT_EQ(pmcsr & 3, 0);
    KT_EQ(pci_cfg_read(d, 0x10, 4), bar0);   /* whatever a reset lost is back */
    KT_EQ(pci_cfg_read(d, 0x04, 2) & 0x7, cmd & 0x7);
    KT_ASSERT(pci_cfg_read(d, 0x04, 2) & 0x400);   /* INTx disabled */
    handle_table_destroy(&t);
    kobject_unref(pci);
    kobject_unref(root);
}
