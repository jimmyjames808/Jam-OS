# RUNWELL plan: does Jam OS run Chromium well, and how would we know

Chromium runs on Jam OS: content_shell with its own multi-process
launcher, the JIT in the renderers, WebGL by SwiftShader, sound,
fonts, base_unittests at 8,901 passing ([CR-PLAN](CR-PLAN.md)). The
owner's next question is not "does it run" but "does it run *well*",
and the answer today is "we don't know". The only Chromium numbers
we have are JetStream and Speedometer with the JIT against
`--jitless` in QEMU ([BENCH](BENCH.md#chromiums-jit-cr-e-qemu-on-the-pc-2026-10-19)),
which say the JIT works and nothing about Jam OS against anything
else; no number has a Linux beside it. The Linux column of the kernel
benchmarks (`tools/linuxbench`) was built for M11.5 and, as far as the
docs say, never run. Meanwhile the owner has noticed slow spots by
feel: ssh and the shell ("last output late", a stall on typed-ahead
input, a slow prompt under load), and the init run is close to its
timeout. This plan is "run well": parked by the owner on 2026-10-18 to
come with the iGPU milestone, after CB6 and the sandbox (CR-f).

**In one paragraph.** "Runs well" is a short list of numbers, each
read against Linux on the same PC with the same Chromium 155 source
and flags as far as the OS allows (section 1). Three kinds of run
produce them: Jam OS and a Linux guest in the same QEMU (cheap,
repeatable, for regressions), the same pair on the bare PC from two
sticks (the headline, and what calibrates the QEMU ratio), and never
WSL2 (it is a different machine). One browser driver, speaking the
DevTools protocol and reading `/proc`, runs unchanged on both systems
and produces page-load times, memory per tab and the browse-50-sites
soak (section 2). Two of WR2's open questions, a store's deadline
counted from the pager's progress and an orderly stop bounded by
progress, are two small tracks (section 3). Where the time goes needs
instruments Jam OS lacks: a POSIX-level benchmark that compiles on
both systems, a sampling profiler, and Linux-named `/proc` counters
(section 4). The init run's time and "tests out of the boot image"
are one tooling track (section 5). The iGPU changes some numbers and
not others; this plan takes the software-rendering baseline first so
the GPU shows up as a measured difference (section 6). Thirteen
tracks, about 32 agent-hours for the instruments and the first
measurement; the fixes the numbers call for are not in that figure.

## What this plan is not

- **Not the GPU.** GPU-PLAN (written beside this plan) decides the
  driver, Mesa, the offload path and the staging. This plan names the
  numbers that change when it lands (section 6) and takes the
  baseline before it does.
- **Not the sandbox.** SANDBOX-PLAN (CR-f) decides what a renderer
  may do. The comparison is made with the flags Jam OS runs today
  (`--no-sandbox --no-zygote`) and again against Linux's defaults
  once the sandbox lands (Q4).
- **Not a performance-fix plan.** This plan builds the measurements
  and the instruments and makes the first comparison. What the
  numbers say is slow is fixed by tracks that do not exist yet; the
  first round is estimated, not designed (section 10).
- **Not a new kernel benchmark.** `bench`, `perop` and `linuxbench`
  ([BENCH](BENCH.md#method)) stay as they are. What is missing is the
  level above them: Linux programs' system calls through Jam OS's
  libc, and Chromium itself.
- **Not Windows.** Nothing here compares to Windows or macOS.
- **No new sites collecting accounts.** The soak visits pages
  anonymously: no logins, no purchases, nothing that posts (the
  owner's rule for the unsandboxed browser: ordinary sites, not
  banking or logins that matter).

## Where it starts

What exists:

- **The Chromium numbers**: JetStream 2.2 (14 JavaScript tests 918.9
  with the JIT, 127.4 `--jitless`; 5 WebAssembly tests 225.4) and
  Speedometer 3.1 (six suites, 18.3 and 12.9), content_shell
  multi-process, 4 CPUs and 6 GiB under KVM on the i7-14700, batch
  19205938; the 30-million-iteration loop 110 ms in one call, 23 ms
  once TurboFan has it ([CR-PLAN](CR-PLAN.md#cr-e-notes)). `tools/cr-test.sh bench` makes
  them in about 6 minutes.
- **The kernel numbers on the bare PC** ([BENCH](BENCH.md)): syscall
  30.7 ns, page fault 720 ns, reply-and-wait call 765 ns, TLB
  shootdown to 27 CPUs 4.2 us. The Linux column of those lines has a
  program (`tools/linuxbench`) and an owner's recipe
  ([README](../tools/linuxbench/README.md): Ubuntu 24.04 live stick,
  results to the SanDisk) and no results.
- **The PC** ([HARDWARE](HARDWARE.md)): i7-14700 (8 P-cores with
  Hyper-Threading, 12 E-cores, 28 CPUs), 32 GB, RTX 4080 SUPER with
  the monitor on it (the UHD 770 is off in the BIOS), RTL8125B on
  VLAN 21, a Crucial NVMe (Windows') and a MAXIO NVMe (Jam OS's).
  Tests run in QEMU/KVM inside WSL2 inside Windows' Hyper-V: **every
  Jam OS Chromium number above is a nested-virtualisation number.**
- **The tools** (section 4): `bench`, `perop`, `top`, `ps -t`,
  `free`, `mem`, `time`, `speed`, `/proc` (stat, status, statm,
  maps, smaps_rollup), CPU-time clocks, `tools/symbolize-threads.py`.
  No profiler.
- **The harness**: `tools/cr-test.sh` drives content_shell in QEMU by
  typing and clicking through the monitor and reads the screen
  (`shell_run`), and `tools/cr/bench-server.py` serves the benchmarks
  with a reporter page. That drives QEMU only: nothing of it works on
  the bare PC or on Linux.
- **Parked pieces this plan takes**: WR2 design questions 1 and 3
  ([WRITEMAP-REVIEW](WRITEMAP-REVIEW.md#design-questions)); the init
  run near its cap (the 10-20 checks: 126 to 133 s on a quiet PC,
  186 to 292 s of the 300 s `init_timeout` under load); "tests out of
  the boot image".

## 1. What "runs well" means

### 1.1 The measures and their targets

Every target is a ratio to Linux on the same PC, on the same
Chromium build as far as the OS allows (1.2). The ratios below are
provisional: nobody has measured the first ratio yet. They are set
now so the first measurement is a pass or a fail and not a number
looking for a meaning, and they are revised once, after the first
report (Q3).

| Measure | How it is taken | Today (QEMU, 4 CPUs) | Target |
|---|---|---|---|
| **JetStream 2.2, JavaScript** (geometric mean of 14) | `tools/cr-test.sh bench` | 918.9 | at least 0.95x Linux's |
| **JetStream 2.2, WebAssembly** (5 tests) | the same | 225.4 | at least 0.95x |
| **Speedometer 3.1** (the six suites of `bench`) | the same | 18.3 | at least 0.90x (DOM, style, layout, paint, IPC: the OS counts more here) |
| **The long loop, on-stack replacement** | `tools/cr-test.sh jit` | 110 ms (23 ms with the compiles on the main thread) | the same as Linux's: if Linux also stays at Maglev's speed, it is V8's tiering and nothing to fix ([CR-PLAN](CR-PLAN.md#cr-e-notes) asked for exactly this) |
| **Page load** (time to first contentful paint, to the load event, to largest contentful paint; p50 and p90 of 5 loads of each page of the replay set) | the browser driver (2.4) against a replay server | none | p50 within 1.2x of Linux's, p90 within 1.5x |
| **Tab open** (`Target.createTarget` of about:blank to its load event: a renderer launched by fork and exec, V8 and Blink initialised) | the driver | none | within 1.5x with matched flags (Linux has a zygote by default: 1.2 only when its zygote is off too) |
| **Browser start** (process created to first window painted, cold and warm page cache) | the driver and the system log | none | cold within 1.5x, warm within 1.2x (a 335 MB static program run from a pager) |
| **Memory per tab** (the system's used memory with 1, 5, 10 and 20 tabs of the replay set open and settled for 30 s, less the memory before the browser; and the sum of the browser's processes' Pss) | the driver and `/proc` | none | within 1.25x of Linux's with matched flags |
| **Idle** (CPU use and wakeups of the system with 10 settled tabs, 5 minutes) | `top`, `/proc/stat` | none | no more than Linux's |
| **Scrolling and animation** (frames drawn and frames late over a scripted 10 s scroll of three heavy pages, and testufo) | the driver's frame counts | none | recorded, no target until the GPU (section 6) |
| **Boot** (kernel start to the shell prompt, and to the desktop's first painted frame; the splash excluded; NVMe and stick) | the kernel log's time stamps | keyboard ready 0.29 s after kernel start (M8.6's track A, [HISTORY](HISTORY.md)); the rest not recorded | no more than a minimal Linux's (kernel handoff to a Wayland compositor's first frame, `systemd-analyze` and `dmesg`) |
| **ssh and shell** (below) | the latency kit (RW2) | none | below |

The ssh and shell measures, the things the owner noticed ("last output
late", finding 1 of [M13-REVIEW-TERMINAL](M13-REVIEW-TERMINAL.md); the
stall on typed-ahead input, SF and SF2 in [M13-PLAN](M13-PLAN.md),
checked by `tools/sshcheck.py`; the session shell slow to show its
prompt under load, a watch item, thought to be the history loading from
ext): each as a
median and p99 over at least 1,000 samples from the Mac over the same
LAN to Jam OS's sshd and to OpenSSH on Linux:

| ssh and shell measure | Target |
|---|---|
| Keystroke echo through `ssh -tt` (one byte written, the echo read back) | median at most 2 ms above Linux's, p99 at most 10 ms above |
| `ssh host true`, connect to exit | within 1.5x of Linux's |
| Output flood (`seq 1 200000`, and `cat` of a 50 MB file) to the client: time to the last byte, and whether the prompt comes after it | at least 0.8x Linux's throughput; the prompt always last |
| Command start (a loop of 1,000 `true`s by the shell: fork, exec, wait) | within 2x of Linux's `sh -c true` loop |
| The shell's prompt after 5 s of load on every CPU (a `soakload`-style load) | under 1 s |
| Typed-ahead: 80 lines written at once, all run in order | exact, as `tools/sshcheck.py` checks today |

Jam OS-only budgets, with no Linux to read them against (the compositor
counts its own frames): key press to the frame that shows it at most 2
frames at 60 Hz (33 ms) in a terminal window; a window drag not
dropping frames at 2560x1440. These are the "feels fast" numbers and
stay Jam OS's own.

### 1.2 Getting the Linux numbers fairly

A comparison is only as good as what is held equal. The list, in the
order of how much each can distort:

1. **The same Chromium source and flags.** Build the same tree
   (`~/cr/155.0.8059.39`, the tarball plus ports/chromium's patches)
   a second time for Linux into its own output directory: the same
   `ports/chromium/files/args.gn` except what is Jam OS's
   (`is_jamos`, `is_musl`, the custom toolchains, `use_jamos_audio`)
   and Linux's own pieces put back (glibc, Chromium's Debian sysroot,
   its ordinary toolchain). Everything the build config can equalise
   stays: release, no PGO, no LTO, no CFI, `symbol_level = 0`,
   `use_custom_libcxx`, static component build, SwiftShader on,
   `ozone_platform = "wayland"`, no proprietary codecs. Jam OS's
   patches (0901 to 0916, "on-jamos" in most names) and the musl ones
   are meant to be conditional on Jam OS or musl; the ones not named
   so (0006 and 0007, the DNS config service and the disabled tests,
   for one) are not known to be. RW1 reads each, says which Linux does
   not run, and if one is not conditional unpacks a second, unpatched
   tree (12 GB) rather than measure a patched Linux. Both sides should
   malloc through PartitionAlloc (the shim's musl patch, 0107, is
   there for that; RW1 confirms it on Jam OS), so the allocator is not
   a variable; musl's `memcpy` and `memset` against glibc's
   ifunc-selected AVX versions is (a suspect to profile, section 4).
2. **The same flags at run time.** `--no-sandbox --no-zygote`
   `--disable-gpu` (and `--enable-unsafe-swiftshader` for WebGL),
   no `--jitless`, `--user-data-dir` in RAM on both, field trials off,
   `--host-resolver-rules` and `--ignore-certificate-errors-spki-list`
   to a replay server (below) so no DNS or live site is a variable.
   Linux's *defaults* (zygote, sandbox) run too, as a second row,
   because they are what a Linux user gets (Q4).
3. **The same machine state.** Same BIOS settings, recorded and not
   changed between runs (XMP, power limits, C-states, hybrid-core
   options); SMT and E-cores on; the CPU governor `performance` and
   the energy preference as `tools/linuxbench` sets them (Jam OS
   leaves the firmware's P-state choice; the record says what each
   side ran at); the same 2560x1440 framebuffer; the same switch port
   and VLAN 21 (Linux needs a VLAN 21 interface; HARDWARE's network
   section has the rest); nothing else running (on the bare PC
   nothing is: the Windows side is off; in QEMU the batch holds the
   whole CPU budget as the 8-CPU init run does, so no agent's QEMU
   runs beside it).
4. **The same order and number of runs.** A, B, A, B on one day, at
   least five of each, the median and the spread reported, the first
   run of each pair thrown away if it was the cold one and kept as the
   "cold" figure. Between pairs a cool-down of five minutes. A drift
   of more than 3 % between the two A runs makes the pair void.
5. **The same data.** Both systems read the same replay archive over
   the same 100 Mbit link at the same latency (a few ms), served from
   the Mac or the PC's WSL by one server; the benchmarks come from the
   pinned copies `tools/cr/toolchain.pins` names.
6. **The record.** Every run saves the environment: both kernels'
   command lines, microcode, the vulnerabilities files (as linuxbench
   does), the Chromium build ids and flags, the CPU frequencies the
   system reports, the BIOS state. A number without its environment is
   not entered in BENCH.

Differences that cannot be equalised, and are named in the report
rather than hidden: musl against glibc; the Wayland compositor (Jam
OS's own against weston or another on Linux); Linux's mitigations
(run with the defaults and with `mitigations=off`, as linuxbench does:
Jam OS has none, and Raptor Lake needs no KPTI); Linux's transparent
huge pages (Ubuntu's `madvise` mode means Chromium gets none either,
and Jam OS maps user memory in 4 KiB pages as far as the docs and
`/proc/meminfo` say; RW10 confirms); and a stock product Chromium,
which is not the same build at all (below).

**A third row for scale.** A stock Linux Chromium (Ubuntu's or the
official Google build) is measured once per session, with its
defaults. It is not the reference for any target: official builds have
PGO, ThinLTO and CFI that Jam OS's build does not, and they are 10 to
20 % faster on a page benchmark for that alone. The row shows how far
the product is from the plain build, so a Jam OS result is not
blamed on the OS for what the build config did.

### 1.3 Why bare metal, and what QEMU is good for

Every Chromium number so far ran as QEMU/KVM inside WSL2 inside
Hyper-V. What that does to each kind of cost:

- **Page faults, `mmap`, TLB misses, IPIs, timer interrupts**: all
  cost more under nested virtualisation (two-dimensional page walks
  with shadowed second-level tables, exits for IPIs and timers).
  BENCH already says QEMU's `perop` numbers "mean nothing". V8's
  garbage collector and PartitionAlloc's commit and decommit are
  exactly this kind of work, and Jam OS's TLB shootdown to 27 CPUs is
  4.2 us on the bare PC.
- **Scheduling**: Windows decides which physical core each vCPU runs
  on, hybrid P and E cores included; Jam OS's placement rules never
  see that.
- **Timers and clocks**: emulated or paravirtual; the TSC deadline
  timer Jam OS uses on hardware is not what the guest gets.
- **Storage and network**: virtio or emulated devices in front of a
  VHDX or a NAT, not the NVMe and the RTL8125B the owner uses.

So QEMU numbers are good for *changes*: a build that is 5 % slower in
the same QEMU than yesterday's is 5 % slower. They are not good for
*levels*: "Jam OS reaches 0.93x of Linux" must be read on the bare
PC. Both Jam OS and Linux in the same QEMU carry the same
virtualisation tax, which makes **a Linux guest in the same QEMU** the
cheap, repeatable comparison (the same `-smp 4 -m 6G -cpu host`, the
same flags, an Alpine or Debian root with the Linux content_shell,
scripted by the same batch): its ratio is not the bare PC's, but the
two ratios should agree to within 10 to 15 points, and the one
session on bare metal says whether they do. If they do, the QEMU
ratio goes into the regression runs; if they do not, the bare PC
decides and the QEMU run only tracks change.

**WSL2 is not a comparison machine.** It is one level of
virtualisation (Jam OS in QEMU is two), a Linux kernel built by
Microsoft with its own scheduler hints, its memory limited by
`.wslconfig` (23 GB here) and ballooned, a virtual disk, a NAT, and
Windows' background work and power plan on the same cores. Using it
as "Linux" would favour Linux by an unknown amount. Its place is
building (the Linux Chromium is built there) and automation.

**The bare-metal session** (the owner's, hardware night): two sticks,
Jam OS's and an Ubuntu 24.04 live stick as `tools/linuxbench` uses
(booted with `toram` so storage is not a variable and `nomodeset`, so
Linux draws to the same GOP framebuffer by CPU, like Jam OS today),
the SanDisk carrying the Linux content_shell with its resources, the
replay archive and the results. In one sitting: `linuxbench` twice (as
README says: defaults and `mitigations=off`), then the browser driver
on each system alternately (1.2, item 4). About three hours, most of
it waiting for the pairs.

### 1.4 How a run is recorded

Results go into [BENCH](BENCH.md) as a new section ("Run well against
Linux") with the environment of 1.2 item 6 above the table, in the
style of its other sections: each row a line, each column a system,
the ratio last. Raw data of a run (the driver's per-page records, the
`/proc` samples) stays in the batch's output directory under `build/`
and the report names its batch id. A target missed is entered as a row
with its ratio in bold, and becomes a fix track only when the owner
says so (the first round, section 10).

## 2. The browse-50-sites soak

The browse soak is not the kernel's soak (`soak-test.sh`, shuffled
tests under load). It answers one question: does the system stay up
and the browser stay right while a person's ordinary browsing goes on
for hours? The name in a runlist is `browse`.

### 2.1 The sites

Fifty real sites an English-reading user, here an Australian one,
visits. Each is visited anonymously by URL, scrolled, and left. The
list is the starting set; it lives in a text file beside the driver
(URL, category, what to expect), the owner edits it (Q5), and RW6
checks every entry on the Linux control first: a site that fails
there is the site's, not Jam OS's.

| Group | Sites |
|---|---|
| Search and reference (6) | google.com (a search for "weather sydney"), duckduckgo.com (a search), en.wikipedia.org/wiki/Alan_Turing (a long article with images and maths), en.wikipedia.org/wiki/List_of_countries_by_population_(United_Nations) (big tables), archive.org, bom.gov.au (radar images that refresh) |
| News (8) | bbc.com/news, abc.net.au/news, theguardian.com/au, nytimes.com (a paywall page: loads, shows the wall), cnn.com, news.com.au, smh.com.au, apnews.com |
| Video and audio (6) | youtube.com, youtube.com/watch?v=aqz-KE-bpKQ (60 s of VP9 and Opus, the video the owner already tried), vimeo.com, twitch.tv, soundcloud.com, bandcamp.com |
| Community and code (6) | reddit.com, news.ycombinator.com, github.com/torvalds/linux, stackoverflow.com/questions/11227809, medium.com, mastodon.social |
| Shopping (5) | amazon.com.au, ebay.com.au, aliexpress.com, etsy.com, jbhifi.com.au |
| Companies and documentation (7) | developer.mozilla.org/en-US/docs/Web/JavaScript, docs.python.org/3/library/, kernel.org, apple.com/au, microsoft.com/en-au, nvidia.com/en-au, w3schools.com/html/ |
| Heavy pages and graphics (8) | openstreetmap.org (tiles and panning), excalidraw.com (a canvas app), photopea.com (a big canvas and WebAssembly app), squoosh.app (WebAssembly codecs), threejs.org/examples/#webgl_animation_keyframes (WebGL), webglsamples.org/aquarium/aquarium.html (WebGL, heavy), testufo.com (frame pacing), lichess.org (WebSockets and canvas) |
| Everyday (4) | imdb.com, rottentomatoes.com, australia.gov.au, poki.com (HTML5 games and ads) |

The build has no H.264 or AAC and no Widevine (`proprietary_codecs =
false`): twitch.tv, vimeo.com and soundcloud.com load and draw but
their media may not play, and the file says so per site (`media:
plays | page only`); for those the check is "no crash and the page
draws". The two WebGL pages run only when the browser has WebGL (the
SwiftShader flag today; the GPU later). A site that shows a bot wall
("Just a moment", a CAPTCHA) is classed `blocked`, not failed, by a
title and text list; the Linux control run on the same network and
day decides which class a doubtful site is in.

### 2.2 What a visit checks

Per visit (about 50 s: up to 25 s for the load, 20 s of dwell with
five scroll steps, then the capture):

- **Loads.** `Page.loadEventFired` within 60 s. No load event and no
  network or paint progress for 120 s is a **hang**: the driver takes
  the evidence (the renderer's `ps -t` thread listing by ssh, a
  screenshot, the log tail) and reports before it goes on.
- **No crash.** No `Inspector.targetCrashed`; the browser's process
  list unchanged apart from renderers; no kernel line killing a
  Chromium process ("page fault: ..." kills, a watchdog line); no new
  Crashpad report.
- **The screenshot draws.** The renderer's `Page.captureScreenshot`
  and the compositor's own screenshot of the window (two paths: the
  second is the one that catches the compositor, the shared buffers
  and the framebuffer going wrong) both pass: not blank (at least 5 %
  of pixels away from the most common colour), not Chromium's error
  page, the title non-empty, and a per-site word in `innerText` (the
  site's name, loose, so a redesign does not fail it).
- **Memory stays bounded.** After each visit and each round the
  system's used memory and the browser processes' Pss are sampled
  (1.1); after a round the figures settle within 3 % of the round
  before, from round 3 on; the browser's total grows at most 20 MB a
  round; the peak stays below 4 GB in QEMU (6 GiB) and 8 GB on the
  PC. After all tabs close and 30 s of idle, used memory is within
  300 MB of the start (a leak in a service shows here).
- **No kernel warnings.** The serial or network log and `dmesg`
  scanned for anything outside an allow-list of known lines: WARN,
  "stuck", "watchdog", "leak", lock-checker lines, a service
  restart, "stores left unwritten", mixer underruns beyond the sound
  case's allowance. The first run writes the allow-list; later runs
  fail on a new line.
- **The system stays responsive.** Every 30 s an ssh `true` round trip
  must answer in under 2 s and the compositor's frame count must have
  moved; this is the hang detector for everything the browser's own
  checks cannot see, and the ssh sample is also the latency record.
- **Counts that must not climb.** Handles, mappings and threads of the
  browser and the services (`ps -k`) at the end of a round, against
  the start of round 2.

A visit ends `ok`, `blocked`, `site-down` (no answer from the Linux
control either), `hang`, `crash`, `blank`, `slow` (loaded after 60 s)
or `warn` (a new kernel line). The run **fails** on any `hang`,
`crash` or `blank` of a site that is `ok` on the control, on a
memory check, and on a new kernel warning.

### 2.3 How long, and where

| Mode | What | Time | Where |
|---|---|---|---|
| short | the 50 sites once, then all tabs closed and the memory check | about 45 min (QEMU, 4 CPUs, 6 GiB); about 30 min on the bare PC | a `cr-test` case `browse` in the runlist when browser-affecting changes merge; not in every pre-push |
| long | 8 rounds of the 50 (in a different order each round), then a churn phase (open 10 tabs, close them, 20 times) and a final 30 min idle | about 7 hours | a detached batch overnight in QEMU, and once on the bare PC in the hardware session or the night after |
| control | the same driver and list on the Linux build, same day | the same | the PC's WSL (headless, for classifying sites only) and the Linux guest or stick for the comparison |

In QEMU the guest needs the real internet: an untagged image
(`JAMOS_VLAN=none`) and QEMU's own NAT (`-nic user`), as the Mac
browsing recipe already does; the PC's bare run uses VLAN 21 and the
LAN's route out (`headless_shell` fetched an HTTPS page that way on
2026-10-18). The soak fetches each site about a dozen times a night:
polite, one visit at a time, 20 s dwell.

### 2.4 Driving the browser (one driver for both systems)

`tools/cr-test.sh` types and clicks through QEMU's monitor, which a
bare PC and Linux do not have. The driver for everything in this plan
is instead a host program (browse.py, in `tools/cr/` beside
`bench-server.py`) speaking the **DevTools protocol** to the browser:
`Page.navigate`, `Page.loadEventFired`, `Page.captureScreenshot`,
`Performance.getMetrics` (script, layout, style and task durations:
a free split of "where the renderer's time went"), `Input.dispatch...`
for the scroll, `Target.createTarget` for tab opens, crash events. It
reads `/proc` and the process list over ssh on both systems, which is
why the Linux-named counters of section 4 matter. The same file runs
against Jam OS and Linux; only the address differs.

Open question, and RW3's first half-hour: **does the browser's debug
port work and is it reachable?** `--remote-debugging-port` listens on
127.0.0.1; whether content_shell honours `--remote-debugging-address`
is unknown to this plan, and Jam OS's sshd (tinyssh) has no port
forwarding. In order of preference: the address flag works; a
forwarding relay program in the guest (forty lines of POSIX, a
package); `--remote-debugging-pipe` driven by an in-guest driver (the
second-best, since it cannot be the same host program). If the port
cannot be reached at all, the QEMU-only fallback is the existing
monitor-driven script, and the bare-metal and Linux runs lose their
driver, which would make RW3 the plan's critical risk (section 10).

For page loads the pages come from a **replay archive** recorded once
from the live sites (a recording proxy on the Mac; the first twelve
sites of 2.1's list that are static enough, plus a few synthetic
pages), served over HTTPS by a local server with a fixed latency.
Both systems run with `--host-resolver-rules="MAP * <server>"` and
`--ignore-certificate-errors-spki-list=<the server's key hash>` (the
way Web Page Replay does it), so the same bytes arrive in the same
order, and nothing live (DNS, the internet, a site changing) moves a
page-load number. The live list of section 2.1 is for the soak, which
wants real sites exactly because they vary.

## 3. WR2's two questions

[WRITEMAP-REVIEW](WRITEMAP-REVIEW.md#design-questions) left four design
questions; WR2 built question 2 (errseq: each mapper hears of a
write-back error once, [HISTORY](HISTORY.md#writable-file-mappings-on-ext)) and
WQ closed its replay window. Questions 1 and 3 are the owner's "run
well" pieces; question 4 is FR1's, with FZ.

### 3.1 Question 1: a store's deadline counted from the pager's last progress (RW7)

**Today** (`kernel/mm/aspace.c` `store_wait`, `kernel/object/pager.c`
`pager_throttle`): a store to a clean page of a writable pager VMO,
when the pager's dirty count is at its limit, waits for a write-back
with `*until` set at its first wait to now plus the pager's answer
time (60 s), and is killed ("page fault: the file's pager didn't
answer", SIGBUS, `ERR_IO` in a user copy) if the count has not fallen
by then. The deadline never moves, whether or not the pager is writing
meanwhile.

**The harm** (review finding 6): a pager that is busy and working, or
contended, kills a healthy writer. 512 MiB dirty on a 10 MB/s stick
takes 50 s to drain; a `sync` in one process, which holds extfs for
its whole run, while another stores at the limit; other writers taking
the room each write-back frees. The writer did nothing wrong and the
pager was not hung. Linux's writers wait as long as write-back goes
on.

**The change.** The pager keeps a counter, `wb_done`, moved (under the
lock it already takes to queue and finish write-backs) each time a
`PAGER_OP_WRITTEN` cleans at least one page of any of its VMOs, with a
success or with an error: an error answer cleans pages too (Q1's
answer, "the pages become clean"), so a failing disk that is still
answering is progress. The throttle and `store_wait` carry a small
state, `{ until, seen }`, instead of the bare `*until`: at each look,
if `wb_done` differs from `seen`, `seen` takes it and `until` moves to
now plus the answer time. The wait on the request is a slice up to
`until`; a slice that ends with progress made looks again, one that
ends with none is the kill, with the old message. The pager's own
deadline for a *read's* request is unchanged (it is the page's own
arrival, as the review says).

**An absolute ceiling (my call).** Progress for others is not progress
for this writer: a store that other writers keep beating to the freed
room could wait for ever while the machine is healthy. The state also
holds `first`, the start of the first wait, and the store is killed
at `first` plus ten times the answer time (ten minutes) with a
different line ("the file's pager is too busy"), so a starved writer
is one visible kill and not an unkillable stall; the fair queue Linux
has per writer is a later addition if the ceiling is ever met.

**Files**: `kernel/mm/aspace.c`, `kernel/object/pager.c`,
`kernel/include/jam/pager.h` (the throttle's signature and the
counter), `kernel/object/vmo.c` (the thin wrapper), and the text of
WRITEMAP-PLAN's 1.4 (it says "at most the answer time in all"; that plan
was joined into HISTORY and deleted on 2026-10-20, so the sentence now
lives in the code's comments and in git history).
**Tests**: ktest cases beside `pager_dirty_store_waits`
(`kernel/test/test_pager_dirty.c`) with a short answer time: a pager
that answers one `WRITTEN` every 0.6 answer times for four answer
times, the store surviving and completing; the same with no progress,
killed at the answer time as today; a starved writer killed at the
ceiling; a user copy in a system call returns `ERR_IO` at the same
points; then `pager-test.sh` `wpress` and `whostile` unchanged. Kernel
change: ktest at 4 and 8 CPUs. About 1.5 hours.

### 3.2 Question 3: an orderly stop bounded by progress (RW8)

**Today.** A reboot, an unmount or a shutdown writes mapped stores back
through a chain of fixed waits ([WRITEMAP-REVIEW](WRITEMAP-REVIEW.md)
finding 8 counts about 8 s): init's `mounts_sync` gives `volumes.sync`
2 s (`user/services/init/mounts.c`) and the shell's `sync` 6 s
(`user/services/init/ctl.c`); volumes sends `fs.sync` to every mount
at once and waits one 5 s deadline for all (`user/services/volumes/disk.c`,
`SYNC_WAIT` in `disk.h`); extfs's `fs.sync` itself (`wback_all`) has no
deadline of its own, so it is those callers' waits that end it; the
orderly stop's own write-back (`wback_stop`) is bounded by 3 s
(`WB_STOP_NS` in `user/services/extfs/wback.c`) and logs "stores left
unwritten" when it gives up. The dirty limit is 512 MiB a mount on the
PC, which a stick writes in 50 s or more; so 400 MiB stored through a
mapping and then `reboot` loses what the 8 s did not reach. (The
review's 8 s is the sum of the last two; the track reads all four
waits and writes down the real chain before it changes one.)

**The change** (the review's recommendation, which I accept):

- **extfs** writes back until it is done or until it has made *no*
  progress for a stall limit (10 s: a run of 128 KiB ends well inside
  that on any disk that works, and NVMe's own command timeout is 30 s,
  which the stall limit must not undercut), not for 3 s in all.
  `wback_stop` and `wback_all` count completed runs; the stop gives
  up on a stall, and says how many MiB it left.
- **volumes** waits for a mount's `fs.sync` for as long as that mount
  reports progress, up to a ceiling (10 minutes), not one fixed
  deadline. The progress report is a new event on the `fs` channel,
  sent about once a second while a sync runs ("n MiB left"): an
  IDL change in its own commit, first, as the rules say; the repo's
  idiom for "still working" is an event, not a poll.
- **init and the shell** (`reboot`, `sync`, the desktop's Reboot
  button) say what is happening ("Writing 340 MiB to the drive...", a
  notice on the desktop, a line in the shell) and a second `reboot -f`
  (which exists) still skips the wait. Power loss is unchanged: the
  data was never promised before the power cut.
- The dirty limit scaled down for a slow disk (so a stick never has
  more dirty than it can write in a minute) is the review's "later,
  M14", and stays later.

**Tests**: utest `extfs_wback` gains a slow-disk case (a RAM disk whose
writes take a set time per run, the stall limit and the ceiling as
test knobs so the case runs in seconds); `tools/pager-test.sh` gains a
case that stores 200 MiB through a mapping on a QEMU drive throttled
to 20 MB/s, orders the reboot, and checks after it, with Linux's look
at the disk as `whostile` already does, that every store is there;
`pager-test.sh` `wcut` (a power cut at each step) unchanged. The
shell scripts `basic`, `cmds`, `commands`, `files` and `kdump save`
(the stop path is shared). **Files**: `abi/idl/fs.idl` and the
generated headers (first commit), `user/services/extfs/wback.c`,
`user/services/volumes/disk.c`, `user/services/init/mounts.c`,
`user/services/init/ctl.c`, the shell's `reboot`. About 2.5 hours.

Order between the two: independent. RW7 is a kernel change (ktest 4 and
8, the independent review); RW8 is services and an IDL.

## 4. Profiling tools

### 4.1 What exists

| Tool | What it tells | What it cannot |
|---|---|---|
| `bench` (`kernel/test/bench.c`) and `tools/linuxbench` | kernel paths in ns, median and p99, with Linux's equivalent | anything above system calls; the Linux column has never been run |
| `perop` | file operations through native calls | POSIX calls through libc |
| `speed` | TCP throughput to a peer | latency distributions |
| `top`, `ps`, `ps -k`, `ps -t <pid>`, `free`, `mem`, `lscpu`, `time` | per-CPU and per-process CPU use, memory (physical pages the process has, not shared-aware), where each thread waits right now with kernel and user code addresses | a history; a sample; why a thread waits for long |
| Kernel counters (`kernel/include/jam/abi.h`'s process info: CPU ns, faults) and the wait notes (`thread_wait_note`) | totals; the current wait's reason | time in each state; time per wait reason |
| `/proc` (`user/posix/sys/procfs.c`): stat, statm, status, maps, smaps_rollup, system stat, meminfo, cpuinfo | the Linux names, so Linux tools and our harness read them | `io`, `schedstat`, per-mapping `smaps`; **Pss is Rss** (nothing says a page is shared: the kernel does not tell), which overcounts a program every renderer maps from the same file |
| CPU-time clocks and `getrusage` (`user/posix/sys/time.c`) | per process and per thread CPU time | `ITIMER_PROF` is `EINVAL` (`user/posix/sys/sigtimer.c`); V8's own sampler uses `pthread_kill` and works |
| `tools/symbolize-threads.py` with `ps -t` | a poor man's profiler: where threads are, on the host, symbolised | repetition and statistics |
| `tools/cr-test.sh bench`, `jit` | JetStream, Speedometer, the JIT's loops | page loads, memory, anything live |
| The compositor's own `bench:` lines, `fbbench` | frame cost | the browser's side |
| `tools/sshcheck.py`, `tools/ssh-test.sh` | ssh works and keeps order | how long it takes |

Not tried on Jam OS, and cheap to try (RW3's list): Chromium's own
tracing (`--trace-startup`, `--trace-to-file`: a per-thread timeline of
the browser, the renderer and the GPU process, which needs only the
CPU-time clocks and a file), `chrome://histograms`, and V8's
`--perf-basic-prof` (a map file of JIT-compiled function names in
`/tmp`, which a sampler's output needs to name JavaScript frames).
These come from Chromium, cost no Jam OS work, and find browser-level
slow spots before any kernel profile is needed.

### 4.2 What is missing, in the order it pays

1. **A POSIX-level benchmark that is one source for both systems
   (RW2).** `linuxbench` measures Linux's system calls and `bench`
   Jam OS's native ones; Chromium makes Linux calls, which on Jam OS go
   through libc and the file, socket and memory services. A new
   posixbench program, plain C against POSIX, compiled for Jam OS as a
   boot-image program and for Linux by the same Makefile, with lines
   named alike and the same method as bench (TSC pair, 4,000 samples,
   median and p99): `getpid`, `clock_gettime`, `open`/`read`/`close`
   of a cached file on ext, `stat`, `mmap`/`munmap` of 4 KiB and 64 MiB,
   `mprotect`, a first-touch page fault, futex wake and wait
   ping-pong, `pthread_create` and join, `fork`+`exec`+`wait`,
   `posix_spawn`, `socketpair` ping-pong at 16 bytes and 64 KiB (Mojo's
   transport), pipe, `eventfd`, `epoll_wait`, TCP over loopback,
   `sched_yield`, and `memcpy`/`memset` at 64 B, 4 KiB and 1 MiB
   (musl against glibc). The ratio per line says which layer call is
   the slow one in a page load before any profiler runs.
2. **The latency kit for ssh and the shell (RW2, with the above).** A
   host script (sshlat.py) that opens `ssh -tt`, writes one byte at a
   time and times the echo, times `ssh host true`, a flood and a
   command loop, and prints median and p99 per line, the same against
   OpenSSH on Linux.
3. **A sampling profiler (RW9).** A statistical profiler of the whole
   system, kernel, services and programs, because Chromium is only one
   of the places time goes (netstack, extfs and the compositor are
   others, and a page load crosses all of them). Design: the
   scheduler tick (or a 1 kHz timer while profiling is on) on each CPU
   records the CPU, process, thread, whether it was in the kernel, the
   instruction pointer and a frame-pointer walk (both Jam OS's own
   code is built with frame pointers (the Makefile has
   `-fno-omit-frame-pointer`) and Chromium's Linux default is on; RW9
   checks the build's flags) into a ring per CPU; the walk uses
   the same fault-safe user copy as a signal frame and gives up on a
   bad chain. A shell command (`prof start|stop|dump`) and a narrow
   capability right (not every program may watch the others) control
   it; the dump is raw addresses plus a snapshot of every sampled
   process's mappings (`process_get_maps`). Overhead target under 3 %
   at 1 kHz; idle samples counted, not stored. On the host, a script
   resolves addresses with `llvm-symbolizer` on the unstripped
   programs (as `tools/symbolize-threads.py` does), reads V8's perf
   map files for JIT frames, writes folded stacks and a one-file HTML
   flame graph (no network, inline script). It works in QEMU (a timer
   needs no PMU) and on the bare PC. This is a kernel change: ktest at
   4 and 8 CPUs, a new capability right, an ABI addition (its own
   commit first).
4. **Linux-named counters in `/proc` and the kernel behind them
   (RW10).** `/proc/<pid>/io` (rchar, wchar, syscr, syscw, read and
   write bytes), `schedstat` and the `stat` fields for minor and major
   faults (a major one is a fault the pager had to fetch from
   extfs), `utime` and `stime`; and the kernel's accounting that
   feeds them: per-thread nanoseconds running, runnable and blocked
   *by reason* (the wait notes already name the reason; accumulate it
   at the wake), per-process syscall counts. And the memory number the
   per-tab target needs: a real Pss (a kernel query for how many
   processes share each page of a mapping, or an exact per-mapping
   shared count; `smaps` per mapping, as Linux has it), so that "memory
   per tab" is not a sum that counts a shared program once per
   renderer. With the Linux names, the browse driver reads both
   systems the same way. An ABI addition (process info grows, as the
   faults field did: 96 to 104 bytes).
5. **Service call counts and latency (later).** The generated servers
   could count calls and time per method (a few counters in the loop
   `genidl` emits); worth building when the profile points at an IPC
   path, not before.
6. **PMU counters (later).** Cycles, instructions, cache and TLB misses
   per process. Raptor Lake's hybrid cores have different counters on P
   and E; the work is real. Wanted if the sampler shows the OS costs
   more cycles than it should and the cause is memory behaviour (4 KiB
   pages against Linux's mixed sizes is the first suspect), not before.

What the first measurement will probably put on this list's top:
`mmap`/`munmap`/`mprotect` and page-fault costs (V8 and PartitionAlloc
commit and decommit constantly; Jam OS's page fault is 720 ns and its
TLB shootdown 4.2 us), `fork`+`exec` (every renderer; the whole launch
goes through init's `/svc/exec`), `socketpair` latency (Mojo), `memcpy`,
and first-run page faults of a static 335 MB program served by a
pager. They are guesses; RW2, RW4 and RW9 turn them into facts.

## 5. The init run's time, and tests out of the boot image

One tooling track (RW11) because the two are one problem: the tests
that ride in every boot image make the run slow to start and the image
big, and the run that uses them is near its cap.

### 5.1 Where the time goes

[TESTING](TESTING.md#the-init-run-in-parts) (written 2026-10-14): the
one-boot run is about 130 s on a quiet PC (utest 118 s, posixtest
12 s), the three-part run (`tools/init-test.sh`) about 45 s, "about
40 s a part", with the rule that a part passing about 60 s gives a
prefix to an `only` line. The 10-20 checks, which use the three-part
run, report 126 to 133 s on a quiet PC and 186 to 292 s of the 300 s
`init_timeout` under load (a Chromium build beside it); 292 s on
2026-10-20 (DL1's check) made the main session cut posixtest's `dirfd`
case from 300 to 100 directories. The likely reading: the slowest part
is now as long as the whole one-boot run was on 10-14, because new
cases land in the `except` part (part 3, which also holds usbtest and
posixtest) and nobody has moved a prefix since; RW11's first step
measures it. utest's 20 slowest cases took 78 s on 10-14 because they
wait out the real timers they test (keep-alive probes, a notice's 5 s,
keys kept 5 s, init's and libjwl's deadlines, logd's syncs); the other
640 took about 40 s. The one-boot run stays in the pre-push list
(`init1b`, and `init1b8` at 8 CPUs) because a case broken by another
part's earlier case only shows there. Raising `init_timeout`
(`kernel/main.c`, default 300; `tools/storage-test.sh` passes 650)
hides the growth; every case added from now lengthens the run.

### 5.2 What the track does

1. **Measure first.** Per-case times from the RESULTS box (or the case
   runner's log) for the one-boot run and each part: the table of the
   forty slowest cases, what each one waits for, and the quiet-PC and
   loaded-PC times side by side. The part times printed by
   `tools/init-test.sh` always, with a warning line at 60 s for a part
   and 150 s for the one-boot run on a quiet machine, so growth is seen
   in the table and not by a timeout.
2. **Make the waits shorter without making the tests weaker.** Where
   a case waits out a real timer to test logic (a 5 s notice, a
   keep-alive), the code under test gets an injectable clock or a
   settings value the test sets small (the scripted-clock pattern
   `updfetch_*`, the DHCP and DNS cores already use). Cases that must
   run in real time (a real driver's timeout) stay and move into one
   "slow" group that is its own part. Aim: every part under 60 s on a
   quiet PC and the one-boot run under 100 s.
3. **Tests out of the boot image.** The boot image (about 30 MB in the
   10-18 build) carries about 12 MB of test programs: utest (4.6 MB),
   posixtest (1.4), exttest (1.1), pxtest, the `posix-*` helpers
   (about 2.5), usbtest, wltest, mixtest, hdatest, nettest, tcptest,
   dnstest and the rest. Every `update` sends them, the ESP keeps three
   builds of them, Limine reads them at every boot, and they sit in
   RAM; the shell's `crash` and `panic` and the `soak` tool come with
   them. The change: the test programs (and their `init.cfg` part
   lines) go in a **second boot module**, a test image Limine loads
   only for test boots. `boot/limine.conf` already has the entries
   (Developer, All tests, the stress and soak entries); they get the
   second `module_path`, the ordinary "Jam OS" entries do not. init
   mounts the second module's files where the first's are today
   (`/boot`), so `boot/init.cfg` and the shell's `run` names do not
   change. QEMU test boots always get the test image; `make image`
   builds both and `tools/mkbootfs.py` already packs a list of files
   (it gains a second output and the size rule applies to the shipped
   one only). The shipped build then has no `utest`, `ktest`,
   `usbtest`, `hdatest`, `mixtest`, `soak` or `crash` in its shell
   (they say "boot the developer entry to run tests"); the owner's
   hardware sign-offs use the menu entries that load the test image,
   as the stick he flashes already has them.
4. **Not in this track**: the host-side build of utest's pure-logic
   cases (parsers, protocol cores) to run in `make check`; possible,
   larger, and not asked for. It would take most of the 640 fast cases
   out of QEMU. A note for later (Q7).

Gates: none (it touches the Makefile, `tools/mkbootfs.py`,
`boot/limine.conf`, `boot/init.cfg`, init's start and the QEMU
harness). It conflicts with anything adding test programs, so it goes
between waves and the main session says when. Tests: `make check`
(a new check that the shipped image has no test program and the test
image has all of them), the init run in parts and the one-boot run,
`tools/update-test.sh` and the update tests (the shipped image is
smaller; `UPDATE_FILE_MAX` and the ESP-third rule see less), the shell
scripts basic, cmds, commands and files, `kdump save`, `layout` and
`install` (the image layout), and a boot of the shipped entry that
checks `utest` is not found.

## 6. The iGPU link

GPU-PLAN is written beside this plan; this section only names what
depends on it and what does not.

**Numbers the GPU changes**, each measured before it (the software
baseline of RW4) and after it, on both systems, in the same session:

- Scrolling and animation smoothness (frames late, 1.1), which is the
  row with no target today. Raster, compositing and a canvas move from
  the CPU to the GPU process; Linux has them on the same UHD 770 through
  Mesa.
- WebGL and canvas pages (the soak's aquarium and three.js; MotionMark
  or a smaller set if it runs): from SwiftShader on the CPU to the
  hardware, usually by an order of magnitude.
- Page loads that are image- or paint-heavy (first contentful paint and
  largest contentful paint): the software row is the baseline; some
  pages gain 10 to 30 %.
- Speedometer 3.1: only its rendering part moves; the JavaScript part
  does not.
- Video decode (the CPU use of 1080p and 4K VP9 and AV1 playback)
  if the GPU plan includes decode (`use_vaapi` is off today); a new
  row, "CPU % playing the soak's YouTube clip".
- Memory per tab: the GPU process's share, shared graphics buffers
  (the UHD 770 uses system RAM), and the tile memory the software
  rasteriser no longer holds.
- Boot: the driver's start and its firmware load (if any) add to the
  boot row.
- **The offload copy.** The owner's setup keeps the monitor on the RTX;
  if the iGPU renders and the frame must be copied to the RTX's
  framebuffer by the CPU, that copy at 2560x1440 is 14.7 MB a frame,
  885 MB/s at 60 Hz: a new line, "frame copy cost and its share of a
  core", on Jam OS only. Whatever path GPU-PLAN chooses, its cost is
  measured here, not argued.

**Numbers it does not change**: JetStream and the JIT loops, the ssh and
shell latencies, tab open, browser start, the init run, WR2's two
questions. They can be worked on before the GPU, and should be.

**Method dependencies**: the Linux side needs the UHD 770 enabled in the
BIOS (it is off now) to use the same iGPU; enabling it changes
the power and memory budget a little, so the "software" rows are
re-taken once in the enabled state in the same session and any
difference between the two states is reported. The sandbox rules for
the GPU process are SANDBOX-PLAN's ("the GPU process's rules need a
pass when the iGPU lands"); a soak run on the GPU build is the sandbox's
and the GPU's joint test.

## 7. Tracks

Each about the hours shown. Models: Opus for the kernel, the
cross-cutting and anything with a design choice in it; Sonnet 5.5 for
mechanical tracks and measurement runs. A track that builds Chromium
needs `~/cr` (the shared build lock). The two kernel tracks of wave 1
(RW7, RW9) touch different files, but their ktest runs at 4 and 8 CPUs
queue on the PC's budget like any other.

| Track | What | Files | Gate | Tests | Hours | Model |
|---|---|---|---|---|---|---|
| **RW1. The Linux build and guest** | the same tree built for Linux into its own output directory (args.gn minus Jam OS's lines; the check of which patches are conditional, and an unpatched tree if one is not); a Linux guest image for the same QEMU (Alpine or Debian root, the Linux content_shell, headless_shell, the replay archive's client side); `tools/cr/build.sh` learns a Linux target; the stage for the sticks (content_shell, resources, the driver) | `ports/chromium/build.sh`, `tools/cr/build.sh`, a new Linux-guest script under `tools/cr/`, `ports/chromium/files/` (a Linux args file) | `~/cr` free (after CB6's build); no other build running | the Linux content_shell loads a page in the guest; JetStream's `bench` runs on it with the same server | 3 (the build is 3 hours of unattended PC time) | Sonnet 5.5 |
| **RW2. posixbench and the latency kit** | 4.2 items 1 and 2: the POSIX benchmark (one source, a Jam OS program and a Linux build by one Makefile), the host script for ssh and shell latency | a new program under `user/tests/`, a new Makefile for Linux beside `tools/linuxbench`, a new host script under `tools/` | none | the program runs in the init run's parts (a quick mode, one second a line) and from the shell; the latency kit against QEMU's sshd (`tools/ssh-test.sh`'s setup) | 2 | Sonnet 5.5 |
| **RW3. The browser driver** | 2.4: the spike (does the debug port work, on what address; Chromium tracing; `--perf-basic-prof`), then browse.py (navigate, metrics, screenshots, crash events, tab opens, `/proc` samples over ssh), the replay server and archive (12 pages), the `cr-test` cases `pageload` and `tabs` | a new driver and replay server under `tools/cr/`, `tools/cr-test.sh`, `tools/cr/bench-server.py` (shared parts) | RW1 for the Linux side (the QEMU side needs only content_shell) | `cr-test.sh pageload` and `tabs` on Jam OS and the Linux guest give numbers and agree on a run-to-run spread under 5 % | 3 | Opus |
| **RW4. The first measurement** | the QEMU pair (Jam OS and the Linux guest, same flags): JetStream, Speedometer, the loops, page loads, tab open, browser start, memory per tab, idle, boot, ssh and shell; the boot lines (a "desktop painted" log line if there is none); the comparison written into BENCH; the target table of 1.1 revised once | `docs/BENCH.md`'s new section, `kernel`/`init` log lines if missing | RW1, RW2, RW3; the PC otherwise idle (whole-budget claim) | the pairs of 1.2 item 4; two pair-sets a day apart agree | 2 | Sonnet 5.5 |
| **RW5. The bare-metal session** | the owner's steps (a README beside linuxbench's: the two sticks, `toram`, `nomodeset`, the BIOS record, the order of runs, the SanDisk's layout), the scripts that run on each side, `linuxbench` on the same night; the analysis when the SanDisk comes back | a new README and scripts under `tools/`, `docs/BENCH.md` | RW4's driver working; the owner at the PC | the owner's session; the QEMU/bare ratios compared (1.3) | 1.5 + about 3 of the owner's | Sonnet 5.5 |
| **RW6. The browse soak** | section 2: the sites file, the checks, the allow-list of kernel lines, the memory checks, short, long and control modes, the `browse` case and a detached runlist for the long one | the driver (RW3's file), a sites file under `tools/cr/`, `tools/cr-test.sh` | RW3; a network route to the internet from the PC's QEMU (untagged image, NAT) | the short mode passes on Jam OS and on the control; the checks fail when fed a deliberately killed renderer, a blank page, a stalled page and a leak | 3 (+ 7 hours of PC time for a long run) | Sonnet 5.5 |
| **RW7. Q1: the store's deadline** | 3.1 | `kernel/mm/aspace.c`, `kernel/object/pager.c`, `kernel/include/jam/pager.h`, `kernel/object/vmo.c`, `kernel/test/test_pager_dirty.c` | none (WK3's files are merged) | ktest 4 and 8; `pager-test.sh` wpress, whostile | 1.5 | Opus |
| **RW8. Q3: the orderly stop** | 3.2 (its IDL commit first) | `abi/idl/fs.idl`, `user/services/extfs/wback.c`, `user/services/volumes/disk.c`, `user/services/init/mounts.c`, `user/services/init/ctl.c`, the shell's `reboot` | none; extfs's files at one track at a time | utest `extfs_wback`, `pager-test.sh` (new case, wcut), shell scripts, kdump save | 2.5 | Opus |
| **RW9. The sampling profiler** | 4.2 item 3 | `kernel/` (the tick, a ring per CPU, a syscall, a right), `user/services/shell/sh_table.c`, a host script under `tools/` | none; kernel files at one track at a time | ktest 4 and 8 (a spinning thread is found; the walk survives a bad chain; overhead measured); a QEMU run profiling a loop and naming its function | 4 | Opus |
| **RW10. Linux-named counters and a real Pss** | 4.2 item 4 | `kernel/` (accounting, a Pss query), `kernel/include/jam/abi.h`, `user/posix/sys/procfs.c`, `user/posix/sys/procinfo.c`, posixtest's procfs | RW9 not touching the same kernel files at once | ktest; posixtest `procfs`; utest; a program with a known shared mapping reports the right Pss | 3 | Opus |
| **RW11. The init run's time and tests out of the boot image** | section 5, in two commits: the measurement, the shorter waits and the budget lines; then the second boot module | `Makefile`, `tools/mkbootfs.py`, `boot/limine.conf`, `boot/init.cfg`, `tools/init-test.sh`, `kernel/main.c`, init's start, `tools/qemu-test.sh`, utest cases that wait on timers | none; goes between waves | 5.2's list | 4.5 | Sonnet 5.5 for the measurement and waits, Opus for the module |
| **RWR. Independent review and fix** | RW7, RW8, RW9, RW10 and RW11's module, findings first (a new docs/RUNWELL-REVIEW.md), High and Medium fixed with a test each: can the sampler leak another process's memory to an unprivileged reader, can a deadline change starve a waiter or hide a hung pager, can an orderly stop lose a store it used to keep, does a missing test image fail loudly | the above | RW7 to RW11 merged | hostile runs per finding | 2 | Opus |
| **RWJ. The join** | ARCHITECTURE, TESTING's rows, BENCH, CR-PLAN, HISTORY's writable-mappings section (WRITEMAP-PLAN is joined and gone; its "still open" paragraph names RW7 and RW8), ROADMAP, HISTORY; this plan folded into HISTORY when it is done | the docs | RWR | `make check` | 0.5 | Sonnet 5.5 |

**Order.**

- **Wave 1, no gates, side by side:** RW2, RW7, RW9, RW11. Four agents,
  four disjoint file sets (RW11 touches the Makefile and init: it goes
  when no other track is changing them). About 12 hours of agent time,
  4 hours of wall time.
- **Wave 2:** RW1 as soon as `~/cr` is free (its build competes with
  every other Chromium build for the PC), RW3 after the spike (the
  QEMU side does not wait for RW1), RW8, RW10 (after RW9 merges: both
  are kernel).
- **Wave 3:** RW4 (needs RW1 to RW3 and an idle PC), RW6, then RW5 with
  the owner at the PC, then RWR on RW7 to RW11, then RWJ.
- **Not before the iGPU, by the owner's parking:** RW4's final form and
  RW5 want CB6's `chrome` target (the real browser, not content_shell)
  and the GPU's second column; the first measurement can use
  content_shell and software rendering now, which is what the baseline
  of section 6 is.

## 8. Tests

| What | Where |
|---|---|
| The store's wait with progress, without it, at the ceiling | ktest beside `pager_dirty_store_waits` (RW7) |
| A healthy writer behind a busy pager survives | `pager-test.sh` `wpress`, `whostile` (RW7) |
| An orderly stop keeps what a slow disk can write | utest `extfs_wback` (slow-disk case), `pager-test.sh` (the reboot-with-200-MiB case) (RW8) |
| posixbench and the latency kit | the init run's parts, `tools/ssh-test.sh` setup (RW2) |
| Linux and Jam OS drivers agree | `cr-test.sh pageload`, `tabs` on both (RW3) |
| The comparison | the pairs of 1.2 item 4, entered in BENCH (RW4, RW5) |
| The browse checks themselves | the short mode fed a killed renderer, a blank page, a stalled page and a leak (RW6) |
| The sampler | ktest, a QEMU run naming a spinning function (RW9) |
| The counters | posixtest `procfs`, a known shared mapping (RW10) |
| The test image | `make check`'s image check, the init run in parts and one-boot, update tests, a shipped boot without `utest` (RW11) |

## 9. Questions for the owner

**Answered (the owner, 2026-10-20): every recommendation above taken** (numbers from a Linux guest in the same QEMU plus one bare-metal session, the live stick's download asked for first; a same-tree Linux build with a stock row; the provisional targets; both flag rows; the 50 sites, the 8-hour browse on request; wave 1 right after CB6; tests out of the boot image; the 10-minute ceiling and the reboot notice; the feels-fast budgets).

Decided here as technical (by the recommendations in the sections
unless the owner says otherwise): a DevTools-protocol driver shared by
both systems; the Linux build from the same tree; the sampler in the
kernel on a timer, no PMU first; Linux names for the new `/proc`
files; a ceiling on the store's wait; events for sync progress; a
second boot module for the tests; the browse soak as a `cr-test` case,
not in every pre-push.

**Q1. Where do the headline numbers come from?**

- **(A)** Bare metal, from two sticks in one hardware session; QEMU
  numbers only track change.
- **(B)** A Linux guest in the same QEMU on the PC (no owner time),
  with one bare-metal session to see whether the ratios agree.
- **(C)** WSL2 as "Linux".

*Recommendation: (B).* It is automatable and repeatable and costs the
owner one evening; (A) alone cannot be run on every change; (C) compares
different machines (1.3). *Whose:* the owner's (his evening and his
sticks).

**Q2. What is the reference?**

- **(A)** A Linux content_shell built from the same source with the
  same flags (the OS is the only difference), plus a stock Chromium row
  for scale.
- **(B)** A stock Linux Chromium only.

*Recommendation: (A).* (B) mixes the build's PGO and LTO into every
"slowness". The stock row is kept, as the distance to the product.

**Q3. The targets of 1.1.** Provisional ratios (JetStream 0.95,
Speedometer 0.90, page load 1.2 at p50, memory 1.25, and the rest),
revised once after the first measurement. *Recommendation: adopt them
as written.* Say if any should be tighter or if "feels fast" should be
the only measure; the numbers above come from what a CPU-bound page
should show when only the OS differs.

**Q4. Which Linux flags?** Matched (`--no-sandbox --no-zygote`) is
the fair OS comparison; Linux's defaults (zygote, sandbox) are what a
Linux user runs and use less memory per tab (a zygote's pages are
shared) and open tabs faster. *Recommendation:* report both rows;
targets against the matched row until CR-f lands, against the defaults
after it (when Jam OS has its own sandbox and may need its own
equivalent of a zygote for memory and tab-open time).

**Q5. The 50 sites and the overnight run.** Approve or edit the list
of 2.1 (no logins stays); and is a nightly eight-hour browse from the
PC's public address acceptable (about a dozen visits to each site a
night, one at a time)? *Recommendation:* approve the list; run the long
mode on request and before the owner's sign-offs, not nightly.

**Q6. When do the instruments get built?** The GPU does not affect
RW2, RW7, RW9, RW10, RW11 or RW8, and the owner parked "run well"
with the iGPU. *Recommendation:* start wave 1 (RW2, RW7, RW9, RW11) as
soon as agents are free, ahead of the iGPU: the init run's cap is a
live problem (RW11), a profiler is wanted by the GPU work too, and
RW4's software baseline must be taken before the GPU lands anyway.
RW1, RW3, RW4, RW6 can follow CB6; RW5 is the hardware night.

**Q7. Tests out of the boot image.** The shipped system then has no
`utest`, `ktest`, `soak` or `crash` in its shell unless the developer
entry is booted (5.2 item 3). *Recommendation: yes.* The other option,
the tests as a package (`pkg install jamos-tests`), cannot run the
tests that need to run before storage works. Also: a host-side build of
the pure-logic utest cases for `make check` is a larger follow-up;
*recommendation: later*, after RW11 shows how many of the fast cases it
would move.

**Q8. WR2 Q1's ceiling and Q3's progress display.** Q1: add the
absolute ceiling of ten answer times (3.1)? Q3: show the "writing N
MiB" notice at reboot and let `reboot -f` skip? *Recommendation: yes to
both.* The stall limit (10 s) and the ceiling (10 minutes) are numbers
to change at will.

**Q9. A "feels fast" budget with no Linux** (key to frame 2 frames,
window drags without a dropped frame): keep as Jam OS-only budgets
(1.1) or drop? *Recommendation: keep.* They need no second system
and catch the regressions the owner would feel first.

## 10. Size and estimate

| Part | Agent hours |
|---|---|
| Instruments: RW2, RW9, RW10 | 9 |
| Driver, Linux build, soak: RW1, RW3, RW6 | 9 |
| WR2's two questions: RW7, RW8 | 4 |
| Tooling: RW11 | 4.5 |
| First measurement and the bare-metal session: RW4, RW5 | 3.5 (+ about 3 of the owner's) |
| Review and join: RWR, RWJ | 2.5 |
| **Total** | **about 32** |

Four to five days with three or four agents at a time, the Linux
build's 3 hours and the long soak's 7 being PC time rather than agent
time. **Not in the figure:** the first round of fixes the numbers call
for. A reserve of 15 to 20 agent-hours (five to eight tracks of two to
three hours) is a guess until RW4 and RW9 report, and the work is
open-ended by nature: measure, find, fix, measure again.

**Risks.**

- **The debug port** (2.4). If the browser's DevTools port cannot be
  reached, the same driver cannot run on the bare PC and Linux, and
  RW3 turns into the in-guest relay or driver. Found in RW3's first
  half-hour, so it costs a pivot, not a wasted plan.
- **Linux parity.** If some Jam OS patch is not conditional, the Linux
  build is not the same code; RW1's check and, if needed, a second
  tree (12 GB, an hour to unpack) answer it. A mismatch in the
  config shows as a Linux that is slower than a stock one by more than
  PGO and LTO explain.
- **A noisy PC.** Measurement runs claim the whole CPU budget; the
  other agents' QEMU runs wait (`tools/remote-slots.py` drains), which
  slows the rest of the week by the hours a pair-set takes. The
  bare-metal session has no such cost.
- **The live internet.** Sites change, bot walls appear, the PC's
  address may be limited. The control run classifies them; the replay
  archive keeps the numbers out of it.
- **The sampler's overhead and safety** (RW9): a timer interrupt that
  walks user stacks is a fault in an interrupt; the walk must be
  fault-safe and bounded, and RWR reviews exactly that.
- **QEMU does not agree with bare metal.** Then QEMU tracks change
  only, and every level comes from the owner's session (1.3); that
  costs an evening per milestone.
