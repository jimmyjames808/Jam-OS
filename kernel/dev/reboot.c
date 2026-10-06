/* reboot: reset the machine through the firmware, trying in turn
 *   1. the ACPI FADT reset register (RESET_REG_SUP; I/O, memory (mapped
 *      uncached at boot: acpi.reset_mmio) or PCI
 *      config space), the method firmware says is right for this board
 *      (the PC's: I/O 0xCF9 = 0x06, the PCH's hard reset);
 *   2. port 0xCF9 (the PCH's Reset Control Register) as a full reset:
 *      0x02, then 0x0E (FULL_RST: the PCH drops the power rails for a few
 *      seconds, a power cycle, so no device keeps the state it had);
 *   3. the 8042 keyboard controller's pulse-reset command (0xFE on 0x64);
 *   4. a triple fault: an empty IDT and an exception.
 * The boot word reset=cf9, reset=8042 or reset=triple starts the list
 * there instead (the QEMU test of each method; on the PC, a way to try
 * the full reset first); reset=none tries none of them (the test of the
 * panic screen's words for a reset that didn't happen).
 *
 * First the machine is made quiet, as kexec_reboot does: the other CPUs
 * halted (NMI), the log written out on the serial port and drawn on the
 * screen again (the kernel takes it back from the console), and bus
 * mastering off on every PCI function but the bridges and the display.
 * Then each method gets RESET_WAIT_MS before the next is tried: a reset
 * the chipset has begun can take a while to stop the CPU (a power
 * management handshake), and a second write to 0xCF9 in the middle of it
 * could wedge it. A line before each attempt says which, on the screen
 * and the serial port, so a machine that hangs shows how far it got: the
 * last line names the method it was on. If none worked the screen says
 * so: hold the power button. Under QEMU with -no-reboot the first reset
 * ends QEMU. */
#include <jam/acpi.h>
#include <jam/cmdline.h>
#include <jam/console_svc.h>
#include <jam/fbcon.h>
#include <jam/iommu.h>
#include <jam/ipi.h>
#include <jam/kexec.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/pci.h>
#include <jam/serial.h>
#include <jam/time.h>
#include <jam/x86.h>

#define RESET_WAIT_MS 1000    /* for each method to take effect */
#define KBC_WAIT_MS   100     /* for the 8042's input buffer to empty */

#define PORT_RST_CNT  0xcf9   /* the PCH's Reset Control Register */
#define RST_SYS       0x02    /* SYS_RST: a hard reset, not just the CPU's INIT */
#define RST_CPU       0x04    /* RST_CPU: the reset starts on its 0 -> 1 edge */
#define RST_FULL      0x08    /* FULL_RST: power cycle through S5 */
#define PORT_KBC_CMD  0x64    /* the 8042's command and status port */
#define KBC_IN_FULL   0x02    /* status: the input buffer is still full */
#define KBC_PULSE_RST 0xfe    /* pulse output line 0: the reset line */

enum method { M_ACPI, M_CF9, M_8042, M_TRIPLE, M_NONE };

/* Where the list starts: the boot word's choice, else the ACPI register
 * when the FADT has one. reset=none tries nothing (a test of what the
 * panic screen says when no reset happens). */
static enum method first_method(void)
{
    if (cmdline_has("reset=none"))
        return M_NONE;
    if (cmdline_has("reset=triple"))
        return M_TRIPLE;
    if (cmdline_has("reset=8042"))
        return M_8042;
    if (cmdline_has("reset=cf9") || !acpi.has_reset_reg)
        return M_CF9;
    return M_ACPI;
}

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

static const char *space_name(uint8_t space)
{
    return space == 1 ? "io" : space == 0 ? "mem" : "pci";
}

