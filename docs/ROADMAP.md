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
| A1 | Audio: HD Audio driver, `beep` | done (PC 2026-10-01: All tests no problems, `soak 10` passed (645 s on 28 CPUs, 4495 kernel tests and 19 utest runs, 0 FAILED); beep, jack detection) |
| A2 | Audio: mixer, `audio` protocol, WAV and MP3 playback, `music`, jamjar | done (the same sign-off) |
| AS | Boot splash: the logo animation with its sound, alpha blending | done (the same sign-off) |
| M8.5 | Kexec for reboot and panic | done (PC 2026-10-01: All tests no problems, `soak 10` passed (645 s on 28 CPUs, 4495 kernel tests and 19 utest runs, 0 FAILED); a panic saves its log and restarts, `reboot` kexecs with all 28 CPUs; its independent review opens M8.6) |
| M8.6 | Cleanup and polish ([M8.6-PLAN.md](M8.6-PLAN.md)) | next |
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

## Next: M8.6, cleanup and polish

The row below has the whole list. First the independent review of
M8.5's code; then three tracks at once: A (keyboard and mouse ready early
in boot, the block cache, the boot disk and the other small items), B
(`/svc` with per-program views, `allow` for programs on `/data`, `play`
and jamjar's cover decoding split into small programs, the root
resource's `RIGHT_READ` split), C (the log off the screen, UTF-8 in the
log, `kernel load`, the date and time, settings); the code check last.

The audio track (A1, A2, AS) and M8.5 are done:
[what they delivered](HISTORY.md#audio-a1-a2-as-and-m85-kexec).

M8 (storage) is done: [what it delivered](HISTORY.md#m8-storage).
Known limits it left:
- Only programs in `/boot` can be run: a file on `/data` or `/esp` does
  not come with the right to execute it (M8.6 adds `allow`).
- A panic's own text is not in that boot's log file: logd can only save
  what it had synced. Since M8.5 the next boot saves the whole log next
  to it as `boot-NNNN-crash.txt`.
- GPT sticks are not read.

## Later

| # | What | Done when |
|---|---|---|
| A3 | Maybe: USB audio devices (headsets, USB sound cards). HDMI/DisplayPort audio through the RTX is not planned | (not planned in detail) |
| M8.6 | Cleanup and polish, in the order of [M8.6-PLAN.md](M8.6-PLAN.md): the independent review of M8.5's code; track A (keyboard and mouse ready early in boot, keys during the splash queued, a write-through block cache, the boot disk from Limine, Ctrl+Alt+Del's wait, a safe `make flash`, a bigger log ring); track B (`/svc` with per-program views, a program's list of what it wants approved by the owner with `allow`, which also marks programs on `/data` runnable, `play` and jamjar's cover decoding in programs of their own, the root resource's `RIGHT_READ` split); track C (the log and the RESULTS boxes off the screen with a short list of useful notices, UTF-8 in the log, `kernel load`, the real date and time from the RTC, settings in `/data/etc/settings`); last, the code check against CODING-GUIDE.md and ARCHITECTURE.md by fresh agents, findings first | keyboard and mouse ready within about 2 s of the kernel's start (the boot log says); the code check's findings fixed or written into the guide; All tests and `soak 10` on the PC with the SanDisk pulled and replugged |
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

Offered or noticed, not scheduled into a milestone yet. M8.6 took
several that were here (its plan has them): running programs from
`/data`, which disk is the boot disk, a narrower namespace per program,
the root resource's `RIGHT_READ` split, a safe `make flash`, a bigger
kernel log ring and the block cache for the slow `/esp` read. The first
ones below are the design questions M8 left open
([its review](history/M8-REVIEW.md) has the details):

- **devmgr's job check at a driver's exit** can report a driver that ended
  cleanly as "did not end cleanly" (seen on the PC for a hid after a warm
  reboot: a request it left queued at usb-bus is still charged to its
  job). A second look a moment later, before counting it as a problem,
  would cover it.
- **logd loses lines during the klog flood test**: the kernel test
  `console_klog_read_after_gap` writes more than the 64 KiB kernel log
  ring holds, on purpose, and no reader can follow it; each live `ktest`
  leaves a "[logd: N bytes of the log were lost]" line. Skip that test
  live, or accept it (M8.6's bigger ring covers the log written while
  the boot stick is out, not this test).
- **GPT sticks** are not read (their partitions are not mounted).
- One bulk transfer at a time inside usb-bus's loop, and devmgr's
  bounded waits (up to 2 s) on a slow usb-storage: both block other work
  meanwhile. Asynchronous transfers would be a redesign of the serve loop.
- The first `make -j8` after a new file in `abi/idl/` can spin forever;
  run again, it builds. Not looked into.
- Names and volume labels from someone else's stick are printed as they
  are, escape sequences included; `ls` and `find` show a directory's
  first 256 entries and say nothing about the rest; init's loop waits up
  to 25 s for a `mount` and 15 s for a `kill`.
- The mouse wheel works on the PC (2026-10-01: hid drives a mouse whose
  report descriptor has a wheel in the report protocol; real mice send no
  wheel in the boot protocol). The boot log has each mouse's descriptor
  in hex, to add to `drivers/hid/fixtures.c`. The horizontal wheel is not
  sent (no field in `input`). The mouse test runs at 1280x800 only.
- The audio reviews' findings that were not fixed: an hda driver that
  dies mid-stream leaves the codec path open until its restart resets
  the link, init waits for the splash with no deadline, and the Lows
  ([AUDIO-REVIEW.md](history/AUDIO-REVIEW.md)); jamjar shares the shell's
  end of the player's channel, and its own Lows
  ([JAMJAR-REVIEW.md](history/JAMJAR-REVIEW.md)).

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
