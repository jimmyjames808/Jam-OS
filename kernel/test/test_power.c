/* PCI power states: devmgr (RIGHT_MANAGE on the
 * function) may change the PM power state, to wake a function it finds in
 * D1-D3 before binding a driver; a driver may not. The system call waits
 * out the transition and puts back what a D3hot -> D0 reset lost. Uses
 * QEMU's e1000e (8086:10d3, it has a PM capability) and is skipped
 * anywhere else: on the PC it would power real devices down and up. */
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
    struct pci_dev *d = pci_find(0x8086, 0x10d3, 0);   /* QEMU's e1000e only */
    if (!d || !d->info.bar[0].size || !(d->info.bar[0].flags & PCI_BAR_MMIO) ||
        !(*pm = pci_find_cap(d, PM_CAP)))
        return NULL;
    return d;
}

KTEST(pci_power_wake_needs_manage)
{
    uint16_t pm = 0;
    struct pci_dev *d = pm_function(&pm);
    if (!d) {
        kprintf("ktest %s: no QEMU e1000e with a PM capability, skipped\n", ktest_current);
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
    KT_EQ(pci_cfg_read(d, 0x04, 2) & 0x3, cmd & 0x3);   /* decode as it was */
    KT_ASSERT(!(pci_cfg_read(d, 0x04, 2) & 0x4) || (cmd & 0x4));   /* BME never turned on */
    KT_ASSERT(pci_cfg_read(d, 0x04, 2) & 0x400);   /* INTx disabled */
    handle_table_destroy(&t);
    kobject_unref(pci);
    kobject_unref(root);
}

/* sys_pci_config_write sleeps out the transition OUTSIDE the
 * command lock, so the function's owner may turn Bus Master Enable off
 * meanwhile (devmgr stopping the driver, its dma_cap closing). The restore
 * must not bring the saved BME back: it keeps BME as it is now. Here on
 * the e1000e, with the register writes a reset (or the owner) would make. */
KTEST(pci_power_restore_keeps_bus_master_off)
{
    uint16_t pm = 0;
    struct pci_dev *d = pm_function(&pm);
    if (!d) {
        kprintf("ktest %s: no QEMU e1000e with a PM capability, skipped\n", ktest_current);
        return;
    }
    uint32_t cmd0 = pci_cfg_read(d, 0x04, 2), bar0 = pci_cfg_read(d, 0x10, 4);
    uint64_t f = pci_cmd_lock();
    pci_set_bus_master(d, true);
    pci_enable_memory(d);
    struct pci_saved_config saved;
    pci_save_config(d, &saved);
    KT_ASSERT(saved.command & 0x4);
    pci_cmd_unlock(f);

    /* 1. No reset; the owner turned BME off during the sleep. */
    f = pci_cmd_lock();
    pci_set_bus_master(d, false);
    pci_cmd_unlock(f);
    f = pci_cmd_lock();
    KT_ASSERT(!pci_restore_config(d, &saved));
    pci_cmd_unlock(f);
    uint32_t cmd = pci_cfg_read(d, 0x04, 2);
    KT_EQ(cmd & 0x4, 0);        /* BME stays off */
    KT_EQ(cmd & 0x2, 0x2);      /* memory decode as saved */
    KT_ASSERT(cmd & 0x400);     /* INTx disabled */

    /* 2. A reset (command register 0, BAR lost): the BARs and decode come
     * back, BME does not. */
    f = pci_cmd_lock();
    pci_cfg_write(d, 0x04, 2, 0);
    pci_cfg_write(d, 0x10, 4, 0);
    KT_ASSERT(pci_restore_config(d, &saved));
    pci_cmd_unlock(f);
    cmd = pci_cfg_read(d, 0x04, 2);
    KT_EQ(pci_cfg_read(d, 0x10, 4), bar0);
    KT_EQ(cmd & 0x4, 0);
    KT_EQ(cmd & 0x2, 0x2);

    /* 3. BME the owner turned ON meanwhile stays on. */
    f = pci_cmd_lock();
    pci_set_bus_master(d, true);
    saved.command &= ~0x4;
    pci_restore_config(d, &saved);
    KT_EQ(pci_cfg_read(d, 0x04, 2) & 0x4, 0x4);
    /* Put it back as it was. */
    pci_set_bus_master(d, cmd0 & 0x4);
    pci_cmd_unlock(f);
}
