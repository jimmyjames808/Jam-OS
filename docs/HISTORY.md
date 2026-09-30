# History

What each milestone delivered, the bugs worth remembering, and the lessons.
Newest first. Status and what comes next are in [ROADMAP.md](ROADMAP.md);
the finished plans are kept as written in [history/](history/). Versions
are the kernel's version string; hashes are commits on main.

## Lessons that keep coming back

- **QEMU is not the PC.** QEMU's TCG has no PCIDs and no TSC-deadline
  timer, so those ran for the first time on the PC. Several bugs only the
  PC could show (M6's BAR sizing, M5's slow-console rate limit). QEMU also
  can't measure page-allocator scaling: stores to some physical pages cost
  100-400x a normal page there, which moves page-related numbers by up to
  30x between builds ([BENCH.md](BENCH.md)).
- **Stress runs find the races.** The 10-minute stress found the lock
  checker's IRQ race (M3), the per-CPU access rule (a thread reading
  another CPU's current thread through `this_cpu()` from preemptible
  code), and a job credited after its unlock (M5).
- **A reviewer who didn't write the code finds what the author can't.**
  M5's review found four quota escapes; M7's found that a `run` program
  could hijack the keyboard. Big changes now always get an independent
  review after the merge.
- **Kernel-first bring-up bought nothing.** M6's xHCI driver behaved the
  same as a kernel process and as a user process, while the process path
  gave better crash reports and could always be killed. Since then drivers
  and services are processes from the start.
- **Measure before optimising.** M5.5's benchmark showed PCIDs saved only
  2% of a process-to-process call; the cost was on the user side of the
  boundary.

## M8: storage

*2026-09-30 to 2026-10-01, 0.0.25-m8.*

Plan: [M8-PLAN.md](history/M8-PLAN.md); review:
[M8-REVIEW.md](history/M8-REVIEW.md). USB mass storage and FAT32, all in
user processes:
- `drivers/usb-storage` (Bulk-Only Transport over new bulk transfers in
  usb-bus) serves a `block` channel per partition; read-only channels are
  refused writes in the driver itself.
- FatFs R0.16 (third_party/fatfs, with its author's patches) as the `fat`
  service, one per volume: the ESP read-only at `/esp`, the data partition
  at `/data`, formatted on first use (only the boot stick's own blank
  data partition is ever formatted).
- devmgr finds disks and partitions and publishes mounts; init keeps the
  system's file namespace and sends each program its mounts (as one
  whole-namespace message, so a program that never looks is never
  flooded); programs open files by path; the shell got `ls cat cp mv rm
  mkdir touch write df sync mount`.
- logd saves each boot's log to `/data/logs/boot-NNNN.txt`, syncing every
  250 ms and flushing before a reboot.
- Other sticks mount read-only at `/usb0`, `/usb1`, ... (`mount -w` to
  write); a stick with no partition table works too.
