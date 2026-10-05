# G1 plan: a compositor that speaks Wayland

Status (2026-10-05): a plan, nothing built. Written against main 83fbe2e,
while wave 1 (M11, M11.5, M11.6) is still being joined. **Waiting for the
owner's answers to the questions below before any code.** G1 is wave 2
of [PLAN-M11-M12.5.md](PLAN-M11-M12.5.md#wave-2-g1-alone): it runs alone,
after wave 1 and before M12, so that M12 reviews the compositor's
protocol with every other interface.

Goal ([roadmap](ROADMAP.md#later)): **a compositor of our own on the
firmware framebuffer that speaks the Wayland protocol. Apps draw into
their own surfaces, input goes only to the focused client, a crashed app
takes only its own window down, rendering is in software. The console
becomes a client: a terminal window.**

**Done when** (the roadmap's row, made concrete):
- windows from several programs on the PC's screen at once (the shell in
  a terminal window, jamjar, a game, sysmon), moved, raised and focused
  with the mouse;
- keys typed go to the focused window only, and a program killed or
  crashed takes only its own window with it;
- the cost of a frame measured on the PC (a full-screen frame, a typical
  one) and in [BENCH.md](BENCH.md);
- and, as every milestone: All tests and `soak 10` on the PC.

## Where the screen is today

- **The console owns the framebuffer** (`framebuffer_take` in
  `abi/syscalls.def`: a write-combining VMO and an owner token; the
  kernel draws its log again when the owner dies, and a panic always
  draws). It draws a grid of 8x16 cells (320x90 at 2560x1440) from a
  shadow grid, never reading the framebuffer back
  (`user/services/console/screen.c`).
- **It is also the input hub.** devmgr asks it for an `input` channel per
  HID driver (`console.connect_input`, `user/services/devmgr/usb.c`), and
  serialin for the serial port. Keys go to a focus stack of `open_keys`
  channels, the newest first; a PROGRAM's focus can't keep Ctrl+C from
  the shell below it; Ctrl+Alt+Del goes to init
  (`user/services/console/keys.c`). The mouse goes to the focus only if
  it asked, as raw relative reports (`<jam/abi.h>`, "input").
- **Programs that draw borrow the whole screen**: `console.lend_screen`
  hands out the framebuffer VMO itself and a lease channel. Every one of
  them goes through libfun's `gfx_open` (`user/apps/fun/gfx.c`): demo,
  fractal, life, tetris, snake, mines, sysmon, jamjar and the boot splash.
  libfun keeps a full-resolution back buffer and a copy of what the
  screen shows, and copies only the 64-pixel row pieces that changed, on
  every CPU. The pointer's arrow is drawn by libfun into the app's frame
  (`user/apps/fun/mouse.c`).
- What lending can't do: two programs on the screen at once; and once
  the lease is given back, the borrower's mapping of the framebuffer is
  not taken from it (the kernel has no way to), so a program that closed
  its lease can still draw over the console. Under a compositor that
  would let any program draw a fake window over a real one.

## Questions for the owner

**The owner's answers (2026-10-05).** The plan follows them; where one
differs from the recommendation below, this note wins:
- Q1 (A) real Wayland on the wire; Q2 (A) the kernel "pages stay" VMO
  flag; Q3 (A) every libfun program a window; Q4 (A) `nocomp` kept until
  the PC sign-off; Q6 (A) one layout file, two outputs, US only;
  Q8 (A) clients reconnect.
- **Q5: both arrangements, switchable.** Floating windows with the
  compositor's title bars, AND a tiling mode, with a key that switches the
  whole screen between them (and the choice kept in /data/etc/settings).
  In tiling mode a window that can't resize sits centred in its tile at its
  own size (with the background around it), never stretched; resizable ones
  (the terminal, jamjar) take the tile. Track C3 owns both.
- **Q7: both in G1.** `&`, `jobs` and `kill %n` in the shell (track S1),
  AND more terminal windows, each with its own shell (a key or a command
  opens one; init supervises each shell as it does the first). That adds
  work in init, the console's window mode (T1) and the shell's supervision:
  the plan's stages gain it, sized as one more track.
- **Q9: (B) a smaller terminal window.** After the splash, the first
  terminal opens as a centred window (not maximised), with the desktop
  background (the splash's dark colour) around it; text stays 8x16 pixels.
- **The look (later the same day, from mockups).** Tiling mode: no title
  bars; the focused tile shows by its border colour; small gaps between
  tiles; Super+Q closes the focused window (both modes). Floating windows
  ("style B1"): rounded corners, a soft shadow, the title centred in a
  smooth (anti-aliased) proportional font, and three circles on the
  LEFT of the title bar in jam colours: raspberry (#d4537e) closes,
  apricot (#ef9f27) minimises, blackcurrant (#7f77dd) makes the window
  full screen; an unfocused window's circles are grey. Extras the owner
  chose: the circles show their symbol (x, -, full screen) on hover; a
  jam-coloured wallpaper instead of a flat colour; double-clicking a
  title bar toggles full screen; open and close animations (fade and
  scale, about 150 ms); minimise, brought back from a top bar; a top bar
  with a clock and the open windows (minimised ones too). The terminal's
  text stays the 8x16 bitmap font. Virtual screens (workspaces), made as
  needed: the desktop starts with one; a new one is made when you go past
  the last (Super+Right, or the "+" at the end of the top bar's screen
  dots) or move a window there (Super+Shift+Right); a screen other than
  the current one that has no windows left goes away; Super+1..9 jump to
  an existing screen; switching slides. Each screen keeps its own
  arrangement (floating or tiling: Super+T switches the current one). A
  window made full screen (its blackcurrant circle, Super+F or a
  double-click) slides onto a new screen of its own next to the one it
  came from, with no top bar, frame or wallpaper; leaving full screen
  slides it back to its place, and that screen goes away. The top bar
  lists the current screen's windows. The top bar (owner's pick, "3 on
  a strip"): a full-width frosted strip, about 40 px at 1x, holding three
  rounded islands slightly lighter than it: left "Jam OS" (the menu) and
  the screen dots with "+"; centre the current screen's windows (the
  focused one tinted raspberry, minimised ones dimmed with an apricot
  dot); right the floating/tiling icon, network, volume and the clock.
  The app menu is a search box (owner's pick): tapping Super alone (pressed
  and released with no other key) or clicking "Jam OS" opens it centred
  near the top, frosted; empty, it lists every app (most recently used
  first) so a mouse user can just click; typing filters, Enter runs the
  top hit, arrows move, Esc closes; its last row offers to run what was
  typed as a command in a new terminal. Apps show as letter tiles in jam
  colours until there are icons.
  Alt+Tab (owner's pick): a compact frosted list, centred, one row per
  window (letter tile and title). Order: the current screen's windows
  (most recently focused first), then every other screen's windows grouped
  under a small "screen N" label, then minimised windows. Holding Alt,
  the first Tab selects the next row and each further Tab the one after
  (Shift+Tab goes back, wrapping); letting go of Alt goes to the selected
  window (sliding to its screen, restoring it if minimised); Esc while
  Alt is held cancels. A quick Alt+Tab tap goes straight to the previous
  window; the list appears only if Alt is held past about 120 ms, so a
  tap doesn't flash it.
  Animations (owner's picks): a window opens by growing from 92% while
  fading in, and closes the reverse (about 150 ms, ease-out); minimising
  shrinks it into its chip in the top bar (about 260 ms) and restoring
  reverses that; switching screens slides the windows sideways (about
  260 ms) while the wallpaper and the top bar stay put, the screen dots
  following; going full screen and back slides the same way. An
  animation that is interrupted (another key, a second click) jumps to
  its end. Animations run at the compositor's paint rate and only damage
  the boxes they touch.
  The strip's frosting is a blurred copy of the wallpaper (made once per screen size,
  so it costs nothing per frame); windows never go under it: the space
  it takes (with the gap below it) is outside every window's reach, in
  floating moves, maximise and tiling alike (full screen has no bar).
  Tracks D1 (the look) and D2 (minimise,
  the top bar, virtual screens, animations) build these after C3 (added to the stages
  when they start).

Each question stands on its own, says what it decides and why it
matters, and ends with a recommendation. The plan assumes the
recommendation until you say otherwise. Q1 to Q4 shape everything; Q5 to
Q9 are smaller.

### Q1. How closely should G1 follow real Wayland?

Wayland is three things: a **model** (a client creates objects such as
surfaces and buffers, sends requests on them and gets events back;
everything a surface shows changes at once, on `commit`); a **wire
format** (each message is 32-bit words: the object's id, then the size
and the message number, then the arguments: integers, fixed-point
numbers, strings, arrays, object ids and file descriptors); and the
**interface files**, XML that lists every interface, message, argument,
version, enum and error code. Linux carries the bytes over a Unix socket
and passes file descriptors beside them.

- **(A) Real Wayland on the wire.** The bytes of every message are
  exactly Wayland's, and the interfaces, message numbers, versions, enums
  and error codes come unchanged from the upstream XML files (vendored
  under third_party, MIT). Only the transport is Jam OS's own: a channel
  instead of a socket, a handle wherever Linux passes a file descriptor,
  and a small header on each channel message (below). The C code is
  generated from the XML by a new generator, as the IDL is from `.idl`
  files. Then at M13/G2 a ported libwayland needs only a transport shim
  (its socket reads and writes become channel reads and writes, fds
  become handles), and ported Wayland programs (foot, SDL) run unchanged.
- **(B) Wayland's model in Jam OS's IDL.** Each interface rewritten as
  genidl methods. No new generator, but every ported program then needs
  a translating proxy, and M12 reviews something that is neither Wayland
  nor ours to change freely.
- **(C) A protocol of our own.** The least work now, and no ported
  programs ever.

Why a new generator and not genidl extended: genidl's model is a call
and its reply, matched by a txid, with fixed-size arguments. Wayland has
no replies (requests and events are one-way and independent), objects
are named by ids the client chooses, a request can create an object
(`new_id`), and arguments include strings, arrays and file descriptors.
Bending genidl to that would make it two generators in one file.

*Recommendation: (A)*, with Jam OS's simplifications kept to what the
compositor advertises (which interfaces and versions) and the transport,
never the bytes of a message.

### Q2. How does the compositor read a client's pixels without trusting it?

A Wayland client draws into shared memory (a `wl_shm_pool`, here a VMO)
and the compositor reads it. On Linux this is the classic `SIGBUS`
problem: the client shrinks the file and the compositor's next read
crashes it (libwayland ships `wl_shm_buffer_begin_access` to catch the
signal). Jam OS has the same problem in two forms: a read past the end
of a VMO the client shrank kills the reader (`kernel/mm/aspace.c`), and a
read of a page the client decommitted commits a fresh page charged to
the *client's* job, so a client sitting at its page limit makes the
compositor's read fault unpayable, which also kills the compositor. As
things are, mapping a client's VMO lets any client kill the compositor.

- **(A) A small kernel change: VMOs whose pages stay.** A new
  `vmo_create` flag makes a paged VMO whose pages are all committed at
  creation (charged to the creator, as now) and never leave: decommit
  and shrinking are refused for every holder, and growing commits the
  new pages at once (which is exactly `wl_shm_pool.resize`: "only
  bigger"). A new `vmar_map` flag refuses any VMO without that flag and
  fills the page tables at map time, so the compositor's reads of a
  client's pool can never fault at all. The compositor maps each pool
  read-only and paints straight from it: no copy. One agent-hour of
  kernel work with its tests, named here for M12's kernel track.
- **(B) No kernel change: copy with `vmo_read`.** `vmo_read` fails
  cleanly past the end and reads a decommitted page as zeros without
  committing it, so the compositor copies each committed buffer's damage
  into a copy of its own and paints from that. Safe, but every damaged
  pixel is copied twice more (the kernel bounces it through a page), the
  compositor holds a copy of every window (29 MiB for two full-screen
  ones), and a full-screen frame costs an estimated 3 to 5 ms more of CPU.

*Recommendation: (A).* It is the one kernel change G1 needs, it is small
and testable (ktests for each refusal), and it makes "a client can't
crash the compositor" a property of the kernel rather than of careful
code. (B) stays the fallback if M12 rejects the flag.

### Q3. The programs that borrow the screen today: windows, or borrowers?

Nine programs draw by borrowing the whole framebuffer (above).

- **(A) Every one gets a window.** libfun is the one place all nine meet
  the screen (`gfx_open`, `gfx_present`, `gfx_key`, `gfx_mouse`), so
  libfun gets a Wayland back end and the apps' own code stays as it is,
  but for one line in each program's list (`svc wayland`) and, where an
  app wants them, two opt-ins: a window that may be resized (jamjar's
  layout already handles any size) and full screen (the splash, demo,
  and any window by a key). Full screen is then a window that covers the
  screen, and the compositor copies it straight to the framebuffer
  (no blending), so it costs what a present costs today plus one copy
  in RAM. `console.lend_screen` goes: no program ever maps the
  framebuffer again.
- **(B) Borrowers stay.** The compositor lends the framebuffer and steps
  aside while a program has it, and hides every window meanwhile. Less
  work now, but the done-when needs windows from several programs
  anyway, and a borrower can draw anything anywhere, fake windows
  included.

*Recommendation: (A).*

### Q4. What the console becomes, and a way back

The roadmap says the console becomes a client, a terminal window. Today
it is four things: the screen's owner, the input hub (with Ctrl+Alt+Del),
the terminal (the text, the kernel log and its notices, the `console`
protocol programs write to) and the serial mirror.

- **The compositor takes** the framebuffer (`RIGHT_ROOT_SCREEN` goes to
  it instead), the input sources (devmgr's HID drivers and serialin
  connect to it), the pointer, and Ctrl+Alt+Del (it asks init, as the
  console does now).
- **The console keeps** being the terminal: the `console` protocol for
  programs (write, size, clear, open_keys, show_log; `lend_screen` gone
  by Q3), its text model, the kernel log and notices, and the copy of
  everything to COM1 (the QEMU tests read it). It draws its cell grid
  into its window's buffer instead of the framebuffer, and its keys come
  from the compositor while its window has the focus; inside it, the
  focus stack of `open_keys` channels works as today (a text program the
  shell runs gets the terminal's keys, Ctrl+C reaches the shell).
- **A way back:** a boot word `nocomp` (and the safe-mode entry) starts no
  compositor: the console owns the screen and the input as today, and
  libfun falls back to `lend_screen`. This costs keeping the console's
  screen code and libfun's old path a while longer.

Options: **(A)** as above, the fallback kept until G1 is signed off on
the PC, and then you decide whether it stays for safe mode; **(B)** no
fallback: the old paths are deleted in G1; **(C)** the fallback kept for
good.

*Recommendation: (A).* If the compositor fails on the PC in a way QEMU
can't show (the GOP framebuffer, the mouse), `nocomp` still boots to a
working shell, as `smp=loader` did for M8.5.

### Q5. How windows are arranged

- **(A) Floating windows with title bars the compositor draws.** Windows
  overlap; a click raises and focuses one; the title bar (the program's
  title and a close box) moves it; a resizable window's edges resize it;
  a double-click on the title bar maximises it; Alt+Tab cycles the focus
  and a key toggles full screen. The compositor draws the title bars
  ("server-side decorations"), so libfun and the console draw none and
  every window looks the same. Wayland's own default is that clients
  draw their decorations; ported programs that can do either are told
  "server-side" by `xdg-decoration`, which G2 adds with the first port.
- **(B) Tiling.** The compositor splits the screen between the windows
  (no overlap, no moving, keyboard to switch); simpler to paint and no
  mouse interaction to build, but every app must cope with any size it
  is given, and most of ours were made for one large surface.

*Recommendation: (A)*, kept small: no minimising, no window menu, no
snapping in G1.

### Q6. Keyboard layouts without xkbcommon

In Wayland the compositor sends raw key numbers (Linux's evdev key codes)
and once, a keymap file in XKB's text format; each client turns keys
into characters itself with libxkbcommon. xkbcommon is MIT-licensed but
large (a parser and compiler for that format) and wants a C library: M13
material. Today hid applies the US layout itself
(`drivers/hid/keyboard.c`) and sends the character along with each key.

- **(A) One layout file in the tree, two outputs.** A small layout file
  (US first: AU keyboards are US) is turned by a generator into a C table
  that Jam OS's own programs decode keys with (in the client library,
  libjwl), and into the XKB text keymap the compositor sends. Our own
  programs never parse XKB; ported programs (with their own xkbcommon,
  later) get the same layout from the XKB text. The generated text is
  checked by xkbcommon's own compiler on the Mac when it is installed
  (`make check` skips that step otherwise). The keymap names its layout
  in a comment on its first line, so libjwl picks the matching table.
- **(B) No keymap now.** The protocol allows `no_keymap`; libjwl assumes
  US. Less work, but the first ported program gets no layout, and the
  next layout has no way to tell our programs which one it is.
- **(C) Port xkbcommon now.** Needs most of a libc: M13.

*Recommendation: (A)*, US only in G1. hid keeps sending its characters
(the console's `nocomp` path and the serial source use them) until M12
reviews `input`; the compositor uses only the HID usage and modifiers.

### Q7. Several graphical programs from one shell

Today the shell runs one program at a time and waits for it; Ctrl+C
kills it. With windows that still works for one program, but the
done-when wants several at once.

- **(A) `&` in the shell.** `tetris &` starts the program and gives the
  prompt back at once; `jobs` lists what runs in the background and
  `kill %2` ends one. Background programs die with the shell (they live
  in its job, as now). A small change to one service.
- **(B) More terminal windows.** A key opens another terminal with a
  shell of its own. init would start a shell per terminal, and the
  console would serve several windows: more work in init, the console
  and the shell's supervision.
- **(C) Both.**

*Recommendation: (A) in G1; (B) in G2.* A program run with `&` that
reads the terminal (not a window) gets no keys: the shell keeps the
terminal's focus.

### Q8. When the compositor itself dies

- **(A) Clients reconnect.** init keeps `/svc/wayland`'s channel across
  the compositor's restart (as it does for the mixer's), the kernel
  shows its log on the screen meanwhile (the owner token closed), and
  each client sees its connection close, opens `/svc/wayland` again and
  builds its windows again. libjwl does the reconnecting; libfun keeps
  the whole frame in its back buffer, so it shows at once; the console
  redraws its grid. A program mid-move or mid-resize just starts over.
- **(B) M11.6's way: the compositor outlives its process.** Its state in
  a VMO, its client channels kept by init, a warm spare: clients never
  notice. Much more work, and M11.6's machinery is still being reviewed.
- **(C) Clients end with it.** The console must not, so (A) is needed for
  it at least.

*Recommendation: (A).* (B) can follow once M12 has reviewed M11.6's
pattern, if the compositor ever proves fragile.

### Q9. What the screen looks like after boot

- **(A)** The splash plays as a full-screen window (the compositor starts
  first and shows only the splash's background meanwhile). Then the
  terminal window opens maximised, so the screen looks as it does today
  with a thin title bar on top (88 rows instead of 90 at 2560x1440).
  Programs open as windows centred on top of it, sized by the program.
  Un-maximising the terminal shows the desktop: the splash's dark
  background colour.
- **(B)** A desktop with the terminal in a smaller window (say 160x50
  cells) in the middle.

*Recommendation: (A).* The text stays 8x16 pixels as today.

## The design in one page

- **One new service, the compositor** (bin/compositor), owns the
  framebuffer and the input. It speaks Wayland to every program that
  asked for `svc wayland` in its list, over one channel per connection
  opened from `/svc/wayland`.
- **The protocol is Wayland's** (Q1): upstream XML, upstream bytes, Jam
  OS's transport (channels, handles for fds, a header carrying flow
  control). A new generator, tools/genwl.py, makes the C tables and
  stubs; one hand-written codec in libos (libjwl) parses and builds every
  message for both sides, against those tables.
- **G1 implements** `wl_display`, `wl_registry`, `wl_callback`,
  `wl_compositor`, `wl_surface`, `wl_region`, `wl_shm`, `wl_shm_pool`,
  `wl_buffer`, `wl_output`, `wl_seat`, `wl_keyboard`, `wl_pointer`,
  `xdg_wm_base`, `xdg_positioner`, `xdg_surface`, `xdg_toplevel` and
  `xdg_popup` (popups dismissed at once, which the protocol allows).
- **Pixels**: pools are VMOs whose pages stay (Q2, one kernel flag); the
  compositor maps them read-only and paints straight from them. It
  paints only what changed, in tiles, on every CPU, into the
  write-combining framebuffer, which it never reads back; the cursor is
  drawn into each tile last.
- **Input**: HID drivers and serialin connect to the compositor
  (`compctl`, a small IDL protocol of its own); keys go to the focused
  window only, the pointer to the window under it (or the one a button
  press started in); the compositor keeps Ctrl+Alt+Del and its own
  window keys, which no client ever sees.
- **Programs**: libfun gets a Wayland back end (Q3), so the nine
  drawing programs become windows without changing; the console becomes
  a terminal window (Q4); the shell gets `&` (Q7).
- **Failure**: a client that breaks the protocol, floods or dies loses
  its connection and its windows, nothing else; a compositor that dies is
  restarted by init and its clients reconnect (Q8).

## The compositor as a service

**Startup handles** (init starts it first on a plain boot, before the
console and the splash):
- the root resource with `RIGHT_ROOT_SCREEN` only (`framebuffer_take`);
  the console loses that right (it keeps `RIGHT_ROOT_KLOG` and
  `RIGHT_ROOT_SERIAL_OUT`), except under `nocomp` (Q4);
- the server end of `/svc/wayland` (init keeps it across restarts and
  publishes the client end; each opener gets its own connection by the
  `svc` protocol's `connect`, as `/svc/music` does);
- the server end of `compctl` (abi/idl/compctl.idl, new, the next free
  IDL id): init's channel; init makes a narrower one for devmgr and one
  for serialin (below);
- init's control channel restricted to `reboot` (as the console has now,
  for Ctrl+Alt+Del);
- a namespace with nothing in it: the compositor reads no files.

**compctl**, the compositor's own protocol (Jam OS IDL, not Wayland:
these are system powers, not app features). Levels fixed on the channel
when it is made, as the console's: ADMIN (init's), INPUT (devmgr's and
serialin's: `connect_input` only).
- `connect_input() -> (handle source)`: a new input source, served the
  existing `input` protocol (`abi/idl/input.idl`) unchanged, exactly as
  `console.connect_input` today. devmgr's `usb.c` and serialin call this
  instead.
- `blank(u8 on)`: the whole screen the splash's background and nothing
  drawn (init's reboot path, as `console.blank` today).
- `new_client(u8 level) -> (handle)`: a narrower channel.
- `stats() -> (...)`: paints, pixels written, the last and worst paint
  time, clients, surfaces, disconnects by reason (for tests and `ps`).
- Reconnect rule: after `ERR_PEER_CLOSED`, ask init again (devmgr's HID
  drivers exit 0 and are bound again, as when the console restarts now).

**The loop.** One thread serves everything, on one port: compctl, every
input source, every client connection, the paint clock. Each turn takes
input first (so typing never waits behind a client), then up to a budget
of each client's messages (64 messages or 2 ms, as the console's
`CLIENT_BUDGET`), then paints if the clock says so. Painting runs on a
pool of worker threads (libfun's `pool_run`) while the loop waits for it:
nothing changes the scene while a paint reads it, so no locks are
needed between protocol code and paint code. A full-screen paint is
estimated at 2 to 4 ms; that is the longest a client's request waits.
The compositor never makes a blocking call to another process, and
never waits on a client: everything it sends a client is written without
waiting, and a client that doesn't read is dealt with by flow control
(below). It follows [the service-loop rule](../CODING-GUIDE.md#a-loop-that-serves-never-blocks)
from the start.

**At start** it takes the framebuffer and fills it with the splash's
background (`SPLASH_BG`, `<splash.h>`), so the screen goes from the
kernel's dark background to the splash with no flash. With no usable
framebuffer (`framebuffer_take` says `ERR_NOT_FOUND`) it runs headless:
the protocol works, nothing is drawn (tests use this; `headless` as an
argument forces it).

**The kernel log.** The compositor shows none of it; the terminal window
shows the log and its notices as the console does today. A panic draws
over everything, as now. While the compositor is dead the kernel draws
its log (the owner token), and the restarted compositor takes the screen
back and repaints all of it.

## The protocol

### What G1 implements

Interfaces from upstream's `wayland.xml` and `stable/xdg-shell/xdg-shell.xml`,
advertised at these versions (a client binds the lower of ours and its
own; every later version only adds messages, so raising one later is
additive):

| Interface | Version | What it gives | Notes |
|---|---|---|---|
| `wl_display` | 1 | sync, get_registry; error, delete_id | |
| `wl_registry` | 1 | the globals; bind | globals never come and go in G1 (one output, one seat) |
| `wl_callback` | 1 | done | sync and frame callbacks |
| `wl_compositor` | 4 | create_surface, create_region | v4 for `damage_buffer` |
| `wl_surface` | 4 | attach, damage, frame, opaque and input regions, commit, buffer transform and scale, damage_buffer; enter, leave | transform `normal` and scale 1 only (below) |
| `wl_region` | 1 | add, subtract | |
| `wl_shm` | 1 | create_pool; format | `argb8888` and `xrgb8888` (the two every compositor must take) |
| `wl_shm_pool` | 1 | create_buffer, resize | the pool is a VMO whose pages stay (Q2) |
| `wl_buffer` | 1 | release | |
| `wl_output` | 3 | geometry, mode, done, scale; release | one output: the framebuffer |
| `wl_seat` | 5 | get_pointer, get_keyboard; capabilities, name | `wl_touch` not offered |
| `wl_pointer` | 5 | set_cursor; enter, leave, motion, button, axis, frame, axis_source, axis_stop, axis_discrete | |
| `wl_keyboard` | 5 | keymap, enter, leave, key, modifiers, repeat_info | |
| `xdg_wm_base` | 1 | create_positioner, get_xdg_surface, pong; ping | |
| `xdg_positioner` | 1 | its setters | kept, used only to validate |
| `xdg_surface` | 1 | get_toplevel, get_popup, set_window_geometry, ack_configure; configure | |
| `xdg_toplevel` | 1 | title, app_id, move, resize, min and max size, maximised, full screen; configure, close | `set_minimized` accepted and ignored (allowed), no window menu |
| `xdg_popup` | 1 | grab; popup_done | dismissed at once with `popup_done`, which the spec allows |

**Not offered in G1** (each is an XML file and a module when a program
needs it; G2's first ports decide the order): `wl_subcompositor`,
`wl_data_device_manager` (copy and paste), `wl_touch`, `wl_shell`
(deprecated upstream), `xdg-decoration` (G2, for ports that draw their
own title bars otherwise), `xdg-activation`, `linux-dmabuf`,
`presentation-time`, `viewporter`, fractional scaling, relative pointer
and pointer constraints (games that capture the mouse), text input.

**Where G1 is simpler than a full compositor, and why:**
- **Scale and transform.** The output says scale 1 and transform
  `normal`, so a well-behaved client never asks for others; a
  `set_buffer_scale` other than 1 or a `set_buffer_transform` other than
  `normal` is a protocol error (`implementation`) rather than a silently
  wrong picture. Scaled output comes with G3's mode setting, if ever.
- **No vsync.** The GOP framebuffer has none, so the compositor paints on
  its own clock (below) and the output's mode says 60 Hz.
- **Popups** are dismissed as they appear (none of our programs has
  menus; G2's toolkit brings real ones).
- **One output, one seat.** No hot-plug of either.

### The wire format over channels

**The bytes of a message are Wayland's:** the sender's object id (u32),
then a u32 whose upper 16 bits are the message's size in bytes (header
included, at least 8) and lower 16 bits the opcode, then the arguments,
each padded to 32 bits: `int`, `uint`, `fixed` (signed 24.8), `object`
(an id, 0 for null where the XML allows it), `new_id` (an id; when the
XML gives no interface, preceded by the interface's name as a string and
its version), `string` (a u32 length including the NUL, the bytes, the
NUL, padding), `array` (a u32 length, the bytes, padding) and `fd`, which
is not in the bytes at all. Client-made ids run from 2 to 0xfeffffff,
the compositor's from 0xff000000; 1 is `wl_display`. (The Wayland
book's "Wire Format" chapter has all of this; upstream's own text is the
reference, not libwayland's code.)

**One channel message is one batch** of whole Wayland messages, behind a
16-byte transport header of Jam OS's own:

| Bytes | Field | Meaning |
|---|---|---|
| 0-3 | magic | "JWL1": the transport's version |
| 4-7 | acked | from a client: how many of the compositor's batches it has read so far (mod 2^32); from the compositor: 0 |
| 8-11 | nfds | how many handles the batch's `fd` arguments use (must equal the handles the channel message carries) |
| 12-15 | reserved | 0 |

- **An `fd` argument is a handle.** The batch's handles travel in the
  channel message's own handle list (moved, as every handle is), and
  the `fd` arguments take them in order, as file descriptors follow the
  bytes on a socket. Too many or too few handles for the batch's `fd`
  arguments is a protocol error. Only two messages G1 implements carry
  one: `wl_shm.create_pool` (the pool's VMO) and `wl_keyboard.keymap`
  (the keymap's VMO).
- **Sizes.** A Wayland message is at most 4096 bytes (libwayland's own
  limit, so a ported client never sends bigger); a batch at most 16 KiB
  and 16 handles; strings and arrays at most 4096 bytes each.
- **Flow control** (why the header has `acked`): every message a sender
  queues on a channel is charged to the sender's job until it is read,
  so a client that never reads its events would make the compositor's
  job pay for them, up to 1024 messages of 16 KiB per client. So the
  compositor sends at most 32 batches a client hasn't acknowledged
  (512 KiB at worst); libjwl puts the count it has read into every batch
  it sends, and sends an empty batch to acknowledge when half the window
  is read and it has nothing else to say. While a client's window is
  full the compositor coalesces its pointer motion (only the latest
  position is kept) and holds the rest in its own memory up to 64 KiB;
  past that it disconnects the client (`wl_display.error`, `no_memory`),
  as libwayland disconnects a client whose socket buffer is full.
- **Errors.** Anything malformed (a bad size, an unknown id or opcode, a
  string without its NUL, an id out of the client's range or not the
  next free one, a handle of the wrong type or with too few rights) is a
  protocol error: the compositor sends `wl_display.error` naming the
  object and code, then closes the connection. Fail closed, as the
  protocol itself demands.
- **Why a header and not a Wayland message of our own** for the flow
  count: the header sits below Wayland, where a ported libwayland's
  shim will sit; a Wayland-level message would need an object id from
  libwayland's own allocator.

### The generator: tools/genwl.py

- **Input:** third_party/wayland-protocols/ (new): `wayland.xml` and
  `xdg-shell.xml` from a pinned upstream release, with their MIT licence
  and a row in `third_party/VERSIONS.md`. The XML is data: reading it is
  allowed, and nothing of libwayland's code is used (the owner's rule).
- **Output** (committed, as genidl's; `make check` fails if stale):
  - user/include/jwl/<protocol>.h: per interface its version, opcodes,
    enums (error codes, `wl_shm.format`, `xdg_toplevel.state`, ...), and
    typed inline wrappers: for a client, one function per request
    (`jwl_wl_surface_attach(...)`) and a struct of event handlers; for
    the compositor, one function per event and a struct of request
    handlers.
  - user/lib/jwl_<protocol>.c: the tables the codec reads: per interface
    its name, version and messages; per message its name, the version
    it came in, and its signature (one letter per argument, plus which
    interface a `new_id` or `object` argument must be, and which may be
    null).
- **C names start with `jwl_`**, never `wl_`, so Jam OS's own library
  never collides with a ported libwayland in one program.
- The generated wrappers do no parsing of their own: they hand the
  arguments to the one codec, so there is one parser to review and fuzz,
  not one per message.

### libjwl: the codec and the client library

In libos (user/lib/jwl_*.c, `<jwl.h>`), used by the compositor and by
every client:
- **the codec**: a batch in, messages out, each checked against its
  signature before any handler sees it (sizes, padding, NUL, ids, handle
  count), arguments decoded into a fixed array (at most 20 per message);
  and the reverse;
- **the object map**: per connection, id to (interface, version,
  object); new ids checked to be next-or-reused, at most 4096 objects a
  connection;
- **the transport**: batching (a flush per loop turn), the header, the
  ack window, handles as fds;
- **for clients**: connect (`svc_open("wayland")`), the registry and
  binding, shm pools (a VMO with the Q2 flag, sized and grown),
  `jwl_window` (a toplevel with a configure/ack loop, two buffers, frame
  callbacks), the keyboard (keymap table by name, modifiers, key repeat
  from `repeat_info`, since Wayland clients repeat keys themselves) and
  the pointer, and reconnecting (Q8).

## Surfaces, buffers and damage

- **Double-buffered state** as the protocol says: attach, damage, the
  opaque and input regions, the frame callbacks, the window geometry
  are pending until `commit`, then applied at once. The compositor
  checks a committed buffer against the surface's role before using it.
- **Pools.** `wl_shm.create_pool(id, fd, size)`: the handle must be a VMO
  with read and map rights and the Q2 flag, `size` within the VMO and the
  client's pool cap; the compositor maps it read-only (filled at map
  time, so it never faults) and closes its handle (the mapping keeps
  the VMO). `resize` maps the bigger range and drops the old mapping
  once no buffer is in a paint. `create_buffer` checks offset, width,
  height and stride without overflow (`stride >= width * 4`,
  `offset + stride * height <= pool size`, width and height 1 to 8192).
  A buffer keeps its pool alive after `wl_shm_pool.destroy`, as the
  protocol says.
- **Formats.** `xrgb8888` is opaque (copied); `argb8888` is premultiplied
  alpha (the protocol's convention, and libfun's already): blended with
  the same rounding as libfun's `px_over`.
- **Release.** The compositor reads a buffer while painting, so it
  releases a buffer when a newer one has been committed on its surface
  and no paint is using it (at once, between paints), or when its
  surface goes. A client with two buffers therefore never waits on a
  hidden window.
- **Damage.** `damage` (surface coordinates) and `damage_buffer`
  (buffer coordinates, the same at scale 1) accumulate in the pending
  state; a commit adds them, clipped to the surface, to the output's
  damage, moved to where the window is. A surface that moves, appears,
  disappears, is raised or changes size damages its old and new
  rectangles. Damage is a list of at most 32 rectangles per surface and
  64 for the output; more merge into their bounding box (correct, just
  more pixels).
- **Regions** (`wl_region`, input and opaque regions, the damage lists):
  one small rectangle-list library in the compositor, bounded (at most
  256 rectangles; a region past that becomes its bounding box for
  opaque regions, which is safe, and is refused for input regions).
- **The paint clock.** The compositor paints when there is damage, at
  most once per period: 60 Hz by default, `display.hz` in the settings
  (`/data/etc/settings`) to change it. A paint after an idle period runs
  at once (typing shows without waiting for a tick).
- **Frame callbacks.** `wl_surface.frame` callbacks of a commit are
  answered (`done`, with the paint's time in ms of uptime) after the
  first paint that showed that commit. A surface that isn't visible at
  all (covered by an opaque window, or off the screen) gets them at
  most once a second, so a hidden animation slows down instead of
  running flat out or stopping dead.

## Software composition

**Where it draws.** Straight into the framebuffer (write-combining
memory), never reading it back, as the console and libfun do today. No
full-screen copy in RAM: each worker composes one tile at a time into a
small buffer of its own (a band of 16 rows, the width of the damaged
rectangle) and then writes that band to the framebuffer with wide
stores (libfun's present already does it this way).

**One tile:** the background colour, then each window that touches the
tile from the bottom up (its title bar and border, then its buffer,
blended or copied), skipping every window under an opaque one that
covers the whole tile, then the cursor. Windows are few (a cap of 64
surfaces a client, but rarely more than a handful on the screen), so
this simple painter's order is enough.

**The full-screen fast path.** One opaque window covering the screen
(the splash, a full-screen game): its damage is copied straight from its
buffer to the framebuffer, a row at a time, with no tile buffer. This is
today's libfun present.

**The cursor** is drawn by the compositor, last in every tile it
touches: a default arrow (libfun's), hidden when a client asks
(`set_cursor` with no surface), or the client's own cursor surface while
the pointer is over its window (up to 64x64, a `cursor` role surface).
Moving the pointer damages two cursor-sized rectangles: microseconds.

**Title bars** are drawn with libfun's text and shapes (the compositor
links libfun: a Makefile line), 24 pixels high at 2560x1440, the focused
window's in a brighter colour.

**The cost of a frame** (estimates, to be replaced by stage P0's numbers
from the PC):

| Frame | Pixels | Estimated cost on the PC |
|---|---|---|
| The whole screen (a full-screen video frame, a repaint) | 3,686,400 px, 14.1 MiB | 2-4 ms on many threads, if the framebuffer takes 4-7 GB/s of write-combined stores (unknown: P0 measures it) |
| A 1280x800 window, every pixel changed, blended over another | 1,024,000 px | about 1 ms |
| One line of terminal text | 2560x16 | tens of microseconds |
| The pointer moving | two 32x32 squares | a few microseconds |

Compared with today: a full-screen app costs one copy more (its back
buffer to its shared buffer, in cached RAM, on its own threads); the
copy to the framebuffer is the same.

**Tearing.** Without vsync a paint can be seen half done, as today's
presents can. Accepted until G3.

## Input

**Sources.** Each HID driver and serialin is an input source on a
`compctl.connect_input` channel, speaking `input` unchanged: keys with
their HID usage, state and modifiers; mouse reports (relative counts,
buttons, wheel); text from a terminal; `ready`.

**The keyboard.**
- Key numbers: the compositor maps each HID usage (the USB HID Usage
  Tables, Keyboard/Keypad page) to the Linux evdev key code Wayland
  expects (the numbers in Linux's input-event-codes.h, used as facts;
  the table is our own and complete for a 104/105-key keyboard). XKB's
  own key numbers are those plus 8, which the generated keymap uses.
- The keymap (Q6): one VMO holding the XKB text, made once, committed,
  and handed to each keyboard read-only (no write, no resize right), as
  `wl_keyboard.keymap` with format `xkb_v1`.
- Modifiers: Shift, Ctrl, Alt, Super from the HID modifier byte; Caps
  Lock and Num Lock as locked modifiers, which the compositor follows
  from the lock keys it sees (hid keeps the lights, as now: it starts
  with Num Lock on, and the compositor starts the same way); sent as
  `wl_keyboard.modifiers` when they change, in XKB's modifier bits for
  the generated keymap.
- Repeat: hid's own repeat events are dropped; `repeat_info` (hid's rate
  and delay, 30 a second after 500 ms) tells clients, which repeat
  themselves (libjwl does it for ours).
- Serial text (QEMU tests, a spare keyboard): each byte or escape
  sequence becomes the key presses that type it on the US layout (the
  inverse of the same table), sent to the focused window. The decoder of
  escape sequences in the console's `keys.c` moves to libos so both use
  one copy.

**The pointer.** The compositor owns its position: relative counts in,
acceleration as libfun's `pointer_move` does now (the compositor uses
that code), clamped to the screen. Clients get positions in their
surface's coordinates (`fixed`), buttons as evdev numbers (left 0x110,
right 0x111, middle 0x112), the wheel as `axis` (vertical, 10 per notch,
as other compositors use) with `axis_source` wheel and `axis_discrete`,
each report closed by `frame`.

**Focus.**
- The pointer's focus is the window under the pointer (its input
  region), except while a button is held: then the window where the
  press started keeps every pointer event until the last button is
  released (the implicit grab), so a drag that leaves the window works.
- The keyboard's focus changes only on a click into a window, Alt+Tab,
  a window closing (the next one down takes it), or a new window:
  a client's **first** window takes the focus when it appears (a
  program the shell just started), later windows of the same client
  don't (a background program can't keep stealing the keys). Focus
  leaves with `leave`, arrives with `enter` (carrying the keys held).
- **Grabs a client may ask for:** `xdg_toplevel.move` and `resize` only
  with the serial of a button press the compositor sent that client and
  that is still held; anything else is ignored (as the protocol allows).
  The move or resize is then the compositor's: the client sees nothing
  of the pointer until it ends. No client can grab the keyboard.
- **Keys no client sees:** Ctrl+Alt+Del (to init, as now), Alt+Tab
  (the next window), and the full-screen toggle (Super+F). They are
  taken before any focus is looked at.
- **Ctrl+C.** Goes to the focused window like any key. A graphical
  program can ignore it, but it can't hold the terminal's keys: clicking
  the terminal (or Alt+Tab) and pressing Ctrl+C kills what the shell
  runs, as today. The "a program can't trap the keys" promise becomes "a
  program can't trap the keys of another window".

## Window management

(Q5, floating.) The compositor keeps a stacking order of toplevels.
- **A new toplevel** is centred on the screen at the size its first
  buffer has (the client chose it), or, for the console's terminal at
  boot, maximised (Q9). A maximised window fills the screen below its
  title bar; a full-screen one fills all of it with no title bar.
- **Configure.** Every size or state change goes to the client as
  `xdg_toplevel.configure` + `xdg_surface.configure`; the compositor
  uses the client's next commit after `ack_configure`. A client that
  doesn't answer a configure within 2 s keeps its old size (the frame
  just isn't resized). For a maximised or full-screen window the client
  must fill the size; a smaller buffer is centred on the background,
  never stretched.
- **Moving** by the title bar; **resizing** by the window's edges,
  only for windows whose min and max sizes differ (libfun's windows
  aren't resizable unless the app opted in); **maximising** by a
  double-click on the title bar or the client's request; **closing** by
  the close box, which sends `xdg_toplevel.close`: the client decides.
- **Not responding.** If a client doesn't answer `xdg_wm_base.ping`
  within 5 s of a close request, its title bar says so. The compositor
  can't kill it (it holds no job of anyone's); the shell can.
- No minimising, no virtual desktops, no snapping, no window menu in G1.

## A crashed client, a crashed compositor

**A client dies, is killed or breaks the protocol:** its connection's
peer closes (or the compositor closes it after `wl_display.error`).
The compositor then, in one loop turn: drops every object of that
connection, unmaps its pools, removes its windows from the stacking
order, damages where they were, moves the keyboard focus to the window
below if it had it, and ends any move or grab it was part of. Every
other window is untouched and the next paint shows the hole filled. The
dead client's VMOs go when the compositor's mappings go (their pages
are charged to the client's dead job until then).

**The compositor dies** (Q8): the kernel shows its log on the screen
(the owner token). init restarts it like any service (the backoff, the
give-up count; but, as the console, never given up for good while the
console needs it), keeping the server end of `/svc/wayland`. The HID
drivers see their `input` channel close and exit 0; devmgr binds them
again, and they connect to the new compositor. Each client sees its
connection close; libjwl opens `/svc/wayland` again and replays what it
needs (the registry, its pools, its windows with their last buffer);
libfun redraws its back buffer, the console its grid. An app that
doesn't use libjwl's window helper sees `ERR_PEER_CLOSED` and decides
for itself.

## Limits and the security model

What must hold: **no client reads another's pixels or input, no client
can crash, stall or exhaust the compositor, and no client can draw
outside its own windows.**

- **Who may open a window:** only a program whose list asks for
  `svc wayland` (`<wants.h>`; the build's `checkwants.py` checks every
  list), plus init's own services (the console, the splash). A text
  program the shell runs never gets the compositor.
- **Pixels.** A client's pools are mapped by the compositor alone, read
  only; no VMO a client sends is ever sent on to anyone. The keymap VMO
  is the only memory the compositor hands out, read-only. No client gets
  the framebuffer (Q3). No protocol in G1 reads the screen (a screenshot
  or screen-recording protocol later needs a permission of its own in
  the program's list). QEMU tests take their screenshots from QEMU's
  monitor.
- **Input.** Keys go only to the client with the keyboard focus, pointer
  events only to the window under the pointer or holding the implicit
  grab, each in its own surface's coordinates; a client never learns
  where the pointer is outside its windows. The keys in the list above
  never reach a client. Focus moves only by the user's action or a
  client's first window.
- **A client can't crash the compositor:** every message is checked
  before use (the codec; a fuzz test with random and mutated batches);
  pools can't shrink or lose pages under the compositor's mappings (Q2,
  enforced by the kernel); sizes from the protocol are checked without
  overflow; nothing in shared memory is ever trusted for a size, an
  offset or a decision (pixels are only ever pixels).
- **A client can't stall it:** the compositor never waits on a client;
  each client's messages are served a budget per turn; flow control
  bounds what it queues for a client that doesn't read; a client that
  doesn't answer configures or pings only stalls its own window.
- **A client can't exhaust it** (caps per connection, each a protocol
  error `no_memory` or the request refused):

| What | Cap per connection |
|---|---|
| objects (ids) | 4096 |
| surfaces | 64 |
| pools | 64 |
| pool bytes mapped in total | 256 MiB (two full-screen buffers are 28 MiB) |
| buffers | 256 |
| pending frame callbacks per surface | 64 |
| damage rectangles kept per surface | 32 (more: merged) |
| region rectangles | 256 |
| events queued unread | 32 batches on the channel, 64 KiB more in the compositor |
| connections | 64 for the whole compositor |

  What a client's VMOs cost is charged to the client's job (it made
  them); the compositor's own page tables and mapping records for them
  are charged to the compositor's job, and bounded by the caps above
  (256 MiB mapped is about 130 page-table pages).
- **Titles and app ids** are shown as UTF-8 text with libfun's glyphs
  (at most 256 bytes kept); a title can't draw outside its title bar.
- **What G1 does not defend against:** a client drawing a picture that
  looks like another program's window inside its own window (every
  window has the compositor's title bar with the program's own title,
  and the focused one is marked, but there is no trusted indicator of
  which process a window belongs to); a program the user runs on
  purpose flooding the screen with windows up to its caps.

## Moving the programs onto it

**The console** (Q4) as a client: user/services/console/ gets a window
mode. Its text model, terminal, notices and clients stay as they are;
`screen.c`'s drawing goes into the window's buffer (cached memory now,
so the shadow grid's job becomes damage tracking); its keys come from
`wl_keyboard` (decoded to the same `input_key_event`s by libjwl's table)
into the existing focus stack; mouse reports for programs that asked
(`INPUT_WANT_MOUSE`) come from `wl_pointer` while the pointer is over
the terminal; the wheel scrolls back as now. `connect_input` and
`lend_screen` answer `ERR_NOT_SUPPORTED` in window mode. Under `nocomp`
the console runs as today.

**libfun** (Q3): `gfx_open` connects to the compositor and opens a
window (the size the app asks for, by default 1600x1000 at 2560x1440 and
the whole screen below that; `gfx_open_fullscreen` for a full-screen
one), keeps its back buffer and its "what is shown" copy, and
`gfx_present` copies the changed 64-pixel pieces into the free one of
two shared buffers (both kept up to date: each piece goes into the next
buffer too), damages them and commits. Keys and the mouse come from the
seat; the pointer's position is the compositor's (the app no longer
draws its own arrow, so `gfx_present_pointer` becomes a no-op).
`gfx_resizable` opts in to resizing (a `KEY_RESIZE` from `gfx_key`, then
the screen's `w` and `h` are new). Under `nocomp`, the old `lend_screen` path.

**The apps:** demo, fractal, life, tetris, snake, mines, sysmon and
jamjar change only their list (`svc wayland`) and, for jamjar, opt in to
resizing; demo and fractal's benchmark open full screen. Their `trace`
lines and selftests stay, so `tools/apps-test.sh` keeps working.

**The splash** opens a full-screen window without taking the focus
(`gfx_open_screen` becomes that), plays as today, and closes it; the
compositor shows nothing but the background until then. init's gating
of the shell on the splash is unchanged.

**The shell:** `&` and `jobs` (Q7); `kill %n`. Everything else is as
today, in its terminal window.

## The kernel change (K1)

The only one G1 needs (Q2); for M12's kernel track to review.
- `vmo_create` flag (a name like `VMO_KEEP_PAGES`): a paged VMO, every
  page committed at creation (charged to the creator's job as usual;
  fails with `ERR_NO_MEMORY` if they can't all be); from then on
  `vmo_decommit` and a shrinking `vmo_set_size` are refused
  (`ERR_BAD_STATE`) for every holder; a growing `vmo_set_size` commits
  the new pages before it returns. Not with the contiguous, DMA32 or
  physical kinds.
- `vmar_map` flag (a name like `VMAR_KEPT_ONLY`): refuses a VMO without
  that flag (`ERR_WRONG_TYPE` or `ERR_NOT_SUPPORTED`, for M12 to pick),
  and fills the mapping's page tables at once, so no access through it
  ever faults; the page tables are charged to the mapper's job at map
  time (a refused charge fails the map, not a later access).
- Tests: ktests for each refusal (decommit, shrink, map without the
  flag), that growth commits, that a mapping made with the flag never
  faults (a fault counter stays flat), the charges; a utest from user
  space with a bad flag combination, a missing right, a wrong handle
  type.

## Interface changes for M12 to review

- The Wayland transport: the 16-byte header, flow control, handles for
  fds, the size limits.
- Which interfaces and versions the compositor offers, and its
  simplifications (scale, transform, popups).
- `compctl` (new IDL), its levels; `console.connect_input` and
  `console.lend_screen` unused in window mode (to be removed if the
  `nocomp` fallback goes); `console.blank` replaced by `compctl.blank`
  for init.
- The kernel flags (K1).
- The keymap format: the layout file, the generated XKB text, the
  layout's name in its first line.
- A new service name, `wayland` (`SVC_NAME_MAX` is 10 bytes: it fits),
  and the want `svc wayland`.
- What `input` should carry once the compositor applies layouts (hid's
  codepoint: kept or dropped).

## Stages and tracks

Each track is about one agent-hour, starts from main, owns the files
listed (an edit outside them stays minimal and is named in its report),
and hands back when its tests pass. Agents run the quick tests only (the
build, `make check`, the init run at `QEMU_SMP=2`, their own tests),
never push, never touch a USB disk, never start agents of their own.
Main is frozen while tracks run. At most four agents test in QEMU at
once.

**The contract the first stage shares**, fixed here so W1 and W2 can
start together: genwl's tables are `struct jwl_interface { const char
*name; uint32_t version; uint16_t nrequests, nevents; const struct
jwl_message *requests, *events; }` and `struct jwl_message { const char
*name; const char *signature; const struct jwl_interface *const *types;
}`, where the signature is one letter per argument (`i u f s o n a h`,
`h` for a handle where Wayland says fd), `?` before a nullable one, and
a leading number for the version it came in; `types` names the
interface of each `o` and `n` argument (NULL for others). Both in
user/include/jwl.h, which W2 owns and W1 includes.

| Track | What | Files it owns (new ones without a path check) | Needs |
|---|---|---|---|
| **P0. Measure** | `fbbench`: borrows the screen today (`lend_screen`) and measures write-combined stores to the framebuffer (1, 4, 8, 16, 28 threads; whole frame and 64-pixel pieces), a RAM-to-RAM copy, the premultiplied blend per megapixel, and `vmo_read`'s copy rate (Q2's (B)); prints `fbbench:` lines. The owner runs it on the PC; the numbers replace this plan's estimates | user/tests/fbbench/ | nothing |
| **W1. The generator** | third_party/wayland-protocols (pinned XML + licence + VERSIONS.md row); tools/genwl.py (gen, check); the generated headers and tables; the Makefile's `wl` target and the check; a genwl selftest on a small XML of its own (every argument type, nullable, enums, `since`) | tools/genwl.py, third_party/wayland-protocols/, `third_party/VERSIONS.md`, user/include/jwl/, user/lib/jwl_wayland.c, user/lib/jwl_xdg_shell.c, the Makefile's lines for them | the owner's answers |
| **W2. The codec** | `<jwl.h>`'s tables struct (the contract), the codec (decode and check a batch against a signature, encode), the object map, the transport header and ack window, handles as fds; a utest with hand-written tables: every argument type, every malformed case, and a fuzz loop (random and mutated batches, never a crash, always an error) | user/include/jwl.h, user/lib/jwl_wire.c, user/lib/jwl_map.c, user/tests/utest/jwl.c, utest's table | the owner's answers |
| **K1. Kept pages** | the kernel flags (above), their ABI constants and checks, ktests, a utest | `kernel/object/vmo.c`, `kernel/mm/aspace.c` (map-time fill only), `kernel/abi/vmo_sys.c`, `kernel/abi/sysc_vm.c`, `kernel/include/jam/abi.h` (two constants), a new kernel/test/ file and its table line, user/tests/utest/keptvmo.c | the owner's answers |
| **KM. Keymap** | abi/keymap/us.txt (the layout), tools/genkeymap.py, the generated user/lib/keymap_us.c (HID usage to evdev code, evdev code and modifiers to code point, and the XKB text), user/include/keymap.h; a utest (every key of a 105-key board, the inverse used for serial text); the xkbcommon check on the Mac when `xkbcli` is installed | abi/keymap/, tools/genkeymap.py, user/lib/keymap_us.c, user/include/keymap.h, user/tests/utest/keymap.c | the owner's answers |
| **C1. The compositor's core** | bin/compositor: the loop, connections (svc connect, budget, flow control, limits, protocol errors, teardown), `wl_display`, `wl_registry`, `wl_callback`, `wl_compositor`, `wl_surface` (state, commit, damage), `wl_region` and the region library, `wl_shm`/pool/buffer (mapping with K1's flag), `wl_output` (geometry only); `headless`; the scene structure C2 to C4 build on (comp.h, written first and handed back early) | user/services/compositor/ main.c, conn.c, display.c, surface.c, region.c, shm.c, comp.h | W1, W2, K1 |
| **L1. The client library** | libjwl's client side: connect, registry, shm pools, `jwl_window` (toplevel, configure/ack, two buffers, frame callbacks), keyboard (keymap by name, modifiers, repeat), pointer, reconnect; wltest (user/tests/wltest: a client that opens windows of known colours, checks configure, release and frame callbacks, prints what it receives) run against C1 headless | user/lib/jwl_client.c, user/lib/jwl_window.c, user/lib/jwl_seat.c, user/include/jwl.h's client half (coordinated with W2's owner by appending only), user/tests/wltest/ | W1, W2, KM (C1 for its end-to-end tests) |
| **C2. Painting** | `framebuffer_take`, the background, tiles on the worker pool, copy and blend, the full-screen path, the cursor (arrow, hidden, client surface), the paint clock, frame callbacks and release timing, `stats`; paint-time lines in the log; the Makefile line linking libfun | user/services/compositor/ output.c, paint.c, cursor.c, clock.c | C1 |
| **C3. Windows** | `xdg_wm_base` (ping), `xdg_positioner`, `xdg_surface`, `xdg_toplevel`, `xdg_popup` (dismissed); stacking, placement, maximise, full screen, move and resize as compositor grabs, title bars and the close box, not-responding | user/services/compositor/ xdg.c, wm.c, deco.c | C1 |
| **C4. The seat** | compctl.idl and its server (levels, connect_input, blank, new_client, stats), input sources (the `input` protocol), serial text to keys (the escape decoder moved to libos), `wl_seat`, `wl_keyboard` (keymap VMO, keys, modifiers, repeat_info), `wl_pointer` (acceleration, enter/leave, buttons, axis, frame), focus and the implicit grab, the reserved keys (Ctrl+Alt+Del to init) | abi/idl/compctl.idl and its generated header, user/services/compositor/ seat.c, keyboard.c, pointer.c, focus.c, sources.c, a libos file for the escape decoder | C1, KM |
| **L2. libfun on windows** | libfun's Wayland back end (gfx_open/present/close, keys, mouse, resizable, full screen), the `lend_screen` path kept for `nocomp`; every drawing app's list gains `svc wayland`; jamjar opts in to resizing; the splash's full-screen window | user/apps/fun/ (a new wl.c, gfx.c, keys.c, mouse.c, fun.h), the `JAM_WANTS` line of each app in user/apps/, user/apps/splash/main.c (its open call only) | L1 (C2-C4 for its screenshots) |
| **T1. The terminal** | the console's window mode: drawing into its buffer, keys and mouse from the seat into the focus stack, reconnect, `connect_input`/`lend_screen` refused in window mode; `nocomp` keeps today's mode | user/services/console/* | L1 |
| **I1. Wiring** | init: start order (compositor first), its handles, `/svc/wayland` kept across restarts, compctl channels for devmgr and serialin, the console without `RIGHT_ROOT_SCREEN`, reboot's blank through compctl, `nocomp` (a boot word kept by kexec, and the safe-mode entry); devmgr's `usb.c` and serialin on compctl; `SVC_WAYLAND` in `<os.h>`; checkwants knows `svc wayland` | `user/services/init/services.c`, `user/services/init/shell.c`, `user/services/init/reboot.c`, `user/services/init/init.h`, `user/services/devmgr/usb.c`, `user/services/serialin/main.c`, `user/include/os.h` (one line), `tools/checkwants.py`, `boot/limine.conf` (the safe-mode entry) | C4 (for compctl), T1 |
| **S1. `&` in the shell** | `cmd &`, `jobs`, `kill %n`; a shell-test script | `user/services/shell/sh_exec.c`, `user/services/shell/sh_program.c`, user/services/shell/cmd/jobs.c, the command table, tools/shell-tests/jobs.txt | nothing (can start any time after the answers) |
| **X1. End-to-end tests** | tools/g1-test.sh (boot with usb-kbd and usb-mouse, at 1280x800 and at 2560x1440); tools/screencheck.py (checks pixels of a QEMU screendump at known places); wlhostile (user/tests/wlhostile: malformed batches, floods, a client that never reads, pools at every cap, a shrink attempt, a client killed mid-commit); shell-test scripts (several windows, focus by click, typing goes to one, kill one) | tools/g1-test.sh, tools/screencheck.py, user/tests/wlhostile/, tools/shell-tests/g1-*.txt | C2, C3, C4, L2, T1, I1 |
| **J. The join** | the docs: ARCHITECTURE ("Graphics" rewritten as built, "Userland", the IPC protocol list, "What Jam OS defends against", the Drivers and services table), CODING-GUIDE (the "add a user program" recipe's step 5: a window through libfun; a recipe "add a Wayland interface"), TESTING (the new scripts and boot words), README (the desktop, `&`), ROADMAP; the area tests once on the merged result | the docs | all above |
| **R. Review and fix** | the independent review-and-fix agent over all of G1 (the standing rule): the codec and the limits first, then focus and input routing, then the kernel flags | what its findings touch | J |

**Order and parallel work:**

1. After the owner's answers: **P0, W1, W2, K1 and KM** together (P0 goes
   to the PC as soon as it lands; the others are independent: new files,
   one kernel area). **S1** whenever an agent is free.
2. **C1**, and **L1** beside it (L1's own tests use a mock until C1
   merges, then wltest against C1 headless). C1 hands back comp.h early.
3. **C2, C3 and C4** together (three different sets of files of one
   service, meeting only in comp.h, which C1 owns: a change there is
   named in the report), with **L2** and **T1** beside them (four agents
   testing at most: L2 and T1 test against a headless compositor until
   C2 merges).
4. **I1**, then **X1**, then **J**, then **R**, then the PC.

Merge order within a step: C1 before anything in the compositor; C4
before I1 (compctl); T1 before I1.

## Tests

| What | Where |
|---|---|
| The generator: every argument type, nullable, `since`, enums; a stale output fails `make check` | genwl's selftest (W1) |
| The codec: every argument type round-trips; every malformed batch (sizes, padding, NUL, ids out of range or not next, unknown opcode, wrong handle count) is an error, never a crash; random and mutated batches | utest (jwl.c) |
| Kept-pages VMOs: refusals, growth commits, a mapping never faults, charges | ktest + utest (keptvmo.c) |
| The keymap: every key of a 105-key board, shift and caps, the inverse for serial text | utest (keymap.c) |
| The compositor headless: globals, surfaces, commit and damage, pools at their caps, buffer checks, release, frame callbacks, configure/ack, teardown on close | wltest against `compositor headless` |
| Hostile clients: malformed batches, floods, never reading (disconnected at the cap, compositor alive), a shrink or decommit of a pool refused, a client killed between attach and commit; every other window still painted | wlhostile in tools/g1-test.sh |
| Painting: known colours at known places, an argb window blended over another to the expected value, the cursor drawn and hidden, a full-screen window | tools/g1-test.sh + screencheck.py |
| Input: a click focuses a window, typed keys reach it and not the others, Alt+Tab, the implicit grab, a move by the title bar, Ctrl+Alt+Del | tools/shell-tests/g1-*.txt (QEMU monitor `mouse_move`, `mouse_button`, `sendkey`) |
| A crashed client: its window gone, the others painted, focus moved | g1-test.sh |
| The compositor killed: the terminal and a libfun app come back on their own | g1-test.sh |
| `nocomp`: today's console and apps, unchanged | the existing screen, apps, mouse and splash tests, run once with `nocomp` |
| Every existing test (the console's, apps, splash, mouse, usbkeys) passes on the compositor | as today; scripts that look at pixel positions get a 2560x1440 or window-aware variant where needed |

## What only the PC can show

1. **After P0** (the owner flashes with `make flash`): `run fbbench`; the
   `fbbench:` lines (from the log on the Mac). They decide the tile
   size and whether a frame fits the estimates.
2. **After I1** (a flash): the compositor on the real framebuffer and
   mouse: the splash, the terminal, `jamjar &`, `tetris &`, `sysmon &`;
   move, raise, focus, type; the compositor's paint lines
   (`grep 'compositor: paint'`), jamjar's frame rate. If anything fails
   to come up: the `nocomp` boot entry.
3. Compositor restart: `kill compositor`; every window comes back.
4. **Sign-off:** windows from several programs on the screen with the
   shell in one; All tests; `soak 10` with the SanDisk mounted
   read-write and pulled and replugged; the BENCH.md lines (a
   full-screen frame, a typical frame, a pointer move, kill-to-repaint
   of the compositor); then G1 is marked done, and the owner decides
   whether `nocomp` stays (Q4).

## Risks

- **The framebuffer's write speed** is unknown on the RTX's GOP
  framebuffer (behind Resizable BAR at 256 GiB); if it is much slower
  than estimated, full-screen frames cost more than 4 ms. P0 measures it
  before the paint code is written; the paint code already writes only
  damage.
- **Wayland's details** (double-buffered state, configure/ack serials,
  object id rules) are easy to get subtly wrong in ways our own clients
  never exercise. The tests use the protocol's own rules, the review
  reads against the XML's text, and G2's first ported client is the
  real test; M12 reviews before anything is frozen.
- **The XKB text** is generated and never used by a Jam OS program in
  G1; only xkbcommon on the Mac checks it, when installed.
- **Ported clients and growing pools.** A POSIX shim at M13 grows a
  shared-memory file with `ftruncate`; kept-pages VMOs grow, so that
  works, but shrinking one fails where Linux allows it (a client shrinks
  only when it gives the pool up, so the shim can make a new VMO).
- **The console's restart rules** change (it is now a client of a
  service that can restart); `tools/shell-tests/console-restart.txt` and
  the console's never-give-up rule must still hold. T1 and I1 check it.
- **Paint inside the loop** holds client requests for a paint's length
  (a few ms). If the PC shows input lag, painting moves to a thread of
  its own with a snapshot of the scene; the loop rule allows it.

## References and licences

- The Wayland protocol: upstream's `wayland.xml` (MIT; read here through
  the Fuchsia mirror of a 1.19-era copy, since the upstream host was
  refusing scripted fetches; W1 vendors a pinned upstream release) and
  wayland-protocols' `stable/xdg-shell/xdg-shell.xml` (MIT). The XML is
  vendored as data under third_party; no libwayland code is used.
- The Wayland book, "Wire Format" (wayland.freedesktop.org/docs/book):
  the message header, argument encodings, id ranges, fd passing.
- USB HID Usage Tables 1.4, section 10 (Keyboard/Keypad page): the usage
  numbers hid already uses.
- Linux's input-event-codes.h: the evdev key and button numbers
  Wayland uses, taken as facts (numbers only, no code).
- The XKB keymap text format, as documented by xkbcommon (MIT): only to
  write the generator's output; xkbcommon itself only as a checking tool
  on the Mac.
