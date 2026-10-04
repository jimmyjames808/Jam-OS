/* The I/O APICs (arch/x86_64/ioapic.c) and the legacy 8259 PICs, which
 * are masked for good. The kernel routes only legacy ISA lines through the
 * I/O APIC (COM1); PCI devices use MSI or MSI-X (dev/pci_msi.c).
 *
 * The routes are written at boot only, before user space, by one CPU;
 * afterwards they are only read (tests), so they need no lock. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/status.h>

/* Remap and mask the 8259s, map every I/O APIC and mask all its pins.
 * Pins are routed one by one afterwards (ioapic_route_isa). */
void ioapic_init(void);
/* Unmask legacy ISA IRQ `irq` (after MADT overrides, with their polarity
 * and trigger mode) as `vector` on the CPU with that APIC id. False if no
 * I/O APIC has its GSI, the routes are full, or the id doesn't fit the
 * entry's 8 bits. Used for COM1, at boot. */
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
};

/* Routes ioapic_route_isa made, and route i with its redirection entry as
 * read back from the I/O APIC (false past the end). For the tests. */
uint32_t ioapic_route_count(void);
bool ioapic_route_get(uint32_t i, struct ioapic_route *out, uint64_t *out_entry);
