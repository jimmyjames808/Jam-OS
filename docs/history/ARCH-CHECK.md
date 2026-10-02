# Architecture check: the owner's ten criticisms

A check of a list of criticisms of Jam OS's architecture against the code
and the docs at main 020f81f (2026-10-02, M8.6 merged, its code check
still running), with what to do about each and when. Findings only: no
code or roadmap was changed. File references are to that commit. Nothing
here needed a QEMU run: the waits are read from the code's deadlines and
the IPC numbers from [BENCH.md](../BENCH.md).

Severity is measured against the owner's goals: daily use on the PC,
networking next (M9), graphics later (G1-G4), and a security story that
says only what is true.

## Summary

| # | Claim | True today? | Severity | Recommendation | When |
|---|---|---|---|---|---|
| 0, 8 | Service loops wait on one thing at a time (usb-bus one bulk transfer; devmgr's 2 s waits) | Partly. usb-bus was rewritten into tasks in M8.6: only bulk transfers are still one at a time, controller-wide. devmgr is as described and has longer waits too (5 s, 15 s); init has the same pattern (25 s, 15 s). Neither is on the file data path | Medium: Low for daily use, High for how M9's services are written | Settle the service-loop rule and give it tools: deferred replies and asynchronous calls in genidl, usb-bus's tasks as a libos library; netdev as shared rings. Per-endpoint bulk transfers as a follow-up | Rule and tools: before M9 (M9's first step). devmgr and init converted: with the devmgr split (M12), sooner if it bites. Bulk: any time, small |
| 1 | devmgr does too much; hand-written protocol | True: a third of devmgr is storage. devmgr.h's reason for not using IDL is out of date | Low now, Medium by M12 | Move disks, filesystem services and mounts into a service of their own and devmgr onto IDL, together | M12 (interface review) |
| 2 | Role checks instead of handles (console levels, exclusive driver, audio channel) | Mostly not: every one is decided by which channel a request came on (a facet), which is what CODING-GUIDE asks for. The residue: hda's owner is a flag in devmgr's match table plus a channel kind made for one client | Low | Replace the audio channel with a devmgr channel scoped to one device, made by init and granted to the one client (hda to the mixer, the NIC to netstack). Console levels stay until G1 replaces them | Before or as the first step of M9 |
| 3, 5 | No IOMMU: drivers are crash-isolated, not contained | True. The docs overclaim in six places. usb-bus parses every USB device's descriptors while holding a `dma_cap` | High for the security story (the docs), Medium for actual risk | Correct the wording now; a short "what Jam OS defends against" paragraph; keep untrusted parsing out of processes that hold a `dma_cap` (an M9 rule); M11 also blocks forged interrupts. The PC has a DMAR table | Docs: M8.6. Rule: M9. VT-d: M11 as planned, before G3 at the latest |
| 4 | Too much scheduler tuning | True in count (placement order with stealing, client/server pairs, spin before idle; wake-affine has no switch). Each was measured, but the benchmark has not run on the PC since M5.5, and two features together caused a PC-only test panic | Low to Medium | Keep the rule (a switch and a BENCH line showing a win, or it doesn't go in); run the benchmark on the PC now; soak once per milestone with the scheduler switches off; look at spin before idle with M10's tickless idle | Benchmark: M8.6 sign-off. Rest: standing rule, M10 |
| 6 | Restarts are seen by clients | True and documented; the music player already hides a mixer restart behind a gap | Low for daily use | Keep M11.6. Design M9's rings so their state can live outside the process | M11.6 |
| 7 | IPC is slow and everything goes through it | The numbers are M5's: since M5.5 a call is 1.4 us on one CPU, 1.2 us to the sibling, 2.1 us to another core. Sound samples and bulk file data don't go per call | Medium (for M9 and G1, not for storage or sound) | netdev as rings from the start; variable-length IDL arrays; keep M11.5's 600 ns target and re-measure | M9 (netdev), M11.5, M12 |
| 9 | FAT32 weakens the security model | Partly: authority never came from the filesystem, on purpose. The real gaps are integrity at rest and crash consistency, both accepted in the docs | Low | No change. One line in the threat model; at M13 the POSIX layer makes mode bits from views and `allow` | M13, or when `/data` holds something that matters |
| 10 | Start order hard-coded in devmgr and init | True but smaller than it sounds: init already makes the mixer's and music's channels once, so their clients don't depend on start order; devmgr's order is the device tree's | Low now, Medium as M9 adds four services | Make "init makes a service's channels once" the rule for every init service (M9's included) and the stop order a rank in init's table; a graph only when that stops being enough | M9 (its services); a graph not before M12 |

**Top three:**

1. **Before M9, settle how a service loop waits** (claims 0, 8 and 7): the
   rule "a loop that serves several clients never blocks on a call", and
   the tools that make it easy (genidl deferred replies and asynchronous
   calls; usb-bus's tasks in libos); netdev designed as shared rings with
   packet parsing only in netstack, which holds no `dma_cap`.
2. **Now, correct the driver-isolation wording** (claims 3 and 5) in
   SECURITY.md, README.md and ARCHITECTURE.md, and add a paragraph on what
   the design defends against; fix the two stale reasons found on the way
   (ROADMAP's usb-bus follow-up, devmgr.h's IDL note).
3. **Before the NIC's driver, replace devmgr's audio channel and the
   exclusive flag with a channel scoped to one device** (claim 2), so
   "hda belongs to the mixer" and "the NIC belongs to netstack" are grants
   init makes, not a new channel kind per consumer.

## 0 and 8: service loops that wait on one thing at a time

These are one issue: a single-threaded loop that, while handling one
request, makes a synchronous call (or waits for a process) with a long
deadline, so every other request waits behind it.

**usb-bus: mostly fixed by M8.6, the claim is out of date.** Since M8.6
every port and every device with requests runs in a cooperative task of
its own in the driver's one thread (`drivers/usb-bus/task.c:1-27`): "a
device that takes seconds to answer holds up only its own port, and a
class driver's request is answered while other ports are still being
enumerated". What is still one at a time:

- the command ring (`task.c:23-25`): one command at a time is how xHCI
  runs them (xHCI 4.6.1), so this is not a limit of ours;
- one control transfer per device's default endpoint: also the USB rule;
- **one bulk transfer at a time, controller-wide** (`bulk.c:11-13`, the
  single `g_hc.bulk` slot in `usbbus.h:292-301`; a second device's
  transfer waits its turn at `bulk.c:347-348`). The wait runs other
  tasks meanwhile, so enumeration, hubs and the keyboard and mouse
  (interrupt endpoints, kept queued by `intr.c`) carry on. What it
  blocks is another stick's bulk transfer: on the PC every USB device is
  on one controller ([HARDWARE.md](../HARDWARE.md#usb)), so a stick that
  stops answering holds the other stick's I/O for up to its command's
  timeout (5 s read, 10 s write: `drivers/usb-storage/scsi.c:32-33`),
  plus reset recovery. Example: the SanDisk misbehaving while music plays
  from it stalls logd's writes to `/data` for that long.

Within one stick, one SCSI command at a time (`drivers/usb-storage/main.c:
24-26`) is the Bulk-Only Transport protocol's rule (it has no command
queue; UAS does), not a design flaw.

ROADMAP's follow-up (`docs/ROADMAP.md:127-129`, "One bulk transfer at a
time inside usb-bus's loop ... Asynchronous transfers would be a
redesign of the serve loop") is stale: the loop redesign happened. What is
left is local to `bulk.c`: move the in-flight state from `g_hc.bulk` into
each interface's `struct bulk` and match transfer events by slot and
endpoint, so each stick has its own transfer in flight. About an
agent-hour with a two-stick test (one stick made slow by the test). It
does not matter for M9 (the NIC is PCIe, not USB); it matters for daily
use only with two sticks and one of them failing. Do it as a follow-up
whenever usb-bus is next open (A3's USB audio would be a natural time).

**devmgr: true, and the waits are longer than 2 s.** devmgr is one thread
on one port (`user/services/devmgr/main.c:609-616`). Its disk steps were
made asynchronous (`disk.c:80-82`: requests "written without waiting"),
but these calls still block the whole loop:

| Where | Wait | When |
|---|---|---|
| `disk.c:229` `storage_disk_id_until` | 2 s (`CALL_WAIT`, `disk.h:16`) | each Jam OS disk found |
| `disk.c:267` `storage_partition_until`, per partition | 2 s each, up to 4 | each disk found |
| `fsvc.c:65` `storage_open_partition_until` | 2 s | each filesystem service started or restarted |
| `usb.c:199` `console_connect_input_until` | 2 s (`CONNECT_WAIT`) | each HID driver started |
| `fsvc.c:273` `fsctl_stop_until` | 5 s (`SYNC_WAIT`) | `mount -w` / `mount -r` |
| `main.c:255`, `bind.c:350`, `main.c:689` waiting for `SIG_TERMINATED` | 15 s (`STOP_WAIT`, `internal.h:20`) | KILL, a remount's stop, shutdown |
| `disk.c:502` `fs_sync_until` | 5 s per volume | shutdown |

While one runs, devmgr binds nothing, restarts no crashed driver, answers
no `GET_SERVICE` (the reconnect rule's second half) and no `MOUNTS`. A
`mount -w` on the 15 GB stick took 0.35 s after M8.6's cache, so the
common case is short; the worst cases are seconds.

**init: the same pattern, not in the claim.** init's control loop waits up
to 25 s for a remount and 15 s for a kill (`user/services/init/ctl.c:
45-47`; ROADMAP's follow-up at line 134-135 names it). The chain shell ->
init -> devmgr -> fat can hold init for 20 s, and while it does init
answers nothing else, Ctrl+Alt+Del's reboot request from the console
included, and restarts no service.

**Not on the data path.** File I/O goes program -> fat -> usb-storage ->
usb-bus directly; devmgr and init only set things up. So daily use sees
this only around plugging, remounting, killing and rebooting.

**Why it matters most at M9.** netstack (lwIP) serves many sockets from
one loop: a `recv` or `accept` must be answered later, when data comes,
not from inside the handler. Today:

- genidl's dispatch is synchronous: the handler returns and the reply is
  written (`tools/genidl.py:425-447`, `_serve_one` at `:520-535`). There
  is no way to keep a request and answer it later. Two services do it by
  hand: hda's `wait_period` (`drivers/hda/irq.c:32-34`) and devmgr's
  `MOUNTS` waiters (`user/services/devmgr/mounts.c`).
- genidl's client stubs are blocking `_until` calls; the only
  asynchronous outgoing calls are hand-written (devmgr's `ask_info`,
  `disk.c:160-174`, with its own txid range).
- usb-bus's task library is the one place where sequential code can wait
  without blocking the loop, and it is private to usb-bus.
- Fourteen processes write their own port loop (`grep -l port_wait`), each
  merging deadlines its own way.

**Recommendation.** Before M9's code, as its first step (about three
agent-hours):

1. A short design section in ARCHITECTURE ("How a service waits"): a loop
   that serves more than one client never makes a blocking call to
   another process, and never waits for a process to end, from inside a
   request; it sends, and takes the answer from its port.
2. genidl: deferred replies (a handler may return "later" and keep a
   small `{channel, txid}` record; a generated `<proto>_<method>_reply`
   answers it) and asynchronous calls (`<proto>_<method>_send` with a
   caller-chosen txid, and a decoder for the reply read from the port).
3. usb-bus's `task.c` as a libos library (`<task.h>`), so a loop can
   still write a multi-step operation as straight-line code that waits at
   each call.
4. netdev (NIC driver to netstack) as shared rings with an event, as the
   mixer's streams are, never a call per packet (claim 7).

Then devmgr's and init's waits can be converted one at a time with those
tools; the storage ones go away with the devmgr split (claim 1, M12).
Convert init's sooner if Ctrl+Alt+Del stuck behind a `mount` is ever seen
on the PC.

## 1: devmgr does too much

**True.** devmgr is 3112 lines (`user/services/devmgr/`): the protocol and
loop (`main.c`, 786), binding (`bind.c`, 401), supervision
(`supervise.c`, 206), USB interfaces (`usb.c`, 402), and storage: disks
(`disk.c` 528 + `disk.h` 76), filesystem services (`fsvc.c`, 290) and
mounts (`mounts.c`, 132): 1026 lines, a third. It serves three channels
(`internal.h:50-57`, `main.c:563`). ARCHITECTURE's table says the same
(`ARCHITECTURE.md:506`).

**The IDL reason is out of date.** `user/include/devmgr.h:9-15` says the
protocol is hand-written "because three replies carry handles, which the
generator can't express". genidl has had handle results since dc82100
(`tools/genidl.py:30-36`). What still keeps devmgr off IDL: handle
*arguments* (`SET_CONSOLE`, `TEST_DISK`; genidl refuses them on purpose,
`genidl.py:37-40`), a reply with a varying number of handles (`MOUNTS`),
and a reply held back until something changes (`MOUNTS`, the deferred
reply of claim 0).

**How much it matters.** For daily use, little: devmgr works and was
signed off on the PC. Its size makes claim 0 worse (storage's waits
block device binding), and it is the one protocol M12 can't review from
an `.idl` file. M9 adds one PCI driver to it (one match-table line), not
a new responsibility.

**Recommendation.** At M12, by one agent: move disks, filesystem services
and mounts (`disk.c`, `fsvc.c`, `mounts.c`; `MOUNTS`, `REMOUNT`,
`TEST_DISK`, `RELEASE`'s disk half) into a volume service (Fuchsia calls
it fshost) that devmgr hands each usb-storage's `storage` channel to,
with supervision shared through a small libos module; then write both
protocols in IDL, with claim 0's deferred replies covering `MOUNTS`.
About three agent-hours with the storage tests and a PC sign-off. Not
before M9: the storage path was just signed off, and the split does not
help networking. Meanwhile, fix devmgr.h's comment (one paragraph).

## 2: access decided by role, not by handle

**Mostly not true.** In each case the server decides by which channel the
request arrived on, and a channel is a handle someone was given. That is
a facet: CODING-GUIDE asks for exactly this ("devmgr's query vs control
channels are two handles, not one handle plus an 'is admin' check",
`CODING-GUIDE.md:22-28`). No server looks at who the caller is: there is
no sender identity in the IPC at all.

- The console's levels (`user/services/console/clients.c:3-8`,
  `abi/idl/console.idl:31-41`) are fixed on a channel when it is made
  (`new_client`, only to a lower level, `clients.c:61-85`); the checks
  are on the channel's level (`clients.c:91,103`, `keys.c:430`).
- devmgr's three channels have a level each (`main.c:563`); the check is
  `b->exclusive && lv == LEVEL_QUERY` (`main.c:327`).
- The root resource's powers were split into rights of their own in M8.6
  ([M8.6-SVC.md](M8.6-SVC.md), "The root resource's rights").

**What is left.** Two weaker spots, neither a role check:

- **The exclusive flag** (`main.c:80`, the match table) puts a policy
  ("hda belongs to the mixer") in devmgr's code, and the channel that
  carries it (`SR_DEVMGR_AUDIO`) exists for one client. M9 brings a
  second case: the NIC's `netdev` channel should reach netstack alone.
  Done the same way, that is a fourth devmgr channel kind or a widened
  audio channel that also reaches the NIC.
- **The console's levels are a fixed ladder**: a client can't be given
  "blank the screen" without everything else SHELL has. Harmless today;
  G1's compositor replaces the console's focus and input routing anyway.

**Recommendation.** Before the NIC driver (or as M9's second step, about
an agent-hour): a devmgr control method that makes a channel scoped to
one device (it answers `GET_SERVICE` and `SUPERVISION` for that device
and nothing else); init makes one for hda and gives it to the mixer, and
one for the NIC for netstack. The scoped channel is then the capability
for that device; `SR_DEVMGR_AUDIO`, `LEVEL_AUDIO` and the per-driver flag
go, and the query channel simply never hands out a device that has a
scoped owner. The reconnect rule still works (the client asks its scoped
channel again). Leave the console's levels alone until G1, and design
G1's seat and focus as facets from the start.

Doc: ARCHITECTURE's "Authority" paragraph (`ARCHITECTURE.md:566-572`)
could say "a level fixed on the channel when it is made" so it doesn't
read as a role check.

## 3 and 5: without an IOMMU, drivers are crash-isolated, not contained

These are one issue. **True.** A driver programs its device through MMIO,
and a DMA address it writes into a descriptor can be anywhere in RAM:
`dma_cap` controls who may pin memory and turn bus mastering on
(`ARCHITECTURE.md:527-540`), not where the device then reads and writes.
`vmo_pin` returns physical addresses "until the IOMMU arrives"
(`ARCHITECTURE.md:529`). Without interrupt remapping a device can also
write the interrupt window (0xFEExxxxx) and send any vector to any CPU.
So a buggy driver can corrupt the kernel through its device, and a
hostile one owns the machine.

What the design does contain, and it is worth saying: a driver's own code
(it can't touch memory it wasn't given, can't program MSI, can't change
another device's config, can't keep its device mastering after it dies:
the safe rebind and the quarantine, `ARCHITECTURE.md:530-540`), and most
untrusted parsing happens in processes with no `dma_cap`: usb-storage
(`drivers/usb-storage/main.c:28-29`), fat, hid, `bin/play`, `bin/jamcover`.
The exception is **usb-bus**: it holds the xHCI's `dma_cap` and parses
every USB device's descriptors (`config.c`, `hub.c`), so a malicious USB
device that finds a bug there has DMA over all of RAM. That is the
largest real exposure today; the network at M9 must not add a second one.

**Where the docs overclaim** (proposed wording after each):

1. `SECURITY.md:8-10`: "Its security model is still worth getting right:
   capabilities, per-program views of services and storage, and drivers
   isolated in their own processes. A way for a program to get something
   it wasn't granted is a real bug."
   Proposed: "Its security model is still worth getting right:
   capabilities and per-program views of services and storage. A way for
   a program to get something it wasn't granted is a real bug. Drivers
   run in processes of their own, so a crashing driver can't take the
   kernel down, but until the IOMMU is in (M11 in the roadmap) a driver
   can program its device to read or write any memory: drivers, and
   usb-bus's parsing of what USB devices send it, are trusted."
   (Also: line 6 says ARCHITECTURE.md "says what it does and doesn't
   defend"; it has no such section yet: see the threat-model paragraph
   below.)
2. `README.md:8-11`: "It is capability-based: a program can do only what
   the handles it holds allow, and every driver and service runs as a
   separate user process that the kernel supervises through those
   handles."
   Proposed, add: "Until the IOMMU work (planned), that keeps a crashed
   driver from taking the system down, not a faulty driver's device from
   writing memory."
3. `ARCHITECTURE.md:36` (table): "IOMMU | Not yet; DMA is gated by
   `dma_cap`, and VT-d will go behind it".
   Proposed: "Not yet (M11). `dma_cap` gates pinning and bus mastering,
   not where a device writes: until VT-d a driver's device can reach all
   of RAM. The PC's firmware has a DMAR table".
4. `ARCHITECTURE.md:50-52`: "A crashing process reports why and where
   ... and can't take the kernel down".
   Proposed, add: "(a driver still can, through its device's DMA, until
   the IOMMU: M11)".
5. `ARCHITECTURE.md:58-62`: "...so one driver can never program another
   device's interrupts or turn DMA back on after it was killed."
   Proposed: "...so one driver can never configure another device's
   interrupts or turn DMA back on after it was killed. What this does not
   cover yet: a device does what its driver tells it, and until the IOMMU
   (M11) a DMA address the driver writes can be anywhere in RAM, the
   kernel included, or the interrupt window. So these rules keep a
   driver's own code in its box and a dead driver's device quiet; they
   don't contain a driver that misprograms its device, by a bug or on
   purpose."
6. `ARCHITECTURE.md:14-16` (Goals): "every driver and service is a user
   process that touches the world only through handles" is true of the
   process; leave it, since (5) now says what the device can do.

**A threat-model paragraph** for ARCHITECTURE (it fits after the table in
"Goals", or as a short section before "The migration rule"):
"What the design defends against today: programs (each gets only its
handles: its list, views of the mounts, the services it was granted);
crashing services and drivers (restarted, the kernel unaffected). Not
yet: a driver that misprograms its device's DMA, or a USB device that
exploits usb-bus (both until the IOMMU, M11); anyone holding the stick
(the ESP, `/data/etc/allow` and the logs can be changed on another
computer); Spectre-class attacks (no mitigations)."

**Recommendation and when.**

- The doc corrections: now, in M8.6 (the docs agent of the code check
  owns these files; the main session applies them with the owner's OK).
  Small.
- An M9 rule, written into M9's row: packets are parsed only in netstack,
  which holds no `dma_cap`; the NIC driver moves buffers between its
  rings and netdev's and never looks inside a packet beyond what the
  hardware needs. Then a remote attacker's first bug lands in a process
  without DMA power, and M9 doesn't wait for M11.
- M11 stays where it is (after M10.5, before M11.5 and the interfaces
  freeze), and must come before G3 at the latest (an iGPU driver is the
  most powerful DMA user). Its "done when" should also cover interrupt
  remapping: a device's write to the interrupt window sends nothing.
  Known now: the PC's ACPI has a DMAR table (boot log of 2026-09-30:
  `acpi: ... DMAR ...`), so VT-d is on in its firmware.
- Cheap hardening meanwhile (optional): fuzz usb-bus's descriptor
  parsers (`config.c`, the hub code) in the host-side unit tests, as
  hid's report parser already has fixtures.

## 4: the scheduler's tuning

**True in count.** Placement in hybrid order (with work stealing that
sends a stolen thread on, `ARCHITECTURE.md:425-440`, switch
`noplaceorder`), client/server pairs on sibling hyperthreads (`:455-458`,
`noaffinepair`), spin before idle (`:459-465`, `nospinidle`/`idlespin=`),
and wake-affine hand-off for `channel_call` (`:448-453`, no switch). The
claim's "direct hand-off" is not built: it is M11.5's plan
(`docs/ROADMAP.md:77`). `kernel/sched/sched.c` is 1027 lines, over the
guide's 800.

Each was measured on the PC with its switch off and on in one run
(`docs/BENCH.md:85-121`): spin before idle halves cross-CPU wakes,
placement fixed the M5 regression, pairs cut a wake from 1080 to 470 ns.
But:

- the benchmark has not run on the PC since M5.5 (`BENCH.md:57-59`);
  the stealing change of 2026-10-01 is "not yet measured on the PC"
  (`BENCH.md:169-177`);
- two of these together caused a test panic that only the PC showed (a
  CPU taking its next thread looked idle to placement; stealing ignored
  the core layout: `docs/HISTORY.md:114-116`);
- nothing runs the soak with the switches off, so the plain paths are
  tested only by the benchmark's off halves (`kernel/test/bench.c:220`;
  `m55` is a benchmark row, not a boot word).

**How much it matters.** Low for daily use (it works and was signed off);
the risk is the next heuristic. M10's tickless idle changes the idle path
that spin before idle sits in, and M11.5's direct hand-off is another
path through the same code.

**Recommendation.** Keep the existing rule (`ARCHITECTURE.md:498-500`)
and make it stricter: a new heuristic comes with a switch and a BENCH line
showing a clear win on the PC, or it doesn't go in. Run "Benchmark" on the
PC at M8.6's sign-off and record the column (it also measures the PCID
+20 ns question). Once per milestone, a QEMU soak booted with
`noplaceorder noaffinepair nospinidle`. At M10, re-measure spin before
idle against tickless idle's wake latency and drop whichever stops
paying. A pluggable scheduler (`sched_ops`) stays off the table, as the
roadmap says.

