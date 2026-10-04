/* Resources, MMIO VMOs for processes, bound DMA capabilities, the
 * config-space write filter and their system calls (sys_* layer).
 *
 * Tests that need a real PCI function (QEMU's edu 1234:11e8, qemu-xhci
 * 1b36:000d) skip themselves where it doesn't exist (the PC). */
#include <jam/acpi.h>
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/vmo.h>

#define PG PAGE_SIZE

#define PTE_P     (1ull << 0)
#define PTE_W     (1ull << 1)
#define PTE_U     (1ull << 2)
#define PTE_PWT   (1ull << 3)
#define PTE_PCD   (1ull << 4)
#define PTE_PAT4K (1ull << 7)
#define PTE_NX    (1ull << 63)
#define PTE_ADDR  0x000ffffffffff000ull
#define PTE_CACHE (PTE_PWT | PTE_PCD | PTE_PAT4K)

/* A physical range that is certainly not RAM (above everything the memory
 * map knows, 1 GiB aligned) and not kernel-owned MMIO. Nothing ever
 * touches it: tests only make page-table entries for it. */
static uint64_t hole(void)
{
    uint64_t top = pmm_max_pfn() << PAGE_SHIFT;
    uint64_t h = ALIGN_UP(top, 1ull << 30) + (4ull << 30);
    if (h < (64ull << 30))
        h = 64ull << 30;
    KT_ASSERT(!pmm_range_has_ram(h, 1ull << 30));
    KT_EQ(resource_phys_mappable(h, 1ull << 30), OK);
    return h;
}

static void job_clean(struct job *j)
{
    struct job_info ji;
    job_get_info(j, &ji);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        KT_EQ(ji.used[k], 0);
}

static struct kobject *root_ref(void)
{
    struct kobject *r = resource_root();
    KT_ASSERT(r);
    KT_EQ(resource_kind(r), RES_ROOT);
    return r;
}

/* ---- the object layer ---------------------------------------------------------- */

