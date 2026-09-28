#include <stdint.h>
#include <jam/boot.h>
#include <jam/cpu.h>
#include <jam/fbcon.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/selftest.h>
#include <jam/serial.h>
#include <jam/string.h>

#define JAMOS_VERSION   "0.0.2-m1"
#define KERNEL_STACK_SZ (64 * 1024)

_Noreturn void stack_switch_call(void *top, void (*fn)(void *), void *arg);

static const char *mem_type_name(enum boot_mem_type t)
{
    switch (t) {
    case BOOT_MEM_USABLE:             return "usable";
    case BOOT_MEM_RESERVED:           return "reserved";
    case BOOT_MEM_ACPI_RECLAIMABLE:   return "acpi-reclaim";
    case BOOT_MEM_ACPI_NVS:           return "acpi-nvs";
    case BOOT_MEM_BAD:                return "bad";
    case BOOT_MEM_LOADER_RECLAIMABLE: return "loader-reclaim";
    case BOOT_MEM_KERNEL_AND_MODULES: return "kernel+modules";
    case BOOT_MEM_FRAMEBUFFER:        return "framebuffer";
    }
    return "?";
}

static int cmdline_has(const char *cmdline, const char *word)
{
    size_t wl = strlen(word);
    for (const char *p = cmdline; *p; p++)
        if ((p == cmdline || p[-1] == ' ') && !memcmp(p, word, wl) &&
            (p[wl] == ' ' || p[wl] == '\0'))
            return 1;
    return 0;
}

static void print_boot_info(const struct boot_info *bi)
{
    kprintf("cpu:         %s (%s)%s\n", cpu_features.brand, cpu_features.vendor,
            cpu_features.hybrid ? " hybrid P/E" : "");
    kprintf("features:    nx=%d 1g=%d pat=%d pge=%d x2apic=%d invariant-tsc=%d\n",
            cpu_features.nx, cpu_features.pages_1g, cpu_features.pat,
            cpu_features.pge, cpu_features.x2apic, cpu_features.tsc_invariant);
    kprintf("loader:      %s, cmdline \"%s\"\n", bi->loader_name, bi->cmdline);
    if (bi->fb.virt)
        kprintf("framebuffer: %ux%u %ubpp pitch %u @ phys %lx\n", bi->fb.width,
                bi->fb.height, bi->fb.bpp, bi->fb.pitch, bi->fb.phys);
    kprintf("kernel:      phys %lx virt %lx\n", bi->kernel_phys_base, bi->kernel_virt_base);
    kprintf("rsdp:        %lx\n", bi->rsdp_phys);
    kprintf("cpus:        %u (bsp lapic %u)\n", bi->cpu_count, bi->bsp_lapic_id);

    if (cmdline_has(bi->cmdline, "memmap")) {
        for (size_t i = 0; i < bi->memmap_count; i++) {
            const struct boot_mem_region *r = &bi->memmap[i];
            kprintf("  %016lx - %016lx  %-14s %lu KiB\n", r->base, r->base + r->length,
                    mem_type_name(r->type), r->length / 1024);
        }
    }
}

static struct boot_info *boot;

/* Runs on the kernel's own stack, with its own page tables. */
_Noreturn static void kmain_stage2(void *arg)
{
    (void)arg;
    uint64_t total, free;
    pmm_stats(&total, &free);
    kprintf("pmm:         %lu MiB managed, %lu MiB free\n", total >> 8, free >> 8);

    if (cmdline_has(boot->cmdline, "selftest"))
        selftest_run();
    selftest_crash(boot->cmdline);

    kprintf("\nM1 complete: memory management up. Halting.\n");
    halt_forever();
}

_Noreturn void kmain(struct boot_info *bi)
{
    boot = bi;
    int has_serial = serial_init();
    fbcon_init(&bi->fb);

    fbcon_set_colors(0xffb000, 0x101018);
    kprintf("Jam OS %s\n", JAMOS_VERSION);
    fbcon_set_colors(0xd0d0d0, 0x101018);
    kprintf("serial:      %s\n", has_serial ? "COM1" : "none");

    cpu_detect();
    gdt_init_bsp();
    idt_init();
    print_boot_info(bi);

    pmm_early_init(bi);
    vmm_init(bi);
    pmm_init();
    vmm_use_buddy();
    heap_init();

    /* Loader-reclaimable memory (Limine's stack, page tables, and the code
     * the parked APs are spinning in) is freed in M2, after the APs start. */
    stack_switch_call(kstack_alloc(KERNEL_STACK_SZ), kmain_stage2, NULL);
}
