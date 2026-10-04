/* VT-d DMA remapping at boot and at the jump (Intel VT-d specification
 * 4.1; the plan is docs/M11-PLAN.md, "The boot handover" and "kexec and
 * panic"; the model is vtd_domain.h). With the boot word `iommu=on` only.
 *
 *   - Before the memory managers (iommu_reserve_early): RMRRs in RAM the
 *     memory map calls usable are made reserved (8.4 says firmware must
 *     report them so; a table that doesn't is caught here).
 *   - After the units are started (iommu_boot), per unit: which PCI
 *     functions it covers (its DRHD scopes, or INCLUDE_PCI_ALL for the
 *     rest of its segment, 8.3), its tables: the blocking domain, the
 *     pass-through domain (TT = 10b where ECAP.PT, else an identity map of
 *     all RAM), a boot domain with its RMRRs for each function an RMRR
 *     names (3.16), every covered function's context entry naming its
 *     home; then the handover (vtd_boot_handover): Set Root Table Pointer
 *     and its invalidations (6.6), translation on, protected memory off.
 *     Translation found on (the firmware's DMA protection, or a kexec'd
 *     kernel that couldn't turn it off) is never turned off: that would
 *     open all of RAM for a moment. The unit is pointed at the new tables
 *     while it translates; 6.6 asks the new tables to translate in-flight
 *     requests as the old did, and they do for every RMRR (mapped for its
 *     device); any other request in flight is blocked and logged, the
 *     state the boot is going to anyway.
 *   - Before a jump into another kernel or a firmware reset
 *     (iommu_jump_off): per started unit, the fault event masked,
 *     interrupt remapping, translation and queued invalidation off, with
 *     no lock taken (other CPUs may be halted holding any).
 *
 * The functions a driver holds a dma_cap for go to the pass-through domain
 * (iommu_device_driven, vtd_domain.c): everything else stays blocked. */
#include <jam/acpi.h>
#include <jam/boot.h>
#include <jam/cmdline.h>
#include <jam/dmar.h>
#include <jam/iommu.h>
#include <jam/kexec.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/report.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/vtd.h>
#include <jam/x86.h>

#include "vtd_domain.h"
#include "vtd_internal.h"

#define PCI_SECONDARY    0x19
#define PCI_SUBORDINATE  0x1a
#define RMRR_MAX_PAGES   (1ull << 18)   /* 1 GiB: a larger RMRR is refused as nonsense */
#define JUMP_WAIT_MS     10             /* per step at the jump: the panic path can't wait long */

static struct dmar_info early_info;   /* the DMAR table as read before the memory managers */
static bool early_read;               /* that pass read it (its RMRRs are reserved) */

/* ---- before the memory managers ------------------------------------------------------- */

status_t vtd_rmrr_carve(struct boot_mem_region *map, size_t *n, size_t cap, uint64_t base,
                        uint64_t limit, uint64_t *out_bytes)
{
    *out_bytes = 0;
    if (limit < base || limit > UINT64_MAX - PAGE_SIZE)
        return ERR_INVALID_ARGS;
    uint64_t lo = ALIGN_DOWN(base, PAGE_SIZE), hi = ALIGN_UP(limit + 1, PAGE_SIZE);
    /* Each overlay takes one overlap out (it is reserved afterwards), so
     * the scan restarts at most once per entry the range touches. */
    for (size_t i = 0; i < *n;) {
        const struct boot_mem_region r = map[i];
        uint64_t a = lo > r.base ? lo : r.base;
        uint64_t b = hi < r.base + r.length ? hi : r.base + r.length;
        if ((r.type != BOOT_MEM_USABLE && r.type != BOOT_MEM_LOADER_RECLAIMABLE) || a >= b) {
            i++;
            continue;
        }
        status_t st = kexec_memmap_overlay(map, n, cap, a, b - a, BOOT_MEM_RESERVED);
        if (st != OK)
            return st;
        *out_bytes += b - a;
        i = 0;
    }
    return OK;
}

