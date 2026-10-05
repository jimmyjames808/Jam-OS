/* VT-d remapping units: what vtd_unit.c, vtd_qi.c and vtd_fault.c share
 * (Intel VT-d specification 4.1). The public side is <jam/vtd.h>.
 *
 * A unit is one DMAR hardware unit whose registers the probe mapped and
 * read. With the boot word `iommu=on` it is started (vtd_units_start):
 *   - the invalidation queue (vtd_qi.c, VT-d 6.5.2): one page of 256
 *     128-bit descriptors in RAM the unit reads (always snooped: ECAP.C
 *     doesn't apply to it). A caller submits a batch of descriptors
 *     followed by an invalidation wait descriptor that writes a sequence
 *     number into a status word of the caller's own (its "slot", in a page
 *     the unit writes coherently), then polls that word with no lock held,
 *     for at most VTD_QI_WAIT_NS. Errors the unit reports (IQE: a refused
 *     descriptor; ITE, ICE: device-TLB errors, never expected since no
 *     device-TLB invalidation is ever sent) are cleared and reported, and
 *     the caller whose descriptor was refused gets ERR_IO;
 *   - the invalidations built on it: context cache (global, domain,
 *     device), IOTLB (global, domain, page-selective in naturally aligned
 *     power-of-two runs), interrupt entry cache (global, index), and the
 *     write-buffer flush for a unit with CAP.RWBF; these are what vtd_pt's
 *     and vtd_ir's flush and invalidate callbacks call (vtd_unit_pt_ops,
 *     vtd_unit_ir_ops);
 *   - the fault event interrupt (vtd_fault.c, VT-d 7.3): its handler copies
 *     each fault record into a ring and clears it; a kernel thread logs
 *     them ("vtd: fault: ..."), counts them per device, and puts the first
 *     per device in the RESULTS box.
 * Translation is not turned on here; interrupt remapping is vtd_irq.c's.
 *
 * Locks, in order: "vtd queue" (one per unit: the queue's tail, slots and
 * owners; never taken in an interrupt handler) before "vtd gcmd" (one per
 * unit: one Global Command at a time, VT-d 6.9; vtd_qi_disable holds the
 * queue while it turns QI off). "vtd fault ring" (one per unit: the ring
 * and the errors the handler saw) and "vtd fault log" (the log thread's
 * wake-up) are taken in the fault interrupt, always irqsave; nothing is
 * taken under the ring lock, and only the log thread's wait queue lock
 * under the log lock (waitqueue_wait_until). Nothing here blocks with a
 * lock held: the waits under "vtd queue" and "vtd gcmd" are bounded
 * register polls. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/spinlock.h>
#include <jam/status.h>
#include <jam/time.h>

#include "vtd_ir.h"
#include "vtd_pt.h"

#define VTD_MAX_UNITS      8                    /* DMAR_MAX_UNITS */
#define VTD_QI_ENTRIES     256                  /* one page of 16-byte descriptors (IQA.QS 0) */
#define VTD_QI_SLOTS       128                  /* callers waiting at once, at most */
#define VTD_QI_SCRATCH     (VTD_QI_SLOTS)       /* status word for a refused descriptor's stand-in */
#define VTD_QI_BATCH_MAX   32                   /* descriptors in one submission, its wait not counted */
#define VTD_WAIT_MS        100u                 /* the deadline for a submission and its
                                                 * wait, and for a Global Command */
#define VTD_QI_WAIT_NS     (VTD_WAIT_MS * NS_PER_MS)
#define VTD_REG_WAIT_NS    (VTD_WAIT_MS * NS_PER_MS)
#define VTD_FAULT_RING     64                   /* fault records the handler holds for the log */

/* ---- descriptors (VT-d 6.5.2) -------------------------------------------------- */

/* One 128-bit descriptor: the queue is set up for these (IQA.DW = 0). */
struct vtd_desc {
    uint64_t lo;   /* bits 63:0: type, granularity, ids */
    uint64_t hi;   /* bits 127:64: an address, or 0 */
};
_Static_assert(sizeof(struct vtd_desc) == 16, "a 128-bit descriptor");

/* The type field, bits 3:0 (11:9 are always 0 for these; Table 23: legacy
 * mode takes types 1 to 5). */
#define VTD_DESC_CC        0x1   /* context-cache invalidate (6.5.2.1) */
#define VTD_DESC_IOTLB     0x2   /* IOTLB invalidate (6.5.2.3) */
#define VTD_DESC_IEC       0x4   /* interrupt entry cache invalidate (6.5.2.7) */
#define VTD_DESC_WAIT      0x5   /* invalidation wait (6.5.2.8) */

