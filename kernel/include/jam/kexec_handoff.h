/* What one Jam OS kernel hands the next when it starts it itself
 * (kexec, kernel/kexec/): the handoff, read by kernel/boot/kexec.c in
 * place of Limine's responses, and the crash record, read by a crash
 * kernel (kernel/kexec/crashlog.c).
 *
 * Both cross from one kernel build to another, so they are wire formats,
 * not kernel structs: fixed-size fields, explicit numbers (never a C enum
 * of either kernel), no implicit padding (_Static_assert below), a magic,
 * a version, their own size and a checksum. The reader checks all of it
 * before it uses a byte, and treats every value as untrusted.
 *
 * At the new kernel's entry (_start): rdi = KEXEC_ENTRY_MAGIC, rsi = the
 * handoff's address in the HHDM of the page tables it runs on; Limine
 * enters with rdi = 0. Those tables map the kernel image at its link
 * address, the handoff, a stack, every range the memory map calls RAM of
 * the new kernel's in the HHDM, the framebuffer, and the crash record
 * and log ring read-only (a crash kernel). */
#pragma once

/* In rdi at _start (entry.S includes this header for it alone). */
#define KEXEC_ENTRY_MAGIC 0x4345584b4d414a

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>
#include <jam/boot.h>

#define KEXEC_HANDOFF_MAGIC   0x46464f444e41484bull /* "KHANDOFF" */
#define KEXEC_HANDOFF_VERSION 1u
#define KEXEC_RECORD_MAGIC    0x44524f4345524b43ull /* "CKRECORD" */
#define KEXEC_RECORD_VERSION  1u

#define KEXEC_MAX_MEMMAP  256
#define KEXEC_MAX_CPUS    256
#define KEXEC_MAX_MODULES 4
#define KEXEC_STR         64    /* loader name, module path and string */
#define KEXEC_CMDLINE     512
#define KEXEC_NAME        32    /* the crash record's log file name */

/* Memory types on the wire (struct boot_mem_region's, by number). */
enum {
    KEXEC_MEM_USABLE = 1,
    KEXEC_MEM_RESERVED = 2,
    KEXEC_MEM_ACPI_RECLAIMABLE = 3,
    KEXEC_MEM_ACPI_NVS = 4,
    KEXEC_MEM_BAD = 5,
    KEXEC_MEM_LOADER_RECLAIMABLE = 6,
    KEXEC_MEM_KERNEL_AND_MODULES = 7,
    KEXEC_MEM_FRAMEBUFFER = 8,
    KEXEC_MEM_FOREIGN = 9,
    KEXEC_MEM_CRASH_LOG = 10,
};

/* handoff.flags */
#define KEXEC_ONE_CPU (1u << 0)   /* start only the CPU that entered (a crash kernel) */
#define KEXEC_FLAGS   KEXEC_ONE_CPU

struct kexec_mem {
    uint64_t base;       /* physical */
    uint64_t length;     /* bytes */
    uint32_t type;       /* KEXEC_MEM_* */
    uint32_t reserved;   /* 0 */
};

struct kexec_cpu {
    uint32_t acpi_uid;   /* ACPI processor UID */
    uint32_t lapic_id;   /* local APIC id */
};

struct kexec_module {
    uint64_t phys;                  /* physical address of its bytes */
    uint64_t size;                  /* bytes */
    char     path[KEXEC_STR];       /* NUL-terminated ("kexec:/boot/bootfs.img") */
    char     string[KEXEC_STR];     /* NUL-terminated */
};

struct kexec_fb {
    uint64_t phys;                  /* 0: no framebuffer */
    uint32_t width, height;         /* pixels */
    uint32_t pitch;                 /* bytes per scanline */
    uint16_t bpp;                   /* bits per pixel */
    uint8_t  red_shift, green_shift, blue_shift;
    uint8_t  reserved[3];           /* 0 */
};