/* Can [pa, pa + len) be read through the loader's direct map now? After
 * Limine (base revision 4 and later, Jam OS asks for 6) every ACPI table
 * is mapped: its protocol guarantees it. After a kexec the previous
 * kernel mapped only RAM (kexec/image.c), so the range must lie in it. */
static bool early_readable(const struct boot_info *bi, uint64_t pa, uint64_t len)
{
    if (!len || pa + len < pa)
        return false;
    if (!bi->kexec_record)
        return true;
    for (uint64_t p = ALIGN_DOWN(pa, PAGE_SIZE); p < pa + len; p += PAGE_SIZE) {
        bool in = false;
        for (size_t i = 0; i < bi->memmap_count && !in; i++) {
            const struct boot_mem_region *r = &bi->memmap[i];
            in = boot_mem_is_ram(r->type) && p >= r->base && p + PAGE_SIZE <= r->base + r->length;
        }
        if (!in)
            return false;
    }
    return true;
}

static const void *early_at(const struct boot_info *bi, uint64_t pa, uint64_t len)
{
    return early_readable(bi, pa, len) ? (const void *)(uintptr_t)(pa + bi->hhdm_offset) : NULL;
}

/* The DMAR table through the RSDP and the XSDT (or RSDT): OK with it, or
 * ERR_NOT_FOUND (there is none), or ERR_BAD_STATE (a table on the way
 * isn't mapped yet). */
static status_t early_dmar(const struct boot_info *bi, const struct acpi_header **out)
{
    const uint8_t *r = bi->rsdp_phys ? early_at(bi, bi->rsdp_phys, 36) : NULL;
    if (!r || memcmp(r, "RSD PTR ", 8))
        return ERR_BAD_STATE;
    uint32_t rsdt;
    uint64_t xsdt;
    memcpy(&rsdt, r + 16, 4);
    memcpy(&xsdt, r + 24, 8);
    bool x = r[15] >= 2 && xsdt;   /* revision 2+: the XSDT */
    uint64_t root = x ? xsdt : rsdt;
    const struct acpi_header *h = early_at(bi, root, sizeof(*h));
    if (!h || h->length < sizeof(*h) || !early_at(bi, root, h->length))
        return ERR_BAD_STATE;
    unsigned entry = x ? 8 : 4;
    const uint8_t *list = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < (h->length - sizeof(*h)) / entry; i++) {
        uint64_t pa = 0;
        memcpy(&pa, list + i * entry, entry);
        const struct acpi_header *t = pa ? early_at(bi, pa, sizeof(*t)) : NULL;
        if (!t || memcmp(t->signature, "DMAR", 4))
            continue;
        if (!early_at(bi, pa, t->length))
            return ERR_BAD_STATE;
        *out = t;
        return OK;
    }
    return ERR_NOT_FOUND;
}

void iommu_reserve_early(struct boot_info *bi)
{
    if (!vtd_iommu_wanted(bi->cmdline))
        return;
    const struct acpi_header *h;
    status_t st = early_dmar(bi, &h);
    if (st == ERR_BAD_STATE)
        kprintf("vtd:         RMRRs not checked before the memory managers: the ACPI tables "
                "are not where the loader maps them\n");
    if (st != OK || dmar_parse(h, h->length, &early_info) != OK)
        return;
    early_read = true;
    for (uint32_t i = 0; i < early_info.nrmrrs; i++) {
        const struct dmar_rmrr *m = &early_info.rmrrs[i];
        uint64_t bytes;
        st = vtd_rmrr_carve(bi->memmap, &bi->memmap_count, BOOT_MAX_MEMMAP, m->base, m->limit,
                            &bytes);
        if (st == ERR_NO_RESOURCES)
            kprintf("vtd:         RMRR %u: the memory map is full: not reserved\n", i);
        if (bytes)
            kprintf("vtd:         RMRR %u [%lx, %lx]: %lu KiB the memory map called usable RAM "
                    "made reserved (a device keeps using it)\n", i, m->base, m->limit,
                    bytes / 1024);
    }
}

/* ---- which unit covers which function (8.3) ------------------------------------------------ */

/* Does one of the scopes name dev: an endpoint that is dev, or a bridge
 * that is dev or has dev on a bus below it? */
