/* Static ACPI tables (the AML side is uACPI, in M10). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct __attribute__((packed)) acpi_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

/* Generic Address Structure. */
struct __attribute__((packed)) acpi_gas {
    uint8_t  space;   /* 0 = memory, 1 = I/O port */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
};

#define ACPI_MAX_CPUS    256
#define ACPI_MAX_IOAPICS 16
#define ACPI_MAX_ISOS    32
#define ACPI_MAX_NMIS    16
#define ACPI_MAX_ECAM    8

struct acpi_info {
    uint8_t  revision;              /* RSDP revision: 0 = ACPI 1.0 */
    char     oem_id[7];

    /* MADT */
    uint64_t lapic_phys;
    bool     pcat_compat;           /* legacy 8259 PICs present */
    uint32_t cpu_count;             /* enabled or online-capable */
    struct { uint32_t uid, apic_id; bool enabled; } cpus[ACPI_MAX_CPUS];
    uint32_t ioapic_count;
    struct { uint8_t id; uint64_t phys; uint32_t gsi_base; } ioapics[ACPI_MAX_IOAPICS];
    uint32_t iso_count;             /* legacy IRQ -> GSI overrides */
    struct { uint8_t irq; uint32_t gsi; uint16_t flags; } isos[ACPI_MAX_ISOS];
    uint32_t nmi_count;             /* LAPIC LINT pins wired to NMI */
    struct { uint32_t uid; uint8_t lint; uint16_t flags; } nmis[ACPI_MAX_NMIS];

    /* FADT */
    bool     has_fadt;
    uint16_t sci_irq;
    uint32_t pm_timer_port;         /* 0 if absent */
    bool     pm_timer_32bit;
    uint16_t boot_arch_flags;       /* IA-PC boot architecture flags */
    bool     has_reset_reg;         /* M7: FADT RESET_REG usable (RESET_REG_SUP) */
    struct acpi_gas reset_reg;
    uint8_t  reset_value;

    /* HPET */
    uint64_t hpet_phys;             /* 0 if absent */

    /* MCFG (PCIe ECAM), for M6 */
    uint32_t ecam_count;
    struct { uint64_t phys; uint16_t segment; uint8_t bus_start, bus_end; } ecam[ACPI_MAX_ECAM];
};

extern struct acpi_info acpi;

void acpi_init(uint64_t rsdp_phys);
/* Map (if needed) and return the n-th table with this signature, or NULL. */
const struct acpi_header *acpi_find(const char sig[4], unsigned n);
/* Make a physical range readable through the HHDM; returns its address. */
void *acpi_map(uint64_t phys, uint64_t len);
