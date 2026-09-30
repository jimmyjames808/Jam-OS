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
| Syscall ABI | Unstable until M12; numbers, wrappers and the kernel dispatch table are generated from one table (`abi/syscalls.def`) |
| IPC | Async channels + synchronous `channel_call`; ports for multi-wait |
| Bulk data | Through shared VMOs (rings + offsets), not 64 KiB channel messages |
| Memory API | VMOs + VMAR handles |
| Scheduler | Per-CPU run queues, 32 priorities, work stealing |
| Filesystem | FAT32 only, on USB mass storage; the boot partition (ESP) is read-only to Jam OS |
| Ported code | Limine; uACPI and lwIP when power management and networking land |
| Executables | Static ELF64 |
| Program output | A stdout channel in the startup message when the parent gives one (the shell does, for pipes); otherwise the `debug_write` syscall into the kernel log, which the console shows |
| IOMMU | Not yet; DMA is gated by `dma_cap`, and VT-d will go behind it |
| Users | Single user, no accounts. Handles are the only authority; a future "user" would be a namespace root plus a job quota (FAT32 can't store owners anyway) |
| Networking | The board's own NIC, driven natively; every frame on VLAN 21 only ([Networking](#networking)) |

## The migration rule

Every driver and service is a userspace process from the start.

- Drivers touch the world **only through handles**: channels, interrupt
  objects, VMOs, resource handles (MMIO, IRQs) and a DMA capability.
- Drivers include only `<jam/driver.h>` (plus the generated IDL headers),
  and `tools/checkdriver.py` fails the build if a driver uses anything else
  (kernel headers, `kmalloc`, another driver, extra sections). Driver code
  never touches kernel structs.
- **Bring-up happens as a process.** A crashing process reports why and
  where (`process "x" killed: ... at rip ...`) and can't take the kernel
  down, and a stuck process can always be killed. Bringing a driver up in
  the kernel first was tried and bought nothing
  ([HISTORY.md](docs/HISTORY.md#lessons-that-keep-coming-back)).
- **One build.** `<jam/driver.h>` has one implementation, over system
  calls (`user/lib/driver_user.c`), and every driver is a process that
  devmgr starts and supervises.
- What stays in the kernel **on purpose** is enforcement, not drivers:
  the PCI core (ECAM, BAR sizing, MSI/MSI-X programming, Bus Master
  Enable), vector allocation and interrupt objects, resources, DMA pins and
  the config-write filter, so one driver can never program another
  device's interrupts or turn DMA back on after it was killed.

## Layers

```
 Userland: init · shell · apps                                 ring 3
 libos runtime (syscalls, malloc, channels, IDL stubs, ELF loader)
 ─────────────────────────────────────────────────────────────
 Services (processes): devmgr · console · serialin
   later: fat32 · netstack (lwIP) · power · audio mixer
 Drivers (processes): usb-bus (xHCI + hubs) → hid
   later: USB mass storage · NIC · HD Audio
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
  `struct boot_info` (all physical addresses) and calls `kmain`.
- Limine modules become **bootfs**, a read-only in-memory FS holding init,
  the services, the drivers and the apps, so user space starts before
  storage works. One module, `bootfs.img` (`tools/mkbootfs.py`): a header,
  an entry table, each file on its own pages. The kernel validates it as
  untrusted input at boot and serves files as physical VMOs over the
  module's pages, without copying.
- The order (`kernel/main.c`): early console, PMM/VMM, heap, bootfs, then
  on the kernel's own stack: ACPI tables (MADT/MCFG/HPET), BSP LAPIC, TSC
  calibration, BSP per-CPU, scheduler (the boot code becomes thread
  "main"), IPIs, IOAPIC, serial interrupts, LAPIC timer, AP startup,
  interrupts on, the PCI core and resources; then either the boot-time
  tests (ktest, bench, stress) or userboot → init.

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
- **Kernel stacks**: `kstack_alloc` (panics) / `kstack_alloc_try` (NULL) map a
  stack under a guard page in the vmap area; `kstack_free` unmaps it (TLB
  shootdown), frees the pages and keeps the virtual range for the next stack
  of the same size, so thread churn neither grows the vmap area nor strands
  page tables.
- **Loader memory** (Limine's stack, tables, and the code the parked APs spin
  in) is reclaimed once every AP has started.
- **VMM**: own 4-level tables (no dependency on the loader's). Kernel image
  mapped per section (text RX, rodata R, data RW+NX), HHDM with 1 GiB/2 MiB
  pages for RAM only (write-back), framebuffer write-combining via PAT index 5.
  W^X holds for every alias of a page, including the HHDM view of the kernel
  image. All 256 kernel-half PDPTs are created up front so every
  address space can share PML4 entries 256-511. Layout:
  `ffff800000000000` HHDM, `ffffc00000000000` vmemmap,
  `ffffd00000000000` vmap (kernel stacks with guard pages, MMIO),
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
  through an iterative teardown (a per-CPU pending list in
  `kernel/object/object.c`), so destroying a channel full of channels never
  recurses.
- Per-process handle table, 65,536 slots. A handle value is
  `(slot + 1) << 15 | generation` (17 + 15 bits; 0 is never valid); freed
  slots are reused FIFO so a stale value takes a long time to come back.
- Rights: `READ WRITE EXEC MAP DUPLICATE TRANSFER SIGNAL WAIT INSPECT
  MANAGE`. `RIGHT_SAME` is only a sentinel for duplicate/replace ("keep the
  rights"). `MANAGE` (jobs only): change limits, kill.
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
  (the rest are dropped and counted).
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
  returns once they are all dead, and the killed jobs take no new
  processes or jobs; init uses it when a program times out, userboot when
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
  (`drivers/include/idl/`). Today: `null` and `edu` (tests), `usbbus` and
  `usb` (usb-bus to devmgr and to class drivers), `input` and `console`.
  Planned: `block`, `fs`, `netdev`, `socket`, `power`, `audio`. Bulk data
  (disk blocks, packets, file contents) moves through a shared VMO ring;
  messages carry offsets. Every protocol defines how a client reconnects
  after `PEER_CLOSED` (the server restarted).

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
| devmgr | the PCI resource | enumeration, driver binding, BAR/MSI/DMA hand-off, supervision, the `usbbus` service to trusted clients; the boot disk's filesystem services and their mounts | yes |
| usb-bus | its PCI device (xHCI) | one `usb` channel per interface; hubs are handled inside it (bus topology, not a class device) | yes |
| hid | a `usb` interface | `input` events (boot keyboard and mouse, keyboard layout) to the console | yes |
| console | the framebuffer, `input`, the kernel log | `console`: a text terminal, and lending the screen to a program | yes |
| serialin | COM1 input | an `input` source (QEMU tests; a spare keyboard if USB breaks) | yes |
| logd | the kernel log, `/data` | each boot's log as a file on the stick | yes |
| USB mass storage (BOT, later UAS) | `usb` | `block` | no |
| fat32 | `block` | `fs` (FAT32 + LFN, read/write) | no |
| NIC: Realtek RTL8125 2.5 GbE | its PCI device (MSI-X, DMA rings) | `netdev` | no |
| netstack | lwIP + `netdev` | `socket` | no |
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
  line). A restart is a bind from scratch, i.e. the safe-rebind path: the
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
- **Authority**: devmgr has two channels, a query channel (look things up)
  and a control channel (change bindings); console clients have a level
  (ADMIN, SHELL, PROGRAM), and a program started from the shell gets a
  PROGRAM channel and nothing of devmgr's.

## Networking

Not built yet; these rules bind every future path that can transmit.

- The NIC is the board's own RTL8125 ([HARDWARE.md](docs/HARDWARE.md#other-devices)),
  driven natively (references: Linux `r8169`, FreeBSD `re`; check whether
  this revision needs Realtek's PHY firmware patch). No USB adapter; the
  Wi-Fi is not planned.
- **Hard requirement: every frame Jam OS sends is tagged 802.1Q VLAN 21,
  and nothing is ever sent untagged or on another VLAN** (the network it
  runs on must not see Jam OS traffic elsewhere). The VLAN is set in one
  place (boot word `vlan=`, default 21) and added to every outgoing frame
  (ARP and DHCP included) below the IP stack. Incoming untagged or
  other-VLAN frames are dropped. With no VLAN configured the NIC stays
  down (fail closed).
- This covers every path that can transmit: the NIC driver, netlog,
  `update`, and a crash kernel if it ever gets networking. A change that
  could transmit comes with a test proving an untagged frame can't leave.

## Userland

- **libos** (`user/lib/`): startup, syscall wrappers, malloc, printf,
  channel/port helpers, `spawn()`, threads, the ELF loader, the file
  namespace and its file calls, and the implementation of
  `<jam/driver.h>`. **libfun** (`user/apps/fun/`): the
  apps' screen, drawing, keys and thread pool.
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
  never write), RESOURCE, DEVMGR, DEVMGR_CTL, CONSOLE, NS (the namespace)
  and program-specific ones (SR_USER + n). `printf` writes to the STDOUT channel when there is
  one, else through `debug_write` (lines prefixed `[process-name]` in the
  kernel log); `debug_report` also puts a line into the RESULTS box.
- **init** holds the root capabilities and starts services with only the
  handles they need: on a plain boot the bootfs server, the console,
  serialin, devmgr, logd (once `/data` is there) and the shell, restarting
  any that die (killing devmgr takes its drivers with its job); for the
  regression run the programs in `boot/init.cfg`. It builds the first namespace (`/boot` at once, `/data`
  and `/esp` when devmgr reports their filesystem services) and gives it
  to what it starts; the shell and logd are sent every later change (a
  mount gone, or back with a new service), each change replacing the one
  they haven't read yet (below). Its control channel (`abi/idl/initctl.idl`) serves
  `kill <name>` and `reboot`, which syncs `/data` first (2 s at most); the
  shell holds one end, the console another that answers only `reboot`
  (Ctrl+Alt+Del).
- **Namespace**: each process has a table of mount point → `fs` channel
  (`/boot`, `/data`, `/esp`), given by whoever started it (startup role
  NS: a channel on which the starter sends the mounts, and later ones to
  a program that is already running). A program reads that channel only
  when it next looks up a path, and some never do again (logd), so a
  starter that follows its mounts for a running program keeps a
  duplicate of the program's end and sends each change as the whole
  namespace (`NS_SET`) after taking back the one not read yet
  (`ns_update`): the program's end holds at most one message however
  often the mounts change, and its next lookup sees the latest
  (`user/include/os.h`, "files", has the protocol). libos finds a path's mount and calls
  that mount's service (`abi/idl/fs.idl`, `abi/idl/file.idl`; file data
  through a shared buffer VMO); `..` never leaves a mount. `/boot` is the
  bootfs image served by a process (`user/services/bootfs/`). No global
  kernel VFS: a program reaches only the mounts it was given, which is the
  only permission system for files. Not built yet: services as paths
  (`/svc/net`, `/dev/console`) and a POSIX `open()` on top.
- **Shell** (`user/services/shell/`): `main.c` is the console I/O, the line
  editor and history; `sh_parse.c` splits a line (`; && || |`, quotes),
  `sh_vars.c` holds variables ($NAME, export -> the environment of `run`)
  and aliases, `sh_exec.c` runs a line (pipes: stages run in turn, each
  one's output captured in memory as the next one's input, by `sh_io.c`; a
  program in a pipe gets an SR_STDOUT channel), `sh_table.c` is the one
  command table (with help), `sh_complete.c` Tab completion, `sh_vfs.c`
  paths and files over libos's namespace (the current directory is the
  shell's own); `cmd/<name>.c` is one file per command.
  System information comes from dedicated syscalls (`sys_info`,
  `cpu_stat`, `proc_list`, `rtc_read`; `RIGHT_READ` on the root resource);
  CPU time is counted per thread and CPU at every switch. The kernel tests
  are shell commands too (`ktest`, `stress 600`), so a test run needs no
  reboot; rebooting is only for loading a new kernel from the stick.
- **Executables**: static ELF64 at 0x400000; no `fork`. `spawn()` loads a
  program from a range of a VMO; code is mapped executable only from a VMO
  handle with `RIGHT_EXEC`, which only the bootfs image's has, so a program
  outside `/boot` can't run yet.

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
  is in software (28 cores and AVX are plenty for 2D at 2560x1440).
- Mode setting and vsync only through the Intel iGPU (documented by
  Intel); the NVIDIA card (GSP firmware, no practical open path) stays a
  plain framebuffer. The IOMMU matters most for GPUs.
- The HID driver handles a mouse as well as a keyboard and sends events
  through a protocol a compositor can take over. Until there is one, the
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
widget graph read and logged (`hda` in the shell), and the path from a
DAC to the front headphone jack: found in the graph by a pure function
(checked at every start against the PC's codec and QEMU's, kept as
fixtures) and set up muted with the pin's output off. Every verb goes
through one file with an allow-list of SET verbs, so the driver can never
write the board's own jack descriptions, GPIOs or vendor coefficients.
Not built yet (the plan is [docs/A1-PLAN.md](docs/A1-PLAN.md)): one
output stream, the unmuting, jack detection, and a mixer service that
owns the device, with programs opening streams and writing samples
through a shared VMO ring.

## Storage

- The USB stick has two FAT32 partitions: the **ESP** (Limine, kernel,
  bootfs), which Jam OS never writes, and a **data partition** mounted at
  `/data` for everything writable. A bug in the FAT32 writer can't make the
  stick unbootable.
- Write ordering: file data, then both FATs, then the directory entry.
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
  2 of type 0x0C: they are `/esp` and `/data`. Each mount is a fat service
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
  `/esp` can never be made writable and `/data` never read-only: init
  passes on nothing but `/usbN`, and devmgr remounts nothing else.
- logd follows the kernel log from its first byte into
  `/data/logs/boot-NNNN.txt`, the next free number each boot, syncing at
  most every 250 ms while the log flows. A reboot loses nothing: init
  syncs the mounts, has logd write out and sync the log up to that line
  (`abi/idl/logctl.idl`), and resets. A panic or a pulled plug loses what
  was logged since the last sync, a quarter of a second at most plus the
  write in flight; the panic's own text is never saved (the crash kernel
  is a later milestone). Without `/data` it waits and tries again; what
  the kernel's 64 KiB ring drops meanwhile, or in a burst faster than the
  stick takes it, is marked in the file as lost.
- The 4 GiB file limit and the lack of owners/permissions are accepted:
  authority comes from namespaces, not the filesystem.

## Debugging

- Framebuffer klog from the first instruction, 64 KiB ring buffer, readable
  from user space through a klog reader handle (the console follows it).
  COM1 too when present.
- Panic screen: message, decoded exception (page-fault cause, NULL and stack
  overflow hints), all registers and control registers, symbolised backtrace
  with repeated frames collapsed, and the log tail. Symbols come from a
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