KTEST(resource_slicing)
{
    struct kobject *root = root_ref(), *mmio, *sub, *bad, *pci;
    uint64_t h = hole();

    /* MMIO slices of the root: size, alignment, wrap. */
    KT_EQ(resource_create(root, RES_MMIO, h, 0, &bad), ERR_INVALID_ARGS);
    KT_EQ(resource_create(root, RES_MMIO, h + 1, PG, &bad), ERR_INVALID_ARGS);
    KT_EQ(resource_create(root, RES_MMIO, h, PG + 8, &bad), ERR_INVALID_ARGS);
    KT_EQ(resource_create(root, RES_MMIO, RES_PHYS_LIMIT, PG, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(resource_create(root, RES_MMIO, RES_PHYS_LIMIT - PG, 2 * PG, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(resource_create(root, RES_MMIO, 0ull - PG, 2 * PG, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(resource_create(root, RES_MMIO, h, 16 * PG, &mmio), OK);
    KT_EQ(resource_kind(mmio), RES_MMIO);

    /* A slice of a slice must lie inside it. */
    KT_EQ(resource_create(mmio, RES_MMIO, h - PG, PG, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(resource_create(mmio, RES_MMIO, h + 15 * PG, 2 * PG, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(resource_create(mmio, RES_MMIO, h + 16 * PG, PG, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(resource_create(mmio, RES_MMIO, h + 4 * PG, 0ull - 2 * PG, &bad), ERR_OUT_OF_RANGE);
    KT_EQ(resource_create(mmio, RES_MMIO, h + 4 * PG, (1ull << 51), &bad), ERR_OUT_OF_RANGE);
    KT_EQ(resource_create(mmio, RES_MMIO, h + 15 * PG, PG, &sub), OK);
    KT_EQ(resource_check_mmio(sub, h + 15 * PG, PG), OK);
    KT_EQ(resource_check_mmio(sub, h + 14 * PG, PG), ERR_OUT_OF_RANGE);
    KT_EQ(resource_check_mmio(sub, h + 15 * PG, 2 * PG), ERR_OUT_OF_RANGE);
    KT_EQ(resource_check_mmio(mmio, h, 16 * PG), OK);
    KT_EQ(resource_check_mmio(root, h, PG), OK);

    /* Kinds: PCI only from the root and with no range; ROOT and PCI_DEV
     * never through resource_create. */
    KT_EQ(resource_create(mmio, RES_PCI, 0, 0, &bad), ERR_WRONG_TYPE);
    KT_EQ(resource_create(root, RES_PCI, 1, 0, &bad), ERR_INVALID_ARGS);
    KT_EQ(resource_create(root, RES_ROOT, 0, 0, &bad), ERR_INVALID_ARGS);
    KT_EQ(resource_create(root, RES_PCI_DEV, 0, 1, &bad), ERR_INVALID_ARGS);
    KT_EQ(resource_create(root, 99, h, PG, &bad), ERR_INVALID_ARGS);
    KT_EQ(resource_create(root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(resource_create(pci, RES_MMIO, h, PG, &bad), ERR_WRONG_TYPE);
    KT_EQ(resource_check_mmio(pci, h, PG), ERR_WRONG_TYPE);
    KT_EQ(resource_pci_device(root, 0, &bad), ERR_WRONG_TYPE);
    KT_EQ(resource_pci_device(pci, pci_count(), &bad), ERR_OUT_OF_RANGE);
    KT_EQ(resource_pci_bar(pci, 0, &bad), ERR_WRONG_TYPE);
    KT_ASSERT(resource_pci_dev(pci) == NULL && resource_pci_dev(root) == NULL);

    /* Not a resource at all. */
    struct vmo *v;
    KT_EQ(vmo_create(PG, 0, &v), OK);
    KT_EQ(resource_create(vmo_kobject(v), RES_MMIO, h, PG, &bad), ERR_WRONG_TYPE);
    KT_EQ(resource_kind(vmo_kobject(v)), 0);
    kobject_unref(vmo_kobject(v));

    kobject_unref(sub);
    kobject_unref(mmio);
    kobject_unref(pci);
    kobject_unref(root);
}

KTEST(resource_mmio_never_ram)
{
    struct kobject *root = root_ref(), *bad;
    uint64_t pa = pmm_alloc_page_phys(0);
    KT_ASSERT(pa);
    KT_ASSERT(pmm_range_has_ram(pa, PG));
    KT_EQ(resource_create(root, RES_MMIO, pa, PG, &bad), ERR_ACCESS_DENIED);
    /* A range that only starts in a hole but runs into RAM. */
    KT_EQ(resource_create(root, RES_MMIO, 0, 1ull << 32, &bad), ERR_ACCESS_DENIED);
    KT_EQ(resource_check_mmio(root, pa, PG), ERR_ACCESS_DENIED);
    KT_EQ(resource_phys_mappable(pa, PG), ERR_ACCESS_DENIED);
    /* The kernel image is RAM too (not in the allocator, still refused). */
    static int in_kernel_data;
    uint64_t kpa = ALIGN_DOWN(vmm_translate(vmm_kernel_pml4(), (uint64_t)&in_kernel_data), PG);
    KT_EQ(resource_create(root, RES_MMIO, kpa, PG, &bad), ERR_ACCESS_DENIED);
    pmm_free_page_phys(pa);
    kobject_unref(root);
}

KTEST(resource_kernel_mmio_not_mappable)
{
    /* The local APIC / MSI window, I/O APICs, HPET and ECAM stay the
     * kernel's even for the root. */
    KT_EQ(resource_phys_mappable(0xfee00000ull, PG), ERR_ACCESS_DENIED);
    KT_EQ(resource_phys_mappable(0xfeeff000ull, PG), ERR_ACCESS_DENIED);
    for (uint32_t i = 0; i < acpi.ioapic_count; i++)
        KT_EQ(resource_phys_mappable(ALIGN_DOWN(acpi.ioapics[i].phys, PG), PG), ERR_ACCESS_DENIED);
    if (acpi.hpet_phys)
        KT_EQ(resource_phys_mappable(ALIGN_DOWN(acpi.hpet_phys, PG), PG), ERR_ACCESS_DENIED);
    for (uint32_t i = 0; i < acpi.ecam_count; i++) {
        uint64_t base = acpi.ecam[i].phys + ((uint64_t)acpi.ecam[i].bus_start << 20);
        KT_EQ(resource_phys_mappable(base, PG), ERR_ACCESS_DENIED);
        KT_EQ(resource_phys_mappable(base + (1ull << 20) - PG, PG), ERR_ACCESS_DENIED);
    }
    KT_EQ(resource_phys_mappable(0, 0), ERR_OUT_OF_RANGE);
    KT_EQ(resource_phys_mappable(RES_PHYS_LIMIT - PG, 2 * PG), ERR_OUT_OF_RANGE);
    /* An MSI-X page of any function (the PCI core's list). */
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (!d->cap_msix || !d->info.bar[d->msix_table_bar].size)
            continue;
        uint64_t tab = d->info.bar[d->msix_table_bar].phys + d->msix_table_off;
        KT_ASSERT(pci_phys_protected(tab, 1));
        KT_EQ(resource_phys_mappable(ALIGN_DOWN(tab, PG), PG), ERR_ACCESS_DENIED);
    }
}

/* ---- the handle layer, with job charges ---------------------------------------- */

static handle_t insert_root(struct handle_table *t, rights_t rights)
{
    struct khandle kh = khandle_from_new(root_ref(), rights);
    handle_t h;
    KT_EQ(handle_insert(t, &kh, &h), OK);
    return h;
}

KTEST(resource_sys_rights_and_charges)
{
    struct job *j = kt_fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = j;
    uint64_t h = hole();

    handle_t root = insert_root(&t, RES_RIGHTS), mmio, pci, x, noslice, nomap;
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 1);   /* the slot; the root object is the kernel's */
    KT_EQ(sys_resource_create(&t, root, RES_MMIO, h, 4 * PG, &mmio), OK);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 3);   /* + slot + object */
    struct kobject *obj;
    rights_t r;
    KT_EQ(handle_get(&t, mmio, OBJ_RESOURCE, 0, &obj, &r), OK);
    KT_EQ(r, RES_RIGHTS);
    KT_EQ(resource_kind(obj), RES_MMIO);
    kobject_unref(obj);

    /* Errors pass through; nothing is charged for them. */
    KT_EQ(sys_resource_create(&t, root, RES_MMIO, h, 0, &x), ERR_INVALID_ARGS);
    KT_EQ(sys_resource_create(&t, mmio, RES_MMIO, h + 4 * PG, PG, &x), ERR_OUT_OF_RANGE);
    KT_EQ(sys_resource_create(&t, HANDLE_INVALID, RES_MMIO, h, PG, &x), ERR_BAD_HANDLE);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 3);

    /* Rights: slicing needs RIGHT_SLICE, mapping RIGHT_MAP; a slice keeps
     * (at most) the rights of the handle it came from. */
    KT_EQ(handle_duplicate(&t, root, RES_RIGHTS & ~RIGHT_SLICE, &noslice), OK);
    KT_EQ(sys_resource_create(&t, noslice, RES_MMIO, h, PG, &x), ERR_ACCESS_DENIED);
    KT_EQ(handle_duplicate(&t, root, RES_RIGHTS & ~(RIGHT_MAP | RIGHT_MANAGE), &nomap), OK);
    KT_EQ(sys_vmo_create_physical(&t, nomap, h, PG, VMO_CACHE_UC, &x), ERR_ACCESS_DENIED);
    KT_EQ(sys_resource_create(&t, nomap, RES_MMIO, h, PG, &x), OK);
    KT_EQ(handle_get(&t, x, OBJ_RESOURCE, 0, &obj, &r), OK);
    KT_EQ(r, RES_RIGHTS & ~(RIGHT_MAP | RIGHT_MANAGE));
    kobject_unref(obj);
    handle_t y;
    KT_EQ(sys_vmo_create_physical(&t, x, 0, PG, VMO_CACHE_UC, &y), ERR_ACCESS_DENIED);
    KT_EQ(handle_close(&t, x), OK);

    /* PCI: enumeration and opening need a RES_PCI. */
    struct pci_dev_info info;
    KT_EQ(sys_pci_enum(&t, root, 0, &info), ERR_WRONG_TYPE);
    KT_EQ(sys_resource_create(&t, root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(sys_pci_enum(&t, pci, pci_count(), &info), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pci_device_open(&t, pci, pci_count(), &x), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pci_device_open(&t, mmio, 0, &x), ERR_WRONG_TYPE);
    if (pci_count()) {
        KT_EQ(sys_pci_enum(&t, pci, 0, &info), OK);
        KT_EQ(info.vendor, pci_get(0)->info.vendor);
        KT_EQ(info.device, pci_get(0)->info.device);
    }
    /* Device calls on something that isn't a device. */
    uint32_t val;
    KT_EQ(sys_pci_config_read(&t, pci, 0, 4, &val), ERR_WRONG_TYPE);
    KT_EQ(sys_pci_config_write(&t, root, 0x3c, 1, 0), ERR_WRONG_TYPE);
    KT_EQ(sys_pci_bar_resource(&t, mmio, 0, &x), ERR_WRONG_TYPE);
    KT_EQ(sys_pci_bus_master(&t, pci, 1), ERR_WRONG_TYPE);
    KT_EQ(sys_dma_cap_create(&t, pci, &x), ERR_WRONG_TYPE);
    KT_EQ(sys_vmo_create_physical(&t, pci, 0, PG, VMO_CACHE_UC, &x), ERR_WRONG_TYPE);

    /* Physical VMOs: never RAM, never kernel MMIO, inside the resource. */
    uint64_t pa = pmm_alloc_page_phys(0);
    KT_EQ(sys_vmo_create_physical(&t, root, pa, PG, VMO_CACHE_UC, &x), ERR_ACCESS_DENIED);
    pmm_free_page_phys(pa);
    KT_EQ(sys_vmo_create_physical(&t, root, 0xfee00000ull, PG, VMO_CACHE_UC, &x),
          ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_create_physical(&t, mmio, 0, 5 * PG, VMO_CACHE_UC, &x), ERR_OUT_OF_RANGE);
    KT_EQ(sys_vmo_create_physical(&t, mmio, 4 * PG, PG, VMO_CACHE_UC, &x), ERR_OUT_OF_RANGE);
    KT_EQ(sys_vmo_create_physical(&t, mmio, 0ull - PG, 2 * PG, VMO_CACHE_UC, &x),
          ERR_OUT_OF_RANGE);
    KT_EQ(sys_vmo_create_physical(&t, mmio, 1, PG, VMO_CACHE_UC, &x), ERR_INVALID_ARGS);
    KT_EQ(sys_vmo_create_physical(&t, mmio, 0, 0, VMO_CACHE_UC, &x), ERR_INVALID_ARGS);
    KT_EQ(sys_vmo_create_physical(&t, mmio, 0, PG, 3, &x), ERR_INVALID_ARGS);
    uint64_t before = job_used(j, JOB_LIMIT_HANDLES);
    handle_t pv;
    KT_EQ(sys_vmo_create_physical(&t, mmio, PG, 2 * PG, VMO_CACHE_WC, &pv), OK);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), before + 2);   /* slot + the VMO struct */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);             /* never counted as RAM */
    KT_EQ(handle_get(&t, pv, OBJ_VMO, RIGHT_MAP, &obj, &r), OK);
    KT_ASSERT(!(r & RIGHT_EXEC));
    struct vmo *v = vmo_from_kobject(obj);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(vmo_commit(v, 0, 2 * PG), OK);          /* a no-op */
    KT_EQ(vmo_decommit(v, 0, PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_set_size(v, PG), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(vmo_page_phys(v, PG), h + 2 * PG);
    kobject_unref(obj);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);

    /* Pins through the handle layer need a bound cap. */
    struct kobject *ucap;
    KT_EQ(dma_cap_create(&ucap), OK);
    struct khandle ck = khandle_from_new(ucap, RIGHTS_BASIC);
    handle_t caph, rv;
    KT_EQ(handle_insert(&t, &ck, &caph), OK);
    KT_EQ(sys_vmo_create(&t, 2 * PG, 0, HANDLE_INVALID, &rv), OK);
    uint64_t addrs[2], pin;
    KT_EQ(sys_vmo_pin(&t, rv, caph, 0, 2 * PG, addrs, &pin), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_pin(&t, rv, caph, 0, (VMO_PIN_SYS_MAX_PAGES + 1) * PG, addrs, &pin),
          ERR_OUT_OF_RANGE);
    KT_EQ(sys_vmo_pin(&t, rv, caph, 0, PG + 1, addrs, &pin), ERR_INVALID_ARGS);
    KT_EQ(sys_vmo_pin(&t, rv, rv, 0, PG, addrs, &pin), ERR_WRONG_TYPE);
    KT_EQ(sys_vmo_pin(&t, caph, caph, 0, PG, addrs, &pin), ERR_WRONG_TYPE);
    KT_EQ(sys_vmo_unpin(&t, rv, caph, 12345), ERR_NOT_FOUND);
    KT_EQ(sys_vmo_unpin(&t, rv, HANDLE_INVALID, 12345), ERR_BAD_HANDLE);

    handle_table_destroy(&t);   /* closes everything */
    job_clean(j);
    job_unref(j);
}

/* A process's mapping of a physical VMO gets the VMO's cache type in its
 * page-table entry: PAT index 3 (PCD|PWT) for UC, index 5 (PAT|PWT) for
 * WC, plain write-back otherwise. */
KTEST(resource_physical_vmo_pte_cache_bits)
{
    struct handle_table t;
    handle_table_init(&t);
    handle_t root = insert_root(&t, RES_RIGHTS);
    uint64_t h = hole();
    static const struct { uint32_t cache; uint64_t bits; const char *name; } cases[] = {
        { VMO_CACHE_UC, PTE_PCD | PTE_PWT, "UC" },
        { VMO_CACHE_WC, PTE_PAT4K | PTE_PWT, "WC" },
        { VMO_CACHE_WB, 0, "WB" },
    };
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    for (unsigned i = 0; i < 3; i++) {
        handle_t vh;
        uint64_t pa = h + i * 4 * PG;
        KT_EQ(sys_vmo_create_physical(&t, root, pa, 4 * PG, cases[i].cache, &vh), OK);
        struct kobject *obj;
        KT_EQ(handle_get(&t, vh, OBJ_VMO, 0, &obj, NULL), OK);
        uint64_t a = 0;
        KT_EQ(aspace_map(as, vmo_from_kobject(obj), 0, 4 * PG, ASPACE_READ | ASPACE_CAN_READ, &a),
              OK);
        kobject_unref(obj);
        KT_EQ(aspace_fault(as, a + 3 * PG, ASPACE_READ), OK);   /* no access is made */
        uint64_t e = aspace_pte(as, a + 3 * PG);
        KT_ASSERT(e & PTE_P);
        KT_ASSERT(e & PTE_U);
        KT_ASSERT(!(e & PTE_W));
        KT_EQ(e & PTE_ADDR, pa + 3 * PG);
        KT_EQ(e & PTE_CACHE, cases[i].bits);
        KT_ASSERT(!strcmp(vmm_cache_type(aspace_pml4(as), a + 3 * PG), cases[i].name));
        KT_EQ(aspace_fault(as, a, ASPACE_WRITE), ERR_ACCESS_DENIED);
        KT_EQ(handle_close(&t, vh), OK);   /* the mapping keeps the VMO */
    }
    aspace_unref(as);
    handle_table_destroy(&t);
}

/* ---- the config-space write filter, on a fake function --------------------------- */

static uint8_t fake_cfg[4096];

static uint32_t fake_read(struct pci_dev *d, uint32_t off, uint32_t width)
{
    (void)d;
    uint32_t v = 0;
    for (uint32_t i = 0; i < width; i++)
        v |= (uint32_t)fake_cfg[off + i] << (8 * i);
    return v;
}

static void put8(uint32_t off, uint8_t v)
{
    fake_cfg[off] = v;
}

static void put16(uint32_t off, uint16_t v)
{
    fake_cfg[off] = (uint8_t)v;
    fake_cfg[off + 1] = (uint8_t)(v >> 8);
}

static void put32(uint32_t off, uint32_t v)
{
    put16(off, (uint16_t)v);
    put16(off + 2, (uint16_t)(v >> 16));
}

static status_t fw(struct pci_dev *d, uint32_t off, uint32_t width, uint32_t value)
{
    return pci_cfg_write_allowed(d, off, width, value, fake_read);
}

KTEST(resource_config_write_filter)
{
    static struct pci_dev d;
    memset(&d, 0, sizeof(d));
    memset(fake_cfg, 0, sizeof(fake_cfg));
    put32(0x00, 0x11e81234);
    put16(0x04, 0x0406);            /* memory decode, bus master, INTx disabled */
    put16(0x06, 0x0010);            /* capability list */
    fake_cfg[0x34] = 0x40;
    put16(0x40, 0x5005);            /* MSI, next 0x50 */
    put16(0x42, 0x0180);            /* 64-bit, per-vector mask: 0x18 bytes */
    put16(0x58, 0x7009);            /* vendor-specific, next 0x70 */
    put16(0x70, 0x8011);            /* MSI-X, next 0x80 */
    put16(0x80, 0x0010);            /* PCIe, end */
    put32(0x100, 0x20010010);       /* SR-IOV, next 0x200 */
    put32(0x200, 0x00010015);       /* Resizable BAR, end */
    put32(0x208, 1u << 5);          /* one BAR: 12 bytes */
    fake_cfg[0x50] = 0;
    /* MSI's next pointer goes to the vendor capability at 0x58. */
    put16(0x40, 0x5805);

    /* Access shape. */
    KT_EQ(fw(&d, 0x3c, 3, 0), ERR_INVALID_ARGS);
    KT_EQ(fw(&d, 0x3d, 2, 0), ERR_INVALID_ARGS);
    KT_EQ(fw(&d, 0x3e, 4, 0), ERR_INVALID_ARGS);
    KT_EQ(fw(&d, 4096, 1, 0), ERR_INVALID_ARGS);
    KT_EQ(pci_cfg_access_ok(4092, 4), OK);

    /* Free registers. */
    KT_EQ(fw(&d, 0x3c, 1, 0x0b), OK);   /* interrupt line */
    KT_EQ(fw(&d, 0x0c, 1, 0x10), OK);   /* cache line size */
    KT_EQ(fw(&d, 0x0d, 1, 0x40), OK);   /* latency timer */
    KT_EQ(fw(&d, 0x5c, 4, 0xffffffffu), OK);   /* vendor-specific */
    KT_EQ(fw(&d, 0x800, 4, 1), OK);

    /* BIST, BARs, expansion ROM. */
    KT_EQ(fw(&d, 0x0f, 1, 0x40), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x0c, 4, 0), ERR_ACCESS_DENIED);   /* reaches BIST */
    for (uint32_t off = 0x10; off < 0x28; off += 4)
        KT_EQ(fw(&d, off, 4, 0xffffffffu), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x27, 1, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x28, 4, 0), OK);                  /* CardBus CIS: read-only anyway */
    KT_EQ(fw(&d, 0x30, 4, 0xfffff800u), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x32, 2, 0), ERR_ACCESS_DENIED);

    /* Command: the decode, bus-master and INTx-disable bits stay as they are. */
    KT_EQ(fw(&d, 0x04, 2, 0x0406), OK);
    KT_EQ(fw(&d, 0x04, 2, 0x0546), OK);             /* parity + SERR# reporting */
    KT_EQ(fw(&d, 0x04, 2, 0x0402), ERR_ACCESS_DENIED);   /* BME off */
    KT_EQ(fw(&d, 0x04, 2, 0x0404), ERR_ACCESS_DENIED);   /* memory decode off */
    KT_EQ(fw(&d, 0x04, 2, 0x0407), ERR_ACCESS_DENIED);   /* I/O decode on */
    KT_EQ(fw(&d, 0x04, 2, 0x0006), ERR_ACCESS_DENIED);   /* INTx back on */
    KT_EQ(fw(&d, 0x04, 1, 0x06), OK);
    KT_EQ(fw(&d, 0x04, 1, 0x02), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x05, 1, 0x04), OK);
    KT_EQ(fw(&d, 0x05, 1, 0x00), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x04, 4, 0xffff0406u), OK);        /* status: write-1-to-clear */
    KT_EQ(fw(&d, 0x06, 2, 0xffff), OK);
    put16(0x04, 0x0000);                            /* now everything is off */
    KT_EQ(fw(&d, 0x04, 2, 0x0004), ERR_ACCESS_DENIED);   /* can't turn BME on */
    KT_EQ(fw(&d, 0x04, 2, 0x0000), OK);
    put16(0x04, 0x0406);

    /* MSI (0x40-0x57), MSI-X (0x70-0x7b). */
    KT_EQ(fw(&d, 0x40, 4, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x42, 2, 0x0181), ERR_ACCESS_DENIED);   /* MSI enable */
    KT_EQ(fw(&d, 0x44, 4, 0xfee00000u), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x54, 4, 0), ERR_ACCESS_DENIED);   /* pending bits */
    KT_EQ(fw(&d, 0x56, 2, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x57, 1, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x58, 4, 0x7009), OK);             /* the next capability */
    KT_EQ(fw(&d, 0x6c, 4, 0), OK);
    KT_EQ(fw(&d, 0x70, 2, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x72, 2, 0xc000), ERR_ACCESS_DENIED);   /* MSI-X enable + mask all */
    KT_EQ(fw(&d, 0x78, 4, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x7b, 1, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x7c, 4, 0), OK);

    /* 32-bit MSI without masking is 10 bytes: 0x40-0x49. */
    put16(0x42, 0x0000);
    KT_EQ(fw(&d, 0x48, 2, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x4a, 2, 0), OK);
    put16(0x42, 0x0180);

    /* PCIe Device Control (0x88): anything but Initiate FLR. */
    KT_EQ(fw(&d, 0x88, 2, 0x0010), OK);
    KT_EQ(fw(&d, 0x88, 2, 0x8000), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x89, 1, 0x80), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x88, 4, 0x00008000u), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x8c, 4, 0xffffffffu), OK);

    /* SR-IOV (0x100-0x13f) and Resizable BAR (0x200-0x20b). */
    KT_EQ(fw(&d, 0x108, 2, 1), ERR_ACCESS_DENIED);   /* VF Enable */
    KT_EQ(fw(&d, 0x13c, 4, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x140, 4, 0), OK);
    KT_EQ(fw(&d, 0x208, 4, 0), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x20c, 4, 0), OK);

    /* A looping capability list ends the walk and finds what it passed. */
    put16(0x80, 0x8010);            /* PCIe points at itself */
    KT_EQ(fw(&d, 0x88, 2, 0x8000), ERR_ACCESS_DENIED);
    KT_EQ(fw(&d, 0x3c, 1, 0), OK);

    /* Bridges and the boot display: nothing at all. */
    d.info.flags = PCI_INFO_BRIDGE;
    KT_EQ(fw(&d, 0x3c, 1, 0), ERR_ACCESS_DENIED);
    d.info.flags = PCI_INFO_DISPLAY;
    KT_EQ(fw(&d, 0x3c, 1, 0), ERR_ACCESS_DENIED);
    d.info.flags = 0;
    d.info.header_type = 0x81;      /* multi-function type 1 */
    KT_EQ(fw(&d, 0x3c, 1, 0), ERR_ACCESS_DENIED);
    d.info.header_type = 0x80;      /* multi-function type 0 */
    KT_EQ(fw(&d, 0x3c, 1, 0), OK);
}

