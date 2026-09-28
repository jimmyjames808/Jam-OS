#include <stdint.h>
#include <jam/boot.h>
#include <jam/fbcon.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/serial.h>
#include <jam/string.h>

#define JAMOS_VERSION "0.0.1-m0"

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

static void print_memmap(const struct boot_info *bi)
{
    uint64_t usable = 0;
    kprintf("memory map (%zu regions):\n", bi->memmap_count);
    for (size_t i = 0; i < bi->memmap_count; i++) {
        const struct boot_mem_region *r = &bi->memmap[i];
        kprintf("  %016lx - %016lx  %-14s %lu KiB\n", r->base, r->base + r->length,
                mem_type_name(r->type), r->length / 1024);
        if (r->type == BOOT_MEM_USABLE)
            usable += r->length;
    }
    kprintf("usable RAM: %lu MiB\n", usable / (1024 * 1024));
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

_Noreturn void kmain(struct boot_info *bi)
{
    int has_serial = serial_init();
    fbcon_init(&bi->fb);

    fbcon_set_colors(0xffb000, 0x101018);
    kprintf("Jam OS %s\n", JAMOS_VERSION);
    fbcon_set_colors(0xd0d0d0, 0x101018);

    kprintf("loader:      %s\n", bi->loader_name);
    kprintf("cmdline:     \"%s\"\n", bi->cmdline);
    kprintf("serial:      %s\n", has_serial ? "COM1" : "none");
    if (bi->fb.virt)
        kprintf("framebuffer: %ux%u %ubpp pitch %u @ phys %lx\n", bi->fb.width,
                bi->fb.height, bi->fb.bpp, bi->fb.pitch, bi->fb.phys);
    kprintf("hhdm offset: %016lx\n", bi->hhdm_offset);
    kprintf("rsdp:        %lx\n", bi->rsdp_phys);
    kprintf("cpus:        %u (bsp lapic %u)\n", bi->cpu_count, bi->bsp_lapic_id);
    kprintf("modules:     %zu\n", bi->module_count);
    for (size_t i = 0; i < bi->module_count; i++)
        kprintf("  %s (%lu bytes)\n", bi->modules[i].path, bi->modules[i].size);
    print_memmap(bi);

    if (cmdline_has(bi->cmdline, "testpanic"))
        panic("test panic requested on the kernel command line");

    kprintf("\nM0 complete: nothing more to do yet. Halting.\n");
    halt_forever();
}
