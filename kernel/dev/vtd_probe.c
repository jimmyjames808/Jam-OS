/* The boot-time VT-d probe (see <jam/vtd.h>): logs the DMAR table and each
 * remapping unit's registers, decoded, so the real PC's IOMMU can be
 * planned from facts. It READS ONLY: no VT-d register is written, nothing
 * the firmware left on is turned off, no PCI register is written. The one
 * thing it changes is the kernel's own page tables: each unit's register
 * page is mapped uncached (vmm_map_mmio), and stays mapped for the IOMMU:
 * the mapping and the registers' range go to vtd_unit.c (vtd_unit_found,
 * vtd_unit_mapped), which starts the units only with `iommu=on`.
 *
 * Every line starts "vtd:" (grep 'vtd:' in a boot log). In order: the
 * table's header, each unit with its device scopes, each reserved memory
 * region (RMRR), ATS root port set (ATSR), SoC ATC set (SATC) and ACPI
 * namespace device (ANDD); then per unit its version, capabilities,
 * status and any fault it has recorded; then which PCI functions had bus
 * mastering on when the probe ran (the firmware's choice, before any
 * driver); then the summary: what the firmware left on. Anything the
 * firmware left on goes in the RESULTS box too (report). */
#include <stdarg.h>
#include <jam/acpi.h>
#include <jam/dmar.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/report.h>
#include <jam/string.h>
#include <jam/vtd.h>

#include "vtd_internal.h"

#define PCI_COMMAND     0x04
#define PCI_CMD_MASTER  (1u << 2)
#define PCI_SECONDARY   0x19

#define MAX_REG_PAGES   16   /* a register set is at most this many pages here */
#define MAX_FAULT_RECS  8    /* fault records read per unit */

static struct dmar_info info;   /* the parsed table; written once, at boot */

/* What the firmware left on, over all units, for the summary. */
static struct {
    uint32_t units_read;     /* units whose registers answered */
    uint32_t translating;    /* GSTS.TES set */
    uint32_t remapping;      /* GSTS.IRES set */
    uint32_t qi_on;          /* GSTS.QIES set */
    uint32_t protected_mem;  /* PMEN.EPM or PRS set */
    uint32_t faults;         /* units whose FSTS shows a recorded or lost fault */
} seen;

/* ---- building a line --------------------------------------------------------- */

struct line {
    char  *buf;    /* where the text goes */
    size_t size;   /* its size, NUL included */
    size_t len;    /* chars written so far (never past size - 1) */
};

