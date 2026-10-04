/* Intel VT-d remapping hardware: the register map and its bits (Intel
 * VT-d specification 4.1, chapter 11), the boot-time probe, and the
 * units' start.
 *
 * The probe (kernel/dev/vtd_probe.c) reads the DMAR table (<jam/dmar.h>)
 * and each unit's registers and logs what it finds, every line starting
 * "vtd:". It only READS: no VT-d register is written, so a machine runs
 * exactly as before it. What the firmware left on (translation, interrupt
 * remapping, protected memory regions, recorded faults) is reported, not
 * changed. Each unit's register mapping is kept for the units' code
 * (kernel/dev/vtd_unit.c).
 *
 * With the boot word `iommu=on` (off by default), vtd_units_start then
 * runs each unit's invalidation queue and its fault reporting, and
 * iommu_boot (<jam/iommu.h>) turns DMA translation on. Without it no
 * VT-d register is ever written. The design for the IOMMU itself is docs/M11-PLAN.md. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- register offsets (VT-d chapter 11) ------------------------------------- */
/* Advanced fault logging (0x058, CAP bit 3, GSTS bits 29:28 in 3.x) was
 * removed in revision 4.0: those are reserved now and not defined here. */

#define VTD_VER      0x000   /* 32: version, major in 7:4, minor in 3:0 */
#define VTD_CAP      0x008   /* 64: capabilities */
#define VTD_ECAP     0x010   /* 64: extended capabilities */
#define VTD_GCMD     0x018   /* 32: global command (write only: never read for state) */
#define VTD_GSTS     0x01c   /* 32: global status */
#define VTD_RTADDR   0x020   /* 64: root table address */
#define VTD_CCMD     0x028   /* 64: context command */
#define VTD_FSTS     0x034   /* 32: fault status */
#define VTD_FECTL    0x038   /* 32: fault event control */
#define VTD_FEDATA   0x03c   /* 32: fault event interrupt data */
#define VTD_FEADDR   0x040   /* 32: fault event interrupt address */
#define VTD_FEUADDR  0x044   /* 32: its upper half */
#define VTD_PMEN     0x064   /* 32: protected memory enable */
#define VTD_PLMBASE  0x068   /* 32: protected low memory base */
#define VTD_PLMLIMIT 0x06c   /* 32: and its limit */
#define VTD_PHMBASE  0x070   /* 64: protected high memory base */
#define VTD_PHMLIMIT 0x078   /* 64: and its limit */
#define VTD_IQH      0x080   /* 64: invalidation queue head */
#define VTD_IQT      0x088   /* 64: invalidation queue tail */
#define VTD_IQA      0x090   /* 64: invalidation queue address */
#define VTD_ICS      0x09c   /* 32: invalidation completion status */
#define VTD_IQERCD   0x0b0   /* 64: invalidation queue error record */
#define VTD_IRTA     0x0b8   /* 64: interrupt remapping table address */
/* The IOTLB Invalidate Register is at ECAP.IRO * 16 + 8 (11.4.6.3). */
#define VTD_IOTLB_REG_OFF 8

/* ---- the Capability Register ------------------------------------------------ */

#define VTD_BITS(v, lo, n) (((uint64_t)(v) >> (lo)) & (((uint64_t)1 << (n)) - 1))

#define VTD_CAP_ND(c)      VTD_BITS(c, 0, 3)    /* domain ids: 2^(4 + 2 * ND) */
#define VTD_CAP_RWBF(c)    VTD_BITS(c, 4, 1)    /* write-buffer flush needed */
#define VTD_CAP_PLMR(c)    VTD_BITS(c, 5, 1)    /* protected low memory region */
#define VTD_CAP_PHMR(c)    VTD_BITS(c, 6, 1)    /* protected high memory region */
#define VTD_CAP_CM(c)      VTD_BITS(c, 7, 1)    /* caching mode: not-present entries cached */
#define VTD_CAP_SAGAW(c)   VTD_BITS(c, 8, 5)    /* page-table levels: bit 1 = 3, 2 = 4, 3 = 5 */
#define VTD_CAP_MGAW(c)    VTD_BITS(c, 16, 6)   /* max guest address width - 1 */
#define VTD_CAP_ZLR(c)     VTD_BITS(c, 22, 1)   /* zero-length reads */
#define VTD_CAP_FRO(c)     VTD_BITS(c, 24, 10)  /* fault recording registers at FRO * 16 */
#define VTD_CAP_SLLPS(c)   VTD_BITS(c, 34, 4)   /* superpages: bit 0 = 2 MiB, 1 = 1 GiB */
#define VTD_CAP_PSI(c)     VTD_BITS(c, 39, 1)   /* page-selective IOTLB invalidation */
#define VTD_CAP_NFR(c)     VTD_BITS(c, 40, 8)   /* fault recording registers - 1 */
#define VTD_CAP_MAMV(c)    VTD_BITS(c, 48, 6)   /* largest PSI: 2^MAMV pages */
#define VTD_CAP_DWD(c)     VTD_BITS(c, 54, 1)   /* write draining */
#define VTD_CAP_DRD(c)     VTD_BITS(c, 55, 1)   /* read draining */
#define VTD_CAP_FL1GP(c)   VTD_BITS(c, 56, 1)   /* first-stage 1 GiB pages */
#define VTD_CAP_PI(c)      VTD_BITS(c, 59, 1)   /* posted interrupts */
#define VTD_CAP_FL5LP(c)   VTD_BITS(c, 60, 1)   /* first-stage 5-level paging */
#define VTD_CAP_ESIRTPS(c) VTD_BITS(c, 62, 1)   /* setting IRTA invalidates the IEC */
#define VTD_CAP_ESRTPS(c)  VTD_BITS(c, 63, 1)   /* setting RTADDR invalidates caches */

