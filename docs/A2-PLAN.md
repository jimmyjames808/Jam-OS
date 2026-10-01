# A2: the audio mixer, the `audio` protocol, WAV playback

A2 runs as two tracks at once: track 1 (WAV playback, below) builds the
client library programs write sound through and the `play` command on
the hda driver's one stream as it is today; track 2 (the mixer) builds
the `audio` protocol and the mixer service, then moves the library's
backend onto it. Each track keeps to its own section of this file.

## Track 1: WAV playback

Status: done in QEMU (branch `a2-wav`); not yet heard on the PC.

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

**The resampler** is linear interpolation between neighbouring input
frames, with the position kept exactly as a count of 1/48000ths of an
input frame (no fixed-point step, so no drift on long files), rounded to
nearest; 48000 Hz input is copied untouched. It is clean for the common
case, upsampling 44100 to 48000; downsampling (96 kHz files) has no
low-pass filter, so anything above 24 kHz in the file aliases. A
windowed-sinc or polyphase filter is the upgrade if that is ever heard.

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

### Tests

- utest `audio_formats`, `audio_resample`, `wav_parse`.
- `tools/play-test.sh` ([TESTING.md](TESTING.md#area-scripts)): seven
  files on the stick image, played in QEMU into a WAV capture and checked
  (frequency per channel, length within 2 %, mono/stereo, silence after,
  no clicks, `-v`, refusals, Ctrl+C with a fade).

### Left for later

- Heard on the PC: `play` of a real song; whether 85 ms periods and a
  341 ms ring are enough with a slow stick (underruns are counted but
  not shown yet).
- A low-pass filter for downsampling; float WAV (`afconvert -d LEI16`
  avoids it); `play` of several files or a directory; a progress line.
- Two programs at once: track 2's mixer.
