# Jam OS architecture

This is the plan of record. Decisions marked *(open)* are not settled yet.

## Goals

- Runs on a real x86_64 PC, booting via UEFI from a USB stick.
- Close to daily-drivable, text console first (graphics are not a goal yet).
- **Monolithic first, microkernel soon after**: drivers and services move to
  userspace once the system works end to end.

| Decision | Choice |
|---|---|
| Language | C (gnu17, freestanding), x86_64-elf-gcc cross toolchain |
| Build | Make |
| Boot | Limine now, behind `struct boot_info`; own UEFI loader later |
| Kernel | Monolithic, strict message-shaped driver boundary from day one |
| SMP | From day one |
| Native API | Capability handles; POSIX layer (musl) possible later |
| IPC | Async channels + synchronous `channel_call`; ports for multi-wait |
| Memory API | VMOs + VMAR handles |
| Scheduler | Per-CPU run queues, 32 priorities, work stealing |
| Filesystem | FAT32 only, on USB mass storage |
| Ported code | Limine, uACPI, lwIP |
| Executables | Static ELF64 |
| IOMMU | Not yet; DMA gated by `dma_cap`, VT-d/AMD-Vi added behind it after M11 |
| Users/logins | *(open)* |
| Target PC | i7-14700 (hybrid 8P+12E, Hyper-Threading on: 28 CPUs in x2APIC mode; xAPIC mode only reached 20), 32 GB, RTX 4080 SUPER (Resizable BAR on, framebuffer at 256 GiB), Intel AX201 Wi-Fi, no serial port |
| Networking | *(open: board Ethernet port or USB Ethernet adapter; Wi-Fi is not planned)* |

## The migration rule

Every driver and service is written as if it were already a userspace process.

- Drivers touch the world **only through handles**: channels, interrupt
  objects, VMOs, resource handles (I/O ports, MMIO, IRQs) and a DMA capability.
- Drivers include only `<jam/driver.h>`. It has two implementations: in the
  monolithic build the calls are direct kernel calls and the driver runs as a
  kernel thread with its own handle table; in the microkernel build the same
  calls are syscalls.
- Driver code must never call `kmalloc`, touch kernel structs, or call another
  driver. A build check will enforce this.

Moving a driver to userspace is then a rebuild and a relaunch, not a rewrite.

## Layers

```
 Userland: init · shell · coreutils                            ring 3
 libos runtime (syscalls, malloc, channels, namespace)
 ─────────────────────────────────────────────────────────────
 Services (in-kernel now, userspace later):
   devmgr · fat32 · netstack (lwIP) · console · power
 Drivers: xHCI → USB HID / USB mass storage · NIC
 ───────────── <jam/driver.h> boundary (handles only) ────────
 Kernel core: objects & handles · channels · ports             ring 0
   scheduler · VMOs & address spaces · IRQ routing · uACPI
   PMM · VMM · per-CPU · LAPIC/IOAPIC · timers · panic/klog
 ─────────────────────────────────────────────────────────────
 struct boot_info  ←  Limine (later: own UEFI loader)
```

## Boot

- `kernel/boot/limine.c` is the only file that includes `limine.h`. It fills
  `struct boot_info` (all physical addresses) and calls `kmain`.
- Limine modules become **bootfs**, a read-only in-memory FS holding init and
  the shell, so userspace starts before USB and FAT32 work.
- Order: early console, PMM/VMM, heap, BSP per-CPU, ACPI tables
  (MADT/MCFG/HPET), LAPIC/IOAPIC, AP startup, scheduler, uACPI, devmgr, init.

## Memory

- **PMM**: buddy allocator (orders 0-10, up to 4 MiB) with a DMA32 zone below
  4 GiB; allocations prefer the normal zone. `struct page` (32 bytes) lives in
  a **vmemmap** indexed by PFN, backed in 2 MiB chunks; chunks over pure MMIO
  holes stay unmapped (a buddy block never spans two chunks). Physical page 0
  is never handed out. An early bump allocator (top-down) builds the first
  page tables and the vmemmap. Per-CPU page caches arrive with M3.
