# M6 plan: PCIe, MSI/MSI-X, devmgr, drivers through handles

Goal (ARCHITECTURE.md milestone row): **a sample driver bound through the
handle-only API runs in the kernel, then as a process.** Concretely:

1. QEMU: the `edu` test device's driver computes a factorial over MMIO,
   takes its MSI as a port packet, and DMAs a buffer through a pinned VMO,
   first as a kernel thread, then as a process that devmgr started.
2. The real PC: the xHCI controller (Intel 8086:7A60) completes a **No Op
   command** and reports it with a real MSI/MSI-X interrupt, in the kernel
   and then as a process. (Intel PCH xHCI controllers often expose MSI
   only, no MSI-X: the first PC run prints the capabilities and the driver
   uses whichever the device has.)
3. Killing a driver process mid-DMA cleans up: Bus Master Enable cleared,
   pins released, vectors freed, its job ends with nothing charged.

uACPI is NOT in M6 (M10). No INTx for drivers (MSI/MSI-X only).

## What M6 adds

| Piece | What it is |
|---|---|
| PCI core (kernel) | ECAM from MCFG, bus walk (firmware's bus numbers, no reassignment), device table, capability lists (standard + extended), BAR decode/sizing, MSI and MSI-X programming, Bus Master Enable control. The kernel keeps the parts a buggy or hostile driver must not do itself |
| Vectors + interrupt objects | per-CPU vector allocator (0x31-0xef), `interrupt` object: an MSI/MSI-X vector that queues a port packet (or raises a signal) and stays quiet until acked |
| Resources | `resource` object: the root of hardware authority (MMIO ranges, PCI devices), sliced into smaller ones; MMIO VMOs for processes need one |
| DMA | `dma_cap` bound to one PCI function: `vmo_pin` returns device addresses, closing it clears Bus Master Enable then releases the pins; contiguous VMOs below 4 GiB for 32-bit DMA |
| `<jam/driver.h>` | the only header a driver includes; two implementations (kernel thread with its own handle table / process over syscalls); a build check enforces it |
| IDL | `tools/genidl.py`: protocol file -> C structs, client stubs, server dispatch; first protocols `null` (test echo) and `edu` |
| devmgr | a process started by init: enumerates via the PCI root resource, matches drivers, starts each driver process with exactly its handles |
| Drivers | `edu` (QEMU), `xhci-noop` (the PC's done test; also works on QEMU's qemu-xhci) |

## Fixed decisions

- **Who does what.** The kernel's PCI core owns ECAM, BAR sizing, MSI/MSI-X
  table/capability writes and Bus Master Enable. devmgr (a process) owns
  *policy*: which driver binds to which device, starting it, handing it
  handles. A driver reads its own function's config space and writes only
  what the kernel's filter allows (`pci_config_read/write` on its
  `RES_PCI_DEV`; BARs, decode/bus-master bits and the MSI/MSI-X
  capabilities are read-only to it); Bus Master Enable needs RIGHT_MANAGE
  on the device, which only devmgr holds. A driver never maps an MSI-X
  table or PBA page and never does port I/O.
- **Boot display is untouchable.** The device whose BAR holds the boot
  framebuffer, and every bridge, are never BAR-sized and their command
  register is never written (sizing disables decode; the console would go
  dark or hang). Their BARs are read as-is.
- **MSI address format**: xAPIC-compatible (destination APIC ID in bits
  19:12, 8 bits), fixed delivery, edge. The PC's APIC IDs are < 256; a CPU
  with an ID >= 256 is never chosen as an MSI target (no interrupt
  remapping until M11). Vector chosen per (cpu, vector) pair.
- **Target CPU**: the allocator spreads vectors over CPUs, preferring
  E-cores then the least-loaded CPU (fewest vectors). CPU0 only if nothing
  else is online. Affinity change is not in M6.
- **Interrupt object semantics**: firing raises `SIG_INTERRUPT`; a driver
  binds the object to a port `PORT_BIND_PERSISTENT` for that signal, so
  the existing coalescing gives ONE packet whose `count` is the fires
  since it was queued (no new packet type). `interrupt_ack` clears the
  signal and unmasks. MSI-X vectors (and maskable MSI) are masked from
  fire to ack; plain MSI (edge) is left unmasked and coalesced.
  Destroying the object: disable the vector at the device, wait until no
  CPU is inside its handler, free the vector.