static bool scopes_name(const struct dmar_info *info, const struct dmar_scopes *list,
                        uint16_t seg, struct pci_dev *dev)
{
    for (uint32_t i = 0; i < list->count; i++) {
        const struct dmar_scope *s = &info->scopes[list->first + i];
        if (s->type != DMAR_SCOPE_ENDPOINT && s->type != DMAR_SCOPE_BRIDGE)
            continue;
        int bus;
        struct pci_dev *d = vtd_scope_fn(seg, s, &bus);
        if (d == dev)
            return true;
        if (!d || s->type != DMAR_SCOPE_BRIDGE)
            continue;
        uint32_t sec = pci_cfg_read(d, PCI_SECONDARY, 1), sub = pci_cfg_read(d, PCI_SUBORDINATE, 1);
        if (dev->info.bus >= sec && dev->info.bus <= sub)
            return true;
    }
    return false;
}

/* The unit covering dev: one whose scopes name it, else the segment's
 * INCLUDE_PCI_ALL unit; -1 for none. */
static int unit_of(const struct dmar_info *info, struct pci_dev *dev)
{
    int all = -1;
    for (uint32_t i = 0; i < info->nunits; i++) {
        const struct dmar_unit *du = &info->units[i];
        if (du->segment != dev->info.segment)
            continue;
        if (du->flags & DMAR_DRHD_INCLUDE_PCI_ALL) {
            if (all < 0)
                all = (int)i;
        } else if (scopes_name(info, &du->scopes, du->segment, dev)) {
            return (int)i;
        }
    }
    return all;
}

/* ---- RMRRs ---------------------------------------------------------------------------------- */

/* Is the RMRR's range one the tables can hold? Its page count in *pages. */
static bool rmrr_sane(const struct dmar_rmrr *m, uint64_t *pages)
{
    if (m->limit < m->base || (m->base & (PAGE_SIZE - 1)) || ((m->limit + 1) & (PAGE_SIZE - 1)))
        return false;
    *pages = (m->limit - m->base + 1) >> PAGE_SHIFT;
    return *pages && *pages <= RMRR_MAX_PAGES;
}

static bool rmrr_names(const struct dmar_info *info, const struct dmar_rmrr *m,
                       const struct pci_dev *dev)
{
    if (m->segment != dev->info.segment)
        return false;
    for (uint32_t i = 0; i < m->scopes.count; i++) {
        const struct dmar_scope *s = &info->scopes[m->scopes.first + i];
        int bus;
        if (s->type == DMAR_SCOPE_ENDPOINT && vtd_scope_fn(m->segment, s, &bus) == dev)
            return true;
    }
    return false;
}

status_t vtd_boot_map_rmrrs(const struct vtd_fn *f, struct vtd_dom *d)
{
    const struct dmar_info *info = vtd_dmar_info();
    for (uint32_t i = 0; i < info->nrmrrs; i++) {
        const struct dmar_rmrr *m = &info->rmrrs[i];
        uint64_t pages;
        if (!rmrr_names(info, m, f->dev) || !rmrr_sane(m, &pages))
            continue;
        status_t st = vtd_dom_map_range(d, m->base, pages);
        if (st != OK)
            return st;
    }
    return OK;
}

/* Table pages for `pages` scattered 4 KiB pages at most, with room. */
static uint32_t tables_for(uint64_t pages)
{
    uint64_t t = pages / 512 + pages / (512 * 512) + 16;
    return t > UINT32_MAX ? UINT32_MAX : (uint32_t)t;
}

/* f's home: a boot domain with its RMRRs when one names it, else the
 * blocking domain. */
