#pragma once

#include <stdint.h>

/* Remap and mask the 8259s, map every I/O APIC and mask all its pins.
 * Devices get routed in M6. */
void ioapic_init(void);
/* GSI a legacy ISA IRQ is wired to, after MADT overrides. */
uint32_t ioapic_isa_to_gsi(uint8_t irq);
