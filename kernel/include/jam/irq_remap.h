/* Interrupt remapping, as the rest of the kernel sees it (the VT-d side is
 * kernel/dev/vtd_irq.c; the design is docs/M11-PLAN.md, "Interrupt
 * remapping").
 *
 * With remapping on, a device's MSI or an I/O APIC's message carries no
 * vector and no CPU: only the index of an entry in the kernel's interrupt
 * remapping table, which says which vector goes to which CPU and which
 * requester may use it (source validation). A device can then raise only
 * the interrupts it was given; a write to the interrupt window in the old
 * (compatibility) format, or naming an entry that isn't its own, is
 * blocked and recorded as a fault.
 *
 * Remapping is turned on once, at boot, with the boot word `iommu=on`,
 * before any MSI is programmed (before user space and the kernel tests);
 * the I/O APIC's routed pins are rewritten then. With it off (the default)
 * nothing here is used: irq_remap_on() is false and every caller programs
 * the compatibility format as before. Nothing turns it off again but
 * irq_remap_off (kexec and panic).
 *
 * Callers: irq.c (msi_message: an interrupt object's MSI or MSI-X
 * message), ioapic.c (its routed pins). Neither knows the formats. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/status.h>

struct pci_dev;

/* Is interrupt remapping on? Lock-free (an acquire load); true from the
 * moment the units remap until irq_remap_off. */
bool irq_remap_on(void);

/* A new entry delivering `vector` to the CPU with APIC id `apic_id`
 * (fixed, edge), usable only by function d's requester id; *out_index is
 * its index (never 0), *out_addr and *out_data the remappable MSI message
 * that names it. ERR_BAD_STATE (remapping is off), ERR_NOT_SUPPORTED (a
 * function on a segment no unit covers), ERR_NO_RESOURCES (the table is
 * full), ERR_INVALID_ARGS (a vector below 32: the CPU's exceptions), or
 * the invalidation's error (the entry is freed again). Thread context,
 * interrupts on, no spinlock held: it waits for the units' interrupt entry
 * caches. */
status_t irq_remap_alloc_pci(const struct pci_dev *d, uint32_t apic_id, uint8_t vector,
                             uint32_t *out_index, uint64_t *out_addr, uint32_t *out_data);

/* What an I/O APIC pin delivers. */
struct irq_remap_pin {
    uint32_t apic_id;      /* the destination CPU's APIC id */
    uint8_t  vector;
    bool     level;        /* level-triggered (else edge) */
    bool     active_low;   /* the pin's polarity */
};

/* A new entry for a pin of the I/O APIC with MADT id `ioapic_id`, usable
 * only by that I/O APIC's requester id (from the DMAR table); *out_rte is
 * the pin's redirection entry in remappable format, masked. Allowed from
 * the moment the units' table exists (while remapping is being turned on,
 * so the pins are ready before it is). ERR_BAD_STATE (no table),
 * ERR_NOT_FOUND (no DMAR scope names that I/O APIC), and the errors of
 * irq_remap_alloc_pci. Context as irq_remap_alloc_pci. */
status_t irq_remap_alloc_ioapic(uint8_t ioapic_id, const struct irq_remap_pin *p,
                                uint32_t *out_index, uint64_t *out_rte);

/* Clear entry `index` (from one of the allocs; 0 does nothing) and give
 * it back once every unit's interrupt entry cache has dropped it; an
 * invalidation that fails keeps the index out of use for good (logged by
 * the queue). The caller has stopped the source first (masked or disabled
 * it at the device), so only a message already in flight can still name
 * it, and that one is blocked and recorded. Context as
 * irq_remap_alloc_pci. */
void irq_remap_free(uint32_t index);

/* Entries in use (tests, and the `iommu` command later). */
uint32_t irq_remap_used(void);

/* Before a kexec jump or on the panic path, after bus mastering went off
 * everywhere: mask the I/O APIC's routed pins, then turn interrupt
 * remapping off on every unit, each wait bounded (panic: short, and a unit
 * that doesn't answer is skipped). Takes no lock and allocates nothing:
 * other CPUs may be halted holding anything. The next kernel builds its
 * own table and takes over a unit left on. No-op when remapping is off.
 * Called by iommu_jump_off (<jam/iommu.h>), the one off path. */
void irq_remap_off(bool panic);
