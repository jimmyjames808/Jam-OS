/* Resources: the authority over hardware, as handles (see
 * <jam/resource.h>).
 *
 * A resource is immutable after creation: its kind, its physical range
 * (RES_ROOT, RES_MMIO) or its PCI function (RES_PCI_DEV). So it needs no
 * lock. The tree is only a creation-time rule (a slice must lie inside its
 * parent); a slice doesn't keep its parent alive and closing a parent
 * revokes nothing.
 *
 *   RES_ROOT     [0, 2^52): every physical address. One, made at boot.
 *                Userboot hands init a handle to it (SR_RESOURCE).
 *   RES_MMIO     a page-aligned range inside a ROOT or MMIO parent that
 *                touches no RAM (the boot memory map's RAM types), checked
 *                when it is made. vmo_create_physical maps from it.
 *   RES_PCI      all PCI functions (pci_enum, pci_device_open); from ROOT.
 *   RES_PCI_DEV  one function, from RES_PCI (pci_device_open). Its BARs
 *                become RES_MMIO with pci_bar_resource.
 *
 * Mapping is refused (resource_phys_mappable) for RAM, for any page that
 * holds an MSI-X table or PBA, and for MMIO the kernel itself drives: the
 * local APIC / MSI window, the I/O APICs, the HPET and the ECAM windows.
 * Holding the root doesn't change that.
 *
 * Config space (pci_config_read/write on a RES_PCI_DEV): reads are free;
 * writes go through pci_cfg_write_allowed below. The command register's
 * decode, bus-master and INTx-disable bits are the kernel's; the check and
 * the write happen under `cmd_lock`, which pci_bus_master and the dma_cap
 * close path also take, so a driver's stale read-modify-write can never
 * turn Bus Master Enable back on after the kernel cleared it.
 *
 * Job charges: every resource made through a system call costs its
 * creator's job one JOB_LIMIT_HANDLES unit until it is destroyed. */
#include <jam/acpi.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/spinlock.h>

struct resource {
    struct kobject  base;          /* OBJ_RESOURCE */
    uint32_t        kind;          /* RES_* */
    uint64_t        start, size;   /* ROOT / MMIO: the physical range */
    struct pci_dev *dev;           /* PCI_DEV */
    struct job     *job;           /* charged one handle unit, or NULL */
    bool            counted;       /* PCI_DEV made by a system call: in dev->proc_users */
};

static struct kobject *root;
static spinlock_t cmd_lock = SPINLOCK_INIT("pci cmd filter");

static struct resource *res_of(struct kobject *obj)
{
    return obj && obj->type == OBJ_RESOURCE ? (struct resource *)obj : NULL;
}

static void resource_destroy(struct kobject *obj)
{
    struct resource *r = (struct resource *)obj;
    if (r->counted)
        __atomic_sub_fetch(&r->dev->proc_users, 1, __ATOMIC_RELAXED);
    job_uncharge(r->job, JOB_LIMIT_HANDLES, 1);
    job_unref(r->job);
    kfree(r);
}

static const struct kobject_ops resource_ops = {
    .name = "resource",
    .destroy = resource_destroy,
};

static status_t res_new(uint32_t kind, uint64_t start, uint64_t size, struct pci_dev *dev,
                        struct kobject **out)
{
    struct resource *r = kzalloc(sizeof(*r));
    if (!r)
        return ERR_NO_MEMORY;
    kobject_init(&r->base, OBJ_RESOURCE, &resource_ops, "resource", 0);
    r->kind = kind;
    r->start = start;
    r->size = size;
    r->dev = dev;
    *out = &r->base;
    return OK;
}

void resource_init(void)
{
    if (root)
        return;
    if (res_new(RES_ROOT, 0, RES_PHYS_LIMIT, NULL, &root) != OK)
        panic("resource: no memory for the root");
    kprintf("resource: root [0, 2^52), %u PCI function%s\n", pci_count(),
            pci_count() == 1 ? "" : "s");
}

struct kobject *resource_root(void)
{
    if (root)
        kobject_ref(root);
    return root;
}

uint32_t resource_kind(struct kobject *res)
{
    struct resource *r = res_of(res);
    return r ? r->kind : 0;
}

struct pci_dev *resource_pci_dev(struct kobject *res)
{
    struct resource *r = res_of(res);
    return r && r->kind == RES_PCI_DEV ? r->dev : NULL;
}

