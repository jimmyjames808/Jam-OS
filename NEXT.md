# Jam OS: handoff (updated 2026-09-29, M5 phase 1 running)

**M5 started 2026-09-29** (user: "write the m5 plan and then get agents to start building
it"). Plan = M5-PLAN.md. Foundation commit 9160daf (headers uentry/usercopy/aspace/syscall/
startup/bootfs/elf + weak stubs core/m5_weak.c + arch_thread_switch hook + #PF hook). Three
background worktree agents launched: Track A entry path, Track B address spaces/VMAR,
Track C userland/ABI generator/bootfs/ELF. When they report: review each branch, merge
A/B/C into main (expect conflicts in Makefile/limine.conf/sys.h/object.h), rerun all tests
at 4+8 CPUs, then phase 2 (processes, syscall glue, userboot, jobs, utest). The PC 10-min
stress of v0.0.6-m4.5 was running when M5 started; result not yet recorded.


State: M0-M4 done; **M4.5 hardening done in QEMU** (v0.0.6-m4.5). Main commits after the
M4 audit: 6a9d463 (ARCHITECTURE.md rewrite), e179adc (small fixes + tests), 32d2571 (lock
checker fast path / 256 classes / mutexes), 0948ed9 (cancellable waits), then the test
hygiene + version commit. QEMU: 78/78 ktests at 4 and 8 CPUs, stress 30 s passes, every
crash test panics with the right message (testbp continues).

**PC 2026-09-29, "All tests": PASSED** (78/78; m4 836,077 calls/s vs 492,673 on M4, worst
37 us vs 67; locks 48 ns avg / 61 worst per nested pair on 28 CPUs; cancel 118/2000 races
cancelled; TSC-deadline, all 28 CPUs 100 ticks). The stick has v0.0.6-m4.5 + the RESULTS
box (commit after 86097ff). Remaining: the 10-minute stress on the PC.

**(Older) Next for the user (on the PC):** the stick still has the PRE-audit M4 build. When the user
plugs it in, copy build/jamos.elf + boot/limine.conf to "/Volumes/NO NAME" (boot/ and
boot/limine/). Then on the PC: "All tests" (expect `ktest: 78 test(s) passed` and
`M4.5 complete`) and the 10-minute stress (expect PASSED). Also worth a look on 28 CPUs:
the `locks:` line from lock_speed_all_cpus and the `m4:` calls/s (was 492,673 on M4).
After that: mark M4.5 ✅ in ARCHITECTURE.md, start M5. (User 2026-09-29: ignore the blueprint artifact from now on.)

Note: 4-CPU stress throughput in QEMU is bimodal (switches 4k..6M in 10 s, spawns 20..53k)
in the OLD build too; CPU-hog threads at 4 vCPUs. Not a regression; don't chase it.

## Decisions the user made (2026-09-29)
- Plan: **M4.5 hardening first, then M5**.
- Users/logins: **single user, no accounts; handles = authority** (a future "user" is a namespace
  root + a job quota). FAT32 can't store owners anyway.
- Networking (M9): **decide later** (reviewer leans USB CDC-NCM/ECM Ethernet adapter; board may
  have no Ethernet chip at all, only AX201 Wi-Fi was listed).
- Roadmap changes, all accepted:
  - **Reboot in M7**, but as a shell `reboot` command + Ctrl+Alt+Del key. User's point: once M7
    has a keyboard + shell, tests become shell COMMANDS (e.g. `ktest`, `stress 600`), no reboot
    per test; reboot is only needed to load a new kernel build from the stick.
  - **Driver restart (devmgr supervision) from M7**; M11 keeps only the IOMMU (+ interrupt remapping).
  - **Read-only boot partition**: Jam OS never writes the ESP; a separate FAT32 data partition
    holds writable files (M8).
  - **Bulk data via VMOs**: block/fs protocols move data through shared VMO rings + offsets,
    not 64 KiB channel messages.
- Earlier (still standing): drivers brought up in-kernel, moved to userspace in the SAME
  milestone once proven on the PC. No Co-Authored-By trailer on commits. Git author on this
  machine comes from ~/.gitconfig ("paper-audit"); user hasn't asked to change it.

## Step 1: rewrite ARCHITECTURE.md (DONE 2026-09-29, kept for reference)
Record the decisions above, add an **M4.5** milestone row, revise M5/M7/M8/M10/M11 rows, and fix
the drift the conformance reviewer found:
1. Decisions table: Users/logins decided; Networking "decide later (leaning USB Ethernet)";
   add "No fork, ever (posix_spawn later)", "Syscall ABI unstable until M12, generated from one
   table", "Programs print via a stdout channel handle in the startup message; M5 backs it with
   a debug_write syscall, the M7 console service replaces it".
2. Boot order (AD ~79): real order is ACPI, LAPIC, TSC, scheduler, IPI, IOAPIC, timer, then APs
   (main.c ~100-113).
3. Memory: per-CPU page caches are LATER (not M3); TLB = synchronous range shootdown per kernel
   unmap (full flush >64 pages), batching + per-address-space CPU masks come with M5; describe
   VMO kinds (paged sparse 3-level up to 64 GiB, contiguous <=4 MiB, DMA32, physical), pin/map
   guards on decommit/shrink, pins hold refs on VMO + dma_cap, sys-layer dma_cap gate for
   CONTIGUOUS/DMA32; VMAR is M5 (flat address space per process, no nested VMARs, W^X with
   executable mappings needing RIGHT_EXEC).
4. SMP: per-CPU block = current thread, preempt count, irq depth, held locks, watchdog, stats
   (run queues are a static array); `swapgs` arrives in M5; lock checker also refuses nesting
   two locks of the same class unless spin_lock_nested(subclass); delete the smp_run_on_all
   sentence; decide the "handlers never log" rule (klog is irqsave now, so handlers MAY log; the
   LAPIC error handler just counts) and fix klog.c ~26 / trap.c int3 comments to match.
5. Objects: handle value = 17-bit (slot+1) | 15-bit generation, FIFO slot reuse, 65,536 slots;
   rights add INSPECT (RIGHT_SAME is a duplicate/replace sentinel); in-transit slots
   (handle_take reserves -> handle_commit / handle_untake, a failed send never loses a handle);
   iterative teardown (per-CPU pending list in object.c); types: vmar/process/thread/job = M5,
   interrupt/resource = M6.
6. IPC: channel limits (64 KiB, 64 handles, 1024 queued -> ERR_SHOULD_WAIT); cycle rule (can't
   send an endpoint whose own queue holds an endpoint; also own endpoint/peer); channel_call
   txid rules (first 4 bytes, reply only to the caller, late replies queued, ERR_CANCELED if
   own endpoint closes, ERR_BAD_STATE on a closed endpoint); ports: ONCE/PERSISTENT with
   coalescing + count, 4096 user packets, 4096 bindings, no port-to-port, lock rank in port.h;
   "scheduler hands the CPU to the server" = NOT BUILT, move to M5; "IRQs are port packets" = M6.
7. Scheduler: to-do list = channel_call handoff / wake-affine placement (M5), hybrid placement
   order (idle P-core pair > idle E-core > busy HT sibling), per-CPU one-shot timers, user
   priority cap (<= 24 without a capability, M5), stack cache holds 256 (beyond that pages
   should go back to the allocator); timers/sleepers woken by CPU 0's tick (~10 ms).
8. Debugging: ktest registry (KTEST(), `ktest` / `ktest=prefix`, page-leak check that FAILS a
   test, lock classes in use), DBG_HOOK injection points (dbghook.h) used by race regression
   tests, crash-test boot entries.
9. Milestone table: M4 ✅ (QEMU 64/64; PC channels+ports 492,673 calls/s; final build still to
   run on PC); add M4.5; revise later rows as below.
10. Drivers section: devmgr programs MSI-X itself; drivers never get raw ECAM nor the MSI-X table
    page in their BAR mapping; dma_cap bound to a device (PCI BDF), vmo_pin returns "device
    addresses"; closing a dma_cap clears Bus Master Enable then releases that device's pins;
    userspace drivers MSI/MSI-X + MMIO only (no port I/O); IDL protocols define reconnect on
    PEER_CLOSED from M7. NIC row: USB CDC-NCM/ECM adapter (leaning), decide at M9.
11. Userland: init is loaded by a tiny in-kernel ELF loader ("userboot"); everything after that
    via the libos loader; startup message = argv, env, namespace entries, stdio, self/address-
    space handles; no fork.
12. Storage: read-only ESP + FAT32 data partition; write ordering data -> both FATs -> dir entry,
    FAT clean-shutdown bit + check on mount, SCSI SYNCHRONIZE CACHE on sync/unmount; 4 GiB file
    limit and no permissions are fine (authority = namespaces).

## M4.5 hardening (DONE in QEMU 2026-09-29; kept for reference)
Small code fixes (conformance review):
- **W^X hole**: kernel text/rodata writable through the HHDM alias (vmm.c ~256-264 maps
  KERNEL_AND_MODULES RW+NX). Map that region's HHDM alias read-only (or leave text unmapped in
  the HHDM); extend `testro` to also write via phys+hhdm_offset.
