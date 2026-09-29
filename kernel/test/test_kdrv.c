/* The kernel build of drivers (driver_kernel.c) must refuse what the
 * system calls refuse a driver process:
 *
 *   m6r_kdrv_config_filter     drv_pci_config_write refuses what the
 *                              pci_config_write syscall refuses (a bridge,
 *                              BIST, INTx Disable)
 *   m6r_kdrv_mmio_kernel_owned drv_mmio_map refuses kernel-owned MMIO (the
 *                              I/O APIC) like vmo_create_physical does
 *
 * They go when the kernel build of drivers does. */
#include <jam/acpi.h>
#include <jam/driver.h>
#include <jam/driver_kernel.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/time.h>
#include <jam/vmo.h>

#define PG PAGE_SIZE

static void run_driver(driver_main_fn fn, struct driver_kernel_handle *hs, unsigned n,
                       struct job *j)
{
    struct process *p;
    KT_EQ(driver_kernel_start("m6r", fn, hs, n, j, &p), OK);
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 30 * NS_PER_S, NULL),
          OK);
    struct process_info info;
    process_get_info(p, &info);
    KT_EQ(info.killed, 0);
    kobject_unref(process_kobject(p));
}

/* ---- the kernel build's config filter --------------------------------------- */

#define ROLE_BRIDGE 0x41
#define ROLE_EDU    0x42

static status_t cf_bridge, cf_bist, cf_intx;

static int cf_main(const struct driver_start *s)
{
    handle_t hb = drv_handle(s, ROLE_BRIDGE), he = drv_handle(s, ROLE_EDU);
    uint32_t v = 0, cmd = 0;
    drv_pci_config_read(hb, 0x3c, 1, &v);
    cf_bridge = drv_pci_config_write(hb, 0x3c, 1, v);   /* same value: harmless */
    cf_bist = drv_pci_config_write(he, 0x0f, 1, 0);      /* edu has no BIST */
    drv_pci_config_read(he, 0x04, 2, &cmd);
    cf_intx = drv_pci_config_write(he, 0x04, 2, cmd ^ 0x400);
    if (cf_intx == OK)
        drv_pci_config_write(he, 0x04, 2, cmd);
    return 0;
}

KTEST(m6r_kdrv_config_filter)
{
    struct pci_dev *hb = pci_get(0), *edu = pci_find(0x1234, 0x11e8, 0);
    if (!hb || !edu || !(hb->info.flags & PCI_INFO_BRIDGE)) {
        kprintf("ktest %s: needs QEMU q35 + edu, skipped\n", ktest_current);
        return;
    }
    struct job *j = kt_fresh_job();
    struct driver_kernel_handle hs[2] = {
        { ROLE_BRIDGE, khandle_from_new(kt_pci_dev_res(hb), RES_RIGHTS) },
        { ROLE_EDU, khandle_from_new(kt_pci_dev_res(edu), RES_RIGHTS) },
    };
    run_driver(cf_main, hs, 2, j);
    /* What the syscall layer answers for the same writes. */
    KT_EQ(cf_bridge, ERR_ACCESS_DENIED);
    KT_EQ(cf_bist, ERR_ACCESS_DENIED);
    KT_EQ(cf_intx, ERR_ACCESS_DENIED);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- the kernel build's drv_mmio_map ------------------------------------------ */

static status_t mm_st;

static int mm_main(const struct driver_start *s)
{
    volatile void *p = NULL;
    mm_st = drv_mmio_map(drv_handle(s, ROLE_EDU), 0, PG, VMO_CACHE_UC, &p);
    return 0;
}

KTEST(m6r_kdrv_mmio_kernel_owned)
{
    if (!acpi.ioapic_count) {
        kprintf("ktest %s: no I/O APIC, skipped\n", ktest_current);
        return;
    }
    uint64_t io = ALIGN_DOWN(acpi.ioapics[0].phys, PG);
    struct kobject *root = resource_root(), *res;
    KT_EQ(resource_create(root, RES_MMIO, io, PG, &res), OK);   /* not RAM: a valid slice */
    kobject_unref(root);
    KT_EQ(resource_phys_mappable(io, PG), ERR_ACCESS_DENIED);   /* what processes get */
    struct job *j = kt_fresh_job();
    struct driver_kernel_handle h = { ROLE_EDU, khandle_from_new(res, RES_RIGHTS) };
    run_driver(mm_main, &h, 1, j);
    KT_EQ(mm_st, ERR_ACCESS_DENIED);
    kt_job_is_empty(j);
    job_unref(j);
}
