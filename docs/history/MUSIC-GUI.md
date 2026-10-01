# jamjar: the music player's window (design note)

The owner asked for "a GUI version of the music command using mouse and
keyboard, that looks unique and cool, and useful, with some gimmicks as
well". This note is the plan, written before the code.

## What it is

`jamjar` (bin/jamjar, `user/apps/jamjar/`) is a full-screen app on libfun,
started from the shell like `sysmon`. It is a remote control and a view of
the background player (bin/music, `abi/idl/music.idl`), never a second
player: the music keeps playing after it quits, and `music` in the shell
sees the same state. It reads the library itself through the namespace it
is given (fs_readdir), and asks the player to play folders.

**How it gets the player's channel.** The shell already holds a client end
of `music` (SR_USER + 4). A new shell command `jamjar` runs bin/jamjar the
way `run` does (its own job, a PROGRAM console channel, SR_AUDIO, the
namespace) plus a duplicate of that end as SR_USER + 4 (the shell's own
number for it, as mixtest gets init's control channel as SR_USER + 3). No
change to init and no new startup slot in the shell (the shell's spawn
limit, AUDIO-REVIEW design question B, is untouched). `run jamjar` gets no
music channel: it says so and shows the library only. A program the shell
runs any other way gets nothing new.

## The look

Jam OS's own palette (docs/logo/): the dark `#1E1A1D` background, the
berries `#8E1B3A` / `#A9244A` / `#C8284F`, gold `#D9A032`, cream `#F6EFE6`
for text. The logo's seven drupelets appear once, as branding: the mark and
the wordmark "jam**jar**" (`jam` cream, `jar` gold, like the lockup) at the
top left. The rest is built from the same shapes: round ends, discs and
soft glows, anti-aliased (libfun's `disc_aa`, `line_aa`, premultiplied
alpha).

```
+-------------------------------------------------------------------------------+
| (*) jamjar     [/ search ................ ]            shuffle   sleep 30:00   |
+------------------------------------------+------------------------------------+
| ARTISTS        ALBUMS          TRACKS     |  +-----------+                     |
| > JAY-Z        [art] 4-44      Kill Jay Z |  |           |  The Story of O.J.  |
|   Kanye West   [art] Reasonable Doubt  .. |  | jar label |  JAY-Z              |
|   ...          ...             ...        |  |  (art)    |  4-44 . 2017        |
|                                           |  +-----------+                     |
|                                           |  1:23 ====o------------ 3:45       |
|                                           |   |<<   (>)   >>|    vol ===o--    |
+------------------------------------------+------------------------------------+
|~~~~~~~~ the simmer: the jam's surface, shaped by the music ~~~~~~~~~~~~~~~~~~~~|
|::::::::::::: o  bubbles rise where a band jumps; gold seeds drift :::::::::::::|
+-------------------------------------------------------------------------------+
```

Everything is placed by a pure layout function from the screen size and
libfun's UI scale (1 up to 1080 lines, 2 above), so it works at QEMU's
1280x800 and 1280x720 and the PC's 2560x1440: the library takes ~55 % of
the width, now playing the rest, the simmer the bottom ~20 % of the
height. The self-test checks the layout at several sizes: every box on the
screen, nothing overlapping, at least a few rows in each list.

## Useful

- **Now playing**: title, artist and album (and the album's year) from the
  path, tidied for display: `_` becomes a space, the file's ending and a
  leading track number go, a leading `YYYY ` of an album folder is shown
  as the year. Elapsed / length and a progress bar (display only: the
  player can't seek).
- **Controls**: play / pause, stop, previous, next, volume (a slider).
- **The library**: three columns, Artists -> Albums -> Tracks, read from
  the music folder (default `/usb0/music`, else `/data/music`; `root=` to
  choose; the owner's is `/usb0/music/OnTheSpot/<Artist>/<Year
  Album>/<Song>.mp3`). A file's artist and album are the two folders above
  it (fewer folders: "(no artist)" / "(no album)"). It is read a few
  entries a frame, so the window is up at once and fills in.
- **Play** an artist (its folder), an album (its folder), a track (its
  album, starting with that track), or everything (`a`): in shuffle, or in
  name order (`s` toggles).
- **Search**: `/` and type; the three columns keep what matches (an
  artist, album or track whose name, or whose artist's or album's name,
  has the text, case-insensitive; accents folded, so "jay" finds "JAŸ-Z").
- **Help**: `?` shows every key and what the mouse does.

## Gimmicks

(As first built; revision 2, at the end, replaced the simmer with a
spectrum analyser, gave the albums their real covers and took the ring
off the roulette.)

1. **The simmer** (the visualizer): the bottom of the screen is jam in a
   pot. Its surface is a smooth curve through sixteen frequency bands of
   what is heard now (the bass in the middle, the highs out to both
   edges), so the bass heaves and the cymbals ripple; a slow travelling wave keeps it alive between beats; its body
   is a crimson-to-dark gradient with a bright rim. When a band jumps
   (a beat, a snare) a bubble rises from the surface there, wobbles and
   pops; gold seeds drift in the jam. Clicking the jam splashes it.
   Nothing like the logo pulsing: the logo stays still.
2. **Jar labels** (generative album art): each album gets a picture made
   from a hash of its name: a "flavour" (two fruit colours, from
   raspberry, blackberry, blueberry, apricot, plum, gooseberry, cherry,
   fig), a cluster of three to nine berries packed in rings with a
   highlight each, and a flavour name ("Blackberry & Fig"). The same
   album always looks the same; no two neighbours look alike.
3. **Jam roulette** (`r`): a reel of jar labels spins across the screen,
   slows with a tick per label, lands on one album, and plays it.
4. **Sleep timer** (`z`: off, 15, 30, 60, 90 minutes): kept by the
   player, so it works after jamjar quits; the last 30 s fade out, then it
   stops (`music sleep` in the shell too).
5. **Full jar** (`f`): the panels fade away and the jam rises to fill the
   screen, with the title over it.

## The player's protocol additions (music.idl, new ordinals only)

| # | Method | What |
|---|---|---|
| 6 | `prev () -> ()` | more than 3 s into the track heard: it starts over; else the track heard before it (the last 64 are kept); the one skipped back from plays next, then the shuffle goes on. ERR_BAD_STATE: not playing |
| 7 | `play (u8[256] folder, u8[256] first, u8 order) -> (u32 found, u8 reading)` | `start` with options: `first` (a file under the folder, "" none) plays first; `order` 1: name order instead of shuffle |
| 11 | `spectrum ()` | `levels` with 64 bands (revision 2) |
| 8 | `levels () -> (u8 playing, u32 serial, u64 elapsed_ms, u64 length_ms, i32 volume, u32 sleep_s, u8[16] bands, u8 level)` | what a view polls many times a second: the state, a number that changes with each track heard, the times, and the sixteen bands and the loudness of what is heard now |
| 9 | `pause (u8 on) -> (u8 paused)` | stop the mixer stream where it is (the ring keeps its frames), or go on |
| 10 | `sleep (u32 seconds) -> (u32 seconds)` | stop after that long playing, the last 30 s fading out; 0: off |

`status`'s `playing` gets the value 3 for paused (old clients that test
non-zero still read "playing"). The shell's `music` gains `prev`,
`pause` (toggles) and `sleep <minutes>|off`, and `status` shows paused and
the sleep timer.

**The bands**, computed by the player from the samples it already decodes
(`user/services/music/spectrum.c`): a mono mix, a 1024-point FFT (Hann
window) every 512 input frames, the power in sixteen bands log-spaced from
40 Hz to 16 kHz with a +3 dB/octave tilt (music's spectrum falls about
that fast, so all bands move), in dB mapped to 0..255; and the block's RMS
level. Each result is kept in a ring with the stream frame it belongs to,
because the player writes up to 1.37 s ahead: `levels` answers the entry
at the mixer's `played` position, so the picture matches what is heard.
The cost is a few MFLOP a second.

**The app's link thread**: the player answers between chunks, so a call
can wait up to a mixer period (43 ms). jamjar's drawing never waits: one
thread of its own makes every call (`levels` ~30 times a second, `status`
when the track changes, and the commands the UI queues) and publishes a
snapshot under a small lock; the UI draws the latest snapshot and eases
toward it.

## Keys and mouse

| Key | |
|---|---|
| Space | play / pause (stopped: play the selection) |
| Enter | play the selection (artist, album, or track in its album) |
| n / . | next track; p / , previous |
| x | stop |
| + / - | volume up / down 2 dB |
| arrows, Tab | move in the lists; left / right or Tab between columns |
| PgUp, PgDn, Home | page, top |
| / | search (Backspace edits, Enter plays, Esc clears) |
| a | play everything; s shuffle or name order |
| r | jam roulette; z sleep timer; f full jar; l show the track playing |
| ? or h | help; q / Esc quit (Esc first closes help, search, roulette) |

Mouse: click selects, double-click plays, the wheel scrolls the list under
it (over the volume: changes it), the transport buttons click, the volume
knob drags, a click on the jam splashes it, a click on the art spins the
roulette.

## Text and Unicode

The console font has ASCII only. libfun's text gets the Latin-1 and
Latin Extended-A glyphs of the same font (Spleen has U+00A0-U+017F), so
`JAŸ-Z` and `Fünf` draw as written; any other code point (and a malformed
byte) draws one fallback box per character, never one per byte.
`tools/bdf2c.py` writes the extra table.

## Tests

- `run jamjar --selftest`: the library model on a fixture of tricky names
  (UTF-8 `JAŸ-Z` and `¥$`, `_` names, numbers, years, files at every
  depth), the title tidying, the search, the UTF-8 decoding, the jar-label
  hash (stable, different for different albums), the layout at
  1280x720, 1280x800, 1920x1080, 2560x1440 and a narrow screen, and
  drawing a whole frame at each size.
- utest: the player's band analysis (sine tones land in their band, the
  ring answers by stream position) and its history for `prev`.
- `tools/jamjar-test.sh` (QEMU, a USB keyboard and mouse, an hda-output
  capture): the self-test; jamjar on a made-up library on `/data/music`,
  screenshots at 1280x800 and 2560x1440; play an album by keys, next,
  prev, pause, volume, search, the roulette, help, full jar, mouse clicks
  on a track and a button; `trace` logs each action and the bands it
  reads, so the log shows the tone heard lands in the right band; quit,
  and the music is still playing (`music status`).
- `tools/music-test.sh` still passes, with `music prev`, `pause` and
  `sleep` added to its script.
- `make check`, ktest at 4 CPUs, the init run.

What only the PC can show: how smooth it is at 2560x1440 (no vsync), the
mouse feel, the bands against real music, the owner's 177-file library.

## As built (2026-10-01, branch `music-gui`)

- As planned, with these differences: the bands are mirrored across the
  jam (the bass in the middle); a line under the controls says which
  folder plays and the main keys, where there is room; the full jar
  shows the album's label and the names over the jam; the shell's `music
  sleep` also takes seconds (`music sleep 40s`, for the test).
- QEMU (TCG, 4 CPUs): a whole frame drawn and presented in about 17 ms
  at 1280x800 (45-50 frames a second) and 48 ms at 2560x1440; the self-test draws one in 18 ms
  at 1280x800 and 112 ms at 2560x1440 (one CPU). The background and the
  jam's body are drawn on every CPU of the pool. jamjar says its frame
  cost when it quits (`jamjar: N frames in S s, U us each`).
- The test library is written by mtools, which gives a name that fits
  8.3 in a DOS code page (`¥$`) a short name only, in code page 850 by
  default; FatFs reads short names as 437, so the yen came back as
  another character (drawn, rightly, as one box). The test sets mtools
  to 437. Whether macOS gives such a name a long name on the owner's
  stick is to be seen on the PC.

## Revision 2 (the owner, after the first look on the PC)

The owner liked it, and asked for three changes.

**Real covers.** Every one of his MP3s carries its album's cover in its
ID3v2 tag (an APIC frame; PNG, about 640x640). An album now shows that
cover wherever its picture appears (the albums column, now playing, the
big view, the roulette), and the jar label stays as what an album shows
without one, or while its cover is read.
- `id3.c` finds the picture: ID3v2.2 (PIC), 2.3 and 2.4, unsynchronised
  tags and frames, a data length indicator, the text encodings of the
  description, the front cover chosen over other pictures, compressed or
  encrypted frames passed over; every length checked (the file is
  untrusted). The image's kind comes from its own first bytes.
- `stbi.c` decodes it: stb_image v2.30 (third_party/stb_image, public
  domain or MIT), PNG and JPEG only, after `stbi_info` has said its size
  (2048x1600 at most), with all of its memory from one 40 MiB arena that
  is emptied after each picture; a picture that needs more fails.
- `cover.c` does it on a thread of its own: the UI asks for every album
  it draws (the newest asked first, so what is on the screen comes
  first), and for every album of the library once, behind those. It
  reads the tag (6 MiB at most), crops the picture to its middle square,
  scales it down by area averaging in premultiplied alpha to 256 px
  (kept for 128 albums, the least recently drawn going first) and, for
  an album drawn bigger, to 512 px (kept for 2). art.c keeps each size
  drawn, with rounded corners.

**A spectrum analyser instead of the simmer.** The jam, its bubbles,
seeds and splashes are gone; `bars.c` draws 64 bars across the width, log
spaced from 40 Hz to 16 kHz (the player's new `spectrum` call, 64 bands
of a sixth of an octave from a 2048-point FFT; bands narrower than a bin,
below about 300 Hz, are read between the two nearest bins). Each bar
blends a quarter of each neighbour, gets a bounded per-band gain from its
long average (so the top octave moves), rises fast and falls at a steady
1.25 heights a second; a cream cap holds 0.5 s, then falls faster and
faster; with nothing heard everything falls to zero. Rounded tops, a gap,
a crimson-to-gold gradient (gold from 80 % of the full height), a faint
reflection, drawn across the thread
pool. `f` is now the big view: the cover and the names over the bars,
which take the lower 70 % of the screen.

**The roulette lands without a ring** round the album it chose: it lands,
shows the name, and plays.