- cmdline.c ~27: cmdline_get_u64 returns hard-coded 600 for a bare key, ignoring dflt.
- channel_sys.c ~55-70: received handles are silently dropped if the table is full -> make
  it an error that leaves the message queued (or reserve slots first).
- RIGHT_SIGNAL granted on channels but no user-signal op; object.h ~47 names a nonexistent
  object_signal -> add sys_object_signal for SIG_USER_ALL bits (or drop the right).
- Channels: add a signal/wait for "queue has room again" (SIG_WRITABLE currently = peer alive).
- Tests: ktest leak check has 8 pages slack (tighten if possible); DBG_HOOKs + all tests are
  linked into every kernel (consider a build flag, e.g. `make KTESTS=0`); add tests for
  PORT_MAX_BINDINGS, full handle table / received-handle path, channel_call ERR_CANCELED,
  lockdep IRQ-safety + same-lock-twice, W^X (rodata, NX data/stack, HHDM alias), IST guards.
- thread_create_on (sched.c ~465) reads this_cpu() preemptibly (hint only; make it compliant).
- boot.h ~61/64: cpus[].loader_handle dangles after reclaim -> fix the comment (or clear it).
- Stale text: handle.h ~3 (says <<8), channel.h ~40 (no cycle rule), panic.c ~2 ("IPI" -> NMI),
  README layout (add kernel/object, kernel/abi, kernel/test) + Tests section (ktest, stress,
  lockorder, watchdog, nodeadline, memmap; QEMU_TIMEOUT), limine.conf (M4 label mention VMOs;
  add teststuck/testbp entries), Makefile (.PHONY run-panic, nonexistent `make gdb`),
  untrack 4 .DS_Store files + .gitignore them.
