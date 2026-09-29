#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Remap and mask the 8259s, map every I/O APIC and mask all its pins.
 * Pins are routed one by one afterwards (ioapic_route_isa). */
void ioapic_init(void);
/* GSI a legacy ISA IRQ is wired to, after MADT overrides. */
uint32_t ioapic_isa_to_gsi(uint8_t irq);
/* Unmask legacy ISA IRQ `irq` (after MADT overrides, with their polarity
 * and trigger mode) as `vector` on the CPU with that APIC id. False if no
 * I/O APIC has its GSI or the id doesn't fit the entry. Used for COM1. */
bool ioapic_route_isa(uint8_t irq, uint8_t vector, uint32_t dest_apic_id);
