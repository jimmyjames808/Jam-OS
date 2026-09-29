/* Starting xhci-noop without devmgr (<jam/xhci_launch.h>): the M6 done
 * test's kernel side. The handles are built here the way devmgr builds
 * them (M6-PLAN.md "Phase 2"): the driver gets its function without
 * RIGHT_MANAGE, BAR 0, one interrupt object (MSI-X entry 0 if the
 * function has MSI-X, else MSI) and a bound dma_cap, with Bus Master
 * Enable turned on here (MSI needs it too). Everything the driver holds
 * goes when it exits: the dma_cap's close turns Bus Master Enable off,
 * the interrupt object's disables MSI / MSI-X; both are checked. */
#include <jam/abi.h>
#include <jam/driver.h>
#include <jam/driver_kernel.h>
#include <jam/interrupt.h>
#include <jam/kprintf.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/report.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/startup.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/xhci_launch.h>

#define S           1000000000ull
#define RUN_LIMIT_S 30            /* the driver's own waits add up to a few seconds */
#define IRQ_RIGHTS  (RIGHTS_BASIC | RIGHTS_IO)
#define DEV_RIGHTS  (RES_RIGHTS & ~RIGHT_MANAGE)

struct pci_dev *xhci_find(uint32_t n)
{
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d && (!pci_hide_in_use || !d->proc_users) && d->info.class_code == 0x0c &&
            d->info.subclass == 0x03 &&
            d->info.prog_if == 0x30 && n-- == 0)
            return d;
    }
    return NULL;
}

/* The four handles, by role; on failure nothing is left behind. */
static status_t build_handles(struct pci_dev *d, uint32_t roles[4], struct khandle kh[4],
                              const char **what)
{
    struct kobject *root = resource_root(), *pci = NULL, *dev = NULL, *bar = NULL;
    struct kobject *irq = NULL, *cap = NULL;
    status_t st = root ? resource_create(root, RES_PCI, 0, 0, &pci) : ERR_BAD_STATE;
    *what = "RES_PCI";
    if (st == OK) {
        *what = "RES_PCI_DEV";
        st = resource_pci_device(pci, d->index, &dev);
    }
    if (st == OK) {
        *what = "BAR 0";
        st = resource_pci_bar(dev, 0, &bar);
    }
    if (st == OK) {
        uint64_t f = pci_cmd_lock();
        st = pci_enable_memory(d);
        pci_cmd_unlock(f);
    }
    if (st == OK) {
        *what = d->cap_msix ? "MSI-X interrupt" : "MSI interrupt";
        st = interrupt_create_msi(d, 0, d->cap_msix ? IRQ_MSIX : 0, &irq);
    }
    if (st == OK) {
        *what = "dma_cap";
        st = dma_cap_create_for(d, &cap);
    }
    if (st == OK) {
        *what = "bus master";
        uint64_t f = pci_cmd_lock();
        st = pci_set_bus_master(d, true);
        pci_cmd_unlock(f);
    }
    if (root)
        kobject_unref(root);
    if (pci)
        kobject_unref(pci);
    if (st != OK) {
        /* The cap's destroy turns Bus Master Enable off again. */
        struct kobject *objs[] = { dev, bar, irq, cap };
        for (unsigned i = 0; i < 4; i++)
            if (objs[i]) {
                struct khandle k = khandle_from_new(objs[i], RIGHTS_BASIC);
                khandle_release(&k);   /* on_zero_handles, then the last reference */
            }
        return st;
    }
    roles[0] = DR_PCIDEV;
    kh[0] = khandle_from_new(dev, DEV_RIGHTS);
    roles[1] = DR_BAR(0);
    kh[1] = khandle_from_new(bar, DEV_RIGHTS);
    roles[2] = DR_IRQ(0);
    kh[2] = khandle_from_new(irq, IRQ_RIGHTS);
    roles[3] = DR_DMA;
    kh[3] = khandle_from_new(cap, DMA_CAP_RIGHTS);
    return OK;
}

