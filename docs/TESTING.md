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
| User regression | `tools/qemu-test.sh build/test init init` (utest, then usbtest: ~30 s) | every change to syscalls, libos, services, drivers |
| Shell scripts | [below](#shell-scripts) | shell, console, input |
| Area scripts | [below](#area-scripts) | the area you touched |
| Soak | `tools/soak-test.sh build/test`, again with `QEMU_SMP=8` (about 3 minutes each) | every change to a kernel test, a service or a driver, and anything that keeps state from one run to the next |
| 2-minute soak | `soak 2` in the shell ([below](#soak)) | after each fix round, on the PC |
| Sign-off | the boot menu's All tests, then `soak 10` in the shell with a second stick mounted read-write and pulled and replugged during the run | milestone sign-off only, on the PC |
| Stress | `QEMU_SMP=8 QEMU_TIMEOUT=200 tools/qemu-test.sh build/test st selftest stress=120` in QEMU; `stress <seconds>` in the shell | kernel work (scheduler, memory, locks), and when user space is broken: it needs none of it |
| The PC | flash the stick and run it ([HARDWARE.md](HARDWARE.md#flash-and-boot-the-stick)) | the final judge |

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

## Build checks

- `make` builds with `-Werror`; `make KTESTS=0` builds a kernel without
  the in-kernel tests, benchmark and test hooks (into `build/noktests/`).
- `make check`: the generated syscall and IDL code matches `abi/`
  (`tools/gensyscalls.py check`, `tools/genidl.py check`); the driver
  isolation check still rejects what it must (`tools/checkdriver-selftest.sh`
  over `tools/checkdriver-tests/`); the docs match the tree
  (`tools/checkdocs.py`: links, anchors, and the repo paths, file names,
  headers and make targets named in backticks); no audio file is tracked
  (`tools/checkaudio.sh`); and the RTL8125 driver transmits only through
  `drivers/rtl8125/tx.c`, behind its gate (`tools/checknotx.sh`: registers
  are written only in `regs.c`, each write through the guard in
  `drivers/rtl8125/notx.h` that refuses every transmit register, and in
  `tx.c`, each write behind the gate; no `regs.c` write names a transmit
  register; the transmitter enable bit is named only in `notx.h` and
  `tx.c`; every function `tx.c` gives other files starts with the gate,
  which is "full mode and a valid VLAN"; the probe's files call nothing of
  `tx.c`; the mode and the VLAN are set once, in `main.c`. It checks each
  rule against `tools/checknotx-tests/` first, counting its offences
  there); and the network test peer and the pcap check pass their own
  self-tests on the host (`tools/netpeer.py --selftest`,
  `tools/pcap-vlan-check.py --selftest`).

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
| `QEMU_TIMEOUT` | 150 | seconds before giving up (only a cap: a run ends when the kernel halts or QEMU goes) |
| `QEMU_IMAGE` | build/jamos.img | another image, e.g. build/noktests/jamos.img |
| `QEMU_XHCI` | | qemu-xhci properties, e.g. `msi=on,msix=off` (MSI-only, like the PC) |
| `QEMU_USB` | | more USB devices; give each a `port=` (port 1 is the stick) |
| `QEMU_EXTRA` | | more QEMU arguments, e.g. `-rtc base=2026-01-15T01:02:03` |
| `QEMU_INPUT` | | a script typed into the serial port by `tools/serial-feed.py` (its header has the commands: `wait`, `seen`, `send`, `type`, `sleep`, `shot`, `monitor`, `usbkeys`); the run passes if every `wait` matched and QEMU ended by itself |
| `QEMU_MONITOR` | | a script of `expect` / `send` / `sleep` lines run against the QEMU monitor |
| `QEMU_SAVE` | | a file to keep the run's stick image in, with what the guest wrote: a later run's `QEMU_IMAGE` boots the same stick again |
| `QEMU_SPLASH` | 0 | 1: keep the boot splash (otherwise the boot word `nosplash` is added, so the tests see the text log) |
| `QEMU_NET` | | `1`: a network card and the test peer ([below](#the-network-peer)); `<peer port>:<qemu port>`: the same card and pcap, with a peer you run yourself |
| `QEMU_NET_VLAN` | 21 | the VLAN the peer and the pcap check want every frame tagged with |
| `QEMU_NET_NONE` | 0 | 1: no frame at all may leave the guest (the `vlan=off` run) |
| `QEMU_NET_PEER` | | more flags for `tools/netpeer.py`, e.g. `--noise 2` |

Examples:

```sh
tools/qemu-test.sh build/test kt ktest                    # every kernel test
tools/qemu-test.sh build/test chan ktest=chan             # tests whose name starts "chan"
QEMU_SMP=8 QEMU_TIMEOUT=60 tools/qemu-test.sh build/test st selftest stress=30
QEMU_XHCI=msi=on,msix=off tools/qemu-test.sh build/test msi init
make run                                                  # interactive: the shell, a USB keyboard, serial on stdio
make debug                                                # the same, stopped for gdb on :1234
```

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
frame is logged in hex and fails the run. It strips the tag and answers
as a small network, tagging every reply: ARP for any IPv4 address (not
probes from 0.0.0.0 nor gratuitous ARPs), ICMP echo for any address (so
`ping 1.1.1.1` works in QEMU), and UDP on ports that have a handler
(`Peer.add_udp`: where DHCP, DNS, netlog and the update server hook in).
`--ping <address>` sends an ICMP echo request to the guest every half
second (from 10.2.21.174, once it has seen the guest's MAC) and counts
the replies (`ping_replies`). `--noise <s>` sends, every s seconds, four frames the guest's driver must
drop (untagged, VLAN 10, a priority tag, QinQ) and one it must pass (a
broadcast ARP request on the VLAN). Its log is `<outdir>/<name>.peer.log`,
its counts `<name>.peer.json`.

Independently, `-object filter-dump,queue=rx` writes every frame the
guest's card sends (and none the peer sends) to `<outdir>/<name>.pcap`,
and `tools/pcap-vlan-check.py` checks it for the same rule: two separate
checks, sharing no code. The run fails if either finds a frame that isn't
tagged with the VLAN, or (`QEMU_NET_NONE=1`) any frame at all.

The peer also runs by hand (`--listen P --qemu Q`, its header has the
flags), reads commands on stdin with `--stdin` (`send <hex>` tagged,
`raw <hex>` as it is, `noise`, `stats`, `quit`), and is a module for test
scripts (`sys.path` with `tools/`, `import netpeer`: `Peer`, the frame
builders, `classify`).

## The boot menu

`boot/limine.conf` is the menu; each entry is a kernel command line.
Every entry loads the kernel a second time, as a module: the pristine
image the kernel stores for the next boot (`tools/qemu-test.sh` does the
same). On every entry a panic starts that stored copy: the next boot is a
plain one (the test words are not passed on), its logd saves the panicked
boot's log as `/data/logs/boot-NNNN-crash.txt` and its shell says so in
one line. Only without a stored kernel, or for a panic within 30 s of a
start after a panic (a crash loop), does the panic screen stay up and the
machine halt ([ARCHITECTURE.md](../ARCHITECTURE.md#kexec-reboot-and-panic)).

| Entry | Command line | What it does |
|---|---|---|
| Jam OS | (empty) | the boot splash ([AS-PLAN.md](history/AS-PLAN.md)): the screen dark from the kernel's start, the logo animation with its sound (played to the end; what is typed meanwhile reaches the shell), then the shell; meanwhile init starts the bootfs server (`/boot`), the console, serialin, devmgr (with the USB and PCI drivers; it mounts the stick's `/esp` and `/data`), the mixer, the music player and logd. `reboot` and a panic look like switching the PC on: the splash background at once, then the next boot's splash; after a panic the shell's first line says what it was and where its log went |
| Jam OS (text log, no splash) | `verbose` | the same with the kernel's text log on the screen instead of the splash, and in the shell as it comes (a plain boot keeps it off the shell's screen but for notices and the commands whose output it is: [ARCHITECTURE.md](../ARCHITECTURE.md#debugging)) |
| Jam OS (safe mode: no USB drivers, serial input only) | `nousb` | the same, but devmgr leaves USB alone: input only over serial |
| Jam OS (network: listen only) | `netprobe` | the everyday boot, plus the RTL8125's listen-only probe ([M9-PLAN.md](M9-PLAN.md#the-first-pc-stage-listen-only)): devmgr binds `drv/rtl8125`, which sends nothing, listens for 60 s after the link comes up, logs its `[rtl8125]` lines and one RESULTS line, and exits. No other boot binds the network chip, and a `reboot` doesn't keep the word |
| Jam OS (network: send test) | `netsend` | the everyday boot, plus the RTL8125's ARP send test ([M9-PLAN.md](M9-PLAN.md#r1-progress-the-full-driver)): devmgr binds `drv/rtl8125` in full mode on the kernel's VLAN (none: "no VLAN: the network stays off", nothing touched), which waits for the link, sends three ARP probes for 10.2.21.1 tagged with the VLAN, waits for the replies, compares the chip's count of frames sent with its own, logs its `[rtl8125]` lines and one RESULTS line, and exits. Nothing else is ever sent; a `reboot` doesn't keep the word |
| Jam OS (network) | `net` | the everyday boot, plus the RTL8125's netdev service ([M9-PLAN.md](M9-PLAN.md#r1-progress-the-full-driver)): devmgr binds `drv/rtl8125` in full mode on the kernel's VLAN, which brings the chip up and serves netdev for netstack until devmgr stops it. Nothing starts netstack yet, so it receives (and drops: no session) and sends nothing; a `reboot` doesn't keep the word |
| Tests / All tests | `ktest` | every in-kernel test at boot, strict, on an idle machine |
| Tests / Stress test (2 minutes) | `selftest stress=120` | the stress test alone, no user space: kernel work |
| Tests / Stress test (10 minutes) | `selftest stress=600` | the same for 10 minutes (it signed off the milestones up to M8; from A1 on the soak does) |
| Tests / Soak test (3 minutes) | `soak=3` | a plain boot whose shell runs `soak 3 halt` by itself ([Soak](#soak)): the first failure panics, and the next boot's shell names it (the log is on the stick); a pass ends with the SOAK RESULTS box and a prompt |
| Tests / Benchmark | `bench` | about 10 s; results go to [BENCH.md](BENCH.md) |
| Tests / init + utest + usbtest | `init` | the user-space regression run: init runs `boot/init.cfg` (utest, then usbtest) and the RESULTS box says whether init's root job ended with nothing charged |
| Tests / Timer fallback | `nodeadline selftest` | the periodic LAPIC timer instead of TSC-deadline |

Other boot words (for `tools/qemu-test.sh`, not in the menu):

- `shell`: the plain boot, spelled out (what the shell scripts use).
- `verbose` or `nosplash`: no boot splash, the text log on the screen,
  the shell's included (`tools/qemu-test.sh` adds `nosplash` unless
  `QEMU_SPLASH=1`, so the shell scripts see the log on the screen as
  before; the serial port has the whole log either way). `nousb`
  and `soak` leave the splash out too.
- `ktest=<prefix>`: the tests whose name starts with the prefix. Tests
  named `review_...` run only when the prefix asks for them.
- With `ktest` or `ktest=<prefix>`: `loops=<n>`, `seed=<s>`, `shuffle`,
  `keep`, `load` ([Soak](#soak)), e.g. `ktest loops=5 seed=42`.
- `soak=<minutes>`: what the Soak entry does, for another length.
- `selftest`: the boot-time self-checks; `stress=<seconds>` adds the stress
  test.
- `splashhang`: on a plain boot with the splash, bin/splash borrows the
  screen and never finishes, so init's deadline is tested: 20 s after
  the splash's start init says `the splash didn't finish`, kills it and
  starts the shell anyway (`tools/splash-test.sh`).
- `pcilist` (the PCI device report), `keytest` (keys to the log for 30 s),
  `memmap`, `init_timeout=<s>` (how long the `init` run may take).
- `hidboot`: hid keeps every mouse in the boot protocol (no wheel on
  most real mice) instead of the report protocol it uses for a mouse
  whose report descriptor has a wheel: the way back if a mouse misbehaves
  in report protocol (no movement, or nonsense). A reboot keeps it.
- `test<name>`: a crash test at boot (`testpf`, `testlockorder`, ...; the
  names are in `kernel/debug/selftest.c`). Each must panic with the right
  message; `testbp` must come back. The early ones run before the stored
  kernel is loaded, so they end on the panic screen.
- Switches for the scheduler and friends, each turning one optimisation
  off to compare: `nopcid` (and `forcepcid`: PCIDs on even where the kernel
  leaves them off for the INVLPG erratum), `nospinidle` (or `idlespin=<us>`),
  `noplaceorder`, `noaffinepair`, `nokmcache`, `nooneshot`, `noserialirq`,
  `nofpuopt`.
- `vlan=<id>`: the network's VLAN, 1..4094 (the default with no word:
  21, `BOOT_VLAN_DEFAULT` in `kernel/main.c`). Every frame Jam OS sends is
  tagged with it and only frames tagged with it are received
  ([ARCHITECTURE](../ARCHITECTURE.md#networking)). `vlan=off`, or any
  value that isn't a VLAN id (`vlan=0`, `vlan=4095`, `vlan=21x`, a bare
  `vlan`, two words that disagree), means no VLAN: the network drivers
  turn neither receiver nor transmitter on. The boot log says which
  (`network:     VLAN 21`), and so does devmgr (`network drivers get
  vlan=21`). The kernel passes it to init (also in the `init` run), init
  to devmgr, devmgr to each network card's driver; netstack hears it only
  from the driver. A reboot keeps it, `vlan=off` too.
- `bootdisk=<n>`: the MBR disk id (decimal) of the disk the machine booted
  from, which a kernel started by kexec gets from the one before (Limine
  tells the first one itself); devmgr takes the Jam OS disk with that id
  as the boot disk.
- `crashkernel=<MiB>`: the stored kernel's region (default 32, 32..1024); `crashkernel=0`: no
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
it must be refused and the panic screen stay up; `kexecstall` and
`kexecfault` break the jump after it was decided, and must end in a
firmware reset), plus `devices`, `usb`,
`pci`, `memmap`. `reboot` kexecs into the stored kernel (or the files on
`/esp` if they changed); `reboot -f` resets through the firmware, which
ends a QEMU run (`-no-reboot`): the shell scripts end with it.
`soak` is the soak test ([Soak](#soak)), and `ktest` takes its options.
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

**A failure.** Every panic screen carries a note under its message
(whatever kind of panic: a failed check, an exception, the watchdog):

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

| Script | What it covers | Extra setup |
|---|---|---|
| `basic.txt` | line editing, history, console protocol levels, restarting serialin and the console; `contest flood` (a client that floods the console must not stop Ctrl+C or other clients) | |
| `cmds.txt` | the everyday commands: information, date and time zones (the clock from the RTC as Sydney's local time), files, pipes and text, variables and aliases, programs, Tab and Ctrl+C; `contest junk` and `contest spew` (oversized or handle-carrying pipe output is dropped with its handles closed; a flooding program still stops on Ctrl+C) | `QEMU_EXTRA="-rtc base=2026-01-15T01:02:03"` (the date checks), `QEMU_SMP=4` |
| `system.txt` | the System and Tests commands through the command table: argv, status, pipes, help, aliases | |
| `console-restart.txt` | a console started again after about 550 KiB of log (the klog gap test, 8 loops) draws only the last 256 KiB of it, and the shell comes back | |
| `commands.txt` | utest, usbtest, pci, memmap, the crash list, demo, Ctrl+C past a program, orphans killed with their job, devmgr restarted by init; ends with a real crash, and the next boot's line about it | |
| `extras.txt` | bench and a short stress from the shell, scrollback, clear; ends with a panic, and the next boot's line about it | |
| `files.txt` | the file namespace: `/boot` as a read-only mount, `run` with a path, the file commands (mkdir touch write cp mv rm df sync) on a writable mount (the tests' RAM filesystem, `run ramfs shell`), a mount that reaches a running shell, the bootfs server killed and mounted again | |
| `files-fat.txt` | the file commands on a real FAT volume (`run utest fat-shell`: bin/fat over a RAM disk): names with spaces and lower case, big copies, rm -r | |
| `unplug.txt` | the stick pulled while the system runs and plugged back in (the monitor's `device_del` / `device_add`): `/data` and `/esp` go, nothing hangs, they come back in the running shell, logd carries on | |
| `cad.txt` | Ctrl+Alt+Del on a USB keyboard: the console blanks the screen and asks init, which syncs `/data` and kexecs into the stored kernel, where the file written before is read back | the `tools/usbkeys-test.sh` `QEMU_USB` |
| `data-1.txt`, `data-2.txt`, `data-3.txt` | three boots of one stick: `/esp` and `/data` from the stick itself, a file kept across a reboot, a boot log per boot, the fat service and devmgr killed, the plug pulled | use `tools/data-test.sh` |
| `sticks.txt` | other people's sticks: read-only at `/usb0` and `/usb1`, writes refused, `mount -w` and `mount -r`, what `mount` refuses, a stick pulled while a file on it is read and another in the middle of a copy onto it, sticks with nothing to mount | use `tools/sticks-test.sh` |
| `fun.txt` | the apps (life, tetris, fractal): self-tests, play, screenshots, kill and crash with the screen borrowed | use `tools/fun-test.sh` |
| `apps.txt` | snake, mines and sysmon: self-tests, play, screenshots; one mouse click in mines; the `sysmon` command, and `run sysmon`, given the same by its list | use `tools/apps-test.sh` |
| `jamjar.txt` | jamjar (the music player's window): its cover helper's self-test (`run jamcover --selftest`: stb_image's checks); jamjar's self-test (its helper decodes a PNG, refuses a non-picture, and one that crashes and one that hangs on purpose are killed and replaced; a decode that timed out is tried again once, later); `run jamjar noplayer` without the player; the `jamjar` command: the covers read from the tags (PNG in ID3v2.3 and 2.4, a JPEG, one 611x640, none in a WAV, an oversized and a garbled one refused), an album played by keys, next, back, pause, volume, search, a calibration track (1 kHz on the left, 4 kHz on the right) whose loudest bars must be the left's 34 and the right's 49, and a shot of it (`jamjar-tone`) in which bar 34 stands only up from the line and bar 49 hangs only down, help, roulette, the big view, the sleep timer; mouse clicks on a button, the volume and a track row; the music still playing after q | use `tools/jamjar-test.sh` |
| `jamjar-hd.txt` | jamjar at 2560x1440 by keys, for screenshots at the PC's size (the stereo bars, the big view's sunburst) | use `JAMJAR_HD=1 tools/jamjar-test.sh` |
| `jamjar-covers.txt` | jamjar at 2560x1440 over 32 albums with covers, everything in shuffle, 18 skips (the first while the covers are still read), two shots of now playing after each | use `tools/jamjar-covers-test.sh` |
| `mouse.txt` | the mouse through QEMU's monitor: the shell and tetris undisturbed by it, the wheel's scroll-back, contest's client getting the movement, buttons and wheel, then mines played with clicks at exact cells (reveal, flag, chord, peek, the buttons), and acceleration | use `tools/mouse-test.sh` |
| `ktest-all.txt` | every kernel test from the shell, live, three times in one boot (a test that leaves something behind fails its next run) | |
| `soak.txt` | `soak loops=2` from the shell: two shuffled loops under load with utest between them, and the SOAK RESULTS box | `QEMU_TIMEOUT=600` |
| `soak-plug.txt` | the soak with a second, writable stick and the boot stick pulled and plugged while it runs | use `tools/soak-test.sh` |
| `screen.txt`, `screen-verbose.txt` | the shell's screen on a plain boot (no log, notices, none from processes that only borrow the names init, devmgr and logd: `utest impostor`) and on a `verbose` one | use `tools/screen-test.sh` |
| `clock-1.txt`, `clock-2.txt` | the date, the time zones, the settings across a reboot | use `tools/clock-test.sh` |
| `nousb.txt` | safe mode | command line `nousb` instead of `shell` |
| `netprobe.txt` | the boot word `netprobe` reaches devmgr (its line), and with no RTL8125 (QEMU) nothing is bound for it | use `tools/netprobe-test.sh` |
| `netsend.txt` | the same for the boot word `netsend` | use `tools/netprobe-test.sh` |
| `netserve.txt` | the same for the boot word `net` | use `tools/netprobe-test.sh` |
| `allow.txt` | programs on `/data`: a copy of bin/soakload refused until `allow`ed (n refuses, y allows), `allow -l`, run, a program can't change `/data/etc`, a changed file refused, a list asking for devmgr or init (a copy of bin/utest) or for `right debug` (a copy of bin/wantdebug) refused, approval or not, `allow -r`, a file off `/data` and a second shell refused | |
| `parse-limits.txt` | the shell's 32-segment limit and unclosed quotes | |
| `hda.txt` | the HD Audio driver's dump, `hda`, `kill hda`, `hda jacks`, `hda gain` and `hda bits` set and read back (all through the mixer's query channels) | use `tools/hda-test.sh` |
| `hdastream.txt` | `hdatest`: the HD Audio output stream (open, a pattern played, a running stream closed, the driver killed mid-stream) | use `tools/hda-stream-test.sh` |
| `play.txt` | `play` of six WAV files on `/data` (four played, garbage/cut/float refused, Ctrl+C) | use `tools/play-test.sh` |
| `quality.txt` | `play -s` of known signals on `/data` | use `tools/audio-quality-test.sh` |
| `mp3.txt` | `play` of MP3 files on `/data` (seven played, noise and an ID3 tag with noise refused, Ctrl+C, `play -n`) | use `tools/mp3-test.sh` |
| `beep.txt` | `beep 440 500` and `hda gain` (shown, set, clamped at both ends) | use `tools/beep-test.sh` |
| `mixer.txt` | `vol` (nothing playing, the master set and clamped, a missing stream, usage), then `mixtest` | use `tools/mixer-test.sh` |
| `music.txt` | the music player: usage, errors, `music start` with the shell in use meanwhile (`ls`, `beep`, `vol`, `music status`, `music vol`, `music next`), past one whole shuffle, `music stop`; it plays on through Ctrl+C, `kill shell` and `kill mixer`; `music prev`, `pause` (and `status` paused), the sleep timer set, shown, off, and 33 s that runs out; a second stick pulled mid-song | use `tools/music-test.sh` |
| `usb.txt` | the `usb` command | `QEMU_USB="-device usb-hub,bus=xhci.0,port=2 -device usb-kbd,bus=xhci.0,port=2.1"` |
| `usbkeys.txt` | typing on a USB keyboard behind a hub; kill hid, the console, devmgr | use `tools/usbkeys-test.sh` |
| `shell-forever.txt` | init never gives up on the shell or the console: each killed 12 times in a row (more than the 10 a minute that give any other service up), and the prompt still answers | |
| `review-cad.txt` | a `run` program can't send Ctrl+Alt+Del | |
| `review-killinit.txt` | the shell refuses to kill init; supervised services come back | |
| `review-longline.txt` | a line longer than the screen row | |
| `review-runshell.txt` | Ctrl+C reaches the outer shell past a second shell | |
| `review-steal.txt` | a `run` program can't take devmgr's console | the `tools/usbkeys-test.sh` `QEMU_USB` (hub on port 2, `usb-kbd,id=keys` at 2.1, mouse on 3) |

## netstack

Most of netstack's tests are in utest, part of the `init` run
([the tiers](#the-tiers)), with no QEMU network card
([M9-PLAN](M9-PLAN.md#stage-3a-built-the-core-without-a-device)). The
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
| `netctl_set_and_clear` | the control channel served by `ctl.c` on a channel of the test's own: `set_ipv4` and `set_dns` show in `info`; 13 addresses a host can't have and 2 bad DNS servers refused, nothing changed; a /30 and a /8 taken; `clear`; a request of the wrong size refused |
| `netctl_process` | bin/netstack started with its control channel at `SR_USER + 0`: set, a refusal, `info` (no device, link down), `stats` (nothing sent or received), `clear`; killed, its job empty |
| `ipv4_text` | libos's `<ipv4.h>`: dotted quads (10 malformed ones refused), formatting, and init's `net.address` form (8 malformed ones refused) |
| `netdrv_ping_and_link` | netstack finds the card through the device channel (GET_SERVICE), netdev.info and netdev.open; ARP and a ping through the rings; the link going down (`NETDEV_SIG_LINK`, netdev.info on the session): nothing sent; up again: the address announced, pings answered |
| `netdrv_restart` | the driver's session closed (a driver restart): netstack asks devmgr again, opens a new session, announces its address, keeps it and its ARP entries; `netctl.device` counts 2 sessions |
| `netdrv_hostile_driver` | rx slots of length 0, 1515 and 13 and with flags set: refused, counted, nothing answered, pings still answered; an rx `produced` five rings ahead and a tx `consumed` ahead of what was sent: netstack ends the session itself, counts the ring errors and opens a new one, which works; killed, its job empty |

End to end, with QEMU's e1000e and the network peer:
`tools/netstack-test.sh <outdir>` (`tools/shell-tests/netstack.txt`)
writes `net.address = 10.2.21.5/24 ...` into a copy of the image's
settings, boots with `QEMU_NET=1` and the peer's `--ping 10.2.21.5`
(an echo request every half second, once it has seen the guest's MAC),
kills netstack in the middle (init starts it again: a new session, the
address again) and passes if the peer's and the pcap's VLAN checks pass
and at least 8 pings were answered. It needs drv/e1000e (stage 2).

## Area scripts

Each prints PASS or FAIL and exits 0 on PASS; `QEMU_SMP` (and where it
matters `QEMU_XHCI`) pass through.

| Script | What |
|---|---|
| `tools/usb-test.sh <outdir>` | the `init` run with a hub, a test keyboard behind it, a CCID device, a mouse and a keyboard; the monitor script answers usbtest's markers (keys, kill hid, unplug, replug, unplug the hub) |
| `tools/usb-early-test.sh <outdir>` | input early in boot (`usb-early.txt`): a plain boot with a hub (a keyboard, a disk and a slow keyboard behind it), a mouse and a slow keyboard on root ports; the slow ones have the serial number `jamos-test-slow`, which usb-bus treats as a device that doesn't answer (its first two attempts on a port each take a second and fail). The console's "input ready: the first keyboard and mouse" line must come under `EARLY_MAX` s (1.5) and before either slow device attached; both slow ports are tried again after 100 and 200 ms and attach on the third attempt; prints the times |
| `tools/bootdisk-test.sh <outdir>` | the boot disk with two Jam OS sticks in (`bootdisk.txt`): a copy of the image with another MBR disk id and a file of its own on its data partition, on qemu-xhci port 2; `/data` must be the boot stick's and the copy's data partition `/usb1`, before and after a `reboot` (kexec), and the kernel must name the boot stick's id both times (the second from the kernel before) |
| `tools/storage-test.sh <outdir>` | the `init` run with two more usb-storage disks behind a hub: usbtest's storage checks (bulk transfers, a STALL and reset recovery, usb-storage taking over a disk left mid-READ, the ESP's boot sector read through `block`, read-only and out-of-range requests refused, a write read back), the second disk read while a READ waits on the slow one (usb-bus runs each device's transfers apart), the monitor script unplugging the second disk while it is being read, and READs timing out on a disk QEMU throttles to 4 KiB/s |
| `tools/usbkeys-test.sh <outdir>` | typing into the shell through usb-bus, hid and the console (`usbkeys.txt`) |
| `tools/fun-test.sh <outdir>` | the apps (`fun.txt`); `FUN_HD=1` runs at the PC's 2560x1440 |
| `tools/apps-test.sh <outdir>` | snake, mines and sysmon (`apps.txt`), with a USB mouse; `APPS_HD=1` runs at 2560x1440 |
| `tools/jamjar-test.sh <outdir>` | jamjar (`jamjar.txt`), with a USB keyboard and mouse and an hda-output capture, on a library the script makes (six made-up stereo songs as MP3 under owner-style UTF-8 names with made-up covers in their tags, drawn with Python's PIL; a calibration track, 1 kHz on the left and 4 kHz on the right; two MP3s with a bad cover); it checks the calibration shot's bars with PIL; `JAMJAR_HD=1` runs `jamjar-hd.txt` at 2560x1440; screenshots `jamjar-*.png` |
| `tools/jamjar-covers-test.sh <outdir>` | jamjar's now-playing cover at 2560x1440 (`jamjar-covers.txt`), on 32 made-up albums of two MP3s, each with a teal PNG cover in its tag: every shot of now playing must show the cover's teal, not a jar label |
| `tools/mouse-test.sh <outdir>` | the mouse end to end (`mouse.txt`): QEMU's monitor moves and clicks a USB mouse, driven in the report protocol (its descriptor has a wheel); `MOUSE_HIDBOOT=1` adds the boot word `hidboot` and checks the boot protocol instead; 1280x800 only (the clicks are at pixel positions) |
| `tools/crash-test.sh <outdir> [name...]` | every crash test from the shell (`crash <name> yes`), each on a fresh boot; each panic starts the stored kernel, whose shell says what happened (`kexecbad`: refused, the panic screen stays up; `kexecstall`, `kexecfault`: a firmware reset) |
| `tools/kdump-test.sh <outdir> [case...]` | a panic starts the stored kernel, each case a fresh boot of a stick image read afterwards with mtools: `save` (`crash panic yes`: no panic screen, the next boot comes up on every CPU, saves `/data/logs/boot-0001-crash.txt` with the panic and the lines before it, logs to `boot-0002` and its shell says so), `loop` (`crashtest=lockorder`: the next boot panics at once, a crash loop, and halts on the red panic screen), `bad` (`crash kexecbad yes`: the damaged stored kernel is refused, red panic screen, nothing saved), `nostick` (the stick pulled first: the shell says the log was not saved and why), `screen` (with the splash: the screen as the next kernel starts is all the splash background) |
| `tools/kexec-reboot-test.sh <outdir> [run...]` | `reboot` by kexec: `kexec` (an unchanged stick: no file read, no firmware reset, the screen all the splash background as the next kernel starts, which brings up every CPU, plays the splash and reaches the shell and `/data`; the old boot's log ends with the reboot's sync), `changed` (the stick swapped for one whose kernel file is longer: both files read and loaded first, the screen blanked meanwhile), `load` (the stick swapped the same way, then `kernel load`: the files read and loaded at once, and the `reboot` after it reads nothing), `broken` (the stick swapped for one whose kernel file is cut short: `kernel load` refuses it and keeps the stored copy, `reboot` says so on the screen and starts the stored copy, no firmware reset), `firmware` (`reboot -f`), `fallback` (`crashkernel=0`: `kernel load` says there is no stored kernel, `reboot` falls back to the firmware) |
| `tools/data-test.sh <outdir>` | the stick's filesystems end to end, three boots of one stick image (`data-1.txt` to `data-3.txt`): written, rebooted, read back; QEMU quit in the middle of writes and the dirty volume mounted again; then the boot logs read off the image with mtools, as the Mac reads the real stick |
| `tools/soak-test.sh <outdir>` | the soak test (`soak-plug.txt`): `soak loops=3` at a fixed seed (`SOAK_LOOPS`, `SOAK_SEED`), under the kernel's and `bin/soakload`'s load, with a second stick (made writable) pulled in the middle of writes and plugged back and then the boot stick pulled and plugged back (`SOAK_LOAD`: the kernel load workers, default one per CPU); PASS needs 0 FAILED kernel tests, utest runs and file checks, the job tree's message bytes grown by at most 32 KiB (unread messages piling up), and the second stick's own files unchanged |
| `tools/ktest-keep-test.sh <outdir>` | the test runner's own failure paths, with three tests that exist for it (`ktest=review_ktest`): with `keep` both failures are recorded and the run goes on; without it (and `crashkernel=0`, so the panic screen stays up) the first panics and the panic screen names loop, seed and test |
| `tools/hda-test.sh <outdir>` | the HD Audio driver (`hda.txt`): two emulated controllers (intel-hda with hda-duplex and hda-output, ich9-intel-hda with hda-micro), each codec's graph in the log, `hda` from the shell, `kill hda` and devmgr's restart; the path self-test passes and every codec's path is DAC 02 -> pin 03, set up muted; the jack self-test passes (the ALC897's jack table and tags, the RIRB's demultiplexer on a fake RIRB, the debounce, unsolicited responses and the polling fallback against a fake codec that allows jack code only SET_UNSOLICITED_ENABLE, SET_PIN_SENSE and GET_PIN_SENSE; QEMU's codecs have no presence detection, so the real jack path runs only on the PC); QEMU's codecs trace every verb they get and every one must be a GET or a silent SET (power D0, a connection select, pin control with the output off, an amp mute), and none a jack verb (0x708, 0x709); `hda` opens no stream, so no converter format or stream tag either; `hda jacks` shows the RIRB interrupt taken (it is on for unsolicited responses while the dumps' commands are polled, and the dumps must still see no timeout); then the `init` run with the same devices, where each driver must stop cleanly and the run end "run complete: no problems" |
| `tools/hda-stream-test.sh <outdir>` | the HD Audio output stream (`hdastream.txt`): intel-hda with an hda-output codec (`mixer=off`) whose samples go to a WAV file through QEMU's wav backend at 48 kHz 16-bit stereo; `hdatest` passes (the position's rate within 2 %, a closed stream released, a kill mid-stream: restart, the dead driver's pins out of the DMA quarantine unwritten); the WAV holds `hdatest`'s one-second pattern sample for sample after the leading silence, then most of a ring of silence (the driver's clear-behind); the codec got only allow-listed verbs, and the path opened only while the converter has the stream's tag and closed again before it is released (`tools/hda-verbs.awk` follows the state the SETs leave; hdatest turns the gain down while it plays) |
| `tools/beep-test.sh <outdir>` | `beep` (`beep.txt`): intel-hda with an hda-output codec with its mixer on (its DAC amp scales what the WAV gets) through QEMU's wav backend; `beep 440 500` at the default -30 dB: the tone's frequency from its zero crossings within 1 %, its length within 30 ms, the fades (first and last 2.5 ms well under the peak), no clicks, silence after, the peak at QEMU's volume for step 44 within 5 %; the codec's verbs: the path opened only while the converter has the stream's tag and muted again before it is released, the DAC's amp opened at step 44 only; the output stage (pin output, EAPD) on before the first stream and never off, the first unmute at least 400 ms after it went on; the driver's lines in order (stream open, unmuted, muted again, stream closed); `hda gain` set and clamped |
| `tools/play-test.sh <outdir>` | `play` (`play.txt`): WAV files made by the script and copied onto the stick image's `/data` with mtools, played through intel-hda with an hda-output codec (mixer on) into QEMU's wav backend: 48 kHz stereo (440 Hz left, 660 Hz right), 44.1 kHz mono, 22.05 kHz 8-bit, 96 kHz 24-bit WAVE_FORMAT_EXTENSIBLE with `play -v -20`; each sound's frequency per channel within 1 %, its length within 2 %, mono equal on both channels, silence after it, no clicks, the `-v` one's peak a tenth of the others' (its mixer stream at -20 dB) and the others' unchanged; garbage, a cut-off header, 32-bit float and a missing file refused with their reasons; a 10 s file stopped by Ctrl+C after 2 s ends within 3 s with a fade; QEMU runs with the trace event `hda_audio_overrun`, and each codec buffer it dropped on a busy host excuses one click or one sound 2048 frames short |
| `tools/audio-quality-test.sh <outdir>` | sound quality (`quality.txt`): signals made with numpy, copied onto the stick image's `/data`, played with `play -s` through the mixer into intel-hda with an hda-output codec (`mixer=off`) and QEMU's wav backend (16-bit); measured: a 48 kHz stereo file bit-exact at 0 dB; a 1 kHz sine at 0/-20/-40/-60 dB of volume at its level within 0.1 dB, its THD+N and spurs, and (dithered) no harmonic above the noise at -40/-60 dB; 44.1 kHz tones at 1-20 kHz within 0.1 dB with no image or alias above -90 dB; a 20 s tone read from the stick with no dropout and its length within 10 ms; no click anywhere; every `play -s` 0 underruns and 0 late periods, and no "frames late" in the log |
| `tools/mp3-test.sh <outdir>` | `play` of MP3s (`mp3.txt`), as play-test does: tones encoded by ffmpeg (libmp3lame and mp2; the script needs it) and copied onto `/data` with mtools: 44.1 kHz stereo 192 kbps CBR (440/660 Hz), 48 kHz mono VBR without an ID3v2 tag, 22.05 kHz MPEG-2 without a Xing/Info tag, one with ~230 KiB of album art in its ID3v2 tag and an ID3v1 tag, MPEG-1 Layer II, one with 3000 bytes of noise in its middle, one cut off mid-frame; each sound's frequency per channel within 1 % (its middle 80 %), its length (within 2 % where a LAME tag gives the delay and padding), mono equal on both channels, silence after it, no clicks (but where frames are missing); noise and an ID3 tag followed by noise refused; a 10 s file stopped by Ctrl+C with a fade; `play -n` decodes all 441000 frames of it and prints the cost per second of audio; QEMU's codec buffer drops excused as in play-test |
| `tools/splash-test.sh <outdir>` | the boot splash (`splash.txt`, `splash-keys.txt`, `splash-off.txt`, `splash-hang.txt`; `tools/splash-check.py` compares with `boot/splash.mpg` decoded by ffmpeg): a plain boot with an hda-output codec into a WAV: the screen all `#1E1A1D` as init starts, two screenshots a second apart that are frames of the video (2560x1440 scaled down by a 2:1 box onto 1280x800) in order, the sound started together with the picture at 0 ms, the last frame lingering 0.5 s, the console's text after the hand-back, the capture the video's sound from where the splash said it joined to its end, `run splash --selftest`; two lines typed while it plays at 2560x1440 (a frame drawn 1:1, no skip, the whole sound taken, the lines run by the shell once it is up); `verbose` and `nosplash` (the text log, no splash); `shell testpf` (the panic screen over the quiet one); `splashhang` (a splash that never finishes: init gives up on it 20 s after its start, kills it and starts the shell, whose text is on the screen); prints the time to the first frame and to the shell with and without |
| `tools/clock-test.sh <outdir>` | the date and the settings (`clock-1.txt`, `clock-2.txt`), one stick image, two boots with the RTC at 2026-01-15 01:02:03: Sydney's time from the RTC taken as local time, the settings file made with its defaults, `date -u`/`-r`/`-z`, `vol master`, `music vol` and the zone kept, then `reboot` (kexec): the next boot has the zone and the volumes; then, with mtools, a file written in boot 1 is dated 2026-01-15 1:02, `logs/boot-0001.txt` starts with its date, the settings file holds what was set and no `settings.new`; `rtc = utc` put in from this side: boot 2 reads the RTC as UTC |
| `tools/screen-test.sh <outdir>` | the screen on a plain boot (`screen.txt`, booted with the splash, and `screen-verbose.txt`): after the splash the shell's banner and prompt and no kernel log (no pixel of the log's colours) and no RESULTS box; a stick plugged in (one notice: `mount -w` and `mount -r` in between add none) and pulled out, the boot stick pulled out and back, each a yellow notice and a `console: notice:` line in the log; `ktest` puts its lines on the screen while it runs; `run utest impostor` (processes called init, devmgr and logd write lines the real ones make notices of: the log has them, and no notice); `run console selftest` (log lines made into notices or not, by their text and their writer: crashes, kills asked for, give-ups, `/data` full, impostors); then a `verbose` boot whose shell screen has the log; then a plain boot with a stick in from the start (`screen-atboot.txt`): no notice for it, one when it is pulled out |
| `tools/mixer-test.sh <outdir>` | the mixer (`mixer.txt`): intel-hda with an hda-output codec (`mixer=off`) into QEMU's wav backend; `mixtest` passes (the protocol's refusals and clamps; two openers of `/svc/audio` and of `/svc/audioctl` each answered on their own channel; the per-opener stream cap, 4 each and 16 in all; `audioctl.device`'s query channel to the driver answers but refuses `open_output` and `query`, at most 8 at once; tone programs at once; one killed; the master and an `audioctl` volume; the mixer killed and the hda driver killed mid-tone, each played on; a stream left empty lets the mixer close the output and its next write wakes it; two programs on `<audio.h>` at once); the WAV, segment by segment: 440 Hz and 1000 Hz at once with the second at -6 dB (amplitude ratio within 3 %), the killed client's partner with no gap (QEMU's own buffer drops on a busy host, traced with `hda_audio_overrun`, are told apart from a gap), -6 dB master and -12 dB `audioctl` volume heard, the library's 44.1 kHz and 48 kHz tones together; the log's stream, restart and output lines; the codec's verbs: muted whenever nothing plays |
| `tools/music-test.sh <outdir>` | the music player (`music.txt`): a folder tree made by the script (ffmpeg, mtools) on `/data/music`: six 2 s tones (MP3 at 44.1, 48 VBR and 22.05 kHz, WAV at 48 and 44.1 kHz) under names with spaces, apostrophes, `$`, `~`, parentheses and UTF-8 (`JAŸ-Z`), an upper-case `.WAV` and `.Mp3`, a garbage `.mp3`, `.DS_Store`/`._` dotfiles, a text file and an empty folder tree; and a second stick of three 8 s WAVs. In the capture (100 ms FFT windows up to a 1000 Hz marker beep typed 2 s after `music stop`): the tracks heard are the log's `track N:` lines in order, the first six are the six tracks once each, at least seven heard, never the same twice in a row; the 1500 Hz `beep` mixed over a track; the stop fades. The log: every title (`Artist - Title` from the path), the garbage file skipped, no dotfile tried, the mixer restart reopened, the pulled stick stopping it after three unreadable files |
| `tools/net-test.sh <outdir> [vlan vlan-off rx]` | the e1000e driver against `nettest`, a hostile netstack ([M9-PLAN](M9-PLAN.md#stage-2-built-drve1000e-and-nettest)), one `init` boot per scenario from a copy of the stick whose bootfs runs `bin/nettest <mode>` from init.cfg (in shell mode netstack holds the card's one session), with `QEMU_NET` and a peer of the script's own (`tools/netpeer.py`'s `Peer` plus nettest's frames; it also fails on a tag inside VLAN 21's): `vlan` (the session rules; every bad length, flags, frames already tagged 0x8100, 0x88a8 and 0x9100, `produced` a ring and one ahead and then behind, a thread rewriting EtherTypes while the driver copies: each refusal counted exactly, and the peer and the pcap each hold exactly the frames the driver queued and the chip sent, all tagged 21 once), `vlan-off` (a `vlan=off` boot: the driver's "no VLAN" line, no service, no frame at all), `rx` (the peer's census of untagged, VLAN 0, other-VLAN, QinQ, nested, 1522-byte and VLAN 21 frames: only the VLAN 21 ones arrive, untagged and whole, every drop counter exact; then 300 frames with the ring unread: 256 given, 44 `rx_ring_full`); every run ends with the driver stopped cleanly and every job empty |
| `tools/netprobe-test.sh <outdir>` | the RTL8125 driver's boot words in QEMU, which has no RTL8125 (the probe and the send test themselves run on the PC only): a `shell netprobe` boot (`netprobe.txt`), a `shell netsend` boot (`netsend.txt`) and a `shell net` boot (`netserve.txt`) where devmgr says it has the word and binds and runs nothing for it, then a plain `shell` boot whose log mentions none of them. utest covers the rest: `netframe_classify` and `netframe_short_frames` (`drivers/include/jam/netframe.h` over hand-made frames: untagged, tagged 21, another VLAN, 4095, priority-tagged, QinQ 0x88a8 and 0x9100, every short length); `netframe_tag` (every length 0-1600: 14-1514 tagged, short ones padded with zeros over a dirty buffer, the rest refused; VLANs 1 and 4094 yes, 0, 4095 and wider no; a buffer one byte short), `netframe_tag_refuses_tagged` (EtherType 0x8100, 0x88a8 or 0x9100 at any length never leaves, and nothing sendable stays behind), `netframe_tx_check` (lengths 18 and 1518, every change to bytes 12-17: another TPID, a priority, DEI, another VLAN, a tag inside), `netframe_tag_copy_is_the_frame` (the caller rewriting its frame after the copy changes nothing; a change to the copy is refused), `netframe_rx` (VLAN 21 kept at any priority and untagged correctly; untagged, VLAN 0, other VLANs, outer tags, a tag inside ours, every short length and over 1518 dropped, each by its reason); `rtl8125_write_guard` (the transmit registers refused in every width and overlap, the command register's transmit bit, TDFNR, nothing else), `rtl8125_tx_gate` (full mode with a VLAN only), `rtl8125_args` (the modes, `netprobe` winning over `netsend`, hostile `vlan=` and `arpto=` words), `rtl8125_arp` (the probe's exact bytes, tagged and checked like any frame; the reply and its near misses), `rtl8125_stays_off` (the driver without a VLAN, and the probe, the send test or the netdev service without hardware, ends at once with exit 0 and an empty job); `netserver_session`, `netserver_tx`, `netserver_rx` (the network drivers' netdev server, `drivers/lib/netserver.c`, linked in, over a fake card with the test as netstack: info and stats, one session at a time, open refused on the session channel, a closed or orphaned session replaced, the rights open hands out; netstack's frames reach the card byte for byte, bad lengths, flags and tags counted and never sent, a card out of descriptors holds the ring until they come back, hostile counts bounded to a ring per turn; received frames into the rx ring with NETDEV_SIG_RX, a full ring and no session dropped and counted, NETDEV_SIG_LINK) |
| `tools/update-test.sh <outdir>` | `update`'s check in init without a network (`bin/updtest` hands init the build from files on `/data/update/`, put there with mtools: this build's kernel, its boot image with one more file `update-marker.txt`, and their manifest from `tools/update-server.py --manifest`), one run, three boots: `run updtest bad` (12 damaged offers, each refused for its own reason: a kernel or boot-image byte changed, the kernel 4 KiB longer or cut to half, a VMO shorter than its length, a garbage, cut, signed or format-2 manifest, a bad magic, one handle, two files that match their manifest but are no kernel), then `reboot`: no marker (the stored kernel untouched); `run updtest good` (accepted and stored), then `reboot` reads nothing from `/esp` and the marker is there (the fetched build runs); exactly one `kexec_load` succeeded. utest covers the parsers and the fetcher: `update_manifest*` (the format, each line wrong each way, every prefix, every byte changed, random bytes), `updwire_*` (golden bytes from the Python tool, every truncation and field, random datagrams), `updfetch_*` (the window against a fake server on a scripted clock: clean, lossy with repeats, reordering and forged replies, the snapshot gone once and always, no answer, a bad or oversize manifest, a lying file size) |
| `tools/sticks-test.sh <outdir>` | other sticks (`sticks.txt`): five more disk images (`tools/mkstick.py`) plugged and pulled through the monitor: an MBR FAT32 stick, one with no partition table, one made writable and pulled mid-copy, one with a blank FAT32-typed partition and a foreign one, one of noise. Afterwards, from the host: the file written after `mount -w` is on the image (mtools) and the refused ones are not; the images that were only read, or held nothing to mount, are byte for byte unchanged (never written, never formatted) |

## The DHCP and DNS cores (utest)

The DHCP client and the DNS resolver
([M9-PLAN.md](M9-PLAN.md#stage-5a-built-the-dhcp-and-dns-cores)) are
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
  a full table; ports the edge says are taken; the cache's bounds.

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

## The other tools

The rest of `tools/` builds, checks and flashes; the tests above use some
of them. Each file's header says more.

| Tool | What |
|---|---|
| `tools/serial-feed.py` | types a shell script into QEMU's serial port (`QEMU_INPUT`) |
| `tools/mkstick.py` | disk images standing in for other people's sticks (`tools/sticks-test.sh`, `tools/soak-test.sh`) |
| `tools/fat-label.py` | checks, or with `--fix` repairs, a FAT volume's label in an image the way other systems read it (the Makefile runs it on every image; `tools/data-test.sh` checks with it) |
| `tools/checkdocs.py` | the docs check of `make check` |
| `tools/checkwants.py` | every boot-image program's list ([`<wants.h>`](../user/include/wants.h)) checked by the rules libos applies to a program on `/data`, before the Makefile packs the boot image: the build's approval |
| `tools/checkdriver.py`, `tools/checkdriver-selftest.sh` | the driver build check, and the proof that it still rejects what it must (`tools/checkdriver-tests/`) |
| `tools/sortincludes.py` | the include-order check of `make check`; `make includes` runs it with `--fix` |
| `tools/gensyscalls.py`, `tools/genidl.py`, `tools/gensyms.py` | the syscall glue, the IDL headers and the kernel symbol table |
| `tools/mkbootfs.py`, `tools/mkimage.py` | the boot image and the two-partition disk image |
| `tools/write-usb.sh`, `tools/mbr-grow.py` | `make usb`: the image onto a stick, the data partition grown to its end and left blank |
| `tools/flash-usb.sh` | `make flash`: a new kernel, boot image and boot menu onto a stick's ESP |
| `tools/flash-test.sh <outdir>` | `tools/flash-usb.sh` on a copy of the image attached with `hdiutil` (never a real disk; macOS): the new files are there, no `.new` file is left (a stale one placed first included), the data partition is unchanged |
| `tools/mksplash.sh` | `boot/splash.mpg` from the owner's animation and its sound (ffmpeg); `make` runs it only when both source files are there (`SPLASH_SRC`) and one is newer |
| `tools/bdf2c.py`, `tools/compdb.py` | `make font` (the console font from Spleen's BDF), `make compdb` |
| `tools/update-server.py` | serves `build/jamos.elf`, `build/bootfs.img` and their manifest to the PC's `update` (UDP 5022, a snapshot per manifest request); `--manifest K B` prints a manifest; `--self-test` runs it against a Python client on 127.0.0.1 (a whole fetch with 10 % lost both ways and the files rebuilt half way: the snapshot's bytes; a new snapshot; GONE, RANGE, a short last piece; malformed requests unanswered; old snapshots dropped; the manifest's rules; the version from `build/jamos.elf`) |
| `tools/netlog-recv.py <folder>` | receives the PC's log (UDP 5021): a file per boot, gaps reported; `--self-test` runs it against a Python sender on 127.0.0.1 (started late, paused, 10 % lost both ways, the PC's ring having dropped the start, a crash log, restarted with and without its files, hostile datagrams). utest's `netlog_*` cover the PC's side: golden and hostile datagrams, the whole log over a lossy reordering network, the Mac away ten minutes (one line said, the waits up to 30 s, one datagram per try), the ring dropping bytes before and during, forged acks, the crash log's stream, the live source over the real kernel log |

## Known noise

- 4-CPU stress throughput under QEMU is bimodal (context switches from
  thousands to millions in 10 s): CPU-hog threads on 4 vCPUs, not a
  regression. Don't chase it.
- QEMU can't measure page-allocator scaling or anything that depends on
  which physical pages a run lands on ([BENCH.md](BENCH.md) has the
  details). Performance is measured on the PC.
