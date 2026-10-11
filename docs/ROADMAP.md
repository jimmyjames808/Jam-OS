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
| M8.6 | Cleanup and polish | done (PC 2026-10-02: `soak 10` passed (663 s on 28 CPUs, 4525 kernel tests and 17 utest runs, 0 FAILED) with the SanDisk mounted read-write; live `ktest` 248 passed and utest 117; `bench` in [BENCH.md](BENCH.md#m86-pc-2026-10-02)) |
| M9 | Networking | done: built, reviewed (no High) and used on the PC from 2026-10-02 (the RTL8125 sending and receiving on the network's VLAN after two driver fixes, `ping`, DHCP, DNS, netlog, signed `update` and `update -w` from the Mac); no sign-off of its own was recorded, and the network has been up in every later PC sign-off's `soak 10` ([HISTORY: M9](HISTORY.md#m9-networking)) |
| M9.5 | Network streams: TCP and sockets on shared rings | done, with M9's status: TCP, sockets on rings, wait sets, the listen permission, `fetch`, `serve`, `speed` and SNTP; reviewed (nothing High or Medium); `speed` on the PC reaches 34.7 MB/s out and 11.6 in over Wi-Fi after window scaling and SACK ([HISTORY: M9.5](HISTORY.md#m95-network-streams)) |
| M11 | IOMMU | done: signed off on the PC on 2026-10-07 (the IOMMU checks, every device, All tests and `soak 10` with it on) and on by default since; `iommu=off` turns it off ([HISTORY: M11](HISTORY.md#m11-the-iommu)); reviewed (the review's findings are in [HISTORY: M11](HISTORY.md#m11-the-iommu)) |
| M11.5 | Performance pass: the IPC fast path, a Linux column in BENCH.md | done: reviewed, and measured on the PC on 2026-10-07 ([BENCH.md](BENCH.md#m115-with-every-follow-up-pc-2026-10-07)): the switch 30.1 ns (92 at M8.6), a process-to-process call 1089 ns (1533), a call to a reply-and-wait server 765 ns against the 600 ns target, read on that line with the lock checker on (the owner, 2026-10-07); the last 165 ns are a follow-up below, and the Linux column is not measured yet ([HISTORY: M11.5](HISTORY.md#m115-the-performance-pass)) |
| M11.6 | Services that outlive their process | done: fat and the mixer ([HISTORY: M11.6](HISTORY.md#m116-services-that-outlive-their-process); QEMU's numbers in [BENCH.md](BENCH.md#m116-qemu-2026-10-05)), reviewed (no High); on the PC with M12's sign-off (2026-10-11) `storm` at 10 and 100 kills a second copied a file with a matching checksum at a median of about 60 ms from kill to first answer. Not run on the PC: `storm` at 0 and 1 kills a second and `storm mixer` by ear |
| G1 | A compositor that speaks Wayland | **done** 2026-10-07 ([HISTORY: G1](HISTORY.md#g1-the-desktop)): X1's hostile clients against the live desktop, the Mac regression, pushed; signed off on the PC (the checklist, All tests, soak 10, and a soak 2 after the last fixes). Its docs pass and review were M12.1's (the owner, 2026-10-07) |
| M12 | Interface review, before anything is frozen | **done**: signed off on the PC on 2026-10-11 (the build updated over the network, ACS isolation, `storm` at 10 and 100 kills a second (MATCH), a stick pulled and put back, network and sound, utest on the PC, All tests: no problems, `soak 10` passed: 3,176 kernel tests and 5 utest runs, 0 failed; bench unchanged from 2026-10-07). Its code check is M12.1 |
| M12.1 | Code check after the interface review | **done**: six parts read by fresh agents (the kernel core, the kernel's devices, the compositor, the desktop's clients, M12's protocol and storage lines, init and devmgr), every High and Medium fixed with a test, new hostile and fuzz tests; in the build M12's PC sign-off ran on (2026-10-11) ([HISTORY: M12.1](HISTORY.md#m121-the-code-check)). Open, in a follow-up: part 1b's design questions B, C and D (init's private channel, a 64-request cap in devmgr's and volumes' loops, the console's dead roles) and its findings 6, 8 and 10 |
| M12.5 | User-space pagers: mapped files, programs loaded on demand | under way, kernel half built: the pager object, its VMOs and the request queue with system calls 160-164 (PK1), and the fault path that waits for a pager (PK2, 12 ktests). Not yet: eviction (PK3), verified pages and `vmo_make_exec_verified` (PK4), the plumbing (PU), fat as a pager and coherence (PF1, PF2), programs from `/data` (PX), the review and the join. Nothing uses a pager yet ([M12.5-PLAN.md](M12.5-PLAN.md)) |
| M12.7 | NVMe: read-only first, then writes to a Jam OS disk, then installing Jam OS to it | under way, in wave 5 with M12.5 and M13 (the owner, 2026-10-07). Stage 1, read-only, is built and tested in QEMU (N1-N5): drv/nvme (no write command in it, checked by `make check`), the partition-table library, `volumes` mounting FAT partitions read-only as `/nvmeN`, and the shell's `disks`, `disk digest` and `disk smart`; it binds on every boot. Its check on Windows' drive on the PC (the drive's write counter unmoved) is not recorded yet. Stage 2 (writes, only to the blank second drive fitted on 2026-10-11) and stage 3 (the installer) are to come, with one review at the end ([M12.7-PLAN.md](M12.7-PLAN.md)) |
| M13 | POSIX on musl | under way ([M13-PLAN.md](M13-PLAN.md)). Merged: the kernel's part (K1: small ids, `thread_interrupt`; K2: the time page), musl 1.2.6 with the sysroot, `jamos-cc` and `posixtest` (A1), libjam and the hand-over (A2), netstack options (N1), the console's connection back end and `tty` (T1, T2), files and directories (B1), pipes, `dup`, `/dev`, `/etc`, memfd and eventfd (B2), `posix_spawn` and waits (B3), signals (B4), `poll`, `select` and `epoll` (B5), and the shell's descriptors and redirections (S1). Not yet: sockets (B6), sessions and ptys (X1), sbase and Lua (P1), libc-test (P2), tinyssh (X2), pipelines (S2), mapped files (F1), the reviews and the join |
| M13.5 | A real POSIX shell, made the default | after M13, alongside M14 (the owner, 2026-10-11) |
| M10 | ACPI power, tickless idle | after M13 (moved there by the owner, 2026-10-02) |
| M10.5 | S3 sleep | after M13 (moved there by the owner, 2026-10-02) |
| M14 | Stable syscall ABI | stretch |
| M14.5 | Maybe: dynamic linking | stretch, only if porting needs it |
| M15 | Self-hosting | stretch |
| G2-G4 | Toolkit and fonts, mode setting, 3D | after G1 |
| Maybe | Own UEFI loader in place of Limine | not planned |

## Now

Three milestones run side by side (wave 5, the owner's order of
2026-10-07):

- **M13, POSIX on musl**: the sockets (B6), sessions and pseudo-terminals
  (X1), the ported utilities and Lua (P1), then libc-test, tinyssh (X2),
  the shell's pipelines (S2) and mapped files (F1); then the reviews and
  the join. Plan: [M13-PLAN.md](M13-PLAN.md).
- **M12.5, user-space pagers**: eviction (PK3) and verified pages (PK4),
  the plumbing (PU), then fat as the first pager (PF1) and programs run
  from `/data` through it (PX). Plan: [M12.5-PLAN.md](M12.5-PLAN.md).
- **M12.7, NVMe**: stage 1 (read-only) waits for its check on the PC;
  stage 2, writes to the blank second drive only (W1-W3), is next. Plan:
  [M12.7-PLAN.md](M12.7-PLAN.md).

Beside them: M12.1's follow-up (see its row above) and the small items
under [Smaller follow-ups](#smaller-follow-ups).

M8.6 (cleanup and polish) is done: [what it delivered](HISTORY.md#m86-cleanup-and-polish).

The audio track (A1, A2, AS) and M8.5 are done:
[what they delivered](HISTORY.md#audio-a1-a2-as-and-m85-kexec).

M8 (storage) is done: [what it delivered](HISTORY.md#m8-storage).
Known limits it left:
- A panic's own text is not in that boot's log file: logd can only save
  what it had synced. Since M8.5 the next boot saves the whole log next
  to it as `boot-NNNN-crash.txt`.
- GPT sticks are not read.

## Later

M11 to M12.1 ran in three waves (what they delivered is in [HISTORY.md](HISTORY.md)); M12.5, M12.7 and M13 run together as wave 5.

| # | What | Done when |
|---|---|---|
| A3 | Maybe: USB audio devices (headsets, USB sound cards). HDMI/DisplayPort audio through the RTX is not planned | (not planned in detail) |
| M9 | Networking on the board's RTL8125, **VLAN 21 only** ([the rule](../ARCHITECTURE.md#networking)): every new loop follows M8.6's service-loop rule; the NIC's devmgr channel goes to netstack alone; netdev is shared rings with events, never a call per packet; packets are parsed only in processes that hold no `dma_cap`; init makes each new service's channels once. The RTL8125 driver, lwIP, DHCP and DNS as processes, netlog (the kernel log over UDP to the Mac), `update` (a new kernel and boot image from the Mac, by kexec). Status: done (the table above) | `ping 1.1.1.1` on the PC through a userspace network stack; a PC run's full log arrives on the Mac; `make` on the Mac + `update` on the PC runs the new build with no stick moved; a service waiting on a slow peer delays only that peer's requests (a test) |
| M9.5 | Network streams, built once properly on M9's stack (the owner, 2026-10-02): **socket data on shared rings** (each socket a pair of shared-memory rings to netstack, as netdev and the mixer; setup stays IDL; UDP moves too; measured before and after in BENCH.md); **TCP** in lwIP with stream sockets (`connect`, `listen`, `accept`, `send`, `recv`, `shutdown`, `close`), backpressure by ring room (netstack never waits on a program), per-program connection and buffer limits; **waiting on many sockets at once** (on the kernel's ports: the groundwork for M13's `poll`/`select`); **accepting connections is a permission of its own** (`svc net listen` in a program's list, granted through `allow`; nothing listens by default). Programs: `fetch` (plain HTTP downloads), a throughput tester against a script on the Mac, SNTP (the clock from the network), and **`serve <file> [port]`**: one file shared read-only over HTTP from the background (as the music player runs), ports 1024 and up, `serve` lists, `serve stop` ends it. Every frame still VLAN 21. Not in it: TLS, IPv6, remote login (needs TLS or SSH first) | on the PC: `fetch` of a large file from the Mac and the Mac fetching a file the PC `serve`s, speeds in BENCH.md; the throughput test both ways; a program stuck on a slow connection delays no other; SNTP sets the clock; QEMU tests with hostile and slow clients and hundreds of connections; All tests + `soak 10` |
| M11 | IOMMU (VT-d) and interrupt remapping behind `dma_cap` (the PC's firmware has a DMAR table). Before G3 at the latest | DMA outside a driver's pinned VMOs is blocked, and a device's write to the interrupt window sends no interrupt; ARCHITECTURE then counts drivers as contained, not only crash-isolated |
| M11.5 | Performance pass, as M5.5 was, before M12 reviews and M14 freezes the system calls: the IPC fast path (one reply-and-wait call, a direct hand-off to a waiting server, one copy of the message, no FPU save on a voluntary switch, no failed read before each wait), the file and block calls' deadlines made cheaper, and every number measured again on the PC. BENCH.md gains a Linux column measured on the same PC (default and `mitigations=off`), per-operation lines (a cached 4 KiB read, a block read) next to per-call ones, and a column with the lock checker off | a process-to-process call on the PC at 600 ns or less (1407 ns at M5.5, 885 ns against a reply-and-wait server on 2026-10-04), nothing given up in isolation or restart |
| M11.6 | Services that outlive their process. A service's queues and state live outside the process (in VMOs and kernel objects the service is handed back when it restarts), so a restarted service reconnects to them and its clients never see the crash; a warm spare makes the restart fast. First for fat and the mixer; the drivers (usb-storage, then hda) are a milestone of their own after M12, from the design note in [HISTORY: M11.6](HISTORY.md#m116-services-that-outlive-their-process). Built on M11.5's call path, before M12 reviews the interfaces it changes. The demonstration: a file copy on the PC that finishes with the right checksum while fat is killed again and again, its throughput printed next to the kill rate | killing fat or the mixer during use is not seen by their clients (no error, no gap in the sound); kill-to-first-answer measured and in BENCH.md |
| M12 | Interface review: the system calls and the service protocols reviewed and reshaped while changing them is cheap, before POSIX builds on them and M14 freezes them, G1's compositor protocol among them. The second cleanup point after M8.6, by fresh agents. devmgr's disks, filesystem services and mounts move into a service of their own, and devmgr's protocol into IDL; variable-length IDL arrays | the review's findings fixed; nothing frozen yet |
| M12.1 | Code check, targeted (the owner, 2026-10-07: rescoped from a sweep of everything changed since M8.6, about 166,000 lines, to where reading finds what tests don't). Fresh agents, findings first, then fixes with a test each: **(1) every line M12 changes**, with the unchanged callers of each interface it changes (M12's own independent review, Q20); **(2) G1's code**, never reviewed whole (the compositor, libjwl, libfun's Wayland back end, the console's window mode, init's terminals and desktop wiring: about 32,000 lines), with **G1's docs pass**; **(3) the kernel's changes since M8.6** (about 20,000 lines: a kernel bug is a crash or a hole). Unchanged user space that the tests and soaks already exercise is left to them. Part of the time saved goes into more hostile and fuzz tests in X1's style (they found more than reading did in G1's last day). About half the old size: 4-5 agents, about a day | the check's findings fixed; the new hostile/fuzz tests passing; All tests and `soak 10` on the PC |
| M12.5 | User-space pagers. A process (a filesystem service) supplies the pages of a VMO on demand: mapped files (a cached read becomes a memory copy, and M13's `mmap` has something to stand on), programs loaded on demand instead of read whole, and the clean way to run programs from `/data` (the pages come from fat, checked against the approved hash). Pager failures (a pulled stick) end in a clean error, never a kernel hang | a file mapped and read through its pages; a program on `/data` runs from a pager; pulling the stick under a mapping fails the access cleanly |
| M12.7 | NVMe and installing (the owner, 2026-10-07). A user-space NVMe driver behind the IOMMU (admin and I/O queues, MSI-X), on the block interface M12 reviewed: the second real block device after USB storage. In stages, because the PC's NVMe drive holds Windows: (1) READ-ONLY: drives found, their GPT partition tables read and listed, FAT partitions mounted read-only, no write path built at all; (2) writes only to a disk or partition marked for Jam OS (a second NVMe drive just for Jam OS is the safest: the owner to say whether the PC has a spare M.2 slot), every write refused elsewhere; (3) an installer: Jam OS on that disk with its own ESP entry or boot menu, booting without the stick, updated by `update` with the same power-cut-safe steps as the stick. QEMU emulates NVMe for the tests | (1) on the PC the drives and partitions listed and a FAT partition read, Windows' disk untouched (its hash before and after); (2) a Jam OS disk written, read back, power cut mid-write survives; (3) the PC boots Jam OS from the internal disk and an `update` lands there |
| M13 | POSIX on musl: file descriptors over handles, `posix_spawn` (no `fork`), paths through the namespace, `poll`/`select` on M9.5's wait sets, a terminal layer (pseudo-terminals: a shell that talks to a connection instead of the screen), then ported programs. **The first test port is tinyssh** (the owner, 2026-10-02): an SSH server, key login only (no passwords), a host key made on first boot, `authorized_keys` in `/data/etc`, the listen permission, one process per connection (its `fork` replaced by `posix_spawn`) | unmodified POSIX programs (shell utilities, a small C program) build and run; `ssh` from the Mac opens a Jam OS shell on the PC |
| M13.5 | A real POSIX shell, made the default (the owner, 2026-10-11). `vfork` emulated in libc (M13's question 4, option B: the child's descriptor changes recorded until `execve` or `_exit`, then `posix_spawn`), and `fork` without `exec` emulated in libc on the kernel's copy-on-write child VMO (built by M13's track F1 for `MAP_PRIVATE`, before M14 freezes the system calls; the owner, 2026-10-11): a new process of the same static program, each writable VMO of the parent given to it as a copy-on-write child at the same address, the calling thread's registers and the descriptors libc chooses to pass, so a fork costs page tables, not a copy of the memory, and the child holds only what libc hands it. Pages a write separates are charged to the writer's job when the child is made (no overcommit, no surprise kill). Shared offsets for inherited files (M13's question 5, option B) come with it, or `{ a; b; } > f` overwrites instead of appending. Not covered, by design: handles without the duplicate right are not passed to the child (it gets only what may be shared). Then a permissively licensed interactive shell ported unpatched (mksh: line editing, history, Tab, job control within M13's limits). The Jam OS commands (`disks`, `usb`, `dmesg`, ...) as programs in `/bin` first, so no command is lost. Once it matches the Jam shell it is the default in terminals and over SSH (`ssh pc command` and later scp through `sh -c`); the Jam shell stays in the boot image as the recovery shell | the default shell in a Jam OS terminal and over SSH is the ported shell; `$(...)`, subshells, pipelines of builtins and `configure`-style scripts run; every Jam OS command still works from it |
| M10 | Moved after M13 (the owner, 2026-10-02). uACPI: poweroff, power button, ACPI reboot (uACPI stays in the kernel); tickless idle (since M11.5 a sleeper due after its CPU's next tick arms no timer and waits for the tick to re-arm for it, `after_next_tick` in `kernel/sched/wait.c`: a CPU that stops its tick must arm its timer for its sleeper queue's head first) | clean shutdown on real hardware |
| M10.5 | Moved after M13 with M10. S3 sleep (suspend to RAM) on top of M10's ACPI: every driver saves and restores its device | the PC suspends and resumes with USB, audio and the network working again |
| M14 | Stable syscall ABI: frozen only after POSIX has put its weight on it; versioned and documented. It starts with a short review of every interface added since M12's review (M10's power calls, M10.5's driver suspend and resume, and whatever M12.5 and M13 add), since M10 and M10.5 now come after M13; G1's protocol is M12's, as G1 comes before it | old binaries keep running on new kernels |
| M14.5 | Maybe: dynamic linking. A loader for shared libraries and `dlopen`, for porting big software built around them (LibreOffice, Python's C extensions, GTK or Qt apps, plugins). Static linking stays the default for Jam OS's own programs. `allow` must then cover the libraries a program loads as well as the program file (each approved by its hash), and a program's list names the libraries it may load. Only worth doing when such a port is a goal | a ported program loads a shared library and a plugin with `dlopen`, and a changed library is refused until approved |
| M15 | Self-hosting: the build tools rewritten in C, then make, binutils and GCC ported onto M13's POSIX layer; a small compiler (TCC/cproc) may come first | Jam OS rebuilds itself on the PC and boots the result, with no Mac involved |
| G1 | A compositor of our own on the firmware framebuffer that speaks the Wayland protocol: Wayland's model (surfaces, buffers, `xdg_toplevel`, a seat for input) with its wire format carried over channels, handles where Linux passes file descriptors, `wl_shm` pools as VMOs, the code generated from Wayland's XML protocol files as the IDL is. Apps draw into their own surfaces, input goes only to the focused client, a crashed app takes only its own window down, software rendering. The console becomes a client (a terminal window). Not a port: Weston and wlroots need Linux's DRM/KMS, Mesa, libinput and udev | windows from several programs on the PC's screen, the shell in one of them |
| G2 | Toolkit, TrueType fonts, GUI apps: ported leaf libraries (stb_truetype or FreeType, microui/Nuklear or LVGL, stb_image). After M13, libwayland ported with a shim from sockets and file descriptors to channels and handles, so Wayland programs that draw in software (foot, SDL) run unchanged; GTK and Qt are a later porting project of their own | |
| G3 | Mode setting and vsync, only through the Intel iGPU (needs the monitor on the board's output and the iGPU enabled) | |
| G4 | 3D as a stretch: a multi-core software rasterizer, or virtio-gpu under QEMU | |
| Maybe | Own UEFI loader in place of Limine: a third filler of `struct boot_info` (after Limine's and M8.5's kexec). Limine does the job, and after M8.5 the kernel needs it only to load three files and jump; our own would cost its boot menu (the test entries) and new firmware quirks for little gain. Revisit if self-hosting (M15) or Secure Boot ever needs it | (not planned in detail) |

## Design ideas, not scheduled

Larger pieces that fit the design and would be worth a milestone each.
None has a plan yet; the order is the current preference.

- **Shared request rings.** A client and a service share a ring of
  requests and replies in a VMO and make a system call only when the other
  side is asleep: the third level after copied messages and shared VMOs
  for bulk data. The ring layout would be generated from the IDL. netdev's
  rings are written by hand this way; the first generated users would be
  `/svc/net`'s sockets (one call per datagram today) and the fs and file
  calls (M11.5).
- **A service dependency graph.** devmgr stops in device-tree order;
  init starts and stops by a list. init already makes the mixer's
  and the music player's channels once, so their clients don't
  depend on start order; done for every service (M9's first), what
  is left is the stop order, a rank in init's table. A declared graph
  is for when that stops being enough.
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
([HISTORY: M8](HISTORY.md#m8-storage) has the details):

- **The call's last 165 ns**: M11.5's 600 ns target, read on the
  reply-and-wait line with the checker on (the owner, 2026-10-07), is at
  765 ns on the PC ([BENCH.md](BENCH.md)); a later speed pass (the M11.5
  review's ideas: re-arming a ONCE binding inside `channel_reply_wait`,
  fewer copies and handle lookups).
- **Paste outside the terminal** (the owner noticed): the compositor's
  search box and libfun's text fields (Jamjar's search) take Ctrl+V /
  Super+V through the clipboard.
- **Jamjar at squarish sizes**: its layout overlaps below 944x568, so it
  now declares that as its minimum window (2026-10-07: tiling opens it on
  a screen of its own rather than squeezing it); still to do, a narrow
  layout with the controls under the cover, and then a smaller minimum.
- **The shell's quiet ktest loops beside the desktop**: a soak's idle
  loops now run next to the compositor painting their output (G1). Once
  in two Mac soaks (2026-10-07, 4-CPU QEMU), `repro_wake_stale_cpu` timed
  out on a 2 s step (`KT_NEEDS_IDLE`: the machine counted as idle). If it
  comes back, the shell's ktest should count a busy desktop as load.
- **The compositor's 64 connections** (`COMP_CLIENTS_MAX`, comp.h): a
  cap is needed (a program can't open connections until the compositor
  runs out), but 64 itself is only a round number well above use (16
  terminals + Jamjar is about 20). Nothing depends on it being 64; the
  per-connection limits are what bound memory. If it is ever hit (the
  compositor counts refusals: `refused` in its stats), raise it to 256
  (fixed tables of a few KB, Alt+Tab's list ~128 KB) and give the refusal
  a notice like "No more terminals" (the owner, 2026-10-07).
- **The shell's history across restarts** (the owner, 2026-10-08): done
  in M12's track P1b (`user/services/shell/sh_history.c`,
  `tools/shell-tests/history.txt`): `/data/etc/history`, the last 500
  lines, shared by the terminals; Up and Down match the typed text; a
  leading space keeps a line out; `history -c`; and the line a shell
  started again after a crash says. Left: two shells appending at the same
  moment can lose one line.
- **GPT sticks** are not read (their partitions are not mounted); M12
  shapes the storage protocol for GPT and M12.7 parses it.
- The mouse wheel works on the PC (2026-10-01: hid drives a mouse whose
  report descriptor has a wheel in the report protocol; real mice send no
  wheel in the boot protocol). The boot log has each mouse's descriptor
  in hex, to add to `drivers/hid/fixtures.c`. The horizontal wheel is not
  sent (no field in `input`). The mouse test runs at 1280x800 only.
- The audio reviews' findings that were not fixed: an hda driver that
  dies mid-stream leaves the codec path open until its restart resets
  the link, and the Lows
  ([HISTORY: audio](HISTORY.md#audio-a1-a2-as-and-m85-kexec)); jamjar shares the shell's
  end of the player's channel, and its own Lows (same section).

The rest:

- The benchmark ([BENCH.md](BENCH.md), last run on the PC 2026-10-07):
  once per milestone, and once per milestone a QEMU soak with the
  scheduler's switches off (`noplaceorder noaffinepair nospinidle`). A new
  scheduler heuristic needs a switch and a BENCH line showing a clear win
  on the PC.
- The address-space switch got ~20 ns dearer with PCIDs on
  ([BENCH.md](BENCH.md)); look at `pcid_load`'s bookkeeping.
- Kernel waits bounded by an iteration count or not at all: the xAPIC ICR
  wait, `serial_rx_start`'s drain, interrupt teardown, `on_cpu`, the
  pmm/heap flag locks and the lockdep graph lock (from the kernel style
  pass).
- Files still over 800 lines: `vmo.c`, `sched.c`, `aspace.c` (splitting
  them needs internal headers).
- Most of the shell has had no independent review (the cleanup's covered
  the segment fix and `kill`, M8's the file commands); M12.1 reads only
  what M12 changes in it, so the rest waits for a later check.
- `job_find_process` is used only by its own test.
- A pluggable scheduler interface (`sched_ops`): not until a second
  policy is needed.

## Not planned

- `fork` in the kernel (a system call that clones a process): the POSIX
  layer gets `posix_spawn`, and M13.5 emulates `fork` in libc on
  copy-on-write child VMOs.
- Wi-Fi.
- HDMI/DisplayPort audio through the RTX.
- Filesystems other than FAT32 (NVMe itself is M12.7, on FAT32 and GPT).
- User accounts: single user; handles are the only authority.
- Running Linux binaries unchanged (a Linux system-call personality, as
  WSL1 or FreeBSD's Linux layer): the owner, 2026-10-09. Programs are
  ported from source onto the POSIX layer instead.
- Own versions of general apps (a YouTube app, a browser, editors,
  players): the owner, 2026-10-11. Jam OS writes its own programs only
  where they are part of the system (settings, the shell and its
  commands, the compositor, system tools); every other app is ported. A
  browser, when it comes, is a port of Chromium starting from its Fuchsia
  code (spawn and channels, no zygote, no `fork`), not its Linux code.
