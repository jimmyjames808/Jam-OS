/* init's "xhcitest" mode (the M6 done test's process half; the kernel
 * passes argv[1] = "xhcitest" on that boot entry). It does what devmgr
 * will do for one device, through the M6 system calls only: slice RES_PCI
 * from the root resource, find the xHCI (class 0c0330) with pci_enum, open
 * it, make its BAR 0 resource, its interrupt object (MSI-X entry 0, else
 * MSI), a dma_cap, turn Bus Master Enable on, and start drv/xhci-noop in a
 * job of its own with those handles (the device without RIGHT_MANAGE).
 * Then it waits for the driver, checks Bus Master Enable went off with the
 * driver's dma_cap and the driver's job is empty, and reports. */
#include <os.h>
#include <jam/driver.h>

#define S             1000000000ull
#define RUN_TIMEOUT_S 30
#define DRV_RIGHTS    (RIGHTS_BASIC | RIGHTS_IO | RIGHT_MAP | RIGHT_SLICE)   /* no MANAGE */

void init_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void close_all(handle_t *hs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (hs[i] != HANDLE_INVALID)
            jam_handle_close(hs[i]);
}

bool init_xhcitest(void)
{
    handle_t root = startup_handle(SR_RESOURCE), pci = HANDLE_INVALID, dev = HANDLE_INVALID;
    status_t st = jam_resource_create(root, RES_PCI, 0, 0, &pci);
    if (st != OK) {
        init_say("init: xhcitest: no RES_PCI from the root resource (%s)", status_str(st));
        return false;
    }
    struct pci_dev_info info;
    uint32_t index = 0;
    bool found = false;
    for (; (st = jam_pci_enum(pci, index, &info)) == OK; index++)
        if (info.class_code == 0x0c && info.subclass == 0x03 && info.prog_if == 0x30) {
            found = true;
            break;
        }
    if (!found) {
        init_say("init: xhcitest: no xHCI (class 0c0330) in pci_enum (%s)", status_str(st));
        jam_handle_close(pci);
        return false;
    }

    /* The driver's handles, by role; [4] is the device for the driver. */
    handle_t h[5] = { HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID,
                      HANDLE_INVALID };
    const char *what = "pci_device_open";
    bool msix = info.msix_vectors > 0;
    st = jam_pci_device_open(pci, index, &dev);
    if (st == OK) {
        what = "pci_bar_resource";
        st = jam_pci_bar_resource(dev, 0, &h[0]);
    }
    if (st == OK) {
        what = msix ? "interrupt_create_msi (MSI-X)" : "interrupt_create_msi (MSI)";
        st = jam_interrupt_create_msi(dev, 0, msix ? IRQ_MSIX : 0, &h[1]);
    }
    if (st == OK) {
        what = "dma_cap_create";
        st = jam_dma_cap_create(dev, &h[2]);
    }
    if (st == OK) {
        what = "pci_bus_master";
        st = jam_pci_bus_master(dev, 1);
    }
    if (st == OK) {
        what = "handle_duplicate (device without RIGHT_MANAGE)";
        st = jam_handle_duplicate(dev, DRV_RIGHTS, &h[3]);
    }
    if (st == OK) {
        what = "BAR 0 without RIGHT_MANAGE";
        handle_t bar = h[0];
        st = jam_handle_duplicate(bar, DRV_RIGHTS, &h[0]);
        jam_handle_close(bar);
    }
    if (st == OK) {
        what = "job_create";
        st = jam_job_create(startup_handle(SR_JOB), 0, &h[4]);
    }
    if (st != OK) {
        init_say("init: xhcitest: %02x:%02x.%u: %s failed (%s)", info.bus, info.dev, info.fn, what,
                 status_str(st));
        jam_pci_bus_master(dev, 0);
        close_all(h, 5);
        jam_handle_close(dev);
        jam_handle_close(pci);
        return false;
    }

    handle_t job = h[4], proc;
    struct spawn_handle extra[4] = {
        { SR_DRIVER(DR_BAR(0)), h[0] },
        { SR_DRIVER(DR_IRQ(0)), h[1] },
        { SR_DRIVER(DR_DMA), h[2] },
        { SR_DRIVER(DR_PCIDEV), h[3] },
    };
    static const char *const argv[] = { "xhci-noop (process)" };
    struct spawn_args a = {
        .path = "drv/xhci-noop", .name = "xhci-noop", .argc = 1, .argv = argv, .job = job,
        .extra = extra, .nextra = 4,
    };
    uint64_t t0 = (uint64_t)jam_clock_get();
    st = spawn(&a, &proc);   /* the four handles are gone either way */
    if (st != OK) {
        init_say("init: xhcitest: can't start drv/xhci-noop (%s)", status_str(st));
        jam_pci_bus_master(dev, 0);
        jam_handle_close(job);
        jam_handle_close(dev);
        jam_handle_close(pci);
        return false;
    }
    struct process_info pi;
    st = spawn_wait(proc, RUN_TIMEOUT_S * S, &pi);
    bool hung = st == ERR_TIMED_OUT;
    if (hung) {
        jam_job_kill(job);
        st = spawn_wait(proc, 10 * S, &pi);
    }
    uint64_t ms = ((uint64_t)jam_clock_get() - t0) / 1000000;
    jam_handle_close(proc);

    /* The dma_cap died with the driver: Bus Master Enable must be off. */
    uint32_t cmd = 0xffff;
    jam_pci_config_read(dev, 0x04, 2, &cmd);
    bool bme = cmd & (1u << 2);
    struct job_info ji;
    bool clean = jam_job_get_info(job, &ji) == OK;
    for (unsigned k = 1; clean && k < JOB_LIMIT_COUNT; k++)
        clean = ji.used[k] == 0;
    bool ok = st == OK && !hung && !pi.killed && pi.exit_code == 0 && !bme && clean;
    if (st != OK)
        init_say("init: xhcitest (process): lost track of xhci-noop (%s)", status_str(st));
    else
        init_say("xhcitest (process): xhci-noop %s %ld after %lu ms; bus master %s, job %s -> %s",
                 hung ? "hung, killed," : pi.killed ? "killed," : "exit", (long)pi.exit_code,
                 (unsigned long)ms, bme ? "STILL ON" : "off", clean ? "clean" : "LEAKED",
                 ok ? "PASS" : "FAIL");
    if (bme)
        jam_pci_bus_master(dev, 0);
    jam_handle_close(job);
    jam_handle_close(dev);
    jam_handle_close(pci);
    return ok;
}
