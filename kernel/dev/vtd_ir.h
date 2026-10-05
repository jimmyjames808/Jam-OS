/* VT-d interrupt remapping: the remapping table, its entries, and the
 * remappable message formats a device or an I/O APIC is programmed with
 * (Intel VT-d specification 4.1, 5.1 and 9.9).
 *
 * The model: with interrupt remapping on, a device's MSI (a 4-byte write to
 * 0xfee00000-0xfeefffff) carries no vector and no CPU, only an index into
 * the unit's interrupt remapping table. The entry (IRTE, 16 bytes) says
 * which vector goes to which CPU, and which requester may use it: source
 * validation compares the message's requester id with the entry's, so a
 * device can raise only the interrupts it was given (docs/M11-PLAN.md,
 * "Interrupt remapping").
 *
 * What is here, all without hardware:
 *   - pure encoders: an IRTE (x2APIC 32-bit or xAPIC 8-bit destinations,
 *     fixed delivery, physical destination, source validation by requester
 *     id or bus range), the remappable MSI address and data, and the
 *     remappable I/O APIC redirection entry;
 *   - the table: one physically contiguous block of 2^n entries, cleared
 *     at creation; entries allocated and freed by index; every change to
 *     an entry is flushed for a unit whose reads don't snoop (ops->flush)
 *     and then invalidated in the unit's interrupt entry cache
 *     (ops->invalidate, waited for). A freed index is reused only after
 *     its invalidation completed.
 *
 * Entry 0 is never handed out: an index of 0 means "none" to callers, and
 * a device whose message was left at zero names an entry that is never
 * present.
 *
 * Locking: the table's lock ("vtd irte", a spinlock, never taken in an
 * interrupt handler) guards only which indices are in use. An allocated
 * entry belongs to its allocator, which writes it without the lock;
 * ops->invalidate waits, so it is called with no lock held. Lock order:
 * "vtd irte" is a leaf. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/spinlock.h>
#include <jam/status.h>

struct dmar_scope;

/* ---- the entry (VT-d 9.9, an IRTE for remapped interrupts) ------------------ */

struct vtd_irte {
    uint64_t lo;   /* present, modes, vector, destination */
    uint64_t hi;   /* source validation */
};
_Static_assert(sizeof(struct vtd_irte) == 16, "an IRTE is 128 bits");

#define VTD_IRTE_P          (1ull << 0)    /* present */
#define VTD_IRTE_FPD        (1ull << 1)    /* fault processing disabled */
#define VTD_IRTE_DM         (1ull << 2)    /* destination mode: 1 logical (never set here) */
#define VTD_IRTE_RH         (1ull << 3)    /* redirection hint (never set here) */
#define VTD_IRTE_TM         (1ull << 4)    /* trigger mode: 1 level */
#define VTD_IRTE_DLM_SHIFT  5              /* delivery mode, 3 bits: 0 fixed */
#define VTD_IRTE_IM         (1ull << 15)   /* posted format (never set here) */
#define VTD_IRTE_V_SHIFT    16             /* vector, 8 bits */
#define VTD_IRTE_DST_SHIFT  32             /* x2APIC destination: bits 63:32 */
#define VTD_IRTE_XDST_SHIFT 40             /* xAPIC destination: bits 47:40 */
#define VTD_IRTE_SQ_SHIFT   16             /* in hi: source-id qualifier, 2 bits */
#define VTD_IRTE_SVT_SHIFT  18             /* in hi: source validation type, 2 bits */

/* Source validation type (SVT). Jam OS always validates: "none" (0) and the
 * reserved value (3) are refused. */
enum vtd_ir_svt {
    VTD_IR_SVT_RID = 1,   /* the requester id, compared as SQ says */
    VTD_IR_SVT_BUS = 2,   /* the requester's bus within [sid >> 8, sid & 0xff] */
};

/* Source-id qualifier (SQ), with VTD_IR_SVT_RID: which bits of the
 * function number are left out of the comparison. */
