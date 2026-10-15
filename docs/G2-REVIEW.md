# G2 review GR: the Wayland socket and the compositor's additions

G2's independent review ([G2-PLAN.md](G2-PLAN.md), track GR), at main
f3255c61, by an agent that wrote none of it. Read in full: GS1's Wayland
socket in libc (`user/posix/sys/wlsock.c`, `user/posix/sys/fdpass.c` and
`fdpass.h`, the service-socket rows of `user/posix/sys/svcsock.c`, the
routing in `sockconn.c`, `memfd.c`'s and `pipe.c`'s additions); GC1's
versions and xdg-decoration (`xdgdeco.c`, the version gates in
`xdgtop.c`, `pointer.c`, `display.c`); GC1b's decoration rule and window
geometry (`xdgdeco.c`, `scene.c`'s boxes, `wm.c`'s placement, `deco.c`'s
hits); GC2's subsurfaces (`subsurface.c`, `surface.c`'s cache and
commits, `scene.c`'s tree walks, `paint.c`'s tree drawing); GC3's popups
and grabs (`xdgpopup.c`, `xdgplace.c`, `xdg.c`, and the seat's parts:
`pointer.c`, `focus.c`, `keyboard.c`, `seat.c`), with the callers of each
interface they changed (conn.c's dispatch and teardown order, shm.c's
buffer references, wm.c, wmgrab.c).

Looked at first, as the stakes ask (a ported client, Chromium first, is
the least trusted program the desktop talks to): can a client make the
compositor read or free memory it shouldn't (a tree, a cache, a popup list
or the grab chain left pointing at something gone), loop or recurse
without bound, take the keys or the pointer from another client's window,
or keep the user from leaving its own; and does the shim keep a stream
socket's promises (partial writes, descriptors with their bytes, no
handle leaked).

Severity, as in the M12.1, M12.7 and M13 checks: **High** is a crash or
hole a client can cause; **Medium** is wrong behaviour the system or a
test will meet, or a missing defence; **Low** is a wart, a fragile limit,
or something only a crafted client meets with no harm to others.

Counts: **0 High, 1 Medium, 5 Low**, and one design question outside
these tracks' files (D1). The Medium is fixed, with a test that failed
before it; the Lows are reported.

## Findings