static struct vtd_dom *home_of(struct vtd_ctl *ctl, struct vtd_fn *f)
{
    const struct dmar_info *info = vtd_dmar_info();
    uint64_t pages = 0, p;
    for (uint32_t i = 0; i < info->nrmrrs; i++)
        if (rmrr_names(info, &info->rmrrs[i], f->dev)) {
            if (rmrr_sane(&info->rmrrs[i], &p))
                pages += p;
            else
                report("vtd: RMRR %u [%lx, %lx] for %02x:%02x.%x is not page-aligned or is "
                       "over 1 GiB: not mapped", i, info->rmrrs[i].base, info->rmrrs[i].limit,
                       f->sid >> 8, (f->sid >> 3) & 0x1f, f->sid & 7);
        }
    if (!pages)
        return ctl->blocking;
    struct vtd_dom *d;
    status_t st = vtd_dom_new(ctl, VTD_DOM_TABLE, NULL, tables_for(pages), "boot (RMRR)", &d);
    if (st == OK && (st = vtd_boot_map_rmrrs(f, d)) != OK)
        (void)vtd_dom_free(d);
    if (st != OK) {
        report("vtd: unit %u: no boot domain for %02x:%02x.%x's RMRR (%d): it is blocked",
               ctl->unit->index, f->sid >> 8, (f->sid >> 3) & 0x1f, f->sid & 7, st);
        return ctl->blocking;
    }
    ctl->nboot++;
    return d;
}

/* ---- the unit's tables ------------------------------------------------------------------- */

/* Without ECAP.PT: a domain mapping every page of RAM (and the unit's
 * RMRRs) at its own address, for the functions with a driver. */
static status_t make_identity(struct vtd_ctl *ctl)
{
    uint64_t max = pmm_max_pfn();
    struct vtd_dom *d = NULL;
    status_t st = vtd_dom_new(ctl, VTD_DOM_TABLE, NULL, tables_for(max), "identity (all RAM)", &d);
    for (uint64_t pfn = 1; pfn < max && st == OK;) {
        uint64_t end = pfn;
        while (end < max && pmm_range_has_ram(end << PAGE_SHIFT, PAGE_SIZE))
            end++;
        if (end > pfn)
            st = vtd_dom_map_range(d, pfn << PAGE_SHIFT, end - pfn);
        pfn = end + 1;
    }
    const struct dmar_info *info = vtd_dmar_info();
    uint16_t seg = info->units[ctl->unit->index].segment;
    for (uint32_t i = 0; i < info->nrmrrs && st == OK; i++) {
        uint64_t pages;
        if (info->rmrrs[i].segment == seg && rmrr_sane(&info->rmrrs[i], &pages))
            st = vtd_dom_map_range(d, info->rmrrs[i].base, pages);
    }
    if (st == OK)
        ctl->pass = d;
    else if (d)
        (void)vtd_dom_free(d);
    return st;
}

static status_t build_unit(struct vtd_ctl *ctl)
{
    status_t st = vtd_dom_new(ctl, VTD_DOM_TABLE, NULL, 1, "blocking", &ctl->blocking);
    if (st != OK)
        return st;
    st = vtd_dom_new(ctl, VTD_DOM_PASS, NULL, 0, "pass-through", &ctl->pass);
    if (st == ERR_NOT_SUPPORTED)
        st = make_identity(ctl);
    for (uint32_t i = 0; st == OK && vtd_fn_at(i); i++) {
        struct vtd_fn *f = vtd_fn_at(i);
        if (f->ctl != ctl)
            continue;
        f->home = home_of(ctl, f);
        st = vtd_fn_place(f, f->home);
    }
    return st;
}

/* ---- the handover ---------------------------------------------------------------------- */

/* Protected memory regions off (11.4.8.1: deprecated, and for before
 * translation), waiting for PRS to say so. Only on a unit that has them. */
static void protected_memory_off(struct vtd_unit *u)
{
    if (!VTD_CAP_PLMR(u->cap) && !VTD_CAP_PHMR(u->cap))
        return;   /* PMEN is read-only 0 then */
    uint32_t p = vtd_rd32(u, VTD_PMEN);
    if (!(p & (VTD_PMEN_EPM | VTD_PMEN_PRS)))
        return;
    /* 6.9: one command at a time, PMEN's included. 30:1 are RsvdP. */
    uint64_t f = spin_lock_irqsave(&u->gcmd_lock);
    vtd_wr32(u, VTD_PMEN, p & ~(VTD_PMEN_EPM | VTD_PMEN_PRS));
    uint64_t deadline = uptime_ns() + VTD_REG_WAIT_NS;
    while ((p = vtd_rd32(u, VTD_PMEN)) & VTD_PMEN_PRS && uptime_ns() < deadline)
        cpu_relax();
    spin_unlock_irqrestore(&u->gcmd_lock, f);
    if (p & VTD_PMEN_PRS)
        report("vtd: unit %u: the protected memory regions did not turn off (pmen %08x)",
               u->index, p);
    else
        kprintf("vtd:         unit %u: protected memory regions were on: turned off\n", u->index);
}