## 6: restarting a service is seen by its clients

**True, and documented.** The roadmap says so and plans the fix (M11.6,
`docs/ROADMAP.md:78`). Today: a dead fat closes its clients' files
(`ERR_PEER_CLOSED`; the reconnect rule gives a new mount, not the open
files back); a dead mixer is restarted by init on the same channels
(`ARCHITECTURE.md:833-834, 855-856`), and its clients open new streams:
the music player does it by itself (`user/services/music/player.c:
337-345`, three tries), losing what was in its ring (up to 1.37 s), and
`play` ends.

**How much it matters.** Low for daily use: fat and the mixer have not
been seen crashing on the PC; a crash is logged and noticed. It matters
for the claim "a crashed driver doesn't matter", which today means "the
system goes on", not "nobody notices".

**Recommendation.** Keep M11.6 as planned, after M11.5's call path and
before M12. One thing to do earlier: M9's netdev rings and socket state
should be laid out so they could live in VMOs the service is handed back
(as M11.6 describes), which costs nothing if decided at design time.

## 7: the cost of IPC

**The numbers are out of date.** The claim's 1.5 us same CPU and 3-3.6 us
across cores are M5's (`BENCH.md:42-45`: 1490, 2956 and 3620 ns). Since
M5.5 a process-to-process call is 1407 ns on one CPU, 1195 ns to the
hyperthread sibling, 2111 ns to another P-core and 2106 ns to an E-core
(`BENCH.md:117-121`). Not re-measured since; the code has changed a lot.

