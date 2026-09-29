/* reboot: reset the machine, trying in turn
 *   1. the ACPI FADT reset register (RESET_REG_SUP; I/O, memory (mapped
 *      uncached at boot: acpi.reset_mmio) or PCI
 *      config space), the method firmware says is right for this board;
 *   2. port 0xCF9 (the PCH's Reset Control Register): 0x02 then 0x06, a
 *      hard reset (Intel chipsets, and QEMU's q35);
 *   3. the 8042 keyboard controller's pulse-reset command (0xFE on 0x64);
 *   4. a triple fault: an empty IDT and an exception.
 * Each gets ~50 ms to take effect. The serial ring is written out first
 * (synchronously) so the last line reaches the log. Under QEMU with
 * -no-reboot the first reset ends QEMU. */
#include <jam/acpi.h>
#include <jam/console_svc.h>
#include <jam/kprintf.h>
#include <jam/serial.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

static void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port));
}

static void delay_ms(uint64_t ms)
{
    uint64_t end = rdtsc() + ms * (tsc_hz / 1000);
    while (rdtsc() < end)
        cpu_relax();
}

static void acpi_reset(void)
{
    const struct acpi_gas *g = &acpi.reset_reg;
    uint8_t v = acpi.reset_value;
    switch (g->space) {
    case 0:     /* system memory: mapped uncached at boot (acpi.h) */
        if (acpi.reset_mmio)
            *acpi.reset_mmio = v;
        break;
    case 1:     /* system I/O */
        outb((uint16_t)g->address, v);
        break;
    case 2: {   /* PCI config space of bus 0: device, function, offset */
        uint32_t dev = (uint32_t)(g->address >> 32) & 0x1f;
        uint32_t fn = (uint32_t)(g->address >> 16) & 0x7;
        uint32_t off = (uint32_t)g->address & 0xff;
        outl(0xcf8, 0x80000000u | dev << 11 | fn << 8 | (off & 0xfc));
        outb((uint16_t)(0xcfc + (off & 3)), v);
        break;
    }
    }
}

void reboot_describe(char *buf, size_t size)
{
    if (acpi.has_reset_reg)
        ksnprintf(buf, size,
                  "ACPI reset register (%s 0x%lx = 0x%x), then 0xCF9, 8042, triple fault",
                  acpi.reset_reg.space == 1 ? "io" : acpi.reset_reg.space == 0 ? "mem" : "pci",
                  acpi.reset_reg.address, acpi.reset_value);
    else
        ksnprintf(buf, size, "no ACPI reset register: 0xCF9, 8042, triple fault");
}

_Noreturn void machine_reboot(void)
{
    char how[128];
    reboot_describe(how, sizeof(how));
    kprintf("reboot: resetting: %s\n", how);
    serial_set_async(false);   /* write out the ring, then everything synchronous */
    cli();   /* the other CPUs keep running: a reset stops them all */

    if (acpi.has_reset_reg) {
        acpi_reset();
        delay_ms(50);
    }
    outb(0xcf9, 0x02);
    delay_ms(1);
    outb(0xcf9, 0x06);
    delay_ms(50);
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
        cpu_relax();   /* the 8042's input buffer must be empty */
    outb(0x64, 0xfe);
    delay_ms(50);

    /* An empty IDT: the int3 below then triple-faults, which resets the CPU. */
    static const struct __attribute__((packed)) { uint16_t limit; uint64_t base; } none = { 0, 0 };
    __asm__ volatile("lidt %0; int3" ::"m"(none));
    for (;;)
        hlt();
}
