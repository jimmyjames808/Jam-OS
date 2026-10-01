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
| **A1** | **Audio: HD Audio driver, `beep`** | **in progress**: stage 0 (the probe) found the PC's codec, a Realtek ALC897 with the front headphone jack on pin 1b; stages 1-3 work in QEMU: the path DAC 02 -> mixer 0c -> pin 1b set up muted through an allow-list of SET verbs, one output stream on it (period interrupts over MSI, clear-behind), and `beep` (the path unmuted at -30 dB only while the stream runs; `hda gain`); sound works on the PC. Stage 4 (jacks): unsolicited responses with a tag per jack pin, the RIRB interrupt, an 80 ms debounce and a 500 ms polling fallback, `hda jacks`; checked in QEMU against fixtures and a fake codec (QEMU's codecs have no presence detection). The plug/unplug lines on the PC and the review are next ([A1-PLAN.md](A1-PLAN.md)) |
| A2 | Audio: mixer, `audio` protocol, WAV playback | **in progress** ([A2-PLAN.md](A2-PLAN.md)): in QEMU, programs play at once through the mixer service (each its own stream in a shared ring, mixed two periods ahead of the play position), `vol` sets each stream's volume and the master, `<audio.h>` (blocking writes, mono to stereo, any rate to 48 kHz) and `play <file.wav>` and `beep` go through it. Two sounds at once on the PC and the review are next |
| AS | Boot splash: the logo animation with its sound, alpha blending | **in progress** ([AS-PLAN.md](AS-PLAN.md)): in QEMU a plain boot is the splash's dark background from the kernel's start, the animation from bootfs's `splash.mpg` (pl_mpeg) with its sound through the mixer, the sound the clock once heard, the last frame held until the shell is up, then faded into the text; a key skips it, `verbose` or `nosplash` shows the log; libfun has premultiplied alpha and anti-aliased shapes. Seen on the PC at 720p ("lower quality"); now 2560x1440 (drawn 1:1, scaled down on smaller screens), picture and sound starting together, the logo lingering 2 s. The PC and the review are next |
| M8.5 | Crash kernel and kexec | **in progress** ([M8.5-PLAN.md](M8.5-PLAN.md)). The first version (a crash kernel as a mode of its own) worked on the PC 2026-10-01; reworked to the owner's Revision 2: one kernel, two ways in. In QEMU `reboot` and a panic both start the kernel's stored copy of the system (reserved, unmapped and checksummed at boot) with every CPU and all of RAM; the screen is the splash background until the next boot's splash; after a panic logd saves the log as `/data/logs/boot-NNNN-crash.txt` and the shell says so in one line; `reboot` reads no file unless `/esp` changed. The PC is next |
| M8.6 | Cleanup: the code checked against the coding guide, the queued design fixes, a block cache in fat, the log off the screen, a command to load the stick's kernel as the stored one, keyboard and mouse ready early in boot | later |
| M9 | Networking | later |
| M10 | ACPI power, tickless idle | later |
| M10.5 | S3 sleep | later |
| M11 | IOMMU | later |
| M11.5 | Performance pass: the IPC fast path, a Linux column in BENCH.md | later |
| M12 | Interface review, before anything is frozen | later |
| G1 | A compositor that speaks Wayland | after M12 |
| M13 | POSIX on musl | stretch |
| M14 | Stable syscall ABI | stretch |
| M15 | Self-hosting | stretch |
| G2-G4 | Toolkit and fonts, mode setting, 3D | after G1 |
| Maybe | Own UEFI loader in place of Limine | not planned |

## Next: A1, audio

HD Audio on the PC's Realtek ALC897 (Intel 8086:7a50 controller) as a
driver process, and `beep` in the shell playing a tone in the
front-panel headphones. The plan is [A1-PLAN.md](A1-PLAN.md). Stage 0 (a
read-only probe of the controller and codec) ran on the PC; stages 1
(codec control and the path to the front headphone jack), 2 (the output
stream) and 3 (`beep`, `hda gain`) work in QEMU, and sound plays on the
PC; stage 4 (jack detection) is built and checked in QEMU against
fixtures. Next: the plug/unplug lines on the PC, and the review.

