# AS: the boot splash

Status: done, signed off on the PC on 2026-10-01 with A1, A2 and M8.5:
the 2560x1440 version, its sound starting with the picture.

On a plain boot the owner's logo animation plays with its sound while
Jam OS starts: the seven drupelets drop in and assemble into the logo,
the gold one glows, the logo slides left and "Jam OS" types out. The
whole animation (8.5 s since the owner's 2560x1440 render) plays every
boot; the last frame stays at least 0.5 s, and until the shell is ready;
then the picture fades into the console's text. Nothing in the code or
the tests assumes the animation's length, size or rate: they come from
the file.

## The video: `boot/splash.mpg`

The animation is made outside the repository (by the owner, in
`~/Movies/motion-graphics/jamos-boot/`: `jamos-boot.mov`, ProRes 4444
with alpha, rendered at 2560x1440, 30 fps, 8.5 s, and `jamos-boot.wav`,
48 kHz stereo). `tools/mksplash.sh` turns them into one MPEG-1 program
stream: the video at **2560x1440, the PC's own resolution** (a source of
another size is scaled to fit with Lanczos), its own frame rate,
composited over the background `#1E1A1D`, `-q:v 2` (quantiser 1-2), no
B-frames; the sound as MP2, 48 kHz stereo, 192 kb/s. The result, 1.71 MB
(1,705,984 bytes), is committed, so a build never needs the owner's
files; `make` regenerates it only when both are there (`SPLASH_SRC`, by
default that folder) and one is newer than it. It goes into bootfs as
`splash.mpg`.

Quality (frame 250, against the source composited losslessly): the first
version (1280x720 at q4, drawn 2x, nearest chroma) was 37.9 dB PSNR, with
stair-stepped edges and soft text; the native 1440p file drawn 1:1 with
interpolated chroma is 48.5 dB, edges and the pixel font as rendered.
MPEG-1's 4:2:0 halves the colour resolution; at 1440p a colour edge
(red on the dark background) is then soft over 2 pixels, which the
chroma interpolation keeps smooth; no softening in the conversion was
needed.

The decoder is pl_mpeg (`third_party/pl_mpeg`, MIT, one header), built
into `bin/splash` only, without stdio: it reads the file straight from
the bootfs mapping.

## Boot order: what the owner sees

