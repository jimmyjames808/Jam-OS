# Jam OS: handoff (updated 2026-09-29, before a context compact)

## CURRENT STATE (read this first)
- **M5 ✅ DONE 2026-09-29.** Main (v0.0.8-m5) passed everything on the PC: init+utest 14/14 (root job
  clean), All tests 131/131, benchmark (BENCH.md M5 column), and the **10-min stress sign-off with 0
  failures** on the f86cb64 build. M0-M5 all confirmed on the PC.
- **M5.5 MERGED 2026-09-29 (dcd70f5, v0.0.9-m5.5).** QEMU: 143/143 ktests at 4 and 8,threads=2,
  init+utest 14/14 root job clean, stress=30 at 4 and 8. Every optimisation has a runtime switch +
  boot word (`nopcid`, `nospinidle`, `idlespin=<us>`, `noplaceorder`, `noaffinepair`, `nokmcache`,
  `nooneshot`, `noserialirq`, `nofpuopt`; `m55` in bench = all) and `bench` prints `off med/p99 on
  med/p99` per line. PCIDs + TSC-deadline one-shots have NEVER run (TCG lacks them): first PC run is
  their first run. If anything looks like memory corruption on the PC, boot with `nopcid` first.
  **PC 2026-09-29: All tests PASSED (143) after fixing a test-only expectation in
  pcid_slot_bookkeeping (8acd552; that block had never run before) - first real PCID run is clean.**
  Benchmark recorded (BENCH.md M5.5 section, IMG_0066); 2-min stress PASSED on the PC.
  Independent review agent RUNNING (worktree, read-only on main). **Next PC round**: flash, then All
  tests, Benchmark (-> M5.5 column of BENCH.md; expected moves listed under "M5.5 (expected)"),
  2-min stress; 10-min stress = M5.5 sign-off after the review's fixes land.
- **M6 STARTED 2026-09-29.** Plan = M6-PLAN.md. Foundation 71aa223 (pci.h, interrupt.h, resource.h,
  drivers/include/jam/driver.h, syscalls 90-102, M6 ABI in abi.h, kernel/core/m6_weak.c, main.c calls
  pci_init/resource_init after smp_report and pci_report on `pcilist`, boot entry "Devices", QEMU `edu`).
  Four phase-1 agents RUNNING in worktrees: A PCI core, B vectors + interrupt objects, C resources +
  MMIO VMOs + bound dma_cap + the other syscalls, D driver.h both builds + IDL + build check + null
  driver. Then phase 2: devmgr process, edu driver, xhci-noop (the PC done test). Design choices:
  interrupts = SIG_INTERRUPT + PERSISTENT port binding (no new packet type); drivers get filtered
  config syscalls on their own RES_PCI_DEV (no pcidev protocol); bus master needs RIGHT_MANAGE
  (devmgr only). Merge order suggestion: A, then C and B, then D; expect conflicts in Makefile,
  kernel/abi, m6_weak.c (delete in phase 2). Big code -> independent review after merging.
  First PC check once A merges: the "Devices" entry (compare with Windows Device Manager; tells
  whether xHCI 8086:7A60 has MSI-X or only MSI).
- **M5.5 review DONE + fixes on main**: far-future deadline wrap (user-triggerable timer storm) fixed,
  spin-idle redundant IPI fixed (now 200/200 polled), PCID-0 flush after a run-time off->on flip, bench
  serial switch drains first, and the open mutex-starvation item FIXED (hand-off to the longest waiter
  after 1 ms; ktest proves it). QEMU: 145/145 ktests, stress=60 at 4+8, init clean. **Next PC round
  (M5.5 sign-off): flash, All tests, then the 10-min stress.**
- **M6 Track A MERGED (fd61839; v0.0.10-m6a = 5c64c9e)**: PCI core, 157/157 ktests, Devices entry works in
  QEMU. On the PC run **Devices first** (pci_init now runs on EVERY boot; BAR sizing of Intel PCH
  functions with decode briefly off is new on real hardware - if a boot hangs, the last `pci:` line
  names the function). fbcon gained fbcon_phys() for display detection.
