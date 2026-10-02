# M9 plan: networking

Status (2026-10-02): the plan, with the owner's answers. **Built and
merged:** stage 0 (the listen-only probe; it ran on the PC:
[results](#stage-0-on-the-pc-2026-10-02-boot-0065-the-results)), stage 1
(the netdev contract, `vlan=`, the QEMU harness), stage 3 (netstack on
lwIP, on the netdev rings, started by init), R1 (the transmit path, the
`netsend` test and the netdev server: [below](#r1-progress-the-full-driver);
its first PC runs found transmit unreliable, a descriptor-size mismatch, now
fixed and waiting for the next PC run:
[the result and the fix](#r1-the-pc-result-and-the-transmit-fix)), stage 4 (sockets for programs: `/svc/net`,
`ping`, `net`: [below](#stage-4-built-sockets-for-programs)), stage 5a (the DHCP and DNS cores), stage 5b
(bin/dhcp, bin/dns and `/svc/dns`, `host`, `ping <name>`, the slow-peer
test: [below](#stage-5b-built-dhcp-and-dns-as-services)) and stages 6a
and 7a (netlog's and `update`'s cores, init's update check, the Mac
tools), stage 6b (bin/netlog:
[below](#stage-6b-built-netlog-on-the-network)) and stage 7b (`update`
over the network, in QEMU:
[below](#stage-7b-built-update-over-the-network)). The sections below say
what each one built and left for the next.

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

**The owner (2026-10-02):** VLAN 21 is the go-to network: the Mac works on
it too (by Wi-Fi it was 10.2.21.174; by its USB Ethernet adapter with the
VLAN 21 interface, 10.2.21.67), so netlog and `update` stay inside VLAN
21 and cross no firewall. (The home network also has a 10.2.10.0/24
Wi-Fi; Jam OS never needs it.) **`net.host` = 10.2.21.174**, the Mac's
Wi-Fi address on VLAN 21 (the owner's choice; a router reservation and
the Mac's "Private Wi-Fi address" set to Fixed keep it from changing).

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
   **The owner (2026-10-02): unsigned for now**, signing later. M9's
   `update` checks the manifest's sizes and SHA-256s (damage in transit),
   not who sent them; `update` runs only when the owner types it. The
   protocol keeps a place for a signature so signing can be added without
   changing it (ROADMAP follow-up).
4. **Does `update` survive a power-off?** *Recommendation:* no. It
   replaces the stored kernel in RAM (the one `reboot` and a panic
   start), never the stick: the ESP stays read-only to Jam OS. `make
   flash` still makes a build permanent.
   **The owner (2026-10-02): RAM only**: `update` for quick testing,
   `make flash` for a build meant to stay.
5. **The PHY firmware patch** ([below](#the-phy-firmware-patch)).
   *Recommendation:* no Realtek blob in the repo. Run without a patch
   first; only if the PC's link misbehaves, take the patch in the
   register-table form OpenBSD's `rge` driver carries (ISC licence).
   **The owner (2026-10-02): no file to start with.** And first of all
   one quick discovery agent: stage 0, listen only, to learn everything
   about the PC's NIC and port that the rest of M9 needs.
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

#### Stage 0, built: the PC run

Built 2026-10-02: `drivers/rtl8125` (main.c lists every register it
writes), `drivers/include/jam/netframe.h` (classify only: the transmit
side's tag and check come with the first driver that sends), the boot
word `netprobe` (kernel, init, devmgr, the driver: as `hidboot` travels;
a reboot doesn't keep it), the boot menu entry, `tools/checknotx.sh` in
`make check`, utest's `netframe_*` and `rtl8125_*` tests and
`tools/netprobe-test.sh` ([TESTING.md](TESTING.md#area-scripts)). What
it does and how it differs from the plan above:

- **No firmware tables at all**: not the PHY patch (question 5), and also
  not rge's other tables (the MAC's break points, the PCIe PHY table,
  the PHY's tuning values), nor rge's CSI and ASPM/CLKREQ changes. The
  chip runs on whatever the board's firmware loaded, and the log says
  what that was (`mac mcu:` and `phy:` lines). rge's MAC settings over
  OCP in `rge_init` are applied (chip.c's `mac_setup`), as is its
  out-of-band exit (RealWoW off).
- **The receive descriptor is 32 bytes**, not 16: with the receive
  configuration rge uses for the 8125B (0x41000c00), its `struct
  rge_rx_desc` has the buffer address at 16 and the status at 28. R1
  keeps this format (or proves the 16-byte one first).
- **The firmware's bus-master bit can't be seen**: devmgr's new
  `dma_cap` turns it off before any driver starts. The chip's own
  registers (command, ring addresses, interrupt mask) still show what the
  firmware left.
- The ring and the tally dump are in one contiguous VMO, the buffers in
  an ordinary one, neither restricted to DMA32: on the PC they should
  land above 4 GiB, which tests the chip's 64-bit DMA (the `ring:` line).
- The census counts by tag (untagged, priority-tagged, per VLAN id, outer
  QinQ tags) with up to six EtherTypes each; it reads the descriptor's
  length and bytes 12-17, never the addresses or the payload.

**The owner's run:** boot "Jam OS (network: listen only)", the cable in.
The probe takes about 75 s (up to 10 s for the link, then 60 s of
listening). During the 60 s, on the Mac on VLAN 21's Wi-Fi: `sudo arp -d
-a`, then `ping -c 20 10.2.21.1` (broadcast ARP on VLAN 21); optionally
pull the PC's cable for a few seconds and put it back (link-change
lines). Then bring back the `[rtl8125]` lines from the log
(`/data/logs/boot-NNNN.txt`, or `log` in the shell) and the RESULTS line
starting `rtl8125:`. The MAC address line stays in the log, never in a
doc.

**What to read from it, for R1:** the chip id (`xid 641`); whether the
firmware left the transmitter on (`WARNING`); the PHY's id and patch
version and the MAC's break points (whether the firmware patched it);
whether the link came up without the tables, at what speed and how fast;
the `ring:` line (64-bit DMA); `interrupts:` (MSI-X delivery: frames
found after an interrupt rather than at the 1 s poll); the census and
the verdict; `tally at end` with tx ok 0 and no `WRITES REFUSED`.

#### Stage 0 on the PC (2026-10-02, boot-0065): the results

The probe ran once on the PC (main f6c57bf), with the Mac pinging the
router and the cable pulled for 3 s. Its RESULTS line: `8125B xid 641,
phy 001cc840 patch 0000, link 1000 full in 2.2 s, 60 s: 2583 frames:
vlan 21: 597, untagged 514, other 1472, irqs 2584, tx tally 0 -> trunk
carrying 21 (untagged too: a native VLAN)`.

- **Nothing was sent:** the chip's own tally says `tx ok 0` at the start
  and at the end, and no write was refused.
- **What the firmware left:** receiver and transmitter off, not out of
  band (no management firmware holding the chip), interrupts masked; the
  transmit ring register held garbage (never armed); **wake-on-LAN armed**
  (magic packet). Wake-on-LAN only listens; R1 turns it off while Jam OS
  runs and the plan's rule (WoL off) stands.
- **The chip:** RTL8125B (xid 641), PCIe gen 2 x1, ASPM off. The MAC has
  no break points (ROM code); the PHY (id 001cc840) has no patch loaded
  (version 0): the board's firmware loads none either. With no tables at
  all it reset in 1 ms and linked at **1000 full in 2.2 s** (it advertised
  2500 too: the switch port is gigabit). The cable pulled at 51 s was seen
  (link down, back at 1000 full 3.2 s later, both by interrupt). So the
  ROM code is enough so far: question 5's "no patch" holds.
- **Interrupts and DMA:** MSI-X works (2584 interrupts, every frame found
  after one, none by the 1 s poll); the ring, tally and buffers were above
  4 GiB and the chip wrote them: 64-bit DMA works, no DMA32 needed.
- **The port is a trunk carrying more than VLAN 21:** frames arrived
  untagged (514: the home network, including the switch's LLDP) and
  tagged 21 (597), 10 (554), 11 (464) and 20 (454). Jam OS's driver keeps
  only VLAN 21 and drops the rest, as planned. The owner may also narrow
  the port on the switch to VLAN 21 tagged only (no native VLAN, no 10,
  11, 20), so the switch enforces the rule as well: defence in depth, not
  needed for M9.
- **For R1:** the 32-byte receive descriptor and receive configuration
  0x41020c0f work as the probe set them; the register values after reset
  are in the log (boot-0065, in the session scratchpad's logs10/).

### The RTL8125 driver

`drv/rtl8125`, bound by devmgr to 10ec:8125, one process like
`drivers/hda`. What it needs, from rge's order (cited in its comments):

- **Bring-up:** chip reset (bounded wait), the 8125B's init tables
  (PCIe PHY values, MAC settings over the chip's OCP register window),
  the PHY's setup (also over OCP), receive and transmit configuration,
  maximum frame size 1522 (1518 plus the tag), then bus mastering on.
  The safe-rebind rule holds: reset first, bus mastering after.
- **Rings:** one transmit and one receive descriptor ring (32-byte
  descriptors both ways, as rge's for the 8125: receive since
  [stage 0](#stage-0-built-the-pc-run), transmit since
  [the first PC runs](#r1-the-pc-result-and-the-transmit-fix); with an
  ownership bit, 256 each, the queue-0 rings only:
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

#### R1 progress: the full driver

Built 2026-10-02 (R1a: everything but the netdev server; R1b: the server):

- **`<jam/netframe.h>`'s transmit and receive halves.** `netframe_tag`
  copies an untagged frame (14..1514 bytes, each byte read once) into the
  driver's own buffer with the tag (0x8100, priority 0, the VLAN) after
  the addresses, pads short frames with zeros to 64 bytes and refuses a
  frame whose EtherType is a tag (0x8100, 0x88a8, 0x9100);
  `netframe_tx_check` is the last look at that copy (18..1518 bytes,
  bytes 12-15 exactly the tag, 16-17 no tag) right before its descriptor
  is handed over. `netframe_rx_check` keeps only 802.1Q frames with the
  VLAN (any priority) and names the reason for every drop;
  `netframe_untag` takes the tag off. utest's `netframe_*` tests try every
  length, every tag EtherType, every edit of bytes 12-17, and a caller
  that rewrites its frame after the copy.
- **The driver's two modes** (`drivers/rtl8125/args.h`): `netprobe` is
  stage 0's probe, unchanged in what it does; full mode needs a valid
  `vlan=` (`netdev_vlan_args`), else "no VLAN: the network stays off" and
  the chip is never touched. In full mode: the receive filter is our
  address and broadcasts (no multicast, not promiscuous), the tags kept
  (rx.c drops and counts the rest); rge's 32-byte transmit descriptors
  (16-byte ones until the first PC runs showed the chip reading them in
  32-byte steps: [below](#r1-the-pc-result-and-the-transmit-fix)) and a
  256-entry ring; one MSI-X vector for receive,
  transmit-done and link change; link changes counted and logged (the
  first 12, then one in 64); wake-on-LAN off in both modes (rge_wol:
  CFG3, CFG5, MAC OCP c0b6), left off at exit; pause still not
  advertised; still no firmware tables.
- **One transmit path: `drivers/rtl8125/tx.c`.** Copy and tag, check the
  copy, descriptor, doorbell. Every function it gives other files starts
  with the gate: full mode and a valid VLAN (`rtl_tx_allowed` in
  notx.h); its register writers check the gate again. regs.c's accessors
  refuse every transmit register in every mode. `tools/checknotx.sh` (in
  `make check`) checks six rules: registers written only in regs.c
  (behind the guard) and tx.c (behind the gate), no transmit register
  through regs.c, the transmitter enable named only in notx.h and tx.c,
  the gate at the top of every tx.c entry point and what the gate is, no
  call from the probe's files into tx.c, the mode and the VLAN set once
  in main.c; each rule is self-tested on `tools/checknotx-tests/`.
- **The tally check** (the plan's PAUSE check), at every exit: the chip's
  count of frames sent between the start and the end against what tx.c
  queued and took back (`drivers/rtl8125/txdesc.h`, `rtl_tx_verdict`):
  taken back <= sent <= queued is progress, and only more sent than
  queued means the chip sent frames of its own. A `tx check:` log line
  and the RESULTS line say `tx N queued, chip sent M, K back (equal)`, or
  `FEWER SENT`, `MORE SENT`, `NOT ALL BACK`.
- **The send test, `netsend`** (boot entry "Jam OS (network: send
  test)"): full mode on the kernel's VLAN, the link (10 s at most), the
  first frame kept on the VLAN (5 s at most, then it sends anyway: the
  switch port forwards), then twenty ARP probes (RFC 5227: sender IP
  0.0.0.0, target 10.2.21.1, VLAN 21's router; a driver word `arpto=`
  could change it, but no boot entry passes one), 200 ms apart, each
  tagged by tx.c; it watches for the router's replies (tagged 21, kept by
  rx.c; the send test alone reads an ARP body, of kept frames only, to
  recognise it), logs per probe its descriptor, when it was queued, when
  the chip handed the descriptor back and when the reply came, then the
  answered pattern and the waits, then stops. Nothing else is ever sent.
- **The netdev server (R1b)**: full mode without `netsend` serves
  abi/idl/netdev.idl on DR_SERVE exactly as `<jam/netdev.h>` says
  (the server, which knows nothing of the chip, was built as
  drivers/rtl8125/server.c and now lives in `drivers/lib/netserver.c`
  for every network driver, see "One netdev server" below; `full.c` is
  the card as the server sees it). `info` (MAC, VLAN, MTU 1500,
  link and speed, link changes, "RTL8125B"); `stats` (the driver's
  counts: frames and bytes each way, drops by reason, refusals by
  length, flags and tag, ring errors, sessions, and the chip's tally
  since the driver started, all five `chip_counted` bits); `open` gives
  a session channel (info and stats on it; open refused there), the two
  ring VMOs and the two events with the header's rights. One session: a
  second open is ERR_BAD_STATE while the first has a client; a session
  whose opener has gone is ended at the next open, and one whose channel
  closes is ended then. netstack's frames go from the tx ring to tx.c's
  `tx_send` and nowhere else (copied out of the slot, then copied,
  tagged and checked again by tx.c); a pass takes at most a ring's worth
  and stops when the chip's descriptors run out, carrying on when they
  come back. Kept frames go into the rx ring, published once per batch
  with NETDEV_SIG_RX when netstack waits; a full ring or no session drops
  and counts. Link changes signal NETDEV_SIG_LINK. All of it on the
  driver's one port. utest's `netserver_*` run `netserver.c` itself
  over a fake card, the test as netstack.
- **Which boot binds the chip:** the plan doesn't say when the everyday
  boot starts the network, and nothing opens the driver yet (stage 3b
  starts netstack with the device channel). So until then the service
  runs only on a boot with the word `net` (boot entry "Jam OS
  (network)"): the chip comes up on VLAN 21, receives and drops (no
  session), and sends nothing (only netstack's frames are ever sent).
  Making it the everyday boot is one line in devmgr's match table (the
  row's word to NULL), for when 3b and the PC run are done.
- **A PC check the `net` boot already gives** (optional, before netstack
  uses it): boot "Jam OS (network)", leave it a few minutes with the
  cable in, then `reboot`. The log should have `full mode on vlan 21:
  serving netdev`, the link line, and at the stop `tx check: the driver
  queued 0 frame(s) ... equal: the chip sent nothing of its own` with the
  transmitter on the whole time (the plan's PAUSE check over a long run),
  and `netdev: 0 session(s); rx 0 frame(s) to netstack, N with no
  session` (N: VLAN 21's broadcasts and ours).
- Wake-on-LAN stays off when the driver exits (not restored): the plan's
  rule is "off while Jam OS runs", and the driver also exits while Jam OS
  goes on (the probe, the send test); the firmware arms it again at its
  next start.

The owner's first runs of `netsend` and `net`, what they showed and the
next run: the section below.

#### R1: the PC result and the transmit fix

**The PC runs (2026-10-02, build 8d98b62; boot-0067 `netsend`, boot-0069
and boot-0070 `net`, netstack at 10.2.21.240):**

- `netsend`: link 1000 full in 2.5 s, the first VLAN 21 frame 2 s later;
  probe 1 no reply, probe 2 no reply, probe 3 answered in 3.9 ms (so the
  tag and the receive path work on the real network). `tx: 3 queued, 1
  sent, ... 2 still out; ... 69912 doorbell(s) again`, the chip's tally
  +2, and the tx check called that "THE CHIP SENT FRAMES THE DRIVER DID
  NOT QUEUE" (wrong: 1 back, 2 sent, 3 queued is progress).
- `net`, first boot: the Mac's pings to 10.2.21.240 got no reply at all,
  yet the chip sent 110 frames in that boot (its tally survives reboots
  and power-off on standby power: 2 at the start of boot-0069, 112 at the
  start of boot-0070). Second boot: `ping -c 10` lost seq 3, 5 and 7;
  `ping -c 100 -i 0.2` lost 61, the answered ones in 3.1/5.3/11.6 ms. The
  driver logged no tx counts: `reboot` ended it before its stop lines.

**The cause: the descriptor's size.** rge_init sets bit 0 of MAC OCP
register 0xeb58, and so did the driver (chip.c copies rge's list), but
it wrote 16-byte transmit descriptors where rge's `struct rge_tx_desc`
is 32 bytes (command and status, extended status, the 64-bit address,
16 bytes left 0; OpenBSD's if_rgereg.h). With that bit set the 8125B
steps through the ring 32 bytes at a time: it read the driver's even
descriptors only (each one correctly, as the command word and the
address sit at the same offsets in both sizes) and never an odd one. The
driver takes descriptors back in order, so it waited at descriptor 1 for
good. That is `netsend` exactly (descriptors 0 and 2 sent, the tally 2;
1 never sent, never back), about every other frame lost in the ping
runs, and worse with time, since the ring never drained: with 255
descriptors outstanding nothing more is queued at all (likely the first
`net` boot's silence after its 110 frames). The doorbell storm came on
top: tx_reap rang the doorbell again at every look while a descriptor was
still the chip's (rge_txeof does so once per pass), and each doorbell
with nothing the chip could take raised "transmit descriptor
unavailable", whose interrupt rang it again.

**Did anything untagged leave?** Not by this reading: the chip took
addresses and lengths only from the first 16 bytes of the driver's own
descriptors, which tx.c wrote and which point at its tagged buffers. The
driver's end-of-ring bit sat where the chip saw entry 127's unused half,
so past entry 127 the chip would have read the tally dump area and zeros
(no ownership bit), and it never got that far. The next run's capture
checks it on the wire.

**What changed** (branch commits, one each):

1. **32-byte descriptors** (`drivers/rtl8125/txdesc.h`, rge's layout; the
   ring 8 KiB, the tally after it). tx_enable reads 0xeb58 back and leaves
   the transmitter off unless its bit 0 says 32 bytes: the log line
   `transmit descriptors: 32 bytes; the chip's format bit (mac 0xeb58
   bit 0) is 1: they agree`. utest `rtl8125_txdesc`.
2. **The doorbell again at most once a second**: for a descriptor still
   the chip's, the first extra doorbell 1 ms after it was queued, then a
   gap growing fourfold to 1 s (`rtl_kick_due`), counted. utest
   `rtl8125_kick`.
3. **The tx check's wording** (above). utest `rtl8125_tx_verdict`.
4. **Stalls are explained in the log.** A descriptor still the chip's
   100 ms after its doorbell is a stall; the first six (one a second at
   most) log `tx STALL:` lines: its 32 bytes and its neighbours' command
   words, the transmit registers read back (ring address against ours,
   command, TXCFG, TXSTART, TDFNR, interrupt status and mask, 0xeb58) and
   the chip's tally against the descriptors back: "it SENT frames it has
   not handed back" (no write-back) or "it NEVER FETCHED this
   descriptor". Every frame's wait from the doorbell to its descriptor
   back is timed: min/avg/max in the `tx:` lines, the RESULTS line and
   netdev.stats (`tx_wait_*`, `tx_stalls`, `tx_kicks`, from its reserved
   words). A `tx so far:` line at most every 10 s while frames move, so a
   run that `reboot` ends still leaves its counts.
5. **`netsend` sends 20 probes 200 ms apart** and logs each (above).
6. **`tools/pcap-vlan-check.py --pc MAC`** for a capture taken at the
   Mac's end of a cable straight to the PC.

**rge_init next to the driver, step by step** (rge's order; "done" means
the driver does the same, with the same values):

| rge | the driver |
|---|---|
| rge_stop, rge_chipinit (reset, out-of-band exit, PHY power) | done (chip_reset), without rge_hw_init's tables (MAC break points, PCIe PHY values) or rge_phy_config's PHY tuning and patch: the owner's call, question 5 |
| the station address written | not written: the chip's own is kept |
| the receive and transmit lists cleared, end-of-ring on the last | done; transmit descriptors now 32 bytes |
| rge_newbuf: every receive descriptor handed over gets its size, extended status 0, the buffer's address, then OWN | done since the receive fix ("R1: receive on the PC"); before it the address was written once, at the start |
| config unlock (EECMD) | done |
| register 0xf1 bit 7 cleared | **left out** (meaning not published) |
| rge_disable_aspm_clkreq, twice | **left out**: ASPM is already off on the PC (`pcie:` line), CLKREQ is power saving |
| the EEE transmit idle timer (0x6048) | **left out**: EEE plus is off |
| receive and transmit ring addresses | done (transmit: tx.c) |
| RXCFG 0x41000c00, TXCFG 0x03000700 | done |
| CSI 0x70c: top byte 0x27 | **left out** (no CSI access; a PCIe setting, said to be the ASPM entry latency, and ASPM is off) |
| 0x382 = 0x221b | **left out** (meaning not published; Linux's start-up writes it too) |
| RSS off, one queue, CFG1 speed-down off, MAC c140, c142, **eb58 bit 0**, e614, e63e, c0b4, eb6a, eb50, e056 | done; eb58 bit 0 now matches the descriptor size |
| TDFNR = 0x10 | done (tx.c, before the MAC settings rather than among them, inside the same unlock) |
| MAC e040, ea1c, e0c0, e052, d430, DLLPR, EEE plus off, ea1c bit 2, the TCAM clear (eb54), 0x1880, INT_CFG0, the timers, 32 moderation registers, c0ac, e098, e032 | done |
| CSI 0x98 bits 15:8 cleared | **left out** (no CSI access) |
| MAC e092 | done |
| tag stripping (VLANSTRIP) | never: the plan's rule |
| CPLUSCMD receive checksum | **left out**: no checksum offload |
| RXMAXSIZE | 2048, not rge's jumbo size |
| the RXDV gate off, 2 ms, rge_iff, lock, 10 us, rge_ifmedia_upd | done (no pause advertised) |
| CMD = TXENB \| RXENB | done (tx_enable, after the format check) |
| rge_setup_intr(SIM): timer moderation | not used: one interrupt per event (IM 0) |
| rge_encap: address and extended status, then OWN with SOF, EOF, EOR and the length | done, with a release fence before OWN and a full fence before the doorbell; the ring is ordinary write-back memory (x86 DMA snoops it), the registers uncached (VMO_CACHE_UC) |
| rge_txstart: TXSTART (0x90) = 1, 16 bits, once per batch | the same write, once per frame |
| rge_txeof: the doorbell again once per pass when stopped at an owned descriptor | at most once a second per descriptor (change 2) |
| the ring aligned to 256 bytes (RGE_ALIGN) | 8 KiB into a page-aligned VMO (a static assert) |

If the next run still stalls with "NEVER FETCHED", the steps marked
**left out** that touch the chip's DMA are the next suspects, in this
order: 0x382, CSI 0x70c, CSI 0x98, 0xf1.

**The next PC run: a cable straight to the Mac.** The PC's port goes to
a one-port bridge, so the Mac can't watch the switch. Instead the PC's
cable goes straight into the Mac's USB Ethernet adapter (en11, which has
the VLAN 21 interface vlan0, service "Home Devices VLAN"): isolated from
the home network, so even a wrong frame goes nowhere, and the Mac sees
every byte the PC sends.

1. Unplug the PC's network cable from the bridge and plug it into the
   Mac's USB Ethernet adapter.
2. Turn the Mac's Wi-Fi off.
3. Give vlan0 the router's address for the test, so the send test's
   probes (always for 10.2.21.1) get an answer:
   `sudo networksetup -setmanual "Home Devices VLAN" 10.2.21.1
   255.255.255.0`. Nothing else is on this cable, so the address clashes
   with nothing.
4. Start the capture on the parent interface, which shows the tags (and
   any untagged frame): `sudo tcpdump -i en11 -e -nn -XX -w
   ~/pc-capture.pcap`. Leave it running for both boots.
5. Boot "Jam OS (network: send test)". It is done about 10 s after the
   link (20 probes, 200 ms apart, then 2 s for late replies).
6. Reboot into "Jam OS (network)". When the shell is up, on the Mac:
   `ping -c 100 -i 0.2 10.2.21.240`. Then wait 10 s (the `tx so far`
   line) and `reboot` the PC.
7. Stop tcpdump (Ctrl+C). Put things back: `sudo networksetup -setdhcp
   "Home Devices VLAN"`, Wi-Fi on, the PC's cable back into the bridge.
8. Check the capture: the PC's MAC is on the driver's `mac:` line in the
   boot log (`/data/logs/boot-NNNN.txt`; it stays out of every doc), then
   `python3 tools/pcap-vlan-check.py --pc <that MAC> ~/pc-capture.pcap`.

**Bring back:** both boots' `[rtl8125]` lines (above all `transmit
descriptors: ...`, the 20 `probe` lines, `send test:`, `tx:`, `tx so
far:`, `tx check:` and any `tx STALL:`), both RESULTS lines starting
`rtl8125:`, the Mac's ping summary and the checker's output.

**What a fixed driver shows:** `they agree`; every probe `back after`
well under a millisecond and answered (`++++++++++++++++++++`); `tx: 20
queued, 20 sent ... 0 still out; ... 0 doorbell(s) again`; `tx check: 20
queued, 20 sent by the chip ..., 20 completions seen: equal`; no `tx
STALL` line; the RESULTS line `netsend vlan 21, link ..., 20/20 probes to
10.2.21.1 answered ++++++++++++++++++++, ..., tx 20 queued, chip sent 20,
20 back (equal), wait ..., 0 stalled, ...`; the ping 0% loss; the checker
`from the PC: N frame(s), 0 NOT tagged 21, 0 garbled`, every ping
answered, and PASS. If it still fails, the `tx STALL` lines say whether
the chip never fetched the descriptor or sent it without writing it
back, and the capture says what actually left.

#### R1: receive on the PC

**The PC run (2026-10-02, build 41ccd54; boot-0073 `netsend`, boot-0075
`net`, the normal network: the trunk port):** transmit is fixed (20/20
probes; 132 queued, 132 sent by the chip, 132 back, 0 stalls in 117 s,
waits 0.04-0.33 ms). Receive is not: pings to and from the PC "worked at
the start, then stopped"; netlog's acks came back until about 72 s
(`the Mac answers again` at 5.3 s, then a `tx so far` line every 10 s
with each acked), `the Mac doesn't answer` at 78.3 s, and nothing came
back after that while the PC went on sending.

**Where it stopped: the end of the chip's first lap.** In full mode the
chip takes our unicast and broadcasts only (rx_filter): the probe boot
counted ~3 broadcasts a second on the port (the rest of its ~41 frames a
second were multicast, which full mode refuses), and netlog's acks,
ARP and pings add to that, so the chip had filled about 256 descriptors,
its whole ring once, at about 70-75 s. The netdev rx ring and netstack
were ruled out in QEMU: `tools/rxsoak-test.sh` pushes ~10 000 frames
(~7 000 on VLAN 21, ~27 laps of the 256-slot netdev ring, ~40 of
e1000e's descriptors) through the same netserver.c and netstack, with
lwIP's receive buffers back at 0 in use, and the guest still answers
every ping at the end.

**The cause: the descriptor's address written once.** The driver wrote
each receive descriptor's buffer address at the start only; giving a
descriptor back after a frame it wrote the extended status and the
command word (ownership, size, end of ring). The 8125's descriptor in
rge's RXCFG (0x41000c00, bit 24: Realtek's "version 3" layout, 32
bytes) has the address in a union with the frame's timestamp
(Realtek's `struct RxDescV3`: `addr` shares its 8 bytes with
TimeStampLow/High), so the chip's write-back may leave something else
there, and on the second lap the chip writes frames wherever that
points. rge_newbuf writes the address into every descriptor it hands
over. Whether the PC's chip really writes there, and what, the next run
shows (below); the fix holds either way.

**What changed** (branch commits, one each):

1. `drivers/rtl8125/rxdesc.h`: the receive descriptor as pure functions,
   behaviour unchanged (utest `rtl8125_rxdesc`).
2. **The address in every hand-back** (`rtl_rxd_arm` takes it and writes
   it before the fence and the ownership bit), as rge_newbuf. utest
   `rtl8125_rxdesc_laps`: a fake chip writing back as the version-3
   layout allows (a timestamp where the address was, RSS bytes in front)
   over three laps; every frame must land in its descriptor's own buffer
   (without the address write the chip "writes to 0"). The driver also
   counts descriptors that come back with another value in the address
   field (the first one kept).
3. **`tools/rxsoak-test.sh`** (netpeer `--flood`): the long run above.
4. **`rx so far` lines**, at most every 10 s and only while frames come,
   next to `tx so far`:
   - rtl8125: `rx so far: N from the chip (tally T, M missed), K kept;
     dropped U untagged, O other vlans, P vlan 0, B bad; S to netstack, F
     ring full, Z no session` and `rx ring: D of 256 descriptors the
     chip's, next I; R no-descriptor and V fifo-overflow interrupts; A
     address field(s) changed by the chip (first X)`;
   - e1000e: the same two lines (its chip counts and RDH/RDT);
   - netstack: `rx so far: N off the ring (B bad), L into lwIP (R
     refused); buffers U in use, M at most, E times none; dropped link
     arp ip icmp udp; P pings answered`.

**The next PC run:** "Jam OS (network)" as before, the Mac pinging
10.2.21.240 and the PC's `ping 10.2.21.1` for well over 2 minutes
(several laps of the ring). **What a fixed driver shows:** the pings
answered to the end; netlog never says `doesn't answer`; every 10 s an
`rx so far` whose counts keep growing past 256 from the chip, 0 missed,
`rx ring: 256 of 256 descriptors the chip's`, 0 no-descriptor
interrupts; netstack's `buffers 0 in use` (or a few), 0 times none. The
`address field(s) changed by the chip` count says whether the chip writes
there (non-zero: the cause confirmed; its first value is likely a
timestamp). **If receive still stops**, the lines say where: the chip's
tally standing still with `256 of 256` is the chip not receiving
(missed growing: it found no descriptor); `from the chip` growing but `to
netstack` not is the driver's filter; `to netstack` growing but netstack's
`off the ring` not is the ring or its wake; `into lwIP` growing with
`buffers` climbing to 128 is a leak in netstack.

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

#### Stage 2, built: drv/e1000e and nettest

Built 2026-10-02 (`drivers/e1000e`, about 1500 lines; devmgr binds it to
8086:10d3, and only QEMU_NET runs have one):

- **Bring-up** (main.c lists every register written): the chip quiet
  (interrupts masked, receiver and transmitter off, the GIO master
  disabled) and reset before bus mastering goes on (the safe-rebind rule);
  the address from the EEPROM (RAL0 if it doesn't answer); our address and
  broadcasts only (no multicast, not promiscuous); CTRL.VME, the VLAN
  filter, checksum offload, interrupt delays and every flow-control
  register off, pause not advertised (the PHY's ANAR through MDIC). No
  valid `vlan=`: "no VLAN: the network stays off", the chip never touched,
  exit 0.
- **Rings:** 256 legacy descriptors each way in one contiguous VMO, 2 KiB
  buffers (receive, then transmit, transmit buffer i for descriptor i) in
  another, neither restricted to DMA32. **Interrupts:** MSI-X vector 0
  for every cause (IVAR, as FreeBSD's em does for the 82574), ICR cleared
  by writing it back; a 1 s poll stands in for a lost one and adds up the
  chip's 32-bit counters.
- **Transmit** (`tx.c`, the only file that turns the transmitter on or
  rings TDT, every entry behind a gate on the VLAN): netdev_take (in the
  netdev server) copies the slot out of netstack's ring (bad length or
  flags: refused, counted), tx_send's netframe_tag copies it again into
  the descriptor's own buffer with the tag (pre-tagged: refused,
  counted), netframe_tx_check looks at that buffer once more, then the
  descriptor (VLE never set); tx_flush rings one doorbell per pass. A
  full descriptor ring leaves the rest in netstack's ring until
  descriptors come back.
- **Receive** (`rx.c`): netframe_rx_check on the length and bytes 12-17;
  kept frames go untagged (netframe_untag) to the netdev server
  (`srv_rx`), which puts them into netstack's rx ring; the rest are
  counted by reason (`rx_untagged`, `rx_priority`,
  `rx_other_vlan` for other VLANs, outer tags and a tag inside ours,
  `rx_bad`); no session or a full ring drops and counts.
- **The netdev server** (built as its own serve.c; now the shared one,
  see "One netdev server" below, with `loop.c` plugging the card in):
  info, stats (the chip's GPTC, GPRC, error and missed counters beside
  the driver's), open: a session channel of its own, the rings and
  events with `<jam/netdev.h>`'s rights; one at a time, an orphaned one
  ended at the next open; port keys carry the session's generation. One
  loop, one port (`loop.c`).
- **nettest** (`user/tests/nettest`) and **`tools/net-test.sh`**: in shell
  mode netstack holds the card's one session, so the script boots `init`
  from a copy of the stick whose bootfs runs `bin/nettest <mode>` from
  init.cfg (init's regression mode starts no netstack). `vlan`: the
  session rules; every bad length, flags, frames already tagged with each
  TPID (each refusal counted exactly), `produced` a ring and one ahead and
  then behind, and a thread rewriting the slots' EtherType while the
  driver copies them (2026-10-02 in QEMU: 892 of 2000 sent, 1108 refused);
  the peer and the pcap each count exactly the frames the driver queued
  and the chip sent (1157), all tagged 21 once, none with a tag inside.
  A deliberately broken driver (frames sent untagged) fails it. `rx`: the
  peer's census (5 untagged, 3 VLAN 0, 6 other VLANs, 4 QinQ, 2 nested, 1
  of 1522 bytes, 10 on VLAN 21 at every priority and length up to 1514):
  only the 10 arrive, whole and untagged, and each drop counter matches
  (the 1522-byte frame never reaches the driver: QEMU's chip drops it,
  RCTL.LPE being off); then 300 frames with the ring unread: 256 given,
  44 counted as `rx_ring_full`, the driver still answering. `vlan-off`:
  no frame at all, the driver finished, no service. Every run ends with
  devmgr stopping the driver cleanly and every job empty.
- netstack (stage 3b) opens a session on it and sees the link at
  1000 Mb/s in shell mode; its end-to-end test (`tools/netstack-test.sh`)
  is stage 3b's to run.
- Two netdev servers existed then: this one and R1b's (chip-independent,
  merged while this stage was being tested). R1b's moved to
  `drivers/lib/netserver.c` so both drivers can link it (see "One netdev
  server" below).

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

**As built (stage 1, `abi/idl/netdev.idl` and `drivers/include/jam/netdev.h`),
three changes from the above:**
- **No `link_wait`.** A link change signals `NETDEV_SIG_LINK` on the
  session's event to netstack, and `info` carries a count of link changes
  (`changes`), so a change is never missed and the driver keeps no
  waiting request.
- **`open` returns a session channel** (with the rings and events).
  devmgr shares the driver's own channel among everyone it hands the
  service to, so the driver could not see netstack go; closing the
  session (or netstack dying) ends it, and a new `open` first ends a
  session whose opener has gone.
- **Two events, one per waiter** (`to_driver`, `to_stack`) rather than one
  per direction: each waiter clears only its own bits, and netstack can
  only signal the driver's (`to_driver` reaches it with RIGHT_SIGNAL
  alone).

**Who reaches the driver:** init claims every network function's
devmgr device channel by class (02 00 00, with
`services_claim_class`, as it does for HD Audio) before it publishes
`/svc/devmgr`, and gives them to netstack alone
([Authority](../ARCHITECTURE.md#drivers-and-services)). A test program
reaches the driver only through devmgr's control channel, which only
programs under `user/tests/` may ask for.

#### One netdev server

The server side of all this (DR_SERVE, the session, its rings and
events) is one file every network driver links:
`drivers/lib/netserver.c`, its interface `<jam/netserver.h>`. The
driver plugs in a `struct srv_dev`: `send` (its own transmit path, which
copies, tags and checks), `room` (free transmit descriptors), `info` and
`stats` (its counts and the chip's); it hands kept, untagged frames to
`srv_rx` and tells the server when descriptors come back and when the
link changes. The Makefile links the file into each driver's object
(`DRV_LIB_<driver>`), so `tools/checkdriver.py` checks it as that
driver's code. utest's `netserver_*` run it over a fake card.

Both drivers use it: the RTL8125 (`full.c` plugs the card in) and the
e1000e (`loop.c`; its own serve.c and the session halves of its tx.c and
rx.c are gone). Each driver still owns its transmit path: the server
only calls `send`, which is the driver's gated tx.c (`tx_send`: copy,
tag, check). The e1000e queues descriptors in `tx_send` and rings TDT
once per pass (`tx_flush`, after `srv_work`). What the e1000e's own
server did that R1b's already had (so nothing was carried over): port
keys with the session's generation; the event bit and the flag cleared
before the tx ring is read, `netdev_sleep` after it (the RTL8125 version
loops again instead of signalling itself); a full descriptor ring leaves
frames in netstack's ring until a reap; the rx ring published only when
a frame went in; an orphaned session ended at the next open; the chip's
counters added up at every 1 s poll (driver-side). Two differences
for the e1000e: `stats.tx_bytes` counts each frame plus its 4-byte tag
(the e1000e counted short frames padded to 64), and session lines are no
longer capped (one line per open and per end). One fix came with the
move: `stats.link_changes` is now the driver's own count, the same as
`info.changes` (the server counted only the changes it was told of, so
the RTL8125's link coming up during bring-up was missing).

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

#### Stage 3a, built: the core without a device

Stage 3 was split in two: 3a (2026-10-02) is everything that doesn't need
the netdev contract; 3b plugs netstack into it.

- **lwIP 2.2.1** in third_party/lwip, only the files compiled
  ([VERSIONS](../third_party/VERSIONS.md)), unmodified, `-Werror` clean
  through its options alone. The port, user/services/netstack/port:
  `lwipopts.h` (what is in and out, and every size with its reason),
  `arch/cc.h`, `sys_arch.c` (`sys_now` from the clock; `LWIP_RAND` from
  RDRAND; lwIP's diagnostics and failed assertions to the log,
  rate-limited to a burst of 20 lines then 5 a second; a failed
  assertion ends netstack).
- **Memory:** all static, about 275 KiB: a 64 KiB heap (what lwIP builds
  to send), 128 receive buffers of 1536 bytes (a whole frame each, so a
  frame is never a chain), fixed pools (24 UDP sockets, 4 raw, 16 ARP
  entries). A full pool drops the frame and counts it; nothing grows.
- **The loop** (`main.c`): one port, the control channel (served 16
  requests a turn), lwIP's timers as the wait's deadline.
- **netctl** (abi/idl/netctl.idl, protocol 28): `set_ipv4` (refuses any
  address a host can't have: a mask of 1 to 30 bits, no subnet or
  broadcast address, no 0/8, 127/8, multicast or 240/4, a gateway in the
  subnet), `set_dns`, `clear`, `info`, `stats` (stack.h's counts and
  lwIP's buffers and heap in use). `dhcp_open` waits for stage 4's sockets.
  Each change is logged once.
- **What it answers:** ARP for its address; pings to its address (not to
  a broadcast); a UDP datagram to a closed port gets lwIP's ICMP port
  unreachable, and an unknown protocol (TCP included) protocol
  unreachable, both rate-limited to 10 a second at the edge. A new
  address is announced with one gratuitous ARP. Fragments and datagrams
  with IP options are dropped. With the link down nothing is sent.
- **Frames:** out untagged, 60 to 1514 bytes, padded with zeros. In: a
  frame under 60 bytes is padded with zeros first, because lwIP's ARP
  input reads its 28-byte header without checking the frame has it (a
  cut-short request was answered from the last frame's bytes; utest's
  `netstack_malformed` shows it).
- **The edge 3b plugs into** (`stack.h`): `struct stack_edge` (`tx`,
  `ctx`, the MAC), `stack_start`, `stack_set_edge` (a device came or
  restarted; a new MAC flushes ARP, the address stays), `stack_set_link`,
  `stack_input(frame, len)` per received frame, `stack_poll` (the timers;
  returns the deadline). 3b, on `<jam/netdev.h>`'s ring code: the rx
  ring's reader takes each frame (`netdev_take`) and calls `stack_input`;
  `tx` is `netdev_room` and `netdev_put` (a full ring is an error:
  counted as dropped), then `netdev_publish`; `NETDEV_SIG_LINK` becomes
  `stack_set_link`; the loop adds the session channel, `to_stack` and the
  devmgr device channel to its port, and init/net.c starts it with netctl
  at `SR_USER + 0`. Until then bin/netstack runs with no device: link down,
  nothing sent.
- **Not in 3a:** init starting netstack; `net.address` from the settings
  file; the ring netif and its utest against a fake driver; the
  end-to-end ping in QEMU.
- **Tests:** utest's `netstack_*` and `netctl_*`
  ([TESTING](TESTING.md#netstack)).

#### Stage 3b, built: netstack on the card, started by init

- **Reaching the driver** (`user/services/netstack/connect.c`): a
  thread of its own that serves nothing makes the calls that may wait:
  devmgr's GET_SERVICE on each device channel in turn, then netdev.info
  and netdev.open (2 s each). It hands the session (its channel, the two
  ring VMOs, the two events) to the loop as one message. A card that
  isn't netdev's (MTU not 1500, no VLAN) is refused.
- **The session, in the loop** (`netif.c`): both rings mapped and checked
  (`netdev_end_attach`); `stack.h`'s `tx` is `netdev_room` +
  `netdev_put` (a full tx ring drops the frame and counts it: netstack
  never waits for the driver); the rx ring drained into `stack_input`, at
  most a ring's worth a turn; counts published once a turn, the driver
  signalled only if it sleeps. `to_stack` is bound ONCE and cleared
  before the rings are looked at; `NETDEV_SIG_LINK` sends netdev.info on
  the session channel without waiting, and its reply (read off the port)
  sets the link. A slot with a bad length or flags is refused and counted;
  **a count out of range on either ring ends the session** (the rings
  can't be trusted any more) and a new one is asked for. A session that
  closes (the driver died or ended) is dropped, and the thread asked again
  after 250 ms (doubling to 5 s while it fails); the address and the ARP
  table stay, and the link coming up announces the address (a gratuitous
  ARP). The first device channel closing means devmgr is gone: netstack
  ends and init starts it with the new devmgr's channels.
- **netctl.device** (method 6): the session, the VLAN, the speed,
  sessions opened, ring errors, bad rx slots, tx frames dropped on a full
  ring, the chip.
- **init** (`user/services/init/net.c`, and a line each in `services.c`,
  `shell.c`, `init.h`): netstack is a supervised service after devmgr,
  started with netctl's server end (init makes the channel once and keeps
  both ends, as the mixer's) and every network card's device channel. It
  is killed and started again when devmgr ends.
- **The static address:** `net.address = <address>/<prefix> [<gateway>
  [<dns> [<dns>]]]` in /data/etc/settings (for example `10.2.21.50/24
  10.2.21.1 10.2.21.1`), parsed by libos's `<ipv4.h>`, is given to
  netstack (`set_ipv4`, `set_dns`, 1 s deadline) whenever it starts and
  whenever /data comes. Without it netstack has no address until the DHCP
  client (5b) sets one; init's netctl client end is the one to hand it.
- **Tests:** utest's `netdrv_*` and `ipv4_text`; `tools/netstack-test.sh`
  (end to end with e1000e and the peer's `--ping`) is written but **not
  yet run: drv/e1000e (stage 2) was not on main** when 3b was handed
  back ([TESTING](TESTING.md#netstack)).

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

#### Stage 4, built: sockets for programs

- **One call per datagram, not a ring per socket (the choice).** Each
  datagram is one message on the socket's channel, its bytes a fixed
  `u8[1472]` array. M9's traffic is small (ping, DHCP, DNS, the log; the
  biggest is `update`'s ~7000 datagrams with 32 in flight), a call costs
  a few microseconds, and netstack never waits inside one: a `sock_recv`
  with nothing queued is a `later` method, answered when a datagram comes
  or its timeout passes. Rings for sockets stay a design idea for TCP and
  bulk transfers ([ROADMAP](ROADMAP.md#design-ideas-not-scheduled)).
- **The protocol** (`abi/idl/net.idl`, id 29; libos's `<net.h>` on top):
  on an opener's channel `iface` (address, mask, gateway, DNS servers,
  MAC, device, link, VLAN, speed, and a `version` that counts address and
  DNS changes), `wait_change(version, timeout_ms)` (`later`), `counts`
  (`struct net_counters`: netstack's, the card session's and the
  programs'), `chip_counts` (`later`: the driver's `netdev.stats`),
  `udp(port)` (a socket: its own channel and its port), `echo(address,
  seq, size, timeout_ms)` (`later`: the round trip, TTL and size). On a
  socket's channel `sock_send_to(address, port, len, data)` (0:0 is the
  connected peer), `sock_recv(timeout_ms)` (`later`; 0: don't wait),
  `sock_connect(address, port)` (only that peer's datagrams, and the
  default for sends), `sock_state`. The shared channel answers only
  `svc.connect`, `iface` and `counts` (an answer that came later there
  could go to any of its holders). What changed from the plan above:
  `wait_up` is `wait_change` (also how DNS hears of new servers;
  `net_wait_up` loops on it), and the names (`iface`, `udp`, `echo`,
  `counts`, `sock_*`) keep the generated `net_*` calls clear of
  `<net.h>`'s.
- **Limits** (`<net.h>`): 32 openers, 16 sockets an opener and 32 in all
  (lwIP's pool is 33: the DHCP socket has the last), 8 requests in flight
  an opener (`wait_change`, `echo`, `chip_counts`) and 64 in all, 32
  datagrams queued a socket. A queued datagram is copied into netstack's
  own heap (so a slow reader never holds lwIP's 128 receive buffers; at
  most about 1.5 MiB in all); one more is dropped and counted (the
  socket's `dropped`, netstack's `dgrams_dropped`). Requests are served 8
  a channel a turn. Ports below 1024 are refused (`ERR_ACCESS_DENIED`),
  a taken one is `ERR_ALREADY_BOUND`, port 0 picks one from 49152.
- **What a program can't do:** send to a broadcast (the subnet's or
  255.255.255.255), multicast, loopback, 0.0.0.0/8 or 240.0.0.0/4, or
  receive broadcasts; send raw packets: an echo is built by netstack (one
  raw ICMP socket for all of them), with the opener's echo id (its slot
  in the low 5 bits, `os_random` bits above), and a reply goes only to
  the echo in flight with that id, seq and peer. An ICMP unreachable
  quoting one of our echoes answers it `ERR_NOT_FOUND`.
- **The DHCP socket** (netctl's `dhcp_open`, method 7: only netctl's
  holder, init or the DHCP client, can have one): port 68, broadcasts
  allowed, sends to port 67 only (255.255.255.255 or a unicast address),
  out of the interface without a route, from 0.0.0.0 while there is no
  address; every datagram to port 68 is taken, the server's answer to an
  address not yet ours too (`LWIP_IP_ACCEPT_UDP_PORT` in lwipopts.h).
  One at a time; it speaks `sock_*` like any socket.
- **netstack:** `progs.h` (the model), `clients.c` (the shared channel,
  openers, requests in flight, echo matching), `sock.c` (sockets and
  their queues), `stack.c`'s UDP and echo edge (`stack_udp_*`,
  `stack_echo_*`: still the only file that sees lwIP), `netif.c` asks the
  driver's counts without waiting. **init** makes `/svc/net`'s channel
  once (net.c), gives netstack the server end at `SR_USER + 1` and
  publishes the client end (a channel per opener); `svc net` is in the
  shell's list. **The shell:** `net`, `net stats`, `ping <address> [-c
  n] [-s size]` (an echo a second on a channel of its own, Ctrl+C at any
  time).
- **Tests:** utest's `netsock_*` (over the fake driver), and
  `tools/ping-test.sh` (`net.txt`: `ping 1.1.1.1` through QEMU's e1000e
  and the peer) ([TESTING](TESTING.md#netstack)). The slow-peer rule for
  netstack is `netsock_slow_reader`; its QEMU form (`tools/net-test.sh
  slow`) comes with 5b's DNS.

**The edges 5b, 6b and 7b use** (a loop that serves others uses the
forms that don't wait: `net_sendto_async`, `net_recv_arm`, then
`net_sock_take` until `ERR_SHOULD_WAIT` whenever the socket's channel
is readable; the generated `net_udp_send`/`net_udp_result` and
`net_wait_change_send`/`_result` for the opener's calls; a program
that serves nobody may block):
1. **bin/dhcp** (netctl only): `netctl_dhcp_open(netctl, &h)`,
   `net_sock_adopt(&s, h, NET_PORT_DHCP_CLIENT)`; `dhcp_io.send` is
   `net_sendto_async(&s, to, NET_PORT_DHCP_SERVER, msg, len)`; each
   datagram from `net_sock_take` goes to `dhcp_input`. netctl has no
   link-change wait: poll `netctl_info`'s `link` (once a second) for
   `dhcp_start`, or add a `later` method to netctl in 5b.
2. **bin/dns** (`/svc/net` and its `/svc/dns` server end): the servers
   from `net_info`'s `dns`, again whenever `wait_change` answers;
   `dns_io.send(port, ...)` opens a socket with `udp(port)` at the port's
   first use (`ERR_ALREADY_BOUND`: the resolver picks another port) and
   `net_sendto_async` to the server's port 53; `release` is
   `net_close`; each socket's channel on dns's port, its datagrams to
   `dns_input`. 16 names in flight is exactly an opener's 16 sockets.
3. **bin/netlog** (`/svc/net`, a klog reader): `net_wait_up(net,
   DEADLINE_NEVER, &i)`, `net_udp_open(net, 0, &s)`, `net_connect(&s,
   host, 5021)` (only the Mac's datagrams come in); `io.send` is
   `net_sendto_async(&s, 0, 0, ...)`; acks from `net_sock_take` to
   `netlog_ack`.
4. **bin/update** (`/svc/net`): `net_udp_open(net, 0, &s)`,
   `net_connect(&s, host, 5022)`; `io.send` is `net_sendto_async`;
   replies from `net_sock_take` to `updfetch_reply`. A socket queues 32
   datagrams, the fetcher's window. ARP keeps one waiting packet per
   address (lwipopts.h's `ARP_QUEUEING` off), so of the first burst, sent
   before the Mac's MAC is known, only the last goes; the fetcher asks
   again after 300 ms.
5. **init:** `/svc/net` is in its namespace: netlog's and update's
   grants name it (`"/svc/" SVC_NET`).

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

#### Stage 5a, built: the DHCP and DNS cores

Built 2026-10-02, before sockets exist: everything in DHCP and DNS that
parses the network's bytes or keeps protocol state, as libraries with
no I/O of their own, and their tests. `bin/dhcp` and `bin/dns` are
built but only say "not built yet" and end; nothing starts them. The
files: `user/services/dhcp/` (`dhcp.h`, `msg.c`: build and parse,
`client.c`: the state machine), `user/services/dns/` (`dns.h`, `msg.c`:
names, query, reply checks, `cache.c`, `resolver.c`: the queries in
flight), `user/include/netbytes.h` (big-endian loads and stores, host
order addresses); the tests are utest's
([TESTING.md](TESTING.md#the-dhcp-and-dns-cores-utest)). What they do,
where the plan above left it open:

- **DHCP:** options 1, 3, 6, 51, 54, 58, 59 read, plus 53 and 52
  (overload: options in the file and sname fields); a repeated option is
  one long option (RFC 3396). Every message sent is 300 bytes with
  option 61 (type 1 and the MAC), the host name `jamos`, the parameter
  list and option 57 (1500). DISCOVER and the first REQUEST ask for a
  broadcast answer (no address yet). Retransmits at 4, 8, ... 64 s, each
  +- 1 s; four REQUESTs, then a new DISCOVER; after a NAK while
  requesting a wait of 2 s doubling to 64 s; renewing and rebinding
  retry at half the time left (at least 60 s). INIT-REBOOT when started
  with the last address. The RFC 5227 ARP probe is a hook (none: no
  probe; no answer in 10 s: the address is free); a conflict sends a
  DECLINE and waits 10 s. A lease under 10 s counts as 10 s; T1 and T2
  default to 1/2 and 7/8; the times count from the exchange's first
  REQUEST.
- **DNS:** each name in flight has **a socket of its own**, on a random
  local port (1024 and up), and a random id, so a forged reply must
  guess about 32 bits, not 16 (the plan said one socket); so at most 16
  names are in flight, netstack's per-opener socket limit. Askers of a
  name in flight share its query (8 at most). Tries at 1, 2, 3 and 4 s
  over the servers in turn (10 s in all), each keeping its id and port;
  SERVFAIL moves to the next server at once; TC is ERR_NOT_SUPPORTED (no
  TCP); CNAMEs followed in one reply and over several, 8 at most.
  Answers are cached under the name asked (32 names, TTL at most a day);
  failures are not cached. An IPv4 literal is answered without a query.

**What 5b connects** (the edge is a struct of function pointers each
library calls; the caller's loop feeds the library and waits for its
deadline, so the service-loop rule holds by construction):

- `bin/dhcp` (holds netctl only): `dhcp_init(c, io, mac)`;
  `dhcp_start(c, now, last_addr)` once the link is up (again with
  `c->lease.addr` after the link or netstack comes back);
  `dhcp_input(c, now, msg, len)` for each datagram on netctl's DHCP
  socket; `dhcp_tick(c, now)` at `dhcp_deadline(c)`;
  `dhcp_stop(c, now, true)` on an orderly shutdown. Its `struct
  dhcp_io`: `send(ctx, to, msg, len)` from port 68 to port 67 of `to`
  (255.255.255.255 or the server); `bound(ctx, lease)`: netctl's
  set_ipv4 (address, mask, router) and set_dns, and the lease logged
  once; `unbound(ctx, why)`: netctl's clear, logged; `probe(ctx, addr)`
  (may stay NULL), answered with `dhcp_probe_done(c, now, addr,
  conflict)`; `random(ctx)`.
- `bin/dns` (holds `/svc/net` and its `/svc/dns` server end):
  `dns_init(r, io)`; `dns_set_servers(r, servers, n)` from netstack's
  DNS list, again whenever it changes; `resolve` (a `later` method) calls
  `dns_ask(r, now, name, cookie)` with a cookie naming the request:
  an error is the reply at once, OK means `answer` brings it; an asker
  whose channel closes: `dns_cancel(r, cookie)` for each of its
  requests; `dns_input(r, now, &datagram)` for each datagram on any of
  its sockets (local port, source address and port, bytes);
  `dns_tick(r, now)` at `dns_deadline(r)`. Its `struct dns_io`:
  `send(ctx, port, server, msg, len)`: open a UDP socket on `port` at its
  first use (ERR_ALREADY_BOUND if taken: the resolver picks another),
  send to the server's port 53; `release(ctx, port)`: close it;
  `answer(ctx, cookie, st, addr, n, ttl)`: the `resolve` reply;
  `random(ctx)`.
- **Randomness:** DNS ids and ports and DHCP's xid and jitter are only
  as unguessable as `random`. It comes from the kernel's generator
  (ChaCha20 seeded from RDSEED/RDRAND:
  [ARCHITECTURE.md](../ARCHITECTURE.md#random-numbers)) through libos's
  `os_random_u32()` (`<os.h>`, the `random_get` system call, no handle
  needed). The hook takes the context and os_random_u32 doesn't, so 5b
  writes one adapter in each of `bin/dhcp` and `bin/dns`:
  `static uint32_t io_random(void *ctx) { (void)ctx; return os_random_u32(); }`
  and sets `io->random = io_random`, in effect `io->random = os_random_u32`.
  Never a fixed seed or a generator of the service's own: forged answers
  would be easy.

#### Stage 5b, built: DHCP and DNS as services

Built 2026-10-02 on stage 4's sockets. The cores are as 5a left them
(the resolver's `dns_resolve` is now `dns_ask`: dns.idl's generated
client call has the name).

- **bin/dhcp** (`user/services/dhcp/main.c`): init starts it once
  netstack runs, with a duplicate of netctl's client end and nothing
  else, **unless `net.address` is set**: the static address wins. As
  /data may come after netstack, the client waits for /data (10 s at
  most, for a machine without one); if /data's settings have a static
  address it is never started ("init: net.address is set: the static
  address, no DHCP client"), and one already running is killed first
  ("... is stopped, the static address wins"). Its loop serves nobody,
  so it calls netstack with 1 s deadlines: `dhcp_open` (again every
  second while it fails), `netctl.info` once a second for the link and
  the MAC (netctl got no wait method: a poll costs nothing), and the
  edge's `set_ipv4`/`set_dns`/`clear`. Link up: `dhcp_start` with the
  last lease's address (INIT-REBOOT); link down, or netstack gone (the
  socket closes): `dhcp_stop` (the address cleared), the client starts
  over once netstack is back, asking for the same address. A new dhcp
  process keeps nothing, but if netstack still has an address (the last
  dhcp's lease) it is cleared and asked for again. Each lease is one log
  line ("dhcp: lease 10.2.21.100/24 from 10.2.21.1: gateway ..., DNS
  ..., 3600 s"); unchanged renewals say nothing. **No ARP probe**
  (`io.probe` is NULL): netstack would need a way to send an RFC 5227
  probe and hear the answer (lwIP's ACD module or an ARP-input hook), not
  cheap here; an ACKed address is taken as free. No RELEASE: dhcp is
  killed at a reboot, never stopped in order.
- **bin/dns** (`user/services/dns/`: `main.c` the loop and the servers,
  `socks.c` the sockets, `askers.c` `/svc/dns`; `dnsd.h`), with the
  server end of `/svc/dns` (made once by init, kept across restarts,
  published as a channel per opener) and `/svc/net` in its namespace.
  **abi/idl/dns.idl** (id 30): `resolve(name u8[256], timeout_ms) ->
  (count, addr0..3, ttl)`, `later`; the shared channel answers only
  `svc.connect`. Limits: 16 openers, 8 resolves in flight an opener, and
  the resolver's own 16 names (a socket each) and 8 askers a name; a
  request's `timeout_ms` (1..60000) ends it with ERR_TIMED_OUT
  (`dns_cancel`). Nothing in its loop waits for netstack: `iface` and
  `wait_change` (the DNS servers, again at each change), and each
  socket's `udp` open, are written with txids of its own on its opener
  channel and answered off the port. A socket opens at a port's first
  send; the datagram waits in its slot until the open is answered; a
  port another program has can only be known then, so that try is lost
  and the next one is told ERR_ALREADY_BOUND and picks another port. When
  netstack ends, every socket closes: dns ends too and init starts it
  again once netstack runs (only its cache is lost; libos's `dns_lookup`
  asks the new one).
- **libos `<dns.h>`**: `dns_lookup(name, deadline, &answer)` (blocking,
  for programs; asked again once if the resolver restarted) and
  `dns_lookup_on`. The shell's `host <name>` and `ping <name>` use a
  helper of the shell's (`sh_lookup.c`) that writes the request without
  waiting so Ctrl+C works. `svc dns` is in `<os.h>` and the shell's list.
- **The peer** (`tools/netpeer.py`): a DHCP server (leases from
  10.2.21.100, router and DNS 10.2.21.1, `--dhcp-lease`) and a DNS
  server (a few names, a CNAME, `fastN.jam`, the slow name `slow.jam`
  that is never answered, NXDOMAIN for the rest), always on
  ([TESTING](TESTING.md#the-network-peer)).
- **The slow-peer test** (the done-when; `tools/dns-test.sh`, which
  takes the place of the planned `net-test.sh slow`): `bin/dnstest`, run
  from the shell, asks for `slow.jam` on one thread and, while it waits,
  20 other names and 3 pings by name, each under 200 ms; the slow name
  ends ERR_TIMED_OUT after the resolver's 10 s. In QEMU (2 CPUs) the
  slowest of the 20 took 4 ms. The same script checks a static boot
  starts no DHCP client and that a netstack restart gets the lease back
  ([TESTING](TESTING.md#dhcp-and-dns-end-to-end)).

### netlog: the log over UDP to the Mac

- **bin/netlog** (user/services/netlog), started by init in
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

#### Stage 6b, built: netlog on the network

Built 2026-10-02 on stage 4's sockets and 6a's core. What is there, and
how it differs from the edges above:

- **bin/netlog** (`user/services/netlog/main.c`, one file): argv is
  `net.host` and the boot id in hex; the root with `RIGHT_ROOT_KLOG` only
  (a klog reader), a namespace with `/svc/net` only, and after a panic
  `SR_CRASHLOG` read-only. It blocks in `net_wait_up` (it serves nobody),
  opens a socket with `udp(0)` and connects it to `net.host`:5021, then
  loops: the socket's channel (each ack from `net.host`:5021 to
  `netlog_ack`), `netlog_poll`, one port wait for the socket, the log
  growing or the poll's deadline. It waits for the log only while the live
  stream's window has room (`netlog_wants_text`, new in the core): a klog
  reader stays readable while the log is past its last read, which a full
  window leaves it, so waiting for it then would spin. netstack ending
  closes the socket: "netlog: netstack has gone", then `wait_up` again, a
  new socket, the same place in the log. Memory: the core's datagram, one
  received datagram; the log is the kernel's ring.
- **The boot id comes from init**, not from netlog: init's net.c works it
  out once a boot (`wallclock_get`: UTC now less the uptime; 0 without a
  clock) and passes it to every netlog it starts. Worked out by each
  netlog, a restart after a `/data` remount (which reads the RTC again,
  to the second) could move it and start a second file on the Mac.
- **A restarted netlog** (init restarts it like any service) starts at
  byte 0 while the Mac already has more than a window. 6a's core ignored
  every ack past what it had sent, so it went back to 0 for ever. The core
  now believes an ack past what it sent if it is within what the source
  holds now (the live log's end, or an ended stream's length), and skips
  there (`skipped`, utest `netlog_sender_restarted`). An ack past the
  source's end is still ignored.
- **init** (net.c): `NETLOG` is a service of shell mode, after logd and
  before the shell; it waits for `/data` (its settings). Started when
  `net.host` is an IPv4 address and `netlog` isn't `off`; otherwise one
  line says why and it is not started this boot. The read-only duplicate
  of `SR_CRASHLOG` is taken in `net_init`, before lastboot.c lets the log
  go; each netlog start gets a duplicate of it.
- **Its lines** ("sending this boot's log [and the last boot's (name)]
  to ...", "netstack has gone", "no network", and the core's three) are
  state changes only. In the QEMU test netlog said 7 lines in the first
  boot (two late or paused receivers, a restart, netstack's restart) and
  sent 81 datagrams for two boots' logs and a crash log.
- **The settings keys** (`<settings.h>`): `net.host` (the Mac's address,
  10.2.21.174 for the owner) and `netlog = off`. netstack refuses
  broadcasts from programs, so netlog can send only to that one address.
- **Not done:** `net` in the shell doesn't show netlog's state (it would
  need a channel to netlog). On a boot without the network card (the
  everyday entry binds no RTL8125 yet) but with `net.address` set,
  netstack has an address with no card: netlog says once that the Mac
  doesn't answer and tries every 30 s, sending nothing that leaves.
- **Test:** `tools/netlog-test.sh` ([TESTING](TESTING.md#netstack)).

### update: a new build from the Mac

- **On the Mac:** `make` as always, then tools/update-server.py (new),
  left running. It serves `build/jamos.elf` and `build/bootfs.img` and
  a manifest: a format line, the build's version string and git hash,
  and each file's size and SHA-256 (unsigned in M9, the owner's
  decision; the manifest keeps a signature field, empty for now). It
  snapshots the files when the manifest is asked for,
  so a `make` running meanwhile can't mix two builds. (Later, with
  signing: a host tool built from vendored Monocypher makes a key once in
  ~/.config/jamos/, outside the repo, signs each manifest, and the public
  half goes into the image.)
- **The protocol** (own, over UDP, port 5022 planned): stateless. A
  request names the snapshot, the file (manifest, kernel, boot image), an
  offset and a length (at most 1400 bytes); the reply carries the same
  and the bytes. The fetcher keeps 32 requests in flight and asks again
  for whatever didn't come. A lost packet costs one retry, and the
  server keeps no state per client. Chosen over TFTP, which has
  lock-step blocks, extensions for speed and retransmission rules to get
  right, and over HTTP, which needs TCP (question 6); the Mac side is a
  script of ours either way (and must sign, later).
- **On the PC:** `update` in the shell asks init (`initctl.update`, a
  `later` method: init's loop goes on serving). init starts
  `bin/update`, the fetcher, with `/svc/net`, the server's address and
  a channel back, nothing else: it parses the network's bytes and holds
  no power. The fetcher fills two VMOs (each at most 32 MiB, the stored
  kernel's region) and sends them and the manifest to init. **init
  checks**: it copies both into VMOs only it holds (so the fetcher can't
  change them after the check), checks the sizes and both SHA-256s
  against the manifest (a signature too, once signing comes), then calls
  `kexec_load`
  (which init alone may) with the new kernel, the new boot image and this
  boot's command line. It notes `/esp`'s files as seen, so the `reboot`
  that follows keeps the fetched build instead of reloading the stick's.
  Then the shell prints `0.0.28-m8.6 (2079f35) -> 0.0.29-m9 (...)` and
  reboots through the normal path (logs synced, devmgr stopped,
  kexec). `update -n` fetches and checks without loading anything
  (as built: [stage 7b](#stage-7b-built-update-over-the-network)).
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
| init | gains: the hash check and `kexec_load` of a fetched build | no: checks bytes it copied, against the manifest |

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
| **0. Listen-only probe** (built) | R | the RTL8125 driver with no transmit code: reset, PHY up, receive ring, link, the 60 s census, RESULTS line, chip id and PHY patch version logged; the frame checks (tag, check, classify) and their utest | drivers/rtl8125/ (main, regs, phy, probe), drivers/include/jam/netframe.h, user/tests/utest/netframe.c; one match line in `user/services/devmgr/main.c`; utest's test table | nothing |
| **1. The contract and the harness** | Q | netdev.idl and netdev.h (rings, events, counters); `vlan=` from the kernel to init, devmgr and network drivers, kept by kexec; init claims class 02 00 00 and holds the channels; QEMU: `-nic none` by default, the spare MSI-X device, QEMU_NET; tools/netpeer.py (frames over dgram, the tag check, ARP and ICMP echo) and tools/pcap-vlan-check.py; TESTING's boot words | abi/idl/netdev.idl, drivers/include/jam/netdev.h, `kernel/main.c` (the word), `kernel/kexec/load.c`, init's and devmgr's argument passing, `tools/qemu-test.sh`, the Makefile's QEMU flags, `user/tests/utest/supervise.c`, tools/netpeer.py, tools/pcap-vlan-check.py, `docs/TESTING.md` | nothing (runs beside 0) |
| **2. e1000e** | Q | drv/e1000e: rings, MSI-X, link, the netdev server, netframe.h on both paths; user/tests/nettest (holds the NIC through devmgr's control channel: hostile transmit frames, receive census); tools/net-test.sh scenarios `vlan` (only VLAN 21 frames leave, whatever the test writes), `vlan-off` (no frame at all), `rx` (untagged and other-VLAN dropped) | drivers/e1000e/, user/tests/nettest/, tools/net-test.sh, one match line in devmgr | 0, 1 |
| **3. netstack core** | S | lwIP vendored (VERSIONS.md); the NO_SYS port (clock, memory, the options file); the netif over the netdev rings; the loop; netctl's `set_ipv4`/`set_dns`/`clear`; `net.address`; init starts netstack with the device channels; a utest of the ring netif against a fake driver; end-to-end: the peer's ICMP echo answered | third_party/lwip/, user/services/netstack/, abi/idl/netctl.idl, user/services/init/net.c (new: netstack and the later network services' starts) | 1 (2 for the end-to-end run) |
| **4. Sockets** (built) | S | net.idl and the socket channels, `/svc/net` (per opener, limits), `ping`, `net`, the settings keys, `svc net` in lists; tools/shell-tests/net.txt: `ping 1.1.1.1` in QEMU | abi/idl/net.idl, netstack's client side, `user/include/os.h` (the name), `tools/checkwants.py`, the shell's net and ping commands | 2, 3 |
| **5. DHCP and DNS** | F | dhcp, dns, dns.idl, `/svc/dns`, `host`, `ping <name>`; the peer's DHCP and DNS (and its slow name); the slow-peer test (DNS, netstack, the driver's ring) | user/services/dhcp/, user/services/dns/, abi/idl/dns.idl, the shell's host command, the peer's DHCP and DNS parts | 4 |
| **6. netlog** | G | netlog, the crash log stream, tools/netlog-recv.py; a QEMU test that the whole log arrives, from its first line, with the receiver started late and paused | user/services/netlog/, tools/netlog-recv.py, a netlog scenario in tools/net-test.sh | 4 |
| **7. update** | H | tools/update-server.py; bin/update; `initctl.update` and init's check (planned user/services/init/update.c); the shell's `update`; a QEMU test: build A boots, the peer serves build B (another version string), `update` runs it by kexec; a bad hash, a wrong size and a truncated file are refused with the running build untouched | tools/update-server.py, user/services/update/, `abi/idl/initctl.idl` (one method), init's update file, the shell's update command, tools/update-test.sh | 4 |
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

## Stages 6a and 7a, built: netlog and update without the network

Built 2026-10-02 (before sockets exist), so stages 6 and 7 split in two:
6a/7a, everything that doesn't need `/svc/net`; 6b/7b, the programs that
use it. What is there, and how it differs from the sections above:

- **The manifest** ([`<update.h>`](../user/include/update.h), parser
  `user/lib/update.c`): six lines, `jamos-update 1`, `version`, `git`,
  `kernel <size> <sha256>`, `bootfs <size> <sha256>`, `signature` (no
  value: a manifest whose signature line has one is refused,
  ERR_NOT_SUPPORTED, until signing is built; a signature will cover the
  bytes before its line). At most 1024 bytes, each file at most 32 MiB.
- **init's check** (`user/services/init/update.c`): a new initctl method,
  `update_offer`, hands out an offer channel; the sender writes one
  `struct update_offer` (the manifest as fetched, each file's length, two
  VMO handles); init copies each file into a VMO of its own, hashing the
  bytes it writes, checks lengths and SHA-256s, calls `kexec_load` (this
  boot's command line) and `reboot_keep_stored()` (reboot.c: `/esp`'s
  files noted as seen), answers one `struct update_answer` and closes the
  channel. Measured in QEMU: 530 ms for a 9 MB build.
- **The protocol** ([`<updwire.h>`](../user/include/updwire.h)): a
  20-byte request (snapshot, file 0/1/2, offset, length <= 1400) and a
  24-byte reply header (the same, a status OK/GONE/RANGE/BAD, the file's
  size). **The fetcher's window** ([`<updfetch.h>`](../user/include/updfetch.h)):
  32 requests in flight, 300 ms before one is sent again, 10 sends at
  most, the manifest again when the snapshot is gone (3 times at most); a
  reply is stored only if it matches a request in flight exactly.
- **netlog's core** ([`<netlog.h>`](../user/include/netlog.h)): the
  datagram as planned plus two fields: the offset the PC was last acked
  (a receiver that lost its files skips to it with a note) and a sequence
  number (the receiver counts lost datagrams). Text is cut after the last
  newline that fits; at most 16 datagrams per poll; a 500 ms first wait
  doubling to 30 s; after 3 waits without an answer it says so once and
  sends one datagram per try. Its sources: a klog reader, and the crash
  log VMO (`netlog_crash_source`, checked with `crashlog_header_ok`).
- **The Mac's tools**: `tools/update-server.py` (also `--manifest`) and
  `tools/netlog-recv.py`, each with `--self-test`. The server waits until
  both files have been unchanged for 1 s before a snapshot, but a snapshot
  taken while `make` is between writing the kernel and the boot image
  still pairs a new kernel with an old image: run `update` after `make`
  has finished.
- **Tests:** utest's `update_*`, `updwire_*`, `updfetch_*`, `netlog_*`;
  `tools/update-test.sh` (init's check fed by `bin/updtest` from files);
  the two self-tests ([TESTING.md](TESTING.md#area-scripts)).

**The edges 7b connects:**
1. `bin/update` (user/services/update/): a UDP socket from `/svc/net`
   to `net.host`:5022, `updfetch_start`, then a loop of
   `updfetch_poll(now)` (its return is the receive deadline) and
   `updfetch_reply` for each datagram from the server's address. Its io:
   `send` = the socket's send_to; `begin` = two VMOs of the manifest's
   sizes, page-rounded (made again after a restart); `store` =
   `jam_vmo_write`. At DONE it writes one `struct update_offer` on its
   offer channel (the manifest bytes `begin` was given) and waits for
   the `struct update_answer`, as `user/tests/updtest/main.c`'s `offer()`
   does.
2. initctl `update` (a `later` method, the next ordinal) in ctl.c: init
   calls `update_offer_new()` itself, starts `bin/update` with `/svc/net`,
   the server's address and the client end, and answers the pending txn
   from `update_event()` (which today answers only the offer channel: it
   needs a hook that hands the answer to ctl.c too). Or the shell makes
   the offer channel (`initctl.update_offer`) and starts `bin/update`
   itself; then init needs no change at all.
3. The shell's `update [-n]`: the running version (sysinfo) and the
   answer's version and git hash, then `reboot`. The running build's git
   hash is nowhere on the PC today.
4. tools/update-test.sh's network run: tools/netpeer.py answers port 5022
   with `Server(kernel, bootfs, sock=None, ...).answer(dgram)` from
   update-server.py (no socket needed); build B is the marked boot image
   the script already makes.

**The edges 6b connects:**
1. `bin/netlog` (user/services/netlog/): `wait_up` on `/svc/net`, a UDP
   socket to `net.host`:5021, the boot id from `wallclock_get` (UTC now
   minus uptime; 0 if the clock isn't set), `netlog_klog_source` over a
   klog reader, `netlog_start`; with a crash log, `netlog_crash_source`
   and `netlog_add_crash`. The loop: `netlog_poll(now)` gives the
   deadline; wait for the reader's `SIG_READABLE` or a datagram until
   then; `netlog_ack` for each datagram from `net.host`:5021 only. io.say
   is `printf`.
2. init (net.c, stage 3's file): start it in shell mode when `net.host`
   is set and netlog isn't off, with the root reduced to
   `RIGHT_ROOT_KLOG`, `/svc/net`, and a read-only duplicate of
   `SR_CRASHLOG` taken before lastboot.c lets the log go.
3. tools/net-test.sh's `netlog` scenario: tools/netpeer.py answers port
   5021 with netlog-recv.py's `Receiver(folder, sock=None).handle(dgram)`
   (it returns the ack) and the test compares the file with the boot's
   log from its first line, the receiver started late and paused.

## Stage 7b, built: `update` over the network

Built 2026-10-02 on stage 4's sockets; tested in QEMU (the PC run waits
for the RTL8125's transmit fix). What is there, and what changed from the
edges above:

- **The shell starts the fetcher, not init** (the plan's second option,
  the simpler one): `update [-n] [address]` (cmd/update.c) reads the
  server's address (`net.host` in /data/etc/settings, or the argument),
  takes an offer channel with `initctl.update_offer` and runs
  `bin/update` (user/services/update) as a helper (`sh_run_helper`): its
  list is `svc net`, its only other handle the offer channel, its
  arguments the address, `load` or `check`, and the running version and
  git commit. init needs no new method, and its authority is unchanged:
  only init's loop calls `kexec_load`, on copies only it holds. A
  refused or failed update returns 1 and nothing else happens; an
  accepted one is followed by the shell's own `reboot`.
- **bin/update**: `net_wait_up` (10 s), a socket from `udp(0)`
  connected to the server's port 5022, `net_recv_arm`, then a loop of
  `updfetch_poll` and, whenever the socket's channel is readable,
  `net_sock_take` into `updfetch_reply` (the server's datagrams only);
  Ctrl+C (the helper's stop channel) is looked at every 100 ms.
  `begin` makes two page-rounded VMOs, `store` writes them; at DONE it
  writes one `struct update_offer` with read-only duplicates
  (`RIGHT_READ | RIGHT_TRANSFER`) and waits up to 60 s for the answer.
  It says what the server has, each quarter fetched, the fetch's counts,
  and `0.0.28-m8.6 (2079f35) -> 0.0.29-m9 (abc1234)`, or why not: no
  answer at all, the server stopped answering, its answers don't match
  its manifest, init's refusal. Measured in QEMU (e1000e, the peer in
  Python): 9.7 MB in 7-8 s, ~6900 requests, 0-2 sent again.
- **`update -n`** is a check only, not a load: a new offer flag,
  `UPDATE_OFFER_CHECK_ONLY` (<update.h>), makes init check everything and
  answer without `kexec_load`, so the stored kernel stays the running
  build's (the plan said "loads without rebooting"; nothing loaded is
  safer: a panic can't start a build nobody switched to).
- **The hash is off init's loop** (the service-loop question 7a left):
  the loop reads the offer, parses the manifest and compares the lengths;
  the copy and the SHA-256s run on a worker thread (`update check`, a
  16 KiB static stack), which queues a packet with the offer's key when
  done; the loop then calls `kexec_load`, notes /esp and answers. In
  QEMU a 9.7 MB build: 225-260 ms hashed on the worker, ~90 ms left in
  the loop (`kexec_load` copying 18 MB into the region, and /esp's stat).
  A new offer channel while a check runs is `ERR_BAD_STATE`.
- **The git commit is in the build**: the Makefile writes
  `build/build.txt` (`git 2079f35`, `-dirty` when tracked files had
  changes; rewritten only when it changes) into bootfs as `build.txt`.
  The shell's `version` shows it (`Jam OS 0.0.28-m8.6, git 2079f35`),
  `update` sends it as the old side, and tools/update-server.py takes the
  manifest's `git` from the served boot image's `build.txt` (else the
  tree's), so the manifest names the build, not the tree.
- **Tests:** tools/update-net-test.sh (one QEMU run, QEMU_NET; the peer's
  `--update` serves update-server.py's `PlannedServer` with build B, and
  each `update` gets the next plan: a damaged kernel byte and a wrong
  manifest hash refused by init, a truncated boot image and a server gone
  mid-fetch failed by the fetcher, `update -n`, then `update` into B;
  B's version and git after the kexec; one `kexec_load`; every frame
  tagged 21). updtest gained an unknown flag (refused) and a check-only
  offer (accepted, not loaded), so tools/update-test.sh counts 13
  refusals. tools/bootfs-edit.py makes the test builds' boot images.
- **The owner's workflow** ([README](../README.md#boot-a-real-pc)): once,
  `net.host = 10.2.21.174` in the PC's settings; on the Mac `make`, then
  `python3 tools/update-server.py` left running; on the PC `update`.
- **Not done:** the PC run (after the RTL8125 fix); a build fetched by
  `update` reports "the stored kernel came from /esp" at its next boot
  (init/reboot.c's line, which can't know: the build itself is right).

## Where tracks meet

- **devmgr's match table** (`user/services/devmgr/main.c`): one line each
  from 0 and 2.
- **init**: stage 1 passes the word and claims the class; stage 3 owns a
  new file for the network services' starts, which 5 and 6 add to; 7
  owns its own file for the update check. Their entries in init's
  service table conflict only trivially.
- **The shell's command table**: 4 (net, ping), 5 (host), 7 (update).
- **`user/include/os.h`**: 4 (net), 5 (dns).
- **The Makefile**: lwIP (3), the two drivers and four services.
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
| update: the new build runs; a bad hash or length is refused | tools/update-test.sh |
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
