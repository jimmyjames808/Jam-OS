# M11 spec check: VT-d bit layouts against Intel's specification

M11 stages 0, 1B and 1C (`kernel/include/jam/vtd.h`, `kernel/dev/vtd_probe.c`,
`kernel/dev/vtd_pt.c/.h`, `kernel/dev/vtd_ir.c/.h`, and the DMAR constants in
`kernel/include/jam/dmar.h` / `kernel/acpi/dmar.c`) were written without the
specification open. Their tests mostly compared the code with itself, so a
wrong bit would pass in QEMU and fail only on the PC. This file checks every
field the code defines or encodes against the PDF, at main 5f76c2e.

**The specification used:**
- **Revision 4.1** (March 2023, order number D51397-016), Intel's PDF at
  `https://cdrdv2-public.intel.com/774206/vt-directed-io-spec%20.pdf`
  (content page intel.com/content/www/us/en/content-details/774206/). It is
  byte-identical (SHA-1 f3cc5f09...) to the archived copy of D51397-016.
  This is the revision the code says it follows; **section, figure and table
  numbers below are 4.1's**.
- **Revision 5.20** (April 2026, D51397-019), the newest,
  `https://cdrdv2-public.intel.com/919688/D51397-019-vt-directed-io-spec.pdf`,
  read for anything that changed since. Where 5.20 differs it is said.

Text was extracted with `pdftotext -layout` and every row read from the
register or entry description table itself, not from the figures alone.

Verdicts: **OK** matches; **WRONG** does not match (fixed below, one commit
each); **STALE** was right in an older revision (3.x) but 4.1 calls the field
reserved.

## Summary

No layout the IOMMU will write is wrong: the second-stage page-table entry
(R, W, SNP, address, the software bits), the IRTE, the remappable MSI address
and data, the remappable I/O APIC entry, IRTA, and every CAP/ECAP field the
code decodes match 4.1 bit for bit, and still match 5.20 in legacy mode. The
existing tests' literal values (IRTE, MSI, RTE, and the Alder Lake and QEMU
CAP/ECAP strings) were recomputed by hand from the spec and are right.

What was wrong:
1. **STALE** register definitions from the 3.x specification: Advanced Fault
   Logging (`VTD_AFLOG` 0x58, `VTD_CAP_AFL` bit 3) and the fault-log status
   bits `VTD_GSTS_FLS`/`VTD_GSTS_AFLS` (29, 28). Revision 4.0 removed advanced
   fault logging; 4.1 marks all four reserved. Unused, removed.
2. **WRONG** citations: vtd_ir.c/.h cite "9.10" for the IRTE of remapped
   interrupts; in 4.1 (and 5.20) that is 9.9 (9.10 is the posted format).
3. **WRONG** decode in the probe's fault-record line: it reported every
   recorded fault as "read"/"write" at an address. For an
   interrupt-remapping fault (reasons 20h-2Fh) the spec says FI bits 63:48
   hold the interrupt index and 47:12 are zero, and the type bits don't
   apply; it also ignored T2 (page request / atomic). Matters for the PC's
   firmware-left faults now and for stage 1A's fault log later.

Notes for the later stages (not errors in this code) are at the end.

## Registers (`kernel/include/jam/vtd.h`), 11.4 Table "Register Descriptions"

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| VER_REG | 0x000, major 7:4, minor 3:0 | 000h; MAX 7:4, MIN 3:0 (11.4.1, Fig 11-1) | OK |
| CAP_REG | 0x008, 64 | 008h, 64 (11.4.2) | OK |
| ECAP_REG | 0x010, 64 | 010h, 64 (11.4.3) | OK |
| GCMD_REG | 0x018, 32, write-only | 018h, 32, fields WO (11.4.4.1) | OK |
| GSTS_REG | 0x01c, 32 | 01Ch, 32 (11.4.4.2) | OK |
| RTADDR_REG | 0x020, 64 | 020h, 64 (11.4.5) | OK |
| CCMD_REG | 0x028, 64 | 028h, 64 (11.4.6.1) | OK |
| FSTS_REG | 0x034 | 034h, 32 (11.4.7.1) | OK |
| FECTL_REG | 0x038 | 038h (11.4.7.2) | OK |
| FEDATA_REG | 0x03c | 03Ch (11.4.7.3) | OK |
| FEADDR_REG | 0x040 | 040h (11.4.7.4) | OK |
| FEUADDR_REG | 0x044 | 044h (11.4.7.5) | OK |
| AFLOG_REG | 0x058 | 058h **Reserved** (4.0 history: "Removal of Advanced Fault Logging") | STALE: removed |
| PMEN_REG | 0x064 | 064h (11.4.8.1) | OK |
| PLMBASE/PLMLIMIT | 0x068/0x06c, 32 | 068h/06Ch, 32 (11.4.8.2-3) | OK |
| PHMBASE/PHMLIMIT | 0x070/0x078, 64 | 070h/078h, 64 (11.4.8.4-5) | OK |
| IQH/IQT/IQA | 0x080/0x088/0x090, 64 | 080h/088h/090h, 64 (11.4.9.1-3) | OK |
| ICS_REG | 0x09c, 32 | 09Ch, 32 (11.4.9.4) | OK |
| IRTA_REG | 0x0b8, 64 | 0B8h, 64 (11.4.10) | OK |

