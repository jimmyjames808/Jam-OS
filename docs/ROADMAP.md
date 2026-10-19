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
| M12.5 | User-space pagers: mapped files, programs loaded on demand | built: the kernel's pager objects and fault path, the page cache with eviction, pages checked against the hash the owner approved (PK1 to PK4), fat as a pager (PU, PF1, PF2) and programs run from `/data` page by page (PX); extfs's pager (M13.6's U10) and writable file mappings ([WRITEMAP-PLAN.md](WRITEMAP-PLAN.md)) stand on it. Its own review track and the join are still open ([M12.5-PLAN.md](M12.5-PLAN.md)) |
| M12.7 | NVMe: read-only first, then writes to a Jam OS disk, then installing Jam OS to it | built and reviewed ([M12.7-REVIEW.md](M12.7-REVIEW.md): stages 2 and 3), and checked on the PC: stage 1 on 2026-10-11 (Windows' drive's own write counter unmoved across Jam OS boots); stages 2 and 3 on 2026-10-18 (the second drive made a Jam OS disk by `disk prepare`, every write to Windows' drive refused, `install nvme1`, and the PC boots Jam OS from that drive with the stick out while Windows still starts from the firmware's boot menu). The power cut in the middle of writes was not tried on the real drive (QEMU's cut tests cover it). The drives are named in PCI order. The join is open ([M12.7-PLAN.md](M12.7-PLAN.md)) |
| M13 | POSIX on musl | built and reviewed ([M13-PLAN.md](M13-PLAN.md); reviews of [the kernel's part](M13-REVIEW-KERNEL.md), [the POSIX layer](M13-REVIEW-POSIX.md) and [the terminals, sessions, SSH and shell](M13-REVIEW-TERMINAL.md)): musl 1.2.6 over handles with files and directories, pipes, `/dev` and `/etc`, `poll` and `epoll`, signals, threads, `posix_spawn`, TCP, UDP and `AF_UNIX` sockets, terminals, ptys and sessions, mapped files, and the shell's descriptors, redirections and pipelines. Ported: sbase, Lua, libc-test and tinyssh (`ssh` into Jam OS works on the PC). Chromium's layers were added to it afterwards (below). The join is open |
| M13.5 | A real POSIX shell, made the default | its first half is built and reviewed: libc's `fork` and `vfork` and an `exec` in place, so Chromium's launcher runs unpatched ([FORK-PLAN.md](FORK-PLAN.md): [kernel review](FORK-REVIEW-KERNEL.md), [libc review](FORK-REVIEW-LIBC.md)). The shell itself (mksh) comes after Chromium |
| M13.6 | A Unix filesystem: symlinks, permissions, large files, case-sensitive names | built and reviewed ([M13.6-PLAN.md](M13.6-PLAN.md), [M13.6-REVIEW.md](M13.6-REVIEW.md)): extfs, our own code in ext4's on-disk format with its journal, the Jam OS drive's `/jam`, and the stick's `/data` by default since 2026-10-15; running on the PC from the installed drive since 2026-10-18. File watching ([INOTIFY-PLAN.md](INOTIFY-PLAN.md), reviewed), record locks per inode, directory handles ([DIRHANDLE-PLAN.md](DIRHANDLE-PLAN.md): built, its review under way) and writable file mappings ([WRITEMAP-PLAN.md](WRITEMAP-PLAN.md), reviewed) were added for Chromium. The join is open |
| M13.7 | The ports system: recipes, dependency order, packages | built except its last rungs ([M13.7-PLAN.md](M13.7-PLAN.md)): the recipe format and builder, signed packages and the Mac's repository, `pkg` on Jam OS (approval by package, the package area on `/jam`, power cuts settled at boot), and the ports zlib, bzip2, xz, libarchive, the Wayland pieces, two font packages and the Chromium recipe. Not built, because nothing needs them yet: make, ncurses and vim (PO10, PO11) and the builder's CVE audit (PO12). Its review and the join are open |
| M10 | ACPI power, tickless idle | not started: after Chromium (the owner, 2026-10-20) |
| M10.5 | S3 sleep | not started: after Chromium |
| M14 | Stable syscall ABI | not started: after Chromium. The ABI stamp still moves with libc and IDL changes, and old static programs relink |
| M14.5 | Maybe: dynamic linking | parked: off Chromium's path (it is built static), only if a port needs `dlopen` |
| M15 | Self-hosting | not started: after Chromium |
| CR | Chromium: the main goal (the owner, 2026-10-11) | under way ([CR-PLAN.md](CR-PLAN.md)). Built: CR-a (the clang and Rust toolchain), CR-b (`base_unittests`: 8,901 of 8,956 pass, 37 skip themselves, 18 are listed with their reasons), CR-c (`headless_shell` pictures of local and HTTPS pages; `content_shell` on the desktop; multi-process with Chromium's own launcher, a killed renderer replaced on reload) and CR-e (V8's compilers and WebAssembly in the renderers; WebGL in software, opt-in), with sound through the mixer and full fonts. CR-d, `chrome` as a package (track CB6), is under way. CR-f, Jam OS's sandbox, is not built; its plan is being written. All of it runs in QEMU; on the PC itself only `headless_shell` has drawn real sites (2026-10-18), and `content_shell`'s multi-process, JIT, WebGL, sound and fonts builds have not been tried there yet |
| G2 | Wayland clients, scoped to Chromium | built and reviewed ([G2-PLAN.md](G2-PLAN.md), [G2-REVIEW.md](G2-REVIEW.md)): the libwayland shim in libc, versions and the owner's decoration rule, subsurfaces, popups with grabs, the base ports, the fonts packages (fonts-jamos, fonts-noto-extra), wlprobe. Still to come: the search box starting a package's program (GD) and the join. foot, SDL2, Doom, swayimg, timerfd, pointer lock and the GTK spike come after Chromium |
| GPU | The iGPU's render offload, and making the browser run well | after the sandbox: the monitor stays on the RTX, the Intel UHD 770 (off in the firmware today) renders; GPU-PLAN.md is being written. "Run well" has its plan, [RUNWELL-PLAN.md](RUNWELL-PLAN.md) (13 tracks: numbers read against Linux on the same PC, a 50-site soak, profiling tools, the init run and tests out of the boot image), parked with the iGPU; its first wave needs no GPU. G3 (mode setting) and G4 (3D) are deferred |
| Maybe | Own UEFI loader in place of Limine | not planned |

## Now

**The goal: Chromium (the owner, 2026-10-11).** Jam OS's main goal is
to run Chromium. It is reached layer by layer, each layer a general piece
of the system that any later port uses too (no Chromium-only shortcuts,
nothing that can't generalise), and only the layers Chromium needs to run
are built now. Everything else waits until after it, unless the owner
sets it as a goal of its own. **The plan is [CR-PLAN.md](CR-PLAN.md)**
(CR0): every layer's design, its tracks and gates, the build, and the
milestones CR-a to CR-f.

**Where it stands (2026-10-20).** `content_shell`, Chromium's plain test
browser, runs on the desktop in QEMU as six processes (the browser, the GPU
process, the network and storage services, the renderers) started by
Chromium's own launcher, with V8's compilers and WebAssembly in the
renderers (JetStream's JavaScript score 7.2 times, Speedometer's 1.4
times what `--jitless` gives: [BENCH.md](BENCH.md#chromiums-jit-cr-e-qemu-on-the-pc-2026-10-19)),
sound through the mixer, and system fonts for the major scripts. WebGL
works in software through SwiftShader, only when asked for
(`--enable-unsafe-swiftshader`). `headless_shell` drew Google, Wikipedia,
GitHub, the BBC, Hacker News and YouTube over HTTPS on the PC itself
(2026-10-18). No sandbox of Chromium's own is in use yet
(`--no-sandbox`), so the owner's rule holds: ordinary sites only, nothing
that matters. Chromium is not a package yet; the `chrome` target and
`pkg install chromium` are CB6's.

**The order (the owner, 2026-10-20: the sandbox comes before the iGPU
work):**

1. **CB6, `chrome` (CR-d).** The `chrome` target with its tab strip, the
   package (programs, paks, licences, the system title bar), the profile
   on `/jam`, the one-instance rule on named `AF_UNIX` sockets, a page for
   a renderer that dies, `pkg install chromium`, and the PC check
   ([CR-PLAN: the PC check](CR-PLAN.md#the-pc-check)). Under way.
2. **CR-f, Jam OS's sandbox.** Each Chromium process type gets its own job
   and a namespace of only what it needs, set when it is started; the
   browser gives the JIT power up at start and hands it to renderers
   alone; a kernel flag keeps the no-writable-and-executable rule across
   `/svc/exec`. After it, SwiftShader may become the default and a bad
   shader is contained. It starts with a planning track: SANDBOX-PLAN.md is
   being written.
3. **The iGPU and "run well", together.** Rendering offloaded to the Intel
   iGPU with the monitor staying on the RTX (GPU-PLAN.md is being
   written), and the measured work of making the browser fast and steady:
   [RUNWELL-PLAN.md](RUNWELL-PLAN.md), parked with the iGPU (the owner,
   2026-10-18) to start after CB6 and the sandbox. Its targets are ratios
   to Linux on the same PC, with a 50-site soak and profiling tools. Its
   first wave needs no GPU and can run before the iGPU work: RW2
   (posixbench and ssh latency), RW7 (the pager's store deadline), RW9
   (a sampling profiler) and RW11 (the init run's time and the tests out
   of the boot image). The GPU process's sandbox rules get a pass when the
   iGPU lands.
4. **After Chromium**, unless the owner moves one: M10 and M10.5 (ACPI
   power, tickless idle, sleep), M13.5's mksh, M14 (the stable ABI), M14.5
   (dynamic linking, parked: only if a port needs it), M15 (self-hosting),
   G2's remaining apps (foot, SDL2, Doom, swayimg, the GTK spike) and
   ports nobody needs yet.

Pending the owner at the PC: update the NVMe drive to the newest build
([the 64 MiB limit](NETWORK.md#what-update-does-to-the-stick-or-the-drive)),
check that its name stays `nvme1` whichever disk boots, and try the
multi-process, JIT, WebGL, sound and fonts builds of `content_shell` on
the real machine.

**What Chromium stands on, as built.** Each layer is general; its tests do
not need Chromium. The route is the owner's (2026-10-11,
[CR-RESEARCH](CR-RESEARCH.md)): Chromium's **Linux** code built as
`target_os=linux` against a musl sysroot, the way Alpine ships it (about
10 small musl patches), over M13's POSIX layer and Jam OS's Wayland
compositor, not its Fuchsia port. Its Ozone/Wayland backend, Views UI,
fontconfig, FreeType, HarfBuzz, ICU, BoringSSL (with its compiled-in root
store: no CA store needed) and libc++ come with it, statically linked.

- the toolchain: clang, lld, libc++, compiler-rt and Rust's std for musl,
  a sysroot with Linux's UAPI headers, built on the PC's WSL (built:
  CT1, CT2);
- the address space: sparse reservations from GiBs up to a TiB and more
  (`PROT_NONE`, `MAP_NORESERVE`), `MAP_FIXED` inside them, decommit (V8's
  cage, PartitionAlloc's pools), reviewed (built: CK1 to CK3, CL1,
  [CR-REVIEW-KERNEL.md](CR-REVIEW-KERNEL.md));
- libc additions: `/proc/self` and the files `base` reads, memfd seals,
  raw call entries, `backtrace` (built: CL2, PRC, CL3, BT);
- writable shared file mappings on ext (`MAP_SHARED` with `PROT_WRITE`,
  `msync` the durability promise; ext only, FAT stays read-only: the
  owner, 2026-10-16), with write-back errors reported once per mapper
  (built and reviewed: [WRITEMAP-PLAN.md](WRITEMAP-PLAN.md));
- file watching (inotify: base's `FilePathWatcher`, 165 tests pass), built
  and reviewed ([INOTIFY-PLAN.md](INOTIFY-PLAN.md): the file services
  make the events; writable FAT refuses watches), and record locks per
  inode, so SQLite and leveldb in several processes meet each other;
- directory handles: a directory descriptor is the file service's own
  channel, the current directory is one, and both cross `fork`, `exec` and
  sockets ([DIRHANDLE-PLAN.md](DIRHANDLE-PLAN.md): built; its review under
  way);
- `AF_UNIX` sockets with descriptor passing (`SCM_RIGHTS`: Mojo, Wayland)
  and named sockets (built: CU1, CU2);
- launching other programs: Chromium's own launcher, unpatched, on libc's
  `fork` and an `exec` in place ([FORK-PLAN](FORK-PLAN.md), built and
  reviewed; the owner, 2026-10-16, over CR0's `posix_spawn` branch), long
  command lines, a program starting itself from its own image (built:
  CS1, CL2);
- G2 for Chromium: the libc shim, subsurfaces, popups, the decoration
  rule, fonts (built; the search box starting a program is GD, to come);
- programs run from `/data` with shared, demand-paged text checked page by
  page (M12.5's PX), programs up to 1 GiB; netstack's socket caps (768
  TCP connections a program); sound through the mixer (CA1: libjam's
  `<audio.h>` and Chromium's back end); system fonts (fonts-jamos and
  fonts-noto-extra: CJK, Arabic, Hebrew, the Indic scripts and more);
- the JIT, by capability (the owner, 2026-10-11): Jam OS keeps "never
  writable and executable" for every program except one that asks for
  `right jit` in its list and is approved (`allow`, a package's approval).
  Such a program may make JIT memory: a VMO of its own with write and
  execute rights, private to it, never file-backed and never counted as
  approved code, marked `[jit]` in `ps`. Built and reviewed twice
  ([CR-REVIEW-JIT.md](CR-REVIEW-JIT.md)): content_shell's renderers have
  it, and so does the GPU process when it runs SwiftShader. Stricter modes
  (dual mapping, a per-thread write/execute switch) can still be required
  later through the same capability;
- faults and signals to a thread (the owner's rule, 2026-10-16: build the
  layer, don't turn Chromium's features off): a fault reaches a handler on
  the faulting thread with Linux's `siginfo_t` and a full `ucontext_t`
  (V8's WebAssembly trap handler, Chromium's crash stack dumps, stack
  overflow on `sigaltstack`), and a signal reaches a chosen thread
  (built and reviewed: [SIGNALS-PLAN.md](SIGNALS-PLAN.md),
  [SG-REVIEW.md](SG-REVIEW.md));
- Jam OS's own sandbox: **not built** (CR-f, step 2 above).

**The owner's choices for Chromium (2026-10-11).** Early builds without
the sandbox may be used for ordinary sites (not banking or logins that
matter) until Jam OS's sandbox lands and is reviewed. Chromium is built on
the PC's WSL (100-150 GB, multi-hour clean builds; the build sizes its
claim on the PC's CPUs and memory from what ninja would do). The version
tracked is Alpine's stable (155 now, already patched for musl), moving
when Alpine does. A plain open build: no H.264 or AAC, no Widevine, no
Google API keys (no sync, no Safe Browsing). Off the path: M14.5, a CA
store, D-Bus, NSS and a zygote (Chromium runs `--no-zygote`; `fork` itself
is on the path since 2026-10-16). Widevine would not be built by reverse
engineering (illegal circumvention); the only road ever considered is
Google's own binary in an isolated small VM, not planned.

Not on the path, so after Chromium (unless the owner makes one a goal):
M13.5's mksh, M14 (the ABI freeze), M15 (self-hosting), M10 and M10.5
(ACPI power, sleep), M12.1's follow-ups, ports nobody needs yet, and new
Jam OS apps beyond the system's own. Parked by the owner: moving the test
programs out of the boot image (13 of its about 29 MiB are tests: RW11 of
[RUNWELL-PLAN.md](RUNWELL-PLAN.md)).

M8.6 (cleanup and polish) is done: [what it delivered](HISTORY.md#m86-cleanup-and-polish).

The audio track (A1, A2, AS) and M8.5 are done:
[what they delivered](HISTORY.md#audio-a1-a2-as-and-m85-kexec).

M8 (storage) is done: [what it delivered](HISTORY.md#m8-storage).
Known limits it left:
- A panic's own text is not in that boot's log file: logd can only save
  what it had synced. Since M8.5 the next boot saves the whole log next
  to it as `boot-NNNN-crash.txt`.
- GPT sticks are not read (an NVMe drive's GPT is).

## Later

What is left of the milestones, in the order the owner set (see [Now](#now)).
A finished milestone's row is gone from here: the status table says what
was built, [HISTORY.md](HISTORY.md) says what M0 to M12.1 delivered, and
the later milestones' plans (M12.5, M12.7, M13, M13.6, M13.7 and the
Chromium layers' plans) hold their design until HISTORY.md joins them.

| # | What | Done when |
|---|---|---|
| CR | Chromium, the main goal (the owner, 2026-10-11), reached layer by layer (see [Now](#now)): every layer general, only the layers Chromium needs built before it. Its plan is [CR-PLAN.md](CR-PLAN.md). Left: CR-d (`chrome`, track CB6) and CR-f (Jam OS's sandbox: each renderer a job with only its handles, the JIT power handed to renderers by the browser alone, a kernel flag across `/svc/exec`; its plan SANDBOX-PLAN.md is being written) | `chrome` opens a window on the PC and browses real sites over HTTPS, installed by `pkg`; then a compromised renderer holds handles that reach nothing but its own channel; then rendering on the iGPU |
| GPU | The iGPU's render offload and "run well" (the owner, 2026-10-20: after the sandbox). The monitor stays on the RTX 4080 SUPER; the Intel UHD 770 (disabled in the firmware today) renders. Since the owner's licence decision of 2026-10-11 (GPL allowed in separate programs) one road is Linux's own GPU drivers run as Jam OS driver programs over a translation layer, as FreeBSD's drm-kmod does; GPU-PLAN.md (being written) chooses the road. "Run well" is [RUNWELL-PLAN.md](RUNWELL-PLAN.md): its targets against Linux on the same PC, the 50-site soak, the profiler, WR2's two questions as tracks and the init run's time; its wave 1 (RW2, RW7, RW9, RW11) needs no GPU. G3 (mode setting and vsync through the iGPU) and G4 (3D beyond software) stay deferred | the browser draws on the iGPU on the PC and its frame times are measured against Linux's on the same machine |
| G2 | What is left of Wayland clients for Chromium ([G2-PLAN.md](G2-PLAN.md)): GD, the search box starting a package's program, and the join. After Chromium, by need: foot, SDL2, Chocolate Doom, swayimg and the GTK spike (GTK and Qt are a later porting project of their own) | `chrome` starts from the search box; the plan's PC check passes |
| M13.5 | The shell. `fork`, `vfork` and `exec` are built. What is left: a permissively licensed interactive shell (mksh) ported unpatched, then the Jam OS commands as programs in `/bin` first, so no command is lost (built for the ones that need no power a program can't hold: `free uptime version lscpu ps dmesg log mem memmap pci iommu bench net ping host usb df hexdump crashlog` are in five boxes in the boot image, [M13-PLAN: Jam commands as programs](M13-PLAN.md#jam-commands-as-programs-notes); the rest wait for a POSIX shell that passes the root resource on, and programs allowed to reach init's and the volumes' control channels). Once it matches the Jam shell it is the default in terminals and over SSH (`ssh pc command` and later scp through `sh -c`); the Jam shell stays in the boot image as the recovery shell. Its `setpgid` and job-control questions (a group join into a sibling's, exec notices) are settled with it | the default shell in a Jam OS terminal and over SSH is the ported shell; `$(...)`, subshells, pipelines of builtins and `configure`-style scripts run; every Jam OS command still works from it |
| M10 | uACPI: poweroff, power button, ACPI reboot (uACPI stays in the kernel); tickless idle (since M11.5 a sleeper due after its CPU's next tick arms no timer and waits for the tick to re-arm for it, `after_next_tick` in `kernel/sched/wait.c`: a CPU that stops its tick must arm its timer for its sleeper queue's head first). After Chromium | clean shutdown on real hardware |
| M10.5 | S3 sleep (suspend to RAM) on top of M10's ACPI: every driver saves and restores its device. After Chromium | the PC suspends and resumes with USB, audio and the network working again |
| M14 | Stable syscall ABI: frozen only after POSIX has put its weight on it; versioned and documented. It starts with a short review of every interface added since M12's review (M10's power calls, M10.5's driver suspend and resume, and everything M12.5, M13 and the Chromium layers added: pagers, descriptor passing, JIT VMOs, the exception entry, directory channels, watches and locks, the audio API); G1's protocol is M12's. After Chromium | old binaries keep running on new kernels |
| M14.5 | Maybe, parked: dynamic linking. A loader for shared libraries and `dlopen`, for porting big software built around them (LibreOffice, Python's C extensions, GTK or Qt apps, plugins, Mesa's drivers if a GPU road needs them). Static linking stays the default for Jam OS's own programs and for Chromium. `allow` must then cover the libraries a program loads as well as the program file (each approved by its hash), and a program's list names the libraries it may load. Only worth doing when such a port is a goal | a ported program loads a shared library and a plugin with `dlopen`, and a changed library is refused until approved |
| M15 | Self-hosting: make, binutils and GCC ported onto M13's POSIX layer, and CPython ported so the build tools (about 19,000 lines of Python in tools/, ports.py among them) run on Jam OS unchanged instead of being rewritten in C (the owner, 2026-10-11); a small compiler (TCC/cproc) may come first. Python's port needs M13.5 (`subprocess`), M13.6 (its library's layout) and its C modules built in until M14.5. After Chromium | Jam OS rebuilds itself on the PC and boots the result, with no Mac involved |
| A3 | Maybe: USB audio devices (headsets, USB sound cards). HDMI/DisplayPort audio through the RTX is not planned | (not planned in detail) |
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
- **A Mac-style frame for `chrome`** (the owner, 2026-10-12, after the
  system title bar of CB6): tabs in the title bar row with the real jam
  circles, as on macOS. A Jam OS Wayland protocol any client may use: the
  client keeps a corner clear and names the parts of its top strip that
  move the window; the compositor draws the circles over that corner and
  handles their clicks ([CR-PLAN: Later](CR-PLAN.md#later)). After CR-d.
- **Address randomisation**: static-PIE for every program, the loader
  taking `ET_DYN` with musl's self-relocating start.

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
- **GPT sticks** are not read (their partitions are not mounted): an NVMe
  drive's GPT is (M12.7's library), but usb-storage still reads the MBR
  only.
- **Small hardening** (2026-10-20): `syncfs` on extfs has
  no error sequence (a write-back error reaches `fsync` and `msync` of the
  file, not `sync`); `ext_iget` should say from the bitmap that an inode is
  free (a freed inode reads like a removed one); a kernel freeze that
  would begin while a writer waits should back off (FR1, option (b), before
  the sandbox: track FZ is on it); `FilePathWatcherTest.RecursiveWatch` is on
  base's skip list until a directory descriptor's `futimens` tells the
  parent's watch (the directory handles' review is on it).
- **The init run is near its time cap** (186 to 292 s of 300 under load on
  the PC): if the run grows, move the tests out of the boot image (13 of
  its about 29 MiB are tests) rather than raise the cap. That is RW11 of
  [RUNWELL-PLAN.md](RUNWELL-PLAN.md).
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

- `fork` as a kernel system call that clones a process: libc's `fork`
  is built on the kernel's copy-on-write child VMOs and `process_replace`
  ([FORK-PLAN.md](FORK-PLAN.md)), which is a safer virtual fork, not a
  clone of everything.
- Wi-Fi.
- HDMI/DisplayPort audio through the RTX.
- Filesystems other than FAT32 (the boot ESP and sticks) and extfs (the
  owner, 2026-10-11, reopened the 2026-10 decision for porting: symlinks,
  permissions, large files). NTFS is never read or written: Windows' drive
  is listed and left alone.
- Widevine (protected video) by reverse engineering: that would be
  circumventing a protection, and it is refused (the owner, 2026-10-19).
  The one road that has been considered is Google's own binary, in an
  isolated small VM, someday; Chromium is an open build without it.
- User accounts: single user; handles are the only authority.
- Running Linux binaries unchanged (a Linux system-call personality, as
  WSL1 or FreeBSD's Linux layer): the owner, 2026-10-09. Programs are
  ported from source onto the POSIX layer instead.
- Own versions of general apps (a YouTube app, a browser, editors,
  players): the owner, 2026-10-11. Jam OS writes its own programs only
  where they are part of the system (settings, the shell and its
  commands, the compositor, system tools); every other app is ported. A
  browser is a port of Chromium: its Linux code on musl over Jam OS's
  POSIX layer and Wayland (the owner, 2026-10-11, after
  [CR-RESEARCH](CR-RESEARCH.md); the plan is [CR-PLAN](CR-PLAN.md)), run
  with no zygote.
  Ports are chosen by need, not collected (the owner, 2026-10-11): a
  program is ported when something needs it (a dependency, a tool a track
  uses) or when the owner sets it as a goal (vim, a browser, Doom), never
  to fill a list.
  Jam OS's own general apps written so far (Jamjar, the music player;
  `play`, `fetch`, `speed`) were tests of the system more than products:
  the owner expects to replace them with ported alternatives once porting
  is routine (2026-10-11).