status_t resource_set_job(struct kobject *obj, struct job *job)
{
    struct resource *r = res_of(obj);
    if (!r || r->job)
        return ERR_BAD_STATE;
    status_t st = job_charge(job, JOB_LIMIT_HANDLES, 1);
    if (st == OK) {
        job_ref(job);
        r->job = job;
        /* A function a process (devmgr) holds is in use by a driver. */
        if (r->kind == RES_PCI_DEV && r->dev && !r->counted) {
            r->counted = true;
            __atomic_add_fetch(&r->dev->proc_users, 1, __ATOMIC_RELAXED);
        }
    }
    return st;
}

status_t resource_range(struct kobject *res, uint64_t *base, uint64_t *size)
{
    struct resource *r = res_of(res);
    if (!r)
        return ERR_WRONG_TYPE;
    if (r->kind != RES_ROOT && r->kind != RES_MMIO)
        return ERR_WRONG_TYPE;
    *base = r->start;
    *size = r->size;
    return OK;
}

/* ---- physical ranges -------------------------------------------------------- */

static bool overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

/* MMIO the kernel drives itself: never mapped for anyone else. */
static bool kernel_owned(uint64_t phys, uint64_t len)
{
    /* The local APIC and the whole MSI address window. */
    if (overlaps(phys, len, 0xfee00000ull, 0x100000ull))
        return true;
    if (acpi.lapic_phys && overlaps(phys, len, ALIGN_DOWN(acpi.lapic_phys, PAGE_SIZE), PAGE_SIZE))
        return true;
    for (uint32_t i = 0; i < acpi.ioapic_count && i < ACPI_MAX_IOAPICS; i++)
        if (overlaps(phys, len, ALIGN_DOWN(acpi.ioapics[i].phys, PAGE_SIZE), PAGE_SIZE))
            return true;
    if (acpi.hpet_phys && overlaps(phys, len, ALIGN_DOWN(acpi.hpet_phys, PAGE_SIZE), PAGE_SIZE))
        return true;
    /* ECAM: from the base (bus 0) up to the last bus, whichever way the
     * firmware meant the base. */
    for (uint32_t i = 0; i < acpi.ecam_count && i < ACPI_MAX_ECAM; i++)
        if (overlaps(phys, len, acpi.ecam[i].phys, ((uint64_t)acpi.ecam[i].bus_end + 1) << 20))
            return true;
    return false;
}

status_t resource_phys_mappable(uint64_t phys, uint64_t len)
{
    if (len == 0 || phys >= RES_PHYS_LIMIT || len > RES_PHYS_LIMIT - phys)
        return ERR_OUT_OF_RANGE;
    const char *why = pmm_range_has_ram(phys, len) ? "RAM"
                    : kernel_owned(phys, len)       ? "MMIO the kernel drives"
                    : pci_phys_protected(phys, len) ? "an MSI-X table/PBA page"
                                                    : NULL;
    if (why) {
        kprintf("resource: mapping [%lx, +%lx) refused: %s\n", phys, len, why);
        return ERR_ACCESS_DENIED;
    }
    return OK;
}

status_t resource_check_mmio(struct kobject *res, uint64_t phys, uint64_t len)
{
    struct resource *r = res_of(res);
    if (!r || (r->kind != RES_ROOT && r->kind != RES_MMIO))
        return ERR_WRONG_TYPE;
    if (len == 0 || phys < r->start || phys - r->start > r->size ||
        len > r->size - (phys - r->start))
        return ERR_OUT_OF_RANGE;
    /* The root is everything minus RAM; an MMIO slice was checked already,
     * but RAM is cheap to check again. */
    if (pmm_range_has_ram(phys, len))
        return ERR_ACCESS_DENIED;
    return OK;
}

/* ---- slicing ------------------------------------------------------------------ */

