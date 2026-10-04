/* Interrupt remapping on the VT-d units (see vtd_irq.h for the model, and
 * <jam/irq_remap.h> for what the rest of the kernel calls): the shared
 * table, turning remapping on at boot, the entries for devices and for the
 * I/O APIC's pins, and turning it off before a kexec.
 *
 * Turning it on (VT-d 4.1, 6.7 and 11.4.4.1), once, at boot:
 *   1. the table, cleared and flushed (vtd_ir_table_init);
 *   2. on every unit: IRTA (the table, its size, EIME), the Set Interrupt
 *      Remap Table Pointer command, then a global interrupt entry cache
 *      invalidation (6.7: needed unless CAP.ESIRTPS; done always). A unit
 *      that has remapping on already (a kexec'd kernel left it, or the
 *      firmware) gets its pointer replaced while on, which 6.7 allows: no
 *      interrupt is in flight (every I/O APIC pin is masked and no device
 *      has an MSI yet), so the old and new tables "provide identical
 *      results" for none;
 *   3. the I/O APIC's routed pins (COM1): masked, an entry each with the
 *      I/O APIC's requester id from the DMAR table, rewritten in
 *      remappable format, still masked (5.1.5.1);
 *   4. on every unit: Compatibility Format Interrupts off if they were on
 *      (11.4.4.1 CFI), then Interrupt Remapping Enable;
 *   5. remap_on published, then the pins unmasked. A COM1 edge lost while
 *      its pin was masked is rescued by CPU 0's tick (serial.c).
 * A failure in 2-4 undoes what came before it (remapping off again on the
 * units that had it, the pins back in compatibility format) and is
 * reported; the table is kept (a unit's IRTA may still point at it).
 *
 * Each entry change is flushed (when a unit doesn't snoop) and invalidated
 * in every unit's interrupt entry cache (the table's ops), and an index is
 * reused only after that (vtd_ir.c). */
#include <jam/acpi.h>
#include <jam/dmar.h>
#include <jam/ioapic.h>
#include <jam/irq_remap.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/pci.h>
#include <jam/report.h>
#include <jam/time.h>
#include <jam/vtd.h>
#include <jam/x86.h>

#include "vtd_domain.h"
#include "vtd_irq.h"

#define OFF_WAIT_PANIC_NS (10 * NS_PER_MS)   /* irq_remap_off's wait per unit on a panic */

static struct vtd_ir_table table;
static bool have_table;                         /* entries may be allocated (release/acquire) */
static bool remap_on;                           /* the units remap (release/acquire) */
static struct vtd_unit *units[VTD_MAX_UNITS];   /* the units that remap */
static uint32_t nunits;
static bool coherent;                           /* every one of them snoops the table */

static const struct dmar_info *info;           /* the probe's DMAR table */
static struct {
    uint8_t              id;                    /* the MADT's I/O APIC id */
    struct vtd_ir_source src;                   /* its requester id */
} ioapic_src[ACPI_MAX_IOAPICS];
static uint32_t nioapic_src;

/* ---- the table's callbacks ----------------------------------------------------- */

static void tbl_flush(void *ctx, const void *va, size_t len)
{
    (void)ctx;
    if (!coherent)
        vtd_flush_lines(va, len);
}

/* Every unit's interrupt entry cache (6.10: the table is shared). The
 * first error, after trying them all. */
static status_t tbl_invalidate(void *ctx, uint32_t index, uint32_t count)
{
    (void)ctx;
    status_t first = OK;
    for (uint32_t i = 0; i < nunits; i++) {
        status_t st = vtd_inv_iec_index(units[i], index, count);
        if (first == OK)
            first = st;
    }
    return first;
}

static const struct vtd_ir_ops tbl_ops = { .flush = tbl_flush, .invalidate = tbl_invalidate };

/* ---- what the machine allows (pure, then the units and the DMAR table) ---------- */

