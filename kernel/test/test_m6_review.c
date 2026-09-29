/* Repros from the independent review of M6 phase 1 (ktest=m6r).
 *
 *   m6r_kdrv_config_filter    the kernel build's drv_pci_config_write must
 *                             refuse what the pci_config_write syscall
 *                             refuses (a bridge, BIST, INTx Disable)
 *   m6r_kdrv_mmio_kernel_owned the kernel build's drv_mmio_map must refuse
 *                             kernel-owned MMIO (the I/O APIC) like
 *                             vmo_create_physical does
 *   m6r_pins_are_charged      every pin is a kernel allocation: it must be
 *                             charged, or a driver pins one page forever
 *                             and fills the kernel heap
 *   m6r_filter_other_resets   Advanced Features FLR and PM D3hot (a reset
 *                             on the way back to D0) are refused like PCIe
 *                             FLR
 *   m6r_filter_uses_known_caps the filter protects the MSI capability the
 *                             kernel found at boot even if the live list
 *                             no longer shows it */
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
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/vmo.h>

#define S  1000000000ull
#define PG PAGE_SIZE

static struct job *fresh_job(void)
{
    struct job *root, *j;
    KT_EQ(userboot_root_job(&root), OK);
    KT_EQ(job_create(root, &j), OK);
    job_unref(root);
    return j;
}

static void job_is_empty(struct job *j)
{
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        if (job_used(j, k))
            panic("ktest %s: job kind %u still has %lu units", ktest_current, k,
                  job_used(j, k));
}

static void run_driver(driver_main_fn fn, struct driver_kernel_handle *hs, unsigned n,
                       struct job *j)
{
    struct process *p;
    KT_EQ(driver_kernel_start("m6r", fn, hs, n, j, &p), OK);
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 30 * S, NULL), OK);
    struct process_info info;
    process_get_info(p, &info);
    KT_EQ(info.killed, 0);
    kobject_unref(process_kobject(p));
}

static struct kobject *pci_dev_res(struct pci_dev *d)
{
    struct kobject *root = resource_root(), *pci, *dev;
    KT_ASSERT(root);
    KT_EQ(resource_create(root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(resource_pci_device(pci, d->index, &dev), OK);
    kobject_unref(pci);
    kobject_unref(root);
    return dev;
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
    struct job *j = fresh_job();
    struct driver_kernel_handle hs[2] = {
        { ROLE_BRIDGE, khandle_from_new(pci_dev_res(hb), RES_RIGHTS) },
        { ROLE_EDU, khandle_from_new(pci_dev_res(edu), RES_RIGHTS) },
    };
    run_driver(cf_main, hs, 2, j);
    /* What the syscall layer answers for the same writes. */
    KT_EQ(cf_bridge, ERR_ACCESS_DENIED);
    KT_EQ(cf_bist, ERR_ACCESS_DENIED);
    KT_EQ(cf_intx, ERR_ACCESS_DENIED);
    job_is_empty(j);
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
    struct job *j = fresh_job();
    struct driver_kernel_handle h = { ROLE_EDU, khandle_from_new(res, RES_RIGHTS) };
    run_driver(mm_main, &h, 1, j);
    KT_EQ(mm_st, ERR_ACCESS_DENIED);
    job_is_empty(j);
    job_unref(j);
}

/* ---- pins are charged --------------------------------------------------------- */

KTEST(m6r_pins_are_charged)
{
    struct pci_dev *d = pci_find(0x1234, 0x11e8, 0);
    if (!d) {
        kprintf("ktest %s: no edu, skipped\n", ktest_current);
        return;
    }
    struct job *j = fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = j;
    handle_t dev, cap, vh;
    struct khandle kh = khandle_from_new(pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&t, &kh, &dev), OK);
    KT_EQ(sys_dma_cap_create(&t, dev, &cap), OK);
    KT_EQ(sys_vmo_create(&t, PG, 0, HANDLE_INVALID, &vh), OK);
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    uint64_t used = job_used(j, JOB_LIMIT_HANDLES);
    KT_EQ(job_set_limit(j, JOB_LIMIT_HANDLES, used + 64), OK);
    /* The same page, again and again: each pin is a kmalloc'd range. */
    unsigned ok = 0;
    for (unsigned i = 0; i < 1000; i++) {
        uint64_t pa, id;
        if (sys_vmo_pin(&t, vh, cap, 0, PG, &pa, &id) != OK)
            break;
        ok++;
    }
    kprintf("ktest %s: %u pins with 64 handle units to spare\n", ktest_current, ok);
    KT_ASSERT(ok <= 64);
    handle_table_destroy(&t);   /* closes the cap: BME off, every pin quarantined (M7) */
    KT_ASSERT(!(pci_cfg_read(d, 0x04, 2) & 0x04));
    struct dma_quarantine_stats q;
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, ok);
    dma_quarantine_flush(d);   /* ... still charged until released */
    job_is_empty(j);
    job_unref(j);
}

/* ---- the config filter: other resets, caps the live list hides ------------------ */

static uint8_t fake[4096];

static uint32_t fake_read(struct pci_dev *d, uint32_t off, uint32_t width)
{
    (void)d;
    uint32_t v = 0;
    memcpy(&v, &fake[off], width);
    return v;
}

static void put8(uint32_t off, uint8_t v) { fake[off] = v; }
static void put16(uint32_t off, uint16_t v) { memcpy(&fake[off], &v, 2); }