/* ---- DMA capabilities -------------------------------------------------------------- */

/* The kernel-test (unbound) cap keeps working, and closing its last handle
 * releases every pin made with it; after that it pins no more. */
KTEST(resource_dma_cap_close_releases_pins)
{
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    KT_ASSERT(dma_cap_device(cap) == NULL);
    KT_ASSERT(dma_cap_bus_master_on(cap));
    kobject_ref(cap);   /* ours, for after the handle is gone */
    struct khandle kh = khandle_from_new(cap, RIGHTS_BASIC);
    handle_t ch;
    KT_EQ(handle_insert(&t, &kh, &ch), OK);

    struct vmo *a, *b;
    KT_EQ(vmo_create(8 * PG, VMO_DMA32, &a), OK);
    KT_EQ(vmo_create(4 * PG, VMO_CONTIGUOUS | VMO_DMA32, &b), OK);
    uint64_t pa[8], pb[4], ida, ida2, idb;
    KT_EQ(vmo_pin(a, cap, 0, 4 * PG, pa, 8, &ida), OK);
    KT_EQ(vmo_pin(a, cap, 4 * PG, 4 * PG, pa + 4, 4, &ida2), OK);
    KT_EQ(vmo_pin(b, cap, 0, 4 * PG, pb, 4, &idb), OK);
    for (unsigned i = 0; i < 8; i++)
        KT_ASSERT(pa[i] && pa[i] < (4ull << 30));
    for (unsigned i = 0; i < 4; i++)
        KT_EQ(pb[i], pb[0] + i * PG);          /* contiguous */
    KT_ASSERT(pb[3] < (4ull << 30));
    KT_EQ(dma_cap_pin_count(cap), 3);
    KT_EQ(vmo_unpin(a, cap, ida2), OK);             /* an unpin takes it off the cap too */
    KT_EQ(dma_cap_pin_count(cap), 2);
    KT_EQ(vmo_decommit(a, 0, PG), ERR_BAD_STATE);   /* still pinned */

    KT_EQ(handle_close(&t, ch), OK);           /* the last handle: the close path */
    KT_EQ(dma_cap_pin_count(cap), 0);
    KT_EQ(vmo_unpin(a, cap, ida), ERR_NOT_FOUND);   /* already released */
    KT_EQ(vmo_unpin(b, cap, idb), ERR_NOT_FOUND);
    KT_EQ(vmo_decommit(a, 0, 8 * PG), OK);
    KT_EQ(vmo_pin(a, cap, 0, PG, pa, 8, &ida), ERR_BAD_STATE);   /* closed: no new pins */
    KT_EQ(dma_cap_pin_count(cap), 0);

    kobject_unref(vmo_kobject(a));
    kobject_unref(vmo_kobject(b));
    kobject_unref(cap);
    handle_table_destroy(&t);
}

