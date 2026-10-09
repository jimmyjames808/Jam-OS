# M12 review W: the compositor's protocol

Stage A of [M12-PLAN.md](M12-PLAN.md#stage-a-the-reviews-findings-first),
review W, at main 56aa8904: docs only, nothing built or run. Read: G1's
"Interface changes for M12 to review" and its "As built" notes
([G1-PLAN.md](G1-PLAN.md#interface-changes-for-m12-to-review)); the JWL1
transport and libjwl, both sides (`user/include/jwl.h`,
`user/lib/jwl_transport.c`, `user/lib/jwl_client.c`, `user/lib/jwl_seat.c`,
`user/lib/jwl_data.c`, `user/lib/jwl_events.c`); the globals and versions
offered (`user/services/compositor/display.c`, comp.h); our own protocol
(`abi/wayland/jam-window-memory-v1.xml`); compctl and its server
(`abi/idl/compctl.idl`, ctl.c, deskctl.c, sources.c, keyboard.c, data.c,
conn.c, shm.c) and its callers (init's comp.c, devmgr's usb.c,
`user/lib/notice.c`); `abi/idl/input.idl` and its senders (hid, serialin);
`abi/idl/console.idl` in window mode; the keymap (`abi/keymap/`,
`user/include/keymap.h`); initctl's `terminal` and `launch`; /svc/notify.
Each is judged against the owner's answers (handles always moved; the
reader pays once the writer closes; events in the IDL; `nocomp` gone; no
character in input reports; one rule for time), a ported libwayland (G2's
shim) and ported apps, bounds, and restarts.

**What holds up.** The wire bytes are Wayland's, the codec is strict and
fails closed, ids follow libwayland's rules (next-or-reused, the delete_id
handshake, zombies), and a shim needs nothing of libjwl's code. Every
per-connection cap is enforced. The keymap format is sound: the XKB text
defines its own four types, so xkbcommon and libjwl's tables agree, and
the layout's name rides in a comment xkbcommon ignores. The clipboard's
transfer is already Q17's pipe (a channel of byte messages, closed at the
end), and with Q2 the hoarding reader pays. Restarts: /svc/wayland and
/svc/notify are init's kept shared channels whose requests carry no
handles, so D2's "receives no handles" mark fits them as they are; input
sources, compctl channels and clients all reconnect by documented rules,
and the arrangement comes back through the state VMO. `wayland` and
`notify` are the 15th and 16th service names of 16 (the plan's item 26,
P6's).

**What must change before M14.** Q1 breaks libjwl's held batches (W1);
the transport ties handles to the batch that carries them, which a
libwayland shim can't honour (W4, DQ4), and trusts the client's own count
for flow control (W3, DQ3); `input` and compctl still answer calls where
Q4's events fit (W7, W9, W11); Q14 leaves a list of callers wider than the
plan names (W14, W15).

## Findings

Kinds: **bug today**; inconsistency (with the owner's answers or the
codebase's rules); M13 or G2 need; cleanup. "Breaks" means callers must
change in the same merge; "adds" means nothing existing changes.

| # | Kind | Where | What | Proposed change | Breaks or adds | Callers |
|---|---|---|---|---|---|---|
| W1 | inconsistency (Q1: a bug the day K1 lands) | `user/lib/jwl_transport.c:283-301`, `user/lib/jwl_transport.c:312-335` | A batch the channel can't take now (`ERR_SHOULD_WAIT`, `ERR_NO_MEMORY`) is held with its handles and written again later. Under Q1 the failed write has already moved them: the retry sends handles that are gone (the connection dies, or a reused handle number goes out). Batches with handles: `wl_shm.create_pool`, `wl_keyboard.keymap`, `wl_data_offer.receive`, `wl_data_source.send`; a full channel is rare, so tests would rarely see it. | `write_batch` duplicates a batch's handles before each try when it has any, and closes the originals once a write succeeds. K1 carries this change (it is its rule's caller fix), with a utest: a client that doesn't read, a keymap batch held, then read with a working VMO. | adds (internal) | libjwl, both sides |
| W2 | **bug today** (small) | `user/lib/jwl_transport.c:36-41`, `user/lib/jwl_transport.c:314`, `user/lib/jwl_transport.c:329` | `JWL_HELD_MAX` (64 KiB) counts a held batch's bytes, not its 80 bytes of bookkeeping and the heap's own. A client that never reads and makes the compositor send one tiny batch a turn (a `wl_display.sync` a turn: 40 bytes) gets about 1,600 batches held: about 200 KiB of the compositor's heap per connection, three or four times the stated bound, about 13 MiB over 64 connections of a 16 MiB heap (Q16 raises the heap; the bound is still wrong). | `hold` appends to the last held batch while the bytes and handles fit (a batch is any run of whole messages up to 16 KiB), and `held_bytes` counts what was allocated. utest: one sync a turn from a client that never reads; the held allocation stays under 64 KiB plus one batch. | adds (internal) | 0 |
| W3 | inconsistency (bounds) | `user/lib/jwl_transport.c:195-198`, `user/include/jwl.h:315-322` | The window trusts the client's `acked`: anything up to the batches sent is taken. A client that acknowledges what it never read (guessing the count; overshooting is only a disconnect) keeps the window open while the kernel's queue holds up to 1,024 of the compositor's batches (16 KiB each) on its channel, charged to the compositor's job, instead of 32. If that reaches the job's limit, every client's writes fail and honest clients are held, then disconnected as "too slow". A shim must also carry the ack logic. | DQ3 (B): the kernel says how many messages and bytes the caller has queued at the peer (a field of Q11's `handle_info`), the compositor holds back on that, and the header loses `acked`. | breaks (the header) | libjwl, both sides; the hostile tests |
| W4 | G2 need | `user/lib/jwl_transport.c:244-246`, `user/include/jwl.h:101-103` | A batch's handles must be used by that batch's own messages, at most 16. libwayland hands its socket a flush's bytes with up to 28 fds and no record of which message takes which (fds are a queue on a Unix socket), and a flush may be bigger than one batch: a shim can't cut it into batches without decoding every message against the whole protocol's tables and object map. | DQ4 (A): handles are a queue per connection, as fds are on a socket: a batch's `nfds` says how many it adds, messages take them in order across batches, at most 28 a batch and 28 unused at once. The 4096-byte message and 16 KiB batch stay (raising either later is an addition). | breaks (the transport's rule; the bytes don't change) | libjwl, both sides; jwl and hostile utests |
| W5 | inconsistency (Q2) | `user/services/compositor/conn.c:15-22`, `user/services/compositor/conn.c:260-296` | A disconnected client keeps a "lingering" slot until it closes its end, so what it never read stays charged to the compositor; each slot is one of the 64 connections, so a program that gets itself disconnected 64 times without closing holds them all. | With K1's Q2 (the reader pays once the writer closes), `client_die` closes and frees at once; `lingering` and its `ONCE` rebinding go. Test: `comp_lost_clients` with message bytes flat and the slot free at once. | adds (internal) | 1 |
| W6 | inconsistency (Q4, Q15); **bug today** (rare) | `abi/idl/input.idl:1-33`, `drivers/hid/hid.c:66-74`, `drivers/hid/hid.c:165`, `drivers/hid/hid.c:190`, `drivers/hid/hid.c:360` | `input` is four blocking calls: hid waits for the compositor's answer to every key and mouse report, and after 2 s drops the event, so a key-up lost while the compositor is slow leaves the key held there: the focused client never sees the release and, as Wayland clients repeat keys themselves, repeats it until another key comes. `key` carries hid's US character (unused: `user/services/compositor/sources.c:36`); `REPEAT` is generated by hid and dropped by the compositor (`user/services/compositor/keyboard.c:322`: only `nocomp`'s console used it); `text` is a fixed `u8[64]`; the header still says the console serves it. | `input` as four events: `key(u16 usage, u8 state, u8 mods)` with `UP` and `DOWN` only, `mouse`, `text(u8[<=64])`, `ready`; hid stops making repeats (the compositor's `repeat_info` already says the rate). On a full queue hid keeps a small queue of its own and never drops a key event (mouse movement may merge). Header rewritten for the compositor. | breaks | hid (2 files), serialin, the compositor's sources.c and keyboard.c, utests compseat, compinput, hid, keymap |
| W7 | inconsistency (one place for a layout, as Q15) | `drivers/hid/keyboard.c:124-127`, `drivers/hid/keyboard.c:196-202`, `user/services/compositor/keyboard.c:61`, `user/services/compositor/keyboard.c:301-304` | Lock state lives twice: each hid toggles its own Caps and Num Lock lights, the compositor keeps its own locked modifiers (from "Num Lock on" at start). Two keyboards, or a compositor restart with Caps Lock on, leave the lights and the typing disagreeing. Once the character leaves hid (Q15) the lights are hid's only use of it. | The compositor owns the locks and sends each keyboard source its lights (`input` event `leds(u8)` from the compositor to the source: DQ1); hid stops toggling, starts dark and lights what it is told. | breaks (with W6) | hid, the compositor's keyboard.c and sources.c |
| W8 | inconsistency (Q4, Q1; devmgr's waits) | `abi/idl/compctl.idl:39-43`, `user/services/devmgr/usb.c:194-204`, `user/services/init/comp.c:136-138` | `connect_input` is a call that answers a handle: devmgr waits up to `CONNECT_WAIT` for it inside a driver's binding, and takes a late answer's source off its queue by hand (`drain`) so it doesn't hold one of the 16 slots. | An event `add_source(handle source)`: the caller makes the channel, hands one end to the driver and gives the compositor the other, which serves it or closes it (16 at most: the driver sees its peer close, as on a compositor restart). Nothing waits, nothing comes late. | breaks | devmgr usb.c, init comp.c, utests compseat, compinput, compplumb |
| W9 | inconsistency (Q5; DQ2) | `abi/idl/compctl.idl:7-21`, `user/services/compositor/ctl.c:133-300` | compctl has three levels, each method checking by hand; the NOTIFY level is really another service (/svc/notify: its own name, cap of 8, restart rule and posters). | Notices get a protocol of their own, `notify` (notify, notify_wait, withdraw, W10's post), on /svc/notify's channels; init asks for its own NOTIFY channel there. The INPUT level per DQ2. `new_client(level)` keeps only what is left. | breaks | notice.c, init comp.c, the shell's notify.c, the compositor's ctl.c and deskctl.c, utests compplumb, compseat, compboot |
| W10 | inconsistency (the service-loop rule, Q4) | `user/lib/notice.c:49-65`, `user/include/notice.h:7-11` | `notice_post` waits up to 200 ms (`NOTICE_CONNECT_WAIT`) inside devmgr's and netstack's loops for `svc.connect`, then sends `notify` as a call whose answer it reads and drops at the next post. | A one-way `post` event (title, body, icon, tint: no buttons, no id), and `svc.connect` sent without waiting, its answer taken from the port as deskctl.c does. | adds (an event); notice.c internal | notice.c (devmgr's 6 posts, netstack's 2) |
| W11 | inconsistency (Q3) | `abi/idl/compctl.idl:87`, `abi/idl/initctl.idl:103`, `abi/idl/initctl.idl:117` | Strings in fixed arrays: `notify`'s title `u8[64]`, body `u8[96]` and three 24-byte button labels packed into `u8[72]`; `initctl.terminal`'s `u8[128]` command; `initctl.launch`'s `u8[16]` app. Each server checks the NUL by hand. | `str[<=63] title, str[<=95] body`, three `str[<=23]` buttons (an empty one ends the list); `str[<=127] command`; `str[<=15] app`. Control characters stay the servers' to refuse. initctl's half is P6's (it owns initctl.idl). | breaks | notify: notice.c, init comp.c, the shell's notify.c, compositor deskctl.c, utest compplumb; terminal and launch: the shell's term.c and launch.c, compositor ctl.c, console keys.c, utests compboot, compplumb |
| W12 | inconsistency (D1; K1's and G's to settle) | `user/services/init/comp.c:89-91`, `user/services/compositor/ctl.c:55-57`, `user/services/compositor/deskctl.c:45-47`, `kernel/object/channel_send.c:21-28`, `kernel/object/channel_send.c:90-95` | Requests sent without waiting carry hand-picked txids (`0x2b80000 \| n`, `0x0cad0002`, ...), and init sends some on its compctl ADMIN channel, which also carries init's calls (comp.c's `blank`, `set_layout`, `new_client`). The kernel gives a reply to the call waiting for its txid, so a hand-picked number equal to a call's kernel txid sends the reply to the wrong waiter. Today a clash needs the global counter to reach the number; with D1's counter per pair it is the same, but no rule says so. | K1 writes the rule: the kernel's txids have the top bit clear and hand-picked ones set it (genidl's `_send` refuses others), or a call reserves one. init's comp.c, ctl.c, deskctl.c and notice.c renumber (other `_send` users: review P). | breaks (constants) | 4 files here |
| W13 | inconsistency (Q14) | `abi/idl/console.idl:1-9`, `abi/idl/console.idl:22-36`, `abi/idl/console.idl:49-54`, `abi/idl/initctl.idl:35-38`, `abi/idl/initctl.idl:96-97` | With `nocomp` gone: `connect_input`, `lend_screen` and `blank` (a no-op in window mode, `user/services/console/screen.c:85-86`) go, and the texts that mention the full screen. Callers beyond the plan's list: init's reboot.c (two `console.blank`), the shell's reboot.c (two), `user/tests/fbbench/main.c` (borrows the screen: P0's tool and `tools/shell-tests/fbbench.txt`), contest's main.c and mouse.c, init's services.c (serialin's console branch), devmgr's usb.c console branch and main.c's `comp` argument (and `DEVMGR_SET_CONSOLE`'s name, track D). | Remove the three methods (ordinals 5, 6, 8 not reused); fbbench measures through a full-screen window (libfun) and keeps its RAM lines, its raw framebuffer lines dropped (the compositor's `stats` has paint time and pixels); devmgr and serialin keep only the compositor path. | breaks | about 13 files |
| W14 | cleanup (Q14) | `kernel/include/jam/abi.h:360-410`, `user/services/console/keys.c` | `console.open_keys`' mouse (`input_want`, `input_mouse_event`) has no user once libfun's borrowed screen goes (libfun's mouse.c console path, contest's mouse.c); the key messages are hand-written structs in a kernel header (the plan's item 7). | Drop the mouse from `open_keys`; terminal programs get the mouse as xterm's escape sequences from M13's terminal layer. The key messages become an IDL protocol of events from the console to the program (DQ1), out of `<jam/abi.h>`. | breaks | console keys.c, libfun mouse.c, contest mouse.c, the shell's main.c |
| W15 | M13 need | `user/services/compositor/shm.c:4-8`, `user/services/compositor/shm.c:36-60` | `create_pool` takes only a VMO made with `VMO_KEEP_PAGES` (else `invalid_fd`). A ported app's pool fd comes from `memfd_create` or `shm_open`; unless M13 makes those VMOs kept, every ported app's first pool is refused. If K2 changes `VMAR_KEPT_ONLY`'s error, `map_refusal` follows. | Written into M13's needs: a memfd or shm object is a kept VMO (the analogue of Linux's `F_SEAL_SHRINK`, which compositors there rely on), growing with its file. K2 updates `map_refusal` if the error changes. | adds (M13) | shm.c |
| W16 | M13 and G2 need | `user/services/compositor/data.c:403-414`, `user/services/compositor/data.c:442` | The compositor checks a paste's handle is a channel end by a zero-byte read, which needs the read right: a write-only end (what M13's `pipe` may hand a ported app as its write end) is refused and the paste is silently empty. | With Q11's `handle_info` (K3 owns the change): a channel end with write and transfer rights, nothing else asked. | adds (relaxes a check) | data.c |
| W17 | **bug today** (minor) | `user/lib/jwl_data.c:156-167` | The clipboard's owner stops at the first refused write (the reader's queue full, its own job out of memory) and closes the end, which the reader takes as the end of the text: a short paste looks whole. | On a refused write keep the end and the rest, and try again each dispatch until the paste's 2 s, then close. utest: a reader's queue nearly full; the paste arrives whole. | adds | jwl_data.c |
| W18 | cleanup | `abi/idl/compctl.idl:30-32` | It says a layout switch the dead compositor hadn't reported is lost; since the state VMO keeps new screens' layout (`user/services/compositor/wmsave.c:174`), the restarted compositor answers init's `layout_wait` at once and init saves it. | Fix the text. | adds | 0 |
| W19 | cleanup | `abi/wayland/jam-window-memory-v1.xml`, `user/services/compositor/memory.c:27-31` | The `key` event answers 0 for a toplevel whose surface is gone; the XML doesn't say 0 means "no key". Upstream's proposed session-management protocol does the same job for ported toolkits. | Say it in the XML before M14 freezes it. G2 adds upstream's protocol beside ours if a port uses it; both can feed `wmsave.c`. | adds | 0 |
| W20 | G2 need | `user/services/compositor/comp.h:91-95`, `user/services/compositor/xdgtop.c:517-521` | `xdg_wm_base` 1 while windows tile: version 2's tiled states tell toolkits that draw their own frames to drop shadows and rounded corners in a tile, version 5's `wm_capabilities` says there is no window menu. `set_minimized` is ignored ("no minimising in G1") though the desktop minimises now. `wl_shm` 1, `wl_output` 3 likewise older than upstream. | Additions for G2's first ports, nothing in M12; `set_minimized` can call the desktop's minimise whenever someone is in xdgtop.c. | adds | 0 |
| W21 | G2 need | `user/services/compositor/ctl.c:531-538`, `user/include/deskapps.h` | The search box's busy cursor ends when a window with the app's title maps: any client can end it. Ported apps say "I am the one you launched" with upstream's `xdg-activation-v1` token. | Later: a token passed to the launched app (a new `initctl` method then, an addition). | adds | 0 |
| W22 | cleanup | `user/lib/jwl_client.c:26-30` | The client's `known` list says "everything this library binds" but lists five of eight; it is never consulted (no event carries an untyped new id). | Fix the comment, or pass none. | adds | 0 |

## Design questions

Only questions the owner's answers leave open.

**DQ1. Events from a server to its client.** Q4 gave the IDL one-way
events "the generated server reads ... as it reads calls": client to
server. Three things here go the other way: the keys `open_keys` sends a
program (hand-written today, W14), the keyboard lights (W7), and compctl's
`layout_wait` and `notify_wait`, which are `later` calls standing in for
pushed events.
- (A) Events may go either way: a protocol marks a method as sent by the
  server, and the client gets a generated reader.
- (B) Client to server only; `open_keys` stays hand-written, the lights
  stay hid's.

*Recommendation: (A)*, used for `open_keys` and the lights; compctl's two
waits stay `later` calls (one answer each, bounded, they work).

**DQ2. Levels on one protocol, or a protocol per level.** Q5 settled a
protocol per kind of channel; compctl (ADMIN, INPUT, NOTIFY), the console
(ADMIN, SHELL, PROGRAM) and initctl (the shell's, the consoles', the
compositor's) instead give one protocol levels and check them by hand in
every method.
- (A) A protocol per level: the generated server can't take a method the
  channel may not use; methods shared between levels are written twice.
- (B) Levels stay, but genidl checks them: a method lists the levels it
  serves, the channel's level is set when it is made.
- (C) As today, except where the level is really another service
  (/svc/notify: W9).

*Recommendation: (C) now, (B) if review P finds more hand checks than
these three protocols.* The splits pay only where the channel has its own
name and restart rule.

**DQ3. Flow control: the client's count or the kernel's** (W3).
- (A) Keep the header's `acked`: the bound against a lying client is the
  kernel's 1,024 messages a channel.
- (B) The kernel tells the sender how many messages and bytes it has
  queued at the peer (`handle_info` on a channel end, Q11); the compositor
  holds back at 32 batches by that, and the header's `acked` goes (the
  magic becomes "JWL2"). One more system call per client per flush that
  sent something.
- (C) A queue limit per channel set when it is made (Q2's (C), not taken).

*Recommendation: (B).* The bound becomes true whatever the client does,
and a shim no longer has to acknowledge (nor libjwl's clients remember to
flush after reading).

**DQ4. Which batch a handle belongs to** (W4).
- (A) A queue per connection: handles go in with a batch and are taken by
  later `fd` arguments in order, across batches, as on a socket; at most
  28 a batch and 28 waiting unused.
- (B) Keep handles in their batch, raise the limit to 28, and require a
  shim to send each libwayland flush as one batch (it can't if a flush is
  over 16 KiB: newer libwayland lets a program raise its buffer past 4 KiB).

*Recommendation: (A).* A shim then needs only the 8-byte header of each
message to cut batches, never the protocol's tables.

## A track for the fixes

The plan's P1 (input, console, compositor) is more than an agent-hour
with these; split it in two, as AGENTS' lessons on sizing say.

1. **With K1, in K1's merge:** W1 (libjwl duplicates a batch's handles), so
   main never runs Q1 with libjwl's held batches unfixed. K1 also writes
   W12's txid rule (G's `_send` follows).
2. **P1a, the transport** (after K1, and K3 for DQ3's `handle_info`; owns
   libjwl's transport, the compositor's conn.c, the jwl and hostile
   utests): W5 (lingering slots), W2 (held batches merged), then DQ3 and
   DQ4 as answered (one header change, both sides in one commit), W17,
   W22. Tests: `tools/shell-tests/g1-hostile.txt`, the `jwl*` and
   `comp_live_*` utests, `tools/clip-test.sh`.
3. **P1b, input, compctl and the console** (after G hands back events and
   strings; owns `abi/idl/input.idl`, `abi/idl/compctl.idl`,
   `abi/idl/console.idl`, the new `notify` protocol, hid's and serialin's
   senders, the console, notice.c, the compositor's ctl.c, sources.c and
   keyboard.c, libfun's screen path, fbbench): W6 then W7 (input; W7 needs
   DQ1), W8, W9 then W10 then W11's compctl half, W13 then W14 (`nocomp`'s
   leftovers), `console.write` as an array, W18. Tests:
   `tools/usbkeys-test.sh`, `tools/mouse-test.sh`, `tools/desk-test.sh`,
   `tools/screen-test.sh`, `tools/usb-early-test.sh`, `tools/splash-test.sh`,
   the comp utests.
4. **Elsewhere:** W11's initctl half to P6; W16 to K3 (its plan already
   names "the compositor's handle check"); W15 and W16's M13 halves into
   M13's list of needs (J writes them down); W19 to whoever is in
   `abi/wayland/` (P1a); W20 and W21 to the roadmap's G2 notes.