**"Every file and audio operation goes through it" is partly wrong.**
Sound samples don't: a client writes into a shared ring and the event is
signalled only when one side waits (`ARCHITECTURE.md:838-845`). File data
moves through a shared 64 KiB buffer, one call per 64 KiB at most
(`abi/idl/file.idl:2-4`), so a 2 us call is about 30 ns per KiB; next to
a USB stick's transfer time it is noise. Where calls do add up: small
operations (`stat`, `readdir` is one call per entry, `fs.idl:25`) and
the fixed-size arrays every call copies (a 256-byte path in each fs
request, 2048 bytes in every `console.write`: the roadmap's
variable-length-arrays follow-up). The breakdown of a call is known
(`BENCH.md:146-152`): ~120 ns for the address-space switches, ~735 ns on
the user side.

**Where it matters.** M9: at 2.5 GbE a full link is ~200,000 packets a
second each way; at 2 us per call per packet per hop that is 40 % of a
core per hop, so per-packet calls can't be the design. G1: a compositor's
traffic is many small messages per frame. Not storage or sound today.

**Recommendation.** netdev as shared rings with batched events from the
start (claim 0, point 4): the mixer's design, which already works. Keep
M11.5's 600 ns target and its per-operation lines. Variable-length IDL
arrays belong to M12's interface review (or M11.5, as a copy saved).
Promote the "shared request rings" design idea's first user to M9's
netdev (see the roadmap changes below). Run the benchmark on the PC
(claim 4) so the next comparison isn't against M5.5.