Design items that belong in M4.5 (both needed before processes exist):
- **Lock checker scaling**: lockless fast path (edge bits / irq flags only ever get set, so a
  racy read is safe; take graph lock only for a NEW edge), raise the cap to 256 classes
  (multi-word bitsets), track sleeping mutexes too. Today ~31 of 64 classes are used and every
  spin_lock on all 28 CPUs takes one global test-and-set.
- **Killable/cancellable waits**: per-thread cancel/kill-pending flag; thread_block,
  waitqueue_wait(_until), object_wait_one, port_wait, channel_call, mutex paths return
  ERR_CANCELED when it's set; thread_kill sets it + wakes the thread. Needed for M5 fault
  containment (killing a process blocked in channel_call on a hung server).
Done when: all ktests + new tests pass at 4 and 8 CPUs, stress passes, crash tests still panic,
then the user runs M4 tests + 10-min stress on the PC.

## M5 scope additions from the architecture review
- **Jobs + quotas**: job object with committed-page / handle / thread limits, inherited by
  children; charge queued channel messages to the sender's job (closes TODO(M5)/O3c in
  channel.c ~96). Every syscall-reachable allocation returns ERR_NO_MEMORY instead of panicking
  (thread_alloc, alloc_table, kstack_alloc, the kmalloc in smp_call_others). Processes are
  created from a job handle; only a `resource` handle grants hardware access.
- **VMO user mappings**: keep pins blocking decommit (DMA), but user mappings use a per-VMO
  reverse map so decommit/shrink unmap from every address space (else malloc can't free).
  Lock order: address-space region lock = sleeping mutex ranked above "vmo"; page-table lock =
  spinlock below "vmo". Per-address-space active-CPU mask + "gather" (free pages after flush).
  Revisit TODO(O8) (vmo.c ~488).
- **Entry path**: SMEP + SMAP from day one, stac/clac user copy with an exception fixup table;
  canonical-RIP check before sysret (or iretq); NMI/#MC/#DB detect GS themselves (watchdog NMI
  can land in user mode / the swapgs window); eager XSAVE per thread; channel_read user-copy
  must still release handles on a bad user buffer; channel_call must stamp the txid into the
  kernel copy, not the user buffer.
- channel_call handoff / wake-affine placement before services are built on it.
- User priority cap (<= 24) unless a capability allows higher.
