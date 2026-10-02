# M9 plan: networking

Status: the plan (2026-10-02, on main 2079f35, M8.6 done), with the
owner's answers. **Stage 0, the listen-only probe, is built and ran on
the PC** ([results](#stage-0-on-the-pc-2026-10-02-boot-0065-the-results)).
**Stage 1 part A (the netdev contract, `vlan=`) and stage 3a (netstack's
core without a device)** ([below](#stage-3a-built-the-core-without-a-device))
are merged.
R1's first half (the transmit path, the VLAN filter and the `netsend`
test) is built and waits for its PC run
([below](#r1-progress-the-full-driver-without-the-netdev-server)).

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
- **Rings:** one transmit and one receive descriptor ring (16-byte
  descriptors, though rge's receive descriptor for the 8125 is 32 bytes:
  [stage 0](#stage-0-built-the-pc-run) with an ownership bit, 256 each, the queue-0 rings only:
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

#### R1 progress: the full driver without the netdev server

Built 2026-10-02 (R1's first half; the netdev server is the second):

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
  (rx.c drops and counts the rest); the 16-byte transmit descriptors of
  rge's 8125B and a 256-entry ring; one MSI-X vector for receive,
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
  queued; a `tx check:` log line and the RESULTS line say `chip tally +N
  (equal)` or `(DIFFERS)`.
- **The send test, `netsend`** (boot entry "Jam OS (network: send
  test)"): full mode on the kernel's VLAN, the link (10 s at most), the
  first frame kept on the VLAN (5 s at most, then it sends anyway: the
  switch port forwards), then three ARP probes (RFC 5227: sender IP
  0.0.0.0, target 10.2.21.1, VLAN 21's router; a driver word `arpto=`
  could change it), one second apart, each tagged by tx.c; it waits for
  the router's reply (tagged 21, kept by rx.c; the send test alone reads
  an ARP body, of kept frames only, to recognise it), logs each round
  trip, then stops. Nothing else is ever sent.
- Not built yet: the netdev server (R1b), so a boot without `netprobe`
  or `netsend` still never binds the chip.

**The owner's run:** boot "Jam OS (network: send test)", the cable in.
It takes about 10 s after the link. On the Mac (on VLAN 21's Wi-Fi),
before booting: `sudo tcpdump -i en0 -e -n arp` shows the probes,
broadcast on VLAN 21 (`who-has 10.2.21.1 tell 0.0.0.0`, from the PC's
MAC; the Wi-Fi side sees them untagged). The router's reply is unicast to
the PC, so the Mac may not see it. Then bring back the `[rtl8125]` lines and the RESULTS line starting
`rtl8125: netsend`.

**What it should show:** `wake-on-LAN off: cfg3 ... -> ..., cfg5 ... ->
...` (the WoL bits cleared); `transmit ring: 256 descriptors at ...,
vlan 21 on every frame`; `receiver on, transmitter on: rxcfg 0x41..0c0a
(tag stripping off)` (accept bits 0x0a: our address and broadcasts); the link as before (1000 full in about 2 s);
`first frame on vlan 21 ... ms after the link`; `probe 1: reply in N ms`
three times (a router answers in well under 1 ms on the LAN); `rx on vlan
21: N kept ...; dropped: ... untagged ..., other vlans ...` (the native
and the other VLANs dropped); `tx: 3 queued, 3 sent, 0 with an error`;
`tx check: ... equal: the chip sent nothing of its own`; and the RESULTS
line `netsend vlan 21, link 1000 full in 2.x s, 3 of 3 ARP probes to
10.2.21.1 answered (ms a/b/c), tx 3 queued, chip tally +3 (equal), rx kept
N dropped M`. If the probes go unanswered but the chip's tally says +3,
the frames left: look at the Mac's tcpdump (did they arrive tagged 21?).
If the tally says +0, the transmitter did not run: the `transmit ring`
and `receiver on` lines and the `REFUSED` lines say why.

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
  `dns_resolve(r, now, name, cookie)` with a cookie naming the request:
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
- **Randomness:** libos has no random source yet. DNS ids and ports are
  only as unguessable as `random`: 5b needs a real one (RDRAND, which
  works in user space, checked by CPUID first; or one from the kernel).
  A fixed seed would make forged answers easy.

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
| **4. Sockets** | S | net.idl and the socket channels, `/svc/net` (per opener, limits), `ping`, `net`, the settings keys, `svc net` in lists; tools/shell-tests/net.txt: `ping 1.1.1.1` in QEMU | abi/idl/net.idl, netstack's client side, `user/include/os.h` (the name), `tools/checkwants.py`, the shell's net and ping commands | 2, 3 |
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
