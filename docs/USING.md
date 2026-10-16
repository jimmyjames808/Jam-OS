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
  port and netlog, and what matters to a user comes as a notice.
- The boot menu: "Jam OS" (the desktop), "Jam OS (previous build)" (the
  stick's build before the last `update` or `make flash`: the way back
  from a bad build), and a "Developer" folder with the test and network-test
  entries, the text log, safe mode, "Jam OS (no IOMMU)" and "Jam OS
  (internal disks read-only)" (the boot word `diskro`: every NVMe drive is
  treated as someone else's, so nothing writes one, and a reboot keeps it;
  the way out if a build's write path misbehaves)
  ([TESTING.md](TESTING.md#the-boot-menu)).
- The test entries and the Mac: "Developer > Jam OS (init + utest +
  usbtest, the regression run)" now sends its log to the Mac like a plain
  boot, and saves `/data/logs/boot-NNNN.txt`, when the stick's settings
  have `net.host` (and not `netlog = off`) and the boot has the network:
  leave `python3 tools/netlog-recv.py ~/jamos-logs` running on the Mac
  ([NETWORK.md](NETWORK.md#the-log-on-the-mac)). The file ends at init's
  line `init: the run is over`: every program's `init: bin/... exited with
  code N` line and utest's, usbtest's and posixtest's results are above
  it; the kernel's own last lines (`init: exited with code`, the RESULTS
  box) are on the screen only ([TESTING.md](TESTING.md#the-regression-runs-network)).
  The Soak entry is a plain boot, so its log reaches the Mac too. "All
  tests", the stress tests and "Benchmark" are kernel only: no user space
  runs, so their results are on the screen and the serial port only
  (`ktest`, `stress` and `bench` in a plain boot's shell do reach the Mac).

## The desktop

A compositor of our own that speaks Wayland draws the screen (G1:
[HISTORY: G1](HISTORY.md#g1-the-desktop), [ARCHITECTURE.md](../ARCHITECTURE.md#graphics)),
and the shell runs in a terminal window on the jam wallpaper. Super+Enter
(or `term`) opens another terminal, up to 16 (one more is refused with a
"No more terminals" notice); apps open windows of their own
(`jamjar &`).

**Tiling and floating.** Windows tile by default (each new one halves the
focused tile along its longer side; drag the gap between two to resize
them, or Super+Alt+arrow; a gap stops where a tile would get smaller than
200x120 or than its window's own minimum; a window that would get a tile too small, under 200x120 or under
its own minimum, such as Jamjar's 944x568, opens on a new screen of its
own instead, and when it closes the view comes back) or float (moved by their title bar and raised with the mouse, with
the three jam circles on the left of the title bar: raspberry closes,
apricot minimises, blackcurrant makes the window full screen; a
double-click on the title bar does too). Super+T switches the screen, and
the choice is kept in the settings (`display.layout = tiling`, the
default, or `floating`); from tiling to floating every window stays where
its tile was. Each virtual screen keeps its own arrangement. If the
compositor itself ends (`kill compositor`, a crash), init starts it again
and the windows come back where they were: the same screens, tiles and
sizes, floating places, minimised ones still minimised, the same screen
shown and the keys with the window that had them (a window that isn't
back within 5 s gives up its place).

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

**Notices** come as cards in the top right (after the boot splash,
never over it), in plain words: a stick added or removed, the Jam OS stick pulled out and back, the network connected
or lost, a service or driver that crashed (and was started again) or kept
crashing, an update written (with a Reboot button), a restart after a
panic (with Details: `crashlog` in a new terminal), or the shell's
`notify -b Yes -b No -w Tea? The kettle is on`.

**Terminals.** Every terminal is equal: any of them, the first too,
closes with its close circle, Super+Q or `exit`; with none left the
desktop shows its wallpaper and top bar, and Super+Enter or the search
box opens one. At most 16 are open at once; each is a console and a
shell, a few MiB (more for a big window). The terminals' text is
JetBrains Mono (`terminal.font =
bitmap` in the settings gives the 8x16 bitmap font instead). The desktop
is the only way Jam OS draws: if a build misbehaves, "Jam OS (previous
build)" boots the one before it.

## The shell

- A console and a shell with over 90 commands, pipes, variables, Tab
  completion and file commands (`ls cat cp mv rm mkdir write df mount`).
  Its prompt says where it is: `jam:/>` at the start, `jam:/data/music>`
  after a `cd`. `help` lists the everyday commands, `help dev` the
  developer ones (tests, hardware, the kernel), `help <command>` any one.
- A command line holds up to 240 characters in a terminal of any width.
  A line longer than the terminal scrolls sideways on its row to keep the
  cursor in view, with `<` or `>` where text is cut off; Enter shows it
  whole. The arrows, Home and End, Backspace and Delete, Ctrl+A/E/U, Tab
  completion, the history (up and down) and a paste all work on it.
- The history is every terminal's, kept in `/data/etc/history` (the last
  500 lines; a new terminal starts with them, and they outlive a reboot
  or a shell's crash). With text on the line, Up recalls only the lines
  that start with it, Down goes forward and back to what was typed; on an
  empty line Up steps through them all. A line typed with a leading space
  isn't kept; `history` lists them, `history -c` forgets them.
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

`/tmp` is always there and empty at every boot: the Jam OS drive's
`/jam/tmp` when it is in, the drive's own `/data/tmp` when the PC started
from it, else a volume in memory. Every POSIX program (the ports, Chromium)
may write it; native programs get only what their lists name.

**Internal NVMe drives.** Jam OS binds an NVMe drive's driver on every
boot (M12.7). The drive's FAT partitions (a Windows drive's EFI system
partition) appear read-only at `/nvme0`, `/nvme1`, ..., apart from the
sticks' `/usbN`; the others (Windows' own NTFS, its reserved and recovery
partitions) are listed and left alone. A drive is written only if it is a
**Jam OS disk**: GPT with exactly two partitions, "Jam OS ESP" (the ESP's
type) and "Jam OS data" (Jam OS's own type), which `disk prepare` makes
and nothing else does. Windows' drive is not one, and the driver (which
checks the table itself, and writes no other drive unless you erase it
with `disk prepare`), volumes (a second check of its own) and the shell
all refuse: `mount -w /nvme0` says "not on a Jam OS disk". Drives whose
blocks are not 512 bytes are listed and not mounted (the FAT service reads
512 only).

- `disks` lists every disk, a stick or a drive, by the name the other
  commands use (`nvme0`, `usb0`): its model, serial number, firmware and
  size, its partition table (GPT, MBR, or none), whether it is the Jam OS
  disk, and under each its partitions: type (by name where known, else the
  GUID), size, name and where each is mounted (or "no FAT volume" for one
  that holds none). A drive that hasn't answered yet says so.
- `disk digest nvme0` prints the SHA-256 of a list of regions of the whole
  disk, computed in the driver (the data never leaves it): the first 34
  blocks, each partition under 1 GiB whole, a bigger one's first and last
  64 MiB and 32 samples of 1 MiB, and the last 33 blocks, one line each
  (`part1 lba 2048 count 204800 sha256 ...`), ending with `disk digest:
  done`. `tools/disk-regions.py` makes the same list on Linux from the
  partition table alone, so diffing the two shows Jam OS read every region
  right, and that Windows' disk didn't change across a session. The lines
  are in the boot log too (`/data/logs`, in the form `disk digest: nvme0
  part1 2048+204800 <hash>`). `disk digest nvme0 <lba> <count>` hashes one
  range (at most 1 GiB).
- `disk prepare nvme1` makes an NVMe drive a Jam OS disk. It shows the
  drive's model, serial number and size, and goes on only when you type the
  last four characters of the serial number (any case; anything else stops,
  and so does Ctrl+C): then it writes the partition table (an ESP of 512
  MiB at 1 MiB, a data partition of the rest, at most 2 TiB), formats both
  partitions and mounts them: the ESP FAT32, read-only (`/nvme1`: the
  next free number), the data partition ext, read-write, as `/jam` (with
  `/usr`, `/var`, `/home` and `/tmp` as views of it), and says `nvme1 is a
  Jam OS disk now: ESP 512.0 MiB, data ...`.
  A drive that isn't empty (anything at its first or last 64 KiB, or in
  the first MiB at 1 MiB or at 513 MiB) is **erased**, and only if you
  say so twice: it says "It is NOT empty", lists what is on it (its
  partition table, each partition, the FAT volumes mounted from it), says
  everything on it will be lost, and asks for the serial's last four
  characters and then for `erase`; anything else stops, and nothing is
  written. Check the serial: Windows' drive is shown the same way, and
  erasing it loses Windows. It refuses the disk Jam OS booted from, a
  drive with a volume mounted read-write, every drive on a `diskro` boot,
  and a drive already prepared ("a Jam OS disk already"); an erase is
  never confirmed in a command run over SSH (`ssh pc 'disk prepare ...'`).
  A prepare that was cut short (the power) is finished by running it
  again: whatever it left is a blank drive, a drive with only half a
  table, or a Jam OS disk with a partition that has no volume yet (an
  erase cut short leaves a drive that isn't empty: it asks again). A Jam
  OS disk whose partition table Jam OS reads only from its backup (a
  header damaged) can't be written: its volumes are mounted read-only and
  `disk prepare` offers it for erase like a drive that isn't empty.
- `disk format nvme1 data` formats a Jam OS disk's data partition again,
  **everything on it lost**: an ext volume (a FAT32 one the first `disk
  prepare`s made is turned into ext this way); `disk format nvme1 data
  fat` makes FAT32 instead (for a drive another computer will read). It
  shows the drive and what its data partition holds and goes on only when
  you type the serial's last four characters; the partition's mount
  (`/jam` and its views too) goes first. Not over SSH.
- `install nvme1` writes Jam OS to an NVMe drive so the PC starts from it
  without the stick. It asks the same questions as `disk prepare` (the
  drive's model, serial and size; the serial's last four characters; for a
  drive that isn't empty what is on it and `erase`), prepares the drive
  that way if it isn't a Jam OS disk, then writes Limine, the build this
  boot runs (the stick's kernel and boot image, twice: the second pair is
  the "previous build" entry), `/data/etc` (settings, the update key's
  trust, the network address, the host key) and, last, the boot menu, each
  file read back and compared before the next. Until the menu is written
  the firmware can't start the disk, so a cut at any point never leaves a
  half build bootable: run `install` again to finish. A Jam OS disk can
  be installed on again (`disk prepare` refuses it): its ESP's files are
  replaced, its `/data` kept as it is (`/etc` is copied only to a disk that
  has none). It ends with `Jam OS <version> installed on nvme1: choose
  "UEFI OS (<model>)" (or the drive's name) first in the firmware's boot
  order`; Jam OS writes no firmware variable, so you set the order (Del at
  power-on; F8 picks a drive once). The stick keeps working as a stick:
  picked with F8 it boots itself and mounts the installed drive's ESP
  read-only and its data partition as `/jam`, read-write. Booted from the
  drive, its data partition is `/data` (ext).
  It refuses the disk Jam OS booted from and a drive on a `diskro` boot.
- `disk smart nvme0` prints the drive's own health log: data read and
  written, host read and write commands, power cycles, unsafe shutdowns,
  the percentage of its life used, the critical warning. "Host write
  commands" not moving across a session is the drive's witness that nothing
  wrote to it.

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
  ([HISTORY.md, audio](HISTORY.md#audio-a1-a2-as-and-m85-kexec)).

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
