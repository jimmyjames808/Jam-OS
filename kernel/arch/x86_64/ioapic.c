/* The I/O APICs and the legacy 8259 PICs. At boot the 8259s are moved
 * off the exception vectors and masked, and every I/O APIC pin is masked;
 * ioapic_route_isa then unmasks single legacy ISA lines (COM1), applying
 * the MADT's interrupt source overrides. Each I/O APIC is reached through
 * its index/data register pair (IOREGSEL, IOWIN).
 *
 * Every routed pin is kept in routes[] (for the tests, and for interrupt
 * remapping: not built yet). */
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
    volatile uint32_t *mmio;   /* IOREGSEL at [0], IOWIN at [4] */
    uint32_t gsi_base;         /* first GSI of its pins */
    uint32_t pins;             /* number of redirection entries */
};

static struct ioapic ioapics[ACPI_MAX_IOAPICS];

/* The routed pins (see ioapic.h: written at boot only). */
static struct ioapic_route routes[IOAPIC_MAX_ROUTES];
static uint32_t nroutes;

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

/* ---- one pin's entry ------------------------------------------------------ */

/* MADT interrupt-source-override flags: bits 0-1 polarity (0 = bus
 * default, 1 = active high, 3 = active low), bits 2-3 trigger (0 = bus
 * default, 1 = edge, 3 = level). ISA's default is edge, active high. */
#define REDIR_ACTIVE_LOW (1u << 13)
#define REDIR_LEVEL      (1u << 15)

static const struct ioapic *ioapic_of(const struct ioapic_route *r)
{
    for (uint32_t i = 0; i < acpi.ioapic_count; i++)
        if (acpi.ioapics[i].id == r->ioapic_id)
            return &ioapics[i];
    return NULL;
}

static uint64_t read_entry(const struct ioapic *io, uint8_t pin)
{
    uint32_t lo = io_read(io, REG_REDIR + pin * 2u);
    return (uint64_t)io_read(io, REG_REDIR + pin * 2u + 1) << 32 | lo;
}

/* Write a pin's whole entry: the low half (vector, mask) first with the
 * mask set, so the pin never sends with a half-written entry; then the
 * high half (the destination, or the remappable index); then the low half
 * as asked. */
static void write_entry(const struct ioapic *io, uint8_t pin, uint64_t e)
{
    io_write(io, REG_REDIR + pin * 2u, (uint32_t)e | REDIR_MASKED);
    io_write(io, REG_REDIR + pin * 2u + 1, (uint32_t)(e >> 32));
    io_write(io, REG_REDIR + pin * 2u, (uint32_t)e);
}

static void set_masked(const struct ioapic *io, uint8_t pin, bool masked)
{
    uint32_t lo = io_read(io, REG_REDIR + pin * 2u);
    io_write(io, REG_REDIR + pin * 2u, masked ? lo | REDIR_MASKED : lo & ~REDIR_MASKED);
}

/* The compatibility-format entry, unmasked: fixed delivery, physical
 * destination (the APIC id in bits 63:56). */
static uint64_t compat_entry(const struct ioapic_route *r)
{
    uint64_t lo = r->vector | (r->active_low ? REDIR_ACTIVE_LOW : 0) |
                  (r->level ? REDIR_LEVEL : 0);
    return (uint64_t)r->apic_id << 56 | lo;
}

/* The route for gsi (on ioapics[io_index]): the one there, or a new one. */
static struct ioapic_route *route_slot(uint32_t io_index, uint32_t gsi)
{
    for (uint32_t i = 0; i < nroutes; i++)
        if (routes[i].gsi == gsi)
            return &routes[i];
    if (nroutes == IOAPIC_MAX_ROUTES)
        return NULL;
    struct ioapic_route *r = &routes[nroutes++];
    *r = (struct ioapic_route){ .ioapic_id = acpi.ioapics[io_index].id, .gsi = gsi,
                                .pin = (uint8_t)(gsi - ioapics[io_index].gsi_base) };
    return r;
}

/* Route r (its fields set) on io: masked while it changes, then unmasked. */
static bool program(const struct ioapic *io, struct ioapic_route *r)
{
    set_masked(io, r->pin, true);
    write_entry(io, r->pin, compat_entry(r));
    return true;
}

bool ioapic_route_isa(uint8_t irq, uint8_t vector, uint32_t dest_apic_id)
{
    uint32_t gsi = irq;
    bool active_low = false, level = false;
    for (uint32_t i = 0; i < acpi.iso_count; i++) {
        if (acpi.isos[i].irq != irq)
            continue;
        gsi = acpi.isos[i].gsi;
        active_low = (acpi.isos[i].flags & 3) == 3;
        level = ((acpi.isos[i].flags >> 2) & 3) == 3;
    }
    if (dest_apic_id > 0xff)
        return false;   /* the compatibility entry holds an 8-bit APIC id */
    for (uint32_t i = 0; i < acpi.ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];
        if (gsi < io->gsi_base || gsi >= io->gsi_base + io->pins)
            continue;
        struct ioapic_route *r = route_slot(i, gsi);
        if (!r)
            return false;
        r->vector = vector;
        r->level = level;
        r->active_low = active_low;
        r->apic_id = dest_apic_id;
        if (!program(io, r))
            return false;   /* left masked */
        kprintf("ioapic: ISA IRQ %u -> GSI %u -> vector 0x%x on lapic %u (%s, active %s)\n",
                irq, gsi, vector, dest_apic_id, level ? "level" : "edge",
                active_low ? "low" : "high");
        return true;
    }
    return false;
}

uint32_t ioapic_route_count(void)
{
    return nroutes;
}

bool ioapic_route_get(uint32_t i, struct ioapic_route *out, uint64_t *out_entry)
{
    if (i >= nroutes)
        return false;
    *out = routes[i];
    const struct ioapic *io = ioapic_of(&routes[i]);
    *out_entry = io ? read_entry(io, routes[i].pin) : 0;
    return true;
}