- **Tracks B + C MERGED** (a9cc0b6, 27a612b; v0.0.11-m6abc): per-CPU vectors + interrupt objects (edu MSI ->
  port packet works in QEMU, ~10-100 us under TCG), resources, physical VMOs, bound dma_caps, config
  filter, all syscalls 90-102; userboot passes the root resource (SR_RESOURCE) and init checks it.
  186/186 ktests at 4 and 8. Pre-existing object.c race fixed (a89dc4d: deferred on_zero_handles now holds
  a ref). Open notes for phase 2: pci_msi_enable's INTx-disable write should take Track C's
  pci_cmd_lock; devmgr's RES_PCI_DEV needs RIGHT_MANAGE; MSI needs BME on even for IRQ-only devices;
  object_wait_one on an interrupt can miss a fire (use the PERSISTENT port binding) - say so in driver.h.
- **Track D MERGED; M6 PHASE 1 COMPLETE** (4752cbc, v0.0.12-m6p1): driver.h both builds (kernel process via
  process_create_kernel / driver_kernel_start; process via driver_crt), IDL (tools/genidl.py,
  abi/idl -> drivers/include/idl), build check (tools/checkdriver.py + `make check` negative tests),
  null + drvtest drivers. 189/189 ktests at 4+8, utest 16/16, stress=60 at 4+8, KTESTS=0 builds.
  RUNNING now: independent review of phase 1; phase 2 agents (1) devmgr + drivers/edu + kill/rights
  utests, (2) drivers/xhci-noop + boot entry "USB controller test (xHCI no-op, M6 done test)" (`xhcitest`).
  devmgr gets a one-line match entry 0c0330 -> drv/xhci-noop when both merge.
- **xhci-noop MERGED** (M6 done-test driver + boot entry "USB controller test (xHCI no-op, M6 done test)",
  `xhcitest`): QEMU PASS kernel + process mode (also MSI-only via QEMU_XHCI=msi=on,msix=off). 191/191.
- **PC Devices run 2026-09-29 (IMG_0067, v0.0.12-m6p1): booted fine, no hang in BAR sizing.** 24 functions on
  6 buses; boot display 01:00.0 RTX 10de:2702 (fb 0x4000000000) marked D. **xHCI 00:14.0 8086:7a60 rev 11:
  MSI only (8 vectors, 64-bit, not maskable), NO MSI-X**, BAR0 mem64 0x4016200000 64K. **Ethernet 05:00.0
  10ec:8125 rev 05: MSI 1 (64-bit, maskable), MSI-X 32 (table bar4+0x0, pba bar4+0x800)**, bar0 io 0x3000,
  bar2 mem64 0x86500000 64K (the registers), bar4 mem64 0x86510000 16K (MSI-X only) - so the M9 driver's
  registers never share a page with the MSI-X table. Others: Wi-Fi 00:14.3 8086:7a70 (MSI-X 16), VMD/RAID
  00:0e.0 8086:a77f, NVMe 02:00.0 c0a9:5421 (Crucial), SATA, HD audio x2, SMBus, serial-bus (I2C/SPI)
  functions, 6 bridges, no iGPU function (disabled in firmware).
