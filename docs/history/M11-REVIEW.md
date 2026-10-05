# M11 review: the IOMMU (VT-d DMA and interrupt remapping)

An independent read of milestone M11 at main 1f93f4d (every stage merged,
and its join), by an agent that wrote none of it, against CODING-GUIDE.md,
SECURITY.md, ARCHITECTURE.md ("The IOMMU" and the threat model),
docs/M11-PLAN.md (as built, and each stage's deviations),
docs/history/M11-SPEC-CHECK.md and Intel's VT-d specification revision 4.1
(D51397-016; section numbers below are 4.1's).

The IOMMU is security code: its point is to contain a hostile driver and its
device. Read, in order: `kernel/dev/vtd_pt.c` (tables, pin counts, gathers),
`vtd_qi.c` (the queue, the wait slots, the error recovery, every
invalidation), `vtd_unit.c`, `vtd_domain.c` (context entries, domain ids,
the mute), `vtd_boot.c` (the handover, the jump off), `vtd_fault.c`,
`vtd_ir.c` and `vtd_irq.c` (interrupt remapping), `vtd_report.c`,
`kernel/acpi/dmar.c`, the headers (`<jam/iommu.h>`, `<jam/irq_remap.h>`,
`vtd_internal.h`, `vtd_domain.h`, `vtd_pt.h`, `vtd_ir.h`),
`kernel/object/dma_cap.c` and `vmo.c`'s pin paths, `interrupt.c`'s teardown,
`ioapic.c`'s remapped pins, the kexec / panic / firmware-reset off path,
`resource.c`'s MMIO rules, drv/hda's `vtdtest` and the `iommu` command.

Severity: **High** lets a device reach memory its driver didn't pin (or a
page after its unpin or its cap's close, before the unit confirmed), or
raise an interrupt it wasn't given, on the PC; **Medium** is the same on a
narrower path, a broken VT-d rule that can open such a hole, or a device
that can take a CPU away; **Low** is a narrow case, a wrong claim, a gap in
a check, or a design point.

**Nothing High was found** (3 Medium, 11 Low). What held up under a careful read:

- **Only what is pinned is mapped.** A domain holds the cap's pins and the
  function's RMRRs, nothing else; IOVA = physical address. `vmo_pin` maps
  before it publishes (a closed cap's in-flight pin is unmapped and waited
  for), `vmo_unpin` unmaps, invalidates and waits before `range_remove`, and
  every unmap error (`ERR_TIMED_OUT`, `ERR_IO`, anything) keeps the pages
  for good (`pin_keep`). A paged VMO's pinned pages can't be decommitted or
  shrunk away (the range), and a VMO can't be destroyed while pinned. Two
  pins of a page share a mapping by a pin count; a page is unmapped only at
  0. Physical VMOs can't cover RAM, the interrupt window, the local APIC,
  the I/O APICs, the HPET, ECAM, MSI-X tables or any VT-d register page.
- **Close and rebind.** A new cap points the context entry at its empty
  domain (one 16-byte CMPXCHG16B, then device-selective context-cache and
  the old domain's IOTLB invalidation, Table 25) before anyone can turn Bus
  Master Enable on; a closed cap's domain is taken away by the "dma
  quarantine" thread, which waits for pins in flight and frees the pages
  only after the domain id's context-cache and IOTLB invalidation
  (`vtd_did_free`, Table 25's "re-use domain-id") completed.
- **The tables.** Every page-table page is cleared and flushed before its
  linking entry is written; each leaf and link is flushed after it is
  written; only the ignored software bits (61:52, M11-SPEC-CHECK) change
  without a flush. Emptied tables go on the gather and are freed only after
  the invalidation (IH = 0 when a table was unlinked, 6.5.2.3 and the text
  after Table 25: one page-selective invalidation of an address in the
  unlinked span covers it). Page-selective runs are naturally aligned
  power-of-two blocks of at most 2^MAMV (`vtd_split_aligned`), and fall back
  to domain-selective past the batch. Caching mode (CM = 1) invalidates new
  mappings too, and not-present context entries with domain id 0 (6.2.2);
  domain id 0 is never handed out. CAP.RWBF is honoured.
- **The queue.** Waits complete in queue order with the fence flag; a
  waiter's status word can't be fooled by a late write from a slot's
  previous owner (fresh sequence numbers, written behind it in the queue);
  a refused descriptor (IQE) is replaced by a harmless wait, its batch's
  caller gets `ERR_IO` (6.5.2.10), and every caller treats `ERR_IO` like a
  time-out (pages kept). IOTLB invalidations ask for read and write
  draining where CAP.DRD/DWD say so (6.5.4; the PC has both).
- **Interrupts.** Every IRTE is source-validated against the function's
  full requester id (SVT 01b, SQ 00b), fixed delivery, physical mode, a
  vector from `vector_alloc` (0x31 and up); entry 0 is never handed out;
  entries are written high half first, cleared low half first, flushed when
  a unit doesn't snoop, and an index is reused only after every unit's
  interrupt entry cache dropped it (a failed invalidation keeps it out of
  use). Compatibility-format interrupts are blocked (EIME, or CFI turned
  off). The I/O APIC's pins validate against its DMAR requester id. Drivers
  can't make interrupt objects (`interrupt_create_msi` needs RIGHT_MANAGE on
  the device: devmgr's), so the 1024 entries are bounded by devmgr.
- **`iommu=off`** is the old behaviour: no unit is started, no domain is
  made (`iommu_domain_create` says `ERR_NOT_SUPPORTED`, the cap keeps the
  quarantine), no remapping entry, the jump finds no started unit. The only
  differences are the probe's read-only `vtd:` lines and the VT-d register
  pages refused to physical VMOs (both intended).
- **Lock order and context.** "dma owner" before "vtd context" before
  "vtd domain" before "vtd queue" before "vtd gcmd"; the fault ring and log
  locks are irqsave leaves; no invalidation is waited for with a spinlock
  held or from an interrupt handler (interrupt objects free their entry in
  the teardown, preemption off, interrupts on: a bounded poll). The jump
  and panic path takes no lock.
- **The DMAR parser** checks every length before it reads.

## Findings

| # | Sev | Where | What |
|---|-----|-------|------|
| 1 | Medium | `kernel/dev/vtd_domain.c:122` (`vtd_ctl_init`) | **The root table is zeroed with `memset` (`PMM_ZERO`) and never flushed from the CPU's caches.** Only the root entries for buses that get a context table are flushed (`table_for`). On a unit whose table walks don't snoop (ECAP.C = 0: the PC, `ecap f050da`) the unit reads the other 240-odd root entries from memory, which still holds the page's previous contents until the dirty lines happen to be evicted: a stale qword with bit 0 set is a *present* root entry naming an arbitrary page as a context table, whose entries may be present and pass-through. A request carrying a requester id on such a bus (a device that spoofs its requester id, a function the enumeration didn't see) is then translated through garbage instead of faulting "root entry not present". Every other table (context tables, page tables, the IRTE table) is flushed whole after it is cleared. **QEMU can't show it** (its unit reads guest memory directly), so no QEMU test fails; on the PC it depends on what the page held. **Reproduce:** read `vtd_ctl_init`: `pmm_alloc_page_phys(PMM_ZERO)`, no `vtd_flush_lines`. |
| 2 | Medium | `kernel/dev/vtd_fault.c:138` (`fault_irq`), `vtd_domain.c:491` (`vtd_domain_fault_seen`) | **A fault storm that can't be muted takes a CPU.** The mute (fault processing disabled in the context entry after 8 DMA faults) covers only DMA faults of a function a unit covers. Interrupt-remapping faults (reasons 20h-26h) are never muted (an IRTE's FPD can't be used: the index is past the table, not present, or someone else's), nor are DMA faults from a requester id with no covered function. Each fault is one fault event interrupt (with one fault recording register, 1 record per interrupt) plus a wake-up and a pass of the log thread, with no limit. **Reproduce:** a driver points any DMA its device makes continuously at the interrupt window, e.g. drv/hda's DMA position buffer (DPLBASE = 0xfee00000: one compatibility-format write, fault 25h, per position update), the RTL8125's receive buffers (one per frame at line rate), or edu's DMA in a loop; `iommu` shows the fault interrupts climbing for as long as it likes. **Allows:** a hostile driver keeps the fault interrupt's CPU (and the log thread) busy indefinitely, whatever its device's MSI masking; the log stays quiet (8 lines) so the storm is invisible except in the counters. |
| 3 | Medium | `kernel/object/dma_cap.c:193` (`take_domain_away`), `:248` (`release_batch`), `:216` (`requeue`) | **When the unit doesn't confirm that a closed cap's domain is gone, its pages are still released.** `take_domain_away` failing (the detach's or the domain id's invalidation timed out or was refused) sets `b->dom = NULL` and requeues the batch as a plain quarantined one: its pages go back after `DMA_QUARANTINE_TIMEOUT_NS` (30 s), or 1 s after the next driver turns Bus Master Enable on. But the domain is "kept for good" with its page table **still mapping those pages**, and the unit may still hold the function's old context entry: a later switch of the entry invalidates device-selectively *by the domain id in memory* (6.5.1.1: "context-cache entries associated with the specified source-id **and domain-id**"), not the stale one, and the stale domain id is never invalidated because it is never freed. So once the next driver turns bus mastering on, its device can still translate through the dead domain into pages that were freed and handed to someone else: a use-after-free, not the bounded grace period the quarantine gives without an IOMMU. It is also inconsistent with `vmo.c`'s `pin_keep` (an unconfirmed unmap keeps the pages for good). **Reproduce (QEMU, `iommu=on`):** pin a page with a cap, make the unit's invalidations fail (turn its queue off: `vtd_qi_disable`), close the cap, `dma_quarantine_flush`: the page is released (`released` rises); QEMU's context cache and IOTLB still hold the old entry. Needs an invalidation failure, which a device can't cause on the PC as far as I can see (it can't stall draining), hence Medium. |
| 4 | Low | `kernel/dev/vtd_boot.c:199` (`unit_of`), `kernel/object/dma_cap.c:535` | **Requester-id aliasing isn't handled** (the plan: "refuses a cap for an aliased function until it is needed"). Behind a PCIe-to-PCI(-X) bridge, conventional devices' requests carry (secondary bus, 00.0) or the bridge's id. Then a device behind it at another slot uses the domain (and the IRTEs, by source validation) of whichever function has that id: its driver reaches that function's pinned buffers and can raise its interrupts. The PC has no such bridge (its 6 bridges are PCIe root ports), so its devices fault instead of misbehaving. |
| 5 | Low | `kernel/dev/vtd_ir.c:56` (`vtd_irte_encode`), `:90` (`vtd_ir_rte_encode`), `kernel/include/jam/irq_remap.h:39` | The documented vector guard accepts 16-31. Those are architecturally reserved exception vectors (16 #MF, 17 #AC, 18 #MC, 19 #XM, 20 #VE, 21 #CP); the local APIC delivers them, into the exception handlers. The kernel never asks (`vector_alloc` starts at 0x31, COM1 is 0x30), so only the guard is wrong. |
| 6 | Low | `kernel/dev/vtd_domain.c:539` (`DRIVER_MAX_TABLES`), M11-PLAN "Page tables", ARCHITECTURE "Pins" | "512 table pages = 1 GiB of scattered pins" holds only when pins are dense within each 2 MiB: a leaf table covers 2 MiB of IOVA = physical addresses, so pins of pages scattered one per 2 MiB need a table page each, and the cap is reached at ~505 pinned pages. On the PC usb-bus pins a 384-page non-contiguous DMA32 pool, the scratchpad and 17 pages per bulk interface (~450 pages with a few sticks), the RTL8125 ~260; on a fragmented machine (a long soak, a usb-bus restart) usb-bus's pin could fail `ERR_NO_RESOURCES`. Design question C. |
| 7 | Low (deviation D2 a) | `kernel/object/dma_cap.c:549`, `vtd_pt.c:179` | Table pages are charged to devmgr's job, which has no page limit; the drivers' 16 MiB job limits don't see them. Bounded by finding 6's cap: at most 2 MiB per cap, and caps are made by devmgr only (one per binding, the old one's domain freed by the release thread). Acceptable for M11; design question B. |
| 8 | Low | `kernel/dev/vtd_boot.c:297` (`make_identity`), `:329` | The pass-through domain is built at every `iommu=on` boot for the tests only. On a unit without ECAP.PT it is an identity map of all RAM built at boot: about one table page per 2 MiB of RAM (~128 MiB on a 64 GiB machine), and a domain that, if a bug ever put a driver's function in it, gives the device all of RAM. The PC has PT, so it costs only a domain id there. Design question A. |
| 9 | Low | `kernel/dev/vtd_domain.c:250` (`finish`), `vtd_pt.c:424` | When a gather's invalidation fails, its table pages stay counted as `pending` (correct: never freed) but stay linked through their `struct page` nodes to a list head on the caller's stack, which is gone when `finish` returns. Nothing walks or frees those pages again (the domain refuses to be destroyed while `pending`), so it is a dangling pointer, not a bug today. |
| 10 | Low | `kernel/dev/vtd_domain.c:594` (`iommu_detach`), `dma_cap.c:196` | `iommu_detach`'s `ERR_BAD_STATE` means "not attached" but is also `vtd_qi_submit`'s "the queue is off", passed up through `vtd_fn_switch_locked`; `take_domain_away` reads it as "a newer cap replaced it". Harmless: the domain destroy that follows fails the same way. |
| 11 | Low (judgement) | `kernel/dev/vtd_boot.c:367` (`vtd_boot_handover`), deviation D1 b | **The 6.6 deviation is acceptable.** 6.6 asks the new tables to translate in-flight requests as the old did; Jam OS's give only the RMRRs, so another in-flight DMA is blocked and logged: the safe direction, and the invalidations that follow end the mixed period. One residual: when a kexec'd kernel left translation on (its jump couldn't turn it off in 10 ms), the old tables live in RAM the new kernel reuses before the takeover; only Bus Master Enable being off (and the display and bridges, never touched, not mastering) makes that window safe. On the PC everything is off at a cold boot. |
| 12 | Low | `drivers/hda/vtdtest.c:258` | Phase B (the RIRB pointed at 0xfee00000) runs whenever phase A was blocked, without knowing whether interrupt remapping is on; with translation on but remapping off (an I/O APIC without a DMAR scope) the write is a real interrupt. `window_write_safe` keeps it to a fixed-delivery vector of 32 or more to APIC id 0, counted and ignored. Test-only. |
| 13 | Low (judgement) | `kernel/test/test_dma_iommu.c:60` (`edu_translated`) | The join's note is right: the reset of edu's `dma_faults` is redundant, since every `dma_iommu_*` test makes a new cap before it provokes a fault and the cap's attach starts the count (and clears the mute) itself. Remove it. |
| 14 | Low (found while fixing 2) | `kernel/dev/vtd_qi.c:399` (`log_error`) | Every refused descriptor after the first is a log line, with no limit (the faults stop at 8 per device). Only the kernel can make the queue refuse one, but `vtd_unit_fault_storm_masked`'s 1500 refusals flooded the soak's log until lines cut into each other. |

### Design questions for the owner

- **A (findings 8, 13; the join's note).** Remove the pass-through domain
  (`iommu_device_driven`, `VTD_DOM_PASS`, `make_identity`)? Recommendation:
  yes. The tests that use it (`vtd_domain_pass_dma_lands`,
  `vtd_domain_handover_while_on`) can map just the pages they use in a
  domain of their own; it removes ~120 lines, a domain id, the all-of-RAM
  footgun and the boot-time identity map on units without PT.
- **B (finding 7, deviation D2 a).** Charge a cap's table pages to the
  driver's job (M12's system call review: the cap is made before the
  driver's job exists, so it needs `dma_cap_set_job` to move the domain's
  charge, or devmgr to make the job first), or keep devmgr's job with the
  per-domain cap as the bound?
- **C (finding 6).** Keep 512 table pages per domain (fine on today's PC,
  tight for usb-bus under fragmentation), raise it (2048 = 8 MiB worst
  case), or allocate DMA pools contiguously in the drivers?
- **D (finding 4).** When a machine with a PCIe-to-PCI bridge appears:
  refuse caps for functions behind it (the plan), or give every function
  that shares a requester id one shared domain?
- **E (finding 3's fix).** As fixed below, a closed cap whose domain the
  unit didn't confirm gone keeps its pages, and the release thread retries
  the teardown every second until the unit confirms. The alternative is
  keeping them for good with no retry (as `pin_keep` does).

## Outcomes

| # | Outcome |
|---|---------|
| 1 | **Fixed** (27df4a7 refactor: `vtd_root_table_new`; 643b2ca fix): the cleared root page is flushed whole when the unit doesn't snoop. Test `vtd_domain_root_table_flushed` (pure, runs in every ktest): a new `DBG_VTD_FLUSH` hook records what `vtd_flush_lines` flushes, and every line of the new root page must be in it; failed without the fix ("flushed failed"). The PC is the only place the flush itself matters. |
| 2 | **Fixed** (c319fae): the storm guard. The handler counts its interrupts; past 32 in 100 ms it masks FECTL.IM, the log thread polls the unit every 10 ms (draining records) until a look finds no new event (records, overflow, queue errors), then unmasks; the first storm goes to the RESULTS box, the `iommu` command shows the count. Test `vtd_unit_fault_storm_masked`: 1500 refused descriptors (each one a fault event, 7.3: QEMU's fastest real source; a device's window writes are the same interrupt): 1490 interrupts without the fix (failed), 33-45 with it; unmasked after, and the next single event heard. |
| 3 | **Fixed** (b7e50b1): a batch whose domain the unit didn't confirm gone keeps its domain, its cap (until its pins in flight are done) and its pages, and is retried every second; only a confirmed domain-id invalidation frees them. `dma_quarantine_flush` tries each listed batch once. Test `dma_iommu_unconfirmed_close_keeps_pages` (`KT_SKIP_LIVE`): queue off, close with a page pinned, flush: held (without the fix: released, failed); queue on, flush: freed. Design question E. (c319fae also carries this fix's shortened report line.) |
| 4 | Not fixed: no such bridge on the PC; design question D. |
| 5 | **Fixed** (22b5e88): `vtd_irte_encode` and `vtd_ir_rte_encode` refuse vectors below 32 (`VTD_IR_VECTOR_MIN`). Tests `vtd_ir_irte_refuses`, `vtd_ir_rte_exact_bits` refuse 16, 18, 31; failed without the fix. |
| 6 | Claim corrected in ARCHITECTURE and `vtd_domain.c` (this commit); the cap itself is design question C. |
| 7 | Design question B; no change. |
| 8 | Design question A; no change. |
| 9 | Not fixed: moving kept table pages off the caller's list breaks `vtd_dom_map`'s retry with the same gather in caching mode; harmless as it stands. |
| 10 | Not fixed (harmless, as said). |
| 11 | Accepted as built. |
| 12 | Not fixed (test-only; a driver can't see whether interrupt remapping is on: SYSINFO_IOMMU is translation only). |
| 13 | **Done** (f074617): the reset removed, the comment says why it isn't needed. No test (test-only change). |
| 14 | **Fixed** (2e5593b): the first refusal in the RESULTS box, the next 7 logged, the rest counted, as faults are. No test fails without it: checked by a `vtd_unit` run's line count (1500 lines before, 8 after). |

Also: `tools/vtd-test.sh` knows the two new tests and the lines they
provoke on purpose (9f2ff59).

## Tests run (QEMU)

Baseline at 1f93f4d before any change: `make`, `make KTESTS=0`,
`make check`; `QEMU_SMP=4 tools/qemu-test.sh <out> kt ktest` 378 passed;
the same with `QEMU_IOMMU=eim QEMU_WORDS=iommu=on` 378 passed.

After the fixes: `make`, `make KTESTS=0`, `make check` clean; full ktest
at 4 CPUs 381 passed, and 381 with `QEMU_IOMMU=eim QEMU_WORDS=iommu=on`;
`tools/vtd-test.sh` PASS (both caching modes, pt=off, kexec, panic);
with `QEMU_IOMMU=cm0 QEMU_WORDS=iommu=on`: hda-test (its IOMMU checks
boot included), usb-test, storage-test, net-test PASS, and no `vtd:
fault:` line but hda's provoked reads; the `init` run at 2 CPUs PASS
without and with `QEMU_IOMMU=1 iommu=on`; soak-test at 4 CPUs with
`QEMU_IOMMU=1 QEMU_WORDS=iommu=on` PASS (383 s, 5 loops, 1797 passed, 108
skipped, 0 FAILED; 3 utest runs, 3325 file cycles, 0 FAILED); it showed
finding 14, fixed after it (`ktest=vtd_unit` and `tools/vtd-test.sh` run
again after that fix: PASS, the second time; the first ended in a host-side
QEMU segmentation fault during `vtd_domain_handover_while_on` with
caching mode off, which didn't come back in six more runs of the
`vtd_domain` tests or a full `vtd-test.sh`: QEMU's, not the guest's,
since nothing a guest does should crash QEMU; noted in case it returns).

## What only the PC can show

- Finding 1's flush matters only on a unit that doesn't snoop: the PC's.
  Nothing to see unless something was wrong (a DMA from a requester id on
  an unused bus); `iommu` should show no fault but the checks'.
- Finding 2 on real hardware: the "All tests" boot with `iommu=on` runs
  `vtd_unit_fault_storm_masked` against the real unit (1500 refused
  descriptors, as `vtd_unit_refused_descriptor_reported` does 2); expect its
  line "N fault interrupt(s)" with N about 33 and "unmasked after: yes".
  A real device storm: not provoked by any test (`vtdtest` makes 1-2
  faults).
