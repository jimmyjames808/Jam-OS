# AS: the boot splash

Status: works in QEMU (branch `as-splash`); not yet seen on the PC.

On a plain boot the owner's logo animation plays with its sound while
Jam OS starts: the seven drupelets drop in and assemble into the logo,
the gold one glows, the logo slides left and "Jam OS" types out. The
whole animation (6.5 s) plays every boot; the last frame stays until the
shell is ready; then the picture fades into the console's text.

## The video: `boot/splash.mpg`

The animation is made outside the repository (by the owner, in
`~/Movies/motion-graphics/jamos-boot/`: `jamos-boot.mov`, ProRes 4444
with alpha, 1920x1080, 30 fps, and `jamos-boot.wav`, 48 kHz stereo).
`tools/mksplash.sh` turns them into one MPEG-1 program stream: the video
at 1280x720, 30 fps, composited over the background `#1E1A1D`, `-q:v 4`,
no B-frames; the sound as MP2, 48 kHz stereo, 160 kb/s. The result, 469 KB,
is committed, so a build never needs the owner's files; `make`
regenerates it only when they are there (`SPLASH_SRC`, by default that
folder) and newer than it. It goes into bootfs as `splash.mpg`.

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
   **the splash**, then serialin, devmgr, the mixer, logd. The shell waits:
   it starts once the splash says `SPLASH_PLAYED` (or ends).
3. **The splash** (`user/apps/splash`) borrows the screen and the keys
   from the console (a PROGRAM-level console channel, `gfx_open_on`: the
   screen stays `#1E1A1D`), shows the first frame at once and plays on
   the timer; a thread decodes the MP2 track whole and opens a stream on
   the mixer (`SR_AUDIO`), which answers once devmgr has bound the hda
   driver and init has started the mixer. The sound joins at the
   animation's current time (not from the beginning).
4. When the animation ends (or a key skips it) the splash tells init
   `SPLASH_PLAYED`; init starts the shell; the shell, once its banner is
   written, calls `initctl.shell_ready`; init sends `SPLASH_GO`
   (`<splash.h>`); the splash fades the last frame into the console's
   background (8 steps of 30 ms, premultiplied alpha) and closes the
   lease; the console draws its text, the shell's prompt at the bottom.

Keys: the splash has the only key focus while it plays (the shell starts
after it), so a key typed then goes to the splash, never to the shell.
Any key skips the rest: the sound fades out over 5 ms.

## Sync: the sound is the clock

Media time is ns from the first frame; frame n is due at n/30 s, the
sound's frame f at f/48000 s. The file's time stamps line them up: the
MP2 decoder's 481-sample delay is in the stamps (the sound's start 10 ms
before the video's), and `sound_decode` drops those frames, so the sound
is where the owner's WAV had it.

- Until the sound is heard, media time is the timer.
- When the stream opens, the sound starts at the media time then plus a
  guess of how long until it is heard (`HEARD_IN`, 50 ms: at boot the
  splash's stream is the only one, and the mixer opens the output with
  its first frames first).
- Once the mixer's `stream_position` says frames are heard (`played`,
  which the mixer interpolates from the driver's position), media time is
  the frame being heard: the position is asked every 20 ms (or after each
  write, at most 85 ms apart) and moved on by the timer in between. The
  log says how far the timer was off at the switch (one jump of the video,
  that once).
- The video shows a frame when the clock reaches it; a frame whose next
  frame is already due is dropped (decoded, not drawn); none is early.
  It can't drift from the sound.

## Drawing

`video.c` converts each frame's YCbCr 4:2:0 (studio-range BT.601, 16.16
fixed point) into libfun's back buffer at the largest integer scale that
fits (2x for 1280x720 on 2560x1440, nearest neighbour), centred, in bands
of 16 rows on the thread pool; `gfx_present` copies what changed.

## Alpha blending in libfun (`user/apps/fun/alpha.c`)

Premultiplied 0xAARRGGBB over opaque surfaces: `argb_pm`, `px_over`,
`fill_pm` (two pixels at a time in GCC vector types: SSE2), `blit_pm`,
`disc_aa` and `line_aa` (each pixel's coverage as alpha). The splash's
fade-out uses `fill_pm`; `run splash --selftest` checks them all.

## Tests

- `run splash --selftest`: the alpha functions against their formulas
  (the SSE2 fill against `px_over` on random rows), and `splash.mpg`
  decoded whole: 195 frames of 1280x720 at 30 fps, the background and the
  gold drupelet where they belong, 6.5 s of 48 kHz sound.
- `tools/splash-test.sh` ([TESTING.md](TESTING.md#area-scripts)): five
  boots (the splash with the sound captured, a key skip at 2560x1440,
  `verbose`, `nosplash`, a panic at boot over the quiet screen).
  Every other test boots with `nosplash` (`tools/qemu-test.sh` adds it
  unless `QEMU_SPLASH=1`).

Numbers in QEMU (TCG on the Mac, 4 CPUs, 2026-10-01): the first frame at
1.45-1.8 s of uptime (init starts at 1.1 s; the kernel spends its first
second measuring the CPUs' ticks); 185-193 of 195 frames shown at
1280x800, the rest dropped; the sound joined 0.3-0.5 s into the animation,
and the capture matches the video's sound from that point to its end
(37-39 dB above the difference: two MP2 decoders, pl_mpeg's and ffmpeg's);
the shell up at 8.0-8.2 s of uptime with the splash and 1.5 s without.
pl_mpeg decodes a frame in 9.7 ms under QEMU.

## What only the PC can show

- The time from Limine to the dark screen (fbcon's first act) and to the
  first frame: init starts at about 1.05 s, so the first frame should be
  at about 1.1 s; the log line `splash: first frame at N ms of uptime`.
- The sound joining: `splash: sound joins at N ms of the animation` and
  `the sound is heard from N ms: the clock now (the timer was ... by N
  ms)`: how long the hda output takes to start on the real codec (if it
  is far from 50 ms, `HEARD_IN` should change), and that sound and
  picture look together.
- `splash: played at ...: N frames shown, M dropped` (0 dropped expected).
- `run splash --selftest`: pl_mpeg's speed per frame on the PC.
- `run splash` from the shell plays it again (without init it holds the
  last frame a second or until a key).

## Left for later

- The `verbose` boot menu entry is the way to see the log; Esc during the
  splash only skips it (the log is in `/data/logs/` and on serial).
- The sound's start is a guess, corrected once; measuring the output's
  start (a silent pre-roll) would remove the one jump.
