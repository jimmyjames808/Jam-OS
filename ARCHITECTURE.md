# Jam OS architecture

This is the plan of record. Decisions marked *(open)* are not settled yet.

## Goals

- Runs on a real x86_64 PC, booting via UEFI from a USB stick.
- Close to daily-drivable, text console first (graphics are not a goal yet).
- **Microkernel one driver at a time**: each driver and service is brought up
  inside the kernel (where debugging on the real PC is easiest) and moved to
  userspace in the same milestone, as soon as it is proven working there.

| Decision | Choice |
|---|---|
| Language | C (gnu17, freestanding), x86_64-elf-gcc cross toolchain |
| Build | Make |
| Boot | Limine now, behind `struct boot_info`; own UEFI loader later |
| Kernel | Monolithic, strict message-shaped driver boundary from day one |
| SMP | From day one |
| Native API | Capability handles; POSIX layer (musl) possible later |
| Process creation | No `fork`, ever; a POSIX layer gets `posix_spawn` |
| Syscall ABI | Unstable until M12; numbers, wrappers and the kernel dispatch table are generated from one table |
| IPC | Async channels + synchronous `channel_call`; ports for multi-wait |
| Bulk data | Through shared VMOs (rings + offsets), not 64 KiB channel messages |
| Memory API | VMOs + VMAR handles |
| Scheduler | Per-CPU run queues, 32 priorities, work stealing |
| Filesystem | FAT32 only, on USB mass storage; the boot partition (ESP) is read-only to Jam OS |
| Ported code | Limine, uACPI, lwIP |
| Executables | Static ELF64 |
| Program output | Every program gets a stdout channel handle in its startup message; M5 backs it with a `debug_write` syscall, the M7 console service replaces that |
| IOMMU | Not yet; DMA gated by `dma_cap`, VT-d added behind it in M11 |
| Driver migration | Decided 2026-09-28: move each driver out once proven on the PC, not in one late milestone |
| Users/logins | Decided 2026-09-29: single user, no accounts. Handles are the only authority; a future "user" would be a namespace root plus a job quota (FAT32 cannot store owners anyway) |
| Target PC | i7-14700 (hybrid 8P+12E, Hyper-Threading on: 28 CPUs in x2APIC mode; xAPIC mode only reached 20), 32 GB, RTX 4080 SUPER (Resizable BAR on, framebuffer at 256 GiB), Intel AX201 Wi-Fi, no serial port |
| Networking | *(open, decide at M9)*: leaning towards a USB Ethernet adapter (CDC-NCM/ECM, reuses the M7 USB stack); the board may have no Ethernet chip. Wi-Fi is not planned |

## The migration rule

Every driver and service is written as if it were already a userspace process.

- Drivers touch the world **only through handles**: channels, interrupt
  objects, VMOs, resource handles (MMIO, IRQs) and a DMA capability.
- Drivers include only `<jam/driver.h>`. It has two implementations: in the
  monolithic build the calls are direct kernel calls and the driver runs as a
  kernel thread with its own handle table; in the microkernel build the same
  calls are syscalls.
- Driver code must never call `kmalloc`, touch kernel structs, or call another
  driver. A build check will enforce this.

Moving a driver to userspace is then a rebuild and a relaunch, not a rewrite.

**When a driver moves:** as soon as it is proven working on the real PC, in
the same milestone that introduced it. Bring-up happens as a kernel thread
(full panic screen, direct logging); the milestone is only done once the
same driver passes the same checks as a userspace process. M6 therefore
builds the userspace driver support (interrupt objects, MMIO resource
handles, DMA-pinned VMOs mapped into processes, devmgr as a process) along
with the framework itself.

## Layers