/* Granularities (the G fields). */
enum vtd_cc_g { VTD_CC_GLOBAL = 1, VTD_CC_DOMAIN = 2, VTD_CC_DEVICE = 3 };
enum vtd_iotlb_g { VTD_IOTLB_GLOBAL = 1, VTD_IOTLB_DOMAIN = 2, VTD_IOTLB_PAGE = 3 };

/* The encoders: pure, no checks beyond masking each field to its width
 * (the callers check ranges). */
struct vtd_desc vtd_desc_cc(enum vtd_cc_g g, uint16_t did, uint16_t sid, uint8_t fm);
/* drain: bit 0 = DR (drain reads), bit 1 = DW (drain writes); am: the
 * page-selective run is 2^am pages at addr (aligned to it); ih: keep the
 * paging-structure caches (only leaves changed). addr and am are ignored
 * (written 0) unless g is VTD_IOTLB_PAGE. */
struct vtd_desc vtd_desc_iotlb(enum vtd_iotlb_g g, uint16_t did, unsigned drain, uint64_t addr,
                               unsigned am, bool ih);
/* global, or index-selective: 2^im entries from index (aligned to it). */
struct vtd_desc vtd_desc_iec(bool global, uint32_t index, unsigned im);
/* Status write of `data` to `addr` (4-byte aligned), fence flag set. */
struct vtd_desc vtd_desc_wait(uint64_t addr, uint32_t data);

/* Split [first, first + n) into naturally aligned power-of-two blocks of at
 * most 2^max_order each, in order: out[i].first is a multiple of
 * 2^out[i].order. The number of blocks, or -1 when more than `cap` are
 * needed (the caller then invalidates more widely). n = 0 gives 0. Pure.
 * Used for IOTLB page runs (MAMV) and interrupt entry runs (MHMV). */
struct vtd_block {
    uint64_t first;   /* first page (or entry) */
    unsigned order;   /* 2^order of them */
};
int vtd_split_aligned(uint64_t first, uint64_t n, unsigned max_order, struct vtd_block *out,
                      unsigned cap);

/* ---- a unit ----------------------------------------------------------------- */

/* Counters, for the log, the tests and (later) the iommu command. Written
 * with __atomic_add_fetch; read relaxed. */
struct vtd_unit_stats {
    uint64_t submissions;   /* batches submitted (each ends in a wait) */
    uint64_t descriptors;   /* descriptors written, waits included */
    uint64_t wraps;         /* times the tail went past the queue's end */
    uint64_t refused;       /* descriptors the unit refused (IQE) */
    uint64_t ite, ice;      /* device-TLB time-out and completion errors */
    uint64_t timeouts;      /* waits past VTD_QI_WAIT_NS */
    uint64_t fault_irqs;    /* fault event interrupts taken */
    uint64_t faults;        /* fault records read */
    uint64_t faults_lost;   /* overflow (PFO), or the ring was full */
};

/* One fault record as the handler copied it (FRCD, 11.4.7.6). */
struct vtd_fault_rec {
    uint64_t lo, hi;
};

struct vtd_unit {
    uint32_t          index;        /* the DMAR table's unit number */
    uint64_t          base;         /* the register set's physical address */
    volatile uint8_t *regs;         /* its uncached mapping (the probe's) */
    uint64_t          span;         /* bytes mapped */
    uint64_t          cap, ecap;    /* CAP_REG, ECAP_REG */
    uint32_t          ver;          /* VER_REG */
    unsigned          haw;          /* the DMAR's host address width */
    bool              started;      /* vtd_unit_get hands it out (release/acquire) */
    spinlock_t        gcmd_lock;    /* "vtd gcmd": one Global Command at a time */

    /* The invalidation queue. */
    spinlock_t        qlock;        /* "vtd queue": the fields to `seq` */
    bool              qi_on;        /* submissions are taken */
    struct vtd_desc  *queue;        /* VTD_QI_ENTRIES descriptors (kernel map of RAM) */
    uint64_t          queue_phys;
    uint32_t          tail;         /* next index software writes (mirrors IQT) */
    uint64_t          slot_used[VTD_QI_SLOTS / 64];   /* a caller holds the slot */
    bool              slot_refused[VTD_QI_SLOTS];     /* its batch had a descriptor refused */
    uint8_t           owner[VTD_QI_ENTRIES];          /* the slot whose batch wrote each entry */
    uint32_t          seq;          /* the last sequence number handed out */
    volatile uint32_t *status;      /* the status page: a word per slot, then scratch */
    uint64_t          status_phys;
    uint32_t          last_iqei;    /* why the last refusal happened (IQERCD.IQEI) */

    /* Faults (vtd_fault.c). */
    bool              fault_on;     /* the interrupt is routed (release/acquire) */
    uint32_t          fault_cpu;    /* where the fault event interrupt goes */
    uint8_t           fault_vec;
    spinlock_t        ring_lock;    /* "vtd fault ring": ring, rhead, rtail, inv_errs */
    struct vtd_fault_rec ring[VTD_FAULT_RING];
    uint32_t          rhead, rtail; /* records [rhead, rtail) wait for the log thread */
    uint32_t          inv_errs;     /* FSTS IQE/ICE/ITE seen by the handler, for the thread */