- **IRQ context**: the fire path must not allocate or sleep. Raising a
  signal from an interrupt is already done by timers; Track B checks the
  whole signal -> observer -> port packet path is IRQ-safe and
  allocation-free for a persistent binding, and fixes it if not.
- **Resources**: kinds `RES_ROOT`, `RES_MMIO` (phys base + size),
  `RES_PCI` (all of PCI: enumerate + make per-device handles),
  `RES_PCI_DEV` (one function, segment:bus:dev.fn). A slice must lie inside
  its parent. Userboot hands init the root; init hands devmgr a `RES_PCI`
  sliced from it; devmgr gets per-device handles with `pci_device_open`
  and each BAR as an MMIO resource with `pci_bar_resource`.
- **MMIO for processes**: `vmo_create_physical(res, offset, size, cache)`
  as a syscall needs an MMIO resource (or a BAR resource from
  `pci_bar_resource`) covering the range. The kernel refuses (ERR_ACCESS_DENIED)
  a physical VMO overlapping a page that holds an MSI-X table or PBA of any
  device. Cache: `VM_UC` for registers, `VM_WC` allowed for framebuffers.
- **DMA**: `dma_cap_create` needs a `RES_PCI_DEV` handle; the cap records
  the BDF; `vmo_pin` needs a `dma_cap` whose device has BME on (devmgr
  enables it with `pci_bus_master`); device address = physical
  address today. Closing the last handle of the cap: BME off first, then a
  config read-back (flushes posted writes), then unpin. A process kill
  runs the same path. `VMO_CONTIGUOUS | VMO_DMA32` gives a run below 4 GiB.
- **Driver entry**: `int driver_main(const struct driver_start *s)` where
  `driver_start` lists the handles by role (`DR_PCIDEV`, `DR_SERVE`,
  `DR_DMA`, `DR_BAR(n)`, `DR_IRQ(n)`). In the kernel build a
  kernel thread runs it with its own handle table (a *kernel process*: a
  `struct process` with the kernel address space, so handle rights and
  jobs work unchanged); in the process build libos's `_start` decodes the
  startup message into the same struct.
- **driver.h surface** (both builds; `drivers/include/jam/driver.h` in the
  foundation is authoritative): `drv_` handles, channels, ports, waiting,
  VMO create/read/write/map/pin, `drv_mmio_map`, `drv_interrupt_ack`,
  `drv_pci_config_read/write`, `drv_log`/`drv_report`, clock, sleep,
  threads, `drv_malloc/drv_free` (libos malloc in the process build, a
  VMO-backed per-driver heap in the kernel build, never kmalloc), MMIO
  accessors. Nothing else.
- **Build check**: each driver is compiled with `-nostdinc -I
  kernel/include/jam/driver-only/` (only `driver.h` and freestanding
  headers visible), and `tools/checkdriver.py` rejects an object whose
  undefined symbols are not in `driver.h`'s list. Fails the build.
- **Lock order** (new): `pci` config lock (spinlock, IRQ-safe) is a leaf;
  vector allocator lock is a leaf; `interrupt` object lock -> port packet
  lock; `dma_cap` close: pin list lock -> vmo (existing order).
- **Job charges**: interrupt objects, resources and dma_caps are small
  objects and cost one handle unit each (like VMO structs); pinned pages
  stay charged to the VMO's job as before.
- **QEMU**: `tools/qemu-test.sh` and `make run` add
  `-device edu,dma_mask=0xffffffff` (default mask is 28 bits). qemu-xhci is
  already there (the boot stick is on it) and supports MSI-X.

## Tracks

Phase 1 runs four agents in parallel on top of a foundation commit (headers
+ weak stubs + reserved syscall numbers), like M5. Phase 2 joins them.

### Foundation (on main, before the agents; done)
- Headers: `jam/pci.h` (kernel PCI core), `jam/interrupt.h` (vectors +
  interrupt objects), `jam/resource.h` (+ bound dma_cap),
  `drivers/include/jam/driver.h` (+ `driver_start` roles).