static void acpi_reset(void)
{
    const struct acpi_gas *g = &acpi.reset_reg;
    uint8_t v = acpi.reset_value;
    kprintf("reboot: trying the ACPI reset register (%s 0x%lx = 0x%x)\n", space_name(g->space),
            g->address, v);
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

/* The reset bits cleared first (SYS_RST alone starts nothing), so the
 * write that sets RST_CPU is a 0 -> 1 edge even if an earlier write left
 * it set; the reserved bits kept as read (as Linux's reboot=pci does). */
static void cf9_reset(void)
{
    kprintf("reboot: trying 0xCF9's full reset (0x02, then 0x0e: a power cycle)\n");
    uint8_t keep = inb(PORT_RST_CNT) & (uint8_t)~(RST_SYS | RST_CPU | RST_FULL);
    outb(PORT_RST_CNT, keep | RST_SYS);
    delay_ms(1);
    outb(PORT_RST_CNT, keep | RST_SYS | RST_CPU | RST_FULL);
}

static void kbc_reset(void)
{
    kprintf("reboot: trying the 8042 keyboard controller's reset (0xfe to port 0x64)\n");
    uint64_t end = rdtsc() + KBC_WAIT_MS * (tsc_hz / 1000);
    while ((inb(PORT_KBC_CMD) & KBC_IN_FULL) && rdtsc() < end)
        cpu_relax();   /* no 8042 reads 0xff: the wait runs out, the write is harmless */
    outb(PORT_KBC_CMD, KBC_PULSE_RST);
}

_Noreturn static void triple_fault(void)
{
    kprintf("reboot: trying a triple fault, the last way. If this line stays on the screen, "
            "nothing reset the machine: hold the power button\n");
    /* An empty IDT: the int3 below then triple-faults, which resets the CPU. */
    static const struct __attribute__((packed)) { uint16_t limit; uint64_t base; } none = { 0, 0 };
    __asm__ volatile("lidt %0; int3" ::"m"(none));
    for (;;)
        hlt();
}

void reboot_describe(char *buf, size_t size)
{
    enum method m = first_method();
    if (m == M_NONE) {
        ksnprintf(buf, size, "reset=none: no way is tried (a test)");
        return;
    }
    const char *word = m == M_TRIPLE ? "reset=triple: " : m == M_8042 ? "reset=8042: "
                       : m == M_CF9 && acpi.has_reset_reg ? "reset=cf9: " : "";
    const char *rest = m == M_TRIPLE ? "triple fault"
                       : m == M_8042 ? "8042, triple fault"
                                     : "0xCF9 full reset, 8042, triple fault";
    if (m == M_ACPI)
        ksnprintf(buf, size, "ACPI reset register (%s 0x%lx = 0x%x), then %s",
                  space_name(acpi.reset_reg.space), acpi.reset_reg.address, acpi.reset_value,
                  rest);
    else
        ksnprintf(buf, size, "%s%s%s", word, acpi.has_reset_reg ? "" : "no ACPI reset register: ",
                  rest);
}

/* The other CPUs halted (a BSP that would wait for a kexec jump halts
 * too; after a panic they are halted already), the locks they may hold
 * dropped, the serial port synchronous, bus mastering off. With
 * `screen`, the screen the kernel's again with the whole log redrawn
 * (the lines of init and the shell before this one included); without,
 * it stays as it is (the panic screen's). */
static void quiet_machine(bool screen)
{
    kexec_reset_coming();
    uint32_t halted = panic_in_progress ? 0 : ipi_halt_others();
    klog_force_unlock();
    serial_panic();   /* the ring written out: what follows is synchronous */
    if (screen) {
        fbcon_force_unlock();
        fbcon_release();
    }
    uint32_t off = pci_panic_bus_master_off();
    iommu_jump_off();   /* a reset that fails half-way leaves no table of ours in use */
    kprintf("reboot: %u other CPU(s) halted, bus mastering off on %u PCI function(s)\n", halted,
            off);
}

/* The methods before the triple fault, from the first (first_method). */
static void try_methods(enum method m)
{
    if (m == M_ACPI) {
        acpi_reset();
        delay_ms(RESET_WAIT_MS);
    }
    if (m <= M_CF9) {
        cf9_reset();
        delay_ms(RESET_WAIT_MS);
    }
    if (m <= M_8042) {
        kbc_reset();
        delay_ms(RESET_WAIT_MS);
    }
}

_Noreturn void machine_reboot(void)
{
    char how[160];
    reboot_describe(how, sizeof(how));
    kprintf("reboot: resetting: %s\n", how);
    cli();
    quiet_machine(true);
    enum method m = first_method();
    if (m == M_NONE) {
        kprintf("reboot: nothing reset the machine: hold the power button\n");
        halt_forever();
    }
    try_methods(m);
    triple_fault();
}

void machine_reset_try(void)
{
    char how[160];
    reboot_describe(how, sizeof(how));
    kprintf("reboot: resetting: %s\n", how);
    cli();
    quiet_machine(false);
    enum method m = first_method();
    if (m != M_NONE)
        try_methods(m);
    kprintf("reboot: still here after %s\n", m == M_NONE ? "trying nothing (reset=none)"
                                                          : "every way but the triple fault");
}

void machine_reset_triple(void)
{
    if (first_method() != M_NONE)
        triple_fault();
}