enum vtd_ir_sq {
    VTD_IR_SQ_ALL    = 0,   /* all 16 bits */
    VTD_IR_SQ_NOT_F2 = 1,   /* all but function bit 2 */
    VTD_IR_SQ_NOT_F21 = 2,  /* all but function bits 2:1 */
    VTD_IR_SQ_NOT_F  = 3,   /* bus and device only */
};

/* Who may use an entry. */
struct vtd_ir_source {
    uint16_t sid;   /* requester id (bus 15:8, device 7:3, function 2:0), or a bus range */
    uint8_t  svt;   /* enum vtd_ir_svt */
    uint8_t  sq;    /* enum vtd_ir_sq (0 with VTD_IR_SVT_BUS) */
};

/* What an entry delivers: always fixed delivery to one CPU, physical
 * destination mode. Vectors 0-31 are the CPU's exceptions (the local APIC
 * delivers 16-31 into their handlers): never in an entry. */
#define VTD_IR_VECTOR_MIN 32
struct vtd_irte_spec {
    uint32_t             dest;     /* the CPU's APIC id (at most 255 in xAPIC format) */
    uint8_t              vector;   /* VTD_IR_VECTOR_MIN..255 */
    bool                 level;    /* level-triggered (I/O APIC level pins); MSIs are edge */
    struct vtd_ir_source src;
};

static inline uint16_t vtd_ir_rid(uint8_t bus, uint8_t dev, uint8_t fn)
{
    return (uint16_t)(bus << 8 | (dev & 31) << 3 | (fn & 7));
}

/* A PCI function's source: its own requester id, every bit compared. */
static inline struct vtd_ir_source vtd_ir_source_device(uint8_t bus, uint8_t dev, uint8_t fn)
{
    return (struct vtd_ir_source){ vtd_ir_rid(bus, dev, fn), VTD_IR_SVT_RID, VTD_IR_SQ_ALL };
}

/* The source of an I/O APIC or HPET from its DMAR device scope: the
 * requester id the scope's one-step path names. ERR_INVALID_ARGS for
 * another scope type, ERR_NOT_SUPPORTED for a path through bridges (its
 * id would need the bridges' bus numbers; not built). Pure. */
status_t vtd_ir_source_from_scope(const struct dmar_scope *s, struct vtd_ir_source *out);

/* Encode an entry: present, fixed delivery, physical destination. eim: the
 * table takes 32-bit (x2APIC) destinations, else 8-bit (xAPIC).
 * ERR_INVALID_ARGS (vector below VTD_IR_VECTOR_MIN, a source that validates nothing,
 * reserved SVT or SQ, an empty bus range), ERR_OUT_OF_RANGE (an xAPIC
 * destination over 255). Pure. */
status_t vtd_irte_encode(const struct vtd_irte_spec *s, bool eim, struct vtd_irte *out);

/* ---- the remappable message formats (VT-d 5.1.2.2, 5.1.5) ----------------------- */

#define VTD_IR_MSI_BASE   0xfee00000u   /* bits 31:20 */
#define VTD_IR_MSI_FORMAT (1u << 4)     /* interrupt format: remappable */
#define VTD_IR_MSI_SHV    (1u << 3)     /* sub-handle valid (never set here) */
#define VTD_IR_MSI_H15    (1u << 2)     /* handle bit 15 */
#define VTD_IR_MSI_H_SHIFT 5            /* handle bits 14:0 at 19:5 */

#define VTD_IR_RTE_FORMAT (1ull << 48)  /* interrupt format: remappable */
#define VTD_IR_RTE_I_SHIFT 49           /* index bits 14:0 at 63:49 */
#define VTD_IR_RTE_I15    (1ull << 11)  /* index bit 15 */
#define VTD_IR_RTE_MASKED (1ull << 16)
#define VTD_IR_RTE_LEVEL  (1ull << 15)
#define VTD_IR_RTE_ACTIVE_LOW (1ull << 13)

/* A device's MSI or MSI-X message for entry `index`: the address names the
 * entry, sub-handle not valid, so the data (0) is ignored. ERR_OUT_OF_RANGE
 * past 0xffff. Pure. */
struct vtd_ir_msi {
    uint64_t address;   /* the upper 32 bits are 0 */
    uint32_t data;
};
status_t vtd_ir_msi_encode(uint32_t index, struct vtd_ir_msi *out);