static void add(struct line *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void add(struct line *l, const char *fmt, ...)
{
    if (l->len + 1 >= l->size)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(l->buf + l->len, l->size - l->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        l->len += (size_t)n < l->size - l->len ? (size_t)n : l->size - l->len - 1;
}

/* ---- decoding (pure) ----------------------------------------------------------- */

void vtd_describe_caps(char *buf, size_t n, uint64_t cap, uint64_t ecap)
{
    struct line l = { buf, n, 0 };
    if (n)
        buf[0] = '\0';
    add(&l, "domains %u, levels", 1u << (4 + 2 * (unsigned)VTD_CAP_ND(cap)));
    uint64_t sagaw = VTD_CAP_SAGAW(cap);
    for (unsigned b = 1; b <= 3; b++)
        if (sagaw & (1u << b))
            add(&l, " %u(%u-bit)", b + 2, 30 + 9 * b);
    if (!(sagaw & 0xe))
        add(&l, " none(sagaw %lx)", sagaw);
    add(&l, ", mgaw %u bits, cm %u, rwbf %u, superpages %s%s, psi %u (mamv %u), "
            "fault records %u at %lx, drain r%u w%u, plmr %u phmr %u, pi %u; "
            "coherent %u, qi %u, ir %u, eim %u, pt %u, sc %u, dt %u, iotlb at %lx, "
            "scalable %u, pasid %u, nest %u",
        (unsigned)VTD_CAP_MGAW(cap) + 1, (unsigned)VTD_CAP_CM(cap),
        (unsigned)VTD_CAP_RWBF(cap), (VTD_CAP_SLLPS(cap) & 1) ? "2M" : "-",
        (VTD_CAP_SLLPS(cap) & 2) ? "+1G" : "", (unsigned)VTD_CAP_PSI(cap),
        (unsigned)VTD_CAP_MAMV(cap), (unsigned)VTD_CAP_NFR(cap) + 1, VTD_CAP_FRO(cap) * 16,
        (unsigned)VTD_CAP_DRD(cap), (unsigned)VTD_CAP_DWD(cap), (unsigned)VTD_CAP_PLMR(cap),
        (unsigned)VTD_CAP_PHMR(cap), (unsigned)VTD_CAP_PI(cap), (unsigned)VTD_ECAP_C(ecap),
        (unsigned)VTD_ECAP_QI(ecap), (unsigned)VTD_ECAP_IR(ecap), (unsigned)VTD_ECAP_EIM(ecap),
        (unsigned)VTD_ECAP_PT(ecap), (unsigned)VTD_ECAP_SC(ecap), (unsigned)VTD_ECAP_DT(ecap),
        VTD_ECAP_IRO(ecap) * 16, (unsigned)VTD_ECAP_SMTS(ecap), (unsigned)VTD_ECAP_PASID(ecap),
        (unsigned)VTD_ECAP_NEST(ecap));
}

void vtd_describe_status(char *buf, size_t n, uint32_t gsts, uint32_t pmen, uint32_t fsts)
{
    struct line l = { buf, n, 0 };
    if (n)
        buf[0] = '\0';
    add(&l, "translation %s, interrupt remapping %s, queued invalidation %s, "
            "root table %s, irq table %s, compat irqs %s, protected memory %s, faults %s",
        (gsts & VTD_GSTS_TES) ? "ON" : "off", (gsts & VTD_GSTS_IRES) ? "ON" : "off",
        (gsts & VTD_GSTS_QIES) ? "ON" : "off", (gsts & VTD_GSTS_RTPS) ? "set" : "not set",
        (gsts & VTD_GSTS_IRTPS) ? "set" : "not set", (gsts & VTD_GSTS_CFIS) ? "pass" : "-",
        (pmen & (VTD_PMEN_EPM | VTD_PMEN_PRS)) ? "ON" : "off",
        (fsts & (VTD_FSTS_PPF | VTD_FSTS_PFO)) ? "RECORDED" : "none");
    if (fsts & (VTD_FSTS_IQE | VTD_FSTS_ICE | VTD_FSTS_ITE))
        add(&l, ", invalidation errors %x", fsts & (VTD_FSTS_IQE | VTD_FSTS_ICE | VTD_FSTS_ITE));
}

void vtd_describe_fault(char *buf, size_t n, uint64_t lo, uint64_t hi)
{
    struct line l = { buf, n, 0 };
    if (n)
        buf[0] = '\0';
    uint32_t sid = (uint32_t)VTD_FRCD_SID(hi);
    uint64_t reason = VTD_FRCD_REASON(hi);
    add(&l, "%02x:%02x.%x ", sid >> 8, (sid >> 3) & 0x1f, sid & 7);
    if (reason == VTD_FRCD_IR_COMPAT)
        add(&l, "interrupt in compatibility format");
    else if (reason >= VTD_FRCD_IR_FIRST && reason <= VTD_FRCD_IR_LAST)
        add(&l, "interrupt, index %lx", VTD_FRCD_INDEX(lo));
    else
        add(&l, "%s at %lx",
            VTD_FRCD_TYPE1(hi) ? (VTD_FRCD_TYPE2(hi) ? "atomic" : "read")
                               : (VTD_FRCD_TYPE2(hi) ? "page request" : "write"),
            lo & ~(uint64_t)0xfff);
    add(&l, ", reason %lx", reason);
}

/* ---- the table ------------------------------------------------------------------- */

/* The enumerated function at seg:bus:dev.fn, or NULL. */
static struct pci_dev *find_fn(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn)
{
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->info.segment == seg && d->info.bus == bus && d->info.dev == dev &&
            d->info.fn == fn)
            return d;
    }
    return NULL;
}

/* Follow a scope's path through bridges (each step but the last is a
 * bridge whose secondary bus the next step is on). The function it names,
 * or NULL (not present: a disabled device, or a path that leads nowhere);
 * *bus gets the last step's bus either way, or -1 if a bridge was missing. */
static struct pci_dev *scope_fn(uint16_t seg, const struct dmar_scope *s, int *bus)
{
    uint8_t b = s->start_bus;
    *bus = -1;
    if (!s->path_len || s->path_len != s->path_full)
        return NULL;
    for (uint32_t i = 0; i + 1 < s->path_len; i++) {
        struct pci_dev *br = find_fn(seg, b, s->path[i].dev, s->path[i].fn);
        if (!br || !(br->info.flags & PCI_INFO_BRIDGE))
            return NULL;
        b = (uint8_t)pci_cfg_read(br, PCI_SECONDARY, 1);
    }
    *bus = b;
    return find_fn(seg, b, s->path[s->path_len - 1].dev, s->path[s->path_len - 1].fn);
}

