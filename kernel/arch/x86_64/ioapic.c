/* The I/O APICs and the legacy 8259 PICs. At boot the 8259s are moved
 * off the exception vectors and masked, and every I/O APIC pin is masked;
 * ioapic_route_isa then unmasks single legacy ISA lines (COM1), applying
 * the MADT's interrupt source overrides. Each I/O APIC is reached through
 * its index/data register pair (IOREGSEL, IOWIN). */
#include <jam/acpi.h>
#include <jam/ioapic.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/x86.h>

#define REG_ID    0x00
#define REG_VER   0x01
#define REG_REDIR 0x10
#define REDIR_MASKED (1u << 16)

struct ioapic {
    volatile uint32_t *mmio;
    uint32_t gsi_base;
    uint32_t pins;
};

static struct ioapic ioapics[ACPI_MAX_IOAPICS];

static uint32_t io_read(const struct ioapic *io, uint32_t reg)
{
    io->mmio[0] = reg;   /* IOREGSEL */
    return io->mmio[4];  /* IOWIN at +0x10 */
}

static void io_write(const struct ioapic *io, uint32_t reg, uint32_t v)
{
    io->mmio[0] = reg;
    io->mmio[4] = v;
}

/* Move the 8259s off the exception vectors, then mask every line. Even
 * masked, they can raise a spurious IRQ7/15, which lands on 0x27/0x2f. */
static void pic_disable(void)
{
    outb(0x20, 0x11); outb(0xa0, 0x11);                 /* ICW1: init, ICW4 follows */
    outb(0x21, VEC_PIC_BASE); outb(0xa1, VEC_PIC_BASE + 8);
    outb(0x21, 4); outb(0xa1, 2);                       /* cascade on IRQ2 */
    outb(0x21, 1); outb(0xa1, 1);                       /* 8086 mode */
    outb(0x21, 0xff); outb(0xa1, 0xff);                 /* mask all */
}

void ioapic_init(void)
{
    if (acpi.pcat_compat)
        pic_disable();

    for (uint32_t i = 0; i < acpi.ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];
        io->mmio = vmm_map_mmio(acpi.ioapics[i].phys, PAGE_SIZE);
        io->gsi_base = acpi.ioapics[i].gsi_base;
        io->pins = ((io_read(io, REG_VER) >> 16) & 0xff) + 1;
        for (uint32_t pin = 0; pin < io->pins; pin++) {
            io_write(io, REG_REDIR + pin * 2, REDIR_MASKED);
            io_write(io, REG_REDIR + pin * 2 + 1, 0);
        }
        kprintf("ioapic: id %u, GSIs %u-%u, all masked\n", acpi.ioapics[i].id,
                io->gsi_base, io->gsi_base + io->pins - 1);
    }
}

uint32_t ioapic_isa_to_gsi(uint8_t irq)
{
    for (uint32_t i = 0; i < acpi.iso_count; i++)
        if (acpi.isos[i].irq == irq)
            return acpi.isos[i].gsi;
    return irq;
}

/* MADT interrupt-source-override flags: bits 0-1 polarity (0 = bus
 * default, 1 = active high, 3 = active low), bits 2-3 trigger (0 = bus
 * default, 1 = edge, 3 = level). ISA's default is edge, active high. */
#define REDIR_ACTIVE_LOW (1u << 13)
#define REDIR_LEVEL      (1u << 15)

bool ioapic_route_isa(uint8_t irq, uint8_t vector, uint32_t dest_apic_id)
{
    uint32_t gsi = irq, low = vector;   /* fixed delivery, physical destination */
    for (uint32_t i = 0; i < acpi.iso_count; i++) {
        if (acpi.isos[i].irq != irq)
            continue;
        gsi = acpi.isos[i].gsi;
        if ((acpi.isos[i].flags & 3) == 3)
            low |= REDIR_ACTIVE_LOW;
        if (((acpi.isos[i].flags >> 2) & 3) == 3)
            low |= REDIR_LEVEL;
    }
    if (dest_apic_id > 0xff)
        return false;   /* the redirection entry holds an 8-bit APIC id */
    for (uint32_t i = 0; i < acpi.ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];
        if (gsi < io->gsi_base || gsi >= io->gsi_base + io->pins)
            continue;
        uint32_t pin = gsi - io->gsi_base;
        io_write(io, REG_REDIR + pin * 2 + 1, dest_apic_id << 24);
        io_write(io, REG_REDIR + pin * 2, low);
        kprintf("ioapic: ISA IRQ %u -> GSI %u -> vector 0x%x on lapic %u (%s, active %s)\n", irq,
                gsi, vector, dest_apic_id, low & REDIR_LEVEL ? "level" : "edge",
                low & REDIR_ACTIVE_LOW ? "low" : "high");
        return true;
    }
    return false;
}