## 9: FAT32 as the only filesystem

**Partly true.** Authority in Jam OS never came from the filesystem, by
design: "The 4 GiB file limit and the lack of owners/permissions are
accepted: authority comes from namespaces, not the filesystem"
(`ARCHITECTURE.md:1027-1028`; `:37`, "FAT32 can't store owners anyway").
A program's reach is its views (read-only, or with `etc` guarded) and its
list, which owners and mode bits would not improve on. `allow` is not a
stand-in for an execute bit: it binds the owner's approval to the exact
bytes (a SHA-256), which an execute bit can't, and the check can't be
raced (`vmo_make_exec`, `ARCHITECTURE.md:716-735`).

What FAT32 does cost:
- **Integrity at rest**: anyone holding the stick can change `/data`
  (including `/data/etc/allow`) on another computer. Accepted and said:
  `ARCHITECTURE.md:729-732`, decision 11 in M8.6-PLAN.md. No filesystem
  fixes that without keys the system doesn't have.
- **Crash consistency**: no journal, no fsck; write ordering and the
  clean bit only (`ARCHITECTURE.md:946, 956-963`). A reliability limit,
  not a security one; the pulled-plug test passed on the PC.

**Recommendation.** No change: the owner declined other filesystems
(`docs/HISTORY.md`, 2026-09-30). Put "anyone holding the stick" in the
threat-model paragraph (claim 3). At M13 the POSIX layer makes `st_mode`
from what a program holds (a read-only view: no write bits; `allow`:
execute), so ported programs see sensible modes. Revisit only if `/data`
ever holds secrets.