/* ---- the Extended Capability Register --------------------------------------- */

#define VTD_ECAP_C(e)      VTD_BITS(e, 0, 1)    /* page walks snoop the CPU's caches */
#define VTD_ECAP_QI(e)     VTD_BITS(e, 1, 1)    /* queued invalidation */
#define VTD_ECAP_DT(e)     VTD_BITS(e, 2, 1)    /* device TLBs (ATS) */
#define VTD_ECAP_IR(e)     VTD_BITS(e, 3, 1)    /* interrupt remapping */
#define VTD_ECAP_EIM(e)    VTD_BITS(e, 4, 1)    /* 32-bit destination ids (x2APIC) */
#define VTD_ECAP_PT(e)     VTD_BITS(e, 6, 1)    /* pass-through translation type */
#define VTD_ECAP_SC(e)     VTD_BITS(e, 7, 1)    /* snoop control */
#define VTD_ECAP_IRO(e)    VTD_BITS(e, 8, 10)   /* IOTLB registers at IRO * 16 */
#define VTD_ECAP_MHMV(e)   VTD_BITS(e, 20, 4)   /* largest handle mask */
#define VTD_ECAP_NEST(e)   VTD_BITS(e, 26, 1)   /* nested translation */
#define VTD_ECAP_PRS(e)    VTD_BITS(e, 29, 1)   /* page requests */
#define VTD_ECAP_PASID(e)  VTD_BITS(e, 40, 1)   /* PASIDs */
#define VTD_ECAP_SMTS(e)   VTD_BITS(e, 43, 1)   /* scalable-mode translation */
#define VTD_ECAP_SLTS(e)   VTD_BITS(e, 46, 1)   /* second-stage translation (scalable mode) */

/* ---- the Global Status Register --------------------------------------------- */

#define VTD_GSTS_TES   (1u << 31)   /* translation enabled */
#define VTD_GSTS_RTPS  (1u << 30)   /* root table pointer set */
#define VTD_GSTS_WBFS  (1u << 27)   /* write-buffer flush in progress */
#define VTD_GSTS_QIES  (1u << 26)   /* queued invalidation on */
#define VTD_GSTS_IRES  (1u << 25)   /* interrupt remapping on */
#define VTD_GSTS_IRTPS (1u << 24)   /* interrupt remapping table pointer set */
#define VTD_GSTS_CFIS  (1u << 23)   /* compatibility-format interrupts pass */

/* ---- the Global Command Register (11.4.4.1) --------------------------------- */
/* One command per write: GCMD = (GSTS & VTD_GSTS_KEEP) with one bit changed,
 * then wait until GSTS shows it. VTD_GSTS_KEEP drops the one-shot status
 * bits (RTPS 30, WBFS 27, IRTPS 24) and the reserved 29:28, which must not
 * be written back as commands. */

#define VTD_GCMD_TE    (1u << 31)   /* translation enable */
#define VTD_GCMD_SRTP  (1u << 30)   /* set root table pointer */
#define VTD_GCMD_WBF   (1u << 27)   /* write-buffer flush (only with CAP.RWBF) */
#define VTD_GCMD_QIE   (1u << 26)   /* queued invalidation enable */
#define VTD_GCMD_IRE   (1u << 25)   /* interrupt remapping enable */
#define VTD_GCMD_SIRTP (1u << 24)   /* set interrupt remapping table pointer */
#define VTD_GCMD_CFI   (1u << 23)   /* compatibility-format interrupts pass */
#define VTD_GSTS_KEEP  0x96ffffffu

/* ---- register-based invalidation (11.4.6), only waited for, never used ------ */

#define VTD_CCMD_ICC   (1ull << 63) /* a context-cache invalidation is in progress */
#define VTD_IOTLB_IVT  (1ull << 63) /* an IOTLB invalidation is in progress */

/* ---- the invalidation queue registers (11.4.9) ------------------------------- */

#define VTD_IQ_SHIFT   4            /* IQH/IQT hold the descriptor index << 4 (18:4) */
#define VTD_IQA_DW     (1ull << 11) /* 256-bit descriptors (scalable mode only: never set) */
#define VTD_IQA_QS(v)  VTD_BITS(v, 0, 3)   /* 2^QS pages, 2^(QS + 8) 128-bit entries */
#define VTD_IQERCD_IQEI(v) VTD_BITS(v, 0, 4)   /* why IQE was set (1-6), 0 unknown */

