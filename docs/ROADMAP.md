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
| **M8** | **Storage** | **next** |
| A1, A2 | Audio | right after M8 |
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
is saved as `/data/logs/boot-NNNN.txt`.

Done when: `ls /boot` and writing a file under `/data` work from a
userspace filesystem service; the stick still boots after a pulled-plug
test; a PC run's log can be read on the Mac from the stick.

Notes for the plan (write the plan as docs/M8-PLAN.md before launching
agents, like the [earlier plans](history/)):
- The stick (058f:6387) is high-speed and sits behind the ASMedia hub, so
  storage goes through the hub ([HARDWARE.md](HARDWARE.md#usb)).
- The shell's mount table (`user/services/shell/sh_vfs.c`) is ready for
  `/data`.
- Design work the cleanup left for this plan: a system file namespace
  protocol (an `fs` IDL), bulk data through shared VMOs in IDL, and shrinking
  `debug_command`.
- FAT names: the 2025 attempt's hand-written FAT made naming files
  painful (no spaces from its shell; `notes.txt` shown as `NOTES.TXT`
  because the lowercase flags were never set; long names cut to 8.3, or
  every alias `~1` so aliases collided; forbidden characters accepted).
  Decided: **port FatFs** (ChaN's FatFs, BSD-style licence) instead of
  writing FAT32 again; it gets long names, the case flags, `~N` numbering
  and the character rules right. Vendor it in a new third_party/fatfs directory with
  its licence and a `third_party/VERSIONS.md` entry; the FAT service
  supplies FatFs's disk callbacks (read/write sectors through the block
  service) and runs as a process like any other service.

## Later

| # | What | Done when |
|---|---|---|
| A1 | HD Audio driver + `beep`: controller reset, command rings, codec widget graph, the front-panel headphone pin, one output stream from a pinned DMA32 buffer, jack detection | `beep` in the shell plays a tone in the headphones on the real PC; unplugging and replugging them is logged |
| A2 | `audio` protocol + mixer service: streams through shared VMO rings, volume from the shell, WAV playback from `/data` | two programs play at once |
| A3 | Microphone input, HDMI/DP audio on the RTX, USB audio devices | (not planned in detail) |
| M8.5 | Crash kernel, the Linux kdump approach: reserve RAM at boot and load a second Jam OS there; on a panic jump into it (kexec, one CPU, controllers reset before use), save the crashed kernel's log ring as `/data/logs/boot-NNNN-crash.txt`, reboot. The same kexec gives `reboot` a fast path, which needs the kernel's own AP startup (INIT-SIPI-SIPI) so all 28 CPUs come back without Limine. A RAM log kept across a warm reset (pstore) only as a fallback | a deliberate panic on the PC ends with its full log as a file on the stick; `reboot` kexecs into the kernel on the stick with all CPUs up, without a firmware reboot |
| M9 | RTL8125 driver, lwIP, DHCP/DNS (processes), **VLAN 21 only** ([the rule](../ARCHITECTURE.md#networking)); netlog (the kernel log over UDP to the Mac); `update` (fetch a new kernel + bootfs from the Mac and kexec). Ask the owner first whether the switch port is a trunk or an access port on VLAN 21 | `ping 1.1.1.1` on the PC through a userspace network stack; a PC run's full log arrives on the Mac; `make` on the Mac + `update` on the PC runs the new build with no stick moved |
| M10 | uACPI: poweroff, power button, ACPI reboot (uACPI stays in the kernel); tickless idle | clean shutdown on real hardware |
| M11 | IOMMU (VT-d) and interrupt remapping behind `dma_cap` | DMA outside a driver's pinned VMOs is blocked |
| M12 | S3 sleep, own UEFI loader, POSIX on musl, stable syscall ABI | stretch |
| M13 | Self-hosting: the build tools rewritten in C, then make, binutils and GCC ported onto M12's POSIX layer; a small compiler (TCC/cproc) may come first | Jam OS rebuilds itself on the PC and boots the result, with no Mac involved |
| G1 | A compositor on the firmware framebuffer: apps draw into their own surface VMOs, input routed to the focused client, software rendering | |
| G2 | Toolkit, TrueType fonts, GUI apps | |
| G3 | Mode setting and vsync, only through the Intel iGPU (needs the monitor on the board's output and the iGPU enabled) | |
| G4 | 3D as a stretch: a multi-core software rasterizer, or virtio-gpu under QEMU | |

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
- SSH (discussed; the owner chose not to add it).
- Wi-Fi.
- User accounts: single user; handles are the only authority.
