/* The ACPI DMAR table: where the machine's VT-d remapping units are and
 * which devices each one covers (Intel VT-d specification 4.1, chapter 8).
 *
 * dmar_parse turns the table's bytes into a struct dmar_info with fixed
 * arrays and no allocation. The table comes from the firmware, so it is
 * untrusted input: every length is checked against what contains it, every
 * array has a cap, and what does not fit is counted (`dropped`), never
 * written past an end. A structure whose length is impossible stops the
 * walk there (`malformed`); what came before it is kept.
 *
 * Who uses it: the boot-time VT-d probe (kernel/dev/vtd_probe.c), which
 * logs it, and later the IOMMU itself (docs/M11-PLAN.md). */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <jam/status.h>

/* DMAR header flags (VT-d 8.1). */
#define DMAR_F_INTR_REMAP     (1u << 0)   /* interrupt remapping is supported */
#define DMAR_F_X2APIC_OPT_OUT (1u << 1)   /* firmware asks the OS not to use x2APIC */
#define DMAR_F_DMA_CTRL_OPT_IN (1u << 2)  /* firmware's own DMA protection, to hand over */

/* Remapping structure types (VT-d 8.2). */
enum dmar_type {
    DMAR_TYPE_DRHD = 0,   /* a remapping hardware unit */
    DMAR_TYPE_RMRR = 1,   /* reserved memory a device keeps using */
    DMAR_TYPE_ATSR = 2,   /* root ports where devices may use ATS */
    DMAR_TYPE_RHSA = 3,   /* a unit's NUMA proximity domain */
    DMAR_TYPE_ANDD = 4,   /* an ACPI namespace device (scope type 5) */
    DMAR_TYPE_SATC = 5,   /* SoC devices with an address translation cache */
    DMAR_TYPE_SIDP = 6,   /* SoC integrated device properties */
};

/* Device scope types (VT-d 8.3.1). */
enum dmar_scope_type {
    DMAR_SCOPE_ENDPOINT = 1,   /* one PCI function */
    DMAR_SCOPE_BRIDGE   = 2,   /* a bridge and everything below it */
    DMAR_SCOPE_IOAPIC   = 3,   /* an I/O APIC (enum_id is its APIC id) */
    DMAR_SCOPE_HPET     = 4,   /* an MSI-capable HPET (enum_id is its number) */
    DMAR_SCOPE_ACPI     = 5,   /* an ACPI namespace device (enum_id: ANDD's number) */
};

#define DMAR_DRHD_INCLUDE_PCI_ALL (1u << 0)   /* covers every device no other unit lists */
#define DMAR_ATSR_ALL_PORTS       (1u << 0)   /* every root port of the segment */
#define DMAR_SATC_ATC_REQUIRED    (1u << 0)   /* the devices need ATS on to work */

#define DMAR_MAX_UNITS  8
#define DMAR_MAX_RMRRS  16
#define DMAR_MAX_ATSRS  4
#define DMAR_MAX_SATCS  4
#define DMAR_MAX_ANDDS  8
#define DMAR_MAX_SCOPES 64   /* device scopes of all structures together */
#define DMAR_MAX_PATH   4    /* (device, function) steps kept per scope */
#define DMAR_NAME_MAX   24   /* an ANDD's object name, NUL included */

/* One device scope: a starting bus and a path of (device, function) steps
 * through bridges to the device. */
struct dmar_scope {
    uint8_t type;        /* DMAR_SCOPE_* (other values kept as found) */
    uint8_t enum_id;     /* I/O APIC id, HPET number or ANDD device number */
    uint8_t start_bus;   /* the bus the path starts on */
    uint8_t path_len;    /* steps in path[] */
    uint8_t path_full;   /* steps in the table (more than DMAR_MAX_PATH: cut) */
    struct { uint8_t dev, fn; } path[DMAR_MAX_PATH];
};

/* A structure's device scopes: scopes[first .. first + count) of the info. */
struct dmar_scopes {
    uint16_t first;      /* index of the first */
    uint16_t count;      /* how many were kept */
};

struct dmar_unit {
    uint64_t base;            /* the register set's physical address */
    uint16_t segment;         /* PCI segment */
    uint8_t  flags;           /* DMAR_DRHD_* */
    uint8_t  size;            /* the register set is 2^size pages (0 in old tables: 1 page) */
    struct dmar_scopes scopes;
};

struct dmar_rmrr {
    uint64_t base;            /* first byte */
    uint64_t limit;           /* last byte (inclusive, as the table says it) */
    uint16_t segment;
    struct dmar_scopes scopes;
};

/* ATSR and SATC share a shape: a segment, flags and scopes. */
struct dmar_portset {
    uint16_t segment;
    uint8_t  flags;           /* DMAR_ATSR_* or DMAR_SATC_* */
    struct dmar_scopes scopes;
};

struct dmar_andd {
    uint8_t dev_num;          /* matched by a scope's enum_id (DMAR_SCOPE_ACPI) */
    char    name[DMAR_NAME_MAX];   /* the ACPI object's path, NUL-terminated (cut if long) */
};

struct dmar_info {
    uint8_t  revision;        /* the table's revision */
    uint8_t  haw;             /* host address width in bits (the table's field + 1) */
    uint8_t  flags;           /* DMAR_F_* */
    uint8_t  malformed;       /* the walk stopped at a structure with an impossible length */
    uint32_t malformed_at;    /* that structure's byte offset in the table */
    uint32_t nunits, nrmrrs, natsrs, nsatcs, nandds, nrhsas;
    uint32_t nscopes;         /* scopes[] used */
    uint32_t unknown;         /* structures of a type this parser doesn't know (skipped) */
    uint32_t dropped;         /* structures or scopes past a cap, or scopes cut (not kept) */
    struct dmar_unit    units[DMAR_MAX_UNITS];
    struct dmar_rmrr    rmrrs[DMAR_MAX_RMRRS];
    struct dmar_portset atsrs[DMAR_MAX_ATSRS];
    struct dmar_portset satcs[DMAR_MAX_SATCS];
    struct dmar_andd    andds[DMAR_MAX_ANDDS];
    struct dmar_scope   scopes[DMAR_MAX_SCOPES];
};

/* Parse a DMAR table of `len` bytes into *out (zeroed first). Pure: reads
 * only [table, table + len), writes only *out, no lock, no allocation.
 * ERR_INVALID_ARGS if the header is not a DMAR's (signature, a length
 * shorter than the header or longer than len, a bad checksum): *out is
 * then all zero. Otherwise OK, with out->malformed set if a structure's
 * length stopped the walk early. */
status_t dmar_parse(const void *table, size_t len, struct dmar_info *out);

/* A name for a remapping structure type or a scope type, for log lines. */
const char *dmar_type_name(uint32_t type);
const char *dmar_scope_name(uint32_t type);