M8 (storage) is done: [what it delivered](HISTORY.md#m8-storage).
Known limits it left:
- Only programs in `/boot` can be run: a file on `/data` or `/esp` does
  not come with the right to execute it (decided: after M8).
- A panic's own text is not in the boot log: logd can only save what it
  had synced before. Since M8.5 the next boot saves it next to the boot
  log (Revision 2 not yet on the PC).
- GPT sticks are not read.

## Later

| # | What | Done when |
|---|---|---|
| A1 | HD Audio driver + `beep`: controller reset, command rings, codec widget graph, the front-panel headphone pin, one output stream from a pinned DMA32 buffer, jack detection. The plan is [A1-PLAN.md](A1-PLAN.md) | `beep` in the shell plays a tone in the headphones on the real PC; unplugging and replugging them is logged |
| A2 | `audio` protocol + mixer service: streams through shared VMO rings, volume from the shell, WAV playback from `/data` | two programs play at once |
| AS | Boot splash, at the end of the audio track. A splash program is the first thing init starts: it takes the screen and plays the boot animation with its sound while the services start, holds the last frame until the shell is ready, then hands the screen back. The animation is made outside the repository (2560x1440 with alpha); `make` converts it with ffmpeg to MPEG-1 at 2560x1440 over the dark background (about 1.7 MB, drawn 1:1 on the PC) and puts it in the boot image. Decoded by pl_mpeg (MIT, in `third_party/`), sound through A2's mixer. A key skips it; a `verbose` boot word shows the text log instead; a panic always draws over it. With it, alpha blending in libfun (premultiplied alpha, blended fills and images, anti-aliased shapes) | the PC boots into the animation with its sound, and the shell comes up when it ends |
| A3 | Maybe: USB audio devices (headsets, USB sound cards). HDMI/DisplayPort audio through the RTX is not planned | (not planned in detail) |
| M8.5 | Kexec for reboot and panic (the owner's Revision 2 of a Linux kdump-style crash kernel): reserve RAM at boot and store a fresh copy of the same Jam OS there; `reboot` and a panic both jump into it (every CPU, all of RAM, every controller reset by its normal driver), so either looks like switching the PC on, except that after a panic the next boot saves the dead boot's log ring as `/data/logs/boot-NNNN-crash.txt` and the shell says so. Needs the kernel's own AP startup (INIT-SIPI-SIPI) so all 28 CPUs come back without Limine. Done properly or not at all: the stored kernel's RAM is unmapped from the running kernel and checksummed before the jump, the panic path takes no locks and allocates nothing, the panicking kernel never writes to the stick itself, bus mastering is off before the jump, a crash loop halts, and nothing relies on firmware keeping RAM across a reset (no pstore) | a deliberate panic on the PC comes back to the shell with its full log as a file on the stick; `reboot` kexecs with all CPUs up, without a firmware reboot or a file read when the stick is unchanged |
| M8.6 | Cleanup, as M7.5 was: fresh agents check the kernel, the drivers, user space and the tools against CODING-GUIDE.md and ARCHITECTURE.md (findings first, no edits), the owner and the main session sort them, then fixes in batches with behaviour-preserving changes kept apart. With it the audio review's open design questions: service channels in the namespace (`/svc/...`) in place of the shell's startup handles, well-formed UTF-8 through the kernel log, `play` as its own program holding only the audio service and the file; and the decision on running programs from `/data`. Also a block cache in fat (repeated reads served from memory, written back in the order ARCHITECTURE.md's storage rule needs; a restart only loses the cache) and one copy fewer in usb-storage: small, and nothing given up. And the log off the screen: the console stops echoing the kernel log (boot lines, drivers' and services' messages, hot-plug lines) into the shell; it still goes to `/data/logs` and the serial port, `log`/`dmesg` show it on demand, and the boot words `verbose`/`nosplash` and the "text log" boot entry still print it as today. Only the shell's own output and the programs it runs reach the screen. And a shell command (e.g. `kernel load`) that reads `/esp`'s kernel and boot image into the stored slot at once and says whether it loaded (`kexec_load`), so the owner can pull the stick, flash it on the Mac, plug it back and load the new build before rebooting: the reboot is then instant, and a panic after that point also comes back in the new build. `reboot` already reads a changed stick by itself; the command moves the read to when the owner chooses. And input early in boot (the PC's log: the USB driver starts at 1.06 s but the keyboard is usable only at 6.31 s, with the shell at about 6.4 s): usb-bus answers the HID drivers as soon as each device is configured instead of after every port; root ports reset and devices set up in parallel so one slow device delays only itself; the gaming mouse's (258a:0033) refused Address Device costs 3.2 s: short backing-off retries and the cause found; keys typed during the splash reach the shell; the boot timer self-check (1 s of waiting before user space starts) leaves the normal boot path (test entries, or in the background). Done when the boot log's own line says keyboard and mouse ready within about 2 s of the kernel's start | the findings fixed or written into the guide; All tests and `soak 10` on the PC |
| M9 | RTL8125 driver, lwIP, DHCP/DNS (processes), **VLAN 21 only** ([the rule](../ARCHITECTURE.md#networking)); netlog (the kernel log over UDP to the Mac); `update` (fetch a new kernel + bootfs from the Mac and kexec). Find out first whether the switch port is a trunk or an access port on VLAN 21 | `ping 1.1.1.1` on the PC through a userspace network stack; a PC run's full log arrives on the Mac; `make` on the Mac + `update` on the PC runs the new build with no stick moved |
| M10 | uACPI: poweroff, power button, ACPI reboot (uACPI stays in the kernel); tickless idle | clean shutdown on real hardware |
| M10.5 | S3 sleep (suspend to RAM) on top of M10's ACPI: every driver saves and restores its device | the PC suspends and resumes with USB, audio and the network working again |
| M11 | IOMMU (VT-d) and interrupt remapping behind `dma_cap` | DMA outside a driver's pinned VMOs is blocked |
| M11.5 | Performance pass, as M5.5 was, before M12 reviews and M14 freezes the system calls: the IPC fast path (one reply-and-wait call, a direct hand-off to a waiting server, one copy of the message, no FPU save on a voluntary switch, no failed read before each wait), the file and block calls' deadlines made cheaper, and every number measured again on the PC. BENCH.md gains a Linux column measured on the same PC (default and `mitigations=off`), per-operation lines (a cached 4 KiB read, a block read) next to per-call ones, and a column with the lock checker off | a process-to-process call on the PC at 600 ns or less (1407 ns today), nothing given up in isolation or restart |
| M12 | Interface review: the system calls and the service protocols reviewed and reshaped while changing them is cheap, before POSIX builds on them and M14 freezes them. The second cleanup point after M8.6, by fresh agents | the review's findings fixed; nothing frozen yet |
| M13 | POSIX on musl: file descriptors over handles, `posix_spawn` (no `fork`), paths through the namespace, then ported programs | unmodified POSIX programs (shell utilities, a small C program) build and run |
| M14 | Stable syscall ABI: frozen only after POSIX has put its weight on it; versioned and documented | old binaries keep running on new kernels |
| M15 | Self-hosting: the build tools rewritten in C, then make, binutils and GCC ported onto M13's POSIX layer; a small compiler (TCC/cproc) may come first | Jam OS rebuilds itself on the PC and boots the result, with no Mac involved |
| G1 | A compositor of our own on the firmware framebuffer that speaks the Wayland protocol: Wayland's model (surfaces, buffers, `xdg_toplevel`, a seat for input) with its wire format carried over channels, handles where Linux passes file descriptors, `wl_shm` pools as VMOs, the code generated from Wayland's XML protocol files as the IDL is. Apps draw into their own surfaces, input goes only to the focused client, a crashed app takes only its own window down, software rendering. The console becomes a client (a terminal window). Not a port: Weston and wlroots need Linux's DRM/KMS, Mesa, libinput and udev | windows from several programs on the PC's screen, the shell in one of them |
| G2 | Toolkit, TrueType fonts, GUI apps: ported leaf libraries (stb_truetype or FreeType, microui/Nuklear or LVGL, stb_image). After M13, libwayland ported with a shim from sockets and file descriptors to channels and handles, so Wayland programs that draw in software (foot, SDL) run unchanged; GTK and Qt are a later porting project of their own | |
| G3 | Mode setting and vsync, only through the Intel iGPU (needs the monitor on the board's output and the iGPU enabled) | |
| G4 | 3D as a stretch: a multi-core software rasterizer, or virtio-gpu under QEMU | |
| Maybe | Own UEFI loader in place of Limine: a third filler of `struct boot_info` (after Limine's and M8.5's kexec). Limine does the job, and after M8.5 the kernel needs it only to load three files and jump; our own would cost its boot menu (the test entries) and new firmware quirks for little gain. Revisit if self-hosting (M15) or Secure Boot ever needs it | (not planned in detail) |

## Design ideas, not scheduled

Larger pieces that fit the design and would be worth a milestone each.
None has a plan yet; the order is the current preference.

- **A faster call path.** A process-to-process call costs 1407 ns
  ([BENCH.md](BENCH.md)), and most of that is not the price of isolation:
  one call is several kernel entries, copies and handle lookups. A
  combined reply-and-wait call, a direct hand-off to a waiting server and
  one copy should bring it to roughly 400-600 ns (an estimate). Scheduled
  as M11.5.
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
- Reading a big file from `/esp` is slow: the ESP is FAT32 on 63 MiB, so
  its clusters are one sector, and FatFs reads a file a cluster (one bulk
  transfer) at a time. A kexec `reboot` after the stick's kernel changed
  reads 7.8 MB from it: about 10 s in QEMU, 10-20 s on the PC (an
  unchanged stick reads nothing). M8.6's block cache in fat with a
  read-ahead would fix it.
- The mouse wheel on the PC: hid drives a mouse whose report descriptor
  has a wheel in the report protocol (real mice send no wheel in the boot
  protocol); not yet confirmed on the PC. The boot log has each mouse's
  descriptor in hex, to add to `drivers/hid/fixtures.c`. The horizontal
  wheel is not sent (no field in `input`). The mouse test runs at
  1280x800 only.

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
