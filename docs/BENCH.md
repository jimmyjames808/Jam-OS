# Benchmark baselines

"Benchmark" in the boot menu, or `bench` in the shell; the method is in
`kernel/test/bench.c`.
Each entry: median / p99 of 4000 samples, kernel threads only, lock checker
on. Record a new column for every milestone run on the real PC, so a
regression shows up as a number, not a feeling.

The PC is described in [HARDWARE.md](HARDWARE.md) (28 CPUs: 8 P-cores with
Hyper-Threading + 12 E-cores). CPUs used: P = cpu2, P2 = cpu4 (another
P-core), HT = cpu3 (P's sibling), E = cpu16.

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

The same lines from the M8 sign-off build's All tests, 2026-10-01 (commit
36372eb; its version string still read 0.0.24-m7; read from a photo of the
RESULTS box, IMG_0085): 1 server + 27 clients, 54,000 calls in 55 ms =
975,166 channel_calls/s, average call 27,603 ns, worst 31 us; nested
lock+unlock pair on 28 CPUs at once: average 51 ns, worst CPU 64 ns. The
benchmark itself had not been run on the PC since M5.5 until M8.6's
sign-off (the last section).

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

## M5.5 (0.0.9-m5.5), PC 2026-09-29

Read from a photo of the RESULTS box (IMG_0066), every digit checked zoomed.
Every M5.5 optimisation has a run-time switch, and a line it should move is
measured with the switch off and then on in the same run, printed as
`<switch> off median/p99 on median/p99`. Switches (boot word to disable):
spinidle (`nospinidle`), placeorder (`noplaceorder`), affinepair
(`noaffinepair`), kmcache (`nokmcache`), oneshot (`nooneshot`), serialirq
(`noserialirq`), fpuopt (`nofpuopt`), pcid (`nopcid`); `m55` flips all of
them at once. Header lines: `fpu: XSAVEOPT ... pcid=1/1 invpcid=1`,
`timer: TSC-deadline, one-shot timers at 100 Hz ... all ok`.

