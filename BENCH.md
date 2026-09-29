# Benchmark baselines

"Benchmark" in the boot menu (`bench`); method in `kernel/test/bench.c`.
Each entry: median / p99 of 4000 samples, kernel threads only, lock checker
on. Record a new column for every milestone run on the real PC, so a
regression shows up as a number, not a feeling.

PC: Intel Core i7-14700 (8 P-cores with HT + 12 E-cores = 28 CPUs), TSC 2112
MHz. CPUs used: P = cpu2, P2 = cpu4 (another P-core), HT = cpu3 (P's
sibling), E = cpu16.

| Benchmark | M4.5, 2026-09-29 | M5 (0.0.8-m5), 2026-09-29 |
|---|---|---|
| timestamp cost (subtracted) | 8.9 / 9.9 ns | 8.5 / 9.9 ns |
| spin_lock + spin_unlock, uncontended | 26.4 / 27.5 ns | 26.6 / 27.8 ns |
| kmalloc(64) + kfree | 109.2 / 112.6 ns | 81.8 / 84.5 ns |
| page alloc + free, one CPU | 53.4 / 56.6 ns | **19.0 / 19.5 ns** (per-CPU stash) |
| page alloc + free, all 28 CPUs at once | 19 us / 19 us | **24.0 / 25.5 ns** (~800x) |
| context switch (yield, 2 threads, same CPU) | 28.7 / 29.7 ns | 30.2 / 30.5 ns |
| block+wake round trip, same CPU | 442.1 / 445.0 ns | 471.0 / 495.6 ns |
| channel_call round trip, same CPU | 622.5 / 638.1 ns | 623.9 / 655.6 ns |
| cache-line round trip P->HT | 37.8 / 77.1 ns | 35.9 / 75.2 ns |
| cache-line round trip P->P2 | 108.8 / 112.6 ns | 97.0 / 98.9 ns |
| cache-line round trip P->E | 103.6 / 106.9 ns | 96.5 / 97.9 ns |
| block+wake round trip (idle CPU) P->HT | 1081.7 / 1127.6 ns | 1102.1 / 1142.3 ns |
| block+wake round trip (idle CPU) P->P2 | 1525.3 / 1596.8 ns | 1539.5 / 1588.3 ns |
| block+wake round trip (idle CPU) P->E | 1904.5 / 2855.1 ns | 1900.2 / 2696.0 ns |
| IPI function call round trip P->HT | 347.0 / 575.6 ns | 344.1 / 571.8 ns |
| IPI function call round trip P->P2 | 699.7 / 776.8 ns | 583.2 / 660.8 ns |
| IPI function call round trip P->E | 1205.3 / 2242.0 ns | 1154.1 / 2135.5 ns |
| channel_call round trip, 1 client, P->HT | 1267.8 / 1324.6 ns | 1293.3 / 1325.0 ns |
| channel_call round trip, 1 client, P->P2 | 2003.4 / 2069.2 ns | 2166.3 / 2229.3 ns (+8%, watch) |
| channel_call round trip, 1 client, P->E | 2333.4 / 3727.1 ns | 2582.4 / 3841.7 ns (+11%, watch) |
| channel_call, P client, server unpinned | (new) | **623.0 / 655.2 ns** (= same-CPU line: wake-affine) |
| channel_call, P client, server not on P | (new) | **1292.8 / 1326.5 ns** (= P->HT line: idle sibling) |
| TLB shootdown, 1 page, 27 other CPUs | 5646.8 / 6359.8 ns | 4975.5 / 6064.4 ns |
| address-space switch (CR3 load + masks, P) | (new) | 46.5 / 47.0 ns |
| user: syscall round trip (unused number, P) | (new) | 30.0 / 30.4 ns |
| user: clock_get syscall (P) | (new) | 44.8 / 44.9 ns |
| user: page fault, fresh zero page (P) | (new) | 744.2 / 959.1 ns |
| user: process->process channel_call, same CPU (P) | (new) | 1489.8 / 1506.8 ns |
| user: process->process channel_call P->HT | (new) | 1890.8 / 1919.6 ns |
| user: process->process channel_call P->P2 | (new) | 2956.4 / 3054.9 ns |
| user: process->process channel_call P->E | (new) | 3619.7 / 3975.7 ns |

M5 numbers were read from a photo of the RESULTS box (IMG_0064), every digit legible.

Other PC numbers from the M4.5 build ("All tests"): 1 server + 27 clients,
836,077 channel_calls/s, worst call 37 us; nested lock pair on 28 CPUs at
once 48 ns.