status_t vtd_irq_check_units(const uint64_t *ecap, uint32_t n, bool x2apic, bool *out_eim)
{
    if (n == 0)
        return ERR_NOT_FOUND;
    bool eim = x2apic;
    for (uint32_t i = 0; i < n; i++) {
        if (!VTD_ECAP_IR(ecap[i]) || !VTD_ECAP_QI(ecap[i]))
            return ERR_NOT_SUPPORTED;
        if (!VTD_ECAP_EIM(ecap[i]))
            eim = false;
    }
    *out_eim = eim;
    return OK;
}

/* units[] from the started units: all of the mapped ones, on segment 0. */
static status_t gather_units(uint32_t started, uint32_t mapped, bool *eim, const char **why)
{
    *why = "not every unit started";
    if (started != mapped)
        return ERR_BAD_STATE;
    uint64_t ecap[VTD_MAX_UNITS];
    nunits = 0;
    coherent = true;
    for (uint32_t i = 0; i < VTD_MAX_UNITS; i++) {
        struct vtd_unit *u = vtd_unit_get(i);
        if (!u)
            continue;
        if (u->index >= info->nunits || info->units[u->index].segment != 0) {
            *why = "a unit on a PCI segment other than 0";
            return ERR_NOT_SUPPORTED;
        }
        ecap[nunits] = u->ecap;
        coherent &= VTD_ECAP_C(u->ecap) != 0;
        units[nunits++] = u;
    }
    status_t st = vtd_irq_check_units(ecap, nunits, lapic_x2apic(), eim);
    *why = st == ERR_NOT_FOUND ? "no unit started" : "a unit without interrupt remapping";
    return st;
}

/* The scope that names the I/O APIC with MADT id `id`, in any unit. */
static const struct dmar_scope *ioapic_scope(uint8_t id)
{
    for (uint32_t u = 0; u < info->nunits; u++) {
        const struct dmar_scopes *l = &info->units[u].scopes;
        for (uint32_t k = 0; k < l->count && l->first + k < info->nscopes; k++) {
            const struct dmar_scope *s = &info->scopes[l->first + k];
            if (s->type == DMAR_SCOPE_IOAPIC && s->enum_id == id)
                return s;
        }
    }
    return NULL;
}

/* Every I/O APIC the MADT lists needs its requester id from the DMAR table
 * (8.3.1): without it its entries can't be validated, and in remapping
 * mode they can't be sent in the old format either. A firmware bug. */
static status_t find_ioapics(const char **why)
{
    info = vtd_dmar_info();   /* the probe's: no units when it found no valid table */
    *why = "no valid DMAR table";
    if (!info->nunits)
        return ERR_NOT_FOUND;
    nioapic_src = 0;
    for (uint32_t i = 0; i < acpi.ioapic_count && i < ACPI_MAX_IOAPICS; i++) {
        const struct dmar_scope *s = ioapic_scope(acpi.ioapics[i].id);
        struct vtd_ir_source src;
        if (!s || vtd_ir_source_from_scope(s, &src) != OK) {
            *why = "an I/O APIC with no usable DMAR scope (a firmware bug)";
            return ERR_NOT_FOUND;
        }
        ioapic_src[nioapic_src].id = acpi.ioapics[i].id;
        ioapic_src[nioapic_src].src = src;
        nioapic_src++;
    }
    return OK;
}

status_t vtd_irq_ioapic_source(uint8_t id, struct vtd_ir_source *out)
{
    for (uint32_t i = 0; i < nioapic_src; i++)
        if (ioapic_src[i].id == id) {
            *out = ioapic_src[i].src;
            return OK;
        }
    return ERR_NOT_FOUND;
}

/* ---- turning it on ---------------------------------------------------------------- */

/* Step 2 on one unit: the table pointer, then the cache (6.7). */
static status_t set_table(struct vtd_unit *u)
{
    if (vtd_rd32(u, VTD_GSTS) & VTD_GSTS_IRES)
        kprintf("vtd:         unit %u: interrupt remapping was on (table %lx): its table is "
                "replaced while on\n", u->index, vtd_rd64(u, VTD_IRTA));
    vtd_wr64(u, VTD_IRTA, vtd_ir_irta(&table));
    status_t st = vtd_gcmd(u, VTD_GCMD_SIRTP, true, VTD_GSTS_IRTPS);
    return st != OK ? st : vtd_inv_iec_global(u);
}

