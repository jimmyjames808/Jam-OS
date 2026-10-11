# G2 plan: Wayland clients, for Chromium

Status (2026-10-11): a plan, **rescoped to what Chromium needs** (the
owner, 2026-10-11: [the rescope](#the-chromium-rescope-2026-10-11)); GC1
built ([GC1 notes](#gc1-notes)), the rest not. First written against main
e30d7fec; rescoped against main d28b8b52 (M13's sockets, sessions, ptys,
tinyssh and pipelines merged; M12.5's PF1 and M13.6's U1 merged; M13.7's
PO1, PO2, PO3 and PO8 merged; GP1 running). Q1 and Q4 are answered (each
question's status line says how); the questions the rescope made moot say
so. The shim (GS1) and the compositor's tracks touch no other
milestone's files; the ports use M13.7's builder (merged).

Goal ([roadmap](ROADMAP.md#now), the owner, 2026-10-11): **the Wayland
floor Chromium's Ozone/Wayland backend stands on, and nothing Chromium
doesn't need yet.** libwayland reaches the compositor through a shim in
libc from Unix sockets and file descriptors to Jam OS channels and
handles (Chromium builds its own libwayland and calls `sendmsg` like any
client, so a shim below libwayland serves it unchanged); the compositor
([HISTORY: G1](HISTORY.md#g1-the-desktop)) offers the protocols Chromium
requires and the ones its browser UI uses (subsurfaces for its bubbles,
popups for its menus, decorations, the clipboard, cursors); fonts arrive
as a data package; and a small test client on the real libwayland
(wlprobe) proves each layer before Chromium does. Every piece is general:
any later Wayland port uses it unchanged. Jam OS writes no general apps
of its own ([Not planned](ROADMAP.md#not-planned)); the apps this plan
first chose as proofs (foot, SDL2, Chocolate Doom, swayimg, a GTK spike)
come after Chromium, by need ([Later](#later)).

**Done when:**
1. **libwayland-client is a port with nothing patched in its transport**:
   built static by M13.7's builder, it connects to `/svc/wayland` through
   libc's new Wayland socket (section 1), and a test client built on it
   (wlprobe, track GT) passes in QEMU: the registry, a `wl_shm` pool from a
   memfd, the keymap mapped and compiled by xkbcommon, a paste both ways
   through a pipe, 28 descriptors in one flush, libwayland's own
   one-byte-with-descriptors flush, a slow reader, 1,000 connections
   opened and closed, and a pool made from a memfd that another process
   handed over (Chromium's GPU process hands its buffers to the browser
   this way).
2. **The compositor offers what Chromium's Ozone/Wayland uses**
   ([the table](#what-chromiums-wayland-backend-needs)): its required
   globals at the versions it binds, the decoration rule the owner chose
   (section 3), subsurfaces (GC2) and real popups with grabs (GC3), each
   with its protocol tests in utest and a wlprobe case.
3. **Chromium's first window is G2's end-to-end proof**: the Chromium
   plan's `content_shell` on Wayland (its milestone CR-c) draws a page in
   a Jam OS window in QEMU and on the PC, with keys, the mouse, the wheel,
   a `<select>` list (a popup) and copy and paste both ways with a Jam OS
   terminal; then `chrome`'s omnibox list (a subsurface) and its menus.
4. **A ported client can hurt only itself**: the compositor's hostile
   tests run again with libwayland-based clients (X1's style, G1), the
   shim gets a fuzz loop, and Chromium killed, hung or fed garbage leaves
   the desktop and the other windows as they were.
5. Every library is a package built from a recipe (M13.7), static, with
   its licence; the fonts are a data package.
6. As every milestone: an independent review-and-fix, then All tests and
   `soak 10` on the PC with a libwayland client open (wlprobe's hold
   mode, then Chromium once CR-c has landed).

## What G2 is not

- **No GPU.** Software rendering only. G3 (mode setting) and G4 (3D) are
  deferred (porting first). No OpenGL or Vulkan in G2: what that rules
  out, and the options for later, are in section 6 and Q3.
- **No apps of its own choosing.** foot, SDL2, Chocolate Doom with
  Freedoom, swayimg and the GTK3 spike were this plan's proofs before the
  rescope; they come after Chromium, by need ([Later](#later)). Their
  findings below (sections 2, 4, 7 and 11) stay as the record for then.
- **No GTK and no Qt.** The roadmap makes them a porting project of their
  own. This plan says what each pulls in (section 7); the GTK3 spike is
  after Chromium.
- **No X11.** No Xwayland, no X server: X11 programs are not ported.
  (Redox and FreeBSD carry X; Jam OS has no use for it while every
  toolkit worth porting speaks Wayland.)
- **No new compositor.** Weston and wlroots stay unported (they need DRM,
  KMS, libinput and udev: [the G1 row](ROADMAP.md#later)); Jam OS's own
  compositor gains the protocols the apps need (section 2).
- **No named Unix-domain sockets.** The shim answers `connect` to
  `/svc/wayland` only (section 1). Descriptor passing (`SCM_RIGHTS`) on
  socketpairs, which Chromium's Mojo needs, is the Chromium plan's general
  layer (CR0); GS1's two translation tables (descriptors to handles and
  back) are written once, in a file of their own, so that layer extends
  them to every descriptor kind instead of writing a second copy.
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
| Globals offered | since GC1: `wl_compositor` 6, `wl_shm` 2 (ARGB8888 and XRGB8888), `wl_output` 4, `wl_seat` 9 (keyboard and pointer, never touch), `xdg_wm_base` 6, `wp_cursor_shape_manager_v1` 2, `wl_data_device_manager` 3, `jam_window_memory_v1` 1, `zxdg_decoration_manager_v1` 1 (before GC1: 4, 1, 3, 5, **1**, and no decorations) | `user/services/compositor/display.c`, the versions in `user/services/compositor/comp.h` |
| Not offered | `wl_subcompositor`, `wp_viewporter`, relative pointer, pointer constraints, primary selection, text input, `xdg_activation_v1`, presentation time, fractional scale, `zxdg_output_manager_v1`, linux-dmabuf | |
| Popups | `xdg_popup` is accepted and dismissed at once (`popup_done`), its positioner checked | `user/services/compositor/xdg.c` |
| Decorations | since GC1: a toplevel with a decoration object gets the compositor's title bar, one without draws its own; a `client_side` request is honoured (section 3 says what changes) | `user/services/compositor/deco.c`, `user/services/compositor/xdgdeco.c` |
| The transport, JWL2 | a batch per channel message: a 16-byte header, whole Wayland messages, at most 16 KiB and 28 handles; handles a queue per direction as fds on a socket; the compositor keeps at most 32 batches unread at a client, then holds 64 KiB, then disconnects it ("too slow"). `<jwl.h>` already describes how a libwayland shim must cut a flush into batches | `user/include/jwl.h`, `user/lib/jwl_transport.c` |
| Limits that match libwayland's | a message at most 4,096 bytes (libwayland's own maximum), 28 handles a batch (libwayland's `MAX_FDS_OUT`) | the same |
| The keymap | the US layout as a whole XKB text keymap, generated (nothing from xkeyboard-config), in a kept VMO handed to each `wl_keyboard` read-only; `tools/genkeymap.py check` compiles it with xkbcommon's `xkbcli` when installed | `tools/genkeymap.py`, `abi/keymap`, `user/services/compositor/keyboard.c` |
| Copy and paste | `wl_data_device_manager` 3, text only, the transfer through a channel the reader makes ("a pipe"); write-only channel ends accepted (M12's W16); `start_drag` cancels its source | `user/services/compositor/data.c` ([ARCHITECTURE: Copy and paste](../ARCHITECTURE.md#copy-and-paste)) |
| memfd and `shm_open` | a kept VMO (`VMO_KEEP_PAGES`), grown with the file and **committed and charged as it grows, never shrinking** (M12's W15); seals kept in libc | `user/posix/sys/memfd.c` |
| Descriptors for the shim | pipes and socketpairs over channels, `poll`, `select`, `epoll`, eventfd, ptys (`posix_openpt`, at most 8, `/dev/pts/N` openable only by its maker); `sendmsg` with ancillary data is EINVAL; `AF_UNIX` is `socketpair` only | `user/posix/sys/`, [M13-PLAN.md](M13-PLAN.md#8-sockets-and-names) |
| Not in the POSIX layer | `timerfd`, `signalfd`, `inotify`, `fallocate`'s hole punching, `fork` (M13.5), a RAM `/tmp`, execute on anonymous memory (no JIT, by design: `user/posix/sys/mem.c`) | |
| Fonts | stb_truetype in libfun, reading only its two built-in faces (Inter, JetBrains Mono, SIL OFL), no shaping | `third_party/stb_truetype`, `third_party/inter`, `third_party/jetbrains-mono` ([Smooth text](../ARCHITECTURE.md#smooth-text)) |
| The Wayland XML | `wayland.xml` 1.26.0, `xdg-shell.xml` and `cursor-shape-v1.xml` from wayland-protocols 1.49, vendored for `tools/genwl.py` | `third_party/wayland-protocols` |
| Ports | M13.7's recipes, builder (meson, cmake and configure styles, a sysroot per port, pkg-config pointed into it), packages and signing: merged (PO1, PO2, PO3, PO8); `pkg` on Jam OS to come (PO4 to PO7). GP1's recipes (libffi, expat, wayland, wayland-protocols, xkbcommon) and host builds running | [M13.7-PLAN.md](M13.7-PLAN.md) |
| The compiler | `tools/jamos-cc` over x86_64-elf-gcc 15.2: C only in practice. `x86_64-elf-g++` exists, but **there is no C++ runtime** (no libstdc++ or libsupc++ for the target). The Chromium plan's toolchain (clang, lld, LLVM's libc++) brings one | `tools/jamos-cc` |
| Launching an app | the shell runs a program with what its list asks (`svc wayland` gives `/svc/wayland`); the search box only starts the apps in `<deskapps.h>` (Jamjar) | `user/include/wants.h`, `user/include/deskapps.h` |

Five facts found while reading, which shaped this plan before the
rescope. The first three and the last are about apps now after Chromium;
the fourth (libffi) still holds for GP1's libwayland port:

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

## The Chromium rescope (2026-10-11)

The owner made Chromium Jam OS's main goal ([roadmap](ROADMAP.md#now)),
reached layer by layer, each layer general, and only the layers
Chromium needs built first. Chromium runs as its **Linux** build
(`target_os="linux"` against a musl sysroot, the way Alpine ships it)
with its **Ozone/Wayland** backend over Jam OS's compositor
([CR-RESEARCH](CR-RESEARCH.md); the plan: [CR-PLAN](CR-PLAN.md)). What
that changes here:

- **Kept on the path**: the shim (GS1), the compositor's versions and
  decorations (GC1, built), subsurfaces (GC2), popups (GC3), the base ports
  (GP1: libffi, expat, wayland, wayland-protocols, xkbcommon, for wlprobe
  and every later port), wlprobe and its script (GT: the shim proven on
  the real libwayland before Chromium), the review (GR) and the join (GJ).
- **New**: GC1b, a small compositor track for the decoration rule the owner
  chose (section 3) and window geometry; the table of what Chromium's
  backend needs, below.
- **Rescoped**: GP2 is the fonts package and its configuration only
  (Chromium builds its own fontconfig, FreeType and HarfBuzz: section 4);
  GD stays only if the owner starts Chromium from the search box (Q9).
- **Off the path, after Chromium by need**: GS2 (timerfd and the rest:
  nothing in Chromium's needs list calls them; a first run's ENOSYS log
  decides, and the Chromium plan adds what it shows), GC4 (pointer lock:
  only the web's Pointer Lock API uses it), GA1 foot, GA2 SDL2, GA3
  Chocolate Doom and Freedoom, GA4 swayimg, GX2 the GTK3 spike.
- **Replaced**: GX1's C++ runtime. Chromium links its own libc++ and
  libc++abi (`use_custom_libcxx`); the Chromium plan's toolchain builds
  LLVM's libc++ for other C++ ports too, so Q6's answer is (C).
  HarfBuzz, FreeType, fontconfig, ICU, libwayland and xkbcommon all come
  bundled in Chromium's tree, built static.

## What Chromium's Wayland backend needs

From `ui/ozone/platform/wayland/host/wayland_connection.cc` and the
window classes beside it (Chromium's main branch, about version 155, read
2026-10-11; links in [CR-RESEARCH](CR-RESEARCH.md)). Chromium binds each
global at the lower of its own maximum and what is offered, so offering
newer versions is harmless.

| Global | Chromium | Offered | G2 |
|---|---|---|---|
| `wl_compositor` | **required** ("No wl_compositor object"); binds at most 4 | 6 | nothing |
| `wl_shm` | **required** ("No wl_shm object"); its software output draws into `wl_shm` buffers made from memfds | 2 | nothing; the shim takes memfds that came from another process (section 1) |
| `xdg_wm_base` | **required** ("No Wayland shell found"); at most 6 | 6 | nothing |
| `wl_seat` | without it a warning and no input | 9 | nothing; its own xkbcommon compiles our keymap |
| `wl_output` | screen size and scale | 4 | nothing |
| `wl_subcompositor` | windows of type popup and bubble with a parent are `WaylandBubble`s, drawn as a desynchronised **subsurface** of their parent (chrome's omnibox list, its bubbles); also every window's primary subsurface when overlay delegation is on (only with `wp_viewporter`) | none | **GC2**: needed by `chrome`; `content_shell` may never make one (*unsure*) |
| `xdg_popup` with grabs | windows of type menu and tooltip are `WaylandPopup`s: menus, tooltips, `<select>` lists | dismissed at once | **GC3** |
| `zxdg_decoration_manager_v1` | at most 1; asks per toplevel | 1 | **GC1b**: the owner's rule (section 3) |
| `wl_data_device_manager` | clipboard (and drag and drop) | 3, text | nothing; drag and drop later |
| `wp_cursor_shape_manager_v1` | cursors by name, no theme needed | 2 | nothing |
| `wp_viewporter` | at most 1; turns on overlay delegation (a primary subsurface per window, buffers scaled by the compositor) | none | **not offered** while rendering is software: one surface a window is Chromium's simplest path. Offered with G3/G4 |
| `wp_presentation` | at most 2; frame timing | none | not offered: frame callbacks after each paint, as today |
| `zwp_linux_dmabuf_v1`, `wl_drm`, `wp_linux_drm_syncobj_manager_v1` | GPU buffers | none | not offered (software: `wl_shm`) |
| `zxdg_output_manager_v1` | at most 3; output names and logical sizes | none | not offered (`wl_output` 4 has the name) |
| `xdg_activation_v1` | focus requests between windows | none | later, small |
| `zwp_text_input_manager_v1` and `_v3` | input methods | none | later (section 8) |
| primary selection (`zwp_` and `gtk_`) | middle-click paste | none | later |
| relative pointer, pointer constraints | the web's Pointer Lock API | none | later (GC4) |
| the rest: pointer gestures, idle inhibit and notify, fractional scale, single-pixel buffer, toplevel icon, xdg-foreign, tablet, colour management, content type, keyboard shortcuts inhibit, toplevel drag, session manager, alpha compositing, `zcr_*`, `org_kde_*` | each checked for, done without | none | not offered |

What else Chromium does that the compositor and the shim must take:
- **`xdg_toplevel`**: titles, app ids, minimum and maximum sizes, maximise,
  full screen, minimise, interactive move and resize (a client-side
  frame), `set_window_geometry`; no window menu (GC1's
  `wm_capabilities` already says so).
- **Buffers from another process.** Multi-process Chromium draws in its
  GPU process (software compositing still runs there) and hands each
  buffer's memfd to the browser over Mojo; the browser makes the
  `wl_shm` pool. So the memfd the shim translates arrived by
  `SCM_RIGHTS` from a sibling: its description holds the same kept VMO,
  and the compositor's check (kept) passes. A window at 2560x1440 is
  14.7 MB a buffer; two or three a window fit the compositor's 256 MiB
  of pools a client.
- **Its own libwayland and xkbcommon.** `use_system_libwayland` is false
  by default: Chromium links the copy in its tree, which is why the shim
  lives in libc (Q1's (A)) and not in a patched libwayland.
- **Several connections per process**: none expected (one per browser
  process); the shim allows any number anyway.

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
| **Fuchsia** | Scenic/Flatland; a **Wayland bridge** (`wayland_bridge`) translates Linux guests' Wayland (over virtio-wl) into Flatland | channels and handles, as ours: the bridge turns fds into VMOs and channels | Linux apps in a VM; Chromium natively (its Fuchsia port; Jam OS takes Chromium's Linux route instead: [CR-RESEARCH](CR-RESEARCH.md)) | **the translation itself**: Wayland's fds as VMOs and channels is exactly G1's choice, and what the shim does on the client's side | a VM |

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

**Since the rescope**, two more rules for GS1:
- **The translation tables live in a file of their own** (the
  descriptor-to-handle and handle-to-descriptor tables above, with the
  kind checks), named for what they do, not for Wayland: the Chromium
  plan's `SCM_RIGHTS` on socketpairs (Mojo) uses the same tables,
  widened there to every descriptor kind the POSIX hand-over can pass
  (files, sockets, eventfds). The Wayland socket keeps its narrow list
  (memfds and channel ends) on top of the shared code.
- **A memfd that came from another process** (by `SCM_RIGHTS` or the
  hand-over) is a memfd like one made here: its description holds the
  kept VMO, so the first row of the table applies. wlprobe tests it with
  a child that makes the pool's memfd and sends it to its parent.

### 2. What the compositor must add

The protocols the first apps used, what the compositor did before GC1,
and what G2 planned to add. "Needs" lists who fails without it; "uses"
who is better with it. **Since the rescope** the columns for foot, SDL2,
swayimg and GTK3 are the record for after Chromium; what Chromium needs
is [its own table](#what-chromiums-wayland-backend-needs), and the G2
column below is cut to it: `wl_subcompositor` (GC2), real popups (GC3)
and the decoration rule (GC1b, section 3) stay; relative pointer and
pointer constraints (GC4) wait; `wp_viewporter` stays unoffered.

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

**The owner's answer (Q4, 2026-10-11): "clients that bind it get the
compositor's title bar."** GC1 built a different rule (its
[notes](#gc1-notes): a `client_side` request is honoured, and a toplevel
without a decoration object draws its own even when its client bound the
manager). **GC1b switches to the owner's rule**, which is this:

- A client that has **bound** `zxdg_decoration_manager_v1` gets
  server-side decorations on **every** toplevel: the compositor's title
  bar, frame, circles and corners. A decoration object's `configure`
  always says `server_side`; `set_mode(client_side)` is answered with a
  `configure` saying `server_side`, as the protocol allows ("the
  compositor may decide to use a different mode"), and `unset_mode` the
  same. A toplevel of such a client made without a decoration object is
  server-side too.
- A client that **never bound** the manager draws its own (GC1's
  client-side look: no title bar, corners and shadow only when floating).
- `mode_for` in `user/services/compositor/xdgdeco.c` and the default in
  `user/services/compositor/xdgtop.c` (a client's bound flag, not the
  toplevel's object) are the whole change, with utest's cases turned
  round: a client that binds and asks `client_side` gets `server_side`
  and a title bar; a client that never binds gets none.

**How Chromium's choice meets it.** Chromium binds the manager when it is
offered and asks a mode per toplevel: `chrome` asks for client-side
unless its "use system title bar and borders" setting is on, because its
tab strip is its title bar. Under the owner's rule it is told
`server_side`; Chromium reads the decoration's `configure` and should then
draw the frame it draws with the system title bar on (the tab strip
under our title bar, no shadows of its own), which is a look Chromium
already supports. *Unsure*: that Chromium follows the `configure` rather
than its own wish; if it doesn't, a `chrome` window shows two title bars
(ours above its tab strip), and the fix is the setting (system title bar
on) or the one line of `mode_for`, the owner's call then. `content_shell`
draws no frame of its own at all, so server-side is right for it either
way. GC1b also uses `set_window_geometry` for placement and the frame
(GC1's "not done" item): a client-side client that draws its own shadow
(any client that never binds the manager, and Chromium if it ever runs
client-side) is placed by its geometry, and gets neither our corners nor
our shadow when its geometry is smaller than its surface.

### 4. Fonts

**For Chromium (the rescope):** Chromium builds its own fontconfig
(`use_bundled_fontconfig`), FreeType (`use_system_freetype` false) and
HarfBuzz into its binary, so G2 ports none of them for it. What it still
needs from the system is **font files and a fontconfig configuration**:
track GP2 is now only the `fonts-jamos` data package below, its
fonts.conf (the font directory, the aliases, the cache directory under
`XDG_CACHE_HOME`) and `FONTCONFIG_FILE` pointing at it, which the shell
sets for every program (section 10) so Chromium runs unwrapped. The font
set grows for the web: **Liberation Sans, Serif and Mono** (SIL OFL 1.1,
metric-compatible with Arial, Times New Roman and Courier New, so pages
laid out for those don't reflow), **Noto Sans, Serif and Sans Mono**
(Latin, Greek, Cyrillic; OFL) for coverage, Inter and JetBrains Mono
whole, and **Noto Color Emoji** (OFL; Chromium's FreeType draws its
colour bitmaps); CJK later (Noto Sans CJK is large: a package of its own
when a page needs it). Aliases: `sans-serif` Noto Sans (Liberation Sans
for Arial and Helvetica), `serif` Noto Serif (Liberation Serif for Times),
`monospace` JetBrains Mono (Liberation Mono for Courier). *Unsure*: the
configuration path Chromium's bundled fontconfig reads when
`FONTCONFIG_FILE` is unset (its build's sysconfdir); setting the variable
avoids depending on it. The rest of this section is for ports after
Chromium.

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

**Since the rescope** only rung 0 is on the path (libwayland and its
cursor library for wlprobe, with xkbcommon from rung 1 for the keymap
case), plus the fonts package. Rung 5's C++ runtime is the Chromium
plan's toolchain (LLVM's libc++); the other rungs come after Chromium, by
need.

**Why foot first** (before the rescope): it is the smallest real Wayland program that needs
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
  Qt, and many apps later (any game in C++). Q6: since the rescope, LLVM's
  libc++ from the Chromium plan's toolchain.
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
  to point it; and `FONTCONFIG_FILE`, the fonts package's configuration
  (section 4), for Chromium's bundled fontconfig and every later port's. Programs that warn without `XDG_RUNTIME_DIR` (GTK, foot's
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

**Since the rescope** the ladder is wlprobe, then Chromium (the Chromium
plan's `content_shell`, then `chrome`); the apps from foot down are after
Chromium, by need, and the table stays as their record.

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
| libc (`user/posix/sys/`) | `connect` on `AF_UNIX` `SOCK_STREAM` to a path in the service-socket table (today `/svc/wayland`); the Wayland socket descriptor kind: `sendmsg`/`recvmsg` with `SCM_RIGHTS` on it, `MSG_CTRUNC`, `MSG_CMSG_CLOEXEC`; the descriptor-to-handle and handle-to-descriptor tables of section 1, in a file of their own that the Chromium plan's socketpair `SCM_RIGHTS` shares. After Chromium, by need (GS2): `getsockopt(SO_PEERCRED)`, `timerfd_*`, `fallocate` |
| the compositor's globals and versions | `wl_compositor` 6, `wl_subcompositor` 1, `wl_shm` 2, `wl_output` 4, `wl_seat` 9, `xdg_wm_base` 6, real `xdg_popup` (3), `zxdg_decoration_manager_v1` 1 under the owner's rule (section 3): the set a client may count on is part of what M14 freezes (G1's protocol is M12's). Relative pointer and pointer constraints after Chromium |
| new caps | subsurface tree depth (8) and children per surface (32); popups per client |
| libjwl | windows bind the decoration manager and ask server-side (`user/lib/jwl_window.c`; GC1, built) |
| the shell's environment | `WAYLAND_DISPLAY`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_DIRS`, `FONTCONFIG_FILE`, and `XDG_RUNTIME_DIR` (Q10) |
| M13.7's recipes | host builds and data ports (GP1); `desktop <program> <name> <summary>` (Q9, if GD runs) |
| `abi/idl/initctl.idl` | `launch` of an approved package program with a `desktop` line (Q9, if GD runs): the same method, a wider check in init |
| `third_party/wayland-protocols` | xdg-decoration XML (GC1, built); nothing new for subsurfaces (core `wayland.xml`) or popups (xdg-shell) |

## Questions for the owner

Each stands alone and ends with a recommendation; the plan assumes the
recommendation until you say otherwise. **Since the rescope** each ends
with its status too: answered, open, or moot (off Chromium's path).

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

**Status:** (A), as recommended (2026-10-11). The rescope makes it the
only one that serves Chromium, which links its own libwayland.

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

**Status:** moot: off the path (Chromium rescope, 2026-10-11). SDL comes
after Chromium, by need; the recommendation stands for then.

### Q3. Drawing for SDL, and GL

- **(A)** No GL in G2: SDL's Wayland driver gains a `wl_shm` framebuffer
  (a patch, offered upstream), and GL programs wait.
- **(B)** Mesa with softpipe behind EGL, in G2.
- **(C)** Mesa with llvmpipe, which needs a JIT rule.

*Recommendation: (A).* About 250 lines against a large port that
probably needs dynamic linking (B) or a new rule on executable memory
(C). softpipe becomes G4's or M14.5's first step; the JIT question is
yours separately, when a program worth it asks.

**Status:** moot: off the path (Chromium rescope, 2026-10-11). Chromium
draws in software with Skia and needs no GL; GL is G3's and G4's.

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

**Status:** answered by the owner (2026-10-11): "clients that bind it
get the compositor's title bar". (A), with the rule keyed on the client
binding the manager; GC1 built a variant, GC1b switches to the owner's
rule (section 3).

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

**Status:** (A) stands for the ports after Chromium. For Chromium itself
only the fonts package and its configuration (section 4): Chromium
builds its own FreeType and fontconfig.

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

**Status:** changed by the rescope (2026-10-11): (C). Chromium links its
own libc++ and libc++abi, and the Chromium plan's toolchain builds
LLVM's libc++ for every other C++ port; GX1 is replaced by it.

### Q7. GTK in G2

- **(A)** None: GTK and Qt are the later project the roadmap says.
- **(B)** A GTK3 spike at G2's end (gtk3-demo and one small app), not
  gating the sign-off, to measure that project.
- **(C)** GTK3 fully in G2.

*Recommendation: (B).* A day of agents turns "GTK is a later project"
into a list of its real gaps (popups, D-Bus, `fork` in GLib, sizes), so
that project can be planned on facts; it starts only after G2's
done-when is met.

**Status:** moot: off the path (Chromium rescope, 2026-10-11). The GTK3
spike comes after Chromium.

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

**Status:** (A), as recommended (2026-10-11), within CONTRIBUTING's
licence rule (GPL and LGPL only in separate programs, source shipped
beside them). It covers Chromium's own LGPL parts too.

### Q9. Ported apps in the search box

- **(A)** Not in G2: ported apps start from a terminal.
- **(B)** A recipe's `desktop` line; once `pkg` installed and you
  approved it, the app is in the search box and init starts it with its
  own list.

*Recommendation: (B)*, as G2's last small track. A GUI app that only
starts from a terminal is half a port. The compositor still can't start
anything but what init's list holds; the list just grows by what you
approved.

**Status:** answered (the owner, 2026-10-12): Chromium starts from the
shell first, then the search box, so GD is on the path
([CR-PLAN's Q3](CR-PLAN.md#q3-how-you-start-chromium)).

### Q10. A runtime directory and `/tmp`

- **(A)** A RAM `/tmp` (the tests' ramfs as a service, M13's noted
  follow-up), with `XDG_RUNTIME_DIR` a private directory in it.
- **(B)** `XDG_RUNTIME_DIR` on `/data` (FAT, kept across boots).
- **(C)** No runtime directory in G2; `WAYLAND_DISPLAY` absolute is
  enough for libwayland, and programs that want one warn.

*Recommendation: (A)*, with (C) until it lands. Sockets and lock files
don't belong on the stick, and a RAM `/tmp` is wanted by ports anyway
(M13's list); G2 doesn't wait for it.

**Status:** (A) with (C) until it lands, as recommended. Chromium warns
without `XDG_RUNTIME_DIR` and runs.

### Q11. The first apps

- **(A)** foot, SDL2's tests then Chocolate Doom with Freedoom, swayimg
  (section 11).
- **(B)** A simpler terminal (havoc) before foot, to separate the shim's
  problems from subsurfaces and fonts.
- **(C)** SDL2 first, foot second.

*Recommendation: (A)*, with wlprobe before all of them taking (B)'s
role: wlprobe separates the shim's problems from the apps', and foot
then tests the floor every toolkit stands on.

**Status:** moot: off the path (Chromium rescope, 2026-10-11). The first
apps are wlprobe and Chromium.

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

**Status:** (A), as recommended. Chromium sizes its memfds to its
buffers; the Chromium plan's kernel work on sparse memory is for
anonymous reservations, not memfds.

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
| **GS1. The Wayland socket** | section 1: the service-socket table, the Wayland descriptor kind (out buffer, batches, the handle queue, partial writes, `MSG_CTRUNC`), the two translation tables **in a file of their own** (the Chromium plan's socketpair `SCM_RIGHTS` widens them), poll/epoll readiness, the shell's `WAYLAND_DISPLAY` and `FONTCONFIG_FILE`; a utest client of the kind against a fake compositor end (a channel the test reads as the compositor would) | new files in user/posix/sys/ (the table, the kind, the translation), the routing lines in `user/posix/sys/socket.c`, `user/posix/sys/sockio.c`, `user/posix/sys/kinds.h`; the variables in `user/services/shell/sh_vars.c`; new utest files | Q1 (answered) | utest: whole messages into batches, a message split over three `sendmsg`s, one byte with 28 descriptors then the rest, 29 descriptors refused, every descriptor kind of the table (and EINVAL for the rest), a memfd received from a child used as a descriptor to send, a batch with handles read with too little room (`MSG_CTRUNC`, handles closed), a bad magic (ECONNRESET), `EAGAIN` on a full channel and `POLLOUT` after; posixtest: `connect` to a path not in the table (ENOENT), without `svc wayland` (ENOENT); a fuzz loop of 10,000 mutated byte streams and batches (no crash, no handle leaked: `handle_info` counts) | **Opus** |
| **GC1. Versions and decorations** | built 2026-10-11 ([notes](#gc1-notes)) | | | | |
| **GC1b. The owner's decoration rule, and window geometry** | section 3: a client that bound the manager gets server-side on every toplevel (`client_side` answered `server_side`, a toplevel without a decoration object server-side too), a client that never bound it draws its own; `set_window_geometry` used for placement, the frame and the hit areas, and no corners or shadow of ours on a client-side window whose geometry is smaller than its surface | `user/services/compositor/xdgdeco.c`, `user/services/compositor/xdgtop.c`, the geometry's lines in `user/services/compositor/wm.c`, `user/services/compositor/scene.c` (`window_frame`, `window_surface_box`) and `user/services/compositor/deco.c`; utest's compositor decoration tests | GC1 merged (it is) | utest: a bound client asking `client_side` gets `server_side` and a title bar; a bound client's toplevel with no decoration object gets one; an unbound client gets none; a client-side window with geometry inset by a shadow placed by its geometry, no shadow of ours; `tools/comp-test.sh`, `tools/desk-test.sh`, `tools/apps-test.sh` unchanged | **Opus** (small: about an hour) |
| **GC2. Subsurfaces** | `wl_subcompositor` and `wl_subsurface` (section 2): trees, sync and desync (Chromium's bubbles are desync), order, position, the tree drawn as one window, input through it, clipping to the window, the new caps; the hostile cases (a cycle, a parent destroyed first, a tree deeper than the cap); GC1's [list of what GC2 needs](#gc1-notes) | `user/services/compositor/surface.c`, a new subsurface file, the painting and input lines it needs (`user/services/compositor/paint.c`, `user/services/compositor/scene.c`, `user/services/compositor/pointer.c`), `user/services/compositor/comp.h` | GC1b merged (the same files) | utest: a parent with three children, commits in sync and desync, place_above/below, a child outside the parent clipped, input to the topmost; a desync child updated while its parent is idle (a bubble's case); the hostile cases refused with the protocol's errors; a screenshot test of a window with subsurfaces (`tools/comp-test.sh`'s way) | **Opus** |
| **GC3. Popups** | real `xdg_popup`: placement by the positioner on the output (flip, slide, resize), `grab` with a live serial, dismissal by a click outside or Escape, nested popups (a menu's submenu), `reposition`; the window manager's rules for popups (never tiled, above their parent); tooltips (popups without a grab) | `user/services/compositor/xdg.c`, a new popup file, the window manager's lines it needs (`user/services/compositor/wm.c`) | GC2 merged | utest: each positioner case placed exactly, a grab without a serial refused, a click outside dismissing the chain, a tooltip that takes no keys; screenshot of a popup at a screen edge | **Opus** |
| **GP1. The base ports** | recipes for libffi, expat, wayland (client, cursor, and wayland-scanner as a host build), wayland-protocols (data), xkbcommon; the host-build and data-port pieces in tools/ports.py with their self-tests; meson's cross file's options (static, no `dlopen`) | ports/ recipes (new), tools/ports.py's host-build part, ports/jamos.meson's lines | M13.7's PO1 merged (it is); running, paused with work in its worktree | each recipe's `test`: libffi's own `ffi_call` tests as a program; xkbcommon compiling the compositor's keymap from `tools/genkeymap.py`'s text; libwayland's `wl_display_connect` from a tiny program; the builder's self-test | Sonnet 5.5 |
| **GP2. The fonts** (rescoped) | the `fonts-jamos` data package (section 4's set: Liberation, Noto Sans, Serif and Sans Mono, Inter, JetBrains Mono, Noto Color Emoji) and its fonts.conf (the directory, the aliases, the cache under `XDG_CACHE_HOME`); every font's licence text in the package | ports/fonts-jamos (new) | GP1's data-port piece | the package's `test`: the files present with their hashes; `fc-match` is not on Jam OS (no fontconfig port), so the configuration is checked on the build host with its own `fc-match` against the staged directory (`sans-serif`, `serif`, `monospace`, `Arial` each resolve as section 4 says) | Sonnet 5.5 |
| **GT. wlprobe and its script** | wlprobe (section 11), a test program on the real libwayland, in the ports tree, with a hold mode (a window that stays, for the soak); tools/wl-port-test.sh (new): a plain boot with packages preinstalled (M13.7's PO8 image), wlprobe's cases by name; the Chromium plan adds its own cases to it later | ports/wlprobe (new), tools/wl-port-test.sh (new) | GS1; GP1; M13.7's PO8 (merged) | itself; it is the end-to-end test of GS1 | Sonnet 5.5 |
| **GD. The search box** (Q9, only if the owner starts Chromium from it) | the recipe's `desktop` line in tools/ports.py and the package's manifest; `pkg`'s approval recording it; init's `launch` accepting an approved package program; the search box's rows | tools/ports.py's line, init's `launch` check (`user/services/init/apps.c`), the compositor's search rows | Q9; M13.7's PO5 and PO6 merged; Chromium's package (the Chromium plan) | utest for init's check (an unapproved program refused, a program without the line refused); `tools/desk-test.sh` with a preinstalled package | Sonnet 5.5 |
| **GR. Independent review and fix** | GS1, GC1, GC1b, GC2, GC3: findings first, then High and Medium fixed with a test each; hostile libwayland clients against the live compositor (X1's style): a subsurface storm, a desync bubble flood, a popup chain at the cap, a decoration object on a destroyed toplevel, 28 handles of the wrong kinds, a client that never reads | the files its fixes need, named in its report | GS1, GC1b, GC2 merged (GC3 if merged by then) | its own; utest; GT's script | **Opus** |
| **GJ. The join** | ARCHITECTURE (Graphics: ported clients, the protocols offered, decorations; Userland: the Wayland socket), the porter's guide (GUI ports: the shim, the environment, the fonts and keyboard pieces, what is offered), TESTING (the new script), `third_party/wayland-protocols`'s README, ROADMAP and HISTORY | the docs | GR merged | `make check` | Sonnet 5.5 |

**Off the path since the rescope** (after Chromium, by need; their rows
are in this file's history, before the rescope): **GS2** (timerfd,
`fallocate`, `inotify_init1`, `SO_PEERCRED`: no Chromium need found; the
Chromium plan's raw-call track adds whatever a first run's ENOSYS log
shows),
**GC4** (relative pointer and pointer constraints), **GA1** foot, **GA2**
SDL2, **GA3** Chocolate Doom and Freedoom, **GA4** swayimg, **GX2** the
GTK3 spike. **GX1** (a C++ runtime) is replaced by the Chromium plan's
toolchain.

## Order

1. **Now, at once**: GS1 (libc), GC1b (the compositor, small), GP1
   (resumed). None touches another's files.
2. **After GC1b**: GC2, then GC3 (one compositor track at a time: the
   same files).
3. **After GP1**: GP2.
4. **After GS1 and GP1**: GT (wlprobe: the shim proven on the real
   libwayland before Chromium).
5. **GR** once GS1, GC1b and GC2 are merged (GC3 joins it if merged; else
   its own small review), then **GJ**, the main session's regression on
   the PC, and the PC check.
6. **GD** only if Q9's answer is the search box, after Chromium's
   package exists.

**Beside the other milestones**:
- **The Chromium plan**: its socketpair `SCM_RIGHTS` track starts from
  GS1's translation file; its `content_shell` on Wayland waits for GS1,
  GT and the compositor tracks it uses (GC1b first; GC3 for menus;
  GC2 for `chrome`).
- **M13**: GS1 adds files to user/posix/sys/ and routes a few calls;
  it takes R1's findings on the call table into account.
- **M13.6**: nothing needed. The fonts package lives where M13.7's prefix
  is.
- **M13.7**: every G2 port is a recipe; GP1 adds host builds and data
  ports to the builder; GD needs PO5 and PO6.
- **M12.5**: nothing waits for it.
- **The PC's QEMU slots**: GT's script runs one QEMU; it staggers with
  the others.

## GC1 notes

*Built 2026-10-11.* What GC1 did, where it differs from the plan, and what
the tracks after it (GC2 first) need to know.

**Versions** (`user/services/compositor/comp.h` has the table): every
request a new version adds is handled and every event it adds is sent
only to an object of that version, so a client bound low sees nothing
newer (utest's `compver.c` checks each at its version and below).

| Global | Offered | What the versions add, as answered |
|---|---|---|
| `wl_compositor` | 6 | `wl_surface.offset` (5): double-buffered with the rest (`struct comp_state`'s `offset_set`, `dx`, `dy`); attach's x and y must be 0 from 5 (`invalid_offset`). `preferred_buffer_scale` 1 and `preferred_buffer_transform` normal (6), sent once when the surface is made. Upstream's 7 (`get_release`) is not offered |
| `wl_shm` | 2 | `release` (it already had a handler) |
| `wl_output` | 4 | `name` (`HEADLESS-1`, or `FB-1` on the framebuffer) and `description`, before `done` |
| `wl_seat` | 9 | the wheel: `axis_value120` (120 a notch) in place of `axis_discrete` from 8, `axis_relative_direction` identical from 9 (`pointer.c`) |
| `xdg_wm_base` | 6 | tiled_left/right/top/bottom (2) for a window in a tiling screen's tree, neither maximised nor full screen (`wm_tiled`); the positioner's `set_reactive`, `set_parent_size`, `set_parent_configure` accepted and `xdg_popup.reposition` checked (3: popups are still dismissed at once, so no `repositioned`); `configure_bounds` (4) when they change, the room under the strip less the window's own decorations (`wm_bounds`); `wm_capabilities` (5) once, before the first configure: maximize, fullscreen, minimize (no window menu), and `set_minimized` now minimises a mapped window; `suspended` (6) while a mapped window isn't shown (another screen, minimised: `wm_suspended`) |
| `zxdg_decoration_manager_v1` | 1 | new (`xdgdeco.c`), global 9 |

The tiled and suspended states and the bounds are not the window
manager's configure (`struct wm_config` is unchanged, so its tests' expected
states are too): `xdgtop.c` works them out when it sends (`struct
xdg_extra`) and a change sends a configure as a size change would. A
window shown or hidden without its configure changing reaches the role
through a new optional hook, `wm_ops.shown`, called by `screens_sync` and
by a first map onto a screen not shown.

**Decorations, as built.** Q4's (A): a toplevel with a decoration object
gets the compositor's title bar and frame; one without draws its own; libjwl
binds the manager and asks `server_side` for every window (made again after
a reconnect, destroyed before its toplevel). One difference from section 3:
a client that binds the manager and **asks for `client_side` gets it**
(draws its own), as the track's brief asked, where section 3 said it would
be answered `server_side` anyway. Either way such a window has one title
bar. `mode_for` in `xdgdeco.c` is the whole policy: one line makes it
section 3's. Version 1 only: version 2's decoration object made for a
mapped toplevel is refused (`unconfigured_buffer`) until a client needs it.
- The mode travels in the configure (`WM_ST_CSD`, a state bit that is never
  an xdg state), so the frame changes with the commit that acks it, like a
  size; that holds for the decoration object's destroy too (the protocol
  says "at the next commit": here, the commit acking the configure that
  follows).
- A client-side window (`COMP_WIN_CSD`, look.h's `LOOK_CSD`): floating, no
  title bar or outline, the rounded corners (`mask_csd`, no edge colour)
  and the floating shadow; maximised, nothing; tiled, tiling's border as
  every tiled window (the focus's colour). It has no resize edges of the
  compositor's: it moves and resizes itself (`xdg_toplevel.move`,
  `resize`); Super+drag still works.
- **Not done, for GC3 or the GTK spike**: `set_window_geometry` is still
  kept and not used for placement. A client-side client that draws its
  own shadow outside its geometry (GTK, libdecor) is placed, framed and
  shadowed by its whole surface, our shadow and corners on top of its
  own. The fix: place by the geometry, and for a client-side window whose
  geometry is smaller than its surface draw neither corners nor shadow.
- The compositor's raw-protocol tests whose windows have title bars ask
  for server-side as libjwl does (`ct_server_side`); the others now run
  their windows client-side, which is the default for a client that
  doesn't ask.

**What GC2 (subsurfaces) needs from this:**
1. **The window's box.** Decorations, placement and hits all use the main
   surface's size: `window_surface_box` and `window_frame` (scene.c),
   `deco_set`, `deco_hit` and `edges_at` (deco.c), and in wm.c `centre_in`,
   `first_place`, `keep_reachable`, `place_floating`'s anchors and
   `note_floating_size` (`s->width`, `s->height`). A tree's window must
   use the tree's box there (or the window geometry: foot sets one).
2. **A client-side tree reaches outside its main surface.** foot drawing
   its own decorations (`csd.preferred=client`; its default is server)
   puts its title bar in a subsurface at a negative y. The tree's box then
   starts above the main surface, and `COMP_WIN_CSD`'s corners and shadow
   follow `window_frame`, so they follow that box.
3. **Commits.** `on_commit` (surface.c) applies the pending state at once
   and then calls the role's `commit`; a synchronized subsurface needs the
   pending state cached and applied at its parent's commit, so the apply
   step must be split from `on_commit`. The offset (`offset_set`, `dx`,
   `dy`) is part of that state; each role says what it means (testwin
   places by it, a toplevel ignores it; for a subsurface it moves the
   content against `set_position`).
4. **Visibility and frames.** `surface_visible` and the frame callbacks
   (`surfaces_frame_done`) go by `s->window`; a subsurface has none of its
   own and must answer as its root's window does. The suspended state
   (`wm_ops.shown`) is the root toplevel's, nothing more to do.
5. **Input.** `wm_window_at`'s `takes_input` (deco.c) and the seat's
   `window_at` look at the window's one surface; they must walk the tree,
   topmost first, each surface by its input region.
6. **Wiring.** `wl_subcompositor` as global 10 in display.c's table, both
   interfaces in conn.c's `known` and `requests` tables (as
   `zxdg_decoration_manager_v1` was added), and the caps in comp.h.
7. **Version 6 surfaces** get `preferred_buffer_scale` when made: a
   subsurface's wl_surface too, nothing to add.

## Tests and the sign-off

| What | Where |
|---|---|
| The shim | utest (GS1's cases), posixtest's socket section, the fuzz loop; wlprobe on the real libwayland (GT) |
| The compositor's additions | utest's compositor tests per protocol (GC1b to GC3); screenshot tests in `tools/comp-test.sh`'s style; every existing desktop script unchanged (`tools/desk-test.sh`, `tools/apps-test.sh`, `tools/clip-test.sh`, `tools/mouse-test.sh`) |
| The ports | each recipe's `test` through M13.7's ports-test script (GP1, GP2, wlprobe) |
| End to end | tools/wl-port-test.sh: wlprobe's cases; then the Chromium plan's `content_shell` and `chrome` cases added to the same script |
| Hostile clients | GR's libwayland-based clients against the live compositor; X1's hostile clients unchanged |
| The bench | BENCH.md gains: wlprobe's round trip and a 28-descriptor flush through the shim against libjwl's; the compositor's paint cost with a full-screen Chromium window (with the Chromium plan) |
| The regression | once, after GJ, on the PC: ktest at 4, the init run twice, every area script, the desktop scripts, the new script, the soak at 4 |

## The PC check

After a flash or an `update`, with the Mac's ports server running:

1. `pkg install wlprobe` (or the test image's preinstalled copy);
   `wlprobe` from the shell: its cases print their lines; its hold mode
   opens a window with the compositor's title bar (it binds the
   decoration manager), tiles with Super+T, closes with Super+Q.
2. A paste from wlprobe's window into a Jam OS terminal and back.
3. Kill wlprobe from the shell: the desktop unchanged. Kill the
   compositor (`kill compositor`): wlprobe dies with its connection
   (libwayland does not reconnect, unlike libjwl's clients; "Risks"), the
   Jam OS windows come back.
4. **Chromium's window** is the Chromium plan's PC check (its CR-c and
   CR-d): the keys, the mouse, the wheel, a `<select>` list, copy and
   paste both ways, `chrome`'s omnibox list and menus.
5. If GD merged: Chromium from the search box.
6. **All tests and `soak 10`** with wlprobe's window open (Chromium's
   once CR-c has landed) and the second stick mounted read-write, pulled
   and replugged (the sign-off tier).

## Size

- **Agents** (since the rescope): GS1, GC1b, GC2, GC3, GP1 (running),
  GP2, GT, GR and GJ, and GD if Q9 says so: **9 or 10**, about **18
  agent-hours** (GS1 3, GC1b 1, GC2 3, GC3 2, GP1 2, GP2 1, GT 1.5, GR 3,
  GJ 1, GD 1). Five Opus (GS1, GC1b, GC2, GC3, GR), the rest Sonnet 5.5.
  GC1 is built (2 hours spent).
- **Dropped from the path**: GS2 1, GC4 1.5, GA1 to GA4 7.5, GX1 2.5, GX2
  5 or more: about 17 agent-hours that wait for need.
- **The uncertain part** is GP1's cross builds (xkbcommon's parser
  generator, libffi's assembly) and GC2 (every place that treats a window
  as one surface).
- **Time**: GS1, GC1b and GP1 at once, about 3 hours; GC2, GP2 and GT
  about 3 more; GC3 about 2; the review and the join about 4; the
  regression and one PC session. About two working days of agents.
- **Lines**: about 900 of C in libc (the shim); about 2,000 in the
  compositor (the rule and geometry 200, subsurfaces 1,000, popups 700,
  tests); recipes for six ports and a data package.

## Risks

- **libwayland clients don't reconnect.** libjwl's clients come back after
  the compositor restarts or calls them too slow; a libwayland client
  gets `wl_display.error` or a closed socket and, in almost every app,
  exits. A compositor crash now closes the owner's Chromium windows and
  their tabs. The compositor's restart memory (`jam_window_memory_v1`)
  can't help them. Mitigation: the compositor's crash rate, which the
  sign-offs measure; the "too slow" limit (32 batches, then 64 KiB) is
  generous for a client that keeps its loop turning, but a browser busy
  for seconds while the mouse moves (motion is coalesced, so this is
  bytes, not a flood) is watched in the Chromium plan's tests.
- **Chromium and the decoration rule.** If Chromium draws its own frame
  even when told `server_side`, a `chrome` window shows two title bars
  (section 3). The setting "use system title bar" or one line of
  `mode_for` fixes it; which is the owner's call when seen.
- **Bubbles need subsurfaces.** Without GC2 a `chrome` window's omnibox
  list or a bubble can't be made (Chromium makes a desynchronised
  subsurface for each, and has no fallback); `content_shell` may never
  ask. GC2 lands before the Chromium plan's `chrome` milestone.
- **Kept memfds commit as they grow** (Q12). Chromium sizes its memfds to
  its buffers, so this costs what it uses; a later port that sizes a pool
  large and sparse costs its whole size in RAM or is refused at 256 MiB,
  until M14's review decides Q12's (B).
- **Subsurfaces in a tiling compositor.** A window is a tree now; every
  place that treats a window as one surface (tiling's minimum sizes,
  animations, the restart memory, screenshots, the frame's shadow) must
  use the tree's box. GC2 lists every caller; GR reads them.
- **Ports find gaps in the layer** (M13.7's risk): wlprobe and Chromium
  use calls nobody has tried on Jam OS yet (`ppoll`,
  `MSG_CMSG_CLOEXEC`). Each is reported as a finding for the layer, never
  patched around in a port.
- **Fonts on FAT32 or the Unix filesystem**: fontconfig's cache and lock
  files work by fallbacks on FAT (no links); a stale cache after a
  `pkg upgrade` of the fonts is fontconfig's usual problem, solved by its
  timestamps.
- **Performance in QEMU**: TCG renders slowly; every timing is read on
  the PC (KVM or real hardware), never from a TCG run.
- **Licences**: the fonts are OFL (each licence text in the package);
  Chromium's own licences are the Chromium plan's.
- **Upstream moves**: Chromium's Ozone backend binds new protocols
  often; `third_party/wayland-protocols` stays the compositor's own pinned
  copy, so a client newer than the compositor simply sees older versions
  offered, and Chromium checks for each optional global before using it.

## Later

- **After Chromium, by need** (the rescope, 2026-10-11; their designs
  stay above as the record, their track rows in this file's history):
  foot (GA1, with GS2's timerfd and its spawn override), SDL2 with its `wl_shm` framebuffer patch and audio
  driver (GA2), Chocolate Doom with Freedoom (GA3, with GC4's relative
  pointer and pointer constraints), swayimg (GA4), the GTK3 spike (GX2),
  and the font stack as ports (FreeType, fontconfig, pixman, fcft) for
  them. Each starts when a goal or a dependency asks for it. (Chromium is
  not in this list: it is the Chromium plan's, and comes through this shim.)
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

## GP1 notes

What track GP1 (the base ports) built, the choices it made, and what GP2,
GS1 and GT need. Built and staged on the Mac and the PC's WSL; **nothing has
run on Jam OS yet** (that waits for GS1's socket and M13.7's PO8
`tools/ports-test.sh`, written since).

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
**For PO3** (done in this track, on its merged `package.py`): a `program` that a
`subpackage` names goes into that package with its list and page table
(`plan()` routes on the `sub` line); `ports.py package` made
`libffi-test`, `wayland-test`, `xkbcommon-test` and `expat` packages on the
Mac (`--test-key`), and nothing for the library-only mains
(`libffi`, `wayland`, `wayland-protocols`, `xkbcommon`).

### Test plan (for whoever runs it next, with the PC back in WSL)

Nothing below has been run on Jam OS or in QEMU; everything above the line
was run on the Mac (`make check`, Mac builds, a forced rebuild giving the
same stage lists) and, before the PC went to Jam OS, on the PC's WSL
(`make check`, Meson builds of all five).

1. **The builds and the lint on the PC** (WSL needs the Meson/Ninja venv of
   TESTING.md's machine setup, `~/.cache/jamos/tools`, already made;
   `libexpat1-dev` and `bison` are apt-installed):
   `JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh make check`
   PASS = exit 0 with `ports lint: 11 recipes, ok`, `ports selftest: PASS`
   and `genkeymap: us's XKB text compiles (xkbcli)`;
   `JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh python3 tools/ports.py --out build/ports-pc build -f libffi expat wayland wayland-protocols xkbcommon`
   PASS = `built in` for each, no `ports:` line. (A forced rebuild here was
   queued when the PC went away: compare `build/ports-pc/<p>/stage/stage.txt`
   before and after, and with the Mac's `build/ports/<p>/stage/stage.txt`;
   the Mac's forced rebuilds gave the same bytes.)
2. **The ports' tests in QEMU** (one remote-test call per port takes a slot;
   `tools/ports-test.sh <outdir> <port>...` prints `ports-test: <port>:
   PASS` or `FAIL`):
   `JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh tools/ports-test.sh build/test/ports libffi expat xkbcommon`
   - `libffi` PASS: `ffi-tests: 98 passed, 0 failed`, `count-98`,
     `ffi-tests: 1 passed, 0 failed`, status 2 for a bad name. A FAIL names
     the test (`FAIL <name> (status N)`); rerun with `run $P/ffi-tests -v <name>`
     to see its own output. This is the first run of libffi's tests on Jam
     OS: a failure is more likely a POSIX-layer gap (mmap, signals) than
     libffi's.
   - `expat` PASS: xmlwf status 0 on the good file, `mismatched tag` and
     status 2 on the bad one.
   - `xkbcommon` PASS: `count-720`, the parse error and status 1, and the
     four `how-to-type` answers (keycode 38 AC01 for a/A, 10 AE01 for `!`,
     36 RTRN for Return).
   - `wayland` (`... ports-test.sh build/test/ports wayland`) **cannot pass
     until GS1 (the Wayland socket) is merged and the test boot has the
     compositor**: it needs `/svc/wayland`. Until then it fails at
     `connected` with `wl-connect-test: can't connect to /svc/wayland: ...`
     (or "not allowed" if the list's `svc wayland` is refused), which is the
     expected state, not a bug in the port. PASS (later) = `connected`, one
     `global` line per global, `wl-connect-test: ok (N globals)`, status 0.
3. **Not tested on Jam OS by design**: `wayland-scanner` (a host tool) and
   `wayland-protocols` (data); the scanner's check is the wayland-protocols
   build itself (it runs the scanner over every protocol file and fails if
   one is refused: 64 `*-enum.h` in the stage).