/* ---- the Fault Event Control Register (11.4.7.2) ----------------------------- */

#define VTD_FECTL_IM   (1u << 31)   /* interrupt masked (the reset value) */
#define VTD_FECTL_IP   (1u << 30)   /* an interrupt is held pending (read only) */
/* Bits 29:0 are RsvdP: written back as read. */

/* ---- Fault Status, Protected Memory Enable, Interrupt Remapping Table Address */

#define VTD_FSTS_PFO   (1u << 0)    /* a fault was lost: the records were full */
#define VTD_FSTS_PPF   (1u << 1)    /* a primary fault is recorded */
#define VTD_FSTS_IQE   (1u << 4)    /* invalidation queue error */
#define VTD_FSTS_ICE   (1u << 5)    /* invalidation completion error */
#define VTD_FSTS_ITE   (1u << 6)    /* invalidation time-out error */
#define VTD_FSTS_FRI(f) VTD_BITS(f, 8, 8)   /* the first record holding a fault */

#define VTD_PMEN_EPM   (1u << 31)   /* protected memory regions on */
#define VTD_PMEN_PRS   (1u << 0)    /* ... and their status */

#define VTD_IRTA_EIME  (1ull << 11) /* the table's entries take 32-bit destinations */
#define VTD_IRTA_S(v)  VTD_BITS(v, 0, 4)   /* 2^(S + 1) entries */

/* A Fault Recording Register's upper 64 bits (VT-d chapter 11). */
#define VTD_FRCD_F         (1ull << 63)           /* the record holds a fault */
#define VTD_FRCD_TYPE1(h)  VTD_BITS(h, 62, 1)     /* T1 (bit 126) */
#define VTD_FRCD_TYPE2(h)  VTD_BITS(h, 28, 1)     /* T2 (bit 92): T1 T2 = 00 write, 01 page
                                                   * request, 10 read, 11 atomic */
#define VTD_FRCD_REASON(h) VTD_BITS(h, 32, 8)     /* the fault reason */
#define VTD_FRCD_SID(h)    VTD_BITS(h, 0, 16)     /* the requester: bus 15:8, dev 7:3, fn 2:0 */
/* The lower 64 bits hold the fault info: for a DMA fault the page address
 * (63:12); for an interrupt-remapping fault (reasons 20h-2Fh) the interrupt
 * index in 63:48, except 25h (a compatibility-format interrupt blocked),
 * where it is undefined (VT-d 11.4.7.6 and 5.1.4.1). */
#define VTD_FRCD_IR_FIRST  0x20
#define VTD_FRCD_IR_LAST   0x2f
#define VTD_FRCD_IR_COMPAT 0x25
#define VTD_FRCD_INDEX(l)  VTD_BITS(l, 48, 16)

/* ---- the probe -------------------------------------------------------------- */

/* Read the DMAR table and every unit's registers and log them ("vtd:"
 * lines); nothing is written. Call once at boot, after pci_init (it names
 * the devices in each scope). A machine with no DMAR logs one line. */
void vtd_probe(void);

/* One line describing a unit's CAP and ECAP registers, for the log and for
 * the tests: what the IOMMU will rely on (levels, widths, domains, caching
 * mode, invalidation, snooping, interrupt remapping, x2APIC). Pure. */
void vtd_describe_caps(char *buf, size_t n, uint64_t cap, uint64_t ecap);

/* One line describing GSTS and the pointers the firmware left: what is on.
 * Pure. */
void vtd_describe_status(char *buf, size_t n, uint32_t gsts, uint32_t pmen, uint32_t fsts);

/* One fault recording register (lo: bits 63:0, hi: bits 127:64), described
 * for the log: the requester, what it did and the reason. Pure. */
void vtd_describe_fault(char *buf, size_t n, uint64_t lo, uint64_t hi);

/* ---- the units (kernel/dev/vtd_unit.c) -------------------------------------- */

/* The boot words: true with `iommu=on` and no `iommu=off` (off wins).
 * Off by default. kexec keeps either word. */
bool vtd_iommu_wanted(const char *cmdline);

/* With `iommu=on`: start every unit the probe could read: its invalidation
 * queue, its fault interrupt and the thread that logs faults ("vtd:"
 * lines; a problem goes to the RESULTS box). Translation and interrupt
 * remapping stay as the firmware left them. Without the word it does
 * nothing at all. Once, at boot, after vtd_probe; interrupts on, no lock
 * held. */
void vtd_units_start(void);

/* Does [phys, phys + len) touch any unit's register set (from the DMAR
 * table, every unit, started or not)? The kernel keeps those pages: they
 * are never mapped for a process (resource.c). Lock-free: the set is
 * written once at boot, before user space. */
bool vtd_regs_overlap(uint64_t phys, uint64_t len);
