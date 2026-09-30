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
| **M8** | **Storage** | **in progress**: the stick's `/esp` and `/data`, files from the shell, a log per boot and other sticks at `/usbN` work in QEMU; the review and the PC are still to do |
| A1, A2 | Audio | right after M8 |
| AS | Boot splash: the logo animation with its sound, alpha blending | right after A2 |
| M8.5 | Crash kernel and kexec | later |
| M9 | Networking | later |
| M10 | ACPI power | later |
| M11 | IOMMU | later |
| M12 | Own loader, POSIX, stable ABI | stretch |
| M13 | Self-hosting | stretch |
| G1-G4 | Graphics | after M12 |

## Next: M8, storage

USB mass storage (Bulk-Only Transport; UAS later) as a class driver on
usb-bus, and a FAT32 filesystem service, both processes. The stick gets a
read-only boot partition (the ESP) and a writable data partition mounted at
`/data` ([ARCHITECTURE.md](../ARCHITECTURE.md#storage)); every boot's log
is saved as `/data/logs/boot-NNNN.txt`. The plan is
[M8-PLAN.md](M8-PLAN.md).

Done when: `ls /boot` and writing a file under `/data` work from a
userspace filesystem service; the stick still boots after a pulled-plug
test; a PC run's log can be read on the Mac from the stick.

Implemented, and passing in QEMU at 4 and 8 CPUs:
- Bulk transfers in usb-bus and the `drivers/usb-storage` class driver
  (Bulk-Only Transport, the partition table, a `block` channel per
  partition).
- FatFs (in `third_party/fatfs`) as the `fat` service: long names, lower
  case, spaces; the ESP mounted read-only at `/esp`, the data partition at
  `/data` (formatted on its first boot).
- devmgr finds the disks and partitions, starts a `fat` per volume and
  publishes the mounts; init keeps the system's file namespace (`/boot`
  from the bootfs server, `/esp`, `/data`) and hands each program its part
  of it; a mount comes and goes in running programs as the stick does.
- Programs open files by path (`<os.h>`), and a program can be started
  from a file. The shell has `ls cat cp mv rm mkdir touch write df sync`
  on every mount.
- logd writes each boot's kernel log to `/data/logs/boot-NNNN.txt`;
  `reboot` and Ctrl+Alt+Del sync `/data` first and have logd save the
  log's last lines.
- `kill` goes through init and devmgr (`debug_command` no longer has it).
- Other sticks: each FAT volume of any other USB stick (an MBR's FAT
  partitions, or a stick with no partition table that is one FAT volume)
  is mounted read-only at `/usb0`, `/usb1`, ... in the order found, on a
  `block` channel usb-storage itself refuses writes on. `mount` lists the
  mounts and which are writable; `mount -w /usb0` reopens one read-write,
  `mount -r /usb0` read-only again. Nothing but the boot stick's own blank
  data partition is ever formatted. GPT sticks are not read yet.
- Tests: the file namespace in utest, the storage checks in usbtest,
  `tools/storage-test.sh`, and `tools/data-test.sh` (three boots of one
  stick: a file kept across a reboot, QEMU quit in the middle of writes,
  the logs read back with mtools); the stick unplugged and replugged while
  the system runs; `tools/sticks-test.sh` (other sticks plugged, written
  after `mount -w`, pulled while in use, and the images of the ones that
  were only read or held nothing to mount compared byte for byte).

The independent review is done ([its findings and what became of
each](history/M8-REVIEW.md)).

Still to do:
- The PC: the stick flashed once with the two-partition layout
  (`make usb`, which erases it), then read-only checks first (`ls /esp`,
  `ls /data`), writes, the boot logs read on the Mac, the pulled-plug test,
  All tests, then `stress 600` and `soak 10` (M8 is signed off by both;
  from then on the soak alone, [TESTING.md](TESTING.md#the-tiers)).

Known limits, for the review:
- Only programs in `/boot` can be run: a file on `/data` or `/esp` does
  not come with the right to execute it.
- A panic's own text is not in the boot log: logd can only save what it
  had synced before (M8.5's crash kernel is what saves a panic).

## Later

| # | What | Done when |
|---|---|---|
| A1 | HD Audio driver + `beep`: controller reset, command rings, codec widget graph, the front-panel headphone pin, one output stream from a pinned DMA32 buffer, jack detection | `beep` in the shell plays a tone in the headphones on the real PC; unplugging and replugging them is logged |
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

Offered or noticed, not scheduled into a milestone yet:

- usb-bus retries a failed root port after 1 s, then 5 s; ports on hubs
  are still looked at again only on a port status change (the same
  pattern would fit `hub->port_fail` in hub.c).
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
- The rest of the shell was not covered by the cleanup's review (only the
  segment fix and `kill` were): review it next time.
- A pluggable scheduler interface (`sched_ops`): not until a second
  policy is needed.

## Not planned

- `fork` (a POSIX layer gets `posix_spawn`).
- SSH (discussed and not added).
- Wi-Fi.
- User accounts: single user; handles are the only authority.