struct kexec_handoff {
    uint64_t magic;                 /* KEXEC_HANDOFF_MAGIC */
    uint32_t version;               /* KEXEC_HANDOFF_VERSION */
    uint32_t size;                  /* sizeof(struct kexec_handoff) */
    uint64_t checksum;              /* kexec_checksum of the struct with this field 0 */
    uint32_t flags;                 /* KEXEC_* */
    uint32_t x2apic;                /* 1: the APICs are in x2APIC mode */
    uint64_t hhdm_offset;           /* virt = phys + hhdm_offset in the tables it runs on */
    uint64_t kernel_phys_base;      /* where the image's first byte is */
    uint64_t kernel_virt_base;      /* its link address */
    uint64_t rsdp_phys;             /* 0: no ACPI */
    uint64_t tsc_hz;                /* the old kernel's measurement (a hint) */
    struct kexec_fb fb;
    uint32_t cpu_count;             /* entries in cpus[] */
    uint32_t memmap_count;          /* entries in memmap[] */
    uint32_t module_count;          /* entries in modules[] */
    uint32_t reserved;              /* 0 */
    char     loader_name[KEXEC_STR];  /* NUL-terminated */
    char     cmdline[KEXEC_CMDLINE];  /* NUL-terminated */
    struct kexec_cpu    cpus[KEXEC_MAX_CPUS];
    struct kexec_mem    memmap[KEXEC_MAX_MEMMAP];
    struct kexec_module modules[KEXEC_MAX_MODULES];
};

/* Filled by a panicking kernel just before it jumps; the crash kernel's
 * command line has its physical address (crashlog=<decimal>). */
struct kexec_crash_record {
    uint64_t magic;                 /* KEXEC_RECORD_MAGIC */
    uint32_t version;               /* KEXEC_RECORD_VERSION */
    uint32_t size;                  /* sizeof(struct kexec_crash_record) */
    uint64_t checksum;              /* kexec_checksum of the struct with this field 0 */
    uint64_t ring_phys;             /* the kernel log ring */
    uint64_t ring_size;             /* bytes, a power of two */
    uint64_t head;                  /* bytes ever written to it, at the jump */
    uint64_t panic_at;              /* the panic's first byte (a head value) */
    uint64_t tail_at;               /* where its copy of the log tail starts (0: none) */
    char     name[KEXEC_NAME];      /* this boot's log file ("boot-0042"), "" if none */
};

_Static_assert(sizeof(struct kexec_mem) == 24, "kexec_mem: no padding");
_Static_assert(sizeof(struct kexec_fb) == 32, "kexec_fb: no padding");
_Static_assert(sizeof(struct kexec_module) == 144, "kexec_module: no padding");
_Static_assert(sizeof(struct kexec_handoff) ==
                   120 + KEXEC_STR + KEXEC_CMDLINE + 8 * KEXEC_MAX_CPUS +
                       24 * KEXEC_MAX_MEMMAP + 144 * KEXEC_MAX_MODULES,
               "kexec_handoff: no padding");
_Static_assert(sizeof(struct kexec_crash_record) == 96, "kexec_crash_record: no padding");

/* The checksum both formats use, and the one over the loaded region: a
 * 64-bit multiply-xor over 8-byte words (len a multiple of 8), started
 * from KEXEC_SUM_SEED; pass a result as the seed to go on with more
 * bytes. Not a cryptographic hash: it is there to catch memory that
 * changed, and any one changed word always changes it. */
#define KEXEC_SUM_SEED 0x9e3779b97f4a7c15ull
uint64_t kexec_checksum(const void *p, uint64_t len, uint64_t seed);
/* The checksum of a struct of len bytes whose own checksum field (8
 * bytes at offset `at`, a multiple of 8) counts as 0. */
uint64_t kexec_struct_sum(const void *p, uint64_t len, uint64_t at);

/* Is h, of `len` bytes, a handoff this kernel can use? NULL if so, else
 * what is wrong with it. Never reads outside h[0..len). */
const char *kexec_handoff_check(const struct kexec_handoff *h, uint64_t len);

/* A memory type as a wire number and back (kernel/kexec/memmap.c); false
 * for a number that names no type. */
uint32_t kexec_mem_to_wire(enum boot_mem_type t);
bool     kexec_mem_from_wire(uint32_t w, enum boot_mem_type *out);

#endif /* __ASSEMBLER__ */
