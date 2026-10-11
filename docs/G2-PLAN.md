# G2 plan: ported Wayland programs

Status (2026-10-11): a plan, nothing built. Written against main e30d7fec
(M13's B1-B6, S1, T1, T2, X1, K1, K2 merged; X2, S2, P1 and P2 running;
M13.6 and M13.7 planned and answered as recommended, their first tracks U1
and PO1 running). **Waiting for the owner's answers to the questions
below.** The shim (GS1) and the compositor's first track (GC1) touch no
other milestone's files and can start as soon as Q1 and Q4 are answered;
the ports wait for M13.7's builder.

Goal ([roadmap](ROADMAP.md#now), "porting first", the owner, 2026-10-11;
the G2 row of [Later](ROADMAP.md#later)): **graphical Linux programs
that draw in software are ported unchanged.** libwayland is ported with a
shim from Unix sockets and file descriptors to Jam OS channels and
handles; the compositor ([HISTORY: G1](HISTORY.md#g1-the-desktop))
offers the protocols those programs need; fonts, keyboards and the
first toolkits arrive as M13.7 ports; and a ladder of real programs
proves it. Jam OS writes no general apps of its own
([Not planned](ROADMAP.md#not-planned)), so this is how every
graphical app reaches it, up to Chromium's Fuchsia port one day.

**Done when:**
1. **libwayland-client is a port with nothing patched in its transport**:
   built static by M13.7's builder, it connects to `/svc/wayland` through
   libc's new Wayland socket (section 1), and a test client built on it
   (wlprobe, track GT) passes in QEMU: the registry, a `wl_shm` pool from a
   memfd, the keymap mapped and compiled by xkbcommon, a paste both ways
   through a pipe, 28 descriptors in one flush, libwayland's own
   one-byte-with-descriptors flush, a slow reader, 1,000 connections
   opened and closed.
2. **foot runs on the PC**: in a window with the compositor's own title
   bar (server-side decorations), tiled and floating, resized; JetBrains
   Mono through fontconfig and FreeType; the keyboard with its modifiers
   and repeat; mouse selection; copy from foot and paste into a Jam OS
   terminal and back; `foot -e vim` (a POSIX program on foot's
   pseudo-terminal) edits a file; foot killed takes only its window.
   With M13.5's mksh merged, a shell in foot as well.
3. **An SDL2 program runs**: SDL's own test programs (a sprite test, the
   keyboard and mouse tests) and then **Chocolate Doom with Freedoom**,
   playable on the PC at its 35 frames a second in a window and full
   screen, with sound through the mixer.
4. **An image viewer runs**: swayimg shows PNG and JPEG files from `/data`,
   zooms, and moves through a directory.
5. **A ported client can hurt only itself**: the compositor's hostile
   tests run again with libwayland-based clients (X1's style, G1), the
   shim gets a fuzz loop, and each of the three apps killed, hung or fed
   garbage leaves the desktop and the other windows as they were.
6. Every library is a package built from a recipe (M13.7), static, with
   its licence; `pkg install foot` on the PC brings its whole closure.
7. As every milestone: an independent review-and-fix, then All tests and
   `soak 10` on the PC with a ported app open, and the desktop's
   checklist by hand with foot, Doom and swayimg among the windows.

## What G2 is not

- **No GPU.** Software rendering only. G3 (mode setting) and G4 (3D) are
  deferred (porting first). No OpenGL or Vulkan in G2: what that rules
  out, and the options for later, are in section 6 and Q3.
- **No GTK and no Qt.** The roadmap makes them a porting project of their
  own. This plan says what each pulls in (section 7) and offers a GTK3
  spike as a stretch track that does not gate the sign-off (Q7).
- **No X11.** No Xwayland, no X server: X11 programs are not ported.
  (Redox and FreeBSD carry X; Jam OS has no use for it while every
  toolkit worth porting speaks Wayland.)
- **No new compositor.** Weston and wlroots stay unported (they need DRM,
  KMS, libinput and udev: [the G1 row](ROADMAP.md#later)); Jam OS's own
  compositor gains the protocols the apps need (section 2).
- **No Unix-domain sockets in general.** The shim answers `connect` to
  `/svc/wayland` only; `SCM_RIGHTS` is carried for that socket kind only
  (section 1). General `AF_UNIX` with descriptor passing (D-Bus, X11) is
  not built.
- **No dynamic linking** (M14.5): every port is static (section 9).
- **No input methods, no drag and drop, no fractional scaling, no second
  keyboard layout** (section 8): each is named, with what it would take.
- **No change to JWL2** (`user/include/jwl.h`): the shim speaks the
  transport G1 built, so the compositor sees a ported client exactly as it
  sees one of ours. That was G1's point.
- **No Jam OS toolkit.** The roadmap's old G2 row listed microui, Nuklear
  or LVGL for Jam OS's own apps; under "porting first" the system's own
  programs (the compositor, settings, the console) keep libfun, and G2
  ports nothing for them (section 7).

## Where G2 starts

| What | Today | Where |
|---|---|---|
| The compositor | Jam OS's own, speaking Wayland's wire format over channels; one output, scale 1, no vsync (frame callbacks after each paint) | `user/services/compositor/` ([ARCHITECTURE: Graphics](../ARCHITECTURE.md#graphics)) |
| Globals offered | `wl_compositor` 4, `wl_shm` 1 (ARGB8888 and XRGB8888), `wl_output` 3, `wl_seat` 5 (keyboard and pointer, never touch), `xdg_wm_base` **1**, `wp_cursor_shape_manager_v1` 2, `wl_data_device_manager` 3, `jam_window_memory_v1` 1 | `user/services/compositor/display.c`, the versions in `user/services/compositor/comp.h` |
| Not offered | `wl_subcompositor`, `zxdg_decoration_manager_v1`, `wp_viewporter`, relative pointer, pointer constraints, primary selection, text input, `xdg_activation_v1`, presentation time, fractional scale | |
| Popups | `xdg_popup` is accepted and dismissed at once (`popup_done`), its positioner checked | `user/services/compositor/xdg.c` |
| Decorations | the compositor draws a title bar on every toplevel (Jam OS's clients draw none) | `user/services/compositor/deco.c` |
| The transport, JWL2 | a batch per channel message: a 16-byte header, whole Wayland messages, at most 16 KiB and 28 handles; handles a queue per direction as fds on a socket; the compositor keeps at most 32 batches unread at a client, then holds 64 KiB, then disconnects it ("too slow"). `<jwl.h>` already describes how a libwayland shim must cut a flush into batches | `user/include/jwl.h`, `user/lib/jwl_transport.c` |
| Limits that match libwayland's | a message at most 4,096 bytes (libwayland's own maximum), 28 handles a batch (libwayland's `MAX_FDS_OUT`) | the same |
| The keymap | the US layout as a whole XKB text keymap, generated (nothing from xkeyboard-config), in a kept VMO handed to each `wl_keyboard` read-only; `tools/genkeymap.py check` compiles it with xkbcommon's `xkbcli` when installed | `tools/genkeymap.py`, `abi/keymap`, `user/services/compositor/keyboard.c` |
| Copy and paste | `wl_data_device_manager` 3, text only, the transfer through a channel the reader makes ("a pipe"); write-only channel ends accepted (M12's W16); `start_drag` cancels its source | `user/services/compositor/data.c` ([ARCHITECTURE: Copy and paste](../ARCHITECTURE.md#copy-and-paste)) |
| memfd and `shm_open` | a kept VMO (`VMO_KEEP_PAGES`), grown with the file and **committed and charged as it grows, never shrinking** (M12's W15); seals kept in libc | `user/posix/sys/memfd.c` |
| Descriptors for the shim | pipes and socketpairs over channels, `poll`, `select`, `epoll`, eventfd, ptys (`posix_openpt`, at most 8, `/dev/pts/N` openable only by its maker); `sendmsg` with ancillary data is EINVAL; `AF_UNIX` is `socketpair` only | `user/posix/sys/`, [M13-PLAN.md](M13-PLAN.md#8-sockets-and-names) |
| Not in the POSIX layer | `timerfd`, `signalfd`, `inotify`, `fallocate`'s hole punching, `fork` (M13.5), a RAM `/tmp`, execute on anonymous memory (no JIT, by design: `user/posix/sys/mem.c`) | |
| Fonts | stb_truetype in libfun, reading only its two built-in faces (Inter, JetBrains Mono, SIL OFL), no shaping | `third_party/stb_truetype`, `third_party/inter`, `third_party/jetbrains-mono` ([Smooth text](../ARCHITECTURE.md#smooth-text)) |
| The Wayland XML | `wayland.xml` 1.26.0, `xdg-shell.xml` and `cursor-shape-v1.xml` from wayland-protocols 1.49, vendored for `tools/genwl.py` | `third_party/wayland-protocols` |
| Ports | M13.7's recipes, builder (meson, cmake and configure styles, a sysroot per port, pkg-config pointed into it), packages and `pkg`: planned and answered, PO1 running | [M13.7-PLAN.md](M13.7-PLAN.md) |
| The compiler | `tools/jamos-cc` over x86_64-elf-gcc 15.2: C only in practice. `x86_64-elf-g++` exists, but **there is no C++ runtime** (no libstdc++ or libsupc++ for the target) | `tools/jamos-cc` |
| Launching an app | the shell runs a program with what its list asks (`svc wayland` gives `/svc/wayland`); the search box only starts the apps in `<deskapps.h>` (Jamjar) | `user/include/wants.h`, `user/include/deskapps.h` |

Five facts found while reading, each of which shapes this plan:

- **SDL has no software path on Wayland.** Neither SDL2 (2.32's branch)
  nor SDL3 (main, fetched 2026-10-11) gives its Wayland driver a window
  framebuffer: `SDL_GetWindowSurface` and SDL's software renderer fall
  back to a GL or GLES texture, and with no GL they fail ("No hardware
  accelerated renderers available"). Their `wl_shm` code draws only
  cursors. So "SDL programs that draw in software" need either a patch
  giving SDL's Wayland driver a `wl_shm` framebuffer (about 250 lines,
  section 7) or Mesa. The roadmap row assumed SDL would just work; it
  won't, unpatched.
- **foot sizes its buffer pool at 512 MiB** (its "scrollable" pool,
  `max-shm-pool-size-mb`): it truncates a memfd to 512 MiB, then shrinks
  it back if hole punching fails. On Jam OS a memfd is a kept VMO, which
  commits and charges every page as it grows and never gives one back,
  and the compositor refuses a client's pools past 256 MiB in all. So
  foot's default would cost half a GiB per window and then be
  disconnected. Its recipe sets the pool's maximum to 0 (foot's own
  switch). Other clients that size pools large and sparse will hit the
  same wall (Q12).
- **foot forks to start its shell**, then `setsid`, opens the pty's slave
  by name, `TIOCSCTTY`, `dup2`s it to 0, 1 and 2 and execs. Jam OS has
  no `fork` before M13.5, and a slave can be opened only by the process
  that made it. foot's spawn needs an override (`posix_spawn` with the
  slave handed as 0, 1 and 2 and `POSIX_SPAWN_SETSID`). And the shell it
  starts must speak bytes on a pty: the Jam OS shell speaks the console
  protocol, so foot's useful shell is M13.5's mksh.
- **libwayland needs libffi.** Its dispatcher calls listeners through
  `ffi_call` (connection.c's `wl_closure_invoke`), so libffi (MIT) is a
  port too; only `ffi_call` is used, never closures, so no executable
  memory is needed. Its build also insists on finding `SFD_CLOEXEC` and
  `TFD_CLOEXEC` in musl's headers (it does), though the client never
  calls them.
- **foot needs `wl_subcompositor` and `timerfd`.** It refuses to start
  without subsurfaces ("no sub compositor"), and it makes about a dozen
  timers with `timerfd_create` (key repeat, the cursor's blink, flashes).
  Neither exists today.

*Unsure*: the SDL finding is from reading both branches' video drivers
and SDL_video.c, not from running them; it matches SDL's design (its
Wayland driver always assumed EGL).

## How others do it

How other systems run Wayland (or X) clients, read for what Jam OS can
use. The closest in spirit is Fuchsia's bridge; the closest in practice is
FreeBSD, which runs libwayland unchanged on its own sockets.

| System | How clients reach the display | Sockets and descriptors | Toolkits and apps | Borrow | Leave |
|---|---|---|---|---|---|
| **FreeBSD** | real Wayland: libwayland as on Linux; compositors (sway, KDE, GNOME) on DRM through LinuxKPI's drm-kmod; X11 too | real `AF_UNIX` with `SCM_RIGHTS`; epoll, timerfd and signalfd from **epoll-shim** (a user-space library over kqueue); inotify from libinotify-kqueue | the whole Linux desktop, from ports, mostly with no patch beyond epoll-shim | **a small user-space shim library in place of kernel features**, linked into every port that needs it (our timerfd); upstream wayland carries FreeBSD and OpenBSD in its own build (`host_machine.system()` checks), so a new platform is accepted there | its kernel sockets: we don't need them |
| **OpenBSD, NetBSD** | the same shape as FreeBSD's, later (wayland's build gained OpenBSD; NetBSD through pkgsrc) | as FreeBSD | sway, foot and others from ports | that upstream takes small portability patches | |
| **Redox** | its own display server, **orbital**, with its own protocol; SDL2, winit and iced have orbital back ends. In November 2025 it ported Smithay's Smallvil compositor *onto* orbital, and GTK3 (and WebKitGTK) ran on it, "performance not adequate" | Unix-domain sockets with `SCM_RIGHTS`, `SO_PEERCRED` and `MSG_DONTWAIT` added to its kernel and relibc for it | GTK3, WebKitGTK; an X11 MATE desktop too | the order: native toolkit back ends first, real Wayland later, GTK3 before GTK4 | building Unix sockets into the kernel: our channels already carry what Wayland needs |
| **Haiku** | a Wayland **server inside each client**: an add-on (by X512) loaded into every Wayland program, translating the protocol into native BWindows and bitmaps; no separate compositor | in-process: no socket at all | GTK3 and GTK4 apps through it; Qt has a native Haiku back end | that a compositor need not be a port of anyone's | the in-process server: our compositor already speaks the protocol, and a per-process server would hand every client the screen's policy |
| **Genode** | its own GUI server (nitpicker) and a `Gui` session; no Wayland | none: sessions and capabilities | **toolkit back ends of its own**: Qt5/Qt6 through a QPA plugin for its GUI session (Falkon with QtWebEngine runs), SDL through a Genode video driver, Mesa (softpipe; GPU drivers for Intel) | a microkernel's road to big apps (a browser) through one toolkit back end; Mesa's softpipe as the first GL | a back end per toolkit: each is maintained against that toolkit forever. *Unsure* whether Genode has llvmpipe |
| **SerenityOS** | its own window server and LibGUI; no Wayland, no X11 | its own IPC | SDL2 with a Serenity video driver, and ports written against LibGUI | a per-port test list | the per-toolkit back ends, as Genode's |
| **Fuchsia** | Scenic/Flatland; a **Wayland bridge** (`wayland_bridge`) translates Linux guests' Wayland (over virtio-wl) into Flatland | channels and handles, as ours: the bridge turns fds into VMOs and channels | Linux apps in a VM; Chromium natively (its Fuchsia port, Jam OS's model for a browser) | **the translation itself**: Wayland's fds as VMOs and channels is exactly G1's choice, and what the shim does on the client's side | a VM |

**What we take**: libwayland unmodified, with the platform's differences
in a small library under it (FreeBSD's epoll-shim pattern, in our libc);
Fuchsia's mapping of descriptors to VMOs and channels (G1 already chose
it); Redox's order (simple apps, then GTK3, never X); and Genode's
lesson that one native back end per toolkit is a maintenance debt we
avoid by having a real Wayland compositor. **What is our own**: the
compositor, its isolation rules (focus-only keys, the clipboard's rules,
per-client caps), and every ported program's list.

## The design

### 1. The shim: libwayland on channels

libwayland-client talks to a socket. It opens it in one of two ways
(wayland-client.c, 1.26): `WAYLAND_SOCKET`, a descriptor number already
connected (used once, then unset), or `socket(PF_LOCAL, SOCK_STREAM)` and
`connect` to `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`, or to `WAYLAND_DISPLAY`
itself when it is an absolute path (since wayland 1.15; then
`XDG_RUNTIME_DIR` is not needed). After that it only calls `sendmsg`
(with `MSG_NOSIGNAL | MSG_DONTWAIT`, its descriptors as one `SCM_RIGHTS`
block of at most 28), `recvmsg` (`MSG_DONTWAIT | MSG_CMSG_CLOEXEC`, room
for 28), `poll` or `ppoll` on the descriptor, `fcntl` and `close`. When a
flush carries descriptors it first sends **one byte** with them (to keep
Linux from separating descriptors and bytes), then the rest; any flush
may stop in the middle of a message.

Three places the shim could live (Q1):

| | (A) A descriptor kind in libc | (B) Overrides in libwayland | (C) A connected descriptor from the starter |
|---|---|---|---|
| What | `connect(AF_UNIX, "/svc/wayland")` gives a **Wayland socket** descriptor whose `sendmsg`/`recvmsg` turn the byte stream and `SCM_RIGHTS` into JWL2 batches and handles | the port overrides wayland-os.c and connection.c's send and receive to speak JWL2 on a channel directly | the shell (or init) opens `/svc/wayland` and hands the program a descriptor in `WAYLAND_SOCKET` |
| libwayland | unmodified | two overridden files, checked against upstream's hashes | unmodified, but the descriptor still needs (A)'s kind |
| Who else gets it | every client of the socket: libwayland's, and later a pure-Rust or Go Wayland library (wayland-rs's own back end, Go's) that calls `sendmsg` itself | only libwayland users | as (A) |
| Connections a process may open | any number | any number | one (libwayland unsets the variable); a toolkit that opens a second connection (clipboard helpers do) fails |
| Where the code is | Jam OS's BSD libc | a port's override (MIT) | the starter and libc |

**Recommendation (Q1): (A).** It is FreeBSD's shape: the platform answers
the calls, the library stays upstream's. The design of (A):

- **Connecting.** libc's `connect` on an `AF_UNIX` stream socket looks the
  path up in a small table of **service sockets**: today one row,
  `/svc/wayland` with the JWL2 framing. A path in the table opens the
  service (`svc_open`, the namespace's `svc.connect`: the program's list
  must have asked for `svc wayland`, else ENOENT, as a missing socket
  file); any other path is ENOENT, as today's EAFNOSUPPORT stays for
  `bind` and `listen`. The shell sets `WAYLAND_DISPLAY=/svc/wayland` for
  every program (harmless for those without the grant), so libwayland
  connects without `XDG_RUNTIME_DIR`. A later service socket (a D-Bus
  broker) is a new row with its own framing, not a new mechanism.
- **Sending** (the stream into batches). The descriptor keeps an out
  buffer of at most one partial message (4,096 bytes) and the handles
  waiting for a batch (at most 28). `sendmsg` translates each
  descriptor in the `SCM_RIGHTS` block to a handle (below) before taking
  any byte, then takes bytes: whole messages (their 8-byte headers give
  the sizes; a size under 8, over 4,096 or not a multiple of 4 is EINVAL,
  as the compositor would refuse it anyway) go into batches of up to 16
  KiB, the waiting handles riding in the first batch written after they
  came (`<jwl.h>`'s rule: a message's handles come no later than the
  message). It returns the bytes it took. When the channel is full it
  keeps what fits in its buffer and returns the count, or EAGAIN when it
  took nothing (libwayland then polls for `POLLOUT`), so a stream
  socket's partial write semantics hold. libwayland's one byte with
  descriptors simply becomes the first 1 byte of a buffered message.
- **Receiving** (batches into the stream). `recvmsg` reads the next batch
  only when the previous one is used up. A batch's handles are delivered
  as descriptors, all of them, with its first bytes (libwayland's room is
  28, JWL2's maximum); a caller with less room gets what fits, the rest
  are closed and `MSG_CTRUNC` is set, as Linux does. Bytes are delivered
  as the caller's buffer allows. A batch that breaks JWL2 (the magic, a
  count over 28) ends the connection with ECONNRESET: the compositor is
  not argued with, as libjwl's client does. `read` and `write` work too
  (no descriptors; handles that arrive with a plain `read` are closed, as
  on Linux).
- **Descriptors to handles** (what `sendmsg` may carry):

  | Descriptor | Handle sent | Used by |
  |---|---|---|
  | memfd or `shm_open` | a duplicate of its kept VMO, read, map, inspect and transfer only | `wl_shm.create_pool` (the compositor checks it is kept) |
  | a pipe's write end, a socketpair end | a duplicate of the channel end, with the description's rights | `wl_data_offer.receive` (W16) |
  | a pipe's read end | the same | (allowed; the compositor refuses what it can't use) |
  | anything else (a file on `/data`, a socket, eventfd, a tty) | none: `sendmsg` fails with EINVAL and logs the descriptor's kind once | an old client making its pool from a file in `XDG_RUNTIME_DIR`: told at once, not a black window |

  Duplicates, never moves: libwayland closes its descriptors after the
  send, and the descriptor may be shared (`dup`).
- **Handles to descriptors** (what `recvmsg` makes):

  | Handle | Descriptor | Used by |
  |---|---|---|
  | a VMO | a memfd description on it, read-only unless the handle has write; its size the VMO's | `wl_keyboard.keymap` (mapped `MAP_PRIVATE`: libc's copy until M13's F1, then copy-on-write; or `MAP_SHARED` read-only on the VMO) |
  | a channel end | a pipe end, readable or writable by its rights | `wl_data_source.send` (the owner writes the selection into it) |
  | any other kind | closed, and the connection ends (ECONNRESET) | (the compositor sends no other) |

  The keymap's handle comes without `RIGHT_DUPLICATE`
  (`user/services/compositor/keyboard.c`); the description holds the one
  handle, so `dup` shares it and a child is not given it (libwayland
  receives with `MSG_CMSG_CLOEXEC` anyway).
- **Waiting.** `poll`, `select` and `epoll` see the descriptor readable
  when bytes or handles are buffered or the channel is readable, writable
  when the channel has room, and hung up when the compositor closed it
  (the compositor's error is read first). Several threads may use it
  (libwayland's `wl_display_prepare_read` and `read_events` do); the
  description's lock covers the buffers, never held across a wait.
- **Across `posix_spawn`.** The descriptor is not handed to a child (a
  connection can't be shared between processes on Jam OS); libwayland
  opens it close-on-exec anyway.
- **What the compositor sees**: a JWL2 client. Nothing changes on its
  side: its codec checks every message, as for libjwl's clients.
- **Size**: about 900 lines of C in user/posix/sys/ (new files: the
  service-socket table and the Wayland kind, about 600; the descriptor
  translation, about 250), plus the routing in
  `user/posix/sys/socket.c` and `user/posix/sys/sockio.c`.

*Unsure*: whether any client relies on Linux's exact behaviour of a
`recvmsg` that receives descriptors with too little room; libwayland
doesn't (its room is the maximum).

### 2. What the compositor must add

The protocols the first apps use, what the compositor does today, and
what G2 adds. "Needs" lists who fails without it; "uses" who is better
with it.

| Protocol | Today | foot | SDL2 2.32 | swayimg | GTK3 (stretch) | G2 | Cost |
|---|---|---|---|---|---|---|---|
| `wl_compositor` | 4 | needs 4 | uses | uses | uses | 6 (`preferred_buffer_scale` 1, `preferred_buffer_transform` normal; `offset` at 5) | small |
| `wl_subcompositor` | none | **needs** (its title bar parts, the search box, URL labels) | uses (libdecor only) | no | uses (menus, offload) | **add**: subsurface trees, sync and desync commits, place above and below, position; damage, input and painting through the tree | the big one: about 1,000 lines |
| `wl_shm` | 1, ARGB8888 and XRGB8888 | needs 1 | needs | needs | needs | 2 (`release`) | small |
| `wl_output` | 3 | needs 2 | uses | uses | uses | 4 (`name`, `description`) | small |
| `wl_seat` | 5 | needs 5, uses `axis_value120` (8) | uses | uses | uses | 9: `axis_value120` (120 a notch), `axis_relative_direction`; the keymap already fits version 7's `MAP_PRIVATE` rule | small |
| `xdg_wm_base` | **1** | needs 1, uses to 7 | needs | needs | needs | 6: the tiled states (2: a tiled window draws no shadow or rounded corner of its own), `configure_bounds` (4), `wm_capabilities` (5: what the title bar's circles can do), `suspended` (6: a window on another screen) | small to medium |
| `xdg_popup` | dismissed at once | no | SDL3's popups | no | **needs** (menus, combo boxes, tooltips) | real popups with grabs: placed by the positioner on the output, `reposition` (3), dismissed by a click outside (GC3, before the GTK spike) | medium: about 700 lines |
| `zxdg_decoration_manager_v1` | none | uses (CSD without it: two title bars) | uses | uses | no (GTK draws its own) | **add**, by Q4's policy (section 3) | small: about 250 lines with libjwl's side |
| `wl_data_device_manager` | 3, text | needs 3 | uses | no | uses | unchanged; drag and drop not added (section 8) | none |
| `wp_cursor_shape_manager_v1` | 2 | uses (else Xcursor themes) | uses | uses | no (themes) | unchanged | none |
| `zwp_primary_selection_device_manager_v1` | none | uses (middle-click paste) | uses | no | uses | not in G2 (section 8) | small, later |
| `zwp_relative_pointer_manager_v1`, `zwp_pointer_constraints_v1` | none | no | **needs for relative mouse** (a shooter's mouse look) | no | no | add for games (GC4): a pointer locked to a surface while it has focus, relative motion, released by Super or focus loss | medium: about 400 lines |
| `wp_viewporter` | none | uses | uses (full screen at another size) | uses | no | not in G2: SDL scales in software without it | small, later |
| `zwp_text_input_manager_v3` | none | uses | uses | no | uses | not in G2 (section 8) | large (an input method) |
| `xdg_activation_v1`, `wp_presentation`, `zwp_idle_inhibit_manager_v1`, `wp_fractional_scale_manager_v1`, `zxdg_output_manager_v1`, `xdg_toplevel_icon_manager_v1` | none | uses some | uses some | uses some | uses some | not in G2: each client checks for them and does without | |

Each addition vendors its XML into `third_party/wayland-protocols` (from
the same wayland-protocols release, the README's table extended) and is
generated by `tools/genwl.py`, as G1's were; libjwl gets the stubs for
free.

**Subsurfaces** are the one large piece. The model: a surface with the
subsurface role is drawn relative to its parent, in its parent's tree's
order; in sync mode (the default) its commits are cached until the parent
commits; a toplevel's tree is drawn as one window (the frame, the tiling,
the animations and the screenshots of today see the tree's bounding box);
input goes to the topmost surface of the tree under the pointer by its
input region; caps per client (`COMP_SURFACES_MAX` already bounds the
surfaces; a tree depth of 8 and 32 children a surface are new caps, as
every cap is). The painting workers clip each subsurface to the window's
box, so a subsurface can't draw outside its toplevel's frame (the
protocol allows it; the compositor clips it, as the rounded corners do
today).

### 3. Decorations

The compositor draws a title bar on every window today; Jam OS's clients
draw none. Ported clients come in three kinds: those that ask for
server-side decorations through `zxdg_decoration_manager_v1` (foot,
SDL2, swayimg, Qt), those that always draw their own (GTK3, GTK4: they
ignore xdg-decoration; libdecor's users without it), and those that draw
nothing either way (games full screen).

**Recommendation (Q4): xdg-decoration with server-side as the
compositor's choice.** A toplevel with a decoration object gets the
compositor's title bar, its circles, its frame and its rounded corners,
as today (and `configure` says `server_side`); a toplevel with none is
taken to draw its own, and gets no title bar, its rounded corners and
shadow from the compositor only when floating (a GTK window looks like a
GTK window, as on GNOME). libjwl's window (`user/lib/jwl_window.c`)
binds the manager and asks for server-side, so Jam OS's own apps keep
their look; the change is one request at window creation. A client that
asks for client-side while bound is answered `server_side` (the protocol
lets the compositor decide), so a client that can do both shows one
title bar, ours.

### 4. Fonts

Ported programs find fonts through **fontconfig** and render them through
**FreeType**, which is what foot (through fcft), SDL2_ttf, swayimg,
cairo, pango and Qt all expect. (Q5.)

| Library | Licence | What it is for | Cost and notes |
|---|---|---|---|
| FreeType 2.13 or newer | FTL (BSD-style with a credit clause) **or** GPL-2.0, the user's choice: we take the **FTL**, and the boot image's and each package's licence list carries its credit line | outlines to bitmaps; hinting; checks every table of a font it reads (stb_truetype checks none: [Smooth text](../ARCHITECTURE.md#smooth-text)) | C, meson or its own make; about 1 MB static with every module, less with a module list (TrueType, CFF, sfnt, smooth, autofit) |
| fontconfig 2.15 or newer | MIT-style (HPND) | which file has which font; matching (`monospace:size=11` to JetBrains Mono) | needs expat (MIT) and FreeType; host tools `gperf` and Python at build time; writes a cache under `XDG_CACHE_HOME`; its atomic cache writes try `link` and fall back to `mkdir` locks (FAT has no links). *Unsure*: that its cache reader falls back to `read` when `mmap` of a file is ENODEV (before M12.5's F1); if not, `FONTCONFIG_CACHE` off and a scan per start (a second with a dozen fonts) |
| HarfBuzz 10 or newer | MIT | shaping: ligatures, scripts that need it | **C++**: needs a C++ compiler and at least libstdc++'s headers; it links no C++ runtime itself (*unsure*: that holds for every module we'd build). Q6 |
| utf8proc | MIT | foot's grapheme clusters (emoji, combining marks) | small, C |
| fcft and tllist | MIT | foot's font loading and glyph cache over fontconfig, FreeType and pixman (HarfBuzz optional) | C, meson |
| pixman | MIT | foot's and cairo's pixel compositing; SSE2 and SSSE3 paths chosen at run time with `cpuid` | C, meson; no JIT |

**libfun keeps stb_truetype** (Q5): it renders only its two built-in
faces, baked once and sealed, never a font from outside; FreeType would
add a megabyte to the compositor and every app for no visible change.
FreeType is for ports, which open fonts from disk.

**The font set** is a package, `fonts-jamos`: Inter and JetBrains Mono
whole (the same OFL files `third_party/` cuts for libfun, uncut), and
Noto Sans and Noto Sans Mono's Latin, Greek and Cyrillic (OFL) for
coverage; emoji and CJK later (Noto Color Emoji is 10 MB and needs
FreeType's colour bitmap support, which is in it). The package installs
them under the package prefix with a fonts configuration file naming that
directory and the aliases (`monospace` JetBrains Mono, `sans-serif`
Inter); fontconfig is built to read its configuration from the prefix.

### 5. Keyboards: xkbcommon

Every client turns `wl_keyboard`'s key codes into characters with
**xkbcommon** (MIT, C, meson; `bison` on the build host). The compositor
already sends a whole XKB text keymap generated from `abi/keymap`
(`tools/genkeymap.py`), so clients compile it with
`xkb_keymap_new_from_string` and need **no xkeyboard-config data on the
disk**: xkbcommon is built without its registry, X11 and tools.

- `tools/genkeymap.py check` compiles the generated keymap with xkbcommon
  when `xkbcli` is installed; G2 makes that a requirement on the PC's
  test host (an `apt` line), so a keymap that xkbcommon would refuse can't
  merge.
- **Compose** (dead keys, `Compose` sequences) needs X's Compose tables
  (libX11's locale files); the US layout has no dead keys, and clients
  that find no table go on without (foot does). A layout with dead keys
  is a later layout's job, with the table it needs.
- More layouts are more files in `abi/keymap` and a setting; the
  compositor picks one keymap for every client (not G2's work).

### 6. Rendering: software only

Every G2 app draws with the CPU into a `wl_shm` buffer: foot with pixman
and fcft, SDL through its software renderer into the framebuffer its
patch adds, swayimg with its own code. At 2560x1440 a full window is
14.7 MB a frame; the compositor already composites only damage.

What that leaves out is GL, which many programs assume (most SDL games
that are not 2D, imv, mpv's default output, GTK4's default renderers,
Qt Quick). The options for later (Q3):

| | Cost | Blockers |
|---|---|---|
| **No GL in G2** (SDL patched for `wl_shm`; GL programs wait) | the SDL patch only | none |
| **Mesa's softpipe** (an interpreted Gallium driver) behind EGL's Wayland platform with `wl_shm` (Mesa's "swrast" path) | Mesa is a large port (meson, Python and Mako on the build host, about 1 million lines in all; a softpipe-only build is a fraction) | Mesa's EGL loads its Gallium driver from a shared library (since 24.x one "megadriver"); a static Mesa is not a supported build, so in practice **M14.5** (*unsure*: a static build may be patchable). softpipe is slow: fine for a spinning cube, not for a game |
| **Mesa's llvmpipe** (LLVM-compiled shaders, many threads) | LLVM as a port: tens of MB of static libraries in every GL program | **a JIT**: llvmpipe writes machine code and runs it. Jam OS's layer refuses execute on anonymous memory by design (`user/posix/sys/mem.c`); a JIT would need a new rule (a program's list asking for it, approved by the owner) and a kernel path for W^X-respecting code pages (write, then seal executable). A security decision of its own |
| **TinyGL or PortableGL** (small software GL 1.x / 3.x implementations, permissive) | small | each needs its own glue into SDL or the app; few real programs run on them |

**Recommendation (Q3): no GL in G2**, the SDL patch instead; Mesa with
softpipe as the first step of G4 (or of M14.5's first port), llvmpipe
only with the JIT rule decided by the owner separately. (Mesa's old
off-screen library, OSMesa, is gone from current Mesa; *unsure* in which
release it was removed, but it is not a road.)

### 7. The toolkit ladder

Each rung adds one toolkit's needs to the one before. Licences are the
libraries' own; "needs" says what has to exist first.

| Rung | What | Pulls in | Licences | Needs |
|---|---|---|---|---|
| 0 | **libwayland-client and -cursor** | libffi; wayland-scanner (a build-host tool, built from the same tarball); wayland-protocols' XML | MIT; MIT | the shim (section 1); M13.7's builder with a host build (section 9) |
| 1 | **xkbcommon, pixman, the font stack** | expat, zlib, libpng, FreeType, fontconfig, utf8proc, fcft, tllist | MIT, Zlib, libpng's, FTL, MIT, MIT, MIT, MIT | rung 0's builder pieces |
| 2 | **foot** (a terminal: no toolkit at all, the best first proof) | rung 0 and 1 | MIT | `wl_subcompositor`, `timerfd`, the decoration manager; ptys (M13's X1, merged); its spawn override; M13.5's mksh for a shell |
| 3 | **SDL2** (2.32, static) and its test programs, then SDL2_mixer for a game's music | SDL2 with only its Wayland video driver, its `wl_shm` framebuffer patch and a Jam OS audio driver (over the mixer's `audio` protocol, `abi/idl/audio.idl`); libdecor off (server-side decorations instead) | Zlib (SDL2, SDL2_mixer); our two additions under BSD-2 in the port's directory | rung 0; the decoration manager; relative pointer for mouse look (GC4) |
| 4 | **swayimg** (an image viewer) | libjpeg-turbo, libpng, Lua's library (M13.7's Lua port staged as a library), FreeType and fontconfig | MIT; IJG, BSD-3 and Zlib (libjpeg-turbo); libpng's; MIT | rung 1; its `inotify` use patched out or answered ENOSYS (a directory isn't watched) |
| 5 | **C++ runtime and HarfBuzz** | libstdc++ (GCC 15.2's, built against our sysroot, with its unwinder) or LLVM's libc++ | GPL-3.0 with the GCC Runtime Library Exception (any program may link it); Apache-2.0 with LLVM's exception | Q6; then HarfBuzz into fcft (foot's ligatures) |
| 6 | **GTK3** (3.24, the last GTK3: a spike in G2, Q7) | GLib (with libffi, PCRE2, zlib), cairo, pango, HarfBuzz, fribidi, gdk-pixbuf, ATK, libepoxy (its GL lookups fail cleanly without a GL library), shared-mime-info's data | LGPL-2.1+ (GLib, GTK, pango, fribidi, ATK), LGPL-2.1 or MPL-1.1 (cairo), MIT (libepoxy, HarfBuzz), BSD (PCRE2) | rung 5; real `xdg_popup` (GC3); `fork` replaced in GLib's spawn (GLib uses `posix_spawn` when it can); D-Bus left out (no accessibility bridge, no portals, GSettings in memory); LGPL static linking (Q8) |
| 7 | **GTK4** | rung 6's minus ATK, plus graphene and its renderers: Vulkan and GL by default, cairo as the fallback | as rung 6 | *unsure*: that GTK4's cairo renderer is enough without any GL library to load; its accessibility needs D-Bus |
| 8 | **Qt 6** (qtbase and qtwayland) | its own copies of HarfBuzz, PCRE2 and more; FreeType, fontconfig, xkbcommon | LGPL-3.0 or GPL; third-party parts permissive | rung 5 with exceptions and threads; Qt's static build (`Q_IMPORT_PLUGIN` for the Wayland platform plugin); C++17; tens of MB per program. Its own project |

**Why foot first**: it is the smallest real Wayland program that needs
everything a toolkit needs underneath (the registry, `wl_shm`, the seat,
xkbcommon, fonts, subsurfaces, the clipboard), and nothing a toolkit adds
on top; a terminal is easy to test by script (bytes in, pixels out); and
its failures name the protocol or call that's missing. **Why SDL2 next**:
it is how most games and many small graphical programs reach a window,
it is C, and its only gap (the framebuffer) is ours to patch. **Why
swayimg**: it is a real desktop app in C that renders in software into
`wl_shm`, reads files the owner has (photos), and exercises the image
libraries GTK will need again. **Why GTK3 before GTK4**: it renders with
cairo into `wl_shm` by design, it is what Redox and Haiku ported first,
and it is stable (no new releases to chase).

### 8. Clipboard, drag and drop, input methods

- **Clipboard**: works as built in G1 (text types only, focus and serial
  rules, the transfer through a channel the shim shows as a pipe). foot's
  copy reaches a Jam OS terminal and back. Images and other types are
  refused by the compositor's type list; an image viewer's "copy image"
  does nothing visible. Adding types is a list change in
  `user/services/compositor/data.c`, not G2's.
- **Primary selection** (select to copy, middle-click to paste): not
  offered; foot works without it. A later small track
  (`zwp_primary_selection_device_manager_v1`, the same rules as the
  clipboard's).
- **Drag and drop**: `start_drag` still cancels its source. DnD needs the
  pointer grab, the drag icon surface, enter, motion and drop to whatever
  window is under the pointer, and a policy for which windows may see an
  offer (today's rule: only the focused client sees data). Later, with the
  file manager that would want it.
- **Input methods** (`zwp_text_input_v3` with an input-method protocol):
  not in G2. Jam OS has no IME, and the US layout needs none. When a
  language needs one, it is a milestone of its own (an IME service like
  fcitx5 ported, or our own), with the compositor in the middle.
- **Fractional and integer scaling**: the output is scale 1 and the
  compositor refuses other buffer scales; `preferred_buffer_scale` says 1.
  A HiDPI mode is a compositor milestone (its own drawing first).

### 9. Ports and linking

- **Every G2 library and app is a package** (M13.7's `place package`): none
  goes into the boot image, which is full. The ladder's recipes live in
  ports/ beside M13.7's.
- **Static linking now.** Every library above builds static (meson's
  `default_library=static`, configure's `--disable-shared`). The places
  where upstream reaches for `dlopen` are each switched off at build
  time: SDL's dynamic Wayland and audio loading (`SDL_WAYLAND_SHARED=OFF`
  and friends), libdecor (off), libepoxy's GL lookups (fail cleanly),
  GTK's input-method modules (built in), gdk-pixbuf's loaders (built in),
  GIO's modules (none), Qt's plugins (static, `Q_IMPORT_PLUGIN`). So
  **G2 needs nothing from M14.5.** What would need it: Mesa (section 6),
  libdecor's plugins, GStreamer's plugins, and anything that loads code
  chosen at run time.
- **What M13.7's builder needs from G2** (named here, added by GP1 to
  tools/ports.py with the porter's guide): **host builds**: a recipe that
  builds a tool for the build machine from the same tarball (wayland-scanner;
  later GLib's `glib-compile-resources` and the like), into
  build/ports/host/, put on the PATH of its dependents' builds (*unsure*:
  whether Homebrew's own `wayland` formula installs on macOS at all; if
  so, a `host` line is enough); **data ports** (wayland-protocols'
  XML, the fonts) with no program; and meson's cross file with the
  options above. The pkg-config `.pc` files of each library are staged
  with its headers (M13.7 already points pkg-config into the port's
  sysroot).
- **LGPL** (GTK, GLib, pango, fribidi, ATK; cairo under its LGPL choice).
  CONTRIBUTING allows LGPL code in separate programs, which every app
  package is. Statically linked, the LGPL asks that the user can change
  the library and relink the program; M13.7 already serves **every
  package's complete source beside it** (the distfiles, the patches, the
  recipe, the Jam OS tree's commit), so a user can rebuild the app
  against a changed GTK from what is offered. Q8 asks the owner to
  confirm that reading (this plan's, not legal advice). M13.7's lint
  refuses an LGPL `needs` unless the recipe says how it is met: the
  recipe's line is "source served beside the package".
- **A C++ runtime** is a prerequisite of HarfBuzz (headers at least),
  Qt, and many apps later (any game in C++). Q6.
- **Sizes.** A static POSIX program starts at about 250 KB (M13.7's
  note). Estimates, *unsure* until built: libwayland-client and libffi
  about 150 KB; xkbcommon 400 KB; FreeType 800 KB; fontconfig and expat
  400 KB; pixman 600 KB; foot in all about 3 MB; SDL2 with its Wayland
  driver about 1.5 MB; swayimg with its image libraries about 2.5 MB.
  Each program carries its own copy (static linking's price, M13.7
  section 5): a FreeType fix rebuilds foot, swayimg and every SDL2 game.

### 10. Environment, files and launching

- **Variables the shell sets for every program** (`user/services/shell/sh_vars.c`):
  `WAYLAND_DISPLAY=/svc/wayland` (absolute: no runtime directory needed
  to connect), `XDG_CONFIG_HOME=/data/.config`, `XDG_CACHE_HOME=/data/.cache`,
  `XDG_DATA_DIRS` the package prefix's share directory; and
  `XDG_RUNTIME_DIR` (Q10) once there is somewhere private and in memory
  to point it. Programs that warn without `XDG_RUNTIME_DIR` (GTK, foot's
  server mode) say so; libwayland itself doesn't need it.
- **Configuration files** (foot's foot.ini, fontconfig's local file) live
  in `/data/.config`, the owner's to edit; packages never ship a file the
  owner edits (M13.7's rule).
- **Launching from the desktop** (Q9): today the search box starts only
  `<deskapps.h>`'s Jamjar. A recipe line `desktop <program> <name>
  <summary>` would let an approved package's GUI program join the search
  box: `pkg`'s approval records it, and init's `launch` accepts it with
  the program's own list (as the shell would give it) and `/svc/wayland`.
  The compositor still names an app by name, never a path.

### 11. The first apps

| Rung | App | Licence | Why this one | Proves |
|---|---|---|---|---|
| 1 | **wlprobe** (ours, a test program in the ports tree, BSD-2) | BSD-2-Clause | the shim's own test on the real libwayland, before any app: every path of section 1 by name | the shim; the compositor's view of a libwayland client |
| 2 | **foot** 1.2x | MIT | the smallest app that needs a toolkit's whole floor; scriptable | subsurfaces, decorations, timerfd, fonts, xkbcommon, the clipboard both ways, ptys |
| 3 | **SDL2's test programs** (testsprite2, testdraw2, testkeys, testmouse, testaudioinfo) | Zlib | SDL's own checks, small, each one feature | the framebuffer patch, the audio driver, key and mouse mapping |
| 4 | **Chocolate Doom** 3.x with **Freedoom** (phase 1 and 2) | GPL-2.0 (the engine, a separate program, its source beside it); BSD-3-Clause (Freedoom) | a real game people know: 35 frames a second, sound effects and music (its OPL emulation), keyboard and mouse, full screen | a full SDL2 program end to end; a GPL package |
| 5 | **swayimg** 3.x or 4.x | MIT | a real desktop app with images, fonts and keys | the image libraries, a Lua dependency, file reading |
| stretch | **gtk3-demo** and one real GTK3 app (a small editor such as mousepad, GPL-2, or a calculator) | LGPL-2.1+ (GTK), the app's | measures the GTK project | GLib, cairo, pango, popups |

Fallbacks, in case a first choice stalls: **havoc** (a minimal Wayland
terminal in C; *unsure* of its licence and of its libtsm dependency's) for
foot; SDL's own `testgles` is not one (it needs GL); **imv** is not one
for swayimg (it renders with OpenGL).

## The interfaces that change (for M14's review)

| Where | What |
|---|---|
| libc (`user/posix/sys/`) | `connect` on `AF_UNIX` `SOCK_STREAM` to a path in the service-socket table (today `/svc/wayland`); the Wayland socket descriptor kind: `sendmsg`/`recvmsg` with `SCM_RIGHTS` on it (and only on it), `MSG_CTRUNC`, `MSG_CMSG_CLOEXEC`; the descriptor-to-handle and handle-to-descriptor tables of section 1; `getsockopt(SO_PEERCRED)` answered with the one user and an unknown pid (*unsure* whether any client asks); `timerfd_create`, `timerfd_settime`, `timerfd_gettime` (a kernel timer as a descriptor: reads give the expirations count); `fallocate` (`FALLOC_FL_PUNCH_HOLE` is EOPNOTSUPP, plain allocation on a memfd grows it) |
| the hand-over (`user/include/posixfds.h`) | timerfd as a kind a child may be given (or not: close-on-exec is how every user opens it; the plan says not, and a child gets nothing) |
| the compositor's globals and versions | `wl_compositor` 6, `wl_subcompositor` 1, `wl_shm` 2, `wl_output` 4, `wl_seat` 9, `xdg_wm_base` 6, real `xdg_popup` (3), `zxdg_decoration_manager_v1` 1, `zwp_relative_pointer_manager_v1` 1, `zwp_pointer_constraints_v1` 1: the set a client may count on is part of what M14 freezes (G1's protocol is M12's) |
| new caps | subsurface tree depth (8) and children per surface (32); popups per client; pointer constraints per client |
| libjwl | windows bind the decoration manager and ask server-side (`user/lib/jwl_window.c`) |
| the shell's environment | `WAYLAND_DISPLAY`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_DIRS`, and `XDG_RUNTIME_DIR` (Q10) |
| M13.7's recipes | host builds; `desktop <program> <name> <summary>` (Q9) |
| `abi/idl/initctl.idl` | `launch` of an approved package program with a `desktop` line (Q9): the same method, a wider check in init |
| `third_party/wayland-protocols` | xdg-decoration, relative-pointer and pointer-constraints XML (the README's table, `third_party/VERSIONS.md`) |

## Questions for the owner

Each stands alone and ends with a recommendation; the plan assumes the
recommendation until you say otherwise.

### Q1. Where the shim lives

- **(A)** In libc: `connect(AF_UNIX, "/svc/wayland")` gives a Wayland
  socket descriptor whose `sendmsg`/`recvmsg` translate the stream and
  `SCM_RIGHTS` into JWL2 batches and handles; libwayland unmodified.
- **(B)** In libwayland: its send and receive overridden to speak JWL2 on
  a channel; libc unchanged but for a channel descriptor to poll.
- **(C)** A connection opened by the starter and passed in
  `WAYLAND_SOCKET`; one connection per process.

*Recommendation: (A).* It keeps libwayland exactly upstream's (updates
are a version bump), works for any Wayland library that calls `sendmsg`
itself, and allows several connections; the translation lives once, in
BSD code next to the pipes and memfds it maps to.

### Q2. SDL2 or SDL3

- **(A)** SDL2 2.32 (the last SDL2 branch, fixes only), static.
- **(B)** SDL3, with sdl2-compat for SDL2 programs (as Fedora and Arch
  now ship it).
- **(C)** Both as separate recipes: SDL2 first, SDL3 when an SDL3 program
  is the goal.

*Recommendation: (C).* The programs worth proving today (Chocolate Doom
and most ported games) are SDL2, and SDL2 is stable; sdl2-compat finds
SDL3 with `dlopen` (*unsure* whether it can be linked statically), so
(B) waits for M14.5 in practice. The framebuffer patch is the same idea
in both and is offered upstream for SDL3 first.

### Q3. Drawing for SDL, and GL

- **(A)** No GL in G2: SDL's Wayland driver gains a `wl_shm` framebuffer
  (a patch, offered upstream), and GL programs wait.
- **(B)** Mesa with softpipe behind EGL, in G2.
- **(C)** Mesa with llvmpipe, which needs a JIT rule.

*Recommendation: (A).* About 250 lines against a large port that
probably needs dynamic linking (B) or a new rule on executable memory
(C). softpipe becomes G4's or M14.5's first step; the JIT question is
yours separately, when a program worth it asks.

### Q4. Window decorations

- **(A)** xdg-decoration with the compositor's title bar for every
  client that binds it (libjwl's windows ask for it), and no title bar
  for a client that doesn't (it draws its own, GTK's way).
- **(B)** The compositor's title bar on every window, as today: GTK
  windows get two.
- **(C)** Client-side for every ported program, the compositor's only
  for Jam OS's own.

*Recommendation: (A).* foot, SDL and swayimg take ours and look like Jam
OS windows; GTK keeps its own header bar without a second one above it;
and the decision is per window, made by what the client says.

### Q5. Fonts

- **(A)** FreeType (under the FTL) and fontconfig as ports, a fonts
  package; libfun keeps stb_truetype for its built-in faces.
- **(B)** (A), and libfun moves to FreeType too.
- **(C)** A small fontconfig stand-in of our own (a fixed list of fonts)
  instead of fontconfig.

*Recommendation: (A).* Every toolkit expects fontconfig's API whole (fcft
alone uses dozens of its calls); FreeType is what reads a font from disk
safely. libfun never opens an outside font, so moving it would add size
and risk for nothing visible.

### Q6. Text shaping and a C++ runtime

- **(A)** In G2: a C++ runtime (libstdc++ from GCC 15.2's sources, built
  against our sysroot) and HarfBuzz, so foot shapes ligatures.
- **(B)** foot without HarfBuzz in G2 (graphemes through utf8proc, no
  ligatures); the C++ runtime and HarfBuzz as rung 5, the first step of
  the GTK project (or earlier if a C++ port comes first).
- **(C)** LLVM's libc++ with clang instead of libstdc++, whenever it comes.

*Recommendation: (B).* Nothing in G2's done-when needs C++, and the
runtime (its unwinder against static musl, `.eh_frame_hdr` kept by
`user/posix/posix.ld`) is a toolchain task worth its own careful track.
libstdc++ over libc++ when it comes: the same GCC we already use, and
its runtime exception lets any program link it.

### Q7. GTK in G2

- **(A)** None: GTK and Qt are the later project the roadmap says.
- **(B)** A GTK3 spike at G2's end (gtk3-demo and one small app), not
  gating the sign-off, to measure that project.
- **(C)** GTK3 fully in G2.

*Recommendation: (B).* A day of agents turns "GTK is a later project"
into a list of its real gaps (popups, D-Bus, `fork` in GLib, sizes), so
that project can be planned on facts; it starts only after G2's
done-when is met.

### Q8. LGPL libraries linked statically

- **(A)** Allowed in app packages, the obligation met by M13.7's complete
  source served beside every package (the app, the libraries, the
  recipe and the tree's commit: enough to change a library and relink).
- **(B)** No LGPL library until M14.5 (dynamic linking).
- **(C)** (A), and each such package also ships its object files in a
  `-relink` subpackage.

*Recommendation: (A).* Every package is built from public source by a
public recipe, so the user can always rebuild with a changed library;
(B) would block GTK on a milestone it doesn't otherwise need. This is
the plan's reading of LGPL-2.1 section 6 and LGPL-3.0 section 4, not
legal advice; (C) is the cautious fallback.

### Q9. Ported apps in the search box

- **(A)** Not in G2: ported apps start from a terminal.
- **(B)** A recipe's `desktop` line; once `pkg` installed and you
  approved it, the app is in the search box and init starts it with its
  own list.

*Recommendation: (B)*, as G2's last small track. A GUI app that only
starts from a terminal is half a port. The compositor still can't start
anything but what init's list holds; the list just grows by what you
approved.

### Q10. A runtime directory and `/tmp`

- **(A)** A RAM `/tmp` (the tests' ramfs as a service, M13's noted
  follow-up), with `XDG_RUNTIME_DIR` a private directory in it.
- **(B)** `XDG_RUNTIME_DIR` on `/data` (FAT, kept across boots).
- **(C)** No runtime directory in G2; `WAYLAND_DISPLAY` absolute is
  enough for libwayland, and programs that want one warn.

*Recommendation: (A)*, with (C) until it lands. Sockets and lock files
don't belong on the stick, and a RAM `/tmp` is wanted by ports anyway
(M13's list); G2 doesn't wait for it.

### Q11. The first apps

- **(A)** foot, SDL2's tests then Chocolate Doom with Freedoom, swayimg
  (section 11).
- **(B)** A simpler terminal (havoc) before foot, to separate the shim's
  problems from subsurfaces and fonts.
- **(C)** SDL2 first, foot second.

*Recommendation: (A)*, with wlprobe before all of them taking (B)'s
role: wlprobe separates the shim's problems from the apps', and foot
then tests the floor every toolkit stands on.

### Q12. Big, sparse memory pools

- **(A)** Live with kept memfds committing as they grow: recipes cap the
  pools their apps make (foot's maximum pool 0), and a client that makes
  a huge pool pays for it or is refused by the compositor's 256 MiB.
- **(B)** A kernel change: a kept VMO that commits its pages on first
  touch, charged to its maker's job, still never taken away once there
  (the compositor's read of an untouched page would commit it on the
  maker's account).
- **(C)** A memfd isn't kept until it is first sent to the compositor.

*Recommendation: (A)* in G2, with (B) written into M14's review list. (B)
is the right shape but a kernel and accounting design (who pays when the
compositor's read commits a page, and what happens when that charge
fails), not a porting task; (C) changes nothing for a pool sized before
it is sent, which is foot's case.

## Tracks

Each track is about one to three agent-hours, starts from main, owns the
files listed (an edit outside them stays minimal and is named in its
report; new files are written without a path check), commits refactors
and behaviour changes separately, adds a test for each behaviour (one
that fails without it), and hands back at each usable stage. QEMU tests
run on the PC only, through `tools/remote-test.sh` (AGENTS.md's rule):
the quick tier (the build, `make check`, the init run) plus the track's
own scripts, in parallel shards. **Agents never push, never touch main,
`~/jamos` or a USB disk, and never start agents of their own.**

Models follow the owner's split: **Opus** for the compositor (every
client talks to it), the shim (it decides what crosses into a handle),
and the review; **Sonnet 5.5** for the ports, the recipes, the tests and
the docs.

| Track | What | Files it owns | Waits for | Tests | Model |
|---|---|---|---|---|---|
| **GS1. The Wayland socket** | section 1: the service-socket table, the Wayland descriptor kind (out buffer, batches, the handle queue, partial writes, `MSG_CTRUNC`), the two translation tables, poll/epoll readiness, the shell's `WAYLAND_DISPLAY`; a utest client of the kind against a fake compositor end (a channel the test reads as the compositor would) | new files in user/posix/sys/ (the table and the kind), the routing lines in `user/posix/sys/socket.c`, `user/posix/sys/sockio.c`, `user/posix/sys/kinds.h`; the variable in `user/services/shell/sh_vars.c`; new utest files | Q1 | utest: whole messages into batches, a message split over three `sendmsg`s, one byte with 28 descriptors then the rest, 29 descriptors refused, every descriptor kind of the table (and EINVAL for the rest), a batch with handles read with too little room (`MSG_CTRUNC`, handles closed), a bad magic (ECONNRESET), `EAGAIN` on a full channel and `POLLOUT` after; posixtest: `connect` to a path not in the table (ENOENT), without `svc wayland` (ENOENT); a fuzz loop of 10,000 mutated byte streams and batches (no crash, no handle leaked: `handle_info` counts) | **Opus** |
| **GS2. Small POSIX additions** | `timerfd_create`/`settime`/`gettime` (a kernel timer as a descriptor: `CLOCK_MONOTONIC` and `CLOCK_REALTIME`, absolute and relative, the expirations count, `TFD_NONBLOCK`, poll and epoll); `fallocate` on a memfd (grow) and EOPNOTSUPP for holes; `inotify_init1` ENOSYS (a port that needs it learns at once); `SO_PEERCRED` | new user/posix/sys/ files for timerfd, the lines in `user/posix/sys/calls.c` and `user/posix/sys/kinds.h` | M13's R1 review merged if it is moving `user/posix/sys/calls.c` (the call table's size question) | posixtest: a periodic timer counted over a second, an absolute timer after a clock change, epoll on three timers, `read` of 7 bytes EINVAL; fallocate's cases | Sonnet 5.5 |
| **GC1. Versions and decorations** | the version bumps of section 2's table (`wl_compositor` 6, `wl_shm` 2, `wl_output` 4, `wl_seat` 9 with `axis_value120`, `xdg_wm_base` 6 with the tiled states, `configure_bounds`, `wm_capabilities`, `suspended`); xdg-decoration by Q4's policy, the frame and title bar following it; libjwl's windows asking for server-side; the XML vendored and generated | `user/services/compositor/display.c`, `user/services/compositor/xdg.c`, `user/services/compositor/xdgtop.c`, `user/services/compositor/seat.c`, `user/services/compositor/pointer.c`, `user/services/compositor/deco.c`, `user/services/compositor/comp.h`, a new decoration file in the compositor, `user/lib/jwl_window.c`, `third_party/wayland-protocols` (one file and the README), the generated stubs | Q4 | utest's compositor tests: a window asking server-side and one asking nothing (title bar and none), each new event at each version, a client bound at version 1 seeing nothing newer; `tools/comp-test.sh`, `tools/desk-test.sh`, `tools/apps-test.sh` unchanged (Jam OS's windows look the same) | **Opus** |
| **GC2. Subsurfaces** | `wl_subcompositor` and `wl_subsurface` (section 2): trees, sync and desync, order, position, the tree drawn as one window, input through it, clipping to the window, the new caps; the hostile cases (a cycle, a parent destroyed first, a tree deeper than the cap) | `user/services/compositor/surface.c`, a new subsurface file, the painting and input lines it needs (`user/services/compositor/paint.c`, `user/services/compositor/scene.c`, `user/services/compositor/pointer.c`), `user/services/compositor/comp.h` | GC1 merged (the same files) | utest: a parent with three children, commits in sync and desync, place_above/below, a child outside the parent clipped, input to the topmost; the hostile cases refused with the protocol's errors; a screenshot test of a window with subsurfaces (`tools/comp-test.sh`'s way) | **Opus** |
| **GC3. Popups** | real `xdg_popup`: placement by the positioner on the output (flip, slide, resize), `grab` with a live serial, dismissal by a click outside or Escape, nested popups, `reposition`; the window manager's rules for popups (never tiled, above their parent) | `user/services/compositor/xdg.c`, a new popup file, the window manager's lines it needs (`user/services/compositor/wm.c`) | GC2 merged | utest: each positioner case placed exactly, a grab without a serial refused, a click outside dismissing the chain; screenshot of a popup at a screen edge | **Opus** |
| **GC4. Relative pointer and pointer constraints** | `zwp_relative_pointer_manager_v1` (relative motion from the mice's counts, unaccelerated too) and `zwp_pointer_constraints_v1` (lock and confine, only while the surface has the pointer focus, released by focus loss or Super) | `user/services/compositor/pointer.c`, a new constraints file, `third_party/wayland-protocols` (two files) | GC2 merged | utest: a lock follows focus, motion while locked is relative only, Super releases it; `tools/mouse-test.sh` unchanged | **Opus** |
| **GP1. The base ports** | recipes for libffi, expat, wayland (client, cursor, and wayland-scanner as a host build), wayland-protocols (data), xkbcommon; the host-build and data-port pieces in tools/ports.py with their self-tests; meson's cross file's options (static, no `dlopen`) | ports/ recipes (new), tools/ports.py's host-build part, ports/jamos.meson's lines | M13.7's PO1 merged (PO3 for packages) | each recipe's `test`: libffi's own `ffi_call` tests as a program; xkbcommon compiling the compositor's keymap from `tools/genkeymap.py`'s text; libwayland's `wl_display_connect` from a tiny program; the builder's self-test | Sonnet 5.5 |
| **GP2. The font stack** | recipes for libpng (zlib from M13.7's PO9), FreeType (FTL; a module list), fontconfig (configuration in the prefix, cache under `XDG_CACHE_HOME`), pixman, utf8proc, tllist, fcft; the `fonts-jamos` package (Inter, JetBrains Mono, Noto's Latin, Greek and Cyrillic) and its configuration | ports/ recipes (new) | GP1's builder pieces; M13.7's PO9 (zlib) | `fc-list` and `fc-match monospace` on Jam OS; FreeType rendering a string to a PGM compared with the Mac's output (the same library, the same bytes); pixman's own test programs | Sonnet 5.5 |
| **GT. wlprobe and the scripts** | wlprobe (section 11), a test program on the real libwayland, in the ports tree; tools/wl-port-test.sh (new): a plain boot with packages preinstalled (M13.7's PO8 image), wlprobe's cases by name, then each app's script as it lands (a screenshot checked against expected pixels, as `tools/clip-test.sh` does) | ports/wlprobe (new), tools/wl-port-test.sh (new) | GS1; GP1; M13.7's PO8 | itself; it is the end-to-end test of GS1 | Sonnet 5.5 |
| **GA1. foot** | foot's recipe: its spawn override (`posix_spawn`, the slave as 0, 1 and 2, `POSIX_SPAWN_SETSID`), the pool maximum 0, no utmp, no systemd, fcft without HarfBuzz (Q6); its terminfo (foot's own `foot` entry, in ncurses' terminfo package from M13.7's PO11, or `TERM=xterm-256color`) | ports/foot (new) | GS1, GS2, GC1, GC2, GP1, GP2, GT; M13.5's mksh for the interactive shell (`foot -e` programs until then) | GT's foot script: `foot -e` a program that prints a known screen (colours, box drawing, wide characters), the screenshot checked; a selection copied and pasted into a Jam OS terminal (the clipboard script's way); a resize and its SIGWINCH; foot killed and the desktop unchanged | Sonnet 5.5 |
| **GA2. SDL2** | SDL2 2.32's recipe with only the Wayland video driver; the `wl_shm` framebuffer patch (`CreateWindowFramebuffer`, `UpdateWindowFramebuffer` with damage, double buffering on `wl_buffer.release`); the Jam OS audio driver (an `audio` stream to the mixer); SDL's test programs as a `-test` subpackage | ports/sdl2 (new; the two additions as files of ours, BSD-2) | GS1, GC1, GP1, GT | GT's SDL script: testdraw2's screenshot, testkeys's lines for typed keys, testaudioinfo naming the output and a tone's samples reaching the mixer (`tools/mixer-test.sh`'s way) | Sonnet 5.5 |
| **GA3. Chocolate Doom and Freedoom** | recipes for SDL2_mixer (and SDL2_net if Chocolate Doom insists), Chocolate Doom (GPL-2.0: its source served beside it), the Freedoom data package | ports/ recipes (new) | GA2; GC4 for mouse look (keyboard play before it) | GT's Doom script: the demo played by `-timedemo` to its end with its frame count printed (a number for BENCH.md), a screenshot of the title screen; by hand on the PC: a level played with sound | Sonnet 5.5 |
| **GA4. swayimg** | recipes for libjpeg-turbo and swayimg (Lua's library from M13.7's Lua port; `inotify` off) | ports/ recipes (new) | GP2, GC1, GT | GT's swayimg script: a known PNG and JPEG shown, the screenshot's pixels checked; keys to the next file | Sonnet 5.5 |
| **GD. The search box** (Q9) | the recipe's `desktop` line in tools/ports.py and the package's manifest; `pkg`'s approval recording it; init's `launch` accepting an approved package program; the search box's rows | tools/ports.py's line, init's `launch` check (`user/services/init/apps.c`), the compositor's search rows | M13.7's PO5 and PO6 merged; GA1 | utest for init's check (an unapproved program refused, a program without the line refused); `tools/desk-test.sh` with a preinstalled package | Sonnet 5.5 |
| **GR. Independent review and fix** | GS1, GS2, GC1 to GC4: findings first, then High and Medium fixed with a test each; hostile libwayland clients against the live compositor (X1's style): a subsurface storm, a popup chain at the cap, a decoration object on a destroyed toplevel, 28 handles of the wrong kinds, a client that never reads | the files its fixes need, named in its report | GS1, GC1, GC2 merged (GC3 and GC4 if merged by then) | its own; utest; GT's scripts | **Opus** |
| **GJ. The join** | ARCHITECTURE (Graphics: ported clients, the protocols offered, decorations; Userland: the Wayland socket), the porter's guide (GUI ports: the shim, the environment, the font and keyboard pieces, what is offered), USING (foot, Doom, swayimg), TESTING (the new scripts), `third_party/wayland-protocols`'s README, ROADMAP and HISTORY | the docs | GR merged | `make check` | Sonnet 5.5 |
| **GX1. Stretch: C++ runtime and HarfBuzz** (Q6) | libstdc++ and its unwinder built from GCC 15.2's sources against the sysroot; `tools/jamos-cc` as a C++ driver too; `.eh_frame_hdr` kept by the linker script; a C++ test program (exceptions across a throw, static constructors, threads); then HarfBuzz into fcft | the toolchain's new build files, `tools/jamos-cc`, `user/posix/posix.ld` (one line), ports/harfbuzz (new) | GA1 | posixtest's C++ program; foot shaping a ligature (its screenshot) | **Opus** (a toolchain decision) |
| **GX2. Stretch: GTK3 spike** (Q7) | recipes for GLib, cairo, pango, fribidi, gdk-pixbuf, ATK, libepoxy, GTK3; gtk3-demo; a written list of what broke and what each fix would cost | ports/ recipes (new), a findings section added to this plan | GX1, GC3, G2's done-when met | gtk3-demo opens and its widgets draw (screenshots); the findings list | Sonnet 5.5 (Opus if GLib's spawn or D-Bus need design) |

## Order

1. **Now**: the owner's answers. Q1 gates GS1; Q4 gates GC1; Q2, Q3 and
   Q6 shape GA2 and the stretch; Q9 gates GD.
2. **At once, in parallel**: GS1 (libc), GS2 (libc, other files), GC1
   (the compositor). None touches another milestone's files; GS2 checks
   with M13's R1 first if it is moving `user/posix/sys/calls.c`.
3. **After M13.7's PO1 merges**: GP1, then GP2 beside it once GP1's
   builder pieces are in.
4. **After GC1**: GC2, then GC3 and GC4 (one compositor track at a time:
   the same files).
5. **After GS1 and GP1**: GT (wlprobe first: the shim proven on the real
   libwayland before any app).
6. **The apps**: GA2 after GT and GC1 (SDL needs no subsurfaces); GA1
   after GC2, GS2 and GP2; GA4 after GP2; GA3 after GA2 (and GC4 for the
   mouse).
7. **GR** once GS1, GC1 and GC2 are merged (GC3 and GC4 join it if
   merged; else their own small review), then **GD** (after M13.7's `pkg`),
   then **GJ**, the main session's regression on the PC, and the PC
   session.
8. **The stretch** (GX1, then GX2) only after the done-when is met.

**Beside the other milestones**:
- **M13**: GS1 and GS2 add files to user/posix/sys/ and route a few
  calls; they take R1's findings on the call table into account and
  rebase on R1's changes. X2 (tinyssh) and S2 don't touch these files.
- **M13.5**: foot's shell. Until mksh merges, foot is tested with
  `foot -e` POSIX programs.
- **M13.6**: nothing needed. Fonts and packages live on FAT32 under
  M13.7's rules; fontconfig's links fall back.
- **M13.7**: every G2 port is a recipe; GP1 adds host builds and data
  ports to the builder (named in its report for PO1's owner); GA1 wants
  PO11's terminfo or uses xterm's entry; GD needs PO5 and PO6.
- **M12.5**: F1's mapped files make fontconfig's cache cheaper and the
  keymap's `MAP_PRIVATE` a copy-on-write instead of a copy; nothing waits
  for it.
- **The PC's QEMU slots**: GT's scripts run one QEMU per app; they
  stagger with the others.

## Tests and the sign-off

| What | Where |
|---|---|
| The shim | utest (GS1's cases), posixtest's socket section, the fuzz loop; wlprobe on the real libwayland (GT) |
| timerfd and the rest | posixtest (GS2) |
| The compositor's additions | utest's compositor tests per protocol (GC1 to GC4); screenshot tests in `tools/comp-test.sh`'s style; every existing desktop script unchanged (`tools/desk-test.sh`, `tools/apps-test.sh`, `tools/clip-test.sh`, `tools/mouse-test.sh`) |
| The ports | each recipe's `test` through M13.7's ports-test script |
| The apps end to end | tools/wl-port-test.sh's scripts: foot, SDL's tests, Doom's timedemo, swayimg |
| Hostile clients | GR's libwayland-based clients against the live compositor; X1's hostile clients unchanged |
| The bench | BENCH.md gains: Doom's timedemo frames a second (QEMU and the PC), foot's time to scroll 10 MB of text (`cat` of a file; the same on Linux on the PC for comparison), the compositor's paint cost with a foot window full screen |
| The regression | once, after GJ, on the PC: ktest at 4, the init run twice, every area script, the desktop scripts, the new scripts, the soak at 4 |

## The PC check

After a flash or an `update`, with the Mac's ports server running:

1. `pkg install foot`; foot from the shell: JetBrains Mono, the
   compositor's title bar; type, select with the mouse, Super+C, paste
   into a Jam OS terminal with Super+V, and back; `foot -e vim
   /data/notes.txt`, edit and save; resize by dragging, tile with
   Super+T; Super+Q closes it. A photo of foot next to a Jam OS terminal.
2. `pkg install chocolate-doom freedoom`; play the first level: frames
   a second (its `-timedemo` result into BENCH.md), sound effects and
   music through the headphones, the mouse turning (after GC4), full
   screen and back.
3. `pkg install swayimg`; open a folder of the owner's photos on `/data`;
   zoom, next, previous.
4. With all three open: kill each from the shell in turn; the others and
   the desktop unchanged. Kill the compositor (`kill compositor`): the
   ported clients die with their connection (libwayland does not
   reconnect, unlike libjwl's clients; section "Risks"), the Jam OS
   windows come back.
5. If GD merged: foot from the search box.
6. **All tests and `soak 10`** with foot open and the second stick
   mounted read-write, pulled and replugged (the sign-off tier).

## Size

- **Agents**: 13 tracks (GS1, GS2, GC1 to GC4, GP1, GP2, GT, GA1 to GA4)
  plus GD, the review and the join: **17**, about **30 agent-hours**
  (GS1 3, GS2 1, GC1 2, GC2 3, GC3 2, GC4 1.5, GP1 2, GP2 2.5, GT 1.5,
  GA1 2, GA2 2.5, GA3 1.5, GA4 1.5, GD 1, GR 3, GJ 1). The stretch adds
  about 8 (GX1 2.5, GX2 5 or more). Seven Opus (GS1, GC1 to GC4, GR,
  GX1), the rest Sonnet 5.5.
- **The uncertain part** is the ports: a library that fights its cross
  build (fontconfig's build-time tools, xkbcommon's parser generator,
  libffi's assembly) can take twice its hour, and an app that finds a gap
  in the POSIX layer stops at a question, not a fix (M13.7's rule).
- **Time**: GS1, GS2 and GC1 in parallel, about 3 hours; GC2 and GP1,
  GP2 about 3 more; then the apps two or three at a time, about 4; the
  review and the join about 4; the regression and one PC session. About
  two to three working days of agents, after M13.7's PO1 and PO8 land.
- **Lines**: about 1,200 of C in libc (the shim 900, timerfd and the rest
  300); about 2,800 in the compositor (versions and decorations 400,
  subsurfaces 1,000, popups 700, constraints 400, tests 300); about 600
  of SDL patches (the framebuffer 250, audio 350); recipes and small
  patches for about 20 ports.

## Risks

- **libwayland clients don't reconnect.** libjwl's clients come back after
  the compositor restarts or calls them too slow; a libwayland client
  gets `wl_display.error` or a closed socket and, in almost every app,
  exits. A compositor crash now closes the owner's foot windows and
  their shells. The compositor's restart memory (`jam_window_memory_v1`)
  can't help them. Mitigation: the compositor's crash rate, which the
  sign-offs measure; the "too slow" limit (32 batches, then 64 KiB) is
  generous for an app that keeps its loop turning, but a game loading a
  level for seconds while the mouse moves (motion is coalesced, so this
  is bytes, not a flood) should be watched in GA3's test.
- **Kept memfds commit as they grow** (Q12). foot's pool is capped by its
  recipe; the next app that sizes a pool large and sparse (browsers do)
  costs its whole size in RAM or is refused at 256 MiB. Each such port
  needs its own cap until M14's review decides Q12's (B).
- **The SDL patch is ours to carry** until upstream takes it (SDL's
  developers have assumed GL on Wayland). It is about 250 lines in one
  driver file; written as SDL3 code first and offered there, it has the
  best chance. Until then it is an override checked against upstream's
  hash on every version bump.
- **Subsurfaces in a tiling compositor.** A window is a tree now; every
  place that treats a window as one surface (tiling's minimum sizes,
  animations, the restart memory, screenshots, the frame's shadow) must
  use the tree's box. GC2 lists every caller; GR reads them.
- **Ports find gaps in the layer** (M13.7's risk, sharper here): foot,
  SDL and swayimg use calls nobody has tried on Jam OS yet (`timerfd`,
  `inotify`, `ppoll`, `MSG_CMSG_CLOEXEC`, `sigabbrev_np`, `execvpe`). Each
  is reported as a finding for the layer, never patched around in a
  port.
- **foot's spawn**: its override hands the slave as descriptors with
  `POSIX_SPAWN_SETSID`; *unsure* whether the child then has the slave as
  its controlling terminal on Jam OS (M13's `user/posix/sys/tty.c` makes
  a first terminal description the process's terminal unless
  `O_NOCTTY`), which decides whether Ctrl+C in foot reaches the program.
  GA1 tests it first.
- **Fonts on FAT32**: fontconfig's cache and lock files work by
  fallbacks (no links); a stale cache after a `pkg upgrade` of the fonts
  is fontconfig's usual problem, solved by its timestamps (FAT's 2-second
  times are enough).
- **Performance in QEMU**: TCG renders fonts and SDL frames slowly; every
  timing is read on the PC (KVM or real hardware), never from a TCG run.
- **Licences**: FreeType's FTL credit line must reach every package's
  licence list; Chocolate Doom is GPL-2.0 (source beside it, M13.7's
  rule); the LGPL reading of Q8 is the owner's to confirm.
- **Static sizes**: about 3 MB per GUI program and every program its own
  copy of FreeType, xkbcommon and libwayland. Fine on `/data`; a FreeType
  security fix rebuilds every GUI package (M13.7's audit sees it).
- **Upstream moves**: foot, SDL and wayland-protocols release often; the
  recipes pin versions and hashes, and `third_party/wayland-protocols`
  stays the compositor's own pinned copy, so a client newer than the
  compositor simply sees older versions offered.

## Later

- **GTK** (rung 6 and 7) and **Qt** (rung 8) as their own project, from
  GX2's findings.
- **GL**: Mesa with softpipe (G4 or M14.5), llvmpipe with a JIT rule
  decided by the owner.
- **Primary selection, drag and drop, more clipboard types, input
  methods, more keyboard layouts and Compose, HiDPI scaling,
  `wp_viewporter`, `xdg_activation_v1`**: each a small compositor track
  when a port wants it.
- **D-Bus** (portals, accessibility, notifications from ported apps into
  the compositor's notices): a service socket of its own (section 1's
  table) and a broker port.
- **Chromium** ([Not planned](ROADMAP.md#not-planned)'s browser line):
  from its Fuchsia code, not through this shim; it will want GL (or
  SwiftShader, a JIT) and a sandbox story of its own.

## GP1 notes

What track GP1 (the base ports) built, the choices it made, and what GP2,
GS1 and GT need. Built and staged on the Mac and the PC's WSL; **nothing has
run on Jam OS yet** (that waits for GS1's socket and M13.7's PO8
ports-test script).

**The recipes** (`ports/`, all `place package`, static):

| Port | Version | Style | Licence (read in the tarball) | Stage |
|---|---|---|---|---|
| `libffi` | 3.8.0 | custom (`build.sh`: configure and make, then the tests) | MIT AND CC0-1.0 (src/closures.c builds in Doug Lea's dlmalloc, a CC0 dedication) | libffi.a 44 KB; `bin/ffi-tests` 390 KB |
| `expat` | 2.9.0 | configure | MIT AND CC0-1.0 (lib/siphash.h) | libexpat.a 302 KB; `bin/xmlwf` 498 KB |
| `wayland-scanner` | 1.26.0 | meson, **place host** | MIT | `bin/wayland-scanner` (for the build machine, 70 KB on the Mac) |
| `wayland` | 1.26.0 | meson | MIT (34 source files read; xcursor.c too) | libwayland-client.a 101 KB, libwayland-cursor.a 32 KB, headers, .pc; `bin/wl-connect-test` 369 KB |
| `wayland-protocols` | 1.49 | meson | MIT AND HPND-sell-variant | 58 XML files, 64 `*-enum.h`, wayland-protocols.pc (all `library`: no package contents) |
| `xkbcommon` | 1.13.2 | meson | see below | libxkbcommon.a 555 KB, headers, .pc; `xkbcli-compile-keymap` and `xkbcli-how-to-type` 666 and 670 KB, `us.xkb` |

The licences are not all "MIT" as the track brief expected, and the
recipes say what was found: **xkbcommon** carries MIT, MIT-open-group, HPND,
HPND-sell-variant, ISC (one header) and Digital Equipment Corporation's 1987
permission notice (src/atom.c; upstream names it
`LicenseRef-digital-equipment-corporation`), all permissive with the notice
kept; **wayland-protocols**' COPYING is MIT but five installed XML files
(text-input-unstable-v3, pointer-gestures-unstable-v1, ext-workspace-v1,
ext-data-control-v1, ext-foreign-toplevel-list-v1) carry the X.org
"permission to use, copy, modify, distribute, and sell" wording
(HPND-sell-variant) in their own copyright blocks, so a package that ships
the XML ships them as they are. `tools/portslib/licence.py` gained
`MIT-open-group`, `HPND-sell-variant` and that one `LicenseRef-` name (a
reviewed exception; every other `LicenseRef-` is still refused).

**What was added to the builder**, each in its own module with a line or two
of hook in the PO1 files:
- `tools/portslib/hostdata.py`: **host builds** and **data ports**, and
  `selftest_hostdata.py` (36 checks, run by `ports.py selftest`).
  - `place host`: built natively (the machine's `cc`, no sysroot, no cross
    tools), into `build/ports/<name>/stage/`, which **is** its prefix (the
    real absolute path, so a `.pc` file or script it installs works in
    place); nothing of it is packaged, checked as a Jam OS program or put in
    a sysroot; its stage list says `place host`. The plan's
    `build/ports/host/` became one stage per tool, like every port.
  - `tool <port>` (recipe key): the build of the port that names it runs the
    host port first, gets its `bin/` on PATH, and, for a Meson build, its
    `lib/pkgconfig` and `share/pkgconfig` in a generated `--native-file`
    (`pkg_config_path` for the build machine only; the cross lookups stay
    the sysroot's). The tool's stamp is in the user's, so a tool change
    rebuilds only what names it. A host port may `tool` another; it may not
    `needs` (no sysroot) or have `library`/`files`/`subpackage` lines.
  - `host-of <port>` (a host port's key): the lint refuses a host port
    whose version or `source` lines differ from the port it is built for
    (wayland's `dependency('wayland-scanner', version: ...)` wants the same
    release). The host recipe is a second recipe directory with the same
    source line, not a second build of one recipe.
  - `style data` with `data <path in the source> <path in the prefix>`
    lines: a port that is files and no build (GP2's fonts, a theme); the
    `files`/`library` lines must keep each destination (lint), a symlink in
    a copied directory is refused. No port uses it yet: wayland-protocols
    is Meson's own build, which also gives its `.pc` and runs the scanner
    over every protocol (the `-enum.h` headers are that check, so the
    scanner is tested on all 64 files by building it).
- `ports.py`: `tool` in `graph` and the build order; `~/.cache/jamos/tools/bin`
  (or `$JAMOS_TOOLS/bin`) put first on PATH.
- `stage.py`: a `program` may be in a `subpackage` (it stays a program, with
  its list checked, and gets a `sub` line too). PO3: a `sub` line can name a
  `program` path.
- `toolchain.py`: the port's cc drops `-pthread` (Meson's threads
  dependency adds it; musl has the threads in libc and the cross gcc has no
  such option).
- `ports/jamos.meson`: `prefer_static = true` (`dependency()` asks
  pkg-config for the static link line), `wrap_mode = 'nodownload'`. "No
  dlopen" is not a Meson setting: it is each port's own option, as section 9
  says. The Meson style also passes `--libdir=lib`.
- `tools/genkeymap.py check` also fails if `ports/xkbcommon/override/us.xkb`
  (the copy the xkbcommon test installs) is stale.

**The tests** (`ports/<port>/test`, the shell-test format; **unrun**):
- `libffi`: `bin/ffi-tests`, libffi's own `libffi.call` and the `ffi_call`
  tests of `libffi.complex` (98 tests) linked into one program with each
  `main` renamed and `exit`/`abort` caught (override/ffi-tests.c is the
  driver, BSD-2); closure tests are left out (they need executable memory).
  Run natively on the PC with the same driver and build.sh: 98 passed. On Jam
  OS: `ffi-tests` prints `ffi-tests: 98 passed, 0 failed`; `ffi-tests <name>`
  runs one with its output; `-l` lists.
- `expat`: `xmlwf` on a good file, a bad one, a missing one.
- `wayland`: `bin/wl-connect-test` (override/wl-connect-test.c, BSD-2): connects,
  lists the registry's globals (the events reach the listener through
  libwayland's `ffi_call` dispatcher), checks for `wl_compositor`, `wl_shm`
  and `xdg_wm_base`. Needs `svc wayland` (its list) and the desktop running:
  GS1 first.
- `xkbcommon`: upstream's `xkbcli-compile-keymap --from-xkb` on the
  compositor's `us.xkb` (720 lines back; garbage refused) and
  `xkbcli-how-to-type` (a, A, !, Return find AC01, Shift+AC01, Shift+AE01,
  RTRN). The expected lines are what the same xkbcommon printed built for
  Linux on the PC.
- `wayland-scanner` and `wayland-protocols` have no Jam OS test: the scanner
  runs at build time (above).
All four test programs are in `-test` subpackages (`libffi-test`,
`wayland-test`, `xkbcommon-test`); `xmlwf` is `expat`'s own program.

**Building** Meson ports needs Meson 1.4 or newer (xkbcommon 1.13) and
Ninja: the PC's apt Meson is 1.3.2, so both are in a venv at
`~/.cache/jamos/tools` there (recorded in TESTING.md's machine setup). On the
Mac the same: a venv (or `$JAMOS_TOOLS`), and `bison` 3.6 or newer for
xkbcommon (the system's is 2.3). wayland-scanner links the build machine's own
expat (the Xcode SDK's, Ubuntu's `libexpat1-dev`) and has DTD validation off
(it would need libxml2).

**For GP2**: fontconfig needs expat (`needs expat`) and `gperf` on the host
(not on the Mac: `host gperf` will say so; it is in `/usr/bin` on this Mac
but the PC lacks it, `apt install gperf`); fonts-jamos is the first `style
data` user, or `files` from a `vendored` source. **For GS1/GT**: libwayland
is built with epoll, `SFD_CLOEXEC`/`TFD_CLOEXEC` found, `memfd_create`,
`mremap`, `prctl` and `accept4` found by Meson's link checks (they link in
musl; whether the layer implements them is GS1's to know); `wl-connect-test`
is the first program to try, then wlprobe on `libwayland-client.a` +
`libffi.a` (a port that `needs wayland` gets libffi too). The
client's `.pc` has `Requires: libffi` and `Libs: ... -lm -pthread -lrt`; the
port cc drops `-pthread`, and the sysroot has an empty `librt.a`.
**For PO3**: library-only ports (wayland-protocols, and wayland, libffi,
expat, xkbcommon without their programs) have nothing to package; their
stage lists hold `library` lines and the licence only.