/* A pin holds its VMO; the close path releasing it is what lets it go. */
KTEST(resource_dma_cap_close_frees_pinned_vmo)
{
    struct vmo *keep;
    KT_EQ(vmo_create(PG, 0, &keep), OK);   /* warm the slab */
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    struct khandle kh = khandle_from_new(cap, RIGHTS_BASIC);
    handle_t ch;
    KT_EQ(handle_insert(&t, &kh, &ch), OK);
    struct vmo *v;
    KT_EQ(vmo_create(16 * PG, 0, &v), OK);
    kobject_ref(vmo_kobject(v));
    uint64_t addrs[16], id;
    KT_EQ(vmo_pin(v, cap, 0, 16 * PG, addrs, 16, &id), OK);
    KT_EQ(vmo_committed(v), 16 * PG);
    uint32_t refs = __atomic_load_n(&vmo_kobject(v)->refs, __ATOMIC_RELAXED);
    KT_EQ(handle_close(&t, ch), OK);           /* releases the pin and its VMO reference */
    KT_EQ(__atomic_load_n(&vmo_kobject(v)->refs, __ATOMIC_RELAXED), refs - 1);
    kobject_unref(vmo_kobject(v));
    kobject_unref(vmo_kobject(v));             /* gone now: nothing left holds it */
    handle_table_destroy(&t);
    kobject_unref(vmo_kobject(keep));
}

