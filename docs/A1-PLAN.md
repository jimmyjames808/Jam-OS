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

The codec on the Intel link is not known yet (a Realtek ALC8xx is
likely); nor is how the front-panel jack is wired to it, nor whether its
jack detection works. **Stage 0 (below, done) is a read-only probe whose
only job is to find that out on the PC.** Everything from stage 1 on is
written against its dump.

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
  mixer (A2) converts;
- a 64 KiB contiguous DMA32 ring (341 ms), pinned for the stream's life;
  a Buffer Descriptor List of 4 entries of 16 KiB, each with IOC (a period
  every 85 ms), in a second DMA32 page with the position buffer;
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
   jacks badly), the plan asks the owner, with the dump, which node to use,
   and a boot word `hda_pin=<nid>` overrides the choice;
2. the path: a breadth-first walk back from the pin through connection
   lists (at most 5 widgets deep), to an analog output converter; the
   shortest path wins, then the lowest DAC node;
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
GCTL.UNSOL on, the pin's SET_UNSOLICITED_ENABLE with a tag, the RIRB
interrupt on. An unsolicited response carries the tag in bits 31:26; the
driver then reads GET_PIN_SENSE (bit 31) and logs `hda: headphones
plugged (node xx)` or `unplugged`. A pin that needs a trigger (pin caps
bit 1) gets SET_PIN_SENSE first. If the dump shows the front pin has no
presence detection, or the PC shows no unsolicited responses (a front
panel wired AC'97-style has no sense line), the fallback is to read the
pin sense every 500 ms and log changes; which one runs is in the log.
QEMU's codecs have neither presence detection nor unsolicited responses,
so this is proven on the PC only.

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
| `info() -> (rates, formats, pin, dac, gain steps, jack state)` | 1 | what the driver chose and can do |
| `open_output(rate, channels, bits) -> (ring VMO, size, period)` | 2 | the ring above; one stream at a time (a second open: ERR_BAD_STATE); closing the channel that opened it closes the stream |
| `start()`, `stop()` | 2 | RUN on/off (with the mute ordering) |
| `position() -> (u64 frames played, u32 ring offset)` | 2 | from the position buffer |
| `wait_period(u64 after) -> (u64 frames played, u32 ring offset)` | 2 | answers once a period past `after` has played: the client's write-ahead clock |
| `set_gain(i32 centibels)`, `get_gain` | 3 | the codec's output amp on the path, clamped to its steps |
| `jack() -> (u8 state, u64 changes)` | 4 | plugged / unplugged / unknown, and a change count |

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
| **1. Codec control** | `hda_set` with its allow-list; power-up; the path finder (`path.c`, a pure function over `struct codec`) with a self-test the driver runs at start against fixtures: QEMU's hda-output and hda-duplex, and **the PC's codec as the stage 0 dump showed it**; the path programmed with every amp still muted and the pin output off (no sound possible yet); `hda.info`; `hda` shows the chosen path | `drivers/hda/{verbs,path,fixtures}.c`, `hda.idl` (info) |
| **2. Output stream** | the stream descriptor, BDL, position buffer, the 64 KiB ring, MSI (IOC and RIRB) through the port, clear-behind, `open_output/start/stop/position/wait_period`, the stop order at exit and at client close; TCSEL | `drivers/hda/{stream,irq}.c`, `hda.idl` (stream methods), a test program user/tests/hdatest/ (new) |
| **3. `beep`** (the join of 1 and 2) | the path unmuted at the quiet default gain, `set_gain`/`get_gain`, the shell's `beep` and `hda gain`; the QEMU tone test | user/services/shell/cmd/beep.c (new), tools/beep-test.sh (new), `drivers/hda/main.c` |
| **4. Jacks** | unsolicited responses on (GCTL.UNSOL, the pin's enable, the RIRB interrupt), the tag -> pin table, the plugged/unplugged log lines, the polling fallback, `hda.jack`, `hda` shows the jack state | drivers/hda/jack.c (new), `hda.idl` (jack) |
| **5. Review** | the independent review-and-fix pass over all of A1 (standing rule), then the PC sign-off | whatever its findings touch |

## Tests

| Stage | QEMU | Only the PC |
|---|---|---|
| 0 | `tools/hda-test.sh`: two emulated controllers (intel-hda with hda-duplex + hda-output, ich9-intel-hda with hda-micro): each graph in the log, `hda` from the shell, `kill hda` and devmgr's restart, **every verb the codecs got is a GET** (QEMU's codec trace, `debug=3`), and the `init` run with them: each driver stops cleanly with its controller in reset | the real controller resets; the rings (or the immediate interface) answer; the real codec's graph |
| 1 | the path self-test on every fixture (QEMU's in QEMU; the PC's fixture too, since it is data); the chosen path for hda-output is DAC 02 -> pin 03; the codec trace shows only allow-listed SETs and no amp unmuted | the path found on the real codec is the front headphone jack (the owner reads it off the `hda` output) |
| 2 | `hdatest`: a known pattern through the stream, captured by QEMU's wav backend (`-audiodev wav,id=snd0,path=<file>`, available in this QEMU; the codec with `mixer=off`), compared sample for sample after the leading silence; position advances at 48 kHz within 2 %; the client closing mid-stream stops it; kill mid-stream: restart, quarantine released, no stale DMA | interrupts and the position buffer on the real controller |
| 3 | tools/beep-test.sh (new): `beep 440 500` into the wav file; its zero crossings give 440 Hz within 1 % over 500 ms within 30 ms, fades present, silence afterwards | **the tone in the headphones** (done-when) |
| 4 | the tag -> pin logic against fixtures; the fallback poller with a fake sense source | **unplug/replug logged** (done-when) |
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

## Done when

`beep` in the shell plays a tone in the front-panel headphones on the
real PC; unplugging and replugging them is logged; `hda` shows the path
and the jack state; the tests above pass in QEMU; the review's High and
Medium findings are fixed; `soak 10` on the PC passes with the driver
bound.