- **M6 DONE TEST PASSED ON THE PC 2026-09-29 (IMG_0069, v0.0.14-m6 = 9d4719a)**: xhci-noop, Intel 8086:7a60 rev 11,
  25 ports, 34 scratchpads, MSI vector 0 of 8, BIOS handoff ok (not BIOS-owned); 3 No-Op commands completed
  via MSI in BOTH modes: kernel process latency min/median/max 12/46/54 us, user process 11/46/47 us; bus
  master off, MSI off, job clean -> PASS both; init clean; run complete: no problems. (Latency ~46 us is
  the driver's IMOD = 40 us interrupt moderation, not the kernel.) Two PC-only fixes it took:
  test_pci skips QEMU-only devices (5b06f16); resource_pci_bar's neighbour check limited to sub-page
  slack + decode-on functions, refusals now log their reason (9d4719a).
  Left for M6: merge the devmgr + edu agent (running; it also changes vmo_unpin to take the dma_cap and
  adds devmgr's 0c0330 -> drv/xhci-noop entry), independent review of phase 2, then PC: All tests,
  2-min stress, and ONE 10-min stress that signs off M5.5 and M6 together (user: 2-min until then).
  PC: All tests on 0.0.14-m6 -> run complete: no problems (incl. the xhci ktests on the real controller).
  PC: 2-min stress on 0.0.14-m6 PASSED.
- **devmgr + edu MERGED (v0.0.15-m6)**: user/devmgr (process, started by init; match table 1234:11e8 -> drv/edu,
  class 0c0330 -> drv/xhci-noop, so EVERY plain/init boot now runs xhci-noop on the PC through devmgr),
  drivers/edu both modes, kernel-mode devmgr (kdevmgr, boot word `drivers=kernel`), vmo_unpin(vmo, dma, pin)
  ABI, MSI enable under pci_cmd_lock, utests edu_process / edu_killed_mid_dma / driver_handle_limits
  (on the PC that one uses the first non-display MSI-X function: refused writes + memory decode on only).
  QEMU: 200/200 ktests at 4+8, utest 19/19 clean, devmgr exits 0. Next: independent review of phase 2,
  then PC: "Jam OS (init + utest)", "Drivers as kernel processes", All tests, 2-min, then the 10-min
  sign-off of M5.5 + M6.
- **PC init+utest on 0.0.15-m6 (IMG_0070)**: devmgr bound 00:14.0 8086:7a60 -> drv/xhci-noop as a process, 3
  No-Ops via MSI (9/13/47 us), drvtest 405 ok, root job clean; ONE failure: driver_handle_limits got
  ERR_OUT_OF_RANGE from devmgr's DRIVER_VIEW on the first MSI-X function = the VMD controller 00:0e.0.
  Cause: pci_size_bars treated a 64-bit BAR's hard-wired-zero upper half as size bits (size ~2^64).
  Fixed in 0.0.16-m6: upper half 0 -> size from the low half; implausible sizes (not a power of two,
  > 1 TiB, wrapping, misaligned) are logged and left UNSIZED; the utest tries each MSI-X function until
  one's table BAR can be handed out and prints which it used.
  **PC 0.0.16-m6: init + utest 19/19 passed** (VMD fix confirmed); drivers=kernel: 1 driver bound + stopped, 0 skipped.
- **M6 phase 2 review DONE (0.0.17-m6)**: fixes on main - devmgr kills a driver's whole JOB and turns BME off
  itself; a driver ending by itself = a problem in RESULTS; xhci-noop always halts+resets; resource_pci_bar
  checks the WHOLE BAR again (vs decode-on functions only; the PC's original xHCI refusal was the VMD's
  bogus ~2^64 BAR, fixed in faf32a7); devmgr STOP_WAIT 15 s. OPEN, moved to M7 Track D (M7-PLAN.md):
  stale DMA after a rebind (finding 1, CONFIRMED by `ktest=review_m6p2_stale_dma_after_rebind`; no rebind
  happens on the PC today), non-transferable driver handles, D-state changes for devmgr.
  QEMU: 201/201 ktests at 4+8, utest 19/19 clean, stress=30, xhcitest PASS. **Next: PC final round on
  0.0.17-m6 = All tests + 2-min + 10-min (signs off M5.5 + M6); M7 agents start when the 10-min run starts.**
  PC 0.0.17-m6: All tests 201/201, no problems.
  User runs the 10-min stress directly (no 2-min first before a sign-off) - RUNNING (M5.5 + M6 sign-off).
- **M7 STARTED 2026-09-29** (M7-PLAN.md). Agents running in worktrees: A usb-bus (xHCI + hubs + TT, serves
  usb.idl; devmgr 0c0330 -> drv/usb-bus), C console + shell + kernel services (syscalls 110-119: klog read,
  framebuffer hand-off, debug_command, reboot, COM1 RX), D supervision + safe rebind (syscalls 120-129:
  dma_cap_bus_master; quarantine; non-transferable driver handles; restart with backoff), B hid + keyboard
  layer (mock usb-bus/console tests). Foundation a252f56: IDL handle results + drv_channel_call_h,
  usb/input/console .idl, input ABI, DR_USB/DR_INPUT. Merge order suggestion: D (devmgr/driver.h), C, A,
  B; then phase 2 wiring (devmgr match HID interfaces -> drv/hid with DR_USB + DR_INPUT via
  console.connect_input) and QEMU end-to-end (sendkey -> shell), then PC: USB device list first.

- **Decision 2026-09-29 (user): drivers and services are PROCESSES FROM THE START** (M7 onward: xHCI, hub, HID,
  console, shell, FAT32, NIC all brought up as processes). The kernel build of a driver stays as an optional
  tool (`drivers=kernel`, still built so the discipline holds), not a milestone requirement. ARCHITECTURE.md
  "The migration rule" rewritten.

## PC facts (ASUS TUF GAMING B760-PLUS WIFI, i7-14700 non-F)
28 CPUs (8P+HT, 12E), 32 GB, RTX 4080 SUPER (monitor on it; framebuffer 2560x1440), iGPU UHD 770
present (unused). One USB controller: Intel xHCI 8086:7A60 rev 0x11 (keyboard, mouse, stick), an
ASMedia USB 3 hub (174C:2074/3074; keyboard probably behind it -> M7 hub driver), composite HID
devices (Cooler Master 2516:01C9/01C1, Sino Wealth 258A:0033, Microdia 0C45:652F), ASUS AURA
0B05:19AF. Ethernet: Realtek RTL8125 2.5GbE (M9 native driver); user HAS an Ethernet cable ready (2026-09-29). **Jam OS must only ever send on VLAN 21** (user requirement; never untagged/other VLANs; fail closed; see ARCHITECTURE Networking). Unknown yet: whether the switch port is a trunk (Jam OS tags) or access on 21 (switch tags) - ask at M9. Wi-Fi AX201 (not planned). Working
COM1 UART (no cable/parts; user prefers logs via stick in M8 / network in M9). XSAVE xcr0=7 (832 B).

## How we work with the user (see memory too)
- Stick: flash immediately when the user says it's in (build, copy jamos.elf + bootfs.img +
  limine.conf to "/Volumes/NO NAME", cmp, eject; wait for the mount if /boot isn't there yet).
  Don't add long QEMU pre-checks before flashing.
- PC checks: 2-min stress after each fix round, 10-min only as milestone sign-off. The kernel
  prints a RESULTS box at the end; the user reads lines or sends a phone photo (HEIC in ~/Downloads;
  convert with `sips -s format png`).
- Ask only for the specific lines needed; tell the user exactly which boot entry to pick.
- No Co-Authored-By trailer on commits. Big agent-written code gets an independent review agent.
- The user said to ignore the old blueprint artifact.

## Roadmap additions made 2026-09-29 (all in ARCHITECTURE.md)
M5.5 perf pass (row); M7 = xHCI -> hub -> HID (kb+mouse) -> console -> shell; M8 saves every boot's log
to /data/logs; M8.5 crash kernel (kdump-style kexec) + kexec fast reboot with own AP startup; M9 =
RTL8125 + lwIP + netlog + `update` (fetch kernel from the Mac, kexec); M13 self-hosting; graphics
track G1-G4 after M12 (compositor on the GOP framebuffer; mode setting only via the Intel iGPU).

## History (older notes, newest first)

**2026-09-29 review fix pass (fix agent's branch, on top of the review merge 0d196db)**: all 8
findings fixed, one commit each, every review test moved into the default run as `quota_*`
(kernel/test/test_quota.c; test_review.c is gone, the `review_` skip rule stays for future
reviews). R1 VMO table pages charged (page charged before any table is built); R2 user page
tables + PML4 + a page per 16 mappings charged to the process's job; R3 jobs max 32 deep and
cost their parent a handle unit; R4 new RIGHT_MANAGE (set_limit, job_kill), SR_JOB comes
without it (JOB_RIGHTS_OWN); R5 `starting` flag: no thread_start while process_start makes the
first thread, no panic; R6 threads cost 17 pages, process/VMO objects a handle unit, port
packets/bindings + message handles msg bytes, root job carves handle/msg budgets out of pages;
R7 debug_write prints outside its lock, 100 lines then 50/s per process; R8 job_kill syscall
(80) + job tree lists, init kills a timed-out program's job, userboot kills the root job if init
times out. QEMU 4+8 CPUs: 131/131 ktests, utest 14/14 under init with the root job clean,
stress=20, all crash tests. Seen once in 9 stress runs (4 CPUs): "counter#14 made no progress
for 10 s" (kernel counter threads on the barging counter mutex; code this pass didn't touch;
not reproduced after). Next: merge, then the PC run.

**2026-09-29 latest: M5 phase 2 MERGED (ee670aa, v0.0.8-m5).** Main: 122/122 ktests + stress at 4+8,
init + utest 12/12 at 4+8 with the root job clean afterwards (0 pages/handles/threads/msg bytes), all
15 crash tests OK. An INDEPENDENT REVIEW agent (user's rule: big agent code gets a separate reviewer)
is reviewing the processes code (range 5a2ae4e..worktree-agent-ab062027be2f4bc40) and will commit
failing regression tests only, no fixes. After its report: fix pass with tests, then the PC run
("Jam OS (init + utest)", All tests 122+, Benchmark -> M5 column of BENCH.md, 10-min stress).
PC: the phase-1 build (0.0.7-m5-phase1) also PASSED the 10-min stress (no problems), so the rewritten
interrupt/syscall entry path is confirmed on real hardware.
**Review DONE:** no kernel crash/UAF/cross-process access found; 4 CONFIRMED quota escapes (R1 VMO
table pages uncharged, R2 user page tables uncharged, R3 unbounded free job chains, R4 process can
raise its own job limit) with failing tests in kernel/test/test_review.c (run: ktest=review_...;
skipped by plain ktest), plus R5 process_start OOM race panic (plausible), R6 other uncharged kernel
memory, R7 debug_write IRQs-off printing, R8 no job_kill. Review tests merged into main. A FIX agent
is working on R1-R8. **PC 2026-09-29, 0.0.8-m5: "Jam OS (init + utest)" PASSED on the real PC**: utest 12 passed,
utest exited 0 after 743 ms, init exited 0 after 779 ms, root job clean (0 pages/handles/threads/
msg bytes). First real user processes on the PC. "All tests" first panicked in pcp_cross_cpu_free (a test race with lazy
thread reaping, +1 free page; fixed fc78454), then PASSED 122/122 on the PC. Benchmark recorded (BENCH.md M5 column). The 10-min stress first FAILED at
14 s ("a dead process left something charged to its job": thread_left credited the job after
the unlock; fixed d010dc4), then PASSED with 0 failures on the PC. **Every M5 check has passed
on the PC. Remaining for M5 ✅: merge the review-fix agent's work (R1-R8) and rerun on the PC.**
**Review fixes MERGED 2026-09-29 (6727edf):** RIGHT_MANAGE, job depth cap 32, VMO + user page tables
+ kernel objects charged, process_start STARTING window, debug_write printed outside the lock + rate
limit, job_kill (syscall 80). Merge fix: both branches moved the thread credit, the merge had it
twice -> kept one. Main: 131/131 + stress at 4+8, utest 14/14 with clean root job. OPEN: the fix agent
saw one 4-CPU stress failure "counter#14 made no progress for 10 s" (kernel mutex has no hand-off to
waiters, so a waiter can starve) - look at mutex fairness after M5.5 merges (sched.c is M5.5's).
PC round on 6727edf: init+utest PASSED (utest 14 passed, root job clean); All tests first panicked in
proc_debug_write_rate_limited (fixed bound vs the PC's slow console; ad57a6f), then PASSED 131/131.
Stress pending -> mark M5 done.
**M5.5 agent started 2026-09-29 (base 3259578)**, in parallel with the review-fix agent: spin-before-idle,
hybrid placement, sibling-HT pairs, per-CPU kmalloc caches, per-CPU one-shot timers, interrupt-driven
serial, PCIDs (last; the fix agent also edits aspace.c), and the two M5 bench regressions. Every
optimisation gets a runtime switch + cmdline word and bench measures off|on in the same PC run. The PC is meanwhile running the pre-fix 0.0.8-m5 build (init+utest, All tests,
Benchmark for BENCH.md M5 column, stress).

**M5 phase 2 DONE in QEMU (branch of the phase-2 agent, 2026-09-29)**: process/thread/job
objects (object/process.c, object/job.c), every sysc_* (abi/sysc_*.c), kill = cancel all
threads + last one out tears down, job charges (VMO pages, handle slots, threads, message
bytes to the sender), userboot (core/userboot.c) runs bin/init on a plain boot or `init`,
init runs init.cfg lines as child processes, utest = 12 ring-3 tests, libos spawn()/
thread_spawn(), stress has a user-process worker, bench has `user:` lines + address-space
switch, m5_weak.c deleted, version 0.0.7-m5. QEMU at 4+8 CPUs: 115/115 ktests, utest 12/12,
root job clean after init, stress=20 with ~1600 processes killed at random, all crash
tests. Merge notes: a parallel agent touched pmm.c / stack cache / channel_call placement;
this branch touched sched.c (thread_try_create_on, reap drops leftover aspace, PRIO_USER_MAX,
struct thread `uthread`) and channel.c (msg_free + job charge in msg_new, kfree(m) ->
msg_free(m) at 5 sites). **For the PC**: copy build/jamos.elf + build/bootfs.img to boot/
and boot/limine.conf to boot/limine/ on the stick; run "Jam OS (init + utest)" (expect
`utest: 12 passed`, init exit 0, root job clean), "All tests" (expect 115), the Benchmark
(new `user:` lines -> BENCH.md M5 column), and the 10-minute stress.

**M5 started 2026-09-29** (user: "write the m5 plan and then get agents to start building
it"). Plan = M5-PLAN.md. Foundation commit 9160daf (headers uentry/usercopy/aspace/syscall/
startup/bootfs/elf + weak stubs core/m5_weak.c + arch_thread_switch hook + #PF hook). Three
background worktree agents launched: Track A entry path, Track B address spaces/VMAR,
Track C userland/ABI generator/bootfs/ELF. When they report: review each branch, merge
A/B/C into main (expect conflicts in Makefile/limine.conf/sys.h/object.h), rerun all tests
at 4+8 CPUs, then phase 2 (processes, syscall glue, userboot, jobs, utest). **M4.5 CONFIRMED on the PC 2026-09-29: All tests 78/78 and the 10-min stress passed
(0 failures).**

**Track C MERGED 2026-09-29** (userland build, abi/syscalls.def + tools/gensyscalls.py,
bootfs, elf.c; 83/83 + stress at 4+8 CPUs). Phase-2 notes from C: implement strong sysc_* per
jam/syscall_impl.h; bootfs VMOs are physical and NOT write-protected, so hand out bootfs
handles without RIGHT_WRITE and COPY the ELF RW segment (never map it); libos needs
SR_SELF_VMAR + SR_BOOTFS; startup msg <= 8 KiB / 128 strings. The USB stick now also needs
build/bootfs.img copied to boot/ (limine.conf has module_path on every entry).
**Track B MERGED 2026-09-29** (e7fa047: aspace.c, VMO reverse map + gather, O8 batched
decommit, tlb_shootdown_mask, vmar objects; main now 96/96 + stress at 4+8). Phase-2 notes
from B: a thread's aspace ref must outlive its last time on a CPU (aspace_unref panics if
still loaded); arch_thread_switch calls aspace_switch(prev->aspace,next->aspace) irqs off;
aspace_fault takes the region mutex, so never hold it (or any spinlock) across a user copy;
#PF err W->ASPACE_WRITE, I/D->ASPACE_EXEC else READ; ERR_OUT_OF_RANGE from aspace_fault =
kill (bus error); vmar_create_for(as) for SR_SELF_VMAR; userboot passes ASPACE_CAN_* bits.
**Track A MERGED 2026-09-29** (syscall.S, isr.S rewrite with swapgs + paranoid
IST entry, uentry.c, usercopy.S + __ex_table, fpu.c XSAVE, SMEP/SMAP/UMIP; main = 107/107 +
stress at 4+8, all crash tests incl. testsmap/testsmep, and 107/107 with -smap,-smep).
Phase-2 notes from A: user thread = t->aspace + fpu_ustate_alloc(t) + stack_top, then
arch_enter_user(entry, stack, arg0, arg1); return_to_user_work() and kill_current() in
uentry.c currently thread_exit() -> replace with process kill; test CR3 hooks
(#ifndef JAM_NO_KTESTS) in arch_thread_switch/syscall_entry_c/trap.c; FS/KERNEL_GS bases
zeroed at entry (no TLS yet); aspace teardown must wait until no CPU has it loaded.
**PC 2026-09-29, build 0.0.7-m5-phase1 (4b4db91):** All tests passed (107, first real ring-3
code on the PC), fpu XSAVE xcr0 7 / 832-byte state, smep=smap=umip=1; testsmap and testsmep
panic with the right messages. 10-min stress: PASSED.
**Phase 1 complete. Phase 2 started 2026-09-29 with two worktree agents:** (1) processes/
threads/jobs, all sysc_* glue, kill, quotas + no-panic OOM, userboot, init spawning utest,
utest suite, user-path bench lines, delete m5_weak.c; (2) per-CPU page caches, stack cache
limit + failable kstack_alloc, channel_call wake-affine hand-off, per-thread priority ceiling.
When both report: review, merge (sched.c overlap expected), full tests 4+8, then PC run. **Perf agent MERGED 2026-09-29** (pmm per-CPU stashes 64/16 with
raw stash locks outside lockdep, kstack_free + kstack_alloc_try, prio_cap/PRIO_USER_MAX 24 +
thread_create_capped, thread_wake_sync / thread_set_wake_sync used by channel_call; main 114/114
+ stress at 4+8). QEMU can't measure page-alloc scaling (TCG slow-page artifact, see BENCH.md):
PC must show "all 28 CPUs" page line drop from 19 us toward ~50 ns, new "server unpinned" ~622 ns,
"server not on P" ~1268 ns, M4 calls/s not below 836k. Processes agent still running; its merge
will conflict in sched.c (thread struct, thread creation, stack cache) - use kstack_alloc_try
for ERR_NO_MEMORY and thread_create_capped / PRIO_USER_MAX for user threads.


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
