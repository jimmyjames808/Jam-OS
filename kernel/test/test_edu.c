/* M6 phase 2: drivers bound by the kernel (kernel/drivers/kdevmgr.c) on
 * QEMU's devices. The edu driver as a kernel process with exactly the
 * handles devmgr gives a process (<jam/kdevmgr.h>), called through the
 * generated edu client; killed in the middle of a DMA (Bus Master Enable
 * off, pins released, vector freed, its job clean, the device bindable
 * again); and a driver's limits with qemu-xhci's handles (no MSI-X page,
 * no pin or DMA memory without a dma_cap, no bus mastering through config
 * space). Each test skips itself when QEMU's device isn't there (the PC). */
#include <jam/channel.h>
#include <jam/driver.h>
#include <jam/driver_kernel.h>
#include <jam/interrupt_test.h>
#include <jam/kdevmgr.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <idl/edu.h>

#define S       1000000000ull
#define MS      1000000ull
#define CMD_BME 0x04

static struct pci_dev *edu_dev(void)
{
    struct pci_dev *d = pci_find(0x1234, 0x11e8, 0);
    if (!d)
        kprintf("ktest %s: no edu device (not QEMU?), skipped\n", ktest_current);
    return d;
}

static struct job *root_job(void)
{
    struct job *j;
    KT_EQ(userboot_root_job(&j), OK);
    return j;
}

static bool bus_master_on(struct pci_dev *d)
{
    return pci_cfg_read(d, 0x04, 2) & CMD_BME;
}

/* The whole edu protocol from a kernel process, through <idl/edu.h>; the
 * driver ends cleanly when its client goes and leaves nothing behind. */
KTEST(driver_kernel_edu)
{
    struct pci_dev *d = edu_dev();
    if (!d)
        return;
    struct job *root = root_job();
    uint64_t irqs = interrupt_live_count(), chans = channel_live_count();
    struct kdev_binding b;
    KT_EQ(kdev_bind(d, "edu", root, &b), OK);
    KT_ASSERT(b.irq && b.dma_cap);
    KT_ASSERT(bus_master_on(d));
    KT_EQ(interrupt_live_count(), irqs + 1);
    KT_EQ(kdev_edu_check(&b, "edu (kernel)"), OK);
    struct kobject *irq = b.irq, *cap = b.dma_cap;
    kobject_ref(irq);
    kobject_ref(cap);
    KT_ASSERT(kdev_unbind(&b, 10 * S));   /* exited 0 by itself, job clean */
    KT_ASSERT(!bus_master_on(d));
    KT_EQ(dma_cap_pin_count(cap), 0);
    uint32_t cpu;
    uint8_t vec;
    KT_ASSERT(!interrupt_vector_of(irq, &cpu, &vec));
    kobject_unref(irq);
    kobject_unref(cap);
    KT_EQ(interrupt_live_count(), irqs);
    KT_EQ(channel_live_count(), chans);
    job_unref(root);
}

/* Killed while a DMA it started is still running (QEMU's edu takes ~100
 * ms per transfer; dma_start answers with the pin held). */
KTEST(driver_kernel_edu_killed_mid_dma)
{
    struct pci_dev *d = edu_dev();
    if (!d)
        return;
    struct job *root = root_job();
    uint64_t irqs = interrupt_live_count();
    struct kdev_binding b;
    KT_EQ(kdev_bind(d, "edu", root, &b), OK);
    struct edu_dma_start_req q = { 0, EDU_DMA_START, 4096 };
    struct edu_dma_start_rep r;
    uint32_t n = 0;
    KT_EQ(channel_call((struct channel *)b.client.obj, &q, sizeof(q), NULL, 0, &r, sizeof(r), &n,
                       NULL, 0, NULL, uptime_ns() + 10 * S),
          OK);
    KT_EQ(idl_rep_status(&r, n, sizeof(r)), OK);
    KT_ASSERT(r.device_addr && r.device_addr < (1ull << 32));
    KT_EQ(dma_cap_pin_count(b.dma_cap), 1);   /* mid-DMA: pinned, bus master on */
    KT_ASSERT(bus_master_on(d));
    uint32_t cpu;
    uint8_t vec;
    KT_ASSERT(interrupt_vector_of(b.irq, &cpu, &vec));

    uint64_t t0 = uptime_ns();
    process_kill(b.proc, PROCESS_KILLED_CODE, true);
    KT_EQ(object_wait_one(process_kobject(b.proc), SIG_TERMINATED, uptime_ns() + 10 * S, NULL),
          OK);
    kprintf("ktest %s: killed mid-DMA, dead after %lu us\n", ktest_current,
            (uptime_ns() - t0) / 1000);
    KT_ASSERT(!bus_master_on(d));
    KT_EQ(dma_cap_pin_count(b.dma_cap), 0);
    KT_ASSERT(!interrupt_vector_of(b.irq, &cpu, &vec));   /* vector freed */
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        KT_EQ(job_used(b.job, k), 0);
    KT_ASSERT(!kdev_unbind(&b, S));   /* it was killed: not a clean exit */
    KT_EQ(interrupt_live_count(), irqs);   /* our extra reference was the last */

    /* The device's transfer ends by itself (with Bus Master Enable off it
     * reaches no memory); the device is free for a new driver, whose
     * setup waits for it. */
    KT_EQ(kdev_bind(d, "edu", root, &b), OK);
    struct edu_factorial_req fq = { 0, EDU_FACTORIAL, 10 };
    struct edu_factorial_rep fr;
    KT_EQ(channel_call((struct channel *)b.client.obj, &fq, sizeof(fq), NULL, 0, &fr, sizeof(fr),
                       &n, NULL, 0, NULL, uptime_ns() + 10 * S),
          OK);
    KT_EQ(idl_rep_status(&fr, n, sizeof(fr)), OK);
    KT_EQ(fr.result, 3628800);
    KT_ASSERT(kdev_unbind(&b, 10 * S));
    KT_EQ(interrupt_live_count(), irqs);
    job_unref(root);
}

