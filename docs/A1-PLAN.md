# A1 plan: HD Audio, a tone in the front-panel headphones

Goal ([roadmap](ROADMAP.md#later)): **`beep` in the shell plays a tone in
the headphones plugged into the case's front panel on the real PC, and
unplugging and replugging them is logged.** The driver is a process like
every other ([ARCHITECTURE.md](../ARCHITECTURE.md#audio)); the kernel gains
nothing.

The source for every register, bit and verb is the Intel High Definition
Audio Specification, revision 1.0a (2010); the driver's comments cite its
sections. Linux's `snd-hda-*` and ALSA are GPL: read for hardware facts
only if a quirk needs it, never copied or paraphrased
([CODING-GUIDE.md](../CODING-GUIDE.md#licence-and-outside-code)).

## The hardware

From the PC's own boot log ([HARDWARE.md](HARDWARE.md#other-devices)):

| Function | What | A1 |
|---|---|---|
| 00:1f.3 | Intel Raptor Lake PCH HD Audio, 8086:7a50 rev 11, class 04 03 00 (HDA mode, not the audio DSP's 04 01 / 04 03 80). MSI (1 vector, 64-bit), no MSI-X. BAR0 mem64 16 KiB (the HDA registers), BAR4 mem64 1 MiB (the DSP's, unused in HDA mode) | **the target** |
| 01:00.1 | NVIDIA HDMI/DP audio on the RTX, 10de:22bb | never: left without a driver, like any device devmgr has no driver for |

Stage 0's probe found the codec on the PC (boot log of 2026-10-01): a
**Realtek ALC897** (10ec:0897, subsystem 1043:8841, ASUS) at codec
address 0, every widget in D0. The front-panel headphone jack is **pin
1b** (configuration default 02214020: jack, external front, hp-out,
presence detection with a trigger, EAPD), and the path to it is **DAC 02
-> mixer 0c -> pin 1b**. STATESTS also shows codec address 2, which never
answers (most likely the disabled iGPU's HDMI codec): it is skipped. The
dump itself is the ALC897 fixture in `drivers/hda/fixtures.c`. Whether
the jack's presence detection and unsolicited responses work is still
for stage 4 to find out.

## Fixed decisions

**One driver process per controller, `drivers/hda`** (drv/hda), bound by
devmgr to vendor 8086 with class 04 03 00. Other vendors' HD Audio
functions stay unbound (the RTX's HDMI audio is not planned), and
04 03 80 (Intel's DSP) needs firmware Jam OS doesn't have.

**Commands go through the CORB/RIRB rings** (spec chapter 4), in one pinned DMA32
page: the CORB (256 x 4 bytes) at 0, the RIRB (256 x 8 bytes) at 2 KiB.
The immediate command interface (the ICOI/ICII/ICIS registers) is optional in
the spec and cannot carry unsolicited responses, and jack detection is
made of unsolicited responses. Stage 0 falls back to it only if the rings
don't answer, and says so, so the first PC run yields a dump either way;
from stage 1 on the rings are the path. One command is in flight at a time
(a verb is answered in microseconds; nothing needs more).

**Only a typed set of SET verbs.** `hda_get` refuses anything but GET
verbs (stage 0 has nothing else). Stage 1 adds `hda_set` with an
allow-list: connection select, amplifier gain/mute, pin widget control,
EAPD, power state, converter format and stream/channel, unsolicited
response enable, pin sense trigger. Never the configuration default (the
board's own description of its jacks), never a function-group reset,
never vendor coefficients unless the PC shows the codec needs one, and
then named in the plan first.

**Interrupts: MSI, bound to a port as every driver does; position from the
DMA position buffer.** The controller's one MSI carries both RIRB
responses (jack events) and stream buffer completions (IOC). The driver
binds it PERSISTENT to its port, acks the object, then reads INTSTS,
RIRBSTS and SDnSTS and clears what it saw (the edu pattern). The play
position comes from the DMA position buffer (DPLBASE: the controller
writes each stream's position to memory), read at each IOC,
with LPIB as the check. The port wait always has a deadline of one period:
a lost interrupt costs one period of latency, never a stall. Stage 0 uses
no interrupt at all (it polls the RIRB write pointer).

**One output stream, from a pinned DMA32 buffer** (the stream
descriptor registers and the Buffer Descriptor List, spec chapters 3
and 4):
- the first output stream descriptor (index ISS), stream tag 1;
- 48 kHz, 16-bit, stereo (format 0x0011): every HDA codec has it, and the
  mixer (A2) converts. Since the audio quality pass: 16, 20, 24 or 32
  bits as the client asks and the DAC's P_PCM allows (20-32 in 32-bit
  containers, left-justified; the mixer asks the largest: 24 on the PC,
  format 0x0031), capped by `hda bits`;
- a contiguous DMA32 ring of 16384 frames (341 ms: 64 KiB at 16-bit,
  128 KiB in 32-bit containers), pinned for the stream's life; a Buffer
  Descriptor List of 8 entries (2048 frames each, with IOC: a period
  every 42.7 ms; it was 4 of 85 ms before the quality pass), in a second
  DMA32 page with the position buffer;
- setup: SRST 1 then 0 (each read back, bounded), CBL, LVI, BDL address,
  format, tag, then the codec's converter (SET_STREAM_CHANNEL,
  SET_CONVERTER_FORMAT), then RUN;
- Intel's TCSEL (PCI config 0x44, bits 2:0) set to TC0, a register the
  driver may write through its filtered config access.

**The ring is shared with the client, and the driver clears behind the
hardware.** The client maps the ring (READ | WRITE | MAP, no other
rights: it can't resize a pinned VMO anyway) and writes ahead of the play
position; at each period the driver zeroes the period just played. A
client that stops writing therefore makes silence within one ring, never
a loop of the last 341 ms. A stream with no client (the channel closed)
is stopped.

**The path from a DAC to the front headphone jack** (spec chapter 7):
1. the pin: a pin complex whose configuration default says jack (port
   connectivity not "none"), location external front, default device
   headphone out. If there are several, the lowest sequence in the lowest
   association. If there is none (a board whose firmware describes its
   jacks badly), any headphone-out pin, then a line-out, then a speaker
   (QEMU's codecs have only those); if that picks the wrong jack on some
   board, the fix starts from its dump, added as a fixture;
2. the path: a breadth-first walk back from the pin through connection
   lists (at most 5 widgets deep), across mixers and selectors only and
   never into one whose connection list holds a pin (an input or loopback
   mixer, like the ALC897's 0b), to an analog output converter; a DAC no
   other pin with its output on uses wins, then the shortest path, then
   the lowest DAC node (`drivers/hda/path.c` has the rules);
3. power: the audio function group to D0 (SET_POWER_STATE, then
   GET_POWER_STATE until it reads D0, bounded), then every widget on the
   path that has power control;
4. routing: SET_CONNECT_SEL on each selector (and the pin, if its list has
   more than one entry) to the next widget on the path; on a mixer the
   input amp of the path's input unmuted, every other input of that mixer
   muted (no analog loopback noise);
5. gains, quietly: output amps unmuted at 0 dB (the step `offset`) or
   lower, the DAC's own amp at -20 dB for the first PC run (a tone into
   headphones at full scale is loud); `hda gain` and A2 change it;
6. the pin: SET_PIN_WIDGET_CONTROL out enable + headphone amp (0xc0), EAPD
   on if the pin has it.
Everything else is left as the link reset left it. The order at stop is
the reverse: pin output off, amps muted, stream stopped.

**Jack detection by unsolicited responses** (the Unsolicited Response and Pin Sense verbs):
GCTL.UNSOL on, each jack pin's SET_UNSOLICITED_ENABLE with a tag of its
own, the RIRB interrupt on. An unsolicited response carries the tag in
bits 31:26; the driver then reads GET_PIN_SENSE (bit 31), debounces it and
logs `headphones plugged in (front, pin 1b)` or `headphones unplugged
(front, pin 1b)`. A pin that needs a trigger (pin caps bit 1) gets
SET_PIN_SENSE first. If a pin can't send unsolicited responses, or the PC
shows a change without one (a front panel wired AC'97-style has no sense
line), the fallback is to read the pin sense every 500 ms and log changes;
which one runs is in the log. QEMU's codecs have neither presence
detection nor unsolicited responses, so this is proven on the PC only
(QEMU checks the logic against fixtures and a fake codec).

**Supervision.** A driver that dies is restarted by devmgr like any other.
Its dma_cap closing turns Bus Master Enable off, so a running stream can
no longer read memory (at most the codec's FIFO plays out, a click);
the pins are quarantined by the kernel until the new driver has reset
the controller (which stops every DMA engine) and turned bus mastering
back on. The new driver starts from scratch: link reset, codecs read,
path programmed; the stream is not restarted by itself. Clients follow
the reconnect rule (`hda.idl`): on ERR_PEER_CLOSED they ask devmgr again
and reopen the stream. A clean exit (devmgr stopping) mutes, stops the
stream, stops the rings and puts the controller back in reset.

**`beep`.** A shell command, `beep [hz] [ms]` (default 440 Hz, 300 ms,
at most 5 s; Ctrl+C stops it): it opens the output stream on the hda
driver, writes a sine (with a 5 ms fade in and out, so no click) ahead of
the play position, waits for it to play, and closes the stream. The sine
is made in the shell (a user program may use floating point); the driver
never makes sound of its own. In A2 `beep` becomes a client of the mixer
instead, unchanged for the user.

## How A2 builds on it

A1's `hda` protocol is the device-level contract, and A2's mixer is meant
to be its only client, so it is designed as the mixer needs it now:

| Method (abi/idl/hda.idl) | Stage | What |
|---|---|---|
| `dump() -> (text VMO, length, codecs)` | 0 (done) | the graph, read now |
| `info() -> (rates, formats, pin, dac, gain steps, jack state)` | 1 (done) | what the driver chose and can do |
| `open_output(rate, channels, bits) -> (stream channel, ring VMO, size, period)` | 2 (done) | the ring above; one stream at a time (a second open: ERR_BAD_STATE); the stream methods below are served on the `stream` channel it returns, and closing that channel closes the stream |
| `start()`, `stop()` | 2, 3 (done) | RUN on/off; the path unmuted just before RUN and muted again right after it clears (stage 3) |
| `position() -> (u64 frames played, u32 ring offset)` | 2 (done) | from the position buffer |
| `wait_period(u64 after) -> (u64 frames played, u32 ring offset)` | 2 (done) | answers once the period holding frame `after` has played: the client's write-ahead clock |
| `set_gain(i32 centibels)`, `get_gain` | 3 (done) | the codec's output amp on the path (the DAC's), rounded to its step, clamped to its range and to 0 dB; both answer the gain, the step and the range |
| `jacks() -> (count, state, changes, pins, states, text)` | 4 (done) | every jack with presence detection: plugged / unplugged / unknown, how it is watched; the path's pin's state and change count (also `info`'s `jack`) |

A2 then adds the `audio` protocol (programs' streams, each its own shared
VMO ring) served by a mixer service that holds the one `hda` output stream,
mixes, and sets the volume through `set_gain`; `beep` and WAV playback
move to it. devmgr then hands `hda`'s channel to the mixer only (today
GET_SERVICE gives it to the shell). Nothing in A1's methods changes: A2 is
new methods (e.g. more rates) or a new protocol, never a rework.

## Stages

Each is about one agent-hour, builds on main, and hands back when its
tests pass. Plumbing lands before its users. Stages 1 and 2 touch
different files and can run as two tracks at once.

| Stage | What | Files it owns |
|---|---|---|
| **0. Probe** (done) | read-only: reset, rings (immediate fallback), codecs from STATESTS, each codec's graph logged (one line per widget, two for pins), RESULTS line, `hda.dump`, the shell's `hda`; devmgr binds 8086 / 04 03 00; `kill hda` reaches PCI drivers | `drivers/hda/{main,ctrl,graph,dump}.c`, `hda.h`, `abi/idl/hda.idl`, the shell's `cmd/hda.c`, `tools/hda-test.sh`, `tools/shell-tests/hda.txt` |
| **1. Codec control** (done) | `hda_set` with its allow-list; power-up; the path finder (`path.c`, a pure function over `struct codec`) with a self-test the driver runs at start against fixtures: QEMU's hda-output and hda-duplex, and **the PC's codec as the stage 0 dump showed it**; the path programmed with every amp still muted and the pin output off (no sound possible yet); `hda.info`; `hda` shows the chosen path | `drivers/hda/{verbs,path,fixtures}.c`, `hda.idl` (info) |
| **2. Output stream** (done) | the stream descriptor, BDL, position buffer, the 64 KiB ring, MSI (IOC and RIRB) through the port, clear-behind, `open_output/start/stop/position/wait_period`, the stop order at exit and at client close; TCSEL | `drivers/hda/{stream,irq}.c`, `hda.idl` (stream methods), a test program user/tests/hdatest/ (new) |
| **3. `beep`** (the join of 1 and 2, done) | the path unmuted at the quiet default gain, `set_gain`/`get_gain`, the shell's `beep` and `hda gain`; the QEMU tone test | user/services/shell/cmd/beep.c (new), tools/beep-test.sh (new), `drivers/hda/main.c` |
| **4. Jacks** (done in QEMU) | unsolicited responses on (GCTL.UNSOL, the pin's enable, the RIRB interrupt), the tag -> pin table, the plugged/unplugged log lines, the polling fallback, `hda.jacks`, `hda` shows the jack state | drivers/hda/jack.c (new), `ctrl.c` (the RIRB's demultiplexer), `irq.c`, `hda.idl` (jacks) |
| **5. Review** | the independent review-and-fix pass over all of A1 (standing rule), then the PC sign-off | whatever its findings touch |

## Stage 2: what was built and learned

- **The stream has a channel of its own.** devmgr hands every client a
  duplicate of the one DR_SERVE channel, so "the channel that opened it"
  can't be told apart from the others and its close is never seen.
  `open_output` therefore returns a new channel (`stream`) with the ring;
  start, stop, position and wait_period are served on it (refused on
  DR_SERVE), and its peer closing (the client closes it, or dies) stops
  and releases the stream. A2's mixer holds it.
- **Clear-behind is per byte, at every position read** (every interrupt,
  every request, and at least once a period), not per period: the driver
  zeroes exactly what has played since the last read, before it tells
  anyone the new position. So a client may write anywhere in
  [position, position + ring) and its data is never zeroed; clearing
  whole periods would wipe what a client wrote into the played part of
  the period in progress.
- **The converter's format and stream tag are the only SET verbs**
  (`hda_converter_set`, which refused any other), sent at `open_output`
  (and stream 0 at the close) to the first analog DAC of the first codec
  that has one: DAC 02 on QEMU's codecs and on the PC's ALC897, the same
  DAC the path finder is expected to choose. At the join (stage 3) the
  stream took the DAC from stage 1's path and its two SETs moved onto
  `hda_set`'s allow-list, so there is one command gate. The probe itself still sends only GETs (`tools/hda-test.sh`,
  unchanged, 524 verbs).
- **MSI for the stream only.** INTCTL gets GIE and the stream's bit while
  a stream is open; CIE (the RIRB's interrupt) stays off, since commands
  are polled and answered in microseconds. Stage 4 turns CIE on for jack
  events; irq.c is where they will arrive.
- **The stop order** at a close and at the driver's exit: RUN clear
  (waited for), SRST 1/0, the stream's interrupt off, DPLBASE off, the
  converter to stream 0, then the pins released. If RUN never clears the
  pins are kept (logged): the dma_cap's close quarantines them.
- **A kill mid-stream** (QEMU): the dma_cap's close turns bus mastering
  off and quarantines the ring, the BDL page and the command-ring page
  (3 pins, 18 pages); the restarted driver's reset finds the stream
  descriptor still running ("stream 4 was running: stopping it") and
  stops it before turning bus mastering on; the quarantine is released
  1 s later with no page written.
- **In QEMU** the capture is exact (the one-second pattern, sample for
  sample), the position advances at 48.17 kHz by the guest's clock, the
  position buffer and LPIB agree while running (QEMU does not reset LPIB
  at SRST, only at RUN, so the gap is sampled while running only), and
  there is one interrupt per period. QEMU's codec keeps its converter's
  stream tag across the link reset; a real codec resets it.

**What only the PC can show** (hdatest from the shell, or `hdatest` in
the log after it): the MSI arriving (the close lines count "period
interrupt(s)": about one per 42.7 ms), the position buffer against LPIB
("position buffer vs LPIB up to N bytes": expect a FIFO's worth or less)
and against Intel's DPIB register ("vs DPIB up to N bytes"),
the FIFO size (the open line), TCSEL (a "TCSEL was TCn" line only if the
firmware left it non-zero), the rate hdatest measures (48 kHz within
2 %) and the kill test's quarantine line. Nothing is heard: no path is
programmed until stage 3.

## Tests

| Stage | QEMU | Only the PC |
|---|---|---|
| 0 | `tools/hda-test.sh`: two emulated controllers (intel-hda with hda-duplex + hda-output, ich9-intel-hda with hda-micro): each graph in the log, `hda` from the shell, `kill hda` and devmgr's restart, **every verb the codecs got is a GET** (QEMU's codec trace, `debug=3`), and the `init` run with them: each driver stops cleanly with its controller in reset | the real controller resets; the rings (or the immediate interface) answer; the real codec's graph |
| 1 | the path self-test on every fixture (QEMU's in QEMU; the PC's fixture too, since it is data); the chosen path for hda-output is DAC 02 -> pin 03; the codec trace shows only allow-listed SETs and no amp unmuted | the path found on the real codec is the front headphone jack (the owner reads it off the `hda` output) |
| 2 | `hdatest`: a known pattern through the stream, captured by QEMU's wav backend (`-audiodev wav,id=snd0,path=<file>`, available in this QEMU; the codec with `mixer=off`), compared sample for sample after the leading silence; position advances at 48 kHz within 2 %; the client closing mid-stream stops it; kill mid-stream: restart, quarantine released, no stale DMA | interrupts and the position buffer on the real controller |
| 3 | tools/beep-test.sh (new): `beep 440 500` into the wav file; its zero crossings give 440 Hz within 1 % over 500 ms within 30 ms, fades present, silence afterwards | **the tone in the headphones** (done-when) |
| 4 | the jack self-test at every driver start: the tag -> pin table on the fixtures, the RIRB's demultiplexer on a fake RIRB, the debounce, unsolicited responses and the fallback poller against a fake codec (which also checks jack code sends only 0x708, 0x709 and GET_PIN_SENSE); tools/hda-test.sh: no jack verb to QEMU's codecs, the RIRB interrupt taken while dumps stay right | **unplug/replug logged** (done-when) |
| all | `make`, `make KTESTS=0`, `make check`; the `init` run clean; `tools/usb-test.sh` and `tools/storage-test.sh` (devmgr changes) | `soak 2` after each round |

The ktests do not change (the driver is a process); they are run once per
stage anyway, at 4 CPUs.

## On the PC after stage 0

1. `make flash`; boot the everyday entry (Jam OS).
2. Nothing to do on the PC but wait for the shell (the probe runs at boot;
   `hda` shows it again). Optionally plug headphones into the front jack
   before boot, then run `hda` again after pulling them, so the dump shows
   the pin's presence bit both ways.
3. `reboot` (so logd saves the tail), plug the stick into the Mac, and read
   `/data/logs/boot-NNNN.txt` (the newest): every line with `[hda]`, and
   the RESULTS line starting `hda: controller 8086:7a50`.
4. From those lines stage 1 gets: the codec's vendor and device id, which
   command path worked, the widget list, the front-panel pin (its
   configuration default and presence detection) and the connection lists
   from it back to a DAC.

If the probe fails on the PC, the lines to bring back are the same; a
failed reset or a codec that doesn't answer says so in them, and the
RESULTS line says `start failed` with the reason.

Known ways it could fail there, and what the lines would show:
- **No codec in STATESTS** (`codecs 0x0000`): newer Intel PCH controllers
  have dynamic clock gating and power gating controls in their vendor PCI
  registers that some drivers turn off around the link reset. The probe
  logs PCI config 0x40-0x4f (the `pci config 40-4f` line) so stage 1 can
  check them against Intel's PCH datasheet before writing anything there.
- **The rings don't answer** but the immediate interface does: the
  controller line says `commands through the immediate interface`; stage 1
  then looks at the CORB/RIRB setup (sizes, RINTCNT) before building on
  it, since jack detection needs the RIRB.
- **A verb times out** now and then: the last line counts them. A late
  answer could be taken for the next verb's (the RIRB is read in order and
  only the codec address is checked); stage 1 drains the RIRB after a
  timeout if the count is not 0.

## What stage 1 built and learned

- **One way to a codec.** `drivers/hda/verbs.c` is the only caller of the
  raw send (ctrl.c's `hda_command`). `hda_get` takes GET verbs;
  `hda_set` takes, each with only the payload bits the spec defines for
  it: connection select, power state (D0-D3, never D3cold), converter
  stream/channel, pin widget control, unsolicited enable, pin sense,
  EAPD/BTL, converter format (PCM only) and amp gain/mute (naming an amp
  and a side). Anything else is refused and logged: the configuration
  default, the function group reset, GPIOs, the subsystem id, beep,
  digital converter controls, vendor coefficients.
- **The path, silent.** At start the driver runs the path self-test (7
  fixtures: QEMU's hda-output, hda-duplex and hda-micro, and the ALC897
  with three variations: the rear line-out playing, the front jack not
  described, a pin with no connection), finds each codec's path, checks
  that its own dump of each live codec parses back to the same path, and
  sets up the best one: the AFG and the path's powered widgets to D0
  (waiting for D0), the pin's output and headphone bits off, every amp on
  the path muted at gain step 0 (every input of a mixer on it too), the
  path's connection selects. The DAC's stream and format are left for
  stage 2. A failed self-test or round trip sets nothing up.
- **The ALC897's DACs have no mute** (out-amp 0-87, 0.75 dB steps, 0 dB
  at 87, no mute bit): at step 0 they are at -65.25 dB. What keeps the
  path silent there is mixer 0c's input mute, pin 1b's out-amp mute and
  the pin's output being off. Stage 3 unmutes in the order of step 5.
- **Mixer 0c is shared.** It is the only input of the rear green
  line-out pin 14 and the selected input of pins 18, 19 and 1a. Any of
  them with its output on would play the headphones' sound too; all are
  off (pin control 0x20, input only), and the driver leaves them so. The
  path line lists them ("pins that select a node of it too").
- **QEMU's codecs ignore SET_PIN_WIDGET_CONTROL**: their output pins read
  back 0x40 (output on) whatever is set. The driver logs "kept its output
  on"; tools/hda-test.sh checks from QEMU's own verb trace that the SET
  asked for the output off. Their AFG reports no power states, but
  GET_POWER_STATE reads D0.
- `hda.info` returns the path's codec, pin, DAC, the DAC's PCM rates,
  formats and amp capabilities, the node list and a line of what is set
  on each node (read back); the shell's `hda` prints it after the dump.
- Not built: the `hda_pin=<nid>` boot word (the PC describes its front
  jack, so nothing needs it yet).

On the PC the path is now set up, still silent. `hda` and the boot log
should show (the first line is what the ALC897 fixture gives; the second
is read back from the codec, so its amp values are the expected ones,
not yet seen)

```
codec 0 path: dac 02 -> mixer 0c -> pin 1b (front headphone jack); pins that select a node of it too: 14 18 19 1a
path: codec 0 dac 02 -> mixer 0c -> pin 1b (front headphone jack), muted: afg D0; dac 02 D0 out 0; mixer 0c in m0 m0; pin 1b D0 sel 0 ctl 20 (output off) out m0 in 0 eapd off
```

and the RESULTS line ends `path 02-0c-1b muted`. A line starting `path
self-test:` other than "7 of 7 fixture(s) passed", or one saying a dump
"parses back to another path", means nothing was set up.

## What stage 3 built and learned

- **The join.** Stage 2's stream now plays to the DAC of stage 1's path
  (no path set up: `open_output` fails ERR_NOT_FOUND), and its two
  converter SETs go through `hda_set`'s allow-list: `hda_converter_set`
  is gone, so verbs.c is the one gate for every verb.
- **The path is open only while the stream runs.** `start` opens it just
  before RUN (verbs.c `hda_output_open`, steps 4-6 above); `stop`, the
  stream's close and the driver's exit close it right after RUN clears.
  On the PC's ALC897 the opening is, in order: mixer 0c input 0 unmuted
  (`3 0c 7000`: its amp has only a mute), DAC 02 output amp to the gain
  (`3 02 b02f`, step 47: -30 dB), pin 1b output amp unmuted (`3 1b b000`),
  pin 1b control 0xc0 (`707 1b c0`: output + headphone amp, since its pin
  caps have HP drive) and EAPD on (`70c 1b 02`). The closing: EAPD off
  (`70c 1b 00`), pin control back to what the muted set-up left (0x20,
  input only), every output amp muted at step 0 (`3 1b b080`, `3 02 b080`:
  the DAC has no mute, so step 0, -65.25 dB) and mixer 0c input 0 muted
  (`3 0c 7080`). The other inputs of the mixer are never unmuted. After
  opening, the driver reads back the pin control, EAPD and the DAC amp and
  logs them.
- **If a verb fails while opening**, it is logged by name, the path is
  muted again (every closing verb tried) and `start` fails with its
  status; `beep` says the path stayed muted.
- **The gain**: -30 dB at every driver start (on the ALC897 DAC 02 step
  47 of 0-87, 0.75 dB steps, 0 dB at 87; on QEMU's codecs step 44 of
  0-74). `set_gain` takes centibels, rounds to the nearest step and clamps
  to the amp's range and never above 0 dB; it is sent at once if a stream
  plays. The volume amp is the first on the path with gain steps (the
  DAC's on both codecs); the path self-test checks which one, its default
  step, its range and the clamping on every fixture.
- **`beep` writes at -12 dBFS** (a quarter of full scale) with 5 ms linear
  fades, so with the default gain a beep leaves the DAC at about -42 dBFS.
  The sine comes from a rotating phasor in doubles (renormalised every
  1024 frames), set up with a series for sin and cos: no libm.
- **hdatest turns the gain to its lowest** for its run (its pattern is a
  near full-scale sawtooth, a test signal) and puts it back; on QEMU's
  mixer=off codec there is no gain, so nothing changes there.
- **In QEMU** (tools/beep-test.sh, the codec's mixer on, so its amp
  scales the samples): `beep 440 500` gives 440.00 Hz, 500.0 ms, peak
  4850 (a quarter of full scale at QEMU's linear volume for step 44),
  fades (the first and last 2.5 ms reach about a third of the peak), no
  clicks, silence after; the codec got only allow-listed verbs, the path
  opened only while the converter had the stream's tag, closed before it
  was released, and nothing open at the end. A driver killed mid-stream
  can't mute: its successor's set-up does, before anything else.

**On the PC** (the owner, after `make flash`). The first beep is loud
enough to hear and not more, but a first beep on new code is still a
first: headphones' own volume (if they have one) down, headphones off
the head, near enough to hear.
1. Plug the headphones into the front jack. `hda | grep -E "path|gain"`
   should end with `hda: path: codec 0 dac 02 -> mixer 0c -> pin 1b (front
   headphone jack), muted: ...` and `hda: gain -30.0 dB (step 47; -65.3
   to 0.0 dB), heard only while a stream plays`.
2. `beep`: a 440 Hz tone for 0.3 s; the shell says `beep: 440 Hz for 300
   ms at -30.0 dB`. The log (`log 6`) shows `[hda] stream: open on
   descriptor 7, tag 1, format 0x0011, converter 0/02`, `[hda] output:
   unmuted at -30.0 dB (node 02 step 47); read back: pin 1b ctl c0 eapd
   02, volume amp 2f`, `[hda] output: muted again` and the stream's close
   line.
3. Louder or quieter: `hda gain -20` (or -40), then `beep` again. 0 dB is
   the most the driver allows; `hda gain` alone shows it.
4. If nothing is heard: the read-back line says what the codec took (ctl
   should be c0, eapd 02, volume amp 2f); `hdatest` checks the stream
   itself (now quiet: it turns the gain down).

## What stage 4 built and learned

- **One demultiplexer for the RIRB** (ctrl.c, `hda_rirb_sort`). Every
  entry goes through it, whether a command reads it while waiting for its
  answer or the RIRB interrupt drains it: an unsolicited entry (the high
  word's bit 4) is queued for jack.c (32 deep; overflow counted) and is
  never an answer; a solicited one is the answer only if a command to its
  codec waits, else it is counted late and dropped. Every command first
  drains what is already in the RIRB, so a late answer to a command that
  timed out is dropped before the next command goes out (stage 1's open
  item). What can still be confused: an answer later than the 100 ms
  timeout landing between the next command's send and its answer.
- **Commands stay polled; the RIRB interrupt is for the rest.** When the
  loop starts with the rings and the MSI bound, GCTL.UNSOL and INTCTL.CIE
  go on (also in QEMU and with no jack, so every test runs this way).
  RINTCNT stays 1, so the interrupt also fires for every answer, which the
  drain then finds taken: a wake per verb, and it proves the interrupt
  works before any jack needs it. An interrupt with no status bit while
  CIE is on counts as the RIRB's (a polling command cleared RINTFL first).
- **The jacks** (jack.c): every pin whose configuration default says a
  jack (connectivity jack, or jack and fixed), with no "no presence
  detection" bit, and whose pin capabilities have Presence Detect. On the
  PC that is seven, with tags in node order: 14 rear green line-out (1),
  15 rear black line-out (2), 16 rear orange line-out (3), 18 rear mic (4),
  19 front mic (5), 1a rear line-in (6), 1b front headphones (7). The
  S/PDIF pin 1e says "no detect" and is not one. All seven have Unsol
  Capable and Trigger Required, none impedance sensing.
- **The trigger.** The spec ties SET_PIN_SENSE (Execute) to the impedance
  measurement and gives no settle time; presence is a level the codec
  samples by itself. The driver triggers every trigger pin of a read at
  once, waits 1 ms, then reads them all (the debounce re-reads anyway).
- **The debounce**: a read that differs from the jack's state is held;
  it is read again 80 ms later and kept only if it still differs; a read
  that agrees in between cancels it. A 20 ms flicker gives no line.
- **Polling, per jack**: a pin without unsolicited responses is read
  every 500 ms. One with them is polled too until its first unsolicited
  response, with the RIRB interrupt seen working, proves them ("jack: pin
  1b sends unsolicited responses: no longer polled"). A change the poll
  finds with no response from that pin since its last change means they
  don't work there: it is polled for good ("... changed without an
  unsolicited response: polled every 500 ms from now on"). So the log says
  which mode each jack ended in; **on the PC the front headphones should
  end in unsolicited responses** (Realtek codecs send them, and the front
  panel's sense line is wired: the configuration default says presence
  detection); polling is the fallback if the panel is wired AC'97-style.
- **Nothing else changes on a plug.** An unplug while playing is logged
  and nothing more: the stream and the mixer keep running, and no amp or
  pin control is touched by jack code (the self-test's fake codec
  refuses anything but 0x708, 0x709 and GET_PIN_SENSE).
- At a clean exit unsolicited responses are turned off on each pin, then
  GCTL.UNSOL and CIE; the exit line counts the stream's and the RIRB's
  interrupts, the unsolicited responses and the late answers.
- `hda.jacks` (IDL 11) answers each jack's state and a text line per jack;
  `info`'s `jack` is now the path's pin's state; `hda` ends with the
  `hda: jack ...` lines and `hda jacks` prints only them.

**On the PC** (the owner, after `make flash`; boot the everyday entry):
1. Boot with the headphones out of the front jack. `dmesg | grep jack`
   should show `[hda] jacks: 7 with presence detection; unsolicited
   responses on for all; polled every 500 ms until each one's first
   response`, then one line per jack, e.g. `[hda] jack:
   headphones (front, pin 1b): unplugged; unsolicited responses on, polled
   until one comes, tag 7`.
2. Plug the headphones into the front jack: within about 0.1 s `[hda]
   jack: pin 1b sends unsolicited responses: no longer polled` (first time
   only) and `[hda] headphones plugged in (front, pin 1b)`. Pull them out:
   `[hda] headphones unplugged (front, pin 1b)`. Do it twice more.
3. `hda jacks`: the first line counts the unsolicited responses received
   and the RIRB interrupts; the pin 1b line should say `plugged in` or
   `unplugged` as they are, `6 change(s)` (or more) and `unsolicited
   responses, tag 7`. `dmesg | grep plugged` lists every change.
4. If instead `jack: pin 1b changed without an unsolicited response:
   polled every 500 ms from now on` appears, the plug is still logged (up
   to 0.6 s late): bring that line and `hda jacks` back. If no plug line
   appears at all, bring `hda jacks` and `hda | grep "c0 1b"` back (the
   dump should end `unsol on tag 7`).
5. Optional: plug and pull the front mic (19) or a rear jack; each logs
   its own line (`microphone plugged in (front, pin 19)`, `line-out
   unplugged (rear green, pin 14)`).
6. Optional: `play` something and pull the headphones mid-song: the line
   is logged and the song plays on (to nothing).

## Done when

`beep` in the shell plays a tone in the front-panel headphones on the
real PC; unplugging and replugging them is logged; `hda` shows the path
and the jack state; the tests above pass in QEMU; the review's High and
Medium findings are fixed; `soak 10` on the PC passes with the driver
bound.
