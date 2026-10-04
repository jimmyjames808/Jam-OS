/* Interrupt remapping on the VT-d units (vtd_irq.c): what vtd_unit.c and
 * the tests use. The rest of the kernel sees <jam/irq_remap.h>.
 *
 * One remapping table, shared by every unit (VT-d 4.1, 5.1.3: "remapping
 * hardware units in the platform may be configured to share interrupt-
 * remapping table or use independent tables"; 6.10: a change is then
 * invalidated on each unit). So an entry's index means the same on every
 * unit, and which unit a device sits behind never matters: interrupt
 * remapping is turned on only when every unit the probe could read has
 * started and can remap (ECAP.IR), on all of them at once.
 *
 * Destinations are 32-bit x2APIC ids (IRTA.EIME) when the APICs run in
 * x2APIC mode and every unit has ECAP.EIM; otherwise 8-bit ids, with
 * compatibility-format interrupts blocked by CFI clear (5.1.4). Either way
 * a device's old-format message is blocked and recorded (fault 25h).
 *
 * The table's own lock ("vtd irte", vtd_ir.h) is the only lock here; the
 * rest is written at boot, by one CPU, before anything uses it, and only
 * read afterwards (remap_on, published with a release store). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/status.h>

#include "vtd_internal.h"

#define VTD_IRQ_ENTRIES 1024   /* 16 KiB: every live interrupt object needs one */

/* Turn interrupt remapping on (from vtd_units_start, with `iommu=on`, after
 * the units started: `started` of the `mapped` units the probe read). A
 * reason it can't goes to the RESULTS box and remapping stays off; the
 * rest of the IOMMU is not affected. Once, at boot, before user space and
 * before any MSI is programmed. */
void vtd_irq_start(uint32_t started, uint32_t mapped);

/* May units with these ECAP registers remap interrupts (each needs IR and
 * QI), and with 32-bit destinations (x2APIC mode and EIM on every one)?
 * OK and *out_eim; ERR_NOT_FOUND (no unit), ERR_NOT_SUPPORTED. Pure. */
status_t vtd_irq_check_units(const uint64_t *ecap, uint32_t n, bool x2apic, bool *out_eim);

/* ---- for the tests ------------------------------------------------------------ */

/* The table, or NULL before vtd_irq_start made it. */
struct vtd_ir_table *vtd_irq_table(void);
/* The units remapping (at most max of them into out[]); their count. */
uint32_t vtd_irq_units(struct vtd_unit **out, uint32_t max);
/* The requester id the DMAR table gives the I/O APIC with MADT id `id`. */
status_t vtd_irq_ioapic_source(uint8_t id, struct vtd_ir_source *out);
/* Turn remapping on again after irq_remap_off(false), with the same table
 * and entries (what a takeover does: the table pointer set again, the
 * cache invalidated, IRE on, the I/O APIC's pins unmasked). Thread
 * context. */
status_t vtd_irq_reenable(void);