/* ---- a driver's limits, with qemu-xhci's handles (it has MSI-X) --------------- */

static struct {
    uint32_t bar;           /* the BAR with the MSI-X table (DR_BAR(bar)) */
    uint32_t table_off, pba_off, cap_msix;
    uint32_t checks, failures;
} view;

#define VCHECK(c)                                                       \
    do {                                                                \
        view.checks++;                                                  \
        if (!(c)) {                                                     \
            view.failures++;                                            \
            drv_log("FAILED at line %d: %s", __LINE__, #c);             \
        }                                                               \
    } while (0)

/* Runs as a kernel-process driver holding what devmgr would give
 * xhci-noop, minus the interrupt and the dma_cap. */
static int view_main(const struct driver_start *s)
{
    handle_t dev = drv_handle(s, DR_PCIDEV), bar = drv_handle(s, DR_BAR(view.bar)), v;
    volatile void *m = NULL;
    VCHECK(dev != HANDLE_INVALID && bar != HANDLE_INVALID);
    /* The MSI-X table and PBA pages are the kernel's. */
    VCHECK(drv_mmio_map(bar, view.table_off, 16, VMO_CACHE_UC, &m) == ERR_ACCESS_DENIED);
    VCHECK(drv_mmio_map(bar, view.pba_off, 8, VMO_CACHE_UC, &m) == ERR_ACCESS_DENIED);
    /* The rest of its BAR is its own. */
    VCHECK(drv_mmio_map(bar, 0, 4096, VMO_CACHE_UC, &m) == OK);
    VCHECK(m && drv_read8(m, 0) != 0);   /* xHCI CAPLENGTH */
    /* Config space: reads yes; bus mastering and the MSI-X capability no. */
    uint32_t cmd = 0, ctl = 0;
    VCHECK(drv_pci_config_read(dev, 0x04, 2, &cmd) == OK);
    VCHECK(drv_pci_config_write(dev, 0x04, 2, cmd ^ CMD_BME) == ERR_ACCESS_DENIED);
    VCHECK(drv_pci_config_read(dev, view.cap_msix + 2, 2, &ctl) == OK);
    VCHECK(drv_pci_config_write(dev, view.cap_msix + 2, 2, ctl | 0x4000) == ERR_ACCESS_DENIED);
    /* No dma_cap: no DMA memory, no pins. */
    VCHECK(drv_vmo_create(4096, DRV_VMO_CONTIGUOUS | DRV_VMO_DMA32, &v) == ERR_ACCESS_DENIED);
    VCHECK(drv_vmo_create(4096, DRV_VMO_DMA32, &v) == ERR_ACCESS_DENIED);
    VCHECK(drv_vmo_create(4096, 0, &v) == OK);
    uint64_t addr = 0, pin = 0;
    VCHECK(drv_vmo_pin(v, HANDLE_INVALID, 0, 4096, &addr, &pin) == ERR_BAD_HANDLE);
    VCHECK(drv_vmo_pin(v, dev, 0, 4096, &addr, &pin) == ERR_WRONG_TYPE);   /* not a dma_cap */
    VCHECK(addr == 0);
    drv_log("%u checks, %u failed", view.checks, view.failures);
    return view.failures ? 1 : 0;
}

KTEST(driver_kernel_limits_with_xhci_handles)
{
    struct pci_dev *d = pci_find(0x1b36, 0x000d, 0);
    if (!d || !d->cap_msix) {
        kprintf("ktest %s: no qemu-xhci with MSI-X, skipped\n", ktest_current);
        return;
    }
    KT_EQ(d->msix_table_bar, d->msix_pba_bar);   /* QEMU: both in BAR 0 */
    view = (typeof(view)){ .bar = d->msix_table_bar, .table_off = d->msix_table_off,
                           .pba_off = d->msix_pba_off, .cap_msix = d->cap_msix };
    struct job *root = root_job(), *j;
    KT_EQ(job_create(root, &j), OK);
    struct kobject *rootres = resource_root(), *pci, *dev, *bar;
    KT_EQ(resource_create(rootres, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(resource_pci_device(pci, d->index, &dev), OK);
    KT_EQ(resource_pci_bar(dev, view.bar, &bar), OK);
    struct driver_kernel_handle hs[2] = {
        { DR_PCIDEV, khandle_from_new(dev, RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE) },
        { DR_BAR(view.bar), khandle_from_new(bar, RIGHTS_BASIC | RIGHT_MAP) },
    };
    uint16_t cmd = pci_cfg_read(d, 0x04, 2), msix = pci_cfg_read(d, d->cap_msix + 2, 2);
    struct process *p;
    KT_EQ(driver_kernel_start("xhci-view", view_main, hs, 2, j, &p), OK);
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 10 * S, NULL), OK);
    struct process_info info;
    process_get_info(p, &info);
    kobject_unref(process_kobject(p));
    KT_EQ(info.killed, 0);
    KT_EQ(info.exit_code, 0);
    KT_ASSERT(view.checks >= 14);
    KT_EQ(pci_cfg_read(d, 0x04, 2), cmd);                   /* untouched */
    KT_EQ(pci_cfg_read(d, d->cap_msix + 2, 2), msix);
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        KT_EQ(job_used(j, k), 0);
    kobject_unref(pci);
    kobject_unref(rootres);
    job_unref(j);
    job_unref(root);
}
