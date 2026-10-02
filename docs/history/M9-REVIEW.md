# M9 review: networking

An independent read of milestone M9 at main 632e9e8 against
CODING-GUIDE.md, ARCHITECTURE.md ("Networking", "How a service waits",
"What Jam OS defends against") and docs/M9-PLAN.md, by an agent that wrote
none of it. In order: the VLAN rule (drivers/rtl8125, drivers/e1000e,
`<jam/netframe.h>`, tools/checknotx.sh, the `vlan=` plumbing and what a
restart or a kexec does to it); DMA safety in both drivers; every parser of
network bytes (netstack's edge, dhcp, dns, netlog's acks, update's
manifest, wire and fetcher, init's check); authority; the service-loop
rule in every new loop; resource bounds; then clarity and the tests.
Findings first; each one's outcome is filled in as it is fixed.

Severity: **High** lets a frame leave untagged or on another VLAN, lets
the network or a program reach what it should not, or corrupts memory;
**Medium** is wrong behaviour the owner will meet, or one client's
trouble holding up others; **Low** is a narrow case, a wart, a gap in a
check, or a rule of the guide broken.

Nothing High was found. What held up under a careful read:

- **The transmit rule, both drivers.** Every frame netstack writes is
  copied out of the shared ring once (`netdev_take`: length and flags read
  once, checked, then copied into the server's own buffer), copied again
  by `netframe_tag` into the driver's DMA buffer for the next free
  descriptor (each byte read once, the tag after the addresses, zero
  padding to 64, a tag EtherType refused) and checked once more on that
  buffer (`netframe_tx_check`) right before the descriptor's ownership
  bit. Nothing but tx.c writes a transmit buffer or descriptor, a buffer is
  reused only after its descriptor came back, the descriptors' VLAN fields
  (RTL8125 extended status, e1000e VLE and special) are always 0, and the
  chips' own tag insertion and stripping are never turned on. The
  RTL8125's transmitter goes on only when MAC OCP 0xeb58 bit 0 reads back
  as the 32-byte format the driver writes.
- **Fail closed.** No valid `vlan=` (none, `off`, out of range, two that
  disagree, at the kernel and again in devmgr and the driver): neither
  driver touches its chip. kexec keeps every `vlan` word, well formed or
  not, first. The probe (`netprobe`) never reaches tx.c (mode set once in
  main.c; the gate is mode and VLAN; regs.c refuses every transmit
  register in every mode).
- **Frames the chips could send by themselves.** Pause is advertised by
  neither (RTL8125 ANAR read back after the restart says so on the PC;
  e1000e RFCE/TFCE and the FC registers 0), wake-on-LAN is off on the
  RTL8125 while Jam OS runs, its out-of-band firmware is taken back
  (`exit_oob`), and the tally check at exit would show a frame the driver
  did not queue (but see 7).
- **Restarts.** A driver that dies has its bus mastering turned off by the
  kernel before anything else; the next one resets the chip (transmitter
  off, FIFOs dropped) before it turns bus mastering on again, and so does
  devmgr's restart. kexec turns bus mastering off on every function.
- **DMA and teardown.** Both drivers stop the chip (receiver and
  transmitter off, reset), then bus mastering off, then unpin, on every
  exit path, failed bring-ups included. The RTL8125 writes each receive
  descriptor's buffer address on every hand-back (the PC's receive fix),
  the e1000e keeps one descriptor back from the chip as it should, and
  every length either chip reports is bounded (`RX_LEN` masked, a frame
  over its buffer is "split" and dropped, `netframe_rx_check` refuses over
  1518) before a byte of the buffer is read. The tally dump is waited for
  with a deadline and lands inside the ring VMO, which stays pinned until
  the chip is reset.
- **The netdev rings, both ends.** Each side keeps its own count and
  reads the other's once a pass, clamped (`netdev_ring_ready`,
  `netdev_ring_room`); netstack ends a session whose counts go out of
  range; slots' lengths are read once and bounded by the slot. The wake
  protocol (flag, fence, look again) can't lose a wake. Session keys carry
  a generation on both sides.