/* An I/O APIC's or HPET's scope names the requester id its interrupts
 * carry (bus, device, function), not a PCI function: said as such, and an
 * I/O APIC matched with the MADT's by its id. */
static void add_source_id(struct line *l, const struct dmar_scope *s)
{
    if (s->path_len != 1 || s->path_full != 1) {
        add(l, ", a path of %u steps (one expected)", s->path_full);
        return;
    }
    add(l, ", requester id %02x:%02x.%x", s->start_bus, s->path[0].dev, s->path[0].fn);
    if (s->type != DMAR_SCOPE_IOAPIC)
        return;
    for (uint32_t i = 0; i < acpi.ioapic_count && i < ACPI_MAX_IOAPICS; i++)
        if (acpi.ioapics[i].id == s->enum_id) {
            add(l, " (the MADT's I/O APIC at %lx, GSIs from %u)", acpi.ioapics[i].phys,
                acpi.ioapics[i].gsi_base);
            return;
        }
    add(l, " (NO I/O APIC with this id in the MADT)");
}

/* A PCI scope: its path, and the function it leads to. */
static void add_pci_path(struct line *l, uint16_t seg, const struct dmar_scope *s)
{
    add(l, ", bus %02x path", s->start_bus);
    for (uint32_t i = 0; i < s->path_len; i++)
        add(l, " %02x.%x", s->path[i].dev, s->path[i].fn);
    if (s->path_full > s->path_len)
        add(l, " ... (%u steps: too long to follow)", s->path_full);
    int bus;
    struct pci_dev *d = scope_fn(seg, s, &bus);
    if (d)
        add(l, " = %02x:%02x.%x %04x:%04x class %02x%02x", d->info.bus, d->info.dev,
            d->info.fn, d->info.vendor, d->info.device, d->info.class_code, d->info.subclass);
    else if (bus >= 0 && s->path_len)
        add(l, " = %02x:%02x.%x, not present", bus, s->path[s->path_len - 1].dev,
            s->path[s->path_len - 1].fn);
    else
        add(l, ", leads to no function (a bridge on the way is missing)");
}

/* One line per scope of a structure: "<what> <n> scope <i>: ...". */
static void log_scopes(const char *what, uint32_t n, uint16_t seg,
                       const struct dmar_scopes *list)
{
    char buf[256];
    for (uint32_t i = 0; i < list->count; i++) {
        const struct dmar_scope *s = &info.scopes[list->first + i];
        struct line l = { buf, sizeof(buf), 0 };
        add(&l, "vtd:           %s %u scope %u: %s", what, n, i, dmar_scope_name(s->type));
        if (s->type == DMAR_SCOPE_IOAPIC || s->type == DMAR_SCOPE_HPET) {
            add(&l, " %u", s->enum_id);
            add_source_id(&l, s);
        } else if (s->type == DMAR_SCOPE_ACPI) {
            add(&l, " %u, requester id %02x:%02x.%x", s->enum_id, s->start_bus,
                s->path_len ? s->path[0].dev : 0, s->path_len ? s->path[0].fn : 0);
        } else {
            add_pci_path(&l, seg, s);
        }
        kprintf("%s\n", buf);
    }
}

static void log_units(void)
{
    for (uint32_t i = 0; i < info.nunits; i++) {
        const struct dmar_unit *u = &info.units[i];
        kprintf("vtd:         unit %u: registers %lx (%u page%s), segment %u%s, %u scope%s\n", i,
                u->base, 1u << u->size, u->size ? "s" : "", u->segment,
                (u->flags & DMAR_DRHD_INCLUDE_PCI_ALL) ? ", INCLUDE_PCI_ALL" : "",
                u->scopes.count, u->scopes.count == 1 ? "" : "s");
        log_scopes("unit", i, u->segment, &u->scopes);
    }
}

static void log_rmrrs(void)
{
    for (uint32_t i = 0; i < info.nrmrrs; i++) {
        const struct dmar_rmrr *r = &info.rmrrs[i];
        bool sane = r->limit >= r->base && r->limit != UINT64_MAX;
        uint64_t len = sane ? r->limit - r->base + 1 : 0;
        kprintf("vtd:         RMRR %u: [%lx, %lx] (%lu KiB), segment %u, %s\n", i, r->base,
                r->limit, (len + 1023) / 1024, r->segment,
                !sane                              ? "LIMIT BELOW BASE"
                : pmm_range_has_ram(r->base, len) ? "IN RAM Jam OS manages"
                                                  : "not RAM Jam OS manages");
        log_scopes("RMRR", i, r->segment, &r->scopes);
    }
}

