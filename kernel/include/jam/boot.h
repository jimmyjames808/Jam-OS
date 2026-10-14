/* Loader-independent boot information.
 *
 * Only boot/limine.c knows about Limine. It translates Limine's responses into
 * this struct, and a future Jam OS UEFI loader will fill in the same struct.
 * All addresses here are PHYSICAL unless the field name says otherwise. */
#pragma once

#include <stdbool.h>
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
    /* RAM that is not this kernel's (kernel/kexec): the reserved region
     * that holds the stored kernel. Never mapped, never managed, never
     * handed out. */
    BOOT_MEM_FOREIGN,
    /* The previous kernel's crash record and log ring, after a kexec:
     * mapped, kept out of the allocator until they are read at boot
     * (kernel/kexec/crashlog.c), then freed like loader-reclaimable. */
    BOOT_MEM_CRASH_LOG,
};

/* Types that are real RAM this kernel maps and keeps struct pages for (as
 * opposed to MMIO holes, firmware-reserved ranges and foreign RAM). */
static inline bool boot_mem_is_ram(enum boot_mem_type t)
{
    return t == BOOT_MEM_USABLE || t == BOOT_MEM_LOADER_RECLAIMABLE ||
           t == BOOT_MEM_KERNEL_AND_MODULES || t == BOOT_MEM_ACPI_RECLAIMABLE ||
           t == BOOT_MEM_ACPI_NVS || t == BOOT_MEM_CRASH_LOG;
}

/* Types that are RAM at all, this kernel's or not, bad RAM included: no
 * MMIO resource or physical VMO may ever cover one (pmm_range_has_ram). */
static inline bool boot_mem_is_any_ram(enum boot_mem_type t)
{
    return boot_mem_is_ram(t) || t == BOOT_MEM_BAD || t == BOOT_MEM_FOREIGN;
}

struct boot_mem_region {
    uint64_t base;             /* physical */
    uint64_t length;           /* bytes */
    enum boot_mem_type type;   /* what the region holds */
};

struct boot_framebuffer {
    uint64_t phys;                                /* physical address */
    void    *virt;                                /* already mapped by the loader */
    uint32_t width, height;                       /* pixels */
    uint32_t pitch;                               /* bytes per scanline */
    uint16_t bpp;                                 /* bits per pixel (fbcon draws only 32) */
    uint8_t  red_shift, green_shift, blue_shift;  /* bit positions of the 8-bit colour channels */
};

#define BOOT_STR_MAX 128

struct boot_module {
    uint64_t phys;                   /* physical address of its bytes */
    uint64_t size;                   /* bytes */
    char     path[BOOT_STR_MAX];     /* the loader's path for it */
    char     string[BOOT_STR_MAX];   /* the loader's string for it */
};

struct boot_cpu {
    uint32_t acpi_uid;        /* ACPI processor UID */
    uint32_t lapic_id;        /* local APIC id */
    void    *loader_handle;   /* opaque, for boot_start_cpu; points into loader memory,
                                 * so it dangles once smp_start_aps reclaims that */
};

/* Everything in boot_info lives in kernel memory (strings are copied), so
 * loader-reclaimable memory can be freed without breaking it. */
struct boot_info {
    char     loader_name[BOOT_STR_MAX];  /* the loader's name and version, NUL-terminated */
    char     cmdline[BOOT_STR_MAX * 4];              /* kernel command line, NUL-terminated */
    uint64_t hhdm_offset;                            /* virt = phys + hhdm_offset for all RAM */
    uint64_t kernel_phys_base;                       /* physical address of __kernel_start */
    uint64_t kernel_virt_base;                       /* virtual address of __kernel_start */
    uint64_t rsdp_phys;                              /* 0 if no ACPI */
    uint64_t tsc_hz_loader;                          /* loader's TSC estimate, 0 if unknown */
    uint64_t kexec_record;                           /* the previous kernel's crash record
                                                      * (physical; kexec), 0 if none */
    uint32_t boot_disk_id;                           /* the MBR disk id of the disk the
                                                      * kernel was read from, 0: unknown */
    uint8_t  boot_disk_guid[16];                     /* its GPT disk GUID, the bytes on
                                                      * disk; zeros: an MBR disk, or
                                                      * unknown */
    uint32_t cpu_count;                              /* entries in cpus[] */
    uint32_t bsp_lapic_id;                           /* the CPU running the boot code */
    int      x2apic;                                 /* loader switched the APICs to x2APIC mode */
    struct boot_cpu cpus[BOOT_MAX_CPUS];             /* every CPU, the BSP included */

    struct boot_framebuffer fb;                      /* fb.virt == NULL if none */

    size_t                 memmap_count;             /* entries in memmap[] */
    struct boot_mem_region memmap[BOOT_MAX_MEMMAP];  /* the physical memory map */

    size_t             module_count;                 /* entries in modules[] */
    struct boot_module modules[BOOT_MAX_MODULES];    /* files the loader loaded (bootfs.img) */
};

/* Release an application processor parked by the loader: it calls
 * entry(arg) on a small loader stack, with the loader's page tables. */
void boot_start_cpu(const struct boot_cpu *cpu, void (*entry)(void *), void *arg);

/* Kernel entry after the loader-specific glue has filled in boot_info. */
_Noreturn void kmain(struct boot_info *bi);

/* The disk this kernel was booted from (main.c), named one of two ways:
 * an MBR disk (a stick) by its MBR disk id, a GPT disk (an installed
 * NVMe drive) by its disk GUID. Each is the loader's, or after a kexec the
 * previous kernel's, passed on as the boot word bootdisk=N (the MBR id, in
 * decimal) or bootdisk=gpt:<GUID>. init gets the word bootdisk=0x<hex> or
 * bootdisk=gpt:<GUID> and passes it to volumes (its boot disk).
 * boot_disk_id: the MBR disk id, 0 if the disk is GPT or unknown.
 * boot_disk_gpt: the GPT disk GUID's printed form (36 characters,
 * lowercase, 8-4-4-4-12 as UEFI prints it), NULL if the disk is MBR or
 * unknown. At most one of the two is set. */
uint32_t    boot_disk_id(void);
const char *boot_disk_gpt(void);

/* The printed form of a GUID held as its bytes on disk (the first three
 * fields little-endian), into out (BOOT_GUID_TEXT bytes, terminated). */
#define BOOT_GUID_TEXT 37
void boot_guid_text(const uint8_t guid[16], char out[BOOT_GUID_TEXT]);
/* The first n bytes of s as a GUID's printed form: exactly 36 characters,
 * hex digits with a dash at 8, 13, 18 and 23, either case, and not all
 * zeros. Lowercased into out; false (out empty) if they aren't one. */
bool boot_guid_parse(const char *s, size_t n, char out[BOOT_GUID_TEXT]);

/* The loader's memory map, as the "memmap" boot word prints it (main.c;
 * the shell's `memmap` through debug_command). */
void kmain_print_memmap(void);
