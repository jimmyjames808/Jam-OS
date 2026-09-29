/* Finds and parses the static ACPI tables: MADT (CPUs, I/O APICs, interrupt
 * overrides), FADT (PM timer, SCI), HPET and MCFG. Tables can sit in memory
 * the kernel's HHDM does not cover (firmware "reserved"), so every access
 * goes through acpi_map. */
#include <jam/acpi.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/string.h>

struct acpi_info acpi;

struct __attribute__((packed)) rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
};

#define MAX_TABLES 64
static uint64_t tables[MAX_TABLES];
static unsigned table_count;

void *acpi_map(uint64_t phys, uint64_t len)
{
    uint64_t pml4 = vmm_kernel_pml4();
    for (uint64_t pa = ALIGN_DOWN(phys, PAGE_SIZE); pa < phys + len; pa += PAGE_SIZE) {
        uint64_t va = pa + hhdm_offset;
        if (vmm_translate(pml4, va) == UINT64_MAX)
            vmm_map(pml4, va, pa, PAGE_SIZE, VM_GLOBAL | VM_SMALL);
    }
    return phys_to_virt(phys);
}

static bool checksum_ok(const void *p, uint32_t len)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum += ((const uint8_t *)p)[i];
    return sum == 0;
}

static const struct acpi_header *map_table(uint64_t phys)
{
    const struct acpi_header *h = acpi_map(phys, sizeof(*h));
    return acpi_map(phys, h->length);
}

const struct acpi_header *acpi_find(const char sig[4], unsigned n)
{
    for (unsigned i = 0; i < table_count; i++) {
        const struct acpi_header *h = map_table(tables[i]);
        if (!memcmp(h->signature, sig, 4) && n-- == 0)
            return h;
    }
    return NULL;
}

/* ---- MADT ------------------------------------------------------------- */

struct __attribute__((packed)) madt {
    struct acpi_header h;
    uint32_t lapic_addr;
    uint32_t flags;
    uint8_t  entries[];
};

static void add_cpu(uint32_t uid, uint32_t apic_id, uint32_t flags)
{
    /* Bit 0 = enabled, bit 1 = online-capable (hot-pluggable). */
    if (!(flags & 3) || acpi.cpu_count == ACPI_MAX_CPUS)
        return;
    for (uint32_t i = 0; i < acpi.cpu_count; i++)
        if (acpi.cpus[i].apic_id == apic_id)
            return;   /* listed as both LAPIC and x2APIC */
    acpi.cpus[acpi.cpu_count++] = (typeof(acpi.cpus[0])){ uid, apic_id, flags & 1 };
}

static void parse_madt(const struct madt *m)
{
    acpi.lapic_phys = m->lapic_addr;
    acpi.pcat_compat = m->flags & 1;

    const uint8_t *p = m->entries, *end = (const uint8_t *)m + m->h.length;
    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        switch (p[0]) {
        case 0:   /* processor local APIC */
            add_cpu(p[2], p[3], *(const uint32_t *)(p + 4));
            break;
        case 1:   /* I/O APIC */
            if (acpi.ioapic_count < ACPI_MAX_IOAPICS)
                acpi.ioapics[acpi.ioapic_count++] = (typeof(acpi.ioapics[0])){
                    p[2], *(const uint32_t *)(p + 4), *(const uint32_t *)(p + 8) };
            break;
        case 2:   /* interrupt source override */
            if (acpi.iso_count < ACPI_MAX_ISOS)
                acpi.isos[acpi.iso_count++] = (typeof(acpi.isos[0])){
                    p[3], *(const uint32_t *)(p + 4), *(const uint16_t *)(p + 8) };
            break;
        case 4:   /* local APIC NMI */
            if (acpi.nmi_count < ACPI_MAX_NMIS)
                acpi.nmis[acpi.nmi_count++] = (typeof(acpi.nmis[0])){
                    p[2] == 0xff ? 0xffffffff : p[2], p[5], *(const uint16_t *)(p + 3) };
            break;
        case 5:   /* 64-bit local APIC address override */
            acpi.lapic_phys = *(const uint64_t *)(p + 4);
            break;
        case 9:   /* processor local x2APIC */
            add_cpu(*(const uint32_t *)(p + 12), *(const uint32_t *)(p + 4),
                    *(const uint32_t *)(p + 8));
            break;
        case 10:  /* local x2APIC NMI */
            if (acpi.nmi_count < ACPI_MAX_NMIS)
                acpi.nmis[acpi.nmi_count++] = (typeof(acpi.nmis[0])){
                    *(const uint32_t *)(p + 4), p[8], *(const uint16_t *)(p + 2) };
            break;
        }
        p += p[1];
    }
}

/* ---- FADT / HPET / MCFG -------------------------------------------------- */

