/* Repros from the independent review of M6 phase 1 (ktest=m6r).
 *
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
    /* M7 (review finding 7): devmgr (RIGHT_MANAGE) may change the power
     * state, to wake a function left in D3; still no FLR for anyone. */
    KT_EQ(pci_cfg_write_allowed_as(&d, 0x44, 2, 3, fake_read, true), OK);
    KT_ASSERT(pci_cfg_write_changes_power(&d, 0x44, 2, 3, fake_read));
    KT_ASSERT(pci_cfg_write_changes_power(&d, 0x44, 1, 3, fake_read));
    KT_ASSERT(pci_cfg_write_changes_power(&d, 0x44, 4, 3, fake_read));
    KT_ASSERT(!pci_cfg_write_changes_power(&d, 0x44, 2, 0x8000, fake_read));
    KT_ASSERT(!pci_cfg_write_changes_power(&d, 0x45, 1, 3, fake_read));
    KT_ASSERT(!pci_cfg_write_changes_power(&d, 0x40, 4, 0x03000000, fake_read));   /* PMC, not PMCSR */
    KT_EQ(pci_cfg_write_allowed_as(&d, 0x54, 1, 1, fake_read, true), ERR_ACCESS_DENIED);
    put16(0x44, 3);   /* in D3hot now: back to D0 */
    KT_EQ(pci_cfg_write_allowed(&d, 0x44, 2, 0, fake_read), ERR_ACCESS_DENIED);
    KT_EQ(pci_cfg_write_allowed_as(&d, 0x44, 2, 0, fake_read, true), OK);
    KT_ASSERT(pci_cfg_write_changes_power(&d, 0x44, 2, 0, fake_read));
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
