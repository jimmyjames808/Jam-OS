# M9 plan: networking

Status: the plan, for the owner to read and answer before any code
(2026-10-02, on main 2079f35, M8.6 done). Nothing here is built yet.

Goal ([roadmap](ROADMAP.md#later)): **Jam OS on the network, through its
own driver for the board's RTL8125 and a network stack in user space,
on VLAN 21 only.**

**Done when** (the roadmap's row, unchanged):
- `ping 1.1.1.1` works on the PC, through the user-space network stack;
- a PC run's full log arrives on the Mac;
- `make` on the Mac and `update` on the PC run the new build, no stick
  moved;
- a service waiting on a slow peer delays only that peer's requests (a
  test);
- and, as every milestone: All tests and `soak 10` on the PC.

The rule this plan is built around ([ARCHITECTURE](../ARCHITECTURE.md#networking)):
**every frame Jam OS sends is 802.1Q-tagged VLAN 21; nothing is ever
sent untagged or on another VLAN.**

## Questions for the owner

Each has a recommendation; the plan assumes it until you say otherwise.

**Found on the Mac (2026-10-02),** plugged into the port the PC will use
(read-only checks): the port is a **trunk**. Untagged it gives the home
network (10.2.0.0/24, router 10.2.0.1); tagged 21 it gives VLAN 21
(10.2.21.0/24, "Home Devices VLAN": DHCP, router and DNS all 10.2.21.1,
24 h leases, the internet reachable: 1.1.1.1 answered in 14 ms). The Mac
was 10.2.21.67 on VLAN 21, by DHCP. So questions 1 and 2 are mostly
answered: Jam OS's tagging matches the port, and an untagged frame would
land on the home network, which is exactly what the rule forbids. Stage 0
still listens first to confirm it from the PC's own port.

1. **Is the PC's switch port a trunk carrying VLAN 21 tagged, or an
   access port on VLAN 21?** You don't need to know: stage 0 listens
   (it transmits nothing) and tells us from the tags that arrive
   ([the probe](#the-first-pc-stage-listen-only)). If it is an access
   port, the rule (Jam OS tags every frame) and the port disagree, and
   one of them must change. *Recommendation:* make the port a trunk that
   carries 21 tagged, ideally with no untagged (native) VLAN at all. The
   rule then stays as written, and the PC plugged into any other port
   reaches nothing.
2. **The Mac on VLAN 21.** netlog and `update` talk to the Mac. Is it on
   VLAN 21 (the same subnet as the PC), and can it keep one address (a
   DHCP reservation or a static one)? Does VLAN 21 have a DHCP server
   and a route to the internet (`ping 1.1.1.1` needs one)?
   *Recommendation:* the Mac on VLAN 21 with a reserved address, written
   once into the PC's settings file (`net.host`).
3. **Signed updates.** `update` replaces the running kernel with bytes
   from the network. A hash fetched from the same server proves only that
   nothing was corrupted, not who sent it: anyone on VLAN 21 who can pose
   as the Mac could run their own kernel on the PC. *Recommendation:*
   sign. An Ed25519 key made once on the Mac (kept outside the repo), its
   public half built into each image; the PC refuses an update the key
   didn't sign. Costs one small vendored library (Monocypher, BSD-2 or
   CC0).
4. **Does `update` survive a power-off?** *Recommendation:* no. It
   replaces the stored kernel in RAM (the one `reboot` and a panic
   start), never the stick: the ESP stays read-only to Jam OS. `make
   flash` still makes a build permanent.
5. **The PHY firmware patch** ([below](#the-phy-firmware-patch)).
   *Recommendation:* no Realtek blob in the repo. Run without a patch
   first; only if the PC's link misbehaves, take the patch in the
   register-table form OpenBSD's `rge` driver carries (ISC licence).
6. **TCP in M9?** Nothing in the done-when needs it. *Recommendation:*
   no. M9 is IPv4 with ARP, ICMP and UDP; TCP (and IPv6) come with the
   first program that needs them. Less code that parses what arrives.
7. **Should the PC answer pings?** *Recommendation:* yes (ICMP echo only:
   handy for seeing it is up from the Mac). Nothing else listens.
8. **netlog on by default** once `net.host` is set, sending only to that
   address (never a broadcast)? *Recommendation:* yes.

## The hardware

From the PC's boot log ([HARDWARE.md](HARDWARE.md#other-devices)):
05:00.0, **Realtek RTL8125, 10ec:8125 rev 05**, MSI-X (32 vectors,
table in BAR4, so the registers in BAR2 never share a page with it), a
cable plugged in. PCI revision 05 is the **RTL8125B**; the driver
confirms it from the chip's own id bits in its transmit configuration
register (0x641 for the 8125B; 0x609 is the first 8125, 0x688/0x689 the
8125D) and logs it. Stage 0 supports the 8125B only and says so for
anything else.

No datasheet is public. References, in the order to use them:
- **OpenBSD `rge(4)`** (if_rge.c and if_rgereg.h, by Kevin Lo): **ISC
  licence**, compatible with BSD-2. It drives the 8125, 8125B, 8125D,
  8126 and 8127. The first reference for the init order, registers and
  PHY setup; Jam OS writes its own code and cites it.
- FreeBSD: `re(4)` in base does not drive the 8125; Realtek's own
  BSD-licensed driver is in ports (net/realtek-re-kmod). Second opinion.
- **Linux `r8169`: GPL**. Read for hardware facts only (register
  meanings, quirks), never copied or paraphrased
  ([CODING-GUIDE](../CODING-GUIDE.md#licence-and-outside-code)).

## Decisions

### Where the VLAN tag is enforced: in the NIC driver

Every transmitted frame goes through the NIC's driver, and only the
driver holds the device, so the tag goes on there, below netstack: a
netstack that is buggy or taken over still can't send an untagged frame.

- **Transmit.** netstack writes plain (untagged) Ethernet frames into
  the shared transmit ring. The driver reads a frame's length once,
  checks it (14 to 1514 bytes), and copies the frame into its own DMA
  buffer, which netstack can't see, inserting the 4-byte tag (TPID
  0x8100, priority 0, the VLAN) after the two addresses. It refuses a
  frame whose EtherType is already a tag (0x8100, 0x88a8, 0x9100): no
  frame leaves with a tag netstack chose. Because the checked bytes are
  the driver's own copy, netstack changing the ring afterwards changes
  nothing (no check-then-use race).
- **Software, not the NIC's tag insertion.** The hardware's insertion
  needs a bit set right in every descriptor, and one wrong descriptor is
  an untagged frame. In software the tag is bytes in memory the driver
  wrote, the same code in both drivers (the RTL8125's and QEMU's), tested
  in QEMU and in utest. The descriptor's VLAN field stays 0, and the
  driver checks bytes 12-15 of its buffer once more before handing the
  descriptor to the NIC. The cost is a 4-byte shift inside a copy that
  happens anyway.
- **Receive.** The NIC's tag stripping is off (on the 8125 the bit is in
  the receive configuration register, not C+ command as on older
  Realtek chips), so the driver sees each frame's tag. It passes on only
  frames with TPID 0x8100 and VLAN id = the configured VLAN (any
  priority), with the tag removed; untagged, priority-tagged (VLAN 0) and
  other-VLAN frames are dropped and counted.
- **What the driver reads of a frame, all of it:** its length, and bytes
  12-15 (on transmit 12-13: the EtherType). Never the addresses, never
  the payload: everything else is parsed in netstack, which holds no
  `dma_cap` ([ARCH-CHECK claim 3](history/ARCH-CHECK.md#3-and-5-without-an-iommu-drivers-are-crash-isolated-not-contained)).
  The probe (stage 0) also counts EtherTypes (bytes 16-17 of a tagged
  frame) and logs counts only, no addresses.
- **Frames the NIC would send by itself.** 802.3x PAUSE frames are
  untagged MAC control frames the NIC can send on its own when its
  receive buffer fills. Flow control is off: pause is not advertised in
  autonegotiation and the MAC's pause transmit is off. Wake-on-LAN and
  Realtek's management features stay off. On the PC the chip's own count
  of frames sent must equal the driver's count of frames it queued (a
  `net` line), which shows the NIC sent nothing of its own.

These checks are pure functions in one shared header (planned:
drivers/include/jam/netframe.h, static inline: tag, check, classify),
unit-tested in utest and used by both drivers.

### The VLAN number: one place

- The kernel reads the boot word `vlan=<1..4094>`; with no word the
  VLAN is 21 (one constant, in `kernel/main.c` with the other boot
  words). `vlan=off` or any value that isn't a VLAN id means no VLAN.
- The kernel passes init `vlan=<n>` (only when there is a VLAN), init
  passes it to devmgr, and devmgr to every network driver as an argument
  (as `hidboot` reaches hid today). netstack is never asked: a word from
  netstack can't change the VLAN.
- **No VLAN: the NIC stays down.** A network driver started without a
  valid `vlan=` turns on neither its receiver nor its transmitter, logs
  `no VLAN: the network stays off`, and ends cleanly.
- kexec keeps the word (`kernel/kexec/load.c`'s kept words gain `vlan=`),
  so `reboot`, a panic and `update` all come back on the same VLAN.
- `net` in the shell shows the VLAN: the driver's `info`, which netstack
  only passes on.

### The first PC stage: listen only

The switch-port question is answered by listening, with nothing sent.

- Stage 0's RTL8125 driver has **no transmit code at all**: the
  transmitter enable bit is never set, no transmit ring address is ever
  written, the transmit doorbell is never touched. It resets the chip,
  turns bus mastering on, reads the MAC address, brings the PHY up
  (autonegotiation at 10/100/1000/2500, no pause), sets up a receive
  ring with the tag stripping off and accepting every frame, and waits
  for the link (bounded, 10 s).
- For 60 s after link-up it counts frames by tag: untagged, and per
  VLAN id, with the EtherTypes seen. Then the receiver goes off and it
  logs a summary and one RESULTS line, for example
  `rtl8125: 8125B xid 641, link 1000 full, 60 s: 412 frames: vlan 21:
  398, untagged 14 -> trunk carrying 21`.
- Reading it:
  - **tagged VLAN 21 frames arrive** (with or without untagged ones):
    a trunk carrying 21. The rule works as written. (Untagged ones too
    means the trunk has a native VLAN: worth turning off, question 1.)
  - **only untagged frames**: an access port. Which VLAN it is on can't
    be seen without sending. Owner decision (question 1); the full
    driver's stage waits for it.
  - **tagged frames, but none on 21**: the port doesn't carry 21.
  - **no frames**: nothing was broadcast in 60 s. Run it again while
    something on VLAN 21 sends broadcasts (an `arp -d -a` then a ping
    from the Mac does).
- The probe stays available afterwards: the boot word `netprobe` (a boot
  menu entry, "Jam OS (network: listen only)") starts the full driver in
  this mode.

### The RTL8125 driver

`drv/rtl8125`, bound by devmgr to 10ec:8125, one process like
`drivers/hda`. What it needs, from rge's order (cited in its comments):

- **Bring-up:** chip reset (bounded wait), the 8125B's init tables
  (PCIe PHY values, MAC settings over the chip's OCP register window),
  the PHY's setup (also over OCP), receive and transmit configuration,
  maximum frame size 1522 (1518 plus the tag), then bus mastering on.
  The safe-rebind rule holds: reset first, bus mastering after.
- **Rings:** one transmit and one receive descriptor ring (16-byte
  descriptors with an ownership bit, 256 each, the queue-0 rings only:
  no RSS, no second queue), 2 KiB buffers, all in contiguous DMA memory
  the driver pins with its `dma_cap` (64-bit addresses are fine: no
  DMA32 needed).
- **Interrupts:** one MSI-X vector (devmgr makes vector 0), bound to the
  driver's port; the 8125's 32-bit interrupt mask and status
  registers; receive, transmit-done, link change, receive overflow.
  The port wait has a deadline (1 s), so a lost interrupt costs time,
  never a stall, and the link is polled then too.
- **Link:** the PHY status register's link and speed bits; link changes
  are logged (one line each, rate-limited) and passed to netstack.
- **The loop** follows the service-loop rule: one port (the interrupt,
  netstack's transmit event, its `netdev` channel); nothing waits inside
  a request. A full receive ring toward netstack drops and counts; it
  never waits for netstack.
- **Statistics:** the chip's tally counters (dumped by DMA) next to the
  driver's own counts, for `net stats`.
- **Exit:** receiver and transmitter off, the chip reset, everything
  unpinned.

### The PHY firmware patch

Linux loads a "firmware patch" for the 8125B, `rtl_nic/rtl8125b-2.fw`
from the linux-firmware repository: microcode for the PHY's controller,
put into its RAM at every start. Its licence (`LICENCE.rtl_nic`)
allows redistributing the data with Realtek's copyright notice; it gives
no source and no right to change it. Linux's r8169 carries on without
it when the file is missing (it logs that the patch could not be
loaded), with the PHY running its ROM code. OpenBSD's rge applies the
same kind of patch from tables in its own source (`mac_r25b_mcu` in
if_rgereg.h, ISC).

The plan: **no patch at first.** Stage 0 logs the PHY's patch version
register (to see whether the firmware's UEFI driver left one loaded) and
whether and how fast the link comes up. If the link is unreliable or
slow on the PC, a later stage applies rge's tables (vendored in
third_party with their ISC notice; question 5), never the Linux blob.
A patch that fails to apply is logged and the PHY runs its ROM code:
never a reason to stay down.

### Testing without the real NIC: QEMU's e1000e

QEMU can't emulate the RTL8125. A second, small driver for a NIC QEMU
does emulate speaks the same `netdev` protocol and uses the same
netframe.h, so everything above the driver (netstack, DHCP, DNS, netlog,
`update`, the VLAN rule) is tested in QEMU; only the RTL8125's register
work is proven on the PC alone.

**The choice: e1000e** (QEMU's Intel 82574L, 8086:10d3), over
virtio-net:

| | e1000e | virtio-net |
|---|---|---|
| Shape | a register-programmed NIC with descriptor rings, an ownership bit, an interrupt cause register and a link bit: the RTL8125's shape | feature negotiation and virtqueues: nothing the RTL8125 has |
| MSI-X | yes (5 vectors), as the RTL8125 | yes |
| Work | ~600-800 lines, from Intel's public 82574 datasheet | ~500 lines plus a virtqueue layer Jam OS doesn't have |
| Already in every QEMU run | yes: q35's default NIC (utest's `driver_handle_limits` uses it as an unbound MSI-X function) | no |

The VLAN side: e1000e's tag insertion and stripping (CTRL.VME) and VLAN
filter stay off, so tags pass as bytes, as on the RTL8125.

**The harness:**
- QEMU's `-netdev dgram` (QEMU 10.0.2 on the Mac has it) carries each
  frame as one UDP datagram on 127.0.0.1 to a **test peer**,
  tools/netpeer.py (new). The peer fails the test on any frame that
  isn't VLAN 21, strips the tag, and answers as a small network: ARP,
  ICMP echo for any address (so `ping 1.1.1.1` works in QEMU), a DHCP
  server, a DNS server with a fixed table and a "slow" name, and the
  real netlog receiver and update server (it imports their code from the
  Mac tools, so the tests run them). It sends its replies tagged 21, plus
  untagged and other-VLAN frames that must be dropped.
- `-object filter-dump` records every frame as QEMU's NIC sends it to a
  pcap file; tools/pcap-vlan-check.py (new) fails on any frame from the
  guest that isn't tagged 21. Two independent checks of the same rule.
- QEMU's user networking (slirp) is not used: it doesn't speak 802.1Q,
  and the tests must not need the internet.
- The other QEMU tests stop getting a NIC: `tools/qemu-test.sh` and the
  Makefile's `run` add `-nic none`, and `driver_handle_limits` gets a
  spare MSI-X function with no driver instead (QEMU's igb, for example).
  Network tests set the NIC with a new QEMU_NET variable. Ports are
  chosen free per run (several agents test at once).

### netdev: rings, not calls

Between a NIC driver and netstack (`abi/idl/` gains netdev.idl; the ring
layout in a header, planned drivers/include/jam/netdev.h, the way
`user/include/mixer.h` describes the mixer's rings):

- **Control, in IDL:** `info` (MAC address, link, speed, VLAN, chip),
  `open` (the rings and events, below; one opener at a time; closing
  the channel ends it), `link_wait` (a `later` method: answers when the
  link changes), `stats` (the driver's counts and the chip's).
- **Data, in two ring VMOs:** transmit (netstack produces, the driver
  consumes) and receive (the other way). Each is a header page (each
  side's counter on its own cache line, frame counts since the open that
  never wrap, and a "sleeping, wake me" flag per side) then 256 slots of
  2 KiB (a length, then the frame). One event per direction. A side
  signals only when the other said it is sleeping, as the mixer does: no
  call and no system call per frame while traffic flows.
- **Who maps what:** the driver makes both ring VMOs (ordinary memory,
  not DMA memory) and maps them; netstack gets handles that can read,
  write and map them but not resize or pass them on (so the driver's
  mapping is safe), and maps them too. The DMA rings and buffers are the
  driver's alone. netstack holds no `dma_cap` and no hardware handle.
- **Trust:** the driver treats every count and length netstack writes as
  hostile: counters clamped to the ring, a length read once, the frame
  copied before it is checked. netstack treats the receive ring the same
  way (the driver is trusted more, but a bad length must not crash
  netstack).
- **Restarts:** a restarted driver ends the channel; netstack asks its
  devmgr device channel for the service again and opens new rings. A
  restarted netstack closes the channel; the driver drops the rings and
  waits for the next `open`. The rings are VMOs, so M11.6 can later keep
  them across a restart.

**Who reaches the driver:** init claims every network function's
devmgr device channel by class (02 00 00, with
`services_claim_class`, as it does for HD Audio) before it publishes
`/svc/devmgr`, and gives them to netstack alone
([Authority](../ARCHITECTURE.md#drivers-and-services)). A test program
reaches the driver only through devmgr's control channel, which only
programs under `user/tests/` may ask for.

### netstack: lwIP, single-threaded

`bin/netstack` (planned user/services/netstack), lwIP vendored in
third_party (the latest 2.2.x release; **licence BSD-3-Clause**,
allowed).

- **NO_SYS mode** (lwIP's raw API, no threads), not lwIP's own sockets
  over libos threads. One loop on one port: the receive event, each
  client's channel, and lwIP's timers (`sys_check_timeouts` at the next
  timeout). That is the service-loop rule by construction: nothing
  blocks, so nothing needs locks; a `recv` with nothing queued is a
  `later` method answered when a datagram arrives or its timeout
  passes. lwIP's sockets and netconn layers would need threads,
  mailboxes and semaphores only to give a blocking API that the IPC
  already gives.
- **Compiled in:** IPv4, ARP, ICMP (echo replies; question 7), UDP, and
  raw ICMP inside netstack for `ping`. **Left out:** TCP and IPv6
  (question 6), IP fragmentation and reassembly (no M9 path needs
  datagrams over 1472 bytes, and reassembly is a classic place for
  bugs), IGMP, lwIP's own DHCP and DNS (they are processes, below),
  lwIP's VLAN support (the driver does the tag). MTU 1500.
- Memory: lwIP's pools and heap sized in its options file, in netstack's
  own memory; per-opener limits (16 sockets, 64 queued datagrams per
  socket), so one program can't use up netstack.
- **Logging:** state changes only (link, address), rate-limited, never
  per packet (see netlog).
- The address: static if the settings file has `net.address` (address,
  prefix, gateway, DNS), else DHCP.

### Programs and sockets

- **`/svc/net`** (init publishes it; each opener gets a channel of its
  own with `svc.connect`), protocol planned as abi/idl/net.idl:
  `info` (address, mask, gateway, DNS servers, MAC, VLAN, link),
  `wait_up` (`later`: the address is set), `udp_open(port)` (a channel
  per socket; closing it closes the socket; port 0 picks one; ports below
  1024 refused), `ping(address, seq, size, timeout)` (`later`, answers
  the round trip), `stats`. On a socket's channel: `send_to` and `recv`
  (`later`), one datagram per call, up to 1472 bytes (a fixed-size IDL
  array until IDL has variable-length ones). A call per datagram is fine
  at M9's rates (ping, DNS, a log, a 10 MB fetch: about 7000 calls at
  ~2 us); rings for sockets are a design idea for later
  ([ROADMAP](ROADMAP.md#design-ideas-not-scheduled), shared request
  rings).
- **Not for programs:** broadcasts, raw sockets, sending from another
  address. Those are on **netctl** (planned abi/idl/netctl.idl:
  `dhcp_open` (a socket on port 68 that may send from 0.0.0.0 to
  255.255.255.255), `set_ipv4`, `set_dns`, `clear`), a channel init
  makes and gives to the DHCP client alone; never published.
- **Lists:** a program's list asks for `svc net` (and `svc dns`):
  `user/include/os.h` gains the names, `tools/checkwants.py` accepts
  them, `allow` shows them. The shell gets `/svc/net` for its commands.
- **Shell commands:** `net` (address, link, VLAN, counters; `net stats`
  the full counts with the chip's), `ping <address or name> [count]`,
  `host <name>`, `update`.

### DHCP and DNS: processes of their own

As the roadmap says, and for the same reason as `bin/play` and
`bin/jamcover`: they parse what strangers on the network send, so each
gets a process that holds almost nothing.

- **dhcp** (planned user/services/dhcp): holds netctl and nothing else.
  DISCOVER, OFFER, REQUEST, ACK; renewal at T1, rebinding at T2, the
  address cleared when the lease ends; options 1, 3, 6, 51, 54, 58, 59
  read by a bounded parser; client id = the MAC, host name `jamos`.
  Logs the lease once (address, gateway, DNS, lease time).
- **dns** (planned user/services/dns, protocol abi/idl/dns.idl):
  `/svc/dns`, a channel per opener; `resolve(name)` is `later`: each
  query is sent at once and answered when its reply comes (a slow name
  holds up only its own askers). IPv4 (A) answers only; a small cache
  bounded by count and TTL; compression pointers bounded (no loops); the
  query id and port random. It asks netstack for the DNS servers and
  uses an ordinary UDP socket.
- **The slow-peer test** (the done-when): the test peer delays answers
  for one name by 5 s; while a program waits for it, another resolves a
  fast name 20 times and each answer comes in under 200 ms. The same
  for netstack: one socket's `recv` waiting on a silent peer while
  another program pings, and the driver: netstack not reading its
  receive ring makes the driver drop and count, never wait.

### netlog: the log over UDP to the Mac

- **bin/netlog** (planned user/services/netlog), started by init in
  shell mode when `net.host` is set and netlog isn't switched off
  (question 8). It holds a kernel log reader (`RIGHT_ROOT_KLOG`, as logd
  does), `/svc/net` and nothing else.
- **From the first line:** it reads the log from position 0
  (`klog_read`): the ring keeps the last 4 MiB, so the whole boot is
  still there when the network comes up seconds later. If the ring has
  dropped lines meanwhile, a marker line says how many bytes.
- **Before the network is up** it waits (`wait_up`, in a process that
  serves nobody, so it may block). Nothing is sent before.
- **The protocol:** UDP to `net.host`, port 5021 (planned). Each
  datagram: a magic, a version, the boot's id (the kernel's start time in
  UTC nanoseconds), the byte offset in the log, up to 1400 bytes of
  text. The Mac acks the offset up to which it has everything; netlog
  keeps at most 64 KiB unacked and resends from the acked offset after
  500 ms without progress (go-back-N). With no answer it backs off to
  30 s between tries and keeps its place.
- **Never loops on itself:** netlog writes a log line only when its
  state changes (started, the Mac stopped answering, it answers again),
  never per datagram or per retry; netstack and the drivers never log
  per packet either. Its own lines are sent like any other; they can't
  multiply.
- **After a panic** the next boot's netlog also sends the panicked
  boot's log (the `SR_CRASHLOG` VMO init already has, duplicated
  read-only), as a stream of its own.
- **The Mac:** tools/netlog-recv.py (new) writes one file per boot
  (named by the boot's start time, in a folder given on its command
  line), prints the lines as they come, and says where a gap is.

### update: a new build from the Mac

- **On the Mac:** `make` as always, then tools/update-server.py (new),
  left running. It serves `build/jamos.elf` and `build/bootfs.img` and
  a manifest: a format line, the build's version string and git hash,
  and each file's size and SHA-256, signed with the update key
  (question 3). It snapshots the files when the manifest is asked for,
  so a `make` running meanwhile can't mix two builds. A small host tool
  built from the same vendored Monocypher makes the key once
  (~/.config/jamos/, outside the repo) and signs; the public half goes
  into the image. A build without a key works, and its `update` says
  so and refuses.
- **The protocol** (own, over UDP, port 5022 planned): stateless. A
  request names the snapshot, the file (manifest, kernel, boot image), an
  offset and a length (at most 1400 bytes); the reply carries the same
  and the bytes. The fetcher keeps 32 requests in flight and asks again
  for whatever didn't come. A lost packet costs one retry, and the
  server keeps no state per client. Chosen over TFTP, which has
  lock-step blocks, extensions for speed and retransmission rules to get
  right, and over HTTP, which needs TCP (question 6); the Mac side is a
  script of ours either way, since it must sign.
- **On the PC:** `update` in the shell asks init (`initctl.update`, a
  `later` method: init's loop goes on serving). init starts
  `bin/update`, the fetcher, with `/svc/net`, the server's address and
  a channel back, nothing else: it parses the network's bytes and holds
  no power. The fetcher fills two VMOs (each at most 32 MiB, the stored
  kernel's region) and sends them and the manifest to init. **init
  checks**: it copies both into VMOs only it holds (so the fetcher can't
  change them after the check), checks the signature with the key built
  into the image, the sizes and both SHA-256s, then calls `kexec_load`
  (which init alone may) with the new kernel, the new boot image and this
  boot's command line. It notes `/esp`'s files as seen, so the `reboot`
  that follows keeps the fetched build instead of reloading the stick's.
  Then the shell prints `0.0.28-m8.6 (2079f35) -> 0.0.29-m9 (...)` and
  reboots through the normal path (logs synced, devmgr stopped,
  kexec). `update -n` loads without rebooting.
- **What it means:** the fetched build runs until a power-off or until
  `/esp` changes; a panic comes back in it too. The stick is never
  written (question 4). Downgrades are allowed (the owner runs it); the
  versions are printed.

### What each new process holds

| Process | Holds | Parses network data |
|---|---|---|
| drv/rtl8125, drv/e1000e | its PCI function, BAR, interrupt, `dma_cap`; the netdev server end | no: length and bytes 12-15 only |
| netstack | the NIC's devmgr device channel(s); its `/svc/net` and netctl server ends | yes (Ethernet, ARP, IPv4, ICMP, UDP) |
| dhcp | netctl | yes (DHCP replies) |
| dns | `/svc/net`; its `/svc/dns` server end | yes (DNS replies) |
| netlog | a klog reader, `/svc/net` | the Mac's acks |
| bin/update | `/svc/net`, a channel to init | yes (fetch replies) |
| init | gains: the signature check and `kexec_load` of a fetched build | no: checks bytes it copied, against a signature |

init makes every one of these services' channels once and keeps their
server ends across restarts, as for the mixer
([ARCH-CHECK claim 10](history/ARCH-CHECK.md#10-start-order-hard-coded-in-devmgr-and-init)).

## Stages

Each stage is about one agent-hour, starts from main, and hands back
when its tests pass. Plumbing lands before its users. Every agent works
in its own worktree and branch, owns the files listed (an edit outside
them stays minimal and is named in its report), never pushes, never
touches a USB disk and never starts agents of its own.

| Stage | Track | What | Files it owns (new ones without a path check) | Needs |
|---|---|---|---|---|
| **0. Listen-only probe** | R | the RTL8125 driver with no transmit code: reset, PHY up, receive ring, link, the 60 s census, RESULTS line, chip id and PHY patch version logged; the frame checks (tag, check, classify) and their utest | drivers/rtl8125/ (main, regs, phy, probe), drivers/include/jam/netframe.h, user/tests/utest/netframe.c; one match line in `user/services/devmgr/main.c`; utest's test table | nothing |
| **1. The contract and the harness** | Q | netdev.idl and netdev.h (rings, events, counters); `vlan=` from the kernel to init, devmgr and network drivers, kept by kexec; init claims class 02 00 00 and holds the channels; QEMU: `-nic none` by default, the spare MSI-X device, QEMU_NET; tools/netpeer.py (frames over dgram, the tag check, ARP and ICMP echo) and tools/pcap-vlan-check.py; TESTING's boot words | abi/idl/netdev.idl, drivers/include/jam/netdev.h, `kernel/main.c` (the word), `kernel/kexec/load.c`, init's and devmgr's argument passing, `tools/qemu-test.sh`, the Makefile's QEMU flags, `user/tests/utest/supervise.c`, tools/netpeer.py, tools/pcap-vlan-check.py, `docs/TESTING.md` | nothing (runs beside 0) |
| **2. e1000e** | Q | drv/e1000e: rings, MSI-X, link, the netdev server, netframe.h on both paths; user/tests/nettest (holds the NIC through devmgr's control channel: hostile transmit frames, receive census); tools/net-test.sh scenarios `vlan` (only VLAN 21 frames leave, whatever the test writes), `vlan-off` (no frame at all), `rx` (untagged and other-VLAN dropped) | drivers/e1000e/, user/tests/nettest/, tools/net-test.sh, one match line in devmgr | 0, 1 |
| **3. netstack core** | S | lwIP vendored (VERSIONS.md); the NO_SYS port (clock, memory, the options file); the netif over the netdev rings; the loop; netctl's `set_ipv4`/`set_dns`/`clear`; `net.address`; init starts netstack with the device channels; a utest of the ring netif against a fake driver; end-to-end: the peer's ICMP echo answered | third_party/lwip/, user/services/netstack/, abi/idl/netctl.idl, user/services/init/net.c (new: netstack and the later network services' starts) | 1 (2 for the end-to-end run) |
| **4. Sockets** | S | net.idl and the socket channels, `/svc/net` (per opener, limits), `ping`, `net`, the settings keys, `svc net` in lists; tools/shell-tests/net.txt: `ping 1.1.1.1` in QEMU | abi/idl/net.idl, netstack's client side, `user/include/os.h` (the name), `tools/checkwants.py`, the shell's net and ping commands | 2, 3 |
| **5. DHCP and DNS** | F | dhcp, dns, dns.idl, `/svc/dns`, `host`, `ping <name>`; the peer's DHCP and DNS (and its slow name); the slow-peer test (DNS, netstack, the driver's ring) | user/services/dhcp/, user/services/dns/, abi/idl/dns.idl, the shell's host command, the peer's DHCP and DNS parts | 4 |
| **6. netlog** | G | netlog, the crash log stream, tools/netlog-recv.py; a QEMU test that the whole log arrives, from its first line, with the receiver started late and paused | user/services/netlog/, tools/netlog-recv.py, a netlog scenario in tools/net-test.sh | 4 |
| **7. update** | H | Monocypher vendored; the key and signing host tool; tools/update-server.py; bin/update; `initctl.update` and init's check (planned user/services/init/update.c); the shell's `update`; a QEMU test: build A boots, the peer serves build B (another version string), `update` runs it by kexec; a bad signature, a bad hash and a truncated file are refused with the running build untouched | third_party/monocypher/, the host tool, tools/update-server.py, user/services/update/, `abi/idl/initctl.idl` (one method), init's update file, the shell's update command, tools/update-test.sh | 4 |
| **R1. RTL8125, full** | R | the transmit path (copy and tag), interrupts, link changes, the netdev server, tally counters, `netprobe` and its boot entry; tested on the PC only | drivers/rtl8125/, `boot/limine.conf` (the entry) | 0's PC run and the port answer; 1; 2 as the model |
| **8. The join** | J | one QEMU run with every path that transmits (DHCP, DNS, ping, netlog, update's fetch) whose pcap has no frame but VLAN 21's, and the `vlan=off` run with none at all (tools/net-vlan-test.sh); the docs: ARCHITECTURE's Networking (what was built), the services table, the layers; README (commands, tools); HARDWARE (the port, the chip, the Mac's address) | tools/net-vlan-test.sh, the docs | all above |
| **9. Review and fix** | | the independent review-and-fix agent over all of M9 (the standing rule): findings listed first, then High and Medium fixed one commit each with a test; the VLAN rule and the parsers first | whatever its findings touch | 8 |

**Order and parallel work** (at most three or four agents testing in
QEMU at once; main frozen while tracks run):

1. **Now:** stages 0 and 1 together. Merge 0 first (it is small) and
   flash it: the owner's first PC run answers question 1, the chip and
   the link.
2. Stages 2 and 3 together; R1 too, once the port is a trunk.
3. Stage 4.
4. Stages 5, 6 and 7 together (they meet only in init's start table and
   the shell's command table).
5. Stage 8, then the review (9), then the PC sign-off.

Merge order: 0, 1, 2, 3, 4, then 5, 6, 7 as they finish, R1 when its PC
run passes, 8, 9.

## Where tracks meet

- **devmgr's match table** (`user/services/devmgr/main.c`): one line each
  from 0 and 2.
- **init**: stage 1 passes the word and claims the class; stage 3 owns a
  new file for the network services' starts, which 5 and 6 add to; 7
  owns its own file for the update check. Their entries in init's
  service table conflict only trivially.
- **The shell's command table**: 4 (net, ping), 5 (host), 7 (update).
- **`user/include/os.h`**: 4 (net), 5 (dns).
- **The Makefile**: lwIP (3), Monocypher and the host tool (7), the two
  drivers and four services.
- **tools/net-test.sh and the peer**: 1 makes the peer, 2 makes the
  script; 5, 6, 7 each add a scenario and their part of the peer.

## Tests

| What | Where |
|---|---|
| Tag, check, classify: every frame shape (untagged, 21, other VLAN, VLAN 0, double-tagged, runt, too long, an EtherType that is a tag) | utest (netframe.c) |
| The rings: counters clamped, a hostile length, wrap-around, the sleep/wake flags | utest, against a fake driver |
| **No untagged frame leaves, from any path:** the driver (nettest's hostile frames), netstack (ARP, ICMP, UDP), DHCP, DNS, netlog, update's fetch | tools/net-test.sh `vlan`, tools/net-vlan-test.sh: the peer and the pcap, both |
| No frame at all with `vlan=off` (the driver logs "no VLAN") | tools/net-vlan-test.sh |
| Untagged and other-VLAN frames never reach netstack | tools/net-test.sh `rx` (the driver's counts) |
| `ping 1.1.1.1` (the peer answers), DHCP lease, DNS | tools/shell-tests/net.txt |
| The slow-peer test | tools/net-test.sh `slow` |
| netlog: the whole log, from its first line, with the receiver late | tools/net-test.sh `netlog` |
| update: the new build runs; a bad signature, hash or length is refused | tools/update-test.sh |
| A driver or netstack killed and restarted: the other reconnects, the address comes back | tools/net-test.sh `restart` |

The existing suites keep passing with `-nic none` (stage 1 checks).

## What only the PC can show

In order; the owner flashes with `make flash` each time:

1. **After stage 0** (plain boot): the probe's RESULTS line and its
   `[rtl8125]` log lines: chip id, PHY patch version, link speed and how
   long it took, the census. Answers question 1.
2. **After R1 and stage 5:** link at 2.5G or 1G, a DHCP lease in VLAN
   21's subnet, `ping 1.1.1.1`, `ping one.one.one.one`, `net stats`:
   frames sent by the chip = frames the driver queued; frames dropped as
   untagged or other-VLAN counted. Optional, if the switch can mirror a
   port to the Mac: a capture there shows every frame of the PC's tagged
   21.
3. **After stages 6 and 7:** the boot's log arrives on the Mac from its
   first line; `make` on the Mac, `update` on the PC, the new version in
   the banner.
4. **Sign-off:** All tests, `soak 10` with the SanDisk mounted read-write
   and pulled and replugged, the four done-when checks again on one
   build; then M9 is marked done (ROADMAP, HISTORY, the version
   0.0.29-m9).

## References and licences

| What | Licence | Use |
|---|---|---|
| OpenBSD `rge(4)` | ISC | the RTL8125 reference; its PHY patch tables if ever needed (question 5) |
| Realtek's FreeBSD driver (ports) | BSD | second opinion on the 8125 |
| Linux `r8169` | GPL-2.0 | facts only, nothing copied |
| `rtl8125b-2.fw` (linux-firmware) | `LICENCE.rtl_nic`: redistribution with the notice, no source | not used |
| Intel 82574 datasheet | public | the e1000e driver |
| lwIP 2.2.x | BSD-3-Clause | netstack |
| Monocypher 4.x | BSD-2-Clause or CC0 | `update`'s signatures (question 3) |