static void parse_fadt(const struct acpi_header *h)
{
    const uint8_t *f = (const uint8_t *)h;
    acpi.has_fadt = true;
    acpi.sci_irq = *(const uint16_t *)(f + 46);
    acpi.pm_timer_port = *(const uint32_t *)(f + 76);
    uint32_t flags = *(const uint32_t *)(f + 112);
    acpi.pm_timer_32bit = flags & (1u << 8);   /* TMR_VAL_EXT */
    if (h->length >= 111)
        acpi.boot_arch_flags = *(const uint16_t *)(f + 109);
    /* M7: RESET_REG (offset 116, a GAS) and RESET_VALUE (128), valid when
     * flags.RESET_REG_SUP (bit 10) is set. */
    if (h->length >= 129 && (flags & (1u << 10))) {
        acpi.reset_reg = *(const struct acpi_gas *)(f + 116);
        acpi.reset_value = f[128];
        acpi.has_reset_reg = acpi.reset_reg.address != 0;
    }
    /* ACPI 2.0+: X_PM_TMR_BLK wins when it is an I/O port. */
    if (h->length >= 208 + 12) {
        const struct acpi_gas *x = (const struct acpi_gas *)(f + 208);
        if (x->address && x->space == 1)
            acpi.pm_timer_port = (uint32_t)x->address;
    }
}

static void parse_hpet(const struct acpi_header *h)
{
    const struct acpi_gas *base = (const struct acpi_gas *)((const uint8_t *)h + 40);
    if (base->space == 0)
        acpi.hpet_phys = base->address;
}

static void parse_mcfg(const struct acpi_header *h)
{
    const uint8_t *p = (const uint8_t *)h + 44;
    for (; p + 16 <= (const uint8_t *)h + h->length && acpi.ecam_count < ACPI_MAX_ECAM; p += 16)
        acpi.ecam[acpi.ecam_count++] = (typeof(acpi.ecam[0])){
            *(const uint64_t *)p, *(const uint16_t *)(p + 8), p[10], p[11] };
}

/* ---- entry ------------------------------------------------------------ */

void acpi_init(uint64_t rsdp_phys)
{
    if (!rsdp_phys)
        panic("acpi: no RSDP; Jam OS needs ACPI");

    const struct rsdp *r = acpi_map(rsdp_phys, sizeof(struct rsdp));
    if (memcmp(r->signature, "RSD PTR ", 8) || !checksum_ok(r, 20))
        panic("acpi: bad RSDP at %lx", rsdp_phys);
    acpi.revision = r->revision;
    memcpy(acpi.oem_id, r->oem_id, 6);

    bool xsdt = r->revision >= 2 && r->xsdt;
    const struct acpi_header *root = map_table(xsdt ? r->xsdt : r->rsdt);
    if (!checksum_ok(root, root->length))
        panic("acpi: bad %s checksum", xsdt ? "XSDT" : "RSDT");

    unsigned entry = xsdt ? 8 : 4;
    unsigned n = (root->length - sizeof(*root)) / entry;
    const uint8_t *list = (const uint8_t *)(root + 1);
    kprintf("acpi:        rev %u, OEM \"%s\", %s with %u tables:", acpi.revision,
            acpi.oem_id, xsdt ? "XSDT" : "RSDT", n);
    for (unsigned i = 0; i < n && table_count < MAX_TABLES; i++) {
        uint64_t pa = xsdt ? *(const uint64_t *)(list + i * 8) : *(const uint32_t *)(list + i * 4);
        if (!pa)
            continue;
        const struct acpi_header *h = map_table(pa);
        bool ok = checksum_ok(h, h->length);
        char sig[5] = { h->signature[0], h->signature[1], h->signature[2], h->signature[3], 0 };
        kprintf(" %s%s", sig, ok ? "" : "(bad)");
        if (ok)
            tables[table_count++] = pa;
    }
    kprintf("\n");

    const struct acpi_header *h;
    if (!(h = acpi_find("APIC", 0)))
        panic("acpi: no MADT");
    parse_madt((const struct madt *)h);
    if ((h = acpi_find("FACP", 0)))
        parse_fadt(h);
    if ((h = acpi_find("HPET", 0)))
        parse_hpet(h);
    if ((h = acpi_find("MCFG", 0)))
        parse_mcfg(h);

    kprintf("acpi:        %u CPUs, %u I/O APIC(s), %u overrides, %u NMI pins, lapic %lx%s\n",
            acpi.cpu_count, acpi.ioapic_count, acpi.iso_count, acpi.nmi_count,
            acpi.lapic_phys, acpi.pcat_compat ? ", legacy PICs" : "");
    kprintf("acpi:        HPET %s, PM timer %s", acpi.hpet_phys ? "yes" : "no",
            acpi.pm_timer_port ? "yes" : "no");
    for (uint32_t i = 0; i < acpi.ecam_count; i++)
        kprintf(", ECAM seg %u buses %u-%u @ %lx", acpi.ecam[i].segment,
                acpi.ecam[i].bus_start, acpi.ecam[i].bus_end, acpi.ecam[i].phys);
    kprintf("\n");
}