### Capability Register (11.4.2, Figure 11-2)

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| ND | 2:0, domains 2^(4+2·ND) | 2:0; 000b=16 ... 110b=64K, **111b reserved** | OK (a reserved 7 would print 262144; real units never report it) |
| AFL | bit 3 | 3: RsvdZ (Reserved) | STALE: removed |
| RWBF | 4 | 4 | OK |
| PLMR | 5 | 5 | OK |
| PHMR | 6 | 6 | OK |
| CM | 7 | 7 | OK |
| SAGAW | 12:8; bit 1=3 levels (39), 2=4 (48), 3=5 (57) | 12:8; bit 1 39-bit/3-level, 2 48/4, 3 57/5, 0 and 4 reserved | OK |
| MGAW | 21:16, width = N+1 | 21:16, MGAW = N+1 | OK |
| ZLR | 22 | 22 | OK |
| (DEP) | not defined | 23 deprecated, 0 | OK |
| FRO | 33:24, records at FRO·16 | 33:24, X+16·Y | OK |
| SLLPS | 37:34, bit 0 2M, bit 1 1G | 37:34 (SSLPS), bit 0 2M, bit 1 1G | OK |
| PSI | 39 | 39 | OK |
| NFR | 47:40, count N+1 | 47:40, N+1 | OK |
| MAMV | 53:48 | 53:48 | OK |
| DWD | 54 | 54 | OK |
| DRD | 55 | 55 | OK |
| FL1GP | 56 | 56 (FS1GP; renamed "first stage") | OK |
| PI | 59 | 59 | OK |
| FL5LP | 60 | 60 (FS5LP) | OK |
| (ECMDS) | not defined | 61 | OK |
| ESIRTPS | 62 | 62 | OK |
| ESRTPS | 63 | 63 | OK |

Same in 5.20.

