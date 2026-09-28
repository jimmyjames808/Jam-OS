/* Loader-independent boot information.
 *
 * Only boot/limine.c knows about Limine. It translates Limine's responses into
 * this struct, and a future Jam OS UEFI loader will fill in the same struct.
 * All addresses here are PHYSICAL unless the field name says otherwise. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define BOOT_MAX_MEMMAP  256
#define BOOT_MAX_MODULES 32

enum boot_mem_type {
    BOOT_MEM_USABLE,
    BOOT_MEM_RESERVED,
    BOOT_MEM_ACPI_RECLAIMABLE,
    BOOT_MEM_ACPI_NVS,
    BOOT_MEM_BAD,
    BOOT_MEM_LOADER_RECLAIMABLE, /* free once we stop using boot_info data */
    BOOT_MEM_KERNEL_AND_MODULES,
    BOOT_MEM_FRAMEBUFFER,
};

struct boot_mem_region {
    uint64_t base;
    uint64_t length;
    enum boot_mem_type type;
};

struct boot_framebuffer {
    uint64_t phys;
    void    *virt;          /* already mapped by the loader */
    uint32_t width, height;
    uint32_t pitch;         /* bytes per scanline */
    uint16_t bpp;
    uint8_t  red_shift, green_shift, blue_shift;
};

struct boot_module {
    uint64_t    phys;
    uint64_t    size;
    const char *path;
    const char *string;
};

struct boot_info {
    const char *loader_name;
    uint64_t    hhdm_offset;     /* virt = phys + hhdm_offset for all RAM */
    uint64_t    rsdp_phys;       /* 0 if no ACPI */
    uint32_t    cpu_count;
    uint32_t    bsp_lapic_id;
    const char *cmdline;

    struct boot_framebuffer fb;  /* fb.virt == NULL if none */

    size_t                 memmap_count;
    struct boot_mem_region memmap[BOOT_MAX_MEMMAP];

    size_t             module_count;
    struct boot_module modules[BOOT_MAX_MODULES];
};

/* Kernel entry after the loader-specific glue has filled in boot_info. */
_Noreturn void kmain(struct boot_info *bi);
