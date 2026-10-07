# G1 sign-off on the PC

The desktop's last check (G1-PLAN's X1 and the PC's part): about 40
minutes on the PC at 2560x1440. Tick each line; anything that looks wrong
goes to the main session with what you did and what you saw (the netlog
has the rest).

Before you start: `update`, then `reboot`, so the PC runs the build being
signed off. `version` says which one.

## 1. Hostile programs against the live desktop (about 5 minutes)

Set the desktop up as you use it: Jamjar playing a track, two terminals
(Super+Enter for the second). In one terminal:

- [ ] `utest only comp_live` ends with `utest: 4 passed`. Meanwhile it
      connects to the compositor and does everything a broken or nasty
      program could: bad requests of every kind, every per-program limit,
      a program that floods requests and never reads, programs that crash
      holding windows (one between drawing and showing). The music keeps
      playing, the other windows stay, the pointer and keys keep working.
- [ ] `kill compositor`: the screen goes and comes back within a few
      seconds with both terminals and Jamjar, each still working (type in
      both terminals; Jamjar's controls answer; the music never stopped).
- [ ] `run utest only comp_live &` then keep typing in the other terminal
      while it runs: no stalls.

## 2. The desktop, by hand (about 20 minutes)

Boot and splash:
- [ ] The splash is the first thing on the screen (no flash of the
      desktop first), plays with its sound, and gives way to the desktop.
- [ ] No notice shows while the splash is up; "Connected" (and any other)
      appears once the desktop is there.

Tiling (the default) and windows:
- [ ] Three terminals and Jamjar tile with gaps, no title bars; the
      focused one has the lit border.
- [ ] Super+H/J/K/L and Super+arrows move the focus.
- [ ] Super+Shift+direction swaps; Super+drag one onto another swaps.
- [ ] Dragging a gap resizes (it lights up, resize cursor);
      Super+Alt+direction does it from the keys.
- [ ] Tiles glide to their new places when a window opens or closes.
- [ ] Super+T: the screen turns floating: title bars with the three
      circles (raspberry close, apricot minimise, blackcurrant full
      screen), hover symbols in them, rounded corners and shadows.
- [ ] Floating: drag a title bar to move; drag an edge or corner to
      resize; Super+drag moves from anywhere; Super+right-drag resizes.
- [ ] Double-click a title bar: full screen; again: back.
- [ ] Minimise (circle or Super+M): the window shrinks into its chip in
      the top bar; clicking the chip brings it back.
- [ ] Super+Q closes the focused window; the open and close animations
      look smooth.

Screens:
- [ ] Super+Ctrl+Right past the last screen makes a new one (slide
      animation); an empty one disappears when you leave it.
- [ ] Super+1..9 jumps; Super+Shift+N moves the focused window there.
- [ ] Super+F (or the full-screen circle): the window gets a screen of
      its own; again: back where it was.

The top bar and search:
- [ ] The strip is frosted (blurred wallpaper), windows never go under
      it; the clock is right (Sydney time).
- [ ] Super tapped alone, or the Jam OS button: search opens; typing
      finds Terminal and Jamjar; Enter starts it.
- [ ] Alt+Tab: this screen's windows first, then the other screens',
      then minimised ones; each Tab moves on; letting go of Alt goes
      there; Esc cancels.
- [ ] The volume popover: its slider changes the volume, the percentage
      has room at 100%; the network one shows the address; the clock one
      the date. Each sits just under the strip, right edges aligned with
      their icons, dividers inset.
- [ ] `notify -b Yes -b No -w Tea? Kettle is on` shows a card with two
      buttons; pressing one ends the command with that answer.

Terminal and clipboard:
- [ ] The prompt reads `jam:/>` (`jam:/data/music>` after `cd /data/music`).
- [ ] Drag to select (double-click a word, triple-click a line);
      Super+C copies; Super+V in another terminal pastes; a pasted
      multi-line command does not run line by line.
- [ ] `jamjar &` opens Jamjar in the background; `jobs` lists it.
- [ ] Resizing a terminal sideways doesn't flash.

Cursors:
- [ ] The arrow is black with a white outline; the text bar over a
      terminal; resize arrows over gaps and edges; the busy ring while
      something starts.

## 3. The tests (about 15 minutes)

- [ ] Boot "Developer > All tests": `ktest: N passed`, "run complete: no
      problems" (the stress line marked "faked on purpose" is expected).
- [ ] Boot "Developer > Soak test" with `soak=10` typed into its line
      (Limine's E), a second stick mounted read-write and pulled/replugged
      once: `soak: PASSED`.

When every box is ticked, G1 is signed off: the main session marks it
done in ROADMAP.md.