/* A process killed while it holds the only handle to a cap: the kill
 * closes it like any other handle, so the pins made with it go, the VMO
 * they held goes, and the job ends with nothing charged. (The cap is an
 * unbound kernel one pinned from here; utest's edu_killed_mid_dma does the
 * same with a real device mid-DMA.) */
KTEST(resource_kill_releases_pins)
{
    struct job *j = kt_fresh_job();
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    KT_EQ(dma_cap_set_job(cap, j), OK);
    KT_EQ(dma_cap_set_job(cap, j), ERR_BAD_STATE);
    kobject_ref(cap);   /* ours; the other goes to the child as a handle */
    struct vmo *v;
    KT_EQ(vmo_create(8 * PG, VMO_DMA32, &v), OK);
    KT_EQ(vmo_set_job(v, j), OK);

    const char *argv[] = { "utest", "spin" };
    struct userboot_handle x = { SR_USER, khandle_from_new(cap, RIGHTS_BASIC) };
    struct process *p;
    KT_EQ(userboot_spawn("bin/utest", argv, 2, j, &x, 1, NULL, &p), OK);
    uint64_t addrs[8], id;
    KT_EQ(vmo_pin(v, cap, 0, 8 * PG, addrs, 8, &id), OK);
    KT_EQ(dma_cap_pin_count(cap), 1);
    KT_ASSERT(job_used(j, JOB_LIMIT_PAGES) >= 8);
    kobject_unref(vmo_kobject(v));   /* only the pin holds the VMO now */

    process_kill(p, PROCESS_KILLED_CODE, true);
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 20000000000ull, NULL),
          OK);
    kobject_unref(process_kobject(p));
    KT_EQ(dma_cap_pin_count(cap), 0);
    kobject_unref(cap);
    job_clean(j);
    job_unref(j);
}