- `make usb` makes the two-partition stick; `make flash` updates one in
  place (macOS doesn't mount an MBR ESP by itself).

Tracks A (usb-storage), B (fat), C (namespace, init, shell) and D (devmgr,
logd) in parallel, a join, other sticks, then the review (28 findings;
two High: fat mapped a client's buffer, so any program with `/data` could
crash it; and a partitioned stick whose first sector looked like a FAT
volume was served whole). The PC rounds found more: a second `ktest` in
one boot always panicked (a test's static fake CPUs), the boot log lost
its tail, two leaks (mount notices nobody read, 30 KB per utest run; the
user heap never merged freed blocks), a global fault hook that failed
other threads' process starts, and a placement test failing on the PC
that turned out to be two scheduler bugs (a CPU taking its next thread
looked idle to placement; stealing ignored the core layout). The soak
test (`ktest loops= seed= keep load`, `soak`) was built to find this
kind of bug, and replaced the stress test as the PC's tier.

On the PC (i7-14700K class Raptor Lake, microcode 0x11f: the INVLPG/PCID
erratum is fixed there, so PCIDs stay on): the stick's `/esp` and
`/data`, a SanDisk at `/usb0` read and written (macOS's check clean
afterwards), the boot stick pulled mid-soak and replugged (the soak
passed, the volume came back dirty and was mounted), the logs read on the
Mac; All tests 224 passed (27 clients: 975,166 calls/s), `stress 600`
passed, `soak 10` passed (645 s, 21 loops: 4285 kernel tests, 19 utest
runs and 1889 file cycles, 0 failed; the SanDisk pulled twice).

## M7.5: cleanup

*2026-09-30.*

Plan: [CLEANUP-PLAN.md](history/CLEANUP-PLAN.md). Readability and
structure with no behaviour change: same commands, output, boot menu,
ktest names, syscall numbers and IDL; the version string stayed 0.0.24-m7.
Five parallel tracks, then one pass for the moves that would collide:

- The kernel build of drivers went (the kernel-process machinery,
  `drivers=kernel`, the xhci-noop driver and its `xhcitest` entry, the
  weak-symbol stubs: a missing syscall handler is now a link error).
  `<jam/driver.h>` has one implementation and gained `drv_snprintf`.
- The shell got one command table and one file per command; usb-bus's
  1834-line enumeration file was split by step; the kernel was split by
  subsystem (`sched`, `proc`, `debug`) with tests named by subject; user
  programs by role (`services`, `apps`, `tests`) on libos and libfun.
- CODING-GUIDE.md; `-Wvla` everywhere and `-Wframe-larger-than=3072` for
  the kernel.
- 66,065 lines in 237 files became 64,094 in 326; files over 800 lines
  20 -> 13; ktests 227 -> 219 (the kernel build's tests went with it).

Bugs found and fixed on the way: the shell wrote one past its segment array
for 32 segments ending in an unclosed quote; usb-bus left a slot enabled on
an out-of-range slot id, parsed an alternate setting without its
SuperSpeed companion, and didn't clamp `bMaxBurst`; the dma_cap quarantine
counters could be read mid-update (an 8-CPU ktest failure); Tab completion
ignored the line cap.

After the tracks: an independent review (no High findings), then two
style passes that brought kernel/, drivers/ and user/ in line with
CODING-GUIDE.md (every struct field commented, nesting over 3 levels gone,
`volatile` kept for device and loader memory with explicit atomics
elsewhere, the long functions split into named steps) and a docs overhaul
(one home per fact, `tools/checkdocs.py`). The repository went public on
GitHub under the BSD-2-Clause licence.

Behaviour changes accepted on the way (not bugs): usb-bus now counts every
endpoint descriptor of an alternate setting against its 8-endpoint limit
and uses only the first copy of a duplicated (interface, alternate)
descriptor; the demo, rebuilt on libfun, refuses to start without the
keyboard, needs a screen of at least 320x200 and names its threads
"worker"; `sys_pci_enum`'s `dma_quarantined` also counts batches that are
mid-release.

The last fix round of the day: the flaky devmgr ktest (it read devmgr's
job usage before devmgr was back asleep), an unbounded HPET calibration
loop that could hang the boot, `stress` failures that stuck across runs,
a devmgr handle leak, the console letting one flooding client starve Ctrl+C
and the others, the console ignoring failed port binds, and the shell's
pipe drain (no limit, deaf to Ctrl+C, stuck on an oversized message).
ktests 219 -> 221.

Signed off on the PC the same day: All tests 221/221, the shell's `ktest`
212 passed, the 10-minute stress passed. The PC round found two tests that
were wrong, not the code: `stress_failure_does_not_stick` saw 4 pages kept
(the first stress run on 28 CPUs grows per-CPU caches to a high-water
mark; the test now checks a third, identical run gives every page back,
through the new `KT_OWN_LEAK_CHECK`), and `serial_irq_drains_ring` expected
the shared COM1 ring to empty while the console kept it full from the shell
(its counts are now boot-menu-only checks).

## M7: USB, console, shell

*2026-09-29, 0.0.18-m7a to 0.0.24-m7.*

Plan: [M7-PLAN.md](history/M7-PLAN.md). Four tracks, then the join:

- **usb-bus**: xHCI, hubs (inside usb-bus, as Linux's usbcore does),
  Transaction Translator routing, hot-plug. On the PC it enumerated all 8
  devices, 0 failed, the first time.
- **hid**: boot keyboard and mouse on each interface of composite devices,
  US layout, key repeat, Ctrl+Alt+Del.
- **console** owns the framebuffer (the kernel's log mirrored in it) and
  **shell**: line editing, history, then ~60 commands with pipes,
  variables, aliases, Tab completion and Ctrl+C.
- Kernel services: reading the kernel log, the framebuffer hand-off,
  `debug_command` (ktest, bench and stress from the shell), `reboot`,
  COM1 input.
- **Supervision and safe rebind**: devmgr restarts dead drivers with
  backoff; only a driver that quiesced its device turns bus mastering back
  on; pins a dead driver left are quarantined. This fixed M6's confirmed
  stale-DMA-after-rebind finding.
- Apps with real pixels through `console.lend_screen` (0.0.24-m7): fractal
  (AVX2, deep zoom to 1e28 with double-double perturbation), life, tetris.

On the PC: typing into the shell with the real keyboard, `kill hid-10:0`
recovers, reboot and Ctrl+Alt+Del. The PC run found that init didn't
restart devmgr (killing it lost USB until a reset): fixed, devmgr is back in
~0.6 s. The review found that kill never reached ancestor jobs, a set of
usb-bus robustness gaps, and an authority hole: programs started with `run`
got devmgr's channel and a full console channel (keyboard hijack,
Ctrl+Alt+Del, keylogging). Fixed with separate query and control channels
for devmgr and console client levels (ADMIN, SHELL, PROGRAM). `ktest` from
the shell panicked on a busy system (global counts), so tests now relax
system-wide checks when run live. Signed off by `stress 600` from the shell
on 0.0.23-m7: 0 failures. On 0.0.24-m7 `ktest starvation` passed on the
PC (so 0 anti-starvation boosts in PC stress runs is legitimate: 28 CPUs
steal waiting threads first).

## M6: PCI, MSI, devmgr, drivers through handles

*2026-09-29, 0.0.10-m6a to 0.0.17-m6.*

Plan: [M6-PLAN.md](history/M6-PLAN.md). The kernel's PCI core (ECAM, BAR
sizing, MSI/MSI-X, Bus Master Enable), per-CPU vectors and interrupt
objects, resources, physical VMOs, `dma_cap` bound to one function, the
config-write filter, `<jam/driver.h>`, the IDL generator, the driver build
check, devmgr, and the `edu` (QEMU) and xhci-noop drivers.

The done test passed on the PC: the Intel xHCI completed No-Op commands
with a real MSI, both as a kernel process and as a user process (median 46
us: the controller's 40 us interrupt moderation). Bugs only the PC showed:
a test assumed QEMU-only devices; a BAR neighbour check was too strict; and
the VMD controller's 64-bit BAR has a hard-wired-zero upper half, which
`pci_size_bars` sized as ~2^64 (now implausible sizes are logged and left
unsized). A pre-existing object teardown race was fixed on the way. The
second review confirmed stale DMA after a rebind, fixed in M7. All tests
201/201; one 10-minute stress (0 failures) signed off M5.5 and M6 together.

## M5.5: performance pass

*2026-09-29, 0.0.9-m5.5.*

Spin before idle, hybrid P/E placement, client/server pairs on sibling
hyperthreads, per-CPU kmalloc magazines, per-CPU one-shot timers,
interrupt-driven serial output, PCIDs and XSAVEOPT; each with a run-time
switch and a boot word, and the benchmark measures each line off and on in
one run. On the PC ([BENCH.md](BENCH.md)): cross-CPU wakeups 2-2.4x faster,
kmalloc 4.3x faster (about 800x with 28 CPUs at once), timers 350 ns late
instead of up to 10 ms, and M5's cross-CPU regression gone (a wake-affine
scan of every CPU's struct). PCIDs gave only 2%.

The first real PCID run passed after fixing a test-only expectation. The
review found a far-future deadline that wrapped (a user-triggerable timer
storm), a redundant spin-idle IPI and a missing PCID flush after a switch
flip, and fixed an old flake ("counter#14 made no progress"): mutexes now
hand off to the longest waiter after 1 ms.

## M5: ring 3, processes, init

*2026-09-29, 0.0.7-m5-phase1 to 0.0.8-m5.*

Plan: [M5-PLAN.md](history/M5-PLAN.md). Phase 1 in three tracks: the entry
path (`syscall`/`sysret`, `swapgs`, SMEP/SMAP, user copies with a fixup
table, eager XSAVE), address spaces and VMARs, and the userland build (the
syscall table generator, bootfs, the ELF loader). Phase 2: processes,
threads, jobs and quotas, userboot, init, utest, per-CPU page stashes and
wake-affine `channel_call`.

The phase-1 build passed All tests and the 10-minute stress on the PC:
the first ring-3 code there. The review found no crash or cross-process
access but four quota escapes (VMO table pages and user page tables not
charged, unbounded job chains, a process raising its own job's limit) and
four smaller issues; the fixes added `RIGHT_MANAGE`, a job depth cap, a
charge for every kernel allocation a process can cause, and `job_kill`.
The PC then found two more: a test race with lazy thread reaping, and a
dead process's thread credited to its job after the unlock (the stress
failed at 14 s). Final PC run: init + utest with the root job clean, All
tests 131/131, the benchmark, 10-minute stress 0 failures. Per-CPU page
stashes took page alloc+free with 28 CPUs at once from 19 us to 24 ns.

## M4.5: hardening

*2026-09-29, 0.0.6-m4.5.*

From a conformance review: W^X also through the HHDM alias of the kernel
image; received handles no longer dropped when the table is full; a
channel "has room" signal; the lock checker got a lockless fast path, 256
classes and sleeping mutexes; cancellable waits (needed to kill a process
blocked in `channel_call`); `make KTESTS=0`. On the PC: 78/78, one server
with 27 clients at 836,077 calls/s (M4: 492,673), worst call 37 us,
10-minute stress passed.

## M0 to M4

*2026-09-28.*

- **M0**: toolchain, QEMU q35 + OVMF, the USB image, framebuffer console,
  panic screen; booted on the PC.
- **M1**: physical and virtual memory, heap, GDT/TSS/IDT, the panic screen
  with symbols.
- **M2**: ACPI tables, LAPIC (x2APIC and xAPIC), IOAPIC, TSC and APIC
  timers, all cores, P/E topology, loader memory reclaimed. On the PC: 28
  CPUs, each exactly 100 ticks, TSC-deadline. xAPIC mode only reached 20
  CPUs, so x2APIC it is.
- **M3**: scheduler, kernel threads, ticket spinlocks and the lock-order
  checker, IPIs, TLB shootdown, watchdog, the stress test. The first PC
  stress failed at 230 s on an IRQ race in the checker; after the fix the
  10-minute stress passed (112 threads, 28 CPUs).
- **M4**: objects and handles, channels, ports, events, timers, VMOs; a
  two-part audit and 20 fixes. PC: 492,673 calls/s, worst 67 us.

## Decisions

Dated decisions, newest first. The design they produced is in
[ARCHITECTURE.md](../ARCHITECTURE.md); this is the when and why.

- 2026-09-30: storage stays as planned: the USB stick and FAT32 only. An
  NVMe driver (easier, and the internal Crucial drive already has a FAT
  partition from an earlier attempt) and other filesystems (exFAT,
  ext2, littlefs, an own copy-on-write one) were discussed and declined.
  FAT32 comes from a FatFs port, not a hand-written driver.
  The repository went public under BSD-2-Clause; no GPL code may be copied.
- 2026-09-30: the kernel build of drivers is removed (nothing used it once
  devmgr ran every driver as a process).
- 2026-09-29, after M7: the audio track comes right after M8. SSH was
  discussed and not added. The boot menu became Jam OS / safe mode / a
  Tests folder; everything else is a shell command.
- 2026-09-29, after M6: drivers and services are processes from the start
  (writing drivers in the kernel first turned out to buy nothing; M6
  showed it). This replaced "bring up in the kernel, move
  out in the same milestone".
- 2026-09-29, after the M6 device listing: networking uses the board's own
  RTL8125 natively, not a USB Ethernet adapter (the earlier lean, when it
  wasn't known the board had a wired NIC); every frame on VLAN 21 only.
- 2026-09-29: the roadmap gained M4.5 (hardening before processes), M5.5
  (performance), M8.5 (crash kernel and kexec), M13 (self-hosting) and the
  graphics and audio tracks. Reboot became a shell command plus
  Ctrl+Alt+Del in M7 (once there is a shell, tests are commands and need no
  reboot); driver restart moved to M7 (M11 keeps only the IOMMU); the boot
  partition is read-only to Jam OS, with a separate data partition; bulk
  data moves through shared VMOs, not large channel messages.
- 2026-09-29: single user, no accounts; handles are the only authority.
- 2026-09-28: the base choices (C, Make, Limine behind `struct boot_info`,
  SMP from day one, capability handles, no `fork`, FAT32 only).