status_t vtd_boot_handover(struct vtd_unit *u, uint64_t root_phys)
{
    bool on = vtd_rd32(u, VTD_GSTS) & VTD_GSTS_TES;
    if (on && VTD_RTADDR_TTM(vtd_rd64(u, VTD_RTADDR)) != 0 && !VTD_CAP_ESRTPS(u->cap))
        return ERR_NOT_SUPPORTED;   /* 11.4.5: TTM can't change while translating */
    vtd_wr64(u, VTD_RTADDR, root_phys);   /* TTM 00: legacy mode */
    status_t st = vtd_gcmd(u, VTD_GCMD_SRTP, true, VTD_GSTS_RTPS);
    if (st == OK && !VTD_CAP_ESRTPS(u->cap)) {
        /* 6.6: the context cache, then the IOTLB, globally. */
        st = vtd_inv_context_global(u);
        if (st == OK)
            st = vtd_inv_iotlb_global(u);
    }
    if (st == OK && !on)
        st = vtd_gcmd(u, VTD_GCMD_TE, true, VTD_GSTS_TES);
    if (st == OK)
        protected_memory_off(u);
    return st;
}

/* ---- the boot ------------------------------------------------------------------------- */

static void log_functions(const struct vtd_ctl *ctl)
{
    for (uint32_t i = 0; vtd_fn_at(i); i++) {
        const struct vtd_fn *f = vtd_fn_at(i);
        if (f->ctl != ctl)
            continue;
        kprintf("vtd:         unit %u: %02x:%02x.%x %04x:%04x class %02x%02x: %s, domain %u\n",
                ctl->unit->index, f->sid >> 8, (f->sid >> 3) & 0x1f, f->sid & 7,
                f->dev->info.vendor, f->dev->info.device, f->dev->info.class_code,
                f->dev->info.subclass, f->home->what, f->home->ud.did);
    }
}

static void start_unit(struct vtd_ctl *ctl)
{
    struct vtd_unit *u = ctl->unit;
    bool was_on = vtd_rd32(u, VTD_GSTS) & VTD_GSTS_TES;
    uint64_t was_root = vtd_rd64(u, VTD_RTADDR);
    status_t st = build_unit(ctl);
    if (st == OK)
        st = vtd_boot_handover(u, ctl->root_phys);
    if (st != OK) {
        report("vtd: unit %u: translation %s (%d): %s", u->index,
               st == ERR_NOT_SUPPORTED ? "was on in a mode other than legacy" : "not turned on",
               st, was_on ? "left as the firmware had it" : "left off");
        return;
    }
    __atomic_store_n(&ctl->live, true, __ATOMIC_RELEASE);
    log_functions(ctl);
    kprintf("vtd:         unit %u: translation on (%s): %u function%s, %u with an RMRR boot "
            "domain, the rest blocked; driven ones go to domain %u (%s)\n", u->index,
            was_on ? "it was on: taken over" : "it was off", ctl->nfn, ctl->nfn == 1 ? "" : "s",
            ctl->nboot, ctl->pass->ud.did, ctl->pass->what);
    if (was_on)
        kprintf("vtd:         unit %u: the old root table was %lx (ttm %lu)\n", u->index,
                was_root & ~(uint64_t)0xfff, VTD_RTADDR_TTM(was_root));
}

