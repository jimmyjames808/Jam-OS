# A2 plan: the mixer and the `audio` protocol

Goal ([roadmap](ROADMAP.md#later)): **two programs play at once**, each
through its own stream, with volume from the shell and WAV playback from
`/data`. A1's driver serves one output stream ([A1-PLAN.md](A1-PLAN.md),
"How A2 builds on it"); A2 puts a mixer service in front of it.

Two tracks ran at once: track 1, WAV playback (the client library
`user/include/audio.h`, `play`, `beep` on it), and track 2, below.

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
  `mixtest`) also get `SR_AUDIO_CTL` and init's control channel.
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
| `start`, `stop` | stream | the mixer takes frames from the ring (from the next period it mixes) or leaves them there |
| `drain() -> (frames)` | stream | answers once everything written before the call has played |
| `position() -> (written, consumed, played)` | stream | frames: in the ring, taken by the mixer, heard (estimated from the driver's position) |
| `set_volume(cb) -> (cb)` | stream | this stream's volume |
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

### Done when

Two programs play at once through the mixer in QEMU (above), `vol` works
from the shell, beep and play go through the mixer, and on the PC two
sounds are heard at once.

### For AS (the boot splash)

The splash gets `SR_AUDIO` from init. Its `open_output` may come before
the mixer has found the driver: the call waits for the mixer (init holds
the channel), and the mixer opens the driver when the splash starts its
stream. For lip sync it uses `position().played` (the mixer's estimate
of what is heard, to within a period's interpolation), not what it wrote.
