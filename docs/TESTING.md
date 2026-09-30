# Testing Jam OS

How to test a change, cheapest first, with the exact commands. The rule
that a bug fix comes with a test is in
[CODING-GUIDE.md](../CODING-GUIDE.md#7-testing); the latest results are in
[NEXT.md](../NEXT.md) and, per milestone, in [HISTORY.md](HISTORY.md).

## The tiers

| Tier | Command | When |
|---|---|---|
| Build | `make`, `make KTESTS=0`, `make check` | every commit |
| Kernel tests | `QEMU_SMP=4 QEMU_TIMEOUT=90 tools/qemu-test.sh build/test kt ktest`, again with `QEMU_SMP=8` (the full run takes ~25-30 s, too close to the 30 s default) | every kernel change |
| User regression | `tools/qemu-test.sh build/test init init` | every change to syscalls, libos, services, drivers |
| Shell scripts | [below](#shell-scripts) | shell, console, input |
| Area scripts | [below](#area-scripts) | the area you touched |
| 2-minute stress | `QEMU_SMP=8 QEMU_TIMEOUT=200 tools/qemu-test.sh build/test st selftest stress=120` | after each fix round, in QEMU and on the PC |
| 10-minute stress | the boot menu's 10-minute entry, or `stress 600` in the shell | milestone sign-off only, on the PC |
| The PC | the owner flashes the stick and runs it ([HARDWARE.md](HARDWARE.md#flash-and-boot-the-stick)) | the final judge |

- QEMU passing is necessary, not sufficient: TCG has no PCIDs and no
  TSC-deadline timer, and USB timing differs. Say what still needs the PC.
- The Mac is shared by several agents: a timing failure may be load. Rerun
  once; a second failure is real.
- On the PC the 2-minute stress is skipped right before a sign-off: the
  10-minute run covers it.

## Build checks

- `make` builds with `-Werror`; `make KTESTS=0` builds a kernel without
  the in-kernel tests, benchmark and test hooks (into `build/noktests/`).
- `make check`: the generated syscall and IDL code matches `abi/`
  (`tools/gensyscalls.py check`, `tools/genidl.py check`); the driver
  isolation check still rejects what it must (`tools/checkdriver-selftest.sh`
  over `tools/checkdriver-tests/`); and the docs match the tree
  (`tools/checkdocs.py`: links, anchors, and the repo paths, file names,
  headers and make targets named in backticks).

## Running QEMU: tools/qemu-test.sh

```sh
tools/qemu-test.sh <outdir> <name> [kernel command line...]
```

Copies `build/jamos.img`, sets the boot entry's command line, boots it
headless (q35, OVMF, the stick on qemu-xhci port 1, the `edu` test device),
waits until the kernel halts or idles, and leaves `<outdir>/<name>.log`
(serial) and `<name>.png` (the screen; needs Pillow). Environment:

| Variable | Default | What |
|---|---|---|
| `QEMU_SMP` | 4 | CPUs |
| `QEMU_MEM` | 2G | memory |
| `QEMU_CPU` | max | CPU model, e.g. `max,-x2apic` |
| `QEMU_TIMEOUT` | 30 | seconds before giving up |
| `QEMU_IMAGE` | build/jamos.img | another image, e.g. build/noktests/jamos.img |
| `QEMU_XHCI` | | qemu-xhci properties, e.g. `msi=on,msix=off` (MSI-only, like the PC) |
| `QEMU_USB` | | more USB devices; give each a `port=` (port 1 is the stick) |
| `QEMU_EXTRA` | | more QEMU arguments, e.g. `-rtc base=2026-01-15T01:02:03` |
| `QEMU_INPUT` | | a script typed into the serial port by `tools/serial-feed.py` (its header has the commands: `wait`, `seen`, `send`, `type`, `sleep`, `shot`, `monitor`, `usbkeys`); the run passes if every `wait` matched and QEMU ended by itself |
| `QEMU_MONITOR` | | a script of `expect` / `send` / `sleep` lines run against the QEMU monitor |

Examples:

```sh
tools/qemu-test.sh build/test kt ktest                    # every kernel test
tools/qemu-test.sh build/test chan ktest=chan             # tests whose name starts "chan"
QEMU_SMP=8 QEMU_TIMEOUT=60 tools/qemu-test.sh build/test st selftest stress=30
QEMU_XHCI=msi=on,msix=off tools/qemu-test.sh build/test msi init
make run                                                  # interactive: the shell, a USB keyboard, serial on stdio
make debug                                                # the same, stopped for gdb on :1234
```

## The boot menu

`boot/limine.conf` is the menu; each entry is a kernel command line.

| Entry | Command line | What it does |
|---|---|---|
| Jam OS | (empty) | init starts the console, serialin, devmgr (with the USB drivers) and the shell |
| Jam OS (safe mode) | `nousb` | the same, but devmgr leaves USB alone: input only over serial |
| Tests / All tests | `ktest` | every in-kernel test at boot, strict |
| Tests / Stress test (2 minutes) | `selftest stress=120` | after each fix |
| Tests / Stress test (10 minutes) | `selftest stress=600` | milestone sign-off |
| Tests / Benchmark | `bench` | about 10 s; results go to [BENCH.md](BENCH.md) |
| Tests / init + utest + usbtest | `init` | the user-space regression run: init runs `boot/init.cfg` (utest, then usbtest) and the RESULTS box says whether init's root job ended with nothing charged |
| Tests / Timer fallback | `nodeadline selftest` | the periodic LAPIC timer instead of TSC-deadline |

Other boot words (for `tools/qemu-test.sh`, not in the menu):

- `shell`: the plain boot, spelled out (what the shell scripts use).
- `ktest=<prefix>`: the tests whose name starts with the prefix. Tests
  named `review_...` run only when the prefix asks for them.
- `selftest`: the boot-time self-checks; `stress=<seconds>` adds the stress
  test.
- `pcilist` (the PCI device report), `keytest` (keys to the log for 30 s),
  `memmap`, `init_timeout=<s>` (how long the `init` run may take).
- `test<name>`: a crash test at boot (`testpf`, `testlockorder`, ...; the
  names are in `kernel/debug/selftest.c`). Each must end on the panic
  screen with the right message; `testbp` must come back.
- Switches for the scheduler and friends, each turning one optimisation
  off to compare: `nopcid`, `nospinidle` (or `idlespin=<us>`),
  `noplaceorder`, `noaffinepair`, `nokmcache`, `nooneshot`, `noserialirq`,
  `nofpuopt`.
- `panic_reboot=<s>`: after a panic, count down s seconds (1..3600) and reboot instead of halting.

## From the shell

Most tests are shell commands, so a test run needs no reboot: `ktest
[prefix]`, `bench`, `stress <seconds>`, `utest`, `usbtest`, `crash <name>
yes` (the deliberate panics), plus `devices`, `usb`, `pci`, `memmap`.
`ktest` from the shell runs "live" next to the rest of user space: checks
on system-wide counts are not made, and tests that need the machine to
themselves are skipped (`KT_SKIP_LIVE`, in `kernel/include/jam/ktest.h`);
the summary line says how many.

## Shell scripts

Each file in `tools/shell-tests/` types into the shell over the serial
port. Unless the table says otherwise, run it as:

```sh
QEMU_INPUT=tools/shell-tests/<name>.txt tools/qemu-test.sh build/test <name> shell
```

| Script | What it covers | Extra setup |
|---|---|---|
| `basic.txt` | line editing, history, console protocol levels, restarting serialin and the console; `contest flood` (a client that floods the console must not stop Ctrl+C or other clients) | |
| `cmds.txt` | the everyday commands: information, date and time zones, files, pipes and text, variables and aliases, programs, Tab and Ctrl+C; `contest junk` and `contest spew` (oversized or handle-carrying pipe output is dropped with its handles closed; a flooding program still stops on Ctrl+C) | `QEMU_EXTRA="-rtc base=2026-01-15T01:02:03"` (the date checks), `QEMU_SMP=4` |
| `system.txt` | the System and Tests commands through the command table: argv, status, pipes, help, aliases | |
| `commands.txt` | utest, usbtest, pci, memmap, the crash list, demo, Ctrl+C past a program, orphans killed with their job, devmgr restarted by init; ends with a real crash | |
| `extras.txt` | bench and a short stress from the shell, scrollback, clear; ends with a panic over the console | |
| `fun.txt` | the apps (life, tetris, fractal): self-tests, play, screenshots, kill and crash with the screen borrowed | use `tools/fun-test.sh` |
| `ktest-all.txt` | every kernel test from the shell, live | |
| `nousb.txt` | safe mode | command line `nousb` instead of `shell` |
| `parse-limits.txt` | the shell's 32-segment limit and unclosed quotes | |
| `usb.txt` | the `usb` command | `QEMU_USB="-device usb-hub,bus=xhci.0,port=2 -device usb-kbd,bus=xhci.0,port=2.1"` |
| `usbkeys.txt` | typing on a USB keyboard behind a hub; kill hid, the console, devmgr | use `tools/usbkeys-test.sh` |
| `review-cad.txt` | a `run` program can't send Ctrl+Alt+Del | |
| `review-killinit.txt` | the shell refuses to kill init; supervised services come back | |
| `review-longline.txt` | a line longer than the screen row | |
| `review-runshell.txt` | Ctrl+C reaches the outer shell past a second shell | |
| `review-steal.txt` | a `run` program can't take devmgr's console | the `tools/usbkeys-test.sh` `QEMU_USB` (hub on port 2, `usb-kbd,id=keys` at 2.1, mouse on 3) |

## Area scripts

Each prints PASS or FAIL and exits 0 on PASS; `QEMU_SMP` (and where it
matters `QEMU_XHCI`) pass through.

| Script | What |
|---|---|
| `tools/usb-test.sh <outdir>` | the `init` run with a hub, a test keyboard behind it, a CCID device, a mouse and a keyboard; the monitor script answers usbtest's markers (keys, kill hid, unplug, replug, unplug the hub) |
| `tools/usbkeys-test.sh <outdir>` | typing into the shell through usb-bus, hid and the console (`usbkeys.txt`) |
| `tools/fun-test.sh <outdir>` | the apps (`fun.txt`); `FUN_HD=1` runs at the PC's 2560x1440 |
| `tools/crash-test.sh <outdir> [name...]` | every crash test from the shell (`crash <name> yes`), each on a fresh boot |

## Known noise

- 4-CPU stress throughput under QEMU is bimodal (context switches from
  thousands to millions in 10 s): CPU-hog threads on 4 vCPUs, not a
  regression. Don't chase it.
- QEMU can't measure page-allocator scaling or anything that depends on
  which physical pages a run lands on ([BENCH.md](BENCH.md) has the
  details). Performance is measured on the PC.