    struct vtd_unit_stats stats;
};

/* The started unit number i (0 .. VTD_MAX_UNITS - 1), or NULL. */
struct vtd_unit *vtd_unit_get(uint32_t i);

/* From the probe, at boot: every unit the DMAR table lists (its register
 * set's range, kept from processes), and each one whose registers it
 * mapped and read (kept for vtd_units_start). */
void vtd_unit_found(uint32_t index, uint64_t base, uint64_t len);
void vtd_unit_mapped(uint32_t index, volatile uint8_t *regs, uint64_t span, unsigned haw);

/* Register access (the unit's mapping; offsets inside span). */
uint32_t vtd_rd32(const struct vtd_unit *u, uint32_t off);
uint64_t vtd_rd64(const struct vtd_unit *u, uint32_t off);
void     vtd_wr32(const struct vtd_unit *u, uint32_t off, uint32_t v);
void     vtd_wr64(const struct vtd_unit *u, uint32_t off, uint64_t v);

/* One Global Command: set (on) or clear one GCMD bit and wait until the
 * GSTS bit `status` agrees (VT-d 11.4.4.1's sequence). ERR_TIMED_OUT
 * after VTD_REG_WAIT_NS (logged with GSTS). Interrupts on or off; takes
 * "vtd gcmd". */
status_t vtd_gcmd(struct vtd_unit *u, uint32_t bit, bool on, uint32_t status);
/* The same without the unit's lock, for the kexec and panic paths (another
 * CPU may have stopped holding it). True when GSTS agreed within wait_ns.
 * Takes no lock; interrupts on or off. */
bool vtd_gcmd_nolock(struct vtd_unit *u, uint32_t bit, bool on, uint32_t status,
                     uint64_t wait_ns);

/* The write-buffer flush (VT-d 6.8): needed only with CAP.RWBF, after a
 * table change that is followed by no invalidation (a not-present entry
 * made present with CAP.CM = 0). Every context-cache, IOTLB and interrupt
 * entry cache invalidation flushes the write buffer implicitly first, so
 * after one of those it is never needed. OK at once without RWBF.
 * ERR_TIMED_OUT. */
status_t vtd_unit_flush_write_buffer(struct vtd_unit *u);

/* Make the CPU's writes to [va, va + len) visible to a unit whose table
 * reads don't snoop (CLFLUSHOPT or CLFLUSH over the lines, then SFENCE). */
void vtd_flush_lines(const void *va, size_t len);

/* ---- the queue (vtd_qi.c) --------------------------------------------------- */

/* Set the queue up and turn it on: a queue left on (by the firmware or a
 * kexec'd kernel) is first waited empty and turned off, as 6.5.2 asks;
 * a pending register-based invalidation is waited for. Unit not started
 * yet; ERR_NO_MEMORY, ERR_TIMED_OUT, ERR_BAD_STATE (left on and not empty:
 * the unit is left alone). */
status_t vtd_qi_init(struct vtd_unit *u);
/* Wait for the queue to drain, then turn it off (GCMD.QIE, 6.5.2). The
 * queue memory stays. ERR_TIMED_OUT (left on). For a takeover test and the
 * kexec path; no caller may submit meanwhile. */
status_t vtd_qi_disable(struct vtd_unit *u);
/* Turn a queue vtd_qi_disable turned off on again. */
status_t vtd_qi_enable(struct vtd_unit *u);

/* Submit d[0..n) and an invalidation wait, then wait for it. n is 1 to
 * VTD_QI_BATCH_MAX. OK; ERR_IO when the unit refused one of them (IQE:
 * the refusal is logged and reported, and the queue goes on with the
 * rest); ERR_TIMED_OUT when the wait didn't complete in VTD_QI_WAIT_NS
 * (logged and reported, with the queue's registers); ERR_BAD_STATE when
 * the queue isn't running; ERR_INVALID_ARGS for n. Thread context,
 * interrupts on, no spinlock held. */
status_t vtd_qi_submit(struct vtd_unit *u, const struct vtd_desc *d, uint32_t n);

/* Look at FSTS for invalidation errors and handle them (the recovery
 * above). Called by waiters and by the fault log thread. Thread context. */
void vtd_qi_check_errors(struct vtd_unit *u);

/* ---- the invalidations (vtd_qi.c) ------------------------------------------- */
/* Each submits its descriptors and waits; errors as vtd_qi_submit, and
 * ERR_OUT_OF_RANGE for a domain id the unit doesn't have (CAP.ND). */

