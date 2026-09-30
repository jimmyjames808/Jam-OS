# Roadmap

The one place milestone status lives. What each finished milestone
delivered is in [HISTORY.md](HISTORY.md); the design they build is in
[ARCHITECTURE.md](../ARCHITECTURE.md). Every milestone is checked on
[the real PC](HARDWARE.md): QEMU passing is not "done".

## Status

| # | Milestone | Status |
|---|---|---|
| M0 | Toolchain, QEMU q35/OVMF, USB image, framebuffer console, panic screen | done |
| M1 | PMM, VMM, heap, GDT/TSS/IDT, panic screen with symbols | done |
| M2 | ACPI tables, LAPIC/IOAPIC, TSC and APIC timers, all 28 CPUs, P/E topology | done |
| M3 | Scheduler, kernel threads, spinlocks + lock-order checker, IPIs, TLB shootdown, watchdog, stress test | done |
| M4 | Objects, handles, channels, ports, events, timers, VMOs | done |
| M4.5 | Hardening: W^X everywhere, lock checker scaling, cancellable waits | done |
| M5 | Ring 3, syscalls, address spaces, processes, threads, jobs + quotas, init | done |
| M5.5 | Performance pass, measured on the PC ([BENCH.md](BENCH.md)) | done |
| M6 | PCI core, MSI/MSI-X, interrupt objects, resources, DMA, devmgr, drivers as processes | done |
| M7 | USB (xHCI, hubs, HID), console, shell, driver supervision | done |
| M7.5 | Cleanup, no behaviour change | done (PC 2026-09-30: All tests 221, shell ktest 212, 10-minute stress passed) |
| M8 | Storage: USB mass storage, FAT32 (FatFs), `/esp` and `/data`, other sticks at `/usbN`, a log per boot | done (PC 2026-10-01: All tests 224, `stress 600` and `soak 10` passed) |
| **A1** | **Audio: HD Audio driver, `beep`** | **in progress**: stage 0 (the read-only probe) ran on the PC (a Realtek ALC897); stages 1 and 2 done in QEMU; stage 3 (`beep`) next ([A1-PLAN.md](A1-PLAN.md)) |
| A2 | Audio: mixer, `audio` protocol | after A1 |
| AS | Boot splash: the logo animation with its sound, alpha blending | right after A2 |
| M8.5 | Crash kernel and kexec | later |
| M9 | Networking | later |
| M10 | ACPI power | later |
| M11 | IOMMU | later |
| M12 | Own loader, POSIX, stable ABI | stretch |
| M13 | Self-hosting | stretch |
| G1-G4 | Graphics | after M12 |

## Next: A1, audio

HD Audio on the PC's Realtek ALC897 (Intel 8086:7a50 controller) as a
driver process, and `beep` in the shell playing a tone in the
front-panel headphones. The plan is [A1-PLAN.md](A1-PLAN.md). Stage 0 (a
read-only probe of the controller and codec) ran on the PC; stages 1
(codec control and the path to the front headphone jack) and 2 (the
output stream) are done in QEMU; stage 3 (`beep`) is being built.