## 10: start order hard-coded in devmgr and init

**True, but smaller than it sounds.**

- init's order is a list (`user/services/init/services.c:556-567`:
  bootfs, console, splash, serialin, devmgr, mixer, music, logd, shell),
  and what a restart of one means for the others is code
  (`services_closed`, `services.c:569-613`); reboot stops the sound's
  clients before devmgr by hand (`ARCHITECTURE.md:1130-1134`).
- But init already makes the mixer's and the music player's channels once
  and keeps their server ends (`services.c:615-628`,
  `ARCHITECTURE.md:833-834`), so a client's call simply waits on the
  channel until the service serves it: for those, start order and
  restarts don't matter to clients. That is the useful half of a
  dependency graph, already built.
- devmgr's stop order (filesystems, then USB class drivers, then bus
  drivers: `main.c:668-677`) is the device tree's order, children before
  parents. A graph would restate the tree.

**How much it matters.** Low now (nine services). M9 adds netstack, DHCP,
DNS and netlog, with real dependencies (netlog wants the network early,
DHCP needs netstack, everything stops before a kexec).

**Recommendation.** For M9: every init service gets its channels made
once by init (the mixer's pattern), so start order stops mattering for
clients; devmgr's channels move to that pattern too (today they are
remade per devmgr, `services.c:575-586`). The stop order becomes a rank
in init's service table instead of code in `reboot.c`. Keep the "service
dependency graph" design idea for when that stops being enough; not
before M12.

