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
  (`tools/gensyscalls.py check`, `tools/genidl.py check`), the generated
  Wayland code matches the vendored XML (`tools/genwl.py check`) and the
  Wayland generator passes its own tests (`tools/genwl.py selftest`:
  tools/genwl-tests/, built and run with the Mac's compiler); the driver
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
  `tools/pcap-vlan-check.py --selftest`); `tools/checkwants.py
  --selftest`: the program-list check takes `svc net listen` and refuses
  `svc net-listen`, a listen on another service and a list over its 24
  wants; the FAT32 checker finds each kind of damage it knows in volumes
  it makes and damages (`tools/fatcheck.py --selftest`); the update signing tool passes Monocypher's Ed25519 vectors
  (`build/host/jamos-sign self-test`); and libfun's smooth text holds up
  against hostile input under ASan and UBSan (`build/host/fontcheck
  --check`, [below](#smooth-text)).

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
| `QEMU_BOOT_PREV` | 0 | 1: boot the stick's previous build (`/esp/boot/prev-jamos.elf` and `prev-bootfs.img`, the boot menu's "Jam OS (previous build)") instead of its own |
| `QEMU_MENU_AT` | | `efi`: the run's one-entry menu goes to `/esp/EFI/BOOT/limine.conf` (Limine 11 reads it before `/esp/boot/limine/limine.conf`), so the stick keeps its own boot menu (`tools/update-menu-test.sh`: `update` writes that one) |
| `QEMU_STICK_THROTTLE` | | the stick as slow as a real one: QEMU `throttle-group` limits, e.g. `x-iops-write=100,x-bps-write=10485760` (every write command waits its turn, 100 a second, whatever its size, as on a cheap USB 2 stick) |
| `QEMU_SPLASH` | 0 | 1: keep the boot splash (otherwise the boot word `nosplash` is added, so the tests see the text log) |
| `QEMU_NET` | | `1`: a network card and the test peer ([below](#the-network-peer)); `<peer port>:<qemu port>`: the same card and pcap, with a peer you run yourself |
| `QEMU_NET_VLAN` | 21 | the VLAN the peer and the pcap check want every frame tagged with, or `none`: every frame untagged and none tagged. The guest boots with the same `vlan=` word unless its command line has one or `QEMU_NET_WORD=0`, so a network test doesn't depend on the build's default |
| `QEMU_NET_WORD` | 1 | 0: don't add the `vlan=` word (a run of the build's default; set `QEMU_NET_VLAN` to it) |
| `QEMU_NET_NONE` | 0 | 1: no frame at all may leave the guest (the `vlan=off` run) |
| `QEMU_NET_PEER` | | more flags for `tools/netpeer.py`, e.g. `--noise 2` |
| `QEMU_IOMMU` | | QEMU's VT-d unit (`intel-iommu`, interrupt remapping offered; the guest starts it only with the boot word `iommu=on`): `1` caching mode on (new mappings need invalidating too), `cm0` caching mode off (the PC's case), `eim` caching mode on with x2APIC destination ids |
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
frame from neither address fails too (the PC's under another source) ([M9-PLAN.md](M9-PLAN.md#r1-the-pc-result-and-the-transmit-fix)).

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
one line. Only without a stored kernel, or for a panic within 30 s of a
start after a panic (a crash loop), does the panic screen stay up and the
machine halt ([ARCHITECTURE.md](../ARCHITECTURE.md#kexec-reboot-and-panic)).

| Entry | Command line | What it does |
|---|---|---|
| Jam OS | (empty) | the boot splash ([AS-PLAN.md](history/AS-PLAN.md)): the screen dark from the kernel's start, the logo animation with its sound (played to the end, full screen; what is typed meanwhile reaches the shell), then the desktop: the shell in a terminal window on the wallpaper; meanwhile init starts the bootfs server (`/boot`), the compositor, the console (the first terminal), serialin, devmgr (with the USB and PCI drivers; it mounts the stick's `/esp` and `/data`), the mixer, the music player, netstack, dhcp (without `net.address`), dns, logd and netlog (with `net.host`). devmgr binds the PC's network chip as the netdev service on the kernel's VLAN (21), so the shell has `net`, `ping`, `host` and `update`; QEMU's e1000e, when there is one, is bound the same way. "Jam OS (no network)" (below) keeps both off. `reboot` and a panic look like switching the PC on: the splash background at once, then the next boot's splash; after a panic the shell's first line says what it was and where its log went |
| Jam OS (text log, no splash) | `verbose` | the same with the kernel's text log on the screen instead of the splash, and in the shell as it comes (a plain boot keeps it off the shell's screen but for notices and the commands whose output it is: [ARCHITECTURE.md](../ARCHITECTURE.md#debugging)) |
| Jam OS (no compositor) | `nocomp` | the everyday boot as it was before G1: no compositor, the console draws the whole screen and takes the keys (one terminal, no Super+Enter or `term`), programs borrow the screen. The way back if the compositor misbehaves on the PC ([G1-PLAN.md](G1-PLAN.md#q4-what-the-console-becomes-and-a-way-back)); a reboot keeps the word |
| Jam OS (IOMMU) | `iommu=on` | the everyday boot with the IOMMU on (`iommu=on`, below): DMA translation and interrupt remapping. An entry of its own while it is new ([M11-PLAN.md](M11-PLAN.md#questions-for-the-owner), question 6); once the PC checks pass it becomes the default and `iommu=off` turns it off |
| Jam OS (safe mode: no USB drivers, serial input only) | `nousb` | the same, but devmgr leaves USB alone: input only over serial |
| Jam OS (network: listen only) | `netprobe` | the everyday boot, plus the RTL8125's listen-only probe ([M9-PLAN.md](M9-PLAN.md#the-first-pc-stage-listen-only)): devmgr binds `drv/rtl8125`, which sends nothing, listens for 60 s after the link comes up, logs its `[rtl8125]` lines and one RESULTS line, and exits. A `reboot` doesn't keep the word (the next boot is the everyday one, on the network) |
| Jam OS (network: send test) | `netsend` | the everyday boot, plus the RTL8125's ARP send test ([M9-PLAN.md](M9-PLAN.md#r1-progress-the-full-driver)): devmgr binds `drv/rtl8125` in full mode on the kernel's VLAN (none: "no VLAN: the network stays off", nothing touched), which waits for the link, sends twenty ARP probes for 10.2.21.1, 200 ms apart, tagged with the VLAN, logs for each when it was queued, when the chip handed its descriptor back and when the reply came, compares the chip's count of frames sent with its own, logs its `[rtl8125]` lines and one RESULTS line, and exits. Nothing else is ever sent; a `reboot` doesn't keep the word |
| Jam OS (no network) | `vlan=off` | the everyday boot with the network off: every network driver starts without a VLAN, logs `no VLAN: the network stays off` and leaves its card alone; netstack has no session, so nothing is ever sent ([ARCHITECTURE.md](../ARCHITECTURE.md#networking)) |
| Jam OS (previous build) | (empty) | the everyday boot of the build the stick had before the last `update` (`-w`) or `make flash` (`/esp/boot/prev-jamos.elf` and `/esp/boot/prev-bootfs.img`; `tools/qemu-test.sh` boots them with `QEMU_BOOT_PREV=1`); a stick that has had neither has no such files, and Limine says it can't open them |
| Tests / All tests | `ktest` | every in-kernel test at boot, strict, on an idle machine |
| Tests / Stress test (2 minutes) | `selftest stress=120` | the stress test alone, no user space: kernel work |
| Tests / Stress test (10 minutes) | `selftest stress=600` | the same for 10 minutes (it signed off the milestones up to M8; from A1 on the soak does) |
| Tests / Soak test (3 minutes) | `soak=3` | a plain boot whose shell runs `soak 3 halt` by itself ([Soak](#soak)): the first failure panics, and the next boot's shell names it (the log is on the stick); a pass ends with the SOAK RESULTS box and a prompt |
| Tests / Benchmark | `bench` | about 10 s; results go to [BENCH.md](BENCH.md) |
| Tests / IOMMU checks | `iommu=on vtdtest` | a plain boot with the IOMMU on in which drv/hda provokes its faults on purpose before it serves (`drivers/hda/vtdtest.c`): its command ring pointed at a page it doesn't hold pinned (a read the IOMMU blocks: a `vtd: fault:` line naming 00:1f.3), then its response ring at the interrupt window 0xfee00000 (a write blocked with fault 25h, no interrupt), then the rings back and sound as usual. `hda: vtdtest:` lines say what happened; if the controller could read the page anyway (no translation), it says so and provokes nothing. `iommu` in the shell shows the units, domains and fault counts |
| Tests / Compositor test scene | `comptest` | the compositor alone on the screen with its test scene (below, `comptest`): windows in the desktop's look held about 4 s each, then the RESULTS box with its `compositor: bench:` paint times |
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
- `nocomp`: a plain boot without the compositor (the menu's "Jam OS (no
  compositor)"; `user/services/init/comp.c`): the console owns the screen
  and the input as before G1, one terminal. Without it (every plain boot)
  init starts the compositor before the console, every console runs in
  window mode (a terminal window), the keyboards, the mouse and serialin
  are the compositor's input, programs that ask get `/svc/wayland` and
  open windows, and `term` and Super+Enter open more terminals. A reboot
  keeps it. The scripts that look at the console's own screen
  (`screen-test.sh`, `mouse.txt`, most of `splash-test.sh`) add it.
- `comptest`: init starts only the compositor, on the screen, running its
  test scene (`user/services/init/comptest.c`): three steps of windows and
  two of the desktop held 4 s each, then its `compositor: bench:` lines;
  the RESULTS box says how it ended (`tools/comp-test.sh`).
- `reset=cf9`, `reset=8042`, `reset=triple`: the firmware reset
  (`reboot -f`, and every other reset through the firmware) starts its
  list of methods there instead of at the ACPI reset register
  (`kernel/dev/reboot.c`; `tools/reboot-firmware-test.sh` tries each). A
  reboot keeps it.
- `hidboot`: hid keeps every mouse in the boot protocol (no wheel on
  most real mice) instead of the report protocol it uses for a mouse
  whose report descriptor has a wheel: the way back if a mouse misbehaves
  in report protocol (no movement, or nonsense). A reboot keeps it.
- `iommu=on`: start each VT-d unit's invalidation queue and fault
  interrupt (kernel/dev/vtd_unit.c; [M11-PLAN](M11-PLAN.md)) and turn DMA
  translation on (kernel/dev/vtd_boot.c): a function nobody drives is
  blocked (its DMA faults and is logged), one an RMRR names reaches only
  that region, and one a driver holds a DMA capability for reaches only
  what that capability pinned (its own domain: kernel/object/dma_cap.c;
  a dead driver's pins are freed once the unit confirms the domain is
  gone, not quarantined); and turn interrupt remapping on
  (kernel/dev/vtd_irq.c). Off by default: without it no VT-d register is
  written. `iommu=off` wins over it. The `vtd_unit_*`, `vtd_irq_*`,
  `vtd_domain_*` and `dma_iommu_*` ktests need it (and `QEMU_IOMMU`); they
  skip themselves without it; the other `dma_*` ktests check whichever
  mode the boot is in. A reboot keeps either
  word; the jump into the next kernel (a reboot or a panic) turns
  interrupt remapping, translation and the queue off first.
- `vtdtest`: devmgr passes it to drv/hda, which runs its IOMMU checks
  (`drivers/hda/vtdtest.c`; the "IOMMU checks" entry above) before it
  serves; only in shell mode, and a reboot doesn't keep it.
- `nospare`: init keeps no warm spare of the mixer
  (`user/services/init/spare.c`), and devmgr none of fat
  (`user/services/devmgr/spare.c`), so a restart starts a process, as
  before spares (it still hands the new instance its state and keep
  channel, and keeps the restart rule for a deliberate kill): the switch
  for comparing restart times. A reboot keeps it.
- `nolockdep`: the lock-order checker (`kernel/debug/lockdep.c`) checks
  no spinlock or mutex this boot: the switch for the benchmark's
  checker-off column (`bench` also flips it for spinlocks within one run,
  as the `lockdep` switch). The everyday boot checks every lock. A reboot
  keeps it.
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
`pci`, `memmap`, `iommu` (the IOMMU's units, domains and faults:
[The IOMMU](#the-iommu)). `reboot` kexecs into the stored kernel (or the files on
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
| `basic.txt` | line editing, history, console protocol levels, restarting serialin and the console (both modes: add `nocomp` for the console's); `contest flood` (a client that floods the console must not stop Ctrl+C or other clients) | |
| `cmds.txt` | the everyday commands: information, date and time zones (the clock from the RTC as Sydney's local time), files, pipes and text, variables and aliases, programs, Tab and Ctrl+C; `contest junk` and `contest spew` (oversized or handle-carrying pipe output is dropped with its handles closed; a flooding program still stops on Ctrl+C) | `QEMU_EXTRA="-rtc base=2026-01-15T01:02:03"` (the date checks), `QEMU_SMP=4` |
| `system.txt` | the System and Tests commands through the command table: argv, status, pipes, help, aliases | |
| `console-restart.txt` | a console started again after about 550 KiB of log (the klog gap test, 8 loops) draws only the last 256 KiB of it, and the shell comes back | |
| `commands.txt` | utest, usbtest, pci, memmap, the crash list, demo, Ctrl+C past a program, orphans killed with their job, devmgr restarted by init; ends with a real crash, and the next boot's line about it | |
| `extras.txt` | bench and a short stress from the shell, scrollback, clear; ends with a panic, and the next boot's line about it | |
| `perop.txt` | `perop -q /data/` (the per-operation lines, a tenth of the samples): every line printed (stat, open + close, a 4 KiB block read through usb-storage, cached 4 KiB and 64 KiB reads, a 64 KiB write through to the stick), its scratch file gone afterwards; a usage error (2). QEMU's numbers mean nothing: the PC gives them ([BENCH.md](BENCH.md#method)) | |
| `files.txt` | the file namespace: `/boot` as a read-only mount, `run` with a path, the file commands (mkdir touch write cp mv rm df sync) on a writable mount (the tests' RAM filesystem, `run ramfs shell`), a mount that reaches a running shell, the bootfs server killed and mounted again | |
| `files-fat.txt` | the file commands on a real FAT volume (`run utest fat-shell`: bin/fat over a RAM disk): names with spaces and lower case, big copies, rm -r | |
| `unplug.txt` | the stick pulled while the system runs and plugged back in (the monitor's `device_del` / `device_add`): `/data` and `/esp` go, nothing hangs, they come back in the running shell, logd carries on | |
| `cad.txt` | Ctrl+Alt+Del on a USB keyboard: the compositor (under `nocomp` the console) blanks the screen and asks init, which syncs `/data` and kexecs into the stored kernel, where the file written before is read back | the `tools/usbkeys-test.sh` `QEMU_USB` |
| `data-1.txt`, `data-2.txt`, `data-3.txt` | three boots of one stick: `/esp` and `/data` from the stick itself, a file kept across a reboot, a boot log per boot, the fat service and devmgr killed, the plug pulled | use `tools/data-test.sh` |
| `sticks.txt` | other people's sticks: read-only at `/usb0` and `/usb1`, writes refused, `mount -w` and `mount -r`, what `mount` refuses, a stick pulled while a file on it is read and another in the middle of a copy onto it, sticks with nothing to mount | use `tools/sticks-test.sh` |
| `fun.txt` | the apps (life, tetris, fractal): self-tests, play, screenshots, kill and crash with the screen borrowed | use `tools/fun-test.sh` |
| `apps.txt` | snake, mines and sysmon: self-tests, play, screenshots; one mouse click in mines; the `sysmon` command, and `run sysmon`, given the same by its list | use `tools/apps-test.sh` |
| `jamjar.txt` | jamjar (the music player's window): its cover helper's self-test (`run jamcover --selftest`: stb_image's checks); jamjar's self-test (its helper decodes a PNG, refuses a non-picture, and one that crashes and one that hangs on purpose are killed and replaced; a decode that timed out is tried again once, later); `run jamjar noplayer` without the player; the `jamjar` command: the covers read from the tags (PNG in ID3v2.3 and 2.4, a JPEG, one 611x640, none in a WAV, an oversized and a garbled one refused), an album played by keys, next, back, pause, volume, search, a calibration track (1 kHz on the left, 4 kHz on the right) whose loudest bars must be the left's 34 and the right's 49, and a shot of it (`jamjar-tone`) in which bar 34 stands only up from the line and bar 49 hangs only down, help, roulette, the big view, the sleep timer; mouse clicks on a button, the volume and a track row; the music still playing after q | use `tools/jamjar-test.sh` |
| `jamjar-hd.txt` | jamjar at 2560x1440 by keys, for screenshots at the PC's size (the stereo bars, the big view's sunburst) | use `JAMJAR_HD=1 tools/jamjar-test.sh` |
| `jamjar-covers.txt` | jamjar at 2560x1440 over 32 albums with covers, everything in shuffle, 18 skips (the first while the covers are still read), two shots of now playing after each | use `tools/jamjar-covers-test.sh` |
| `desktop.txt` | the compositor's seat with real USB input: typing in the first terminal, Super+Enter (terminal 2 takes the keys), a click on each window moving the keys, terminal 2's close circle closing it (`pointer` puts the mouse at exact pixels: the geometry is in the script) | use `tools/mouse-test.sh` |
| `mouse.txt` | the mouse through QEMU's monitor, booted with `nocomp`: the shell and tetris undisturbed by it, the wheel's scroll-back, contest's client getting the movement, buttons and wheel, then mines played with clicks at exact cells (reveal, flag, chord, peek, the buttons), and acceleration | use `tools/mouse-test.sh` |
| `ktest-all.txt` | every kernel test from the shell, live, three times in one boot (a test that leaves something behind fails its next run) | |
| `soak.txt` | `soak loops=2` from the shell: two shuffled loops under load with utest between them, and the SOAK RESULTS box | `QEMU_TIMEOUT=600` |
| `soak-plug.txt` | the soak with a second, writable stick and the boot stick pulled and plugged while it runs | use `tools/soak-test.sh` |
| `screen.txt`, `screen-verbose.txt` | the shell's screen on a plain boot (no log, notices, none from processes that only borrow the names init, devmgr and logd: `utest impostor`) and on a `verbose` one | use `tools/screen-test.sh` |
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
| `allow.txt` | programs on `/data`: a copy of bin/soakload refused until `allow`ed (n refuses, y allows), `allow -l`, run, a program can't change `/data/etc`, a changed file refused, a list asking for devmgr or init (a copy of bin/utest) or for `right debug` (a copy of bin/wantdebug) or for the ports below 1024 (a copy of bin/serve, `svc net listen low`) refused, approval or not, `svc net listen` (bin/wantlisten: port 5000 refused on `/svc/net`, taken on `/svc/net-listen`, a TCP listener on port 80 refused there, from `/boot`, and from `/data` once `allow` showed "accepting connections from the network"), `allow -r`, a file off `/data` and a second shell refused | |
| `parse-limits.txt` | the shell's 32-segment limit and unclosed quotes | |
| `terms.txt` | more terminals: `term` opens terminal 2 (its shell's banner copied to COM1), its shell and its console killed and started again, the compositor killed and both consoles connected again, terminals up to the limit of 8 and `term` refusing one more | |
| `terms-windows.txt` | terminals as windows: the first terminal's window, `term`'s window taking the keys, `exit` closing it and the keys back to the first (where `exit` refuses), Super+Enter on a USB keyboard, the compositor killed and both windows back; screenshots | `QEMU_USB="-device usb-kbd,bus=xhci.0,port=2"` |
| `jobs.txt` | programs in the background (`prog &`): the prompt back at once, their output on the screen, the "[n] done" notice and its exit code, one outliving a foreground program, Ctrl+C reaching only the foreground, `jobs`, `kill %n` (and `kill <name>` unchanged), the refusals (a pipeline, a shell command, an alias), tetris in a window of its own (its keys, q), the limit of 8, and background programs ending with their shell | |
| `hda.txt` | the HD Audio driver's dump, `hda`, `kill hda`, `hda jacks`, `hda gain` and `hda bits` set and read back (all through the mixer's query channels) | use `tools/hda-test.sh` |
| `vtdtest.txt` | the IOMMU checks boot (`iommu=on vtdtest`): `iommu`, then `hda` still answering after drv/hda's checks | use `tools/hda-test.sh` |
| `hdastream.txt` | `hdatest`: the HD Audio output stream (open, a pattern played, a running stream closed, the driver killed mid-stream) | use `tools/hda-stream-test.sh` |
| `play.txt` | `play` of six WAV files on `/data` (four played, garbage/cut/float refused, Ctrl+C) | use `tools/play-test.sh` |
| `quality.txt` | `play -s` of known signals on `/data` | use `tools/audio-quality-test.sh` |
| `mp3.txt` | `play` of MP3 files on `/data` (seven played, noise and an ID3 tag with noise refused, Ctrl+C, `play -n`) | use `tools/mp3-test.sh` |
| `beep.txt` | `beep 440 500` and `hda gain` (shown, set, clamped at both ends) | use `tools/beep-test.sh` |
| `mixer.txt` | `vol` (nothing playing, the master set and clamped, a missing stream, usage), then `mixtest` | use `tools/mixer-test.sh` |
| `mixer-restart.txt` | `mixramp 20 300` (a ramp played while init kills the mixer 20 times, other calls made meanwhile), then `vol` | use `tools/mixer-restart-test.sh` |
| `mixer-spare.txt`, `mixer-nospare.txt` | the mixer's warm spare: `ps` with the spare, `kill mixer` once and 12 times in a row, `kill devmgr` (the mixer exits by itself), `vol` after each; then, booted with `nospare`, five kills each starting a new process | use `tools/mixer-spare-test.sh` |
| `fat-restart.txt` | fat carrying on from a dead instance's state (`user/services/fat/adopt.c`), on `/data` through devmgr: files written and never synced, `kill fat-data` right after, read back; an append and two kills; the kernel and the bootfs copied, kills, their checksums; bursts of five kills between appends; `sync`, `reboot -f` | use `tools/fat-restart-test.sh` |
| `fat-spare.txt`, `fat-nospare.txt` | devmgr's warm spare fat: `ps` with the spare; a file written and synced, `kill fat-data`, the file read back, nine times; `kill fat-esp`; ten kills of fat-data in a row; the files all there and logd's log on `/data` still growing; then, booted with `nospare`, five kills each starting a new process | use `tools/fat-spare-test.sh` |
| `storm.txt` | `storm` (the copy under kills): `/usb0` made writable, then a file copied from it to `/data` at 0, 1, 10 and 100 kills a second (fat-usb0 and fat-data in turn), each MATCH; one stopped by Ctrl+C (130); a rate too high (2); `storm mixer 2 10` while the music player plays a WAV from `/data/music`; `music stop`, `sync`, `reboot -f` | use `tools/fat-storm-test.sh` |
| `music.txt` | the music player: usage, errors, `music start` with the shell in use meanwhile (`ls`, `beep`, `vol`, `music status`, `music vol`, `music next`), past one whole shuffle, `music stop`; it plays on through Ctrl+C, `kill shell` and `kill mixer`; `music prev`, `pause` (and `status` paused), the sleep timer set, shown, off, and 33 s that runs out; a second stick pulled mid-song | use `tools/music-test.sh` |
| `fetch.txt` | `fetch`: files saved, a pipe, redirects, hostile servers refused, Ctrl+C | use `tools/fetch-test.sh` |
| `serve.txt` | `serve`: two files in the background (one on port 80), its refusals, its list and `serve stop`, while the Mac fetches; `run nolisten` refused | use `tools/serve-test.sh` |
| `serve-nonet.txt` | `serve` with no network card: "no network address yet" after bin/serve's 5 s wait | use `tools/serve-test.sh` |
| `speed.txt` | `speed`: TCP both ways, UDP, a refused port, `speed -l`, Ctrl+C | use `tools/speed-test.sh` |
| `usb.txt` | the `usb` command | `QEMU_USB="-device usb-hub,bus=xhci.0,port=2 -device usb-kbd,bus=xhci.0,port=2.1"` |
| `usbkeys.txt` | typing on a USB keyboard behind a hub; kill hid, the compositor (hid reconnects to the new one), the console, devmgr | use `tools/usbkeys-test.sh` |
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
| `nettcp_connect` | TCP in-process (the `nettcp_*` tests: `stack.c`'s TCP edge and `tcp.c`'s connections over the same kind of fake edge, the test as the peer, byte by byte with checksums, `user/tests/utest/tcppeer.c`, and as the program on the other side of each connection's rings, [M9.5-PLAN](M9.5-PLAN.md#track-c-part-1-built-tcp-inside-netstack)): netstack connects (SYN with MSS 1460, the handshake's ACK announcing the rx ring's window), 100 KB each way through 16 KiB rings, the program waking netstack only when it sleeps on a ring; netstack's FIN first (the program's END), then the peer's: CLOSED with no error, lwIP keeps the TIME_WAIT; nothing left in lwIP's buffers, segments or heap |
| `nettcp_listen` | a listener: the SYN-ACK announces the accepted rings' window (8 KiB, not lwIP's 64 KiB), bytes with the handshake's ACK wait in the ring until the program accepts; the peer's FIN first, then netstack's, the last ACK frees it (no TIME_WAIT); a backlog of 2 leaves a third SYN unanswered; closing the listener resets the two waiting; a SYN then is refused with a reset |
| `nettcp_slow_reader` | a 4 KiB rx ring: the window closes to 0 when it is full, bytes past it are dropped by lwIP and never held (no receive buffer in use), netstack sleeps on the ring past half full so the program's read of 3000 bytes wakes it and the window opens by 3000 at once; then 64 KiB through it, 700 bytes a read, every byte in the ring and none in lwIP at every step |
| `nettcp_reset` | a reset for our SYN: CLOSED `ERR_NOT_FOUND`; a reset out of the window ignored, then one in it: CLOSED `ERR_PEER_CLOSED`, the bytes before it still in the ring and no END; `ntcp_conn_abort` sends a RST; a connection freed with bytes unread sends one too (after the ACK lwIP was delaying) |
| `nettcp_card_full` | the card's tx ring full (the fake edge refuses, as a full ring does): the segments of a write stay in lwIP and a turn sends nothing; room for one slot sends one, room for all sends the rest at once (`stack_tx_resume`, as netstack does on the driver's room signal), not on lwIP's 250 ms timer; each byte once, in order, and nothing again after the peer's ACK |
| `nettcp_window_scale` | window scaling ([M9.5-PLAN](M9.5-PLAN.md#window-scaling-and-bulk-rings-as-built)): netstack connects to a peer that scales (shift 7): both SYNs carry the option, netstack's SYN lwIP's unscaled 65535, the handshake's ACK the 2 MiB rx ring's window; 100 segments (146000 bytes) from the peer without an ACK all land in the ring, and every ACK's window (shift 6) is the ring's room rounded down to 64 bytes, never more, read bytes given back; with a 256 KiB tx ring and the peer acking each segment, more than 64 KiB in flight from netstack, with a 64 KiB one never more than 64 KiB; a peer that doesn't scale gets 64240 |
| `nettcp_listen_scale` | a listener with 2 MiB rx rings: a SYN that offers scaling gets a SYN-ACK that does (its window field 65535, never scaled), then the scaled window, the peer's bytes past 64 KiB in the ring; a SYN that doesn't, a SYN-ACK without the option and 64240 |
| `nettcp_heap_shares` | lwIP's memory shared out: three connections with 2 MiB tx rings and a peer that never acks (the card taking nothing) get about 4.6 MiB into lwIP between them, two whole send buffers, and leave the heap's last part (1 MiB and 64 KiB, less their own first 64240 bytes); a fourth with a 16 KiB ring still sends all of it, and the last 64 KiB stays free |
| `nettcp_ooseq_sack` | segments past a hole kept, and SACK ([M9.5-PLAN](M9.5-PLAN.md#out-of-order-segments-and-sack-as-built)): netstack's SYN offers SACK; with a peer whose SYN-ACK offers it, segments 2-9 past a hole at 1 are each answered at once with the hole's ACK and one SACK block growing over them, the ring holding only segment 0 and lwIP 8 receive buffers; past the hole a segment with no ACK flag and one acking bytes never sent are dropped before lwIP (`no_acks`, `bad_acks`), not kept; a second hole gives two blocks, newest first; the first hole filled, the ACK jumps past everything kept (nothing sent twice) and the ring has segments 0-9 byte for byte; the second filled, no blocks and no buffer held. A peer that didn't offer SACK: the same queue, ACKs without blocks. A listener's SYN-ACK offers SACK only to a SYN that did |
| `nettcp_ooseq_bounds` | a peer whose hole never fills: one byte at every other sequence number in a 64240-byte window keeps 44 receive buffers (its window in full segments), the rest cut and counted (`ooseq_cut`); filled, the next byte comes in order; a connection let go of gives its buffers back at once and keeps none after. Eight bulk connections (2 MiB rings) flooded past a hole: the first keeps exactly STACK_OOSEQ_MAX - STACK_OOSEQ_KEEP_BULK (768), five more 45 each and one the last 31, so the queues hold STACK_OOSEQ_MAX (1024) together, the last 128 buffers free and no frame ever without one; an ordinary connection meanwhile receives 60000 bytes in order; the first's hole filled, its 768 kept segments come out in order and exact and the ACK says where its queue was cut |
| `nettcp_syn_flood` | 500 SYNs at a listener with a backlog of 4: 4 SYN-ACKs and 4 half-open pcbs, no pcb refused; meanwhile a live connection moves 20 KB each way and another listener accepts and serves a connection |
| `nettcp_pool_full` | every listener 15 half-open connections deep (240), then programs' connects until lwIP's pcb pool is full (TIME_WAITs recycled first): a SYN then gets no pcb and no answer, a program's connect still gets one (lwIP recycles a half-open pcb, never a program's) |
| `nettcp_malformed` | on a live connection: a bad TCP checksum, data offsets 4 and 15, a header cut short, SYN with FIN, an ACK for bytes never sent and one from long ago (both dropped before lwIP, RFC 5961), a sequence number long past and one past the window, bytes with no ACK flag (dropped before lwIP, RFC 9293): no byte reaches the ring, nothing answers but an ACK, the drops counted; SYNs with broken options at a listener; the connection still carries bytes |
| `nettcp_fuzz` | 4000 segments near what a live connection expects, flags at random, bytes flipped (the checksum fixed half the time); every connection a mutation ends replaced by a new one: no crash, and nothing left in lwIP once the address is gone |
| `nettcp_limits` | programs' TCP calls on bin/netstack over the fake driver (as the `netsock_*` tests): `tcp_listener` refused without the listen permission (on `/svc/net` and `/svc/net-sys`) and below port 1024, a bad backlog or ring size refused; 4 listeners an opener, the ordinary openers' backlog stopping at 96, port 0 picked from 49152; `accept` at once (`ERR_SHOULD_WAIT`), timing out, and one at a time (`ERR_BAD_STATE`); port 0, broadcast, loopback and a bad ring size refused to `tcp`; 64 connections an opener, the ordinary openers' 192, a system opener's from the reserve; an opener's end resetting its connections (CLOSED, `ERR_PEER_CLOSED`, kept until closed); every count back to nothing |
| `nettcp_bulk_rings` | a listener with 2 MiB tx rings (`NET_TCP_BULK`) and five connections from the test as the peer on the fake driver: as many as its opener's 8 MiB hold get 2 MiB tx rings, the rest the default 16 KiB one (rx as asked), none reset |
| `nettcp_hostile_ring` | the program writes a tx count far past its ring, an rx count past what was put there, or flags nobody defined: the connection is reset before a byte of it is sent (a RST, CLOSED `ERR_OUT_OF_RANGE`, the ring error counted); bytes after its own END are ignored (its FIN went, nothing follows) |
| `netctl_set_and_clear` | the control channel served by `ctl.c` on a channel of the test's own: `set_ipv4` and `set_dns` show in `info`; 13 addresses a host can't have and 2 bad DNS servers refused, nothing changed; a /30 and a /8 taken; `clear`; a request of the wrong size refused |
| `netctl_process` | bin/netstack started with its control channel at `SR_USER + 0`: set, a refusal, `info` (no device, link down), `stats` (nothing sent or received), `clear`; killed, its job empty |
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
| `netsock_udp` | programs' sockets on `/svc/net` (the `netsock_*` tests run bin/netstack over the same fake driver, with the test as a program that opens its own channels with `svc.connect`, [M9-PLAN](M9-PLAN.md#stage-4-built-sockets-for-programs), on `/svc/net-listen`: they take fixed ports; the datagrams go through each socket's rings, `user/include/sockring.h`): a port taken (`ERR_ALREADY_BOUND`), one below 1024 refused, an ephemeral one from 49152; a datagram in (sender, port, bytes) and out (ports, bytes, the UDP checksum), the biggest (1472 bytes) too; `net_recvfrom` at once (`ERR_SHOULD_WAIT`) and at its timeout; a loop's socket (`net_sock_bind`) whose key fires when the datagram comes; `sock_connect`'s filter and default peer; broadcast, the subnet's broadcast, loopback, multicast, port 0 and 1473 bytes refused (the blocking `net_sendto` returns netstack's reason), nothing sent |
| `netsock_ping` | `echo`: the request's bytes, the reply's round trip, TTL and size; two openers pinging the same peer with the same seq get different ids, and a reply with one's id never answers the other (it times out); a reply after its timeout goes nowhere; an ICMP unreachable answers `ERR_NOT_FOUND`; the same seq twice refused; bad address, timeout and size refused; libos's blocking `net_ping` answered and timed out |
| `netsock_iface` | `iface` (address, gateway, MAC, VLAN 21, 1000 Mb/s, the device and the link); `wait_change` answered by `set_dns`, by its timeout, and at once for an old version; `chip_counts` (netstack asks the driver's `netdev.stats` and answers); `net_wait_up` at once, timing out after `clear`, and answered by `set_ipv4` |
| `netsock_limits` | the limits and the ordinary programs' shares ([ARCHITECTURE](../ARCHITECTURE.md#networking)): 24 ordinary openers (from `/svc/net` or `/svc/net-listen`), the 25th refused on both, 8 system ones (`/svc/net-sys`), then refused; 16 sockets an opener; the ordinary openers' 24 sockets, then refused; the system openers' up to 48 in all, then refused; one closed, one more opened; an opener's end ends its sockets (their status line CLOSED, `ERR_PEER_CLOSED`) and frees its slot; 8 requests in flight an opener, the 9th refused at once; afterwards no opener, socket, request or ring byte left |
| `netsock_hostile` | on an opener's, a socket's and the shared channel: 2 bytes (no answer), 6 bytes, an unknown method, another protocol's, a request a byte short, 8 KiB, a request carrying a handle: each refused; each kind of channel refuses the other's methods (the shared one answers `iface`, not `udp`, `udp_rings` or `sock_rings`); a socket's rings asked for twice, and ring sizes that aren't (1, 2048, 5000, 512 KiB, 4 GiB); closing a socket asleep on its rx ring, and an opener with an echo and a wait in flight, then their answers arriving: nothing breaks, pings still answered, nothing left behind |
| `netsock_slow_reader` | the slow-peer rule: 12 datagrams of 1000 bytes to a socket with a 4 KiB rx ring nobody reads (4 kept, 8 dropped and counted, lwIP's receive buffers not held) and a socket asleep on a silent peer, while another program's datagram is answered at once (under 500 ms) and pings are answered; the ring then read oldest first, `dropped` 8 |
| `netsock_busy_client` | one opener's channel kept full of requests by a thread (written without waiting) while another opener's `iface` is answered in under 500 ms and pings are answered: a busy client holds up nobody else (netstack used to skip its port wait while any channel had more queued) |
| `netsock_len_lies` | libos's `net_sock_take` over a hand-made netstack (a thread answering `sock_rings` for `net_sock_adopt`) whose rx records say more bytes than a datagram holds (1473, 65535): skipped and counted, never read past its datagram; a 1472-byte one between them taken |
| `netsock_shares` | the fair shares: one program takes every ordinary opener, socket and request in flight it can, and ring bytes (2 MiB rings: an opener's 8 MiB, then the ordinary openers' 16 MiB); a system opener (`/svc/net-sys`) still opens sockets with 2 MiB rings, waits, and has datagrams both ways |
| `netsock_hostile_rings` | a program writes garbage into its rings: tx counts far ahead, backwards, off a record's edge, an unknown flag; records too long (1473, 65535) and with flags; its rx ring's count ahead. Each counted in its status line (`ring_errors`, `rx_dropped`), nothing of it sent, another program served within 500 ms and pings answered meanwhile; its VMO handle can't shrink or copy the VMO, and once it closes its socket netstack has shrunk the VMO to 0 bytes |
| `netwait_udp` | real UDP sockets (`<net.h>`, `net_sock_waitable`) in a wait set: writable at once, readable when a datagram comes (netstack signals the flag the set raised), not again once it is taken, both sockets' datagrams reported |
| `netsock_bench` | datagrams through a socket and bin/netstack, the test as the program and as the card: one at a time (a round trip) and 16 in flight (a stream), 64 and 1472 bytes; prints µs a datagram ([BENCH.md](BENCH.md)); fails only if a datagram is lost or wrong |
| `netsock_dhcp` | netctl's `dhcp_open`: one at a time; port 68 refused to a program; a program's socket gets no broadcast and can't send one, and sends nothing with no address; with no address the DHCP socket sends a 300-byte datagram from 0.0.0.0:68 to 255.255.255.255:67 (the broadcast MAC), only to port 67, and hears the server's answer to the broadcast address and to an address it hasn't got yet; closed, another may be opened |
| `sockring_counts` | a socket's rings (`user/include/sockring.h`, [M9.5-PLAN](M9.5-PLAN.md#the-contract-a-sockets-rings)), the counts as pure functions: empty, full, far into the stream; a producer's count backwards or over a ring ahead, a consumer's ahead or over a ring behind; the ring sizes and record sizes |
| `sockring_dgram` | 700 datagrams of every length each way through 4 KiB rings (one VMO mapped as netstack and as the program, a page nobody may touch after it): each as it went in, a full ring refusing the next without writing, records straddling the ring's end; a datagram too long or with flags or reserved bits refused |
| `sockring_stream` | 200 KiB each way in writes and reads of uneven sizes, then the producer's end: every byte, then the end, nothing written after it; bytes a producer adds after its end ignored and counted |
| `sockring_wake` | the flags: a sleeping consumer is signalled, an awake one not; a producer sleeps for room for its next datagram (some room is not enough) and is signalled; the end wakes a sleeping reader |
| `sockring_hostile` | a program's counts out of range (backwards, far ahead, off a record's edge, an unknown flag), each clamped and counted; netstack never reads its own count back; bad record headers (too long, flags, reserved, past what was published): everything published dropped, nothing copied; a header page that isn't the agreed one refused by the program; 3000 turns of garbage in the header page and the rings for each framing: counts only move forward, a ring a look at most, nothing read past the rings |
| `sockring_exchange` | a fake netstack thread echoing the tx ring into the rx ring, the test as the program with the rights netstack hands out (no resize, no duplicate, `to_stack` signal only): 3000 datagrams and 1 MiB of bytes (ending with the end both ways), both sides sleeping for data and for room and woken |
| `netwait_api` | wait sets (`user/include/netwait.h`, [M9.5-PLAN](M9.5-PLAN.md#track-d-as-built-waiting-on-many-sockets)) over 64 fake sockets (`user/tests/utest/fakesock.c`: rings in one VMO, a `to_prog` event and a channel each, the test as netstack and as the program): bad interest bits, `tx_need`, rings missing or netstack's side, the same rings twice, a full set, ids of removed entries refused; a look and a 30 ms wait time out; with 64 entries asleep a signal on one wakes a wait that looks at that one entry only (twice while it stays ready, once more to put it to sleep: 3 looks, not 64 each); a wait sleeping until a helper thread's data or `netwait_wake`; 100 wakes count as one; ready entries win over a wake; ten ready, four a wait: all ten in three waits; a packet of a removed entry ignored, the entry put back reported; 256 entries ready at once and a 257th refused; the job's message bytes back where they were |
| `netwait_states` | a socket's life: connecting (not writable), open (writable, twice), interest changed, a stream's end (readable and `RX_END`, still after every byte is read), closed with `ERR_TIMED_OUT` (HUP and ERROR, twice), closed cleanly with no interest (HUP only), netstack's end of the channel closed (HUP and ERROR `ERR_PEER_CLOSED`, its rings no longer looked at); a socket with nothing to say waits through all of it; plain handles: a channel readable until read, not with no interest, HUP when its peer closes; an event's user bit |
| `netwait_level` | 1500 turns over 64 sockets (datagrams and streams): netstack moves at random (data in, data taken, a few streams ended), the program changes interests and takes entries out and puts them back; after every turn one look must report exactly what the rings say for every socket (computed from the shared counts by the test's own code): data left unread is reported again with no new signal, and a lost wake shows as a socket missing |
| `netwait_stress` | a fake netstack thread sending 160 datagrams or 40 KiB (then the end) to each of 64 sockets and taking 80 datagrams or 20 KiB from each, sleeping now and then, while the test blocks in the set (5 s deadline: a lost wake fails it) and moves everything, every report checked against the rings (never readable when empty, never writable when full), an entry taken out and put back every 50 waits, and netstack dying under one socket half way (reported HUP and ERROR); every byte checked both ways |
| `netlisten_udp` | the listen permission (netstack's `listen.h`, [M9.5-PLAN](M9.5-PLAN.md#track-b-built-sntp-and-the-listen-permission)), over the fake driver: an opener of `/svc/net` gets ports 0 and 49152-65535 and is refused 123, 68, 1024, 5000 and 49151, and a datagram to its picked port arrives; an opener of `/svc/net-listen` gets 1024-49151 too (a datagram nobody asked for arrives on 5000), never below 1024; the listen channel's shared end answers `iface` and refuses `udp`; a port a listener holds is taken for everyone |
| `netlisten_wants` | `svc net listen` in a list: grants `/svc/net` and `/svc/net-listen`, shown "net, accepting connections from the network", no root powers; `svc net listen low` grants `/svc/net` and `/svc/net-low`, shown "..., below port 1024 too"; `svc net-listen`, `svc net-low`, `svc dns listen`, `svc net low`, extra or misspelt words refused |
| `netlisten_low` | the ports below 1024 ([M9.5-PLAN](M9.5-PLAN.md#ports-below-1024-as-built)), over the fake driver: openers of `/svc/net` and `/svc/net-listen` are refused TCP listeners on 80 and 1 and UDP port 53; an opener of `/svc/net-low` listens on 1, 80, 1023, 8080 and 0, binds UDP 53 and 5000, and is refused the DHCP ports 67 and 68 |
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
[M9-PLAN](M9-PLAN.md#r1-receive-on-the-pc)). `net stats` twice, then the
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

## Area scripts

Each prints PASS or FAIL and exits 0 on PASS; `QEMU_SMP` (and where it
matters `QEMU_XHCI`) pass through.

| Script | What |
|---|---|
| `tools/usb-test.sh <outdir>` | the `init` run with a hub, a test keyboard behind it, a CCID device, a mouse and a keyboard; the monitor script answers usbtest's markers (keys, kill hid, unplug, replug, unplug the hub) |
| `tools/usb-early-test.sh <outdir>` | input early in boot (`usb-early.txt`): a plain boot with a hub (a keyboard, a disk and a slow keyboard behind it), a mouse and a slow keyboard on root ports; the slow ones have the serial number `jamos-test-slow`, which usb-bus treats as a device that doesn't answer (its first two attempts on a port each take a second and fail). The console's "input ready: the first keyboard and mouse" line must come under `EARLY_MAX` s (1.5) and before either slow device attached; both slow ports are tried again after 100 and 200 ms and attach on the third attempt; prints the times |
| `tools/bootdisk-test.sh <outdir>` | the boot disk with two Jam OS sticks in (`bootdisk.txt`): a copy of the image with another MBR disk id and a file of its own on its data partition, on qemu-xhci port 2; `/data` must be the boot stick's and the copy's data partition `/usb1`, before and after a `reboot` (kexec), and the kernel must name the boot stick's id both times (the second from the kernel before) |
| `tools/storage-test.sh <outdir>` | the `init` run with two more usb-storage disks behind a hub: usbtest's storage checks (bulk transfers, a STALL and reset recovery, usb-storage taking over a disk left mid-READ, the ESP's boot sector read through `block`, read-only and out-of-range requests refused, a write read back, and the fence: WRITEs a client queued and left behind, while usb-storage was busy with another channel's READs, are dropped and never land, and usb-storage logs `its client left with 4 request(s) queued: dropped`), the second disk read while a READ waits on the slow one (usb-bus runs each device's transfers apart), the monitor script unplugging the second disk while it is being read, and READs timing out on a disk QEMU throttles to 4 KiB/s |
| `tools/usbkeys-test.sh <outdir>` | typing into the shell through usb-bus, hid, the compositor and the console (`usbkeys.txt`): the keyboard's hid, the compositor, the console and devmgr killed in turn, typing after each |
| `tools/fun-test.sh <outdir>` | the apps (`fun.txt`); `FUN_HD=1` runs at the PC's 2560x1440 |
| `tools/apps-test.sh <outdir>` | snake, mines and sysmon (`apps.txt`), with a USB mouse; `APPS_HD=1` runs at 2560x1440 |
| `tools/jamjar-test.sh <outdir>` | jamjar (`jamjar.txt`), with a USB keyboard and mouse and an hda-output capture, on a library the script makes (six made-up stereo songs as MP3 under owner-style UTF-8 names with made-up covers in their tags, drawn with Python's PIL; a calibration track, 1 kHz on the left and 4 kHz on the right; two MP3s with a bad cover); it checks the calibration shot's bars with PIL; `JAMJAR_HD=1` runs `jamjar-hd.txt` at 2560x1440; screenshots `jamjar-*.png` |
| `tools/jamjar-covers-test.sh <outdir>` | jamjar's now-playing cover at 2560x1440 (`jamjar-covers.txt`), on 32 made-up albums of two MP3s, each with a teal PNG cover in its tag: every shot of now playing must show the cover's teal, not a jar label |
| `tools/comp-test.sh <outdir>` | the compositor on QEMU's framebuffer (`comptest`): a screenshot of each of its test scene's steps (opaque floating windows on the wallpaper, a translucent one over them, the arrow on the first's circles; one moved, a tiled window's look, one raised, no arrow; a full-screen window copied straight from its buffer; the desktop: the top bar with three toplevels' chips (one focused, one minimised), the clock's popover, a notification with buttons and the hand on a chip; then the search box with "te" typed) compared by `tools/comp-check.py` with the steps the log describes, painted from scratch with the look's numbers (wallpaper, shadows, title bars, outlines, borders, rounded corners: every third pixel each way; the titles' text found in their bars, each circle's colour or symbol where it is all circle; the arrow where it is all fill; the strip's frosting and islands pixel for pixel, the lit dot and chip, no toplevel under the strip, the popover 2 pixels under it with its right edge on its icon's, the calendar's today, the notification's tile and buttons, the search box's letter tiles, from the layout the test scene logs); prints the compositor's `bench:` lines (frame costs: QEMU's only show the code runs) |
| `tools/mouse-test.sh <outdir>` | two boots: the compositor's seat with a USB keyboard and mouse (`desktop.txt`), and the mouse end to end under `nocomp` (`mouse.txt`): QEMU's monitor moves and clicks a USB mouse, driven in the report protocol (its descriptor has a wheel); `MOUSE_HIDBOOT=1` adds the boot word `hidboot` and checks the boot protocol instead; 1280x800 only (the clicks are at pixel positions) |
| `tools/crash-test.sh <outdir> [name...]` | every crash test from the shell (`crash <name> yes`), each on a fresh boot; each panic starts the stored kernel, whose shell says what happened (`kexecbad`: refused, the panic screen stays up; `kexecstall`, `kexecfault`: a firmware reset) |
| `tools/kdump-test.sh <outdir> [case...]` | a panic starts the stored kernel, each case a fresh boot of a stick image read afterwards with mtools: `save` (`crash panic yes`: no panic screen, the next boot comes up on every CPU, saves `/data/logs/boot-0001-crash.txt` with the panic and the lines before it, logs to `boot-0002` and its shell says so), `loop` (`crashtest=lockorder`: the next boot panics at once, a crash loop, and halts on the red panic screen), `bad` (`crash kexecbad yes`: the damaged stored kernel is refused, red panic screen, nothing saved), `nostick` (the stick pulled first: the shell says the log was not saved and why), `screen` (with the splash: the screen as the next kernel starts is all the splash background) |
| `tools/kexec-reboot-test.sh <outdir> [run...]` | `reboot` by kexec: `kexec` (an unchanged stick: no file read, no firmware reset, the screen all the splash background as the next kernel starts, which brings up every CPU, plays the splash and reaches the shell and `/data`; the old boot's log ends with the reboot's sync), `changed` (the stick swapped for one whose kernel file is longer: both files read and loaded first, the screen blanked meanwhile), `load` (the stick swapped the same way, then `kernel load`: the files read and loaded at once, and the `reboot` after it reads nothing), `broken` (the stick swapped for one whose kernel file is cut short: `kernel load` refuses it and keeps the stored copy, `reboot` says so on the screen and starts the stored copy, no firmware reset), `firmware` (`reboot -f`), `fallback` (`crashkernel=0`: `kernel load` says there is no stored kernel, `reboot` falls back to the firmware) |
| `tools/reboot-firmware-test.sh <outdir> [run...]` | `reboot -f`, the reset through the firmware, which must end QEMU (`-no-reboot`) before the timeout: `data` (`/data` mounted and a file written: init syncs, flushes the log and stops devmgr's drivers; the kernel halts the other CPUs, turns bus mastering off and tries the ACPI reset register, which resets the machine before any other method is tried; a second boot of the stick reads the file, `/data` was left clean, boot-0001.txt has the sync's line), `cf9`, `8042`, `triple` (the boot word `reset=<method>`: that method alone resets the machine). About 20 s |
| `tools/data-test.sh <outdir>` | the stick's filesystems end to end, three boots of one stick image (`data-1.txt` to `data-3.txt`): written, rebooted, read back; QEMU quit in the middle of writes and the dirty volume mounted again; then the boot logs read off the image with mtools, as the Mac reads the real stick |
| `tools/soak-test.sh <outdir>` | the soak test (`soak-plug.txt`): `soak loops=3` at a fixed seed (`SOAK_LOOPS`, `SOAK_SEED`), under the kernel's and `bin/soakload`'s load, with a second stick (made writable) pulled in the middle of writes and plugged back and then the boot stick pulled and plugged back (`SOAK_LOAD`: the kernel load workers, default one per CPU); PASS needs 0 FAILED kernel tests, utest runs and file checks, the job tree's message bytes grown by at most 32 KiB (unread messages piling up), and the second stick's own files unchanged |
| `tools/ktest-keep-test.sh <outdir>` | the test runner's own failure paths, with three tests that exist for it (`ktest=review_ktest`): with `keep` both failures are recorded and the run goes on; without it (and `crashkernel=0`, so the panic screen stays up) the first panics and the panic screen names loop, seed and test |
| `tools/hda-test.sh <outdir>` | the HD Audio driver (`hda.txt`): two emulated controllers (intel-hda with hda-duplex and hda-output, ich9-intel-hda with hda-micro), each codec's graph in the log, `hda` from the shell, `kill hda` and devmgr's restart; the path self-test passes and every codec's path is DAC 02 -> pin 03, set up muted; the jack self-test passes (the ALC897's jack table and tags, the RIRB's demultiplexer on a fake RIRB, the debounce, unsolicited responses and the polling fallback against a fake codec that allows jack code only SET_UNSOLICITED_ENABLE, SET_PIN_SENSE and GET_PIN_SENSE; QEMU's codecs have no presence detection, so the real jack path runs only on the PC); QEMU's codecs trace every verb they get and every one must be a GET or a silent SET (power D0, a connection select, pin control with the output off, an amp mute), and none a jack verb (0x708, 0x709); `hda` opens no stream, so no converter format or stream tag either; `hda jacks` shows the RIRB interrupt taken (it is on for unsolicited responses while the dumps' commands are polled, and the dumps must still see no timeout); then the `init` run with the same devices, where each driver must stop cleanly and the run end "run complete: no problems"; then a shell boot with `QEMU_IOMMU=eim` and `iommu=on vtdtest` (`vtdtest.txt`): each drv/hda's read of a page it unpinned is blocked (QEMU hands the controller a 0, so the codec's answer is not the command's) and the kernel logs a read fault naming that controller at that address, then its response ring is pointed into the interrupt window (QEMU passes old-format writes through: blocked only on the PC), `hda` still answers with no timeout (the rings were put back), and `iommu` shows each controller in a domain of its own with its fault counted |
| `tools/vtd-test.sh <outdir>` | the boot-time VT-d probe ([M11-PLAN](M11-PLAN.md#stage-0-the-read-only-probe)), three `pcilist` boots: with QEMU's `intel-iommu` (interrupt remapping and caching mode on) the DMAR table's lines (the unit, its I/O APIC scope matched with the MADT's, each endpoint scope named by its ids), the unit's registers decoded, translation and interrupt remapping off as the firmware left them, the handover line, nothing in the RESULTS box, and QEMU's trace of the unit's registers: reads, not one write; with `eim=on`: x2APIC ids; without an IOMMU: "no DMAR table". Then two `ktest=vtd iommu=on` boots, caching mode on and off: the unit started and every `vtd_unit_*` test passed (every invalidation kind, the page-table and interrupt-entry callbacks, a refused descriptor reported with the queue going on, the queue wrapping, every CPU submitting at once, the queue turned off and on), interrupt remapping on and every `vtd_irq_*` test passed (COM1's pin remapped, an MSI through its own entry, entries freed on close, another function's, a freed and an out-of-range entry refused and recorded, edu's DMA into the interrupt window blocked, the timer, IPIs and COM1 unaffected, remapping off and on again), translation on (every function in its home domain) and every `vtd_domain_*` test passed (edu blocked with its fault seen, in a domain of its own, a driver's domain taking pins spread one per 2 MiB up to its 2048 table pages, a function whose requester id is shared refused a domain (on a made-up topology, and edu made to look shared), muted after its faults, the tables switched while translating), nothing else in the RESULTS box but the faults those tests provoke; two `ktest=dma iommu=on` boots, caching mode on and off: every `dma_*` test passed, none skipped (edu reaches only what its DMA capability pinned, a killed driver's pages freed once its domain is gone, the quarantine's tests in their translated form, the pin and unpin cost in invalidation waits printed); the same `vtd_domain_*` tests on a unit without pass-through (`pt=off`: nothing depends on it); a boot with a PCIe-to-PCI bridge and a second edu behind it: both named in the RESULTS box as sharing a requester id, and neither given a domain; then two shell boots with `iommu=on` jumping into the stored kernel, by `reboot` and by `crash panic yes`: the jump turns interrupt remapping, translation and the queue off, so both boots find the unit all off, and the next kernel turns interrupt remapping and translation on again. `VTD_TEST_INIT=1` adds the `init` run with the IOMMU present, left off and then started with `iommu=on`. About a minute (two more with the init runs) |
| `tools/hda-stream-test.sh <outdir>` | the HD Audio output stream (`hdastream.txt`): intel-hda with an hda-output codec (`mixer=off`) whose samples go to a WAV file through QEMU's wav backend at 48 kHz 16-bit stereo; `hdatest` passes (the position's rate within 2 %, a closed stream released, a kill mid-stream: restart, the dead driver's pins out of the DMA quarantine unwritten); the WAV holds `hdatest`'s one-second pattern sample for sample after the leading silence, then most of a ring of silence (the driver's clear-behind); the codec got only allow-listed verbs, and the path opened only while the converter has the stream's tag and closed again before it is released (`tools/hda-verbs.awk` follows the state the SETs leave; hdatest turns the gain down while it plays) |
| `tools/beep-test.sh <outdir>` | `beep` (`beep.txt`): intel-hda with an hda-output codec with its mixer on (its DAC amp scales what the WAV gets) through QEMU's wav backend; `beep 440 500` at the default -30 dB: the tone's frequency from its zero crossings within 1 %, its length within 30 ms, the fades (first and last 2.5 ms well under the peak), no clicks, silence after, the peak at QEMU's volume for step 44 within 5 %; the codec's verbs: the path opened only while the converter has the stream's tag and muted again before it is released, the DAC's amp opened at step 44 only; the output stage (pin output, EAPD) on before the first stream and never off, the first unmute at least 400 ms after it went on; the driver's lines in order (stream open, unmuted, muted again, stream closed); `hda gain` set and clamped |
| `tools/play-test.sh <outdir>` | `play` (`play.txt`): WAV files made by the script and copied onto the stick image's `/data` with mtools, played through intel-hda with an hda-output codec (mixer on) into QEMU's wav backend: 48 kHz stereo (440 Hz left, 660 Hz right), 44.1 kHz mono, 22.05 kHz 8-bit, 96 kHz 24-bit WAVE_FORMAT_EXTENSIBLE with `play -v -20`; each sound's frequency per channel within 1 %, its length within 2 %, mono equal on both channels, silence after it, no clicks, the `-v` one's peak a tenth of the others' (its mixer stream at -20 dB) and the others' unchanged; garbage, a cut-off header, 32-bit float and a missing file refused with their reasons; a 10 s file stopped by Ctrl+C after 2 s ends within 3 s with a fade; QEMU runs with the trace event `hda_audio_overrun`, and each codec buffer it dropped on a busy host excuses one click or one sound 2048 frames short |
| `tools/audio-quality-test.sh <outdir>` | sound quality (`quality.txt`): signals made with numpy, copied onto the stick image's `/data`, played with `play -s` through the mixer into intel-hda with an hda-output codec (`mixer=off`) and QEMU's wav backend (16-bit); measured: a 48 kHz stereo file bit-exact at 0 dB; a 1 kHz sine at 0/-20/-40/-60 dB of volume at its level within 0.1 dB, its THD+N and spurs, and (dithered) no harmonic above the noise at -40/-60 dB; 44.1 kHz tones at 1-20 kHz within 0.1 dB with no image or alias above -90 dB; a 20 s tone read from the stick with no dropout and its length within 10 ms; no click anywhere; every `play -s` 0 underruns and 0 late periods, and no "frames late" in the log |
| `tools/mp3-test.sh <outdir>` | `play` of MP3s (`mp3.txt`), as play-test does: tones encoded by ffmpeg (libmp3lame and mp2; the script needs it) and copied onto `/data` with mtools: 44.1 kHz stereo 192 kbps CBR (440/660 Hz), 48 kHz mono VBR without an ID3v2 tag, 22.05 kHz MPEG-2 without a Xing/Info tag, one with ~230 KiB of album art in its ID3v2 tag and an ID3v1 tag, MPEG-1 Layer II, one with 3000 bytes of noise in its middle, one cut off mid-frame; each sound's frequency per channel within 1 % (its middle 80 %), its length (within 2 % where a LAME tag gives the delay and padding), mono equal on both channels, silence after it, no clicks (but where frames are missing); noise and an ID3 tag followed by noise refused; a 10 s file stopped by Ctrl+C with a fade; `play -n` decodes all 441000 frames of it and prints the cost per second of audio; QEMU's codec buffer drops excused as in play-test |
| `tools/splash-test.sh <outdir>` | the boot splash (`splash-comp.txt`, `splash.txt`, `splash-keys.txt`, `splash-off.txt`, `splash-hang.txt`; `tools/splash-check.py` compares with `boot/splash.mpg` decoded by ffmpeg): on the compositor (the default), the screen all `#1E1A1D` as init starts, two frames of the video in its full-screen window, and lines typed while it plays, once it has played and right after it has gone all run by the shell (the splash's window takes no keys); the rest with `nocomp`: a plain boot with an hda-output codec into a WAV: the screen all `#1E1A1D` as init starts, two screenshots a second apart that are frames of the video (2560x1440 scaled down by a 2:1 box onto 1280x800) in order, the sound started together with the picture at 0 ms, the last frame lingering 0.5 s, the console's text after the hand-back, the capture the video's sound from where the splash said it joined to its end, `run splash --selftest`; two lines typed while it plays at 2560x1440 (a frame drawn 1:1, no skip, the whole sound taken, the lines run by the shell once it is up); `verbose` and `nosplash` (the text log, no splash); `shell testpf` (the panic screen over the quiet one); `splashhang` (a splash that never finishes: init gives up on it 20 s after its start, kills it and starts the shell, whose text is on the screen; booted with every option word the kernel hands init, all of which must reach it); prints the time to the first frame and to the shell with and without |
| `tools/clock-test.sh <outdir>` | the date and the settings (`clock-1.txt`, `clock-2.txt`), one stick image, two boots with the RTC at 2026-01-15 01:02:03: Sydney's time from the RTC taken as local time, the settings file made with its defaults, `date -u`/`-r`/`-z`, `vol master`, `music vol` and the zone kept, then `reboot` (kexec): the next boot has the zone and the volumes; then, with mtools, a file written in boot 1 is dated 2026-01-15 1:02, `logs/boot-0001.txt` starts with its date, the settings file holds what was set and no `settings.new`; `rtc = utc` put in from this side: boot 2 reads the RTC as UTC |
| `tools/screen-test.sh <outdir>` | the console's own screen (`nocomp`) on a plain boot (`screen.txt`, booted with the splash, and `screen-verbose.txt`): after the splash the shell's banner and prompt and no kernel log (no pixel of the log's colours) and no RESULTS box; a stick plugged in (one notice: `mount -w` and `mount -r` in between add none) and pulled out, the boot stick pulled out and back, each a yellow notice and a `console: notice:` line in the log; `ktest` puts its lines on the screen while it runs; `run utest impostor` (processes called init, devmgr and logd write lines the real ones make notices of: the log has them, and no notice); `run console selftest` (log lines made into notices or not, by their text and their writer: crashes, kills asked for, give-ups, `/data` full, impostors); then a `verbose` boot whose shell screen has the log; then a plain boot with a stick in from the start (`screen-atboot.txt`): no notice for it, one when it is pulled out |
| `tools/mixer-test.sh <outdir>` | the mixer (`mixer.txt`): intel-hda with an hda-output codec (`mixer=off`) into QEMU's wav backend; `mixtest` passes (the protocol's refusals and clamps; two openers of `/svc/audio` and of `/svc/audioctl` each answered on their own channel; the per-opener stream cap, 4 each and 16 in all; `audioctl.device`'s query channel to the driver answers but refuses `open_output` and `query`, at most 8 at once; tone programs at once; one killed; the master and an `audioctl` volume; the mixer killed and the hda driver killed mid-tone, each played on; a stream left empty lets the mixer close the output and its next write wakes it; two programs on `<audio.h>` at once); the WAV, segment by segment: 440 Hz and 1000 Hz at once with the second at -6 dB (amplitude ratio within 3 %), the killed client's partner with no gap (QEMU's own buffer drops on a busy host, traced with `hda_audio_overrun`, are told apart from a gap), -6 dB master and -12 dB `audioctl` volume heard, the library's 44.1 kHz and 48 kHz tones together; the log's stream, restart and output lines; the codec's verbs: muted whenever nothing plays |
| `tools/mixer-restart-test.sh <outdir>` | the mixer killed while it plays is not seen by its clients (`mixer-restart.txt`, booted once with the warm spare and once with `nospare`), intel-hda with an hda-output codec (`mixer=off`) into QEMU's wav backend: `mixramp` plays a 16-bit ramp at 0 dB while init kills the mixer 20 times and a third thread keeps calling it (volume, position, a stream opened and closed); every call succeeds; one `mixer: restart (killed` line per kill, each with the output still running and the lead left, none falling back to a fresh start; the output opened once; the WAV is the whole ramp, every frame one more than the last (QEMU's own buffer drops, traced with `hda_audio_overrun`, are counted apart). Prints the lead left, kill-to-first-answer (mixramp's and init's) with and without the spare, and what each restart found in progress |
| `tools/mixer-spare-test.sh <outdir>` | init's warm spare mixer and its restart rule (`mixer-spare.txt`, then `mixer-nospare.txt` booted with `nospare`), with an intel-hda card: a spare waits from the boot, each `kill mixer` promotes it and a new one starts, 13 deliberate kills are never given up on, the mixer's own exit when devmgr is killed is restarted at once from the spare; without spares each kill starts a process; prints the spare's memory (`ps`) and the restart and kill-to-first-answer times with and without a spare |
| `tools/fat-restart-test.sh <outdir>` | fat carrying on from a dead instance's state through devmgr's kills (`fat-restart.txt`, booted with the warm spare and again with `nospare`): every instance that got going carried on (its `fat /data: restart (killed` line, never `starting fresh`), the mount never goes, nothing given up on, unsynced writes there after the kill, the copies' checksums the originals'; then on the host (mtools) the stick's files hold what was written and the copy of the kernel is the kernel. Prints kill-to-first-answer (devmgr's probe) and the kill to fat carrying on, with and without the spare. The exact deaths are utest's `fat_restart_steps` (a script of every kind of request, FS_GATHER held across deaths among them, run undisturbed and then with fat ended at a held write, the commit, a block write of the send, before and after the answer, several times a run: the same answers and the two RAM disks the same byte for byte), `fat_restart_handles` (fs.open's and fs.view's replies whose handles died with the process made again) and `fat_restart_bad_request` (a request that crashes fat: ERR_IO after two crashes, fat still serving; two deliberate kills don't count), in the `init` run |
| `tools/fat-spare-test.sh <outdir>` | devmgr's warm spare fat and the restart rule of a filesystem service (`fat-spare.txt`, then `fat-nospare.txt` booted with `nospare`), on the boot stick: a spare waits from the boot, each `kill fat-data` promotes it and a new one starts, the mount never goes (no `is gone` before the reboot), 20 deliberate kills are never given up on, the files written before them are there; without spares each kill starts a process; prints the spare's memory (`ps`) and the restart, kill and kill-to-first-answer times with and without a spare |
| `tools/fat-storm-test.sh <outdir>` | `storm`, the demonstration of M11.6 (`storm.txt`), with a second stick of one file of random bytes (`STORM_MIB`, default 8 MiB; the PC copies 256) and an intel-hda card: every copy at 0, 1, 10 and 100 kills a second says MATCH and its SHA-256 lines are the host's for the file; more kills the higher the rate, none refused, each answered after; Ctrl+C said; no restart started fresh, nothing given up on, no mount gone. Then on the host: each copy on the boot stick's `/data` (mtools) is the file byte for byte, and `tools/fatcheck.py --require-clean` finds both disks clean (no damage, no lost cluster, marked clean). The mixer: one restart line per kill, the least lead left above the 256-frame guard, no late period in storm's line, the log or the music stream's close line. Prints the four storm lines, the mixer's, fatcheck's verdicts and, on a Mac, `fsck_msdos -n`'s on both disks (read-only `hdiutil` attach; not part of PASS: mtools' own volume label makes it warn) |
| `tools/music-test.sh <outdir>` | the music player (`music.txt`): a folder tree made by the script (ffmpeg, mtools) on `/data/music`: six 2 s tones (MP3 at 44.1, 48 VBR and 22.05 kHz, WAV at 48 and 44.1 kHz) under names with spaces, apostrophes, `$`, `~`, parentheses and UTF-8 (`JAŸ-Z`), an upper-case `.WAV` and `.Mp3`, a garbage `.mp3`, `.DS_Store`/`._` dotfiles, a text file and an empty folder tree; and a second stick of three 8 s WAVs. In the capture (100 ms FFT windows up to a 1000 Hz marker beep typed 2 s after `music stop`): the tracks heard are the log's `track N:` lines in order, the first six are the six tracks once each, at least seven heard, never the same twice in a row; the 1500 Hz `beep` mixed over a track; the stop fades. The log: every title (`Artist - Title` from the path), the garbage file skipped, no dotfile tried, `kill mixer` not seen by the player (the mixer's restart adopts its stream), the pulled stick stopping it after three unreadable files |
| `tools/net-test.sh <outdir> [vlan vlan-off rx]` | the e1000e driver against `nettest`, a hostile netstack ([M9-PLAN](M9-PLAN.md#stage-2-built-drve1000e-and-nettest)), one `init` boot per scenario from a copy of the stick whose bootfs runs `bin/nettest <mode>` from init.cfg (in shell mode netstack holds the card's one session), with `QEMU_NET` and a peer of the script's own (`tools/netpeer.py`'s `Peer` plus nettest's frames; it also fails on a tag inside VLAN 21's): `vlan` (the session rules; every bad length, flags, frames already tagged 0x8100, 0x88a8 and 0x9100, `produced` a ring and one ahead and then behind, a thread rewriting EtherTypes while the driver copies: each refusal counted exactly, and the peer and the pcap each hold exactly the frames the driver queued and the chip sent, all tagged 21 once), `vlan-off` (a `vlan=off` boot: the driver's "no VLAN" line, no service, no frame at all), `rx` (the peer's census of untagged, VLAN 0, other-VLAN, QinQ, nested, 1522-byte and VLAN 21 frames: only the VLAN 21 ones arrive, untagged and whole, every drop counter exact; then 300 frames with the ring unread: 256 given, 44 `rx_ring_full`); every run ends with the driver stopped cleanly and every job empty |
| `tools/net-vlan-test.sh <outdir> [21\|none]` | the network rule over every path that transmits ([ARCHITECTURE](../ARCHITECTURE.md#networking)); with `none`, one boot in the untagged mode (`net-untagged`: the same script with `net` saying untagged, the peer and the pcap with `--vlan none`: every frame untagged, any tag fails); otherwise two boots with QEMU's e1000e and the peer (`--netlog`, `--update` serving this build, `--ntp` with the Mac's time, `--ping 10.2.21.100`, `--noise 5`, and its TCP relay: `--tcp-relay` 8000 to `tools/httptest.py` and 5201 to `tools/speed.py server`, `--tcp-forward` a Mac port to the guest's 8080), only `net.host = 10.2.21.174` and a 64 KiB `served.bin` on the stick: `net-vlan` (`net-vlan.txt`: the DHCP lease, netlog, `host one.one.one.one`, `ping mac.jam`, `ping 1.1.1.1`, `update -n` (checked by init, not loaded), `fetch` of `small.txt` into a pipe, `serve /data/served.bin` fetched by curl on the Mac (its bytes must be the file's), a 1 s `speed` sending, sntp setting the clock from the gateway, netstack answering ARP and the peer's pings; the peer's and the pcap's checks, then the pcap's frames counted per path: ARP, DHCP, DNS, echo requests, echo replies, netlog, update, SNTP, fetch, serve and speed must each have sent at least one, none may be from another path, and the peer must have seen exactly the pcap's frames) and `net-vlan-off` (`net-vlan-off.txt`: a `vlan=off` boot with the same commands: not one frame (the peer's `--expect-none` and the pcap's), the driver's "no VLAN" line and nothing touched, `net` with no card, `host` and `ping` with no DNS server or route, `update` with no address, `fetch`, `serve` and `speed` giving up on an address that never comes, no lease, no netlog file). A mode that is the build's default (build/build.txt's `net` line) boots with no `vlan=` word and the kernel must say `(the build's default)`; the other gets the word: run both modes to cover both. About 1 minute each (QEMU, 2 CPUs, 2026-10-02: about 43000 frames in the VLAN run, 35000 of them `speed`'s) |
| `tools/sntp-test.sh <outdir>` | bin/sntp end to end, three boots with QEMU's e1000e, the RTC at 2026-01-15 and the peer's SNTP server (`--ntp`: 2031-02-27 from its start): `sntp` (`sntp.txt`: the gateway asked, each answer after a forged one (`--ntp-forge`: the origin a bit off, a year later) that is said to be ignored, the five-year step confirmed by a second reply, `date -u` 2031, `date -r` "set from the network", `date -z` keeps it, sntp restarted moves it under a second, devmgr restarted and init keeps the network's time); checked afterwards: the kernel's set near the peer's time, never the forged year; `sntp-name` (`ntp.server = router.jam`, through `/svc/dns`); `sntp-off` (`ntp = off`: no sntp, not one SNTP request, the RTC's date). About 3 minutes |
| `tools/ping-test.sh <outdir>` | the shell's `net` and `ping` (`net.txt`) with QEMU's e1000e and the network peer: `ping 1.1.1.1` answered through the gateway, Ctrl+C, `net stats` ([netstack](#netstack)) |
| `tools/rxsoak-test.sh <outdir>` | the receive path over a long run (`rxsoak.txt`, about 4 minutes): the peer floods ~10 000 frames (a trunk port's mix, on VLAN 21 and off it) while it pings every 0.2 s; the late pings must be answered and the guest's own `ping 10.2.21.1` too ([netstack](#netstack)) |
| `tools/dns-test.sh <outdir>` | bin/dhcp and bin/dns end to end with QEMU's e1000e and the peer's DHCP and DNS servers, two boots ([DHCP and DNS](#dhcp-and-dns-end-to-end)) |
| `tools/tcp-test.sh <outdir>` | TCP end to end (`tcp.txt`, [M9.5-PLAN](M9.5-PLAN.md#track-c-part-2-built-tcp-for-programs)): with `net.address` in the stick's settings and the peer's TCP side, `run tcptest send 10.2.21.174 5030 10485760` (10 MiB to the peer and 10 MiB back on one connection, every byte checked on both sides, the SHA-256 said, then CLOSED with no error; the speeds both ways) `run tcptest send 10.2.21.174 5032 8388608 2097152` (8 MiB each way with 2 MiB rings, to a server whose ACKs come 20 ms after their segments: window scaling), and `run tcptest serve 5031 20 262144` (the peer's 20 connections at once to a guest listener, served from one wait set holding the listener and every connection, 256 KiB each way each); PASS needs the peer's 22 connections right and none wrong, on the 2 MiB rings' connection a scaled window over 64 KiB and more than 64 KiB in flight both ways (the peer's `tcp_5032_*` counts), on the default rings' one no window and no flight over 64 KiB, and the run's VLAN checks. `QEMU_NET_VLAN=none` runs it untagged. About 2 minutes |
| `tools/fetch-test.sh <outdir>` | `fetch` end to end (`fetch.txt`, [M9.5-PLAN](M9.5-PLAN.md#track-e-as-built-fetch-serve-and-speed)): with `net.address` in the stick's settings, `tools/httptest.py` on the Mac and the peer's `--tcp-relay 8000:<its port>`, fetch saves a 6 MiB file by Content-Length, the same chunked, a body ended by the connection, a file by name (`mac.jam`) and one at the end of three redirects (301, a relative 302, an absolute 307), and writes one into a pipe (`| wc -c`); it refuses four redirects, https typed and redirected to, a head over 16 KiB (one giant line, and lines that never end), a head dripping a byte a second (given up at 20 s), a body cut short, a broken chunk size and a 404, each with exit 1 and nothing left on `/data`; Ctrl+C stops it (130). Afterwards, with mtools, the saved files' SHA-256 must be the server's and no `x`, `x.part`, `y` or `y.part` may be on the stick. About 2 minutes |
| `tools/serve-test.sh <outdir>` | `serve` end to end (`serve.txt`): a 5 MiB file and a page copied onto `/data`, served on 8080 and 80 (below 1024: bin/serve's `svc net listen low`) in the background while the shell goes on; `tools/servecheck.py` on the Mac fetches through the peer's `--tcp-forward` (two Mac ports to the guest's two) with a silent connection held open all along: two curls at once (two paths, the same file; their SHA-256), `curl -I`, a range, the page (text/html), a request that isn't HTTP (400) and a head over 8 KiB (431), then nothing served after `serve stop`; the shell's side refuses a port served already, a folder and port 0, the request lines and `serve`'s list are checked, and `run nolisten` (a program whose list has `svc net` without `listen`) must be refused listeners on 8081 and 80 (ERR_ACCESS_DENIED, neither `/svc/net-listen` nor `/svc/net-low`); then a boot with no network card (`serve-nonet.txt`): `serve` says "no network address yet". About 2 minutes |
| `tools/speed-test.sh <outdir>` | the throughput tester (`speed.txt`): `tools/speed.py server` on the Mac behind the peer's `--tcp-relay` and `--udp-relay 5201`, and a `--tcp-forward` to the guest's 5202: `speed 10.2.21.174 -t 3` (TCP out), `speed mac.jam -r -t 3` (TCP in), `speed 10.2.21.174 -u -t 2` (UDP out, the losses counted), a port nobody serves (refused: the relay resets a SYN to a port it doesn't relay, as a closed port does), then `speed -l 5202` with `speed.py client` sending and receiving, and sending again through a second forward that drops `SPEED_LOSS` percent (default 2) of its data segments to the guest: the relay must see SACK blocks and resend at most 3 times the bytes it dropped (the guest keeps what comes past a hole: about 1.0 times; without that, 38 times); stopped with Ctrl+C (exit 130), and a 30 s send stopped after 2 s the same way; both sides' lines printed, and the relay's counts for the lossy run. QEMU's numbers, through a relay in Python (2026-10-02, 2 CPUs: TCP out 58, in 70, UDP 109 MB/s; the Mac's client 69 out, 47 in; 2026-10-04, 2 % lost: 40.9 MB/s in, 11.8 before segments past a hole were kept), not the PC's. About 1 minute |
| `tools/netlog-test.sh <outdir>` | netlog end to end (`netlog.txt`): the whole log of two boots and the panicked one's, from the first line, with the receiver late and paused, netlog and netstack killed ([netstack](#netstack)) |
| `tools/netprobe-test.sh <outdir>` | the RTL8125 driver's boot words in QEMU, which has no RTL8125 (the probe and the send test themselves run on the PC only): a `shell netprobe` boot (`netprobe.txt`), a `shell netsend` boot (`netsend.txt`) and a `shell net` boot (`netserve.txt`) where devmgr says it has the word and binds and runs nothing for it, and again after a `reboot` (kexec keeps `net`), then a plain `shell` boot whose log mentions none of them. utest covers the rest: `netframe_classify` and `netframe_short_frames` (`drivers/include/jam/netframe.h` over hand-made frames: untagged, tagged 21, another VLAN, 4095, priority-tagged, QinQ 0x88a8 and 0x9100, every short length); `netframe_tag` (every length 0-1600: 14-1514 tagged, short ones padded with zeros over a dirty buffer, the rest refused; VLANs 1 and 4094 yes, 0, 4095 and wider no; a buffer one byte short), `netframe_tag_refuses_tagged` (EtherType 0x8100, 0x88a8 or 0x9100 at any length never leaves, and nothing sendable stays behind), `netframe_tx_check` (lengths 18 and 1518, every change to bytes 12-17: another TPID, a priority, DEI, another VLAN, a tag inside), `netframe_tag_copy_is_the_frame` (the caller rewriting its frame after the copy changes nothing; a change to the copy is refused), `netframe_rx` (VLAN 21 kept at any priority and untagged correctly; untagged, VLAN 0, other VLANs, outer tags, a tag inside ours, every short length and over 1518 dropped, each by its reason); `rtl8125_write_guard` (the transmit registers refused in every width and overlap, the command register's transmit bit, TDFNR, nothing else), `rtl8125_txq_guard` (the other queues' ring addresses and the tail and close pointers refused in every width and overlap; through the OCP windows, which take whole 32-bit writes only, a 16-byte descriptor format, a second transmit queue, pause advertised or a PHY test mode refused, every value the bring-up writes allowed), `rtl8125_tx_gate` (full mode with a VLAN only), `rtl8125_args` (the modes, `netprobe` winning over `netsend`, hostile `vlan=` and `arpto=` words), `rtl8125_arp` (the probe's exact bytes, tagged and checked like any frame; the reply and its near misses), `rtl8125_stays_off` (the driver without a VLAN, and the probe, the send test or the netdev service without hardware, ends at once with exit 0 and an empty job); `rtl8125_txdesc` (`drivers/rtl8125/txdesc.h`: the 32-byte descriptor, the transmitter refused unless the chip's format bit agrees, the end-of-ring bit on the last descriptor only, a chip walking the ring in 32-byte steps finds every descriptor in order), `rtl8125_kick` (a stuck descriptor gets a handful of extra doorbells in 4 s however often the driver looks, never one within 1 ms of its own), `rtl8125_tx_verdict` (the chip's tally against queued and handed back: fewer sent is progress, only more sent is foreign); `rtl8125_guard` (`drivers/rtl8125/guard.h`, failing closed: frames in flight carry on, one frame more than queued, PAUSE TX with the link up, a tally gone backwards or 10 looks without one stop the chip; a 100000-step healthy run never trips, and comparing at the ask instead of the look would), `rtl8125_dump` (the tally dump's bookkeeping: one out at a time, once a second or 1 ms after a reap or a request, given up on after 10 ms), `rtl8125_guard_fake` (`guard.c`, `regs.c`, `chip.c` and `tx.c` linked in over a fake chip whose registers and DMA memory are the test's: each stop turns the transmitter off and resets the chip, in full mode and in the probe), `rtl8125_stats_nowait` (`netdev.stats` and the "so far" lines answer from the last dump that landed: 1280 requests against a chip that never dumps take no wait, where each used to cost 10 ms); `netserver_session`, `netserver_tx`, `netserver_rx` (the network drivers' netdev server, `drivers/lib/netserver.c`, linked in, over a fake card with the test as netstack: info and stats, one session at a time, open refused on the session channel, a closed or orphaned session replaced, the rights open hands out; netstack's frames reach the card byte for byte, bad lengths, flags and tags counted and never sent, a card out of descriptors holds the ring until they come back, hostile counts bounded to a ring per turn; received frames into the rx ring with NETDEV_SIG_RX, a full ring and no session dropped and counted, NETDEV_SIG_LINK) |
| `tools/update-test.sh <outdir>` | `update`'s check in init without a network (`bin/updtest` hands init the build from files on `/data/update/`, put there with mtools: this build's kernel, its boot image with one more file `update-marker.txt`, and their manifest from `tools/update-server.py --manifest`, signed with a throwaway key whose public half the stick's build has (`tools/update-test-key.sh`, keys in `<outdir>/testkey`, never `~/.config`), once more with a second key, once saying the other network default (`untagged` for a VLAN build, `vlan21` for an untagged one), and a signed manifest for two files that are no kernel), one run, three boots: `run updtest bad` (20 damaged offers, each refused for its own reason: a kernel or boot-image byte changed, the kernel 4 KiB longer or cut to half, a VMO shorter than its length, a garbage, cut or short-signature manifest, a format-3 one and a signed one with a must-understand line (`!future-must`: "needs a newer build"; the same changed after signing: its signature), an unsigned one, one changed after signing, one signed by the second key, one carrying another manifest's signature, a bad magic, one handle, an unknown flag, the build with the other network default, two files that match their signed manifest but are no kernel; the build's manifest with an extension line no build knows, signed, check only: taken; and the good build, and the other-network one forced (`update -f`), offered check-only: accepted, not loaded), then `reboot`: no marker (the stored kernel untouched); `run updtest good` (accepted and stored), then `reboot` reads nothing from `/esp` and the marker is there (the fetched build runs); exactly one `kexec_load` succeeded. Then a second run on a build without a key: `update`, `update -w` and `update -m` fetch nothing and say why, `update -n -r` is a usage error, and `run updtest nokey` (the build offered plain, check-only and to be written to the stick) is refused for that alone. utest covers the parsers, the signature and the fetcher: `update_manifest*` (the format, each line wrong each way, the `net` line's values, the `menu` line's, every prefix, every byte changed, random bytes), `bootmenu_*` (the boot menu's check, `<bootmenu.h>`: a menu shaped like `boot/limine.conf` passes, each rule broken refused at its line, a missing file named, random damage), `update_build_net` (build.txt's `net` line), `update_signature` (RFC 8032's tests 1-3, a manifest `jamos-sign` signed, every byte it covers changed, another key, a bent signature, none; the key file's format), `updwire_*` (golden bytes from the Python tool, every truncation and field, random datagrams), `updfetch_*` (the window against a fake server on a scripted clock: clean, lossy with repeats, reordering and forged replies, the snapshot gone once and always, no answer, a bad or oversize manifest, a lying file size, a boot menu fetched as a third file and never asked for without its line) |
| `tools/update-net-test.sh <outdir>` | `update` over the network (`updnet` run, QEMU_NET with the peer's `--update`; `make -s image` first): build B is this build's kernel with its version string's last character changed and its boot image with `build.txt` saying `git b0b0b0b` (and this build's `net` line) and one more file, `update-marker.txt`; `net.address` and `net.host = 10.2.21.174` in the stick's settings; A and B have a test key (`tools/update-test-key.sh`) and the server signs with it. Each `update` is a new client of `tools/update-server.py`'s `PlannedServer`, which serves the next plan: `damage` (a kernel byte changed after the manifest: init refuses the SHA-256), `wronghash` (the manifest's boot image SHA-256 wrong: refused), `truncated` (the boot image half the manifest's size: the fetch fails), `gone` (the server stops answering after 300 replies: the fetch fails), `badsig` (the manifest changed after it was signed: init refuses the signature), `othernet` twice (the signed manifest says the other network default: `update` refused by init, then `update -f -n` taken, checked and not loaded), `mustknow` (the signed manifest has a must-understand line, `!future-must`: init refuses it, "needs a newer build"), `extension` (an extension line no build knows, signed, with `update -n`: skipped, B checked), then `update -n` (B fetched and checked, `old -> new` said, nothing loaded), `update -m` (loaded into memory only: no reboot, `version` still A's, `sha256sum /esp/boot/jamos.elf` still A's kernel) and `update` (loaded and written to the stick, and no reboot: `version` still A's, the stick's kernel B's); then `reboot` (kexec, nothing read from `/esp`): `version` is B's and the marker is there; `reboot -f` ends the run. Exactly two `kexec_load`s, every frame tagged VLAN 21 (the peer's and the pcap's checks; the run boots with `vlan=21`). Then `updcold`, a cold boot of the stick it left (what `reboot -f` starts): B, with the marker. About 2 minutes |
| `tools/update-write-test.sh <outdir>` | `update`'s stick write (`make -s image` first): the stick's build A has a test key (`tools/update-test-key.sh`), build B is made as `tools/update-net-test.sh` makes it, signed, and build C the same with another version and git c0c0c0c. QEMU runs, each a cold boot of the stick image the last one left (`QEMU_SAVE`, `QEMU_IMAGE`): `wfail` (`run updtest writefail`: B to be written with a test's failure at each step, making room, once the older previous build is removed, half way through the new kernel, between the two renames of the new build: each answered not written with the stick booting A, renamed back after the swap began (the second failure removes the previous build; the fourth write copies A as it first); then `reboot -f`; on the Mac, with mtools: A under both names, no `.new` or `.old` file), `wstop` (on a copy of that stick: B and C written in turn, `run updtest writestop <n> b|c`, each stopped dead after change n of the swap's eight changes of names, `UPDATE_OFFER_STOP`, as a power cut there, and after each `run updtest espcheck`: one of the two entries holds a whole build, A, B or C, read and hashed from `/esp`; then a write of C with no stop: on the Mac C as the stick's build, B as the previous one, no `.new` or `.old`; `wstopcold` and `wstopprev` boot that stick's two entries: C and B), `wnet` (a cold boot runs A; `update -m` over QEMU_NET, the peer serving B: loaded into memory only; `reboot` (kexec) runs B, `version` and the marker are B's, the stick's kernel still A's; then `update -w`, the old spelling of plain `update`, with B running: written, no reboot (one kexec in the run); on the Mac: B as the stick's build, A as the previous one, no `.new` file; every frame tagged 21), `wcold` (a cold boot runs B) and `wprev` (`QEMU_BOOT_PREV=1`, the boot menu's "Jam OS (previous build)": A). A keyless build's `update -w` is `tools/update-test.sh`'s. About 8 minutes |
| `tools/update-menu-test.sh <outdir>` | `update`'s stick write and the boot menu (`make -s image` first; `user/services/init/espmenu.c`): the stick's build A has a test key and an old menu (`boot/limine.conf` without its IOMMU entries); build B is made as `tools/update-write-test.sh` makes it; the new menu is `boot/limine.conf`, a bad one the same with "Jam OS (previous build)" booting `/boot/jamos.elf`; manifests for B signed with the test key, without a menu and with each. Every run keeps the stick's menu (`QEMU_MENU_AT=efi`). `menus` (`run updtest menubad`: a menu byte changed refuses the update, its SHA-256, and so do a menu longer than its manifest says, its VMO missing, a menu with a manifest that names none, a menu length with none; check-only and RAM-only offers with the menu are taken and leave the menu alone; then `run updtest menuwrite` of B with the bad menu: the build written, the menu refused by the check; with no menu line: the menu untouched; with the new menu: written, the old as `limine.conf.prev`; again: "the stick has this boot menu already"; `run updtest menucheck` between them says which whole menu Limine reads; on the Mac the new menu, the old as `.prev`, no `.new`, no spare), `stops` (the menu's swap stopped dead after each of its six changes, `UPDATE_OFFER_STOP` 9 to 14, as a power cut, each followed by `menucheck`: always a whole menu, from the spare `/esp/boot/limine.conf` after 12; the next write undoes a cut swap), `cut` (a stop after change 12 and a firmware reset: no `/esp/boot/limine/limine.conf`, the spare is the stick's menu; that stick, with no menu of the test's, boots B from the spare's default entry), `settle` (the next write undoes it and writes its menu), `net` (`update -n`, then `update -w` with the menu damaged on the way: "a file's SHA-256 isn't the manifest's: menu"; plain `update` from a server without menus: the build alone, no reboot, then `reboot`; `update -w` with the menu: written, "this is the build running now, and the stick has it already", then `reboot`; every frame tagged 21) and `entries` (that stick booted by Limine's own pick, then each of its menu's 16 entries that boot, through a copy of the menu with `default_entry` at `/esp/EFI/BOOT/limine.conf`: each starts its kernel, B or A for the previous build, with its command line; each boot stopped at the kernel's `loader:` line). `MENU_RUNS` picks runs. About 15 minutes |
| `tools/update-pc-test.sh <outdir>` | `update`'s stick write as on the PC (`make -s image` first): a stick that writes like a real USB 2 one (`QEMU_STICK_THROTTLE`) and a second stick at `/usb0` (MBR, one FAT32 partition of type 0b, like the owner's SanDisk; `PC_SANDISK=0` leaves it out); the peer serves build B, made and signed as `tools/update-write-test.sh` makes it. Four runs (`PC_RUNS` picks): `fast` (100 write commands a second and 10 MB/s, `PC_THROTTLE` overrides it, `""` for none: plain `update` answers "written" within `PC_WAIT` seconds (150) and doesn't reboot; on the Mac the stick holds B, A as the previous build, no `.new` or `.old` file, and the served boot menu, the stick's own kept as `limine.conf.prev`: every run keeps the stick's menu, `QEMU_MENU_AT=efi`), `again` (that stick boots B; `update -w` of B writes nothing: "the stick has this build already", "... this boot menu already", and the shell says B is the running build and the stick has it), `stuck` (3 write commands a second: the write stops at its 120 s limit, `update` answers "not written" with B loaded all the same, `/esp` is back, `reboot` (kexec) runs B from memory, the stick still boots A with no `.new` or `.old` file) and `reboot` (Ctrl+C during the write, then `reboot`: kexec within 60 s into B, the stick still boots A). init's progress lines (`init: update: write: ...`) are printed. Before the fix of 2026-10-02 the `fast` write took 445 s (one WRITE command per 512-byte cluster) and `update` gave up after 300 s; now about 6 s. All four about 9 minutes, `fast` alone about 2 |
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
[M11.5-PLAN.md](M11.5-PLAN.md)) has tests at three levels, all in the
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

## Services that outlive their process

fat and the mixer carry on from a dead instance's state
([ARCHITECTURE.md](../ARCHITECTURE.md#services-that-outlive-their-process),
[M11.6-PLAN.md](M11.6-PLAN.md)). The tests, cheapest first:

- **The pieces, in the `init` run** (utest): `keep_*` (the keep channel:
  put, drop, restore in 64-handle batches with the same objects and the
  same rights, slots the state doesn't know, the keeper's refusals of
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
  `disk_fs_restart` (devmgr's kept `fs` channel: the mount
  never goes), and the exact deaths: `fat_restart_steps` (a script of
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

**On the PC** (after a flash; the plan's
[demonstration](M11.6-PLAN.md#the-demonstration-and-its-tests)): on the
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

## The IOMMU

The IOMMU ([ARCHITECTURE.md](../ARCHITECTURE.md#the-iommu),
[M11-PLAN.md](M11-PLAN.md)) is off unless a boot has `iommu=on`, so a
plain test run never uses it. It has tests at three levels:

- **Kernel tests**, in `tools/vtd-test.sh` (the table under
  [Area scripts](#area-scripts) says what each boot checks): `vtd_unit_*`
  (the queue and every invalidation), `vtd_irq_*` (interrupt remapping),
  `vtd_domain_*` (domains, edu blocked and let through, a driver's table
  cap, the mute, the handover while translating), `dma_iommu_*` and the
  `dma_*` tests in their translated form, with QEMU's caching mode on and
  off and on a unit without pass-through; and a `reboot` and a panic
  jumping into the next kernel with everything on. The pure ones (`vtd_pt_*`, `vtd_ir_*`, the
  entry bit layouts, the DMAR parser) run in every `ktest`.
- **The deliberate faults**: `tools/hda-test.sh`'s third boot runs drv/hda's
  IOMMU checks (`iommu=on vtdtest`, the "Tests > IOMMU checks" entry on
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
"Tests > IOMMU checks" entry, then the "Jam OS (IOMMU)" entry with every
device in use, `iommu` (no fault but the checks'), `bench`'s IOMMU lines,
a `reboot` and `reboot -f`, then All tests and `soak 10` booted with
`iommu=on` (added with E in the boot menu). Only after that does
`iommu=on` become the default.

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
`desktop.txt` (`tools/mouse-test.sh`) and `screen-test.sh` (`nocomp`, the
bitmap) take screenshots of the windows and the full screen.

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
[G1-PLAN](G1-PLAN.md#questions-for-the-owner), "The look"). In utest
(the `init` run), its logic is linked in on the window manager's harness
(`user/tests/utest/compwm.h`: the seat played by the test, fake
toplevels), the desktop on, no animations unless a test turns them on, the
clock fixed (`user/tests/utest/compdesk.c`, `compdesk2.c`):

| Test | What it checks |
|---|---|
| `desk_screens` | one screen at the start; Super+Right past the last makes one and an empty one left behind goes; Super+1..9; each screen's own arrangement (Super+T on the current one, new screens take it); Super+Shift+Right/Left move the focused window, making a screen and dropping the one left empty |
| `desk_fullscreen` | full screen on a screen of its own right of its home, no strip, the whole output; back home, its screen gone; closed while full screen; a new window while a full-screen screen is current opens on the normal one left of it |
| `desk_minimise` | hidden, its chip dimmed, the focus moved on; its screen stays; Alt+Tab's way and a chip bring it back focused (from another screen too); a chip click minimises the focused window; in tiling the others take its tile |
| `desk_room` | no window under the strip and its 6-pixel gap: first places, the cascade, a drag up, a maximise, every tile, tiling's maximise; full screen has all of the output; with the desktop off all of it is the windows' |
| `desk_strip` | the islands' places and items ("Jam OS", dots, "+", chips, icons, the clock "Mon 5 Oct  14:32"), the focused chip lit; each item's click |
| `desk_cursors` | the cursor set's pictures at known pixels and hot spots, busy's arc turning; the resize arrows on each edge and corner, the hand on circles, islands and rows; busy from a launch until the app's first window or 10 s |
| `desk_anim` | open, minimise, restore and close end exactly where the change ends (mapped or not, drawn as itself, the snapshot freed); a key mid-way jumps to the end; a frame damages only its boxes; a slide offsets both screens' windows and ends at 0 with the old screen's unmapped |
| `desk_alttab` | the order (this screen's windows most recently focused first, other screens grouped, minimised last); Tab, Shift+Tab, wrapping; letting go of Alt goes there (its screen, restored); Esc cancels; the list shows after 120 ms |
| `desk_search` | Super tapped alone (not with another key or a click) toggles it; typing filters, the run row; arrows, Backspace; Enter runs the app (`ctl_launch`) or the command (`ctl_run_in_terminal`); a click on a row; its place |
| `desk_popover` | each popover 2 pixels under the strip, its right edge on its icon's, one at a time, closed by its icon, Esc or a click elsewhere; the volume slider by a click and a drag; the calendar Monday first (leap years) |
| `desk_notify` | cards stacked from under the strip, the newest on top; the plain one goes after 5 s, the one with buttons stays until a button (`ctl_notify_answered`); a click sends a plain card; at most 5 |

With the real seat (`user/tests/utest/compwmseat.c`, bin/compositor
headless), `wm_seat_cursors` checks the cursor drawn over a window's edges,
corner and circles, and pixel for pixel over its surface, and a client
asking for the text bar and the hand through wp-cursor-shape-v1 (a stale
serial ignored, a shape there isn't an error). The other compositor tests
that start it headless pass `nodesk` (`comptest.h`): the window manager
alone, the whole output the windows'.

## The other tools

The rest of `tools/` builds, checks and flashes; the tests above use some
of them. Each file's header says more.

| Tool | What |
|---|---|
| `tools/serial-feed.py` | types a shell script into QEMU's serial port (`QEMU_INPUT`) |
| `tools/mkstick.py` | disk images standing in for other people's sticks (`tools/sticks-test.sh`, `tools/soak-test.sh`) |
| `tools/fatcheck.py <image>[@@offset] [-p N] [--require-clean]` | a read-only FAT32 checker for an image or a stick's raw device (the kill storm's, on the Mac): the FATs agree, every chain is valid (in range, no loop, no cross-link, never into a free, reserved or bad cluster), directory entries are sane (short names, `.` and `..`, long names' order and checksums), each file's size matches its chain, and lost clusters are counted and listed; FSInfo and the dirty bit are notes (`--require-clean`: dirty is damage). Exit 0 clean, 1 damage, 2 lost clusters only, 3 not FAT32. `--selftest` damages small volumes it makes in each of those ways (part of `make check`) |
| `tools/fat-label.py` | checks, or with `--fix` repairs, a FAT volume's label in an image the way other systems read it (the Makefile runs it on every image; `tools/data-test.sh` checks with it) |
| `tools/checkdocs.py` | the docs check of `make check` |
| `tools/checkwants.py` | every boot-image program's list ([`<wants.h>`](../user/include/wants.h)) checked by the rules libos applies to a program on `/data`, before the Makefile packs the boot image: the build's approval |
| `tools/checkdriver.py`, `tools/checkdriver-selftest.sh` | the driver build check, and the proof that it still rejects what it must (`tools/checkdriver-tests/`) |
| `tools/sortincludes.py` | the include-order check of `make check`; `make includes` runs it with `--fix` |
| `tools/gensyscalls.py`, `tools/genidl.py`, `tools/gensyms.py` | the syscall glue, the IDL headers and the kernel symbol table |
| `tools/genwl.py` | the Wayland tables and typed stubs from upstream's XML (`gen`, `check`, `selftest`) |
| `tools/mkbootfs.py`, `tools/mkimage.py` | the boot image and the two-partition disk image |
| `tools/bootfs-edit.py <in> <out> name=file...` | a boot image with files added, replaced or (`name=`) left out (the update tests' build B, and their builds with or without a test key) |
| `tools/update-test-key.sh <outdir> <in.img> <out.img> [nokey]` | for the update tests: two throwaway keys in `<outdir>/testkey` (made once by `build/host/jamos-sign keygen`) and a copy of the image whose build has the first one's public half (or none) |
| `build/host/jamos-sign self-test` | the Mac's signing tool (`tools/jamos-sign.c`, built by `make`) against Monocypher's Ed25519 vectors and a round trip; part of `make check` |
| `tools/write-usb.sh`, `tools/mbr-grow.py` | `make usb`: the image onto a stick, the data partition grown to its end and left blank |
| `tools/flash-usb.sh` | `make flash`: a new kernel, boot image and boot menu onto a stick's ESP, the stick's kernel and boot image kept as the previous build |
| `tools/flash-test.sh <outdir>` | `tools/flash-usb.sh` on a copy of the image attached with `hdiutil` (never a real disk; macOS): the new files are there, the stick's old kernel and boot image are the previous build (an older one placed first replaced), no `.new` file is left (stale ones placed first included), the data partition is unchanged |
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