/* Step 4 on one unit. CFI goes first: every later command writes GSTS's
 * CFIS back as it is (VTD_GSTS_KEEP). */
static status_t enable(struct vtd_unit *u)
{
    status_t st = OK;
    if (vtd_rd32(u, VTD_GSTS) & VTD_GSTS_CFIS)
        st = vtd_gcmd(u, VTD_GCMD_CFI, false, VTD_GSTS_CFIS);
    return st != OK ? st : vtd_gcmd(u, VTD_GCMD_IRE, true, VTD_GSTS_IRES);
}

/* Steps 2 to 5 (step 3 only when `pins`: the first time). */
static status_t turn_on(bool pins, const char **why)
{
    status_t st = OK;
    *why = "setting the table pointer";
    for (uint32_t i = 0; i < nunits && st == OK; i++)
        st = set_table(units[i]);
    if (st == OK && pins) {
        *why = "the I/O APIC's entries";
        st = ioapic_remap_prepare();
    }
    if (st != OK)
        return st;
    uint32_t on = 0;
    *why = "turning it on";
    while (on < nunits && (st = enable(units[on])) == OK)
        on++;
    if (st != OK) {
        while (on--)
            (void)vtd_gcmd(units[on], VTD_GCMD_IRE, false, VTD_GSTS_IRES);   /* logged inside */
        if (pins)
            ioapic_remap_undo();
        return st;
    }
    __atomic_store_n(&remap_on, true, __ATOMIC_RELEASE);
    ioapic_remap_unmask();
    return OK;
}

static void say_on(void)
{
    uint32_t pins = 0;
    for (uint32_t i = 0; i < ioapic_route_count(); i++) {
        struct ioapic_route r;
        uint64_t e;
        pins += ioapic_route_get(i, &r, &e) && r.remap;
    }
    kprintf("vtd:         interrupt remapping on: %u unit%s, one table at %lx (%u entries, "
            "%s destinations%s), %u I/O APIC pin%s remapped\n", nunits, nunits == 1 ? "" : "s",
            table.phys, table.size, table.eim ? "x2APIC 32-bit" : "8-bit",
            table.eim ? "" : ", compatibility format blocked", pins, pins == 1 ? "" : "s");
    for (uint32_t i = 0; i < nioapic_src; i++)
        kprintf("vtd:         I/O APIC %u: its entries validated against requester id "
                "%02x:%02x.%x\n", ioapic_src[i].id, ioapic_src[i].src.sid >> 8,
                (ioapic_src[i].src.sid >> 3) & 0x1f, ioapic_src[i].src.sid & 7);
}

void vtd_irq_start(uint32_t started, uint32_t mapped)
{
    const char *why;
    bool eim = false;
    status_t st = find_ioapics(&why);
    if (st == OK)
        st = gather_units(started, mapped, &eim, &why);
    if (st == OK) {
        why = "no memory for the table";
        st = vtd_ir_table_init(&table, VTD_IRQ_ENTRIES, eim, &tbl_ops, NULL);
    }
    if (st == OK) {
        __atomic_store_n(&have_table, true, __ATOMIC_RELEASE);
        st = turn_on(true, &why);
    }
    if (st != OK) {
        report("vtd: interrupt remapping stays off: %s (%d)", why, st);
        return;
    }
    say_on();
}

status_t vtd_irq_reenable(void)
{
    if (!__atomic_load_n(&have_table, __ATOMIC_ACQUIRE))
        return ERR_BAD_STATE;
    const char *why;
    return turn_on(false, &why);
}

/* ---- entries ------------------------------------------------------------------------ */

bool irq_remap_on(void)
{
    return __atomic_load_n(&remap_on, __ATOMIC_ACQUIRE);
}

