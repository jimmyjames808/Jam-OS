# Using Jam OS

What Jam OS does today, as someone at the PC sees it: the desktop, the
shell, files, sound and settings. The network has a page of its own
([NETWORK.md](NETWORK.md)); the design behind all of it is in
[ARCHITECTURE.md](../ARCHITECTURE.md), and the boot menu's entries and
the test commands are in [TESTING.md](TESTING.md).

## Starting up

- UEFI boot from a USB stick (via Limine) on a real PC with 28 CPUs, and
  in QEMU ([HARDWARE.md](HARDWARE.md) has the PC and how to flash the
  stick).
- A boot splash: the logo animation with its sound while Jam OS starts
  (it plays to the end, and what is typed meanwhile reaches the shell).
  The boot menu's "Developer > Jam OS (text log, no splash)" (the boot
  word `verbose`) shows the text log instead.
- A quiet boot: the first terminal starts at the shell's banner and
  prompt on every boot but the Developer folder's text-log entry
  (`verbose`); the log stays in `log`, `dmesg`, `/data/logs`, the serial
  port and netlog, and what matters to a user comes as a notice. Without
  the compositor (`nocomp`) the full-screen console is just as quiet and
  shows those notices as short yellow lines.
- The boot menu: "Jam OS" (the desktop), "Jam OS (previous build)" (the
  stick's build before the last `update` or `make flash`), "Jam OS (no
  compositor)" (`nocomp`: the full-screen console, until G1 is signed off
  on the PC), and a "Developer" folder with the test and network-test
  entries, the text log, safe mode and "Jam OS (no IOMMU)"
  ([TESTING.md](TESTING.md#the-boot-menu)).

## The desktop

A compositor of our own that speaks Wayland draws the screen (G1:
[G1-PLAN.md](G1-PLAN.md), [ARCHITECTURE.md](../ARCHITECTURE.md#graphics)),
and the shell runs in a terminal window on the jam wallpaper. Super+Enter
(or `term`) opens another terminal, up to eight; apps open windows of
their own (`jamjar &`).

**Tiling and floating.** Windows tile by default (each new one halves the
focused tile along its longer side; drag the gap between two to resize
them) or float (moved by their title bar and raised with the mouse, with
the three jam circles on the left of the title bar: raspberry closes,
apricot minimises, blackcurrant makes the window full screen; a
double-click on the title bar does too). Super+T switches the screen, and
the choice is kept in the settings (`display.layout = tiling`, the
default, or `floating`). Each virtual screen keeps its own arrangement.

**The keys.** Keys go to the focused window only, but for the window keys
(Super is the logo key; a direction is H/J/K/L or an arrow):

| Keys | Do |
|---|---|
| Super+direction | focus the nearest window that way |
| Super+Shift+direction | tiling: swap with that neighbour |
| Super+Alt+direction | tiling: push the tile's edge that way; floating: grow or shrink |
| Super+Q, Super+M, Super+F | close, minimise, full screen (and back) |
| Super+T | this screen floating or tiling |
| Super+drag, Super+right-drag | move (floating) or swap (tiling); resize (floating) |
| Super+1..9, Super+Shift+1..9 | go to screen N; move the focused window there |
| Super+Ctrl+Left/Right (or H/L) | the screen before or after (past the last: a new one) |
| Super+Ctrl+Shift+the same | move the focused window to that screen |
| Super alone, Super+Enter | search; a new terminal |
| Alt+Tab, Alt+Shift+Tab | the switcher (hold Alt; Esc cancels) |
| Ctrl+Alt+Del | reboot |

**Copy and paste in the terminals.** Drag with the mouse to select text
(a double click takes a word, a triple click a line; a click or a key
clears it), then Super+C (or Ctrl+Shift+C) copies it and Super+V (or
Ctrl+Shift+V) pastes into the terminal with the keys. Ctrl+C still stops
a program. A paste goes onto the shell's line, line breaks as spaces, and
runs only when you press Enter. Pasting outside the terminals (the search
box, Jamjar's search) is not there yet.

**The search box** (Super alone, or the top bar's "Jam OS") starts
Terminal or Jamjar, or runs what is typed in a new terminal (`term
<command>` does too); init starts only the desktop's own apps for it.

**The top bar** shows the screens (a dot each, "+" for a new one), the
current screen's windows (a click focuses, minimises or brings one back),
the floating/tiling switch, and the network, volume and clock icons,
which open popovers: the volume popover sets the mixer's master volume
and shows the output and what plays; the network popover the link,
address and rates; the clock popover the date and a month's calendar.

**Notices** come as cards in the top right, in plain words: a stick added
or removed, the Jam OS stick pulled out and back, the network connected
or lost, a service or driver that crashed (and was started again) or kept
crashing, an update written (with a Reboot button), a restart after a
panic (with Details: `crashlog` in a new terminal), or the shell's
`notify -b Yes -b No -w Tea? The kettle is on`.

**Terminals.** Every terminal is equal: any of them, the first too,
closes with its close circle, Super+Q or `exit`; with none left the
desktop shows its wallpaper and top bar, and Super+Enter or the search
box opens one. The terminals' text is JetBrains Mono (`terminal.font =
bitmap` in the settings gives the 8x16 bitmap font instead). The boot
entry "Jam OS (no compositor)" (the boot word `nocomp`) is the way back
to the full-screen console until G1 is signed off on the PC.

## The shell

- A console and a shell with over 90 commands, pipes, variables, Tab
  completion and file commands (`ls cat cp mv rm mkdir write df mount`).
  Its prompt says where it is: `jam:/>` at the start, `jam:/data/music>`
  after a `cd`. `help` lists the everyday commands, `help dev` the
  developer ones (tests, hardware, the kernel), `help <command>` any one.
- Programs in the background: `run prog args &` (or `prog &`) gives the
  prompt back at once; `jobs` lists them, `kill %2` ends one, and the next
  prompt says when one ends (`[2] done: utest (exit 0)`). Such a program
  gets none of the terminal's keys (Ctrl+C is the foreground program's);
  what it prints is shown as it comes. At most 8 at once; they end with
  the shell. A pipeline, a shell command or an alias can't go in the
  background yet. An app that draws opens a window of its own, in the
  background too (`jamjar &`).
- Each program gets only what it asks for: a list in its own file (the
  services under `/svc` and the mounts it wants, read-only or writable).
  A program copied to `/data` runs once the owner has said yes to its list
  with `allow`.
- `version` names the git commit a build was made from; `uname -a` the
  version, the machine and the CPU.
- `kernel load` loads a freshly flashed kernel from the stick while Jam OS
  runs, so the next `reboot` is instant.

## Files and sticks

USB sticks work through a usb-storage driver and a FAT32 service (FatFs)
per volume. The stick Jam OS boots from has its boot partition at `/esp`,
read-only, and a data partition at `/data`; any other stick shows up
read-only at `/usb0`, `/usb1`, ... and `mount -w` makes it writable. Each
boot's kernel log is saved to `/data/logs/` and can be read on another
computer; after a panic the next boot saves the panicked boot's log too,
and `crashlog` shows it ([ARCHITECTURE.md](../ARCHITECTURE.md#storage)).

**Services that outlive their process.** The FAT32 service and the sound
mixer keep their state and their clients' channels outside the process,
so when one is killed or crashes a waiting spare carries on where it
stopped and programs see nothing: no error, no lost write, no gap in the
sound. `storm /usb0/big.bin /data/big.bin 10` shows it: it copies a file
while the filesystem services of both sides are killed ten times a
second, then prints the throughput, the kills, kill-to-first-answer
(median, p99, worst) and both files' SHA-256 (MATCH or DIFFERENT); `storm
mixer 2 60` kills the mixer while music plays. `tools/fatcheck.py` checks
the stick's FAT32 afterwards on the Mac. (Built and tested in QEMU, where
a 32 MiB copy at 100 kills a second matches; not signed off on the PC
yet: [ARCHITECTURE.md](../ARCHITECTURE.md#services-that-outlive-their-process).)

## Sound and music

- `beep`, and `play /data/song.wav` plays a PCM WAV file (8- to 32-bit,
  mono or stereo, any common rate) in the headphones; `play
  /data/song.mp3` plays an MP3 (CBR or VBR, ID3 tags skipped, decoded by
  dr_mp3) copied straight from the Mac.
- A mixer service plays several programs at once; `vol` sets their
  volumes (and the top bar's volume popover the master volume).
- `music start` plays a folder of MP3s and WAVs in shuffle in the
  background while the shell goes on.
- Jamjar is the same player in a window (`jamjar &`, or from the search
  box): the library by artist and album (read from `/usb0/music` if a
  stick has one, else `/data/music`, laid out Artist/Album/Song.mp3),
  search, the controls with keys and the mouse, a sleep timer, the albums'
  own covers (read from the MP3s' tags), stereo spectrum bars across the
  bottom of the window (left up, right down) and a sunburst in the
  full-screen view. The music goes on after Jamjar quits
  ([history/MUSIC-GUI.md](history/MUSIC-GUI.md)).

## The clock and the settings

The real date and time: the PC's real-time clock read at boot, kept in
UTC by the kernel and shown in the owner's time zone (Sydney, with
daylight time; `date -z` changes it); with the network up, the clock is
set from it ([NETWORK.md](NETWORK.md#the-clock-from-the-network)). Files
on the sticks and the boot logs are dated. Settings that survive a reboot
live in `/data/etc/settings`: the zone, the volumes, the music folder,
the windows' layout (`display.layout`), the terminals' font
(`terminal.font`), and the network's keys
([NETWORK.md](NETWORK.md#settings);
[ARCHITECTURE.md](../ARCHITECTURE.md#time-and-settings)).
