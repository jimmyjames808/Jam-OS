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
#define BOOT_MAX_CPUS    256

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

/* Types that are real RAM (as opposed to MMIO holes or firmware-reserved). */
static inline int boot_mem_is_ram(enum boot_mem_type t)
{
    return t == BOOT_MEM_USABLE || t == BOOT_MEM_LOADER_RECLAIMABLE ||
           t == BOOT_MEM_KERNEL_AND_MODULES || t == BOOT_MEM_ACPI_RECLAIMABLE ||
           t == BOOT_MEM_ACPI_NVS;
}

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

#define BOOT_STR_MAX 128

struct boot_module {
    uint64_t phys;
    uint64_t size;
    char     path[BOOT_STR_MAX];
    char     string[BOOT_STR_MAX];
};

struct boot_cpu {
    uint32_t acpi_uid;
    uint32_t lapic_id;
    void    *loader_handle;   /* opaque, for boot_start_cpu; points into loader memory,
                                 * so it dangles once smp_start_aps reclaims that */
};

/* Everything in boot_info lives in kernel memory (strings are copied), so
 * loader-reclaimable memory can be freed without breaking it. */
struct boot_info {
    char     loader_name[BOOT_STR_MAX];
    char     cmdline[BOOT_STR_MAX * 4];
    uint64_t hhdm_offset;        /* virt = phys + hhdm_offset for all RAM */
    uint64_t kernel_phys_base;   /* physical address of __kernel_start */
    uint64_t kernel_virt_base;
    uint64_t rsdp_phys;          /* 0 if no ACPI */
    uint64_t tsc_hz_loader;      /* loader's TSC estimate, 0 if unknown */
    uint32_t cpu_count;
    uint32_t bsp_lapic_id;
    int      x2apic;             /* loader switched the APICs to x2APIC mode */
    struct boot_cpu cpus[BOOT_MAX_CPUS];

    struct boot_framebuffer fb;  /* fb.virt == NULL if none */

    size_t                 memmap_count;
    struct boot_mem_region memmap[BOOT_MAX_MEMMAP];

    size_t             module_count;
    struct boot_module modules[BOOT_MAX_MODULES];
};

/* Release an application processor parked by the loader: it calls
 * entry(arg) on a small loader stack, with the loader's page tables. */
void boot_start_cpu(const struct boot_cpu *cpu, void (*entry)(void *), void *arg);

/* Kernel entry after the loader-specific glue has filled in boot_info. */
_Noreturn void kmain(struct boot_info *bi);
