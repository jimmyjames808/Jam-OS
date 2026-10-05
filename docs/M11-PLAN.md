# M11 plan: the IOMMU (VT-d) and interrupt remapping

Status (2026-10-05): **built and tested in QEMU**, stages 0 to 6
([as built](#as-built)); behind the boot word `iommu=on`, off by default.
Next: the PC run (the IOMMU checks entry, then every device, All tests and
`soak 10` with `iommu=on`), then the default turned on. The independent
review (stage 7) is done ([M11-REVIEW.md](history/M11-REVIEW.md)). The design as built is
[ARCHITECTURE.md](../ARCHITECTURE.md#the-iommu). M11 runs beside M11.5 and
M11.6 ([PLAN-M11-M12.5.md](PLAN-M11-M12.5.md#wave-1-m11-m115-and-m116-together)).

Goal ([roadmap](ROADMAP.md#later)): **a device can reach only the memory
its driver pinned for it, and can't send an interrupt it wasn't given.**
Today `dma_cap` decides who may pin and who may turn bus mastering on, but
not where a device writes: a driver that misprograms its device (a bug, or
on purpose) can have it read or write anywhere in RAM, the kernel
included, or write to the interrupt window and raise any vector on any CPU
([what Jam OS defends against](../ARCHITECTURE.md#what-jam-os-defends-against)).

**Done when** (the roadmap's row, unchanged):
- DMA outside a driver's pinned VMOs is blocked (a test in QEMU, and a
  deliberate one on the PC);
- a device's write to the interrupt window sends no interrupt (the same);
- ARCHITECTURE counts drivers as contained, not only crash-isolated;
- and, as every milestone: All tests and `soak 10` on the PC, every
  device still working (USB, the stick, sound, the network).

## Questions for the owner

**Answered 2026-10-03: the owner took every recommendation (1-8).**
Stage 0's PC run may still reopen 2 and 3 if the firmware's tables
say something unexpected.

Each has a recommendation; the plan assumes it until you say otherwise.
Stage 0's PC run (below) may change some of them: it shows what the
board's firmware really describes.

**1. What address does a device see for a pinned page?**
Today `vmo_pin` gives the driver each page's physical address, and the
driver writes those into its device's descriptors. With the IOMMU the
device's addresses go through a page table the kernel builds for that
device (an "I/O virtual address", IOVA, translated to a physical one).
The kernel can choose either:
- *the same number* (IOVA = physical address, an "identity" mapping of
  just the pinned pages): drivers and the system call don't change at
  all; a device still can't reach any page that isn't pinned for it,
  because only pinned pages are in its table. Simple, and nothing to
  allocate;
- *its own numbers* (an IOVA allocator per device): hides physical
  addresses from drivers, and could make a pin of scattered pages look
  contiguous to the device. It costs an allocator, and the drivers would
  see different numbers than today (same calls).

*Recommendation:* the same number. The protection is identical (what
isn't pinned isn't mapped), no driver changes, and allocated addresses
can come later without changing any interface.

**2. Devices that have no driver.**
The PC has functions Jam OS never drives: the NVMe disk, the Wi-Fi, SATA,
the VMD controller, the NVIDIA GPU and its HDMI audio. With the IOMMU on,
the kernel decides what each may reach. *Recommendation:* nothing. Every
function without a current `dma_cap` gets a table with nothing in it, so
any DMA it tries is blocked and logged. Stage 0 logs which functions the
firmware left bus mastering on; if one of them keeps trying (a storm of
faults), its faults are counted and the log goes quiet after the first
few. (The GPU shows the screen from its own memory, not over DMA from
RAM, so blocking it changes nothing on screen; the PC run proves it.)

**3. The USB controller's reserved region (RMRR), if the PC has one.**
Firmware lists memory that a device keeps using after the hand-over
(typically the xHCI's, for USB keyboard emulation before an OS driver
takes over; and the iGPU's, which is off on this PC). The rule is that
the OS keeps such a region mapped for that device. *Recommendation:* map
it, for that device only, for as long as the system runs, as Linux does.
The region is firmware's reserved memory, not RAM Jam OS hands out
(stage 0 checks this: its RMRR line says "not RAM Jam OS manages"), so
the device reaches nothing of Jam OS's through it. The tighter choice
(drop it once usb-bus has reset the controller) saves nothing real and
adds a case to get wrong.

**4. A dead driver's pins: the quarantine.**
Today, when a driver dies with pages still pinned, the kernel can't know
whether the device will still write to them, so it keeps them
("quarantined") until the next driver has turned bus mastering on and a
grace period passed, up to 30 s
([ARCHITECTURE: safe rebind](../ARCHITECTURE.md#drivers-and-services)).
With the IOMMU the kernel can take the device's access away instead:
remove the mappings, wait for the IOMMU to say its caches are flushed,
and free the pages at once. *Recommendation:* do that whenever the
IOMMU translates; keep the quarantine, unchanged, for a machine with no
VT-d or a boot with `iommu=off` (question 6).

**5. A device that faults.**
When a device tries to reach something it can't, the IOMMU blocks it and
records a fault (the device, the address, read or write, the reason).
*Recommendation:* log it (the device, the address, the reason in words),
count it per device (the `iommu` debug command and a RESULTS line), and
show a console notice the first time per device. Nothing more in M11: no
automatic restart or stop of the driver. A fault is a driver bug or an
attack, and both are better seen than hidden behind a restart; devmgr
can get a policy later once real faults have been seen.

**6. On by default, and a way to turn it off.**
*Recommendation:* the first PC runs of DMA remapping and of interrupt
remapping use a boot entry of their own ("Jam OS (IOMMU)", the boot word
`iommu=on`), so the everyday entry keeps working while they are new; once
the PC checks (stage 5) pass, it is on by default, and the boot word
`iommu=off` (added by pressing E in Limine's menu, as `smp=loader` is
today) runs without it, for troubleshooting.

**7. Rebooting by kexec with the IOMMU on.**
Jam OS reboots and handles panics by jumping into a stored copy of the
kernel ([Kexec](../ARCHITECTURE.md#kexec-reboot-and-panic)). The IOMMU
keeps translating across that jump with the old kernel's tables, which
live in RAM the new kernel will reuse for anything. *Recommendation:* the
old kernel turns bus mastering off on every function (it already does),
then turns translation and interrupt remapping off, then jumps; the new
kernel builds its own tables, as after a firmware boot. The window with
the IOMMU off is one where no device may master. The new kernel also
handles finding it on (firmware with pre-boot DMA protection, or a kexec
that failed half-way): it takes over without turning translation off
([the boot handover](#the-boot-handover)).

**8. Read-only pins (later).** A device that only reads a buffer (a
transmit ring, an audio stream, a command ring) could get a read-only
mapping, so even its own driver can't make it write there. It needs a
flag on `vmo_pin` (a system call change). *Recommendation:* not in M11;
put it on M12's list for the system call review, where every flag is
looked at once.

## What the PC is expected to have

From Intel's documentation for Raptor Lake desktop platforms and other
boards' logs; **stage 0's PC run replaces this with facts**, and
[HARDWARE.md](HARDWARE.md) gets them:

- A DMAR table with two remapping units: one for the iGPU (00:02.0,
  disabled on this PC in the firmware: its unit may be absent, or listed
  and not answering), and one with INCLUDE_PCI_ALL for every other device,
  whose scopes name the PCH's I/O APIC and HPET (the requester ids their
  interrupts carry).
- Flags: interrupt remapping supported; maybe "DMA control opt-in"
  (firmware protects DMA before the OS starts, Windows' "Kernel DMA
  Protection"); x2APIC opt-out probably not set (if it is, we talk: Jam OS
  needs x2APIC mode for 28 CPUs).
- RMRRs: maybe one for the xHCI (00:14.0), one for the iGPU.
- The main unit's registers like other Alder/Raptor Lake client parts
  (Linux logs "cap d2008c40660462 ecap f050da" there): 4-level page
  tables (48-bit), 39-bit guest addresses, 256 domain ids, caching mode
  off, page walks **not** coherent with the CPU's caches (the kernel must
  flush each page-table line it writes), queued invalidation, interrupt
  remapping with x2APIC ids, pass-through, snoop control, page-selective
  invalidation, one fault recording register at 0x400, the IOTLB
  registers at 0x500. No device TLBs (ATS) and no scalable mode needed.
- Translation and interrupt remapping off when Jam OS starts, unless the
  firmware's pre-boot DMA protection leaves them on.

## Background: what VT-d does, in one page

- A **remapping unit** sits between PCIe devices and memory. Each DMA a
  device makes carries its **requester id** (bus, device, function). The
  unit looks the id up in a **root table** (one entry per bus) and a
  **context table** (one entry per device and function), which names a
  **domain** (an id, for the unit's caches) and that domain's **page
  table** (the same 4-level shape as the CPU's). An address with no
  mapping, or a context with none, is blocked and recorded as a **fault**.
- The unit caches all of it (the context cache, the IOTLB, the paging
  structure caches). Software changes the tables in memory, then tells
  the unit to drop what it cached (**invalidation**), and must not reuse
  a page the device could still reach until the unit says it is done.
- **Interrupt remapping**: a device's MSI is a 4-byte write to the
  interrupt window (0xfee00000-0xfeefffff). With remapping on, the write
  carries only an index into the kernel's **interrupt remapping table**;
  each entry (IRTE) says which vector goes to which CPU, and which
  requester id may use it. A write in the old ("compatibility") format,
  or naming an entry that isn't there or isn't this device's, is blocked
  and recorded. So a device can raise only the interrupts it was given.
- The specification: Intel Virtualization Technology for Directed I/O,
  revision 4.1 (2023). Chapter 3 (DMA remapping), 5 (interrupt
  remapping), 6 (caching and invalidation), 7 (faults), 8 (the DMAR
  table), 9 (the table formats), 11 (the registers).

## Decisions

### One domain per `dma_cap`

A `dma_cap` is already bound to one PCI function, and a function's newest
cap is its current one (safe rebind). So the cap is the natural owner of
a domain: **each bound cap gets its own domain** (an id and an empty page
table) when it is made, and the function's context entry points at the
current cap's domain. A rebind (a new cap for the function) switches the
context entry to the new, empty domain before bus mastering can come back
on: the dead driver's device can reach nothing it had.

- usb-bus holds the xHCI's cap, so every USB device's transfers share
  usb-bus's domain (USB devices have no requester id of their own). What a
  hostile USB device can reach through a bug in usb-bus is then usb-bus's
  pinned buffers, not all of RAM: the biggest exposure in
  [what Jam OS defends against](../ARCHITECTURE.md#what-jam-os-defends-against)
  shrinks to usb-bus's own memory.
- Functions that share a requester id (behind a PCIe-to-PCI bridge, or a
  quirk) would have to share a domain. Stage 0's scopes and the PCI
  listing say whether the PC has any; the plan assumes none and refuses a
  cap for an aliased function until it is needed.
- Unbound caps (kernel tests, no device) have no domain, as today.
- Domain ids are bounded by the unit (CAP.ND: 256 expected on the PC);
  ids are reused only after their invalidation completed. Out of ids:
  `dma_cap_create` fails `ERR_NO_RESOURCES`.

### IOVA = physical address (question 1)

`vmo_pin` maps each pinned page in the cap's domain at its own physical
address, then returns the same addresses as today. Two pins of the same
page in one domain share one mapping: each mapping keeps a pin count in
the page-table entry's software bits (VT-d leaves bits 52-61 to
software), and is removed when it drops to 0. A pin of a physical VMO
(MMIO: device-to-device DMA) maps that range too, as long as the resource
rules allowed the VMO (they never allow the interrupt window, the local
APIC, the I/O APICs or, from stage 1, the VT-d registers).

### Page tables: legacy mode, 4 levels, 4 KiB pages

- **Legacy mode** root and context tables (VT-d chapter 9), not scalable
  mode (no PASIDs needed; the PC's unit may not have it). Root table one
  page; a context table (one page) per bus that has a device.
- **Second-level page tables** of the width CAP.SAGAW allows (4 levels,
  48-bit, on the PC; QEMU offers 3 or 4). Only 4 KiB leaves: pins are a
  few pages to a few MiB, and superpages would add a split path for
  nothing.
- Read and write on every mapping (pins are writable today; question 8),
  and the snoop bit where the unit has snoop control (ECAP.SC), so a
  device's no-snoop request still sees what the CPU wrote.
- **Not coherent** (ECAP.C = 0 expected on the PC): after writing a
  table entry the kernel flushes its cache line (CLFLUSHOPT, then SFENCE)
  before the unit may look at it. QEMU can't catch a missing flush (it
  reads guest memory directly): the PC is the only place this is proven.
- Page-table pages are kernel memory a driver makes the kernel hold by
  pinning, so they are **charged** to the cap's job (pages) and capped
  per domain (enough for every driver's pins today with room to spare,
  planned 512 table pages = 1 GiB of scattered pins); over the cap the
  pin fails `ERR_NO_RESOURCES`.

### Invalidation: the queue, and what it costs

- **Queued invalidation** (VT-d chapter 6), which the PC's unit has and
  interrupt remapping needs anyway (the interrupt entry cache can only be
  invalidated through the queue). No register-based invalidation path: a
  unit without a queue gets no remapping at all (logged); a unit with
  interrupt remapping always has one.
- One queue per unit, one page (256 descriptors). Submitting takes the
  unit's queue lock; **waiting does not**: each caller appends an
  invalidation-wait descriptor that writes a value to a slot of its own,
  drops the lock and polls its slot with a deadline (100 ms; past it the
  call fails `ERR_TIMED_OUT`, the unit's invalidation errors are logged
  and a RESULTS line says so). Expected time on the PC: a few
  microseconds.
- **Mapping needs no invalidation** when CAP.CM = 0 (the unit caches no
  not-present entries; the PC). With CM = 1 (QEMU's `caching-mode=on`) a
  new mapping is invalidated like a removed one; the code takes the
  unit's flag, and the QEMU tests run both ways.
- **Unmapping** clears the entries, flushes their lines, then invalidates
  the IOTLB **page-selectively** (CAP.PSI: one descriptor per naturally
  aligned run, up to 2^MAMV pages) for a pin of a few runs, or
  **domain-selectively** for more (one descriptor, the whole domain's
  cached entries), and waits. Only after the wait are the pages given back
  to their VMO and empty table pages freed: the same rule as the CPU's
  TLB gather.
- **Batching:** today's drivers pin when they start (rings, buffers, the
  bulk buffer per USB interface) and unpin when they stop, never per
  transfer, so a wait per unpin costs nothing measurable. A cap's close
  removes the whole domain at once: the context entry first (one
  context-cache invalidation), then one domain-selective IOTLB
  invalidation, one wait, then every page freed.

### Pins, unpins and the cap's close (question 4)

- `vmo_pin` with a bound cap: as today (the cap must be current with bus
  mastering on), then map the pages in its domain, then publish the pin.
  A failure to map undoes the pin.
- `vmo_unpin`: unmap, invalidate, wait, then release the pin.
- The cap's close: the function's context entry goes to the **blocking
  domain** (a domain with an empty table, shared by every function with
  no current cap) with its invalidation; Bus Master Enable goes off as
  today; then the cap's pins are unmapped together and **freed at once**,
  not quarantined. While the IOMMU translates, the quarantine is never
  used; with `iommu=off` or no VT-d it works exactly as today, and its
  tests run in that mode.
- The kernel test `dma_stale_write_after_rebind` (edu's delayed write
  after its driver died) then expects the stale write to fault, not to
  land in quarantined pages: one expectation per mode.

### RMRRs (question 3)

Each RMRR is identity-mapped, read and write, in the domain of each device
its scopes name, for the whole boot: in the blocking domain's place
before the device has a driver (a device listed in an RMRR gets its own
"boot domain" with just the region), and in every domain a cap makes for
it. An RMRR in RAM Jam OS manages (stage 0 says) is taken out of the
memory map at boot, before the PMM starts, as the kexec region is. An
RMRR naming a device that isn't there (the iGPU) is ignored.

### Devices with no driver (question 2)

Every function the unit covers (INCLUDE_PCI_ALL: all of segment 0 that
no other unit lists) gets a context entry from the start: the blocking
domain, or its RMRR boot domain. Its faults are recorded; after the
first 8 from one device (as built: 8 since its last attach, so a new
driver's domain starts the count again), that device's context entry gets the
fault-processing-disable bit, so a device stuck retrying can't flood the
fault registers (the log says it was muted, and the count still shows in
the `iommu` command from the context's last state).

### The boot handover

What the firmware leaves (stage 0 tells for this PC):

- **Translation off** (the expected case): build the root table with
  every context entry (blocking or boot domains), set the root table
  pointer, invalidate the context cache and IOTLB globally, turn
  translation on, then turn the protected memory regions off if the
  firmware left them on (they are for before translation).
  Devices that were bus mastering for the firmware (stage 0's "bus
  mastering on" line; the xHCI is the one that matters) are blocked from
  that moment, except into their RMRR.
- **Translation on** (the firmware's pre-boot DMA protection, or a kexec
  that couldn't turn it off): never turn it off, since that opens all of
  RAM for a moment. Build the new tables, point the unit at them while
  translation stays on (the specification allows it), invalidate globally. The
  firmware's tables live in memory the boot map calls reserved, so they
  stay valid until the switch.
- **Interrupt remapping on, or queued invalidation on:** the same idea;
  the queue is taken over by resetting its tail and head through the
  documented disable/enable sequence (queued invalidation off is safe:
  it is only the unit's command channel).
- When: right after PCI enumeration (the scopes need the bus numbers),
  before resources and user space, so no driver ever runs without it.
  The probe stays as the first step (its log lines are the boot's record
  of what the firmware left).

### kexec and panic (question 7)

`kexec`'s jump and the panic path already turn bus mastering off on every
function (`pci_panic_bus_master_off`). Then, per unit: interrupt
remapping off, translation off, queued invalidation off, each with a
bounded wait. The I/O APIC's entries are masked first (the new kernel
reprograms them). On the panic path every step is best-effort with a
short deadline: a unit that doesn't answer is skipped, and the new kernel
takes over whatever is left on.

### Interrupt remapping

- **One table, shared by every unit** (as built, stage 4: 1024 entries,
  16 KiB, since every live interrupt object needs one and Jam OS has
  dozens). VT-d allows it (5.1.3: units "may be configured to share
  interrupt-remapping table"; 6.10: a change is invalidated on each unit),
  and it means an entry's index is the same whichever unit a device sits
  behind, so no device-to-unit lookup is needed; remapping goes on only
  when every unit the probe read started and offers it (ECAP.IR), on all
  of them. Entries are allocated with the interrupt object and freed with
  it, after an interrupt-entry-cache invalidation on every unit.
- **One entry per MSI or MSI-X vector**: present, fixed delivery, edge,
  physical destination = the CPU's APIC id (x2APIC ids, 32 bits, when the
  unit has ECAP.EIM; the PC runs in x2APIC mode), the vector, and **source
  validation**: the entry may only be used by the function's requester id
  (SVT = 1, SQ = 0). The device's MSI address and data are then the
  remappable format: 0xfee00000 with the entry's index and the format bit,
  data 0 (VT-d chapter 5). The kernel's PCI core still programs them; drivers
  never see either.
- **The I/O APIC**: in x2APIC mode compatibility-format interrupts must
  stay blocked (CFI off), so the I/O APIC's live entries (COM1's today)
  are rewritten in the remappable format too, each with an entry whose
  source id is the I/O APIC's from the DMAR scope (stage 0 logs it next
  to the MADT's I/O APIC). An I/O APIC with no scope is a firmware bug: no
  interrupt remapping then (logged, reported), DMA remapping still on.
- **Turning it on**: table zeroed, entries written for the live I/O APIC
  entries (masked meanwhile), the table pointer set (extended mode when
  x2APIC), a global interrupt-entry-cache invalidation, then interrupt
  remapping on, then the I/O APIC entries rewritten and unmasked. A COM1
  edge lost in between is rescued by CPU 0's tick (serial.c already
  does). It runs before any MSI is programmed (before user space), so no
  device's message has to be rewritten live.
- **Moving an interrupt** to another CPU, if the scheduler ever does,
  changes the entry and invalidates it: the device isn't touched.
- **The interrupt window**: with remapping on, a device's write there in
  the compatibility format is blocked (fault reason 0x25 in VT-d's table),
  and a remappable one naming an entry that isn't its own fails source
  validation (0x26) or finds no entry (0x22): no interrupt, a fault
  recorded. That is the done-when's second half. **QEMU 10.0 can show only
  part of it** (found building stage 4): it passes compatibility-format
  writes through even with remapping on (it never looks at CFIS or EIME),
  and a DMA engine's write carries no requester id there, so its faults
  name ff:1f.7 and source validation can't apply to it. The QEMU tests
  therefore write the window in remappable format (an entry never present,
  one past the table: blocked, recorded) and check source validation with
  edu's real MSI (another function's entry: 0x26; a freed one: 0x22); the
  literal 0xfee00000 write blocked with fault 0x25 is the PC's check
  (stage 5).
- **Kexec and panic**: `irq_remap_off` (jam/irq_remap.h) masks the I/O
  APIC's routed pins, then turns remapping off on every unit, taking no
  lock (bounded waits, short on a panic). D1's IOMMU off path calls it.
  A unit found with remapping on at boot (a kexec that didn't turn it off)
  gets its table pointer replaced while on (6.7 allows it: nothing is in
  flight then).

### Faults

- The unit's fault event interrupt (FECTL, FEDATA, FEADDR and, in x2APIC
  mode, FEUADDR) goes to one CPU through a vector of its own (as built:
  the CPU `vector_alloc` picks, one whose APIC id fits 8 bits); fault events
  are not themselves remapped. The handler reads the fault status, walks
  the recording registers from the first full one (bounded by their
  count), copies each fault (requester id, address, read or write, reason)
  into a small ring, clears it (write 1), and clears the overflow and
  pending bits. A kernel thread logs from the ring: `vtd: fault: 00:1f.3
  write at 0x12345000: no mapping (reason 0x05)`, with the reason in
  words for the ones Jam OS can cause, the hex otherwise.
- Counted per device; the first fault per device is a console notice and
  a RESULTS line (question 5); a lost fault (the overflow bit) is
  counted too.
- A debug command `iommu` (through the existing debug command path, no
  new system call) prints the units, the domains (owner, pages mapped,
  table pages), the interrupt entries and the fault counts.

### The interface the rest of the kernel sees

One small header (planned: kernel/include/jam/iommu.h), so dma_cap.c,
vmo.c, interrupt.c and ioapic.c don't know VT-d's formats: `iommu_on()`,
`iommu_domain_new(pci_dev)`, `iommu_domain_attach/detach`,
`iommu_map(domain, addr, pages, n)` and `iommu_unmap(...)` (both with a
batch that ends in one invalidation and wait), `iommu_irq_alloc(pci_dev
or I/O APIC, cpu, vector, out msi_address, out msi_data)` and
`iommu_irq_free` (as built: the interrupt calls are a header of their
own, `kernel/include/jam/irq_remap.h`: `irq_remap_alloc_pci`,
`irq_remap_alloc_ioapic`, `irq_remap_free`). With the IOMMU off every call is a no-op returning
the old behaviour (physical addresses, compatibility-format messages).
The VT-d code itself lives in kernel/dev/ next to the probe (planned
vtd_unit.c, vtd_qi.c, vtd_fault.c, vtd_pt.c, vtd_domain.c, vtd_boot.c,
vtd_ir.c), each under 600 lines.

## Testing: QEMU's intel-iommu, and what only the PC shows

QEMU (10.0 on the Mac, TCG) emulates a VT-d unit:
`-device intel-iommu,intremap=on,caching-mode=on` (and `eim=on` for
x2APIC destination ids). It works anywhere in the device list under TCG
(no `kernel-irqchip=split`: that is only for KVM). Its DMAR lists one
unit at 0xfed90000 with an explicit endpoint scope per PCI function (no
INCLUDE_PCI_ALL) and the I/O APIC's scope; no RMRR. Every emulated device
(xHCI, e1000e, HD Audio, edu) does its DMA through it, so every QEMU test
that uses a device tests the remapping.

- A new `QEMU_IOMMU` variable in tools/qemu-test.sh (stage 1A) adds the
  device: `1` (caching mode on, the strict case for maps), `cm0`
  (caching mode off, the PC's case), `eim` (x2APIC ids).
- **Blocked DMA**, in QEMU: edu's DMA engine copies to any bus address
  its driver names, so a test points it at a page that isn't pinned: the
  page must stay unchanged and a fault must name edu's requester id and
  that address. Then the same at a pinned page: it must land.
- **The interrupt window**, in QEMU: edu's DMA writes to 0xfee00000:
  no interrupt arrives on any CPU, and a fault is recorded.
- Every area test that uses a device runs again with `QEMU_IOMMU=1` in
  the join (usb, storage, hda, mixer, net, the init run).
- What QEMU can't show, so the PC must: missing cache flushes (QEMU's
  unit reads guest memory directly, so coherence bugs pass there), real
  invalidation timing, the real firmware's tables (two units,
  INCLUDE_PCI_ALL, RMRRs, the I/O APIC's real source id), what the
  firmware left on, devices' real DMA habits (xHCI scratchpad and
  prefetching, HD Audio's position buffer, the RTL8125's descriptor
  prefetch), x2APIC ids above 15, and whether any undriven device keeps
  mastering.

## The cost, and M11.5's numbers

- **The IPC path doesn't change**: nothing in a channel call touches the
  IOMMU, so M11.5's call and switch numbers are unaffected.
- **Pin and unpin get dearer**: page-table writes and line flushes per
  page; an unpin also waits for an invalidation (microseconds). Drivers
  pin only when they start and stop, so no per-transfer cost. New BENCH
  lines: pin and unpin of 1 and of 64 pages, with the IOMMU and with
  `iommu=off`.
- **Device side**: each device access to a page not in the unit's IOTLB
  costs a page walk in the unit (hundreds of nanoseconds), against
  transfers that take micro- to milliseconds (USB 2, gigabit, audio
  periods): expected within noise. `speed` and a file read from the stick
  are measured with and without.
- **Interrupts**: one cached table lookup in the root complex; the
  existing interrupt latency lines are measured with and without.
- As the wave plan says, M11.5's final numbers are measured after M11 is
  merged, so its BENCH column is the system as it stays.

## Stage 0: the read-only probe

Built (branch of this plan). **It writes no VT-d register**: it maps each
unit's register page (uncached) and reads. So it can be flashed at once:
the PC runs exactly as before, with more log lines.

What it logs, every line starting `vtd:`:
- the DMAR header: revision, host address width, the flags in words
  (interrupt remapping, x2APIC opt-out, DMA control opt-in), how many of
  each structure;
- each unit: register base, size, segment, INCLUDE_PCI_ALL; then one line
  per device scope: an endpoint or bridge followed through the bridges to
  its function, named by its ids and class, or "not present" (the iGPU);
  an I/O APIC or HPET by its requester id, the I/O APIC matched with the
  MADT's;
- each RMRR (range, size, whether it lies in RAM Jam OS manages, its
  devices), ATSR, SATC and ANDD;
- per unit: version, CAP and ECAP in hex and decoded (domains, page-table
  levels, MGAW, caching mode, write-buffer flush, superpages,
  page-selective invalidation, fault registers, drain, protected memory
  regions, posted interrupts; coherent walks, queued invalidation,
  interrupt remapping, x2APIC ids (EIM), pass-through, snoop control,
  device TLBs, scalable mode); the global status in words (translation,
  interrupt remapping, queued invalidation, root and interrupt table
  pointers, compatibility interrupts, protected memory, faults); the raw
  pointers (root table, interrupt table, queue, fault control); and any
  fault the unit has recorded;
- the PCI functions with bus mastering on at that moment (nothing in Jam
  OS has turned it on yet: the firmware's choice, or after a `reboot` the
  previous kernel's);
- the highest APIC id (above 255 would need remapping for MSIs);
- the summary (the "handover" line): units that answered, and how many
  have translation, interrupt remapping, queued invalidation, protected
  memory or recorded faults on. Anything left on, a unit that doesn't
  answer, or a table that doesn't parse also goes to the RESULTS box.

Files: kernel/acpi/dmar.c (the parser), `kernel/include/jam/dmar.h`,
kernel/dev/vtd_probe.c (the registers and the log), `kernel/include/jam/vtd.h`
(the register map, kept for the later stages), one call in
`kernel/main.c` after `pci_init`. Tests:
- ktests `dmar_*` (kernel/test/test_dmar.c): a table shaped as the PC's is
  expected to be, parsed field by field; bad headers refused; a structure
  with an impossible length stops the walk with what came before kept;
  scopes that don't fit, an odd path, a path too long to keep; more units
  and scopes than the arrays hold; 300 tables of random bytes. `vtd_*`:
  the decode of an Alder Lake unit's CAP/ECAP and QEMU's, exact strings;
  the status line with everything on.
- `tools/vtd-test.sh`: QEMU with intel-iommu (the lines, nothing reported,
  and QEMU's own register trace: 12 reads, 0 writes), with `eim=on`, and
  without an IOMMU (one "no DMAR table" line); `VTD_TEST_INIT=1` adds the
  init run with the IOMMU present and off.

**For the owner, on the PC:** flash this build (or `update -w`), then
**power the PC off and on** (a cold boot: what the firmware leaves is the
question; a `reboot` is a kexec and shows what Jam OS left instead), and
boot the everyday entry. Then bring back, from that boot's log (the stick's
`logs/boot-NNNN.txt`, or `~/jamos-logs` on the Mac through netlog):

```sh
grep -E 'vtd:|acpi:' boot-NNNN.txt
```

(`acpi:` adds the line that lists every table, so a missing DMAR shows.)
A `reboot` after it and the same grep of the next log is a bonus: it
should say the same, which shows the probe changed nothing and what a
kexec'd kernel finds.

What the PC run answers: whether there is a DMAR at all; one unit or two,
and whether the iGPU's answers; the main unit's CAP/ECAP (levels, MGAW,
coherence, caching mode, queued invalidation, interrupt remapping, EIM,
domain count, PSI, fault register count); the I/O APIC's requester id;
the RMRRs (questions 3 and 2); what the firmware left on (the handover
path); which functions master at boot (question 2); the x2APIC opt-out
flag (question 7 if set).

## Stages and tracks

Each stage or track is about one agent-hour, starts from main, owns the
files listed (an edit outside them stays minimal and is named in the
report), hands back when its tests pass, never pushes, never touches a
USB disk and never starts agents of its own. Quick tests only (`make`,
`make check`, the init run at 2 CPUs, its own tests); the full suites run
once on the merged milestone. "New" files are planned names.

| Stage | Track | What | Files it owns | Needs |
|---|---|---|---|---|
| **0. The probe** (built) | | above | kernel/acpi/dmar.c, kernel/dev/vtd_probe.c, `kernel/include/jam/dmar.h`, `kernel/include/jam/vtd.h`, kernel/test/test_dmar.c, `tools/vtd-test.sh` | nothing |
| **1A. The unit** (built) | A | per unit: the register mapping kept from the probe, the invalidation queue (enable, submit, the wait slots, errors), the invalidations (context global/domain/device, IOTLB global/domain/page-selective, interrupt entry cache global/index), the fault interrupt, ring and log thread, the boot words `iommu=on/off`, the VT-d register pages added to the MMIO the kernel keeps (resource.c), `QEMU_IOMMU` in qemu-test.sh. Tests in QEMU: every invalidation completes, an error is reported, the queue wraps | new kernel/dev/vtd_unit.c, vtd_qi.c, vtd_fault.c, vtd_internal.h, new kernel/test/test_vtd_unit.c; `kernel/include/jam/vtd.h`; `kernel/object/resource.c` (one check); `tools/qemu-test.sh`; `docs/TESTING.md` (the variable) | 0's PC run |
| **1B. Page tables** (built) | B | the second-level tables as a module that takes its page allocator and its flush as functions, so it is tested without hardware: map and unmap runs of pages, pin counts in the software bits, empty tables freed only after the caller's invalidation, the per-domain cap, the charge; the address and width checks (MGAW) | new kernel/dev/vtd_pt.c, vtd_pt.h, new kernel/test/test_vtd_pt.c | nothing (beside 1A) |
| **1C. Interrupt entries** (built) | C | the remapping table (allocate, zero, free entries), the entry format (x2APIC and xAPIC destinations, source validation), the remappable MSI address/data and I/O APIC entry encoders, all pure and tested on their own | new kernel/dev/vtd_ir.c, vtd_ir.h, new kernel/test/test_vtd_ir.c | nothing (beside 1A) |
| **2. Domains and translation on** (built) | D1 | root and context tables, domain ids, the blocking domain, RMRR boot domains (and RMRRs in RAM out of the memory map), attach and detach with their invalidations, the fault-processing-disable mute, the boot handover (off, on, queue on), protected memory off, translation on at boot; off before a kexec and on the panic path; the planned jam/iommu.h interface | new kernel/dev/vtd_domain.c, vtd_boot.c, new kernel/include/jam/iommu.h, new kernel/test/test_vtd_domain.c; `kernel/main.c` (the call); `kernel/kexec/jump.c` and `kernel/dev/reboot.c` (one call each) | 1A, 1B |
| **3. Pins through the IOMMU** (built) | D2 | `dma_cap` makes and switches domains; `vmo_pin` maps, `vmo_unpin` unmaps and waits; the close blocks then frees at once (the quarantine only with `iommu=off`); edu tests: an unpinned page is never written and the fault names edu, a pinned one is; `dma_stale_write_after_rebind` per mode | `kernel/object/dma_cap.c`, `kernel/object/vmo.c` (the pin paths), `kernel/test/test_dma.c`, `drivers/test/edu/edu.c` if a test needs a command (as built: the IOMMU's own tests in new kernel/test/test_dma_iommu.c, the edu and fault-watch helpers shared in kernel/test/ktest_util.c, `dma_cap_create_for` takes the job its domain is charged to; a closed cap's domain is taken away by the "dma quarantine" thread, since the close can't wait; the pass-through domain stayed for the tests only, removed after the review) | D1 |
| **4. Interrupt remapping on** (built) | E | interrupt objects allocate entries (`irq.c`'s message address and data from them), the I/O APIC's entries remapped, the enable sequence, EIM with x2APIC; tests: every device's interrupts in QEMU with `QEMU_IOMMU=eim`, edu's write to 0xfee00000 raises nothing and is recorded | `kernel/object/interrupt.c`, `kernel/arch/x86_64/irq.c`, `kernel/arch/x86_64/ioapic.c`, vtd_ir.c (wiring), new kernel/test/test_vtd_irq.c (as built: the wiring is kernel/dev/vtd_irq.c, its interface `kernel/include/jam/irq_remap.h`) | 1A, 1C (beside D1/D2) |
| **5. The PC checks** (built) | F | drv/hda's test word `vtdtest`: before its normal start, the controller's command ring read from an address that isn't pinned (a read fault naming 00:1f.3) and its response ring written to 0xfee00000 (blocked, no interrupt), then a reset and the normal start; the boot entry "Jam OS (IOMMU)" (question 6) and its test entry; the `iommu` debug command; the bench lines | `drivers/hda/` (a test file), devmgr's word passing, `boot/limine.conf`, `kernel/debug/dbgcmd.c`, `kernel/test/bench.c`, `docs/BENCH.md` | 3, 4 |
| **6. The join** (built) | J | every device area test with `QEMU_IOMMU=1` and `cm0`; the docs: ARCHITECTURE (the IOMMU row, the threat model: drivers contained, Memory's DMA line, the drivers' rules), SECURITY.md, README, HARDWARE (the PC's DMAR from stage 0), TESTING, ROADMAP; default on after the PC (question 6) | `tools/vtd-test.sh`, the docs | 5 |
| **7. Review and fix** | | the independent review-and-fix agent over all of M11 (the standing rule): findings first, then High and Medium fixed one commit each with a test | what its findings touch | 6 |

**Order** (wave 1 runs M11.5 and M11.6 beside it, so at most three of
M11's agents testing at once):
1. Now: stage 0 merged and flashed; the owner's PC run.
2. 1A, 1B and 1C together.
3. D1 and E together (they meet only in vtd_unit's enable order and
   `kernel/main.c`'s one call), then D2.
4. F, then the PC session: the IOMMU entry, the test entry, every device,
   All tests and `soak 10`; then the join and the default; then the
   review.

## As built

Stages 0 to 6 are merged (2026-10-05). The design as it stands is
[ARCHITECTURE.md](../ARCHITECTURE.md#the-iommu); the tests are in
[TESTING.md](TESTING.md#the-iommu). Everything is behind `iommu=on`, off
by default until the PC passes (question 6).

- **0, the probe**: as planned; its PC run (2026-10-04, build b7713bd,
  [HARDWARE.md](HARDWARE.md#the-iommu-vt-d)) found one unit with
  INCLUDE_PCI_ALL, no RMRR, CAP/ECAP exactly as guessed above, everything
  off at a cold boot, the highest APIC id 86. Questions 2 and 3 stayed as
  answered.
- **Spec check** (between 1B/1C and 1A, [M11-SPEC-CHECK.md](history/M11-SPEC-CHECK.md)):
  every layout the IOMMU writes checked against VT-d 4.1 bit by bit, and
  5.20; fixed: stale advanced-fault-logging definitions, a section
  citation, the probe's decode of an interrupt fault record.
- **1A, the unit**: the invalidation queue (one page, a status slot per
  waiter, waits with no lock held, 100 ms), every invalidation kind, the
  write-buffer flush where CAP.RWBF, the fault interrupt, ring and log
  thread, `iommu=on` / `iommu=off` (kept by a reboot), `QEMU_IOMMU` (`1`,
  `cm0`, `eim`).
- **1B, page tables** and **1C, interrupt entries**: pure modules
  (`kernel/dev/vtd_pt.c`, `kernel/dev/vtd_ir.c`) as planned, tested
  without hardware.
- **2 (D1), domains and translation on**: root and context tables, domain
  ids, the blocking domain, RMRR boot domains and RMRRs in RAM reserved
  early, context entries by one 16-byte atomic write, the mute, the
  handover (off: build and turn on; on: take over while translating), the
  jump off. Deviations: (a) until D2, a driven function went to the
  unit's **pass-through domain**; D2 replaced that with per-cap domains,
  and the pass-through domain stayed for the tests only, until the
  review's design question A removed it
  ([M11-REVIEW](history/M11-REVIEW.md#design-questions-for-the-owner));
  (b) **the takeover
  while translating** honours only the RMRRs for requests in flight: VT-d
  6.6 asks the new tables to give the old ones' results for those, and any
  other in-flight DMA (the firmware's) is blocked and logged instead, the
  state the boot is going to anyway; (c) interrupt remapping is turned on
  before translation (E's order), so an `iommu=on` boot logs
  "interrupt remapping on" before "translation on".
- **3 (D2), pins**: as planned, with the notes in the table above (the
  close hands its domain to the "dma quarantine" thread, which may wait;
  `SYSINFO_IOMMU`). Deviations: (a) **the table pages are charged to
  devmgr's job**, the job that makes a driver's cap (`dma_cap_create`),
  not the driver's; still bounded (512 pages per domain), and M12's
  review may move the charge to the driver; (b) a function's DMA fault
  count, which decides the mute, was **not reset per attach**: a new
  driver after one that had faulted 8 times was muted at its first fault.
  Fixed in the join (the count starts again at each attach;
  `vtd_domain_new_driver_fresh_count`).
- **4 (E), interrupt remapping**: **one table shared by every unit**
  (1024 entries) instead of one per unit (see
  [Interrupt remapping](#interrupt-remapping)); 8-bit destinations when a
  unit lacks EIM or the CPUs aren't in x2APIC mode; QEMU shows only part
  of the window check (it passes old-format writes through), so the
  0xfee00000 write blocked with 25h is the PC's.
- **5 (F), the PC checks**: drv/hda's `vtdtest` (the command ring read
  from a page it unpinned, then its response ring pointed at the
  interrupt window), the boot entries "Jam OS (IOMMU)" and "Tests > IOMMU
  checks", the `iommu` command (shell and debug), `bench`'s IOMMU lines,
  HARDWARE's section.
- **6, the join**: every device area test with the IOMMU on (the matrix
  in [TESTING.md](TESTING.md#the-iommu)): on 2026-10-05 with
  `QEMU_IOMMU=1` usb, storage, net, hda, play, mouse, mixer, data, sticks,
  fetch, serve, tcp, music, kexec-reboot, reboot-firmware and the `init`
  run all passed, with `cm0` usb, storage, net and hda, and
  `tools/vtd-test.sh` with its `init` runs; not one `vtd: fault:` line in
  any of their logs but those the tests provoke (each sound controller's
  one read in hda-test's IOMMU checks; edu's in vtd-test). No device
  needed a change and no IOMMU bug turned up (two first runs ended "with
  problems" only for a loaded Mac's timer check, and passed again). The docs as
  built (ARCHITECTURE's section and threat model, SECURITY.md, README,
  HARDWARE, TESTING, ROADMAP); the two small leftovers: the fault count
  per attach (above) and the test scripts that stopped at their first
  failure report under `set -e`.

- **7, the review** ([M11-REVIEW.md](history/M11-REVIEW.md)): no High;
  three Mediums fixed: the root table's cleared page is flushed for a unit
  that doesn't snoop; a storm of fault events (an interrupt's faults can't
  be muted) masks the fault interrupt and the log thread polls instead; a
  closed cap's domain the unit doesn't confirm gone keeps its pages (tried
  again every second) instead of releasing them after the quarantine's
  time. Two Lows fixed (vectors 16-31 refused in an entry; a redundant test
  reset dropped); the "512 table pages = 1 GiB of scattered pins" claim
  corrected (fully scattered pins hit it at ~500 pages); design questions
  A-E for the owner there.

**Left for the PC** ([What only the PC can show](#what-only-the-pc-can-show)),
then the default.

## Where M11 meets the rest of wave 1

- **M11.5** (the IPC fast path) changes channels and the scheduler's
  switch: no shared file. Its final benchmark runs after M11 merges.
- **M11.6** (services that outlive their process) keeps state in VMOs: if
  it pins or changes `vmo.c`'s pin paths, D2 and M11.6 merge carefully;
  M11 adds no VMO kind and changes no VMO semantics outside pinning.
- **The syscall table**: M11 adds no system call (the `iommu` command
  uses the debug command path); question 8 would, in M12.

## What only the PC can show

- Stage 0's lines (above): the real DMAR, units and firmware state.
- The everyday boot with translation on: USB (keyboard, mouse, the
  stick through the hub), sound (`beep`, `play`), the network (`ping`,
  `update`, netlog) all working, and no fault in the log.
- The deliberate faults (stage 5): a read fault naming 00:1f.3 at the
  chosen address, the interrupt window write blocked with no interrupt.
- Cache coherence of the tables (no fault and no hang over `soak 10`).
- A `reboot` (kexec) and a panic with the IOMMU on: the next kernel
  comes up and takes over cleanly; `reboot -f` too.
- The bench lines with and without `iommu=off`.

## References and licences

- **Intel Virtualization Technology for Directed I/O, Architecture
  Specification, revision 4.1** (2023): the primary reference for every
  format and sequence. Jam OS's code is written from it.
- **FreeBSD's DMAR driver** (sys/x86/iommu/, BSD-2-Clause) and
  **OpenBSD's acpidmar** (ISC): permissive, for the order of operations
  and quirks; cited where used, never pasted.
- **Fuchsia's intel-iommu** (BSD-3-Clause): how a capability system ties
  a domain to a bus transaction initiator (its BTI is Jam OS's
  `dma_cap`), for design only.
- **Linux's intel-iommu and dmar** (GPL): hardware facts only (quirks,
  values other boards report), never copied or paraphrased
  ([CODING-GUIDE](../CODING-GUIDE.md#licence-and-outside-code)).
- **QEMU's intel_iommu.c** (GPL): what the emulator does, for the tests;
  never code.
