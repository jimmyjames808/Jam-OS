# Jam OS architecture

How Jam OS is built and why. This page is the design only: milestone status
is in [docs/ROADMAP.md](docs/ROADMAP.md), how it got here in
[docs/HISTORY.md](docs/HISTORY.md), the machine it runs on in
[docs/HARDWARE.md](docs/HARDWARE.md), and how the code is written in
[CODING-GUIDE.md](CODING-GUIDE.md). Where a section describes something not
built yet, it says so.

## Goals

- Runs on a real x86_64 PC, booting via UEFI from a USB stick.
- Close to daily-drivable, text console first.
- A microkernel structure: every driver and service is a user process that
  touches the world only through handles; the kernel keeps mechanisms and
  enforcement ([the migration rule](#the-migration-rule)).

| Decision | Choice |
|---|---|
| Language | C (gnu17, freestanding), x86_64-elf-gcc cross toolchain |
| Build | Make |
| Boot | Limine, behind `struct boot_info`; an own UEFI loader later |
| Kernel | Mechanisms and enforcement only; drivers and services are processes |
| SMP | From day one |
| Native API | Capability handles; a POSIX layer (musl) possible later |
| Process creation | No `fork`, ever; a POSIX layer gets `posix_spawn` |
| Syscall ABI | Unstable until M14; numbers, wrappers and the kernel dispatch table are generated from one table (`abi/syscalls.def`) |
| IPC | Async channels + synchronous `channel_call`; ports for multi-wait |
| Bulk data | Through shared VMOs (rings + offsets), not 64 KiB channel messages |
| Memory API | VMOs + VMAR handles |
| Scheduler | Per-CPU run queues, 32 priorities, work stealing |
| Filesystem | FAT32 only, on USB mass storage; the boot partition (ESP) is read-only to every program; only init writes it, for `update -w` |
| Ported code | Limine, FatFs (the FAT32 code, in the `fat` service), dr_mp3 (MP3), pl_mpeg (the boot splash's video), stb_image (album covers), lwIP (netstack's IPv4, ARP, ICMP and UDP); uACPI when power management lands |
| Executables | Static ELF64 |
| Program output | A stdout channel in the startup message when the parent gives one (the shell does, for pipes); otherwise the `debug_write` syscall into the kernel log, which the console shows (on a plain boot only while the shell runs that program in the foreground: [Debugging](#debugging)) |
| IOMMU | Not yet (M11). `dma_cap` gates pinning and bus mastering, not where a device writes: until VT-d a driver's device can reach all of RAM. The PC's firmware has a DMAR table |
| Users | Single user, no accounts. Handles are the only authority; a future "user" would be a namespace root plus a job quota (FAT32 can't store owners anyway) |
| Networking | The board's own NIC, driven natively; every frame in the configured mode only: tagged with one VLAN (the owner's builds: VLAN 21), or untagged and never tagged (a public build's default) ([Networking](#networking)) |

## What Jam OS defends against

What the design defends against today:

- **Programs.** A program can do only what its handles allow: the
  services under `/svc` and the mounts its list asks for (read-only, or
  writable with `etc` guarded), approved by the owner with `allow` for a
  program on `/data`. A server decides by the channel a request came on,
  never by who sent it (there is no sender identity in the IPC). A way for
  a program to get something it wasn't granted is a bug
  ([SECURITY.md](SECURITY.md)).
- **Crashing services and drivers.** Each is a process: a crash ends that
  process, devmgr or init starts it again, and the kernel goes on (its
  clients see the restart until M11.6). A driver's code can reach only
  what it was given: it can't program MSI, change another device's
  config, or keep its device mastering after it died
  ([the rules](#drivers-and-services)).

Not yet:

- **A driver that misprograms its device**, by a bug or on purpose. A
  device does what its driver tells it, and until the IOMMU (M11: VT-d
  and interrupt remapping) a DMA address the driver writes can be
  anywhere in RAM, the kernel included, or the interrupt window (any
  vector to any CPU). Drivers are crash-isolated, not contained: they are
  trusted.
- **A USB device that exploits usb-bus.** usb-bus holds the xHCI's
  `dma_cap` and parses every USB device's descriptors, so a bug there
  gives a hostile device DMA over all of RAM: the largest exposure today.
  The other parsers of what comes from outside (usb-storage, fat, hid,
  `play`, `jamcover`) hold no `dma_cap`, and neither do the processes
  that parse what the network sends (netstack, dhcp, dns, netlog,
  `bin/update`): the NIC's driver reads only a frame's length and tag.
- **Anyone holding the stick.** The ESP, `/data/etc/allow` and the logs
  can be changed on another computer: FAT32 keeps no owners and nothing
  on it is checked at boot (only `update`'s builds are signed, and the
  key that checks them is in the build on the stick), and authority never
  came from the filesystem.
- **Spectre-class attacks**: no mitigations.

## The migration rule

Every driver and service is a userspace process from the start.

- Drivers touch the world **only through handles**: channels, interrupt
  objects, VMOs, resource handles (MMIO, IRQs) and a DMA capability.
- Drivers include only `<jam/driver.h>` (plus the generated IDL headers
  and `<jam/task.h>`, libos's cooperative tasks), and `tools/checkdriver.py` fails the build if a driver uses anything else
  (kernel headers, `kmalloc`, another driver, extra sections). Driver code
  never touches kernel structs.
- **Bring-up happens as a process.** A crashing process reports why and
  where (`process "x" killed: ... at rip ...`) and can't take the kernel
  down (a driver still can, through its device's DMA, until the IOMMU:
  M11), and a stuck process can always be killed. Bringing a driver up in
  the kernel first was tried and bought nothing
  ([HISTORY.md](docs/HISTORY.md#lessons-that-keep-coming-back)).
- **One build.** `<jam/driver.h>` has one implementation, over system
  calls (`user/lib/driver_user.c`), and every driver is a process that
  devmgr starts and supervises.
- What stays in the kernel **on purpose** is enforcement, not drivers:
  the PCI core (ECAM, BAR sizing, MSI/MSI-X programming, Bus Master
  Enable), vector allocation and interrupt objects, resources, DMA pins and
  the config-write filter, so one driver can never configure another
  device's interrupts or turn DMA back on after it was killed. What this
  does not cover yet: a device does what its driver tells it, and until
  the IOMMU (M11) a DMA address the driver writes can be anywhere in RAM,
  the kernel included, or the interrupt window. So these rules keep a
  driver's own code in its box and a dead driver's device quiet; they
  don't contain a driver that misprograms its device, by a bug or on
  purpose ([what Jam OS defends against](#what-jam-os-defends-against)).

## Layers

```
 Userland: init · shell · apps                                 ring 3
 libos runtime (syscalls, malloc, channels, IDL stubs, ELF loader,
   the file namespace)
 ─────────────────────────────────────────────────────────────
 Services (processes): devmgr · console · serialin · bootfs ·
   fat (one per volume) · logd · mixer · music
   netstack (lwIP) · dhcp · dns · netlog
   later: power
 Drivers (processes): usb-bus (xHCI + hubs) → hid, usb-storage
   hda (HD Audio) · rtl8125, e1000e (network cards)
 ───────────── <jam/driver.h> boundary (handles only) ────────
 Kernel core: objects & handles · channels · ports             ring 0
   scheduler · VMOs & address spaces · PCI core · IRQ routing
   PMM · VMM · per-CPU · LAPIC/IOAPIC · timers · panic/klog
   later: uACPI
 ─────────────────────────────────────────────────────────────
 struct boot_info  ←  Limine (later: own UEFI loader)
```

## Boot

- `kernel/boot/limine.c` is the only file that includes `limine.h`. It fills
  `struct boot_info` (all physical addresses) and calls `kmain`. A kernel
  started by another Jam OS kernel ([kexec](#kexec-reboot-and-panic)) is
  entered at the same `_start` with a magic in `rdi`, and
  `kernel/boot/kexec.c` fills `struct boot_info` from the handoff instead.
- Limine modules become **bootfs**, a read-only in-memory FS holding init,
  the services, the drivers and the apps, so user space starts before
  storage works. One module, `bootfs.img` (`tools/mkbootfs.py`): a header,
  an entry table, each file on its own pages. The kernel validates it as
  untrusted input at boot and serves files as physical VMOs over the
  module's pages, without copying.
- The order (`kernel/main.c`): early console, PMM/VMM, heap, bootfs, then
  on the kernel's own stack: ACPI tables (MADT/MCFG/HPET), BSP LAPIC, TSC
  calibration, the wall clock, the [random number generator](#random-numbers),
  BSP per-CPU, scheduler (the boot code becomes thread
  "main"), IPIs, IOAPIC, serial interrupts, LAPIC timer, AP startup,
  interrupts on, the PCI core and resources; then either the boot-time
  tests (ktest, bench, stress) or userboot → init.
- **The boot splash** ([docs/history/AS-PLAN.md](docs/history/AS-PLAN.md)): on a plain
  boot (without `verbose`, `nosplash`, `nousb` or `soak`) fbcon's first
  act fills the screen with the splash's background and it draws no text
  from then on (quiet: the log still goes to the ring and serial; a panic
  draws as always); init gets the argument `splash`, starts the console
  quiet and then `bin/splash` before every other service, and starts the
  shell only once the animation has played, or 20 s after the splash's
  start, whichever comes first: a splash that hangs is killed then (the
  console gets the screen back) and the log says so. The splash borrows the screen
  like any app (but not the keys), starts the video and its sound together
  once the mixer is up (2 s at most, then silently), plays to the end,
  holds the last frame at least 0.5 s and until the shell calls
  `initctl.shell_ready`, and fades into the console's text. No key skips
  it: what is typed meanwhile waits in the console (keys typed before
  anyone listens go to the first to open the keys) and reaches the shell
  once it is up. `run splash` from the shell takes the keys, and there a
  key skips it.
- **The timer check** (every CPU's ticks counted over 1 s): the test,
  benchmark and regression entries run it before anything else; a plain
  boot runs it in a kernel thread next to user space, so the second is not
  spent before init starts (its lines come in the log when it ends).

## Memory

- **PMM**: buddy allocator (orders 0-10, up to 4 MiB) with a DMA32 zone below
  4 GiB; allocations prefer the normal zone. `struct page` (32 bytes) lives in
  a **vmemmap** indexed by PFN, backed in 2 MiB chunks; chunks over pure MMIO
  holes stay unmapped (a buddy block never spans two chunks). Physical page 0
  is never handed out. An early bump allocator (top-down) builds the first
  page tables and the vmemmap.
- **Per-CPU page stashes**: with 28 CPUs allocating at once a global buddy
  lock collapses ([BENCH.md](docs/BENCH.md)), so each CPU keeps up
  to 64 free single pages, refilled and drained 16 at a time under one
  acquisition of the buddy lock. They hold the normal zone only (DMA32 when
  there is no memory above 4 GiB); DMA32 and multi-page requests go straight
  to the buddy lists. Stashed pages count as free in `pmm_stats`. Before an
  allocation fails, every stash is drained (stolen under its own lock, no
  IPIs, so it works with spinlocks held); below 8 MiB free the stashes are
  bypassed. Details and races at the top of `kernel/mm/pmm.c`.
- **The vmap area** holds kernel stacks, kernel mappings of VMOs
  (`vmo_map_kernel`) and MMIO. Ranges come first-fit from an address-ordered
  free list, else from the top; a range given back (after its unmap and TLB
  shootdown) merges with its free neighbours and folds into the top when it
  reaches it. Page tables made for a range are never freed but are reused
  with it, so they grow only with the area's high-water mark, not with how
  often things are mapped. Every range ends with a guard page that is never
  mapped, so every mapping has an unmapped page on both sides.
- **Kernel stacks**: `kstack_alloc` (panics) / `kstack_alloc_try` (NULL) map a
  stack in a vmap range; `kstack_free` unmaps it (TLB shootdown), frees the
  pages and gives the range back, so thread churn neither grows the vmap
  area nor strands page tables.
- **Loader memory** (Limine's stack, tables, and the code the parked APs spin
  in until INIT resets them) is reclaimed once every AP has started.
- **The AP trampoline's page**: one page of usable RAM in [64 KiB, 640 KiB),
  the highest, is taken out of the memory map right after the early
  allocator starts (`pmm_early_alloc_low`) and never given back; after the
  startup it holds a halt stub.
- **The stored kernel's region** (32 MiB below 4 GiB by default,
  `crashkernel=<MiB>`) is taken out of the memory map before the PMM
  starts and typed `BOOT_MEM_FOREIGN`: RAM nobody may map as MMIO, but not
  the PMM's, not in the HHDM and in no other mapping
  ([Kexec: reboot and panic](#kexec-reboot-and-panic)). After a kexec the
  previous kernel's crash record and log ring (`BOOT_MEM_CRASH_LOG`) stay
  out of the PMM until they are read at boot, then join it.
- **VMM**: own 4-level tables (no dependency on the loader's). Kernel image
  mapped per section (text RX, rodata R, data RW+NX), HHDM with 1 GiB/2 MiB
  pages for RAM only (write-back), framebuffer write-combining via PAT index 5.
  W^X holds for every alias of a page, including the HHDM view of the kernel
  image. All 256 kernel-half PDPTs are created up front so every
  address space can share PML4 entries 256-511. Layout:
  `ffff800000000000` HHDM, `ffffc00000000000` vmemmap,
  `ffffd00000000000` vmap (kernel stacks, VMO mappings and MMIO, with guard
  pages),
  `ffffffff80000000` kernel image.
- **Heap**: `kmem_cache` slabs (header on-slab, pages tagged `PG_SLAB`) with
  kmalloc classes 16-2048; larger requests take whole buddy blocks.
  **Per-CPU magazines**: each CPU keeps up to 16 free objects per
  cache in front of the cache lock, refilled and drained 8 at a time, in the
  style of the page stashes (flag lock, interrupts off; order magazine ->
  cache -> pmm). A refill never creates a slab while a partial one exists,
  and `pmm_stats` drains every magazine first, so page counts (the ktest
  leak check) stay exact. An allocation that finds no page for a new slab
  drains every magazine and retries. Switch `heap_percpu`, boot
  `nokmcache`. Details at the top of `kernel/mm/heap.c`.
- **VMO** kinds:
  - *paged*: sparse 3-level page tree, pages committed on demand, up to 64 GiB;
  - *contiguous*: one buddy block, up to 4 MiB, committed at creation;
  - *DMA32*: like contiguous, below 4 GiB;
  - *physical*: a fixed physical range (MMIO, framebuffer), never freed.
  Contiguous and DMA32 VMOs need a `dma_cap` handle at the syscall layer.
  Guards: a pinned or kernel-mapped range can't be decommitted or cut off by
  a shrink (`ERR_BAD_STATE`); pins hold references on both the VMO and the
  `dma_cap`.
- **VMAR**: one flat address space per process, no nested VMARs. Maps
  VMOs with R/W/X; W^X always, and an executable mapping needs `RIGHT_EXEC`
  on the VMO handle. User mappings are tracked in a per-VMO reverse map, so
  decommit and shrink unmap the pages from every address space (pins, which
  are DMA, still block them). Lock order: address-space region lock (a
  sleeping mutex) above `vmo`, page-table lock (a spinlock) below it.
- **DMA**: `vmo_pin` needs a `dma_cap`. The IOMMU goes behind the same API.
- **TLB**: a kernel unmap does a synchronous range shootdown to all CPUs (a
  full flush above 64 pages). User address spaces keep a mask of the CPUs
  running them, so a shootdown interrupts only those, and a "gather" frees
  pages only after the flush.
- **PCIDs** (`kernel/arch/x86_64/pcid.c`): each user address space has a
  never-reused id and a TLB generation; each CPU has 8 PCID slots (PCID 0 is
  the kernel's tables) remembering which address space they hold and the
  generation they last flushed at. A load whose slot is current keeps the
  entries (CR3 NOFLUSH), otherwise it flush-loads that PCID. Invalidation:
  `gather_note` bumps the generation before reading the active mask, a
  switch-in sets its active bit before reading the generation, so either
  the CPU is shot down while it runs the address space or it flushes on its
  next load; CPUs that only ran it earlier are never interrupted. Kernel
  entries are global (PCIDs require PGE); INVPCID is not used. Switch
  `pcid_set`, boot `nopcid`. QEMU's TCG has no PCIDs: the PC is the only
  place this runs for real. On Alder Lake and Raptor Lake CPUs whose
  microcode is older than Intel's fix, INVLPG may leave global entries
  while PCIDs are on (errata ADL063, RPL042), which would break the
  kernel's own shootdowns: there PCIDs stay off (`pcid_decide`; the boot
  log's `pcid:` line says which and why; boot `forcepcid` overrides).

## SMP

- Per-CPU block through `GS`: current thread, preempt count, IRQ depth, held
  locks (for the checker), watchdog state, statistics. Run queues live in a
  static array indexed by CPU. NMI, #MC and #DB check GS themselves, since
  they can land in user mode or in the `swapgs` window.
- **Ticket spinlocks** (`spin_lock_irqsave` where interrupt handlers also
  take the lock). Holding one disables preemption. A lock's *name* is its
  class for the lock-order checker (`kernel/debug/lockdep.c`), which records every
  "A held while taking B" pair in a 256x256 bit matrix and panics on the first
  acquisition that closes a cycle (ABBA), on taking a lock twice, on nesting
  two locks of the same class (unless taken with `spin_lock_nested(subclass)`),
  on a class used both in interrupt handlers and with interrupts on, and on
  any lock spinning for 5 s (naming the holder). Always on, with a
  lockless fast path (edges and IRQ flags only ever get set, so only a new
  edge takes the graph lock); it tracks sleeping mutexes too.
- IPIs: reschedule, cross-CPU calls (`smp_call_on/others/all`, which refuse
  to run with interrupts off), TLB shootdown, and NMI halt of all other
  CPUs on panic.
- **Watchdog**: each CPU checks the next one's tick count once a second; a
  CPU stuck with interrupts off for 5 s gets an NMI and panics with its own
  registers and backtrace.
- Log lines carry `[seconds.micros]` timestamps. The log lock is always taken
  with interrupts off, so interrupt handlers may log; hot handlers (the LAPIC
  error handler) just count instead.
- **Serial output** (`kernel/dev/serial.c`): klog copies into a 64 KiB
  transmit ring; COM1's IRQ 4 (through the I/O APIC, MADT overrides
  honoured) goes to CPU 0, whose handler refills the 16-byte FIFO. Writers
  kick it by enabling the transmit-empty interrupt; a full ring drops and
  counts (a RESULTS line says so). CPU 0's tick rescues a stalled
  transmitter; if no interrupt ever comes, output goes back to synchronous.
  The panic path flushes the ring synchronously and stays synchronous.
  Switch `serial_async`, boot `noserialirq`.
- **Per-CPU access rule**: `this_cpu()` is two instructions (load the struct
  pointer, then the field), so from preemptible code a thread can migrate in
  between and read another CPU's data. Preemptible code uses single
  GS-relative instructions (`current_thread()`, `preempt_disable/enable`);
  everything else calls `this_cpu()` only with preemption or interrupts off.
- A wakeup that lands before the thread has switched out just marks it
  running again (as Linux does); waiting for it to switch out could deadlock
  when the waker is an interrupt on that thread's own CPU.
- Timekeeping: TSC measured against the HPET (then ACPI PM timer, CPUID 15h,
  loader estimate; all printed for comparison). LAPIC timer in TSC-deadline
  mode where available, else in APIC one-shot count mode (QEMU's
  TCG); `nodeadline` forces the periodic fallback, which is calibrated as
  the median of 5 bracketed runs. In the one-shot modes each CPU's timer is
  re-armed after every interrupt for the earlier of its next 100 Hz tick
  (an absolute TSC deadline, no drift) and its earliest sleeper (per-CPU
  one-shot timers, see Scheduler). Tickless idle is not built yet.
- APs: the kernel starts them itself, on every boot, with the Intel SDM's
  INIT-SIPI-SIPI (`kernel/arch/x86_64/apboot.c`): INIT to each CPU of the
  boot list by APIC ID (never a broadcast), 10 ms, SIPI to each, 200 us,
  SIPI again, 200 us, so all start at once (about 11 ms in all). Each AP
  runs a real-mode trampoline (`kernel/arch/x86_64/trampoline.S`) 16 -> 32 ->
  64-bit on a transition page table below 4 GiB (the trampoline page
  identity-mapped, plus the kernel half), loads the kernel's CR3, finds its
  slot by its APIC ID (CPUID) and moves onto the struct cpu the BSP
  prepared (64 KiB guard-paged stack, own GDT/TSS with guarded IST stacks).
  It sets NX/WP/PGE/PAT, its per-CPU state, its APIC (in the BSP's mode),
  then **claims** its start (a compare-and-swap); the BSP gives up on a CPU
  after 1 s with the same compare-and-swap and sends it INIT, so a CPU that
  comes late parks itself and changes nothing shared; the CPUs that came up
  are then numbered densely, so one that did not leaves no hole in
  `cpus[]`. Each AP compares its
  microcode, MTRRs and TSC_ADJUST with the BSP's (and loads the BSP's MTRRs
  if they differ); the boot log says if anything differs. The boot word
  `smp=loader` has Limine start them instead (it parks them; `boot_start_cpu`
  releases each), kept for troubleshooting. After a kexec there is no loader: the kernel's
  startup is the only way. Design and reasons: [M8.5-AP-STARTUP.md](docs/history/M8.5-AP-STARTUP.md).
  Loader-reclaimable memory is freed only once every AP is online.
- Topology per CPU: P-core/E-core from CPUID 1Ah, core/thread ids from
  CPUID 1Fh/0Bh; the report says whether Hyper-Threading is on.
- ACPI tables can live in firmware-reserved memory the HHDM skips, so they
  are read through `acpi_map`, which maps pages on demand.

## Objects and handles

- Every object embeds a `kobject`: type, refcount, handle count, signal bits,
  observers (waiters and port bindings). The last reference is dropped
  through an iterative teardown (a per-CPU pending list in
  `kernel/object/object.c`), so destroying a channel full of channels never
  recurses.
- Per-process handle table, 65,536 slots. A handle value is
  `(slot + 1) << 15 | generation` (17 + 15 bits; 0 is never valid); freed
  slots are reused FIFO so a stale value takes a long time to come back.
- Rights: `READ WRITE EXEC MAP DUPLICATE TRANSFER SIGNAL WAIT INSPECT
  MANAGE RESIZE`. `RIGHT_SAME` is only a sentinel for duplicate/replace ("keep the
  rights"). `MANAGE` (jobs only): change limits, kill. `RESIZE` (VMOs, with
  `WRITE`): change the size, decommit; a service that maps a buffer it
  shares hands the other side a handle without it.
- Sending handles uses **in-transit slots**: `handle_take` reserves the slot,
  then `handle_commit` (sent) or `handle_untake` (put back under the same
  value). A failed send never loses a handle.
- Types (`kernel/include/jam/object.h`): `event timer channel port vmo
  dma_cap process thread interrupt resource vmar job klog serial screen`.
- **Processes** (`kernel/object/process.c`): a handle table, an address
  space, a job and user threads (`thread` objects wrapping scheduler
  threads). NEW (the parent maps the program, makes a thread) → RUNNING
  (`process_start` moves the startup channel into the child) → DYING →
  DEAD (`SIG_TERMINATED`). **Kill** (`process_kill`, `process_exit`, a
  fatal fault, or the last thread leaving): every thread is cancelled
  (`thread_cancel`); one in a cancellable wait gets `ERR_CANCELED`, one in
  user mode stops at its next kernel entry. Each thread drops its own
  address-space reference on the way out; the last one closes the handle
  table, drops the process's address space and job reference and only
  then signals `SIG_TERMINATED`, so a waiter sees everything already given
  back (and closing the job then destroys it at once). A fatal fault logs
  `process "x" killed: <why> at rip ..., address ...`. While
  `process_start` makes the first thread, no other thread of the process
  can be started (`starting`), so a failed start leaves it NEW and
  untouched. `debug_write` builds lines under a per-process spinlock but
  prints them with no lock held, 100 lines at once then 50/s per process
  (the rest are dropped and counted). The kernel log marks every line a
  process writes with the process's koid (the kernel's own lines have no
  mark), which `klog_lines` gives a reader: the name in front of a line is
  only what its creator called the process, the mark is who wrote it.
- `resource` is the root of hardware authority (MMIO ranges, PCI devices).
  Userboot hands init the root; init passes a PCI slice to devmgr, which
  gives each driver only its own device, BARs and interrupt.
- **Jobs** (`kernel/object/job.c`): every process belongs to a job; jobs form
  a tree at most 32 deep (`JOB_MAX_DEPTH`; deeper `job_create` fails
  `ERR_OUT_OF_RANGE`), so a charge walks at most 32 levels. Four counters,
  each with an optional limit, and between them every piece of kernel
  memory user code can make the kernel hold is charged to some job:
  - **pages**: committed VMO pages and the VMO's own mid/leaf table pages
    (the job of the process that created the VMO; a commit is charged
    before any table is built for it, so a refused one builds nothing); a
    process's address space: PML4, every user page-table page (charged
    before it is allocated) and a page per 16 mappings (the process's
    job); 17 pages per running user thread (64 KiB kernel stack + XSAVE
    area);
  - **handles**: handle-table slots in use, plus one unit per small object
    that can outlive its handles: each job (charged to its parent), each
    process (until it is torn down) and each VMO struct. So a unit stands
    for at most `JOB_OBJECT_BYTES` (1 KiB) of kernel memory;
  - **threads**: live threads;
  - **message bytes**: channel messages plus 1 KiB per carried handle (to
    the SENDER's job for as long as the message exists), port user packets
    and port bindings plus 1 KiB for the watched object (to the job that
    queued/bound them).
  A charge adds to the job and every ancestor or to none (lock-free), so a
  child job can never use more than its parent has left. Over the limit:
  `ERR_NO_MEMORY` (pages, message bytes) or `ERR_NO_RESOURCES` (handles,
  threads); a page fault that can't be paid for kills the process. The
  root job (`userboot_root_job`) leaves the kernel 32 MiB (or a quarter of
  memory), and carves the handle budget (1/16 of the rest, at most 16384
  units) and the message budget (1/8, at most 64 MiB) out of the page
  limit, so the three can't together exceed free memory minus that
  reserve. **Rights**: `job_create` gives the creator `JOB_RIGHTS`
  (including `MANAGE`: `job_set_limit`, `job_kill`); a program is handed
  its own job (`SR_JOB`) with `JOB_RIGHTS_OWN`, without `MANAGE`, so it
  can start processes and child jobs in it but can't lift the limits its
  parent set (init gets the root job the same way). **Kill**: `job_kill`
  kills every process in the job and all jobs below it (orphans too),
  returns once each has been taken off its job's list, and the killed jobs
  take no new processes or jobs. That happens in the teardown just before
  the process drops its address space, so the address space's pages can
  still be on their way back for a moment after `job_kill` returns (a test
  that checks the job is empty waits for them, bounded); init uses it when a program times out, userboot when
  init does. Each job lists its child jobs and live processes under its
  object lock for this. Any allocation a syscall can reach returns an
  error instead of panicking (thread structs and stacks, handle tables,
  page tables for kernel stacks, the timer service, `smp_call_others`).

## IPC

- **Channel**: bidirectional endpoint pair; message = up to 64 KiB of bytes +
  up to 64 handles; sending a handle moves it. At most 1024 messages queue on
  one endpoint (then `ERR_SHOULD_WAIT`), and every message's memory is
  charged to its sender's job until it is read or dropped. Cycle rule: you
  can't send an endpoint of the channel you are writing to, its peer, or an
  endpoint whose own queue already holds an endpoint.
- **`channel_call`**: write + wait for the reply whose first 4 bytes (the
  txid) match. The kernel stamps the txid; only the calling thread gets the
  reply; a reply arriving after a timeout stays queued as a normal message;
  closing your own endpoint mid-call returns `ERR_CANCELED`; calling on a
  closed endpoint returns `ERR_BAD_STATE`. The request hands the server
  the caller's CPU (wake-affine, see [Scheduler](#scheduler)).
- **Port**: bind many handles and wait on all of them; matching signals queue
  packets. `ONCE` bindings fire once; `PERSISTENT` ones stay and coalesce
  into one queued packet with a count. Limits: 4096 user packets, 4096
  bindings per port; ports can't be bound to ports. The lock rank is
  documented in `kernel/include/jam/port.h`. **Interrupts are port
  packets**: a driver binds its interrupt object `PERSISTENT` to a port.
- **`object_wait_one`** for simple waits.
- **Protocols** are written in a small IDL (`abi/idl/*.idl`), turned into C
  structs, client stubs and server dispatch by `tools/genidl.py`
  (`drivers/include/idl/`). Today: `null`, `edu` and `idltest` (tests), `usbbus` and
  `usb` (usb-bus to devmgr and to class drivers), `input` and `console`,
  `storage` and `block` (usb-storage to devmgr and to a filesystem),
  `fs` and `file` (a filesystem to programs), `fsctl` (devmgr stopping a
  filesystem; its counters, for tests), `initctl` and `logctl` (init's and logd's control
  channels), `hda` (the HD Audio driver), `audio` and `audioctl` (the
  mixer), `netdev` (a network driver to netstack: rings and events),
  `netctl` (netstack's control channel), `net` (UDP and TCP sockets and ping for
  programs, `/svc/net`), `dns` (the resolver, `/svc/dns`). devmgr's own
  protocol is still written by hand (`user/include/devmgr.h`). Planned:
  `power`. Bulk data
  (disk blocks, packets, file contents) moves through a shared VMO ring;
  messages carry offsets. Every protocol defines how a client reconnects
  after `PEER_CLOSED` (the server restarted).
- **Generated calls in three shapes** (`tools/genidl.py` has the
  details): the blocking `<proto>_<method>` / `_until`; a call that
  doesn't wait (`<proto>_<method>_send` with a txid of the caller's own,
  the reply read off a port-bound channel with `idl_reply_read` and
  decoded by `<proto>_<method>_result`); and on the server, a method
  marked `later` may keep its request (`struct idl_txn`) and answer it
  after its handler returned, with `<proto>_reply_<method>`. One channel
  end uses blocking calls or calls that don't wait, never both (the
  kernel's txids could match the caller's).

## How a service waits

A service or driver that serves several clients runs one loop on one
port, so while it handles one request every other client waits behind it.

- **The rule: a loop that serves several clients never blocks inside a
  request.** No blocking call to another process, no wait for a process
  to end, no sleep longer than its own hardware needs. Instead it
  - **answers later**: keeps the request and replies when the answer is
    there (a `later` method and `<proto>_reply_<method>`; hda's
    `wait_period` and devmgr's `MOUNTS` waiters do this by hand);
  - **calls without waiting**: sends the request
    (`<proto>_<method>_send`), goes on serving, and takes the reply from
    its port like any other event (`idl_reply_read`,
    `<proto>_<method>_result`);
  - or runs a job of several steps as a **cooperative task**
    (`<jam/task.h>`, libos): straight-line code that gives the loop back
    at each wait (`task_wait`) and is run again when its deadline passes
    or the loop kicks it. usb-bus runs every port and every device this
    way, so a slow device holds up only itself.
- **Allowed:** short waits with a deadline on the loop's own hardware (a
  register poll, a command the device answers within milliseconds:
  [CODING-GUIDE](CODING-GUIDE.md#every-hardware-wait-is-bounded)), and
  blocking calls in code that serves nobody: a program, a shell command,
  or a thread of its own that serves nothing else.
- **Data at packet or sample rate** goes through shared rings with an
  event, not a call each (the mixer's streams, `netdev`, sockets).
- **Not followed everywhere yet:** devmgr's and init's loops still make
  blocking calls of up to 2 to 25 s (listed in
  [ARCH-CHECK](docs/history/ARCH-CHECK.md#0-and-8-service-loops-that-wait-on-one-thing-at-a-time));
  they move to these tools one at a time. The network's services and
  drivers follow the rule from the start
  ([Networking](#networking), "the service-loop rule, as applied").

## Scheduler

- Per-CPU run queues, 32 priority levels (31 most urgent), round-robin
  within a level, 20 ms slices on the 100 Hz tick.
- **Placement** (hybrid order): an idle P-core whose HT sibling is idle too
  > an idle E-core > the idle HT sibling of a busy P-core > least loaded
  (ties to P-cores, then the thread's last CPU); within the
  idle classes the last CPU wins. Without hybrid cores it is "whole idle
  core > idle sibling > busy". Placement reads only the CPUs a thread may
  use, from a read-mostly topology table (sibling, core type) and each run
  queue's own line (`nr_ready`, `busy`, `cur_prio`), never another CPU's
  `struct cpu`: scanning every CPU's struct costs ~2 x 28 cache-line reads
  per call round trip. A CPU is marked busy before its next thread leaves
  the queue, so a CPU starting work never reads as idle. Idle CPUs steal
  the best waiting thread from busy ones (two run queue locks, always lower
  CPU index first); a stealer that is only half a core (its sibling busy)
  or an E-core sends the thread on to a better idle CPU if placement finds
  one, so a steal lands where placement would put the thread. 256-bit
  affinity masks; `thread_create_on` sets the mask before the thread first
  runs. Switch `sched_place_order`, boot `noplaceorder`.
- **Anti-starvation**: once a second each CPU boosts threads that have waited
  over 1 s to priority 30 for one slice. Priority 31 is above the boost, so
  real-time threads can still starve others by design. Each thread has a
  priority ceiling (`thread_create_capped`, `thread_set_priority_cap`;
  `PRIO_MAX` for kernel threads); user threads start at 16
  with ceiling `PRIO_USER_MAX` = 24; `thread_set_priority` above it fails
  with `ERR_ACCESS_DENIED` (a capability may allow more later).
- **Wake-affine hand-off**: a waker that is about to block for the
  thread it wakes (`thread_wake_sync`, or `thread_set_wake_sync` around a
  send whose wakeup happens inside an observer) places it on its own CPU if
  allowed and nothing else is queued there, else on its idle HT sibling,
  else as usual. `channel_call` uses it for the request, and a server's
  reply uses it when nothing else is queued for the server.
- **Client/server pairs**: every wake from thread context records
  the waker in the wakee; two threads that each woke the other twice
  running are a pair, and a plain wake (the waker keeps running) puts the
  wakee on the waker's idle HT sibling, so the pair shares a core. E-cores
  have no sibling. Switch `sched_affine_pair`, boot `noaffinepair`.
- **Spin before idle**: an idle CPU polls its run queue and
  `need_resched` for 10 us (pause, interrupts on) before `hlt`, advertising
  it in `cpu->idle_polling` so `sched_kick` skips the IPI; a Dekker
  handshake (clear flag, fence, recheck / set need_resched, fence, read
  flag) loses no wakeup. Costs full clock for the window after each idle
  entry (0.1% with the 100 Hz tick). Switch `sched_idle_spin_ns`, boot
  `idlespin=<us>` / `nospinidle`.
- Preemptible kernel: switches happen on interrupt exit or when the last
  spinlock is dropped, never with one held (`schedule()` panics if called
  with preemption disabled).
- Switch-safety: the run queue lock is held across `switch_context` and
  released by the next thread; `on_cpu` stays set until a switched-out
  thread's registers are saved, and whoever picks it waits for that. This
  covers a thread being woken while it is still switching out.
- Kernel threads: `thread_create/exit/join/yield/sleep`, `thread_block(lock,
  deadline)`, wait queues with a condition-variable style
  `waitqueue_wait(wq, lock)`, sleeping mutexes (a waiter that has waited
  1 ms gets the mutex handed to it, so none starves). Up to 256 exited
  threads' stacks are cached for reuse; stacks beyond that are freed (by
  the next thread creation or exit, since the reaper runs with interrupts
  off and can't shoot down TLBs). **Sleepers** (every wait with a
  deadline, and so the timer-object service) go on a deadline-ordered
  queue of the CPU they block on, whose LAPIC timer is armed for its head.
  Only the owning CPU adds and arms; removal from anywhere under that
  queue's lock. Switch `lapic_oneshot`, boot `nooneshot` (then each CPU's
  tick expires its own queue, 10 ms resolution).
- User FPU state: XSAVEOPT where available, and no XRSTOR when the
  CPU's registers still hold the incoming thread's state (last restored
  here, not restored elsewhere since). Switch `fpu_opt`, boot `nofpuopt`.
- **Cancellable waits**: `thread_cancel(t)` sets a per-thread flag for
  good and wakes t. The cancellable waits (`thread_block_cancellable`,
  `waitqueue_wait_cancellable`, `thread_sleep_cancellable`,
  `mutex_lock_cancellable`, and through them `object_wait_one`, `port_wait`
  and `channel_call`) return `ERR_CANCELED`; plain waits just see a spurious
  wakeup. A wait that is also satisfied still succeeds, and a cancelled
  port/mutex waiter passes its wakeup on. The waiter moves itself
  BLOCKED -> RUNNING by CAS, and `thread_wake` does the same on its on-CPU
  path, so neither can overwrite a state the other set. Killing a process
  blocked in `channel_call` on a hung server relies on this.
- Every optimisation above that has a switch can be turned off at boot, and
  the benchmark measures its lines both ways in one run
  ([BENCH.md](docs/BENCH.md)).

## Drivers and services

| Component | Uses | Provides | Built |
|---|---|---|---|
| devmgr | the PCI resource | enumeration, driver binding, BAR/MSI/DMA hand-off, supervision, the `usbbus` service to trusted clients; every disk's filesystem services and the mounts ([Storage](#storage)) | yes |
| usb-bus | its PCI device (xHCI) | one `usb` channel per interface; hubs are handled inside it (bus topology, not a class device); every port's attach and every device's requests in a task of their own, so a slow device delays only itself (libos's cooperative tasks, `<jam/task.h>`: [How a service waits](#how-a-service-waits)) | yes |
| hid | a `usb` interface | `input` events (boot keyboard, keyboard layout; mouse in boot or report protocol) to the console | yes |
| console | the framebuffer and COM1's output (the root's `RIGHT_ROOT_SCREEN` and `RIGHT_ROOT_SERIAL_OUT`, which no other program holds), `input`, the kernel log | `console`: a text terminal (UTF-8: ASCII and the Latin letters drawn), the kernel log or its notices ([Debugging](#debugging)), and lending the screen to a program | yes |
| serialin | COM1 input | an `input` source (QEMU tests; a spare keyboard if USB breaks) | yes |
| usb-storage | a `usb` mass-storage interface (Bulk-Only Transport; UAS later) | `storage` to devmgr, a `block` channel per partition | yes |
| fat | one partition's `block` channel | `fs` and `file` for one volume (FAT32 + long names, read/write, on FatFs); `fsctl` to devmgr | yes |
| bootfs | the bootfs image VMO | `fs` and `file` for `/boot`, read-only | yes |
| logd | the kernel log, `/data` | each boot's log as a file on the stick | yes |
| hda | its PCI device (Intel HD Audio) | `hda` ([Audio](#audio)) | yes |
| mixer | `hda`, through the sound cards' devmgr device channels | `audio` and `audioctl`: every program's sound mixed into the one output, and query channels to the sound card ([Audio](#audio)) | yes |
| music | `audio`, the namespace | `music`: a folder played in shuffle in the background ([Audio](#audio)) | yes |
| rtl8125 | its PCI device (the PC's Realtek RTL8125B: MSI-X, DMA rings) | `netdev`, every frame in the network mode: tagged with the VLAN, or untagged ([Networking](#networking)) | yes (on every boot but "Jam OS (no network)", `vlan=off`) |
| e1000e | its PCI device (QEMU's Intel 82574L, for the tests) | `netdev`, the same rules | yes |
| netstack | lwIP (IPv4, ARP, ICMP, UDP, TCP; single-threaded, NO_SYS), the network cards' device channels | `netctl` (the address, the DHCP socket), `/svc/net` (UDP and TCP sockets and ping for programs) | yes |
| dhcp | `netctl` | the address, when the settings have no `net.address` | yes |
| dns | `/svc/net-sys` | `/svc/dns`: names to IPv4 addresses | yes |
| netlog | the kernel log, `/svc/net-sys` | each boot's log over UDP to the Mac | yes |
| power | uACPI | shutdown, reboot, power button, later S3 | no |

uACPI will live in the kernel; everything else is a process.

Rules for userspace drivers:

- The kernel's PCI core programs MSI/MSI-X. A driver never gets raw ECAM
  access, and its BAR mapping never includes the MSI-X table page.
- A `dma_cap` is bound to one device (PCI bus/device/function). `vmo_pin`
  returns *device addresses* (physical until the IOMMU arrives).
- **Safe rebind**: the newest `dma_cap` of a function is its *current*
  one; making it turns Bus Master Enable off, and only it turns it back on
  (`dma_cap_bus_master`, `drv_dma_bus_master`), which a driver does only
  after quiescing its device (reset it, or see its DMA engine idle), so a
  transfer a dead driver left queued never runs with the new driver's bus
  mastering. Nobody else turns it on (`pci_bus_master` only turns it off).
  Closing the current cap clears Bus Master Enable (an older cap's close
  leaves it alone); pins still held then are *quarantined*, not freed
  (Fuchsia's BTI quarantine): the pages stay, charged to their VMO's job,
  until 1 s after the function's next driver turned bus mastering on, or
  30 s if none does; a page the device wrote meanwhile is logged.
- MSI/MSI-X and MMIO only: no port I/O and no INTx for userspace drivers.
- **Supervision**: devmgr restarts a driver process that dies
  unexpectedly (crash, kill, error exit; an exit 0 by itself means the
  driver is finished): backoff 100 ms, doubling per restart within the
  last 60 s up to 5 s; the 6th death within 60 s gives up (log + RESULTS
  line). A driver that ended by itself must leave its job empty; one
  that hasn't quite yet (a request it sent still queued at the server it
  called, its pages being given back on another CPU) is looked at again
  for 2 s before it counts as not ended cleanly. A restart is a bind from scratch, i.e. the safe-rebind path: the
  function woken to D0, a new `dma_cap` (bus mastering off until the new
  driver has quiesced the device; the dead driver's pins stay quarantined
  meanwhile), a new interrupt object. A driver's hardware handles are not
  transferable (no `RIGHT_DUPLICATE`/`RIGHT_TRANSFER`, handed over with
  `channel_write_rights`), so nothing of the device outlives its job.
- **Reconnect rule**: a client whose call fails with `PEER_CLOSED` asks
  devmgr for the service again (`GET_SERVICE`) and retries; from the
  moment the driver died devmgr hands out the channel its restart will
  serve, and calls on it wait for the new driver. Drivers keep no state
  across a restart; each protocol's IDL says what a client must set up
  again (`input` and `console` define theirs).
- **Kill**: the shell's `kill <name>` asks init (`initctl.kill`), which
  kills its own services itself and asks devmgr to kill what devmgr runs:
  a USB class driver, a filesystem service or a PCI driver (usb-bus, hda),
  found by name through devmgr's bindings. Whoever supervises it starts it
  again. The shell refuses to kill init.
- **Authority**: devmgr has a query channel (look things up), a control
  channel (change bindings), the ESP channel (init's alone: the boot
  stick's ESP made writable for `update -w`, which no other channel may
  ask, [Storage](#storage)) and *device channels*, each scoped to one
  device and made on the control channel (`DEVMGR_DEVICE_CHANNEL` in
  `user/include/devmgr.h`): a device channel answers about its own device
  alone (its service, its driver, its supervision), and while a device
  has one the query channel refuses its service (`ERR_ACCESS_DENIED`).
  Holding the channel is the right to use the device; there is no flag
  and no role. init decides who gets which: every HD Audio controller's
  to the mixer (each has one output stream, the mixer's), and every
  network card's to netstack. Console clients have a level fixed on their channel
  when it is made (ADMIN, SHELL, PROGRAM), and a program started from the
  shell gets a PROGRAM channel and nothing of devmgr's.

## Networking

IPv4 with ARP, ICMP, UDP and TCP, through Jam OS's own NIC drivers and a
network stack in a process of its own; no IPv6 yet. The plan,
its stages and the PC's runs are in [M9-PLAN.md](docs/M9-PLAN.md); the
PC's chip and switch port in [HARDWARE.md](docs/HARDWARE.md#the-network).

**Hard requirement: every frame Jam OS sends is in the configured
network mode, and nothing else ever leaves.** The mode is one of:
- **a VLAN** (1..4094): every frame is tagged 802.1Q with that VLAN, and
  nothing is ever sent untagged or on another VLAN; incoming untagged and
  other-VLAN frames are dropped. **The owner's builds are VLAN 21** (his
  network must not see Jam OS traffic elsewhere: [HARDWARE](docs/HARDWARE.md#the-network));
- **untagged**: every frame is sent untagged and never one with a tag
  (bytes 12-13 are never 0x8100, 0x88a8 or 0x9100), so Jam OS can't put
  itself on a VLAN; incoming tagged frames are all dropped, priority tags
  (VLAN 0) too. A build of the public tree is untagged by default, so it
  works on an ordinary network;
- **off**: the NIC stays down (fail closed), as with any word that isn't
  a mode.

The mode is chosen at build time, with the default for a boot without a
`vlan=` word, and can be chosen at boot: see "The mode: one place" below.
A change that could transmit comes with a test proving no frame outside
the mode can leave (an untagged one in a VLAN mode, a tagged one in the
untagged mode).

```
 the shell (net, ping, host, update), programs     /svc/net  /svc/dns
 dhcp (netctl) · dns · netlog · sntp · update (bin/update)
 netstack: lwIP, one loop                           /svc/net, netctl
 ───── netdev: two ring VMOs and two events per session ─────────
 drv/rtl8125 (the PC) · drv/e1000e (QEMU)           tag, check, DMA
```

**The mode: one place.** The build chooses the default: the Makefile
reads `JAMOS_VLAN` from a git-ignored `local.mk` at the top of the tree
(`local.mk.example`: `JAMOS_VLAN := 21`, or `none`), or from the make
command line, never from the environment; without one the default is
untagged. It goes into the kernel as `JAMOS_NET_DEFAULT` (kernel/main.c),
`make` says which it built (`network default: VLAN 21 (local.mk)`), and
the boot image's `build.txt` records it (`net vlan21`, `net untagged`).
At boot the kernel reads the boot word: `vlan=<1..4094>` a VLAN,
`vlan=none` (or `vlan=untagged`) untagged, `vlan=off` off, no word the
build's default, and anything else (a bad value, two words that
disagree) off. It logs the mode and where it came from (`network:
VLAN 21 (the build's default)`), and passes it to init as `vlan=<id>` or
`vlan=none`, init to devmgr, and devmgr to every network driver as an
argument; kexec keeps the word, all three forms, so `reboot`, a panic and
`update` come back in the same mode. netstack is never asked: a word from
netstack can't change the mode. `net` shows the driver's.

**The owner's PC never changes mode by accident.** A boot with no word
takes its build's default, so two guards keep an untagged build off the
owner's stick: init's update check refuses a fetched build whose
manifest's `net` (its `build.txt`'s) differs from the running build's,
unless `update -f`; and `make flash` (`tools/flash-usb.sh`) prints the
build's default and asks before it writes an untagged one (or one that
doesn't say).

**The drivers** (`drivers/rtl8125` for the PC's RTL8125B,
`drivers/e1000e` for QEMU's 82574L) apply the mode in software, below
netstack, so a netstack that is buggy or taken over still can't send a
frame outside it: in a VLAN mode no untagged frame, in the untagged mode
no tagged one. The checks are pure functions in one header both use,
`<jam/netframe.h>` (the mode is one number: a VLAN id, or
`NETFRAME_MODE_UNTAGGED`, 0x1000, which no 12-bit VLAN id can be):
- **Transmit**: netstack's frame (14 to 1514 bytes, untagged) is copied
  out of its ring slot, then copied again into the driver's own DMA buffer
  with the tag (TPID 0x8100, priority 0, the VLAN) after the addresses,
  padded with zeros to 64 bytes (`netframe_tag`); a frame whose EtherType
  is already a tag (0x8100, 0x88a8, 0x9100) is refused, so no frame leaves
  with a tag netstack chose. The copy is checked once more
  (`netframe_tx_check`: bytes 12-15 exactly the tag) right before its
  descriptor goes to the NIC; netstack can't see that buffer, so changing
  its ring afterwards changes nothing. The NIC's own tag insertion is never
  used (one wrong descriptor bit would be an untagged frame). In the
  untagged mode the frame is copied as it is, padded with zeros to 60
  bytes (`netframe_plain`), a frame whose EtherType is a tag's is refused
  the same way, so netstack can never choose a VLAN, and the last check
  (`netframe_tx_check_plain`) is that bytes 12-13 are no tag's TPID.
  `netframe_tx_copy` and `netframe_tx_final` pick by the mode, and a mode
  that is neither sends nothing.
- **Receive**: the NIC's tag stripping is off; `netframe_rx_check` keeps
  only 802.1Q frames on the VLAN (any priority) and takes the tag off;
  untagged, priority-tagged, other-VLAN and QinQ frames are dropped and
  counted by reason. In the untagged mode `netframe_rx_check_plain` keeps
  only untagged frames (14..1514 bytes) and drops every tagged one,
  priority-tagged (VLAN 0) ones too: nothing on an untagged network should
  tag a frame to us, and taking one would be taking a tag.
- A driver reads a frame's length and bytes 12-17, never the addresses or
  the payload: everything else is parsed above it, in processes with no
  `dma_cap`. Flow control is off (pause not advertised, so the NIC sends
  no PAUSE frames of its own), wake-on-LAN is off while Jam OS runs, and
  no firmware tables are loaded.
- **Without a valid mode** (`vlan=off`, or no word from devmgr) a driver
  turns on neither receiver nor transmitter, logs `no VLAN: the network
  stays off`, and ends.

**Machine checks of the rule.** Each driver has one transmit file
(`tx.c`); every function it gives other files starts with a gate (full
mode and a configured network mode: a VLAN, or untagged). For the RTL8125, `tools/checknotx.sh` (in `make
check`, self-tested) holds the transmit registers to `tx.c` and the gate
to the top of every entry, and keeps the listen-only probe's files from
calling it. While it runs, about once a second and after every reap, the
RTL8125 driver compares the chip's own count of frames sent with the
frames it queued; more sent than queued (frames of the chip's own), or a
link that resolved to sending PAUSE, turns the transmitter off, resets
the chip and ends the driver with an error, fail closed
(`drivers/rtl8125/guard.c`); the same comparison is logged at its exit
(`tx check:`). utest's `netframe_*`
tests try every length, tag and edit. In QEMU two separate checks look at
every frame the guest sends, `tools/netpeer.py` and
`tools/pcap-vlan-check.py` (each with `--vlan none` for the untagged
mode: any tag fails), and `tools/net-vlan-test.sh` runs every path that
transmits in one boot, then the same commands on a `vlan=off` boot, which
must send nothing; `tools/net-vlan-test.sh <out> none` runs every path
untagged ([TESTING.md](docs/TESTING.md#area-scripts)).

**netdev: rings, not calls** (`abi/idl/netdev.idl`,
`<jam/netdev.h>`). `info` (MAC, the mode in its VLAN field, MTU, link,
speed, a count of link changes, the chip), `stats` (the driver's counts and the chip's) and
`open`, which gives a session channel, two ring VMOs (transmit and
receive: a header page, then 256 slots of 2 KiB) and two events (one per
waiter). The driver makes the rings in ordinary memory; netstack can map
them but not resize or pass them on. A side signals the other only when
it said it is sleeping: no call per frame. One session at a time; closing
the session channel ends it, and a session whose opener has gone is ended
at the next `open`. The driver treats every count and length netstack
writes as hostile (clamped to the ring, read once, the frame copied before
it is checked); netstack treats the receive ring the same way and ends a
session whose counts are out of range. The server side is one file every
network driver links, `drivers/lib/netserver.c`: the driver plugs in its
transmit path and its counts. A full receive ring, or no session, drops
and counts; the driver never waits for netstack.

**Who reaches the driver.** init claims every network function's devmgr
device channel (class 02 00 00) before it publishes `/svc/devmgr`, and
gives them to netstack alone ([Drivers and services](#drivers-and-services),
"Authority"); a test program reaches a driver only through devmgr's
control channel, which only programs under `user/tests/` may ask for.

**netstack** (`user/services/netstack`) is lwIP 2.2.1 in its NO_SYS mode:
one loop on one port (the receive event, every client's channel, lwIP's
timers as the wait's deadline), so nothing locks. In: IPv4, ARP, ICMP
(echo replies: the PC answers pings), UDP, TCP. Out: IPv6, fragments and
reassembly, IP options, IGMP, and lwIP's own DHCP, DNS and VLAN code. Its
memory is static (lwIP's 1 MiB heap, mostly TCP's unacked bytes, 128
receive buffers, fixed pools: 384 TCP pcbs, 1024 segments); a full pool
drops the frame and counts it. `stack.c` is the only
file that sees lwIP. The calls that may wait (devmgr's GET_SERVICE,
`netdev.info` and `open`, 2 s each) run on a thread of its own that
serves nothing (`connect.c`), which hands the session to the loop; a
session that closes is asked for again, the address and ARP table kept.
It logs state changes only (link, address), never a packet. It serves two
channels, both made once by init, which keeps the server ends across
restarts:
- **netctl** (`abi/idl/netctl.idl`): `set_ipv4` (refuses any address a
  host can't have), `set_dns`, `clear`, `info`, `stats`, `device`, and
  `dhcp_open` (a socket on port 68 that may broadcast and send from
  0.0.0.0). Held by init and dhcp; never published.
- **`/svc/net`** (`abi/idl/net.idl`, libos's `<net.h>`), a channel per
  opener: `iface`, `wait_change` (answers when the address or DNS servers
  change), `counts`, `chip_counts`, `echo` (a ping, built by netstack),
  and `udp_rings(port, tx, rx)`, a socket: a channel of its own
  (`sock_connect`, `sock_state`; closing it closes the socket) and its
  **rings** (`user/include/sockring.h`): one VMO netstack makes and maps,
  a header page, a tx ring the program writes and an rx ring netstack
  writes (4 KiB to 256 KiB each; UDP's 16 and 32 KiB by default), and two
  events, the netdev rings' model. A datagram is a record (16 bytes of
  address, port and length, then at most 1472 bytes); while datagrams flow
  neither side makes a call or a system call per datagram, only a signal
  when the other side said it sleeps. netstack treats the rings as hostile
  (its own counts, the program's clamped, a record's header read once and
  checked, its bytes copied before lwIP sees them; a broken ring is looked
  at again only on the program's next signal), reads a tx ring only while
  the card's ring has room (a full tx ring is the program's backpressure)
  and drops and counts a datagram that doesn't fit an rx ring, so a slow
  reader never holds lwIP's buffers or netstack. A refused record (an
  address a program can't send to, no route) counts in the socket's
  status line, with its reason; the blocking `net_sendto` waits for
  netstack to take its datagram and returns that reason. The VMO is
  netstack's (its pages charged to netstack's job), so it is shrunk to
  nothing once the program's end of the channel closes; a socket whose
  opener went is ended (its status says CLOSED) but kept until then.
  Limits: 32 openers, 16 sockets an opener and 48 in all, 8 requests in
  flight an opener and 64 in all, 2 MiB of ring bytes an opener and 16 MiB
  in all; of each, ordinary programs together get only their share
  (24 openers, 24 sockets, 48 requests, 8 MiB), and the rest is the
  network's own services' (`/svc/net-sys`, below). A program can't send to
  a broadcast, multicast or loopback address, can't bind a port below 1024
  (nor one below 49152 without the listen permission, below), and sends no
  raw packets. A wait set (`<netwait.h>`) waits on many sockets at once.
- **TCP** on the same channels (`user/services/netstack/tcpsock.c` for the
  calls, `tcp.c` for the connections, `stack.c`'s TCP edge for lwIP;
  [M9.5-PLAN](docs/M9.5-PLAN.md#track-c-part-1-built-tcp-inside-netstack)):
  `tcp(address, port, tx, rx)` opens a connection (CONNECTING at once; the
  status line says OPEN, or CLOSED and why), `tcp_listener(port, backlog,
  tx, rx)` listens (only an opener of `/svc/net-listen`, ports from 1024),
  and `accept` on the listener's channel is answered when a connection
  comes (a wait set watches that channel). A connection is a socket like
  a UDP one, but its rings carry a byte stream: SOCKRING_END on the tx ring
  is a FIN after the last byte, on the rx ring the peer's FIN. **The
  receive window is the rx ring's free room**: bytes go into the ring as
  they arrive and are given back to the window only as the program reads
  them, so a slow reader stops its own sender and lwIP holds no received
  byte; bytes go from the tx ring into lwIP only as the peer's window takes
  them (and a segment more), so a peer that stops reading holds no more
  than its window. A listener's connections get their rings when their
  handshake finishes (the bytes that come first wait there), counted
  against the listener's opener. Limits: 64 connections an opener, 256 in
  all (192 for ordinary programs), 4 listeners an opener, 16 in all, a
  backlog of 16 a listener and 128 together (96 for programs); half-open
  connections and the ones netstack let go of have lwIP's lowest priority,
  so a SYN flood fills only its listener's backlog and never takes a
  program's pcb. Initial sequence numbers come from the kernel's random
  source, and a segment whose ACK is for bytes never sent is dropped before
  lwIP (RFC 5961). Closing a connection with bytes unread resets it.
- **`/svc/net-sys`**, the network's own services' reserve: the same
  protocol on a third shared channel (netstack's SR_USER + 3), whose
  openers are counted apart from programs', so no program can take the
  openers, sockets, requests or ring bytes dns, netlog, sntp and
  `bin/update` need. Like the listen permission it is fixed at connect by
  the channel an opener came through: init grants the name to its network
  services, `tools/checkwants.py` only to a program under
  `user/services/` (`bin/update`'s list), and `allow` refuses it for a
  program on `/data`. libos's `net_svc` opens it when the namespace has
  it.
- **`/svc/net-listen`**, the listen permission: the same protocol on a
  second shared channel init makes and publishes (netstack's SR_USER + 2),
  whose openers may also bind a fixed UDP port from 1024 to 49151, where
  servers live and where a peer sends unasked, and listen for TCP
  connections (`listen_may_accept`, `user/services/netstack/listen.h`).
  netstack learns the permission from the channel an opener came through,
  set once at connect, never from anything the program says, so it can't
  be forged: a program has it only if it holds that channel. The shell
  gives it only to a program whose list says `svc net listen` (which
  grants `/svc/net` too; the bare name `svc net-listen` is no want);
  `allow` shows it to the owner as "accepting connections from the
  network" for a program on `/data`, and `tools/checkwants.py` approves it
  for the boot image. Without it a program binds port 0 (netstack picks
  from 49152) or a port of its own picking from 49152 (the resolver's
  random source ports) and still gets every reply on them: UDP can't tell
  a reply from a datagram nobody asked for, so what the permission guards
  is a port someone else could know in advance. Nothing in the boot image
  listens but `bin/wantlisten`, the allow test's program.

**The address.** `net.address = <address>/<prefix> [<gateway> [<dns>
[<dns>]]]` in `/data/etc/settings` is a static address, given to netstack
whenever it starts and whenever `/data` comes; without it the DHCP client
asks for one.

**dhcp** (`user/services/dhcp`) holds a duplicate of netctl's client end
and nothing else; init starts it only without `net.address`. An RFC 2131
client: DISCOVER, OFFER, REQUEST, ACK, renewal and rebinding, the address
cleared when the lease ends; after a restart of its own or netstack's it
asks for the same address again (INIT-REBOOT). One log line per lease. No
ARP probe of the offered address (an ACKed address is taken as free).

**dns** (`user/services/dns`) serves `/svc/dns` (`abi/idl/dns.idl`:
`resolve`, answered when the reply comes, so a slow name holds up only its
own askers) and holds `/svc/net-sys`. Each name in flight has a socket of its
own on a random port with a random id (`os_random`), 16 names at most and
8 askers each; A records only, CNAMEs followed, a cache of 32 names (TTL
at most a day). libos's `dns_lookup` (`<dns.h>`) is the client.

**netlog** (`user/services/netlog`) holds a kernel log reader, `/svc/net-sys`
and, after a panic, the panicked boot's log read-only. init starts it when
`net.host` is set and `netlog` isn't `off`. It sends the log from its
first line (the 4 MiB ring still has the whole boot when the network comes
up) over UDP to `net.host` port 5021 and nowhere else, up to 1400 bytes
of it a datagram with the boot's id and the byte offset; the Mac's acks
move a 64 KiB window, and without them it sends again from the last ack
(after 500 ms, backing off to 30 s). After a panic it also sends the
panicked boot's log. It logs only when its state changes, so its own lines
can't multiply. On the Mac, `tools/netlog-recv.py` writes a file per boot.

**sntp** (`user/services/sntp`) sets the clock from the network (SNTP, RFC
4330; the checks in `ntp.c`, a core with no I/O that utest drives). init
starts it once `/data`'s settings are read, unless `ntp = off`, with
`ntp.server` as its argument, `/svc/net-sys` and `/svc/dns`, and the root with
`RIGHT_ROOT_CLOCK` only: it is the one service besides init and the shell
that may set the clock. Without `ntp.server` it asks the network's gateway
(the DHCP lease's router, which on the owner's network is also its DNS
server), then `pool.ntp.org` if the gateway gives no time. It waits for an
address, then sends up to 4 requests 2 s apart from a port netstack picks,
connected to the server's port 123; each request carries 64 random bits
as its transmit timestamp and nothing of our clock, and only a reply whose
origin is exactly those bits, from a synchronized server (mode 4, version
3 or 4, stratum 1-15, no leap alarm, both timestamps, root delay / 2 + root
dispersion under 1 s, sent after it was received, a round trip under 5 s,
a time from 2026 to 2199) is believed. Anything else is counted and said
in one line, never used: a kiss-o'-death too only if its origin matches.
The time is the server's transmit time plus half the round trip less the
server's own time, at the uptime the reply came, which is what the kernel
keeps. The first time it takes the whole step, but a step over 60 s needs
a second reply, to a second nonce, that agrees within a second. Then it
asks every hour and moves the clock at most 5 s a time (a step cut short
asks again in 64 s), so a lying server can only drag it slowly. A round
with no time backs off from 16 s to 1024 s. It sets the clock with
`WALLCLOCK_NET`, and logs each set with the offset it found
([Time and settings](#time-and-settings)).

**update** (`user/services/update`, `user/services/init/update.c`). The
shell's `update [-n] [address]` takes an offer channel from init
(`initctl.update_offer`) and runs `bin/update` with that channel and
`/svc/net-sys` only. It fetches the manifest, kernel and boot image from the
Mac (`net.host`, UDP port 5022, `tools/update-server.py`: a request names
a snapshot, a file, an offset and a length; 32 in flight; the server keeps
no state per client) and offers them to init as read-only VMOs. init
first checks the manifest's signature: Ed25519 (RFC 8032, SHA-512;
Monocypher, `third_party/monocypher`) over every byte before its
signature line, against the public key in the running build's own boot
image (`/boot/update.pub`, built in by the Makefile from the owner's
`~/.config/jamos/update.pub`). Nothing else in the manifest is used
before that passes; a build without a key refuses every update, and so
does an unsigned manifest. Then it copies the files into VMOs only it
holds, checks each size and SHA-256 against the manifest, calls
`kexec_load` (which init alone may) and notes `/esp`'s files as seen, so
the `reboot` that follows starts the fetched build
([Kexec](#kexec-reboot-and-panic)). `-n` checks without loading. A build
whose network default (the manifest's `net`, from its `build.txt`, signed
with the rest) isn't the running build's is refused (`update -f` takes
it), so `update` never moves a PC from VLAN 21 to untagged or back by
accident. By default only RAM changes: a power-off brings back the stick's build.
`update -w` has init also write the build to the stick's ESP once it is
loaded (the stick's own build kept as the previous one), so it survives a
power-off ([Storage](#storage) has who may write the ESP and in what
order); if the write fails, the build stays loaded, the stick still boots,
and the answer says how far it got. The key's secret
half stays on the Mac (`build/host/jamos-sign`, from the same Monocypher,
makes it and signs each manifest the server hands out), so a device on
VLAN 21 posing as the Mac can serve only builds the owner signed; an
older signed build is one of those (downgrades are allowed: the owner
types `update` and sees both versions). Each build carries the key it
will check the next update with, so the first build with a key, or with
a new key, goes on the stick by `make flash`.

**The manifest stays readable by every build to come** (its grammar is
in `user/include/update.h`). Format 2 (`jamos-update 2`, then `version`,
`git`, `net`, `kernel`, `bootfs`, `signature`, in that order) is the
stable base: those lines keep their meaning for good. Anything a later
build adds goes in a new extension line (`<key> [<value>]`, anywhere
before the signature line, signed with the rest), which a build that
doesn't know it skips, so an older build always takes a newer build's
manifest and `update` moves it forward. A line the running build must
act on to run the new one right is a must-understand line (its key
starts with `!`): a build that doesn't know it refuses the update, once
the signature has checked out, and says which line it needs ("needs a
newer build"); it never skips one. Each update request carries the
manifest format the asking build reads (an older build's says nothing:
format 2), and the server makes the manifest in the newest format that
build reads, so a newer server always serves an older build. Only a
change of the signature scheme itself would need a new format, and then
a `make flash`.

**What each process holds:**

| Process | Holds | Parses network data |
|---|---|---|
| drv/rtl8125, drv/e1000e | its PCI function, registers, interrupt and `dma_cap`; the netdev server end | no: a frame's length and bytes 12-17 only |
| netstack | the network cards' devmgr device channels; the server ends of netctl and `/svc/net` | yes: Ethernet, ARP, IPv4, ICMP, UDP |
| dhcp | netctl | yes: DHCP replies |
| dns | `/svc/net-sys`; the server end of `/svc/dns` | yes: DNS replies |
| netlog | a klog reader, `/svc/net-sys`, the panicked boot's log (read-only) | the Mac's acks |
| bin/update | `/svc/net-sys`, its offer channel to init | yes: the fetch's replies and the manifest |
| sntp | `/svc/net-sys`, `/svc/dns`, the root with `RIGHT_ROOT_CLOCK` | yes: SNTP replies (48 bytes) |
| init | the fetched build's copies, `kexec_load`, the update key's public half (its boot image's), devmgr's ESP channel (`update -w`) | the manifest only (a strict parser, then its signature); the files it copied are only hashed |

**The service-loop rule, as applied** ([How a service waits](#how-a-service-waits)):
each driver runs one loop on one port (its interrupt, netstack's event,
its netdev channels); netstack's loop never waits (its waiting calls are
on `connect.c`'s thread); dns writes its calls to netstack without waiting
and takes the answers off its port. dhcp, netlog, sntp and `bin/update`
serve nobody, so they may block, always with a deadline. init's update
check hashes on a worker thread, and `update -w`'s stick write runs on
the same worker after it; its loop does only the `kexec_load` and
`/esp`'s stat.

**Waiting on many sockets** (`user/include/netwait.h`, libos;
[M9.5-PLAN](docs/M9.5-PLAN.md#track-d-as-built-waiting-on-many-sockets)).
A wait set is one port with up to 256 entries: sockets (their rings,
`to_prog` event and channel) and any other handle, each with an interest
(read, write). `netwait_wait` returns the ready entries. Readiness is
computed from the socket's rings and status line whenever the set looks,
never from signal bits, so the set is level-triggered and a coalesced or
early signal can't lose a wake: a signal only says which entry to look
at. An entry that is not ready is armed (its event's bits cleared, the
rings' `waits` flags raised, one more look) and leaves the list; a ready
one stays on it, so a wait costs the entries that are ready or were
signalled, not all of them. Hung up (netstack's end of the channel closed,
a stream closed) and errors are always reported. This is what M13's
`poll`, `select` and `epoll` will be built on.

**Which boot uses the network.** QEMU's e1000e is bound on every boot
that has one. The PC's RTL8125 is too (the owner's call, 2026-10-02): as
the netdev service on the everyday boot, as the probe with `netprobe` and
the send test with `netsend` ([TESTING.md](docs/TESTING.md#the-boot-menu)).
The entry "Jam OS (no network)" boots with `vlan=off`, so every network
driver starts with the network off and leaves its card alone. kexec keeps
the `vlan=` word (so a `reboot` of "Jam OS (no network)" stays off) and
`net`, not the one-shot `netprobe` and `netsend`.

## Userland

- **libos** (`user/lib/`): startup, syscall wrappers, malloc, printf,
  channel/port helpers, `spawn()`, threads, the ELF loader, the file
  namespace and its file calls, random numbers (`os_random`, from the
  [kernel's generator](#random-numbers)), and the implementation of
  `<jam/driver.h>`. malloc (`user/lib/heap.c`) serves blocks from one VMO
  mapped on first use: freed blocks go on a free list kept in address
  order and merge with free neighbours (or go back to the end of the
  heap), and an allocation takes the lowest block that fits, so a
  long-running program that frees what it allocates stops growing. **libfun** (`user/apps/fun/`): the
  apps' screen, drawing (premultiplied alpha and anti-aliased shapes in
  `alpha.c`), text (UTF-8: the console font's ASCII and Latin-1 and
  Latin Extended-A glyphs, one box for any other character), keys and
  a thread pool (its workers spin briefly between batches that come back
  to back, and sleep as soon as the app waits for keys: `pool_rest`).
- **userboot** (`kernel/proc/userboot.c`): a tiny ELF loader in the kernel
  starts init from bootfs under a root job, waits for it and reports its
  exit code and whether the root job ended with nothing charged.
  Everything after that is loaded by libos (`spawn()`, the same rules
  through ordinary syscalls); the kernel only provides "create process,
  map, start at entry with stack". Text and rodata map the bootfs pages
  directly (never writable: those pages are not write-protected); writable
  data is copied into a fresh VMO, bss is zero; the stack has a no-access
  guard page below it.
- **Startup message**: every process starts with one channel message holding
  argv, environment, and handles by role (`kernel/include/jam/startup.h`):
  SELF_PROCESS, SELF_VMAR, SELF_THREAD, JOB, STDOUT, BOOTFS (read/map/exec,
  never write), RESOURCE, DEVMGR and DEVMGR_CTL (devmgr's own server
  ends), DEVMGR_DEVICE (a devmgr device channel: the mixer's sound cards),
  CONSOLE, NS (the namespace: mounts and
  services, below), AUDIO and AUDIO_CTL (the mixer's server ends), CRASHLOG
  (init and logd on the boot after a panic: the panicked boot's log) and
  program-specific ones (SR_USER + n). A program reaches a service by its
  name in the namespace, not by a startup role, so a new service costs no
  startup slot. `printf` writes to the STDOUT channel when there is
  one, else through `debug_write` (lines prefixed `[process-name]` in the
  kernel log); `debug_report` also puts a line into the RESULTS box.
- **init** holds the root capabilities and starts services with only the
  handles they need: on a plain boot the bootfs server, the console,
  the boot splash (once; the shell waits for it), serialin, devmgr, the
  mixer, the music player, netstack, dhcp (without a static address), dns,
  logd (once `/data` is there), netlog (when `net.host` is set), sntp
  (once `/data` is there, unless `ntp = off`) and the shell, restarting
  any that die (killing devmgr takes its drivers with its job), backing
  off up to 5 s; one that dies more than 10 times in a minute is given up
  on, except the console and the shell, which nobody could do without
  (an end of serialin's or the shell's that the console's took with it
  doesn't count); for the
  regression run the programs in `boot/init.cfg`. It builds the first namespace (`/boot` at once, `/data`
  and `/esp` when devmgr reports their filesystem services) and publishes
  its services in it under `/svc` (`audio` and `audioctl`, the mixer's,
  each a channel per opener; `music`, a channel per opener; `devmgr`, a
  channel per opener, and `devmgr-ctl`, each devmgr's;
  `init`, the shell's control channel; `logd`, a channel per opener;
  `net`, netstack's sockets for programs, `net-listen`, the same with the
  listen permission, `net-sys`, the same for the network's services
  ([Networking](#networking)), and `dns`, the resolver,
  each a channel per opener). The services it starts
  that have a namespace get the part of it their grants name: the shell
  all of it as it is, the music player every mount read-only and the
  mixer, logd `/data` with its top-level `etc` guarded, the splash the
  mixer; they are sent every later change (a mount gone, or back with a
  new service, a new devmgr's channels), each change replacing the one
  they haven't read yet (below). Its control channel (`abi/idl/initctl.idl`) serves
  `kill <name>` ([Drivers and services](#drivers-and-services)), `sync`,
  `mount` (`-w`/`-r` for a `/usbN`, passed on to devmgr), `shell_ready`
  (the shell is up: the splash gives the screen back) and `reboot`,
  which syncs `/data` and every `/usbN` first (2 s at most) and has logd
  write out the log's last lines before the restart (a kexec, or the
  firmware's reset if that fails; `reboot_firmware` always resets;
  [Kexec](#kexec-reboot-and-panic)); the shell holds one
  end, the console another that answers only `reboot` (Ctrl+Alt+Del).
- **Namespace**: each process has a table of mount point → `fs` channel
  (`/boot`, `/esp`, `/data`, `/usbN`) and of services → their channel
  (`/svc/<name>`), given by whoever started it (startup role
  NS: a channel on which the starter sends them, and later ones to
  a program that is already running). A program reads that channel only
  when it next looks up a path, and some never do again (logd), so a
  starter that follows its mounts for a running program keeps a
  duplicate of the program's end and sends each change as the whole
  namespace (`NS_SET`) after taking back the one not read yet
  (`ns_update`): the program's end holds at most one message however
  often the mounts change, and its next lookup sees the latest. `NS_SET`
  replaces only what the starter gave; mounts the program made itself
  (`ns_mount`) stay unless the set has one of the same path
  (`user/include/os.h`, "files", has the protocol). libos finds a path's mount and calls
  that mount's service (`abi/idl/fs.idl`, `abi/idl/file.idl`; file data
  through a shared buffer VMO); `..` never leaves a mount. `/boot` is the
  bootfs image served by a process (`user/services/bootfs/`). No global
  kernel VFS: a program reaches only the mounts and services it was
  given, which is the only permission system for files and services.
  **Services**: `svc_open(name)` gives the caller a channel of its own
  where the service hands them out (the `svc` protocol's `connect`,
  `abi/idl/svc.idl`: the music player, the mixer's two, devmgr's
  queries, logd and netstack's `net` do), else a duplicate of the
  shared one; `svc_get` keeps one and opens it again once its service
  has restarted. `/` lists `svc`, `/svc` the names. **Views**: a mount's
  service hands out narrower channels onto the same volume (`fs.view`,
  `<fsview.h>`): read-only (every change refused, `statfs` says so), or
  with the volume's top-level `etc` guarded (no change at or under it,
  however the name is spelled); the service checks every request on a
  view before its own code sees it (fat and libos's fsserver). **Grants**:
  a starter names what a child gets (`<os.h>` "grants"): everything as
  it has it, one mount as it is or as a view (`/data:r`, `/data:w`), every
  other stick (`/usb*`), every mount as views (`*:r`, `*:w`), a service;
  the views are made before the child starts. Not built yet: a POSIX
  `open()` on top.
- **The program's list** (`<wants.h>`, [M8.6-SVC.md](docs/history/M8.6-SVC.md)):
  a program declares in its source what it wants (`JAM_WANTS("svc
  music\n" "mount /data r\n")`: services, mounts read-only or writable,
  the root resource's powers it needs, and `svc net listen` for the
  network with the permission to listen, [Networking](#networking)); the text is an ELF note
  under a `PT_NOTE` program header, and `tools/checkwants.py` checks every
  program's list when the boot image is built (that is the build's
  approval; the services that kill drivers and services are for tests
  only). The shell gives a program it runs exactly its list (every mount
  as a view: read-only, or writable with `etc` guarded; the root with
  only the powers named, which it can't pass on) and its terminal (a
  PROGRAM-level console channel, its output channel in a pipe); a
  program with no list gets the terminal only. Only init and the shell
  hold a `/data` whose `etc` they may change.
- **Shell** (`user/services/shell/`): `main.c` is the console I/O, the line
  editor and history; `sh_parse.c` splits a line (`; && || |`, quotes),
  `sh_vars.c` holds variables ($NAME, export -> the environment of `run`)
  and aliases, `sh_exec.c` runs a line (pipes: stages run in turn, each
  one's output captured in memory as the next one's input, by `sh_io.c`; a
  program in a pipe gets an SR_STDOUT channel), `sh_program.c` starts
  programs (each with its list, above), `sh_table.c` is the one
  command table (with help), `sh_complete.c` Tab completion, `sh_vfs.c`
  paths and files over libos's namespace (the current directory is the
  shell's own); `cmd/<name>.c` is one file per command.
  System information comes from dedicated syscalls (`sys_info`,
  `cpu_stat`, `proc_list`, `rtc_read`; `RIGHT_ROOT_SYSINFO` and
  `RIGHT_ROOT_CLOCK` on the root resource: each power over the system is
  a right of its own, `<jam/abi.h>` RIGHT_ROOT_*, held only where used);
  CPU time is counted per thread and CPU at every switch. The kernel tests
  are shell commands too (`ktest`, `stress 600`), so a test run needs no
  reboot; rebooting is only for loading a new kernel from the stick.
- **Executables**: static ELF64 at 0x400000; no `fork`. `spawn()` loads a
  program from the bootfs image, from a range of any VMO, or from a file
  read through the namespace into a VMO; code is mapped executable only
  from a VMO handle with `RIGHT_EXEC`, which only the bootfs image's has,
  so a program outside `/boot` is refused (`ERR_ACCESS_DENIED`), but for
  one on `/data` the owner marked: `allow <file>` in the shell init starts
  shows the file's list and asks y/n; a yes writes `<sha-256>\t<path>\t
  <list>` into `/data/etc/allow` (`allow -l` lists, `allow -r <name>`
  takes back). To run it the shell reads the file into a VMO only it
  holds, turns that handle into one that may execute and not write
  (`vmo_make_exec`, which needs `RIGHT_ROOT_VMEX`, refuses a VMO with
  another handle, a mapping, a pin or a write in progress, and seals it: the
  VMO itself refuses every later write, even from a call that started
  before), hashes those bytes, and starts it
  only if a line has that path and hash, with the list read from the
  same bytes. No program can mark one: every program's `/data` is a view
  that leaves `etc` alone, and none holds `RIGHT_ROOT_VMEX`. A list that
  asks for devmgr's or init's channels (`svc devmgr`, `devmgr-ctl`,
  `init`) is refused, approval or not: they reach drivers, devices and
  the filesystems unguarded, past every view; so is one that asks for
  `right debug` (the kernel's debug commands panic and crash the machine
  on purpose). Whoever holds
  the stick can edit the file on another computer, as they could replace
  the kernel. User-space pagers
  ([ROADMAP.md](docs/ROADMAP.md#design-ideas-not-scheduled)) would be the
  clean way to map such code.

## Graphics

Text console first; these choices keep later graphics possible. The
monitor is on the RTX ([HARDWARE.md](docs/HARDWARE.md#the-machine)).

- The display is the firmware (GOP) framebuffer: fixed mode, no vsync. The
  console owns it and can lend it to one program (`console.lend_screen`),
  which is how the apps draw real pixels; the kernel takes it back on a
  panic, from anyone.
- A future compositor process owns the framebuffer VMO; apps draw into
  their own surface VMOs and send damage rectangles over a channel; input
  goes to the compositor, which routes it to the focused client. Rendering
  is in software (28 cores and AVX are plenty for 2D at 2560x1440). It
  speaks the Wayland protocol (G1 in the roadmap): Wayland's model and
  wire format over channels, handles where Linux passes file descriptors,
  `wl_shm` pools as VMOs; our own compositor, not a port.
- Mode setting and vsync only through the Intel iGPU (documented by
  Intel); the NVIDIA card (GSP firmware, no practical open path) stays a
  plain framebuffer. The IOMMU matters most for GPUs.
- The HID driver handles a mouse as well as a keyboard and sends events
  through a protocol a compositor can take over. Keyboards stay in the
  boot protocol. A mouse whose report descriptor has a wheel is driven in
  the report protocol, decoded with the layout a small bounded parser
  (`drivers/hid/report.c`) finds in the descriptor, since a real mouse's
  boot protocol has no wheel; the boot word `hidboot` keeps every mouse
  in the boot protocol. The horizontal wheel is parsed but not sent: the
  `input` protocol has no field for it. Until there is one, the
  console passes mouse reports on to the client that has the key focus, on
  its key channel, and only if that client asked for them (the wire format
  is in `<jam/abi.h>` with the key event's); the apps library turns them
  into a pointer and draws its arrow.

## Audio

The target is headphones in the case's front-panel jack, which hangs off
the board's Intel HD Audio controller and its codec
([HARDWARE.md](docs/HARDWARE.md#other-devices)). The driver, `drivers/hda`,
is a process like any other (PCI, MSI, DMA through pinned DMA32 buffers),
bound by devmgr to Intel's HD Audio functions (class 04 03 00). Built so
far: the controller reset, the CORB/RIRB command rings, each codec's
widget graph read and logged (`hda` in the shell), the path from a DAC to
the front headphone jack, and one output stream on that DAC. The path is
found in the graph by a pure function (checked at every start against
the PC's codec and QEMU's, kept as fixtures) and set up muted with the
pin's output off. Every verb goes through one file with an allow-list of
SET verbs, so the driver can never write the board's own jack
descriptions, GPIOs or vendor coefficients. `open_output` hands its
client a channel of its own (closing it stops and releases the stream)
and a DMA32 ring of 341 ms as a VMO to map, at the largest sample size
the DAC takes (24-bit on the PC, in 32-bit containers; 16-bit in QEMU;
`hda bits` caps it), played in eight periods of 42.7 ms with an
interrupt (MSI, through the driver's port) at each; the position comes
from the DMA position buffer, and the driver zeroes the ring behind it,
so a client that stops writing gives silence, never a loop (no margin
is needed there: every reported position trails the DMA engine's
fetch). The path is
unmuted only while the stream runs, at a gain that starts at -30 dB
(`hda gain`, `set_gain`: the DAC's amp, never above 0 dB), and muted
again as soon as it stops, so the jack is silent whenever nothing plays
(but for a driver killed or crashed mid-stream: no more samples are
fetched once its bus mastering is off, but the path stays open until
its restart resets the link).
The muting is done by the path's amps where one can mute (the PC's, and
QEMU's codecs with their mixer): the output stage (the pin's output and
headphone amp, EAPD) goes on once, with every amp muted, at the driver's
start, and stays on until it exits, so its power-up thump is never heard
(the first unmute waits until it has been on 400 ms). Where no amp can
mute, the pin's output is switched with the stream instead.
`beep` in the shell makes the samples (the driver never makes sound of
its own).

**Jacks.** Every pin the configuration defaults call a jack and whose pin
capabilities have presence detection is watched (seven on the PC's ALC897:
the rear line-outs, the mics, the line-in and the front headphones). Each
gets an unsolicited response tag of its own; the controller takes
unsolicited responses (GCTL.UNSOL) and the RIRB interrupt goes on, while
commands are still polled: one demultiplexer sorts every RIRB entry,
whoever reads it, so an unsolicited response is never taken for an answer
and a late answer never for the next command's. On a response the pin's
presence is read (after SET_PIN_SENSE on pins that want the trigger),
held 80 ms against contact bounce and logged (`headphones plugged in
(front, pin 1b)`). A pin is also polled every 500 ms until its first
unsolicited response proves it sends them, and for good if a change comes
without one, so detection works either way. A change only logs: an unplug
while playing stops nothing. `hda jacks` (`hda.jacks`) shows each jack's
state and how it is watched.

**The mixer** (`user/services/mixer`, [docs/history/A2-PLAN.md](docs/history/A2-PLAN.md))
is the driver's only client but for the tests (`hdatest` holds devmgr's
control channel): init asks devmgr for each sound card's device channel
before anyone else can ask, and gives them to the mixer, so devmgr hands
the driver's channel out only there. Everyone else reaches the driver
through `audioctl.device`: the mixer asks the driver for a *query
channel* (`hda.query`), which answers the dump, info, jacks, gain and bits
but refuses `open_output`, so the shell's `hda` can look and set the gain
but never take the output stream. The mixer is the driver's only client
while anything plays: every program's sound
goes through it, so several play at once. init starts it after devmgr
and makes its two channels once, keeping their server ends, so a
restarted mixer serves the same channels: `audio`
(`abi/idl/audio.idl`), published as `/svc/audio` (a program whose list
asks for it plays sound), and `audioctl` (every stream's volume and the
master volume, `/svc/audioctl`: the shell's `vol` and test programs).
Each opener of either gets a channel of its own (`svc.connect`), so a
late answer goes only to the program that asked; it ends with the mixer
that made it, and the next open waits on the shared channel for the
restarted one. An opener holds at most 4 of the 16 streams
(`MIXER_STREAMS_PER_CLIENT` in `user/include/mixer.h`, where the number is
argued): a program looping over the library can't starve the others.
`open_output` gives a client a stream of its own: a channel (start,
stop, drain, position, its volume; closing it ends the stream), a ring
VMO (a header page with the client's `write` and the mixer's `read`
counts, then 1.37 s of 48 kHz stereo frames) and an event. The client
writes frames into its mapping with no call; the event is signalled
only when the client waits for room or the mixer sleeps. The mixer never
maps a client's ring (the client could shrink it): it copies frames out
with `vmo_read`, once a period. It holds the driver's stream open only
while a stream plays, sends `wait_period` without waiting for the answer
(one thread, one port; `audioctl.device`, which asks devmgr and then the
driver, has a thread of its own so it never holds up the mixing), and at each period's end mixes until four
periods are written ahead of the play position (128-171 ms): each
stream at its Q15 gain, summed in 32 bits with 8 bits below the 16-bit
step, the master gain, a lookahead limiter instead of clipping (it does
nothing below full scale), then out at 24 bits (or, to a 16-bit device,
rounded with TPDF dither where a volume left a fraction): one stream at
0 dB is bit-exact. A slow
client gives silence for what it lacks (an underrun); a dead one's
stream is dropped; a driver that restarts is reopened; a mixer that dies
is restarted by init and its clients open new streams. Programs write
sound through `<audio.h>` in libos (open with their own rate and
channels, blocking writes, drain, close), a mixer stream underneath: the
library makes mono stereo and resamples to 48 kHz (a polyphase
windowed-sinc filter: flat to 20 kHz, 100 dB down from 22.05 kHz, its
position kept exactly); `beep` and `play` (WAV files, parsed by
`<wav.h>`; `play -s` prints underruns, late periods and the least lead
afterwards) use it.

**MP3** ([docs/history/A2-PLAN.md](docs/history/A2-PLAN.md#mp3)): `play` picks a file's
format by its first bytes, not its name (`RIFF`: WAV; MPEG audio frames,
after any ID3v2 tag: MP3; anything else is refused), through a small
source interface in libos (`<play_src.h>`, `user/lib/play_src.c`: open, read
16-bit frames, close), which the music player uses too. The shell's
`play` only checks its arguments and opens the file: the file is decoded
and played by `bin/play` (`user/apps/play`), a program of its own that
holds nothing but that open file (handed over with `file_give` and
`file_adopt`), its list's `/svc/audio` and a channel for its lines, so a
crafted file on someone's stick reaches the sound output and nothing
else. Ctrl+C sends it a byte on a stop channel: it fades out and says
where it stopped, and its job is killed if it hasn't ended 3 s later.
MP3s are decoded there by `<mp3.h>` in libos, which wraps dr_mp3 (`third_party/dr_mp3`, public
domain or MIT-0, a fork of minimp3, vendored unmodified, SSE2 on): MPEG-1,
2 and 2.5, Layers I to III, CBR and VBR, into 16-bit frames at the
file's rate, which `<audio.h>` resamples like any other. libos's
`mp3_sniff` skips the tags at both ends and confirms a frame by the next
one before the decoder sees the file, so a file that isn't MPEG audio is
refused at once instead of searched end to end; the decoder reads the
file 64 KiB at a time and skips frames it can't decode. Only programs
that call `mp3_open` link the decoder (about 35 KiB of code; ~33 KiB of
state and a 64 KiB buffer while a file plays). Decoding costs about
4 % of a core under QEMU's emulation (`play -n`), far less on the PC.

**The music player** (`user/services/music`,
[docs/history/A2-PLAN.md](docs/history/A2-PLAN.md#music-player)) is a service init starts
after the mixer, in a job of its own, so it plays on while the shell runs
other commands, through Ctrl+C and a restart of the shell. init makes its
`music` channel (`abi/idl/music.idl`: start, stop, next, status,
set_volume, prev, play, levels, pause, sleep, spectrum, stereo) once and
keeps both ends, as
the mixer's, and publishes it as `/svc/music`. Each opener gets a channel
of its own (the player answers the `svc` protocol's `connect` on its
channel and serves up to 8 of them), so a call cut short leaves its late
answer with the opener: the shell's for `music start
[folder] | stop | next | prev | pause | status | vol | sleep`, and
bin/jamjar's, the player's window. It walks the folder for `.mp3` and `.wav` files, shuffles
them (every track once per pass, never the same twice in a row) and
plays them back to back through one mixer stream, reading each with
`<play_src.h>` and switching the resampler between files with
`audio_set_input`, so there is no gap; `next` drops what is queued with
`audio_discard`'s fade. It is single-threaded: it answers its channel
between chunks of about 20 ms. A file it can't read, three in a row (the
stick pulled), a pass that plays nothing, or the mixer stream failing
three times in a row stops it with a line in the log; a refused file is
skipped for good. It keeps the last 64 tracks for `prev`, a sleep timer
(the last 30 s fade out), and for `levels` the loudness of what is heard
in 64 frequency bands, for each channel (`stereo`) and the mono mix
(`spectrum`; `levels` has 16): one complex FFT of the two channels of
each ~11 ms of what it writes, kept
by stream frame because it writes up to 1.37 s ahead
(`user/services/music/spectrum.c`).

**jamjar** (`user/apps/jamjar`, [the design note](docs/history/MUSIC-GUI.md))
is the player's window, an app whose list asks for the player (it opens
`/svc/music`, a channel of its own), `/data` and the other sticks
read-only; it plays nothing itself. It reads the library through those
and the albums' covers on a thread of its own: the ID3v2 picture frame
found by its own bounded parser. The picture is decoded by
`bin/jamcover` (`user/apps/jamcover`, `abi/idl/jamcover.idl`), a helper
jamjar starts in a job of its own with nothing but a channel, the
picture's bytes (a VMO it may only read) and a VMO for the pixels: no
namespace, no console, no services. There PNG or JPEG goes through
stb_image, only after `stbi_info` has said the size (at most 2048 on a
side and 2048x1600 pixels), with all of stb_image's memory from one
40 MiB arena, and is cropped square and scaled to 256 and 512 pixels.
jamjar reads the pixels back with `vmo_read` and never maps them. A
picture that crashes the helper, or keeps it busy past 5 s twice in a
row (the second try 5 s after the first, in case the machine was only
busy), costs that album its cover: jamjar kills the helper and starts
another for the next cover. The pictures it keeps are capped at 8 MiB.

## Storage

- The USB stick has two FAT32 partitions: the **ESP** (Limine, kernel,
  bootfs), which no program ever writes, and a **data partition** mounted
  at `/data` for everything writable. A bug in a program's file writes
  can't make the stick unbootable. The Mac writes the ESP: `make usb` lays
  out a new stick (the data partition grown to the end of it and left
  blank for Jam OS to format), `make flash` copies a new kernel, boot
  image and boot menu onto the ESP of one that has the layout
  ([HARDWARE.md](docs/HARDWARE.md#flash-and-boot-the-stick)), keeping the
  stick's kernel and boot image as the previous build
  (`/esp/boot/prev-jamos.elf`, `/esp/boot/prev-bootfs.img`: the boot menu's "Jam OS
  (previous build)").
- **On the PC only init writes the ESP, and only for `update -w`** (a
  build it has checked and loaded, [Networking](#networking)). The power
  is a channel: devmgr makes the ESP's partition writable only when asked
  on its ESP channel (`DEVMGR_ESP_WRITE`), whose one client end init made
  when it started devmgr (devmgr's startup role `DEVMGR_SR_ESP`) and gives
  to nobody; the control channel the shell and the tests hold can't ask
  it. devmgr stops the ESP's fat in order and starts it again on a
  `block` channel opened read-write, hands init a channel to it, and lists
  no `/esp` meanwhile, so the writable channel is in no namespace, the
  shell's included; then the same back to read-only (the volume marked
  clean), and `/esp` comes back. init's writer (`user/services/init/espwrite.c`,
  on update.c's worker thread) keeps the stick bootable at every step:
  the stick's build is copied, not moved, to the previous-build files and
  read back first; the new build goes under `*.new` names and is read
  back and checked against the signed manifest's SHA-256s; only then do
  the names switch, one directory entry at a time, during which the
  previous-build entry still boots the old build. A failure stops it
  there, and after the switch has begun puts the old build back under
  the default names (copied again from the previous build) while the
  ESP's fat still answers. The read-back goes through fat, whose cache
  may answer it. Every call of the write ends by one deadline (120 s for
  the steps, 60 s for the clean-up after a failure), so `update -w`
  always gets an answer; each step logs its time.
- Write ordering: file data, then both FATs, then the directory entry.
- fat keeps a write-through block cache (`user/services/fat/cache.c`):
  a miss reads 4 KiB, and twice as much as the last one when it carries
  on where that one ended, up to a 64 KiB line (one block call); 16 lines
  per volume, the least recently used given up first; every write still
  goes to the
  stick at once and is then copied into the lines that hold its sectors,
  so the cache never holds anything the stick doesn't (sticks get
  pulled). A big read (a whole 64 KiB) goes past it. Nobody else writes a
  partition while its fat runs, so the cache never goes stale.
- The exception is a file opened `FS_GATHER` (`<os.h>`), which only init's
  ESP write uses: FatFs writes a file a cluster at a time, one sector on
  the ESP, and a cheap stick takes milliseconds per write command, so its
  writes are held in fat (`user/services/fat/hold.c`, about a MiB) and go
  out in 64 KiB writes, in the order above, at its sync or close (or when
  anything else writes, or the hold is full). A pulled stick loses what
  was held; a held write that fails stops fat writing until it starts
  again. An unlink's FAT writes are held the same way, and sent before it
  is answered.
- The FAT "clean shutdown" bit is cleared on the stick before the first
  sector written after a sync, and set again once everything is flushed (a
  sync, the last written file closed, a clean stop). A volume found dirty
  is mounted anyway and logged: there is no fsck. A sync sends SCSI
  SYNCHRONIZE CACHE when a sector was written since the last one: a
  file's sync is one, with the clean bit written after it and carried by
  the next; a filesystem's sync (the shell's `sync`, init before a reboot)
  flushes the bit too.
- One file is one open file in fat, however many programs have it open:
  a file being written can be opened by others to read, and they see what
  has been written so far. A second writer is refused. fat never maps the
  buffer it shares with a file's client (the client can shrink it).
- Only the boot disk's blank data partition (no boot signature) is
  formatted; one that holds another filesystem or a damaged FAT is left
  alone. fat formats only when it is started with an explicit flag
  (`FAT_ARG_FORMAT` in `user/include/fatsvc.h`), and devmgr gives the flag
  to that one partition; on any other partition of any disk fat writes
  nothing it wasn't asked to write through the `fs` protocol.
- devmgr is the only client of a disk's `storage` channel. The disk Jam OS
  booted from is the one with partition 1 of type 0xEF holding
  boot/jamos.elf (it looks through a read-only fat service) and partition
  2 of type 0x0C: they are `/esp` and `/data`. With two Jam OS sticks in,
  the one the machine booted from: Limine names the disk it read the
  kernel from by its MBR disk id, the kernel passes it on to init and
  devmgr (`bootdisk=0x<id>`) and to the kernel it kexecs (`bootdisk=N` on
  that one's command line), and usb-storage reads each disk's id
  (`storage.disk_id`). A Jam OS disk with another id waits for that one
  (up to 10 s after devmgr started, at least 3 s) and is then not the boot
  disk; if that one never comes (the stick was swapped) it may be after
  all. `make usb` gives every stick a random id (`tools/mkimage.py`); on
  a stick made before, whose id is 0, the first Jam OS disk found is the
  boot disk, as before. Each mount is a fat service
  holding one partition's `block` channel, supervised like a driver; init
  gets the mounts' `fs` channels from devmgr (`DEVMGR_MOUNTS` in
  `user/include/devmgr.h`), with a generation that moves whenever a mount
  comes, goes or is restarted.
- Any other stick: each partition whose MBR type says FAT (01 04 06 0B 0C
  0E EF) gets a fat service and the lowest free `/usbN`, one at a time so
  the numbers follow the order found; a stick with no partition table
  whose block 0 is a FAT boot sector is served by usb-storage as one
  partition over the whole disk (a block 0 that holds a partition table
  is never taken for one, whatever else it holds). GPT is not read. A partition that turns
  out to hold no FAT volume is left alone and said so in the log.
- Read-only is enforced below the filesystem: a `/usbN` partition's
  `block` channel is opened read-only, and usb-storage refuses every write
  on such a channel, so a bug in fat or FatFs can't change someone's
  stick. `mount -w /usbN` (the shell asks init, init asks devmgr:
  `DEVMGR_REMOUNT`) stops that fat and starts a new one on a channel
  opened read-write; `mount -r` goes back. The stop is in order (devmgr's
  own `fsctl` channel to each fat, `abi/idl/fsctl.idl`): files closed and
  flushed, the volume marked clean, then the exit, so a program writing
  at that moment gets an error for the write that came too late and loses
  none that was answered. The mount is gone
  for a moment either way, and files open on it are closed. `/boot` and
  `/esp` can never be made writable by `mount` and `/data` never
  read-only: init passes on nothing but `/usbN`, and devmgr remounts
  nothing else that way (making the ESP writable for `update -w` is the
  ESP channel's alone, above).
- logd follows the kernel log from its first byte into
  `/data/logs/boot-NNNN.txt`, the next free number each boot, after one
  line that dates the file (when the kernel started, by the wall clock in
  the system's zone: [Time and settings](#time-and-settings)), syncing at
  most every 250 ms while the log flows. A reboot loses nothing: init
  syncs the mounts, has logd write out and sync the log up to that line
  (`abi/idl/logctl.idl`), and resets. A panic or a pulled plug loses what
  was logged since the last sync, a quarter of a second at most plus the
  write in flight; a panic's log, its own lines included, is saved by
  the next boot next to the boot's file as `boot-NNNN-crash.txt`
  ([Kexec: reboot and panic](#kexec-reboot-and-panic)). logd gives the
  kernel its file's name (`klog_name`) for that, and counts a number as
  taken when either file has it. Without `/data` it waits and tries again; what
  the kernel's 4 MiB ring drops meanwhile, or in a burst faster than the
  stick takes it, is marked in the file as lost.
- The 4 GiB file limit and the lack of owners/permissions are accepted:
  authority comes from namespaces, not the filesystem.

## Time and settings

- **The wall clock** is the kernel's, in UTC: an offset from the uptime
  (`kernel/dev/wallclock.c`), so it needs no tick. At boot the kernel has
  only the real-time clock's reading, taken as UTC until someone says
  otherwise: a PC keeps its RTC in UTC or (Windows) in local time, and the
  RTC can't say which. init sets it (`wallclock_set`, RIGHT_ROOT_CLOCK on
  the root resource) at the start of shell mode and again whenever `/data`
  comes: it reads the RTC (`rtc_read`), takes it as the setting `rtc`
  says (`local`, the default; `utc`; or a zone's name: the RTC keeps
  that zone's time, e.g. Windows set to another zone than the one Jam OS
  shows), and gives the kernel the time
  zone too, a name. Any program reads the time without a handle
  (`wallclock_get`): it is the uptime plus an offset. Once the network has an
  address, bin/sntp sets it from the network's time
  ([Networking](#networking)), marking it `WALLCLOCK_NET` (the one flag
  `wallclock_set` takes; a set without it clears it): init then keeps
  that time when `/data` comes back and gives the kernel only the zone,
  `date -z` keeps the mark, and `date -r` says where the time came from.
- **Time zones are libos's** (`<wallclock.h>`): the calendar, a small
  table of named zones with their daylight-time rules (Australia's,
  Auckland, London, New York, Los Angeles; `Australia/Sydney` the
  default) and fixed offsets (`UTC+05:30`). The kernel keeps only the
  zone's name. FAT stamps files in local time, as FAT does (fat's
  `get_fattime`); the shell shows times in `$TZ` or the system's zone;
  each log file starts with its date.
- **Settings that survive a reboot**: `/data/etc/settings`, plain text,
  one `key = value` a line, `#` comments (`<settings.h>` has the format
  and the keys: `timezone`, `rtc`, `volume`, `music.volume`,
  `music.folder`, and the network's `net.address`, `net.host`,
  `netlog`, `ntp` and `ntp.server`: [Networking](#networking)). init reads them when `/data`
  comes (the clock, the network's address) and when the mixer, the music
  player, netstack and netlog start; the shell's
  `vol master`, `music vol`, `music start <folder>` and `date -z` write
  them. A write goes to `settings.new`, is synced, and then takes the old
  file's name, so a pulled stick leaves the old settings or the new ones,
  never half of either (a lone `settings.new` is read in their place).
  init writes a commented file with the defaults the first time `/data`
  has none.

## Random numbers

- **One generator, the kernel's** (`kernel/dev/random.c`, `<jam/random.h>`),
  built like OpenBSD's arc4random and Linux's crng: a 256-bit ChaCha20
  key (`kernel/lib/chacha20.c`, written from RFC 8439) and nothing else.
  Each request turns the key into one block under a lock: half of it is
  the next key, half keys that request's own stream, made after the lock
  is dropped. The old key is gone at once (fast key erasure), so memory
  read later can't give back earlier output. It never blocks and works
  from any CPU, interrupts off included.
- **The seed**, at boot once the TSC is calibrated: RDSEED, else RDRAND
  (each only if CPUID has it, drawn with a retry limit and refused if it
  repeats a value or gives all zeros or all ones, as a stuck or broken
  source does), mixed with TSC timings and what differs between machines
  and boots (the loader's memory map and CPUs, ACPI's tables, the RTC's
  date). The first request 60 s after the last reseed mixes in fresh
  hardware words and the TSC. The log names the source (`random:` line);
  without RDSEED and RDRAND (QEMU's qemu64) it says, also in the RESULTS
  box, that the numbers are weak, seeded from timing only. The PC has
  both ([HARDWARE.md](docs/HARDWARE.md#the-machine)).
- **For programs: the `random_get` system call**, at most
  `RANDOM_GET_MAX` (256) bytes a call, with no handle or right: anyone
  may ask, as anyone may read the clock, and a call costs only the
  caller's time. libos wraps it as `os_random(buf, len)` and
  `os_random_u32()` (`<os.h>`), for whatever an attacker must not guess:
  DNS query ids and ports, DHCP transaction ids. Nothing is generated in
  user space, so a restarted program never repeats a stream.

## Kexec: reboot and panic

One kernel, two ways in: Limine at power-on, or a jump from a running Jam
OS, after `reboot` or a panic. Either way the boot is a normal one (every
CPU, every driver, all of RAM, the splash, the shell), and the screen
shows nothing but the splash background from the moment `reboot` starts
or the panic happens until the next boot's splash. After a panic the next
boot saves the panicked boot's log first and the shell prints one line
about it. Code in `kernel/kexec/`; the plan, with the layout and the
decisions, is [docs/history/M8.5-PLAN.md](docs/history/M8.5-PLAN.md) ("Revision 2").

- **The region** (32 MiB below 4 GiB, 2 MiB aligned: `crashkernel=<MiB>`,
  0 turns it all off) is unmapped from the running kernel, so a wild write
  can't reach it. It is written only through a 2 MiB window mapped,
  written and unmapped with a TLB shootdown (`region.c`).
- **The stored kernel** is this boot's own kernel and bootfs: Limine loads
  `jamos.elf` a second time as a module (the running image's data has
  changed since it started), and a kernel started by kexec is handed the
  same two files as modules. At boot they are laid out in the region with
  everything the jump needs (`image.c`): the segments at their link
  address, the kernel file again (for the next one's own copy), page
  tables mapping exactly what the new kernel touches before it builds its
  own, a stack, and the handoff (`<jam/kexec_handoff.h>`): a versioned,
  checksummed wire format (it crosses kernel builds), which
  `kernel/boot/kexec.c` checks completely before using. The loaded bytes
  are checksummed. Its command line is this boot's, keeping only the
  hardware switches and the words that choose how a plain boot looks
  (`kexec_next_cmdline`): after a panic in a test entry the next boot is
  a plain one. Its memory map gives it all of RAM except the region's
  loaded parts and this kernel's crash record and log ring
  (`BOOT_MEM_CRASH_LOG`), which it reads at boot and then frees.
- **The panic path** decides first, with no lock taken and nothing
  allocated: a stored kernel is armed, its checksum (read through the
  window's own page-table entries) matches, and this is not a crash loop
  (a panic within 30 s of a start that was itself a panic's, or the third
  panic in a row however far apart). Then the
  panic's lines go to the log and the serial port but not the screen;
  the crash record is filled (a panic, the log ring's place and head,
  where the panic's lines start, its message, the boot's log name, the
  panics in a row and the uptime), Bus Master Enable goes off on every PCI
  function but bridges and the display (so no device keeps writing memory
  or sending MSIs), the other CPUs (halted by the panic's NMI) are sent
  INIT, the framebuffer is filled with the splash background through its
  existing mapping, and the jump goes through a trampoline page mapped at
  the same address in both kernels' tables. The last three happen on the
  bootstrap processor: an INIT to the BSP would start the firmware or
  reset the board, so the BSP, halted like the rest, waits for a jump
  decided on an AP and makes it itself; the next kernel always starts on
  the BSP and starts every AP. A jump decided and then not made (the AP
  never hands it over within 10 s, or the panicking CPU faults again)
  resets through the firmware: the screen is dark by then, and a reset
  beats a dark hang. Without a stored kernel
  (`crashkernel=0`, no region, a damaged one) or in a crash loop the panic
  screen is drawn as before M8.5, with the reason, and the machine halts.
- **The next boot after a panic** checks the record and the ring as
  untrusted input and copies the log into a VMO for init (`SR_CRASHLOG`).
  init gives it to logd, which writes `/data/logs/<name>-crash.txt` (never
  overwriting) and syncs before it opens this boot's own log, and answers
  on a channel of init's (`<crashlog.h>`). The boot's first shell waits for
  the answer (20 s for `/data`, 60 s for the save) and prints
  `the last boot panicked: <message> (saved as ...)`, or why it was not.
- **`reboot`** (initctl.reboot, so also Ctrl+Alt+Del): the shell (or the
  console) blanks the screen first (`console.blank`: the splash background,
  nothing drawn). init starts the stored kernel as it is unless `/esp`
  has another kernel or boot image than the ones it noted (size and
  modification time) when `/esp` was first mounted; then it reads both
  and calls `kexec_load` (`RIGHT_ROOT_KEXEC` on the root resource) to replace
  the stored kernel. It syncs and flushes the log as before, stops the
  sound's clients for good (the music player, the splash and the mixer:
  the hda driver ends only once its client has gone), stops devmgr in
  order (`DEVMGR_SHUTDOWN`) and calls `kexec_reboot` (the other CPUs
  halted, the kernel's screen quiet, the image verified, bus mastering
  off, the jump). Any failure before the jump falls back to the firmware
  reset; `reboot -f` always uses it, after the same sync, log flush and
  devmgr shutdown. The kernel's firmware reset (`kernel/dev/reboot.c`)
  halts the other CPUs, takes the screen back, turns bus mastering off,
  then tries the FADT's reset register, 0xCF9's full reset, the 8042 and
  a triple fault, a second apart, each said on the screen first. M9's `update` hands init a fetched
  build on an offer channel (initctl.update_offer, `<update.h>`): init
  checks the manifest's signature with its build's key, copies the files
  into VMOs of its own, checks each length and SHA-256 against
  the manifest, calls `kexec_load` with the copies and notes `/esp`'s
  files as seen, so `reboot` starts the fetched build
  (`user/services/init/update.c`; the fetch: [Networking](#networking)).
- **A stick whose files don't load** (a flash pulled half way, a damaged
  copy): the kernel refuses them and keeps the stored copy armed, so
  `reboot` starts that one, the last good build, after a short notice on
  the screen ("the stick's kernel didn't load (...): restarting the one in
  memory"); `kernel load` says so and changes nothing. Only with no
  stored copy at all (`crashkernel=0`) does `reboot` use the firmware.
- **`kernel load`** (the shell; initctl.kernel_load, so init, which holds
  `kexec_load`'s right, does the work): `/esp`'s kernel and boot image
  read and made the stored copy now, and noted as what the stored copy
  came from, so the `reboot` after it reads nothing, and a panic before
  that reboot comes back in the new build too.

## Debugging

- Framebuffer klog from the first instruction, 4 MiB ring buffer, readable
  from user space through a klog reader handle (the console follows it,
  and logd saves it to `/data/logs/`, [Storage](#storage)). COM1 too when
  present. The log is text whoever wrote it: printable ASCII, tabs,
  newlines and well-formed UTF-8 go in as they are (a song's "JAŸ-Z"),
  any other control character and each bad piece of ill-formed UTF-8
  (Unicode's maximal-subpart rule) as one `?` (`kernel/debug/klog.c`).
- **The log and the screen**: on a plain boot (the splash plays) the
  console keeps the kernel log off the shell's screen (init starts it
  with "nolog"); `log` and `dmesg` show it, and the shell asks for it on
  the screen (`console.show_log`) while a command whose output is the log
  runs: the kernel's commands (`ktest`, `ps -k`, `pci`, ...) and the test
  programs with every line, a program `run` starts with its own lines and
  the kernel's. A few things the log says still get a line of their own,
  in yellow (`user/services/console/notices.c`): another stick plugged
  in or pulled out, the Jam OS stick pulled out and back, `/data` full or
  not mounted, a service or driver that crashed and is being started
  again or was given up on. Only lines the kernel marks as its own or as
  init's, devmgr's or logd's real process count (`klog_lines`; init keeps
  those three koids in a page the console reads, `<logwriters.h>`), so a
  program that starts a process called "init" makes no notice. Each is
  announced once things have settled
  (a remount says nothing), never twice within 30 s, at most four in
  10 s. `verbose`, `nosplash` and the safe mode show the whole log as it
  comes, and the boot tests draw it from the kernel.
- Panic screen: message, decoded exception (page-fault cause, NULL and stack
  overflow hints), all registers and control registers, symbolised backtrace
  with repeated frames collapsed, and the log tail: drawn only when there
  is no stored kernel to start (or in a crash loop); otherwise the same
  lines go to the log and the serial port, and the next boot saves them
  ([Kexec: reboot and panic](#kexec-reboot-and-panic)). Symbols come from a
  two-pass link: `.ksyms` is the last section, so filling it in moves nothing
  (checked by `tools/gensyms.py verify`). #DF, NMI and #MC run on IST stacks.
  The other CPUs are halted by NMI first.
- **ktest**: `KTEST(name)` registers a test in the `.ktests` section, run
  at boot (`ktest`, `ktest=prefix`) or from the shell. Each test fails if
  it leaks more than 2 pages; the run reports how many lock classes are in
  use. `make KTESTS=0` builds a kernel without the tests or the DBG_HOOKs.
  The set can be repeated in one boot, shuffled from a seed and run under
  load (`ktest loops=5 seed=42 load`; the shell's `soak` does all of it
  and adds user-space load): a test must pass on any run and in any order.
  Every panic screen names the loop, the seed and the test that was running.
- **DBG_HOOK** injection points (`kernel/include/jam/dbghook.h`) let race
  regression tests stop a thread at an exact line.
- The RESULTS box: every run ends with a box of the lines that matter
  (`report()`, `debug_report`), so a PC run can be read from one photo.
- Per-CPU watchdog heartbeat: a stuck core turns into a panic screen.
- QEMU mirrors the PC: q35, OVMF, xHCI USB boot (`make run`), gdb stub
  (`make debug`). How to run the tests: [docs/TESTING.md](docs/TESTING.md).