## Other drift found on the way

- `README.md:66-67`: "Not yet: networking, power management, running
  programs from `/data`": programs on `/data` run since M8.6 (`allow`).
- `SECURITY.md:6`: points at ARCHITECTURE for "what it does and doesn't
  defend"; there is no such section (claim 3's paragraph fixes it).
- `user/include/devmgr.h:9-15`: the IDL reason (claim 1).
- `docs/ROADMAP.md:127-129`: the usb-bus follow-up (claim 0).

## Suggested roadmap changes

For the main session to apply with the owner's OK. `docs/ROADMAP.md` at
020f81f.

1. **Smaller follow-ups**, replace the bullet at lines 127-129 ("One bulk
   transfer at a time inside usb-bus's loop, ...") with:

   > - usb-bus runs one bulk transfer at a time across the controller.
   >   Since M8.6 every port and device has a task of its own, so
   >   enumeration, control transfers and the keyboard and mouse carry
   >   on, but a stick that stops answering holds the other sticks'
   >   transfers for up to its command's timeout (5 s read, 10 s write).
   >   A transfer in flight per endpoint removes that (local to
   >   `bulk.c`). One Bulk-Only stick still runs one command at a time:
   >   the protocol has no queue.
   > - devmgr and init are single-threaded loops that wait inside a
   >   request: devmgr up to 2 s per call to a usb-storage, 5 s for a
   >   filesystem's stop and 15 s for a killed driver; init up to 25 s for
   >   a `mount` and 15 s for a `kill`. Meanwhile devmgr binds and
   >   restarts nothing and init answers nothing, Ctrl+Alt+Del included.
   >   M9's first step gives loops the tools to stop doing this
   >   ([ARCH-CHECK.md](history/ARCH-CHECK.md)).

   and delete "init's loop waits up to 25 s for a `mount` and 15 s for a
   `kill`" from the bullet at lines 132-135 (now in the one above).

2. **M9's row** (line 73), prepend to "What":

   > First, how a service waits ([ARCH-CHECK.md](history/ARCH-CHECK.md),
   > claim 0): a loop serving several clients never blocks on a call;
   > genidl gains deferred replies and asynchronous calls; usb-bus's
   > tasks become a libos library. Then a devmgr channel scoped to one
   > device, so the NIC's channel reaches netstack alone (and hda's the
   > mixer, replacing the audio channel). netdev is shared rings with
   > events, never a call per packet; packets are parsed only in
   > netstack, which holds no `dma_cap`; init makes each new service's
   > channels once.

   and add to "Done when": "a service waiting on a slow peer delays only
   that peer's requests (a test)".