- **The parsers.** DHCP (`dhcp/msg.c`: every option bounded by its field,
  overload, RFC 3396 joining, fixed lengths exact, mask and router
  checked), DNS (`dns/msg.c`: names with strictly backward compression
  pointers and a jump limit, every record walked and bounded, only the
  asked name's chain believed, TTLs capped), update's manifest
  (`lib/update.c`: an exact six-line grammar, sizes and hashes bounded),
  its wire (`lib/updwire.c`: every field checked, data only within the
  datagram), the fetcher (`lib/updfetch.c`: a reply stored only if it
  matches a slot exactly, never past the manifest's size), netlog's acks
  (exact size and fields, an ack past what the source holds ignored) and
  netstack's edge (frames padded to 60 so lwIP's ARP input can't read
  stale bytes; a frame never a pbuf chain; ICMP errors rate-limited; no
  fragments, options, TCP or IPv6).
- **init's update check.** The offer is read into init's heap, the files
  are copied into VMOs only init holds while the bytes written are
  hashed, and only those copies go to `kexec_load`: nothing the fetcher
  does after the offer changes what was checked. The copy and hash run on
  a worker; only `kexec_load` and the answer are in the loop.
- **Authority.** The cards' devmgr device channels go to netstack alone
  (claimed by init before `/svc/devmgr` is published; devmgr refuses a
  query channel's GET_SERVICE for a claimed device); netctl (and with it
  `dhcp_open`, the only socket that may broadcast or send from 0.0.0.0) is
  held by init and dhcp only; programs get `/svc/net` per opener, ports
  from 1024, unicast destinations only; `bin/update` holds `/svc/net` and
  its offer channel and nothing else; only the shell's (admin) initctl
  channel can ask for an offer channel; netlog holds a klog reader and the
  crash log read-only.
- **The drivers' and the netdev server's loops** follow the rule: one
  port each, every piece of work within a budget, a busy server polling
  the port rather than skipping it (the RTL8125's `cap = now`, the
  e1000e's `busy ? 0`), the rx ring full or no session dropping and
  counting, never waiting.

## Findings

| # | Sev | Where | What |
|---|---|---|---|
| 1 | Medium | `user/services/netstack/main.c:138-139` | **netstack's loop skips its port wait while any channel has work left** (`if (l.ctl_pending \|\| dev_pending(&l.dev) \|\| progs_pending()) continue;`). Every channel is bound PERSISTENT and fires on edges only, so a channel's new requests, the card's NETDEV_SIG_RX/LINK event and every other opener's and socket's packets are learnt only from the port. A program that keeps one opener or socket channel non-empty (more than PROGS_BUDGET = 8 requests per turn, e.g. a loop of `iface` or `sock_send_to` written without waiting, which any holder of `/svc/net` can do) keeps `progs_pending()` true for as long as it likes: netstack never reads the port again, so no other opener, no socket and not init's netctl is served, and once the rx ring's flag is up (`netdev_sleep`) received frames wait for a wake that is never read. A busy or hostile client holds up every other client and the network itself: the slow-peer rule broken. A compromised dhcp can do the same through netctl (CTL_BUDGET 16). |
| 2 | Medium | `user/services/dns/main.c:157-158` | **The same in bin/dns**: `if (D.net_pending \|\| socks_pending() \|\| askers_pending()) continue;`. A program that keeps its `/svc/dns` channel full of `resolve` requests (each past its 8 in flight is answered ERR_NO_RESOURCES at once, so it can write them as fast as it likes) keeps `askers_pending()` true: dns never reads its port again, so the DNS servers' replies on its sockets, netstack's answers on its opener channel (socket opens, `wait_change`) and every other asker go unseen. Every other program's `host`, `ping <name>` and `dns_lookup` time out while one program floods. |
| 3 | Medium | `kernel/kexec/load.c:33-37` (`kept_words`), `kernel/main.c:279` | **A kexec drops `net`, so the PC comes back without its network** after `reboot`, after a panic and after `update` from the "Jam OS (network)" boot. The RTL8125 is bound only on a boot with `net`, `netsend` or `netprobe`, and none is kept (kernel/main.c: "a reboot doesn't keep it"). So on the PC: `update` fetches and loads a build that then boots with no network (no netlog, and no second `update` without a power cycle, which brings back the stick's build); a panic on a network boot comes back without the network, so netlog never sends the panicked boot's log, the plan's crash path; and a `reboot` silently leaves the network. ARCHITECTURE says "kexec keeps it, so `reboot`, a panic and `update` come back on the same VLAN", which only the e1000e (bound on every boot) gives; QEMU's tests can't see it. `netprobe` and `netsend` are one-shot tests and are right not to be kept; `net` is a mode. |
| 4 | Low | `user/services/netstack/clients.c:250` | `echo` checks only `ctl_unicast(address)`, not the subnet's broadcast address as sockets do (`sendable` in sock.c): a program can ping 10.2.21.255, a broadcast on VLAN 21 (every host answers; the replies match no echo and are dropped). ARCHITECTURE: "A program can't send to a broadcast". |
| 5 | Low | `user/lib/net.c:196-199` (`net_sock_take`) | The datagram's `len` from netstack's `sock_recv` reply is taken as it comes (u16), while `d->data` is 1472 bytes. dhcp (`dhcp_input(d.data, d.len)`), dns (`dg.len`) and bin/update pass it on, and their parsers read `len` bytes: a netstack that answers with a length over 1472 (netstack parses the network, so it is the one most likely to be taken over) makes them read past their buffer. netstack itself never does. |
| 6 | Low | `tools/checknotx.sh` (the six rules), `drivers/rtl8125/notx.h:50-67` | **The rules are sufficient for the code as written, but not for every change that could transmit.** Not caught: (a) a register written through the mapping itself (`*(volatile uint16_t *)((uint8_t *)t->r + 0x90) = 1`): rule 1 looks for `drv_write*` only; (b) transmit descriptors or buffers written outside tx.c (`t->ring + TX_RING_OFF`, `t->txbufs`): a descriptor with OWN set by another file goes out at tx.c's next doorbell, untagged if its buffer was; (c) MAC OCP writes that change how the transmitter reads its ring (0xeb58 is written in chip.c, before the format check, which is right today); (d) the guard's register list is rge's: the 8125's other transmit queues and its tail-pointer doorbell (Realtek's own driver has them in the 0x2100 and 0x2800 ranges: unverified here) are not refused. And the e1000e has no guard and no check at all (QEMU only, but it is the driver every network test runs). |
| 7 | Low (design) | `drivers/rtl8125/tx.c:336-355` (`tx_tick`), `regs.c:189-220`, `chip.c:369-380` | **A chip sending frames of its own is only reported, and only at the driver's exit.** The tally check (chip's count of frames sent against what tx.c queued) runs when the driver stops; `tx_tick`'s 10 s line runs only while the driver's own counts move, so a chip sending PAUSE or management frames while the driver is idle shows nowhere until a `reboot`. A link that resolves to sending pause (`PAUSE TX` in the link line) is logged and the network carries on. Nothing fails closed at run time. |
| 8 | Low | `drivers/rtl8125/chip.c:347-348` | `chip_autoneg` keeps GTCR's other bits as it found them, test mode (bits 15:13) included: a PHY left in a test mode by the firmware would go on putting test waveforms on the wire (not frames, but nothing Jam OS asked for). rge writes only the two advertisement bits too; clearing 15:13 costs nothing. |
| 9 | Low | `user/services/netstack/netif.c:116-126` (`set_link`) | netstack logs every link up and down, with no limit, where the drivers log the first 12 changes and then one in 64. A flapping cable or port fills the log (and netlog sends it) from netstack's lines. |
| 10 | Low (design) | `user/include/net.h:31-35`, `user/services/netstack/progs.h:44`, `user/include/dns.h:21` | One program can take every slot: 32 openers (svc.connect repeated), all 32 sockets (two openers of 16), all 64 requests in flight (8 openers of 8); dns's 16 openers. Then dns can't open sockets, netlog and `bin/update` can't connect, and the shell's `ping` and `host` fail, for as long as that program runs. The limits bound memory, as intended; they don't share it. The same holds for every per-opener service today (devmgr's 32 channels, the mixer). |
| 11 | Low | `user/services/init/net.c:138-141, 153-156` | init's loop calls netstack with `_until` (1 s each, `set_ipv4` and `set_dns` when /data comes) and waits up to 1 s for a stopped DHCP client to end: up to 3 s of init's loop on a netstack that doesn't answer. Known class (ARCHITECTURE: init's loop still makes blocking calls), new instances. |
| 12 | Low | `tools/pcap-vlan-check.py:203-221` (`--pc`) | On the direct-cable capture, only frames whose source is the PC's MAC are judged; anything else is counted as the Mac's own. A frame the PC sent with another source address (a bug above the driver can write any source) would pass. On a cable that has only the PC and the Mac, every frame not from the Mac's adapter is the PC's. |
| 13 | Low | `drivers/rtl8125/full.c:50-78` (`dev_stats`), `drivers/lib/netserver.c:17,190-199` | Each `netdev.stats` makes the driver dump the tally and wait for it in its loop (10 ms at most), and the server takes up to 64 requests a turn: a netstack that floods `stats` holds the driver's loop for up to 640 ms a turn (receive and transmit wait). netstack, the only client, asks at most once in 2 s; only a broken netstack does this, and it delays only its own card. |
| 14 | Low (tests) | `tools/*-test.sh` | What let 1-3 through: no test floods one client of netstack or dns while another asks (`netsock_slow_reader` and `dns-test.sh`'s slow name are slow *peers*, not busy clients); no QEMU test can bind the RTL8125, so a `net` boot across kexec is the PC's to show (the e1000e is bound on every boot). The RTL8125's ring wrap is tested only in utest (`rtl8125_rxdesc_laps`, `rtl8125_txdesc`): QEMU has no RTL8125, as the PC's receive stall showed. |

### Design questions

- **A. Fail closed when the chip sends of its own (item 7).** A live check
  every 10 s whether frames move or not (one tally dump), and when the
  chip has sent more than tx.c queued, or the link resolved to pause:
  reset the chip and end the driver with a loud line, rather than report
  at exit. It changes the driver's behaviour on the network the owner
  relies on; the plan made the check a PC observation. The owner's call.
- **B. Sharing the per-opener limits (item 10).** A share per program
  needs a notion of "program" the IPC deliberately doesn't have (a server
  decides by the channel, never by the sender). Options: a smaller cap per
  opener with a few slots reserved for the services init starts (dns,
  netlog), or init handing dns and netlog their own channels instead of
  the published one. Cross-cutting (devmgr, the mixer): for after M9.
- **C. checknotx's reach (item 6).** Rules (a) and (b) are cheap grep
  rules (no `->r` outside regs.c, tx.c and main.c's map; no `TX_RING_OFF`
  or `txbufs` written outside tx.c and ring.c's allocation); (c) and (d)
  need register knowledge only the PC can confirm. The e1000e could get
  the same guard and rules, though it never runs on a real network.
- **D. Keeping `net` across a kexec (item 3)** was fixed here because the
  plan's own `update` workflow and crash path depend on it. If the owner
  prefers a reboot to fall back to the everyday boot (the network only
  when picked), revert that commit and instead say in README that `update`
  on the PC comes back without the network.

## Outcomes

| # | Outcome |
|---|---|
| 1 | |
| 2 | |
| 3 | |
| 4 | |
| 5 | |
| 6 | |
| 7 | Not fixed: design question A. |
| 8 | |
| 9 | |
| 10 | Not fixed: design question B. |
| 11 | Not fixed: the known class (ARCH-CHECK, items 0 and 8); netstack answers at once in practice. |
| 12 | |
| 13 | Not fixed: only a broken netstack reaches it, and it delays only its own card. |
| 14 | Covered by the tests of 1-3 where QEMU can. |