/* An I/O APIC redirection entry in remappable format. The vector must equal
 * the entry's: the I/O APIC matches a level pin's EOI by it. */
struct vtd_ir_rte_spec {
    uint32_t index;        /* the IRTE */
    uint8_t  vector;       /* the IRTE's vector, VTD_IR_VECTOR_MIN..255 */
    bool     level;        /* level-triggered (and the IRTE's TM) */
    bool     active_low;   /* the pin's polarity */
    bool     masked;
};
/* ERR_OUT_OF_RANGE (index past 0xffff), ERR_INVALID_ARGS (vector below
 * VTD_IR_VECTOR_MIN). Pure. */
status_t vtd_ir_rte_encode(const struct vtd_ir_rte_spec *s, uint64_t *out);

/* ---- the table ------------------------------------------------------------------ */

#define VTD_IR_MIN_ENTRIES 256      /* one page */
#define VTD_IR_MAX_ENTRIES 65536    /* what a 16-bit index reaches */

/* What the table needs from the unit. ctx is vtd_ir_table.ctx. */
struct vtd_ir_ops {
    /* Make the CPU's writes to [va, va + len) visible to the unit
     * (CLFLUSHOPT, SFENCE). NULL for a unit whose reads snoop (ECAP.C). */
    void (*flush)(void *ctx, const void *va, size_t len);
    /* Invalidate entries [index, index + count) in the unit's interrupt
     * entry cache and wait: OK, or the error. */
    status_t (*invalidate)(void *ctx, uint32_t index, uint32_t count);
};

struct vtd_ir_table {
    struct vtd_irte         *entries;   /* the table, through the kernel's map of RAM */
    uint64_t                 phys;      /* its physical address (IRTA) */
    uint32_t                 size;      /* entries, a power of two */
    bool                     eim;       /* 32-bit destinations (IRTA.EIME) */
    const struct vtd_ir_ops *ops;
    void                    *ctx;
    spinlock_t               lock;      /* "vtd irte": the three fields below */
    uint64_t                *used;      /* one bit per entry: allocated (entry 0 always) */
    uint32_t                 nused;     /* allocated entries, entry 0 not counted */
    uint32_t                 next;      /* where the search for a free entry starts */
};

/* A cleared table of `entries` (a power of two, VTD_IR_MIN_ENTRIES to
 * VTD_IR_MAX_ENTRIES) entries, flushed. ERR_INVALID_ARGS, ERR_NO_MEMORY.
 * Kernel memory for the machine, charged to nobody. */
status_t vtd_ir_table_init(struct vtd_ir_table *t, uint32_t entries, bool eim,
                           const struct vtd_ir_ops *ops, void *ctx);
/* Free the table. Only when no unit's IRTA points at it any more (or
 * remapping is off and the cache invalidated). */
void vtd_ir_table_destroy(struct vtd_ir_table *t);

/* The Interrupt Remapping Table Address register's value for t. Pure. */
uint64_t vtd_ir_irta(const struct vtd_ir_table *t);

/* A free entry's index (never 0); the entry stays not present until set.
 * ERR_NO_RESOURCES when every entry is in use. */
status_t vtd_ir_alloc(struct vtd_ir_table *t, uint32_t *out_index);

/* Write entry `index` (allocated by the caller) from s, flush, invalidate.
 * A present entry may change its destination, vector and trigger mode;
 * not its source (ERR_BAD_STATE: free it and take another). Also
 * ERR_INVALID_ARGS (an index not allocated, or as vtd_irte_encode),
 * ERR_OUT_OF_RANGE, and the invalidation's error (the entry is written
 * then). Interrupts on, no spinlock held. */
status_t vtd_ir_set(struct vtd_ir_table *t, uint32_t index, const struct vtd_irte_spec *s);

/* Clear entry `index`, flush, invalidate, and give the index back. On an
 * invalidation error the entry is cleared but the index is never reused
 * (the unit may still hold the old entry); the error is returned.
 * ERR_INVALID_ARGS for an index not allocated. Interrupts on, no spinlock
 * held. */
status_t vtd_ir_free(struct vtd_ir_table *t, uint32_t index);
