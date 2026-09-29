# Benchmark baselines

"Benchmark" in the boot menu (`bench`); method in `kernel/test/bench.c`.
Each entry: median / p99 of 4000 samples, kernel threads only, lock checker
on. Record a new column for every milestone run on the real PC, so a
regression shows up as a number, not a feeling.

PC: Intel Core i7-14700 (8 P-cores with HT + 12 E-cores = 28 CPUs), TSC 2112
MHz. CPUs used: P = cpu2, P2 = cpu4 (another P-core), HT = cpu3 (P's
sibling), E = cpu16.

| Benchmark | M4.5, 2026-09-29 |
|---|---|
| timestamp cost (subtracted) | 8.9 / 9.9 ns |
| spin_lock + spin_unlock, uncontended | 26.4 / 27.5 ns |
| kmalloc(64) + kfree | 109.2 / 112.6 ns |
| page alloc + free, one CPU | 53.4 / 56.6 ns |
| page alloc + free, all 28 CPUs at once | **19 us / 19 us** (one global buddy lock; per-CPU caches planned in M5) |
| context switch (yield, 2 threads, same CPU) | 28.7 / 29.7 ns |
| block+wake round trip, same CPU | 442.1 / 445.0 ns |
| channel_call round trip, same CPU | 622.5 / 638.1 ns |
| cache-line round trip P->HT / P->P2 / P->E | 37.8 / 77.1, 108.8 / 112.6, 103.6 / 106.9 ns |
| block+wake round trip (idle CPU) P->HT / P->P2 / P->E | 1081.7 / 1127.6, 1525.3 / 1596.8, 1904.5 / 2855.1 ns |
| IPI function call round trip P->HT / P->P2 / P->E | 347.0 / 575.6, 699.7 / 776.8, 1205.3 / 2242.0 ns |
| channel_call round trip, 1 client, P->HT / P->P2 / P->E | 1267.8 / 1324.6, 2003.4 / 2069.2, 2333.4 / 3727.1 ns |
| channel_call round trip, P client, server unpinned / server not on P | (new in M5: placement decides; wake-affine should bring these to about the same-CPU and P->HT lines) |
| TLB shootdown, 1 page, 27 other CPUs | 5646.8 / 6359.8 ns (old all-CPU path) |

Other PC numbers from the same build ("All tests"): 1 server + 27 clients,
836,077 channel_calls/s, worst call 37 us; nested lock pair on 28 CPUs at
once 48 ns.

Notes
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
