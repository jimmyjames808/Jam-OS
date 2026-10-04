/* The I/O APICs (arch/x86_64/ioapic.c) and the legacy 8259 PICs, which
 * are masked for good. The kernel routes only legacy ISA lines through the
 * I/O APIC (COM1); PCI devices use MSI or MSI-X (dev/pci_msi.c).
 *
 * With interrupt remapping (<jam/irq_remap.h>) a routed pin's entry is in
 * remappable format: it names an entry of the remapping table instead of a
 * vector and a CPU. Remapping is turned on after the pins were routed, so
 * the pins are rewritten then (ioapic_remap_prepare, ioapic_remap_unmask);
 * a pin routed later gets its entry at once.
 *
 * The routes are written at boot only, before user space, by one CPU;
 * afterwards they are only read (tests, irq_remap_off), so they need no
 * lock. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/status.h>

/* Remap and mask the 8259s, map every I/O APIC and mask all its pins.
 * Pins are routed one by one afterwards (ioapic_route_isa). */
void ioapic_init(void);
/* Unmask legacy ISA IRQ `irq` (after MADT overrides, with their polarity
 * and trigger mode) as `vector` on the CPU with that APIC id. False if no
 * I/O APIC has its GSI, the routes are full, or (without remapping) the id
 * doesn't fit the entry's 8 bits; with remapping on, also if no remapping
 * entry could be had. Used for COM1, at boot. */
bool ioapic_route_isa(uint8_t irq, uint8_t vector, uint32_t dest_apic_id);

/* A routed pin. */
#define IOAPIC_MAX_ROUTES 8
struct ioapic_route {
    uint8_t  ioapic_id;    /* the MADT's id of its I/O APIC */
    uint8_t  pin;          /* the pin on that I/O APIC */
    uint32_t gsi;
    uint8_t  vector;
    bool     level;        /* level-triggered (else edge) */
    bool     active_low;
    uint32_t apic_id;      /* the destination CPU */
    uint32_t remap;        /* its interrupt remapping entry; 0: compatibility format */
};

/* Routes ioapic_route_isa made, and route i with its redirection entry as
 * read back from the I/O APIC (false past the end). For the tests. */
uint32_t ioapic_route_count(void);
bool ioapic_route_get(uint32_t i, struct ioapic_route *out, uint64_t *out_entry);

/* For interrupt remapping (kernel/dev/vtd_irq.c), while it is turned on:
 * prepare masks every routed pin, gives it a remapping entry and writes it
 * in remappable format, still masked (on an error it undoes what it did and
 * returns the error: the pins are back as they were); unmask unmasks them
 * once remapping is on; undo puts them back in compatibility format,
 * unmasked, and frees their entries (remapping did not come on).
 * Thread context, at boot. */
status_t ioapic_remap_prepare(void);
void ioapic_remap_unmask(void);
void ioapic_remap_undo(void);

/* Mask every routed pin: for irq_remap_off (kexec and panic), before
 * remapping goes off. Takes no lock. */
void ioapic_mask_routed(void);