/* Pins racing the close: threads on other CPUs pin and unpin with the cap
 * until it refuses; the close lands somewhere in the middle. Afterwards no
 * pin may be left on the cap or the VMOs (every page decommits again). */
#define RACE_THREADS 3

struct pin_race {
    struct kobject *cap;    /* the DMA capability pinned through */
    struct vmo     *v;      /* the VMO pinned */
    volatile bool   go;     /* start line for the racing threads */
    uint64_t        pins;   /* pins made */
};

static void pin_racer(void *arg)
{
    struct pin_race *r = arg;
    while (!r->go)
        thread_yield();
    for (;;) {
        uint64_t addrs[4], id;
        status_t st = vmo_pin(r->v, r->cap, 0, 4 * PG, addrs, 4, &id);
        if (st == ERR_BAD_STATE)
            break;   /* closed */
        KT_EQ(st, OK);
        r->pins++;
        st = vmo_unpin(r->v, r->cap, id);
        KT_ASSERT(st == OK || st == ERR_NOT_FOUND);   /* the close may have taken it */
    }
}

KTEST(resource_pin_close_race)
{
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    kobject_ref(cap);
    struct khandle kh = khandle_from_new(cap, RIGHTS_BASIC);
    handle_t ch;
    KT_EQ(handle_insert(&t, &kh, &ch), OK);
    static struct pin_race r[RACE_THREADS];
    struct thread *th[RACE_THREADS];
    for (unsigned i = 0; i < RACE_THREADS; i++) {
        r[i] = (struct pin_race){ .cap = cap };
        KT_EQ(vmo_create(4 * PG, 0, &r[i].v), OK);
        th[i] = thread_create("pin-racer", pin_racer, &r[i], PRIO_DEFAULT);
    }
    for (unsigned i = 0; i < RACE_THREADS; i++)
        r[i].go = true;
    thread_sleep_ns(2000000);   /* 2 ms of pinning */
    KT_EQ(handle_close(&t, ch), OK);
    uint64_t total = 0;
    for (unsigned i = 0; i < RACE_THREADS; i++) {
        thread_join(th[i]);
        total += r[i].pins;
    }
    KT_EQ(dma_cap_pin_count(cap), 0);
    for (unsigned i = 0; i < RACE_THREADS; i++) {
        KT_EQ(vmo_decommit(r[i].v, 0, 4 * PG), OK);   /* nothing pinned any more */
        kobject_unref(vmo_kobject(r[i].v));
    }
    kprintf("ktest %s: %lu pins before the close\n", ktest_current, total);
    kobject_unref(cap);
    handle_table_destroy(&t);
}

/* ---- with a real function (QEMU's edu, qemu-xhci) ----------------------------------- */

#define EDU_VENDOR 0x1234
#define EDU_DEVICE 0x11e8
#define CMD_BME    0x04

static struct pci_dev *edu(void)
{
    struct pci_dev *d = pci_find(EDU_VENDOR, EDU_DEVICE, 0);
    if (!d)
        kprintf("ktest %s: no free edu device (QEMU -device edu only; from the shell its "
                "driver has it), skipped\n", ktest_current);
    return d;
}

/* Open the edu function through the handle layer: *devh is devmgr's copy
 * (RIGHT_MANAGE). Returns false if there is none. */