Notes
- M5 reading: user code pays little for the kernel boundary itself (a
  syscall round trip is 30 ns; Jam OS has no Spectre/Meltdown mitigations,
  which cost Linux most of its syscall time, so don't compare 1:1). A
  process->process call on one CPU costs ~870 ns more than the kernel-thread
  version: two address-space switches without PCIDs (so the TLB refills),
  XSAVE/XRSTOR of 832 bytes per switch, syscalls and user copies. PCIDs are
  M5.5. The cross-CPU channel_call lines got 8-11% slower with the M5 code
  (both threads pinned, so wake-affine can't apply): check again in M5.5.
- The first PC run printed the P->HT tests as skipped (P was cpu1, whose
  sibling is cpu0, which the benchmark avoids); fixed in 28a9f99. The
  context-switch number was the same before and after that fix, which
  divides by the switches the scheduler actually counted.
- Cross-CPU wakeups go to an idle CPU waiting in `hlt`, so they include
  its wake-from-halt time.
- QEMU (TCG on the Mac) can't show the all-CPU page line's contention. Its
  numbers are dominated by an emulator artifact: stores to some physical
  pages reclaimed from the loader/firmware (probably ones QEMU once
  translated code from) cost 200-700 ns each, 100-400x a normal page, and
  which pages a benchmark lands on moves every page-related line (kmalloc,
  channel_call, the M4 calls/s) by up to 30x between builds. With loader
  reclaim switched off, the old global-lock allocator also measured the same
  on 8 vCPUs as on one. Page-allocator scaling has to be measured on the PC.

## M5.5 (expected)

Built 2026-09-29, not yet run on the PC. Every M5.5 optimisation has a
run-time switch, and a line it should move is measured with the switch off
and then on in the same run, printed as `<switch> off median/p99 on
median/p99` (units inline). The "off" half is the M5 behaviour, so it should
match the M5 column; the "on" half is the M5.5 number. Switches (boot word
to disable): spinidle (`nospinidle`), placeorder (`noplaceorder`),
affinepair (`noaffinepair`), kmcache (`nokmcache`), oneshot (`nooneshot`),
serialirq (`noserialirq`), fpuopt (`nofpuopt`), pcid (`nopcid`); `m55` flips
all of them at once.

| Line | Switch | Expected on the PC |
|---|---|---|
| kmalloc(64) + kfree (P) | kmcache | ~82 ns -> ~15-25 ns (no lock, no lockdep) |
| kmalloc(64)+kfree, all 28 CPUs at once (new) | kmcache | lock collapse (us) -> same as one CPU |
| block+wake round trip (idle CPU) P->HT/P2/E | spinidle | wake-from-halt gone: ~1.1/1.5/1.9 us -> a few hundred ns |
| IPI function call round trip P->x | spinidle | somewhat faster (target polling, not halted) |
| channel_call round trip P->x, 1 client | spinidle | lower by the halt exit on both sides; P->P2/P->E also recover the M5 regression (see below) with the switch in either position |
| block+wake round trip P->unpinned partner (new) | affinepair | off ~ P->P2 line, on ~ P->HT line |
| placement of 19 busy threads (new) | placeorder | off: ~8 share a core, 4 on E; on: 0 share, 12 on E |
| serial_write of a 100-character line (new) | serialirq | ~8.7 ms -> a few us (one port write + a copy) |
| sleep 100 us / 1000 us: how late it wakes (new) | oneshot | off: 0-10 ms (tick); on: tens of us |
| XRSTOR + XSAVE of user FPU state (new) | fpuopt | XSAVE -> XSAVEOPT of an init-state area: cheaper save |
| address-space switch (CR3 load + masks) | pcid | 46.5 ns -> lower (no TLB flush on the load) |
| user: process->process channel_call, same CPU | pcid | ~1490 ns -> lower by the TLB refills |
| user: thread->thread channel_call, 1 process (new) | fpuopt | the same call without CR3 switches (breakdown) |
| user: process->process channel_call P->x | m55 | all of the above together |

Unchanged lines (no switch): timestamp, spin_lock, page alloc (one CPU and
all CPUs), context switch, same-CPU block+wake and channel_call, cache-line
round trips, the two wake-affine placement lines, TLB shootdown, user
syscall / clock_get / page fault. Each off/on line flips only its own
switch; the others stay as booted (all on), so an "off" half is M5 for that
feature only (e.g. the channel_call P->P2 "spinidle off" half already has
the placement fix). Compare each on half with its off half.

Investigations:
- (b) Pinned cross-CPU channel_call +8-11% in M5: the M5 wake-affine code
  (select_cpu_affine) found the waker's HT sibling by scanning every CPU's
  struct cpu (core_id, online, current). With client and server pinned to
  different cores both the request and the reply fall through that scan and
  then select_cpu: ~2 x 28 cache-line reads per round trip (+163 ns P->P2,
  +249 ns P->E; the P->HT scan stops at cpu 3, +25 ns). Fixed: a
  read-mostly topology table, an online mask, and placement reads only the
  CPUs a thread may use (a pinned thread reads one run queue). Expect the
  P->P2 and P->E lines back at or below their M4.5 values.
- (a) process->process channel_call on one CPU costs ~870 ns more than the
  kernel-thread version. The run now prints the pieces: the thread->thread
  line (same calls, no CR3 switches) minus the kernel line is syscalls
  (5 per round trip, ~30 ns each by the syscall line), user copies, handle
  lookups and FPU; the XRSTOR+XSAVE line times the FPU part (2 of each per
  round trip); process->process minus thread->thread is the two CR3
  switches plus the TLB refills, which PCIDs remove (pcid off/on on the
  same line). The user echo server also does one failed channel_read per
  round trip that the kernel server doesn't (it reads before waiting).
