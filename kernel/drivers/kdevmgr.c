/* Drivers bound by the kernel (M6 phase 2): devmgr's job done in kernel
 * mode, for the `drivers=kernel` boot word and the ktests. The model and
 * the handles a driver gets are in <jam/kdevmgr.h>; user/devmgr makes the
 * same handles with system calls.
 *
 * Everything here uses the object layer directly (resource_create,
 * resource_pci_device, resource_pci_bar, interrupt_create_msi,
 * dma_cap_create_for) with the kernel as the authority: no handle table
 * of its own, the objects charged to nobody until they land in the
 * driver's table. The driver runs as a kernel process
 * (driver_kernel_start) in a job of its own. */
#include <jam/channel.h>
#include <jam/driver.h>
#include <jam/driver_kernel.h>
#include <jam/interrupt.h>
#include <jam/kdevmgr.h>
#include <jam/kprintf.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/report.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <check/edu_check.h>

#define S              1000000000ull
#define CH_RIGHTS      (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_SIGNAL)
#define DRV_DEV_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)
#define DRV_BAR_RIGHTS (RIGHTS_BASIC | RIGHT_MAP)
#define DRV_IRQ_RIGHTS (RIGHTS_BASIC | RIGHTS_IO)
#define DRV_DMA_RIGHTS RIGHTS_BASIC
#define EDU_CHECK_ROLE 0x40   /* the check's role for its channel to the edu server */

/* Per-driver job limits (a driver that runs away hits these, not the
 * kernel): 16 MiB, 256 handles, 16 threads, 1 MiB of messages. */
static const struct { uint32_t kind; uint64_t value; } limits[] = {
    { JOB_LIMIT_PAGES, 4096 },
    { JOB_LIMIT_HANDLES, 256 },
    { JOB_LIMIT_THREADS, 16 },
    { JOB_LIMIT_MSG_BYTES, 1u << 20 },
};

const struct kdev_match kdev_matches[] = {
    { 0x1234, 0x11e8, KDEV_ANY_CLASS, "edu" },         /* QEMU's edu test device */
    { 0xffff, 0xffff, 0x0c0330, "xhci-noop" },         /* any xHCI (skipped until built in) */
    { 0, 0, 0, NULL },
};

static bool matches(const struct kdev_match *m, const struct pci_dev *d)
{
    const struct pci_dev_info *i = &d->info;
    uint32_t cls = (uint32_t)i->class_code << 16 | (uint32_t)i->subclass << 8 | i->prog_if;
    return (m->vendor == 0xffff || m->vendor == i->vendor) &&
           (m->device == 0xffff || m->device == i->device) &&
           (m->class_code == KDEV_ANY_CLASS || m->class_code == cls);
}

const char *kdev_match_driver(const struct pci_dev *d)
{
    if (d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY))
        return NULL;   /* never ours to drive */
    for (const struct kdev_match *m = kdev_matches; m->driver; m++)
        if (matches(m, d))
            return m->driver;
    return NULL;
}

/* ---- binding -------------------------------------------------------------------- */

static status_t add(struct driver_kernel_handle *hs, unsigned *n, uint32_t role,
                    struct kobject *obj, rights_t rights)
{
    if (*n == DRV_MAX_HANDLES) {
        kobject_unref(obj);
        return ERR_NO_RESOURCES;
    }
    hs[*n].role = role;
    hs[*n].kh = khandle_from_new(obj, rights);
    (*n)++;
    return OK;
}

status_t kdev_bind(struct pci_dev *d, const char *driver, struct job *parent,
                   struct kdev_binding *out)
{
    memset(out, 0, sizeof(*out));
    driver_main_fn fn = driver_kernel_find(driver);
    if (!fn)
        return ERR_NOT_FOUND;
    out->dev = d;
    size_t len = strlen(driver);
    if (len >= sizeof(out->driver))
        len = sizeof(out->driver) - 1;
    memcpy(out->driver, driver, len);