status_t vtd_inv_context_global(struct vtd_unit *u);
status_t vtd_inv_context_domain(struct vtd_unit *u, uint16_t did);
/* fm: function mask (Table 18): 0 compares every bit of sid. */
status_t vtd_inv_context_device(struct vtd_unit *u, uint16_t did, uint16_t sid, uint8_t fm);
status_t vtd_inv_iotlb_global(struct vtd_unit *u);
status_t vtd_inv_iotlb_domain(struct vtd_unit *u, uint16_t did);
/* The IOTLB for [iova, iova + pages * 4 KiB) in domain did, split into
 * naturally aligned power-of-two runs of at most 2^MAMV pages; a range
 * that needs more than VTD_QI_BATCH_MAX of them, or a unit without
 * CAP.PSI, gets one domain-selective invalidation instead. keep_tables:
 * only leaves changed, so the paging-structure caches may stay (IH = 1). */
status_t vtd_inv_iotlb_pages(struct vtd_unit *u, uint16_t did, uint64_t iova, uint64_t pages,
                             bool keep_tables);
status_t vtd_inv_iec_global(struct vtd_unit *u);
/* Entries [index, index + count), in aligned runs of at most 2^MHMV;
 * global when that takes too many descriptors. */
status_t vtd_inv_iec_index(struct vtd_unit *u, uint32_t index, uint32_t count);

/* ---- the callbacks for vtd_pt and vtd_ir ------------------------------------- */

/* A domain on a unit: vtd_unit_pt_ops' ctx. */
struct vtd_unit_domain {
    struct vtd_unit *unit;
    uint16_t         did;   /* the domain id its context entries carry */
};
/* Table pages from the page allocator, line flushes, and a gather's
 * invalidation: page-selective per run (IH = 0 when tables were unlinked,
 * or in caching mode, where new mappings are what is invalidated), or
 * domain-selective for a whole-domain gather. */
extern const struct vtd_pt_ops vtd_unit_pt_ops;
/* Its invalidate (vtd_qi.c). */
status_t vtd_unit_pt_invalidate(void *ctx, const struct vtd_pt_gather *g);
/* ctx: the struct vtd_unit. Line flushes unless ECAP.C, and index-selective
 * interrupt entry cache invalidations. */
extern const struct vtd_ir_ops vtd_unit_ir_ops;

/* The page-table geometry for domains on u (vtd_pt_geom_from_caps with
 * the DMAR's host address width). A unit with CAP.RWBF gets caching_mode
 * too: every new mapping is then invalidated, and that invalidation is the
 * write-buffer flush 6.8 asks for. */
status_t vtd_unit_pt_geom(const struct vtd_unit *u, struct vtd_pt_geom *out);

/* ---- faults (vtd_fault.c) ------------------------------------------------- */

/* Route u's fault event interrupt to a vector of its own and unmask it.
 * The log thread is started with the first unit. ERR_NO_RESOURCES (no
 * vector), ERR_NO_MEMORY. */
status_t vtd_fault_init(struct vtd_unit *u);
/* Mask the fault event interrupt (FECTL.IM). */
void vtd_fault_mask(struct vtd_unit *u);
/* Read u's fault records into the ring now and wake the log thread: for a
 * condition that raised no interrupt (an error left FSTS non-zero meanwhile). */
void vtd_fault_kick(struct vtd_unit *u);
/* The fault reason in words, for the ones Jam OS can cause (Tables 13 and
 * 26), or NULL. Pure. */
const char *vtd_fault_reason_words(uint32_t reason);
/* The log line for one record: "vtd: fault: unit N: <the probe's
 * description>[: <the reason in words>]". Pure. */
void vtd_fault_line(char *buf, size_t n, uint32_t unit, uint64_t lo, uint64_t hi);

/* Faults counted per requester. */
#define VTD_FAULT_DEVS    64   /* requesters counted one by one; the rest together */
#define VTD_FAULT_LOGGED  8    /* faults logged per requester; the rest only counted */
struct vtd_fault_dev {
    uint32_t unit;    /* the unit's DMAR number */
    uint16_t sid;     /* the requester id */
    uint64_t count;   /* faults so far */
};
struct vtd_fault_counts {
    struct vtd_fault_dev dev[VTD_FAULT_DEVS];
    uint32_t             ndev;    /* entries in dev[] */
    uint64_t             other;   /* faults of requesters that found dev[] full */
};
/* Count one fault of (unit, sid): its count now (1 = the first), or 0 when
 * it has no entry and dev[] is full (counted in `other`). Pure. */
uint64_t vtd_fault_counts_add(struct vtd_fault_counts *c, uint32_t unit, uint16_t sid);
/* The log thread's per-requester fault counts, for the `iommu` command. A
 * reader races benignly with the thread that writes it. */
const struct vtd_fault_counts *vtd_fault_counts_get(void);