- `OBJ_INTERRUPT`, `OBJ_RESOURCE` (already reserved), `SIG_INTERRUPT`,
  `RIGHT_SLICE`, `RES_*`, `VMO_CACHE_*`, `struct pci_dev_info`, `IRQ_MSIX`,
  startup role `SR_RESOURCE` (all in `<jam/abi.h>` / `<jam/startup.h>`).
- `abi/syscalls.def` 90-102: every M6 syscall (the generator's weak
  stubs return ERR_NOT_SUPPORTED until a track implements them).
  Track B implements `interrupt_create_msi` and `interrupt_ack`; Track C
  all the others.
- `kernel/core/m6_weak.c`: weak stubs for every interface function.
- `main.c` calls `pci_init(); resource_init();` after every CPU is online
  (before ktests/bench/stress/userboot), and `pci_report()` on `pcilist`.
- QEMU flags (edu) in tools/qemu-test.sh and `make run`.

### Track A: PCI core (agent 1)
- ECAM mapping per MCFG segment (UC, kernel vmap), config read/write
  8/16/32 with a lock; bus walk from each segment's start bus following
  bridges' secondary/subordinate numbers; device table (`struct pci_dev`:
  BDF, ids, class, header type, BARs, caps, owner flags).
- Capabilities: standard list + extended (0x100+), find by id; MSI (32/64,
  per-vector mask?), MSI-X (table/PBA BAR + offset, size), PCIe.
- BARs: decode 32/64/prefetchable/IO; sizing with decode disabled (never on
  the boot display device or bridges); records MSI-X table/PBA pages as
  protected (`pci_phys_protected(phys)` for Track C).
- MSI/MSI-X primitives: `pci_msi_set(dev, index, addr, data)`,
  `pci_msix_mask(dev, index, bool)`, enable/disable, INTx disable.
- `pci_set_bus_master(dev, bool)` + read-back.
- Log: one line per function (`pci: 00:14.0 8086:7a60 class 0c0330 xHCI
  msi/msix bars ...`) and RESULTS lines: function count, the xHCI and NIC
  lines with their interrupt capabilities.
- Boot menu entry **"Devices"** (`pcilist`, already in boot/limine.conf;
  main.c calls `pci_report()`): enumeration only, the RESULTS box lists
  every function (the first PC check of M6). The box holds 48 lines of
  120 characters and the PC may have 40+ functions: pack two functions per
  line if needed, always show the xHCI and NIC in full with their MSI and
  MSI-X vector counts and BARs.
- ktests on QEMU q35: host bridge 8086:29c0 present, edu 1234:11e8 found
  with MSI, qemu-xhci 1b36:000d with MSI-X, BAR sizes as QEMU defines them,
  sizing restores the BAR, the display device is skipped.

### Track B: vectors and interrupt objects (agent 2)
- Per-CPU vector allocator over 0x31-0xef (0x30 stays COM1), target CPU
  policy above; `irq_register` path for device vectors; spurious/unowned
  vectors counted.
- `interrupt` object: create from (dev, MSI or MSI-X index) using Track A
  primitives; fire path (IRQ context: LAPIC EOI, mask if MSI-X, queue the
  preallocated packet or raise the signal); `interrupt_ack`; destroy with
  `irq_sync` (no handler still running anywhere); process kill releases.
- A kernel-only `interrupt_create_virtual` + `interrupt_fire_test` for
  ktests (no device needed).
- Syscalls `interrupt_create_msi` (RES_PCI_DEV), `interrupt_ack`.
- Check/fix: the signal -> port path from IRQ context (above).
- Bench line: "MSI round trip: device raise -> driver thread wakes" (edu
  raise register, in kernel; process version in phase 2).
- ktests: coalescing (fire 3x before ack -> one packet, count 3), ack
  re-arms, destroy while firing on another CPU (virtual source fired from
  an IPI loop), port closed while a packet is queued, job charge.

### Track C: resources, MMIO VMOs, DMA (agent 3)
- `resource` object: root at boot (whole physical space minus RAM),
  `resource_create(parent, kind, base, size)` slicing, rights (`RIGHT_MAP`,
  `RIGHT_SLICE`); `RES_PCI` / `RES_PCI_DEV` from Track A's table.
- `vmo_create_physical` syscall: resource check, `pci_phys_protected`
  refusal, UC/WC via PAT in user mappings (aspace maps honour the VMO's
  cache type), not committable/decommittable, never counted as RAM.