static void log_others(void)
{
    for (uint32_t k = 0; k < 2; k++) {
        const struct dmar_portset *set = k ? info.satcs : info.atsrs;
        uint32_t n = k ? info.nsatcs : info.natsrs;
        const char *what = k ? "SATC" : "ATSR";
        for (uint32_t i = 0; i < n; i++) {
            kprintf("vtd:         %s %u: segment %u, flags %x (%s)\n", what, i, set[i].segment,
                    set[i].flags,
                    k ? ((set[i].flags & DMAR_SATC_ATC_REQUIRED) ? "ATC required" : "ATC optional")
                      : ((set[i].flags & DMAR_ATSR_ALL_PORTS) ? "all ports" : "listed ports"));
            log_scopes(what, i, set[i].segment, &set[i].scopes);
        }
    }
    for (uint32_t i = 0; i < info.nandds; i++)
        kprintf("vtd:         ANDD %u: ACPI device %u is \"%s\"\n", i, info.andds[i].dev_num,
                info.andds[i].name);
}

static void log_table(const struct acpi_header *h)
{
    status_t st = dmar_parse(h, h->length, &info);
    if (st != OK) {
        report("vtd: the DMAR table is not valid (signature, length or checksum)");
        return;
    }
    kprintf("vtd:         DMAR rev %u, %u bytes, host address width %u bits, flags %x "
            "(interrupt remapping %s, x2APIC opt-out %s, DMA control opt-in %s); %u unit%s, "
            "%u RMRR, %u ATSR, %u SATC, %u ANDD, %u RHSA\n", info.revision, h->length, info.haw,
            info.flags, (info.flags & DMAR_F_INTR_REMAP) ? "yes" : "no",
            (info.flags & DMAR_F_X2APIC_OPT_OUT) ? "YES" : "no",
            (info.flags & DMAR_F_DMA_CTRL_OPT_IN) ? "yes" : "no", info.nunits,
            info.nunits == 1 ? "" : "s", info.nrmrrs, info.natsrs, info.nsatcs, info.nandds,
            info.nrhsas);
    if (info.malformed)
        report("vtd: DMAR structure at byte %u has an impossible length: the rest is skipped",
               info.malformed_at);
    if (info.unknown || info.dropped)
        kprintf("vtd:         %u structure%s of unknown type skipped, %u entr%s past the "
                "parser's limits not kept\n", info.unknown, info.unknown == 1 ? "" : "s",
                info.dropped, info.dropped == 1 ? "y" : "ies");
    log_units();
    log_rmrrs();
    log_others();
}

/* ---- the registers ----------------------------------------------------------------- */

static uint32_t rd32(volatile uint8_t *r, uint32_t off)
{
    return *(volatile uint32_t *)(r + off);
}

static uint64_t rd64(volatile uint8_t *r, uint32_t off)
{
    return *(volatile uint64_t *)(r + off);
}

/* The faults the unit has recorded (left from the firmware's time, or a
 * kexec'd kernel's), read and logged; nothing is cleared. */
static void log_faults(uint32_t idx, volatile uint8_t *r, uint64_t cap, uint64_t span)
{
    uint64_t fro = VTD_CAP_FRO(cap) * 16;
    uint32_t nfr = (uint32_t)VTD_CAP_NFR(cap) + 1;
    for (uint32_t i = 0; i < nfr && i < MAX_FAULT_RECS; i++) {
        uint64_t off = fro + 16ull * i;
        if (off + 16 > span)
            return;
        uint64_t hi = rd64(r, (uint32_t)off + 8);
        if (!(hi & VTD_FRCD_F))
            continue;
        char buf[96];
        vtd_describe_fault(buf, sizeof(buf), rd64(r, (uint32_t)off), hi);
        kprintf("vtd:         unit %u fault record %u: %s\n", idx, i, buf);
    }
}

/* Is a unit's register set somewhere the kernel may map as MMIO? */
static bool regs_mappable(const struct dmar_unit *u, uint64_t span)
{
    return u->base && !(u->base & (PAGE_SIZE - 1)) && u->base + span > u->base &&
           !pmm_range_has_ram(u->base, span);
}