1. **The kernel** (`kernel/main.c`, `kernel/dev/fbcon.c`): on a plain boot
   (no mode word, or `shell`) without `verbose`, `nosplash`, `nousb` or
   `soak`, fbcon is quiet: its first act fills the screen with `#1E1A1D`
   (the video's first frame is that colour, empty) and it draws no text
   from then on; the log still goes to the ring and the serial port. A
   panic draws over it as always (`fbcon_force_unlock` clears quiet); so
   does a console that dies (the kernel draws the log again). init gets a
   second argument, `splash`.
2. **init** (`user/services/init/shell.c`, `splash.c`): bootfs server,
   then the console with the argument `quiet` (it draws nothing until a
   lent screen comes back, or for 5 s if nobody borrows it), then
   **the splash**, then serialin, devmgr, the mixer, the music player,
   logd. The shell waits:
   it starts once the splash says `SPLASH_PLAYED` (or ends).
3. **The splash** (`user/apps/splash`) borrows the screen and the keys
   from the console (a PROGRAM-level console channel, `gfx_open_on`: the
   screen stays `#1E1A1D`) and shows the first frame (dark too); a thread
   decodes the MP2 track whole and opens a stream on the mixer
   (`SR_AUDIO`), which answers once devmgr has bound the hda driver and
   init has started the mixer. The first frame waits for that stream, at
   most `SOUND_WAIT` (2 s) after the splash started, so **picture and
   sound start together** and the sound plays as authored from its first
   sample (the owner's sound hits near full scale 0.1 s in: joining in
   the middle of it was the "punchy" start of the first version). If the
   stream is later than that, the animation starts silently and the
   sound joins where the animation is, faded in over 25 ms (a raised
   cosine), never with a step.
4. When the animation ends (or a key skips it) the splash tells init
   `SPLASH_PLAYED`; init starts the shell; the shell, once its banner is
   written, calls `initctl.shell_ready`; init sends `SPLASH_GO`
   (`<splash.h>`). The last frame stays until then, and at least
   `LINGER` (0.5 s; it was 2 s, too long on the PC) after the end of an animation that played out (a skip
   wants it gone: no linger); 30 s at most for a shell that never comes.
   The splash then fades the last frame into the console's background
   (8 steps of 30 ms, premultiplied alpha) and closes the lease; the
   console draws its text, the shell's prompt at the bottom.

Keys: the splash has the only key focus while it plays (the shell starts
after it), so a key typed then goes to the splash, never to the shell.
Any key skips the rest: the sound fades out over 5 ms.

## Sync: the sound is the clock

Media time is ns from the first frame; frame n is due at n/30 s, the
sound's frame f at f/48000 s. The file's time stamps line them up: the
MP2 decoder's 481-sample delay is in the stamps (the sound's start 10 ms
before the video's), and `sound_decode` drops those frames, so the sound
is where the owner's WAV had it.

- Started together (the usual case): media time stays 0 until the
  sound's first frame is heard (half a second at most), then it is the
  sound's position from the first frame on: exact from the start.
- A late join (the stream opened after `SOUND_WAIT`): until the sound is
  heard, media time is the timer; the sound starts at the media time
  then plus a guess of how long until it is heard (`HEARD_IN`, 50 ms: at
  boot the splash's stream is the only one, and the mixer opens the
  output with its first frames first).
- Once the mixer's `stream_position` says frames are heard (`played`,
  which the mixer interpolates from the driver's position), media time is
  the frame being heard: the position is asked every 20 ms (or after each
  write, at most a mixer period apart) and moved on by the timer in between. The
  log says how far the timer was off at the switch (one jump of the video,
  that once).
- The video shows a frame when the clock reaches it; a frame whose next
  frame is already due is dropped (decoded, not drawn) unless nothing
  has been shown for 100 ms (a machine too slow to decode every frame in
  time, like QEMU, still shows ten a second); none is early.

## Drawing

`video.c` decides where the frame goes; `draw.c` converts its YCbCr
4:2:0 (studio-range BT.601, fixed point) into libfun's back buffer, in
bands of 16 rows on the thread pool; `gfx_present` copies what changed.

- **1:1** on a 2560x1440 screen (the PC), or an integer scale up on a
  screen at least twice as big. The chroma is interpolated to full size
  (9/16 of a pixel's own 2x2 block's sample, 3/16 of each nearest one
  beside and above or below, 1/16 of the diagonal: centre-sited 4:2:0),
  so colour edges are smooth rather than 2x2 steps.
- **Down by an integer n** (2560x1440 on QEMU's 1280x800: n = 2): Y, Cb
  and Cr each averaged over the n x n block (a box filter), then
  converted.
- **Down by any other ratio** (on a 1920x1080 screen: 0.75): bilinear,
  Y and chroma each from their four nearest samples at the output pixel's
  centre. (Under half size it would alias; such screens get a box.)
- Centred, the rest of the screen `#1E1A1D`. A 3840x2160 screen shows it
  1:1 in the middle (no 1.5x scale).

pl_mpeg allocates a decoder's three frames in one block (16.6 MB at
1440p, more than libos's 16 MB heap): `plmpeg.c` gives blocks of 256 KiB
and up a VMO each, and unmaps them on free.

## Alpha blending in libfun (`user/apps/fun/alpha.c`)

Premultiplied 0xAARRGGBB over opaque surfaces: `argb_pm`, `px_over`,
`fill_pm` (two pixels at a time in GCC vector types: SSE2), `blit_pm`,
`disc_aa` and `line_aa` (each pixel's coverage as alpha). The splash's
fade-out uses `fill_pm`; `run splash --selftest` checks them all.

## Tests

- `run splash --selftest`: the alpha functions against their formulas
  (the SSE2 fill against `px_over` on random rows); the three ways of
  drawing on a frame made for it (flat areas exact, the 1:1 chroma edge
  blended, the box's edge pixel an average, 2x, bilinear); the late
  join's fade-in; and `splash.mpg` decoded and drawn 1:1 whole, timed:
  every frame of its length at its rate, the background in the first
  frame's corner, its sound 48 kHz and as long as the video.
- `tools/splash-test.sh` ([TESTING.md](../TESTING.md#area-scripts)): five
  boots (the splash with the sound captured, a key skip at 2560x1440,
  `verbose`, `nosplash`, a panic at boot over the quiet screen).
  Every other test boots with `nosplash` (`tools/qemu-test.sh` adds it
  unless `QEMU_SPLASH=1`).

Numbers for the 2560x1440 file (2026-10-01): pl_mpeg decodes a frame in
3.9 ms natively on the Mac (an M-series core, built with cc -O2; worst
5.7 ms) and 41 ms under QEMU's TCG; `draw.c` takes 39 ms for 1:1 on 4
TCG CPUs. On the PC (a Raptor Lake core is about as fast as the Mac's
for this) that is about 4-6 ms to decode plus about 1 ms to draw on 28
threads and a few ms to present: well inside 33 ms a frame, so nothing
is dropped and decoding stays on the main thread. Under QEMU the picture
falls behind its sound (82 of 255 frames shown at 1280x800, the video
ending at 13 s for 8.5 s of sound); the tests allow for it. The sound
waited 315 ms for its stream and both started together at 0; the first
frame at 1.9 s of uptime; the shell up at 15.6 s with the splash (2 s of
it the linger) and 1.5 s without.

Numbers for the first, 1280x720 file in QEMU (TCG on the Mac, 4 CPUs, 2026-10-01): the first frame at
1.45-1.8 s of uptime (init starts at 1.1 s; the kernel spends its first
second measuring the CPUs' ticks); 185-193 of 195 frames shown at
1280x800, the rest dropped; the sound joined 0.3-0.5 s into the animation,
and the capture matches the video's sound from that point to its end
(37-39 dB above the difference: two MP2 decoders, pl_mpeg's and ffmpeg's);
the shell up at 8.0-8.3 s of uptime with the splash and 1.5 s without.
pl_mpeg decodes a frame in 9.7 ms under QEMU. After the merge with the
mixer's shorter periods the guess `HEARD_IN` was 5-57 ms off at the
switch. On a busy Mac QEMU itself sometimes loses a period of the
capture (the guest's mixer reports no underrun or late period), so the
check follows one such jump; at 2560x1440 QEMU's audio timing is too
disturbed to compare the capture, so the skip boot checks the mixer's
count of frames taken instead.

## What only the PC can show

- The time from Limine to the dark screen (fbcon's first act) and to the
  first frame: init starts at about 1.05 s, so the first frame should be
  at about 1.1 s; the log line `splash: first frame at N ms of uptime`.
- `splash: waited N ms for the sound: they start together` (N under
  2000), `sound joins at 0 ms`, and the first impact heard whole, not
  cut into. Whether the headphone path opening still clicks: the driver
  unmutes as the stream starts and the owner's sound begins with 0.1 s
  of near-silence, so a click there would be the codec's, not the
  sound's (a driver matter).
- `splash: played at ...: N frames shown, M dropped` (0 dropped
  expected), and the picture sharp: edges and the pixel font as rendered.
- `the shell is up N ms after the end; giving the screen back M ms after
  it` (M at least 500: the linger).
- `run splash --selftest`: pl_mpeg's speed per frame on the PC.
- `run splash` from the shell plays it again (without init it holds the
  last frame a second or until a key).

## Left for later

- The `verbose` boot menu entry is the way to see the log; Esc during the
  splash only skips it (the log is in `/data/logs/` and on serial).
- The sound's start is a guess, corrected once; measuring the output's
  start (a silent pre-roll) would remove the one jump.
