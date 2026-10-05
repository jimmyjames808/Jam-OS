# Benchmark baselines

"Benchmark" in the boot menu, or `bench` in the shell; the method is in
`kernel/test/bench.c`.
Each entry: median / p99 of 4000 samples, kernel threads only, lock checker
on. Record a new column for every milestone run on the real PC, so a
regression shows up as a number, not a feeling.

The PC is described in [HARDWARE.md](HARDWARE.md) (28 CPUs: 8 P-cores with
Hyper-Threading + 12 E-cores). CPUs used: P = cpu2, P2 = cpu4 (another
P-core), HT = cpu3 (P's sibling), E = cpu16.

## Method

**`bench`** (the header of `kernel/test/bench.c` has the details): the
TSC read with `lfence; rdtsc; lfence`, the cost of a timestamp pair
measured first and subtracted, operations shorter than a few hundred ns
timed in batches of 64, 20 ms of untimed warm-up, then the median and the
99th percentile of 4000 samples (nothing trimmed, never a mean). Threads
are pinned to the CPUs above at priority 24; CPU 0 is left out. The
`user:` lines are timed in ring 3 by bin/utest, which the kernel starts
pinned. A line an optimisation should move is measured with its switch
off and then on in the same run (each switch is named in
[ARCHITECTURE.md](../ARCHITECTURE.md); M11.5 added `fpucall`,
`lockdep`, `handoff` and `slots`). The `path:` lines are counts
([below](#path-breakdown-m115-stage-0-qemu-2026-10-02)).

The `user:` call lines come in this order: the call against bench-echo, a
server that reads, writes and waits (the line every milestone since M5
has; then the same with a deadline on every call, as libos made them
before `_within`; then that call with one switch at a time off and on;
then thread->thread in one process); then the call against a server on
`channel_reply_wait` (written by hand, with the `handoff` and the `slots`
switch), and through generated code (null.ping with a time limit against
`null_serve`, the way programs and services call since M11.5); then the
cross-CPU calls.

**Per-operation lines** (`perop` from the shell; `user/tests/perop/main.c`
explains each line): what one file operation costs a program, with the
same TSC method in ring 3: a `stat`, an open and close, a 4 KiB and a
64 KiB read that fat has cached, a 4 KiB block read through usb-storage
(a 512-byte read where fat's cache has nothing, so it reads the 4 KiB
around it from the stick), and a 64 KiB write through to the stick. It
writes a scratch file on the mount it is given (`/data` by default;
another stick after `mount -w /usb0`: `perop /usb0`). Unlike bench's user
lines it is not pinned (no system call pins a thread from user space), it
runs on the live system, it measures the TSC's rate against the clock,
and the two lines that reach the stick take 400 samples. The QEMU check
is `tools/shell-tests/perop.txt` (QEMU's numbers mean nothing).

**The Linux column** (`tools/linuxbench`, whose
[README](../tools/linuxbench/README.md) has the owner's steps): a static
Linux program that measures the equivalent of each line on the same PC,
from an Ubuntu 24.04 live stick in text mode, twice: Linux as it comes,
and with `mitigations=off`. Same method: the same TSC pair (its rate
measured against `CLOCK_MONOTONIC_RAW`), the same sample counts, batches
and warm-up, the same choice of P, P2, HT and E, every thread pinned at
SCHED_FIFO 50 (as Jam OS's run above everything else), the `performance`
cpufreq governor and energy preference (Jam OS leaves the firmware's
P-state choice as it is). Each output line carries the Jam OS line's name,
then `=` and what Linux does; the file starts with the kernel's version
and command line, the microcode, and every file in
`/sys/devices/system/cpu/vulnerabilities`.

| Jam OS line | Linux, in linuxbench |
|---|---|
| timestamp cost | the same `lfence; rdtsc; lfence` pair |
| spin_lock + spin_unlock | a ticket lock in user space, without a lock checker |
| kmalloc(64) + kfree | malloc(64) + free (libc) |
| context switch (yield, 2 threads, P) | `sched_yield` between two SCHED_FIFO threads on P |
| block+wake round trip (same CPU, P->P2/HT/E, unpinned partner) | a futex ping-pong between two threads |
| cache-line round trip | the same code (a check that the same CPUs were chosen) |
| IPI function call round trip | `membarrier` (private, expedited) to a thread of ours spinning on the target |
| sleep 100 / 1000 us | `clock_nanosleep` |
| TLB shootdown, 1 page | `mprotect` read-write to read-only of a touched page, a thread of ours on every other CPU |
| user: syscall round trip | the `syscall` instruction with an unused number (and `getppid`) |
| user: clock_get | `clock_gettime` as a system call (and through the vDSO, no kernel entry) |
| user: page fault, fresh zero page | the first write to a page of an anonymous mapping, no huge pages |
| user: process->process channel_call (same CPU, P->P2/HT/E) | 16 bytes each way three ways: a futex and shared memory, a `socketpair` (SEQPACKET), two pipes |
| user: the same with a 60 s deadline | the futex way with a 60 s timeout on every wait |
| user: thread->thread channel_call | a `socketpair`, and a futex, between two threads |
| per-operation lines | `stat`, open + close, `pread` from the page cache, a 4 KiB `pread` with `O_DIRECT`, a 64 KiB `pwrite` with `O_DIRECT`, on the SanDisk |

The kernel-only lines (page allocation, the all-CPU lines, the kernel
channel_call, the interrupt, placement, the serial port, the
address-space switch, XSAVE) have no Linux line. When reading the
columns side by side: Jam OS has no Spectre or Meltdown mitigations
(Raptor Lake needs no KPTI), which is what the `mitigations=off` column
is for; a Linux CPU waiting for a wake idles through its cpuidle driver
where Jam OS's polls for 10 us first (spinidle); and the IPI's target is
busy in user space on Linux, idle on Jam OS.

## M4.5 and M5, PC 2026-09-29

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

## Sockets on rings (M9.5 track A), QEMU 2026-10-02

**QEMU numbers, not the PC's** (TCG on the Mac, other agents' QEMUs
running at the same time): only the ratio between the two columns means
anything. utest's `netsock_bench` (`user/tests/utest/netbench.c`):
datagrams through a program's UDP socket and bin/netstack, the test as
the program and as the card (each datagram leaves through the card's tx
ring and comes back into its rx ring as the peer's answer, so it crosses
netstack twice, and the test's own work as the card is in the time). A
round trip is one datagram at a time with the blocking calls; a stream
keeps 16 in flight with the forms that don't wait. µs a datagram (each
way). One CPU: the test and netstack share it; two CPUs: the scheduler
usually puts them on two. Before: a call per datagram (net.idl's
`sock_send_to`/`sock_recv`, build 9438f6e plus the bench); after: the
rings (498fdfd).

| `netsock_bench` (µs a datagram) | before, 1 CPU | after, 1 CPU | before, 2 CPUs (two runs) | after, 2 CPUs (two runs) |
|---|---|---|---|---|
| round trip, 64 bytes | 210.3 | 162.9 | 3473.7 / 960.7 | 152.1 / 172.8 |
| stream, 64 bytes | 114.5 | 19.0 | 1701.3 / 958.8 | 13.7 / 14.3 |
| round trip, 1472 bytes | 190.5 | 163.1 | 1245.7 / 892.5 | 162.2 / 176.3 |
| stream, 1472 bytes | 141.6 | 46.2 | 1327.2 / 963.8 | 35.7 / 35.0 |

Reading:
- A stream of small datagrams is 6x faster on one CPU and about 70x on
  two: while datagrams flow, neither side makes a call or a system call
  per datagram (a signal only when the other side said it sleeps, about
  once a turn of netstack's loop).
- Across CPUs the call-per-datagram path paid a cross-CPU wake for every
  call and every reply, which QEMU makes very slow (milliseconds, and very
  variable); the rings pay it about once a batch. On the PC the cross-CPU
  wake costs microseconds, so the gain there will be much smaller: measure
  it with track E's throughput tester.
- A round trip still pays two wakes each way (the program's and
  netstack's) and is only 1.2-1.3x faster on one CPU.
- `update`'s fetch in `tools/update-net-test.sh` (QEMU, 2 CPUs, after):
  10.2 MB in 0.8 s.

## Path breakdown (M11.5 stage 0), QEMU 2026-10-02

What one call costs, counted: `bench` ends with `path:` lines
(`kernel/test/bench_path.c`; the probes and the trace are in
`kernel/include/jam/pathstat.h`), and the path tests
(`kernel/test/test_pathstat.c`) pin these numbers. Each case is counted
over 1024 calls after 256 warm-up calls, counting only the call's own
threads and leaving interrupt handlers out. **Counts, not times**, so
QEMU's (TCG on the Mac, 4 CPUs, branch m11.5-perf) are the PC's; the
exceptions are the cross-CPU case's IPIs (spin-idle avoids most on the
PC) and the CR3 flushes (QEMU has no PCIDs: every load flushes; 0 on the
PC). On the PC the same run also prints a timeline of each case: the
median time of each step between named points of the path.

Cases: **switch**, two kernel threads yielding on P (per switch);
**kernel call**, channel_call between two kernel threads on P; **user
call**, utest's `bench-call` against `bench-echo`, two processes on P
(the line M11.5's target is about); **+ deadline**, the same with a
deadline per call as libos's file calls have (`bench-dcall`);
**thread->thread**, the same calls between two threads of one process;
**P->P2**, the user call with the server on another P-core.

| Per call (per switch for the switch) | switch | kernel call | user call | + deadline | thread->thread | P->P2 |
|---|---|---|---|---|---|---|
| system calls | 0 | 0 | **5**: call; read, write, a read that finds nothing, wait_one | 6 (+ clock_get) | 5 | 5 |
| user copies in / out (bytes) | 0 | 0 | 5 / 5 (208 / 44) | 5 / 5 | 5 / 5 | 5 / 5 |
| message copies in the kernel (bytes) | 0 | 4 (64) | 4 (64) | 4 (64) | 4 (64) | 4 (64) |
| kmalloc / kfree | 0 | 2 / 2 | 2 / 2 | 2 / 2 | 2 / 2 | 2 / 2 |
| job charges and credits (levels walked) | 0 | 0 | 4 (4) | 4 (4) | 4 (4) | 4 (4) |
| handle-table operations | 0 | 0 | 5 | 5 | 5 | 5 |
| spinlocks | **1** | 14 | **20** | 22 | 20 | 20 |
| scheduler passes / switches | 1 / 1 | 2 / 2 | 2 / 2 | 2 / 2 | 2 / 2 | 2 / 4 |
| wakes / IPIs | 0 / 0 | 2 / 0 | 2 / 0 | 2 / 0 | 2 / 0 | 2 / 2 |
| FPU saves / restores / kept | 0 | 0 | 2 / 2 / 0 | 2 / 2 / 0 | 2 / 2 / 0 | 2 / 0 / 2 |
| CR3 loads | 0 | 0 | 2 | 2 | 0 | 4 |
| sleeper inserts / timer re-arms | 0 | 0 | 0 | **1 / 1** | 0 | 0 |
| channel reads that found nothing | 0 | 0 | 1 | 1 | 1 | 1 |
| observer callbacks | 0 | 1 | 1 | 1 | 1 | 1 |

Reading:
- The user call's 8 copies of the message bytes are 4 user copies (the
  request in and out, the reply in and out) and 4 in the kernel (into the
  message and out of it, each way); the other 6 user copies are the
  argument structs and the lengths.
- The switch line takes one spinlock (the run queue's) and one scheduler
  pass. A user call takes 20 spinlocks, each through the lock checker
  (`spin_lock + spin_unlock` is 26.7 ns with it on the PC).
- A deadline costs a system call (the clock), two spinlocks (the sleeper
  queue) and a timer re-arm on every call ([M11.5-PLAN.md](M11.5-PLAN.md#q4-how-should-deadlines-get-cheaper)).

## Path counts after M11.5's tracks, QEMU 2026-10-05

The same cases counted at M11.5's join, on the build with every
follow-up, plus the two server shapes M11.5 added. The PC's run of
2026-10-04 agrees with these counts except where the
[next section](#m115-so-far-pc-2026-10-04) says.

| Per call (per switch for the switch) | switch | kernel call | user call | + deadline | thread->thread | reply-and-wait server | generated | P->P2 |
|---|---|---|---|---|---|---|---|---|
| system calls | 0 | 0 | 5 | 6 | 5 | **2**: call; reply_wait | **2** | 5 |
| user copies in / out (bytes) | 0 | 0 | 5 / 5 (208 / 44) | 5 / 5 | 5 / 5 | 4 / 4 (224 / 40) | 4 / 4 (224 / 48) | 5 / 5 |
| message copies in the kernel (bytes) | 0 | 4 (64) | **0** | 0 | 0 | 0 | 0 | 0 |
| kmalloc / kfree | 0 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 | **0** | **0** | 1 / 1 |
| job charges and credits (levels walked) | 0 | 0 | 2 (2) | 2 (2) | 2 (2) | **0** | **0** | 2 (2) |
| handle-table operations | 0 | 0 | 5 | 5 | 5 | **2** | **2** | 5 |
| spinlocks | 1 | 12 | 18 | 20 | 18 | **11** | 13 | 19.3 |
| scheduler passes / switches / hand-offs | 1 / 1 / 0 | 2 / 2 / 1 | 2 / 2 / 1 | 2 / 2 / 1 | 2 / 2 / 1 | 2 / 2 / **2** | 2 / 2 / 2 | 2 / 4 / 0 |
| FPU full saves / call saves / restores | 0 | 0 | 0 / 2 / 2 | 0 / 2 / 2 | 0 / 2 / 2 | 0 / 2 / 2 | 0 / 2 / 2 | 0 / 2 / 2 |
| CR3 loads | 0 | 0 | 2 | 2 | 0 | 2 | 2 | 4 |
| sleeper inserts / timer re-arms | 0 | 0 | 0 | 1 / **0** | 0 | 0 | 1 / 0 | 0 |
| clock reads (the kernel's) | 0 | 0 | 0 | 2 | 0 | 0 | 2 | 0 |
| channel reads that found nothing | 0 | 0 | 1 | 1 | 1 | **0** | **0** | 1 |
| observer callbacks | 0 | 1 | 1 | 1 | 1 | 0 | 0 | 1 |
| interrupt-flag toggles: by the lock checker / at a lock release | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

Reading:
- The message bytes go straight between user memory and the message (no
  kernel copy left), and a message for a thread already waiting for it
  goes in its writer's slot: the reply of every call, and both messages
  with a reply-and-wait server, which therefore allocates and charges
  nothing.
- The reply-and-wait server is the shape the plan aimed at: 2 system
  calls, 2 handle lookups, no read that finds nothing, both wakes handed
  over. Its 11 locks: per side the handle table, the channel pair and the
  peer's endpoint for the send, its own endpoint before it blocks, and
  the scheduler's; the caller also lists itself on its endpoint first. A
  handed wake takes no run queue lock.
- The generated call adds the kernel's two clock reads, a sleeper entry
  and its 2 locks (its time limit, given as a timeout: no `clock_get`),
  and one more copy-out (the reply's status and the request's sizes, in
  one).
- A deadline seconds away costs no timer write any more (the tick looks
  after it), and a wait with no deadline reads no clock.
- The lock checker never turns interrupts off on these paths, and a lock
  release turns them off only when a reschedule is pending (13 toggles a
  user call at the releases alone before that).

## M11.5 so far, PC 2026-10-04

**Measured before the last follow-ups.** Build b7713bd (M11.5's stage 0
and tracks F, D, O, L, P, C and H merged), a normal boot with `bench`
from the shell (step 0's second boot:
[M11.5-PLAN.md](M11.5-PLAN.md#the-switch-regression-first)), read from
the log netlog brought to the Mac. Not in that build: track G (the
generated server on reply-and-wait and `_within`: no generated line
yet), the hand-off recorded without the run queue lock, no relock after
a handed wake, the merged copy-outs and `preempt_check`'s fast path. The
lock checker is on unless a line says otherwise. M8.6's and M5.5's
medians beside them.

| Line | 2026-10-04 (median / p99) | M8.6 median | M5.5 median |
|---|---|---|---|
| timestamp cost | 8.5 / 9.9 ns | 8.9 ns | 8.5 ns |
| spin_lock + spin_unlock (P), lockdep off; on | 12.7 / 12.9 ns; **13.6 / 14.0 ns** | 26.7 ns (on) | 26.5 ns (on) |
| kmalloc(64) + kfree, kmcache on | 19.4 / 19.9 ns | 19.2 ns | 19.2 ns |
| page alloc + free, one CPU; all 28 at once | 18.9 / 19.6 ns; 22.8 / 68.7 ns | 18.9; 23.3 ns | 19.1; 24.0 ns |
| **context switch (yield, 2 threads, P)** | **30.8 / 31.0 ns** | 92.0 ns | 30.1 ns |
| block+wake round trip, same CPU | **380.6 / 406.6 ns** | 580.4 ns | 446.4 ns |
| channel_call round trip, same CPU, lockdep off; on | 392.4 / 414.2 ns; **461.5 / 475.3 ns** | 670.8 ns (on) | 549.6 ns (on) |
| cache-line round trip P->P2 / P->HT / P->E | 106.5 / 37.8 / 103.6 ns | 105.5 / 38.3 / 98.9 ns | 106.9 / 37.8 / 101.7 ns |
| block+wake P->P2 / P->HT / P->E (idle CPU), spinidle on | 902.3 / 385.3 / 930.7 ns | 1060.4 / 630.1 / 1067.5 ns | 1003.6 / 480.9 / 943.9 ns |
| block+wake P->unpinned partner, affinepair on | 386.3 / 596.5 ns | 627.2 ns | 470.0 ns |
| IPI function call P->P2 / P->HT / P->E, spinidle on | 402.8 / 258.4 / 479.0 ns | 420.8 / 272.2 / 483.3 ns | 418.4 / 268.4 / 474.3 ns |
| interrupt: vector on cpu19 -> port_wait wakes P | 934.0 / 1410.7 ns | 998.4 ns | - |
| channel_call P->P2 / P->HT / P->E, 1 client, spinidle on | 1376.6 / 511.7 / 1440.5 ns | 1454.7 / 714.8 / 1558.9 ns | 1385.6 / 586.0 / 1418.8 ns |
| channel_call, P client, server unpinned; not on P | 459.6 / 476.2 ns; 505.1 / 577.0 ns | 668.9; 714.3 ns | 543.0; 589.8 ns |
| placement of 19 busy threads, placeorder on | 0 share a core, 12 on E | the same | the same |
| serial_write of a 100-character line, serialirq on | 6531.6 ns / 12.1 us | 6517.4 ns | 6514.1 ns |
| sleep 100 us; 1000 us: how late it wakes, oneshot on | 304.8 ns; 321.8 ns | 400.9; 404.2 ns | 349.8; 348.8 ns |
| TLB shootdown, 1 page, 27 other CPUs | 4361.5 / 4982.6 ns | 4502.1 ns | 4485.1 ns |
| address-space switch, pcid off; on | 64.3 / 64.7 ns; 61.6 / 62.4 ns | 67.8 ns (on) | 67.9 ns (on) |
| XRSTOR + XSAVE of user FPU state, fpuopt on | 39.7 / 40.2 ns | 39.2 ns | 39.9 ns |
| user: syscall round trip / clock_get / page fault | 30.7 / 45.2 / 669.8 ns | 30.5 / 44.6 / 714.3 ns | 30.1 / 44.9 / 745.1 ns |
| **user: process->process channel_call, same CPU (P)**, pcid off; on | 1255.4 / 1275.3 ns; **1214.7 / 1239.8 ns** | 1532.9 ns | 1406.9 ns |
| user: the same with a deadline per call (labelled "5 s" then; it is 60 s) | 1320.8 / 1343.0 ns | - | - |
| user: the same call, fpucall off; on | 1244.6 / 1266.8 ns; 1211.4 ns (p99 cut off in the log) | - | - |
| user: the same call, lockdep off; on | **1106.8 / 1130.5 ns**; 1213.8 / 1237.0 ns | - | - |
| user: the same call, handoff off; on | 1211.9 / 1236.0 ns; 1211.9 / 1234.6 ns | - | - |
| **user: call to a reply-and-wait server, same CPU (P)**, handoff off; on | 886.7 / 908.4 ns; **884.8 / 906.1 ns** | - | - |
| user: thread->thread channel_call, 1 process, fpuopt on | 1091.2 / 1109.6 ns | 1398.9 ns | 1284.8 ns |
| user: process->process channel_call P->P2 / P->HT / P->E, m55 on | 2015.3 / 1141.3 / 2232.1 ns | 2228.3 / 1359.6 / 2316.4 ns | 2111.4 / 1195.3 / 2105.7 ns |
| the same, m55 off | 2557.3 / 1834.0 / 3402.8 ns | - | - |

The run's path counts equal the table above but for: the kernel call 14
locks, the user call and thread->thread 20, the deadline call 22, the
reply-and-wait server **15** (the hand-off still took the run queue
lock, and a waiter handed its message took its endpoint's lock again);
no generated case yet, and no count yet of interrupt toggles at a lock
release. Its timelines (each step includes one mark, 23.2 ns): a user
thread's switch-in costs ~175 ns from `sched_picked` to `arch_done` (the
FPU call restore ~77 ns, the CR3 load ~97 ns) where a kernel thread's
costs ~46; sending a message (`msg_made -> sent`) ~160-220 ns; bench-echo's
read of the request ~290 ns from its system call's entry.

Reading:
- **The switch regression is gone**: 30.8 ns, M5.5's number, from 92 at
  M8.6, and every line through a wake fell with it (same-CPU block+wake
  381 ns and kernel channel_call 462 ns, both below M5.5; the cross-CPU
  wakes and calls 80-250 ns faster than M8.6). The run had P's lock
  checker fast path, which no longer toggles the interrupt flag on every
  acquisition and release (the spinlock pair 26.7 -> 13.6 ns): most
  likely the cause, but this boot alone can't say. The other two boots of
  step 0 (Tests > Benchmark, and the same with `smp=loader`) confirm it if
  their switch is ~31 ns too.
- **The call**: bench-echo's same-CPU call 1533 -> 1215 ns; against a
  reply-and-wait server **885 ns**, 285 ns over the 600 ns target. Of the
  bench-echo call, the lock checker costs 107 ns, the full FPU save that
  the call rule dropped 33 ns, PCIDs off 41 ns more, and a deadline made
  as libos made them then (a `clock_get` and a sleeper entry) 106 ns.
- **The hand-off gained nothing measurable on this build** (886.7 off,
  884.8 on), though it fired (2.00 hand-offs a round trip in `path
  rwcall`; the `schedule()` pass itself stays, by design: it finds the
  wakee waiting). What it skipped, an enqueue and a pick on an otherwise
  empty run queue, was already cheap; the handed wake still took the run
  queue lock, and the woken waiter took its endpoint's lock again. Both
  went in the follow-ups (the reply-and-wait server 15 -> 11 locks).

What the next PC run should show (`bench` on a build with every
follow-up, on the three boots of step 0, plus a `nolockdep` boot for the
checker-off column):
- `context switch` about 31 ns on all three boots (the regression's
  cause settled);
- the path counts of the table above: the reply-and-wait server 11
  locks, the user call 18, the kernel call 12, no interrupt toggles at a
  lock release;
- the reply-and-wait line below 885 ns, its `handoff` on now ahead of
  off (a handed wake takes no lock), and its new `slots` line (off: both
  messages allocated and charged);
- the new generated line (`user: generated client and server
  (null.ping), same CPU (P)`), which is what programs pay since track G:
  about the reply-and-wait line plus the kernel's clock reads and a
  sleeper entry;
- the deadline line under its right name, `user: the same with a 60 s
  deadline per call (P)`;
- then the Linux column (`tools/linuxbench`) and the per-operation lines
  (`perop`), as the method above says.
