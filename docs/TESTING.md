# Testing Jam OS

How to test a change, cheapest first, with the exact commands. The rule
that a bug fix comes with a test is in
[CODING-GUIDE.md](../CODING-GUIDE.md#7-testing); results per milestone are in
[HISTORY.md](HISTORY.md).

## The tiers

| Tier | Command | When |
|---|---|---|
| Build | `make`, `make KTESTS=0`, `make check` | every commit |
| Kernel tests | `QEMU_SMP=4 tools/qemu-test.sh build/test kt ktest`, again with `QEMU_SMP=8` (the full run takes ~25-30 s) | every kernel change |
| User regression | `tools/init-test.sh build/test/init` (the init run: utest, usbtest, then posixtest, the POSIX layer's test, in three boots at once: ~45 s on the PC, [below](#the-init-run-in-parts)); before a push also `tools/qemu-test.sh build/test init init`, the same run in one boot (~130 s) | every change to syscalls, libos, services, drivers |
| Shell scripts | [below](#shell-scripts) | shell, console, input |
| Area scripts | [below](#area-scripts) | the area you touched |
| Soak | `JAMOS_TIER=push tools/soak-test.sh build/test`, again with `QEMU_SMP=8` (about 3 minutes each) | **before a push or a milestone sign-off only** (not in an agent's or a fix round's tests): after a change to a kernel test, a service or a driver, and anything that keeps state from one run to the next |
| libc-test (all) | `tools/libc-test.sh` ([below](#shell-scripts)) | before a push or a sign-off only; an agent runs it only for the libc or POSIX layer it changed, and then only the tests of that part |
| 2-minute soak | `soak 2` in the shell ([below](#soak)) | on the PC, before a push, when the full soak is more than the change needs |
| Sign-off | the boot menu's All tests, then `soak 10` in the shell with a second stick mounted read-write and pulled and replugged during the run | milestone sign-off only, on the PC |
| Stress | `QEMU_SMP=8 QEMU_TIMEOUT=200 tools/qemu-test.sh build/test st selftest stress=120` in QEMU; `stress <seconds>` in the shell | kernel work (scheduler, memory, locks), and when user space is broken: it needs none of it |
| The PC | flash the stick and run it ([HARDWARE.md](HARDWARE.md#flash-and-boot-the-stick)) | the final judge |

- **What an agent or a fix round runs (owner, 2026-10-12)**: the quick
  tier (build, `make check`, the `init` run) plus the area scripts for what
  it changed plus the kernel tests (kernel tracks at 4 and 8 CPUs; others
  once at 4). **Not** the soak and not the full libc-test: they run before a
  push or a milestone sign-off, once, on main, by the session that merges.
  A change that only a soak can show (a kernel test that keeps state, a new
  service that must survive load) says so in its report and the soak runs
  then.
- QEMU passing is necessary, not sufficient: TCG has no PCIDs and no
  TSC-deadline timer, and USB timing differs. Say what still needs the PC.
- On a busy machine (several QEMU runs at once) a timing failure may be
  load. Rerun once; a second failure is real.
- On the PC the 2-minute soak is skipped right before a sign-off: the
  10-minute run covers it. The soak replaced the stress test as the PC's
  tier on 2026-10-01: it runs the stress test's workers as its load, and
  adds the kernel tests, utest, file I/O and sticks coming and going.
- The tiers ask different questions. All tests: does each check hold once,
  on an idle machine, in the usual order? The stress test: do the
  scheduler, locks and allocators hold under load? The soak: does every
  test still hold the tenth time, in any order, next to load and user
  space, with sticks coming and going?
- **The tier is checked** (`JAMOS_TIER`, which `tools/remote-test.sh`
  forwards): `tools/soak-test.sh`, and `tools/qemu-test.sh` with the soak
  (`soak.txt`, the boot word `soak=`), refuse to run (exit 2) unless
  `JAMOS_TIER=push` (the pre-push pass) or `JAMOS_TIER=kernel` (a kernel
  change that needs it); `ktest` at `QEMU_SMP=8` prints a warning unless
  one of them is set.

## Running tests on the PC

Every QEMU test runs on the PC ([below](#another-machine-for-the-tests)).
More than one run: a batch, which copies the checkout and builds once,
starts every run at once (they queue on the PC's CPU budget) and ends with
one table.

```sh
cat > build/runs.txt <<'EOF'
init   tools/qemu-test.sh build/t/init init init
kt4    tools/qemu-test.sh build/t/kt4 kt4 ktest
cmds   QEMU_INPUT=tools/shell-tests/cmds.txt QEMU_EXTRA="-rtc base=2026-01-15T01:02:03" tools/qemu-test.sh build/t/cmds cmds shell
kd1    tools/kdump-test.sh build/t/kd-save save
kd2    tools/kdump-test.sh build/t/kd-notice notice
nvt1   env NVME_CASES="windows plain" tools/nvme-test.sh build/t/nvt1
EOF
tools/pc-batch.sh start build/runs.txt     # "batch 14203846: 6 runs started" at once
tools/pc-batch.sh wait 14203846            # again until it ends: at most 270 s a call
```

`wait` returns after at most 270 s (`PC_BATCH_WAIT`) with one progress
line (exit 124: not finished, call it again) or, at the end, the table
(exit 0: all passed, 1: one failed): each run's PASS or FAIL, its seconds
and the seconds it queued for the budget (its build's turn too, in a
detached batch), and for a failure its first
failure lines, its log (`build/batch/<id>/<name>.log`: remote-test's lines
and the command's) and its output directory. `report [<id>]` prints the
table again, `cancel [<id>]` stops a batch; `<id>` left out is the newest.
`start --rerun-failed` runs a failure once more: "rerun-PASS" if it passes
then (the known flaky tests, [Known noise](#known-noise)). A line is `<name>
<command...>`, shell words; `VAR=value` words before the command (with or
without `env`) are its environment. `tools/pc-batch.sh <runlist>` does it
all in the foreground (a terminal). If the PC doesn't answer, the batch
tries once more after a minute, then every run is NOT-RUN. The runs
start 0.3 s apart (`PC_BATCH_STAGGER`) and at most 32 are out at once
(`PC_BATCH_JOBS`; the rest show as QUEUED until one ends): each run opens
ssh connections and holds one while it waits for the budget, and the PC's
sshd turns some away while 10 or more are logging in (see [Another
machine](#another-machine-for-the-tests), "Dropped connections", for what
the tools do about it); a run that finds the PC gone before anything ran
(exit 3 and remote-test's "not run" line) is run once more after a minute,
a test's own exit 3 is not. A run that ends with exit 255 either could not
log in (the table says so) or lost its ssh connection after it started (it
may have gone on running there).

**Detached: the PC runs the batch alone** (`start --detach`). Use it when
the Mac may sleep before the batch ends (the owner closes the laptop for
the day) or the batch is long (the pre-push pass): without it every run is
an ssh session from the Mac for its whole length, and a sleeping Mac loses
every run in flight. The Mac copies the checkout (seconds awake: no
waiting for the build's turn in the budget), then hands the whole runlist
to the PC's keeper (`tools/pc-keeper.sh`, the Windows task that keeps WSL
jobs alive) as one job: `tools/pc-batch-driver.sh` runs each line there
under the same CPU budget and with exactly what remote-test would run (the
same build check, so the first run builds and the rest find it built, and
the same forwarded variables, `JAMOS_TIER` too), and writes each run's status,
times, log and a copy of its output directories (but disk images) into
`~/work/.batch/<checkout>/<id>/` on the PC. Once `wait` says "on the PC",
nothing on the Mac is needed until you collect.

```sh
tools/pc-batch.sh start --detach --rerun-failed build/runs.txt   # then close the lid
tools/pc-batch.sh wait 16140212     # hours later: the table, outputs copied back
```

`wait` and `status [<id>]` look at the PC with one short connection every
`PC_BATCH_POLL` (30) s (never a session held open); when the batch is over
they copy every run's output directory back to where the runlist named it
and print the same table. Collecting again (or a week later: the PC keeps
a batch 7 days) prints the table without asking the PC. `wait` exits 3 if
the PC never answered during the call (the batch goes on there; call it
later). `--rerun-failed` reruns on the PC. `cancel` stops every process
of the batch on the PC: TERM to the driver and its runs, whose
`remote-slots.py` governors end their commands and whatever still carries
their claim (as for any cancelled run: "A claim lives and dies with the
process that runs the command", below), then KILL to anything of the batch's
process tree left after 40 s; its claims on the budget go with the
governors, and the table shows its unfinished runs CANCELLED. A PC reboot mid-batch: the runs in flight show
as LOST (the driver's lock is gone and it never wrote done), the ones not
started NOT-RUN; `tools/pc-batch.sh start --detach --resume <id>` runs
them again on the PC's copy as it is. Logs are under
`build/batch/<id>/pc/runs/`. One detached batch at a time per checkout
(`start --detach` refuses a second while the first runs there): don't run
other PC tests from the checkout until it's over, they would change the
copy it tests. Batches from different checkouts don't collide (each has
its own directory and copy). Without a keeper on the PC the hand-off fails
and every run is NOT-RUN ("no keeper runs on the PC").

One run: `JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh
tools/qemu-test.sh build/t/init init init`. Several calls of your own from
one checkout: the first copies and builds, the rest can skip the copy with
`JAMOS_REMOTE_NOSYNC=1` (`tools/remote-test.sh --prepare` copies and builds
and runs nothing). A build with nothing to do (the tree the PC built last
for this checkout) is skipped and claims no CPUs.

## Build checks

- `make` builds with `-Werror`; `make KTESTS=0` builds a kernel without
  the in-kernel tests, benchmark and test hooks (into `build/noktests/`).
- `make check`: the generated syscall and IDL code matches `abi/`
  (`tools/gensyscalls.py check`, `tools/genidl.py check`), the generated
  Wayland code matches the vendored XML (`tools/genwl.py check`) and the
  Wayland generator passes its own tests (`tools/genwl.py selftest`:
  tools/genwl-tests/, built and run with the Mac's compiler); the IDL
  generator passes its own (`tools/genidl.py selftest`: what it must
  refuse, and a stale output failing `check`); the keymap tables match
  their generator (`tools/genkeymap.py check`, `selftest`); the driver
  isolation check still rejects what it must (`tools/checkdriver-selftest.sh`
  over `tools/checkdriver-tests/`); the docs match the tree
  (`tools/checkdocs.py`: links, anchors, and the repo paths, file names,
  headers and make targets named in backticks); no audio file is tracked
  (`tools/checkaudio.sh`); and the RTL8125 driver transmits only through
  `drivers/rtl8125/tx.c`, behind its gate (`tools/checknotx.sh`: registers
  are written only in `regs.c`, each write through the guard in
  `drivers/rtl8125/notx.h` that refuses every transmit register, and in
  `tx.c`, each write behind the gate; no `regs.c` write names a transmit
  register, the other transmit queues' ring addresses and tail pointers
  included; the MAC OCP registers of the descriptor format and the queue
  count are written only in `chip.c`'s bring-up; the transmitter enable
  bit is named only in `notx.h` and `tx.c`; every function `tx.c` gives
  other files starts with the gate, which is "full mode and a configured
  network mode" (a VLAN, or untagged); the probe's files call nothing of
  `tx.c`; the mode and the network mode are set once, in `main.c`; the registers'
  mapping is named only in `regs.c`, `tx.c` and `main.c`, the transmit
  ring and buffers only in `tx.c`, `ring.c` and the header. It checks each
  rule against `tools/checknotx-tests/` first, counting its offences
  there); the network test peer and the pcap check pass their own
  self-tests on the host (`tools/netpeer.py --selftest`,
  `tools/pcap-vlan-check.py --selftest`); the shell scripts' failure
  lines pass theirs (`tools/serial-feed.py --selftest`: the lines a
  failed test prints, and the lines that must not count);
  `tools/checkwants.py
  --selftest`: the program-list check takes `svc net listen` and refuses
  `svc net-listen`, a listen on another service and a list over its 24
  wants; the FAT32 checker finds each kind of damage it knows in volumes
  it makes and damages (`tools/fatcheck.py --selftest`); the update signing tool passes Monocypher's Ed25519 vectors
  and keeps the ports formats' signatures apart by their first lines
  (`build/host/jamos-sign self-test`); the package formats' test vectors
  are what the packager makes and each gets its answer from the
  reference verifier (`tools/ports.py vectors`), the C readers Jam OS
  uses for those formats (`user/lib/pkg*.c`) give every vector, and
  every text and archive the reference was asked about, the same answer,
  with 10,000 mutations of each kind, under ASan and UBSan
  (`build/host/pkgcheck`; utest's `pkgfmt_*` run the same checks on Jam
  OS), and the repository
  server answers only files of the repository, each a whole snapshot
  (`tools/ports-server.py --self-test`); libfun's smooth text holds up
  against hostile input under ASan and UBSan (`build/host/fontcheck
  --check`, [below](#smooth-text)); and the partition table library
  (`drivers/lib/parttable.c`, `drivers/lib/parttable_gpt.c`,
  `drivers/lib/parttable_make.c`) keeps to the
  driver rules and, under ASan and UBSan, gives what `tools/mkgpt.py
  --cases` says on each of its images, reads the Jam OS table it makes
  as Jam OS's (and as blank-in-progress after each of `create_table`'s
  writes before the last), refuses each GPT header rule
  broken alone, and lists nothing outside the disk on thousands of
  mutated tables and a short fuzz pass (`build/host/gptcheck`); usb-bus's
  root port handling (`drivers/usb-bus/rootport.c`) leaves no port change
  unseen under the controller's event rule (xHCI 4.19.2: an event only
  when a port's change bits go from none to some), which QEMU doesn't
  model: a stick pushed back in, or a connector bouncing, at every point
  of a port's handling (`build/host/rootportcheck`); the kernel's AVL
  tree and the interval tree on it (`kernel/lib/avl.c`,
  `kernel/lib/itree.c`: an address space's mappings, a VMO's reverse map)
  keep their links, balance and summaries through thousands of random
  inserts, erases and moves under ASan and UBSan, and every interval
  search returns exactly the ranges a one-by-one search of an array does
  (`build/host/avlcheck`, `build/host/itreecheck`); the ext library
  (`user/services/extfs/ext/`, the format code of M13.6's Unix
  filesystem) builds as Jam OS builds a program and, under ASan and
  UBSan, reads the volumes Linux's mke2fs made (`tools/extfix/`) exactly
  as Linux's debugfs listed them, refuses each structure's checksum
  broken alone, sorts every feature bit as the plan's table says, and
  neither crashes nor reads outside the image on thousands of mutated
  volumes (`build/host/extcheck`), finds each way a volume can be spoilt
  (its checker, which `jamext check` and the shell's `disk check` also
  run), and the host tool jamext makes images from a directory that
  extcheck checks and that come back byte for byte
  (`tools/jamext-test.sh`).

## Running QEMU: tools/qemu-test.sh

```sh
tools/qemu-test.sh <outdir> <name> [kernel command line...]
```

**Build the image first.** Plain `make` does not rebuild `build/jamos.img`,
and the scripts boot whatever image is there, so run `make -s image` (and
`make -s KTESTS=0 image` for `QEMU_IMAGE=build/noktests/jamos.img`) before
any QEMU test: a stale image fails tests that the new code would pass.

Copies `build/jamos.img`, sets the boot entry's command line, boots it
headless (q35, OVMF, the stick on qemu-xhci port 1 as the device `stick`, the `edu` test device,
a `virtio-rng` at 00:02.0 that no driver binds: a spare MSI-X function for
utest's `driver_handle_limits`; **no network card** (`-nic none`) unless
`QEMU_NET` asks for one), waits until the kernel halts or idles, and leaves `<outdir>/<name>.log`
(serial) and `<name>.png` (the screen; needs Pillow). Environment:

| Variable | Default | What |
|---|---|---|
| `QEMU_SMP` | 4 | CPUs |
| `QEMU_MEM` | 2G | memory |
| `QEMU_CPU` | max | CPU model, e.g. `max,-x2apic` |
| `QEMU_TIMEOUT` | 150 (300 with the boot word `init`: the one-boot init run takes about 130 s, the kernel's `init_timeout`) | seconds before giving up (only a cap: a run ends when the kernel halts or QEMU goes) |
| `QEMU_IMAGE` | build/jamos.img | another image, e.g. build/noktests/jamos.img |
| `QEMU_XHCI` | | qemu-xhci properties, e.g. `msi=on,msix=off` (MSI-only, like the PC) |
| `QEMU_USB` | | more USB devices; give each a `port=` (port 1 is the stick) |
| `QEMU_EXTRA` | | more QEMU arguments, e.g. `-rtc base=2026-01-15T01:02:03` |
| `QEMU_INPUT` | | a script typed into the serial port by `tools/serial-feed.py` (its header has the commands: `wait`, `seen`, `send`, `type`, `sleep`, `shot`, `monitor`, `usbkeys`, `pointer`; in a `wait` or `seen`, `{prompt}` is the shell's prompt, `jam:<cwd>>` with its colours, in whatever directory: `wait {prompt}`); the run passes if every `wait` matched, QEMU ended by itself and the guest printed no failed-test line ([how a script's verdict is reached](#how-a-scripts-verdict-is-reached)) |
| `QEMU_MONITOR` | | a script of `expect` / `send` / `sleep` lines run against the QEMU monitor |
| `QEMU_SAVE` | | a file to keep the run's stick image in, with what the guest wrote: a later run's `QEMU_IMAGE` boots the same stick again |
| `QEMU_BOOT_PREV` | 0 | 1: boot the stick's previous build (`/esp/boot/prev-jamos.elf` and `prev-bootfs.img`, the boot menu's "Jam OS (previous build)") instead of its own |
| `QEMU_MENU_AT` | | `efi`: the run's one-entry menu goes to `/esp/EFI/BOOT/limine.conf` (Limine 11 reads it before `/esp/boot/limine/limine.conf`), so the stick keeps its own boot menu (`tools/update-menu-test.sh`: `update` writes that one); `keep`: no menu is written, Limine reads the image's own (an installed drive's, as `install` wrote it: `tools/install-test.sh`; the kernel's command line is the one in that menu) |
| `QEMU_STICK_THROTTLE` | | the stick as slow as a real one: QEMU `throttle-group` limits, e.g. `x-iops-write=100,x-bps-write=10485760` (every write command waits its turn, 100 a second, whatever its size, as on a cheap USB 2 stick) |
| `QEMU_SPLASH` | 0 | 1: keep the boot splash (otherwise the boot word `nosplash` is added, so the tests see the text log) |
| `QEMU_NET` | | `1`: a network card and the test peer ([below](#the-network-peer)); `<peer port>:<qemu port>`: the same card and pcap, with a peer you run yourself |
| `QEMU_NET_VLAN` | 21 | the VLAN the peer and the pcap check want every frame tagged with, or `none`: every frame untagged and none tagged. The guest boots with the same `vlan=` word unless its command line has one or `QEMU_NET_WORD=0`, so a network test doesn't depend on the build's default |
| `QEMU_NET_WORD` | 1 | 0: don't add the `vlan=` word (a run of the build's default; set `QEMU_NET_VLAN` to it) |
| `QEMU_NET_NONE` | 0 | 1: no frame at all may leave the guest (the `vlan=off` run) |
| `QEMU_NET_PEER` | | more flags for `tools/netpeer.py`, e.g. `--noise 2` |
| `QEMU_IOMMU` | | QEMU's VT-d unit (`intel-iommu`, interrupt remapping offered; the guest starts it unless the boot word `iommu=off` says not to): `1` caching mode on (new mappings need invalidating too), `cm0` caching mode off (the PC's case), `eim` caching mode on with x2APIC destination ids |
| `QEMU_NVME` | | emulated NVMe drives from image files, opened writable: a space-separated list, one `-device nvme` each, `img[+img...][,prop=val...]` (one `nvme-ns` per image, the props go to each: `logical_block_size=4096,physical_block_size=4096`); `QEMU_NVME_CTRL` adds controller properties (`mdts=2`). `tools/nvme-test.sh` uses it |
| `QEMU_BOOT_NVME` | 0 | 1: boot from an installed drive instead of a stick: the image, made Jam OS's GPT layout if it is a stick's (`tools/mkimage.py --gpt`: its ESP and data volumes in "Jam OS ESP" and "Jam OS data", a random disk GUID), is an NVMe drive (`bootindex=0`, before `QEMU_NVME`'s drives) and nothing is on xhci port 1; `QEMU_STICK_THROTTLE`, `QEMU_SAVE` and `QEMU_MENU_AT` work on it as on the stick, and a GPT image is booted as it is. `tools/bootdisk-test.sh` uses it; in an `init` run with it utest's `nvme_*` cases skip the boot drive (a Jam OS drive allows the writable opens they expect refused) and say so |
| `QEMU_WORDS` | | more boot words after the script's own, e.g. `QEMU_IOMMU=eim QEMU_WORDS=iommu=on tools/usb-test.sh ...`: an area test with DMA translation and interrupt remapping on |

Examples:

```sh
tools/qemu-test.sh build/test kt ktest                    # every kernel test
tools/qemu-test.sh build/test chan ktest=chan             # tests whose name starts "chan"
QEMU_SMP=8 QEMU_TIMEOUT=60 tools/qemu-test.sh build/test st selftest stress=30
QEMU_XHCI=msi=on,msix=off tools/qemu-test.sh build/test msi init
make run                                                  # interactive: the shell, a USB keyboard, serial on stdio
make debug                                                # the same, stopped for gdb on :1234
```

### The init run in parts

```sh
tools/init-test.sh <outdir> [name] [kernel words...]
tools/init-test.sh --check     # the parts against the whole run, no QEMU (make check)
```

The `init` run (`tools/qemu-test.sh <outdir> <name> init`) runs
`boot/init.cfg`'s programs one after another in one boot: about 130 s on
the PC (2026-10-14: utest 118 s, posixtest 12 s). Most of utest's time is
cases waiting out the timers they test (keep-alive probes, a notice's 5 s,
keys kept 5 s, init's and libjwl's deadlines, logd's syncs): its 20 slowest
cases take 78 s, the other 640 about 40 s. `tools/init-test.sh` runs the
same programs split over `boot/init.cfg`'s `part <k>` lines, a boot each
with the kernel word `initpart=<k>`, all at once: about 45 s.

Each boot is a whole `init` run of its part: devmgr, volumes and their
boot-time checks, the RESULTS box, init's root job checked clean. The parts
are checked to add up to the whole run: before the boots, every program but
utest is in exactly one part with its arguments, and utest's cases are split
by `utest only <prefixes>` lines and one `utest except <prefixes>` whose
list is every `only` prefix, none the start of another (so no case runs
twice and none is left out); after them, the parts' `utest: running <n> of
<m> tests` lines add up to m. `<outdir>/<name>.log` (name `init` by default)
has each part's RESULTS box, its verdict said as `part <k> complete: no
problems` or `part <k> FINISHED WITH PROBLEMS`, and then the run's one
verdict, as a one-boot log ends: `run complete: no problems` or `run
FINISHED WITH PROBLEMS` (exit status 0 only for the first); part k's serial
log is `<name>-<k>.log`.

Boots, not cases at once in one boot: many cases restart or count shared
things (devmgr's drivers, volumes and `/data`, the console, the job's and
the process's counts, the clock), and boots share nothing, so a part's
cases run as in the whole run, in the table's order, only without the other
parts' cases before them. That is what the parts can't catch: a case broken
by another part's case that comes before it in one boot; so before a push
the one-boot run runs too. A case that counts the process's handles stays
behind the cases that make a handle it would otherwise make first
(`jwlc_setup` behind the `jwl_` cases). The split is by time, about 40 s a
part; a new case lands in the `except` part, and when a part passes about
60 s a prefix moves to an `only` line (and the `except` list). On the PC
(`tools/remote-test.sh`) the run costs a QEMU per part of the CPU budget, for
a third of the time: 12 CPUs at the default `QEMU_SMP=4`, but 24 of the 26
at `QEMU_SMP=8`, which waits for an almost idle PC (and holds back the
runs queued behind it): at 8 CPUs run the one-boot run instead.
`INIT_TEST_JOBS=n` runs at most n parts at once, in rounds (the same
boots; 8 CPUs for 2, about 90 s): `remote-test.sh` sets it itself when a
long claim, Chromium's build, leaves the PC too little for all three
([the CPU budget](#another-machine-for-the-tests), `--shrink`).

### Another machine for the tests

QEMU on an Apple Silicon Mac emulates x86 (TCG): slow, and a few
QEMUs at once fill it. An x86 PC runs the same tests under KVM, many at
once: the full kernel test in about 25 s, the whole regression in
minutes. `tools/remote-test.sh` runs any test command there:

```sh
JAMOS_REMOTE=jamos-pc tools/remote-test.sh tools/init-test.sh build/test/init
JAMOS_REMOTE=jamos-pc tools/remote-test.sh tools/qemu-test.sh build/test init init
JAMOS_REMOTE=jamos-pc QEMU_SMP=8 tools/remote-test.sh tools/qemu-test.sh build/test kt8 ktest
JAMOS_REMOTE=jamos-pc tools/remote-test.sh tools/mixer-test.sh build/test/mixer
```

It copies the checkout (no `.git`, no `build/`) to `~/work/<checkout's
name>` there, runs `make image` (so the image is never stale; skipped, with no CPUs claimed, when the PC built this very tree for the checkout last: a stamp of every file's name, size and time, `JAMOS_REMOTE_REBUILD=1` builds anyway), runs the
command with the `QEMU_*`, `KTESTS`, `JAMOS_*`, `PC_RUNS`, `*_CASES` (`PAGER_CASES`, `MMAP_CASES`), `SOAK_*` and `PORTS_TEST_*` variables, and copies
back the output directories its arguments name under `build/` (but stick
images); its exit status is the command's. Commands from every checkout
share a budget of CPUs on the machine (below) and wait until theirs fit:
more runs at once made timing tests fail. Without `JAMOS_REMOTE`, or
when the machine doesn't answer (5 s a try, 3 tries), the command runs
here (with `JAMOS_REMOTE_ONLY=1`: not run, exit 3).

**Dropped connections** (`tools/ssh-retry.sh`, `remote-test.sh`, `pc-batch.sh`).
The PC's sshd turns new connections away at random while 10 or more are
still logging in (`MaxStartups`, more of them as there are more) and,
loaded, answers too late: a batch of 16 runs at once lost some with
`kex_exchange_identification: read: Connection reset by peer` or
`Connection timed out during banner exchange` (exit 255 within seconds:
"the PC didn't answer", or the run failed with 255), a batch of 163
lost 128. Four things, in the order a connection meets them:
- **Few logins at once.** Every ssh and rsync of `remote-test.sh` goes
  through `tools/ssh-retry.sh`, which takes one of 5 login slots
  (`SSH_RETRY_LOGINS`; machine-wide on the Mac, every agent shares them) before it
  connects and frees it the moment ssh is logged in, so a session that lasts
  hours holds none. `pc-batch.sh` starts its runs `PC_BATCH_STAGGER` (0.3) s
  apart and keeps at most `PC_BATCH_JOBS` (32) out at once; the rest wait
  in the batch (table: QUEUED), not as ssh sessions and waiting processes on the PC.
- **A login that fails is tried again.** When ssh exits 255 **before the
  login completed** (ssh runs `touch <marker>` as soon as it is logged in,
  before the remote command starts) and the message is a cut or a refusal:
  6 tries for a reset, a close or a banner timeout, 3 for "refused" and "timed out"
  (a PC that is off is found out in about 20 s), waiting 1, 2, 4, 8 s with
  up to half as much again at random. "Permission denied" and a changed host
  key are not retried. Each repeat prints `ssh-retry: ...: try 2 of 6 in 1.4 s`
  into the run's log and a late success `connected on try 3`; the end of a
  login that never worked is `ssh-retry: not logged in: ...`. `SSH_RETRY_TRIES`,
  `SSH_RETRY_DOWN_TRIES`, `SSH_RETRY_DELAY_MS` change it.
- **A run that found no PC** (exit 3 and remote-test's "not run" line, after
  those tries) is run once more by `pc-batch.sh` after a minute; a test's own
  exit 3 is not.
- **A session lost while the run waits for the budget.** The session that
  runs the command has keepalives (`ServerAliveInterval=20`, a dead
  link ends it in ~3 min). Hours in the queue of the pre-push pass ended 32 runs
  with `Read from remote host: Operation timed out`. If the connection is lost
  after the login (`ssh-retry: connection lost after login`) and
  the command has not started (no `remote-test: running with` line yet),
  `remote-test.sh` starts the session again, up to `JAMOS_REMOTE_REQUEUE`
  (3) times, 5 to 15 s apart; the new session first stops the lost one's
  leftovers on the PC (a pid file `~/work/.runs/<run id>`). Once the command
  has started nothing is repeated: a test's own failure (even exit 255)
  never reruns, and a connection lost mid-run is exit 255 ("the ssh
  connection was lost after the run started" in `pc-batch.sh`'s table; the
  run may have gone on there).

No ssh multiplexing (ControlMaster): the PC allows 10 sessions per
connection (`MaxSessions`), a batch holds many long ones, and a master
outliving a PC reboot leaves stale sockets. If losses still happen, the
fix on the PC is `MaxStartups 64:30:128` and `MaxSessions 64` in Windows'
`sshd_config` then `Restart-Service sshd` (as administrator): the owner's to
do; nothing here edits the PC. Host tests (no PC, seconds):
`tools/ssh-retry-test.sh`, `tools/remote-test-test.sh`.

**The CPU budget** (`tools/remote-slots.py`, run by `remote-test.sh` on the
machine). The PC has 28 CPUs; the old six equal slots left most of them
idle while a queue of one-CPU runs waited. Now a command costs what it can
use and starts when that fits beside what is running:

| Command | Costs |
|---|---|
| a QEMU run | `QEMU_SMP` CPUs (4 if unset, as `qemu-test.sh`) and `QEMU_MEM` of memory (2 GB if unset); `QEMU_SMP=8 ... kt8` is 8 |
| a script that boots several at once | the total: `nvme-driver-test.sh` one boot for each case (6 by default, so 24 CPUs), `init-test.sh` one for each part of `boot/init.cfg` (3, so 12 CPUs) or `INIT_TEST_JOBS` at a time if fewer, `ports-test.sh` `PORTS_TEST_JOBS` (3) at a time or the ports given if fewer; `wl-port-test.sh` `WL_PORT_JOBS` (3) groups at a time or the groups given if fewer (these three also take fewer parts at once by themselves when a long claim leaves too little: `--shrink`, below); `update-menu-test.sh` boots `-smp 2` |
| the build before the command | `JAMOS_REMOTE_BUILD_CPUS` (8) CPUs and `make -j` that many: a build is a short burst, but a burst of 16 jobs beside 26 CPUs of QEMU failed timing tests, and waiting for a whole test to end before building would stall everything, so it queues like a small test (a checkout's builds still take turns; one with nothing to do, the tree built last, is skipped and claims nothing) |
| anything else (`make check`, a checker, a port build) | `JAMOS_REMOTE_LIGHT_CPUS` (2) CPUs and `JAMOS_REMOTE_LIGHT_MEM_MB` (1024): say more for a heavy one, `JAMOS_REMOTE_LIGHT_CPUS=12 tools/remote-test.sh ...` |

The budget is `JAMOS_REMOTE_CPUS` (26) CPUs, `JAMOS_REMOTE_MEM_GB` (20) of
memory and `JAMOS_REMOTE_SLOTS` (16) commands. A command that costs more
than the whole budget runs alone, when nothing else runs. A command that
does not fit prints `waiting: 24 of 26 CPUs in use by 6 run(s) (this run
needs 4 CPUs, 2.0 GB)` (again every 2 minutes) and `running with 24 of 26
CPUs in use (after 41 s)` when it starts. A small command may start ahead
of an older one that doesn't fit yet, but once the older has waited 60 s
(`JAMOS_REMOTE_AGE`) nothing starts ahead of it, so a big run is not
starved by a stream of small ones (only if starting first would keep the
older one from fitting: an older run that would still fit after the younger
started is not waited for, so one stuck on something other than the budget,
an older copy of the script on the PC, say, holds no one up).
**Except a run that cannot fit however
long the others take**: a long claim (below) holds part of the budget for
hours, and a run whose cost plus the long claims' is over the CPUs, the
memory or the slots (the init run's 12 CPUs beside the Chromium build's 16)
used to hold every younger run behind it for as long (the queue stood still
for 3.5 hours on 2026-10-15). Now it is told at once (`waiting: this run
needs 12 CPUs, 6.0 GB, but at most 10 CPUs, 8.0 GB can be free while the
long claim holds 16 CPUs, ...: it starts when that ends and the runs that
fit go ahead of it`), does not count in the rule above, and starts when the
long claim ends. For a run that is made of parts, no waiting at all: see
`--shrink` below.

**Fewer QEMUs for a while** (the owner is training on the PC, say): one
variable, `JAMOS_REMOTE_CPUS=12` in the environment of whoever launches the
tests; or, for everyone at once, including commands already waiting, write
the number into `~/work/.cpu-budget` on the PC (`12`, or `12 10 6` for CPUs,
GB and commands; delete the file to go back). Running commands are never
stopped.

**A long claim** (Chromium's build, hours: [CR-PLAN](CR-PLAN.md#15-the-build-on-the-pc)):
`remote-slots.py --detach <log> <cpus> <mem_mb> <command>` waits for its
turn like any command, then starts the command in the background (a
governor process of its own, in a session of its own, with the command's
output in `<log>`), prints `started <pid>` and returns, so the ssh session
that asked can end. The command holds its CPUs and memory for as long as it
runs; the tests share what is left (with the build's default claim of 10
CPUs and 12 GB, 16 CPUs and 8 GB), so they shrink rather than overcommit
the machine's memory. `tools/cr/build.sh start` is its user.

**`--shrink`: a run made of parts takes fewer while a long claim runs.**
`remote-test.sh` gives `remote-slots.py` how to run fewer parts at once
(`--shrink INIT_TEST_JOBS:4:2048:3`: three parts of 4 CPUs and 2048 MB each,
the variable that says how many run at once) for `init-test.sh`
(`INIT_TEST_JOBS`: its three boots, in rounds of that many),
`ports-test.sh` (`PORTS_TEST_JOBS`) and `wl-port-test.sh`
(`WL_PORT_JOBS`). When all parts would never fit beside the long claims,
the claim is for as many as do (at least one), the command starts with the
variable set, and the run says so (`takes INIT_TEST_JOBS=2 part(s) at a
time (8 CPUs, 4.0 GB) so that it fits`). The same boots run, in two rounds
instead of one (the init run about 90 s instead of 45); a run that fits is
not changed. Set the variable yourself (`INIT_TEST_JOBS=1`, in the runlist
line) to ask for fewer parts at once.

**A claim lives and dies with the process that runs the command (the
"governor"), nothing else.** Each running command has a file in
`~/work/.cpu/run/` with an `flock` held by its governor (`remote-slots.py`)
alone, not by what the command starts. The command runs in a session of its
own with `JAMOS_CLAIM=<id>` in its environment, which everything it starts
inherits, a QEMU or a serial feeder that made a session of its own too.
- The command ends: the governor kills every process still carrying the id
  (`the command left 2 process(es) running: killed them`), then ends the
  claim. A QEMU hung by a cancelled run can no longer keep its CPUs for
  hours (as on 2026-10-15/16, when an `flock` that every leftover process
  shared did).
- A cancel (`pc-batch.sh cancel`, ^C: `remote-test.sh` traps SIGTERM, SIGINT
  and SIGHUP, ends its ssh and asks the PC, in one short ssh, to SIGTERM the
  governor named in `~/work/.runs/<run id>`) reaches the command's whole
  process group; what is still there after `JAMOS_REMOTE_KILL_GRACE` (10) s
  is killed.
- A governor that dies without a word (`kill -9`, the PC's WSL ending it)
  leaves a file nobody holds: the next caller kills the processes carrying
  its id (`the run of pid 123 (...) is gone: killed the 2 process(es) it
  left`) and removes the file. A waiting run whose client is gone, when its
  stderr is closed, gives up its place at its next "waiting" line (2
  minutes).
- A process that clears its environment escapes the sweep (it keeps only
  itself). A governor that lives on although its client is gone (an ssh
  session the PC's sshd still thinks open after the Mac slept) is not
  noticed: setting `ClientAliveInterval 30` and `ClientAliveCountMax 4` in
  the PC's `sshd_config` makes sshd end such sessions in two minutes.

`python3 tools/remote-slots.py --status` (on the PC, in a checkout's
directory) lists what runs and waits with each governor's pid, age and
command (`long` marks a long claim, `GONE` a claim whose governor died). The
host test `tools/remote-slots-test.sh` (a fake lock directory, about 55 s,
no PC) checks the accounting, the oversized run, the crashed and cancelled
governors and what they leave, the deadlock (a 16-CPU long claim, a 12-CPU
waiter, 4-CPU runs that must start) beside the fairness rule for a run that
does fit, `--shrink`, the costs and the long claim.

The machine: Windows with its OpenSSH server (a key login), and WSL2's
Ubuntu 24.04 behind `wsl` with:
- KVM: `/dev/kvm` in group `kvm` (WSL's `/etc/wsl.conf`: `[boot]
  command = "chgrp kvm /dev/kvm && chmod 660 /dev/kvm && sysctl -w
  net.core.rmem_max=8388608"`, the last for the network peer's bursts);
- the Mac's versions of the tools: QEMU 10.0.2 in `~/qemu-10`, and
  x86_64-elf binutils 2.45.1 and gcc 15.2 in `~/cross` (from source);
- mtools 4.0.49 built against GNU libiconv 1.17 (`--enable-extra-encodings`,
  for code page 437) in `~/mtools` and `~/libiconv`: glibc's iconv turns
  "Ÿ" into "Y", and mtools then keeps no long name;
- OVMF, ffmpeg, python3 with numpy and PIL, xkbcli
  (libxkbcommon-tools), the `en_US.UTF-8` locale;
- `/bin/sh` as bash, as the Mac's: the scripts are written for it.
- for the ports builder (`tools/ports.py`, [M13.7](M13.7-PLAN.md)):
  `pkgconf`, `bison` 3.8 and `libexpat1-dev` from apt, and Meson and
  Ninja in a Python venv in the user's home, **`~/.cache/jamos/tools`**
  (`python3 -m venv ~/.cache/jamos/tools && ~/.cache/jamos/tools/bin/pip
  install meson ninja`; 2026-10-11: Meson 1.12.1): Ubuntu 24.04's own
  Meson is 1.3.2, and xkbcommon 1.13 asks for 1.4. Nothing is installed
  system-wide for it; `tools/ports.py` puts that directory's `bin/` (or
  `$JAMOS_TOOLS/bin`) first on PATH. The `host` lines of a recipe and the
  builder's own message name what a machine lacks. The same directory
  holds what Chromium's build needs and `tools/cr/toolchain.sh` puts there
  ([CR-PLAN](CR-PLAN.md#ct2-as-built-2026-10-14)): CMake (a wheel) and
  gperf 3.1, built from its pinned source, never the system's.

On the Mac the same builder runs with what Homebrew has (`cmake`, `ninja`,
`pkgconf`) plus the same venv (or `$JAMOS_TOOLS`) for Meson, and a GNU
`bison` 3.6 or newer for xkbcommon (the system's is 2.3: build one into
`$JAMOS_TOOLS/bin` rather than installing it); the Xcode SDK has the expat
that wayland-scanner's host build links. A Homebrew `binutils` early on
PATH is fine for the cross builds, and the host builds put Apple's `/usr/bin`
first (GNU `ar` writes archives Apple's linker can't read).

`tools/qemu-host.sh` holds what differs between the hosts (the firmware,
KVM or TCG, the split irqchip KVM's interrupt remapping needs). A test
that only fails on the faster host is usually a test that assumed an
order: a line the fast boot prints before the shell's prompt wants a
`seen`, not a `wait`. The flash test is macOS only (`hdiutil`), and the
storage test runs emulated there too (its comment says why).

### The network peer

With `QEMU_NET=1` the machine gets QEMU's e1000e (8086:10d3, its option
ROM left out so the firmware never sends on it) on a `-netdev dgram`:
each Ethernet frame the guest sends is one UDP datagram on 127.0.0.1 to
`tools/netpeer.py`, and each datagram the peer sends back is a frame the
guest receives. `tools/qemu-test.sh` picks two free UDP ports
(`netpeer.py --free-ports 2`, so agents can test at once), starts the
peer, and stops it when QEMU ends. QEMU's user networking (slirp) is not
used: it doesn't speak 802.1Q, and the tests must not need the internet.

The peer checks the rule ([ARCHITECTURE](../ARCHITECTURE.md#networking)):
every frame from the guest must be tagged 802.1Q with the VLAN; an
untagged, priority-tagged (VLAN 0), other-VLAN, QinQ, runt or over-long
frame is logged in hex and fails the run. With `--vlan none` (the
untagged mode) it is the other way round: every frame must be untagged
(14..1514 bytes), and any tagged one, VLAN 0 and VLAN 21 too, fails the
run; then everything below happens untagged, and the noise and the
flood's frames to drop are tagged ones (VLAN 21 among them). It strips
the tag and answers as a small network, tagging every reply: ARP for any IPv4 address (not
probes from 0.0.0.0 nor gratuitous ARPs), ICMP echo for any address (so
`ping 1.1.1.1` works in QEMU), and UDP on ports that have a handler
(`Peer.add_udp`: where DHCP, DNS, netlog and the update server hook in;
`--netlog <folder>` answers port 5021 with `tools/netlog-recv.py`'s
receiver, `--netlog-late <s>` and `--netlog-pause <bytes>:<s>` make it
start late or pause, counted as `netlog_in` and `netlog_dropped`;
`--update <spec.json>` answers port 5022 with `tools/update-server.py`'s
`PlannedServer`, as `tools/update-net-test.sh` does).
`--ping <address>` sends an ICMP echo request to the guest every half
second (from 10.2.21.174, once it has seen the guest's MAC) and counts
the replies (`ping_replies`); `--ping-every <s>` changes the half
second, and `--late-after <s>` also counts the pings sent s seconds or
more after the first and their replies (`pings_late`,
`ping_replies_late`). `--noise <s>` sends, every s seconds, four frames the guest's driver must
drop (untagged, VLAN 10, a priority tag, QinQ) and one it must pass (a
broadcast ARP request on the VLAN). `--flood <n>` (with `--ping`) sends
n frames a second, the mix a busy trunk port carries: on VLAN 21 ARP
requests for other hosts and for the guest, broadcast and multicast
datagrams, UDP to the guest's closed ports, IPv4 to the guest's MAC for
another address, echo requests from another host, frames for another MAC,
IPv6; off it untagged, VLAN 10 and 20, priority-tagged and QinQ frames
(`flood_sent`, `flood_vlan`). `--tcp-serve <port>:<bytes>[:<ms>],...` and
`--tcp-connect <addr>:<port>:<conns>:<bytes>` add a small TCP of the
peer's own (`tools/tcppeer.py`: in-order receiving, every segment acked,
at once or `<ms>` later as over a link with that round trip, a 64 KiB
window or, with window scaling (offered on its SYNs, answered when the
guest offers), 4 MiB; at most 256 KiB in flight, go-back-N after 0.25 s
without an ACK, zero-window probes, FIN both ways): servers the guest
connects to (each reads the guest's bytes to its FIN, then sends as many
and its FIN) and clients that connect to a guest listener (each sends its
bytes and FIN, then reads the guest's to its FIN), every byte checked
against bin/tcptest's patterns (`tcp_ok`, `tcp_bad`, `tcp_bytes_in`,
`tcp_bytes_out`, `tcp_retransmits`, and for each server port P the most
seen: `tcp_P_window` the guest's window, `tcp_P_in_flight` its bytes past
our last ACK, `tcp_P_out_flight` ours it had not acked, `tcp_P_scaled`;
a connection not done is described in the log). The relay
(`tools/tcprelay.py`) scales the same way and counts `relay_scaled`,
`relay_window`, `relay_in_flight` and `relay_out_flight`. Its
own test, a server and a client of it over a lossy wire, is part of
`--selftest`. Its log is `<outdir>/<name>.peer.log`, its counts
`<name>.peer.json`.

A relay (`tools/tcprelay.py`) lets real programs on the Mac talk to the
guest as if the Mac were on the VLAN: `--tcp-relay <port>:<hostport>`
takes the guest's connections to `<port>` of any address (10.2.21.174 in
the tests) and joins each to a connection to 127.0.0.1:`<hostport>` (a
Python web server, `tools/speed.py`); `--tcp-forward
<lport>:<addr>:<port>[:<loss>]` listens on 127.0.0.1:`<lport>` and joins each
connection made there (curl, `tools/speed.py client`) to one from
10.2.21.174 to the guest's `<addr>:<port>`, dropping `<loss>` percent of
the segments with bytes it sends the guest (a lossy link; a fixed seed);
`--udp-relay <port>:<hostport>` passes datagrams both ways the same way.
Each takes several, comma-separated. Its TCP receives in order (its
window the room left for the Mac's socket) and sends as the Mac would:
fast retransmit on three duplicate ACKs; with SACK (offered on its SYNs,
used when the guest's SYN offers it too) only the guest's holes are
resent, all of them again once the guest SACKs bytes sent after them (a
resend lost too), and a timeout (0.25 s) resends the holes below the guest's latest
blocks, then goes back if a second one comes first; without SACK a
partial ACK, or a timeout, goes back to what was acked (go-back-N). FIN
for FIN (a connection closed both ways is kept a second, to ack a FIN of
the guest's again), a reset for a reset, and a reset for a SYN to a port
nothing is relayed to, as a closed port on the Mac answers
(`relay_conns`, `relay_bytes_to_guest`, `relay_bytes_from_guest`,
`relay_resets`, `relay_refused`, `relay_retransmits`, `relay_dgrams_in`,
`relay_dgrams_out`, `relay_sack`, `relay_sack_acks`, `relay_lost`,
`relay_lost_bytes`, `relay_resent_bytes`, `relay_fast_retransmits`,
`relay_go_back`). `--selftest` runs it both ways against
`tools/tcppeer.py`'s TCP as the guest over a lossy wire, with real sockets
as the Mac's programs, and its recovery by hand (20 segments, two holes:
with SACK exactly the two resent and nothing after a partial ACK; without,
the first, then all past the partial ACK).

It also serves DHCP and DNS on VLAN 21 (always; `add_dhcp_dns`): a DHCP
server on port 67 (an address per MAC from 10.2.21.100, mask /24, router
and DNS server 10.2.21.1, a lease of `--dhcp-lease` seconds, default
3600; a NAK for a REQUEST of an address it didn't give; replies to
`ciaddr` when the client has one, else broadcast) and a DNS server on
port 53 (A records for `one.one.one.one` (two), `mac.jam`, `router.jam`,
the CNAME `www.jam` to `mac.jam`, `fastN.jam` = 10.9.0.N; `slow.jam` is
never answered; every other name, `nothing.jam` for one, is NXDOMAIN).
Their counts are `dhcp_*` and `dns_*` in the summary.

Independently, `-object filter-dump,queue=rx` writes every frame the
guest's card sends (and none the peer sends) to `<outdir>/<name>.pcap`,
and `tools/pcap-vlan-check.py` checks it for the same rule: two separate
checks, sharing no code. The run fails if either finds a frame that isn't
tagged with the VLAN (with `--vlan none`: one that carries any tag), or
(`QEMU_NET_NONE=1`) any frame at all. On the real
PC the same tool reads a capture taken at the Mac's end of a cable
straight to the PC: `tools/pcap-vlan-check.py --pc <the PC's MAC>
capture.pcap` checks every frame the PC sent (tagged, padded, well-formed)
and counts the pings it answered; with `--mac <the Mac adapter's MAC>` a
frame from neither address fails too (the PC's under another source) ([HISTORY: M9](HISTORY.md#m9-networking)).

The peer also runs by hand (`--listen P --qemu Q`, its header has the
flags), reads commands on stdin with `--stdin` (`send <hex>` tagged,
`raw <hex>` as it is, `noise`, `stats`, `quit`), and is a module for test
scripts (`sys.path` with `tools/`, `import netpeer`: `Peer`, the frame
builders, `classify`).

## The boot menu

`boot/limine.conf` is the menu; each entry is a kernel command line.
`make flash` copies it to the stick, and so does the PC's `update`, if
it passes init's check (`<bootmenu.h>`), which `make check` runs on this
file too (`build/host/menucheck`): an edit that breaks its rules fails
there. Every entry loads the kernel a second time, as a module: the pristine
image the kernel stores for the next boot (`tools/qemu-test.sh` does the
same). On every entry a panic starts that stored copy: the next boot is a
plain one (the test words are not passed on), its logd saves the panicked
boot's log as `/data/logs/boot-NNNN-crash.txt` and its shell says so in
one line (and on the desktop, a notice whose Details runs `crashlog`).
Only without a stored kernel, or for a panic within 30 s of a start after
a panic (a crash loop), does the panic screen say it can't recover: its
details after 5 s, a firmware reset at 15 s
([ARCHITECTURE.md](../ARCHITECTURE.md#kexec-reboot-and-panic)).

On top are the two everyday entries (Jam OS and its previous build);
everything else is in one folder, Developer (Enter opens it): the text
log, the IOMMU, safe mode, the network checks and every test. The check allows one level of folder (`//` entries), so Developer
holds them all side by side.

| Entry | Command line | What it does |
|---|---|---|
| Jam OS | (empty) | the boot splash ([HISTORY.md, audio and AS](HISTORY.md#audio-a1-a2-as-and-m85-kexec)): the screen dark from the kernel's start, the logo animation with its sound (played to the end, full screen; what is typed meanwhile reaches the shell), then the desktop: the shell in a terminal window on the wallpaper; meanwhile init starts the bootfs server (`/boot`), the compositor, the console (the first terminal), serialin, devmgr (with the USB and PCI drivers; it mounts the stick's `/esp` and `/data`), the mixer, the music player, netstack, dhcp (without `net.address`), dns, logd and netlog (with `net.host`). devmgr binds the PC's network chip as the netdev service on the kernel's VLAN (21), so the shell has `net`, `ping`, `host` and `update`; QEMU's e1000e, when there is one, is bound the same way. "Jam OS (no network)" (below) keeps both off. `reboot` and a panic look like switching the PC on: the splash background at once, then the next boot's splash; after a panic the shell's first line says what it was and where its log went |
| Jam OS (previous build) | (empty) | the everyday boot of the build the stick had before the last `update` (`-w`) or `make flash` (`/esp/boot/prev-jamos.elf` and `/esp/boot/prev-bootfs.img`; `tools/qemu-test.sh` boots them with `QEMU_BOOT_PREV=1`); a stick that has had neither has no such files, and Limine says it can't open them |
| Developer / Jam OS (text log, no splash) | `verbose` | the same with the kernel's text log on the screen instead of the splash, and in the first terminal as it comes (every other boot is quiet: the first terminal starts at the shell's banner and prompt, the log off every terminal's screen but for the commands whose output it is: [ARCHITECTURE.md](../ARCHITECTURE.md#debugging)) |
| Developer / Jam OS (no IOMMU) | `iommu=off` | the everyday boot with the IOMMU off: no DMA translation or interrupt remapping (the IOMMU is on by default since the PC's sign-off, 2026-10-07): the escape hatch if a device ever misbehaves with it |
| Developer / Jam OS (internal disks read-only) | `diskro` | the everyday boot with every NVMe drive treated as someone else's: drv/nvme starts with `ro` (init passes the word to devmgr), so no writable partition is opened and `storage.create_table` is refused, whatever a drive's table says; `disk prepare` is refused. The way out if a build's write path misbehaves. A `reboot` and `update` keep the word (`kexec_next_cmdline`) |
| Developer / Jam OS (safe mode: no USB drivers, serial input only) | `nousb` | the same, but devmgr leaves USB alone: input only over serial |
| Developer / Jam OS (network: listen only) | `netprobe` | the everyday boot, plus the RTL8125's listen-only probe ([HISTORY: M9](HISTORY.md#m9-networking)): devmgr binds `drv/rtl8125`, which sends nothing, listens for 60 s after the link comes up, logs its `[rtl8125]` lines and one RESULTS line, and exits. A `reboot` doesn't keep the word (the next boot is the everyday one, on the network) |
| Developer / Jam OS (network: send test) | `netsend` | the everyday boot, plus the RTL8125's ARP send test ([HISTORY: M9](HISTORY.md#m9-networking)): devmgr binds `drv/rtl8125` in full mode on the kernel's VLAN (none: "no VLAN: the network stays off", nothing touched), which waits for the link, sends twenty ARP probes for 10.2.21.1, 200 ms apart, tagged with the VLAN, logs for each when it was queued, when the chip handed its descriptor back and when the reply came, compares the chip's count of frames sent with its own, logs its `[rtl8125]` lines and one RESULTS line, and exits. Nothing else is ever sent; a `reboot` doesn't keep the word |
| Developer / Jam OS (no network) | `vlan=off` | the everyday boot with the network off: every network driver starts without a VLAN, logs `no VLAN: the network stays off` and leaves its card alone; netstack has no session, so nothing is ever sent ([ARCHITECTURE.md](../ARCHITECTURE.md#networking)) |
| Developer / All tests | `ktest` | every in-kernel test at boot, strict, on an idle machine. Kernel only: no user space runs, so no log leaves the machine (the screen, the serial port and the RESULTS box only; [below](#the-regression-runs-network)) |
| Developer / Stress test (2 minutes) | `selftest stress=120` | the stress test alone, no user space: kernel work (no log leaves the machine) |
| Developer / Stress test (10 minutes) | `selftest stress=600` | the same for 10 minutes (it signed off the milestones up to M8; from A1 on the soak does) |
| Developer / Soak test (3 minutes) | `soak=3` | a plain boot whose shell runs `soak 3 halt` by itself ([Soak](#soak)): the first failure panics, and the next boot's shell names it (the log is on the stick); a pass ends with the SOAK RESULTS box and a prompt. A plain boot, so netlog and logd run as usual: the whole soak reaches the Mac (`net.host`) and `/data/logs` |
| Developer / Benchmark | `bench` | about 10 s; results go to [BENCH.md](BENCH.md) (kernel only: no log leaves the machine; `bench` in the shell of a plain boot does reach the Mac) |
| Developer / IOMMU checks | `iommu=on vtdtest` | a plain boot with the IOMMU on in which drv/hda provokes its faults on purpose before it serves (`drivers/hda/vtdtest.c`): its command ring pointed at a page it doesn't hold pinned (a read the IOMMU blocks: a `vtd: fault:` line naming 00:1f.3), then its response ring at the interrupt window 0xfee00000 (a write blocked with fault 25h, no interrupt), then the rings back and sound as usual. `hda: vtdtest:` lines say what happened; if the controller could read the page anyway (no translation), it says so and provokes nothing. `iommu` in the shell shows the units, domains and fault counts |
| Developer / Compositor test scene | `comptest` | the compositor alone on the screen with its test scene (below, `comptest`): windows in the desktop's look held about 4 s each, then the RESULTS box with its `compositor: bench:` paint times |
| Developer / init + utest + usbtest | `init` | the user-space regression run: init runs `boot/init.cfg` (utest, usbtest, then posixtest) and the RESULTS box says whether init's root job ended with nothing charged; the box holds 256 lines (the first 192 and the last 64: a run that reports more says "N result lines didn't fit" and keeps the verdict, every test's summary and init's `exited with code` lines), and programs may report 160 lines in all With `net.host` in the stick's settings (and `netlog` not `off`) it also starts netstack, the DHCP client or `net.address`, logd and netlog, so the run's log reaches the Mac and `/data/logs` ([below](#the-regression-runs-network)); without it, and in QEMU without `QEMU_NET`, it is what it always was |
| Developer / Timer fallback | `nodeadline selftest` | the periodic LAPIC timer instead of TSC-deadline |

Other boot words (for `tools/qemu-test.sh`, not in the menu):

- `shell`: the plain boot, spelled out (what the shell scripts use).
- `verbose` or `nosplash`: no boot splash, the kernel's text log on the
  screen until user space takes it; `verbose` also shows the log in the
  first terminal as it comes (init's word `verbose`), while `nosplash`
  leaves the terminals quiet as on every boot (`tools/qemu-test.sh` adds
  `nosplash` unless `QEMU_SPLASH=1`; the serial port has the whole log
  either way). `nousb` and `soak` leave the splash out too.
- `ktest=<prefix>`: the tests whose name starts with the prefix. Tests
  named `review_...` run only when the prefix asks for them.
- With `ktest` or `ktest=<prefix>`: `loops=<n>`, `seed=<s>`, `shuffle`,
  `keep`, `load` ([Soak](#soak)), e.g. `ktest loops=5 seed=42`.
- `soak=<minutes>`: what the Soak entry does, for another length.
- `selftest`: the boot-time self-checks; `stress=<seconds>` adds the stress
  test.
- `splashhang`: on a plain boot with the splash, bin/splash opens its
  window and never finishes, so init's deadline is tested: 20 s after
  the splash's start init says `the splash didn't finish`, kills it and
  starts the shell anyway (`tools/splash-test.sh`).
- `pcilist` (the PCI device report), `keytest` (keys to the log for 30 s),
  `memmap`, `init_timeout=<s>` (how long the `init` run may take).
- `initpart=<k>` with `init`: init runs `boot/init.cfg`'s `part <k>` lines
  in place of the others: one boot of `tools/init-test.sh`
  ([below](#the-init-run-in-parts)).
- `comptest`: init starts only the compositor, on the screen, running its
  test scene (`user/services/init/comptest.c`): three steps of windows and
  two of the desktop held 4 s each, then its `compositor: bench:` lines;
  the RESULTS box says how it ended (`tools/comp-test.sh`).
- `reset=cf9`, `reset=8042`, `reset=triple`: the firmware reset
  (`reboot -f`, and every other reset through the firmware) starts its
  list of methods there instead of at the ACPI reset register
  (`kernel/dev/reboot.c`; `tools/reboot-firmware-test.sh` tries each). A
  reboot keeps it. `reset=none` (a test aid) tries none: `reboot -f`
  halts, and the panic screen's reset at 15 s fails, so it shows what it
  says then ("Jam OS couldn't restart the PC", `tools/kdump-test.sh`).
  Not kept by a reboot.
- `panichold=<ms>` (a test aid): a panic that restarts holds its screen
  ("Jam OS hit a problem and is restarting") that long instead of 1.5 s,
  at most 5000, for a screenshot (`tools/kdump-test.sh`). Not kept by a
  reboot.
- `hidboot`: hid keeps every mouse in the boot protocol (no wheel on
  most real mice) instead of the report protocol it uses for a mouse
  whose report descriptor has a wheel: the way back if a mouse misbehaves
  in report protocol (no movement, or nonsense). A reboot keeps it.
- `iommu=off`: leave the VT-d units as the firmware left them (no queue,
  no translation, no remapping); the escape hatch, kept by a reboot. The
  default (and `iommu=on`, which says so explicitly): start each VT-d unit's invalidation queue and fault
  interrupt (kernel/dev/vtd_unit.c; [HISTORY: M11](HISTORY.md#m11-the-iommu)) and turn DMA
  translation on (kernel/dev/vtd_boot.c): a function nobody drives is
  blocked (its DMA faults and is logged), one an RMRR names reaches only
  that region, and one a driver holds a DMA capability for reaches only
  what that capability pinned (its own domain: kernel/object/dma_cap.c;
  a dead driver's pins are freed once the unit confirms the domain is
  gone, not quarantined); and turn interrupt remapping on
  (kernel/dev/vtd_irq.c). With `iommu=off` no VT-d register is
  written; `iommu=off` wins over `iommu=on`. The `vtd_unit_*`, `vtd_irq_*`,
  `vtd_domain_*` and `dma_iommu_*` ktests need it (and `QEMU_IOMMU`); they
  skip themselves without it; the other `dma_*` ktests check whichever
  mode the boot is in. A reboot keeps either
  word; the jump into the next kernel (a reboot or a panic) turns
  interrupt remapping, translation and the queue off first.
- `vtdtest`: devmgr passes it to drv/hda, which runs its IOMMU checks
  (`drivers/hda/vtdtest.c`; the "IOMMU checks" entry above) before it
  serves; only in shell mode, and a reboot doesn't keep it.
- `nvmetest=<words>` (a comma-separated list of `selftest`, `loop`,
  `reset`, `abort`, `slow`): init passes it to devmgr and devmgr to every
  drv/nvme, which reads its namespace's first and last MiB and logs their
  SHA-256 (`selftest`), again and again (`loop`), with a controller reset
  while the reads are out (`reset`), or with the first read timed out at
  once and aborted (`abort`) (`drivers/nvme/selftest.c`; reads only, like
  the whole driver); `slow` answers a `block` channel's reads 20 ms apart,
  as a slow drive would, so a client that leaves has reads out
  (`drivers/nvme/block.c`'s drain rule). In shell mode and in the `init`
  run; a reboot doesn't keep it. `tools/nvme-driver-test.sh` and
  `tools/nvme-test.sh`'s `block` case use it. drv/nvme binds on every boot
  (devmgr's match row has no word since M12.7's N4), so the word is only
  for these test reads; `nvmetest=on` means "no test words" and the scripts
  still give it. The list also carries `instcut<point>`
  (`user/services/init/install.c`'s `cut_after`; drv/nvme ignores it): the
  installer stops for good right after that change reached the drive,
  synced, as a power cut there; `tools/install-test.sh`'s `cut` case ends
  QEMU on init's "cut after" line. The points, in the installer's order:
  `prepared` (the drive made a Jam OS drive), `etcnew` (`/data/etc` copied
  as `/etc.new`), `etc` (renamed), `cleared` (the ESP's menu and an
  update's leftovers removed), `limine`, `kernel`, `prevkernel`, `bootfs`,
  `prevbootfs`, `menu` (the last change).
- `diskro`: init passes it to devmgr, and devmgr starts every drv/nvme with
  `ro`: the driver treats every drive as someone else's (read-only, no
  writable partition, no `create_table`), whatever its table says. The way
  out if a build's write path misbehaves. In shell mode and in the `init`
  run; a reboot (kexec) keeps it, so `update` and `reboot` do not lift it.
  `tools/nvme-test.sh`'s `diskro` case uses it, and
  `tools/kexec-reboot-test.sh`'s `kexec` run boots with it and checks the
  kexec'd kernel's command line still has it.
- `nospare`: init keeps no warm spare of the mixer
  (`user/services/init/spare.c`), and volumes none of fat
  (`user/services/volumes/spare.c`), so a restart starts a process, as
  before spares (it still hands the new instance its state and keep
  channel, and keeps the restart rule for a deliberate kill): the switch
  for comparing restart times. A reboot keeps it.
- `nolockdep`: the lock-order checker (`kernel/debug/lockdep.c`) checks
  no spinlock or mutex this boot: the switch for the benchmark's
  checker-off column (`bench` also flips it for spinlocks within one run,
  as the `lockdep` switch). The everyday boot checks every lock. A reboot
  keeps it. Its guards are weaker, on purpose (the owner, M11.5 review
  question B): with the checker off no CPU counts the spinlocks it holds,
  so `can_trim` (thread.c) and `check_callable` (ipi.c) can't refuse a
  stack trim or a cross-CPU call made while one is held; no caller does
  that today, and `return_to_user_work` still catches a lock held into
  user space through `preempt_count`. Use it only for measuring.
- `test<name>`: a crash test at boot (`testpf`, `testlockorder`, ...; the
  names are in `kernel/debug/selftest.c`). Each must panic with the right
  message; `testbp` must come back. The early ones run before the stored
  kernel is loaded, so they end on the panic screen.
- Switches for the scheduler and friends, each turning one optimisation
  off to compare: `nopcid` (and `forcepcid`: PCIDs on even where the kernel
  leaves them off for the INVLPG erratum), `nospinidle` (or `idlespin=<us>`),
  `noplaceorder`, `noaffinepair`, `nokmcache`, `nooneshot`, `noserialirq`,
  `nofpuopt`, `nofpucall` (a switch inside a system call saves the full
  FPU state again; utest's `fpu_call_keeps_control_words` then reports
  its rounds as kept, not zeroed), `nohandoff` (a wake that would hand
  the CPU straight to the woken thread queues it instead). The message slots
  (`channel_slots`) have no boot word: the benchmark's `slots` switch and
  the path tests turn them off for a moment.
- `vlan=<id>`, `vlan=none`, `vlan=off`: the network's mode
  ([ARCHITECTURE](../ARCHITECTURE.md#networking)). `vlan=<id>` (1..4094):
  every frame Jam OS sends is tagged with that VLAN and only frames tagged
  with it are received. `vlan=none` (or `vlan=untagged`): every frame is
  sent untagged, never one with a tag, and only untagged frames are
  received. With no word the build's default: `JAMOS_VLAN` in `local.mk`
  (the owner's: 21), untagged without one. `vlan=off`, or any value that
  is neither (`vlan=0`, `vlan=4095`, `vlan=21x`, a bare `vlan`, two words
  that disagree), means the network off: the network drivers turn neither
  receiver nor transmitter on. The boot log says which and where it came
  from (`network:     VLAN 21 (the build's default)`, `network:
  untagged (the vlan= word)`), and so does devmgr (`network drivers get
  vlan=21`, `vlan=none`). The kernel passes it to init (also in the
  `init` run), init to devmgr, devmgr to each network card's driver;
  netstack hears it only from the driver. A reboot keeps the word,
  `vlan=none` and `vlan=off` too.
- `bootdisk=<n>`: the MBR disk id (decimal) of the disk the machine booted
  from, which a kernel started by kexec gets from the one before (Limine
  tells the first one itself); volumes takes the Jam OS disk with that id
  as the boot disk. `bootdisk=gpt:<GUID>`: the same for a GPT disk (an
  installed drive), named by its disk GUID (`0f6e1c52-9a3b-4d07-8e21-5c4b3a291807`,
  either case); volumes takes the Jam OS drive (its GPT mark) with that
  GUID. Limine names a GPT disk by its GUID alone.
- `crashkernel=<MiB>`: the stored kernel's region (default 64, 32..1024); `crashkernel=0`: no
  stored kernel, so a panic halts on its screen and `reboot` falls back to the firmware.
- `crashtest=<name>`: the stored kernel's command line gets `test<name>`, so the next boot (after
  a reboot or a panic) runs that crash test: a crash loop for `tools/kdump-test.sh`.
- `smp=loader`: Limine wakes the other CPUs and the kernel releases them,
  instead of its own INIT-SIPI-SIPI: for troubleshooting (no menu entry;
  on the PC, press E on an entry in Limine's menu and add it to its
  `cmdline`). The boot log's `smp: N of M CPUs online in T ms (...)` line says which way
  they were started and how long it took.
- `smp_test_skip=<cpu>` and `smp_test_late=<cpu>` (kernels with tests
  only): that CPU gets no startup IPIs, or comes late (it waits until the
  BSP has given up on it, then its claim must be refused). Either boot
  must log `smp: cpu <cpu> (lapic L) did not start` (the late one also
  `its claim was refused: it parked itself`) and run on with one CPU
  fewer; its RESULTS box says FINISHED WITH PROBLEMS (the CPU's timer
  ticks are missing).

The tests at boot (All tests, the stress test, the benchmark, the timer
fallback) run before user space, so nothing of them reaches the stick:
their RESULTS box on the screen is the only record. Runs from the shell
(and the Soak entry, which is a plain boot) are in the boot log that logd
writes to `/data/logs/`.

## From the shell

Most tests are shell commands, so a test run needs no reboot: `ktest
[prefix]`, `bench`, `stress <seconds>`, `utest`, `usbtest`, `crash <name>
yes` (the deliberate panics; `crash panic yes` is the plain one: the next
boot comes up and says so; `kexecbad` damages the stored kernel first, so
it must be refused, the panic screen saying it can't recover, then a
firmware reset; `kexecstall` and `kexecfault` break the jump after it was
decided, and must end the same way), plus `devices`, `usb`,
`pci`, `memmap`, `iommu` (the IOMMU's units, domains and faults:
[The IOMMU](#the-iommu)). `reboot` kexecs into the stored kernel (or the files on
`/esp` if they changed); `reboot -f` resets through the firmware, which
ends a QEMU run (`-no-reboot`): the shell scripts end with it.
`soak` is the soak test ([Soak](#soak)), and `ktest` takes its options.
`run utest only <prefix>` runs the utests whose name starts with prefix
(a quick run while working on one area; a test that counts the process's
handles may then see one an earlier test would have made first, as
`jwlc_setup` does when it runs before the `jwl_` tests); `only` takes a
comma-separated list of prefixes, and `utest except <prefixes>` runs every
other test (both say `utest: running <n> of <m> tests` in the RESULTS box).
`ktest` from the shell runs "live" next to the rest of user space: checks
on system-wide counts are not made, and tests that need the machine to
themselves are skipped (`KT_SKIP_LIVE`, in `kernel/include/jam/ktest.h`);
the summary line says how many.

## Soak

A test that passes once at boot can still fail the second time, or after
another test, or next to other work: it kept something in a static, or
counted on a fresh machine. (The first such bug found: a second `ktest` in
one boot always failed `pcid_slot_bookkeeping`, whose made-up CPUs kept
the first run's slots.) The soak looks for that class.

**The options of `ktest`** (the same words on the kernel command line and
after the shell's `ktest`; `struct ktest_opts` in
`kernel/include/jam/ktest.h`):

| Word | What |
|---|---|
| `loops=<n>` | the whole set n times in one boot |
| `seed=<s>` | in an order shuffled from s. Loop k uses s + k - 1 and prints it: `ktest seed=<that>` replays that loop alone |
| `shuffle` | a seed from the clock (printed) |
| `keep` | a failed test is recorded and the run goes on; its report says how many FAILED. Without it the first failure panics |
| `load` | with the stress test's workers running (two per CPU: counters, allocations, sleeps, migrations, thread and process churn) and a TLB shootdown round every 250 ms. `load=<n>`: n workers in all (the QEMU script uses one per CPU) |

Plain `ktest` is what it always was: once, in link order, strict.

**Under load** a test must still pass if it is about correctness. One that
asserts exact timing, exact placement or an exact system-wide count needs
an idle machine and says so: `KT_NEEDS_IDLE("why")` skips it (the log line
reads `skipped (busy machine: why)`), `KT_IDLE_EQ` / `KT_IDLE_ASSERT` leave
out one such check. With `load` the run is also "live" (the load makes
channels, processes and pages), so `KT_SKIP_LIVE` tests are skipped and
global counts are not checked. Most tests run under load; the run's
summary says how many were skipped and why. Never mark a test that is only
slow under load; a wait that is only there so a broken kernel fails instead
of hanging takes `kt_patience_ms`.

Which marker to use (all in `kernel/include/jam/ktest.h`; the rules a test
follows are in [CODING-GUIDE.md](../CODING-GUIDE.md#add-a-kernel-test)):

| The check | Use | From the shell or under load |
|---|---|---|
| on the test's own objects | `KT_EQ`, `KT_ASSERT` | always made |
| on a system-wide count (free pages, live channels, port stats) | `KT_GLOBAL_EQ`, `KT_GLOBAL_ASSERT` | not made (counted as relaxed) |
| a test whose whole point is a system-wide count, or that runs the machine out of memory | `KT_SKIP_LIVE("why")` at the top | skipped: `skipped (live system: why)` |
| one check on exact timing, exact placement or an idle CPU | `KT_IDLE_EQ`, `KT_IDLE_ASSERT` | made from the shell, not under load |
| a test that is nothing but such checks | `KT_NEEDS_IDLE("why")` at the top | run from the shell, skipped under load: `skipped (busy machine: why)` |
| a wait that only guards against a hang | a deadline of `kt_patience_ms(ms)` | 30 times longer under load |

The pathstat tests (`kernel/test/test_pathstat.c`) count exactly what one
call does, which holds only while the call's two threads have their CPU
to themselves. Beside the desktop on a small QEMU, the compositor's paint
threads are sometimes ready on that CPU too: a wake is then queued instead
of handed over and the counts change. The scheduler counts such switches
(`PATH_SHARED`), and the harness (`bench_path.c`) measures a shared window
again, up to 5 in all (the log says `path: window N shared its CPU ...:
measuring again`). If every window was shared the test is skipped:
`skipped (shared CPU: ...)`, except under load, where every window is and
the test makes its load-proof checks as it always did. The boot menu's
All tests runs nothing else, so there no window is shared.

**`soak [minutes] [loops=N] [seed=S] [load=N] [halt] [idle]`** in the shell is the
whole thing in one command (default 3 minutes; Ctrl+C ends it after the
step in progress):

- first one shuffled loop on the idle machine (`ktest loops=1 seed=S
  keep`), then the loops under load, each `ktest loops=1 seed=<the next
  seed> keep load` and then `utest`, and at the end, with the load
  stopped, one more idle loop: the tests that need an idle machine run
  there, after everything the soak did to the system;
- during the loops under load `bin/soakload` (`user/tests/soakload/`) writes a file,
  syncs it, reads it back, compares and deletes it on `/data` and on every
  other writable stick (`mount -w /usb0` first), reads the files of
  read-only mounts twice, maps and unmaps memory, makes channel calls and
  starts programs. Pull and plug sticks while it runs: a file cycle that a
  pull cut short is counted as such, not as a failure;
- at the end the SOAK RESULTS box: loops and seeds, tests passed, skipped
  and FAILED (each failure with its check, file and line, and the seed
  that replays its loop), utest runs, file cycles, the slowest tests, and
  what the system held before and after (free pages, channels, interrupt
  objects, and the pages, handles and threads of the shell's job tree).
  Those are printed, not judged: mounts and drivers move them. A figure
  that climbs from one soak to the next is a leak to look for.

`halt` leaves out `keep`: the first failed check panics, as the boot menu's
Soak entry does. `idle` leaves out both loads.

**A failure.** Every panic carries a note under its message, in the log
and in the panic screen's details (whatever kind of panic: a failed
check, an exception, the watchdog):

```
ktest: loop 3 of 5, seed 1236, test 57 of 221: chan_x, under load, live; before it: a, b, c, d
```

Photograph the top of the screen (the message, that note and the
backtrace). `ktest seed=1236` (add `load` if the note says so) runs the
same order again; to narrow it down, `ktest <prefix> seed=1236` shuffles
only the tests the prefix selects. With `keep` a failure is one log line,
`ktest: FAILED <test>: <check> (<file>:<line>) [loop, seed, test n of m]`,
and the test ends there: what it left behind (objects, threads) may make
later tests fail too, so the first failure of a run is the finding.

## Shell scripts

Each file in `tools/shell-tests/` types into the shell over the serial
port. Unless the table says otherwise, run it as:

```sh
QEMU_INPUT=tools/shell-tests/<name>.txt tools/qemu-test.sh build/test <name> shell
```

### How a script's verdict is reached

A shell script passes if every `wait` in it matched, QEMU ended by itself
(the script's `reboot -f`), and `tools/serial-feed.py` saw no failure line
from the guest's test programs. That last part is the same for every
script, whatever it waits for: a script's own `wait utest: 3 passed` is
only a prefix of `utest: 3 passed, 1 FAILED`, and a script that echoes a
constant after a `utest only` would never notice. serial-feed reads every
line the guest prints and fails the run on:

- a summary with failures: `N passed, M FAILED` (utest, usbtest, hdatest,
  mixtest, nettest, ktest and its soak's lines; `dnstest`'s `N failed`),
  `utest: FAILED tests: ...`, a failed check (`FAILED at line N`, from
  `user/include/check.h`), `ktest: FAILED ...`, posixtest's `FAIL` and
  `FAILED` lines, `shell: soak: FAILED`;
- a `utest only` or `utest except` that printed `utest: running N of M
  tests` and never printed its `utest: N passed` summary (utest crashed or
  was killed; `$?` is 137 then, but a script that does not look at it
  would pass): checked when the next one begins and at the end of the run.

On a failure line the feed says which line (`serial-feed: the guest
reports a failed test: ...` in the run's output), stops the script at its
next command or `wait` (that wait is reported as `no '<text>' before the
guest reported a failed test`) and tells QEMU to quit, so a failure does
not sit out the run's timeout. The first failure is the finding: a `utest
only` of several cases stops at its first failed check, with no summary
naming the others (rerun that case alone for more). A script that expects a test to fail would
need a way to say so; none does (the deliberate failures, `ktest=review_ktest`
and the canary below, are not run through serial-feed or are the test of it).
Scripts still check `$?` where they can (`utest only x; echo st-$T=$?`,
`wait st-x=0`), which is the stronger check for a crash.

`utest only verdict_canary` is the proof: a case that passes unless the
shell exported `UTEST_CANARY=fail` (a check fails), `=quiet` (it fails
without printing the check's line: a busy run's printed lines can be dropped,
the RESULTS lines are not) or `=crash` (a NULL write, utest killed).
`tools/verdict-test.sh <outdir> ok|fail|quiet|crash` boots
`shell-tests/verdict.txt`, which is written the weak way (no `$?`, no
summary look), and passes when `ok` passes the script and the others fail it
with the right reason. `serial-feed.py --scan <log>...` says what a saved log
would have failed on.

| Script | What it covers | Extra setup |
|---|---|---|
| `basic.txt` | line editing, history, console protocol levels, restarting serialin and the console; `contest flood` (a client that floods the console must not stop Ctrl+C or other clients) | |
| `cmds.txt` | the everyday commands: `help` (the everyday list) and `help dev` (the developer one), information, date and time zones (the clock from the RTC as Sydney's local time), files, pipes and text, variables and aliases, `true` and `false` with words after them (ignored, as POSIX's; `env x` still refused), programs, Tab and Ctrl+C; `contest junk` and `contest spew` (oversized or handle-carrying pipe output is dropped with its handles closed; a flooding program still stops on Ctrl+C); `run utest msgflood` (a program writing 64 KiB messages on a channel nobody reads stops with `ERR_NO_MEMORY` at its own job's 16 MiB message budget, `DP_JOB_MSG_BYTES`, and the shell goes on); `disks` (the boot stick as the Jam OS disk, its two mounts), `disk`'s usage and unknown disk, `mount -w /nvme0` with no NVMe drive: not mounted | `QEMU_EXTRA="-rtc base=2026-01-15T01:02:03"` (the date checks), `QEMU_SMP=4` |
| `system.txt` | the System and Tests commands through the command table: argv, status, pipes, help, aliases | |
| `console-restart.txt` | a console started again after about 550 KiB of log (the klog gap test, 8 loops) draws only the last 256 KiB of it, and the shell comes back | |
| `commands.txt` | utest, usbtest, pci, memmap, the crash list, fractal in a full-screen window, Ctrl+C past a program, orphans killed with their job, devmgr restarted by init; ends with a real crash, and the next boot's line about it | |
| `extras.txt` | bench and a short stress from the shell, scrollback, clear; ends with a panic, and the next boot's line about it | |
| `perop.txt` | `perop -q /data/` (the per-operation lines, a tenth of the samples): every line printed (stat, open + close, a 4 KiB block read through usb-storage, cached 4 KiB and 64 KiB reads, a 64 KiB write through to the stick), its scratch file gone afterwards; a usage error (2). QEMU's numbers mean nothing: the PC gives them ([BENCH.md](BENCH.md#method)) | |
| `files.txt` | the file namespace: `/boot` as a read-only mount, `run` with a path, the file commands (mkdir touch write cp mv rm df sync) on a writable mount (the tests' RAM filesystem, `run ramfs shell`); `ls -l`, `stat`, `chmod` and `ln` on that mount, which keeps no modes or links and makes them up (M13.6's U8), a mount that reaches a running shell, the bootfs server killed and mounted again | |
| `files-fat.txt` | the file commands on a real FAT volume (`run utest fat-shell`: bin/fat over a RAM disk): names with spaces and lower case, big copies, rm -r | |
| `posix-files.txt` | posixtest's files-many section (too long for the `init` run): 10,000 files made in one directory of the stick's `/data`, listed (each name once, `.` and `..`, telldir and seekdir), removed; about 2.5 minutes | `QEMU_TIMEOUT=600` |
| `posix-run.txt` | posixtest's default sections run from the shell (`run posixtest`): descriptor 0 is the terminal, not the null device, and `/svc` holds only what posixtest's list asks for (net, net-listen, dns, exec, unix); the epoll check opens the null device itself (and the two `/svc` checks skip with a printed reason in a namespace with no `/svc`), so it ends PASS and `exited with code 0` | `QEMU_TIMEOUT=900` |
| `posix-scm.txt` | posixtest's scm section (`run posixtest scm`): socketpairs carrying descriptors (`SCM_RIGHTS`): every kind passed and used on the other side, the read-only twin and seals, 64, 128 and 253 descriptors, too little room (`MSG_CTRUNC`, no handle kept), `SOCK_SEQPACKET`, `shutdown`, Mojo's pattern with a child, the 10,000-round fuzz; also in the `init` run | |
| `posix-procinfo.txt` | posixtest's procfs and procinfo sections (`run posixtest procfs procinfo`) and utest's `process_maps`, `sys_public_info` and `posix_ids` (`utest only <name>`) from the shell: the files of libc's `/proc` that Chromium's base reads (`/proc/self/stat`, `status`, `statm`, `maps`, `/proc/cpuinfo`, `meminfo`, `/proc/stat`, and a child's stat and status from live through zombie to reaped) against proc(5) and what the process really does, and the kernel calls under them (`process_get_maps`, the free-memory figure, a process's creation time and name): [CR-PLAN](CR-PLAN.md#prc-notes), "PRC notes"; also in the `init` run | |
| `posix-execinfo.txt` | posixtest's execinfo section (`run posixtest execinfo`) from the shell: `backtrace()` from nested functions, a second thread, a signal handler and a `qsort` comparator, and the lines of `backtrace_symbols` and `backtrace_symbols_fd` ([CR-PLAN](CR-PLAN.md#bt-notes), "BT notes"); also in the `init` run | |
| `posix-unix.txt` | posixtest's unix section (`run posixtest unix`) against shell mode's broker, bin/unix (`/svc/unix`, [CR-PLAN](CR-PLAN.md#cu2-notes), "CU2 notes"): named `AF_UNIX` sockets, a listener and two connectors, bytes and a descriptor over a connection, the backlog, a socket file removed and a stale one, a 108-byte path's `getsockname` (CR-REVIEW-POSIX finding 5), a child without `/data` refused and one with `/data` read-only let in, abstract names, a child given `/svc/unix` by name binding 32 listeners at most however often it opens it (its opener is its own: CR-REVIEW-POSIX Q2; here it shares posixtest's own); then `ps` shows `unix`; also in the `init` run (where init starts the broker before the programs) | |
| `posix-raw.txt` | posixtest's raw section (`run posixtest raw`) with `JAMOS_ENOSYS_LOG=1` exported: the raw Linux calls Chromium makes (`prctl`, `setpriority`, affinity, the scheduling policies, futex bitset waits) and the ENOSYS log (an unknown call, option, ioctl or path named once per process in the kernel log: `enosys: posixtest: syscall 163 (acct)`); the section counts the log's lines itself and starts children with and without the variable | `QEMU_TIMEOUT=300` |
| `posix-loop.txt` | posixtest's socket and loopback sections (`run posixtest socket loopback`) from the shell: netstack runs with no card and no address, and loopback needs neither | `QEMU_TIMEOUT=300` |
| `posix-net.txt` | posixtest's socket and sockets sections (`run posixtest socket sockets`) against the network peer: run by `tools/posix-net-test.sh`, which sets the address and starts the peer's servers | `QEMU_NET=1` (the script) |
| `libc-test.txt` | libc-test's functional and regression sets, as `tools/libc-test.sh <outdir> [name] [test or prefix ...]` ([ports/libc-test](../ports/libc-test/)): `usr/bin/libc-test` starts each test as a child of its own with `posix_spawn` (output on a pipe, 20 s each) and sorts what happened against `expected-failures.txt`, every line with its reason (`enosys` by design, `limit`, `bug`, `pending`, `skip`); it ends `libc-test: DONE ok` only if every failure is on the list, none is off it, and no listed test passes; about 1.5 minutes | `QEMU_TIMEOUT=1500` |
| `ports.txt` | the ported programs (M13 P1): about 35 of sbase's utilities (`sort uniq wc head tail cat grep sed tr cut paste nl comm fold od expr basename dirname printf seq cp mv rm mkdir rmdir touch tee find xargs env time date uname which cksum dd tar ed`; the names the shell has its own command for run by path, `run /boot/usr/bin/<name>`) on real files in `/data`, each output compared with a file by sbase's own `cmp`, and `lua` (`-e`, a script file with its arguments and exit status, an error, stdin, `io` on `/data`); `find -exec`, `xargs` and `env` start children, so their own output goes through a pipe (a redirected file cannot be passed on to a child yet) | `QEMU_TIMEOUT=240` |
| `posix.txt` | what the shell gives a POSIX program (`pxio`, user/posix/progs/pxio): descriptors, environment, directory, stack; `<`, `>`, `>>`, `2>`, `2>&1` and their order; `PATH` lookup; pipelines of programs that run at once (`yes \| head`, 100 MiB through three programs, builtins mixed in); Ctrl+C (SIGINT, `^C` written once) and Ctrl+\ (SIGQUIT); a program that ignores SIGINT having its job killed 2 s later; `$?` as 128 + N for a signal; `jobs` and `kill %n` (SIGTERM, then the job); a program's last lines before the shell's next ones (`pxio seq 3000; echo`), 25 arguments all reaching the program, typing into a pipeline's first stage (`pxio cat \| pxio count`: one terminal for the line). About 3 minutes | `QEMU_TIMEOUT=900` |
| `sysbox.txt` | Jam OS's own commands as programs (`user/sysbox`, bin/free, bin/dmesg, ...): each run as the builtin and as the program (`run <name>`), the deterministic ones compared with `cmp` (`version lscpu hexdump hd df crashlog usb`, `net` and `host` with no network, hexdump from a pipe and from a redirected stdin), the rest by what they print (`free uptime ps dmesg log memmap pci mem iommu bench`, `ping`'s usage and refusal), a box by its own name and an unknown command (127), and the refusals: `ps -k` needs `right debug` (ERR_ACCESS_DENIED), `env dmesg`, `env mem` and `/bin/sh -c free` get no root resource ("not given the kernel's information", 126), `/bin/sh -c hexdump` has no `/boot`, `env df` has no `/esp`; the builtins stay builtins | `QEMU_TIMEOUT=500` |
| `sysbox-net.txt` | `net`, `ping` and `host` as programs on a network: `net` against the builtin, `net stats`, `ping 1.1.1.1` through the gateway, Ctrl+C ending `ping` with its summary, `host` of an address | use `tools/ping-test.sh` (after `net.txt`) |
| `unplug.txt` | the stick pulled while the system runs and plugged back in (the monitor's `device_del` / `device_add`): `/data` and `/esp` go, nothing hangs, they come back in the running shell, logd carries on; volumes' notices "Jam OS stick removed" and "Jam OS stick is back" | |
| `cad.txt` | Ctrl+Alt+Del on a USB keyboard: the compositor blanks the screen and asks init, which syncs `/data` and kexecs into the stored kernel, where the file written before is read back | the `tools/usbkeys-test.sh` `QEMU_USB` |
| `data-1.txt`, `data-2.txt`, `data-3.txt` | three boots of one stick: `/esp` and `/data` from the stick itself, a file kept across a reboot, a boot log per boot, the fat service and devmgr killed, the plug pulled | use `tools/data-test.sh` |
| `sticks.txt` | other people's sticks: read-only at `/usb0` and `/usb1`, writes refused, `mount -w` and `mount -r`, what `mount` refuses, a stick pulled while a file on it is read and another in the middle of a copy onto it, a Linux stick whose ext volume bin/extfs mounts read-only (`mount -w` refused, `disks` says `(ext)`), sticks with nothing to mount | use `tools/sticks-test.sh` |
| `apps.txt` | programs drawing with libfun in windows: the test programs' self-tests (fractal: the maths, the kernels against each other, the parallel render, libfun's pool resting; wltest: libfun's pointer), fractal explored (screenshots, its benchmark of one CPU against all, the deep tour), `wltest --mouse` before the mouse stirs and with one click in its window, then Ctrl+C in a program, a program killed (from its terminal) and one crashing while their windows are up, and the shell having the keys again | use `tools/apps-test.sh` |
| `jamjar.txt` | jamjar (the music player's window): its cover helper's self-test (`run jamcover --selftest`: stb_image's checks); jamjar's self-test (its helper decodes a PNG, refuses a non-picture, and one that crashes and one that hangs on purpose are killed and replaced; a decode that timed out is tried again once, later); `run jamjar noplayer` without the player; the `jamjar` command: the covers read from the tags (PNG in ID3v2.3 and 2.4, a JPEG, one 611x640, none in a WAV, an oversized and a garbled one refused), an album played by keys, next, back, pause, volume, search, a calibration track (1 kHz on the left, 4 kHz on the right) whose loudest bars must be the left's 34 and the right's 49, and a shot of it (`jamjar-tone`) in which bar 34 stands only up from the line and bar 49 hangs only down, help, roulette, the big view, the sleep timer; mouse clicks on a button, the volume and a track row (where jamjar's trace says they are in its window, plus the window's place, found by a right click jamjar only reports: jamjar is a maximised window under the strip); the music still playing after q | use `tools/jamjar-test.sh` |
| `jamjar-hd.txt` | jamjar at 2560x1440 by keys, for screenshots at the PC's size (the stereo bars, the big view's sunburst) | use `JAMJAR_HD=1 tools/jamjar-test.sh` |
| `jamjar-cpu.txt` | jamjar's CPU use while it plays in a big window: in the background, full screen (Super+F) at 2560x1440, everything in shuffle, then `top -n 2 -d 10` for the compositor's and jamjar's CPU over 10 s (100.0 is one CPU), and jamjar's frame counts when it quits (`N frames redrew only what moved (U us each), M the whole window`); `JAMJAR_ARGS=whole` redraws the whole window every frame (the old way) to compare | use `JAMJAR_CPU=1 tools/jamjar-test.sh` (through `tools/remote-test.sh env JAMJAR_CPU=1 ...`) |
| `jamjar-covers.txt` | jamjar full screen (Super+F: on the compositor's boot it opens in a tile) at 2560x1440 over 32 albums with covers, everything in shuffle, 18 skips (the first while the covers are still read), two shots of now playing after each | use `tools/jamjar-covers-test.sh` |
| `desktop.txt` | the compositor's seat with real USB input, tiling (the default): typing in the first terminal, Super+Enter (terminal 2 takes the right half and the keys), a click moving the keys back, Super+Right and Super+H moving the focus, two more terminals splitting the focused tile (`desktop-tiles`), the gap dragged (`desktop-resized`), Super+Q closing two, then the first (`desktop-reopened`: Super+Enter opens terminal 1 again), Super+T's floating saved and kept across a reboot (`pointer` puts the mouse at exact pixels: the geometry is in the script) | use `tools/mouse-test.sh` |
| `mouse.txt` | the mouse through QEMU's monitor into windows: the shell and `wltest --mouse nomouse` (which never asked: only its two keys reach it) undisturbed by it, then `wltest --mouse noaccel` (libfun's mouse, as an app has it) saying every edge at the pointer's place in its window: a click, a right click held, a chord, a left and a right drag (back to the same place), the wheel each way and the totals | use `tools/mouse-test.sh` |
| `ktest-all.txt` | every kernel test from the shell, live, three times in one boot (a test that leaves something behind fails its next run) | |
| `soak.txt` | `soak loops=2` from the shell: two shuffled loops under load with utest between them, and the SOAK RESULTS box | `QEMU_TIMEOUT=600` |
| `soak-plug.txt` | the soak with a second, writable stick and the boot stick pulled and plugged while it runs | use `tools/soak-test.sh` |
| `screen.txt`, `screen-verbose.txt` | the first terminal on a plain boot (no log in it but while a command whose output it is runs; the desktop's stick notices) and on a `verbose` one | use `tools/screen-test.sh` |
| `clock-1.txt`, `clock-2.txt` | the date, the time zones, the settings across a reboot | use `tools/clock-test.sh` |
| `nousb.txt` | safe mode | command line `nousb` instead of `shell` |
| `netprobe.txt` | the boot word `netprobe` reaches devmgr (its line), and with no RTL8125 (QEMU) nothing is bound for it | use `tools/netprobe-test.sh` |
| `netsend.txt` | the same for the boot word `netsend` | use `tools/netprobe-test.sh` |
| `net.txt` | `net`, `ping 1.1.1.1 -c 3`, Ctrl+C in a ping, `net stats` ([netstack](#netstack)) | use `tools/ping-test.sh` |
| `dns.txt`, `dns-static.txt` | a DHCP lease, `net`, `host`, `ping <name>`, Ctrl+C on a name nobody answers, `run dnstest` (the slow-peer rule), netstack killed and the lease asked for again; a static `net.address` and no DHCP client ([DHCP and DNS](#dhcp-and-dns-end-to-end)) | use `tools/dns-test.sh` |
| `netserve.txt` | the same for the boot word `net`, and a `reboot` keeps it (the next boot's devmgr has it) | use `tools/netprobe-test.sh` |
| `netlog.txt` | netlog: the Mac late and paused (said once each), `kill netlog`, `kill netstack`, `crash panic yes` and the next boot's two streams | use `tools/netlog-test.sh` |
| `net-vlan.txt`, `net-vlan-off.txt` | every path that transmits in one boot (the DHCP lease, netlog, `host`, `ping <name>`, `ping 1.1.1.1`, `update -n`, sntp's requests, the peer's pings answered, `fetch`, `serve` and `speed` over TCP); the same commands on a `vlan=off` boot, each saying why it can't | use `tools/net-vlan-test.sh` |
| `sntp.txt`, `sntp-name.txt`, `sntp-off.txt` | the clock from the network: the jump to the peer's 2031 past forged replies, `date -r`, `date -z`, sntp restarted, devmgr restarted and the network's time kept; `ntp.server` by name; `ntp = off` ([netstack](#netstack)) | use `tools/sntp-test.sh` |
| `allow.txt` | programs on `/data` (and `/jam`: the same words, `extcmds.txt` runs it there): a copy of bin/soakload refused until `allow`ed (n refuses, y allows), `allow -l`, run, a program can't change `/data/etc`, nor read, list or make anything there by any spelling (`ETC`, `etc.`, `x/../etc`, relative to a `cd /data`): utest and sbase's tools from `/boot`, sbase's `cat` allowed from `/data`, a changed file refused, a list asking for devmgr or init (a copy of bin/utest) or for `right debug` (a copy of bin/wantdebug) or for the ports below 1024 (a copy of bin/serve, `svc net listen low`) refused, approval or not, `svc net listen` (bin/wantlisten: port 5000 refused on `/svc/net`, taken on `/svc/net-listen`, a TCP listener on port 80 refused there, from `/boot`, and from `/data` once `allow` showed "accepting connections from the network"), `allow -r`, a file off `/data` and a second shell refused | |
| `jit.txt` | JIT memory from programs ([CR-PLAN](CR-PLAN.md#12-the-jit-by-capability)): no process marked `[jit]` by `ps` at first; bin/jittest (a POSIX program whose list says `right jit`) given the power by the shell: `MAP_JIT` code written and run (`jittest: PASS`); `jittest hold &` marked `[jit]` by `ps` with the footnote, the mark gone with it; `launch jittest` (init's desktop-app start, which the shell's channel may use for init's test programs: an app whose list asks for a right) runs it with the power, `init: jittest ended with code 0` in the log; `posixtest jit` (no power: `EPERM`, and jittest as its child refused); a copy on `/data` refused until allowed, `allow` saying on a line of its own that it writes and runs its own code (n, then y), `allow -l`, run with the power, `allow -r` | |
| `sessions.txt` | init's sessions (`/svc/session`, what an SSH login runs in): `utest only session` (an interactive session: the prompt, `echo`, TERM and a resize as a POSIX program on it sees them, `exit 7` as its end; commands with their status and no banner; hang-ups by `hangup`, by signal 15 and by closing the master, and SIGHUP reaching a POSIX program on the terminal; the limit of 8; posixtest's session section; bad arguments, junk on every channel, the control channel closed first, answers left unread, 1,000 connects and 1,000 sessions opened and closed; a namespace without it), `posixtest pty` from the shell, bin/wantsession given `/svc/session` from `/boot` and refused by `allow` on `/data` | |
| `ssh.txt`, `ssh-again.txt` | the SSH server (bin/sshd, the tinyssh port): init's host key made on the first boot and its fingerprint, sshd listening on 22, the shell's `ssh`, and nothing open once the test host's client is done (its last login, as `sshcheck-done`, refused on purpose); the second boot of the same stick: no new key. Run by `tools/ssh-test.sh`, which puts the keys on the stick and runs the client | `QEMU_NET=1` (the script) |
| `parse-limits.txt` | the shell's 32-segment limit and unclosed quotes | |
| `terms.txt` | more terminals: `term` opens terminal 2 (its shell's banner copied to COM1), its shell and its console killed and started again, the compositor killed and both consoles connected again, terminals up to the limit of 16 (five tiles a screen at 1280x800, the rest spilling to new screens: `terms-last`, Super+1 back to `terms-first`), `kill console-12` and `kill shell-16` started again, `term` refusing one more ("16 terminals is the most"), Super+Enter refused with the notice "No more terminals / 16 is the most. Close one first." and a second Super+Enter at once posting none (`terms-full`); `free` with one terminal and with 16 | `QEMU_USB="-device usb-kbd,bus=xhci.0,port=2"` |
| `clip.txt` | copy and paste between terminal windows: a triple click on a line of terminal 1 (`clip-selected`), Super+C, `term`, Super+V in terminal 2 puts it on the shell's line, bracketed, and only Enter runs it (`clip-pasted`); a drag over two lines (`clip-drag`) copied and pasted with Ctrl+Shift+V into terminal 3 arrives as one line (clicks at 1280x800) | use `tools/clip-test.sh` |
| `longline.txt` | lines longer than the terminal is wide (up to the shell's 240 characters at any width): a 208-character line typed into terminal 2 (the right half, about 67 columns) and run whole; a line edited in its middle (`longline-mid`: one row, scrolled, '<' and '>' at the cut edges); both recalled from the history; a 213-byte paste (two rows of terminal 1, triple-clicked and dragged, Ctrl+Shift+V) run whole (`longline-paste`); a long line kept on its row while the tiles are resized (`longline-resize-1`, `-2`), then finished and run whole (clicks at 1280x800) | `QEMU_USB="-device usb-kbd,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3"` |
| `history.txt` | the shell's history (`/data/etc/history`): a line with a leading space not kept, a repeat kept once in its newest place; Up with text on the line only through the lines that start with it, Down back to the text, an empty line through them all; a new terminal recalling a line of the first's; a shell killed saying it restarted after a crash; `history -c` forgetting them, a new terminal starting with none | |
| `typeahead.txt` | keys typed before the terminal's window is up all arrive: a 213-character line (two thirds typed with Shift) typed the moment terminal 1's prompt shows at boot, while the window opens and is resized, kept by the compositor (`EARLY_MAX` key events) and handed to the console at once (libjwl reads only as its event queue has room), sets a variable that `echo` then shows whole; the same line typed fast into terminal 2 at 67 columns | |
| `comp-restore.txt` | the compositor killed with windows on two screens: terminals 1 and 2 side by side (Super+Enter twice, the third moved to a second screen by Super+Ctrl+Shift+Right, Super+Alt+Left twice pushing the gap), the keys with terminal 2; `kill compositor`: "compositor: layout remembered: 3 windows on 2 screens", "compositor: layout restored: 3 windows in place, the keys with "Terminal 2"", typing reaching terminal 2; screenshots `restore-before` and `restore-after` (the same places), `restore-screen2` (terminal 3 alone) | `QEMU_USB="-device usb-kbd,bus=xhci.0,port=2"` |
| `terms-windows.txt` | terminals as windows: the first terminal's window, `term`'s window taking the keys, `exit` closing it and the keys back to the first; `exit` closing the first too, the desktop with no terminal (`terms-none`), Super+Enter on a USB keyboard opening terminal 1 again and then terminal 2, the compositor killed and both windows back; screenshots | `QEMU_USB="-device usb-kbd,bus=xhci.0,port=2"` |
| `jobs.txt` | programs in the background (`prog &`): the prompt back at once, their output on the screen, the "[n] done" notice and its exit code, one outliving a foreground program, Ctrl+C reaching only the foreground, `jobs`, `kill %n` (and `kill <name>` unchanged), the refusals (a pipeline, a shell command, an alias), wltest in a window of its own (its keys, q), the limit of 8, and background programs ending with their shell | |
| `hda.txt` | the HD Audio driver's dump, `hda`, `kill hda`, `hda jacks`, `hda gain` and `hda bits` set and read back (all through the mixer's query channels) | use `tools/hda-test.sh` |
| `vtdtest.txt` | the IOMMU checks boot (`iommu=on vtdtest`): `iommu`, then `hda` still answering after drv/hda's checks | use `tools/hda-test.sh` |
| `hdastream.txt` | `hdatest`: the HD Audio output stream (open, a pattern played, a running stream closed, the driver killed mid-stream) | use `tools/hda-stream-test.sh` |
| `play.txt` | `play` of six WAV files on `/data` (four played, garbage/cut/float refused, Ctrl+C) | use `tools/play-test.sh` |
| `quality.txt` | `play -s` of known signals on `/data` | use `tools/audio-quality-test.sh` |
| `mp3.txt` | `play` of MP3 files on `/data` (seven played, noise and an ID3 tag with noise refused, Ctrl+C, `play -n`) | use `tools/mp3-test.sh` |
| `beep.txt` | `beep 440 500` and `hda gain` (shown, set, clamped at both ends) | use `tools/beep-test.sh` |
| `mixer.txt` | `vol` (nothing playing, the master set and clamped, a missing stream, usage), then `mixtest` | use `tools/mixer-test.sh` |
| `verdict.txt` | the shell scripts' verdict on a failing utest ([above](#how-a-scripts-verdict-is-reached)): `utest only verdict_canary` made to pass, fail (loudly or quietly) or crash, in a script that looks at nothing itself | use `tools/verdict-test.sh <outdir> <mode>` (mode ok, fail, quiet or crash) |
| `audio-api.txt` | `utest only audio_out` (11 cases of libos's sound output API: float samples, the paced writer, pause, two streams) | use `tools/audio-api-test.sh` |
| `desk.txt` | the desktop's plumbing with real USB input and QEMU's network: netstack's Connected notice with the peer's lease, `notify -w` answered by a click on its card's button, the text bar over a terminal's text (`desk-text-cursor`), `launch` refusing what isn't a desktop app, the search box starting Jamjar through init (busy until its window) and Super+Q ending it, its "Run ... in a terminal" row opening terminal 2 whose shell runs the line (clicks at 1280x800) | use `tools/desk-test.sh` |
| `readme.txt` | the README's desktop pictures at 2560x1440: Jamjar playing a stick's library beside two tiled terminals (`ps`, `uname -a`), the search box open over that desktop, then the same windows floating (Super+T: each where its tile was, none overlapping; the geometry is in its header) | use `tools/readme-shots.sh` |
| `mixer-restart.txt` | `mixramp 20 300` (a ramp played while init kills the mixer 20 times, other calls made meanwhile), then `vol` | use `tools/mixer-restart-test.sh` |
| `mixer-spare.txt`, `mixer-nospare.txt` | the mixer's warm spare: `ps` with the spare, `kill mixer` once and 12 times in a row, `kill devmgr` (the mixer exits by itself), `vol` after each; then, booted with `nospare`, five kills each starting a new process | use `tools/mixer-spare-test.sh` |
| `fat-restart.txt` | fat carrying on from a dead instance's state (`user/services/fat/adopt.c`), on `/data` through volumes: files written and never synced, `kill fat-data` right after, read back; an append and two kills; the kernel and the bootfs copied, kills, their checksums; bursts of five kills between appends; `sync`, `reboot -f` | use `tools/fat-restart-test.sh` |
| `extfs-svc.txt` | the extfs service: `mkdir /data/extfs-e2fsck`, `utest only extfs` (11 cases, each saving its disk there), `sync`, `reboot -f` | use `tools/extfs-svc-test.sh` |
| `extcmds.txt` | the shell's file commands on an ext volume, `/jam` (M13.6's U8; run by `tools/extcmds-test.sh`, which needs U7a's mount): `ls -l` (modes, link counts, owner, a link's target) over a tree jamext made, `stat`, `ln` and `ln -s` (into a directory, through a directory link, a dangling one, one that leads out of the volume said), `readlink`, `chmod` (octal and symbolic) and its refusals, hard links made and unlinked, `allow` and `run` of a program on `/jam`, `disk check /jam` (no problems) and its refusals (`/data` FAT, no such mount, not a mount) | |
| `fat-spare.txt`, `fat-nospare.txt` | volumes' warm spare fat: `ps` with the spare; a file written and synced, `kill fat-data`, the file read back, nine times; `kill fat-esp`; ten kills of fat-data in a row; the files all there and logd's log on `/data` still growing; then, booted with `nospare`, five kills each starting a new process | use `tools/fat-spare-test.sh` |
| `volumes-restart.txt` | volumes started again, and what is under it, on the boot stick: `kill volumes` (its filesystem services go with its job, `/data` and `/esp` leave the namespace and come back once the new volumes has been told of the disk again, logd carries on in its file), `kill devmgr` (its drivers go, usb-storage with them, so do the mounts, and they come back the same way), `kill usb-storage-1:0` (devmgr restarts it, volumes mounts the disk again, and no "Jam OS stick removed" notice: the stick never went) | use `tools/volumes-restart-test.sh` |
| `ctl-wire.txt` | the wire of init's channels and logd's (`initctl_strings`, `splashch_wire`, `crashres_wire`, `logd_crash_result` and `update_offer_call`: [init's channels](#the-wire-of-inits-channels)) | |
| `pkgfmt.txt` | the ports formats' readers (`user/lib/pkg*.c`), each utest alone (`pkgfmt_vectors`, `_misc`, `_table_is_allowd`, `_lists_are_allows`, `_note_is_wants`, `_journal`, `_fuzz`): PO3's vectors, 10,000 mutations, the journal, and the parts Jam OS has twice (the page table, the refused lists, the ELF note) against their other copy | |
| `pkgctl.txt` | the packages' guard and init's approvals (M13.7's PO5), each utest group alone: `view_pkg_names`, `fat_view_pkg` (FS_VIEW_GUARD_PKG refuses every change under the top-level `pkg` by every name FAT takes for it, `PKG`, `pkg.`, ` pkg`, `x/../pkg`, a short name; reads pass; a view without it, bin/pkg's, changes it), `extfs_views` (the same where a path resolves, through a link), `pkgctl_good` .. `pkgctl_other_version` (init's pkgapprove.c over a RAM disk with the vectors' key: a good package's tagged line and its table, the very bytes shipped; no second line; staged at `.new`; another package's line on the path refused; revoke; refused with nothing written: a changed byte, a length, a missing file, a changed table, another key, unsigned, a list that isn't the program's own, lists no `/data` program may have, a replayed manifest of another version), `pkgctl_settle` (a journal cut before the commit point undone, after it finished, a remove finished, a torn one left), `pkgctl_want`, `pkgctl_init` (init's own channel: the vectors' package refused, the build's key isn't theirs; a newer channel closes the older), `pkgfmt_lists_are_allows`; then a package's line written into `/data/etc/allow` as init writes it: `allow -l` shows `[package vec 1.0-r1]`, `allow -r` and `allow <file>` over it refuse | |
| `pkgtxn.txt` | bin/pkg's transactions and plans ([M13.7 PO6 notes](M13.7-PLAN.md#po6-notes)), each utest group alone: `pkgtxn_install` (a package installed over fat on a RAM disk against init's own pkgapprove.c, signed by the vectors' test key: files, record, tagged approvals, the package's hash kept as a sum; again refused; fed in pieces of 1 byte to whole), `pkgtxn_db` (the serials, a serial.new left by a cut, a damaged file, the sums), `pkgtxn_refused` (a byte changed in the manifest, a header, a file, a table, the end; the index naming other bytes; another key; Ctrl+C; a package cut short: nothing left), `pkgtxn_init_no` (init's refusal, a name pkg keeps, a path under db/ or ending in `.new`), `pkgtxn_upgrade` (changed, added, removed files; the old approvals revoked; a rebuild newer by hash; back to 1.0), `pkgtxn_remove` (files, empty directories, record, sums), `pkgtxn_busy` (a file held open: the upgrade waits in the journal at the commit point, listing it; the settle says busy, then finishes), `pkgplan_install`, `_upgrade`, `_remove`, `_paths` (dependencies first, a dependency below the version asked for upgraded, a cycle, unknown and reserved names refused, newer by version, revision or hash, dependents first, files two packages share by FAT's names) | |
| `pkgcut.txt` | bin/pkg's transactions with the power taken away before each change to the prefix in turn (`pkgcut_install`, `_upgrade`, `_remove`; 39, 44 and 29 changes, and a torn write at each write), then the next boot's settle by init's `pkga_settle` and, in another run, bin/pkg's own: the package is exactly the old one or exactly the new one (files, record, approvals, nothing left over) and the two settles go the same way; `pkgcut_double` cuts the settle of a cut upgrade again and again | |
| `fbbench.txt` | `fbbench -q` from the shell: it opens a full-screen window, measures and closes it; each section's lines are there, none FAILED, the shell comes back; then a usage error (the numbers are QEMU's, meaningless) | |
| `wltest.txt` | `run wltest --spawn --script`: wltest's scripted checks against a compositor it starts headless itself, killed half way; libjwl connects again by itself | |
| `storm.txt` | `storm` (the copy under kills): `/usb0` made writable, then a file copied from it to `/data` at 0, 1, 10 and 100 kills a second (fat-usb0 and fat-data in turn), each MATCH; one stopped by Ctrl+C (130); a rate too high (2); `storm mixer 2 10` while the music player plays a WAV from `/data/music`; `music stop`, `sync`, `reboot -f` | use `tools/fat-storm-test.sh` |
| `music.txt` | the music player: usage, errors, `music start` with the shell in use meanwhile (`ls`, `beep`, `vol`, `music status`, `music vol`, `music next`), past one whole shuffle, `music stop`; it plays on through Ctrl+C, `kill shell` and `kill mixer`; `music prev`, `pause` (and `status` paused), the sleep timer set, shown, off, and 33 s that runs out; a second stick pulled mid-song | use `tools/music-test.sh` |
| `fetch.txt` | `fetch`: files saved, a pipe, redirects, hostile servers refused, Ctrl+C | use `tools/fetch-test.sh` |
| `serve.txt` | `serve`: two files in the background (one on port 80), its refusals, its list and `serve stop`, while the Mac fetches; `run nolisten` refused | use `tools/serve-test.sh` |
| `serve-nonet.txt` | `serve` with no network card: "no network address yet" after bin/serve's 5 s wait | use `tools/serve-test.sh` |
| `speed.txt` | `speed`: TCP both ways, UDP, a refused port, `speed -l`, Ctrl+C | use `tools/speed-test.sh` |
| `usb.txt` | the `usb` command | `QEMU_USB="-device usb-hub,bus=xhci.0,port=2 -device usb-kbd,bus=xhci.0,port=2.1"` |
| `usbdirty.txt` | `usbtest` run from the shell right after three copies onto `/data` (fat still holds the writes back): usbtest asks volumes to stop the boot stick's mounts in order before devmgr lets go of the disk, so the stick is not left mid-command and every storage check passes (the run ends on `exited with code 0`). Without usbtest's `svc volumes` want, devmgr killed usb-storage under the write-back about one run in four and six storage checks failed | |
| `usbkeys.txt` | typing on a USB keyboard behind a hub; kill hid, the compositor (hid reconnects to the new one), the console, devmgr | use `tools/usbkeys-test.sh` |
| `review-cad.txt` | a `run` program can't send Ctrl+Alt+Del | |
| `review-killinit.txt` | the shell refuses to kill init; supervised services come back | |
| `review-longline.txt` | a line longer than the screen row: kept on its one row, scrolled sideways (`longline`), edited at its end, and run whole | |
| `review-runshell.txt` | Ctrl+C reaches the outer shell past a second shell | |
| `review-steal.txt` | a `run` program can't take devmgr's console | the `tools/usbkeys-test.sh` `QEMU_USB` (hub on port 2, `usb-kbd,id=keys` at 2.1, mouse on 3) |

## The wire of init's channels

The channels init and logd share with the rest, tested as bytes on the wire
(utest, the `init` run; `tools/shell-tests/ctl-wire.txt` runs them from the
shell with `utest only <name>`, [above](#shell-scripts)). Each names its protocol's file:

| Test | What it checks |
|---|---|
| `initctl_strings` | on the live init (needs the shell's `svc init`, skipped in the `init` run): every malformed form of `kill`'s name, `mount`'s path, `terminal`'s command and `launch`'s app is refused (a count past the cap, bytes left over or missing, bad UTF-8, a NUL, a space or a control character, a path that is no stick's, an app not on the list), the old fixed-array `kill` too; init keeps answering and nothing is started, killed or remounted |
| `splashch_wire` | init's side (the generated server, as init runs it): `played` runs its handler once; the older one-word forms, a call with a txid, the other event's ordinal, another protocol's, bytes after the header and a handle sent with it run nothing; the splash's reader takes `go` the same way and nothing else |
| `crashres_wire`, `logd_crash_result` | init's reader of logd's result (`crashlog_read_result`): nothing queued is `ERR_SHOULD_WAIT`; the older struct, a txid, a path past 95 bytes or not UTF-8, a count past the bytes, bytes after and a handle are each dropped and read past, then the good result comes (a path with control characters made printable, a status above 0 an error); a channel that closes with no result is `ERR_PEER_CLOSED`. With the real logd: given a panicked boot's log and a result channel it saves `<name>-crash.txt` and sends one `result` event (status OK and the path); a VMO that is no log is `ERR_INVALID_ARGS` with no path, and the boot's own file is made all the same |
| `update_offer_call` | the `offer` call of `update` between the fetcher's half (`update_make_offer`) and init's (`update_answer_reply`) over a channel with a thread answering `later`: every field of the offer reaches the server and every field of the answer comes back, an answer whose text is not printable ASCII still goes with `?` for those bytes, the client refuses a manifest past 1024 bytes and a fourth VMO before sending, the server refuses a handle that is no VMO, and the VMOs of a refused offer are gone from the caller's table all the same. Init's own check of malformed offers is `updtest wire` ([update-test.sh](#area-scripts)) |
| `ns_long_names`, `heap_large_alloc` | a mount point of 30 name bytes and a service name of 24 are accepted and travel as `nschan` events whole, one byte more is refused; libos's heap reserves 1 GiB of address space and commits only what is touched (a 256 MiB block whose first and last pages are used costs two pages of the job, three fit, the fourth is NULL, not a crash, and freeing makes room again) |

## netstack

Most of netstack's tests are in utest, part of the `init` run
([the tiers](#the-tiers)), with no QEMU network card
([HISTORY: M9](HISTORY.md#m9-networking)). The
`netstack_*` tests link netstack's core (`user/services/netstack/stack.c`,
`ctl.c`, lwIP) and drive it in-process over a fake edge that catches
every frame lwIP sends; the `netdrv_*` tests run bin/netstack as a
process over a fake driver (the test plays devmgr's device channel and
the driver: real ring VMOs and events, frames into the rx ring and out of
the tx ring). Every frame is checked byte by byte (`user/tests/utest/netpkt.c`),
checksums included, and must be untagged and 60 bytes at least.

| Test | What |
|---|---|
| `netstack_arp` | an ARP request for our address answered (a 60-byte reply, zero-padded); one for another address, and an unasked-for reply, not |
| `netstack_ping` | an echo request from a known peer answered (56 bytes, and the biggest that fits: a 1514-byte frame); from an unknown peer, an ARP request first and the reply once the ARP reply comes |
| `netstack_udp_unreachable` | a datagram to a closed port answered with ICMP port unreachable (the datagram's IP header and 8 bytes quoted); 30 more at once get at most 11 answers (the rate limit, 10 a second); a broadcast one none |
| `netstack_malformed` | runt, oversized, VLAN-tagged, unknown EtherType, IP version 6, IHL 4 and 15, total length past the frame and under 20, bad IP and ICMP checksums, truncated, fragments (more-fragments, an offset), IP options, ARP not for Ethernet, a ping to the broadcast address: none answered, each counted; a cut-short ARP request right after a whole one not answered (lwIP would read the rest from the last frame without netstack's padding); afterwards a ping is answered and no receive buffer or heap byte is held |
| `netstack_fuzz` | 4000 pings, ARP and UDP frames with bytes flipped at random, cut short or grown (a fixed seed): no crash, no buffer or heap byte held once ARP's waiting packets are cleared, a ping answered after |
| `netstack_cleared` | after `clear` neither ARP nor a ping is answered; a new address is announced by one gratuitous ARP and answered again; with the link down nothing is sent |
| `nettcp_connect` | TCP in-process (the `nettcp_*` tests: `stack.c`'s TCP edge and `tcp.c`'s connections over the same kind of fake edge, the test as the peer, byte by byte with checksums, `user/tests/utest/tcppeer.c`, and as the program on the other side of each connection's rings, [HISTORY: M9.5](HISTORY.md#m95-network-streams)): netstack connects (SYN with MSS 1460, the handshake's ACK announcing the rx ring's window), 100 KB each way through 16 KiB rings, the program waking netstack only when it sleeps on a ring; netstack's FIN first (the program's END), then the peer's: CLOSED with no error, lwIP keeps the TIME_WAIT; nothing left in lwIP's buffers, segments or heap |
| `nettcp_listen` | a listener: the SYN-ACK announces the accepted rings' window (8 KiB, not lwIP's 64 KiB), bytes with the handshake's ACK wait in the ring until the program accepts; the peer's FIN first, then netstack's, the last ACK frees it (no TIME_WAIT); a backlog of 2 leaves a third SYN unanswered; closing the listener resets the two waiting; a SYN then is refused with a reset |
| `nettcp_slow_reader` | a 4 KiB rx ring: the window closes to 0 when it is full, bytes past it are dropped by lwIP and never held (no receive buffer in use), netstack sleeps on the ring past half full so the program's read of 3000 bytes wakes it and the window opens by 3000 at once; then 64 KiB through it, 700 bytes a read, every byte in the ring and none in lwIP at every step |
| `nettcp_reset` | a reset for our SYN: CLOSED `ERR_NOT_FOUND`; a reset out of the window ignored, then one in it: CLOSED `ERR_PEER_CLOSED`, the bytes before it still in the ring and no END; `ntcp_conn_abort` sends a RST; a connection freed with bytes unread sends one too (after the ACK lwIP was delaying) |
| `nettcp_card_full` | the card's tx ring full (the fake edge refuses, as a full ring does): the segments of a write stay in lwIP and a turn sends nothing; room for one slot sends one, room for all sends the rest at once (`stack_tx_resume`, as netstack does on the driver's room signal), not on lwIP's 250 ms timer; each byte once, in order, and nothing again after the peer's ACK |
| `nettcp_window_scale` | window scaling ([HISTORY: M9.5](HISTORY.md#m95-network-streams)): netstack connects to a peer that scales (shift 7): both SYNs carry the option, netstack's SYN lwIP's unscaled 65535, the handshake's ACK the 2 MiB rx ring's window; 100 segments (146000 bytes) from the peer without an ACK all land in the ring, and every ACK's window (shift 6) is the ring's room rounded down to 64 bytes, never more, read bytes given back; with a 256 KiB tx ring and the peer acking each segment, more than 64 KiB in flight from netstack, with a 64 KiB one never more than 64 KiB; a peer that doesn't scale gets 64240 |
| `nettcp_listen_scale` | a listener with 2 MiB rx rings: a SYN that offers scaling gets a SYN-ACK that does (its window field 65535, never scaled), then the scaled window, the peer's bytes past 64 KiB in the ring; a SYN that doesn't, a SYN-ACK without the option and 64240 |
| `nettcp_heap_shares` | lwIP's memory shared out: three connections with 2 MiB tx rings and a peer that never acks (the card taking nothing) get about 4.6 MiB into lwIP between them, two whole send buffers, and leave the heap's last part (1 MiB and 64 KiB, less their own first 64240 bytes); a fourth with a 16 KiB ring still sends all of it, and the last 64 KiB stays free |
| `nettcp_ooseq_sack` | segments past a hole kept, and SACK ([HISTORY: M9.5](HISTORY.md#m95-network-streams)): netstack's SYN offers SACK; with a peer whose SYN-ACK offers it, segments 2-9 past a hole at 1 are each answered at once with the hole's ACK and one SACK block growing over them, the ring holding only segment 0 and lwIP 8 receive buffers; past the hole a segment with no ACK flag and one acking bytes never sent are dropped before lwIP (`no_acks`, `bad_acks`), not kept; a second hole gives two blocks, newest first; the first hole filled, the ACK jumps past everything kept (nothing sent twice) and the ring has segments 0-9 byte for byte; the second filled, no blocks and no buffer held. A peer that didn't offer SACK: the same queue, ACKs without blocks. A listener's SYN-ACK offers SACK only to a SYN that did |
| `nettcp_ooseq_bounds` | a peer whose hole never fills: one byte at every other sequence number in a 64240-byte window keeps 44 receive buffers (its window in full segments), the rest cut and counted (`ooseq_cut`); filled, the next byte comes in order; a connection let go of gives its buffers back at once and keeps none after. Eight bulk connections (2 MiB rings) flooded past a hole: the first keeps exactly STACK_OOSEQ_MAX - STACK_OOSEQ_KEEP_BULK (768), five more 45 each and one the last 31, so the queues hold STACK_OOSEQ_MAX (1024) together, the last 128 buffers free and no frame ever without one; an ordinary connection meanwhile receives 60000 bytes in order; the first's hole filled, its 768 kept segments come out in order and exact and the ACK says where its queue was cut |
| `nettcp_syn_flood` | 500 SYNs at a listener with a backlog of 4: 4 SYN-ACKs and 4 half-open pcbs, no pcb refused; meanwhile a live connection moves 20 KB each way and another listener accepts and serves a connection |
| `nettcp_pool_full` | every listener 15 half-open connections deep (240), then programs' connects until lwIP's pcb pool is full (TIME_WAITs recycled first): a SYN then gets no pcb and no answer, a program's connect still gets one (lwIP recycles a half-open pcb, never a program's) |
| `nettcp_malformed` | on a live connection: a bad TCP checksum, data offsets 4 and 15, a header cut short, SYN with FIN, an ACK for bytes never sent and one from long ago (both dropped before lwIP, RFC 5961), a sequence number long past and one past the window, bytes with no ACK flag (dropped before lwIP, RFC 9293): no byte reaches the ring, nothing answers but an ACK, the drops counted; SYNs with broken options at a listener; the connection still carries bytes |
| `nettcp_fuzz` | 4000 segments near what a live connection expects, flags at random, bytes flipped (the checksum fixed half the time); every connection a mutation ends replaced by a new one: no crash, and nothing left in lwIP once the address is gone |
| `nettcp_nodelay` | a connection's Nagle option (`ntcp_conn_options`, in-process): a connection starts with it off, three 10-byte writes leave as three segments before the peer acks any; with it on the first leaves and the next two wait for its ACK (nothing sent by a timer) and leave as one 20-byte segment; switched off again, a held write goes at once |
| `nettcp_keepalive` | keep-alive (in-process, with idle 1 s, interval 1 s): off until asked, nothing is sent to an idle connection; on, a probe (an empty segment one byte before the next sequence number) after the idle second and not before, an answered one keeps the connection and starts the idle time again, off again stops them (2.5 s of silence), and a peer that never answers gets its 2 probes and then a reset: CLOSED, `ERR_TIMED_OUT` (about 9 s) |
| `nettcp_options` | `net_stream.set_options` through bin/netstack over the fake driver: a bit that is no option, a time past 32767 s or a count past 127 refused (`ERR_INVALID_ARGS`); the largest values accepted; Nagle off then on seen on the wire (three segments for three small writes, then one, then the other two as one after the ACK); nothing sent before keep-alive is asked for, then with a silent peer two probes and a reset, the connection CLOSED with `ERR_TIMED_OUT`, a closed connection taking the call and ignoring it |
| `nettcp_reuse` | a listener's address reuse (`NET_LISTEN_REUSE`, SO_REUSEADDR, in-process): two active listeners never share a port (with the flag or without); a connection the program closed first leaves a TIME_WAIT that refuses a listener without the flag (`ERR_ALREADY_BOUND`) and not one with it, which accepts a new peer at once; open connections of a listener made with the flag let a listener with it stand beside them and refuse one without it; open connections of one made without it refuse both, until they are gone |
| `nettcp_reuse_api` | the same through bin/netstack on the fake driver: a flag that is none (2, all bits) is `ERR_INVALID_ARGS`; a second listener on a listening port `ERR_ALREADY_BOUND` with the flag too; the program closes its connection first (FIN, the peer's ACK and FIN, ours), the listener closed: a listener without the flag is refused, one with the flag made on the same port |
| `nettcp_limits` | programs' TCP calls on bin/netstack over the fake driver (as the `netsock_*` tests): `tcp_listener` refused without the listen permission (on `/svc/net` and `/svc/net-sys`) and below port 1024, a bad backlog or ring size refused; 4 listeners an opener, the ordinary openers' backlog stopping at 96, port 0 picked from 49152; `accept` at once (`ERR_SHOULD_WAIT`), timing out, and one at a time (`ERR_BAD_STATE`); port 0, broadcast, loopback's broadcast and a bad ring size refused to `tcp`; 512 connections an opener, the ordinary openers' 768, a system opener's from the reserve up to 1,024 in all, then refused; an opener's end resetting its connections (CLOSED, `ERR_PEER_CLOSED`, kept until closed); every count back to nothing |
| `nettcp_bulk_rings` | a listener with 2 MiB tx rings (`NET_TCP_BULK`) and five connections from the test as the peer on the fake driver: as many as its opener's 8 MiB hold get 2 MiB tx rings, the rest the default 16 KiB one (rx as asked), none reset |
| `nettcp_hostile_ring` | the program writes a tx count far past its ring, an rx count past what was put there, or flags nobody defined: the connection is reset before a byte of it is sent (a RST, CLOSED `ERR_OUT_OF_RANGE`, the ring error counted); bytes after its own END are ignored (its FIN went, nothing follows) |
| `netctl_set_and_clear` | the control channel served by `ctl.c` on a channel of the test's own: `set_ipv4` and `set_dns` show in `info`; 13 addresses a host can't have and 2 bad DNS servers refused, nothing changed; a /30 and a /8 taken; `clear`; a request of the wrong size refused |
| `netctl_process` | bin/netstack started with its control channel at `NETSTACK_SR_CTL`: set, a refusal, `info` (no device, link down), `stats` (nothing sent or received), `clear`; killed, its job empty |
| `posixfds_round_trip` | the POSIX hand-over ([`<posixfds.h>`](../user/include/posixfds.h)) of one descriptor of every kind there and back: records, data and handles (by koid) as sent, the state and the directory; none at all; nothing queued is `ERR_NOT_FOUND` |
| `posixfds_many_handles` | 70 descriptors, 180 handles: three messages or more, every one arriving in order |
| `posixfds_malformed` | 45 ways of breaking a good two-message hand-over (header fields, the directory, the state, each record rule, handle counts and types, padding, lengths, a message missing, one after the last, garbage), each refused whole: the sink reset, nothing queued, every handle closed; a sink that refuses gets its error back the same way |
| `posixfds_fuzz` | 10,000 random hand-overs of 1 to 3 messages, most spoilt (bytes flipped, cut short, a message or a handle dropped): each read refused with everything closed, or taken with every descriptor of its kind's shape |
| `posixfds_writer_refuses` | the writer refuses a descriptor twice, a bad path or name, a descriptor number past the table, an unknown kind, a relative directory, SIGKILL in the mask, with every handle closed and nothing sent; a fault in its second message after the first went (the reader then refuses the hand-over); more than 1024 descriptors |
| `posixfds_spawn_child` | `spawn` with a hand-over of 41 descriptors (81 handles): the child (`utest posix-handover`) reads it before its work and answers on its descriptor 0, a pipe |
| `spawn_stack_size` | `spawn_args.stack_size` 8 MiB: the child (`utest posix-stack`) writes 7 MiB of locals; with the default 128 KiB it is killed at the guard page; more than `SPAWN_STACK_MAX` refused, the hand-over's handles closed |
| `wants_posix` | the list's word `posix` (`<wants.h>`): once, alone on its line, granting nothing; `svc session` parses as a service |
| `ipv4_text` | libos's `<ipv4.h>`: dotted quads (10 malformed ones refused), formatting, and init's `net.address` form (8 malformed ones refused) |
| `http_url` | libos's `<http.h>` (`fetch` and `serve`): URLs (the scheme and host in any case, no scheme as http, a query kept, a fragment cut; https and ftp `ERR_NOT_SUPPORTED`; 12 malformed ones and one too long refused), the file name a URL saves as (never "", "." or "..", unsafe bytes made `_`, cut to the buffer), Locations resolved (relative, absolute path, `//host`, absolute, https refused) |
| `http_response` | a response's head: where it ends (CRLF, bare LF, not yet, none within 16 KiB), Content-Length, chunked winning over a length, `Connection: close`, HTTP/1.0, Location, a 204; refused: two lengths that disagree, a list, a negative or overflowing or over-2^50 length, a folded line, a space before the colon, no colon, an empty name, a bare CR, a NUL, HTTP/2.0, statuses 99, 600 and 2000, a double space, ICY, two Locations, one with a space, a blank line first, 65 headers; `ERR_NOT_SUPPORTED` for gzip and a doubled chunked |
| `http_chunks` | a chunked body with extensions and a trailer decoded whole and in pieces of every size from 1 byte: the same bytes; bare LFs; one not ended yet; refused: a size that isn't hex, no size, no CRLF after a chunk, a size over 2^50 or 17 hex digits, a space inside the size, a control byte in an extension, a bare CR in a trailer; a size line and trailers that never end |
| `http_request` | a request's head: GET and HEAD (case-sensitive: `get` is 501), HTTP/1.0 closing, `Connection: close`, `Content-Length: 0`; 501 for POST (the target still logged), 505 for HTTP/2.0, 400 for no Host in 1.1, two Hosts, a body (a length or Transfer-Encoding), a double space, a control byte, an extra word, no version, a folded line; ranges `a-b`, `a-`, `-n` and past the end, 416 for one starting past the end and `-0`, the whole file for two ranges, `b<a`, another unit and garbage; Content-Types by extension |
| `http_fuzz` | 3000 random heads (half after a good status line) into every parser and the chunked decoder, and 20000 good heads with a byte changed: nothing read past its bytes, nothing taken for more than it is (a status 100..599, a length within 2^50, a chunked length -1, every string terminated) |
| `netdrv_ping_and_link` | netstack finds the card through the device channel (GET_SERVICE), netdev.info and netdev.open; ARP and a ping through the rings; the link going down (`NETDEV_SIG_LINK`, netdev.info on the session): nothing sent; up again: the address announced, pings answered |
| `netdrv_link_flap` | 40 link changes: netstack logs the first 11 and then one in 64, as the drivers do, not a line each (the log reader counts them); still answering after |
| `netdrv_restart` | the driver's session closed (a driver restart): netstack asks devmgr again, opens a new session, announces its address, keeps it and its ARP entries; `netctl.device` counts 2 sessions |
| `netdrv_hostile_driver` | rx slots of length 0, 1515 and 13 and with flags set: refused, counted, nothing answered, pings still answered; an rx `produced` five rings ahead and a tx `consumed` ahead of what was sent: netstack ends the session itself, counts the ring errors and opens a new one, which works; killed, its job empty |
| `netsock_udp` | programs' sockets on `/svc/net` (the `netsock_*` tests run bin/netstack over the same fake driver, with the test as a program that opens its own channels with `svc.connect`, [HISTORY: M9](HISTORY.md#m9-networking), on `/svc/net-listen`: they take fixed ports; the datagrams go through each socket's rings, `user/include/sockring.h`): a port taken (`ERR_ALREADY_BOUND`), one below 1024 refused, an ephemeral one from 49152; a datagram in (sender, port, bytes) and out (ports, bytes, the UDP checksum), the biggest (1472 bytes) too; `net_recvfrom` at once (`ERR_SHOULD_WAIT`) and at its timeout; a loop's socket (`net_sock_bind`) whose key fires when the datagram comes; `sock_connect`'s filter and default peer; broadcast, the subnet's broadcast, loopback's broadcast, multicast, port 0 and 1473 bytes refused (the blocking `net_sendto` returns netstack's reason), nothing sent (a datagram to 127.0.0.1 is taken, and no frame goes to the card) |
| `netsock_ping` | `echo`: the request's bytes, the reply's round trip, TTL and size; two openers pinging the same peer with the same seq get different ids, and a reply with one's id never answers the other (it times out); a reply after its timeout goes nowhere; an ICMP unreachable answers `ERR_NOT_FOUND`; the same seq twice refused; bad address, timeout and size refused; libos's blocking `net_ping` answered and timed out |
| `netsock_iface` | `iface` (address, gateway, MAC, VLAN 21, 1000 Mb/s, the device and the link); `wait_change` answered by `set_dns`, by its timeout, and at once for an old version; `chip_counts` (netstack asks the driver's `netdev.stats` and answers); `net_wait_up` at once, timing out after `clear`, and answered by `set_ipv4` |
| `netsock_limits` | the limits and the ordinary programs' shares ([ARCHITECTURE](../ARCHITECTURE.md#networking)): 24 ordinary openers (from `/svc/net` or `/svc/net-listen`), the 25th refused on both, 8 system ones (`/svc/net-sys`), then refused; 256 sockets an opener; the ordinary openers' 384 sockets, then refused; a system opener's up to 512 in all, then refused; one closed, one more opened; an opener's end ends its sockets (their status line CLOSED, `ERR_PEER_CLOSED`) and frees its slot; 8 requests in flight an opener, the 9th refused at once; afterwards no opener, socket, request or ring byte left |
| `netsock_caps` | an opener's cap and two openers at theirs: one ordinary opener opens 256 datagram sockets (default rings) and the 257th is `ERR_NO_RESOURCES`; a system opener opens 256 more (512 in all, `sockets` and `ring_bytes` counted) and then it and a third are refused; the last socket (a slot far past 255 in netstack's port keys) sends a datagram out through the card and receives one; every socket closes and the counts go to nothing |
| `netloop_udp` | loopback ([ARCHITECTURE](../ARCHITECTURE.md#networking)): datagrams both ways between two sockets over 127.0.0.1 (the sender's address and port seen), 127.0.0.5 answered from 127.0.0.1, a connected socket's peer filter on 127.0.0.9, the block's broadcast refused, a datagram to a closed port taken; the same with no address on the card; a loopback-only socket (`net_socket.loopback_only`) that a datagram from the network to the card's address does not reach and that cannot send to the network (`ERR_INVALID_ARGS`); an ordinary socket still hears the network; `tx_frames` unchanged and no frame in the card's ring for all of it |
| `netloop_ping` | a ping to 127.0.0.1 and 127.0.0.5 answered (with a TTL), to the block's broadcast `ERR_INVALID_ARGS`, nothing on the wire |
| `netloop_tcp` | a connection over 127.0.0.1: connect, accept (peer 127.0.0.1 and the client's port), `net_tcp_ends`, 300 KiB each way through the rings (more than a window) checked byte by byte, an orderly close (FIN both ways, both sides `net_tcp_wait_closed` OK); a connect to a port nobody listens on `ERR_NOT_FOUND`; a connection made to 127.0.0.5 with no address on the card; nothing on the wire; a loopback-only listener (`NET_LISTEN_LOOPBACK`) that the host reaches and a SYN from the network to the card's address finds closed (RST), where a listener on every address answers the SYN (SYN-ACK); every count back to nothing |
| `netloop_martian` | frames from the network with a loopback address as source, as destination or as both are dropped before lwIP and counted in `rx_refused` (3), the datagram after them is taken |
| `netsock_hostile` | on an opener's, a socket's and the shared channel: 2 bytes (no answer), 6 bytes, an unknown method, another protocol's, a request a byte short, 8 KiB, a request carrying a handle: each refused; each kind of channel refuses the other's methods (the shared one answers `iface`, not `udp`, `udp_rings` or `sock_rings`); a socket's rings asked for twice, and ring sizes that aren't (1, 2048, 5000, 512 KiB, 4 GiB); closing a socket asleep on its rx ring, and an opener with an echo and a wait in flight, then their answers arriving: nothing breaks, pings still answered, nothing left behind |
| `netsock_slow_reader` | the slow-peer rule: 12 datagrams of 1000 bytes to a socket with a 4 KiB rx ring nobody reads (4 kept, 8 dropped and counted, lwIP's receive buffers not held) and a socket asleep on a silent peer, while another program's datagram is answered at once (under 500 ms) and pings are answered; the ring then read oldest first, `dropped` 8 |
| `netsock_busy_client` | one opener's channel kept full of requests by a thread (written without waiting) while another opener's `iface` is answered in under 500 ms and pings are answered: a busy client holds up nobody else (netstack used to skip its port wait while any channel had more queued) |
| `netsock_short_counts` | `net_get_counters` and `net_get_chip_counters` over a hand-made netstack that answers a shorter array than the struct (the counts an older or smaller build has): what the answer doesn't carry is zero, never left as it was |
| `netsock_len_lies` | libos's `net_sock_take` over a hand-made netstack (a thread answering `sock_rings` for `net_sock_adopt`) whose rx records say more bytes than a datagram holds (1473, 65535): skipped and counted, never read past its datagram; a 1472-byte one between them taken |
| `netsock_shares` | the fair shares: one program takes every ordinary opener, socket and request in flight it can, and ring bytes (2 MiB rings: an opener's 8 MiB, then the ordinary openers' 16 MiB); a system opener (`/svc/net-sys`) still opens sockets with 2 MiB rings, waits, and has datagrams both ways |
| `netsock_hostile_rings` | a program writes garbage into its rings: tx counts far ahead, backwards, off a record's edge, an unknown flag; records too long (1473, 65535) and with flags; its rx ring's count ahead. Each counted in its status line (`ring_errors`, `rx_dropped`), nothing of it sent, another program served within 500 ms and pings answered meanwhile; its VMO handle can't shrink or copy the VMO, and once it closes its socket netstack has shrunk the VMO to 0 bytes |
| `netwait_udp` | real UDP sockets (`<net.h>`, `net_sock_waitable`) in a wait set: writable at once, readable when a datagram comes (netstack signals the flag the set raised), not again once it is taken, both sockets' datagrams reported |
| `netsock_bench` | datagrams through a socket and bin/netstack, the test as the program and as the card: one at a time (a round trip) and 16 in flight (a stream), 64 and 1472 bytes; prints µs a datagram ([BENCH.md](BENCH.md)); fails only if a datagram is lost or wrong |
| `netsock_dhcp` | netctl's `dhcp_open`: one at a time; port 68 refused to a program; a program's socket gets no broadcast and can't send one, and sends nothing with no address; with no address the DHCP socket sends a 300-byte datagram from 0.0.0.0:68 to 255.255.255.255:67 (the broadcast MAC), only to port 67, and hears the server's answer to the broadcast address and to an address it hasn't got yet; closed, another may be opened |
| `sockring_counts` | a socket's rings (`user/include/sockring.h`, [HISTORY: M9.5](HISTORY.md#m95-network-streams)), the counts as pure functions: empty, full, far into the stream; a producer's count backwards or over a ring ahead, a consumer's ahead or over a ring behind; the ring sizes and record sizes |
| `sockring_dgram` | 700 datagrams of every length each way through 4 KiB rings (one VMO mapped as netstack and as the program, a page nobody may touch after it): each as it went in, a full ring refusing the next without writing, records straddling the ring's end; a datagram too long or with flags or reserved bits refused |
| `sockring_stream` | 200 KiB each way in writes and reads of uneven sizes, then the producer's end: every byte, then the end, nothing written after it; bytes a producer adds after its end ignored and counted |
| `sockring_wake` | the flags: a sleeping consumer is signalled, an awake one not; a producer sleeps for room for its next datagram (some room is not enough) and is signalled; the end wakes a sleeping reader |
| `sockring_hostile` | a program's counts out of range (backwards, far ahead, off a record's edge, an unknown flag), each clamped and counted; netstack never reads its own count back; bad record headers (too long, flags, reserved, past what was published): everything published dropped, nothing copied; a header page that isn't the agreed one refused by the program; 3000 turns of garbage in the header page and the rings for each framing: counts only move forward, a ring a look at most, nothing read past the rings |
| `sockring_exchange` | a fake netstack thread echoing the tx ring into the rx ring, the test as the program with the rights netstack hands out (no resize, no duplicate, `to_stack` signal only): 3000 datagrams and 1 MiB of bytes (ending with the end both ways), both sides sleeping for data and for room and woken |
| `netwait_api` | wait sets (`user/include/netwait.h`, [HISTORY: M9.5](HISTORY.md#m95-network-streams)) over 64 fake sockets (`user/tests/utest/fakesock.c`: rings in one VMO, a `to_prog` event and a channel each, the test as netstack and as the program): bad interest bits, `tx_need`, rings missing or netstack's side, the same rings twice, a full set, ids of removed entries refused; a look and a 30 ms wait time out; with 64 entries asleep a signal on one wakes a wait that looks at that one entry only (twice while it stays ready, once more to put it to sleep: 3 looks, not 64 each); a wait sleeping until a helper thread's data or `netwait_wake`; 100 wakes count as one; ready entries win over a wake; ten ready, four a wait: all ten in three waits; a packet of a removed entry ignored, the entry put back reported; 256 entries ready at once and a 257th refused; the job's message bytes back where they were |
| `netwait_states` | a socket's life: connecting (not writable), open (writable, twice), interest changed, a stream's end (readable and `RX_END`, still after every byte is read), closed with `ERR_TIMED_OUT` (HUP and ERROR, twice), closed cleanly with no interest (HUP only), netstack's end of the channel closed (HUP and ERROR `ERR_PEER_CLOSED`, its rings no longer looked at); a socket with nothing to say waits through all of it; plain handles: a channel readable until read, not with no interest, HUP when its peer closes; an event's user bit |
| `netwait_level` | 1500 turns over 64 sockets (datagrams and streams): netstack moves at random (data in, data taken, a few streams ended), the program changes interests and takes entries out and puts them back; after every turn one look must report exactly what the rings say for every socket (computed from the shared counts by the test's own code): data left unread is reported again with no new signal, and a lost wake shows as a socket missing |
| `netwait_stress` | a fake netstack thread sending 160 datagrams or 40 KiB (then the end) to each of 64 sockets and taking 80 datagrams or 20 KiB from each, sleeping now and then, while the test blocks in the set (5 s deadline: a lost wake fails it) and moves everything, every report checked against the rings (never readable when empty, never writable when full), an entry taken out and put back every 50 waits, and netstack dying under one socket half way (reported HUP and ERROR); every byte checked both ways |
| `netlisten_udp` | the listen permission (netstack's `listen.h`, [HISTORY: M9.5](HISTORY.md#m95-network-streams)), over the fake driver: an opener of `/svc/net` gets ports 0 and 49152-65535 and is refused 123, 68, 1024, 5000 and 49151, and a datagram to its picked port arrives; an opener of `/svc/net-listen` gets 1024-49151 too (a datagram nobody asked for arrives on 5000), never below 1024; the listen channel's shared end answers `iface` and refuses `udp`; a port a listener holds is taken for everyone |
| `netlisten_wants` | `svc net listen` in a list: grants `/svc/net` and `/svc/net-listen`, shown "net, accepting connections from the network", no root powers; `svc net listen low` grants `/svc/net` and `/svc/net-low`, shown "..., below port 1024 too"; `svc net-listen`, `svc net-low`, `svc dns listen`, `svc net low`, extra or misspelt words refused |
| `netlisten_low` | the ports below 1024 ([HISTORY: M9.5](HISTORY.md#m95-network-streams)), over the fake driver: openers of `/svc/net` and `/svc/net-listen` are refused TCP listeners on 80 and 1 and UDP port 53; an opener of `/svc/net-low` listens on 1, 80, 1023, 8080 and 0, binds UDP 53 and 5000, and is refused the DHCP ports 67 and 68 |
| `sntp_request_and_reply` | bin/sntp's core (`user/services/sntp/ntp.c`): the 48-byte request (version 4, mode 3, nothing but the nonce); a good reply's time (the round trip less the server's time, halved, after its transmit time) at the reply's uptime; version 3; bytes after the packet; a server that held the request longer than the round trip (delay 0) |
| `sntp_checks` | each check failed alone on an otherwise good reply: short, mode 3 and 5, versions 2 and 5, the origin a bit off or zero, a kiss-o'-death (its code; no kiss without our origin), stratum 16, leap alarm, no receive or transmit time, root dispersion or delay too far, sent before received, uptimes backwards, a round trip over 5 s, a time in 2025; NTP era 1 (2036 on) |
| `sntp_times` | NTP timestamps and Unix ns both ways, 1970 to 2099, both eras, within the fraction's rounding |
| `sntp_fuzz` | 4000 random replies, from anything to plausible: never a time outside 2026-2199, never one without our origin, a delay never over the round trip; over 500 believed (the checks on a believed one ran) |

End to end, with QEMU's e1000e and the network peer:
`tools/netstack-test.sh <outdir>` (`tools/shell-tests/netstack.txt`)
writes `net.address = 10.2.21.5/24 ...` into a copy of the image's
settings, boots with `QEMU_NET=1` and the peer's `--ping 10.2.21.5`
(an echo request every half second, once it has seen the guest's MAC),
kills netstack in the middle (init starts it again: a new session, the
address again) and passes if the peer's and the pcap's VLAN checks pass
and at least 8 pings were answered.

The receive path over a long run: `tools/rxsoak-test.sh <outdir>`
(`tools/shell-tests/rxsoak.txt`, about 4 minutes) boots the same way
with the peer's `--flood 60 --ping-every 0.2 --late-after 150`: about
10 000 frames in three minutes (two thirds of them on VLAN 21), so the
e1000e's 256 descriptors and the 256-slot netdev rx ring go round dozens
of times (the PC's receive stopped at the end of the RTL8125's first lap:
[HISTORY: M9](HISTORY.md#m9-networking)). `net stats` twice, then the
guest's own `ping 10.2.21.1 -c 3` must get 3 replies; it passes if the
script and the VLAN checks pass, at least 6000 frames were flooded, and
at least 90% of the peer's pings sent 150 s or more after the first were
answered. The drivers' and netstack's `rx so far` lines (every 10 s while
frames come) are in the log. `RXSOAK_FLOOD` changes the rate.

The shell's `net` and `ping`: `tools/ping-test.sh <outdir>`
(`tools/shell-tests/net.txt`) boots the same way (without `--ping`):
`net` shows the address, the DNS server and VLAN 21; `ping 1.1.1.1 -c
3` goes through the gateway to the peer (which answers ICMP echo for any
address) and gets 3 replies; a ping of 1472 data bytes to the Mac's
address is stopped by Ctrl+C after 2; `-c 0` is a usage error; `net
stats` has the programs' and the card's counts. It passes if the script,
the peer's and the pcap's checks pass and the peer answered at least 5
echo requests.

netlog, the log to the Mac: `tools/netlog-test.sh <outdir>`
(`tools/shell-tests/netlog.txt`, about 30 s) writes `net.address` and
`net.host = 10.2.21.174` into a copy of the image's settings and boots
with `QEMU_NET=1` and the peer's `--netlog <outdir>/netlog` (port 5021
answered by `tools/netlog-recv.py`'s receiver, writing its files there),
`--netlog-late 5` (nothing answered until 5 s after the first datagram)
and `--netlog-pause 6000:6` (nothing for 6 s once a stream holds 6000
bytes). The script waits for netlog's lines (the Mac doesn't answer,
answers again, twice), kills netlog (init starts it again: it goes on in
the same file), kills netstack (netlog waits and goes on), then `crash
panic yes`: the next boot (kexec) sends its own log and the panicked
one's. Then the files are checked against the serial log: three files;
each boot's from its first line, every serial line in it in order (the
serial copy can have the shell's echo inside a kernel line, so each file
line must only be somewhere in the serial text), no receiver note (no
gap, nothing lost); the first boot's past netstack's restart; the
`-lastcrash` file with the panic and the first boot's lines; at most 10
of netlog's own lines a boot, fewer than 400 datagrams in all (a sender
whose sends made lines would never stop); and `tools/netlog-recv.py` run
on its own (`--quiet --bind 127.0.0.1 --port <free>`), fed every netlog
datagram of the run from the pcap, acks each and writes the same files.
It also needs the peer's and the pcap's VLAN checks to pass.

## The regression run's network

The boot menu's `init` entry (and `tools/qemu-test.sh ... init`) starts
only bootfs, devmgr and volumes, then runs `boot/init.cfg`'s programs.
Since the PC sign-off of 2026-10-11 that is not all it does when the
stick asks for the network: with `net.host` set to an IPv4 address and
`netlog` not `off` in `/data/etc/settings`, on a boot that has a network
(a `vlan=` word: not the "no network" entry) and a network card whose
device channel init holds (the PC's RTL8125; QEMU's e1000e with
`QEMU_NET`), init also starts, beside the programs
(`user/services/init/regnet.c`):

1. **netstack**, with its control channel, the shared channel of
   `/svc/net-sys` (the reserve of the network's own services) and a
   duplicate of each card's device channel. There is no `/svc/net`,
   `net-listen` or `net-low`: no program of the run may use the network
   (netstack says so once), and nothing is added to init's namespace, so
   the programs of the run get the namespace they always had.
2. **The address**: `net.address`, if the settings have a good one (the
   two netctl calls, as in a plain boot), else **bin/dhcp**.
3. **logd**, once `/data` is mounted, with `/data:w`: `/data/logs/boot-NNNN.txt`
   as in a plain boot. When usbtest takes the stick away from volumes and
   gives it back, init sends logd the new namespace as in shell mode, but
   only once the mount changes have been quiet for 300 ms (the update makes
   a view of `/data`, a call that can wait, and utest's mock disks make
   hundreds of changes that init must read at once), and logd retries by
   itself, as it does there.
4. **netlog**, with the root (kernel log only), a namespace of
   `/svc/net-sys` alone (published in init's namespace for the length of
   the spawn only) and the panicked boot's log if there was one. It reads
   the kernel log from its first byte, so the whole boot goes, the lines
   from before it started too.

init waits for `/data` (15 s at most) before the first program, and a
program that follows one that had `/data` waits up to 2 s for it to come
back (usbtest ends a few ms before volumes' remount reaches init, and a
program started in that gap gets a namespace without `/data` for good:
posixtest's `files` section failed that way in 2 of 6 runs with the network
on the loaded PC; a plain run has the same gap, with more margin), but only
on a boot that has a card and a network: a QEMU run without `QEMU_NET`, a
`vlan=off` boot and a boot image without bin/netstack or bin/netlog are
untouched, to the line. A boot with a card whose settings have no
`net.host` (or `netlog = off`) says so in one line, starts nothing, and
runs as it did. The network never decides a result: a service that
doesn't start or ends is a line in the log, and the run's exit code is
the programs' alone. Nothing supervises the services (the run is short).

**The end of the run.** When the programs are done, in this order: logd
flushes and is stopped (before volumes, which need `/data` to be quiet);
volumes stop (`init: volumes exited with code 0`); init says `init: the
run is over (...)`; netlog is asked (its stop channel is closed) to send
what the log holds by then, wait for the Mac's acks (4 s at most) and
exit; the DHCP client and netstack are killed (their jobs); devmgr and
bootfs stop. So the Mac has everything up to and including volumes' stop
line and `the run is over`: every program's `init: bin/... exited with
code N`, utest's, usbtest's and posixtest's lines and the root job's
neighbours. It does **not** get what the kernel prints once init has
exited: `init: exited with code`, `init: root job afterwards ... (clean)`,
`run complete: ...` and the RESULTS box itself (the box repeats the
`report()` lines the Mac already has, so only its frame and the counts
are missing). The root job has to be empty before the kernel checks it and
prints those lines, so no process can be left running to send them; to send
them too the kernel would have to keep the network services (devmgr, the
card's driver, netstack, netlog) alive past init's exit, print the box, wait
for a flush, and only then kill the root job and check it, which moves the
leak check after the kill: not done. The same lines are on the screen and on
the serial port as always. After a run on the PC the Mac's file ends at `the
run is over`; read the verdict from the `init: bin/... exited with code`
lines above it (and `utest:` summaries), or the screen.

**The other entries.** All tests (`ktest`), the stress tests and Benchmark
run no user space at all (the kernel's own tests, then the RESULTS box):
nothing can send a log, and there is no storage driver in the kernel to
write one to the stick, so the screen, the serial port and the RESULTS box
are all they have. Soak is a plain boot (`soak=3`: the shell runs the soak
by itself), so netlog and logd are running and its whole log reaches the
Mac and `/data/logs` like any plain boot's. For the kernel-only entries
there are two ways: run the same thing from the shell of a plain boot
(`ktest`, `stress <s>`, `bench` run on the live system and report into the
log netlog sends; the All tests entry's "strict, idle machine" is the one
thing that doesn't carry over), or give the kernel entries a user-space
tail: after the tests the kernel would start init in a `ship` mode (bootfs,
devmgr, volumes, then `regnet_start` / `regnet_stop` with no programs: the
log, the box's lines and logd's file, then exit) before it prints the box.
That is a small init change (regnet.c is most of it) and a kernel change
in `run_user_space` that makes the kernel-only entries depend on user
space, which is why they don't have it.

The test: `tools/initnet-test.sh <outdir> [static|dhcp|nohost|off]`
([Area scripts](#area-scripts)).

## Area scripts

Each prints PASS or FAIL and exits 0 on PASS; `QEMU_SMP` (and where it
matters `QEMU_XHCI`) pass through.

| Script | What |
|---|---|
| `tools/usb-test.sh <outdir>` | the `init` run with a hub, a test keyboard behind it, a CCID device, a mouse and a keyboard; the monitor script answers usbtest's markers (keys, kill hid, unplug, replug, unplug the hub); usbtest's `arrays` checks the limits of `usb`'s descriptor and control arrays (the stubs and usb-bus both refuse a count past them) and, on the unplug and replug, that the interface was reported on usb-bus's own events channel (`usbbus_events`, `DR_EVENTS`) |
| `tools/replug-test.sh <outdir> [name] [rounds] [once]` | a stick on port 2 pulled and pushed back in quickly, 81 times by default, by a driver on QEMU's monitor and the log: from usb-bus's "attached" line to the pull 0, 30 or 150 ms, from its "detached" line to the replug 0 to 100 ms (or the replug right after the pull); every replug must attach within 4 s. QEMU refuses a device_add of a drive whose last device it hasn't freed yet (some 70 ms after the pull): the driver tries again, as `serial-feed.py` does; `once` sends each device_add once, as `serial-feed.py` did before, and those replugs are lost (what `screen-test.sh`'s "stick is back" sometimes waited for). About two minutes |
| `tools/usb-early-test.sh <outdir>` | input early in boot (`usb-early.txt`): a plain boot with a hub (a keyboard, a disk and a slow keyboard behind it), a mouse and a slow keyboard on root ports; the slow ones have the serial number `jamos-test-slow`, which usb-bus treats as a device that doesn't answer (its first two attempts on a port each take a second and fail). The compositor's "input ready: the first keyboard and mouse" line must come under `EARLY_MAX` s (1.5) and before either slow device attached; both slow ports are tried again after 100 and 200 ms and attach on the third attempt; prints the times |
| `tools/bootdisk-test.sh <outdir>` | the boot disk with two Jam OS sticks in (`bootdisk.txt`): a copy of the image with another MBR disk id and a file of its own on its data partition, on qemu-xhci port 2; `/data` must be the boot stick's and the copy's data partition `/usb1`, before and after a `reboot` (kexec), and the kernel must name the boot stick's id both times (the second from the kernel before). With `QEMU_BOOT_NVME=1` (M12.7 stage 3) the boot disk is an installed drive, the image as Jam OS's GPT layout on NVMe (`tools/mkimage.py --gpt`), beside the other stick and a second Jam OS drive with another GUID and a file of its own: the kernel names the boot drive's GPT disk GUID from the loader and then from the kernel before, init gets `bootdisk=gpt:<GUID>` and so does the stored kernel's command line, both others wait for it, `/data` is the boot drive's, the stick's data partition `/usb1` and the other drive's `/nvme1` (read-only), before and after the `reboot`. `tools/bootdisk-test.sh <outdir> gone`: the boot names a GPT drive that isn't there (the stick's MBR disk id zeroed, so the kernel takes `bootdisk=gpt:<GUID>` from the boot words, as after a kexec from an installed drive): the stick waits for it and is then `/usb0` and `/usb1`, read-only, never the boot disk (no `/esp`, no `/data`) |
| `tools/storage-test.sh <outdir>` | the `init` run with two more usb-storage disks behind a hub: usbtest's storage checks (bulk transfers after the reset recovery a new owner of the pair needs, a pair opened again on a disk left mid-READ whose next CBW QEMU's stick refuses until reset recovery, a STALL and reset recovery, usb-storage taking over a disk left mid-READ with its reset recovery before its first command, a usb-storage killed with a READ in flight and a new one started on that disk, usb-bus clearing the device's halt on both endpoints when it opens a pair, the ESP's boot sector read through `block`, read-only and out-of-range requests refused, a write read back, several requests in flight on one `block` channel answered by txid in any order (`block_in_flight`), and the fence: WRITEs a client queued and left behind, while usb-storage was busy with another channel's READs, are dropped and never land, and usb-storage logs `its client left with 4 request(s) queued: dropped`), the second disk read while a READ waits on the slow one (usb-bus runs each device's transfers apart), the monitor script unplugging the second disk while it is being read, and READs timing out on a disk QEMU throttles to 4 KiB/s |
| `tools/usbkeys-test.sh <outdir>` | typing into the shell through usb-bus, hid, the compositor and the console (`usbkeys.txt`): the keyboard's hid, the compositor, the console and devmgr killed in turn, typing after each |
| `tools/apps-test.sh <outdir>` | programs that draw in windows (`apps.txt`), with a USB mouse; `APPS_HD=1` runs at 2560x1440 |
| `tools/wlapps-test.sh <outdir>` | programs as windows on a headless compositor of wlapps's own (`wlapps.txt`; wlapps shows each picture in a full-screen window of the system's compositor): two of wltest's windows tiled, wltest's mouse mode and fractal floating, jamjar resizing beside fractal (on a 1920x1080 output: jamjar's minimum is 944 wide), fractal full screen; PASS when each composition has their pixels, with a screenshot of each |
| `tools/jamjar-test.sh <outdir>` | jamjar (`jamjar.txt`), with a USB keyboard and mouse and an hda-output capture, on a library the script makes (six made-up stereo songs as MP3 under owner-style UTF-8 names with made-up covers in their tags, drawn with Python's PIL; a calibration track, 1 kHz on the left and 4 kHz on the right; two MP3s with a bad cover); it checks the calibration shot's bars with PIL; `JAMJAR_HD=1` runs `jamjar-hd.txt` at 2560x1440; `JAMJAR_CPU=1` runs `jamjar-cpu.txt` (the CPU while playing, with `JAMJAR_ARGS` for jamjar's words); screenshots `jamjar-*.png` |
| `tools/jamjar-covers-test.sh <outdir>` | jamjar's now-playing cover at 2560x1440 (`jamjar-covers.txt`), on 32 made-up albums of two MP3s, each with a teal PNG cover in its tag: every shot of now playing must show the cover's teal, not a jar label, where jamjar last said its cover is (`jamjar trace` says it again at each resize) |
| `tools/comp-test.sh <outdir>` | the compositor on QEMU's framebuffer (`comptest`): a screenshot of each of its test scene's steps (opaque floating windows on the wallpaper, a translucent one over them, the arrow on the first's circles; one moved, a tiled window's look, one raised, no arrow; a full-screen window copied straight from its buffer; the desktop: the top bar with three toplevels' chips (one focused, one minimised), the clock's popover, a notification with buttons and the hand on a chip; then the search box with "te" typed; then windows with subsurfaces: one framed round its whole tree, a child below it hidden, a translucent one with a child of its own, and one whose child is clipped to its window geometry) compared by `tools/comp-check.py` with the steps the log describes, painted from scratch with the look's numbers (wallpaper, shadows, title bars, outlines, borders, rounded corners: every third pixel each way; the titles' text found in their bars, each circle's colour or symbol where it is all circle; the arrow where it is all fill; the strip's frosting and islands pixel for pixel, the lit dot and chip, no toplevel under the strip, the popover 2 pixels under it with its right edge on its icon's, the calendar's today, the notification's tile and buttons, the search box's letter tiles, from the layout the test scene logs); prints the compositor's `bench:` lines (frame costs: QEMU's only show the code runs) |
| `tools/mouse-test.sh <outdir>` | two boots, both the compositor's: its seat with a USB keyboard and mouse (`desktop.txt`), and the mouse end to end into an app's window (`mouse.txt`): QEMU's monitor moves and clicks a USB mouse, driven in the report protocol (its descriptor has a wheel); `MOUSE_HIDBOOT=1` adds the boot word `hidboot` and checks the boot protocol instead; 1280x800 only (the clicks are at pixel positions) |
| `tools/clip-test.sh <outdir>` | copy and paste between terminal windows (`clip.txt`): a plain boot with a USB keyboard and mouse; PASS also needs the screenshots' tint where the selections are (#423e71: #7f77dd at 45% over #101018) and the background beside them; screenshots `clip-*.png`; 1280x800 only |
| `tools/resize-test.sh <outdir>` | a tile resized live (`tile-resize.txt`): the compositor's boot with a USB keyboard and mouse, two terminals side by side, the gap between them dragged 6 pixels a step, right 120 and back 240, 21 screenshots `resize-*.png` just after moves; PASS needs the top and bottom padding rows of both tiles to be the terminal's background in every shot (no wallpaper, black or half-drawn frame); QEMU's terminals mostly catch up before a shot, so the moments between are utest's `xdg_tile_resize` and `jwlc_window_sizes`; 1280x800 only |
| `tools/desk-test.sh <outdir>` | the desktop's plumbing (`desk.txt`): a plain boot with a USB keyboard and mouse and `QEMU_NET=1` (untagged, the peer's DHCP); PASS also needs both busy cursors ended by their windows (not the 10 s limit) and no `init: launch fractal: OK`; screenshots `desk-*.png`; 1280x800 only |
| `tools/crash-test.sh <outdir> [name...]` | every crash test from the shell (`crash <name> yes`), each on a fresh boot; each panic shows the panic screen's first case ("... is restarting", its code in the summary) and starts the stored kernel, whose shell says what happened (`kexecbad`: refused; `kexecstall`, `kexecfault`: the jump broken; each the second case, "... can't recover from", and the firmware reset 15 s later) |
| `tools/kdump-test.sh <outdir> [case...]` | a panic starts the stored kernel, each case a fresh boot of a stick image read afterwards with mtools: `save` (`crash panic yes` with `panichold=3000`: the panic screen's first case in `kdump-restarting.png`, the next boot comes up on every CPU, saves `/data/logs/boot-0001-crash.txt` with the panic, its code and the lines before it, logs to `boot-0002` and its shell says so with the code; `crashlog`, `crashlog list` and `crashlog 2` read it back, the first's output in `kdump-save-crashlog.txt`), `notice` (the same with a mouse: the desktop's "Jam OS restarted after a problem" notice in `kdump-notice.png`, its Details clicked: a terminal running `crashlog`, `kdump-details.png`), `loop` (`crashtest=lockorder`: the next boot panics at once, a crash loop: the second case, "Restarting the PC in 15 s" (`kdump-loop-0s.png`), its details at 5 s (`kdump-loop-5s.png`), the firmware reset at 15 s), `bad` (`crash kexecbad yes` with `reset=none`: the damaged stored kernel is refused, nothing saved; the second case at 0 s (`kdump-bad-0s.png`), its details at 5 s (`kdump-bad-5s.png`), the reset at 15 s, which fails: "Jam OS couldn't restart the PC" (`kdump-bad-noreset.png`); the times checked), `nostick` (the stick pulled first: the shell says the log was not saved and why, `crashlog` that there is no `/data/logs`), `screen` (with the splash: the screen as the next kernel starts is all the splash background) |
| `tools/kexec-reboot-test.sh <outdir> [run...]` | `reboot` by kexec: `kexec` (an unchanged stick: no file read, no firmware reset, the screen all the splash background as the next kernel starts, which brings up every CPU, plays the splash and reaches the shell and `/data`; the old boot's log ends with the reboot's sync), `changed` (the stick swapped for one whose kernel file is longer: both files read and loaded first, the screen blanked meanwhile), `load` (the stick swapped the same way, then `kernel load`: the files read and loaded at once, and the `reboot` after it reads nothing), `broken` (the stick swapped for one whose kernel file is cut short: `kernel load` refuses it and keeps the stored copy, `reboot` says so on the screen and starts the stored copy, no firmware reset), `firmware` (`reboot -f`), `fallback` (`crashkernel=0`: `kernel load` says there is no stored kernel, `reboot` falls back to the firmware) |
| `tools/reboot-firmware-test.sh <outdir> [run...]` | `reboot -f`, the reset through the firmware, which must end QEMU (`-no-reboot`) before the timeout: `data` (`/data` mounted and a file written: init syncs, flushes the log, stops volumes' filesystems and devmgr's drivers; the kernel halts the other CPUs, turns bus mastering off and tries the ACPI reset register, which resets the machine before any other method is tried; a second boot of the stick reads the file, `/data` was left clean, boot-0001.txt has the sync's line), `cf9`, `8042`, `triple` (the boot word `reset=<method>`: that method alone resets the machine). About 20 s |
| `tools/data-test.sh <outdir>` | the stick's filesystems end to end, three boots of one stick image (`data-1.txt` to `data-3.txt`): written, rebooted, read back; QEMU quit in the middle of writes and the dirty volume mounted again; then the boot logs read off the image with mtools, as the Mac reads the real stick |
| `tools/extfs-svc-test.sh <outdir>` | the extfs service (M13.6's U4: bin/extfs, `extfs-svc.txt`): utest's `extfs` cases from the shell, each extfs over a RAM disk it formats (files and readdir's types; symbolic and hard links, `ERR_LINK_OUT` and `fs.resolve`; the permission bits; many writers, `FS_APPEND`, unlink and truncate of open files; views judged where a path resolves, a link into `etc` refused as `etc` is, subviews; a damaged block turning the volume read-only with the error recorded; restarts at each step of a request, handles remade, the bad-request rule, the warm spare promoted; `extfs_posix`: the volume mounted at `/u6` and again, a view, at `/u6v`, and posixtest's `links` section run on it, M13.6's U6: libc's symbolic and hard links, links out of a mount and `..` above it, `ELOOP`, modes, owners and times, the umask, `RENAME_EXCHANGE`, an open file unlinked, an `O_APPEND` file written by a child and us at once, a sparse file past 4 GiB; `extfs_map` and `extfs_restart_pager`, M13.6's U10: extfs as a pager, mapped files kept right while they are written and truncated, a program's file `ERR_BUSY` to writers, an unlinked mapped file read on and dropped when its mapping goes, maps and page requests carried across restarts; they need the pager power, which a utest the shell starts lacks, so here they say "skipped" and the init run is where they run), each disk they leave saved sparse on `/data` and then made an image again on the host and checked by Linux's `e2fsck -fn` (needs e2fsprogs and mtools: the PC's WSL). PASS: 13 passed and every disk clean |
| `tools/extcmds-test.sh <outdir>` | the shell's file commands on `/jam` (M13.6's U8, `extcmds.txt`): a Jam OS drive (`tools/mkgpt.py` jamos layout, QEMU's NVMe) whose data partition `jamext mkimage` made from a tree (needs mtools, and U7a's `/jam`); then on the host `jamext --part 2 check --replay`, e2fsck -fn when installed, and the links, modes and file the shell made read back with jamext. `EXTCMDS_INPUT=<script>` runs another script instead |
| `tools/jamext-pc-test.sh <outdir>` | `tools/jamext-test.sh` on a machine with e2fsprogs (the test PC): jamext's round trips plus Linux's e2fsck -fn on every image and debugfs reading jamext's files, links and modes (`make check` runs the same test on the Mac without them) |
| `tools/ports-test.sh <outdir> <port>...` | ported programs on Jam OS ([M13.7 PO8 notes](M13.7-PLAN.md#po8-notes)): each port is built (`tools/ports.py`), its files and approval lines written into a copy of the stick image's data partition with mtools (`tools/ports-preinstall.py`: `/data/pkg/...`, `/data/etc/allow`), the image booted, and the port's own `ports/<port>/test` (a serial-feed script) run in the shell; one QEMU per port in `<outdir>/<port>/`, `PORTS_TEST_JOBS` (3) at a time; prints PASS or FAIL per port. `make image` first (`tools/remote-test.sh` does); PC only. Proven on zlib (minigzip round trips with hash checks) and xz |
| `tools/pkg-test.sh <outdir>` | `pkg` end to end ([M13.7 PO6 notes](M13.7-PLAN.md#po6-notes); `make -s image` first, PC only): the image's `/boot/ports.pub` is the ports vectors' public test key (`tools/bootfs-edit.py`), the stick's settings have `pkg.repo` (the "good" repository) and a static address, and `tools/pkg-test-repo.py serve` (small signed packages made with the builder's writers: `good`, `next`, `old`, the defects `badpkg` `otherkey` `missing` `refuse` `badidx`, `rebuild`, `slow`) is joined by the peer's TCP relay (`10.2.21.174:8000`). Three QEMU runs: `pkgmain` (`shell-tests/pkg-main.txt`: nothing installed; every defect refused with nothing left; `pkg install bbb`, which runs aaa, one question for both; list, check, files, info, `allow -l` showing `[package aaa 1.0-r1]`, `allow -r` refused over a package's program, remove refused while bbb runs aaa; an index from the past refused; audit; upgrade; a rebuild of the same version upgraded; `remove -r`; installed again), `pkgcold` (`pkg-cold.txt`: the stick from cold has what was installed; ccc from the `slow` repository, whose package stalls halfway, and QEMU is quit during the download: a power cut with the journal at its first step) and `pkgsettle` (`pkg-settle.txt`: init's settle line "install ccc undone", `pkg check`, and the host reads the data partition: no ccc, no `.new`, no journal). Prints PASS or FAIL |
| `tools/fonts-check.sh [stagedir]` | the fonts package's fontconfig configuration ([G2-PLAN: GP2 notes](G2-PLAN.md#gp2-notes)), on the build host (Jam OS has no fontconfig): the host's own `fc-match` over `build/ports/fonts-jamos/stage` with its fonts.conf (`/data/pkg` rewritten, a scratch cache) must resolve `sans-serif`, `serif`, `monospace`, Arial, Helvetica, Times New Roman, Courier New, `emoji`, bold, italic, Inter and U+1F600 as G2 section 4 says; prints one line each and PASS or FAIL. Build the port first (`python3 tools/ports.py --out build/ports build fonts-jamos`). The package's own test (the 79 files and their hashes on Jam OS) is `tools/ports-test.sh build/t/fonts fonts-jamos` (about 6 minutes on the PC, mostly the 79 `sha256sum` runs; fetches 260 MB of fonts the first time) |
| `tools/wl-port-test.sh <outdir> [group...]` | Wayland clients on Jam OS, end to end ([G2-PLAN: GT notes](G2-PLAN.md#gt-notes)): wlprobe (`ports/wayland/override/wlprobe.c`, ours, on the real libwayland, libc's Wayland socket and xkbcommon; `wlprobe -l` lists its cases) and the ported hello-wayland and wl-window-test, preinstalled like `ports-test.sh`'s ports into a plain boot with a USB keyboard and mouse. One QEMU per **group** (`tools/shell-tests/wl-port/<group>.txt`), `WL_PORT_JOBS` (3) at a time: `proto` (`wlprobe all` in the tiling layout: globals, bind, shm, output, keymap, toplevel, keyless, deco-server, frames, geometry and its errors, parent, popup (a menu where its positioner says, up, with the keys), popup-nested (the keys up a menu's chain and back), popup-reposition, popup-flip, popup-tooltip (no keys), popup-parent (dismissed with its parent), big-buffer, resize-storm, reuse, clipboard-serial, maximize, fullscreen, minimize, and the shot cases without their holds; then states-tiled and subcompositor), `floating` (Super+T, states-floating, then wl-window-test and the cat with a title bar or none), `tiled` (the same two with the tile's border), `paint` (title, shm-argb alpha over the terminal's ground, damage, csd-geometry: wlprobe holds each window still for the script's screenshots), `pointer` (pointer, keyless-click, popup-click: a menu opened by a click on the window and dismissed by a click on the terminal), `keyboard` (keyboard, close), `cursor` (cursor-shape, cursor-surface: the drawn cursor differs per shape, shows the client's own surface at its hotspot, hides), `clip` (clipboard-in and clipboard-out with a terminal), `hold` (a window redrawn on every frame callback for 20 s), `hostile` (track GR's hostile clients, [G2-REVIEW](G2-REVIEW.md): 1,000 subsurfaces made and destroyed under a window and trees at every cap, then one past each cap (`no_memory`); a desynchronized child drawn 2,000 times flat out with its parent idle; grabbing popups 8 deep and a ninth, 32 popups and a 33rd; a decoration object's toplevel, or the toplevel's `wl_surface`, destroyed first (`orphaned`, `defunct_role_object`); 28 pipe ends given as pools (`invalid_fd`) and descriptors that can't cross the socket (EINVAL, nothing sent); 20,000 syncs never read (dropped as too slow); each followed by a plain window; `hostile-regrab`: a menu dismissed by a click on terminal 1 can't grab straight back with its old serial or an enter, so the next click on terminal 1 reaches it; then the compositor's handles and message bytes in `ps -k` back where they were). `tools/wl-port-check.py` reads each case's PASS/FAIL from the serial log and checks the screenshots: wl-window-test's bands (`still`) at every pixel but the rounded corners, the cat against `cat.png` found by its pixels, the surface coordinates of a click against where a marker block shows the surface is, alpha blended over the terminal's background, and so on. A case the compositor is known to fail is an XFAIL there with its reason (none today), and a case on that list that passes fails the group (XPASS). Prints one line per case and PASS or FAIL per group; 1280x800 only (the clicks are at pixel positions); PC only; about 2 minutes a group. `JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh tools/wl-port-test.sh build/test/wl proto paint` |
| `tools/cr-test.sh <outdir> <case>...` | Chromium's layers on Jam OS ([CR-PLAN](CR-PLAN.md#1-the-toolchain)), PC only; each case in `<outdir>/<case>/`. `hello` (CR-a's C++ half, track CT1): fetches the toolchain if it isn't there (`tools/cr/toolchain.sh`), builds `ports/cr-hello` with `tools/jamos-clang++` and what it needs (`ports/llvm-runtimes`, `ports/linux-uapi`; `CR_JOBS` (8) jobs, at low priority), runs the syscall scan on it (`tools/cr/syscall-scan.py`), checks that the build host's own `c++` build of the same source prints its `expected.txt`, then runs it on Jam OS from `/data` through `tools/ports-test.sh` (the same output, byte for byte). About a minute and a half the first time (the runtimes), seconds after. `rust` (CR-a's Rust half, track CT2): the same steps for `ports/cr-rust-hello`, built by Chromium's pinned Rust (`toolchain clang rust`: cargo builds std from rust-src for `x86_64-unknown-linux-musl`, linked by the port's clang wrapper; the case also fetches Rust, 276 MB, and builds gperf into `~/.cache/jamos/tools` through `tools/cr/toolchain.sh`), the build host's rustc build of the same source printing `expected.txt` and, with the argument `panic`, aborting with status 134 and `expected-panic.txt`, then Jam OS printing the same and killed by SIGABRT (status 134); about 25 s to build after the runtimes. The CB tracks add theirs |
| `tools/cr/build.sh start [--clean] [--gen] [--keep=N] [<target>...]`, `status`, `wait [<s>]`, `stop`, `pkgs`, `scan <outdir>`, `fetch <outdir>` | Chromium's build on the PC ([CR-PLAN](CR-PLAN.md#cb1-as-built-2026-10-14), track CB1), run through `tools/remote-test.sh`: `start` fetches Chromium's pinned tools (`tools/cr/toolchain.sh`), copies the checkout to `~/cr/runs/<run>/`, claims `CR_JOBS` (10) CPUs and `CR_MEM_MB` (12288) in the budget for as long as the build runs (`remote-slots.py --detach`) and returns; the build, `ports.py build -f --kept ~/cr/<version> chromium` at nice 19 and ionice idle, one at a time (`~/cr/.lock`), keeps its source and output in `~/cr/<version>/`. `status` and `wait` (up to 270 s) print ninja's progress, the peak memory (its processes' Pss, sampled every 5 s) and, at the end, `summary.txt`: result, time, peak memory, disk. `pkgs` lists the system libraries GN asks for; `scan` runs the syscall scan on `base_unittests` and `headless_shell`; `fetch` copies the port's stage back for `ports.py package --from`. Waits use a light claim: `JAMOS_REMOTE_LIGHT_CPUS=1 JAMOS_REMOTE_LIGHT_MEM_MB=64` |
| `tools/soak-test.sh <outdir>` | the soak test (`soak-plug.txt`): `soak loops=3` at a fixed seed (`SOAK_LOOPS`, `SOAK_SEED`), under the kernel's and `bin/soakload`'s load, with a second stick (made writable) pulled in the middle of writes and plugged back and then the boot stick pulled and plugged back (`SOAK_LOAD`: the kernel load workers, default one per CPU); PASS needs 0 FAILED kernel tests, utest runs and file checks, the job tree's message bytes grown by at most 32 KiB (unread messages piling up), and the second stick's own files unchanged |
| `tools/verdict-test.sh <outdir> ok\|fail\|quiet\|crash` | a shell script's verdict on a failing utest: `utest only verdict_canary` (user/tests/utest/verdict.c) made to pass (the run passes), to fail a check (the run fails, naming the check's line), to fail without printing it (the run fails on utest's RESULTS lines) or to fault (the run fails: a utest began and gave no summary), in `shell-tests/verdict.txt`, which checks nothing itself. One boot each |
| `tools/ktest-keep-test.sh <outdir>` | the test runner's own failure paths, with three tests that exist for it (`ktest=review_ktest`): with `keep` both failures are recorded and the run goes on; without it (and `crashkernel=0`, so the panic screen says it can't recover) the first panics and its note names loop, seed and test |
| `tools/hda-test.sh <outdir>` | the HD Audio driver (`hda.txt`): two emulated controllers (intel-hda with hda-duplex and hda-output, ich9-intel-hda with hda-micro), each codec's graph in the log, `hda` from the shell, `kill hda` and devmgr's restart; the path self-test passes and every codec's path is DAC 02 -> pin 03, set up muted; the jack self-test passes (the ALC897's jack table and tags, the RIRB's demultiplexer on a fake RIRB, the debounce, unsolicited responses and the polling fallback against a fake codec that allows jack code only SET_UNSOLICITED_ENABLE, SET_PIN_SENSE and GET_PIN_SENSE; QEMU's codecs have no presence detection, so the real jack path runs only on the PC); QEMU's codecs trace every verb they get and every one must be a GET or a silent SET (power D0, a connection select, pin control with the output off, an amp mute), and none a jack verb (0x708, 0x709); `hda` opens no stream, so no converter format or stream tag either; `hda jacks` shows the RIRB interrupt taken (it is on for unsolicited responses while the dumps' commands are polled, and the dumps must still see no timeout); then the `init` run with the same devices, where each driver must stop cleanly and the run end "run complete: no problems"; then a shell boot with `QEMU_IOMMU=eim` and `iommu=on vtdtest` (`vtdtest.txt`): each drv/hda's read of a page it unpinned is blocked (QEMU hands the controller a 0, so the codec's answer is not the command's) and the kernel logs a read fault naming that controller at that address (or counts it lost: QEMU's unit has one fault record, and the two controllers can fault under a millisecond apart; at least one is logged), then its response ring is pointed into the interrupt window (QEMU passes old-format writes through: blocked only on the PC), `hda` still answers with no timeout (the rings were put back), and `iommu` shows each controller in a domain of its own with its fault counted |
| `tools/vtd-test.sh <outdir>` | the boot-time VT-d probe ([HISTORY: M11](HISTORY.md#m11-the-iommu)), three `pcilist` boots: with QEMU's `intel-iommu` (interrupt remapping and caching mode on) the DMAR table's lines (the unit, its I/O APIC scope matched with the MADT's, each endpoint scope named by its ids), the unit's registers decoded, translation and interrupt remapping off as the firmware left them, the handover line, nothing in the RESULTS box, and QEMU's trace of the unit's registers: reads, not one write; with `eim=on`: x2APIC ids; without an IOMMU: "no DMAR table". Then two `ktest=vtd iommu=on` boots, caching mode on and off: the unit started and every `vtd_unit_*` test passed (every invalidation kind, the page-table and interrupt-entry callbacks, a refused descriptor reported with the queue going on, the queue wrapping, every CPU submitting at once, the queue turned off and on), interrupt remapping on and every `vtd_irq_*` test passed (COM1's pin remapped, an MSI through its own entry, entries freed on close, another function's, a freed and an out-of-range entry refused and recorded, edu's DMA into the interrupt window blocked, the timer, IPIs and COM1 unaffected, remapping off and on again), translation on (every function in its home domain) and every `vtd_domain_*` test passed (edu blocked with its fault seen, in a domain of its own, a driver's domain taking pins spread one per 2 MiB up to its 2048 table pages, a function whose requester id is shared refused a domain (on a made-up topology, and edu made to look shared), muted after its faults, the tables switched while translating), nothing else in the RESULTS box but the faults those tests provoke; two `ktest=dma iommu=on` boots, caching mode on and off: every `dma_*` test passed, none skipped (edu reaches only what its DMA capability pinned, a killed driver's pages freed once its domain is gone, the quarantine's tests in their translated form, the pin and unpin cost in invalidation waits printed); the same `vtd_domain_*` tests on a unit without pass-through (`pt=off`: nothing depends on it); a boot with a PCIe-to-PCI bridge and a second edu behind it: both named in the RESULTS box as sharing a requester id, and neither given a domain; then two shell boots with `iommu=on` jumping into the stored kernel, by `reboot` and by `crash panic yes`: the jump turns interrupt remapping, translation and the queue off, so both boots find the unit all off, and the next kernel turns interrupt remapping and translation on again; and the ACS boot (`vtd-acs`, `ktest=pci_acs iommu=on`): four root ports, three with ACS (the boot turns it on: SV RR CR UF, a log line each, one without, and the switch downstream port and the two-function device below ports that have none named as such), an edu below the switch port, a two-function device and alone below each of the other two; the first three are refused a domain ("no DMA for it while translating") and the ones alone below a root port, with or without ACS, and 00:04.0 are not, every `pci_acs_*` ktest passes (the control register's bits from the capability, ACS on wherever it is offered, the refusal on a made-up topology and on the live one). `VTD_TEST_INIT=1` adds the `init` run with the IOMMU present, left off and then started with `iommu=on`. About a minute (two more with the init runs) |
| `tools/hda-stream-test.sh <outdir>` | the HD Audio output stream (`hdastream.txt`): intel-hda with an hda-output codec (`mixer=off`) whose samples go to a WAV file through QEMU's wav backend at 48 kHz 16-bit stereo; `hdatest` passes (the position's rate within 2 %, a closed stream released, a kill mid-stream: restart, the dead driver's pins out of the DMA quarantine unwritten); the WAV holds `hdatest`'s one-second pattern sample for sample after the leading silence, then most of a ring of silence (the driver's clear-behind); the codec got only allow-listed verbs, and the path opened only while the converter has the stream's tag and closed again before it is released (`tools/hda-verbs.awk` follows the state the SETs leave; hdatest turns the gain down while it plays) |
| `tools/beep-test.sh <outdir>` | `beep` (`beep.txt`): intel-hda with an hda-output codec with its mixer on (its DAC amp scales what the WAV gets) through QEMU's wav backend; `beep 440 500` at the default -30 dB: the tone's frequency from its zero crossings within 1 %, its length within 30 ms, the fades (first and last 2.5 ms well under the peak), no clicks, silence after, the peak at QEMU's volume for step 44 within 5 %; the codec's verbs: the path opened only while the converter has the stream's tag and muted again before it is released, the DAC's amp opened at step 44 only; the output stage (pin output, EAPD) on before the first stream and never off, the first unmute at least 400 ms after it went on; the driver's lines in order (stream open, unmuted, muted again, stream closed); `hda gain` set and clamped |
| `tools/play-test.sh <outdir>` | `play` (`play.txt`): WAV files made by the script and copied onto the stick image's `/data` with mtools, played through intel-hda with an hda-output codec (mixer on) into QEMU's wav backend: 48 kHz stereo (440 Hz left, 660 Hz right), 44.1 kHz mono, 22.05 kHz 8-bit, 96 kHz 24-bit WAVE_FORMAT_EXTENSIBLE with `play -v -20`; each sound's frequency per channel within 1 %, its length within 2 %, mono equal on both channels, silence after it, no clicks, the `-v` one's peak a tenth of the others' (its mixer stream at -20 dB) and the others' unchanged; garbage, a cut-off header, 32-bit float and a missing file refused with their reasons; a 10 s file stopped by Ctrl+C after 2 s ends within 3 s with a fade; QEMU runs with the trace event `hda_audio_overrun`, and each codec buffer it dropped on a busy host excuses one click or one sound 2048 frames short |
| `tools/audio-quality-test.sh <outdir>` | sound quality (`quality.txt`): signals made with numpy, copied onto the stick image's `/data`, played with `play -s` through the mixer into intel-hda with an hda-output codec (`mixer=off`) and QEMU's wav backend (16-bit); measured: a 48 kHz stereo file bit-exact at 0 dB; a 1 kHz sine at 0/-20/-40/-60 dB of volume at its level within 0.1 dB, its THD+N and spurs, and (dithered) no harmonic above the noise at -40/-60 dB; 44.1 kHz tones at 1-20 kHz within 0.1 dB with no image or alias above -90 dB; a 20 s tone read from the stick with no dropout and its length within 10 ms; no click anywhere; every `play -s` 0 underruns and 0 late periods, and no "frames late" in the log |
| `tools/mp3-test.sh <outdir>` | `play` of MP3s (`mp3.txt`), as play-test does: tones encoded by ffmpeg (libmp3lame and mp2; the script needs it) and copied onto `/data` with mtools: 44.1 kHz stereo 192 kbps CBR (440/660 Hz), 48 kHz mono VBR without an ID3v2 tag, 22.05 kHz MPEG-2 without a Xing/Info tag, one with ~230 KiB of album art in its ID3v2 tag and an ID3v1 tag, MPEG-1 Layer II, one with 3000 bytes of noise in its middle, one cut off mid-frame; each sound's frequency per channel within 1 % (its middle 80 %), its length (within 2 % where a LAME tag gives the delay and padding), mono equal on both channels, silence after it, no clicks (but where frames are missing); noise and an ID3 tag followed by noise refused; a 10 s file stopped by Ctrl+C with a fade; `play -n` decodes all 441000 frames of it and prints the cost per second of audio; QEMU's codec buffer drops excused as in play-test |
| `tools/splash-test.sh <outdir>` | the boot splash (`splash-comp.txt`, `splash-keys.txt`, `splash-off.txt`, `splash-hang.txt`; `tools/splash-check.py` compares with `boot/splash.mpg` decoded by ffmpeg), all on the compositor: a plain boot with an hda-output codec into a WAV, twelve screenshots from init's start each all `#1E1A1D` or a frame of the video (no desktop before the splash), two screenshots a second apart that are frames of the video (2560x1440 scaled down by a 2:1 box onto 1280x800) in order in its full-screen window, the sound started together with the picture at 0 ms, the last frame lingering 0.5 s, the capture the video's sound from where the splash said it joined to its end, lines typed while it plays, once it has played and right after it has gone all run by the shell (the splash's window takes no keys), `run splash --selftest` and `--alpha`, with QEMU's network: netstack's Connected notice, posted during the splash, shown only once it is over; two lines typed while it plays at 2560x1440 (a frame drawn 1:1, no skip, the whole sound taken, the lines run by the shell once it is up); `verbose` and `nosplash` (the text log, no splash); `shell testpf` (the panic screen over the quiet one); `splashhang` (a splash that never finishes: init gives up on it 20 s after its start, kills it and starts the shell, which runs what is typed; booted with every option word the kernel hands init, all of which must reach it); prints the time to the first frame and to the shell with and without |
| `tools/clock-test.sh <outdir>` | the date and the settings (`clock-1.txt`, `clock-2.txt`), one stick image, two boots with the RTC at 2026-01-15 01:02:03: Sydney's time from the RTC taken as local time, the settings file made with its defaults, `date -u`/`-r`/`-z`, `vol master`, `music vol` and the zone kept, then `reboot` (kexec): the next boot has the zone and the volumes; then, with mtools, a file written in boot 1 is dated 2026-01-15 1:02, `logs/boot-0001.txt` starts with its date, the settings file holds what was set and no `settings.new`; `rtc = utc` put in from this side: boot 2 reads the RTC as UTC |
| `tools/quiet-test.sh <outdir>` | the quiet boot (`quiet.txt`): three compositor boots, `nosplash`, the splash and `verbose`; `tools/quiet-check.py` counts the rows of text in a screenshot of the first terminal: only the banner and the prompt on the first two (the boot's late lines, usb-bus's and the timer check's, come while it is up), at least ten with `verbose`; `dmesg`, `/data/logs` and the serial log still have the lines |
| `tools/screen-test.sh <outdir>` | the first terminal on the desktop on a plain boot (`screen.txt`, booted with the splash, and `screen-verbose.txt`): after the splash the shell's banner and prompt and no kernel log (few pixels of the log's colours) and no RESULTS box; a stick plugged in (one desktop notice, "USB stick added": `mount -w` and `mount -r` in between add none) and pulled out, the boot stick pulled out and back, each a `compositor: notice` line in the log; `ktest` puts its lines in the window while it runs; then a `verbose` boot whose first terminal has the log |
| `tools/mixer-test.sh <outdir>` | the mixer (`mixer.txt`): intel-hda with an hda-output codec (`mixer=off`) into QEMU's wav backend; `mixtest` passes (the protocol's refusals and clamps; two openers of `/svc/audio` and of `/svc/audioctl` each answered on their own channel; the per-opener stream cap, 4 each and 16 in all; `audioctl.device`'s query channel to the driver answers but refuses `open_output` and `query`, at most 8 at once; tone programs at once; one killed; the master and an `audioctl` volume; the mixer killed and the hda driver killed mid-tone, each played on; a stream left empty lets the mixer close the output and its next write wakes it; two programs on `<audio.h>` at once); the WAV, segment by segment: 440 Hz and 1000 Hz at once with the second at -6 dB (amplitude ratio within 3 %), the killed client's partner with no gap (QEMU's own buffer drops on a busy host, traced with `hda_audio_overrun`, are told apart from a gap), -6 dB master and -12 dB `audioctl` volume heard, the library's 44.1 kHz and 48 kHz tones together; the log's stream, restart and output lines; the codec's verbs: muted whenever nothing plays |
| `tools/audio-api-test.sh <outdir>` | libos's sound output API (CA1: `<audio.h>`, `audio-api.txt`), on mixer-test's devices (intel-hda, an hda-output codec with `mixer=off`, QEMU's wav backend): `utest only audio_out` passes 11 of 11 with none skipped (the float conversion's exactness, clipping, NaN and dither; the refused arguments and the calls on a closed output; four streams and a fifth refused, a closed one's place given back; the ring's room to the frame, a wait for room that times out on a paused full ring, `audio_discard` emptying a stopped ring, the wake-up of `audio_arm_room`; a paced writer that pre-rolls, starts, writes with `audio_try_write` and waits for room, its `audio_position` latency exact before the start; two streams started one after the other; pause for half a second with the play position stopping, then resume). The WAV, segment by segment: the 16-bit pattern and the float pattern sample for sample, the 44.1 kHz mono float tone through the resampler at its amplitude with equal channels, the paced 440 Hz tone whole with no gap, 440 and 1000 Hz together at their amplitudes, and the pause case's 60000-frame pattern up to the pause, then the rest in order from a later frame (the mixer closes the device when nothing plays and the lead it had taken, up to 171 ms, is not played: a skip of 0 to 16384 frames at the resume, no frame twice) (QEMU's own buffer drops, traced with `hda_audio_overrun`, excuse an exact comparison that is short by at most 2048 frames a drop); the mixer logs both streams of the two-stream case; the codec's verbs as in mixer-test |
| `tools/mixer-restart-test.sh <outdir>` | the mixer killed while it plays is not seen by its clients (`mixer-restart.txt`, booted once with the warm spare and once with `nospare`), intel-hda with an hda-output codec (`mixer=off`) into QEMU's wav backend: `mixramp` plays a 16-bit ramp at 0 dB while init kills the mixer 20 times and a third thread keeps calling it (volume, position, a stream opened and closed); every call succeeds, and the mixer never logs that the keeper couldn't keep or drop a slot (the churn fills init's keep channel, 1024 messages, in 50 ms: requests wait in their channels for room, `user/services/mixer/state.c`); one `mixer: restart (killed` line per kill, each with the output still running and the lead left, none falling back to a fresh start; the output opened once; the WAV is the whole ramp, every frame one more than the last (QEMU's own buffer drops, traced with `hda_audio_overrun`, are counted apart). Prints the lead left, kill-to-first-answer (mixramp's and init's) with and without the spare, and what each restart found in progress |
| `tools/mixer-spare-test.sh <outdir>` | init's warm spare mixer and its restart rule (`mixer-spare.txt`, then `mixer-nospare.txt` booted with `nospare`), with an intel-hda card: a spare waits from the boot, each `kill mixer` promotes it and a new one starts, 13 deliberate kills are never given up on, the mixer's own exit when devmgr is killed is restarted at once from the spare; without spares each kill starts a process; prints the spare's memory (`ps`) and the restart and kill-to-first-answer times with and without a spare |
| `tools/fat-restart-test.sh <outdir>` | fat carrying on from a dead instance's state through volumes' kills (`fat-restart.txt`, booted with the warm spare and again with `nospare`): every instance that got going carried on (its `fat /data: restart (killed` line, never `starting fresh`), the mount never goes, nothing given up on, unsynced writes there after the kill, the copies' checksums the originals'; then on the host (mtools) the stick's files hold what was written and the copy of the kernel is the kernel. Prints kill-to-first-answer (volumes' probe) and the kill to fat carrying on, with and without the spare. The exact deaths are utest's `fat_restart_steps` (a script of every kind of request, FS_GATHER held across deaths among them, run undisturbed and then with fat ended at a held write, the commit, a block write of the send, before and after the answer, several times a run: the same answers and the two RAM disks the same byte for byte), `fat_restart_handles` (fs.open's and fs.view's replies whose handles died with the process made again) and `fat_restart_bad_request` (a request that crashes fat: ERR_IO after two crashes, fat still serving; two deliberate kills don't count), in the `init` run |
| `tools/volumes-restart-test.sh <outdir>` | volumes started again, and what is under it (`volumes-restart.txt`): `kill volumes`, `kill devmgr` and `kill usb-storage-1:0` in turn on the boot stick; each time `/data` and `/esp` go and come back, logd carries on in its file, no "Jam OS stick removed" notice, nothing given up on, no job left unclean; on the stick (read with mtools) the file written before the kills and appended after them, and one boot log from the boot to the reboot's sync with every kill in it |
| `tools/fat-spare-test.sh <outdir>` | volumes' warm spare fat and the restart rule of a filesystem service (`fat-spare.txt`, then `fat-nospare.txt` booted with `nospare`), on the boot stick: a spare waits from the boot, each `kill fat-data` promotes it and a new one starts, the mount never goes (no `is gone` before the reboot), 20 deliberate kills are never given up on, the files written before them are there; without spares each kill starts a process; prints the spare's memory (`ps`) and the restart, kill and kill-to-first-answer times with and without a spare |
| `tools/fat-storm-test.sh <outdir>` | `storm`, the demonstration of M11.6 (`storm.txt`), with a second stick of one file of random bytes (`STORM_MIB`, default 40 MiB: big enough that the 1-kill-a-second run lands a kill mid-copy on the PC; `STORM_NAME` names the run, which parallel runs of this script need to differ in) and an intel-hda card: every copy at 0, 1, 10 and 100 kills a second says MATCH and its SHA-256 lines are the host's for the file; more kills the higher the rate, none refused, each answered after; Ctrl+C said; no restart started fresh, nothing given up on, no mount gone. Then on the host: each copy on the boot stick's `/data` (mtools) is the file byte for byte, and `tools/fatcheck.py --require-clean` (run before mtools has touched the images) finds both disks clean (no damage, no lost cluster, marked clean: the reboot stops each filesystem service in order, so logd's last write can't land after the final sync and leave `/data` dirty). The mixer: one restart line per kill, the least lead left above the 256-frame guard, no late period in storm's line, the log or the music stream's close line. Prints the four storm lines, the mixer's, fatcheck's verdicts and, on a Mac, `fsck_msdos -n`'s on both disks (read-only `hdiutil` attach; not part of PASS: mtools' own volume label makes it warn) |
| `tools/music-test.sh <outdir>` | the music player (`music.txt`): a folder tree made by the script (ffmpeg, mtools) on `/data/music`: six 2 s tones (MP3 at 44.1, 48 VBR and 22.05 kHz, WAV at 48 and 44.1 kHz) under names with spaces, apostrophes, `$`, `~`, parentheses and UTF-8 (`JAŸ-Z`), an upper-case `.WAV` and `.Mp3`, a garbage `.mp3`, `.DS_Store`/`._` dotfiles, a text file and an empty folder tree; and a second stick of three 8 s WAVs. In the capture (100 ms FFT windows up to a 1000 Hz marker beep typed 2 s after `music stop`): the tracks heard are the log's `track N:` lines in order, the first six are the six tracks once each, at least seven heard, never the same twice in a row; the 1500 Hz `beep` mixed over a track; the stop fades. The log: every title (`Artist - Title` from the path), the garbage file skipped, no dotfile tried, `kill mixer` not seen by the player (the mixer's restart adopts its stream), the pulled stick stopping it after three unreadable files |
| `tools/net-test.sh <outdir> [vlan vlan-off rx]` | the e1000e driver against `nettest`, a hostile netstack ([HISTORY: M9](HISTORY.md#m9-networking)), one `init` boot per scenario from a copy of the stick whose bootfs runs `bin/nettest <mode>` from init.cfg (in shell mode netstack holds the card's one session), with `QEMU_NET` and a peer of the script's own (`tools/netpeer.py`'s `Peer` plus nettest's frames; it also fails on a tag inside VLAN 21's): `vlan` (the session rules; every bad length, flags, frames already tagged 0x8100, 0x88a8 and 0x9100, `produced` a ring and one ahead and then behind, a thread rewriting EtherTypes while the driver copies: each refusal counted exactly, and the peer and the pcap each hold exactly the frames the driver queued and the chip sent, all tagged 21 once), `vlan-off` (a `vlan=off` boot: the driver's "no VLAN" line, no service, no frame at all), `rx` (the peer's census of untagged, VLAN 0, other-VLAN, QinQ, nested, 1522-byte and VLAN 21 frames: only the VLAN 21 ones arrive, untagged and whole, every drop counter exact; then 300 frames with the ring unread: 256 given, 44 `rx_ring_full`); every run ends with the driver stopped cleanly and every job empty |
| `tools/net-vlan-test.sh <outdir> [21\|none]` | the network rule over every path that transmits ([ARCHITECTURE](../ARCHITECTURE.md#networking)); with `none`, one boot in the untagged mode (`net-untagged`: the same script with `net` saying untagged, the peer and the pcap with `--vlan none`: every frame untagged, any tag fails); otherwise two boots with QEMU's e1000e and the peer (`--netlog`, `--update` serving this build, `--ntp` with the Mac's time, `--ping 10.2.21.100`, `--noise 5`, and its TCP relay: `--tcp-relay` 8000 to `tools/httptest.py` and 5201 to `tools/speed.py server`, `--tcp-forward` a Mac port to the guest's 8080), only `net.host = 10.2.21.174` and a 64 KiB `served.bin` on the stick: `net-vlan` (`net-vlan.txt`: the DHCP lease, netlog, `host one.one.one.one`, `ping mac.jam`, `ping 1.1.1.1`, `update -n` (checked by init, not loaded), `fetch` of `small.txt` into a pipe, `serve /data/served.bin` fetched by curl on the Mac (its bytes must be the file's), a 1 s `speed` sending, sntp setting the clock from the gateway, netstack answering ARP and the peer's pings; the peer's and the pcap's checks, then the pcap's frames counted per path: ARP, DHCP, DNS, echo requests, echo replies, netlog, update, SNTP, fetch, serve and speed must each have sent at least one, none may be from another path, and the peer must have seen exactly the pcap's frames) and `net-vlan-off` (`net-vlan-off.txt`: a `vlan=off` boot with the same commands: not one frame (the peer's `--expect-none` and the pcap's), the driver's "no VLAN" line and nothing touched, `net` with no card, `host` and `ping` with no DNS server or route, `update` with no address, `fetch`, `serve` and `speed` giving up on an address that never comes, no lease, no netlog file). A mode that is the build's default (build/build.txt's `net` line) boots with no `vlan=` word and the kernel must say `(the build's default)`; the other gets the word: run both modes to cover both. About 1 minute each (QEMU, 2 CPUs, 2026-10-02: about 43000 frames in the VLAN run, 35000 of them `speed`'s) |
| `tools/sntp-test.sh <outdir>` | bin/sntp end to end, three boots with QEMU's e1000e, the RTC at 2026-01-15 and the peer's SNTP server (`--ntp`: 2031-02-27 from its start): `sntp` (`sntp.txt`: the gateway asked, each answer after a forged one (`--ntp-forge`: the origin a bit off, a year later) that is said to be ignored, the five-year step confirmed by a second reply, `date -u` 2031, `date -r` "set from the network", `date -z` keeps it, sntp restarted moves it under a second, devmgr restarted and init keeps the network's time); checked afterwards: the kernel's set near the peer's time, never the forged year; `sntp-name` (`ntp.server = router.jam`, through `/svc/dns`); `sntp-off` (`ntp = off`: no sntp, not one SNTP request, the RTC's date). About 3 minutes |
| `tools/ping-test.sh <outdir>` | the shell's `net` and `ping` (`net.txt`) with QEMU's e1000e and the network peer: `ping 1.1.1.1` answered through the gateway, Ctrl+C, `net stats` ([netstack](#netstack)); then the same commands as programs on the same stick (`sysbox-net.txt`) |
| `tools/remote-slots-test.sh` | the CPU budget on the test machine ([above](#another-machine-for-the-tests)) against a fake lock directory, no PC, about 55 s: the costs of each kind of command (with the `--shrink` spec of the scripts that have parts); 6 commands of 3 CPUs on a budget of 8 never more than 2 at once; a command bigger than the budget alone; a holder killed (`kill -9`) with and without its command leaks nothing, and a dead governor's command (and a process it left in a session of its own) is killed by the next caller, the CPUs freed at once; a command that leaves such a process has it killed when it ends; a cancelled governor (SIGTERM) takes its tree down, a command that ignores SIGTERM after `JAMOS_REMOTE_KILL_GRACE`, one that ends on it keeps its own status; an old waiter is not passed by a younger small one, unless it would still fit after that one started (a stuck waiter holds no one up); the 2026-10-15 deadlock (a 16-CPU long claim, a 12-CPU waiter told at once that it can't fit, 4-CPU runs that start, the 12-CPU run after the long claim ends, and an 8-CPU run that does fit keeps its place); `--shrink` takes 2 of 3 parts beside the long claim and none off a run that fits; `JAMOS_REMOTE_SLOTS`, the budget file, `--status` (pid, age, command, a dead governor's `GONE`), the exit status; a waiting run whose stderr is closed gives up (141); a `--detach` claim returns at once, holds its CPUs while its command runs (the others wait), logs its output, ends with the command and, made inside another claim, outlives it; prints PASS |
| `tools/pc-batch-test.sh` | `tools/pc-batch.sh` ([Running tests on the PC](#running-tests-on-the-pc)) with a stand-in for remote-test, no PC, about 25 s: start returns at once, a short wait says "not yet" (124), the table (PASS, FAIL with its failure line and log, rerun-PASS, the queue time), `VAR=value` words with and without `env`, one copy then every run without one, bad runlists refused, a PC that doesn't answer tried twice, a run that finds it gone run once more (a test's own exit 3 not), the runs started `PC_BATCH_STAGGER` apart and no more than `PC_BATCH_JOBS` out at once, a `KTESTS=0` run's own build, cancel; then `start --detach` against a fake PC (a home dir with the real keeper, `tools/pc-batch-driver.sh` and `remote-slots.py`; ssh, `wsl`, `setsid` and `flock` stand-ins), about 20 s more: the Mac's driver gone after the hand-off and no ssh until the wait, the table (FAIL, rerun-PASS, `JAMOS_TIER` reaching the run), outputs copied back without disk images, a run id for each try, two checkouts' batches at once, collecting twice and `status` without asking the PC again, a second detached batch of one checkout refused, `wait` with the PC down (exit 3), cancel (a process in a session of its own too, the claim freed, CANCELLED), a reboot (LOST, then `--resume`), no keeper (NOT-RUN); prints PASS |
| `tools/remote-test-test.sh` | `tools/remote-test.sh`'s ssh handling ([Another machine for the tests](#another-machine-for-the-tests), "Dropped connections") with a stand-in for ssh, no PC, about 5 s: a session lost after the login while the command waits for the budget is started again (same run id, up to `JAMOS_REMOTE_REQUEUE` times, then exit 255), one lost after the command started is not, the command's own exit status (5) comes back once, a login cut twice is tried again by `ssh-retry.sh`, keepalives on the session, a cancel (SIGTERM) ends the session's ssh at once and asks the PC in one more ssh to end the run by its pid file (exit 143), `init-test.sh`'s `--shrink` spec and `INIT_TEST_JOBS` reach the PC's script, no temp files left; prints PASS |
| `tools/ssh-retry-test.sh` | `tools/ssh-retry.sh` ([Another machine for the tests](#another-machine-for-the-tests), "Dropped connections") with a stand-in for ssh, no PC, about 6 s: a login cut by the server (kex_exchange_identification, reset, closed) is tried again up to 6 times with the piped-in script intact, nothing there ("refused", "timed out") 3 times, and never again: "Permission denied", a reset after the login ("connection lost after login"), a remote command that exits 255 or 7; the waits double with up to half as much again; `SSH_RETRY_TRIES=1` turns it off; the login slots (8 helpers, 2 slots: never more than 2 logging in; a dead holder's or a 5-minute-old slot is taken over; `SSH_RETRY_LOGINS=0` makes no lock directory); no temp files left; prints PASS |
| `tools/rxsoak-test.sh <outdir>` | the receive path over a long run (`rxsoak.txt`, about 4 minutes): the peer floods ~10 000 frames (a trunk port's mix, on VLAN 21 and off it) while it pings every 0.2 s; the late pings must be answered and the guest's own `ping 10.2.21.1` too ([netstack](#netstack)) |
| `tools/dns-test.sh <outdir>` | bin/dhcp and bin/dns end to end with QEMU's e1000e and the peer's DHCP and DNS servers, two boots ([DHCP and DNS](#dhcp-and-dns-end-to-end)) |
| `tools/tcp-test.sh <outdir>` | TCP end to end (`tcp.txt`, [HISTORY: M9.5](HISTORY.md#m95-network-streams)): with `net.address` in the stick's settings and the peer's TCP side, `run tcptest send 10.2.21.174 5030 10485760` (10 MiB to the peer and 10 MiB back on one connection, every byte checked on both sides, the SHA-256 said, then CLOSED with no error; the speeds both ways) `run tcptest send 10.2.21.174 5032 8388608 2097152` (8 MiB each way with 2 MiB rings, to a server whose ACKs come 20 ms after their segments: window scaling), and `run tcptest serve 5031 20 262144` (the peer's 20 connections at once to a guest listener, served from one wait set holding the listener and every connection, 256 KiB each way each); then `run tcptest small ... 5033 8 10 0` and `... 5034 8 10 1` (eight 10-byte writes 20 ms apart to servers whose ACKs come 200 ms late, with Nagle's algorithm off and on: `net_tcp_set_options`) `run tcptest keepalive 10.2.21.174 5035 5` (idle 5 s with keep-alive on, the peer answering the probes) and `run tcptest relisten 5036 65536` (a listener with address reuse serves the peer's connection and closes it first, so the port is in TIME_WAIT on the guest; a listener without the flag is then refused, one with it made again at once); PASS needs the peer's 26 connections right and none wrong, 6 segments or more from the no-delay writes (8 seen) and 1 to 3 from the Nagle ones (2), at least 2 keep-alive probes (the peer's `tcp_5033_segs`, `tcp_5034_segs`, `tcp_5035_probes`), on the 2 MiB rings' connection a scaled window over 64 KiB and more than 64 KiB in flight both ways (the peer's `tcp_5032_*` counts), on the default rings' one no window and no flight over 64 KiB, and the run's VLAN checks. `QEMU_NET_VLAN=none` runs it untagged. About 2 minutes |
| `tools/posix-net-test.sh <outdir>` | POSIX sockets end to end (`posix-net.txt`, [M13-PLAN](M13-PLAN.md#8-sockets-and-names)): with `net.address` in the stick's settings, the peer's TCP servers and DNS server, `run posixtest socket sockets`: names through `/svc/dns` (`getaddrinfo`, `gethostbyname` of a CNAME, `EAI_NONAME`), 1 MiB each way on a blocking connection (5040), a nonblocking connect with `poll` and an `EPOLLET` read loop (5041), `EINTR` with and without `SA_RESTART` and `SO_RCVTIMEO` while another thread sends (5042), `EPIPE` and SIGPIPE after `shutdown` (5043), 32 connections in one `poll` (5046), a connection handed to a child at two descriptors (5047), UDP both ways (a DNS query to the peer's server: `MSG_PEEK`, `FIONREAD`, `MSG_TRUNC`, `connect`), and the peer's two connections to a guest listener (5045); PASS needs posixtest's PASS, the peer's 39 connections right and none wrong, and the run's VLAN checks. `QEMU_NET_VLAN=none` runs it untagged. Under a minute |
| `tools/pager-test.sh <outdir> [name]` | mapped files and the pagers behind them ([M12.5-PLAN](M12.5-PLAN.md), and M13.6's U10 for extfs), one shell boot per case (`PAGER_CASES`, default all; the script's header says each one's steps): fat's `plumb`, `map`, `budget`, `kill`, `exec` (two boots: programs on `/data` run from their pages, three changed on the image refused or killed), `write` and `pull`; extfs's `jam` (a Jam OS NVMe drive whose data partition is an ext volume Linux's mke2fs made: a file on `/jam` hashed through a mapping and a VMO of its own, pagertest's writes and truncates, a mapped file emptied, and removed: its mapping reads on; e2fsck clean after), `jstorm` (storm with a mapper: a file bigger than the budget hashed six times while `storm` copies on `/jam` and kills `extfs-jam` twice a second: MATCH, every hash right, no page failed) and `xexec` (the `exec` case with the stick's `/data` an ext volume, `tools/ext-stick.sh`, the bytes changed in the files' blocks between the boots; e2fsck clean after each). The ext cases need e2fsprogs and mtools (the PC's WSL) |
| `tools/posix-mmap-test.sh <outdir>` | POSIX mapped files and programs on `/data` ([M13-PLAN](M13-PLAN.md#12-memory-mmap), "F1b notes"), one shell boot per case (`MMAP_CASES`, default all; give each its own output directory to run them side by side): `mmap` (`posix-mmap.txt`: posixtest's mmap section from the shell, on the file's pages), `big` (`posix-mmap-big.txt`: a 100 MiB file on a second stick, made with its 64-bit words' sum by the script, mapped shared and private by `posixtest mmap-big`: the sum right, the job's pages not the file's), `pull` (`posix-mmap-pull.txt`: `posixtest --child-mmap-pull` maps a file on the stick, the stick is pulled; its copy from the mapping through a pipe is `EIO`, its touch ends it with the kernel's line about the file's pager; the shell and `/data` fine), `coherent` (`posix-mmap-coherent.txt`: `posixtest mmap-coherent`, a write through the file seen by a shared mapping and by `read`; needs fat to keep mappings coherent with writes, M12.5's PF2), `spawn` (`posix-mmap-spawn.txt`: a copy of posixtest on `/data` is `EACCES` to `posix_spawn` until allowed; then init's `/svc/exec` seals and starts it and it exits 7: [CR-PLAN](CR-PLAN.md#ce1-notes), "CE1 notes"; started so again, and by the shell's `run`, its `self-image` section passes: `/proc/self/exe` is `/data/pt-prog`, it holds its image, and its spawn of `/proc/self/exe` runs from that very VMO, its job growing no more than for the same program started from the boot image; a child from its image with no hand-over (the shell's way in the background) is told its path by `SR_ARGS`: [CR-PLAN](CR-PLAN.md#cl2-notes), "CL2 notes", and "CS1 notes"). `MMAP_DATA=ext`: the cases with the stick's `/data` an ext volume (`tools/ext-stick.sh`), so extfs and its pager serve the mappings there (M13.6's U10; `spawn`'s copy is made executable first, as a mount with modes wants); the second sticks stay FAT; needs e2fsprogs and mtools |
| `tools/bigprog-test.sh <outdir>` | programs as big as Chromium's on `/data` ([CR-PLAN](CR-PLAN.md#cp1-notes), "CP1 notes"), two shell boots of one stick: the script generates `build/bigprog/bigprog` (the Makefile's `$(BUILD)/bigprog/bigprog`: bin/bigprog with a 300 MiB table of constant pages, page i holding the word i; never in the tree), grows a copy of the image's `/data` to `DATA_MIB` (448) and puts it there as `/data/big`. Boot 1 (`bigprog.txt`): the boot image's 16-page copy checks itself; `/data/big` is refused until allowed, `allow` shows its progress line ("checking /data/big (300 MiB): 10% ... 100%", also on the refusal's whole read) and writes its line and its page table (named by its hash: the script checks it); run from its pages, `lazy` fetches under 1 % of them (fat's "gone" line) and `check` reads every 64th page of the table and its last, each right. The script then changes a byte of table page 38,400 on the image; boot 2 (`bigprog2.txt`): `lazy` still runs, `check` is killed at that page ("a page doesn't match the approved program", fat's mismatch line), the shell lives. About 2 minutes on the PC |
| `tools/ssh-test.sh <outdir>` | the SSH server end to end ([M13-PLAN](M13-PLAN.md#15-tinyssh), "X2 notes"): throwaway keys made with `ssh-keygen` each run (one put in the stick's `/data/etc/authorized_keys`, with an `ssh-rsa` line that must not count), `net.address` set, and `tools/sshcheck.py` running OpenSSH's client on the test host through the peer's `--tcp-forward` to the guest's 22 (`ssh.txt`): commands with their output and exit status (`pxio exit 7`, `false`, `exit 5`); an interactive session on a pty (the prompt, `TERM=xterm-256color` as sent, `pxio size` 80x24, then 100x30 after a window change, Ctrl+C on `sleep 100`, `exit 7` as the client's status); the host key the client saw against the fingerprint init logged; a wrong key, a password attempt (only `publickey` offered, the askpass helper never run) and another user name refused; 8 sessions at once and a 9th connection refused; 4 sessions' 3000 lines at once, whole; a session killed mid-output after 256 KiB of a 4 MiB file; then `ps` from a session of its own (nothing left of the others: one session shell and console, the listener and one tinysshd); `kill sshd` run in a session ends that session, init starts sshd again, and `ps` again shows nothing of it; and the server still serving. Then a second boot of the same stick (`ssh-again.txt`, the saved image): the same host key (the first boot's `known_hosts`, `StrictHostKeyChecking=yes`, the same fingerprint in both logs, no key made). PASS needs both scripts, `sshcheck.py`'s PASS twice and the runs' VLAN checks. Needs `ssh` and `ssh-keygen` on the test host (the PC's WSL has OpenSSH 9.6). About 3 minutes |
| `tools/fetch-test.sh <outdir>` | `fetch` end to end (`fetch.txt`, [HISTORY: M9.5](HISTORY.md#m95-network-streams)): with `net.address` in the stick's settings, `tools/httptest.py` on the Mac and the peer's `--tcp-relay 8000:<its port>`, fetch saves a 6 MiB file by Content-Length, the same chunked, a body ended by the connection, a file by name (`mac.jam`) and one at the end of three redirects (301, a relative 302, an absolute 307), and writes one into a pipe (`| wc -c`); it refuses four redirects, https typed and redirected to, a head over 16 KiB (one giant line, and lines that never end), a head dripping a byte a second (given up at 20 s), a body cut short, a broken chunk size and a 404, each with exit 1 and nothing left on `/data`; Ctrl+C stops it (130). Afterwards, with mtools, the saved files' SHA-256 must be the server's and no `x`, `x.part`, `y` or `y.part` may be on the stick. About 2 minutes |
| `tools/serve-test.sh <outdir>` | `serve` end to end (`serve.txt`): a 5 MiB file and a page copied onto `/data`, served on 8080 and 80 (below 1024: bin/serve's `svc net listen low`) in the background while the shell goes on; `tools/servecheck.py` on the Mac fetches through the peer's `--tcp-forward` (two Mac ports to the guest's two) with a silent connection held open all along: two curls at once (two paths, the same file; their SHA-256), `curl -I`, a range, the page (text/html), a request that isn't HTTP (400) and a head over 8 KiB (431), then nothing served after `serve stop`; the shell's side refuses a port served already, a folder and port 0, the request lines and `serve`'s list are checked, and `run nolisten` (a program whose list has `svc net` without `listen`) must be refused listeners on 8081 and 80 (ERR_ACCESS_DENIED, neither `/svc/net-listen` nor `/svc/net-low`); then a boot with no network card (`serve-nonet.txt`): `serve` says "no network address yet". About 2 minutes |
| `tools/speed-test.sh <outdir>` | the throughput tester (`speed.txt`): `tools/speed.py server` on the Mac behind the peer's `--tcp-relay` and `--udp-relay 5201`, and a `--tcp-forward` to the guest's 5202: `speed 10.2.21.174 -t 3` (TCP out), `speed mac.jam -r -t 3` (TCP in), `speed 10.2.21.174 -u -t 2` (UDP out, the losses counted), a port nobody serves (refused: the relay resets a SYN to a port it doesn't relay, as a closed port does), then `speed -l 5202` with `speed.py client` sending and receiving, and sending again through a second forward that drops `SPEED_LOSS` percent (default 2) of its data segments to the guest: the relay must see SACK blocks and resend at most 3 times the bytes it dropped (the guest keeps what comes past a hole: about 1.0 times; without that, 38 times); stopped with Ctrl+C (exit 130), and a 30 s send stopped after 2 s the same way; both sides' lines printed, and the relay's counts for the lossy run. QEMU's numbers, through a relay in Python (2026-10-02, 2 CPUs: TCP out 58, in 70, UDP 109 MB/s; the Mac's client 69 out, 47 in; 2026-10-04, 2 % lost: 40.9 MB/s in, 11.8 before segments past a hole were kept), not the PC's. About 1 minute |
| `tools/netlog-test.sh <outdir>` | netlog end to end (`netlog.txt`): the whole log of two boots and the panicked one's, from the first line, with the receiver late and paused, netlog and netstack killed ([netstack](#netstack)); the serial copy it compares with may hold the shell's echo of the script's typing inside or before a kernel line, which the comparison allows (only characters the script typed, never a character the file's line lacks) |
| `tools/initnet-test.sh <outdir> [static\|dhcp\|nohost\|off]` | the regression run's network ([above](#the-regression-runs-network)): the plain `init` boot (utest, usbtest, posixtest) with QEMU's e1000e, the peer answering netlog into a folder and `net.host = 10.2.21.174` in the stick's settings (`static`: with `net.address`; `dhcp`: the peer's lease). The run must end `run complete: no problems` with the root job clean (netlog, the DHCP client, netstack and logd stopped by init), every frame tagged VLAN 21, and the receiver's one file must start at the boot's first line and hold every serial line up to `init: the run is over` in order (so every program's result line, utest's summary and volumes' stop line), with no receiver note and none of the kernel's end-of-run lines; the stick's `/data/logs/boot-NNNN.txt` (logd, read back with mtools) must start at the first line and hold every line up to the last program's result. `nohost` and `off`: the same settings without `net.host`, or with `netlog = off`: one line says why, and no netstack, netlog or logd runs and nothing reaches the receiver. About 1 minute each |
| `tools/netprobe-test.sh <outdir>` | the RTL8125 driver's boot words in QEMU, which has no RTL8125 (the probe and the send test themselves run on the PC only): a `shell netprobe` boot (`netprobe.txt`), a `shell netsend` boot (`netsend.txt`) and a `shell net` boot (`netserve.txt`) where devmgr says it has the word and binds and runs nothing for it, and again after a `reboot` (kexec keeps `net`), then a plain `shell` boot whose log mentions none of them. utest covers the rest: `netframe_classify` and `netframe_short_frames` (`drivers/include/jam/netframe.h` over hand-made frames: untagged, tagged 21, another VLAN, 4095, priority-tagged, QinQ 0x88a8 and 0x9100, every short length); `netframe_tag` (every length 0-1600: 14-1514 tagged, short ones padded with zeros over a dirty buffer, the rest refused; VLANs 1 and 4094 yes, 0, 4095 and wider no; a buffer one byte short), `netframe_tag_refuses_tagged` (EtherType 0x8100, 0x88a8 or 0x9100 at any length never leaves, and nothing sendable stays behind), `netframe_tx_check` (lengths 18 and 1518, every change to bytes 12-17: another TPID, a priority, DEI, another VLAN, a tag inside), `netframe_tag_copy_is_the_frame` (the caller rewriting its frame after the copy changes nothing; a change to the copy is refused), `netframe_rx` (VLAN 21 kept at any priority and untagged correctly; untagged, VLAN 0, other VLANs, outer tags, a tag inside ours, every short length and over 1518 dropped, each by its reason); `rtl8125_write_guard` (the transmit registers refused in every width and overlap, the command register's transmit bit, TDFNR, nothing else), `rtl8125_txq_guard` (the other queues' ring addresses and the tail and close pointers refused in every width and overlap; through the OCP windows, which take whole 32-bit writes only, a 16-byte descriptor format, a second transmit queue, pause advertised or a PHY test mode refused, every value the bring-up writes allowed), `rtl8125_tx_gate` (full mode with a VLAN only), `rtl8125_args` (the modes, `netprobe` winning over `netsend`, hostile `vlan=` and `arpto=` words), `rtl8125_arp` (the probe's exact bytes, tagged and checked like any frame; the reply and its near misses), `rtl8125_stays_off` (the driver without a VLAN, and the probe, the send test or the netdev service without hardware, ends at once with exit 0 and an empty job); `rtl8125_txdesc` (`drivers/rtl8125/txdesc.h`: the 32-byte descriptor, the transmitter refused unless the chip's format bit agrees, the end-of-ring bit on the last descriptor only, a chip walking the ring in 32-byte steps finds every descriptor in order), `rtl8125_kick` (a stuck descriptor gets a handful of extra doorbells in 4 s however often the driver looks, never one within 1 ms of its own), `rtl8125_tx_verdict` (the chip's tally against queued and handed back: fewer sent is progress, only more sent is foreign); `rtl8125_guard` (`drivers/rtl8125/guard.h`, failing closed: frames in flight carry on, one frame more than queued, PAUSE TX with the link up, a tally gone backwards or 10 looks without one stop the chip; a 100000-step healthy run never trips, and comparing at the ask instead of the look would), `rtl8125_dump` (the tally dump's bookkeeping: one out at a time, once a second or 1 ms after a reap or a request, given up on after 10 ms), `rtl8125_guard_fake` (`guard.c`, `regs.c`, `chip.c` and `tx.c` linked in over a fake chip whose registers and DMA memory are the test's: each stop turns the transmitter off and resets the chip, in full mode and in the probe), `rtl8125_stats_nowait` (`netdev.stats` and the "so far" lines answer from the last dump that landed: 1280 requests against a chip that never dumps take no wait, where each used to cost 10 ms); `netserver_session`, `netserver_tx`, `netserver_rx` (the network drivers' netdev server, `drivers/lib/netserver.c`, linked in, over a fake card with the test as netstack: info and stats, one session at a time, open refused on the session channel, a closed or orphaned session replaced, the rights open hands out; netstack's frames reach the card byte for byte, bad lengths, flags and tags counted and never sent, a card out of descriptors holds the ring until they come back, hostile counts bounded to a ring per turn; received frames into the rx ring with NETDEV_SIG_RX, a full ring and no session dropped and counted, NETDEV_SIG_LINK) |
| `tools/update-test.sh <outdir>` | `update`'s check in init without a network (`bin/updtest` hands init the build from files on `/data/update/`, put there with mtools: this build's kernel, its boot image with one more file `update-marker.txt`, and their manifest from `tools/update-server.py --manifest`, signed with a throwaway key whose public half the stick's build has (`tools/update-test-key.sh`, keys in `<outdir>/testkey`, never `~/.config`), once more with a second key, once saying the other network default (`untagged` for a VLAN build, `vlan21` for an untagged one), and a signed manifest for two files that are no kernel), one run, three boots: `run updtest bad` (19 damaged offers, each refused for its own reason: a kernel or boot-image byte changed, the kernel 4 KiB longer or cut to half, a VMO shorter than its length, a garbage, cut or short-signature manifest, a format-3 one and a signed one with a must-understand line (`!future-must`: "needs a newer build"; the same changed after signing: its signature), an unsigned one, one changed after signing, one signed by the second key, one carrying another manifest's signature, one handle, an unknown flag, the build with the other network default, two files that match their signed manifest but are no kernel; the build's manifest with an extension line no build knows, signed, check only: taken; and the good build, and the other-network one forced (`update -f`), offered check-only: accepted, not loaded), then `run updtest wire` (the offer's wire built byte by byte: a message of another protocol or of the older form, a manifest past 1024 bytes, a count that is not the bytes there, a byte after or missing, four VMOs, a channel end for a file: each refused by the generated server before init looks at it; no VMOs at all is an answer; four malformed offers and then the good one, check-only, on one channel: accepted; init logs 20 refusals in all), then `reboot`: no marker (the stored kernel untouched); `run updtest good` (accepted and stored, init's "Update loaded" notice on the desktop), then a click on its Reboot button (a USB mouse) is init's `reboot`, which reads nothing from `/esp` and the marker is there (the fetched build runs); exactly one `kexec_load` succeeded. Then a second run on a build without a key: `update`, `update -w` and `update -m` fetch nothing and say why, `update -n -r` is a usage error, and `run updtest nokey` (the build offered plain, check-only and to be written to the stick) is refused for that alone. utest covers the parsers, the signature and the fetcher: `update_manifest*` (the format, each line wrong each way, the `net` line's values, the `menu` line's, every prefix, every byte changed, random bytes), `bootmenu_*` (the boot menu's check, `<bootmenu.h>`: a menu shaped like `boot/limine.conf` passes, each rule broken refused at its line, a missing file named, random damage), `update_build_net` (build.txt's `net` line), `update_signature` (RFC 8032's tests 1-3, a manifest `jamos-sign` signed, every byte it covers changed, another key, a bent signature, none; the key file's format), `updwire_*` (golden bytes from the Python tool, every truncation and field, random datagrams), `updfetch_*` (the window against a fake server on a scripted clock: clean, lossy with repeats, reordering and forged replies, the snapshot gone once and always, no answer, a bad or oversize manifest, a lying file size, a boot menu fetched as a third file and never asked for without its line) |
| `tools/update-net-test.sh <outdir>` | `update` over the network (`updnet` run, QEMU_NET with the peer's `--update`; `make -s image` first): build B is this build's kernel with its version string's last character changed and its boot image with `build.txt` saying `git b0b0b0b` (and this build's `net` line) and one more file, `update-marker.txt`; `net.address` and `net.host = 10.2.21.174` in the stick's settings; A and B have a test key (`tools/update-test-key.sh`) and the server signs with it. Each `update` is a new client of `tools/update-server.py`'s `PlannedServer`, which serves the next plan: `damage` (a kernel byte changed after the manifest: init refuses the SHA-256), `wronghash` (the manifest's boot image SHA-256 wrong: refused), `truncated` (the boot image half the manifest's size: the fetch fails), `gone` (the server stops answering after 300 replies: the fetch fails), `badsig` (the manifest changed after it was signed: init refuses the signature), `othernet` twice (the signed manifest says the other network default: `update` refused by init, then `update -f -n` taken, checked and not loaded), `mustknow` (the signed manifest has a must-understand line, `!future-must`: init refuses it, "needs a newer build"), `extension` (an extension line no build knows, signed, with `update -n`: skipped, B checked), then `update -n` (B fetched and checked, `old -> new` said, nothing loaded), `update -m` (loaded into memory only: no reboot, `version` still A's, `sha256sum /esp/boot/jamos.elf` still A's kernel) and `update` (loaded and written to the stick, and no reboot: `version` still A's, the stick's kernel B's); then `reboot` (kexec, nothing read from `/esp`): `version` is B's and the marker is there; `reboot -f` ends the run. Exactly two `kexec_load`s, every frame tagged VLAN 21 (the peer's and the pcap's checks; the run boots with `vlan=21`). Then `updcold`, a cold boot of the stick it left (what `reboot -f` starts): B, with the marker. About 2 minutes |
| `tools/update-write-test.sh <outdir>` | `update`'s stick write (`make -s image` first): the stick's build A has a test key (`tools/update-test-key.sh`), build B is made as `tools/update-net-test.sh` makes it, signed, and build C the same with another version and git c0c0c0c. QEMU runs, each a cold boot of the stick image the last one left (`QEMU_SAVE`, `QEMU_IMAGE`): `wfail` (`run updtest writefail`: B to be written with a test's failure at each step, making room, once the older previous build is removed, half way through the new kernel, between the two renames of the new build: each answered not written with the stick booting A, renamed back after the swap began (the second failure removes the previous build; the fourth write copies A as it first); then `reboot -f`; on the Mac, with mtools: A under both names, no `.new` or `.old` file), `wstop` (on a copy of that stick: B and C written in turn, `run updtest writestop <n> b|c`, each stopped dead after change n of the swap's eight changes of names, `UPDATE_OFFER_STOP`, as a power cut there, and after each `run updtest espcheck`: one of the two entries holds a whole build, A, B or C, read and hashed from `/esp`; then a write of C with no stop: on the Mac C as the stick's build, B as the previous one, no `.new` or `.old`; `wstopcold` and `wstopprev` boot that stick's two entries: C and B), `wnet` (a cold boot runs A; `update -m` over QEMU_NET, the peer serving B: loaded into memory only; `reboot` (kexec) runs B, `version` and the marker are B's, the stick's kernel still A's; then `update -w`, the old spelling of plain `update`, with B running: written, no reboot (one kexec in the run); on the Mac: B as the stick's build, A as the previous one, no `.new` file; every frame tagged 21), `wcold` (a cold boot runs B) and `wprev` (`QEMU_BOOT_PREV=1`, the boot menu's "Jam OS (previous build)": A), then `wfull` (an old stick made by the script: a 63 MiB ESP as sticks had before 2026-10-12, A as its build and its previous build; `run updtest writefull`: B loaded but refused at ROOM with `ERR_NO_SPACE`, init saying the ESP can't hold three builds; on the Mac its ESP's builds unchanged, no `.new` or `.old`). A keyless build's `update -w` is `tools/update-test.sh`'s. `WRITE_RUNS` picks runs (`wfail wstop wnet wfull`; `wnet` includes `wcold` and `wprev`; without `wfail` the script makes the stick as `wfail` leaves it, A as its build and previous build). With `QEMU_BOOT_NVME=1` every boot of the stick is a boot of an installed drive (the image made Jam OS's GPT layout on NVMe; the ESP is at 1M still): `QEMU_BOOT_NVME=1 WRITE_RUNS=wstop` is the swap's power-cut points on NVMe (M12.7 stage 3; `wstopn` in a batch). About 9 minutes |
| `tools/update-menu-test.sh <outdir>` | `update`'s stick write and the boot menu (`make -s image` first; `user/services/init/espmenu.c`): the stick's build A has a test key and an old menu (`boot/limine.conf` without its IOMMU entries); build B is made as `tools/update-write-test.sh` makes it; the new menu is `boot/limine.conf`, a bad one the same with "Jam OS (previous build)" booting `/boot/jamos.elf`; manifests for B signed with the test key, without a menu and with each. Every run keeps the stick's menu (`QEMU_MENU_AT=efi`). `menus` (`run updtest menubad`: a menu byte changed refuses the update, its SHA-256, and so do a menu longer than its manifest says, its VMO missing, a menu with a manifest that names none, a menu length with none; check-only and RAM-only offers with the menu are taken and leave the menu alone; then `run updtest menuwrite` of B with the bad menu: the build written, the menu refused by the check; with no menu line: the menu untouched; with the new menu: written, the old as `limine.conf.prev`; again: "the stick has this boot menu already"; `run updtest menucheck` between them says which whole menu Limine reads; on the Mac the new menu, the old as `.prev`, no `.new`, no spare), `stops` (the menu's swap stopped dead after each of its six changes, `UPDATE_OFFER_STOP` 9 to 14, as a power cut, each followed by `menucheck`: always a whole menu, from the spare `/esp/boot/limine.conf` after 12; the next write undoes a cut swap), `cut` (a stop after change 12 and a firmware reset: no `/esp/boot/limine/limine.conf`, the spare is the stick's menu; that stick, with no menu of the test's, boots B from the spare's default entry), `settle` (the next write undoes it and writes its menu), `net` (`update -n`, then `update -w` with the menu damaged on the way: "a file's SHA-256 isn't the manifest's: menu"; plain `update` from a server without menus: the build alone, no reboot, then `reboot`; `update -w` with the menu: written, "this is the build running now, and the stick has it already", then `reboot`; every frame tagged 21) and `entries` (that stick booted by Limine's own pick, then each of its menu's 16 entries that boot, through a copy of the menu with `default_entry` at `/esp/EFI/BOOT/limine.conf`: each starts its kernel, B or A for the previous build, with its command line; each boot stopped at the kernel's `loader:` line). `MENU_RUNS` picks runs. With `QEMU_BOOT_NVME=1` every boot is of an installed drive, `entries`' and `cut`'s own QEMU too (the image attached as NVMe): `QEMU_BOOT_NVME=1 MENU_RUNS="menus stops"` is the menu swap's power-cut points on NVMe (M12.7 stage 3; `stopsn` in a batch). About 15 minutes |
| `tools/update-pc-test.sh <outdir>` | `update`'s stick write as on the PC (`make -s image` first): a stick that writes like a real USB 2 one (`QEMU_STICK_THROTTLE`) and a second stick at `/usb0` (MBR, one FAT32 partition of type 0b, like the owner's SanDisk; `PC_SANDISK=0` leaves it out); the peer serves build B, made and signed as `tools/update-write-test.sh` makes it. Four runs (`PC_RUNS` picks): `fast` (100 write commands a second and 10 MB/s, `PC_THROTTLE` overrides it, `""` for none: plain `update` answers "written" within `PC_WAIT` seconds (150) and doesn't reboot; on the Mac the stick holds B, A as the previous build, no `.new` or `.old` file, and the served boot menu, the stick's own kept as `limine.conf.prev`: every run keeps the stick's menu, `QEMU_MENU_AT=efi`), `again` (that stick boots B; `update -w` of B writes nothing: "the stick has this build already", "... this boot menu already", and the shell says B is the running build and the stick has it), `stuck` (3 write commands a second: the write stops at its 120 s limit, `update` answers "not written" with B loaded all the same, `/esp` is back, `reboot` (kexec) runs B from memory, the stick still boots A with no `.new` or `.old` file) and `reboot` (Ctrl+C during the write, then `reboot`: kexec within 60 s into B, the stick still boots A). init's progress lines (`init: update: write: ...`) are printed. Before the fix of 2026-10-02 the `fast` write took 445 s (one WRITE command per 512-byte cluster) and `update` gave up after 300 s; now about 6 s. All four about 9 minutes, `fast` alone about 2 |
| `tools/sticks-test.sh <outdir>` | other sticks (`sticks.txt`): six more disk images (`tools/mkstick.py`) plugged and pulled through the monitor: an MBR FAT32 stick, one with no partition table, one made writable and pulled mid-copy, one with a blank FAT32-typed partition and a foreign one, one of noise, and a Linux stick (M13.6's U7a: an MBR with one partition of type 83 holding the ext4 volume Linux's mke2fs made, `tools/extfix/linux1k.img.gz`: bin/extfs mounts it read-only at `/usb0`, Linux's names (`Makefile` and `makefile`), an indexed directory of 150 entries and a link read, writes and `mount -w` refused, `disks` shows `/usb0 (ext)`). Afterwards, from the host: the file written after `mount -w` is on the image (mtools) and the refused ones are not; the images that were only read, or held nothing to mount, are byte for byte unchanged (never written, never formatted; the Linux stick's too); volumes' desktop notices: one "USB stick added" (size, where, read-only) a stick mounted, none for `mount -w`, one "removed" a stick pulled, none at the reboot |
| `tools/nvme-driver-test.sh <outdir> [case...]` | drv/nvme's controller core, one boot per case, all at once, each on a drive image of random bytes it makes: `init` (the `init` run with an NVMe drive and `nvmetest=selftest`: the namespace's first and last MiB hashed in the driver equal `sha256sum` of the image's ends; devmgr's stop gets the normal shutdown notification), `iommu` (the same with `QEMU_IOMMU=1`, no fault), `ns` (a namespace with metadata not served, a 4096-byte one served), `reset` (a controller reset with the reads out: sent again, the same digests), `abort` (the first read timed out at once: an Abort, answered), `kill` (`kill nvme` while it reads in a loop: the restart disables the controller the dead driver left running and reads the same). Every case: the image unchanged, and QEMU's trace of every command only the read-only list (admin 00 01 02 04 05 06 08 09, I/O Read), and not empty |
| `tools/nvme-test.sh <outdir> [case...]` | M12.7, drv/nvme on emulated NVMe drives (`QEMU_NVME`): read-only on every drive that isn't Jam OS's, and (stage 2) writes to a Jam OS drive only. Cases `windows` (a PC-like GPT disk from `tools/nvme-img.py`: four partitions listed by the shell's `disks`, the ESP mounted read-only at `/nvme0` and a file on it read, `mount -w /nvme0` refused ("not on a Jam OS disk"), `disk smart nvme0` before and after, and `disk digest nvme0`'s region lines (ending `disk digest: done`) equal `tools/disk-regions.py`'s on the image), `twons` (a second namespace is not served), `4k` (listed and digested, not mounted: "no FAT", fat reads 512-byte blocks only), `damaged` (primary GPT broken), `kill` (the driver killed after a read), `iommu`, `block` (utest's NVMe cases, `utest only nvme_`, with `nvmetest=slow`: `storage` and `block` on the PC-like disk, 32 reads in flight answered by txid, a client that leaves with reads out and the drain rule in the driver's log, and `storage.digest`'s region lines equal the host's), `write` (stage 2: the PC-like drive and a blank one, `utest only nvmew_` straight at the driver: every writable open, write and `create_table` on the first refused, the second prepared, written and read back; a second boot of the same images finds it Jam OS's, refuses `create_table`, reads the first boot's blocks and writes again; after each boot `tools/nvme-written.py` checks the Jam OS image's table, its written blocks and that nothing lies outside its table and partitions, and every traced Write (`-trace pci_nvme_write`) lies inside them), `wiommu` (`write` behind QEMU's VT-d, no fault), `prepare` (stage 2's volumes, init and shell: the PC-like drive and a blank one, each found by its QEMU serial in `disks`; `disk prepare` of the PC-like one shows it isn't empty (its GPT and partitions) and asks for its erase, answered with the wrong end of its serial: nothing written; the blank one's serial's last four characters typed wrong, then right: its table, both partitions formatted and mounted read-only as `/nvme1` and `/nvme2`, `disks` showing a Jam OS disk, `mount -w` refused on the PC-like drive and "a Jam OS disk already" on the prepared one, `mount -w /nvme2`, a file copied there and `mount -r`; then the PC-like image unchanged, `tools/nvme-written.py` on the other, its ESP and data partition FAT volumes read with mtools and the file in the data partition, and every traced Write inside the Jam OS drive's table and partitions), `piommu` (`prepare` behind VT-d), `cutprep` (the blank drive cut after `create_table`'s first write, `tools/mkgpt.py --cut backup-array`: the driver calls it blank and `disk prepare` finishes it) and `cutbad` (the same with one stray byte in the array's zeros: not empty, so `disk prepare` shows data Jam OS doesn't recognise and asks for its erase; the serial's end right, then not `erase`: both images unchanged, no Write in the trace), `cuterase` (an erase of a GPT drive cut after `create_table`'s primary entry array, `tools/mkgpt.py --layout jamos --break primary-array`: volumes' mark reads Jam OS's from the backup, the driver calls it someone else's, so `disk prepare` shows it as not empty and asks for its erase again, and the serial's end and `erase` make it a Jam OS disk, with `prepare`'s checks), `erase` (a drive that isn't empty erased, the owner's 2026-10-14 decision: two PC-like drives; the second's erase answered wrong; the first shown (GPT, 4 partitions, its ESP's FAT volume and mount), its serial's end and `erase` typed: erased and prepared, a file written to its new data partition; on the host `tools/nvme-written.py --before` (every block that differs from the image as made lies in the new table or partitions), the new ESP holds none of the old one's files, every traced Write inside the table and partitions, the second image unchanged), `erasestop` (the same drives, never erased: the serial's end wrong; right, then `yes`; right, then Ctrl+C; `disk digest` of its first 34 blocks the same before and after and the host's; `disk prepare usb0` refused as the boot disk; both images unchanged, no Write in the trace), `diskro` (the PC-like, a Jam OS and a blank drive booted with `diskro`: `utest only nvmero_`, every drive refuses a writable open and `create_table`, no Write in the trace), `jam` (M13.6's U7a, on the test PC: a Jam OS drive whose ESP is a FAT volume and whose data partition is an ext volume Linux's mke2fs made with Jam OS's features, beside the Windows-like drive: bin/extfs on it as `/jam`, read-write; `/usr`, `/var`, `/home` and `/tmp` mounted by init as views of it and the old `/tmp` emptied; files written through `/jam` and the views read back through the other name; `disks` says `(ext)`; `mount -w` of the Windows-like drive's ESP refused; then `install` over the drive, which holds its data partition writable for init alone and lets go (`/jam` and the views gone and back, `/data/etc` copied in); on the host e2fsck finds the volume clean after its journal is replayed, the files are in it, the ESP holds the build, the Windows-like image is unchanged) and `checker` (no QEMU: the trace checker on made-up traces). Every case keeps a Windows-like drive attached: its image's SHA-256 before and after is identical (every other image's too, but the Jam OS drive's in `write`), and QEMU's trace of every command (`-trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd`, QEMU 10.0.2) holds only Read on the I/O queue (`write`: also Write and Flush) and the stage's admin opcodes (`tools/nvme-trace-check.py`); a silent or unparsable trace fails. The guest checks grow with the driver stack (`JAMOS_NVME_LEVEL`, by default what the tree has; its header lists the levels). Run cases in parallel with an `<outdir>` each |
| `tools/install-test.sh <outdir> [case...]` | M12.7 stage 3, the installer (`install <disk>`, `user/services/init/install.c`; [I2 and I3 notes](M12.7-PLAN.md#i2-notes)): a stick image with files in `/data/etc` (one, one in a subdirectory, an empty one) and a blank 1 GiB NVMe image (`tools/mkgpt.py --layout blank`, serial `jamos-nvme0`: the typed end is `vme0`); the PC only (`make -s image` first). Cases, each its own `<outdir>/<case>` (run them as batch shards): `install` (`install` with the wrong end of the serial: nothing written; then the right one: "installed on nvme<N>"; on the host with mtools Limine, the build as the current and the previous build (the stick's files byte for byte) and the stick run's boot menu are on the ESP, `/data/etc` is the stick's, `tools/fatcheck.py --require-clean` finds both volumes clean and `tools/nvme-written.py` the table Jam OS's; then a cold boot of the drive alone, `QEMU_BOOT_NVME=1 QEMU_MENU_AT=keep`: the kernel names the drive's GPT disk GUID as the boot disk from the loader, init gets `bootdisk=gpt:<GUID>`, volumes mounts its `/esp` and `/data`, the command line is the stick run's (the installed menu's), `version`, `/data/etc` and the ESP's build are the stick's), `again` (a second install over a Jam OS drive: after the first, a file put on its `/data` and `/etc/probe.txt` changed, and on the host the ESP given an old build, a spare menu and leftovers (`.new`, `.old`, `.prev`); the second stick boot: "a Jam OS disk already", the ESP is the stick's build again with none of the leftovers, `/data` as it was left (`/etc` not copied again), the disk's GUID the same, the cold boot reads them), `cut` (the power cut at each change of the install, `INSTALL_CUTS` in the order `prepared etcnew etc cleared limine kernel prevkernel bootfs prevbootfs menu`: a stick boot with `nvmetest=on,instcut<point>` stops the install after that change and the test ends QEMU on init's "cut after" line; on the host the drive is exactly what the point leaves: the files so far whole, the later ones not there, no menu before the last point, `/etc.new` whole or `/etc` renamed, the FAT volumes undamaged and the table Jam OS's; the points are tried in turn on one drive, each install over what the one before left. If the last cut left no menu, the drive booted alone starts no kernel (the firmware finds no Limine, or Limine finds no menu); then a stick boot installs again ("a Jam OS disk already") and the drive is a whole install and cold-boots), and `both` (the stick beside an installed drive and picked: it boots itself, the kernel names the stick's MBR disk id, `/data` is the stick's, the drive is `/nvme<N>` read-only, a write there is refused, the drive's image is byte for byte the same after the run and QEMU's trace has commands and no Write; then the drive picked with the stick beside it as `/usb1`: the kernel names the drive's GUID, `/data` is the drive's), `damaged` (an installed drive whose primary entry array is then changed by a byte: the driver calls it someone else's, volumes' mark still Jam OS's, both volumes mount from the backup table; `install` on it is refused twice before anything is stopped, never "busy", its data partition stays mounted and its image is unchanged) and `nomenu` (a reinstall cut after the new kernel, the old boot image made distinct first, then the drive picked with a copy of the stick as a second NVMe drive: no kernel starts from the drive's ESP; in QEMU none starts at all). Batch shards as run: `cut1` (`INSTALL_CUTS="prepared etcnew etc cleared"`), `cut2` (`limine kernel prevkernel`), `cut3` (`bootfs prevbootfs menu`), each ending with the finishing install and the cold boot. A stick boot reaches the install in about 7 s of guest time and the install takes 2 s (a 24 MB boot image); on a busy PC a case takes 5 to 10 minutes |

## The DHCP and DNS cores (utest)

The DHCP client and the DNS resolver
([HISTORY: M9](HISTORY.md#m9-networking)) are
libraries with their I/O behind a struct of function pointers, so utest
runs them with no network: a scripted edge records what they send and
answer, and the clock is a number the test sets. They run in every
`init` run (the user regression tier), in well under a second.

- **Hostile input.** Every datagram a test hands a parser is copied to
  end exactly at a page with no access (`user/tests/utest/netfuzz.c`), so
  a read one byte past it faults and fails the run instead of reading
  stale bytes. The fuzz tests (`dhcp_fuzz`, `dhcpc_hostile`, `dns_fuzz`,
  `dnsres_hostile`) mutate valid datagrams `FUZZ_ROUNDS` (4000) times
  per sample from a fixed seed (bit flips, special bytes, cuts, planted
  pointers and big counts, bytes added), so a failure repeats.
- **DHCP** (`dhcp_*`: user/services/dhcp/msg.c): the client's messages
  byte for byte; an OFFER as VLAN 21's router sends it; every length it
  can be cut to; options running past their field, a length byte
  missing, wrong lengths, repeated and split options (RFC 3396), option
  52's file and sname fields (and options running past them), masks,
  routers, DNS servers and offered addresses that can't be used.
  (`dhcpc_*`: client.c) a lease from DISCOVER to the probe and BOUND,
  renewing, rebinding and expiry; retransmit times with their jitter;
  NAKs while requesting and mid-renewal; replies with another xid,
  server or address, or in the wrong state; a probe conflict;
  INIT-REBOOT; T1 and T2; RELEASE.
- **DNS** (`dns_*`: user/services/dns/msg.c): names, IPv4 literals, the
  query byte for byte; replies as servers send them (compressed, a
  CNAME, NXDOMAIN with an SOA); records for other names and classes,
  duplicates, CNAME loops; every truncation; compression pointers to
  themselves, forward, in loops and 17 deep; names over 253 characters;
  bad label types and bytes; counts the bytes don't hold; data past the
  end. (`dnsres_*`: resolver.c and cache.c) answers, the cache and its
  TTL; replies from another server, port or id; the retries and a
  time-out; **the slow-peer rule**: a name that is never answered while
  20 others are each answered the moment their reply comes; CNAMEs over
  several replies; SERVFAIL, TC, NXDOMAIN; shared and cancelled askers;
  a full table; ports the edge says are taken; the cache's bounds; the
  fair shares (`dnsres_shares`): ordinary askers' 12 names in flight, the
  13th refused while the system's reserve still takes 4, 6 ordinary
  askers of one name and 2 system ones, each asker's class kept through
  cancels whichever asker moves into its place, and a name counted as the
  programs' once no system asker waits for it.
- **bin/dns's sockets** (`dnsd_sockets`: user/services/dns/socks.c and
  the resolver, against a fake netstack served in-process with net.idl's
  generated server, `udp_rings`, and the socket's rings read and written
  as netstack does): the open written without waiting and the first
  query waiting for it; a port another program has (`ERR_ALREADY_BOUND`,
  known only when the open is answered) makes the next try use another
  port; the query goes to port 53, the answer comes back through the
  loop's port and the socket closes; a port released while its open is in
  flight is closed when the open answers. `dnsd_shares`: askers.c with
  bin/dns's loop for it on a thread, over `/svc/dns`'s and `/svc/dns-sys`'s
  shared channels: 12 ordinary openers, the 13th refused (counted); their
  12 names in flight (the fake netstack never answers), the 13th refused;
  an opener of `/svc/dns-sys` still connects and has its name in flight.

### DHCP and DNS end to end

`tools/dns-test.sh <outdir>` boots twice with `QEMU_NET=1` and the
peer's DHCP and DNS servers; both runs must pass the peer's and the
pcap's VLAN checks.

- `dns` (`tools/shell-tests/dns.txt`, no `net.address`, a 20 s lease so
  it is renewed during the run): bin/dhcp's lease line (10.2.21.100/24,
  gateway and DNS 10.2.21.1), `net` shows it, `host one.one.one.one`
  (two addresses), `host nothing.jam` (no such name), `ping mac.jam -c 2`
  (resolved, then 2 replies), `ping nothing.jam`, Ctrl+C on `host
  slow.jam`, then `run dnstest` (user/tests/dnstest): **the slow-peer
  rule** (the M9 done-when): one thread asks for `slow.jam`, which the
  peer never answers, and while it waits 20 other names are each answered
  in under 200 ms and `mac.jam` is pinged 3 times in under 200 ms each;
  the slow lookup ends `ERR_TIMED_OUT` after the resolver's 10 s, not
  sooner than 9 s; also NXDOMAIN, a CNAME, two addresses, an address as
  a name, a bad name and the cache; and **a busy asker**: a thread keeps a
  `/svc/dns` channel of its own full of requests while 5 new names are
  each answered in under 1 s (the resolver used to stop reading its port
  while one asker had more queued). Then `kill netstack`: bin/dhcp asks
  for 10.2.21.100 again (INIT-REBOOT) and gets it, and the restarted
  bin/dns answers `host www.jam`. Afterwards: two lease lines (renewals
  aren't logged), at least 3 ACKs at the peer (2 leases and a renewal),
  `slow.jam` asked at least 4 times.
- `dns-static` (`tools/shell-tests/dns-static.txt`, `net.address =
  10.2.21.5/24 10.2.21.1 10.2.21.1` in a copy of the stick's settings):
  init says the static address wins and starts no DHCP client (no
  `dhcp:` line in the log, no DHCP message at the peer), and `host
  www.jam` works with the settings' DNS server.

## The call path

The IPC fast path ([ARCHITECTURE.md](../ARCHITECTURE.md#ipc),
[HISTORY: M11.5](HISTORY.md#m115-the-performance-pass)) has tests at three levels, all in the
quick tiers:

- **Counts** (`ktest=pathstat`, `kernel/test/test_pathstat.c`): the path
  trace's own rules (members only, the window, interrupt handlers left
  out, one trace at a time), then what one call costs, exactly: a context
  switch (1 lock, 1 pass), a kernel `channel_call`, the user call against
  a server that reads, writes and waits (5 system calls, 1 read that
  finds nothing, 1 allocation), the same against a reply-and-wait server
  (2 system calls, 0 empty reads, 0 allocations, 0 job charges, 2
  hand-offs, 11 locks), with a deadline (a `clock_get`, a sleeper entry,
  no timer write), and through generated code (2 system calls, the
  kernel's two clock reads). A change that moves a count updates its
  number in the same commit and says so; the benchmark prints the same
  counts (`bench: path` lines, [BENCH.md](BENCH.md)).
- **Kernel behaviour**: `chanslot_*` (a slot reused call after call, a
  big reply allocated, a slot message nobody waits for queued whole,
  handles in a slot, a user caller killed at every point losing nothing),
  `chanwait_*` (a waiting reader handed the next message that fits, in
  order, with handles, two readers, closed ends, no relock after the
  wake), `chanread_kill_loses_nothing` and
  `chanread_reply_wait_kill_loses_nothing` (a server killed inside its
  read or its reply-and-wait: each request still queued or wholly in its
  buffer), `handoff_*` (a higher-priority thread queued meanwhile runs
  first, a waker that doesn't block queues its wakee, a ping-pong pair
  shares one slice), `preempt_resched_*` (a reschedule pending at the
  last unlock still happens, and waits for interrupts to be on),
  `lockdep_*` (the checker's fast path: an empty top slot seen by an
  interrupt handler, a release out of order, the switch with locks
  held); that the checker still refuses a bad order, a lock taken twice,
  a nested class and a class used in and out of interrupts is shown by
  the crash tests `lockorder`, `lockself`, `locknest` and `lockirq`
  (`tools/crash-test.sh`), since each one panics.
- **From user space** (utest): `reply_wait_*` (the mark written before
  the wait and not after a failed reply, a reply to a caller that has
  gone, a second process answering on the same end, the port form, bad
  pointers, rights and handle types), `call_timeout_relative`
  (`CHANNEL_CALL_TIMEOUT`), `fpu_call_keeps_control_words` (MXCSR and the
  x87 control word survive a blocking call, the vector registers come
  back zero, never another thread's), `idl_serve_*` and
  `idl_within_times_out` (the generated server on reply-and-wait, a
  client that went away, `_within`).

The times are the PC's: `bench` ([BENCH.md](BENCH.md)) prints every
switch's line off and on in one run, and `perop` the per-operation lines.

## Channels, handles, threads and the other calls

What [M12](M12-PLAN.md)'s fixes to the system calls are tested by. The
ktests run in every `ktest` (at 4 and 8 CPUs); the utests in the `init`
run (`run utest only <name>` alone).

| Test | What it checks |
|---|---|
| ktests `chan_writer_pays_*` | a writer's unread messages stay charged to its job until they are read or dropped, also after it closed its end or its last process exited: a client that fills a connection and closes it gets no quota back (its first backlog stays its own, so each later connection gets nothing until the server reads); each message read pays the writer back; the reading end closing drops the backlog and pays at once; a job with no process left stays alive on its unread messages alone and goes when the last is read or dropped |
| utest `unread_charged_to_writer` | the same from user space: a child in a job of its own writes 8 KiB and exits unread; its job still pays after its exit, each read pays it back, and ours is where it was |
| ktests `chan_no_handles_mark`, `chan_no_handles_mark_needs_sole_holder`; utest `channel_no_handles` | `channel_set_flags(CHANNEL_NO_HANDLES)`: a message with handles to a marked end is refused (`ERR_NOT_SUPPORTED`) with its handles closed, plain messages still go; the mark needs `RIGHT_READ` and the end's sole holder (an end with two handles, or one in a message: `ERR_BAD_STATE`), is refused while a message with handles waits, and never comes off |
| ktests `chan_txid_per_pair`, `chan_txid_both_ends_call`, `channel_late_reply_dropped`, `chanwait_canceled_by_own_close` | a call's txid has `CHANNEL_TXID_KERNEL` set and comes from one counter of its channel pair, whichever end calls (two ends calling each other at once never draw equal txids), and a new pair starts its own; a reply to a call that gave up is dropped with its handles, any other txid is queued; a reader's own end closing under its wait ends it with `ERR_CANCELED` |
| utest `handles_always_moved` | a send's handles leave the sender's table whatever the status: a refused list (a handle with no `RIGHT_TRANSFER`, the channel's own end), an argument error after the list was read (a length past `CHANNEL_MAX_BYTES`, a bad flag, a reserved field, a bad wait handle, a dead peer), a failed `process_start`; a list the kernel can't read (past `CHANNEL_MAX_HANDLES`, no address) leaves them; nothing is queued and the job's handle and message counts are as before |
| utest `handle_info`; ktest `handle_info_names_the_object` | each kind of object we hold is named, with the handle's rights, its koid, a channel end's peer, a thread's process, a process's job, and what waits on a channel's two sides; no right is needed |
| utest `replace_consumes`; `length_limits`; ktests `status_codes_named`, `m45_event_signal_user_bits` | `handle_replace` consumes the handle whatever the status (rights it hasn't close it, and the job is charged nothing after); a length past a call's limit is `ERR_OUT_OF_RANGE`, nothing cut and nothing done (libos cuts a name itself); every status has a name (the five filesystem codes and the interrupted wait's too); an event's user bits are `event_signal`'s |
| ktests `port_binding_ends_with_last_handle`, `port_bind_on_a_closed_object_refused` | a binding ends the moment its object has no handle left anywhere (the packet it queued stays); one made as the last handle closes is ended by the scan or refused, and nothing is left once the bind returned |
| utest `port_again` | a PERSISTENT binding fires when its channel turns readable, not while it stays so: a reader that keeps to a budget a turn gets no packet for what it left, nor for a message written after; `port_again` brings its next turn with a user packet, and with the port full of user packets by binding the channel again (which fires at once); read to empty, the new binding fires on the next edge as the old one did |
| ktests `proc_start_bad_entry_refused`, `vmo_make_exec_dup_never_unsealed` | an entry outside user space fails `process_start` and the process stays as it was; a duplicate taken while `vmo_make_exec` runs never ends up with write access to the sealed VMO |
| ktests `uentry_fs_base_*`, `uentry_thread_yield_alternates`; utest `thread_fs_base` | threads with FS bases of their own keep them across switches and moves to every CPU (the same value in different address spaces too; one that loads a selector into FS never takes another thread's base); a base past the user range is refused and keeps the old one; `thread_start`'s base is the thread's from its first instruction; two threads calling `thread_yield` alternate |
| ktests `futex_*`; utest `futex_calls` | private futexes: a changed value returns at once, the deadline, bad addresses and flags, `futex_wake` wakes the oldest first and says how many, `futex_requeue` moves waiters, a cancelled waiter leaves the queue, the same address in another address space is another futex, no wake is lost, a word on a pager VMO's page keeps the copy's status (`ERR_IO` when its pager fails the page or doesn't answer, `ERR_CANCELED` for a thread killed while it waits for the page); from ring 3 a thread is woken and a wake with nobody waiting is 0 |
| ktests `intr_*`, `futex_interrupt_before_during_after`; utest `thread_interrupt_calls` | `thread_interrupt` ends `object_wait_one`, `port_wait`, `futex_wait` and `sleep_until` with `ERR_INTERRUPTED`: before the wait (latched once), during it, after one that ended (not lost); a wait whose deadline passed before the interrupt came times out and leaves it latched; a storm of them against a waiter on another CPU loses none and ends none twice; `channel_call` and `channel_reply_wait` are never interrupted |
| ktests `ids_*`, `proc_tid_word_join_unmap`, `proc_cpu_ns_only_while_running`, `sysinfo_cpu_time_never_backwards`; utests `posix_ids`, `cpu_ns`, `sys_public_info`, `vmar_replace_calls` | small pids and tids: in rising order, given back behind the cursor and reached only after the lap, held when given back ahead of it, a full space refused, unique among the living under creates and kills on every CPU; a joiner that unmaps a thread's stack the moment its tid word reads 0 (300 times) never kills the thread; a process's CPU time grows while it runs and stands still while every thread waits, and a thread's never goes back when it is switched out and in during a read; `sys_public_info` needs no handle; `VMAR_REPLACE` without `VMAR_FIXED` or with `VMAR_KEPT_ONLY` is refused with the caller's address word untouched, and with it replaces part of a mapping in one call |
| ktests `proc_kill_spawner`, `stack_cache_skips_stackless_reap` | a spawner (utest's `spawn-loop` mode) killed at a random point of its spawns, 40 times, its children up to the job's 320 threads at once: each kill ends it, its children end by themselves, the job is empty and every page comes back (after a first spawner has grown the kernel stacks' page tables); the stack cache is full afterwards, and thread `main`'s reap at the end of the run (it has no stack of the cache's) leaves it alone, full or with room (M13 B3's init run at 2 CPUs panicked there) |
| ktests `upcall_*` (kernel/test/test_upcall.c; also with `nofpuopt nofpucall`) | fault upcalls ([SIGNALS-PLAN](SIGNALS-PLAN.md) section 1, SG1), with real processes running programs assembled in the test: with no entry a fault is the old kill, line and code byte for byte (int3 is now "breakpoint", RIP after it); each reachable vector (0, 1 by `int1` and by TF, 3 by `int3` and `int $3`, 6, 12, 13 by `hlt`, a non-canonical address and `int $0x80`, 14, 16, 19) reaches the entry with its vector, error code, RIP and every other register; page faults with the address, the error code and the status (nothing mapped, read-only, past a shrunk VMO, a pager's `ERR_IO`, no execute, kernel addresses always "present"); the entry gets the frame 64-byte aligned below the red zone, RSP as after a call with a 0 return address, DF TF AC clear, MXCSR 0x1f80, the x87 default, zeroed xmm0 and ymm0, its own FS base and every other register; every GPR, xmm and ymm, MXCSR and RFLAGS through an `EXC_RESUME` that edited rbx and xmm2; IOPL, NT, RF, VM, VIF and VIP stripped, IF back on, CS and SS the user's; refused frames (the cookie, the sizes, RIP at `USER_TOP`, non-canonical or in the kernel, XSTATE_BV past XCR0, XCOMP_BV, a reserved header byte, MXCSR's reserved bits, a thread that was never given one) kill with their line, never the kernel; a resumed RSP non-canonical, in the kernel half or 0 is taken, and the next fault is the old kill; a fault in the window kills with the first fault's line and ", again in its exception entry"; `EXC_UNHANDLED` is the old kill line for line; `EXC_LEAVE` once, then `ERR_BAD_STATE`, and a fault after it a new upcall; `exception_entry_set`'s refusals and 0 removing it; a frame that can't be written, out of memory and a page that doesn't match the approved program stay the old kill; calls with bad pointers return their errors; 100,000 faults and resumes on 8 threads two to a CPU, each frame and resume its own thread's (registers, xmm0, its own cookie), the job empty after; `upcall_bench_program`: bench's upcall line's program ([BENCH.md](BENCH.md)) runs. SG2: process_info names the fault (`PROCESS_FLAG_FAULT`, its vector and status) after every fault's kill (the window's kill the first fault's, refused frames 13, out of memory and the tamper check 14 with their status) and none after an exit; `upcall_estack_*`: a stack overflow into nothing mapped reaches the entry at the exception stack's top (and is the old kill without one), a fault in a handler running there nests below RSP on it, RSP inside it with no room or a stack smaller than a frame is the old kill, `thread_exception_stack`'s refusals; `upcall_raise_*`: a raise to a thread spinning on another CPU reaches its entry within 10 ms (idle machine) with RIP in the loop and a frame of kind `EXC_RAISED`, one CPU interrupt per latch; one to a thread in `sleep_until` doesn't end the call and comes after its return, never on it; one in the window waits for `EXC_LEAVE` (two raises, one latch); `RIGHT_SIGNAL` checked, a raise of itself comes at its next interrupt, a thread gone `ERR_BAD_STATE`; dropped with no entry or an unwritable frame (the thread goes on); 10,000 raises from 4 kernel threads to 4 spinning threads in 25 bursts: an upcall per thread per burst, every frame a raise's, upcalls and CPU interrupts at most the latches; `upcall_raise_bench_program`: bench's raise line's program runs (2 CPUs or more) |
| ktests `fuzz_*` (with utest's `fuzz-calls` and `fuzz-chans` modes) | hostile programs in a job of their own with limits, twelve threads at once: random system calls with random arguments, and channel ends raced through writes, calls, replies, closes, marks and port bindings; the program exits with its threads inside their calls or is killed at a random moment; the kernel survives, the job is back to zero on every count and no channel end, port or binding is left |
| ktests `aspace_clone_*`, `aspace_maps_shapes` | clone modes ([FORK-PLAN](FORK-PLAN.md#11-the-address-space-fk1-fk2), FK1): without a flag a mapping is a copy where its maximum (the handle's rights) has write, else shared (read-only, execute-only, a guard, a kept mapping), and a contiguous or physical VMO's is shared where it would be copied; `VMAR_CLONE_SHARE` and `VMAR_CLONE_NONE` win otherwise, both together or either with `VMAR_KEPT_ONLY` refused with nothing mapped; a JIT VMO's mapping whose maximum has write is none whatever is asked; the mode is kept by protect (`vmar_protect` refuses the flags), splits, an unmap and a replace that cut it, neighbours merge only with the same mode, and `process_get_maps`' `flags` shows it |

## The IDL (genidl)

`make check` runs `tools/genidl.py selftest` (genidl on small trees of its
own: what it must refuse, and a stale output failing `check`) and keeps
`drivers/include/idl/` current. The generated code is tested from utest, in
the `init` run, on the test protocols of `abi/idl/idltest.idl` and on
`null`:

| Test | What it checks |
|---|---|
| `idl_answer_later`, `idl_later_blocking_clients`, `idl_later_handles`, `idl_async_through_port`, `idl_txid_next`, `idl_txid_mixing` | answering later and calls that don't wait: a server that keeps requests and answers them while it goes on serving, to clients blocked in calls and to one that sends without waiting and takes the replies from a port; blocking and asynchronous calls on one channel end never take each other's reply |
| `idl_serve_reply_wait`, `idl_serve_gone_client`, `idl_within_times_out` | `<proto>_serve` answers in the system call that takes the next request (handle results too); a request it can't take is answered or dropped and the loop goes on; a reply to a client that has gone has its handles closed; `_within` is a timeout from the call's start |
| `idl_slot_take_run`, `idl_slot_idempotent`, `idl_slot_reply_after`, `idl_handle_slot` | requests read into slots and run again with the same answer, `idempotent`, the reply that goes with the next take, a request that carries handles refused without its handler |
| `idl_wrong_protocol`, `idl_big_messages`, `idl_consts` | several protocols in one file: each has its own ordinals, so a server of one refuses the other's request (`ERR_NOT_SUPPORTED`); messages up to 16 KiB; the `const` lines |
| `idl_var_arrays`, `idl_var_refused`, `idl_scalar_arrays` | `u8[<=N]`, the wider scalars and `str[<=N]` both ways: the client refuses an array past its N and a bad `str` before sending, the server refuses the same in a request (and a count past the message's end, bytes left over, padding that isn't zero) before a handler runs, a handler's bad result is `ERR_INTERNAL` |
| `idl_events_to_server`, `idl_events_to_client`, `idl_events_handle_lists` | events with txid 0, never answered, both ways in one protocol, with arrays and handles: a handler owns the event's handles; one that breaks its format, goes the wrong way or has no handler is dropped, its handles closed; an event sent with a txid is refused; a call's reply is taken past an event queued before it |
| `idl_handle_args`, `idl_handle_args_fail`, `idl_handle_lists`, `idl_handle_lists_async`, `idl_handle_server_refuses`, `idl_handle_later`, `idl_handle_results_refused`, `idl_handle_typed` | handles in requests and replies: a client's are gone after the call whatever its status, the server's handler owns what it gets and the loop closes what no handler reached; lists (`handle[<=N]`), typed handles (`handle<vmo>`: a wrong type is `ERR_WRONG_TYPE`), results the handler left unfilled or past their N |
| `idl_fuzz` | [below](#the-protocols-servers-against-hostile-clients): every protocol's generated server |

## Services that outlive their process

fat and the mixer carry on from a dead instance's state
([ARCHITECTURE.md](../ARCHITECTURE.md#services-that-outlive-their-process),
[HISTORY: M11.6](HISTORY.md#m116-services-that-outlive-their-process)). The tests, cheapest first:

- **The pieces, in the `init` run** (utest): `keep_*` (the keep channel:
  put, drop, restore in 64-handle batches with the same objects and the
  same rights, slots the state doesn't know, a keeper that listens to a
  service stopping after a bound and saying messages are left
  (`keep_drain_bounded`), the keeper's refusals of
  other handle types, malformed messages and too many slots or handles,
  a successor's of a malformed restore), `svcstate_*` (a fresh, an adopted and a refused state:
  every header check and a corrupted one set up empty; the slots, the
  commit word and the four cases a successor finds; the `replied` mark;
  a spare waiting before `main` and promoted), `idl_slot_*` (the
  generated `_take_slot`, `_run_slot`, `_idempotent` and the reply that
  goes with the next take), `fat_layout` (every FatFs field adoption
  relies on, against the vendored FatFs), `fat_gather` (held writes go
  out together), `fat_truncate_steps` (a truncate to 0 too big for the
  hold, which goes out in steps: the disk, watched write by write, never
  frees the head of the chain the file's entry still names),
  `disk_fs_restart` (volumes' kept `fs` channel: the mount
  never goes), `disk_own_channel` (volumes' syncs on a channel of its
  own, which a restart keeps too, answered at once while a client floods
  the shared one and reads everything on it), and the exact deaths: `fat_restart_steps` (a script of
  every kind of request run undisturbed, then with fat ended at a held
  write, the commit, a block write of the send, before and after the
  answer: the same answers and the RAM disks the same byte for byte),
  `fat_restart_handles` and `fat_restart_bad_request`. In the kernel:
  `chanread_kill_loses_nothing` and `chanread_reply_wait_kill_loses_nothing`
  (a request is either queued or wholly in the slot). usbtest's
  `storage_fence` (a dead client's queued writes never land).
- **The area scripts** (rows above): `tools/fat-restart-test.sh`,
  `tools/fat-spare-test.sh`, `tools/mixer-restart-test.sh` (the ramp
  exact under 20 kills), `tools/mixer-spare-test.sh` and
  `tools/fat-storm-test.sh` (`storm`; the copies checked on the host and
  both disks by `tools/fatcheck.py`). Each boots once with the warm spare
  and once with `nospare`, and prints kill-to-first-answer
  ([BENCH.md](BENCH.md#m116-qemu-2026-10-05)). The scripts that killed
  fat or the mixer before M11.6 expect no error now: `data-test`'s
  killed fat keeps logd's file open, `mixer-test`'s killed mixer plays
  on, `music-test`'s player never sees the mixer's restart.
- **Two test builds of fat**, made by hand (`make EXTRA_CFLAGS_fat="...
  -DFAT_RERUN_CHECK"`, the flags in `user/services/fat/request.c`):
  every request is run, undone and run again from its slot before its
  commit, as a successor would, and the two runs must answer and hold the
  same ("rerun check FAILED" in the log otherwise); and `-DFAT_UNDO_OFF`
  (`user/services/fat/undo.c`): no undo copy, only to measure what it
  costs.

**On the PC** (after a flash; as in
[HISTORY: M11.6](HISTORY.md#m116-services-that-outlive-their-process)): on the
Mac first, a 256 MiB file of random bytes on the SanDisk and its SHA-256
(`head -c 268435456 /dev/urandom > "/Volumes/NO NAME/big.bin"`,
`shasum -a 256`); on the PC `mount -w /usb0`, then `storm /usb0/big.bin
/data/big.bin 0`, and again at 1, 10 and 100 (an `rm /data/big.bin`
between); `music start`, `storm mixer 2 60` while listening, `music
stop`; `ps` (the spares' memory); `sync`, `reboot -f`, and the sticks
pulled at the boot menu. On the Mac: `grep -E 'storm:|mixer: restart
\(killed'` in the boot's log, `fsck_msdos -n` and `sudo python3
tools/fatcheck.py /dev/rdiskNs2 --require-clean` on the Jam OS stick
(the SanDisk's partition is `/dev/rdiskMs1`).

**Noise.** A kill-to-first-answer sample includes whatever else the host
was doing: on a Mac running several QEMUs the medians double and single
samples reach 50-80 ms. A Mac asleep (lid closed, on battery) stops the
guest for minutes, which shows as silent gaps in the guest's log and
timeouts anywhere: check `pmset -g log` before calling such a failure a
bug. `mixer-restart`'s check excuses the buffers QEMU's own audio
dropped (traced with `hda_audio_overrun`), as mixer-test does.

## devmgr and volumes

devmgr binds and supervises the drivers; volumes
([ARCHITECTURE.md](../ARCHITECTURE.md#storage)) owns the disks, their
filesystem services and the mounts. In utest (the `init` run) the first
group runs against the live devmgr and edu, the second on a mock disk
(`user/tests/utest/diskmock.c`) against the live volumes:

| Test | What it checks |
|---|---|
| `devmgr_addresses` | a device is named by `(kind, a, b, n)`: edu by its ids, its class, its place among the bound functions and its bus address each name it; a field a kind says is 0, or an unknown kind, is `ERR_INVALID_ARGS`; get_driver's pci_index and get_function agree on which function it is |
| `devmgr_kill_process` | `devmgrctl.kill_process`: the driver whose process has that koid is killed wherever devmgr has it, the old koid is gone (the restart is a new process), a koid devmgr doesn't run is `ERR_NOT_FOUND` |
| `devmgr_openers`, `devmgr_ctl_openers`, `devmgr_query_channel`, `devmgr_query_refuses_hda`, `devmgr_device_channel` | `/svc/devmgr` and `/svc/devmgr-ctl` hand each opener a channel of its own, a request answered on that one only and a closed opener's slot free again; the query channel answers the queries and refuses everything that changes something or hands out hardware, and refuses a device that has a device channel (the sound card's, a network card's) however it is named, which the control channel hands out; a device channel for edu answers about edu alone, one at a time, and the query channel hands edu's service out again once it is closed |
| `volumes_watch`, `volumes_release_disk`, `volumes_new_channel`, `volumes_loop_answers` | `volumes.watch` lists every mount then `settled`, a remount is `unmounted` and `mounted` again at a new generation, a watcher that never reads is dropped before `MOUNTS_UNREAD_MAX` events wait for it; `release_disk` stops a disk's mounts in order and leaves them gone; a restart that failed after the mount's channel went tells the watch of the new one; while a remount waits for a slow disk, volumes answers its other requests |
| `disk_open_ro` | `voldisks.open_partition` (the shell's `disk check`): a block channel on a partition of a test disk, read-only (the disk's block says so), a disk that isn't there ERR_NOT_FOUND, nothing written, no mount made |
| `disk_list_channel` | `volumes.list_channel`: a channel that speaks `voldisks` (the disk listing, digest and health) and refuses `watch`, `kill`, `remount` and another list channel with ERR_ACCESS_DENIED; a control channel speaks `voldisks` too |
| `disk_mounts`, `disk_not_boot`, `disk_other`, `disk_vanishes`, `disk_fs_restart` | the mock usb-storage handed over with `volumes.test_disk`, the real fat on its partitions: the boot disk's ESP (read-only) and blank data partition (formatted) are real filesystems, reached from a watch alone; a disk that isn't the boot disk is never `/esp` or `/data` and its FAT volumes are read-only `/usbN`, reopened read-write by `remount`, a blank partition never opened for writing or formatted; a killed filesystem service comes back on the channel volumes kept (an open file stays open, unsynced writes are there, the watch hears nothing); a disk that goes takes its mounts with it |
| `disk_ext`, `disk_ext_none` | M13.6's U7a: a partition that may hold ext (type 83, and the boot disk's data partition) is read-only opened and its first sectors read for the ext superblock; `disk_ext` has a mock disk (`dm_start_sized`: a 16 MiB data partition) whose type-83 partition holds an ext volume that a test-run bin/extfs formatted: volumes starts bin/extfs on it and mounts `/usbN-test` read-write (a test disk's ext volume is written), the file and directory are there and a file is written; killed, it restarts as a new process (no warm spare: the spare is a fat), an open file carried across and the channel the client had still good; `remount` to read-only (writes refused, the disk never asked) and back; `disk_ext_none` a type-83 partition with no ext volume: opened once, read-only, no service, no mount, not one write. The FAT cases count the extra read-only open of the boot disk's data partition |
| `disk_answer_storm` | 199 messages on the mock's `storage` channel ahead of its answers (more than volumes reads a turn, 64): the disk still mounts and goes (volumes used to read 64 and never get another packet for that channel) |
| `volumes_hostile` | [below](#the-protocols-servers-against-hostile-clients) |

`tools/volumes-restart-test.sh` ([Area scripts](#area-scripts)) kills
volumes, devmgr and the boot stick's driver in turn.

## The fs and file protocols' Unix calls (M13.6's U5)

fat and the boot image answer the calls M13.6 added to `fs` and `file`
([the plan](M13.6-PLAN.md#u5-notes)); utest runs these in the `init` run
(`user/tests/utest/fs_attr.c`):

| Test | What it checks |
|---|---|
| `dirent_types` | readdir's `type` byte: put and get agree, and a record whose type is unknown to the protocol or says the opposite of `is_dir` is refused |
| `fat_attrs` | fat's getattr (the root, a file, a directory, a name in another case, the open file's own, the modification time as all three times, blocks in 512-byte units, the errors), setattr (the checks, nothing changed), open's mode and mkdir's mode ignored, `FS_NOFOLLOW`, `FS_DIRECTORY` (`ERR_NOT_DIR`, `ERR_IS_DIR`), rename's exchange refused with nothing moved, `symlink`, `readlink`, `link`, `resolve` and `subview` `ERR_NOT_SUPPORTED`, readdir's types, statfs's magic, name length and features |
| `fat_attrs_read_only` | a read-only volume: no write bits in getattr, setattr and file.setattr `ERR_ACCESS_DENIED`, statfs says read-only |
| `fat_attr_views` | a read-only view (changes refused, no write bits in getattr), a guarded view (every new call that changes `etc` refused, by any spelling, a root at `etc` refused unless read-only) and a key-hiding view (every new call that names `etc/ssh`) |
| `fs_attr_ns` | libos's wrappers through the namespace: on a fat mount and on `/boot` (modes of the boot image, `ERR_NOT_SUPPORTED`, `file_open_mode`, `file_getattr`/`file_setattr`, `fs_statfs_kind`, readdir's type) |

## The IOMMU

The IOMMU ([ARCHITECTURE.md](../ARCHITECTURE.md#the-iommu),
[HISTORY: M11](HISTORY.md#m11-the-iommu)) is on by default (since the PC's sign-off,
2026-10-07) wherever there is a DMAR table; QEMU has none unless a script
adds `intel-iommu` (`QEMU_IOMMU`), so a plain QEMU test run never uses it,
and `iommu=off` turns it off on any boot. It has tests at three levels:

- **Kernel tests**, in `tools/vtd-test.sh` (the table under
  [Area scripts](#area-scripts) says what each boot checks): `vtd_unit_*`
  (the queue and every invalidation), `vtd_irq_*` (interrupt remapping),
  `vtd_domain_*` (domains, edu blocked and let through, a driver's table
  cap, the mute, the handover while translating), `dma_iommu_*` and the
  `dma_*` tests in their translated form, with QEMU's caching mode on and
  off and on a unit without pass-through; and a `reboot` and a panic
  jumping into the next kernel with everything on. The pure ones (`vtd_pt_*`, `vtd_ir_*`, the
  entry bit layouts, the DMAR parser) run in every `ktest`.
- **DMA pins** (`ktest=dma`, QEMU's edu, with and without translation):
  `dma_cap_charges_driver_job` (the cap, each pin and the domain's table
  pages are charged to the job `dma_cap_create` is given, the driver's, not
  the caller's), `dma_pin_charges_pinner` (a pin is the pinner's, never the
  VMO's holder's, and refunded to the pinner on every path),
  `dma_read_only_pin_rules` and `dma_read_only_pin_needs_committed` (a
  read-only pin needs only `RIGHT_READ`, takes only committed pages and
  commits nothing, and a page is pinned one way at a time),
  `dma_iommu_read_only_pin` and `vtd_pt_read_only_leaves` (its pages are
  mapped without write permission: the device's write faults),
  `dma_pin_fuzz` and `dma_pin_races_close` (random pins, unpins, commits and
  closes of the cap checked against a model after every step, and pins
  racing the cap's close), `dma_iommu_unpin_races_device` (a write the device
  has queued while the driver unpins never changes the page once the unpin
  returned) and `dma_iommu_failed_map_unconfirmed_keeps_pages` (a pin whose
  map failed after a page was mapped, with the unit confirming nothing,
  keeps its pages).
- **The deliberate faults**: `tools/hda-test.sh`'s third boot runs drv/hda's
  IOMMU checks (`iommu=on vtdtest`, the "Developer > IOMMU checks" entry on
  the PC).
- **Every device with the IOMMU on**: the area scripts run unchanged with
  QEMU's unit and the word, `QEMU_IOMMU=<mode> QEMU_WORDS=iommu=on
  tools/<area>-test.sh <outdir>`. QEMU_WORDS reaches every boot a script
  makes (a script's own `QEMU_IOMMU` and `QEMU_WORDS`, as hda-test's third
  boot has, win for that boot). The matrix, run at each IOMMU change and
  at M11's join:

  | `QEMU_IOMMU` | Area scripts |
  |---|---|
  | `1` (caching mode on: every new mapping invalidated too) | usb, storage, net, hda, play, mouse, mixer, data, sticks, fetch, serve, tcp, music, kexec-reboot, reboot-firmware, and the `init` run (`tools/qemu-test.sh <outdir> init init`) |
  | `cm0` (caching mode off, as on the PC) | the ones whose devices do the most DMA: usb, storage, net, hda |
  | `eim` (x2APIC destination ids) | hda-test's IOMMU checks boot; vtd-test's `vtd-eim` probe |

  Each must pass as it does without the IOMMU, and the logs must hold no
  `vtd: fault:` line but those a test provokes (vtd-test's and
  hda-test's own: edu at 00:04.0, QEMU's DMA engine as ff:1f.7, each
  sound controller's one read). A `vtd: fault:` line anywhere else is a
  device reaching memory its driver didn't pin: a bug in the driver or in
  the IOMMU code.

What QEMU can't show (its unit reads the tables straight from guest
memory and lets old-format interrupt writes through) is the PC's: the
"Developer > IOMMU checks" entry, everyday boots with every device in
use, `iommu` (no fault but the checks'), `bench`'s IOMMU lines, a `reboot`
and `reboot -f`, then All tests and `soak 10` with it on. All passed on
2026-10-07, and the IOMMU has been on by default since.

## Random numbers

The kernel's generator ([ARCHITECTURE.md](../ARCHITECTURE.md#random-numbers))
has ktests `random_*` (`kernel/test/test_random.c`, `ktest=random`, a
few milliseconds): RFC 8439's ChaCha20 vectors (2.3.2, A.1 #1 and #2,
2.4.2), the output as a known function of the key and the old key gone
after a request, mixing, a due reseed, the stuck-source check, the
machine's source against CPUID (and RDRAND alone where there is RDRAND),
the path without RDSEED and RDRAND, two calls that differ, and 64 KiB
whose byte counts (a chi-square) and one bits stay inside bounds a
working generator leaves about once in 10^9 runs. utest's `random_get`
and `os_random` call it from user space, with the refusals (too long, a
NULL, kernel or read-only buffer).

QEMU's default `-cpu max` has RDSEED and RDRAND. The path without them
needs another CPU model: `QEMU_CPU=qemu64 tools/qemu-test.sh build/test
rnd ktest=random` boots with `random: no RDSEED/RDRAND: seeded from
timing only ...: WEAK` in the log and the RESULTS box.

## Smooth text

libfun's anti-aliased text ([ARCHITECTURE](../ARCHITECTURE.md#smooth-text)).
In utest (the `init` run), every expectation from the font's own numbers
(`user/tests/utest/smoothfont.c`):

| Test | What it checks |
|---|---|
| `font_open` | sizes and weights out of range refused; ascent, descent, line height and capital height at 13 and 26 pixels; each bake's time (`utest: font: ... baked in`) |
| `font_measure` | widths of known strings in both weights and sizes, kerning included ("AV" narrower than "A" plus "V"); "" is 0; `font_draw` returns x plus the width |
| `font_pixels` | 'I', "II" (the second glyph at a half-pixel position) and '-' (rows cut by its edges) at 13 px Regular and 'I' at 26 px Medium, white on black: every pixel's coverage is its share of the glyph's rectangle (from the outline's coordinates), exact where it is 0 or 255, within 1 elsewhere |
| `font_blend` | coloured text over a pattern is exactly `px_over(pattern, argb_pm(colour, coverage))` per pixel, coverage from white on black; the colour's top byte ignored |
| `font_clip` | 13 clip rectangles (across each edge of the text, a box inside, empty, negative, huge, outside): unclipped pixels inside, nothing changed outside; text off each edge and corner of a surface inside a guarded buffer: the surface's part right, the guard untouched |
| `font_ellipsis` | `font_ellipsize` against a search of every cut for 7 strings (malformed UTF-8 and "" among them) at every width and buffer size; no space before the "…"; `font_draw_in`, left and centred, draws exactly the cut text where it says, clipped to its rectangle |
| `font_threads` | four threads drawing titles with two shared fonts get the pixels one thread got; a child that writes into a font's memory is killed (`utest font-write`): fonts are read-only |

On the Mac, `make` builds `build/host/fontpreview` from the same files
and draws `build/fontpreview.png` (the floating windows' title bars at 1x
and 2x, focused and not, and sample text at six sizes): look at it after
a change to the text or the font. `make check` runs `build/host/fontcheck
--check`, the same program with ASan and UBSan: 4,000 random strings
(malformed UTF-8, other scripts, controls) drawn at random places
through random clip rectangles into a surface inside a guarded buffer,
in two fonts, with nothing outside the clip changed; and 4,000 random
cuts, each fitting its width and buffer, a start of the string plus "…",
and the longest such start.

## The terminal windows

The console in window mode ([ARCHITECTURE](../ARCHITECTURE.md#the-terminal-windows)):
its cells, padding and view. In utest (the `init` run), the console's
pure files linked in (`wlinput.c`, `view.c`, `cellpaint.c`;
`user/tests/utest/conwin.c`, `conpaint.c`):

| Test | What it checks |
|---|---|
| `conwin_grid` | the first window's grid on six output sizes in both fonts' cells (three quarters of the output less the padding, at most 160x50, at least 80x24 where it fits); the window asked for holds that grid exactly and fits the output |
| `conwin_padding` | a window's grid inside its 10 pixels of padding: one pixel less loses a row and a column, what is past the last whole cell is margin; maximised and tiny sizes; the padding covers the compositor's rounded corners |
| `conwin_view` | text from the top in a window: the current line on row n after n lines, scrolling only once it reaches the bottom row, how far back the view goes; a clear starts at the top row with the old lines still behind it; every height keeps the current line on the screen; the full screen's current line on the bottom row as before |
| `conwin_font_setting` | `terminal.font`'s values: `smooth`, `bitmap`, nothing else |
| `conpaint_metrics` | JetBrains Mono at 15 px: 9x21 cells, baseline 16, a line 1.35 to 1.45 times the size; every code point a cell can hold one cell wide in both weights; '─' has a full row of ink and '│' a full column (neighbours meet); the box drawing and Latin Extended-A have glyphs of their own (not the box); the bitmap's look 8x16 |
| `conpaint_cells` | a cell writes its own pixels only; a space is all background, the cursor on it all foreground; ten characters (ASCII, Latin-1, Latin Extended-A, box drawing) are exactly `font_draw`'s glyph at the cell's origin and baseline, clipped to the cell; cells partly off a buffer |
| `conpaint_bold` | a bold cell is JetBrains Mono Bold's pixels (`font_draw` with the Bold font), not Regular's, with more ink |
| `conpaint_blocks` | block elements: the full block, halves (upper and lower meet with no gap), eighths, quadrants and three of them with the fourth making the full block, the shades as the colour at 64, 128 and 192 over the background |
| `conpaint_bitmap` | `terminal.font = bitmap`: the 8x16 font's bits for ASCII and the Latin letters, '?' for the box drawing it lacks, bold changing nothing, the bitmap's own half blocks and shades |

The setting end to end (init's `font=bitmap` and `console.set_font`) is
seen in a boot whose stick's settings say `terminal.font = bitmap`: the
log has "init: the terminals' font: bitmap, as the settings say" and
"console: terminal 1: the 8x16 bitmap font"; `terms-windows.txt`,
`desktop.txt` (`tools/mouse-test.sh`) and `screen-test.sh` take
screenshots of the windows.

## Input as events (M12's review W6, W7)

`input` (abi/idl/input.idl) is events both ways: hid sends a key's usage
and modifiers (no character, no repeats), keeps what the compositor's full
queue refuses, and lights what the compositor says. The utests (hid's
against the mock compositor of `user/tests/utest/hidmock.c`; the
compositor's headless on the seat's harness):

| Test | What it checks |
|---|---|
| `hid_typing`, `hid_modifiers`, `hid_rollover`, `hid_composite`, `hid_unplug_and_console_gone` | the usages and the modifier byte of each event, the keypad and the lock keys as themselves, the lights dark at start and untouched by the lock keys |
| `hid_repeat` | a key held 1.4 s, Shift pressed meanwhile: its DOWN and UP and Shift's, no REPEAT |
| `hid_leds` | `input.leds` lights the keyboard (SET_REPORT with that byte), the same lights again send nothing, only the three LED bits go |
| `hid_leds_storm` | 200 lights queued while hid waits for the keyboard to take the first (more than three of its 64-event turns): the last is lit (hid used to read two turns and never the rest) |
| `hid_queue_full` | the mock compositor stops reading while 1200 key events are typed: hid keeps what the queue refuses and, once it reads again, every event arrives in order |
| `keymap_matches_hid` | every key hid sends is its own usage, with Shift or not, and the keymap has an evdev code for it |
| `comp_seat_leds` | a keyboard source is sent its lights after its ready (Num Lock on), every keyboard again when Caps, Num or Scroll Lock changes them (whichever keyboard pressed it), a mouse none; a state that is neither UP nor DOWN is no key |
| `conwin_keys` | (with the rest) a keypad key with Num Lock off (no character) is the key printed on it, as hid made it before |

## The Wayland transport

JWL2 ([ARCHITECTURE.md](../ARCHITECTURE.md#graphics)): batches of whole
messages with a queue of handles per connection, held and merged when a
reader is behind. libjwl's both sides, in utest (the `init` run):

| Test | What it checks |
|---|---|
| `jwl_conn_held_handles` | a batch with a handle the channel can't take now is held with it and goes later with a handle that still works: a write takes the handles it is given whatever its status, so the transport writes a batch with handles only when the channel has room (the compositor's queue filled by hand) |
| `jwl_conn_held_merged` | a client that falls behind while the compositor sends one tiny batch a turn: the held batches merge into blocks of up to a batch (each took about 160 bytes of heap for 28 of the batch's before), and once caught up the client reads every event in order |
| `jwl_conn_handle_queue` | handles are a queue per connection, as fds on a socket: a batch may bring handles before the message that takes them, or a message's two handles come in two batches; each takes the next in order, and a message whose handle hasn't come is a protocol error |
| `fun_no_window` | libfun's window against a compositor that refuses every connection: `gfx_open_on` times out, presenting and closing are no-ops, and the job's handles and message bytes are as before |

## Clients the compositor loses

bin/compositor headless in a job of the test's own
(`user/tests/utest/comp_bad.c`): `comp_bad_requests`, `comp_caps`,
`comp_connections`, `comp_client_crash` and `comp_never_reads` end with
the compositor's handles back where they began and its job's message
bytes back at or below where they began (`ct_msg_back`: a client gone,
however it went, leaves nothing charged to the compositor). The
`comp_live_*` versions run against the desktop's compositor, whose job
the test can't read: `ps -k` before and after shows it (job "compositor",
its message bytes the same).

| Test | What it checks |
|---|---|
| `comp_connect_storm` | 48 connects queued on `/svc/wayland` while the compositor starts (three of its 16-a-turn budgets): every one answered, in order, and the next program's after them (it used to answer 32 and never read the shared channel again) |
| `comp_slow_clients_kept` | 6 clients flood the compositor and read nothing until it drops them as too slow, then keep their ends open: the compositor withdraws what each had not read (`channel_withdraw`) before its error, so its message bytes stay within 6 errors of the start, and each client reads the error first |
| `msgbudget_comp_flood` | a client writes 16 KiB batches of `wl_surface.damage` flat out while the compositor takes 64 requests a turn: the compositor's end of the connection is `CHANNEL_BYTE_BOUND`, so the write that finds 1 MiB waiting is `ERR_SHOULD_WAIT`, with at most 1 MiB and a batch waiting and never `ERR_NO_MEMORY` (before: 1024 batches, 16 MiB, charged up to the root's message budget, which four such clients filled); another client is served meanwhile, and the flooding one round-trips once the compositor is through (`user/tests/utest/msgbudget.c`) |
| `comp_lost_clients` | 4 rounds of a client that closes after a round trip, one disconnected for a bad request and then closed (its slot lingers until then), and one that crashes holding a window: after each round the compositor's handles and message bytes are back at the start (until 2026-10-07 each client whose end was closed before its slot was freed left its port binding, 1216 bytes, on the compositor's job) |
| `comp_fuzz` | 48 seeded rounds (`user/tests/utest/comp_fuzz.c`, M12.1): a client sets up what a real one has (every global, a seat, surfaces, a toplevel, a pool and buffer, a region, a positioner, a clipboard source and device, a cursor shape device), then writes JWL2 batches of its own: real opcodes with arguments chosen to hurt (the int32 ends, caps' edges, other objects' ids, strings without their NUL or not UTF-8, handles of every kind or none), sizes that lie, batches of noise; it never reads. After each round a healthy client round-trips, and at the end the compositor holds the handles and message bytes it held before. A failure names the round and its seed |
| `comp_chaos` | the desktop on, three clients speaking proper Wayland and a fake keyboard and mouse, 2,400 seeded steps at once (`comp_fuzz.c`, M12.1): toplevels made, drawn at their configure's size or any, unmapped, destroyed in order and out of it (the client disconnected and back), limits from 0 to INT32_MAX, full screen, maximised, titles, frame callbacks, buffers destroyed while shown, pools grown, clients closed mid-way; meanwhile Alt held and Tabbed, the window keys, the search box, clicks and drags. Every client answered throughout, the compositor's job empty at the end |
| `comp_live_fuzz` | 16 of those rounds against the desktop's own compositor (`utest only comp_live`, `g1-hostile.txt`), which must keep serving |

## Subsurfaces

`wl_subcompositor` and `wl_subsurface` (`user/services/compositor/subsurface.c`),
in utest (the `init` run; `run utest only comp_sub` alone,
`user/tests/utest/compsub.c`). The paint tests run the compositor's test
scene (its `sub=` and `geom=` commands); the others are clients speaking
Wayland to a headless compositor with the `testwin` power, as the seat's
tests are. `tools/comp-test.sh`'s step 6 shows the same on QEMU's screen.

| Test | What it checks |
|---|---|
| `comp_sub_paint` | a main surface with five subsurfaces (two above it, one translucent, out past its edges; two below it, one hidden under it, one showing past it; a child's child) and a second window whose window geometry is its surface, its child clipped to it: every pixel exact against the reference painter |
| `comp_sub_frame` | a floating window's frame, title bar and shadow go round its whole tree (a child above and left of its surface); a hole in the tree shows the wallpaper, with no shadow on it |
| `comp_sub_commits` | a synchronized child's commits, its position and the order wait for the parent's commit; place_above and place_below among three children and the parent; a desynchronized child updated with its parent idle, its frame callbacks answered; a synchronized child of a desynchronized one applied by that one's commit; set_desync showing what was cached; a buffer offset (attach's x and y) moving a child until set_position sets its place again (review GR's Low 2); a wl_subsurface destroyed, unmapped at once. A negative ("not shown yet") is checked after a frame callback on another window: a paint after the request has come |
| `comp_sub_input` | the pointer enters the topmost surface of the tree under it, in its coordinates; a child with an empty input region lets it through to its parent; an input region, the order and a position changed by the parent's commit move the focus; a press reaches the client; a child destroyed under the pointer hands it to the one below |
| `comp_sub_hostile` | refused with the protocol's errors: a surface its own parent, a cycle (bad_parent); a surface with a wl_subsurface already, an xdg_surface's, a test window's (bad_surface); place_above naming itself, place_below naming a sibling's child (wl_subsurface's bad_surface); a wl_surface destroyed before its wl_subsurface (defunct_role_object); 9 nested, a subtree that would make 9, 33 children, 49 subsurfaces (no_memory). Every bad client's objects gone after (the handles back); a parent destroyed first is no error (the child inert, its role kept for a new wl_subsurface), and a client connected all along is never disturbed |

## Copy and paste

The clipboard ([ARCHITECTURE](../ARCHITECTURE.md#copy-and-paste)): the
compositor's `wl_data_device_manager`, libjwl's `jwl_clip_*`, the
terminal's selection and keys, the shell's bracketed paste. In utest (the
`init` run; `run utest only comp_data`, `only jwlc_clip`, `only consel`
or `only sh_paste` alone):

| Test | What it checks |
|---|---|
| `comp_data_selection` | the compositor headless with test clients: set_selection refused (its source cancelled) with a serial that isn't one of the client's input events (the enter a new `wl_keyboard` is told the focus with isn't one), or without the keyboard focus; only text types offered on; the focused client told the selection before `wl_keyboard.enter`, an unfocused one never; a receive passed on with a channel end the source can write and not read, the data reaching the reader; receives refused (the reader's end closed, the source told nothing) for a type not offered, a handle that isn't a channel, a reader without the focus, and past 32 since its focus came (a new data device's new offer too); the owner going clears the selection |
| `comp_data_rules` | drag and drop refused (`start_drag` cancels its source; the source used again, a drag's source as the selection, `finish` on a selection's offer are protocol errors); a source that never writes holds up nobody (keys still routed, the reader's end closes when the source closes its own); a client that never destroys its offers is told "none" at the cap |
| `jwlc_clip` | libjwl against the real compositor: a client copies and pastes its own text, then, its connection bound to a port as a program's is, 8 times more, leaving no message bytes on the job once the port is read (each transfer's channel unbound from the port as the paste ends); another, whose window then has the focus, pastes 300 KiB of it (several messages); a raw client's selection replaces it (the first hears `JWL_EV_COPY_CANCELLED`); a fourth pastes from that raw owner, which never writes (timed out after 2 s), writes one byte over 1 MiB, or sends a handle (both refused), or writes empty messages flat out (one dispatch reads at most 64 of them, and the paste still ends at its deadline: M12.1's C2) |
| `consel_drag` | the cell under a pixel (padding and edges clamped); a drag across lines, forwards and backwards, its cells and its text (trailing blanks dropped, lines joined by newlines, UTF-8); a press alone selects nothing until the drag reaches another cell |
| `consel_word_line` | a double click takes a word (paths and options are words; blanks and other punctuation are runs of their own) and a drag after it goes on by words; a triple click takes a line, a drag after it by lines |
| `consel_scrollback` | a range far back in the scrollback with a line no longer kept (skipped, its newline kept); a copy cut at its buffer; clicks counted within 500 ms on one cell, a fourth starting again |
| `consel_keys` | Super+C and Ctrl+Shift+C copy, Super+V and Ctrl+Shift+V paste, Ctrl+C and other chords don't; which keys clear the selection; pasted text as keys (CR LF one newline, tabs kept, ESC, other control characters, C1 controls and bad UTF-8 dropped) |
| `consel_tint` | a selected cell's background is #7f77dd at 45% over its own, its text unchanged; the cursor wins |
| `sh_paste` | the shell's markers (`ESC [ 200 ~`, `ESC [ 201 ~`, from a keyboard's Escape key or a terminal's byte), a marker's start, near misses; a pasted key's byte on the line (line breaks and tabs a space, nothing that isn't ASCII) |

End to end, `tools/clip-test.sh <outdir>` ([below](#area-scripts)):
`clip.txt` on a plain boot with a USB keyboard and mouse.

## The clients against a hostile compositor or peer

M12.1's review of the desktop's clients
([M12.1-REVIEW-CLIENTS.md](M12.1-REVIEW-CLIENTS.md)): libjwl's client
side, the console's window mode and libfun's window, against a compositor
that sends anything and a peer that floods. In utest (the `init` run;
`run utest only jwlc_fuzz`, `only con_hostile` alone):

| Test | What it checks |
|---|---|
| `jwl_bad_empty_run` | a run of batches with no message: 64 in a row and then a message are fine, a 65th in a row is refused at once (the compositor says `invalid_method`) with the batch after it unread, on both sides (C1: one `jwl_conn_next` read them all, so a client writing them flat out froze the compositor) |
| `jwlc_keyboard_hostile` | a repeat rate of INT32_MAX is 1000 a second, a held key's next repeat 1 ms on (C4); a negative delay is none; a key code past every table types nothing |
| `jwlc_held_retry` | a client whose channel is full holds its next request, and its deadline is then within 10 ms; the queue drained, the dispatch at that deadline writes it and it is answered (C5) |
| `jwlc_connect_pending` | a connect function's "not yet" six times: the client is ready in about 300 ms, not after a pause that doubles each time (C13) |
| `jwlc_slow_reader` | the compositor sends keys to a client that reads none until the transport drops it (no_memory "reads too slowly"): the client reads none of the keys (the compositor took its window back, `channel_withdraw`), hears `JWL_EV_DISCONNECTED` (`ERR_NO_MEMORY`), is not dead, connects again and gets its window back (title, pixels, `configure.rebuilt`); dropped four times in a row it waits 50, 100, 200, 400 ms; any other no_memory ("too many regions") still ends it (C12, DQ1) |
| `jwlc_fuzz_events` | a ready client with a drawn, focused window gets thousands of well-formed events with hostile values on every object it has but wl_display (words at their ends, random strings and arrays, objects of the right type or null, handles of every kind), the program taking its events and drawing between bursts, and the compositor dying twice: no crash, still ready, every handle and message byte back at the end |
| `jwlc_fuzz_batches` | random and mutated raw batches onto a client's connection: each taken, or the connection dropped and made again; nothing crashes or leaks |
| `fun_window_present_rects` | `gfx_present_rects`: only the rectangles are compared and sent, damaged by their pieces, with what the other buffer was owed; a change outside them is not looked at (no commit) until a `gfx_present` finds it; two rectangles, the bottom row and right edge, rectangles off the picture, empty, negative or partly outside are clipped (never a crash, never a commit for nothing); n = 0 does nothing |
| `fun_window_input` | (with the rest) the wheel at INT32_MIN notches twice is 128 notches the right way, a pointer past every edge is the window's corner (C6) |
| `con_hostile_compositor` | bin/console as init starts a terminal, its /svc/wayland the test's: eight connections that get garbage at once (no batch, a wrong magic, a message to nothing, 65 empty batches, over 16 KiB, a header naming more handles than it brings, 29 handles, a delete_id of nothing), each dropped and made again, then the fake compositor with hostile values (sizes of 4000x3000, 1x1, 0x0 and negative, the pointer past every edge with a drag, the wheel at INT32_MIN, key codes past every table, every modifier bit, copy and paste with no clipboard, a repeat rate of INT32_MAX with a key held, a junk keymap, an output of negative size, 500 pings, frame callbacks held): it answers `console.size` throughout with a grid inside 480x180, and the close box ends it with 0 |
| `con_conn_shell` | bin/console as a connection console (`conn`, `cols=100 rows=30`; its master and control channels the test's, `<termconn.h>`) with bin/shell on a SHELL client: the size from the arguments, from `conn_ctl.resize` (a new size, 0 refused, 60000 clamped to 480x180); the prompt as bytes; typed `echo hello-conn` answered after a CR LF (the LF of the output sent as CR LF), `ls /boot` listing `init.cfg`; a bracketed paste with a line break held until Enter (the shell asked for the markers); Ctrl+C on `sleep 100` ends it inside 5 s (`^C`, a new prompt); the master closed: the console ends with 0 and the shell with 2 |
| `con_conn_log` | a connection console given the root with `RIGHT_ROOT_KLOG` alone: a client's `console.show_log` gets a line a process logs as text (CR LF), an escape sequence in it shown as `?`, no line from before the console started and no ESC byte at all on the master (skipped without that right: the `init` run has it, `run utest only` doesn't) |
| `con_conn_hostile` | 6 messages of junk (777 bytes and 64 KiB, mostly ESC, `[` and `~`), sequences without an end, and a message with a handle on the master; short, unknown-ordinal and half-bodied requests on the control channel; the console still answers `console.size` and `resize`, keeps the size when the control channel is closed, and ends with 0 when the master is |
| `con_conn_backpressure` | 2600 writes of 2048 bytes (5.3 MB, more than the master's 1024 messages hold) with nobody reading the master: the writer's call times out (the console stopped serving) and so does another client's `console.size`; the master then read to the end: every byte there, all `x`, in messages of at most 4096, and the console serves again |
| `con_conn_stop` | a connection console started and stopped (`conrig.c`'s `rig_stop`, which every test here ends with): once the stop returns, the console has ended and the test's job holds the same handles, threads and message bytes as before the start: a test after it that compares its job's counts, as `jwlc_real_compositor` does, sees no console still ending |
| `con_tty_canonical` | the terminal's POSIX side (`console.open_tty`, the `tty` protocol) on a connection console: a line typed with an erase in it is echoed (`\b \b`) and arrives on the data channel as one message; output's LF shown as CR LF; Ctrl+D at the start of a line is an empty message; the settings' defaults; `get_size`, and `conn_ctl.resize` as the `size` event; Ctrl+C with no shell below (an ADMIN client's tty) as the `signal` event, the line typed before it flushed |
| `con_tty_raw` | `set_attr` now: `ICANON`, `ECHO` and `ISIG` off, Ctrl+C is the byte 3 and no event, no echo; `set_attr` after a drain answered only once the output written before it is on the master, `drain` the same; `flush` of the input and `set_attr` after a flush leave nothing to read; a bad `when` or `queue` refused; the next client finds raw mode (the settings are the terminal's) |
| `con_tty_focus` | a SHELL client's keys below a PROGRAM client's tty: the keys go to the tty, `ISIG`'s Ctrl+C to the shell's keys (not the tty); a line typed and not read and a line half typed reach the shell's keys as keys when the tty closes; an output message over `TTY_OUT_MAX` hangs the tty up; 8 ttys at once, the 9th refused |
| `con_tty_order` | a tty's output filled until the console stops reading it (nobody reads the master), then a client's `console.write`: once the master is read, the client's text comes after the tty's last message, not among its messages (a program's last lines before the shell's prompt) |
| `con_tty_unread` | 40 `tty.get_attr` requests on a tty, then 40 `console.size` on a PROGRAM client, none of the answers read: the console answers 8 (the protocol's unread bound) and holds the rest until they are read, then answers them all, and still serves |
| `ldisc_table` | `<ldisc.h>` alone: 32 byte sequences under termios settings, each with its echo and its records (erase, word erase, kill with `ECHOKE` and `ECHOK`, reprint, end of file, `ECHO` off and `ECHONL`, `ECHOCTL`, a tab erased, `ICRNL`/`IGNCR`/`INLCR`, `ISIG` and `NOFLSH`, Ctrl+Z doing nothing, `VLNEXT`, `IEXTEN` off, raw bytes, a UTF-8 character erased), and `MAX_CANON` |
| `ldisc_queue` | output processing (`ONLCR`, `OCRNL`, `OPOST` off) and the column a tab is erased from; canonical mode turned off with a line half typed; a peeked record closed; records put back at the front; the queue's record and byte limits |
| `ldisc_read_rules` | `ld_read_deadline`: canonical mode, and the four cases of `VMIN` and `VTIME` |
| `keybytes` | `<keybytes.h>` against xterm's bytes (text as UTF-8, Enter, Backspace, Tab and Shift+Tab, Ctrl and Alt, the cursor keys in both cursor modes and with modifiers, Home, End, Insert, Delete, the page keys, F1 to F12, a release and a modifier alone giving none); every key `<termkeys.h>` knows through both and back, and its sequences back through both |

posixtest's tty section (the `init` run; `posixtest tty`) drives a
connection console of its own: a child started with a PROGRAM client as
`SR_CONSOLE` and no hand-over finds descriptors 0 to 2 a terminal
(`isatty`, `ttyname` `/dev/tty`, `tcgetattr`'s defaults, `TIOCGWINSZ`),
reads a typed line canonically (the echo on the master, a short read and
its rest), goes raw with `tcsetattr` (Ctrl+C the byte 3; `poll` for
writing, and for the typing), times out a read with `VMIN` 0 and `VTIME`
2, gets `EAGAIN` with `O_NONBLOCK`, and goes back with `TCSADRAIN`; a
second child gets a terminal as descriptor 0 in a hand-over
(`POSIXFDS_TTY`) and writes through it. Then the terminal's signals: a
third child's blocked read ends with `EINTR` when Ctrl+C is typed (`ISIG`,
no shell below: the `signal` event is SIGINT), and a resize of the
connection (`conn_ctl.resize`) is SIGWINCH, with `TIOCGWINSZ` giving the
new size; a fourth child, a busy loop making no calls, is ended by Ctrl+C
(SIGINT's default action, from the signal thread).

posixtest's signal section (the `init` run; `posixtest signal`):
in one process, a handler runs on the thread a signal was routed to (the
one not blocking it) and `pthread_kill`'s on the thread named; a blocked
signal stays pending and runs once unblocked; an ignored pending signal is
dropped; `SA_RESETHAND`; a `read` and a `poll` blocked on a pipe end with
`EINTR`, the `read` restarted under `SA_RESTART` and the `poll` not; two
signals sent back to back at a thread blocked in one wait both run, 1,000
rounds (none lost); `sigwaitinfo`, `sigtimedwait`'s timeout, `sigsuspend`;
SIGPIPE ignored (`EPIPE`) and handled (on the writing thread); `setitimer`
every 10 ms, `alarm`, POSIX timers with `SIGEV_SIGNAL` and `SIGEV_THREAD`;
`pthread_cancel` of a thread blocked in `read`. Between processes (each
child its own signal channel): 10,000 SIGUSR1 to a child, each answered
before the next, then 500 `sigqueue` values counted and summed; SIGCHLD
reaping 200 children; default actions (SIGINT on a busy loop, SIGTERM,
SIGKILL, `raise`, `abort`, SIGPIPE) seen by `waitpid` as `WIFSIGNALED`;
`kill` of a process group; the ignored signals and mask a child keeps.

posixtest's mmap section (the `init` run; `posixtest mmap`): a file on
`/data` mapped on its own pages where the mount's service is a pager
(fat): `MAP_SHARED` read-only (the file's bytes, zero past its end, kept
after the descriptor's close; `PROT_WRITE` refused: `EACCES` without
write access, `ENODEV` with it, since the pager never writes pages back),
`MAP_PRIVATE` read-only then `mprotect` adding write (a copy-on-write child
put in its place: the file and the shared mapping never see the writes),
`MAP_PRIVATE` writable (charged in full at `mmap`; `MADV_DONTNEED` gives
the file's bytes back), `msync`'s checks, a mapping at an offset, 4 MiB
read without a copy (the job grows by its tables, not 1,024 pages); a
file on `/boot` (no pager): `MAP_SHARED` `ENODEV`, `MAP_PRIVATE` a copy.
Without a pager on `/data` it says so on a line and checks what a copy
gives. The cases that need a second stick, a pulled stick or fat's
coherence are `tools/posix-mmap-test.sh`'s.

posixtest's jit section (the `init` run; `posixtest jit`): `MAP_JIT`
([CR-PLAN](CR-PLAN.md#12-the-jit-by-capability)) from a program without
the power (posixtest's list has no `right jit`, and init's run gives
RIGHT_ROOT_JIT only to a list that asks): `EPERM` writable and executable,
writable, and for a `PROT_NONE` reservation; `EINVAL` with `MAP_SHARED` or
a descriptor; execute on plain memory `EACCES`; and bin/jittest started by
`posix_spawn` (`jittest without`) finds the same refusals, since a child
gets none of its parent's powers. Then every other way to writable and
executable memory, by the raw calls ([CR-REVIEW-JIT](CR-REVIEW-JIT.md)):
its own code and read-only data (W+X `ERR_INVALID_ARGS`, write
`ERR_ACCESS_DENIED`, `mprotect` `EACCES`), its stack and heap (W+X
`ERR_INVALID_ARGS`, execute `ERR_ACCESS_DENIED`; mapped over in place by
`VMAR_REPLACE` or `MAP_FIXED`: refused, its page unchanged), a VMO handle
widened to execute by a duplicate, a replace or a send (`ERR_INVALID_ARGS`),
a copy-on-write child (no execute right), the root
widened to the power, `vmo_make_jit` with its root or a VMO as the root,
and, where init's run gives it `RIGHT_ROOT_VMEX`, sealed code it wrote
(never writable, never JIT memory); it is never marked. With the power (`jittest`, from the
shell: `jit.txt`) a `MAP_JIT` page is written and run, patched, switched
between writable and executable with `mprotect`, and a reservation's pages
made executable, decommitted in place by `MAP_FIXED | MAP_JIT`, and made
plain memory again by a `MAP_FIXED` without it.

posixtest's execinfo section (the `init` run; `posixtest execinfo`):
`backtrace()` and its lines ([CR-PLAN](CR-PLAN.md#bt-notes), "BT notes").
Four nested functions (each noting `__builtin_return_address(0)`) give a
trace whose first frame is in the innermost and whose next three are the
return addresses they were called with, up to the test function, `main`
and the program's start (at least 7 frames); a one-slot array stores one
frame and leaves the next untouched, size 0 stores nothing. The same
chain in a second thread ends in libc's thread start. From a signal
handler (`raise(SIGUSR1)` in a function of the test): the handler's frame,
then libc's delivery and musl's `raise` (the layer's and musl's unwind
tables), then the raising function and its caller, within 12 frames. A
`qsort` comparator's trace reaches the function that called `qsort`
(musl's own frames between). `backtrace_symbols()` gives one block (the
pointers, then the strings) and `backtrace_symbols_fd()` the same lines on
a pipe: `name() [0xaddr]` for an address in the program, `[0xaddr]` for a
stack or heap one.

posixtest's launch section (the `init` run; `posixtest launch`): what
Chromium's launcher needs of `posix_spawn` ([CR-PLAN](CR-PLAN.md#cs1-notes),
"CS1 notes"). A child given 503 arguments (500 of 128 bytes) and 64 KiB of
environment, started as `/bin/posixtest` and as `/proc/self/exe`, checks
every byte of them (they travel in libjam's `SR_ARGS` VMO, too big for the
startup message); 3 MiB of arguments is `E2BIG`;
`posix_spawn_file_actions_addclosefrom_np` (a negative start `EBADF`)
leaves a child exactly 3, 4 and 9 from 3 up, as its `/proc/self/fd` lists
them: the `dup2`s before it at 3 and 4, not the one at 7, the one after it
at 9, and none of our other open descriptors (5, 6, 30, 100). Malformed
`SR_ARGS` VMOs from a starter ([CR-REVIEW-POSIX](CR-REVIEW-POSIX.md),
"Tests"): a bad magic or version, strings past the VMO, the pointers' room
past 2 MiB (finding 4's wrap, at 262,143 and 262,144 strings), argc and
envc of 2^31, a path without its NUL, a relative one, one of one byte, a
string without its NUL, more strings than bytes: each child starts with
argc 0 (posixtest's main then exits 9) and a "no usable argument VMO" line
in the log, never killed; a good VMO first, which exits 0.

posixtest's peer section (the `init` run; `posixtest peer`): our IPC
against a hostile peer in another process ([CR-REVIEW-POSIX](CR-REVIEW-POSIX.md),
"Tests"). A receiver child in a job of its own is given 400 socketpair
ends (one in four `SOCK_SEQPACKET`) over a control socketpair, and then
accepts 150 connections to an abstract name of its own through
`/svc/unix`; into the raw channel of each we write what the scm section's
fuzz makes (our sends of random bytes and descriptors of every kind, taken
off a raw channel and mutated: bytes changed, handles dropped, added or
swapped for any kind of object, cut short, junk between) and close it.
The receiver reads every one (as many as it is told) to its end with
random room and flags: no
`recvmsg` may take over a second, no `poll` wait 10 s for an end already
closed, and after the last round its job holds the handles it held after
the first and its lowest free descriptor is the same; it must not be
killed (60 s at most). The seed is fixed and printed (`posixtest: peer:
seed 0x7065657232, ...`); a receiver's failure says why in the kernel log
(`posixtest: peer child: ...`). Then finding 2's case: a file whose
channel never answers, received with `MSG_DONTWAIT`, comes at once, and
closes at once.

posixtest's socket section (the `init` run; `posixtest socket`) needs no
network: `socket`'s refusals (`AF_INET6`, `AF_UNIX`, a raw socket, a
protocol that isn't the type's); a new stream socket's read-only options,
POSIX's defaults (`TCP_NODELAY` off) and options set and read back
(`SO_SNDBUF` as a ring's power-of-two size, `SO_RCVTIMEO`, `SO_LINGER`) or
refused (`ENOPROTOOPT`, `EINVAL`, `EDOM`); a socket never connected
(`ENOTCONN`, `EPIPE` with `MSG_NOSIGNAL`, `poll` writable at once, a
connect to 127.0.0.1 `ECONNREFUSED` where netstack runs (a reset from its loopback), `EACCES` where this program has no network); socketpair ends through
`send`, `recv`, `sendmsg`, `recvmsg` (`MSG_DONTWAIT`, `MSG_WAITALL` across
a writer's pieces) and their names; `getaddrinfo` of a numeric address and
of `localhost` (libc's `/etc/hosts`). The sockets section talks to the
network peer and runs only when named (`tools/posix-net-test.sh`). The
loopback section needs netstack but no peer and no card (`posixtest
loopback`, `posix-loop.txt`): a TCP echo over 127.0.0.1 (a listener bound
to it, by address, by `localhost` and by 127.0.0.7), 100 KiB each way at
once with a thread per direction checked byte by byte, the end of file
after `shutdown`, `getsockname` and `getpeername` on both ends, a connect to
a closed port `ECONNREFUSED`; UDP between two sockets bound to 127.0.0.1
(the sender's address and port, `poll`, `MSG_PEEK`, a connected socket).

## The protocols' servers against hostile clients

M12.1's review of M12's protocol and storage lines
([M12.1-REVIEW-PROTOCOLS.md](M12.1-REVIEW-PROTOCOLS.md)). In utest (the
`init` run; `run utest only idl_fuzz`, `only fat_`, `only volumes_hostile`
alone). genidl writes `drivers/include/idl/fuzz.h` for the first: every
protocol's dispatch over stub handlers, and the shape of each message.

| Test | What it checks |
|---|---|
| `idl_fuzz` | every protocol's generated server dispatch (and its client's event dispatch) gets 1,000 messages made from its messages' shapes: valid, cut, lengthened, counts past their maxima, bad UTF-8, bad padding, flipped bits, other ordinals, no txid, with handles of every kind and wrong counts. The request ends at a page with no access, the reply buffer at another (a canary up to it); every handle given is gone after, every request with a txid answered with it, no answer past `REP_MAX` or `REP_HANDLES`, no failure with handles |
| `fat_hostile` | bin/fat over a RAM disk, 1,600 such messages on a view (`fs`) and an open file (`file`), paths among the strings: each answered, neither channel closed on the client, fat still answering, its job empty when it ends |
| `fat_unread_flood` | a view writes 300 readdirs of a directory of long names and reads no answer, fat's job limited as volumes limits it: at most a few answers go out before it reads, everyone else is answered meanwhile, the 300 are answered in order once read (finding 1: before, the job filled and every other answer was dropped) |
| `idl_unread_bound` | a child serving `null` in a job limited to 8 KiB of queued messages, once with the generated loop (`null_serve`, a thread a channel) and once with a port loop of its own (`null_serve_one` after `null_unread_full`, a `struct idl_held`): one client writes 300 pings and reads none, and is left at most 8 answers; the other client's 50 calls are answered meanwhile; nothing is dropped, and the 300 come in order once read (without the bound the job fills after about a hundred answers and the rest are lost) |
| `fat_rename_into_alias` | a directory moved into itself by its 8.3 alias, another case or by the alias as `from`: ERR_INVALID_ARGS, nothing lost (finding 2: FatFs lost the directory) |
| `volumes_hostile` | volumes' control channel gets 800 such messages (none that stops it or takes a mount away), then a test disk whose `storage` driver answers garbage for 2 s (junk, the right txid with a wrong answer, handles, partitions past counting, a `block` that is an event) and goes: volumes answers throughout, and no test mount is left |

## The compositor's look

The floating windows' title bars, circles, rounded corners and shadows,
tiling's borders and the wallpaper (`user/services/compositor/look.h`).
In utest (the `init` run), the compositor's test scene runs headless
and a reference painter of the test's own (`user/tests/utest/comp_ref.c`:
the wallpaper's formula, the corners' supersampled circles, the shadow
as a count of the ways three numbers add up) paints the same scene;
every pixel it knows is compared exactly
(`user/tests/utest/comp_look.c`):

| Test | What it checks |
|---|---|
| `comp_look_title` | two floating windows, focused and not: bars, outlines, corners and shadows exact; the circles in their colours or grey, no symbols; each title's text centred in its bar, clear of the circles |
| `comp_look_corners` | a floating window over a picture: the client's own corner pixels cut, the picture (and the shadow on it) showing through, the outline along the curve; a translucent floating window over it |
| `comp_look_shadow` | a focused and an unfocused window's shadows exact; a move paints exactly the old and new extents (frame and shadow) and leaves nothing behind |
| `comp_look_buttons` | the pointer on an unfocused window's circles: its colours and each symbol; a focused window's without symbols; the pointer gone: grey again |
| `comp_look_tiled` | tiled windows' borders, focused and not, rounded with the border along the curve; the corners and gaps show the wallpaper, no shadow |
| `comp_look_wallpaper` | the wallpaper is look.h's formula pixel for pixel, the same on a second run, and no neighbouring pixels differ by more than 2 in a channel |

`compwm`'s `wm_window_at` and `wm_floating_move` press the circles'
hit boxes (to the pixel) and click each circle; `wm_states` makes a
window full screen by a double-click on its title bar.
`tools/comp-test.sh` checks the same look on QEMU's screen.

## The desktop

The top bar, virtual screens, minimising, Alt+Tab, the search box,
popovers, notifications, animations and cursors
(`user/services/compositor/desk.h`; the owner's picks in
[HISTORY: G1](HISTORY.md#g1-the-desktop)). In utest
(the `init` run), its logic is linked in on the window manager's harness
(`user/tests/utest/compwm.h`: the seat played by the test, fake
toplevels), the desktop on, no animations unless a test turns them on, the
clock fixed (`user/tests/utest/compdesk.c`, `compdesk2.c`):

| Test | What it checks |
|---|---|
| `desk_screens` | one screen at the start; Super+Ctrl+Right (or L) past the last makes one and an empty one left behind goes; Super+Right and Super+Left move the focus, not the screen; Super+1..9; each screen's own arrangement (Super+T on the current one, new screens take it); Super+Ctrl+Shift+Right/Left (or L/H) move the focused window, making a screen and dropping the one left empty; Super+Shift+N moves it to screen N |
| `desk_fullscreen` | full screen on a screen of its own right of its home, no strip, the whole output; back home, its screen gone; closed while full screen; a new window while a full-screen screen is current opens on the normal one left of it |
| `desk_minimise` | hidden, its chip dimmed, the focus moved on; its screen stays; Alt+Tab's way and a chip bring it back focused (from another screen too); a chip click minimises the focused window; in tiling the others take its tile |
| `desk_room` | no window under the strip and its 6-pixel gap: first places, the cascade, a drag up, a maximise, every tile, tiling's maximise; full screen has all of the output; with the desktop off all of it is the windows' |
| `desk_strip` | the islands' places and items ("Jam OS", dots, "+", chips, icons, the clock "Mon 5 Oct  14:32"), the focused chip lit; each item's click |
| `desk_cursors` | the cursor set's pictures at known pixels and hot spots, busy's arc turning; the resize arrows on each edge and corner, the hand on circles, islands and rows; busy from a launch until the app's first window or 10 s |
| `desk_anim` | open, minimise, restore and close end exactly where the change ends (mapped or not, drawn as itself, the snapshot freed); a key mid-way jumps to the end; a frame damages only its boxes; a slide offsets both screens' windows and ends at 0 with the old screen's unmapped |
| `desk_anim_big` | a window whose frame is bigger than the output opens and closes with no snapshot (M12.1 review C2: a client's 8192x8192 buffer made a 256 MiB picture at each map and unmap); one that fits still animates |
| `desk_alttab` | the order (this screen's windows most recently focused first, other screens grouped, minimised last); Tab, Shift+Tab, wrapping; letting go of Alt goes there (its screen, restored); Esc cancels; the list shows after 120 ms |
| `desk_alttab_gone` | windows destroyed by their clients while the list is open (M12.1 review C1): their rows go, the selection stays on a live window, letting go of Alt goes there; the last one gone ends the list |
| `desk_search` | Super tapped alone (not with another key or a click) toggles it; typing filters, the run row; arrows, Backspace; Enter runs the app (`ctl_launch`) or the command (`ctl_run_in_terminal`); a click on a row; its place |
| `desk_popover` | each popover 2 pixels under the strip, its right edge on its icon's, one at a time, closed by its icon, Esc or a click elsewhere; the volume slider by a click and a drag, ending 10 pixels plus its knob before the percentage's slot, as wide as "100%"; the calendar Monday first (leap years) |
| `desk_notify` | cards stacked from under the strip, the newest on top; the plain one goes after 5 s, the one with buttons stays until a button (`ctl_notify_answered`); a click sends a plain card; at most 5; a terminal refused (`desk_terminals_full`): "No more terminals / 16 is the most. Close one first.", its text within the card, no second card while it is up, again once it has gone (its 5 s, or a click); none without the desktop |
| `desk_notify_held` | no card while the boot splash is up (the wait for it, its overlay, its fade-out): each notice gets its id and waits; one withdrawn meanwhile never shows; when the splash is over they show in the order they came, their 5 s counted from then (one with buttons still waits for a press); at most 5 wait, the oldest without buttons dropped; with no splash a notice shows at once; nothing wakes the loop while they wait |
| `desk_notify_dismissed` | a card with buttons pushed off the screen by newer ones (the five all have buttons: the oldest goes), shown or held through the splash, is answered to its poster as `NOTIFY_DISMISSED`, once (M12.1 review C11/DQ3); a plain card pushed out first, or a card withdrawn, is not |
| `desk_overlay` | a full-screen window whose client takes no keys (the splash's) is a boot overlay: no screen or dot of its own, no chip, no Alt+Tab row, never cycled to, not minimised; the whole output over the strip (hidden under it) and every window focused or opened after it; screens change under it without moving it; its fade (200 ms, over the strip, its box only) ends with the strip back and its snapshot freed; out of full screen it is a window on the current screen; a client with keys still gets a screen of its own |
| `desk_overlay_first` | a keyless client's full-screen window is a boot overlay only while the boot's wait for the splash is on (`comp.splash_until`) and only the first (M12.1 review DQ1): with no wait, or a second one in the wait, it is an ordinary full-screen window on a screen of its own; the first stays an overlay after the wait ends and when it asks for full screen again; once it has gone, the next is still none |

The plumbing behind the hooks (track D2b,
[ARCHITECTURE](../ARCHITECTURE.md#the-desktops-plumbing)), with bin/compositor
headless on the seat's harness, which gives it `/svc/notify`'s shared
channel and fake ends of the mixer's desktop channel and netstack's
read-only one (`user/tests/utest/compplumb.c`), and the services' sides:

| Test | What it checks |
|---|---|
| `comp_notify` | a `notify` channel from `/svc/notify` may notify, post, notify_wait and withdraw, compctl's methods there are another protocol's (ERR_NOT_SUPPORTED, an add_source's end closed); compctl's channels, ADMIN's too, can't post and there is no NOTIFY level; bad text, tint, icon refused, and the strings' limits and UTF-8 by the stubs; a post makes a card (the ids count it) and a bad one none; three cards with buttons a channel; withdraw only one's own; a click on a button answers the poster's notify_wait, or is kept for its next one; `nodesk`: ERR_NOT_SUPPORTED |
| `comp_notify_splash` | started with `splash` (no splash ever comes): a notice gets its id but no card; its poster can withdraw it; a click where its button would be answers nothing; after the 5 s wait the card shows and its press answers the notify_wait sent during the splash |
| `comp_notify_dismissed` | two channels post three cards with buttons each; the sixth pushes out the first channel's oldest, whose `notify_wait` answers its id with `NOTIFY_DISMISSED` (255), once; its other cards are still up |
| `comp_notify_flood` | a program posts 2,500 notices and 2,500 refused ones flat out while the compositor's log (SR_STDOUT) is a channel nobody reads (M12.1 review C5): the lines about them stay within their budget (10 a second each kind), so the compositor never waits on its log and answers the poster's next call and a client's round trip |
| `comp_launch` | the search box: "jamj", Enter sends `initctl.launch("jamjar")`; the cursor busy (the image round the pointer) until init refuses, or until a window titled Jamjar maps; "ls" as `initctl.terminal("ls")`, busy until "Terminal 3" (init's answer) maps, not "Terminal 2" |
| `comp_volume_net` | `audioctl.desk` and `netctl.summary` asked at the start; the volume icon (found from the layout icon, by clicking along the strip) asks again; the slider sends `set_master` on the popover's dB scale (three quarters: -15 dB; the left end: silence); the network popover asks every second while open |
| `comp_seat_ctl` | (with the levels) level 2 (NOTIFY, gone to notify.idl) and 3 are no level; a 17th input source's end is closed and a slot used again |
| `netctl_read_only` | netstack's read-only control channel answers info and summary as the full one does and refuses set_ipv4, set_dns, clear and dhcp_open; summary counts a frame's bytes |
| `conwin_pointer_shape` | the terminal window's pointer: the text bar over the grid's cells, the arrow over the padding |
| `jwlc_real_compositor` | (with the rest) libjwl binds wp_cursor_shape_manager_v1 and `jwl_client_set_cursor` goes |
| `jwlc_restore` | windows back in their places after a restart, end to end: the real compositor (tiling, headless) started with a state VMO the test keeps, as init does; three programs a window each (one title), each splitting the last: a left, b top right, c bottom right; each window's key (jam_window_memory_v1) comes, three different ones; the compositor killed and started again with the same state, the programs reconnecting c, b, a, each configured before the next connects: every window's first configure its old size, its key kept |

The mixer's side is `mixtest`'s `desk` phase (`tools/mixer-test.sh`):
`desk_channel` has the desktop's authority (desk's master, the output's
name, what plays with a stream's title; set_master seen on the full
channel; streams, set_volume, device and another desk_channel refused).
volumes' notices are checked by `tools/sticks-test.sh` (one "USB stick
added" a stick mounted, none for `mount -w`, one "removed" a stick
pulled); init's update notice by `tools/update-test.sh`, which clicks its
Reboot button for the second reboot.

With the real seat (`user/tests/utest/compwmseat.c`, bin/compositor
headless), `wm_seat_cursors` checks the cursor drawn over a window's edges,
corner and circles, and pixel for pixel over its surface, and a client
asking for the text bar and the hand through wp-cursor-shape-v1 (a stale
serial ignored, a shape there isn't an error); `wm_seat_overlay_keys` a
boot overlay whose client binds a keyboard after it mapped: a click on it
reaches it, the keys stay where they were (M12.1 review C3). The other compositor tests
that start it headless pass `nodesk` (`comptest.h`): the window manager
alone, the whole output the windows'; `comp_screen_on_top` runs both ways
(a boot overlay over a window that takes the keys: a click where the strip
is reaches it, the keys stay the window's, before and after it goes; it
starts the compositor waiting for the splash, `cs_start_splash_plain`, as
a boot overlay is one only then: `comp_overlay_late` has none, a window
mapped after the keyless full-screen one is above it and takes the click: DQ1).
`comp_paint_overlay` paints it in the test scene (`top=...,F`, `close=`,
`animate`, `tick=`): its own pixels on all of the output, copied straight
from its buffer; half-way through its fade neither picture; at the end
exactly the desktop without it. `comp_paint_splash` starts the boot's
wait for the splash (`splash`, as init's argument; `splashtick=` moves its
clock): with the desktop, a window and the cursor there, every paint is
the splash's background only, nothing drawn from a window; once an
overlay maps, it everywhere, direct, and its fade-out ends on exactly the
desktop; with none after 5 s, exactly the desktop.
`comp_paint_splash_notice`: a notice posted during the wait is drawn
neither then nor over the overlay (every pixel the overlay's, still
direct); after the fade-out it slides in, ending on exactly the picture of
the same notice posted with no splash.
`comp_paint_overlay_open`: with the animations on, a window opening just
before a boot overlay maps, or under it, is not drawn over it 100 ms into
its opening (the overlay everywhere). splash-test.sh's
compositor boot checks the same on QEMU: twelve screenshots 0.1 s apart
from init's start, each all `#1E1A1D` or a frame of the video (with init
not asking for the wait, one of them is the desktop).

## The tiler and the window keys

Tiling is dwindle (`user/services/compositor/wmtile.c`; the owner's pick,
[HISTORY: G1](HISTORY.md#g1-the-desktop)), the
keys are `wmkeys.c`'s, Super+drag and the gaps `wmgrab.c`'s, the glide
`anim.c`'s. In utest (the `init` run), on the window manager's harness
(`compwm.h`), with the desktop off (1280x800: every tile worked out by
hand in the test) unless a test says (`user/tests/utest/compwm.c`,
`compwm2.c`, `compwm3.c`):

| Test | What it checks |
|---|---|
| `wm_tiling` | one window fills the room less the gap and its border; a fixed one joining is centred in the half it gets; the focused tile is the one split, along its longer side; a press on a border only focuses; a client's maximise; Super+Q; a window that goes leaves its sibling their parent's room |
| `wm_switch` | floating to tiling builds the tree in the windows' order, each splitting the last; back to floating, every window in its tile's place (the arrangement kept), not where it floated before, a fixed one keeping its size |
| `wm_tile_tree` | splits by the focus (or the last tile, the focus elsewhere); a split's ratio set by its gap, stopped at the tiler's 200 a tile (two side by side: 406), with a small minimum clamped to 15% and 85%; a window going; a swap moving only the two; another screen's own tree and ratios; floating and back made again in order |
| `wm_tile_small` | a spiral of five windows, then a sixth with the focus on the newest tile (312x192, whose halves would be under 200 pixels wide): it goes to a new tiling screen at the end with all the room (its configure too) and the view follows, the first screen's tiles unmoved and hidden; that window closed: the view back on the first screen, the emptied one gone; opened again, it spills again; Super+T on a floating screen of six: five tiled, the sixth on a new screen, the view staying; that window moved onto the full first screen: to another new screen, followed; the windows' own minimums: a new window's 700 wide not fitting the right half (a new screen), 500x300 fitting (a split), the old window's 500 high not fitting its half, a fixed 600x450 bigger than its half (a new screen), a fixed 300x200 fitting (a split) |
| `wm_tile_gaps` | a gap's 12-pixel hit box (not under a maximised window), the resize arrows (left-right, up-down), its bar lit while hovered (not before a mouse first moves the pointer, which starts on it) or dragged; a drag resizing both sides live with no glide, held at the other tile's 200; a stacked gap |
| `wm_tile_float` | tiling to floating keeps the arrangement: three windows that floated first, tiled, then floating again: each frame inside its old tile (4 pixels in), left/top right/bottom kept, no two overlapping, not at the old floating place; a fourth with a min width bigger than its tile keeps it, on the output; again after tiling and back; with the desktop on, under the strip; a window maximised before its first buffer, then not: 60% of the room, centred |
| `wm_tile_push` | Super+Alt with H/J/K/L and the arrows: the focused tile's edge 48 pixels that way, with no edge there its other edge, nothing with no split that way, held at the other tile's 200, gliding; floating: the right or bottom edge out or in, within its limits; a fixed window unchanged |
| `wm_tile_mins` | the windows' minimums against gaps (the owner's PC: Jamjar beside a terminal, the terminal's tile grew over Jamjar's content): a gap dragged towards a window with a 500x300 minimum stops where its tile keeps 504 (its border), towards a plain one at the tiler's 200, and follows the pointer back at once; Super+Alt+direction and `tiles_push` the same, saying nothing moved at the stop, both ways; a minimum told after the window is tiled moves the gap at once, and a drag towards it then stops there; both minimums together more than the room: the room shared by need, each window wider than its tile clipped to it from its top left (its frame the tile's, no overlap), nothing clipped once the minimum goes; nested splits: the root's gap stops at the right column's need (a tile of 200 beside a 300 minimum, 510), inside it the 200 and the 304 kept, the left window's 400 minimum the other way, the inner gap and the column's stacked gap at their halves' needs (200 wide, 120 high) |
| `wm_save_restore` | the compositor's memory across a restart (wmsave.c): two screens, the first tiling (0 beside 1 over 2, the root's gap moved; 0 and 1 of one title), the second floating (3 moved by its title bar, 4 minimised), the first shown, 1 focused; described, a fresh window manager handed it: both screens and their layouts, the first shown, the places kept empty; back in the order 4, 2, 1, 3, 0: 4 minimised on the second screen, each tiled window's first configure its old tile while the others are missing, 3 at its floating place and size there, hidden, the focus back on 1, the tree's ratio the same, the wait ended by the last; described again: the same tree, the same focus; another window presenting a key already taken gets a new one |
| `wm_save_late` | a window that doesn't come back: its place kept until `WM_SAVE_HOLD_NS` after the start, then gone as a closed window's (its sibling takes their parent's room); presenting its key later: a new key and a new window; a window with no key during the wait is a new window, the remembered ones still go to their places |
| `wm_save_full` | a full-screen window comes back full screen on a full screen of its own, shown and focused if it was the one shown; if the screen shown was the normal one, the view stays there |
| `wm_save_bad` | a description refused whole, nothing of it used: a bad magic, another version, a bad checksum, another size, each count past its bound or 0, no such screen shown or layout, a window on no screen, with no key, a key twice, no such mode, a floating size or place out of range, unknown flags, a minimised window in a tile, a tile of no window, a window in two tiles, a ratio past 85% or of nothing, a tree cut short, never ending, not where it says, on a floating screen, the focus or the shown full screen on no window; refused (ERR_BAD_STATE) once windows are there; 400 random damages with the checksum made to fit: refused or a sane arrangement whose windows come back and whose wait ends, never a crash |
| `wm_save_big_min` | a client's limits past any buffer (10000 by 10000, a max of INT32_MAX) are held to 8192, and the arrangement it is in comes back after a restart (M12.1 review C4: saved as sent, the check refused the whole description) |
| `wm_save_unmapped` | a window that presents its key but isn't mapped when the wait ends is on no screen (M12.1 review C9: it kept its remembered screen, which the wait's end dropped), and maps as a new window on the screen shown |
| `wm_focus_dir` | Super with H/J/K/L and the arrows: the nearest window that way by where the windows are, tiled and floating; none that way, it stays; nothing focused, the screen's last focused window |
| `wm_swap` | Super+Shift+direction swaps with the neighbour there (none: nothing); Super+drag lifts a tile (its picture follows the pointer, the tile under it marked), swaps on release, nothing over the background or itself or once the target went; floating Super+drag moves from inside the surface, Super+right-drag resizes from the nearest corner, stopping below the strip, within the minimum size |
| `wm_reflow` | with animations on, a window opening, a swap, a key's resize and a window going each glide the tiles that move: they start where they were, half way they are between with every damage box on a gliding window's way (or the picture of the one opening or closing), and end exactly in place; another key jumps a glide to its end |
| `wm_keys` | the owner's table: Super+Q, M, F, T, Enter and keypad Enter; Super+1..9 and Shift+1..9; Super+Ctrl with Left/Right/H/L and Shift; Super+Left and Super+Right never switch screens; keys without Super, and Super with keys not in the table, reach the client |

With the real seat, `comp_layout_wait` (`compboot.c`) starts the
compositor without a `layout=` argument and finds it tiling; the other
headless tests pass `layout=floating` (they check floating places), and
`xdg_tiling` checks a client's tiles and the gap between them (not lit:
the pointer starts on it, but no mouse has moved it), and
`xdg_tile_resize` a tile resized three times by a window joining and
going while its client is slow to draw: its last picture stays, at the
tile's top left, clipped to the tile (the gap stays the wallpaper) or
padded in its own colour, never the wallpaper. `tools/resize-test.sh`
drags the gap between two terminals in QEMU with screenshots. `tools/shell-tests/desktop.txt` tiles terminals
with real input.

## The other tools

The rest of `tools/` builds, checks and flashes; the tests above use some
of them. Each file's header says more.

| Tool | What |
|---|---|
| `tools/serial-feed.py` | types a shell script into QEMU's serial port (`QEMU_INPUT`); fails the script on any failed-test line the guest prints ([verdicts](#how-a-scripts-verdict-is-reached)); `--selftest`, `--scan <log>` |
| `tools/mkstick.py` | disk images standing in for other people's sticks (`tools/sticks-test.sh`, `tools/soak-test.sh`) |
| `tools/disk-regions.py <device>`, `tools/disk-regions.ps1 -Disk N` | the plan's region list of a disk (the GPT areas, small partitions whole, samples of big ones), each region's SHA-256, opened read-only (the .py refuses a path that is not a block device, `--image` for a file; the .ps1 uses `FileAccess.Read`): the owner's proof that Windows' drive was not touched, and the lines Jam OS's `disk digest` must equal. Never run against a disk by an agent; the tests use images ([M12.7 plan](M12.7-PLAN.md#proving-windows-disk-untouched)) |
| `tools/ext-stick.sh <image>`, `tools/ext-stick.sh --volume <file> <tree> <label>` | ext volumes for the pager's tests (M13.6's U10), by Linux's e2fsprogs: a stick image's FAT data partition made again as an ext volume holding its files (extfs then serves the stick's `/data`), or an ext volume in a file from a tree; Jam OS's feature set, every file owned by uid 1000. `tools/pager-test.sh` (`jam`, `jstorm`, `xexec`) and `tools/posix-mmap-test.sh` (`MMAP_DATA=ext`) use it |
| `tools/nvme-img.py`, `tools/nvme-trace-check.py` | the test drives of `tools/nvme-test.sh` (a Windows-like GPT disk, a second namespace, 4096-byte blocks, a damaged table; files only) and the checker of QEMU's NVMe command trace (with `--write-ranges`, every Write inside the blocks given) |
| `tools/nvme-written.py <image> [--pattern none\|some\|all] [--ranges] [--before <image>]` | a Jam OS drive's image after `tools/nvme-test.sh`'s `write`, `prepare` and `erase` cases: Jam OS's table (both headers, CRCs, the two partitions), no block outside the table and the partitions that isn't zeros (with `--before`, the image as it was before an erase: none that differs from it), utest's written blocks each in its place; `--ranges` prints the blocks a write may touch for the trace checker; reads only the file |
| `tools/fatcheck.py <image>[@@offset] [-p N] [--require-clean]` | a read-only FAT32 checker for an image or a stick's raw device (the kill storm's, on the Mac): the FATs agree, every chain is valid (in range, no loop, no cross-link, never into a free, reserved or bad cluster), directory entries are sane (short names, `.` and `..`, long names' order and checksums), each file's size matches its chain, and lost clusters are counted and listed; FSInfo and the dirty bit are notes (`--require-clean`: dirty is damage). Exit 0 clean, 1 damage, 2 lost clusters only, 3 not FAT32. `--selftest` damages small volumes it makes in each of those ways (part of `make check`) |
| `tools/mkgpt.py` | disk images with a GPT (the PC's Windows-like layout, Jam OS's, damaged and hostile ones) or an MBR, sparse; `--cases <dir>` makes every case `build/host/gptcheck` checks, each with the partitions it must list |
| `build/host/gptcheck <dir> [--mutations N] [--fuzz N] [--seed S]` | the partition table library on the Mac (`tools/gptcheck.c`, `tools/gptmutate.c`, `tools/gptmake.c`) against `tools/mkgpt.py --cases`'s images, Jam OS's table as `create_table` writes it (read back after each of its writes), mutated tables and a fuzz pass; part of `make check` (a long pass: `--fuzz 1000000`) |
| `tools/extfix/make.sh <outdir>` | extcheck's fixture volumes, made by Linux's e2fsprogs (mke2fs, debugfs, e2fsck) and listed as debugfs reads them (`tools/extfix/listing.py`); run on the PC through `tools/remote-test.sh` (the Mac has no e2fsprogs), the results gzipped into `tools/extfix/` by hand |
| `build/host/extcheck <fixtures dir> <images dir> [--fuzz N] [--seed S]` | the ext library on the Mac (`tools/extcheck.c`, `tools/extbreak.c`, `tools/extmutate.c`) against the fixtures, broken checksums, the feature table and a fuzz pass, and the checker's detection cases (`tools/extdetect.c`: each way a volume can be spoilt is said, a good one is silent); `extcheck --image <file>...` mounts and checks images another tool made; part of `make check` (a long pass: `--fuzz 300000`) |
| `build/host/jamext` , `tools/jamext-test.sh <jamext> <extcheck> <work dir> [<fixtures dir>]` | the host tool on the same library (`tools/jamext/`, M13.6's U8: `mkfs`, `mkimage`, `put`, `get`, `ls`, `ln`, `check`...): an image from a directory of every kind of name (files over a megabyte, an empty one, a 5 MiB hole, symbolic and hard links, a 255-byte name, 400 names in one directory) comes back byte for byte, shows its links and modes, is the same bytes from the same `--seed` and `--time`, passes `jamext check` and `extcheck --image`; put/get/ln/mkdir/rm/chmod changes checked after each kind; a volume at an offset and `--part 1` of an MBR; what must fail (a missing name, no space leaves no half file, a file that is not a volume); the Linux fixtures `jamext check` clean; where installed, debugfs reads the same files; part of `make check` |
| `tools/fat-label.py` | checks, or with `--fix` repairs, a FAT volume's label in an image the way other systems read it (the Makefile runs it on every image; `tools/data-test.sh` checks with it) |
| `tools/checkdocs.py` | the docs check of `make check` |
| `tools/checkwants.py` | every boot-image program's list ([`<wants.h>`](../user/include/wants.h)) checked by the rules libos applies to a program on `/data`, before the Makefile packs the boot image: the build's approval |
| `tools/checkdriver.py`, `tools/checkdriver-selftest.sh` | the driver build check, and the proof that it still rejects what it must (`tools/checkdriver-tests/`) |
| `tools/sortincludes.py` | the include-order check of `make check`; `make includes` runs it with `--fix` |
| `tools/gensyscalls.py`, `tools/genidl.py`, `tools/gensyms.py` | the syscall glue, the IDL headers and the kernel symbol table |
| `tools/genwl.py` | the Wayland tables and typed stubs from upstream's XML (`gen`, `check`, `selftest`) |
| `tools/mkbootfs.py`, `tools/mkimage.py` | the boot image and the two-partition disk image (ESP to `ESP_END_MIB`, 256 MiB; /data 64 MiB after it). The boot image may be what `update` takes (`UPDATE_FILE_MAX`) or a third of the ESP beside the kernel, whichever is less: `update` keeps three builds on the ESP while it writes one; the image rule checks its real ESP the same way (`mkbootfs.py --esp-check`). Tests that write /data's files take its offset from the image (`mkimage.py --data-at`, the scripts' `DATA_AT`) |
| `tools/bootfs-edit.py <in> <out> name=file...` | a boot image with files added, replaced or (`name=`) left out (the update tests' build B, and their builds with or without a test key) |
| `tools/update-test-key.sh <outdir> <in.img> <out.img> [nokey]` | for the update tests: two throwaway keys in `<outdir>/testkey` (made once by `build/host/jamos-sign keygen`) and a copy of the image whose build has the first one's public half (or none) |
| `build/host/jamos-sign self-test` | the Mac's signing tool (`tools/jamos-sign.c`, built by `make`) against Monocypher's Ed25519 vectors, a round trip, and the ports formats' domains (a signed package manifest, index or VULNS checks as its own format only); part of `make check` |
| `tools/write-usb.sh`, `tools/mbr-grow.py` | `make usb`: the image onto a stick, the data partition grown to its end and left blank |
| `tools/flash-usb.sh` | `make flash`: a new kernel, boot image and boot menu onto a stick's ESP, the stick's kernel and boot image kept as the previous build |
| `tools/flash-test.sh <outdir>` | `tools/flash-usb.sh` on a copy of the image attached with `hdiutil` (never a real disk; macOS): the new files are there, the stick's old kernel and boot image are the previous build (an older one placed first replaced), no `.new` file is left (stale ones placed first included), the data partition is unchanged |
| `tools/readme-shots.sh <outdir> <music folder>`, `tools/readme-music.py <dir>` | the README's pictures, taken again: the music library (public-domain recordings from the Internet Archive, each file's MD5 checked, laid out Artist/Album/Track.mp3 with covers made in the jam colours) into a folder outside the repository, then one QEMU boot at 2560x1440 (`readme.txt`) and the three pictures as PNG (the search box's cut to the part of the screen centred on it); `README_SHOTS_INSTALL=1` copies them into `docs/images/`. No music is ever committed |
| `tools/mksplash.sh` | `boot/splash.mpg` from the owner's animation and its sound (ffmpeg); `make` runs it only when both source files are there (`SPLASH_SRC`) and one is newer |
| `tools/bdf2c.py`, `tools/compdb.py` | `make font` (the console font from Spleen's BDF), `make compdb` |
| `tools/update-server.py` | serves `build/jamos.elf`, `build/bootfs.img` and their manifest to the PC's `update` (UDP 5022, a snapshot per manifest request); `--manifest K B` prints a manifest; `--self-test` runs it against a Python client on 127.0.0.1 (a whole fetch with 10 % lost both ways and the files rebuilt half way: the snapshot's bytes; a new snapshot; GONE, RANGE, a short last piece; malformed requests unanswered; old snapshots dropped; the manifest's rules; the version from `build/jamos.elf`; the commit from a boot image's `build.txt`; each `PlannedServer` plan; signing with a throwaway key: a signed manifest checks, another key's, a changed byte's and the `unsigned` and `badsig` plans' don't) |
| `tools/netlog-recv.py <folder>` | receives the PC's log (UDP 5021): a file per boot, gaps reported; `--self-test` runs it against a Python sender on 127.0.0.1 (started late, paused, 10 % lost both ways, the PC's ring having dropped the start, a crash log, restarted with and without its files, hostile datagrams). utest's `netlog_*` cover the PC's side: golden and hostile datagrams, the whole log over a lossy reordering network, the Mac away ten minutes (one line said, the waits up to 30 s, one datagram per try), the ring dropping bytes before and during, forged acks, a restarted sender whose receiver kept its file (it skips to the receiver's offset), the crash log's stream, the live source over the real kernel log. End to end: `tools/netlog-test.sh` |

## Known noise

- 4-CPU stress throughput under QEMU is bimodal (context switches from
  thousands to millions in 10 s): CPU-hog threads on 4 vCPUs, not a
  regression. Don't chase it.
- On a busy Mac (several QEMUs at once, load above the core count) a run
  can end "FINISHED WITH PROBLEMS" for one reason only: the boot's
  `timer: ... MISMATCH` line, every CPU short of its 100 ticks by more
  than 10% (87-89 seen) because the host didn't run the vCPUs. Check that
  line before looking for a bug, and rerun when the load is lower.
- QEMU can't measure page-allocator scaling or anything that depends on
  which physical pages a run lands on ([BENCH.md](BENCH.md) has the
  details). Performance is measured on the PC.