M8 (storage) is done: [what it delivered](HISTORY.md#m8-storage).
Known limits it left:
- Only programs in `/boot` can be run: a file on `/data` or `/esp` does
  not come with the right to execute it (decided: after M8).
- A panic's own text is not in the boot log: logd can only save what it
  had synced before (M8.5's crash kernel is what saves a panic).
- GPT sticks are not read.

## Later

| # | What | Done when |
|---|---|---|
| A1 | HD Audio driver + `beep`: controller reset, command rings, codec widget graph, the front-panel headphone pin, one output stream from a pinned DMA32 buffer, jack detection. The plan is [A1-PLAN.md](A1-PLAN.md) | `beep` in the shell plays a tone in the headphones on the real PC; unplugging and replugging them is logged |
| A2 | `audio` protocol + mixer service: streams through shared VMO rings, volume from the shell, WAV playback from `/data` | two programs play at once |
| AS | Boot splash, at the end of the audio track. A splash program is the first thing init starts: it takes the screen and plays the boot animation with its sound while the services start, holds the last frame until the shell is ready, then hands the screen back. The animation is made outside the repository (1920x1080 with alpha); `make` converts it with ffmpeg to MPEG-1 at 1280x720 over the dark background (about 0.5 MB, drawn 2x to fill 2560x1440) and puts it in the boot image. Decoded by pl_mpeg (MIT, in `third_party/`), sound through A2's mixer. A key skips it; a `verbose` boot word shows the text log instead; a panic always draws over it. With it, alpha blending in libfun (premultiplied alpha, blended fills and images, anti-aliased shapes) | the PC boots into the animation with its sound, and the shell comes up when it ends |
| A3 | Maybe: USB audio devices (headsets, USB sound cards). HDMI/DisplayPort audio through the RTX is not planned | (not planned in detail) |
| M8.5 | Crash kernel, the Linux kdump approach: reserve RAM at boot and load a second Jam OS there; on a panic jump into it (kexec, one CPU, controllers reset before use), save the crashed kernel's log ring as `/data/logs/boot-NNNN-crash.txt`, reboot. The same kexec gives `reboot` a fast path, which needs the kernel's own AP startup (INIT-SIPI-SIPI) so all 28 CPUs come back without Limine. A RAM log kept across a warm reset (pstore) only as a fallback | a deliberate panic on the PC ends with its full log as a file on the stick; `reboot` kexecs into the kernel on the stick with all CPUs up, without a firmware reboot |
| M9 | RTL8125 driver, lwIP, DHCP/DNS (processes), **VLAN 21 only** ([the rule](../ARCHITECTURE.md#networking)); netlog (the kernel log over UDP to the Mac); `update` (fetch a new kernel + bootfs from the Mac and kexec). Find out first whether the switch port is a trunk or an access port on VLAN 21 | `ping 1.1.1.1` on the PC through a userspace network stack; a PC run's full log arrives on the Mac; `make` on the Mac + `update` on the PC runs the new build with no stick moved |
| M10 | uACPI: poweroff, power button, ACPI reboot (uACPI stays in the kernel); tickless idle | clean shutdown on real hardware |
| M11 | IOMMU (VT-d) and interrupt remapping behind `dma_cap` | DMA outside a driver's pinned VMOs is blocked |
| M12 | S3 sleep, own UEFI loader, POSIX on musl, stable syscall ABI | stretch |
| M13 | Self-hosting: the build tools rewritten in C, then make, binutils and GCC ported onto M12's POSIX layer; a small compiler (TCC/cproc) may come first | Jam OS rebuilds itself on the PC and boots the result, with no Mac involved |
| G1 | A compositor on the firmware framebuffer: apps draw into their own surface VMOs, input routed to the focused client, software rendering | |
| G2 | Toolkit, TrueType fonts, GUI apps | |
| G3 | Mode setting and vsync, only through the Intel iGPU (needs the monitor on the board's output and the iGPU enabled) | |
| G4 | 3D as a stretch: a multi-core software rasterizer, or virtio-gpu under QEMU | |

## Design ideas, not scheduled

Larger pieces that fit the design and would be worth a milestone each.
None has a plan yet; the order is the current preference.

- **A faster call path.** A process-to-process call costs 1407 ns
  ([BENCH.md](BENCH.md)), and most of that is not the price of isolation:
  one call is several kernel entries, copies and handle lookups. A
  combined reply-and-wait call, a direct hand-off to a waiting server and
  one copy should bring it to roughly 400-600 ns (an estimate).
- **Shared request rings.** A client and a service share a ring of
  requests and replies in a VMO and make a system call only when the other
  side is asleep: the third level after copied messages and shared VMOs
  for bulk data. The ring layout would be generated from the IDL.
- **User-space pagers.** A process (a filesystem service) supplies the
  pages of a VMO on demand. It gives mapped files (a cached read becomes a
  memory copy), programs loaded on demand, and the clean way to run
  programs from `/data`.
- **Services that outlive their process.** A service's queues and state
  live outside the process, so a restarted service reconnects to them and
  its clients never see the crash; with a warm spare the restart is fast
  enough to measure as a benchmark. Today a restart is visible to every
  client.
- **A service dependency graph.** devmgr and init know the order by code
  (filesystems, then USB class drivers, then the bus drivers). Declared
  dependencies would drive start, stop and restart order instead.
- **Leases on handles.** Handles given out for one device binding are
  revoked together when the device or its driver goes, including the ones
  passed on to other processes; jobs already reclaim what the dead process
  itself held.
- **CPU time as a capability.** A budget and period per service, so one
  cannot starve the rest; a server can then run on its caller's budget.
  For when audio or a compositor needs guaranteed time.
- **Protection-key domains** (Intel PKU): a trusted-but-buggy service in
  its caller's address space behind a protection key, a call costing tens
  of nanoseconds. It keeps crash containment for bugs and gives up
  protection against a malicious service, so it would be an optional mode
  next to processes, measured side by side.

## Smaller follow-ups

Offered or noticed, not scheduled into a milestone yet. The first ones
are the design questions M8 left open
([its review](history/M8-REVIEW.md) has the details):

- **Running programs from `/data`**: decided to wait until after M8, so
  due now. Code runs only from a VMO with `RIGHT_EXEC`, which only the
  boot image's has; the choice is who may make a file executable (and
  whether the answer is user-space pagers, under Design ideas).
- **Which disk is the boot disk.** devmgr takes the first disk with Jam
  OS's layout and kernel, not the one the machine booted from: with two
  Jam OS sticks in, enumeration order decides which becomes `/data` (and
  is formatted if blank). Limine reports the boot volume's MBR disk id;
  devmgr could be told it.
- **Every program gets every mount read-write**: init gives the shell all
  of them and the shell passes them all on. A narrower namespace per
  program (no `/data`, or read-only) is possible and unused.
- **devmgr's job check at a driver's exit** can report a driver that ended
  cleanly as "did not end cleanly" (seen on the PC for a hid after a warm
  reboot: a request it left queued at usb-bus is still charged to its
  job). A second look a moment later, before counting it as a problem,
  would cover it.
- **logd loses lines during the klog flood test**: the kernel test
  `console_klog_read_after_gap` writes more than the 64 KiB kernel log
  ring holds, on purpose, and no reader can follow it; each live `ktest`
  leaves a "[logd: N bytes of the log were lost]" line. Skip that test live, or accept
  it. A bigger ring would also cover the log written while the boot stick
  is out.
- **GPT sticks** are not read (their partitions are not mounted).
- One bulk transfer at a time inside usb-bus's loop, and devmgr's
  bounded waits (up to 2 s) on a slow usb-storage: both block other work
  meanwhile. Asynchronous transfers would be a redesign of the serve loop.
- `RIGHT_READ` on the root resource is one right for the kernel log, the
  serial port, the process list and the clock: sysmon and logd each get
  more than they use.
- `make flash` copies the three files in place, so a stick pulled half
  way does not boot (copy to a new name, then rename, would not).
- The first `make -j8` after a new file in `abi/idl/` can spin forever;
  run again, it builds. Not looked into.
- Names and volume labels from someone else's stick are printed as they
  are, escape sequences included; `ls` and `find` show a directory's
  first 256 entries and say nothing about the rest; init's loop waits up
  to 25 s for a `mount` and 15 s for a `kill`.
- The mouse wheel's scroll-back passes QEMU's mouse test but does not work
  on the PC: not looked into yet. The mouse test runs at 1280x800 only.

The rest:

- usb-bus retries a failed root port after 1 s, then 5 s; ports on hubs
  are still looked at again only on a port status change (the same
  pattern would fit `hub->port_fail` in hub.c).
- The `init` QEMU run ends "with problems" about one run in three: usbtest's last check restarts usb-storage, devmgr starts `fat-esp`
  again, and devmgr's shutdown stops usb-bus while that fat is still
  mounting; it exits ERR_IO and devmgr reports "fat-esp bin/fat did not
  end cleanly". A fat that finds its disk gone while mounting could exit 0,
  or devmgr could excuse a filesystem service whose disk went first.
- devmgr's protocol is hand-written, not IDL.
- `console.write` always sends a 2048-byte array; variable-length IDL
  arrays would fix it.
- The address-space switch got ~20 ns dearer with PCIDs on
  ([BENCH.md](BENCH.md)); look at `pcid_load`'s bookkeeping.
- Kernel waits bounded by an iteration count or not at all: the xAPIC ICR
  wait, `serial_rx_start`'s drain, interrupt teardown, `on_cpu`, the
  pmm/heap flag locks and the lockdep graph lock (from the kernel style
  pass).
- Files still over 800 lines: `vmo.c`, `sched.c`, `aspace.c` (splitting
  them needs internal headers).
- Most of the shell has had no independent review (the cleanup's covered
  the segment fix and `kill`, M8's the file commands): review the rest
  next time.
- `job_find_process` is used only by its own test.
- A pluggable scheduler interface (`sched_ops`): not until a second
  policy is needed.

## Not planned

- `fork` (a POSIX layer gets `posix_spawn`).
- SSH (discussed and not added).
- Wi-Fi.
- HDMI/DisplayPort audio through the RTX.
- Storage other than USB sticks with FAT32: an NVMe driver and other
  filesystems were discussed and declined.
- User accounts: single user; handles are the only authority.