3. **M11's row** (line 76), "Done when" becomes: "DMA outside a driver's
   pinned VMOs is blocked, and a device's write to the interrupt window
   sends no interrupt; ARCHITECTURE then counts drivers as contained, not
   only crash-isolated". Add to "What": "(the PC's firmware has a DMAR
   table). Before G3 at the latest."

4. **M12's row** (line 79), add to "What": "devmgr's disks, filesystem
   services and mounts move into a service of their own, and devmgr's
   protocol into IDL; variable-length IDL arrays."

5. **Design ideas**, "Shared request rings" (lines 96-99), add: "The
   first user is M9's netdev; then the fs and file calls (M11.5)."
   "A service dependency graph" (lines 100-102), replace with:

   > - **A service dependency graph.** devmgr stops in device-tree order;
   >   init starts and stops by a list. init already makes the mixer's
   >   and the music player's channels once, so their clients don't
   >   depend on start order; done for every service (M9's first), what
   >   is left is the stop order, a rank in init's table. A declared graph
   >   is for when that stops being enough.

6. **The rest** (line 150), replace "devmgr's protocol is hand-written,
   not IDL." with: "devmgr's protocol is hand-written, not IDL: handle
   results are in genidl now (devmgr.h's reason is out of date); what
   remains is handle arguments, a varying number of reply handles and
   the held-back `MOUNTS` reply. M12."

7. **A new follow-up** under "The rest": "The benchmark has not run on
   the PC since M5.5 (BENCH.md): run it at M8.6's sign-off. Once per
   milestone a QEMU soak with the scheduler's switches off
   (`noplaceorder noaffinepair nospinidle`). A new scheduler heuristic
   needs a switch and a BENCH line showing a clear win on the PC."

8. **The doc corrections of claim 3** (SECURITY.md, README.md,
   ARCHITECTURE.md lines 36, 50-52, 58-62, and the threat-model
   paragraph) are not roadmap rows: apply them in M8.6.