    struct driver_kernel_handle hs[DRV_MAX_HANDLES];
    unsigned n = 0;
    struct kobject *root = resource_root(), *pci = NULL, *dev = NULL, *obj;
    struct job *job = NULL;
    status_t st = root ? resource_create(root, RES_PCI, 0, 0, &pci) : ERR_BAD_STATE;
    if (st == OK)
        st = resource_pci_device(pci, d->index, &dev);
    /* Each memory BAR (a 64-bit one fills two slots; I/O BARs are never a
     * driver's). Memory decode on, as pci_bar_resource does. */
    for (uint32_t b = 0; st == OK && b < 6; b++) {
        uint32_t f = d->info.bar[b].flags;
        if (!d->info.bar[b].size || !(f & PCI_BAR_MMIO) || (f & PCI_BAR_UNSIZED))
            continue;
        st = resource_pci_bar(dev, b, &obj);
        if (st == OK) {
            uint64_t cf = pci_cmd_lock();
            (void)pci_enable_memory(d);
            pci_cmd_unlock(cf);
            st = add(hs, &n, DR_BAR(b), obj, DRV_BAR_RIGHTS);
        }
    }
    if (st == OK && (d->cap_msix || d->cap_msi)) {
        st = interrupt_create_msi(d, 0, d->cap_msix ? IRQ_MSIX : 0, &obj);
        if (st == OK) {
            kobject_ref(obj);
            out->irq = obj;
            st = add(hs, &n, DR_IRQ(0), obj, DRV_IRQ_RIGHTS);
        }
    }
    if (st == OK)
        st = dma_cap_create_for(d, &obj);
    if (st == OK) {
        kobject_ref(obj);
        out->dma_cap = obj;
        st = add(hs, &n, DR_DMA, obj, DRV_DMA_RIGHTS);
    }
    if (st == OK) {
        st = add(hs, &n, DR_PCIDEV, dev, DRV_DEV_RIGHTS);
        dev = NULL;
    }
    struct channel *a, *b;
    if (st == OK && (st = channel_create(&a, &b)) == OK) {
        out->client = khandle_from_new((struct kobject *)a, CH_RIGHTS);
        st = add(hs, &n, DR_SERVE, (struct kobject *)b, CH_RIGHTS);
    }
    if (st == OK && (st = job_create(parent, &job)) == OK)
        for (unsigned i = 0; st == OK && i < sizeof(limits) / sizeof(limits[0]); i++)
            st = job_set_limit(job, limits[i].kind, limits[i].value);
    if (st == OK) {
        st = driver_kernel_start(out->driver, fn, hs, n, job, &out->proc);   /* consumes hs */
        n = 0;
    }
    for (unsigned i = 0; i < n; i++)
        khandle_release(&hs[i].kh);
    if (dev)
        kobject_unref(dev);
    if (pci)
        kobject_unref(pci);
    if (root)
        kobject_unref(root);
    if (st != OK) {
        khandle_release(&out->client);
        if (out->irq)
            kobject_unref(out->irq);
        if (out->dma_cap)
            kobject_unref(out->dma_cap);
        job_unref(job);
        memset(out, 0, sizeof(*out));
        return st;
    }
    out->job = job;
    return OK;
}

static bool job_clean(struct job *j)
{
    bool clean = true;
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        if (job_used(j, k)) {
            kprintf("kdevmgr: the driver's job still has %lu units of kind %u\n",
                    job_used(j, k), k);
            clean = false;
        }
    return clean;
}

bool kdev_unbind(struct kdev_binding *b, uint64_t timeout_ns)
{
    if (!b->proc)
        return false;
    khandle_release(&b->client);   /* a serving driver sees its client gone and returns */
    bool ok = true;
    if (object_wait_one(process_kobject(b->proc), SIG_TERMINATED, uptime_ns() + timeout_ns,
                        NULL) != OK) {
        kprintf("kdevmgr: %s did not stop by itself: killing its job\n", b->driver);
        job_kill(b->job, NULL);
        ok = false;
    }
    struct process_info info;
    process_get_info(b->proc, &info);
    ok &= info.state == PROCESS_DEAD && !info.killed && info.exit_code == 0;
    ok &= job_clean(b->job);
    kobject_unref(process_kobject(b->proc));
    if (b->irq)
        kobject_unref(b->irq);
    if (b->dma_cap)
        kobject_unref(b->dma_cap);
    job_unref(b->job);
    memset(b, 0, sizeof(*b));
    return ok;
}