```
 Userland: init · shell · coreutils                            ring 3
 libos runtime (syscalls, malloc, channels, namespace, ELF loader)
 ─────────────────────────────────────────────────────────────
 Services (in-kernel for bring-up, then userspace):
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
- Order today (`kernel/core/main.c`): early console, PMM/VMM, heap, then on
  the kernel's own stack: ACPI tables (MADT/MCFG/HPET), BSP LAPIC, TSC
  calibration, BSP per-CPU, scheduler (the boot code becomes thread "main"),
  IPIs, IOAPIC, LAPIC timer, AP startup, interrupts on. Later: uACPI, devmgr,
  userboot → init.

## Memory

- **PMM**: buddy allocator (orders 0-10, up to 4 MiB) with a DMA32 zone below
  4 GiB; allocations prefer the normal zone. `struct page` (32 bytes) lives in
  a **vmemmap** indexed by PFN, backed in 2 MiB chunks; chunks over pure MMIO
  holes stay unmapped (a buddy block never spans two chunks). Physical page 0
  is never handed out. An early bump allocator (top-down) builds the first
  page tables and the vmemmap. Per-CPU page caches: later, if profiling asks.
- **Loader memory** (Limine's stack, tables, and the code the parked APs spin
  in) is reclaimed in M2, after the APs have started.
- **VMM**: own 4-level tables (no dependency on the loader's). Kernel image
  mapped per section (text RX, rodata R, data RW+NX), HHDM with 1 GiB/2 MiB
  pages for RAM only (write-back), framebuffer write-combining via PAT index 5.
  W^X holds for every alias of a page, including the HHDM view of the kernel
  image (M4.5). All 256 kernel-half PDPTs are created up front so every
  address space can share PML4 entries 256-511. Layout:
  `ffff800000000000` HHDM, `ffffc00000000000` vmemmap,
  `ffffd00000000000` vmap (kernel stacks with guard pages, MMIO),
  `ffffffff80000000` kernel image.
- **Heap**: `kmem_cache` slabs (header on-slab, pages tagged `PG_SLAB`) with
  kmalloc classes 16-2048; larger requests take whole buddy blocks.
- **VMO** kinds:
  - *paged*: sparse 3-level page tree, pages committed on demand, up to 64 GiB;
  - *contiguous*: one buddy block, up to 4 MiB, committed at creation;
  - *DMA32*: like contiguous, below 4 GiB;
  - *physical*: a fixed physical range (MMIO, framebuffer), never freed.
  Contiguous and DMA32 VMOs need a `dma_cap` handle at the syscall layer.
  Guards: a pinned or kernel-mapped range can't be decommitted or cut off by
  a shrink (`ERR_BAD_STATE`); pins hold references on both the VMO and the
  `dma_cap`.
- **VMAR** (M5): one flat address space per process, no nested VMARs. Maps
  VMOs with R/W/X; W^X always, and an executable mapping needs `RIGHT_EXEC`
  on the VMO handle. User mappings are tracked in a per-VMO reverse map, so
  decommit and shrink unmap the pages from every address space (pins, which
  are DMA, still block them). Lock order: address-space region lock (a
  sleeping mutex) above `vmo`, page-table lock (a spinlock) below it.
- **DMA**: `vmo_pin` needs a `dma_cap`. IOMMU goes behind the same API later.
- **TLB**: today every kernel unmap does a synchronous range shootdown to all
  CPUs (a full flush above 64 pages). M5 adds per-address-space active-CPU
  masks and batching ("gather": free pages only after the flush).

## SMP

- Per-CPU block through `GS`: current thread, preempt count, IRQ depth, held
  locks (for the checker), watchdog state, statistics. Run queues live in a
  static array indexed by CPU. `swapgs` arrives with ring 3 in M5; NMI, #MC
  and #DB will check GS themselves, since they can land in user mode or in
  the `swapgs` window.
- **Ticket spinlocks** (`spin_lock_irqsave` where interrupt handlers also
  take the lock). Holding one disables preemption. A lock's *name* is its
  class for the lock-order checker (`core/lockdep.c`), which records every
  "A held while taking B" pair in a 64x64 bit matrix and panics on the first
  acquisition that closes a cycle (ABBA), on taking a lock twice, on nesting
  two locks of the same class (unless taken with `spin_lock_nested(subclass)`),
  on a class used both in interrupt handlers and with interrupts on, and on
  any lock spinning for 5 s (naming the holder). Always on. M4.5 gives it a
  lockless fast path (edges and IRQ flags only ever get set, so only a new
  edge takes the graph lock), 256 classes, and sleeping-mutex tracking.
- IPIs: reschedule, cross-CPU calls (`smp_call_on/others/all`, which refuse
  to run with interrupts off), TLB shootdown on every kernel unmap, and NMI
  halt of all other CPUs on panic.
- **Watchdog**: each CPU checks the next one's tick count once a second; a
  CPU stuck with interrupts off for 5 s gets an NMI and panics with its own
  registers and backtrace.
- Log lines carry `[seconds.micros]` timestamps. The log lock is always taken
  with interrupts off, so interrupt handlers may log; hot handlers (the LAPIC
  error handler) just count instead.
- **Per-CPU access rule**: `this_cpu()` is two instructions (load the struct
  pointer, then the field), so from preemptible code a thread can migrate in
  between and read another CPU's data. Preemptible code uses single
  GS-relative instructions (`current_thread()`, `preempt_disable/enable`);
  everything else calls `this_cpu()` only with preemption or interrupts off.
  (The stress test found this: a thread saw another CPU's current thread and
  "didn't own" its own mutex.)
- A wakeup that lands before the thread has switched out just marks it
  running again (as Linux does); waiting for it to switch out could deadlock
  when the waker is an interrupt on that thread's own CPU.
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
- ACPI tables can live in firmware-reserved memory the HHDM skips, so they
  are read through `acpi_map`, which maps pages on demand.

## Objects and handles

- Every object embeds a `kobject`: type, refcount, handle count, signal bits,
  observers (waiters and port bindings). The last reference is dropped
  through an iterative teardown (a per-CPU pending list in `object.c`), so
  destroying a channel full of channels never recurses.
- Per-process handle table, 65,536 slots. A handle value is
  `(slot + 1) << 15 | generation` (17 + 15 bits; 0 is never valid); freed
  slots are reused FIFO so a stale value takes a long time to come back.
- Rights: `READ WRITE EXEC MAP DUPLICATE TRANSFER SIGNAL WAIT INSPECT`.
  `RIGHT_SAME` is only a sentinel for duplicate/replace ("keep the rights").
- Sending handles uses **in-transit slots**: `handle_take` reserves the slot,
  then `handle_commit` (sent) or `handle_untake` (put back under the same
  value). A failed send never loses a handle.
- Types today: `channel port event timer vmo dma_cap`. M5 adds
  `vmar process thread job`; M6 adds `interrupt resource`.
- `resource` is the root of hardware authority (MMIO ranges, IRQs). init
  holds the root, passes slices to devmgr, which gives each driver only its
  own BARs and IRQ.
- **Jobs** (M5): every process belongs to a job with limits on committed
  pages, handles and threads, inherited by child jobs. Queued channel
  messages are charged to the sender's job. Any allocation a syscall can
  reach returns `ERR_NO_MEMORY` instead of panicking.

## IPC

- **Channel**: bidirectional endpoint pair; message = up to 64 KiB of bytes +
  up to 64 handles; sending a handle moves it. At most 1024 messages queue on
  one endpoint (then `ERR_SHOULD_WAIT`). Cycle rule: you can't send an
  endpoint of the channel you are writing to, its peer, or an endpoint whose
  own queue already holds an endpoint.
- **`channel_call`**: write + wait for the reply whose first 4 bytes (the
  txid) match. The kernel stamps the txid; only the calling thread gets the
  reply; a reply arriving after a timeout stays queued as a normal message;
  closing your own endpoint mid-call returns `ERR_CANCELED`; calling on a
  closed endpoint returns `ERR_BAD_STATE`. Handing the CPU straight to the
  server thread (wake-affine placement) is not built yet: M5, before services
  are built on it.
- **Port**: bind many handles and wait on all of them; matching signals queue
  packets. `ONCE` bindings fire once; `PERSISTENT` ones stay and coalesce
  into one queued packet with a count. Limits: 4096 user packets, 4096
  bindings per port; ports can't be bound to ports. The lock rank is
  documented in `port.h`. From M6, **IRQs are port packets**, the same
  in-kernel and in userspace.
- **`object_wait_one`** for simple waits.
- **Protocols** are written in a small IDL, turned into C structs and stubs by
  a Python generator: `block netdev hid fs console power devmgr socket usb-bus`.
  Bulk data (disk blocks, packets, file contents) moves through a shared VMO
  ring; messages carry offsets. From M7 every protocol defines how a client
  reconnects after `PEER_CLOSED` (the server restarted).

## Scheduler

- Per-CPU run queues, 32 priority levels (31 most urgent), round-robin
  within a level, 20 ms slices on the 100 Hz tick.
- Placement: least-loaded allowed CPU; ties go to P-cores, then the thread's
  last CPU. Idle CPUs steal the best waiting thread from busy ones (two run
  queue locks, always lower CPU index first). 256-bit affinity masks;
  `thread_create_on` sets the mask before the thread first runs.
- **Anti-starvation**: once a second each CPU boosts threads that have waited
  over 1 s to priority 30 for one slice. Priority 31 is above the boost, so
  real-time threads can still starve others by design. User threads are
  capped at priority 24 unless a capability allows more (M5).
- Preemptible kernel: switches happen on interrupt exit or when the last
  spinlock is dropped, never with one held (`schedule()` panics if called
  with preemption disabled).
- Switch-safety: the run queue lock is held across `switch_context` and
  released by the next thread; `on_cpu` stays set until a switched-out
  thread's registers are saved, and whoever picks it waits for that. This
  covers a thread being woken while it is still switching out.
- Kernel threads: `thread_create/exit/join/yield/sleep`, `thread_block(lock,
  deadline)`, wait queues with a condition-variable style
  `waitqueue_wait(wq, lock)`, sleeping mutexes. Up to 256 exited threads'
  stacks are cached for reuse. Timers and sleepers are woken by CPU 0's tick
  (about 10 ms resolution).
- **Cancellable waits** (M4.5): a per-thread cancel flag; every blocking path
  (`thread_block`, wait queues, `object_wait_one`, `port_wait`,
  `channel_call`, mutexes) returns `ERR_CANCELED` once it is set, and
  `thread_kill` sets it and wakes the thread. M5 needs this to kill a process
  blocked in `channel_call` on a hung server.
- To do: `channel_call` handoff / wake-affine placement (M5); hybrid
  placement order (idle P-core pair > idle E-core > busy HT sibling);
  per-CPU one-shot timers instead of CPU 0's tick; stack cache pages beyond
  256 go back to the allocator; tickless idle; `sched_ops`.

## Drivers and services

| Component | Uses | Provides |
|---|---|---|
| devmgr | root resource, uACPI | PCIe enumeration (ECAM/MCFG), driver binding, BAR/IRQ(MSI-X)/DMA handoff |
| xHCI | PCI resources | `usb-bus` |
| USB HID | usb-bus | `hid` → keyboard layer |
| USB mass storage (BOT, later UAS) | usb-bus | `block` |
| fat32 | `block` | `fs` (FAT32 + LFN, read/write) |
| NIC *(decide at M9; leaning USB CDC-NCM/ECM adapter)* | usb-bus (or PCI resources) | `netdev` |
| netstack | lwIP + `netdev` | `socket` |
| power | uACPI | shutdown, reboot, power button, later S3 |
| console | framebuffer VMO + `hid` | text terminal |

uACPI stays in the kernel permanently; everything else moves out.

Rules for userspace drivers:

- devmgr programs MSI/MSI-X itself. A driver never gets raw ECAM access, and
  its BAR mapping never includes the MSI-X table page.
- A `dma_cap` is bound to one device (PCI bus/device/function). `vmo_pin`
  returns *device addresses* (physical today, IOMMU addresses after M11).
  Closing a `dma_cap` clears the device's Bus Master Enable, then releases
  its pins.
- MSI/MSI-X and MMIO only: no port I/O for userspace drivers.
- **Supervision from M7**: devmgr restarts a crashed driver process, and
  clients reconnect on `PEER_CLOSED` as their protocol defines.

## Userland

- **libos**: startup, syscall wrappers, malloc, printf, channel/port helpers,
  IDL stubs, the ELF loader.
- **userboot**: a tiny ELF loader in the kernel starts init from bootfs.
  Everything after that is loaded by libos; the kernel only provides "create
  process, map, start at entry with stack".
- **Startup message**: every process starts with one channel message holding
  argv, environment, namespace entries, stdin/stdout/stderr, and handles to
  itself and its address space.
- **Namespace**: each process gets a table of path → channel handle (`/boot`,
  `/data`, `/svc/net`, `/dev/console`). No global kernel VFS. A future POSIX
  `open()` is built on this. Namespaces are the only permission system.
- **init**: holds root capabilities, reads `/boot/init.cfg`, starts services
  with only the handles they need.
- **Shell**: processes, pipes over channels; built-ins `ls cat cp rm mkdir ps
  kill mem ifconfig ping reboot poweroff`. From M7 the kernel tests are shell
  commands too (`ktest`, `stress 600`), so a test run no longer needs a
  reboot; rebooting is only for loading a new kernel from the stick.
- **Executables**: static ELF64; no `fork`.

## Storage

- The USB stick has two FAT32 partitions: the **ESP** (Limine, kernel,
  bootfs modules), which Jam OS never writes, and a **data partition**
  mounted at `/data` for everything writable. A bug in the FAT32 writer can't
  make the stick unbootable.
- Write ordering: file data, then both FATs, then the directory entry.
- The FAT "clean shutdown" bit is cleared while mounted and set on unmount;
  a dirty volume is checked on mount. `sync` and unmount send SCSI
  SYNCHRONIZE CACHE.
- The 4 GiB file limit and the lack of owners/permissions are accepted:
  authority comes from namespaces, not the filesystem.

## Debugging

- Framebuffer klog from the first instruction, 64 KiB ring buffer (later
  readable via `klog_read`). COM1 too when present (QEMU).
- Panic screen: message, decoded exception (page-fault cause, NULL and stack
  overflow hints), all registers and control registers, symbolised backtrace
  with repeated frames collapsed, and the log tail. Symbols come from a
  two-pass link: `.ksyms` is the last section, so filling it in moves nothing
  (checked by `tools/gensyms.py verify`). #DF, NMI and #MC run on IST stacks.
  The other CPUs are halted by NMI first.
- **ktest**: `KTEST(name)` registers a test in the `.ktests` section. Boot
  with `ktest` (all) or `ktest=prefix`. Each test fails if it leaks pages;
  the run reports how many lock classes are in use.
- **DBG_HOOK** injection points (`dbghook.h`) let race regression tests stop
  a thread at an exact line (the lost-wakeup and teardown races each have
  one).
- `tools/qemu-test.sh` boots any kernel command line headless
  (`QEMU_SMP`, `QEMU_MEM`, `QEMU_CPU`, `QEMU_TIMEOUT`) and saves the serial
  log and a screenshot; the boot menu has matching test entries, including
  crash tests that must panic (`testpf`, `testro`, `teststack`,
  `testlockorder`, `testwatchdog`, `testpanic`).
- QEMU mirrors the PC: q35, OVMF, xHCI USB boot, e1000e (`make run`), gdb
  stub (`make debug`).
- Per-CPU watchdog heartbeat: a stuck core turns into a panic screen.

## Milestones

| # | Milestone | Done when |
|---|---|---|
| **M0** ✅ | Toolchain, QEMU q35/OVMF, USB image, framebuffer console, panic screen | booted on the real PC 2026-09-28 |
| **M1** ✅ | PMM, VMM, heap, GDT/TSS/IDT, full panic screen with symbols | all tests passed on the real PC 2026-09-28 |
| **M2** ✅ | ACPI tables, LAPIC (x2APIC + xAPIC), IOAPIC/PIC masked, TSC + APIC timers, all cores, P/E topology, loader memory reclaimed | real PC 2026-09-28: 28 CPUs (16 P-threads + 12 E-cores, HT on), all exactly 100 ticks, TSC-deadline |
| **M3** ✅ | Scheduler, kernel threads, ticket locks + lock-order checker, IPIs, TLB shootdown, watchdog, stress test | real PC 2026-09-28: 10-min stress passed (112 threads, 28 CPUs, lock checking on), after fixing an IRQ race in the checker found by the first run at 230 s |
| **M4** ✅ | Objects, handles, channels, ports, events, timers, VMOs, handle-level `sys_` API; two-agent audit, 20 fixes | QEMU 64/64 ktests at 4 and 8 CPUs; real PC 2026-09-28 (channels + ports): 1 server + 27 clients, 492,673 calls/s, worst 67 us. The final audited build still has to run on the PC |
| M4.5 | Hardening: W^X on the HHDM alias, received-handle and signal gaps, channel "has room" signal, lock checker scaling, cancellable waits, new tests, stale docs | all ktests + new tests at 4 and 8 CPUs, stress and crash tests pass; then M4 tests + 10-min stress on the PC |
| M5 | Ring 3 (SMEP/SMAP, `swapgs`, eager XSAVE), syscalls, VMARs, processes, threads, jobs + quotas, userboot, bootfs, init, `debug_write` stdout | init runs from bootfs; a process killed mid-`channel_call` cleans up; a runaway process hits its job quota, not a panic |
| M6 | devmgr, PCIe, MSI/MSI-X, `<jam/driver.h>` in both builds; interrupt objects, resource handles, DMA VMOs for processes | a sample driver bound through the handle-only API runs in the kernel, then as a process |
| M7 | xHCI → HID → console → interactive shell (each moved to userspace once working); driver supervision; `reboot` command + Ctrl+Alt+Del; tests as shell commands | typing into the shell on the real PC with the USB drivers as processes; killing the HID driver mid-use recovers; `ktest` runs from the shell |
| M8 | USB mass storage → FAT32 (userspace once working), read-only ESP + writable data partition | `ls /boot` and writing a file under `/data` from a userspace filesystem service; the stick still boots after a pulled-plug test |
| M9 | NIC (decide: likely USB CDC-NCM/ECM) → lwIP → DHCP/DNS (userspace once working) | `ping 1.1.1.1` on the real PC through a userspace network stack |
| M10 | uACPI poweroff, power button, ACPI reboot (stays in the kernel) | clean shutdown on real hardware |
| M11 | IOMMU (VT-d) + interrupt remapping behind `dma_cap` | DMA outside a driver's pinned VMOs is blocked |
| M12 | S3 sleep, own UEFI loader, POSIX on musl, stable syscall ABI | stretch |

Every milestone is checked on the real PC from M0 on.