- `dma_cap` bound to a BDF; `vmo_pin`/`vmo_unpin` syscalls with user
  copies of the address list; the close path (BME off, read-back, unpin);
  `VMO_DMA32` contiguous allocation (pmm zone already exists).
- The rest of the M6 syscalls: `resource_create`, `pci_enum`,
  `pci_device_open`, `pci_config_read/write` with the filter,
  `pci_bar_resource`, `pci_bus_master` (RIGHT_MANAGE); userboot puts the
  root resource into init's startup message (`SR_RESOURCE`).
- ktests: slice outside parent refused, MMIO over RAM refused, MSI-X page
  refused, pin without BME refused, close clears BME (read config back),
  config filter, kill mid-pin, job clean afterwards.

### Track D: driver.h, both builds, IDL, build check (agent 4)
- `driver.h` + the kernel implementation (kernel process: `struct process`
  on the kernel aspace, `driver_thread_start(name, driver_main, start)`)
  + the libos implementation; `dmalloc` in both.
- Build: `drivers/<name>/*.c` compiled twice (kernel object linked into
  jamos.elf; user ELF into bootfs), include isolation, `checkdriver.py`.
- `tools/genidl.py` + `abi/idl/null.idl`, `abi/idl/edu.idl`; generated
  code committed and checked for staleness like syscalls.def.
- A tiny `drivers/null` test driver (echo server over an IDL protocol)
  that runs both ways, with ktests/utests using the generated client.

### Phase 2: devmgr, edu, xhci-noop (after the merge; 1-2 agents)
- **devmgr** (user process from bootfs, started by init): `pci_enum`,
  match table (vendor/device/class -> driver ELF), per device:
  `pci_device_open`, BAR resources, interrupt objects, dma_cap (and
  `pci_bus_master` on), a copy of the device handle without RIGHT_MANAGE,
  spawn the driver in a child job with a quota. Logs bindings.
- **edu driver**: identification register, liveness (0x04 invert),
  factorial with and without interrupt, DMA RAM -> device -> RAM through a
  pinned DMA32 VMO with a completion interrupt; exposes the `edu` IDL
  protocol; init/utest calls it as a client.
- **xhci-noop driver**: BIOS/OS handoff (USBLEGSUP), halt + HCRST, wait
  CNR, scratchpad buffers, DCBAA, command ring, event ring + ERST,
  interrupter 0 (IMAN.IE), MSI or MSI-X via an interrupt object, run,
  doorbell 0 with a No Op Command TRB, wait for the Command Completion
  event (completion code Success) via the interrupt, report latency, halt
  the controller again. Runs on qemu-xhci and on the PC.
- Boot entries: kernel-thread mode (`drivers=kernel`) and process mode
  (default, under init + devmgr); RESULTS lines from each driver.
- utest: kill the edu driver mid-DMA -> BME off, pins gone, vectors freed,
  job clean; a driver can't map an MSI-X page; can't pin without a
  dma_cap; can't open another device's config.
- Bench: MSI round trip in a process; edu DMA 4 KiB round trip.

## Done when
- QEMU at 4 and 8 CPUs: all ktests; init + utest; edu and xhci-noop pass
  in both modes; the kill-mid-DMA test; stress; every crash test.
- The real PC: "Devices" lists the functions (matches Windows' Device
  Manager: xHCI 8086:7A60, RTL8125 10EC:8125, the RTX card, ...);
  xhci-noop completes via MSI/MSI-X in the kernel and as a process; All
  tests, Benchmark (new interrupt lines), 2-min stress; 10-min stress as
  the sign-off.

## Rules for the agents
- Work only in your worktree and your track's files; the foundation
  headers are the contract. If an interface needs to change, say so in the
  report instead of changing another track's side.
- Every commit leaves the tree building with all existing ktests passing
  (4 and 8 CPUs in QEMU: `tools/qemu-test.sh`). Add tests for everything.
- No `Co-Authored-By` trailer on commits. Don't push, don't touch main,
  never write to a USB disk.
- Don't run tests in long repeated loops; one run at 4 and one at 8 CPUs
  per change is enough unless you are chasing a race.
- Anything that only runs on the PC (not in QEMU) must say so in the
  report, with what the PC run should show.