status_t resource_create(struct kobject *parent, uint32_t kind, uint64_t base, uint64_t size,
                         struct kobject **out)
{
    struct resource *p = res_of(parent);
    if (!p)
        return ERR_WRONG_TYPE;
    switch (kind) {
    case RES_MMIO:
        if (p->kind != RES_ROOT && p->kind != RES_MMIO)
            return ERR_WRONG_TYPE;
        if (size == 0 || ((base | size) & (PAGE_SIZE - 1)))
            return ERR_INVALID_ARGS;
        if (base < p->start || base - p->start > p->size || size > p->size - (base - p->start))
            return ERR_OUT_OF_RANGE;
        if (pmm_range_has_ram(base, size))
            return ERR_ACCESS_DENIED;
        return res_new(RES_MMIO, base, size, NULL, out);
    case RES_PCI:
        if (p->kind != RES_ROOT)
            return ERR_WRONG_TYPE;
        if (base || size)
            return ERR_INVALID_ARGS;
        return res_new(RES_PCI, 0, 0, NULL, out);
    default:
        /* RES_ROOT is made once at boot; RES_PCI_DEV by pci_device_open. */
        return ERR_INVALID_ARGS;
    }
}

status_t resource_pci_device(struct kobject *pci, uint32_t index, struct kobject **out)
{
    struct resource *p = res_of(pci);
    if (!p || p->kind != RES_PCI)
        return ERR_WRONG_TYPE;
    struct pci_dev *d = index < pci_count() ? pci_get(index) : NULL;
    if (!d)
        return ERR_OUT_OF_RANGE;
    return res_new(RES_PCI_DEV, 0, 0, d, out);
}

status_t resource_pci_bar(struct kobject *dev, uint32_t bar, struct kobject **out)
{
    struct resource *p = res_of(dev);
    if (!p || p->kind != RES_PCI_DEV)
        return ERR_WRONG_TYPE;
    if (bar >= 6)
        return ERR_OUT_OF_RANGE;
    const struct pci_dev_info *info = &p->dev->info;
    uint32_t flags = info->bar[bar].flags;
    if (flags & (PCI_BAR_IO | PCI_BAR_UNSIZED))
        return ERR_NOT_SUPPORTED;   /* port I/O never; unsized: size unknown */
    if (!(flags & PCI_BAR_MMIO) || info->bar[bar].size == 0)
        return ERR_NOT_FOUND;       /* not implemented (or a 64-bit BAR's upper half) */
    uint64_t phys = info->bar[bar].phys, end = phys + info->bar[bar].size;
    if (end < phys || end > RES_PHYS_LIMIT)
        return ERR_OUT_OF_RANGE;
    uint64_t lo = ALIGN_DOWN(phys, PAGE_SIZE), hi = ALIGN_UP(end, PAGE_SIZE);
    const struct pci_dev_info *me = info;
    if (pmm_range_has_ram(lo, hi - lo)) {
        kprintf("resource: %02x:%02x.%u BAR %u [%lx, %lx) refused: overlaps RAM\n", me->bus,
                me->dev, me->fn, bar, lo, hi);
        return ERR_ACCESS_DENIED;
    }
    /* Refuse it if its pages -- the BAR itself, and for a BAR smaller than
     * a page the slack it is rounded out with -- reach another function's
     * registers: a stale, unassigned or overlapping BAR value would
     * otherwise hand out a RES_MMIO over that function (and pci_bar_resource
     * then turns this one's decode on). Only functions with memory decode
     * on claim addresses: a disabled one can hold a stale or unassigned BAR
     * value (the PC's first run refused the xHCI's 64 KiB BAR because of
     * one). The whole BAR is checked, not only a sub-page BAR's slack: a
     * page-aligned BAR can overlap too (test: m6p2_bar_overlaps_live_function). */
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *o = pci_get(i);
        if (!o || o == p->dev || !(pci_cfg_read(o, 0x04, 2) & 0x2))
            continue;
        for (unsigned b = 0; b < 6; b++) {
            const typeof(o->info.bar[0]) *ob = &o->info.bar[b];
            if (!(ob->flags & PCI_BAR_MMIO))
                continue;
            /* An unsized BAR (display, bridges) covers at least its page. */
            uint64_t olen = ob->size ? ob->size : PAGE_SIZE;
            if (overlaps(lo, hi - lo, ob->phys, olen)) {
                kprintf("resource: %02x:%02x.%u BAR %u [%lx, %lx) refused: it reaches "
                        "%02x:%02x.%u BAR %u [%lx, +%lx)\n", me->bus, me->dev, me->fn, bar,
                        lo, hi, o->info.bus, o->info.dev, o->info.fn, b, ob->phys, olen);
                return ERR_ACCESS_DENIED;
            }
        }
    }
    return res_new(RES_MMIO, lo, hi - lo, NULL, out);
}

/* ---- config space filter ------------------------------------------------------- */