static void probe_unit(uint32_t idx, const struct dmar_unit *u)
{
    uint64_t pages = 1ull << u->size;
    uint64_t span = (pages > MAX_REG_PAGES ? MAX_REG_PAGES : pages) * PAGE_SIZE;
    if (!regs_mappable(u, span)) {
        report("vtd: unit %u's registers at %lx are not MMIO Jam OS can map: not read", idx,
               u->base);
        return;
    }
    volatile uint8_t *r = vmm_map_mmio(u->base, span);
    uint32_t ver = rd32(r, VTD_VER);
    if (ver == 0xffffffffu) {
        report("vtd: unit %u at %lx does not answer (version reads all ones)", idx, u->base);
        return;
    }
    seen.units_read++;
    vtd_unit_mapped(idx, r, span, info.haw);
    uint64_t cap = rd64(r, VTD_CAP), ecap = rd64(r, VTD_ECAP);
    uint32_t gsts = rd32(r, VTD_GSTS), pmen = rd32(r, VTD_PMEN), fsts = rd32(r, VTD_FSTS);
    char buf[480];
    vtd_describe_caps(buf, sizeof(buf), cap, ecap);
    kprintf("vtd:         unit %u: version %u.%u, cap %lx ecap %lx\n", idx, (ver >> 4) & 0xf,
            ver & 0xf, cap, ecap);
    kprintf("vtd:         unit %u caps: %s\n", idx, buf);
    vtd_describe_status(buf, sizeof(buf), gsts, pmen, fsts);
    kprintf("vtd:         unit %u status: %s\n", idx, buf);
    kprintf("vtd:         unit %u pointers: gsts %08x rtaddr %lx irta %lx iqa %lx pmen %08x "
            "fsts %08x fectl %08x\n", idx, gsts, rd64(r, VTD_RTADDR), rd64(r, VTD_IRTA),
            rd64(r, VTD_IQA), pmen, fsts, rd32(r, VTD_FECTL));
    seen.translating += !!(gsts & VTD_GSTS_TES);
    seen.remapping += !!(gsts & VTD_GSTS_IRES);
    seen.qi_on += !!(gsts & VTD_GSTS_QIES);
    seen.protected_mem += !!(pmen & (VTD_PMEN_EPM | VTD_PMEN_PRS));
    seen.faults += !!(fsts & (VTD_FSTS_PPF | VTD_FSTS_PFO));
    log_faults(idx, r, cap, span);
}

/* ---- the machine around it ------------------------------------------------------- */

/* Which functions had bus mastering on (nothing in Jam OS has turned it on
 * yet at this point of the boot: the firmware's, or a kexec'd kernel's). */
static void log_bus_masters(void)
{
    char buf[480];
    struct line l = { buf, sizeof(buf), 0 };
    uint32_t n = 0;
    add(&l, "vtd:         bus mastering on at the probe:");
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        uint32_t cmd = pci_cfg_read(d, PCI_COMMAND, 2);
        if (cmd == 0xffff || !(cmd & PCI_CMD_MASTER))
            continue;
        add(&l, " %02x:%02x.%x%s", d->info.bus, d->info.dev, d->info.fn,
            (d->info.flags & PCI_INFO_BRIDGE) ? "(bridge)" : "");
        n++;
    }
    kprintf("%s%s\n", buf, n ? "" : " none");
}

static void log_apic(void)
{
    uint32_t max_id = 0;
    for (uint32_t i = 0; i < acpi.cpu_count; i++)
        if (acpi.cpus[i].apic_id > max_id)
            max_id = acpi.cpus[i].apic_id;
    kprintf("vtd:         CPUs: highest APIC id %u (%s); without interrupt remapping an MSI "
            "names 8-bit APIC ids only\n", max_id,
            max_id > 255 ? "ABOVE 255: needs remapping with x2APIC ids (eim)" : "all below 256");
}

static void log_summary(void)
{
    kprintf("vtd:         handover: %u of %u unit%s answered; translation on in %u, interrupt "
            "remapping on in %u, queued invalidation on in %u, protected memory on in %u, "
            "faults recorded in %u; the probe changed none of it\n", seen.units_read,
            info.nunits, info.nunits == 1 ? "" : "s", seen.translating, seen.remapping,
            seen.qi_on, seen.protected_mem, seen.faults);
    if (seen.translating || seen.remapping || seen.protected_mem)
        report("vtd: the firmware left VT-d on: translation in %u unit%s, interrupt "
               "remapping in %u, protected memory in %u (left as it was)", seen.translating,
               seen.translating == 1 ? "" : "s", seen.remapping, seen.protected_mem);
}

void vtd_probe(void)
{
    const struct acpi_header *h = acpi_find("DMAR", 0);
    if (!h) {
        kprintf("vtd:         no DMAR table: the firmware reports no VT-d remapping hardware\n");
        return;
    }
    log_table(h);
    for (uint32_t i = 0; i < info.nunits; i++)
        vtd_unit_found(i, info.units[i].base, (1ull << info.units[i].size) * PAGE_SIZE);
    for (uint32_t i = 0; i < info.nunits; i++)
        probe_unit(i, &info.units[i]);
    log_bus_masters();
    log_apic();
    log_summary();
}