/* ---- the edu check ------------------------------------------------------------------ */

static int edu_check_main(const struct driver_start *s)
{
    struct edu_check_result r;
    status_t st = edu_check(drv_handle(s, EDU_CHECK_ROLE), drv_clock_ns() + 60 * S, &r);
    if (st != OK) {
        drv_report("FAILED after %u checks (%s)", r.checks, status_str(st));
        return 1;
    }
    drv_report("factorial(10)=%u ok, DMA 4 KiB round trip ok in %lu us, MSI -> driver in %lu us",
               r.fact10, (unsigned long)(r.dma_ns / 1000), (unsigned long)(r.msi_ns / 1000));
    return 0;
}

status_t kdev_edu_check(struct kdev_binding *b, const char *name)
{
    if (!b->client.obj)
        return ERR_BAD_STATE;
    /* A second handle to our end: the check closing it (when it ends)
     * doesn't close the channel for the driver. */
    kobject_ref(b->client.obj);
    struct driver_kernel_handle h = { EDU_CHECK_ROLE, khandle_from_new(b->client.obj, CH_RIGHTS) };
    struct process *p;
    status_t st = driver_kernel_start(name, edu_check_main, &h, 1, b->job, &p);
    if (st != OK)
        return st;
    st = object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 120 * S, NULL);
    if (st != OK)
        process_kill(p, PROCESS_KILLED_CODE, true);
    struct process_info info;
    process_get_info(p, &info);
    kobject_unref(process_kobject(p));
    return st != OK ? st : info.killed || info.exit_code ? ERR_INTERNAL : OK;
}

/* ---- the boot word ---------------------------------------------------------------- */

bool kdev_run_kernel_mode(void)
{
    struct job *root;
    if (userboot_root_job(&root) != OK) {
        report("drivers=kernel: no job for the drivers");
        return false;
    }
    bool ok = true;
    unsigned bound = 0, skipped = 0;
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        const char *drv = d ? kdev_match_driver(d) : NULL;
        if (!drv)
            continue;
        const struct pci_dev_info *in = &d->info;
        if (!driver_kernel_find(drv)) {
            kprintf("drivers=kernel: %02x:%02x.%x %04x:%04x: driver %s not built in, skipped\n",
                    in->bus, in->dev, in->fn, in->vendor, in->device, drv);
            skipped++;
            continue;
        }
        struct kdev_binding b;
        status_t st = kdev_bind(d, drv, root, &b);
        if (st != OK) {
            report("drivers=kernel: %02x:%02x.%x %04x:%04x -> %s: bind failed (%s)", in->bus,
                   in->dev, in->fn, in->vendor, in->device, drv, status_str(st));
            ok = false;
            continue;
        }
        bound++;
        kprintf("drivers=kernel: %02x:%02x.%x %04x:%04x -> %s (kernel process)\n", in->bus,
                in->dev, in->fn, in->vendor, in->device, drv);
        if (!strcmp(drv, "edu") && (st = kdev_edu_check(&b, "edu (kernel)")) != OK) {
            report("drivers=kernel: the edu check failed (%s)", status_str(st));
            ok = false;
        }
        if (!kdev_unbind(&b, 30 * S)) {   /* xhci-noop runs to its end: a few s */
            report("drivers=kernel: %s did not end cleanly", drv);
            ok = false;
        }
    }
    report("drivers=kernel: %u driver(s) bound as kernel processes and stopped%s, %u skipped "
           "(not built in)", bound, ok ? "" : " WITH PROBLEMS", skipped);
    job_unref(root);
    return ok;
}
