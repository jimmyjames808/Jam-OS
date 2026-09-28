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

static uint32_t io_read(struct ioapic *io, uint32_t reg)
{
    io->mmio[0] = reg;   /* IOREGSEL */
    return io->mmio[4];  /* IOWIN at +0x10 */
}

static void io_write(struct ioapic *io, uint32_t reg, uint32_t v)
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