#define CFG_STATUS        0x06
#define CFG_CAP_PTR       0x34
#define STATUS_CAP_LIST   0x10
#define CAP_ID_PM         0x01
#define CAP_ID_MSI        0x05
#define CAP_ID_AF         0x13
#define CAP_ID_PCIE       0x10
#define CAP_ID_MSIX       0x11
#define EXT_ID_SRIOV      0x10
#define EXT_ID_REBAR      0x15
#define EXT_ID_VF_REBAR   0x24

status_t pci_cfg_access_ok(uint32_t off, uint32_t width)
{
    if ((width != 1 && width != 2 && width != 4) || (off & (width - 1)) || off >= 4096)
        return ERR_INVALID_ARGS;
    return OK;
}

/* Standard capability list, bounded (a looping list ends the walk). */
static uint32_t std_cap(struct pci_dev *d, pci_cfg_reader_t rd, uint32_t id)
{
    if (!(rd(d, CFG_STATUS, 2) & STATUS_CAP_LIST))
        return 0;
    uint32_t off = rd(d, CFG_CAP_PTR, 1) & 0xfc;
    for (unsigned i = 0; i < 48 && off >= 0x40 && off < 0x100; i++) {
        uint32_t hdr = rd(d, off, 2);
        if ((hdr & 0xff) == id)
            return off;
        off = (hdr >> 8) & 0xfc;
    }
    return 0;
}

/* Extended capability list (PCIe, 0x100 up), bounded the same way. */
static uint32_t ext_cap(struct pci_dev *d, pci_cfg_reader_t rd, uint32_t id)
{
    uint32_t off = 0x100;
    for (unsigned i = 0; i < 960 && off >= 0x100 && off < 4096; i++) {
        uint32_t hdr = rd(d, off, 4);
        if (hdr == 0 || hdr == 0xffffffffu)
            return 0;
        if ((hdr & 0xffff) == id)
            return off;
        off = (hdr >> 20) & 0xffc;
    }
    return 0;
}

struct cfg_hole {
    uint32_t start, end;   /* bytes [start, end) a driver may not write */
};

/* The write filter. Refused (ERR_ACCESS_DENIED):
 *   - anything at all for a bridge (header type 1/2) or the boot display;
 *   - BIST (0x0f), the BARs (0x10-0x27), the expansion ROM BAR (0x30-0x33);
 *   - a command-register write that changes I/O decode (bit 0), memory
 *     decode (1), Bus Master Enable (2) or INTx Disable (10): those are
 *     the kernel's (pci_bus_master, pci_enable_memory, MSI setup). The
 *     other command bits (MWI, parity/SERR# reporting, ...) and the status
 *     register's write-1-to-clear bits are the driver's;
 *   - any byte inside the MSI capability (its size from Message Control)
 *     or the 12-byte MSI-X capability, both where the live list shows them
 *     and where the kernel found them at boot;
 *   - PCIe Device Control with Initiate Function Level Reset (bit 15) set,
 *     Advanced Features Control with Initiate FLR (bit 0) set, and a PM
 *     PowerState change (D3hot -> D0 resets the function): a reset would
 *     undo what the kernel set up behind its back. A RIGHT_MANAGE
 *     holder (devmgr, `manage`) may change the PowerState: it wakes a
 *     function left in D1-D3 before binding a driver, and
 *     sys_pci_config_write waits out the transition (10 ms) and puts back
 *     the BARs and command register a reset lost;
 *   - the SR-IOV capability (new functions with their own BARs) and the
 *     Resizable BAR / VF Resizable BAR capabilities (BAR sizes).
 * Everything else a function's own driver may write (cache line size,
 * latency timer, interrupt line, device-specific registers, the rest of
 * the PCIe capabilities). */
status_t pci_cfg_write_allowed(struct pci_dev *d, uint32_t off, uint32_t width, uint32_t value,
                               pci_cfg_reader_t rd)
{
    return pci_cfg_write_allowed_as(d, off, width, value, rd, false);
}

bool pci_cfg_write_changes_power(struct pci_dev *d, uint32_t off, uint32_t width, uint32_t value,
                                 pci_cfg_reader_t rd)
{
    uint32_t pm = std_cap(d, rd, CAP_ID_PM);
    if (!pm || pm + 4 < off || pm + 4 >= off + width)
        return false;
    uint8_t vb = (uint8_t)(value >> (8 * (pm + 4 - off)));
    return ((vb ^ rd(d, pm + 4, 1)) & 0x03) != 0;
}