### Extended Capability Register (11.4.3, Figure 11-3)

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| C | 0 | 0, page-walk coherency; covers root, context, legacy second-stage tables **and the interrupt-remapping table**; the queue and wait-status writes are always snooped | OK (so vtd_ir's flush is right to follow ECAP.C too) |
| QI | 1 | 1 | OK |
| DT | 2 | 2 | OK |
| IR | 3 | 3 | OK |
| EIM | 4 | 4 | OK |
| PT | 6 | 6 (5 deprecated) | OK |
| SC | 7 | 7, allows SNP=1 in second-stage leaves | OK |
| IRO | 17:8, IOTLB regs at IRO·16 | 17:8, X+16·Y | OK |
| MHMV | 23:20 | 23:20 | OK |
| NEST | 26 | 26 | OK |
| PRS | 29 | 29 | OK |
| PASID | 40 | 40 | OK |
| SMTS | 43 | 43 | OK |
| SLTS | 46 | 46 (SSTS) | OK |

5.20 adds IRREQ (62) and EIMER (61), none on the PC's generation (see notes).

### Global Status Register (11.4.4.2, Figure 11-5)

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| TES | 31 | 31 | OK |
| RTPS | 30 | 30 | OK |
| FLS | 29 | 29:28 **RsvdZ** | STALE: removed |
| AFLS | 28 | (same) | STALE: removed |
| WBFS | 27 | 27 | OK |
| QIES | 26 | 26 | OK |
| IRES | 25 | 25 | OK |
| IRTPS | 24 | 24 | OK |
| CFIS | 23 | 23 (meaningful only with IRES=1 and EIME=0) | OK |

### Fault Status, Fault Recording, PMEN, IRTA

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| FSTS.PFO | 0 | 0 (11.4.7.1, Fig 11-10) | OK |
| FSTS.PPF | 1 | 1 | OK |
| FSTS.IQE | 4 | 4 | OK |
| FSTS.ICE | 5 | 5 | OK |
| FSTS.ITE | 6 | 6 | OK |
| FSTS.FRI | 15:8 | 15:8 | OK |
| FRCD.F | hi bit 63 | bit 127 (11.4.7.6, Fig 11-15) | OK |
| FRCD.T1 | hi bit 62, 0 write 1 read | 126; T1/T2: 00 write, 01 page request, 10 read, 11 AtomicOp | OK (field); the probe's use: WRONG, below |
| FRCD.T2 | comment "bit 28" of hi | 92 = hi bit 28 | OK |
| FRCD.FR | hi 39:32 | 103:96 | OK |
| FRCD.SID | hi 15:0 | 79:64 | OK |
| FRCD.FI | lo & ~0xfff | 63:12: page address for translation faults; for interrupt faults other than 25h, **63:48 = interrupt_index**, 47:12 zero; 25h undefined | field OK; probe's use WRONG, below |
| PMEN.EPM | 31 | 31 (11.4.8.1, Fig 11-16) | OK |
| PMEN.PRS | 0 | 0 | OK |
| IRTA.EIME | 11 | 11 (11.4.10, Fig 11-30) | OK |
| IRTA.S | 3:0, 2^(S+1) entries | 3:0, 2^(X+1) entries; 10:4 RsvdZ; IRTA 63:12 4 KiB aligned | OK |

## Second-stage paging entries (`vtd_pt`), 9.8 Tables 37-43 and 3.7

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| R | bit 0 | 0 in every second-stage entry | OK |
| W | bit 1 | 1 | OK |
| Present | R or W set | 3.7: "R and W both 0" = not used to reference or map: not present | OK |
| PS | bit 7, never set | 7 in PDPE/PDE: must be 0 for a table reference; reserved in PML4E/PML5E; **ignored** in the 4 KiB PTE | OK (0 everywhere) |
| SNP | bit 11, leaves only, only when ECAP.SC | 11 in leaves (Table 43); in non-leaf entries 11 is **Reserved (0)**; in a leaf it is reserved when SC = 0 (3.7 list) | OK |
| ADDR | 51:12 mask; written as the page's address | (HAW-1):12; 51:HAW reserved (0) | OK: every address written is below 2^addr_bits <= HAW (geometry), so 51:HAW stay 0 |
| Bits 6:2, 10:8 | 0 | ignored, A (8), D (9) ignored in legacy mode (SSADE only in scalable mode); EMT/IPAT (5:3, 6) ignored in legacy mode (TTM=00b) | OK (0) |
| Software bits | 61:52 (10 bits, pin and use counts), in leaves **and** table entries | 61:52 "IGN: Ignored by hardware" in all of Tables 37-43 (PML5E, PML4E, PDPE both kinds, PDE both kinds, PTE) | OK |
| Bit 62 | 0 | **Reserved (0)** (older revisions' "TM, transient mapping" is gone) | OK (never set) |
| Bit 63 | 0 | Ignored | OK |
| ATS / device TLB | not used | translation completions carry R/W/U/N, not the ignored bits; DT=0 on the PC | OK, no conflict |
| 5.20: bits 61, 62 | (61 used by software) | 5.20 Tables 46-47: 61 = IR, 62 = IW **only when RTADDR.SSIRWE = 1**, else 61 ignored and 62 reserved. SSIRWE set with TTM = 00b (legacy) is an error (5.20 fault RTA.1.4) | OK for Jam OS (legacy mode). A future scalable-mode user must not use bit 61 |
| Levels from SAGAW | shallowest offered level count covering MGAW, else the deepest offered | AW must be one SAGAW offers (9.3, 11.4.2); 3.7: input addresses above MGAW fault | OK |
| addr_bits | min(MGAW, 12+9·levels, HAW) | 3.7: "minimum of MGAW and the AGAW" checked; ADDR limited by HAW | OK |
| Context AW (vtd_pt_aw) | levels - 2 (1 = 39-bit/3-level, 2 = 48-bit/4-level) | 9.3, AW bits 66:64: 001b 39-bit, 010b 48-bit, 011b 57-bit | OK |
| Table walk index | iova >> (12 + 9·(level-1)) & 511 | 3.7: bits 11:3 of the entry address = 47:39 / 38:30 / 29:21 / 20:12 | OK |
| Mapping without invalidation when CM = 0 | yes | 11.4.2 CM: with 0, "invalidations are not required for modifications to individual not present" entries; required when permissions decrease | OK |
| Unmap with tables unlinked: IH = 0 | told to the caller (ops->invalidate) | 6.5.2.3: IH = 1 keeps paging-structure caches | OK |

The PC's expected unit (Linux's "cap d2008c40660462 ecap f050da", as the plan says) decodes by hand to: ND 2 (256), SAGAW 4-level only, MGAW 39, CM 0, RWBF 0, FRO 0x40 (0x400), NFR 0 (1 record), PSI 1, MAMV 18, SLLPS 3; ECAP C 0, QI 1, IR 1, EIM 1, PT 1, SC 1, IRO 0x50 (0x500). vtd_pt picks 4 levels with addr_bits 39, AW 2. The ktest strings for it and for QEMU's unit match this hand decode.

## Interrupt remapping (`vtd_ir`)

### IRTE for remapped interrupts (9.9, Figure 9-9)

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| P | lo 0 | 0 | OK |
| FPD | lo 1 | 1, evaluated even when P = 0 | OK (cleared with the entry) |
| DM | lo 2, 0 physical | 2, 0 physical | OK |
| RH | lo 3, 0 | 3 | OK |
| TM | lo 4, 1 level | 4, 1 level | OK |
| DLM | 7:5, 0 fixed | 7:5, 000b fixed (edge or level) | OK |
| AVAIL | not used | 11:8, ignored by hardware | OK |
| Reserved | 14:12, 31:24 written 0 | RsvdZ when P = 1 | OK |
| IM | lo 15, 0 | 15, 0 remapped, 1 posted | OK |
| Vector | 23:16 | 23:16 | OK |
| DST x2APIC | 63:32 when eim | 63:32 when IRTA.EIME = 1 | OK |
| DST xAPIC | 47:40, 63:48 and 39:32 zero | EIME = 0: 47:40 = APIC id 7:0; 63:48, 39:32 reserved | OK |
| SID | hi 15:0 | 79:64 | OK |
| SQ | hi 17:16 | 81:80; 01b ignore SID bit 2, 10b bits 2:1, 11b bits 2:0 | OK |
| SVT | hi 19:18, 1 = RID, 2 = bus range, 0 and 3 refused | 83:82; 01b RID+SQ, 10b bus range (SID 15:8 start, 7:0 end), 00b none, 11b reserved | OK |
| Reserved | hi 63:20 zero | 127:84 RsvdZ | OK |
| Update order | present: hi then lo; change: lo only with hi equal; clear: lo then hi | 5.1.4: hardware reads the whole IRTE as a single operation | OK |
| Citation | "VT-d 9.10" (vtd_ir.c:5, vtd_ir.h:3, :44) | 9.9 (9.10 is the posted-interrupt IRTE) | WRONG: fixed |

Same in 5.20 (only DLM's NMI text changed).

### Remappable MSI (5.1.2.2 Tables 11-12, 5.1.3, 5.1.5, 5.1.5.2 Figure 5-4)

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| Address 31:20 | 0xfee | FEEh | OK |
| Handle 14:0 | 19:5 | 19:5 | OK |
| Interrupt format | bit 4 = 1 | 4 = 1 | OK |
| SHV | bit 3 = 0 | 3; with 0 interrupt_index = handle and data is ignored | OK |
| Handle 15 | bit 2 | 2 | OK |
| 1:0 | 0 | don't care | OK |
| Data | 0 | 5.1.5.2 programs 0h; ignored with SHV = 0 | OK |
| Upper address | 0 | | OK |
| SHV = 0 vs 5.1.5.2 | single-vector messages only (pci_msi.c clears MME) | 5.1.5 lists "SHV = 0; handle = interrupt_index" as valid; 5.1.5.2's SHV = 1 is needed only for multi-message MSI | OK |

### Remappable I/O APIC entry (5.1.5.1, Figure 5-3)

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| Vector | 7:0, must equal the IRTE's | 7:0; must match the IRTE's for level pins (EOI broadcast) | OK |
| Delivery mode | 10:8 = 000 | 10:8 = 000b (keeps SHV clear) | OK |
| Index 15 | bit 11 | 11 | OK |
| Polarity | 13 | 13 | OK |
| Trigger | 15, equal to IRTE.TM | 15, must match IRTE TM | OK |
| Mask | 16 | 16 | OK |
| Format | 48 | 48 = 1 | OK |
| Index 14:0 | 63:49 | 63:49 | OK |
| Delivery status, remote IRR | 12, 14 not written | read-only | OK |

### Table and source ids

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| IRTE size | 16 bytes, 256 per page | 128 bits (5.1.3) | OK |
| Table size | 256..65536, power of two | up to 64K; index >= size faults (21h) | OK |
| IRTA value | phys, EIME 11, S | 11.4.10 | OK |
| I/O APIC / HPET requester id | start bus : path[0] dev.fn | 8.3.1: HPET (and ANDD) scope's start bus = SID 15:8, path = SID 7:0; I/O APIC: path from the host bridge, one step for a root-complex device | OK |

## The probe's decoders (`vtd_probe.c`)

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| Caps line | every CAP/ECAP field above | as above | OK |
| Register span | 2^Size pages, Size 3:0 | 8.3 DRHD Size bits 3:0, 2^N 4 KiB pages | OK |
| Fault records | NFR+1 records at FRO·16, 16 bytes each, F checked in the upper half first | 11.4.7.6; footnote: read the top doubleword and check F first | OK |
| Fault record line | "BB:DD.F read/write at <FI>, reason R" for every record | interrupt faults (20h-2Fh): FI 63:48 = index, type bits n/a; 25h: FI undefined; T1/T2 2 bits (write, page request, read, atomic) | **WRONG: fixed** |
| Status line | TES, IRES, QIES, RTPS, IRTPS, CFIS; PMEN EPM or PRS; FSTS PPF/PFO; IQE/ICE/ITE | as above | OK |

## DMAR constants (`dmar.h`, `dmar.c`), chapter 8

| Field | Code | Spec 4.1 | Verdict |
|---|---|---|---|
| Header HAW | byte 36, +1 | 36, HAW = N+1 (8.1) | OK |
| Header flags | byte 37: 0 INTR_REMAP, 1 X2APIC_OPT_OUT, 2 DMA_CTRL_PLATFORM_OPT_IN | 37, the same (8.1) | OK |
| First structure | byte 48 | 48 | OK |
| Structure types | 0 DRHD, 1 RMRR, 2 ATSR, 3 RHSA, 4 ANDD, 5 SATC, 6 SIDP | 8.2, the same | OK |
| DRHD | flags 4 (bit 0 INCLUDE_PCI_ALL), size 5 (3:0), segment 6, base 8, scopes 16 | 8.3 | OK |
| RMRR | segment 6, base 8, limit 16 (inclusive), scopes 24 | 8.4 | OK |
| ATSR / SATC | flags 4 (bit 0 ALL_PORTS / ATC_REQUIRED), segment 6, scopes 8 | 8.5, 8.8 | OK |
| RHSA | fixed 20 | 8.6, 20 bytes | OK |
| ANDD | device number 7, name from 8 | 8.7 | OK |
| Scope | type 0, length 1, enum id 4, start bus 5, path from 6 (dev, fn pairs) | 8.3.1 | OK |
| Scope types | 1 endpoint, 2 bridge, 3 I/O APIC, 4 HPET, 5 ACPI | 8.3.1 | OK |

## Fixes

| # | What | Commit | Test |
|---|---|---|---|
| 1 | STALE AFLOG/AFL/FLS/AFLS removed from vtd.h | (filled in below) | none practical: unused definitions deleted; the build is the check |
| 2 | IRTE citations 9.10 -> 9.9 | | comments only |
| 3 | Fault-record decode: interrupt index for reasons 20h-2Fh, the 2-bit request type | | `vtd_describe_fault_per_spec` (exact strings) |

Also added: `vtd_spec_bits_literal` pins every vtd.h, vtd_pt.h and vtd_ir.h
constant and a raw leaf entry to the spec's numbers written as literals, so a
wrong constant can no longer pass by agreeing with itself.

## Notes for the later stages (not errors here)

- **1A, RWBF**: with CAP.RWBF = 1 a write-buffer flush (GCMD.WBF) is needed
  after table updates (11.4.2, 6.8); vtd_pt only flushes cache lines. The PC
  reports RWBF = 0; QEMU too.
- **1A, PSI**: page-selective invalidation takes a naturally aligned
  2^AM-page region (6.5.2.3); a gather's runs are not aligned, so 1A must
  round each run up to an aligned power of two (or go domain-wide).
- **D1, context entries**: updating a present legacy context entry's
  SSPTPTR or TT needs a 16-byte aligned atomic write that changes DID with
  it (6.2.2.1, Context-Entry Programming Considerations). The plan's "switch the context entry to
  the new domain" changes DID and SSPTPTR of a present entry: use
  CMPXCHG16B, or go through not-present with its invalidation.
- **D1, PMEN**: 4.1 says new software should not use the protected memory
  registers (deprecated); turning them off at handover, as planned, is
  what it allows.
- **E, CFI and EIM**: with IRTA.EIME = 1 compatibility-format interrupts are
  blocked whatever CFIS says (5.1.4), as the plan expects.
- **5.20 additions** (newer hardware than the PC): ECAP.IRREQ (62) blocks
  MSIs while interrupt remapping is off; ECAP.EIMER (61) blocks everything
  until EIME = 1. Worth decoding in the probe when a machine reports them.