/* MSI / MSI-X enable bits of d, read back. */
static bool msi_on(struct pci_dev *d)
{
    bool on = false;
    if (d->cap_msi)
        on |= pci_cfg_read(d, d->cap_msi + 2, 2) & 1;
    if (d->cap_msix)
        on |= (pci_cfg_read(d, d->cap_msix + 2, 2) >> 15) & 1;
    return on;
}

bool xhci_launch(struct pci_dev *d, bool process)
{
    const char *mode = process ? "process" : "kernel";
    const struct pci_dev_info *in = &d->info;
    struct job *root, *job;
    if (userboot_root_job(&root) != OK) {
        report("xhcitest (%s): no memory for a job", mode);
        return false;
    }
    status_t st = job_create(root, &job);
    job_unref(root);   /* job keeps it */
    if (st != OK) {
        report("xhcitest (%s): no job (%s)", mode, status_str(st));
        return false;
    }

    uint32_t roles[4];
    struct khandle kh[4];
    const char *what = "";
    st = build_handles(d, roles, kh, &what);
    if (st != OK) {
        report("xhcitest (%s): %02x:%02x.%u: can't make the %s (%s)", mode, in->bus, in->dev,
               in->fn, what, status_str(st));
        job_unref(job);
        return false;
    }

    uint64_t t0 = uptime_ns();
    struct process *p = NULL;
    if (process) {
        static const char *const argv[] = { "xhci-noop (process)" };
        struct userboot_handle extra[4];
        for (unsigned i = 0; i < 4; i++) {
            extra[i].role = SR_USER + roles[i];   /* SR_DRIVER(role) in <os.h> */
            extra[i].kh = kh[i];
        }
        st = userboot_spawn("drv/xhci-noop", argv, 1, job, extra, 4, NULL, &p);
    } else {
        struct driver_kernel_handle hs[4];
        for (unsigned i = 0; i < 4; i++) {
            hs[i].role = roles[i];
            hs[i].kh = kh[i];
        }
        driver_main_fn fn = driver_kernel_find("xhci-noop");
        st = fn ? driver_kernel_start("xhci-noop (kernel)", fn, hs, 4, job, &p) : ERR_NOT_FOUND;
        if (!fn)
            for (unsigned i = 0; i < 4; i++)
                khandle_release(&hs[i].kh);
    }
    if (st != OK) {
        report("xhcitest (%s): can't start xhci-noop (%s)", mode, status_str(st));
        job_unref(job);
        return false;
    }

    st = object_wait_one(process_kobject(p), SIG_TERMINATED, t0 + RUN_LIMIT_S * S, NULL);
    bool hung = st != OK;
    if (hung) {
        process_kill(p, PROCESS_KILLED_CODE, true);
        object_wait_one(process_kobject(p), SIG_TERMINATED, DEADLINE_NEVER, NULL);
    }
    struct process_info info;
    process_get_info(p, &info);
    kobject_unref(process_kobject(p));
    uint64_t ms = (uptime_ns() - t0) / 1000000;

    /* Its handles are gone with it: bus master and MSI off, job empty. */
    bool bme = pci_cfg_read(d, 0x04, 2) & (1u << 2), msi = msi_on(d);
    bool clean = true;
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        clean &= job_used(job, k) == 0;
    job_unref(job);
    bool ok = !hung && !info.killed && info.exit_code == 0 && !bme && !msi && clean;
    if (hung)
        report("xhcitest (%s): xhci-noop still running after %u s: killed", mode, RUN_LIMIT_S);
    else
        report("xhcitest (%s): xhci-noop %s %ld after %lu ms; bus master %s, MSI %s, job %s -> %s",
               mode, info.killed ? "killed," : "exit", (long)info.exit_code, ms,
               bme ? "STILL ON" : "off", msi ? "STILL ON" : "off", clean ? "clean" : "LEAKED",
               ok ? "PASS" : "FAIL");
    return ok;
}