status_t pci_cfg_write_allowed_as(struct pci_dev *d, uint32_t off, uint32_t width, uint32_t value,
                                  pci_cfg_reader_t rd, bool manage)
{
    if (pci_cfg_access_ok(off, width) != OK)
        return ERR_INVALID_ARGS;
    if ((d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY)) || (d->info.header_type & 0x7f))
        return ERR_ACCESS_DENIED;

    struct cfg_hole holes[12];
    unsigned n = 0;
    holes[n++] = (struct cfg_hole){ 0x0f, 0x28 };   /* BIST (0x0f) through the BARs */
    holes[n++] = (struct cfg_hole){ 0x30, 0x34 };   /* expansion ROM */
    uint32_t msi = std_cap(d, rd, CAP_ID_MSI);
    if (msi) {
        uint32_t ctrl = rd(d, msi + 2, 2);
        uint32_t size = 0x0a + ((ctrl & 0x80) ? 4 : 0) + ((ctrl & 0x100) ? 10 : 0);
        holes[n++] = (struct cfg_hole){ msi, msi + size };
    }
    uint32_t msix = std_cap(d, rd, CAP_ID_MSIX);
    if (msix)
        holes[n++] = (struct cfg_hole){ msix, msix + 12 };
    /* The capabilities the kernel found at boot (and programs) as well: the
     * live walk trusts pointers a device might let a vendor register
     * change. The largest MSI layout; MSI-X as above. */
    if (d->cap_msi && d->cap_msi != msi)
        holes[n++] = (struct cfg_hole){ d->cap_msi, d->cap_msi + 0x18 };
    if (d->cap_msix && d->cap_msix != msix)
        holes[n++] = (struct cfg_hole){ d->cap_msix, d->cap_msix + 12 };
    uint32_t sriov = ext_cap(d, rd, EXT_ID_SRIOV);
    if (sriov)
        holes[n++] = (struct cfg_hole){ sriov, sriov + 0x40 };
    uint32_t rebar_ids[2] = { EXT_ID_REBAR, EXT_ID_VF_REBAR };
    for (unsigned k = 0; k < 2; k++) {
        uint32_t rb = ext_cap(d, rd, rebar_ids[k]);
        if (rb) {
            uint32_t nbars = (rd(d, rb + 8, 4) >> 5) & 7;
            if (nbars == 0 || nbars > 6)
                nbars = 6;
            holes[n++] = (struct cfg_hole){ rb, rb + 4 + 8 * nbars };
        }
    }
    uint32_t pcie = std_cap(d, rd, CAP_ID_PCIE);
    /* Other ways to reset the function: a PM power-state change (D3hot and
     * back to D0 resets it unless No_Soft_Reset) and Advanced Features'
     * Initiate FLR. */
    uint32_t pm = std_cap(d, rd, CAP_ID_PM);
    uint32_t af = std_cap(d, rd, CAP_ID_AF);

    for (uint32_t i = 0; i < width; i++) {
        uint32_t b = off + i;
        uint8_t vb = (uint8_t)(value >> (8 * i));
        for (unsigned h = 0; h < n; h++)
            if (b >= holes[h].start && b < holes[h].end)
                return ERR_ACCESS_DENIED;
        if (b == 0x04 && ((vb ^ rd(d, 0x04, 1)) & 0x07))   /* I/O, memory, bus master */
            return ERR_ACCESS_DENIED;
        if (b == 0x05 && ((vb ^ rd(d, 0x05, 1)) & 0x04))   /* INTx Disable (bit 10) */
            return ERR_ACCESS_DENIED;
        if (pcie && b == pcie + 9 && (vb & 0x80))          /* Device Control: FLR */
            return ERR_ACCESS_DENIED;
        if (!manage && pm && b == pm + 4 && ((vb ^ rd(d, pm + 4, 1)) & 0x03))   /* PowerState */
            return ERR_ACCESS_DENIED;
        if (af && b == af + 4 && (vb & 0x01))              /* AF Control: Initiate FLR */
            return ERR_ACCESS_DENIED;
    }
    return OK;
}

uint64_t pci_cmd_lock(void)
{
    return spin_lock_irqsave(&cmd_lock);
}

void pci_cmd_unlock(uint64_t f)
{
    spin_unlock_irqrestore(&cmd_lock, f);
}