| Line | Switch | off (median / p99) | on (median / p99) | M5 median |
|---|---|---|---|---|
| kmalloc(64) + kfree (P) | kmcache | 53.0 / 53.4 ns | **19.2 / 19.3 ns** | 81.8 ns |
| kmalloc(64)+kfree, all 28 CPUs at once (new) | kmcache | 19.2 / 19.6 us | **23.9 / 29.2 ns** (~800x) | - |
| block+wake round trip (idle CPU) P->P2 | spinidle | 1387.1 / 1436.3 ns | **1003.6 / 1048.1 ns** | 1539.5 ns |
| block+wake round trip (idle CPU) P->HT | spinidle | 1085.5 / 1113.4 ns | **480.9 / 691.1 ns** | 1102.1 ns |
| block+wake round trip (idle CPU) P->E | spinidle | 1773.8 / 2384.5 ns | **943.9 / 1172.6 ns** | 1900.2 ns |
| block+wake round trip P->unpinned partner (new) | affinepair | 1079.8 / 1219.5 ns | **470.0 / 695.9 ns** | - |
| IPI function call round trip P->P2 | spinidle | 589.8 / 658.5 ns | **418.4 / 611.1 ns** | 583.2 ns |
| IPI function call round trip P->HT | spinidle | 348.9 / 388.6 ns | **268.4 / 514.6 ns** | 344.1 ns |
| IPI function call round trip P->E | spinidle | 1130.9 / 2504.3 ns | **474.3 / 1201.9 ns** | 1154.1 ns |
| channel_call round trip P->P2, 1 client | spinidle | 1798.9 / 1929.1 ns | **1385.6 / 1558.0 ns** | 2166.3 ns |
| channel_call round trip P->HT, 1 client | spinidle | 1240.8 / 1272.5 ns | **586.0 / 1006.4 ns** | 1293.3 ns |
| channel_call round trip P->E, 1 client | spinidle | 2226.4 / 3255.1 ns | **1418.8 / 1693.8 ns** | 2582.4 ns |
| placement of 19 busy threads (1 per core but cpu0's) (new) | placeorder | 15 share a core, 4 on E | **0 share, 12 on E** | - |
| serial_write of a 100-character line (P) (new) | serialirq | 8948.0 / 8974.0 us | **6514.1 ns / 17.7 us** | - |
| sleep 100 us (P): how late it wakes (new) | oneshot | 9899.9 / 9900.0 us | **349.8 / 414.7 ns** | - |
| sleep 1000 us (P): how late it wakes (new) | oneshot | 8999.9 / 9000.0 us | **348.8 / 436.4 ns** | - |
| XRSTOR + XSAVE of user FPU state (832 B, P) (new) | fpuopt | 46.4 / 47.6 ns | **39.9 / 40.3 ns** | - |
| address-space switch (CR3 load + masks, P) | pcid | 66.6 / 66.9 ns | 67.9 / 68.2 ns | 46.5 ns (**+20 ns, see below**) |
| user: process->process channel_call, same CPU (P) | pcid | 1436.3 / 1453.8 ns | 1406.9 / 1433.4 ns | 1489.8 ns |
| user: thread->thread channel_call, 1 process (P) (new) | fpuopt | 1301.4 / 1327.4 ns | 1284.8 / 1317.0 ns | - |
| user: process->process channel_call P->P2 | m55 | 2754.7 / 2853.2 ns | **2111.4 / 2192.8 ns** | 2956.4 ns |
| user: process->process channel_call P->HT | m55 | 1957.0 / 1989.2 ns | **1195.3 / 1584.0 ns** | 1890.8 ns |
| user: process->process channel_call P->E | m55 | 3546.8 / 4595.8 ns | **2105.7 / 2406.8 ns** | 3619.7 ns |

Lines without a switch (M5.5 run, M5 for comparison): timestamp 8.5 / 9.9 ns
(8.5); spin_lock+unlock 26.5 / 27.0 (26.6); page alloc+free one CPU 19.1 /
19.5 (19.0); all 28 CPUs 24.0 / 24.9 (24.0); context switch 30.1 / 30.4
(30.2); block+wake same CPU 446.4 / 478.1 (471.0); channel_call same CPU
549.6 / 583.2 (623.9, the magazines); cache-line P->P2 106.9 / 109.8 (97.0),
P->HT 37.8 / 76.6 (35.9), P->E 101.7 / 104.6 (96.5) (hardware floor, noise);
channel_call P client, server unpinned 543.0 / 573.3 (623.0); server not on
P 589.8 / 1003.6 (1292.8: spinning sibling); TLB shootdown 1 page 27 CPUs
4485.1 / 5594.7 (4975.5); user syscall 30.1 / 30.3 (30.0); clock_get 44.9 /
45.1 (44.8); user page fault 745.1 / 971.9 (744.2).

Reading:
- Everything cross-CPU got much faster: an idle CPU that spins for 10 us
  answers without the wake-from-halt, so block+wake P->HT is 2.3x faster,
  P->E 1.9x, IPIs to an E-core 2.4x, kernel channel_call P->HT 2.1x, and a
  user process->process call P->E 1.7x (3620 -> 2106 ns). Cost: p99 spreads
  a little (a wake that just misses the window still pays the halt exit).
- The M5 cross-CPU regression is gone: channel_call P->P2 / P->E with only
  spin-idle off are 1799 / 2226 ns, below M5 (2166 / 2582) and M4.5
  (2003 / 2333): the placement fix alone.
- kmalloc is 4.3x faster on one CPU and ~800x on 28 at once; timers wake
  350 ns late instead of up to 10 ms; a serial line costs 6.5 us instead of
  9 ms; hybrid placement puts 19 busy threads on 19 different cores.
- **PCIDs barely moved anything** (process->process same CPU 1436 -> 1407
  ns, 2%). The breakdown answers investigation (a): kernel channel_call 550
  ns, thread->thread in one process 1285 ns, process->process 1407 ns. So
  the two CR3 switches plus TLB refills cost only ~120 ns; the other ~735 ns
  is on the user side of the boundary (5 syscalls ~150 ns, FPU ~80 ns, and
  ~500 ns of user copies, handle lookups, the extra failed channel_read,
  entry/exit). That is where the next process-IPC win is, not the TLB.
- **Watch: address-space switch 46.5 -> 66.6 / 67.9 ns** in both switch
  positions: the PCID bookkeeping in pcid_load (epoch, slot search, seq_cst
  generation read) runs whenever the CPU has PCIDs, or CR4.PCIDE makes the
  CR3 write itself dearer. Real calls still got faster; look at it in a
  later pass (`nopcid` boot makes the CPU skip PCIDs entirely, to compare).
  Where the kernel leaves PCIDs off for the INVLPG erratum the pcid rows
  measure nothing: boot the benchmark with `forcepcid` to get them.

Design notes. Unchanged lines (no switch): timestamp, spin_lock, page alloc (one CPU and
all CPUs), context switch, same-CPU block+wake and channel_call, cache-line
round trips, the two wake-affine placement lines, TLB shootdown, user
syscall / clock_get / page fault. Each off/on line flips only its own
switch; the others stay as booted (all on), so an "off" half is M5 for that
feature only (e.g. the channel_call P->P2 "spinidle off" half already has
the placement fix). Compare each on half with its off half.

Since 2026-10-01 (after this run) `placeorder` also covers work stealing:
an idle CPU that is only half a core, or an E-core, sends a stolen thread
on to a whole idle core if one is free (ARCHITECTURE.md, Scheduler), and a
CPU taking its next thread no longer reads as idle to placement for a
moment. The placement line should stay at 0 share; the stats line after
the run counts the steals sent on. QEMU (16 vCPUs, threads=2): the line is
"off 7 share, on 0" before and after. Not yet measured on the PC: the
next benchmark run there should check the placement line, and the
cross-CPU lines that stealing can move, against the M5.5 column.

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

## M8.6, PC 2026-10-02

The M8.6 sign-off build (commit 1789cac; its version string still read
0.0.27-m8.5), `bench` from the shell twice in one boot (boot-0064), read
from the log on the stick. The first run; the second agreed within a few
percent on every line. M5.5's medians beside them.

| Line | M8.6 (median / p99) | M5.5 median |
|---|---|---|
| timestamp cost | 8.9 / 9.9 ns | 8.5 ns |
| spin_lock + spin_unlock | 26.7 / 27.5 ns | 26.5 ns |
| kmalloc(64) + kfree, kmcache on | 19.2 / 19.3 ns | 19.2 ns |
| page alloc + free, one CPU | 18.9 / 19.6 ns | 19.1 ns |
| page alloc + free, all 28 CPUs at once | 23.3 / 24.4 ns | 24.0 ns |
| kmalloc(64)+kfree, all 28 CPUs, kmcache on | 23.8 / 24.9 ns | 23.9 ns |
| **context switch (yield, 2 threads, P)** | **92.0 / 94.8 ns** | 30.1 ns (**3x, watch**) |
| **block+wake round trip, same CPU** | **580.4 / 604.0 ns** | 446.4 ns (+30%) |
| **channel_call round trip, same CPU** | **670.8 / 708.6 ns** | 549.6 ns (+22%) |
| cache-line round trip P->P2 / P->HT / P->E | 105.5 / 38.3 / 98.9 ns | 106.9 / 37.8 / 101.7 ns |
| block+wake P->P2 (idle CPU), spinidle on | 1060.4 / 1075.5 ns | 1003.6 ns |
| block+wake P->HT (idle CPU), spinidle on | 630.1 / 940.1 ns | 480.9 ns (+31%) |
| block+wake P->E (idle CPU), spinidle on | 1067.5 / 1148.0 ns | 943.9 ns |
| block+wake P->unpinned partner, affinepair on | 627.2 / 803.8 ns | 470.0 ns (+33%) |
| IPI function call P->P2 / P->HT / P->E, spinidle on | 420.8 / 272.2 / 483.3 ns | 418.4 / 268.4 / 474.3 ns |
| interrupt: vector on cpu18 -> port_wait wakes P (new) | 998.4 / 1501.1 ns | - |
| channel_call P->P2 / P->HT / P->E, 1 client, spinidle on | 1454.7 / 714.8 / 1558.9 ns | 1385.6 / 586.0 / 1418.8 ns |
| channel_call, P client, server unpinned | 668.9 / 703.9 ns | 543.0 ns |
| channel_call, P client, server not on P | 714.3 / 780.6 ns | 589.8 ns |
| placement of 19 busy threads, placeorder on | 0 share a core, 12 on E | 0 share, 12 on E |
| serial_write of a 100-character line, serialirq on | 6517.4 ns / 17.6 us | 6514.1 ns |
| sleep 100 us / 1000 us: how late it wakes, oneshot on | 400.9 / 404.2 ns | 349.8 / 348.8 ns |
| TLB shootdown, 1 page, 27 other CPUs | 4502.1 / 5682.8 ns | 4485.1 ns |
| address-space switch, pcid on | 67.8 / 68.1 ns | 67.9 ns |
| XRSTOR + XSAVE of user FPU state, fpuopt on | 39.2 / 40.0 ns | 39.9 ns |
| user: syscall round trip / clock_get / page fault | 30.5 / 44.6 / 714.3 ns | 30.1 / 44.9 / 745.1 ns |
| user: process->process channel_call, same CPU, pcid on | 1532.9 / 1554.2 ns | 1406.9 ns (+9%) |
| user: thread->thread channel_call, 1 process, fpuopt on | 1398.9 / 1424.9 ns | 1284.8 ns |
| user: process->process channel_call P->P2 / P->HT / P->E, m55 on | 2228.3 / 1359.6 / 2316.4 ns | 2111.4 / 1195.3 / 2105.7 ns |

Reading:
- The allocators, IPIs, cache lines, timers, the serial port, TLB
  shootdowns and syscalls are where M5.5 left them; the placement fix
  holds on the PC (0 busy threads share a core).
- **Every line through the scheduler's switch got slower since M5.5:** a
  context switch 30 -> 92 ns, and each wake-and-switch line about +120 ns
  (same-CPU block+wake +134, channel_call +121, P->HT +149, the user calls
  +115 to +210). One cost added to every switch, not a scaling problem.
  Candidates, unmeasured: M8's scheduler change (busy set before dequeue,
  steals sent on to whole cores), the soak's and ktest's hooks on the
  switch path, the lock checker's larger class table (78 classes in use).
  To find before M11.5's IPC pass builds on these numbers.

