# A2: the audio mixer, the `audio` protocol, WAV playback

A2 runs as two tracks at once: track 1 (WAV playback, below) builds the
client library programs write sound through and the `play` command on
the hda driver's one stream as it is today; track 2 (the mixer) builds
the `audio` protocol and the mixer service, then moves the library's
backend onto it. Each track keeps to its own section of this file.

## Track 1: WAV playback

Status: done, heard on the PC (A2 signed off there on 2026-10-01).

### The library: `<audio.h>` (user/lib/audio.c, in libos)

The contract between the tracks: callers use only these calls, so the
backend can change under them.

| Call | What |
|---|---|
| `audio_open(&a, rate, channels)` | an output for 16-bit frames of `rate` Hz (8000-192000) and 1 or 2 channels. ERR_NOT_FOUND: no device; ERR_BAD_STATE: busy (today one program plays at a time); ERR_NOT_SUPPORTED: rate or channels out of range |
| `audio_write(&a, frames, n)` | blocking: converts and copies into the ring as room frees up; returns `n` or a negative status. Write a few thousand frames at a time (Ctrl+C is checked between calls) |
| `audio_drain(&a)` | waits until everything written has played, plus one period of silence (the device's buffers empty) |
| `audio_set_volume(&a, cB)`, `audio_get_volume(&a, &cB)` | centibels, at most 0. Today the device gain (hda `set_gain`), put back as it was at close; with the mixer, the stream's own |
| `audio_close(&a)` | stops; anything still queued is dropped with a 5 ms fade (no click) |
| `audio_devmgr()` | the devmgr channel the backend looks the device up through: libos's default is SR_DEVMGR (a weak symbol); the shell defines its own (init sends it each new devmgr) |

Pure helpers, tested in utest without hardware (`audio_formats`,
`audio_resample`, `wav_parse`): `audio_rs_*` (the resampler),
`audio_s16_from_u8/s24le/s32le`.

**The resampler** was linear interpolation; the sound quality pass
(below) replaced it with a polyphase windowed-sinc filter (flat to
20 kHz, 100 dB down from 22.05 kHz; the position still kept exactly, no
drift). 48000 Hz input is copied untouched.

**The backend today** (all of it in audio.c): finds the first hda driver
with a path through devmgr's GET_SERVICE, `open_output` (48 kHz 16-bit
stereo), maps the 64 KiB ring, writes device frame f at f mod 4096. The
stream starts when the ring is first full (or at drain); after that each
write asks `position` once and waits with `wait_period` when the ring is
full. Writes that fall behind the play position skip ahead (the gap is
silence, the driver's clear-behind) and count `underruns`. Nothing is
written within 256 frames (5.3 ms) of the position, which the controller
may have fetched already.

**For track 2**: replace `find_device`, `audio_open`'s open and map,
`refresh`, `wait`, `start`, `put`, `audio_drain`, `fade_out` and the two
volume calls with the mixer's protocol; the resampler and conversions can
stay in the client (the mixer then only ever mixes 48 kHz stereo) or
move to the mixer. beep and play need no change.

### `play` (the shell's cmd/play.c)

`play [-v dB] <file.wav>`: reads the header with `<wav.h>` (RIFF/WAVE,
`fmt ` and `data`, other chunks skipped), prints
`play: <name>: <rate> Hz, <bits>-bit, <channels> ch, <m:ss>`, then reads
the samples 4096 frames at a time (the file is never loaded whole) and
writes them through `<audio.h>`. Formats: PCM (format 1) and
WAVE_FORMAT_EXTENSIBLE with the PCM subformat; 8-bit unsigned, 16-, 24-
and 32-bit signed; mono or stereo; 8000-192000 Hz. Refused with a reason:
not RIFF/WAVE, cut off in its header, floating point, compressed, more
than two channels, other sample sizes or rates. A data chunk longer than
the file plays as far as the file goes. Ctrl+C stops within a chunk
(about 0.1 s), fades out over 5 ms, prints `play: <name>: stopped at
m:ss` and returns 130. `-v -20` plays 20 dB down for this file only.

`beep` writes its tone through the same library (48 kHz mono); its
output and `tools/beep-test.sh` are unchanged, and Ctrl+C during a beep
now fades too.

### Getting a song onto the stick (the Mac)

Convert to 16-bit PCM WAV at 48 kHz (the device's rate: no resampling),
copy it to the stick's data partition, eject:

    afconvert -f WAVE -d LEI16@48000 song.m4a song.wav
    cp song.wav /Volumes/JAMOS-DATA/

(The data partition is the stick's only volume macOS mounts by itself;
it may be named NO NAME instead of JAMOS-DATA.)

Then on the PC, headphones in the front jack: `play /data/song.wav`
(`hda gain -20` first if -30 dB is too quiet). 44.1 kHz files
(`LEI16@44100`, or any WAV a CD ripper writes) play too, resampled.
MP3s need no conversion: copy them as they are ([MP3](#mp3) below).

### Tests

- utest `audio_formats`, `audio_resample`, `wav_parse`.
- `tools/play-test.sh` ([TESTING.md](TESTING.md#area-scripts)): seven
  files on the stick image, played in QEMU into a WAV capture and checked
  (frequency per channel, length within 2 %, mono/stereo, silence after,
  no clicks, `-v`, refusals, Ctrl+C with a fade).

### Left for later

- Done since: a real song heard on the PC; underruns shown (`play -s`,
  the sound quality pass below); a filtering resampler; two programs at
  once (track 2's mixer).
- Float WAV (`afconvert -d LEI16` avoids it); `play` of several files or
  a directory; a progress line.

## Track 2: the `audio` protocol and the mixer

### Fixed decisions

- **The mixer is a service init starts** in shell mode, after devmgr
  (`bin/mixer`, `user/services/mixer/`), supervised like the others. It
  is the hda driver's only client while anything plays: it finds the
  driver through devmgr's query channel (GET_SERVICE, the first service
  that answers `hda.info` with a path, as `beep` did), opens the one
  output stream when the first stream starts and closes it when none
  plays, so the jack is muted whenever nothing plays (A1's driver
  unmutes only while its stream runs).
- **One format: 48 kHz, 16-bit, stereo** (the driver's and the mixer's).
  `open_output` refuses anything else (`ERR_NOT_SUPPORTED`); the client
  library converts other rates, mono and 8-bit before writing.
- **Clients reach the mixer by a startup role.** Two new roles in
  `kernel/include/jam/startup.h`: `SR_AUDIO` (the `audio` service channel:
  open a stream) and `SR_AUDIO_CTL` (the `audioctl` control channel: list
  streams, set any stream's volume, the master volume). init makes both
  channels once and keeps a duplicate of their server ends, so a mixer
  that dies and is restarted serves the same channels: a client's call
  made meanwhile waits for the new mixer, and its stream (a channel of
  its own) reports `ERR_PEER_CLOSED`, after which it opens a new one on
  the same `SR_AUDIO`. The shell gets both; every program the shell runs
  gets `SR_AUDIO`; test programs (`utest`, `usbtest`, `hdatest`,
  `mixtest`) also get `SR_AUDIO_CTL`, and `mixtest` init's control
  channel (it kills the mixer and the hda driver).
- **Rights: a client holds only its stream.** `open_output` returns a
  stream channel (start, stop, drain, position, its own volume; closing
  it ends the stream), the ring VMO (read, write, map) and an event (wait,
  signal). Nothing on the service channel names another stream; only
  `audioctl` does.
- **Volume in centibels, attenuation only**: 0 dB at most (no clipping
  from boost), -96.0 dB and below is silence. Per-stream and master
  volume are applied by the mixer in fixed point (Q15); `hda gain` stays
  the codec's own output level (the headphones' loudness), untouched by
  the mixer.

### The protocol (`abi/idl/audio.idl`, `abi/idl/audioctl.idl`)

| Method | On | What |
|---|---|---|
| `open_output(rate, channels, bits, name[16]) -> (stream, ring, event, id, frames, lead)` | `SR_AUDIO` | a new stream, stopped, empty; `frames`: the ring's size; `lead`: how far ahead of the speaker the mixer reads (frames). `ERR_NO_RESOURCES`: 16 streams already; `ERR_NOT_FOUND`: no audio output (no hda driver with a path) |
| `start`, `stop` (the stream methods are `stream_*` in the IDL, clear of `<audio.h>`'s names) | stream | the mixer takes frames from the ring (from the next period it mixes) or leaves them there |
| `drain() -> (frames)` | stream | answers once everything written before the call has played |
| `position() -> (written, consumed, played)` | stream | frames: in the ring, taken by the mixer, heard (estimated from the driver's position) |
| `set_volume(cb) -> (cb)` | stream | this stream's volume |
| `levels() -> (volume, master, device)` | stream | what it is heard at: its volume, the master, the driver's gain |
| `streams() -> (count, master, list)` | `SR_AUDIO_CTL` | every stream: id, volume, state, underruns, frames played, name |
| `set_volume(id, cb) -> (cb)`, `set_master(cb) -> (cb)` | `SR_AUDIO_CTL` | for `vol` |

### The ring: no copies on the client's side, no syscall per write

One VMO per stream, made by the mixer: a header page, then the samples
(16384 frames, 64 KiB, 341 ms). The header (`user/include/mixer.h`) has
`write` (frames written, ever; only the client writes it), `read` (frames
taken; only the mixer writes it) and two flags, each on its own cache
line. The client writes samples straight into its mapping and then
`write`: no call. The event is used only when someone would otherwise
wait:
- **the client is blocked for space**: it sets `waiting`, clears the
  event's SPACE bit and waits on it; the mixer signals SPACE after taking
  frames from a ring whose `waiting` is set;
- **the mixer is starved**: when every playing stream has been empty for
  about a second the mixer closes the driver's stream (the path is muted)
  and sets `idle` in their headers; a client that writes while `idle` is
  set signals the event's DATA bit, which wakes the mixer.
A missed wake costs at most one period (the mixer looks at every ring
each period anyway).

The mixer never maps a client's ring: a client holds the VMO with
`RIGHT_WRITE` and could shrink it under a mapping (as fat never maps a
file client's buffer). It reads the header and the frames with `vmo_read`
(one copy, into the buffer it mixes from) and writes `read` with
`vmo_write`; it keeps its own `read` and never reads it back. A client
that lies in `write` gets at most its own ring's frames again.

### The mixer

- **One thread, one port** (as the hda driver): the service and control
  channels, each stream's channel and event, devmgr's channel (its close
  ends the mixer, which init restarts with the new devmgr) and the
  driver's stream channel. `wait_period` is sent without waiting for the
  answer (its reply arrives on the port), so client calls are served
  while a period plays.
- **Periods and latency**: the driver's 4 periods of 4096 frames (85 ms).
  At each period's end the mixer mixes so that two periods are written
  ahead of the play position: what a client wrote is heard 85 to 170 ms
  after the mixer takes it (`lead` = 8192 frames), plus however far ahead
  the client keeps its own ring.
- **Mixing**: for each playing stream its frames (`vmo_read`) times its
  gain in Q15, rounded, summed into 32-bit accumulators; then the master
  gain and saturation to 16 bits, straight into the driver's mapped ring.
  At 0 dB the samples pass through unchanged (`user/lib/mixmath.c`).
- **A slow client** contributes what it has and silence for the rest
  (counted as an underrun); the others are not affected. **A dead
  client's** stream channel closes and the stream is dropped at once.
- **The driver restarting**: the driver's stream channel closes; the
  mixer gets the service again and reopens the output (streams keep their
  rings; a gap is heard). **The mixer dying**: init restarts it (as any
  service); clients' stream channels close; a client reopens.
- No stream playing: the driver's stream is closed at once (the path
  muted). Every playing stream empty for 12 periods (~1 s): closed too,
  reopened when one of them writes (DATA) or another starts.

### `vol`

`vol` lists the streams (id, volume, state, underruns, name) and the
master volume; `vol <id> <dB>` sets a stream's, `vol master <dB>` the
master (`audioctl`).

### Tests

- utest `mix_*`: the arithmetic (0 dB passes samples through exactly,
  -6 dB is Q15 16423, saturation at both ends, master, silence).
- `tools/mixer-test.sh` (QEMU, hda-output codec with `mixer=off` into a
  WAV file, like hda-stream-test): `mixtest` runs two tone programs at
  once (440 Hz, and 1000 Hz at -6 dB): both frequencies are in the
  capture, the second at half the first's amplitude; one of two killed
  mid-tone, the other plays on without a gap; master and `audioctl`
  volume heard; the mixer killed (its client sees `ERR_PEER_CLOSED`,
  reopens and plays again); the hda driver killed (the mixer reopens the
  output, the stream plays on); a stream left empty lets the mixer close
  the output and a later write wakes it; the codec's verbs show the path
  muted at the end; `vol` from the shell.

### What was built

As planned above, plus:
- **`<audio.h>` on the mixer** (after track 1 merged): `audio_open` is a
  mixer stream on the program's `SR_AUDIO` (`audio_open_as` names it for
  `vol`: beep and play do); the library converts to 48 kHz stereo and
  writes with `mixer_write` (`<mixer.h>`, libos: the client side of the
  ring); `audio_drain` is the mixer's drain, then a period of silence
  drained too (as before: whatever records the output gets past the
  sound's end); `audio_close` fades the frames the mixer has not taken
  yet over 5 ms. `audio_set_volume` is now the stream's own volume (it was
  the device's gain, put back at close), so `play -v -20` plays that file
  20 dB below the others instead of moving `hda gain`; `audio_get_volume`
  is the level heard (stream + master + device gain: `levels`), so
  `beep` still says "at -30.0 dB". The weak `audio_devmgr` hook is gone.
- **Numbers in QEMU** (tools/mixer-test.sh, 2026-10-01): 440 Hz and
  1000 Hz at -6 dB from two programs: amplitudes 8192 and 4106, ratio
  0.5012 (Q15 16423 / 32768 = 0.50119); a 1500 ms tone whose partner was
  killed at 700 ms: no 10 ms window under 90 % of its amplitude; master
  -6 dB: 4106; `audioctl` -12 dB mid-tone: 0.2512 of the start; the mixer
  killed mid-tone: the client saw `ERR_PEER_CLOSED` and had a new stream
  42 ms later; the hda driver killed mid-tone: the output reopened 0.25 s
  later on its restart, the stream played to its end; an empty stream
  let the output close after 12 periods, its next write reopened it.
  The codec's verbs: the path open only while a stream ran.
- Not possible from one shell: `play` and `beep` at the same time (the
  shell runs one command at a time); `mixtest` runs two programs on
  `<audio.h>` at once instead (a 44.1 kHz tone resampled and a 48 kHz one).

### What only the PC can show

- Two sounds at once in the headphones: the shell runs one command at
  a time, so on the PC that is `mixtest` (two tone programs at once, at a
  quarter of full scale and `hda gain`; headphones off the head first).
  It also kills the mixer and the hda driver once, as in QEMU.
- The mixer keeping up on the real controller: `vol` shows each
  stream's underruns (0 expected), the log says "frames late" if a
  period's end came too late.

### Left for later

- devmgr still hands the hda driver's channel to any GET_SERVICE caller
  (the shell's `hda`, `hda gain` and `hdatest` use it): a program that
  holds the driver's one stream (`hdatest`) makes a stream's start fail
  `ERR_BAD_STATE` meanwhile.
- The mixer's loop waits in its calls to the driver (an open up to 3 s,
  finding the driver up to 2 s per service) while the driver restarts:
  its clients' calls wait as long.
- `played` is interpolated within a period from the driver's position;
  nothing measures the codec's own delay.

### Done when

Two programs play at once through the mixer in QEMU (above), `vol` works
from the shell, beep and play go through the mixer, and on the PC two
sounds are heard at once.

### The sound quality pass (branch `audio-quality`)

After the owner's first song on the PC ("smooth ... a little bit not
perfect", build 11dfa9a, `play` straight to the driver), every source of
imperfection that could be measured was, in QEMU with
`tools/audio-quality-test.sh` (numpy over the wav capture).

| What | Before | After |
|---|---|---|
| Output sample size | 16-bit always | the DAC's best: 24-bit on the PC (32-bit containers), 16-bit in QEMU; `hda bits` caps it |
| Mixing | each stream rounded to 16 bits, then saturated | Q8 sums (24-bit resolution through volumes), a lookahead limiter instead of clipping, TPDF dither only where a 16-bit output drops a fraction; one stream at 0 dB bit-exact |
| 1 kHz at -40/-60 dB of volume (16-bit QEMU output) | all error in harmonics (undithered) | harmonics at the noise; on the PC the 24-bit output keeps the arithmetic's error ~48 dB lower still |
| 44.1 kHz resampling, 20 kHz tone | -6.3 dB, image -3.2 dB | -0.000 dB, nothing above -101 dB |
| 44.1 kHz, 1 kHz tone | image at 4.9 kHz, -65 dB | nothing above -104 dB |
| Driver periods | 4 x 4096 frames (85 ms) | 8 x 2048 frames (42.7 ms) |
| Mixer lead | 2 periods: 85-170 ms ahead | 4 periods: 128-171 ms ahead (QEMU: never under 118 ms) |
| Client ring (a program's read-ahead) | 16384 frames, 341 ms | 65536 frames, 1.37 s |
| Underruns, late periods, a 20 s tone read from the stick | 0, not measured, no dropout | 0, 0, no 10 ms window off by 0.01 dB |
| Seeing it on the PC | the mixer's close line had underruns | `play -s`, the mixer's close lines, the driver's DPIB/LPIB gaps |

Findings that needed no change:
- **Clear-behind** needs no margin behind the position: the position
  buffer, LPIB and Intel's DPIB all trail the DMA engine's fetch, so the
  bytes behind them were read already (stream.c's header). The margin is
  the writer's: the mixer counts a period end with under 256 frames
  written ahead as late (`LATE_GUARD`), more than a FIFO.
- **Gain staging**: the path's only gain is the DAC's own amp (0.75 dB
  steps; the pin's amp is 0-0). Attenuation there, below the DAC's
  input, keeps a 16-bit source bit-exact into the DAC; with the 24-bit
  stream a mixer volume costs nothing either. So the default stays:
  -30 dB at the DAC (`hda gain`), the mixer at 0 dB. Not made louder.
- **Codec**: the loopback mixer 0b and the other inputs of mixer 0c stay
  muted (only 0c's input from DAC 02 opens), pin 1b drives its
  headphone amp (ctl 0xc0) with EAPD on, all while a stream plays only.
  Unused DACs and pins were left as the firmware set them (the driver
  writes nothing off its path). Untested: whether the pin and EAPD
  switching at each stream's start and end pops; if `play` makes a pop
  at its start or end, that is it (a fix would keep the path open a
  little after the last stream, which bends "muted unless a stream
  plays", so it is the owner's call).

**On the PC** (headphones off the head for the first play after
flashing: the output is 24-bit now):

    hda bits                    # "the DAC takes 16, 20, 24"
    play -s /data/audio/tone1k.wav
    play -s /usb0/big-poppa.wav

Perfect is: `0 underruns, 0 late periods`, `mixer >= ` about 120-128 ms
(under 100 means the mixer ran late at some point), `slowest read`
well under 1000 ms (above about 1200 ms the ring runs dry: an
underrun), `0 limited`, `out 48 kHz 24-bit`; and `log` shows the
driver's `stream: closed ... 0 FIFO error(s)`. A pure tone is the
hardest test: any gap clicks. To compare by ear, `hda bits 16` then
play again (`hda bits 24` back).

The test files (on the Mac; copy them to the stick's data partition's
`audio` folder):

    python3 - <<'PY'
    import math, struct, wave
    def write(name, rate, secs, phase, amp=0.25):
        w = wave.open(name, "wb")
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
        n, fade, out = int(rate * secs), int(rate * 0.01), bytearray()
        for k in range(n):
            env = min(1.0, k / fade, (n - 1 - k) / fade)
            v = int(round(amp * env * 32767 * math.sin(phase(k / rate))))
            out += struct.pack("<hh", v, v)
        w.writeframes(bytes(out)); w.close()
    write("tone1k.wav", 48000, 60, lambda t: 2 * math.pi * 1000 * t)
    write("tone1k-44.wav", 44100, 60, lambda t: 2 * math.pi * 1000 * t)
    K = 20.0 / math.log(1000.0)    # 20 Hz to 20 kHz in 20 s
    write("sweep-44.wav", 44100, 20, lambda t: 2 * math.pi * 20 * K * (math.exp(t / K) - 1))
    PY

`tone1k` (48 kHz, no resampling: the driver and mixer alone) and
`tone1k-44` (the resampler too) are a steady 1 kHz at -12 dBFS for a
minute: any tick, dropout or warble is a fault. `sweep-44` rises from
20 Hz to 20 kHz: it should rise smoothly and fade into inaudibility at
the top, with no second tone falling while it rises (that would be
aliasing).

### For AS (the boot splash)

The splash gets `SR_AUDIO` from init. Its `open_output` may come before
the mixer has found the driver: the call waits for the mixer (init holds
the channel), and the mixer opens the driver when the splash starts its
stream. For lip sync it uses `position().played` (the mixer's estimate
of what is heard, to within a period's interpolation), not what it wrote.

## MP3

Status: done, heard on the PC (A2 signed off there on 2026-10-01).

`play song.mp3` plays an MPEG audio file the way it plays a WAV file:
the same `<audio.h>` stream (resampled to 48 kHz), Ctrl+C with the 5 ms
fade, `-v`.

### The decoder: dr_mp3

`third_party/dr_mp3` (third_party/VERSIONS.md): dr_mp3 by David Reid,
public domain or MIT-0, one header, vendored unmodified at dr_libs commit
51e61d3 (v0.7.4, unreleased: the commits after v0.7.3 fix an
out-of-bounds read in its Xing/Info tag parsing). Chosen over minimp3
(CC0), which it is a fork of, because minimp3 has been unmaintained
since 2021 and dr_mp3 carries the fixes since, and because dr_mp3 already
does what a player needs around the decoder: read/seek callbacks with a
no-stdio build, its own allocation callbacks, ID3v2/ID3v1/APE tags,
Xing/Info frames and LAME's encoder delay and padding (so a file plays
exactly as long as its source). libmad and mpg123 are GPL/LGPL: out.

It decodes MPEG-1, MPEG-2 and MPEG-2.5 (8000 to 48000 Hz), Layers I, II
and III, CBR, VBR and free format, mono and stereo, into 16-bit samples
(its float synthesis rounded and saturated inside it). SSE2 is on (user
programs may use SSE; the kernel saves each thread's FPU state). Its
configuration is `user/lib/mp3port/dr_mp3_impl.c`, compiled into libos
with that directory's string.h and stdlib.h (onto libos's memcpy,
memmove, memset, malloc and free); libos has no realloc, so the
allocation callbacks leave it out and dr_mp3 grows its buffer with
malloc, a copy and free. It needs no maths library. About 35 KiB of
code, linked only into programs that call `mp3_open` (the shell, utest);
while a file plays, ~33 KiB of decoder state and a 64 KiB read buffer
on the heap and at most ~1.2 KiB of stack.

### `<mp3.h>` (user/lib/mp3.c, libos)

- `mp3_header_parse`: one frame header (version, layer, rate, bitrate,
  frame length), from the standard's tables.
- `mp3_sniff` (pure apart from its read callback): skips ID3v2 tags
  (up to 8 in a row, footers included), finds an ID3v1 tag (the last 128
  bytes, "TAG") and an APEv2 tag before it (its "APETAGEX" footer), then
  looks in the first 8 KiB of what is left for a frame header followed
  by a second one where the first frame ends (same version, layer and
  rate), or by the end of the audio (a one-frame file). A file without
  one is refused at once: dr_mp3 alone would search a 15 MB file to its
  end for a frame. It also reads a VBRI (Fraunhofer) header.
- `mp3_open`/`mp3_decode`/`mp3_close`: dr_mp3 through read/seek/tell
  callbacks over the caller's reader, 64 KiB at a time, never the whole
  file; `mp3_decode` gives interleaved 16-bit frames, 0 at the end, or
  the reader's error. The length: a Xing/Info header's frame count less
  LAME's delay and padding (exact), else VBRI's count, else the audio's
  bytes at the first frame's bitrate (CBR: right to a frame), else
  unknown.
- **A dr_mp3 bug, worked around, not patched**: after it reads a
  Xing/Info frame, dr_mp3 sets its stream cursor back to that frame's end
  although it has already read up to 64 KiB further, so the clamp that
  keeps it out of the ID3v1 tag lets it read the tag after the last
  frame; the last frame then fails its "ends where the data ends" check
  and is dropped (a LAME file with an ID3v1 tag played 26 ms short, its
  tag bytes fed to the decoder). `mp3_open` gives dr_mp3 the file without
  its end tags, so the cursor's error never reaches them. Worth reporting
  upstream.

### `play`

`<play_src.h>` (libos's `user/lib/play_src.c`; it was the shell's
`cmd/play_src.c` until the music player needed it too) is a small source
interface (open, read 16-bit frames, close, a description for the first
line) with a WAV source (what play.c did before) and an MP3 source;
play.c's loop reads from whichever was opened. The format is chosen by content, not the
name: `RIFF` at the start is WAV, else MP3 if `mp3_sniff` finds frames,
else `not a WAV or MP3 file (no RIFF/WAVE header, no MPEG audio frames)`.

    play: song.mp3: MP3, 44100 Hz, 2 ch, 192 kbps, 4:22

(`MP2`/`MP1` for Layers II and I; `VBR` when a Xing or VBRI header says
so, `free format` when the bitrate isn't in the header; `?` when the
length is unknown.) Frames the decoder can't decode are skipped (that
bit of the music is missing; no silence is put in); a file cut off
mid-frame ends at its last whole frame; a read error (the stick pulled)
stops it with the error once dr_mp3's 64 KiB buffer has played.

`play -n <file>` decodes (or for a WAV, reads) the whole file as fast as
it goes and prints how long that took per second of audio: the CPU cost
of a file, for the PC.

### Getting MP3s onto the stick

Copy them as they are to the stick's data partition from the Mac, then
eject: `cp song.mp3 /Volumes/JAMOS-DATA/` (or NO NAME). Then
`play /data/song.mp3`. Album art in the ID3 tag is skipped, not read.

### CPU

QEMU (TCG emulation on the Mac, `play -n`, including reading the file
from the emulated stick): a 30 s 44.1 kHz stereo file at 320 kbps CBR
1.14 s, 38 ms per second of audio (48 kHz 320 kbps: 40 ms; VBR -q:a 0:
25 ms; 128 kbps: 24-28 ms). Reading a WAV of the same 30 s costs more
(115 ms per second: its 5.3 MB through emulated USB), so most of the MP3
figure is the emulator. The same decoder on the Mac natively: 0.22 ms per
second of audio. On the PC (Raptor Lake) it should be well under 1 ms per
second of audio (under 0.1 % of one core): `play -n` there gives the real
number.

### Tests

- utest `mp3_header` (versions, layers, rates, lengths, free format,
  every reserved value refused), `mp3_sniff` (ID3v2 with footer, two
  tags, junk, APEv2 + ID3v1 at the end, one-frame and cut files; a
  mismatched second header, a lone header, a tag alone, noise, a WAV,
  empty: refused), `mp3_decode` (silent frames built in memory decode to
  1152 zero frames each; a Xing frame: VBR, exact, not played; VBRI;
  mono with tags at both ends; a read error past dr_mp3's first 64 KiB;
  noise).
- `tools/mp3-test.sh` ([TESTING.md](TESTING.md#area-scripts)) in QEMU.
  Numbers on 2026-10-01: 44.1 kHz stereo 192 kbps CBR 2490.0 ms for a
  2.49 s tone (LAME's delay and padding dropped exactly), 440.00/660.00
  Hz; 48 kHz mono VBR 1000.0 ms, 1000.00 Hz; 22.05 kHz MPEG-2 without a
  Xing tag 1500.8 ms; ID3 art + ID3v1 1000.0 ms; Layer II 1002.0 ms,
  300.00 Hz; 3000 bytes of noise mid-file: 2333 ms (frames lost, played
  on), cut at 60 %: 1464 ms, clean end; Ctrl+C with a fade; `play -n`:
  441000 of 441000 frames.
- Also checked on the Mac (not committed): mp3.c and dr_mp3 built for
  the host with AddressSanitizer and UBSan, 400 mutated files (bytes
  changed, cut, noise spliced in, headers fuzzed): no error.

### Left for later

- `play -n` on the PC (real songs have been heard there).
- Seeking, a progress line, playing several files or a directory.
- Upstream: the stream-cursor bug above.

## Music player

Status: done, heard on the PC (A2 signed off there on 2026-10-01).

`music start` plays a folder of MP3 and WAV files in shuffle, forever, in
the background: the shell stays free for other commands meanwhile.

### Design: a service init runs

- **bin/music** (`user/services/music/`) is a service init starts in
  shell mode after the mixer, supervised like the others, in a job of its
  own under init's. Nothing of it is in the shell's job, so it plays on
  while the shell runs commands, through Ctrl+C (which only reaches the
  shell's own command) and across a restart of the shell; only
  `music stop` stops it (or `kill music`, after which init starts a new,
  stopped player).
- **Its channel**: `abi/idl/music.idl` (protocol 24: `start`, `stop`,
  `next`, `status`, `set_volume`; jamjar later added `prev`, `play`,
  `levels`, `pause`, `sleep`, `spectrum` and `stereo`:
  [MUSIC-GUI.md](history/MUSIC-GUI.md)). init makes it once and keeps both
  ends, as it does the mixer's, so a restarted player serves the same
  channel and the shell's end (startup role SR_USER + 4) never goes
  stale. The player gets the server end (SR_USER + 0), a client end of
  the mixer's `audio` channel (SR_AUDIO) and init's namespace (SR_NS),
  which init keeps up to date as it does the shell's, so `/data` and
  `/usbN` come and go under it.
- **Idle until started**: it waits on its channel and holds no stream; a
  stream ("music" in `vol`) is open only while it plays.
- **One thread**: while playing, each step reads at most 1024 frames from
  the current file and writes them through `<audio.h>` (blocking at most
  about one mixer period while the ring is full), then answers whatever is
  queued on the channel, so `stop` and `next` act within about 50 ms.
- **The sources** are `<play_src.h>`, moved from the shell into libos for
  this (the format chosen by content, as `play` does). **Gapless**: one
  mixer stream for the whole session; between files `audio_set_input`
  (new in `<audio.h>`) writes out what the old resampler still owes and
  switches it to the next file's rate and channels (the same ones: it
  carries on untouched). **`next`** uses `audio_discard` (new): what the
  mixer hasn't taken is dropped with the 5 ms fade, the stream stays open.
- **What is heard**: the player writes up to the ring's 1.37 s ahead, so
  each track's first frame in the stream is kept as a mark and `status`
  names the track at the mixer's `played` position. `next` skips that
  track; if the writer was already on the following one, that one starts
  over from its beginning (logged "from the start").

### Commands (the shell's `music`, cmd/music.c)

| Command | What |
|---|---|
| `music start [folder]` | default `/data/music`; a relative folder is the shell's (`cd`). Walks it (any depth, at most 16 folders down, 4096 files) for `.mp3` and `.wav` files, any case; names starting with `.` (`.DS_Store`, `._x.mp3`) are left out, folders too. Says `music: playing N tracks from F in shuffle`. A folder that takes more than half a second to read is read on after the answer, a few entries between the player's other calls (`music: reading F (N tracks so far): it plays once it is read`; `music status` shows the count; audio review, item 2) Already playing: the old folder stops (fade) and the new one starts (chosen over "already playing": switching albums is one command) |
| `music stop` | stops with the 5 ms fade: `music: stopped` (or `not playing`) |
| `music next` | skips the track heard now |
| `music status` | `music: playing Artist - Title  1:23 / 3:45`, the file's path, the folder with its track count (and how many were unplayable), tracks started, the volume; stopped: why, if it stopped by itself |
| `music vol <dB>` | its stream's volume (0 dB the most); kept across tracks, stops and starts; `music vol` alone shows it |
| `music prev` | back: the track before the one heard (or the same from its start, after 3 s) |
| `music pause` | pause, or go on (the mixer stream stops where it is) |
| `music sleep <min>` | stop after that many minutes, the last 30 s fading out; `off` turns it off |
| `music` | usage (exit 2) |

**The shuffle**: Fisher-Yates over the list, seeded from the clock (ns
since boot when `start` was typed); every track plays once before the
next shuffle, and a new shuffle never starts with the track just played
(with two or more tracks).

**The title** comes from the path, as music libraries lay files out
(`Artist/Album/N. Title.mp3`): the file's name without its ending and
without a leading track number (`1. `, `01 - `, `7-`; a number followed
by a space only, as in `99 Problems`, stays), and the artist is the
folder above the album's when the file is that deep below the folder
started: `Artist - Title`. Shallower, the title alone. Tags are not read.

**The log**: one line per track as the player starts writing it (about
1.4 s before it is heard): `[music] music: track 3: Drake - Passionfruit
(4:58)`; the console shows log lines above the prompt, so they never break
the line being typed. The kernel log is printable ASCII, so each byte of a
non-ASCII character shows there as `?` (`JA??-Z`); `music status` prints
the name as it is (`JAŸ-Z`).

### When things go wrong

- A file the source refuses (not WAV or MP3 inside, a rate it can't
  play): one log line, skipped, never tried again; when every file is
  refused the player stops (`none of the N files in F is a WAV or MP3
  file`).
- A file that can't be opened or read (the stick pulled: its mount goes,
  reads fail `ERR_PEER_CLOSED`): one line; three files in a row and it
  stops: `stopped: 3 files in a row could not be read (was the stick
  pulled?)`. No retry loop, nothing busy.
- A whole pass of the shuffle that played no frames: it stops.
- A folder with no `.mp3`/`.wav` files: `music start` says so and nothing
  plays.
- The mixer restarting (`kill mixer`, or a crash): the stream's channel
  closes, the player opens a new stream (waiting for the new mixer) and
  the same track goes on (what was queued in the old ring, up to 1.37 s, is
  lost). Three failures in a row without a write between: it stops.
- The player itself dying: init starts it again, stopped.
- Names with spaces, quotes, `$`, `~` and UTF-8 are only ever bytes in a
  path given to the file calls; nothing parses them.

### Tests

`tools/music-test.sh` ([TESTING.md](TESTING.md#area-scripts)). In QEMU on
2026-10-01: seven tracks heard in the log's order before the stop (the
six once each, then a seventh), the 1500 Hz `beep` mixed over a track,
the stop faded (the music's last millisecond at 842 of a tone's 8192), no
dotfile or text file tried, the garbage file skipped, `kill mixer`
reopened in 27 ms and played on, the pulled second stick stopped it 0.2 s
after the pull (`stopped at 0:05: ERR_PEER_CLOSED`, then two `can't open
it (ERR_NOT_FOUND)`), mixer streams closed with 0 underruns and 0 late
periods.

### On the PC

Copy the library to the stick's data partition from the Mac (keep its
folders): `cp -R ~/Music/OnTheSpot/Tracks /Volumes/JAMOS-DATA/music/OnTheSpot`
(the volume may be `NO NAME`). Then, headphones in the front jack:

    music start /data/music/OnTheSpot
    music status
    music next
    music vol -10
    music stop

`music start` alone plays everything under `/data/music`.

### Left for later

- Reading titles from ID3 tags (the path is used); seeking; a queue or a
  playlist file; repeat/no-shuffle modes; resuming after a restart.
- Done since: a folder that takes more than half a second to read is
  read on after `start`'s answer (the audio review, item 2).
