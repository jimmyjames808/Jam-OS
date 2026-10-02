/* Intel VT-d remapping hardware: the register map and its bits (Intel
 * VT-d specification 4.1, chapter 11), and the boot-time probe.
 *
 * The probe (kernel/dev/vtd_probe.c) reads the DMAR table (<jam/dmar.h>)
 * and each unit's registers and logs what it finds, every line starting
 * "vtd:". It only READS: no VT-d register is written, so a machine runs
 * exactly as before it. What the firmware left on (translation, interrupt
 * remapping, protected memory regions, recorded faults) is reported, not
 * changed. The design for the IOMMU itself is docs/M11-PLAN.md. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- register offsets (VT-d chapter 11) ------------------------------------- */

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
#define VTD_AFLOG    0x058   /* 64: advanced fault log */
#define VTD_PMEN     0x064   /* 32: protected memory enable */
#define VTD_PLMBASE  0x068   /* 32: protected low memory base */
#define VTD_PLMLIMIT 0x06c   /* 32: and its limit */
#define VTD_PHMBASE  0x070   /* 64: protected high memory base */
#define VTD_PHMLIMIT 0x078   /* 64: and its limit */
#define VTD_IQH      0x080   /* 64: invalidation queue head */
#define VTD_IQT      0x088   /* 64: invalidation queue tail */
#define VTD_IQA      0x090   /* 64: invalidation queue address */
#define VTD_ICS      0x09c   /* 32: invalidation completion status */
#define VTD_IRTA     0x0b8   /* 64: interrupt remapping table address */

/* ---- the Capability Register ------------------------------------------------ */

#define VTD_BITS(v, lo, n) (((uint64_t)(v) >> (lo)) & (((uint64_t)1 << (n)) - 1))

#define VTD_CAP_ND(c)      VTD_BITS(c, 0, 3)    /* domain ids: 2^(4 + 2 * ND) */
#define VTD_CAP_AFL(c)     VTD_BITS(c, 3, 1)    /* advanced fault logging */
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
#define VTD_GSTS_FLS   (1u << 29)   /* fault log pointer set */
#define VTD_GSTS_AFLS  (1u << 28)   /* advanced fault logging on */
#define VTD_GSTS_WBFS  (1u << 27)   /* write-buffer flush in progress */
#define VTD_GSTS_QIES  (1u << 26)   /* queued invalidation on */
#define VTD_GSTS_IRES  (1u << 25)   /* interrupt remapping on */
#define VTD_GSTS_IRTPS (1u << 24)   /* interrupt remapping table pointer set */
#define VTD_GSTS_CFIS  (1u << 23)   /* compatibility-format interrupts pass */

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
#define VTD_FRCD_TYPE1(h)  VTD_BITS(h, 62, 1)     /* T1: 0 write, 1 read (T2, bit 28: a page request) */
#define VTD_FRCD_REASON(h) VTD_BITS(h, 32, 8)     /* the fault reason */
#define VTD_FRCD_SID(h)    VTD_BITS(h, 0, 16)     /* the requester: bus 15:8, dev 7:3, fn 2:0 */

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