- **Loader memory** (Limine's stack, tables, and the code the parked APs spin
  in) is reclaimed in M2, after the APs have started.
- **VMM**: own 4-level tables (no dependency on the loader's). Kernel image
  mapped per section (text RX, rodata R, data RW+NX), HHDM with 1 GiB/2 MiB
  pages for RAM only (write-back), framebuffer write-combining via PAT index 5.
  All 256 kernel-half PDPTs are created up front so every address space can
  share PML4 entries 256-511. Layout:
  `ffff800000000000` HHDM, `ffffc00000000000` vmemmap,
  `ffffd00000000000` vmap (kernel stacks with guard pages, MMIO),
  `ffffffff80000000` kernel image.
- **Heap**: `kmem_cache` slabs (header on-slab, pages tagged `PG_SLAB`) with
  kmalloc classes 16-2048; larger requests take whole buddy blocks.
- **VMO**: pages on demand; pinnable and physically contiguous for DMA;
  shareable by handle.
- **VMAR**: handle to an address-space region; map VMOs with R/W/X rights.
- **DMA**: `vmo_pin` needs a `dma_cap`. IOMMU goes behind the same API later.
- **TLB**: batched shootdowns via IPI.

## SMP

- Per-CPU block through `GS` (current thread, run queue, timer, IRQ depth);
  careful `swapgs` at every entry point.
- Ticket spinlocks that save/restore IF, with **lock-order checking** in debug
  builds.
- IPIs: reschedule, TLB shootdown, panic halt, cross-CPU calls.
- Timekeeping: TSC measured against the HPET (then ACPI PM timer, CPUID 15h,
  loader estimate; all printed for comparison). LAPIC timer in TSC-deadline
  mode where available (`nodeadline` forces the periodic fallback, which is
  calibrated as the median of 5 bracketed runs). 100 Hz tick for now;
  tickless idle later.
- APs: Limine parks them; `boot_start_cpu` releases each onto a struct cpu
  prepared by the BSP (64 KiB guard-paged stack, own GDT/TSS with guarded IST
  stacks). The AP enables NX/WP/PGE/PAT before loading the kernel CR3.
  Loader-reclaimable memory is freed only once every AP is online.
- Topology per CPU: P-core/E-core from CPUID 1Ah, core/thread ids from
  CPUID 1Fh/0Bh; the report says whether Hyper-Threading is on.
- Interrupt handlers never log (the interrupted code may hold the log lock);
  they count errors, reported later. `smp_run_on_all` runs a function on
  every CPU by piggybacking on the tick, a stopgap until M3 has IPIs.
- ACPI tables can live in firmware-reserved memory the HHDM skips, so they
  are read through `acpi_map`, which maps pages on demand.

## Objects and handles

- Every object embeds a `kobject`: type, refcount, signal bits, waiter list.
- Per-process handle table. A handle is 32 bits (slot + generation) with a
  rights mask: `READ WRITE EXEC MAP DUPLICATE TRANSFER SIGNAL WAIT`.
- Types: `process thread vmo vmar channel port event timer interrupt resource dma_cap`.
- `resource` is the root of hardware authority (port ranges, MMIO ranges,
  IRQs). init holds the root, passes slices to devmgr, which gives each driver
  only its own BARs and IRQ.

## IPC

- **Channel**: bidirectional endpoint pair; message = up to 64 KiB of bytes +
  up to 64 handles; sending a handle moves it.
- **`channel_call`**: write + wait for the reply with the matching txid. The
  scheduler hands the CPU directly to a waiting server thread.
- **Port**: bind many handles and wait on all of them; matching signals queue
  packets. **IRQs are port packets**, the same in-kernel and in userspace.
- **`object_wait_one`** for simple waits.
- **Protocols** are written in a small IDL, turned into C structs and stubs by
  a Python generator: `block netdev hid fs console power devmgr socket usb-bus`.

## Scheduler

- Per-CPU run queues, 32 priority levels, round-robin within a level.
- Work stealing when idle; CPU affinity mask.
- IPC handoff on `channel_call`.
- Policy behind `sched_ops` so it can be replaced.
- Preemptible kernel, except under spinlocks or with IRQs off.

## Drivers and services

| Component | Uses | Provides |
|---|---|---|
| devmgr | root resource, uACPI | PCIe enumeration (ECAM/MCFG), driver binding, BAR/IRQ(MSI-X)/DMA handoff |
| xHCI | PCI resources | `usb-bus` |
| USB HID | usb-bus | `hid` → keyboard layer |
| USB mass storage (BOT, later UAS) | usb-bus | `block` |
| fat32 | `block` | `fs` (FAT32 + LFN, read/write) |
| NIC (Intel e1000e/igc or Realtek r8169/r8125, per the PC) | PCI resources | `netdev` |
| netstack | lwIP + `netdev` | `socket` |
| power | uACPI | shutdown, reboot, power button, later S3 |
| console | framebuffer VMO + `hid` | text terminal |

uACPI stays in the kernel permanently; everything else moves out.

## Userland

- **libos**: startup, syscall wrappers, malloc, printf, channel/port helpers,
  IDL stubs.
- **Namespace**: each process gets a table of path → channel handle (`/boot`,
  `/usb0`, `/svc/net`, `/dev/console`). No global kernel VFS. A future POSIX
  `open()` is built on this.
- **init**: holds root capabilities, reads `/boot/init.cfg`, starts services
  with only the handles they need.
- **Shell**: processes, pipes over channels; built-ins `ls cat cp rm mkdir ps
  kill mem ifconfig ping reboot poweroff`.
- **Executables**: static ELF64; the loader lives in libos, the kernel only
  provides "create process, start at entry with stack".

## Debugging

- Framebuffer klog from the first instruction, 64 KiB ring buffer (later
  readable via `klog_read`). COM1 too when present (QEMU).
- Panic screen: message, decoded exception (page-fault cause, NULL and stack
  overflow hints), all registers and control registers, symbolised backtrace
  with repeated frames collapsed, and the log tail. Symbols come from a
  two-pass link: `.ksyms` is the last section, so filling it in moves nothing
  (checked by `tools/gensyms.py verify`). #DF, NMI and #MC run on IST stacks.
  M3 halts the other CPUs first.
- `tools/qemu-test.sh` boots any kernel command line headless and saves the
  serial log and a screenshot; the boot menu has matching test entries.
- QEMU mirrors the PC: q35, OVMF, xHCI USB boot, e1000e (`make run`), gdb
  stub (`make debug`).
- Per-CPU watchdog heartbeat: a stuck core turns into a panic screen.

## Milestones

| # | Milestone | Done when |
|---|---|---|
| **M0** ✅ | Toolchain, QEMU q35/OVMF, USB image, framebuffer console, panic screen | booted on the real PC 2026-09-28 |
| **M1** ✅ | PMM, VMM, heap, GDT/TSS/IDT, full panic screen with symbols | all tests passed on the real PC 2026-09-28 |
| **M2** ✅ | ACPI tables, LAPIC (x2APIC + xAPIC), IOAPIC/PIC masked, TSC + APIC timers, all cores, P/E topology, loader memory reclaimed | QEMU: 4/8/20 CPUs tick at 100 Hz, 20-CPU allocator stress passes; *next: run on the real PC* |
| M3 | Scheduler, kernel threads, locks, IPIs | 10-min stress test with lock checking |
| M4 | Objects, handles, channels, ports, VMOs | in-kernel channel ping-pong |
| M5 | Ring 3, syscalls, ELF loader, bootfs, init | init runs from bootfs |
| M6 | devmgr, PCIe, MSI, `<jam/driver.h>` | drivers bound through the handle-only API |
| M7 | xHCI → HID → interactive shell | typing into the shell on the real PC |
| M8 | USB mass storage → FAT32 | `ls /usb0` |
| M9 | NIC → lwIP → DHCP/DNS | `ping 1.1.1.1` on the real PC |
| M10 | uACPI poweroff/reboot/power button | clean shutdown on real hardware |
| M11 | Migrate drivers to userspace (NIC, then USB storage, then xHCI) | same behaviour; a crashed driver restarts |
| M12 | S3 sleep, own UEFI loader, POSIX on musl, IOMMU | stretch |

Every milestone is checked on the real PC from M0 on.