/* Each function onto the unit that covers it; how many no unit covers. */
static uint32_t assign_functions(const struct dmar_info *info)
{
    uint32_t none = 0;
    for (uint32_t i = 0; vtd_fn_at(i); i++) {
        struct vtd_fn *f = vtd_fn_at(i);
        int k = unit_of(info, f->dev);
        f->ctl = k >= 0 ? vtd_ctl_get((uint32_t)k) : NULL;
        if (f->ctl)
            f->ctl->nfn++;
        else
            none++;
    }
    return none;
}

/* RMRRs in RAM the early pass couldn't take out of the memory map. */
static void check_rmrrs(const struct dmar_info *info)
{
    if (early_read)
        return;
    for (uint32_t i = 0; i < info->nrmrrs; i++) {
        const struct dmar_rmrr *m = &info->rmrrs[i];
        uint64_t pages;
        if (rmrr_sane(m, &pages) && pmm_range_has_ram(m->base, pages << PAGE_SHIFT))
            report("vtd: RMRR %u [%lx, %lx] lies in RAM and was not reserved at boot: the "
                   "allocator may hand its pages out", i, m->base, m->limit);
    }
}

void iommu_boot(void)
{
    const struct dmar_info *info = vtd_dmar_info();
    if (!vtd_iommu_wanted(cmdline_get()) || !info->nunits)
        return;
    uint32_t n = 0;
    status_t st = vtd_fns_init();
    for (uint32_t i = 0; i < VTD_MAX_UNITS && st == OK; i++) {
        struct vtd_unit *u = vtd_unit_get(i);
        if (u && (st = vtd_ctl_init(u)) == OK)
            n++;
    }
    if (st != OK) {
        report("vtd: no memory for the remapping tables: translation stays off");
        return;
    }
    uint32_t none = assign_functions(info);
    for (uint32_t i = 0; i < VTD_MAX_UNITS; i++)
        if (vtd_ctl_get(i))
            start_unit(vtd_ctl_get(i));
    check_rmrrs(info);
    if (none)
        kprintf("vtd:         %u PCI function%s no started unit covers: not remapped\n", none,
                none == 1 ? "" : "s");
    if (!n)
        kprintf("vtd:         no started unit: translation stays off\n");
}

/* ---- the jump ------------------------------------------------------------------------- */

/* Has a wait at the jump that started at `start` (the TSC) run out? No
 * lock, no clock but the TSC; before it is calibrated, a spin count. */
static bool jump_wait_over(uint64_t start, uint32_t spins)
{
    if (!tsc_hz)
        return spins > 10000000u;
    return rdtsc() - start > JUMP_WAIT_MS * (tsc_hz / 1000);
}

/* One Global Command turning `cmd` off (11.4.4.1's steps) if `status`
 * says it is on. */
static void command_off(struct vtd_unit *u, uint32_t cmd, uint32_t status)
{
    uint32_t g = vtd_rd32(u, VTD_GSTS);
    if (!(g & status))
        return;
    vtd_wr32(u, VTD_GCMD, (g & VTD_GSTS_KEEP) & ~cmd);
    /* A unit that doesn't answer is left: the next kernel takes it over. */
    uint64_t start = rdtsc();
    for (uint32_t spins = 0; (vtd_rd32(u, VTD_GSTS) & status) && !jump_wait_over(start, spins);
         spins++)
        cpu_relax();
}

void iommu_jump_off(void)
{
    for (uint32_t i = 0; i < VTD_MAX_UNITS; i++) {
        struct vtd_unit *u = vtd_unit_get(i);
        if (!u)
            continue;
        vtd_fault_mask(u);
        command_off(u, VTD_GCMD_IRE, VTD_GSTS_IRES);
        command_off(u, VTD_GCMD_TE, VTD_GSTS_TES);
        /* 6.5.2: the queue must be empty before it goes off. */
        uint64_t start = rdtsc();
        for (uint32_t spins = 0;
             vtd_rd64(u, VTD_IQH) != vtd_rd64(u, VTD_IQT) && !jump_wait_over(start, spins); spins++)
            cpu_relax();
        if (vtd_rd64(u, VTD_IQH) == vtd_rd64(u, VTD_IQT))
            command_off(u, VTD_GCMD_QIE, VTD_GSTS_QIES);
    }
}