static bool open_edu(struct handle_table *t, handle_t *devh)
{
    struct pci_dev *d = edu();
    if (!d)
        return false;
    handle_t root = insert_root(t, RES_RIGHTS), pci;
    KT_EQ(sys_resource_create(t, root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(sys_pci_device_open(t, pci, d->index, devh), OK);
    KT_EQ(handle_close(t, pci), OK);
    KT_EQ(handle_close(t, root), OK);
    return true;
}

KTEST(resource_pin_needs_bus_master)
{
    struct pci_dev *d = edu();
    if (!d)
        return;
    struct kobject *cap;
    KT_EQ(dma_cap_create_for(d, NULL, &cap), OK);
    KT_ASSERT(dma_cap_device(cap) == d);
    struct vmo *v;
    KT_EQ(vmo_create(PG, VMO_CONTIGUOUS | VMO_DMA32, &v), OK);
    uint64_t pa, id;
    KT_EQ(pci_set_bus_master(d, false), OK);
    KT_EQ(vmo_pin(v, cap, 0, PG, &pa, 1, &id), ERR_BAD_STATE);
    KT_EQ(pci_set_bus_master(d, true), OK);
    KT_EQ(vmo_pin(v, cap, 0, PG, &pa, 1, &id), OK);
    KT_ASSERT(pa < (4ull << 30));
    KT_EQ(vmo_unpin(v, cap, id), OK);
    KT_EQ(pci_set_bus_master(d, false), OK);
    kobject_unref(vmo_kobject(v));
    kobject_unref(cap);
}

/* A function devmgr opened (RIGHT_MANAGE, from a process's
 * table) stays in use by a driver for `ktest` from the shell even when no
 * process holds it any more: while devmgr or the driver restarts,
 * proc_users is 0 for a moment and a test must not grab the device then. */
KTEST(resource_managed_function_stays_in_use)
{
    struct pci_dev *d = edu();   /* live: devmgr's edu driver has it, so skipped */
    if (!d)
        return;
    bool was = __atomic_load_n(&d->driver_managed, __ATOMIC_RELAXED);
    __atomic_store_n(&d->driver_managed, false, __ATOMIC_RELAXED);
    struct job *j = kt_fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = j;
    handle_t dev, drv;
    KT_ASSERT(open_edu(&t, &dev));
    KT_ASSERT(__atomic_load_n(&d->driver_managed, __ATOMIC_RELAXED));
    KT_ASSERT(__atomic_load_n(&d->proc_users, __ATOMIC_RELAXED));
    /* A narrowed copy (a driver's) is not a binding by itself. */
    KT_EQ(handle_duplicate(&t, dev, RES_RIGHTS & ~RIGHT_MANAGE, &drv), OK);
    handle_table_destroy(&t);
    job_unref(j);
    KT_EQ(__atomic_load_n(&d->proc_users, __ATOMIC_RELAXED), 0);
    KT_ASSERT(pci_in_use(d));   /* nobody holds it: still the drivers' */
    bool hide = __atomic_load_n(&pci_hide_in_use, __ATOMIC_RELAXED);
    __atomic_store_n(&pci_hide_in_use, true, __ATOMIC_RELAXED);
    KT_ASSERT(pci_find(EDU_VENDOR, EDU_DEVICE, 0) != d);
    __atomic_store_n(&pci_hide_in_use, false, __ATOMIC_RELAXED);
    KT_ASSERT(pci_find(EDU_VENDOR, EDU_DEVICE, 0) == d);   /* the boot menu's tests see it */
    __atomic_store_n(&pci_hide_in_use, hide, __ATOMIC_RELAXED);
    __atomic_store_n(&d->driver_managed, was, __ATOMIC_RELAXED);
}

KTEST(resource_dma_close_clears_bus_master)
{
    struct job *j = kt_fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = j;
    handle_t dev, drv, cap, vh, x;
    if (!open_edu(&t, &dev)) {
        handle_table_destroy(&t);
        job_unref(j);
        return;
    }
    struct pci_dev *d = edu();
    /* A driver's copy has no RIGHT_MANAGE: no bus mastering, no dma_cap. */
    KT_EQ(handle_duplicate(&t, dev, RES_RIGHTS & ~RIGHT_MANAGE, &drv), OK);
    KT_EQ(sys_pci_bus_master(&t, drv, 1), ERR_ACCESS_DENIED);
    KT_EQ(sys_dma_cap_create(&t, drv, &x), ERR_ACCESS_DENIED);
    /* Nor devmgr's: bus mastering goes on only through the dma_cap. */
    KT_EQ(sys_pci_bus_master(&t, dev, 1), ERR_ACCESS_DENIED);

    KT_EQ(sys_dma_cap_create(&t, dev, &cap), OK);
    KT_ASSERT(!(pci_cfg_read(d, 0x04, 2) & CMD_BME));   /* a new cap starts with it off */
    KT_EQ(sys_vmo_create(&t, 2 * PG, VMO_CONTIGUOUS | VMO_DMA32, cap, &vh), OK);
    uint64_t addrs[2], id;
    KT_EQ(sys_vmo_pin(&t, vh, cap, 0, 2 * PG, addrs, &id), ERR_BAD_STATE);   /* BME off */
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    KT_ASSERT(pci_cfg_read(d, 0x04, 2) & CMD_BME);
    KT_EQ(sys_pci_bus_master(&t, dev, 0), OK);   /* devmgr may still turn it off */
    KT_EQ(sys_vmo_pin(&t, vh, cap, 0, 2 * PG, addrs, &id), ERR_BAD_STATE);
    KT_EQ(sys_dma_cap_bus_master(&t, cap, 1), OK);
    KT_EQ(sys_vmo_pin(&t, vh, cap, 0, 2 * PG, addrs, &id), OK);
    KT_EQ(addrs[1], addrs[0] + PG);
    /* The driver can't turn BME off (or back on) through config space. */
    uint32_t cmd;
    KT_EQ(sys_pci_config_read(&t, drv, 0x04, 2, &cmd), OK);
    KT_EQ(sys_pci_config_write(&t, drv, 0x04, 2, cmd & ~CMD_BME), ERR_ACCESS_DENIED);
    KT_EQ(sys_pci_config_write(&t, drv, 0x04, 2, cmd), OK);

    struct kobject *capobj;
    KT_EQ(handle_get(&t, cap, OBJ_DMA_CAP, 0, &capobj, NULL), OK);
    KT_EQ(dma_cap_pin_count(capobj), 1);
    KT_EQ(handle_close(&t, cap), OK);           /* last handle: BME off, then quarantine */
    KT_ASSERT(!(pci_cfg_read(d, 0x04, 2) & CMD_BME));
    KT_EQ(dma_cap_pin_count(capobj), 0);
    struct dma_quarantine_stats q;
    dma_quarantine_stats(d, &q);
    if (!dma_cap_translated(capobj)) {   /* (the IOMMU's frees them at once: test_dma*.c) */
        KT_EQ(q.pins, 1);
        KT_EQ(q.pages, 2);
    }
    KT_EQ(sys_vmo_unpin(&t, vh, cap, id), ERR_BAD_HANDLE);   /* the cap's handle is gone */
    struct kobject *vo;
    KT_EQ(handle_get(&t, vh, OBJ_VMO, 0, &vo, NULL), OK);
    KT_EQ(vmo_unpin(vmo_from_kobject(vo), capobj, id), ERR_NOT_FOUND);   /* and so is the pin */
    kobject_unref(vo);
    kobject_unref(capobj);
    KT_EQ(sys_pci_config_write(&t, drv, 0x04, 2, cmd), ERR_ACCESS_DENIED);   /* stale RMW */

    handle_table_destroy(&t);
    dma_quarantine_flush(d);   /* the pages are still charged to j until released */
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, 0);
    job_clean(j);
    job_unref(j);
}

KTEST(resource_pci_device_calls)
{
    struct job *j = kt_fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = j;
    handle_t dev, bar, x;
    if (!open_edu(&t, &dev)) {
        handle_table_destroy(&t);
        job_unref(j);
        return;
    }
    struct pci_dev *d = edu();
    uint32_t v;
    KT_EQ(sys_pci_config_read(&t, dev, 0, 4, &v), OK);
    KT_EQ(v, (uint32_t)EDU_DEVICE << 16 | EDU_VENDOR);
    KT_EQ(sys_pci_config_read(&t, dev, 1, 2, &v), ERR_INVALID_ARGS);
    KT_EQ(sys_pci_config_read(&t, dev, 4096, 1, &v), ERR_INVALID_ARGS);
    KT_EQ(sys_pci_config_write(&t, dev, 0x10, 4, 0xffffffffu), ERR_ACCESS_DENIED);   /* BAR0 */
    KT_EQ(sys_pci_config_read(&t, dev, 0x10, 4, &v), OK);
    KT_EQ(v & ~0xfu, (uint32_t)d->info.bar[0].phys);   /* untouched */
    if (d->cap_msi)
        KT_EQ(sys_pci_config_write(&t, dev, d->cap_msi + 4, 4, 0), ERR_ACCESS_DENIED);
    KT_EQ(sys_pci_config_write(&t, dev, 0x3c, 1, 0x0b), OK);

    /* BAR 0 (edu: 1 MiB of registers) as a resource, mapped uncached. */
    KT_EQ(sys_pci_bar_resource(&t, dev, 6, &x), ERR_OUT_OF_RANGE);
    KT_EQ(sys_pci_bar_resource(&t, dev, 1, &x), ERR_NOT_FOUND);
    KT_EQ(sys_pci_bar_resource(&t, dev, 0, &bar), OK);
    handle_t mv;
    KT_EQ(sys_vmo_create_physical(&t, bar, 0, PG, VMO_CACHE_UC, &mv), OK);
    KT_EQ(sys_vmo_create_physical(&t, bar, 0, d->info.bar[0].size + PG, VMO_CACHE_UC, &x),
          ERR_OUT_OF_RANGE);
    struct kobject *obj;
    KT_EQ(handle_get(&t, mv, OBJ_VMO, 0, &obj, NULL), OK);
    KT_EQ(vmo_page_phys(vmo_from_kobject(obj), 0), d->info.bar[0].phys);
    void *regs;
    KT_EQ(vmo_map_kernel(vmo_from_kobject(obj), 0, PG, 0, &regs), OK);
    KT_EQ(*(volatile uint32_t *)regs & 0xff, 0xed);   /* edu identification: 0xRRrr00ed */
    KT_EQ(vmo_unmap_kernel(vmo_from_kobject(obj), regs), OK);
    kobject_unref(obj);

    handle_table_destroy(&t);
    job_clean(j);
    job_unref(j);
}

/* qemu-xhci has MSI-X: no physical VMO may cover its table or PBA page. */
KTEST(resource_msix_page_refused)
{
    struct pci_dev *d = pci_find(0x1b36, 0x000d, 0);
    if (!d || !d->cap_msix) {
        kprintf("ktest %s: no qemu-xhci with MSI-X, skipped\n", ktest_current);
        return;
    }
    struct handle_table t;
    handle_table_init(&t);
    handle_t root = insert_root(&t, RES_RIGHTS), pci, dev, bar, x;
    KT_EQ(sys_resource_create(&t, root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(sys_pci_device_open(&t, pci, d->index, &dev), OK);
    uint64_t bar_phys = d->info.bar[d->msix_table_bar].phys;
    uint64_t tab = ALIGN_DOWN(bar_phys + d->msix_table_off, PG);
    uint64_t pba = ALIGN_DOWN(d->info.bar[d->msix_pba_bar].phys + d->msix_pba_off, PG);
    KT_EQ(sys_vmo_create_physical(&t, root, tab, PG, VMO_CACHE_UC, &x), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_create_physical(&t, root, pba, PG, VMO_CACHE_UC, &x), ERR_ACCESS_DENIED);
    KT_EQ(sys_pci_bar_resource(&t, dev, d->msix_table_bar, &bar), OK);
    KT_EQ(sys_vmo_create_physical(&t, bar, tab - ALIGN_DOWN(bar_phys, PG), PG, VMO_CACHE_UC, &x),
          ERR_ACCESS_DENIED);
    uint64_t base, size;
    struct kobject *obj;
    KT_EQ(handle_get(&t, bar, OBJ_RESOURCE, 0, &obj, NULL), OK);
    KT_EQ(resource_range(obj, &base, &size), OK);
    kobject_unref(obj);
    KT_EQ(sys_vmo_create_physical(&t, bar, 0, size, VMO_CACHE_UC, &x), ERR_ACCESS_DENIED);
    /* Its MSI-X capability is read-only to a driver. */
    KT_EQ(sys_pci_config_write(&t, dev, d->cap_msix, 4, 0xc0000000u), ERR_ACCESS_DENIED);
    handle_table_destroy(&t);
}

/* ---- the config filter: other resets, caps the live list hides ------------------ */

KTEST(m6r_filter_other_resets)
{
    static struct pci_dev d;
    memset(&d, 0, sizeof(d));
    memset(fake_cfg, 0, sizeof(fake_cfg));
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
    /* devmgr (RIGHT_MANAGE) may change the power
     * state, to wake a function left in D3; still no FLR for anyone. */
    KT_EQ(pci_cfg_write_allowed_as(&d, 0x44, 2, 3, fake_read, true), OK);
    KT_ASSERT(pci_cfg_write_changes_power(&d, 0x44, 2, 3, fake_read));
    KT_ASSERT(pci_cfg_write_changes_power(&d, 0x44, 1, 3, fake_read));
    KT_ASSERT(pci_cfg_write_changes_power(&d, 0x44, 4, 3, fake_read));
    KT_ASSERT(!pci_cfg_write_changes_power(&d, 0x44, 2, 0x8000, fake_read));
    KT_ASSERT(!pci_cfg_write_changes_power(&d, 0x45, 1, 3, fake_read));
    /* PMC, not PMCSR */
    KT_ASSERT(!pci_cfg_write_changes_power(&d, 0x40, 4, 0x03000000, fake_read));
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
    memset(fake_cfg, 0, sizeof(fake_cfg));
    /* The kernel found MSI at 0x60 at boot; the live list now says there
     * are no capabilities (a device whose list a vendor register can hide). */
    d.cap_msi = 0x60;
    d.cap_msix = 0x70;
    put16(0x60, 0x0005);
    put16(0x70, 0x0011);
    KT_EQ(pci_cfg_write_allowed(&d, 0x64, 4, 0xfee00000u, fake_read), ERR_ACCESS_DENIED);
    KT_EQ(pci_cfg_write_allowed(&d, 0x72, 2, 0x8000, fake_read), ERR_ACCESS_DENIED);
}

KTEST(m6p2_bar_overlaps_live_function)
{
    struct pci_dev *e = pci_find(0x1234, 0x11e8, 0), *x = pci_find(0x1b36, 0x000d, 0);
    if (!e || !x) {
        kprintf("ktest %s: no edu + qemu-xhci, skipped\n", ktest_current);
        return;
    }
    KT_ASSERT(pci_cfg_read(x, 0x04, 2) & 0x2);   /* the xHCI decodes its BAR 0 */
    KT_ASSERT(e->info.bar[0].size >= x->info.bar[0].size);
    struct kobject *root = resource_root(), *pci, *dev, *res = NULL;
    KT_ASSERT(root);
    KT_EQ(resource_create(root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(resource_pci_device(pci, e->index, &dev), OK);
    /* edu's BAR 0 as firmware might have left a disabled function's: over
     * the xHCI's registers (page-aligned and bigger than a page, so it has
     * no rounding slack). */
    uint64_t saved = e->info.bar[0].phys;
    e->info.bar[0].phys = x->info.bar[0].phys;
    status_t st = resource_pci_bar(dev, 0, &res);
    e->info.bar[0].phys = saved;
    uint64_t base = 0, size = 0;
    if (st == OK)
        resource_range(res, &base, &size);
    kprintf("ktest %s: a BAR over 00:%02x.%u's registers [%lx, +%lx): %s%s\n", ktest_current,
            x->info.dev, x->info.fn, x->info.bar[0].phys, x->info.bar[0].size, status_str(st),
            st == OK ? " (a RES_MMIO over another function's registers)" : "");
    if (res)
        kobject_unref(res);
    kobject_unref(dev);
    kobject_unref(pci);
    kobject_unref(root);
    KT_EQ(st, ERR_ACCESS_DENIED);
}