/* A new entry written from s (and invalidated), or the error. */
static status_t new_entry(const struct vtd_irte_spec *s, uint32_t *out_index)
{
    if (!__atomic_load_n(&have_table, __ATOMIC_ACQUIRE))
        return ERR_BAD_STATE;
    uint32_t index;
    status_t st = vtd_ir_alloc(&table, &index);
    if (st != OK)
        return st;
    st = vtd_ir_set(&table, index, s);
    if (st != OK) {
        irq_remap_free(index);   /* cleared; kept out of use if the cache can't be told */
        return st;
    }
    *out_index = index;
    return OK;
}

status_t irq_remap_alloc_pci(const struct pci_dev *d, uint32_t apic_id, uint8_t vector,
                             uint32_t *out_index, uint64_t *out_addr, uint32_t *out_data)
{
    if (!irq_remap_on())
        return ERR_BAD_STATE;
    if (d->info.segment != 0)
        return ERR_NOT_SUPPORTED;   /* no unit covers it */
    struct vtd_irte_spec s = {
        .dest = apic_id, .vector = vector, .level = false,
        .src = vtd_ir_source_device(d->info.bus, d->info.dev, d->info.fn),
    };
    uint32_t index;
    status_t st = new_entry(&s, &index);
    if (st != OK)
        return st;
    struct vtd_ir_msi m;
    (void)vtd_ir_msi_encode(index, &m);   /* index < VTD_IRQ_ENTRIES: always OK */
    *out_index = index;
    *out_addr = m.address;
    *out_data = m.data;
    return OK;
}

status_t irq_remap_alloc_ioapic(uint8_t ioapic_id, const struct irq_remap_pin *p,
                                uint32_t *out_index, uint64_t *out_rte)
{
    struct vtd_irte_spec s = { .dest = p->apic_id, .vector = p->vector, .level = p->level };
    status_t st = vtd_irq_ioapic_source(ioapic_id, &s.src);
    if (st != OK)
        return st;
    uint32_t index;
    st = new_entry(&s, &index);
    if (st != OK)
        return st;
    struct vtd_ir_rte_spec r = { index, p->vector, p->level, p->active_low, true };
    (void)vtd_ir_rte_encode(&r, out_rte);   /* the index and vector passed vtd_ir_set */
    *out_index = index;
    return OK;
}

void irq_remap_free(uint32_t index)
{
    if (index == 0)
        return;
    status_t st = vtd_ir_free(&table, index);
    if (st == ERR_INVALID_ARGS)
        kprintf("vtd: interrupt remapping entry %u freed but not allocated\n", index);
}

uint32_t irq_remap_used(void)
{
    if (!__atomic_load_n(&have_table, __ATOMIC_ACQUIRE))
        return 0;
    spin_lock(&table.lock);
    uint32_t n = table.nused;
    spin_unlock(&table.lock);
    return n;
}

/* ---- turning it off (kexec, panic) ---------------------------------------------------- */

void irq_remap_off(bool panic)
{
    if (!irq_remap_on())
        return;
    /* The pins first: once remapping is off, a remappable entry would be
     * read as an old-format message (5.1.4). Devices can't send: bus
     * mastering is off. */
    ioapic_mask_routed();
    uint64_t wait = panic ? OFF_WAIT_PANIC_NS : VTD_REG_WAIT_NS;
    for (uint32_t i = 0; i < nunits; i++)
        (void)vtd_gcmd_nolock(units[i], VTD_GCMD_IRE, false, VTD_GSTS_IRES, wait);   /* skipped */
    __atomic_store_n(&remap_on, false, __ATOMIC_RELEASE);
}

/* ---- for the tests ------------------------------------------------------------------- */

struct vtd_ir_table *vtd_irq_table(void)
{
    return __atomic_load_n(&have_table, __ATOMIC_ACQUIRE) ? &table : NULL;
}

uint32_t vtd_irq_units(struct vtd_unit **out, uint32_t max)
{
    for (uint32_t i = 0; i < nunits && i < max; i++)
        out[i] = units[i];
    return nunits;
}
