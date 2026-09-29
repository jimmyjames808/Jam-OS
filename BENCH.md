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
