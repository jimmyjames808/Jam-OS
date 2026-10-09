# M12 review P: the protocols

Stage A of [M12-PLAN.md](M12-PLAN.md#stage-a-the-reviews-findings-first),
review P. Docs only: nothing here is built. Read against main 56aa8904.

What was read: the 26 files in `abi/idl/`, `tools/genidl.py` and what it
writes (`drivers/include/idl/`), the hand-written protocols (devmgr's in
`user/include/devmgr.h`, the keep channel in `user/include/keep.h` and
`user/lib/keep.c`, the namespace's messages in `user/include/os.h` and
`user/lib/ns.c`, the startup and promotion messages in
`kernel/include/jam/startup.h`, `console.open_keys`'s messages in
`kernel/include/jam/abi.h`, the update offer in `user/include/update.h`,
the state VMO in `user/include/svcstate.h`) and the shared-memory
layouts (`user/include/mixer.h`, `user/include/sockring.h`,
`drivers/include/jam/netdev.h`), with their callers. Reading the callers
found eight more small hand-written channels (the splash's, logd's crash
result, serve's `give`, init's note to the first shell, usb's interrupt
reports, `SR_STDOUT`, the stop channels of play, fetch and speed, and the
crash-test driver's protocol), and two copies of devmgr's protocol
outside `user/include/devmgr.h`.

Every finding is read in the light of
[the owner's answers](M12-PLAN.md#the-owners-answers-2026-10-07):
handles given to a send are always moved; `event` methods are one-way
and may carry handles; `u8[<=N]` and a checked `str[<=N]`; a protocol per
kind of channel; the storage split into volumes; disk protocols shaped
for GPT and NVMe; the file protocol changed now; the five new error
codes; deadlines, with IDL timeouts only where the server bounds a wait
the caller can't; `nocomp` removed; the character out of input reports.
The compositor's protocol (`compctl.idl`, JWL1, the clipboard) is review
W's and devmgr's storage side and the new volumes protocol are review
S's: this file only notes where they meet the rest.

"Callers" counts files outside `drivers/include/idl/` that include the
protocol's header or build its messages (tests included); "breaks"
means a caller must change, "adds" that existing callers keep working.

**In short:** 60 findings: 2 bugs today (one latent, one improbable), 9
inconsistencies, 30 needs of M12's own decisions or of M13, 19
cleanups. Five design questions are new. Nothing found needs a change
to the owner's answers, but Q3's own trigger for records ("until a third
case asks") is met by M12's changes (question D2).

## Findings

Kinds: **bug** (wrong today), **inc** (an inconsistency), **need** (what
M12's decisions or M13 need), **clean** (a cleanup). "Track" is the
plan's [Stage C](M12-PLAN.md#stage-c-the-fixes) track that would carry it.

### genidl and what every protocol shares

| # | Kind | Where | What | Proposed change | Breaks / adds | Callers | Track |
|---|---|---|---|---|---|---|---|
| 1 | **bug** (latent) | `user/include/devmgr.h:102`, `abi/idl/idltest.idl:10`, `tools/genidl.py:1252-1256` | Plan item 17, confirmed: devmgr's hand-written protocol id 3 is idltest's. `DEVMGR_STATUS` (0x00030001) is `IDLTEST_ECHO`'s wire ordinal. Only utest serves idltest, so nothing is misrouted today, and the sizes differ (16 bytes against 12), so a stray request is answered `ERR_INVALID_ARGS` rather than run as the other method; but genidl only checks ids among `.idl` files. devmgr's comment still says "next to the IDL's null (1) and edu (2)". | **Fix now, small:** idltest takes a free id (33), `make idl`; its two utest files use names only. devmgr gets its own id when track D writes its IDL. | adds (idltest is test-only) | 2 | main session now, then G |
| 2 | inc | `tools/genidl.py:1252-1256` | No registry of protocol ids. Ids and magics outside the `.idl` files: devmgr 3 (literal ordinals in `user/include/devmgr.h`, `kernel/test/test_devmgr.c:51-53`, `user/tests/utest/drivers.c:173`); the crash-test driver's 0xc4a5 (`user/include/devmgr.h:361-363`, `drivers/test/crasher/crasher.c`); and every hand-written message whose second word an IDL server would read as an ordinal: the startup message's magic "JAMS" (id 0x534d), the promotion's "PROM" (0x4d4f), the update offer's "JUPO" (0x4f50) and answer's "JUPA" (0x4150); keep's kinds 1-4 and the namespace's 1-3 (id 0). | One registry, abi/idl/ids.txt (a new file): one line per id with its name and where it lives (an `.idl`, or a header for one that stays hand-written). genidl reads it, refuses an `.idl` whose id isn't listed under its name and an id listed twice, and writes the list into `drivers/include/idl/common.h`. Id 0 is reserved for "no IDL message" (finding 13). | adds | 1 (genidl) | G |
| 3 | need | `tools/genidl.py:137` | `MSG_MAX = 8192`, "both sit on a kernel stack". The comment is wrong: the kernel's limit is 65536 (`kernel/include/jam/channel.h:39`) and the request and the reply sit on the **server's** stack (`<proto>_serve` and `<proto>_serve_one` put `REQ_MAX` + `REP_MAX` there; so does `user/lib/fsview.c`). And Q8's paths (`str[<=4096]`) make `fs.rename`'s request 8 + 2 x 4098 = 8204 bytes: genidl would refuse it. Today's largest is console's 2058. | `MSG_MAX` 16 KiB; the generated loops take their buffers from the heap once (or from the caller) when `REQ_MAX + REP_MAX` passes 4 KiB; the comment fixed. | adds (generated) | 0 | G |
| 4 | need | `user/include/devmgr.h:199-211`, `user/include/keep.h:77-89`, `user/include/os.h:254-260`, `user/include/update.h:190-202` | Q1 makes handle arguments safe, but every hand-written protocol that would move to IDL carries a *varying* number: devmgr's (volumes') mount list, keep's restore (up to 64), the namespace's (up to 24), the update offer (2 or 3). | `handle[<=N]` (N up to 64), as an argument or a result: the count is the message's own handle count, C gets a pointer and a count. With the typed handles of question D4. | adds | 0 | G (second hand-back, after K1) |
| 5 | **bug** (improbable) | `tools/genidl.py:96-100`; `user/services/init/comp.c:89-90,133,138,145,192,236,354` | genidl's rule "don't mix blocking and asynchronous calls on one channel end" is broken by init: on the compositor's ADMIN channel it makes blocking calls (`new_client`, `connect_input`, `blank`, `set_layout`) and asynchronous ones with fixed txids (`LAYOUT_TXID` 0x1a70000, `NOTE_TXID` + n, `NOTE_WAIT`). A blocking call whose kernel txid (one global counter today, `kernel/object/channel_send.c:21`) equals an outstanding asynchronous txid takes that reply (`ERR_INTERNAL` there) and the `layout_wait` answer is lost. Improbable, not impossible. | With K1's per-pair counter (D1): txids the kernel stamps have bit 31 set and `idl_txid_next` stays below it. Mixing is then safe, the rule goes from genidl's doc, and init needs no change. | adds | 1 (+ every async user, unchanged) | K1 + G |
| 6 | need (Q1) | `drivers/include/idl/common.h` (`idl_reply_write`, `idl_serve_next`, `idl_reply_tried`), every `<proto>_serve_one`; `user/lib/fsview.c:150`, `user/lib/svcserve.c:50-51`, `user/lib/keep.c:80,418`, `user/lib/ns.c:704,754`, `user/services/devmgr/chans.c:70,135` | Once handles are always moved, code that closes them after a failed write closes handles it no longer has: harmless until the number is reused by a new handle, then it closes someone else's. A grep finds about 46 such sites in about 40 files; the ones above are the protocol libraries every server shares. | In K1's merge: generated code and these libraries stop closing after a failed write (only after a failure before it, e.g. a duplicate that failed). K1 counts the rest of the tree as the plan says. | breaks (run time only) | ~40 | K1 + G |
| 7 | clean | `tools/genidl.py:1032` (`gen_protocol`) | A generated header carries the methods' comments but not the file's: the restart rule genidl insists on, and which channels speak the protocol, are invisible to a reader of `<idl/fs.h>`. | Copy the file comment into the header. | adds | 0 | G |
| 8 | need | `user/include/svcstate.h:73,84`; `user/services/fat/request.c`, `user/services/mixer/streams.c:422` | A protocol served from request slots (`fs`, `file`, `audio`, `audioctl`) can't replay handles: they die with the instance. `<proto>_run_slot` refuses a request with handles, which is right only while no method takes any. | A protocol-level mark, `protocol fs 16 kept`: genidl refuses handle arguments in it, and the mark replaces the prose check for "Restart: ... not seen". | adds | 4 servers | G |
| 9 | clean (M11.6's names) | `tools/genidl.py`; `user/tests/utest/idlslot.c:171-180` | `idempotent` and `<proto>_idempotent` are read by nothing but utest: no server consults them. `take_slot`, `run_slot`, `REQ_MAX`, `REP_MAX`, `_serve`, `_within` read well and are used. | Keep `idempotent` as the note for the drivers that outlive their process (the milestone after M12), and say "read by nothing yet" in genidl's doc. The other names stay. | none | 0 | G |
| 10 | need | across `abi/idl/` | A protocol's numbers live away from its `.idl`: `FS_*` flags in `user/include/os.h`, `INPUT_*` in `kernel/include/jam/abi.h` (plan item 7), console's and compctl's levels as bare 0, 1, 2, USB speeds written out in both `usb.idl` and `usbbus.idl`, `DEVMGR_SUP_*`, `MIXER_STATE_*`, hda's jack states, `NET_WAIT_FOREVER`. | Question D3 (constants in the IDL). | adds | ~30 | G |
| 11 | need (Q4) | see per-protocol notes | Which methods become events: `input` (all four), `usbbus.interface_attached`, keep's four kinds, the namespace's three, `console.open_keys`'s three, the splash's two, logd's crash result, init's note to the shell. Five of them flow from the server's end to the client's. | Question D1 (which way an event goes). | breaks (each protocol's own track) | see rows | G, then P1, P4, P6 |
| 12 | inc | `user/include/os.h:254-260`, `user/include/keep.h:70-89`, `kernel/include/jam/startup.h:56-97`, `user/include/update.h:193,307`, `user/services/init/terms.c:248`, `user/include/serve.h:22-33`, `user/include/crashlog.h:18-22`, `user/include/splash.h` | The first word of a hand-written message: a txid of 0 (namespace, keep, startup, promotion, update offer and answer), a kind (init's note to the shell: 2), a magic (`serve_give`), a status (`serve_answer`, `crashlog_result`), a bare kind (the splash). An IDL server that is sent one takes its first word for a txid and answers it. | Hand-written messages that stay start with u32 0, then a registered word (finding 2). Most go to IDL instead (findings 17, 31, 46-50). | breaks (small) | ~12 | P6 and owners |
| 13 | inc | `user/services/console/keys.c:400-404`, `user/services/compositor/sources.c:53-57`, `abi/idl/audio.idl:33`, `abi/idl/audio.idl:85`, `abi/idl/initctl.idl:95`, `abi/idl/compctl.idl:75` | Strings: every server checks its own NUL, length and characters, and the policies differ: audio's `name` and `stream_set_title` replace bad characters with '?', initctl's `terminal` and compctl's `notify` refuse control characters. | With `str[<=N]` the generated server refuses bad UTF-8 and a NUL; a server's own rule for control characters stays, written as one rule: labels shown to people replace them, commands and names refuse them. The hand-written length checks go. | breaks (tiny) | ~10 | each P track |
| 14 | clean | `abi/idl/console.idl`, `compctl.idl`, `hda.idl`, `audioctl.idl`, `netctl.idl`, `net.idl`, `fs.idl`, `user/include/devmgr.h` | Eight protocols have narrower channels (levels: console's three, compctl's three, devmgr's four, hda's query channel, audioctl's desktop channel, netctl's read-only one, net's shared channel, fs views), each by hand and each answering `ERR_ACCESS_DENIED` consistently. | No change: a level is not a kind of channel, so Q5 doesn't split them (it would double the protocols for nothing). Noted so the P tracks don't. | none | 0 | none |

### Input and the console (P1)

| # | Kind | Where | What | Proposed change | Breaks / adds | Callers | Track |
|---|---|---|---|---|---|---|---|
| 15 | need (Q4) | `drivers/hid/hid.c:165,190,360`, `user/services/serialin/main.c`, `abi/idl/input.idl:21-33` | Plan item 20: hid waits for the compositor's answer to every key and mouse report (`input_key_until` with `INPUT_TIMEOUT`). | `input` as four events (`key`, `mouse`, `text`, `ready`); the queue's limit is the flow control; hid drops a report the full queue refuses (mouse reports carry the whole button state; a lost key is logged). | breaks | 12 | P1 |
| 16 | need (Q15) | `kernel/include/jam/abi.h:379-384`, `abi/idl/input.idl:9-17` | `struct input_key_event` is both `input.key`'s report and `console.open_keys`' message. Q15 takes the character out of the report only: a keys client (the shell, a program) still needs it. | `input.key (u16 usage, u8 state, u8 mods)`; the keys channel's key event keeps `codepoint`, which the console fills from libos's keymap. The struct leaves the kernel header (item 7). | breaks | ~10 (hid, compositor sources, console keys, tests) | P1 |
| 17 | need (Q4, Q5) | `kernel/include/jam/abi.h:362-409` | `console.open_keys`' channel is a hand-written protocol in a kernel header, its three messages told apart by their size (key 8 bytes, mouse 12 console to client; want 8 client to console). | An IDL protocol of its own (`keys`, in `console.idl` once a file may hold several): events `key`, `mouse` from the console and `want` from the client (question D1). The definitions leave `<jam/abi.h>`. | breaks | ~9 (console, shell's `main.c`, `sh_io.c`, `sh_paste.c`, contest, utest conwin, consel; libfun until Q14 removes it) | P1 |
| 18 | need (Q3) | `abi/idl/console.idl:14`, `abi/idl/input.idl:27` | `console.write` sends 2048 bytes for every line; `input.text` 64 with its own `length`. Every caller pads a 2048-byte buffer by hand (`user/apps/fun/util.c:89`, `user/services/init/reboot.c:170`, `user/services/shell/main.c:60`, contest). | `write (u8[<=2048] text)`, bytes rather than `str` (a program's output may be any bytes, and the terminal shows them); `text (u8[<=64] bytes)`. Both `length` arguments and their checks go. | breaks | 6 + 2 | P1 |
| 19 | need (Q14) | `abi/idl/console.idl:1-9,21-36,67-74`, `user/include/devmgr.h:152-159`, `abi/idl/initctl.idl:36-38`, `user/include/devmgr.h:289-291`, `user/include/os.h:332-334` | With `nocomp` gone: `connect_input` (5) and `lend_screen` (6) go (ordinals not reused); console.idl's opening ("owns the screen after boot"), its restart paragraph ("a lent screen and input connections") and `set_font`'s full-screen sentence go stale; devmgr's `SET_CONSOLE` then always carries a compctl INPUT channel and devmgr no longer needs `<idl/console.h>` (`user/services/devmgr/usb.c`); "not under `nocomp`" in devmgr.h and os.h, and initctl.reboot's "under `nocomp` the caller does (console.blank)". | Remove and reword in P1; `SET_CONSOLE` renamed `SET_INPUT` in track D. | breaks | 5 (console callers of the two methods) | P1, D |
| 20 | clean (stale) | `abi/idl/input.idl:1-17` | input.idl says the console serves `input` and decides about Ctrl+Alt+Del; the compositor does both (`compctl.connect_input`, its reserved keys). | Rewrite with finding 15. | none | 0 | P1 |

### Sound (P2)

| # | Kind | Where | What | Proposed change | Breaks / adds | Callers | Track |
|---|---|---|---|---|---|---|---|
| 21 | need (Q5) | `abi/idl/audio.idl:11-12,30-87`, `user/services/mixer/streams.c:272,372,422` | One protocol on two kinds of channel: the opener's (`open_output`) and the stream's (`stream_*`, prefixed "so the generated audio_stream_* calls stay clear"). The mixer already serves them with two ops tables. | `audio` (open_output) and `audio_stream` (start, stop, drain, position, set_volume, levels, stats, set_title: prefixes gone). The mixer runs both from one slot: its state layout version goes up and `tools/mixer-restart-test.sh` runs (the plan's rule). | breaks | 8 | P2 |
| 22 | need (Q5) | `abi/idl/hda.idl:47-87`, `drivers/hda/irq.c:187-190,269-278` | hda's stream channel speaks start, stop, position and wait_period, refused on the driver's channel by a second ops table. | `hda` and `hda_stream`; the query channel stays a level of `hda` (finding 14). | breaks | 8 | P2 |
| 23 | need (Q3) | `abi/idl/hda.idl:45,121,136`, `abi/idl/audioctl.idl:30,52`, `abi/idl/audio.idl:44,87` | Fixed arrays with a count beside them or text padded with NULs: hda `info` (`count` + `u8[8] nodes`, `u8[240] text`), `jacks` (`count` + two `u8[16]`, `u8[1024] text`), `output_name u8[48]`; audioctl `streams` (`count` + `u8[640]` of 40-byte records), `desk`'s `title u8[64]` and `output u8[48]`; audio's `name u8[16]`, `title u8[64]`. | `u8[<=8] nodes`, `u8[<=16] pins, states` (counts go), texts and names as `str[<=N]`; `streams` as `u8[<=640]` of the same records (Q3: records stay bytes). | breaks | 11 | P2 |
| 24 | clean (stale) | `abi/idl/audioctl.idl:31-32` | "as audio.set_volume on its own channel": the method is `stream_set_volume`. | Reworded with finding 21 (`audio_stream.set_volume`). | none | 0 | P2 |
| 25 | clean | `abi/idl/music.idl:30,58-64` | Plan item 25: `start` is `play` with no first file and shuffle. | One method: `start (str folder, str first, u8 order)` at a new ordinal, `play` and the old `start` gone. | breaks | 4 (`user/apps/jamjar/link.c`, the shell's `music`, init's settings, the player) | P2 |
| 26 | need (Q8, Q9) | `abi/idl/music.idl:30,46,64`, `user/services/music/tracks.c:100` | music's paths are `u8[256]` (folder, first, the status's path and folder) beside titles and notes of 128; "it is a file" is `ERR_WRONG_TYPE`. | Paths `str[<=4095]` like the file protocol's, titles and notes `str[<=127]`; "it is a file" `ERR_NOT_DIR`. | breaks | 4 | P2 |

### The network (P3)

| # | Kind | Where | What | Proposed change | Breaks / adds | Callers | Track |
|---|---|---|---|---|---|---|---|
| 27 | need (Q5) | `abi/idl/net.idl:26-36`, `user/services/netstack/clients.c:331-345`, `user/services/netstack/sock.c:427`, `user/services/netstack/tcpsock.c:252,351` | Four kinds of channel in one protocol (opener, UDP socket, TCP socket, listener) plus the shared channel, told apart by `sock_*` names and five ops tables. | `net` (the opener's methods), `net_socket` (UDP and netctl's DHCP socket: connect, state, rings), `net_stream` (TCP: state), `net_listener` (accept), each numbered from 1. The shared channel stays a level of `net` (svc.connect, iface, counts). | breaks | 14 | P3 |
| 28 | need (Q5) | `abi/idl/netdev.idl:41-52` | A session channel answers info and stats; `open` on it is refused by hand. | `netdev` (info, stats, open) and `netdev_session` (info, stats). | breaks | 7 | P3 |
| 29 | need (Q3) | `abi/idl/net.idl:84,89`, `abi/idl/netdev.idl:39`, `abi/idl/dns.idl:44`, `abi/idl/serve.idl:33,38`, `abi/idl/netdev.idl:35`, `abi/idl/netctl.idl:62,77` | `counts`, `chip_counts`, `stats` are `u8[256]` holding a C struct with reserved fields; dns's name `u8[256]` and its answer `count` + `addr0` to `addr3`; serve's names `u8[128]`; the chips' names `u8[16]`. | Counts as `u8[<=256]` (a shorter answer reads as zeros: room to grow); dns name `str[<=253]` (a DNS name's limit), addresses `u32[<=4]` (question D2) or `u8[<=16]`; names as `str`. | breaks | ~15 | P3 |
| 30 | inc (Q19) | `abi/idl/net.idl:81,112,190`, `abi/idl/dns.idl:44`, `abi/idl/usbbus.idl:25`, `abi/idl/usb.idl:61,64` | The seven IDL timeouts all fit Q19's rule (the server bounds a wait the caller can't: a later answer holding one of the server's slots, a resolver query, a hardware transfer that must be stopped), so all seven stay. But their ranges differ: `NET_WAIT_FOREVER` (wait_change, accept), 0 for "at once" (accept), 1..60000 (echo, resolve), up to 60000 with 0 refused (bulk), anything (wait_settled). | One rule: `timeout_ms` 1..60000, a protocol constant for "for ever" where a wait may be endless (wait_change, accept), 0 only where it means "don't wait" (accept). | breaks (values) | ~12 | P3, P4 |
| 31 | need (Q1) | `abi/idl/serve.idl:16-33`, `user/include/serve.h:18-33` | serve's `share` answers a `give` channel on which the caller writes `struct serve_give` with two handles and reads `struct serve_answer`: a hand-written protocol (magic first, no txid) that exists only because genidl refused handle arguments. | `share (u16 port, str[<=127] name, u64 size, handle file, handle buffer) -> (u16 port) later`; the `give` channel, `SERVE_GIVE_WAIT` and both structs go. | breaks | 3 | P3 (after G's second hand-back) |
| 32 | need (M13, adds) | `abi/idl/net.idl:131,190` | No socket's local address, and no TCP peer after `accept` (sock_state has a UDP peer only); no TCP half-close of receiving, no options (no-delay, address reuse); IPv4 addresses only. POSIX's getsockname, getpeername, shutdown and setsockopt need them. | While P3 is in that code: `net_stream.state` answers the local and peer address and port. The rest is M13's (additions), IPv6 later still. | adds | 0 | P3 |

### USB (P4)

| # | Kind | Where | What | Proposed change | Breaks / adds | Callers | Track |
|---|---|---|---|---|---|---|---|
| 33 | inc | `abi/idl/usbbus.idl:38-44`, `drivers/usb-bus/serve.c:192`, `user/services/devmgr/usb.c:327-355`, `user/include/devmgr.h:115-117` | `interface_attached` is written on DR_SERVE, the channel whose client end devmgr hands duplicates of to anyone who asks (`DEVMGR_GET_SERVICE`: the shell's `usb`, usbtest). devmgr reads that end for its events and drops whatever else it finds as "a late reply nobody waits for": a holder that used `_send` would lose its replies to devmgr, or take devmgr's attach events. Nobody does today; a comment forbids reading. | Q4 and Q5: usb-bus's events go on a channel of their own (a new driver role), as `usbbus_events.interface_attached (..., str path, handle usb)` (an event with a handle); DR_SERVE speaks `usbbus` only and its generated client stub for ordinal 100 goes. | breaks | 4 (usb-bus serve.c, devmgr usb.c and bind.c, usbtest) | P4 (+ D for the role) |
| 34 | clean | `abi/idl/usbbus.idl:23-25`, `drivers/usb-bus/serve.c:46-50,349-380,566-607` | `wait_settled` is answered later by a hand-written loop beside the generated dispatch (it predates `later`). | Mark it `later`, answer with `usbbus_reply_wait_settled`. | none | 1 | P4 |
| 35 | need (Q3) | `abi/idl/usb.idl:23,29,31`, `abi/idl/usbbus.idl:29,33,44` | `get_descriptor` and `control_in` answer `actual` + `u8[1024]`; `control_out` takes `length` + `u8[64]`; usbbus's `path`, `product_name`, `serial` are NUL-padded fields, `endpoints` a `u8[8]` beside `num_endpoints`. | `-> (u8[<=1024] data)`, `control_out (..., u8[<=64] data)` (`actual` and `length` go); the names as `str`; `endpoints u8[<=8]`. | breaks | 7 + 6 | P4 |
| 36 | clean | `abi/idl/usb.idl:32-38` | `open_interrupt_in`'s `reports` channel carries raw report bytes, no header: a channel of byte messages, as Q17's pipe. | Stays hand-written by design; named in the list of hand-written channels (ARCHITECTURE). | none | 0 | J |

### Files (P5)

| # | Kind | Where | What | Proposed change | Breaks / adds | Callers | Track |
|---|---|---|---|---|---|---|---|
| 37 | need (M13) | `abi/idl/fs.idl:39-40` | `unlink` removes a file or an empty directory alike. POSIX's unlink refuses a directory and rmdir a file; with Q9's codes M13 can answer both only if the call says which it meant. | `unlink (str path, u32 flags)` with a flag for "a directory" (as unlinkat's AT_REMOVEDIR): `ERR_IS_DIR` / `ERR_NOT_DIR` for the wrong one. Bundled with Q8's change to `fs`. | breaks | ~16 (fs_idl.h users) | P5 |
| 38 | need (M13) | `user/services/fat/fsops.c:140-156`, `abi/idl/fs.idl:41-42` | `rename` onto an existing name fails (FatFs `FR_EXIST`: `ERR_ALREADY_EXISTS`); POSIX's rename replaces it. | `rename (str from, str to, u32 flags)` with a "replace" flag; fat replaces a file (or answers `ERR_NOT_SUPPORTED` until it can). The argument now, while `fs` breaks anyway. | breaks | ~16 | P5 |
| 39 | need (M13, adds) | `abi/idl/fs.idl:32,37`, `abi/idl/file.idl:27` | No file id (`st_ino`). Once fat keys its open files by directory entry (Q8), the entry's place is that id. | `stat`, `readdir`'s entries and `file.stat` answer a `u64 id`. Cheap while P5 is there. | adds | 0 | P5 |
| 40 | need (Q9) | `user/services/fat/path.c:138`, `user/services/fat/fileops.c:44,454,461,497,504`, `user/services/fat/fsops.c:50,136`, `abi/idl/fs.idl:6-8` | fat's lock answers `ERR_BAD_STATE` (busy), a readdir of a file and an open of a directory `ERR_WRONG_TYPE`, a non-empty directory's unlink `ERR_ACCESS_DENIED`; fs.idl's list of errors misses `ERR_BAD_STATE`. | `ERR_BUSY`, `ERR_NOT_DIR`, `ERR_IS_DIR`, `ERR_NOT_EMPTY`; fs.idl lists every code. | breaks (codes) | fat + libos + tests | P5 |
| 41 | clean | `user/include/fs_idl.h` | Eleven generated client stubs are renamed by macros (`idl_fs_stat`, `idl_file_read`, ...) because libos's path API has the same names. | Leave: M13's libc replaces libos's file calls, and the header goes then. | none | 16 | none (M13) |
| 42 | clean | `user/include/os.h:380-385` | `fs_path_clean` takes the 256-byte field and checks it holds a NUL. | Takes a pointer and a length (the generated `str`). | breaks | fat, bootfs server, libos | P5 |
| 43 | inc (S) | `abi/idl/fs.idl:11-21`, `abi/idl/file.idl:8-15`, `abi/idl/fsctl.idl:1-11`, `abi/idl/storage.idl`, `abi/idl/block.idl` | The restart paragraphs name devmgr's keeper and supervision. | Name volumes when S1 moves them (S's tracks edit these paragraphs; P5 must not). | none | 0 | S1, S2 |

### init and the rest (P6)

| # | Kind | Where | What | Proposed change | Breaks / adds | Callers | Track |
|---|---|---|---|---|---|---|---|
| 44 | need (Q3) | `abi/idl/initctl.idl:27,48,103,117` | Names in fixed fields: `kill`'s `u8[32]`, `mount`'s `u8[16]`, `terminal`'s `u8[128]`, `launch`'s `u8[16]`. | `str[<=31]`, `str[<=15]` (unless S2 moves `mount`), `str[<=127]`, `str[<=15]`. | breaks | 18 | P6 |
| 45 | clean (stale) | `abi/idl/initctl.idl:17-26` | `kill`'s list of names is already incomplete (no compositor, netstack, dns, dhcp ...). | Say what kinds of process (init's services, devmgr's drivers and filesystem services) and point at `user/services/init/ctl.c`. | none | 0 | P6 |
| 46 | need (Q1, Q4) | `user/include/keep.h`, `user/lib/keep.c` | The keep channel: hand-written because genidl had no one-way messages with handles. The keeper tells a handle's type by up to five probing calls (`user/lib/keep.c:246-267`). | IDL protocol `keep`: events `put (u32 slot, handle[<=4])` and `drop (u32 slot)` from the service, `restore (slots, handle[<=64])` and `done (u32 slots, u32 handles)` from the keeper (question D1 for the direction, D2 for the slot list). The probes become one `handle_info` (K3) or the typed handles of question D4. | breaks | 10 (`user/lib/keep.c`, fat's adopt and keeper, the mixer's adopt and state, init's and devmgr's spare, utest keep and fat_restart) | P6 |
| 47 | need (Q4) | `user/include/os.h:245-262`, `user/lib/ns.c:154-178,698-757` | The namespace's three messages (mount, unmount, set), hand-written for the same reason; the reader doesn't check that the first word is 0 (`user/lib/ns.c:154`). Plan item 26: `NS_NAME_MAX` 16 is full ("/svc/devmgr-ctl", "/svc/net-listen" are 15 bytes), `NS_MAX_SVCS` 16 nearly. | IDL protocol `ns`: events with the names (question D2) and `handle[<=N]`; `NS_NAME_MAX` 32, `SVC_NAME_MAX` 24, `NS_MAX_SVCS` 32 (the message stays far below 64 KiB and 64 handles: 8 mounts + 32 services = 40). | breaks | ~8 (libos ns, spawn, init, shell, tests) | P6 |
| 48 | need (Q1) | `user/include/update.h:187-336`, `abi/idl/initctl.idl:66-84` | The update offer: one hand-written message with 2 or 3 VMOs, and one answer. Both ends come from one build (bin/update and init), so the update across M12 is untouched: the manifest and the network protocol (`user/include/updwire.h`) don't change. | An IDL protocol `update` on the offer channel: `offer (u32 flags, u64 kernel_bytes, u64 bootfs_bytes, u64 menu_bytes, u8[<=1024] manifest, handle[<=3] files) -> (the answer's fields, strings as str) later`. | breaks | 5 (init's update.c, `user/services/update`, updtest, utest update) | P6 |
| 49 | clean | `user/include/devmgr.h:95-100`, `user/services/init/terms.c:240-252`, `user/services/shell/sh_handles.c:10-12,72-90` | init's note to the first shell (the line after a panic) lives in devmgr.h (nothing to do with devmgr), on a channel of its own (the shell's `SR_USER + 2`), first word a kind (2). | The line as a startup string (an environment variable of the first shell): the channel and both constants go. | breaks | 2 | P6 |
| 50 | clean | `user/include/splash.h:1-14`, `user/include/crashlog.h:1-22` | Two more one-shot hand-written channels: the splash's (bare u32 messages both ways) and logd's crash result (status first, no txid). | Events in an IDL protocol each (`splash`, and the result as an event on logd's channel), or the header rule of finding 12. Either is small; IDL is the one rule. | breaks | 2 + 2 | P6 |
| 51 | clean | 169 bare `SR_USER + n` against 45 named roles, e.g. `user/services/init/comp.c:370-380`, `user/services/init/services.c:203,241,417,544,564`, `user/tests/utest/compseat.c:50-53`, `user/tests/utest/logd.c` (redefines `LOGD_SR_*`) | A service names its startup roles in its own `.c` (netstack's `main.c`, the compositor's `ctl.c`), while its starter (init) and its tests use bare numbers or copies. | Each service's roles in one header its starter and tests include; the bare numbers go. With K3 moving the service roles out of `kernel/include/jam/startup.h` (item 7). | breaks (mechanical) | ~40 | P6 (+ K3) |
| 52 | clean (stale) | `kernel/include/jam/startup.h:19,26-27` | `SR_STDOUT`: "libos prints through debug_write instead"; libos's printf writes to it (`user/lib/printf.c:6-9,221`). `SR_CONSOLE`'s comment predates the terminals. | Reword with K3's move of the roles. | none | 0 | K3 |
| 53 | clean (stale) | `abi/idl/svc.idl:6-7` | "The music player does": every service that hands out a channel per opener does (the mixer, netstack, dns, devmgr, logd, the compositor's notices and `wayland`). | Reword. | none | 0 | P6 |
| 54 | clean | `kernel/include/jam/startup.h:56-97` | The startup and promotion messages stay hand-written: the kernel builds init's, and libos reads both before `main`. Their limits (16 handles, 64) are plan item 27; nothing else. | Name them in the list of hand-written protocols, with why. | none | 0 | J |

### Where this meets the other reviews

| # | Kind | Where | What | Proposed change | Breaks / adds | Callers | Track |
|---|---|---|---|---|---|---|---|
| 55 | inc (D) | `kernel/test/test_devmgr.c:47-70`, `user/tests/utest/drivers.c:173,191` | Two copies of devmgr's protocol outside `user/include/devmgr.h`: the kernel's devmgr test speaks it by hand (`DM_STATUS`, `DM_GET_DRIVER`, `DM_REBIND`, its own `dm_req` and `dm_rep`: the kernel can't include user headers), and utest forges ordinal 0x00030063. Track D's file list has neither; both build and fail only at run time (the kernel one only in ktest on QEMU). | Add both files to track D. | breaks (run time) | 2 | D |
| 56 | inc (D) | `user/include/devmgr.h:354-363`, `drivers/test/crasher/crasher.c` | The crash-test driver's protocol (ping, crash, exit: id 0xc4a5) is written out twice by hand. | An `.idl` with D, or at least the registry (finding 2). | breaks | 3 | D |
| 57 | inc (K) | `kernel/object/channel_send.c:166-196`; `user/include/os.h:243`; `user/include/devmgr.h:176-179` | A reply to a call that gave up is queued on the caller's end and charged to the server until read, and a client that only makes blocking calls never reads it: up to 1024 per channel end. Callers design around it (dns's margin, devmgr's `DEVMGR_MOUNTS_WAIT` plus a second), but `FS_CALL_TIMEOUT` and every `_within` call can leave one. | Question D5, for K. | | | K1 |
| 58 | need (S) | `abi/idl/storage.idl:19-33`, `abi/idl/block.idl` | Q7's shapes (a partition's GUID, name and scheme; requests in flight) are S2's. From here only: `storage.info`'s vendor and product are space-padded ASCII (`u8[8]`, `u8[16]`), which `str` could carry trimmed. | S2's call. | | | S2 |
| 59 | clean (W) | `abi/idl/compctl.idl:16,85` | compctl's own `nocomp` words, and its `input` connection "(abi/idl/input.idl, unchanged)", change with finding 15. | W's file list; P1 edits only the input lines. | | | P1, W |
| 60 | clean | `user/include/mixer.h:50`, `user/include/sockring.h:186`, `drivers/include/jam/netdev.h:142` | The shared rings have a magic but no version (the state VMO and bootfs have one). Both ends always come from one build; a mixer ring kept across a mixer restart is the same build's. | No change: a layout change changes the magic. | none | 0 | none |

## The protocols, one by one

What each protocol is, where it runs and what changes. Methods are
counted from the `.idl`; "channel kind" is what Q5 splits by.

| Protocol (id) | Channel kinds today | Methods | What changes |
|---|---|---|---|
| null (1) | a test driver's DR_SERVE | 4 | nothing (a fixed `u8[16]` is the test of fixed arrays) |
| edu (2) | the edu driver's DR_SERVE | 4 | nothing |
| idltest (3) | utest only | 4 | id 33 now (1); grows a case per new type (G) |
| usb (10) | one interface's channel | 11 | arrays (35); `reports` channel stays raw (36); bulk timeouts stay (30) |
| input (11) | an input source's channel to the compositor | 4 | all events, no character (15, 16, 20) |
| console (12) | client channels at three levels; open_keys' channel (hand-written) | 10 | `write` and arrays (18); `connect_input`, `lend_screen` gone (19); `keys` protocol for open_keys' channel (17) |
| usbbus (13) | usb-bus's DR_SERVE; events on the same channel | 5 + 1 event | events on a channel of their own (33); `wait_settled` later (34); names as str (35) |
| storage (14) | usb-storage's DR_SERVE | 4 | S2 (Q7, 58) |
| block (15) | one partition's channel | 5 | S2 (Q7) |
| fs (16) | a mount's channel; views (a level) | 9 | Q8; `unlink` and `rename` flags (37, 38); ids (39); codes (40); `kept` (8) |
| file (17) | an open file's channel | 5 | Q8 (`is_dir`), id (39) |
| initctl (18) | init's control channel at three levels | 10 | names as str (44, 45); `update_offer`'s channel speaks `update` (48) |
| logctl (19) | logd's channel (+ svc.connect) | 1 | the crash result as an event (50), else nothing |
| fsctl (20) | fat's control channel | 2 | S (volumes holds it) |
| hda (21) | the driver's channel; the query level; the stream channel | 13 | `hda` + `hda_stream` (22); arrays (23) |
| audio (22) | the opener's channel; the stream channel | 9 | `audio` + `audio_stream` (21); names (23); mixer layout version |
| audioctl (23) | an opener's channel; the desktop level | 6 | arrays (23); wording (24) |
| music (24) | init's kept channel | 12 | one `start` (25); paths and codes (26) |
| svc (25) | every shared /svc channel, beside its service's protocol | 1 | wording (53) |
| jamcover (26) | jamjar's helper channel | 1 | nothing |
| netdev (27) | the driver's channel; the session channel | 3 | `netdev` + `netdev_session` (28); counts (29) |
| netctl (28) | netstack's control channel; the read-only level | 8 | chips as str (29); `dhcp_open` returns a `net_socket` (27) |
| net (29) | opener, UDP socket, TCP socket, listener, shared | 13 | four protocols (27); arrays (29); timeouts (30); TCP addresses (32) |
| dns (30) | an opener's channel (+ shared for connect) | 1 | name str, addresses (29); timeout stays (30) |
| serve (31) | init's kept channel; share's `give` channel (hand-written) | 3 | `share` takes the handles, `give` goes (31) |
| compctl (32) | ADMIN, INPUT, NOTIFY levels | 9 | review W |
| devmgr (3, hand-written) | QUERY, DEVICE, CONTROL levels; the ESP channel | 16 | review S drafts it as IDL, track D builds it; its other copies (55, 56) |
| keep (hand-written) | a service's keep channel, both ways | 4 kinds | IDL with events and handles (46) |
| namespace (hand-written) | SR_NS, starter to program | 3 kinds | IDL events, bigger names (47) |
| keys (hand-written in `<jam/abi.h>`) | console.open_keys' channel, both ways | 3 kinds | IDL `keys` (17) |
| update offer (hand-written) | one offer channel from init | 1 + answer | IDL `update` (48) |
| serve's give (hand-written) | one channel per share | 1 + answer | goes (31) |
| splash, crash result, init's note (hand-written) | one-shot channels from init's children | 1-2 each | events or a startup string (49, 50) |
| startup, promotion (hand-written) | a process's first channel; SR_STANDBY | 1 each | stay hand-written (54) |
| usb reports, `SR_STDOUT` (hand-written) | byte messages | none | stay: channels of byte messages (36; Q17) |
| stop channels (play, fetch, speed) | signals only (readable or closed) | none | stay |
| the clipboard, JWL1 | | | review W |

**The hand-written protocols left after M12**, if every proposal is
taken: the startup and promotion messages (the kernel builds one, libos
reads both before `main`), the byte channels (usb's reports, `SR_STDOUT`,
M13's pipes), the stop channels (no messages at all), and JWL1 (review
W). Shared memory stays hand-written by nature: the mixer's, the
sockets' and the network cards' rings, the state VMO, the log writers'
page, bootfs and the crash log.

## Design questions

Only questions the owner's answers don't settle. Each ends with a
recommendation.

### D1. Which way an event goes

Q4 gives the IDL one-way `event` methods. Most flow toward the server
(input reports, keep's puts, the keys channel's `want`), but five flow
the other way, from the end that answers calls to the end that makes
them: usb-bus's `interface_attached`, keep's `restore` and `done`, the
console's keys and mouse reports on a keys channel, the namespace's
messages (the program never writes), the splash's `go`.

- **(A)** A protocol says what one end serves. Events go toward it only;
  the other direction is a second protocol, served by the other end
  (`keep` and `keep_restore`, `keys` and `keys_want`).
- **(B)** One protocol per conversation, both directions in it: `event`
  methods toward the server, and server events (marked, say, `-> event`)
  that the generated client reads with a dispatcher beside
  `idl_reply_read` (a message with txid 0 is an event, any other a
  reply).

*Recommendation: (B).* Three of the five (keys, keep, the splash) are
one conversation each, and (A) gives each two names and two ordinal
spaces for it. (B) is also how Wayland and FIDL put it, which G2's ports
will meet anyway.

### D2. Arrays beyond bytes

Q3 decided `u8[<=N]` and `str[<=N]`, records staying byte arrays of a C
struct "until a third case asks". M12's own changes bring the third
case and more: batched `readdir` (entries with a name each), the mount
list volumes answers (names with a handle each), keep's restore (slot and
count pairs), the namespace's names, dns's four addresses, audioctl's
stream list, hda's pins and states.

- **(A)** As decided: byte arrays, one hand-written decoder per record
  kind in libos.
- **(B)** (A), plus arrays of any scalar type (`u32[<=4]`, `u64[<=N]`):
  dns's addresses, keep's pairs as `u32[<=128]`.
- **(C)** (B), plus `struct` declarations of fixed-size fields and arrays
  of them; a record holding a string stays a byte array.
- **(D)** Full records, strings inside.

*Recommendation: (B) now.* It is a few lines in genidl and covers the
numeric lists; `readdir`'s entries and the names lists stay byte arrays
with one decoder each in libos (a list of strings is two cases, not
twenty). Records (C or D) wait for M14, with M13's experience.

### D3. Constants in the IDL

A protocol's flags, levels and states live in C headers away from its
`.idl` (finding 10), one of them in a kernel header (plan item 7).

- **(A)** `const <NAME> <value>` lines in the `.idl`, written into the
  protocol's header as `<PROTO>_<NAME>`; the C headers keep only what
  isn't a protocol's (the client libraries).
- **(B)** As today.

*Recommendation: (A).* It gives `INPUT_*` and the `FS_*` flags a home
when they leave `<jam/abi.h>` and `<os.h>`, and the levels a name.

### D4. Typed handle arguments

With Q1 a method may take handles; a server must then check what it was
given. Today each receiver probes by hand: the keeper with up to five
calls, the compositor's clipboard with a zero-byte read; devmgr assumes
`SET_CONSOLE`'s handle is a channel.

- **(A)** Plain `handle`; each server calls `handle_info` (Q11) itself.
- **(B)** `handle:channel`, `handle:vmo`, `handle:event`, ... : the
  generated server refuses a request whose handle is another type
  (`ERR_WRONG_TYPE`, one `handle_info` per handle), and the generated
  client checks handle results the same way. Rights stay prose.

*Recommendation: (B).* One check, written once, for every handle that
crosses a protocol; it costs a system call per handle, and handles are
rare in requests.

### D5. Replies to calls that gave up (for review K)

Finding 57. A late reply stays queued on the caller's end, charged to
the server's job, and a blocking-only client never reads it.

- **(A)** As today; protocols keep giving their callers deadlines longer
  than the server's own (dns, devmgr's mounts do).
- **(B)** The kernel drops a reply whose txid belongs to a call that
  ended on that end: with finding 5's rule (the kernel's txids have bit 31
  set), it knows which txids are its own, and a short list per end (16)
  of txids given up is enough.
- **(C)** Generated clients drain stale replies before each call (racy
  when threads share a channel).

*Recommendation: (B) if K1 takes finding 5's txid rule, else (A).*

## Suggested split into tracks

The plan's tracks hold, with these contents and this order. Each P
track owns both ends of its protocols and their tests, bumps a state
layout version when a slot's request format changes, and runs its area
scripts.

1. **G, first hand-back** (on today's kernel): the registry (2) and
   idltest's id if the main session hasn't fixed it (1); `MSG_MAX` and the
   serve buffers (3); `u8[<=N]`, `str[<=N]` and scalar arrays (Q3, D2);
   events in both directions (Q4, D1); several protocols in one file
   (Q5); constants (D3); the file comment in the header (7); `kept` (8);
   `idempotent`'s note (9).
2. **P2, P3 (but serve), P4, P5** in parallel as G hands back: each
   rewrites its `.idl` files with the new types and the Q5 split, then
   its servers and callers. P5 also takes Q8, findings 37 to 42.
3. **K1**, then **P1** (input events, the keys protocol, `console.write`,
   `nocomp`'s methods: 15 to 20).
4. **G, second hand-back** (after K1): handle arguments and
   `handle[<=N]` (4), typed handles (D4), the txid split (5) and the
   close-after-failure lines in `drivers/include/idl/common.h` (6).
5. **P3's tail**: serve's `share` with handle arguments (31).
6. **S2, then D** (review S): devmgr's IDL, with findings 19 (the
   `SET_INPUT` rename), 33's driver role, 55 and 56.
7. **P6** (after D and K3): keep (46), the namespace (47), the update
   offer (48), init's note (49), the splash and the crash result (50),
   the startup roles (51), initctl (44, 45), svc's wording (53), Q16's
   heap.
8. **J**: the list of hand-written protocols with why (36, 54), the IDL
   recipe in CODING-GUIDE (arrays, events, handles, constants).

Small enough for the main session now: finding 1 (idltest's id; two
utest files and `make idl`). The stale comments (20, 24, 45, 52, 53) go
with their tracks, since those tracks rewrite the same lines.