KTEST(m6r_filter_other_resets)
{
    static struct pci_dev d;
    memset(&d, 0, sizeof(d));
    memset(fake, 0, sizeof(fake));
    put16(0x06, 0x10);             /* capability list */
    put8(0x34, 0x40);
    put16(0x40, 0x5001);           /* PM (01) at 0x40 -> 0x50 */
    put16(0x50, 0x0013);           /* Advanced Features (13) at 0x50, end */
    /* PMCSR (0x44): D0 -> D3hot and back resets a function without
     * No_Soft_Reset, like FLR. */
    KT_EQ(pci_cfg_write_allowed(&d, 0x44, 2, 3, fake_read), ERR_ACCESS_DENIED);
    KT_EQ(pci_cfg_write_allowed(&d, 0x44, 1, 3, fake_read), ERR_ACCESS_DENIED);
    KT_EQ(pci_cfg_write_allowed(&d, 0x44, 2, 0x8000, fake_read), OK);   /* PME status W1C */
    /* AF Control (0x54) bit 0: Initiate FLR. */
    KT_EQ(pci_cfg_write_allowed(&d, 0x54, 1, 1, fake_read), ERR_ACCESS_DENIED);
    KT_EQ(pci_cfg_write_allowed(&d, 0x54, 1, 0, fake_read), OK);
}

KTEST(m6r_filter_uses_known_caps)
{
    static struct pci_dev d;
    memset(&d, 0, sizeof(d));
    memset(fake, 0, sizeof(fake));
    /* The kernel found MSI at 0x60 at boot; the live list now says there
     * are no capabilities (a device whose list a vendor register can hide). */
    d.cap_msi = 0x60;
    d.cap_msix = 0x70;
    put16(0x60, 0x0005);
    put16(0x70, 0x0011);
    KT_EQ(pci_cfg_write_allowed(&d, 0x64, 4, 0xfee00000u, fake_read), ERR_ACCESS_DENIED);
    KT_EQ(pci_cfg_write_allowed(&d, 0x72, 2, 0x8000, fake_read), ERR_ACCESS_DENIED);
}

/* ---- unpin needs nothing but a writable VMO handle ------------------------------- */

/* A client hands its buffer VMO (RIGHT_WRITE, as a block or network client
 * would) to a driver, which pins it for DMA. The client, not the driver,
 * then unpins the driver's pin (ids count up from 1 per VMO) and
 * decommits the page: it goes back to the page allocator while the device
 * still has it as a DMA target and Bus Master Enable is on. */
KTEST(m6r_unpin_by_other_holder)
{
    struct pci_dev *d = pci_find(0x1234, 0x11e8, 0);
    if (!d) {
        kprintf("ktest %s: no edu, skipped\n", ktest_current);
        return;
    }
    struct job *j = fresh_job();
    struct handle_table td, tc;
    handle_table_init(&td);
    handle_table_init(&tc);
    td.job = tc.job = j;
    handle_t dev, cap, vc, vd;
    struct khandle kh = khandle_from_new(pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&td, &kh, &dev), OK);
    KT_EQ(sys_dma_cap_create(&td, dev, &cap), OK);
    KT_EQ(sys_dma_cap_bus_master(&td, cap, 1), OK);
    KT_EQ(sys_vmo_create(&tc, PG, 0, HANDLE_INVALID, &vc), OK);
    struct kobject *vo;
    KT_EQ(handle_get(&tc, vc, OBJ_VMO, 0, &vo, NULL), OK);
    struct khandle kv = khandle_from_new(vo, RIGHTS_BASIC | RIGHTS_IO | RIGHT_MAP);
    KT_EQ(handle_insert(&td, &kv, &vd), OK);   /* "sent" to the driver */
    uint64_t pa, id;
    KT_EQ(sys_vmo_pin(&td, vd, cap, 0, PG, &pa, &id), OK);
    /* The client, with a guessed id: without a cap, and with a cap of its
     * own (bound to the same function, so it's a valid one). */
    handle_t ccap, dev2;
    struct khandle kd = khandle_from_new(pci_dev_res(d), RES_RIGHTS);
    KT_EQ(handle_insert(&tc, &kd, &dev2), OK);
    KT_EQ(sys_dma_cap_create(&tc, dev2, &ccap), OK);
    status_t un0 = sys_vmo_unpin(&tc, vc, HANDLE_INVALID, id);
    status_t un = sys_vmo_unpin(&tc, vc, ccap, id);
    status_t dc = sys_vmo_decommit(&tc, vc, 0, PG);
    kprintf("ktest %s: client unpin of the driver's pin %lu: %s / %s, then decommit: %s "
            "(device still has 0x%lx, BME %s)\n", ktest_current, id, status_str(un0),
            status_str(un), status_str(dc), pa, (pci_cfg_read(d, 0x04, 2) & 0x04) ? "on" : "off");
    handle_table_destroy(&tc);
    handle_table_destroy(&td);   /* the driver's cap: its pin is quarantined (M7) */
    dma_quarantine_flush(d);
    job_is_empty(j);
    job_unref(j);
    /* Only the pin's DMA capability may undo it (fixed in M6 phase 2:
     * vmo_unpin takes the dma_cap), and the page stays pinned. */
    KT_EQ(un0, ERR_BAD_HANDLE);
    KT_EQ(un, ERR_ACCESS_DENIED);
    KT_EQ(dc, ERR_BAD_STATE);
}