| # | Sev | Where | What is wrong | Failure scenario | Fix | Outcome |
|---|---|---|---|---|---|---|
| 1 | Medium | `user/services/compositor/xdgpopup.c:509` (grant), `user/services/compositor/seat.c:88` (seat_input_serial_ok), `user/services/compositor/pointer.c:274` (the dismissing press), `user/services/compositor/keyboard.c:263,281` (enters that register a serial) | A grab is granted for any of the client's last 8 input serials while its toplevel has the keyboard focus, and the press that dismisses a grab is swallowed and moves no focus (both decided in GC3). Together: the client still has the focus and its serials after the dismissal, so it can take the grab again at once, with the serial it used before, with the `wl_keyboard.enter` the keys coming back from the popup give it, or with one from a `wl_keyboard` it makes. A grabbing popup need not even be mapped to hold the pointer. | A focused client answers `popup_done` by making a new popup and grabbing with its last serial (or re-grabs an unmapped popup each turn). Every click on another window, the background or the strip is swallowed and closes a menu nobody sees: the mouse can't leave the client. Only the keyboard (Alt+Tab, the Super keys) gets out. | When the compositor dismisses a client's grab (a press outside, the focus moving, another client's grab), that client's grabs are refused until it is sent a key or button press. A user's dismissal stays the user's: the next press elsewhere goes through. | Fixed, with utest `popup_grab`'s new steps and the live `hostile-regrab` case |
| 2 | Low | `user/services/compositor/scene.c:82,142` (walk, surface_on_output), `user/services/compositor/surface.c:313` | `wl_surface.offset` (and attach's x and y below version 5) on a subsurface is kept (`s->dx`, `s->dy`) and never used: its content does not move. GC1's notes said it would ("for a subsurface it moves the content against set_position"); weston adds it to the subsurface's position. | A client growing a subsurface to the left or up by an offset sees it grow right or down instead. No known client does it (Chromium places its subsurfaces with set_position). | Add each applied offset to the subsurface's position, as weston does. | Reported |
| 3 | Low | `user/services/compositor/subsurface.c:173,385` (subsurface_applied, on_set_desync) | A desynchronized child under a synchronized one is effectively synchronized, so its commits are cached; when its parent is applied without a cache of its own (it hasn't committed since), or turns desynchronized, the grandchild's cache stays until the grandchild commits again. wlroots does the same; weston applies the whole subtree. The protocol leaves it open. | A nested tree's innermost content shows a commit late. | Apply the subtree's caches whenever an ancestor is applied as synchronized (weston's rule). | Reported |
| 4 | Low | `user/services/compositor/xdgpopup.c:367` (hide), `:537` (on_popup_grab) | A grabbing popup hidden by a commit with no buffer leaves the chain and forgets its grab, but the popups grabbed on it stay in the chain, and a later grab on it by a child is `invalid_grab`, a protocol error that ends the client, where the protocol keeps that error for a parent that never grabbed (one whose grab is over is "dismissed at once"). | A client hides a menu that has a submenu open, or opens a submenu on a hidden menu. The left-over chain entries can't be shown and any focus change dismisses them: no harm to others. | hide() dismisses what is above it in the chain; a grab on a popup whose grab ended is a dismissal, not an error. | Reported |
| 5 | Low | `user/services/compositor/xdgpopup.c:583` (xdg_popups_turn), `:307` (family_top), `:322` (above) | Each turn walks the stacking order once or twice for every shown popup: with every client at its caps (64 clients, 32 popups each, about 4,000 windows) that is about 10^7 steps a turn. | Needs every client slot; one client's caps make it about 32 x 64 steps. | Keep a count of shown popups, or restack only when the order changed. | Reported |
| 6 | Low | `user/services/compositor/keyboard.c:281` (keyboard_create) | Each new `wl_keyboard` of the focused client is sent an enter whose serial counts as an input event's (`seat_input_serial`): a focused client can make serials for `set_selection` at will. | None beyond what focus already allows (`set_selection` needs the focus anyway); finding 1's fix does not count enters. | Only enters from a focus change count. | Reported |

Not findings (checked and correct):

- **The shim (GS1).** Descriptors are turned into handles before a byte is
  taken and ride the first batch written after them; a call that takes
  nothing gives them back to the caller (closed: they were duplicates), and
  once taken they go with the connection if it ends. A bad header is never
  taken, and every later call stops at it. Batches are read only when the
  last is used up; the header, the handle count and every handle's kind
  are checked, and anything else ends the connection with ECONNRESET, its
  handles closed. The control data is parsed with bounds on every block,
  29 descriptors are EINVAL, a short control buffer gets what fits and
  MSG_CTRUNC. The compositor's pool is passed read, map, inspect and
  transfer only, and a kept VMO never shrinks (the kernel refuses), so the
  pool can't fault under the compositor. Only memfds and pipe and
  socketpair ends cross; the compositor checks a receive's handle is a
  channel end before passing it on.
- **Subsurface trees (GC2).** Every child in a parent's `pend` has a live
  `wl_subsurface` state, and `cur` is always part of `pend`; every way a
  child leaves (its `wl_subsurface` or `wl_surface` destroyed, its parent's
  `wl_surface` destroyed, the client torn down in either order) takes it
  out of both lists at once or marks it inert. Cycles are refused before
  the depth is counted, and the depth cap holds for a subtree brought
  under a new parent, so every walk is bounded by the cap (and each walk
  bounds itself again). The caps are checked against the arrays' sizes
  (`npend` counts the parent too). A cache holds its buffer, so a pool the
  client destroyed stays mapped until the cache is applied or dropped.
  Frame callbacks keep their order through the cache. The pointer's
  surface, the keys' surface and the cursor surface are cleared when their
  surface goes; a subsurface left under the pointer by an implicit grab is
  still a live surface of the same client.
- **Popups and the grab (GC3).** The popup list keeps creation order, so a
  parent always comes before its children and dismissing newest first
  never meets a parent before its child; dismissing only destroys windows,
  so every walk over the list is safe. Every way a popup or its parent goes
  (`xdg_popup` destroyed, its `wl_surface` destroyed, its parent's role or
  surface going, the client torn down) clears the pointers to it and takes
  it out of the chain. The chain is one client's and each popup in it is on
  the one before. A grab needs the toplevel focused and a serial the client
  was sent; the focus moving dismisses the chain, so a client can never
  keep the keys after losing the focus. Escape is the client's (decided):
  the keys reach the grabbing popup and nothing more. A popup is stacked
  above its own family only, never over a window above its toplevel.
  Positions and sizes are held to +-2^20, so no sum overflows.
- **Decorations and geometry (GC1, GC1b).** A decoration object can't
  outlive its toplevel's state (destroying the toplevel first is
  `orphaned`, the `wl_surface` first `defunct_role_object`, both ending
  the client before anything else is dispatched), and its data is the
  toplevel's own `xdg_surf`. Requests newer than an object's version are
  refused by libjwl. The window geometry is clamped to the tree, so a
  geometry far away falls back to the whole tree.
- **Teardown order** (`conn.c`: the clipboard, xdg-shell, surfaces, shm,
  the display, the seat): roles go before their surfaces, a dead client is
  never sent anything, and the focus moves on to a live client's window.

## Design questions for the owner

1. **D1: queued messages to a service are charged up the writer's job
   tree with no limit of the program's own** (`user/services/compositor/conn.c:134`,
   and every service channel; outside these tracks' files). A message
   waiting unread is charged to its writer's job and every job above it,
   up to the root's message budget (1/8 of memory, at most 64 MiB:
   `kernel/proc/userboot.c`). The compositor's ends of its connections are
   not byte bound (`CHANNEL_BYTE_BOUND`, as a pipe's read end is since M13),
   and the programs the shell starts have no `JOB_LIMIT_MSG_BYTES` of their
   own, so a client writing valid JWL2 batches faster than the compositor's
   64 messages a turn reads them can hold 1,024 batches of 16 KiB on each
   connection: four connections fill the root's budget, and every other
   writer in the system then fails with ERR_NO_MEMORY until the compositor
   catches up. The same holds for any other service channel that isn't
   byte bound (up to 1,024 messages of 64 KiB each). Two fixes, not exclusive:
   byte-bound the compositor's ends (a full one is EAGAIN to the shim,
   which libwayland waits out with `poll`), cheap and local; and give the
   jobs of the programs the shell and `/svc/exec` start a message budget of
   their own, which closes it for every service. Recommendation: both, the
   second as its own track.

## Hostile clients against the live compositor

A new group of `tools/wl-port-test.sh`, `hostile`, runs wlprobe's
`hostile-*` cases (real libwayland, through libc's Wayland socket) against
the desktop's own compositor, then checks the desktop still serves a
normal client and the terminal still answers:

| Case | What it does | What must happen |
|---|---|---|
| `hostile-sub-storm` | a toplevel with subsurfaces made and destroyed 1,000 times, trees at every cap at once (32 children, 8 deep, 48 in all), then one more of each | each cap past is `no_memory` on a client of its own; the storm leaves nothing (the next client's window maps and draws) |
| `hostile-desync-flood` | a desynchronized child committing a new buffer 2,000 times with its parent idle and no reads in between | the client stays connected, the parent's frame callbacks still come |
| `hostile-popup-cap` | a grabbing chain 8 deep, then a ninth; 32 popups, then a 33rd | the chain works to its top; each one past a cap is `no_memory` |
| `hostile-deco` | a decoration object whose toplevel is destroyed first; one whose `wl_surface` is | `orphaned`; `defunct_role_object` |
| `hostile-handles` | 28 pipe ends as 28 `wl_shm.create_pool` pools in one flush; 28 eventfds and files in one `sendmsg` | `invalid_fd`, every handle closed; EINVAL from the socket before a byte is sent |
| `hostile-never-reads` | a client that asks for events (round trips, frame callbacks) and never reads | the compositor drops it as too slow; the desktop goes on |
| `hostile-regrab` | finding 1: a grabbing menu, dismissed by the script's click outside, grabbed again at once with the old serial, then with a new keyboard's enter | each new grab dismissed at once; the next click outside reaches the terminal |
