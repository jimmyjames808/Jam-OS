# Audio review (A1, A2, AS)

An independent read of the audio work at commit 823c178: the hda driver
(`drivers/hda`), the protocols (`hda`, `audio`, `audioctl`, `music` and
initctl's splash methods), the mixer, the music player, libos's audio
library (`audio.c`, `mixer_client.c`, `mixmath.c`, `wav.c`, `mp3.c`,
`play_src.c`), the shell's `play`, `beep`, `hda`, `vol`, `music`,
`hdatest`, `mixtest`, the boot splash (`user/apps/splash`, the kernel's
quiet fbcon and init's side), how dr_mp3 and pl_mpeg are pinned, built and
fed, and the audio test scripts. Findings first; each one's outcome is
filled in as it is fixed.

Severity: **High** lets a program reach what it should not, or makes
sound when nothing should play; **Medium** is wrong behaviour a user will
meet, or a test that fails for a reason that is not the code's; **Low** is
a wart or a narrow race.

Nothing High was found. The parts a hostile program can reach held up:
the mixer never maps a client's ring and clamps what the client's `write`
claims; the driver's ring is a contiguous pinned VMO (no resize); every
IDL request is checked for its exact size and protocol; the codec gate
only lets allow-listed SET verbs through with their payload bits; the
WAV, ID3/APE and MPEG frame header code bounds every read; the resampler's
indices stay inside its tables at every rate 8-192 kHz. The path is muted
whenever the mixer has no stream playing, on every path through the mixer
and the driver that I could find; the exceptions are a driver that dies
or hangs mid-stream (14).

## Findings

| # | Sev | Where | What |
|---|---|---|---|
| 1 | Medium | `tools/mixer-test.sh:133-149` | client_killed's "440 Hz has no gap" check fails now and then (1 in 3 here, two agents before). Not the mixer: QEMU's hda-codec drops its whole 8 KiB buffer (2048 frames) when its audio backend is late on a loaded host (`hda_audio_output_cb`, trace event `hda_audio_overrun`). The failing WAV (run mix1) shows exactly that: tone-a's phase jumps by 2048 frames at 110 ms with full amplitude either side, the segment is 2048 frames short, the mixer logged 0 late periods and stayed 94 ms ahead. A window straddling the jump measures 75 % and fails the 90 % check. |
| 2 | Medium | `user/services/music/main.c:31`, `player.c:90`, `tracks.c:111`; `user/services/shell/cmd/music.c:13,44` | `music start` walks the whole folder inside the call (known item 5). The shell waits up to 60 s with no Ctrl+C; the player answers nothing meanwhile (`music status` from nowhere, `stop` queued); what was playing has already stopped. Past 60 s the shell prints "can't play it (timed out)" and then the player starts anyway. FAT's readdir by index is quadratic, so a folder of a few thousand files gets there. |
| 3 | Medium (design) | `drivers/hda/verbs.c:250-338`, `stream.c:294,314` | The pop when the path opens and closes (known item 1): every stream start turns the headphone pin's output and headphone amp on, then EAPD; every stop turns them off. The amps were unmuted first, so the pin's own power-up step is heard. See "Design questions". |
| 4 | Medium (design) | `user/services/shell/cmd/play.c:170`, `user/lib/mp3.c`, `third_party/dr_mp3` | `play` decodes a file from any stick inside the shell's own process. dr_mp3 is a development snapshot ("v0.7.4 - TBD", dr_libs 51e61d3, unmodified); a memory bug in it would run with everything the shell holds (devmgr's control channel, init's control channel, the root with READ and MANAGE). The music player decodes the same files with only the mixer's channel and a read view of the mounts. |
| 5 | Medium (design) | `user/lib/spawn.c:378`, `user/services/init/shell.c:585-596` | The shell is at the spawn limit (known item 4): 16 startup handles, 5 fixed and SR_NS, 10 extras, all used. The next service the shell needs makes `spawn` fail ERR_OUT_OF_RANGE and init cannot start the shell at all. |
| 6 | Low | `user/apps/splash/plmpeg.c:8,21-52`, `sound.c:138` | The `bigs` table (pl_mpeg's big blocks, each a VMO) is used from two threads without a lock, and its comment ("the sound thread's are small") is wrong: `plm_create_with_memory` makes the video decoder and reads its sequence header before `plm_set_video_enabled(p, 0)` can run, so the sound thread maps a 16.6 MB frame buffer too (checked on the Mac with the real splash.mpg). Two threads picking the same free slot lose a mapping; `plm_release` of a pointer missing from the table hands a VMO mapping to `free`. The window: the sound thread still decoding when the main thread closes its video (a key at the very start on a slow machine). |
| 7 | Low | `kernel/main.c:245` with the quiet fbcon | On a plain boot fbcon draws nothing until the console takes the screen. If init ends before that (a crash early in init or in the console), the kernel's RESULTS box and "Idling" are never drawn: the screen stays dark. |
| 8 | Low | `user/services/init/shell.c:727` | init waits for the splash with no deadline: a splash that hangs without exiting (it never says SPLASH_PLAYED) keeps the shell from ever starting. Its own waits are bounded, so this needs a bug in it or in the console it draws through. |
| 9 | Low (design) | `kernel/object/process.c:329` | Every byte >= 0x7f in a program's output becomes `?`: `JAŸ-Z` (UTF-8 C5 B8) logs as `JA??-Z` (known item 6). See "Design questions". |
| 10 | Low | `user/services/mixer/streams.c:20` | A client's ring handle has RIGHT_WRITE, which also allows `vmo_set_size`. Shrinking is handled (the mixer never maps it); growing it to 64 GiB and touching the pages charges them to the mixer's job. No job has a page limit today, so it is no worse than the client allocating them itself. |
| 11 | Low | `user/services/mixer/streams.c:166`, the shared `SR_AUDIO` channel | Any program can open all 16 streams, or keep the shared service channel's queue full, so other programs (the music player) can't open a stream while it runs. It ends with the program. |
| 12 | Low | `drivers/hda/irq.c:319`, `jack.c:216`, `hda.h:203` | The driver's one loop waits for verbs (100 ms each when a codec doesn't answer) and sleeps for pin-sense triggers. A codec that stops answering makes each 500 ms jack poll take 1.4 s (7 jacks, 2 verbs): the mixer's period ends come late (it closes and reopens the output as "stalled"), and a loop held up for more than a ring (341 ms) lets the position wrap unseen: clear-behind misses a ring, so up to 341 ms of old samples play once more. |
| 13 | Low | `drivers/hda/ctrl.c:306-313,329` | rirb_poll clears RINTFL after each entry it takes, after reading the write pointer once. A response that lands between that read and the last clear has its interrupt flag cleared without being taken; it waits for the next drain. Drains happen at every loop turn, so it is late, not lost; for a proven jack with no stream running the next turn may be the next unrelated event. |
| 14 | Low (design) | `drivers/hda/stream.c:50`, devmgr's restarts | A driver killed or crashed mid-stream can't mute: the kernel turns Bus Master Enable off, so no more samples are fetched, but the codec path stays open (pin output, EAPD, amps unmuted) until the restarted driver resets the link. If devmgr gives up on the driver it stays open. A driver that hangs (alive, loop stuck) keeps it open and its last ring playing. See "Design questions". |
| 15 | Low | `user/lib/mp3.c:121` | mp3_sniff's free-format check scans forward from every candidate header: a crafted 8 KiB of free-format headers that never repeat costs about 33 million header parses (a fraction of a second), once per file. |
| 16 | Low | `user/services/mixer/output.c:42-71,426-444` | Known, in A2-PLAN's "Left for later": `out_find` (devmgr's GET_SERVICE and `hda.info` to each service, 2 s each), `out_position` and `out_device_gain` are synchronous calls inside the mixer's loop, made for clients' `open_output`, `position` and `levels`. While no driver is found every open costs those calls. |

### Known item 2: the splash's sound start

Read, not changed. With the sound decoded in time (the usual case: the
first frame waits up to SOUND_WAIT 2 s for the stream to open), the clock
is held at 0 until the mixer reports the first frame played (or 500 ms),
so picture and sound start together from frame 0; HEARD_IN (50 ms) is only
used for a late join, which also fades in over 25 ms. Nothing in the
splash makes a step at the start: its first samples are the owner's file
as authored. A click at the very start is the path opening (item 3): the
mixer opens the driver's stream for the splash's first frames, and the
driver turns the pin and EAPD on at that moment.

### Known item 3: mixer-test's client_killed

The previous theory (which tone client the mixer catches first) is
covered already: the check starts where 440 Hz first reaches full level.
The failure seen here was QEMU's (item 1). To be sure of it the test now
runs QEMU with `-trace hda_audio_overrun`; in 12 more runs here no
overrun happened and all passed. See the outcome.

## Design questions

**A. The path-open pop (item 3, known item 1).** Recommendation: keep
the pin's output, its headphone amp and EAPD on from the first stream
until the driver stops (or for a hold time, say 2 s, after the last
stream), and keep "silent when nothing plays" with the amps alone: the
DAC's and the mixer widget's amps muted, the pin's output amp muted. A
muted amp is silence; the pop comes from the pin's driver and the external
amplifier switching on, which then happens once per boot (or per hold)
instead of per sound. Open in this order: pin output and EAPD on with the
amps still muted, wait about 50 ms for the amplifier to settle, then
unmute; close in reverse (mute, wait a period, then the pin off if the
hold has passed). This keeps the owner's rule "muted unless a stream
plays" in the sense that matters (no signal reaches the jack), bends it
only in that the pin stays powered. The 1 s hold the owner was asked about
(keep everything open after the last stream) avoids the pop between
sounds but still pops at the first sound and after every pause; the
amp-only mute avoids both. Either way, the PC decides: listen for the pop
with `beep` twice in a row.

**B. The shell at the spawn limit (item 5, known item 4).** Today's
options, cheapest first: (1) raise STARTUP_MAX_HANDLES (the startup
message has room; every program is rebuilt with the kernel); (2) stop
giving the shell channels it only passes to test programs (SR_DEVMGR_CTL,
SR_AUDIO_CTL, init's control channel could come from one "test" channel
of init's, asked for by name); (3) the capability way: put service
channels in the namespace (`/svc/audio`, `/svc/music`, ...), served by
init, so a new service costs no startup slot and a program's namespace
says what it may reach. Recommendation: (3) as the design, (1) as the
stopgap the next time a slot is needed.

**C. UTF-8 in the log (item 9, known item 6).** The log file (logd) and
the serial port would show `JAŸ-Z` if well-formed UTF-8 passed through;
the console's font is ASCII, so it would draw one `?` per character
instead of one per byte. What must stay out is what a terminal acts on:
C0 controls, DEL, the C1 controls U+0080-U+009F (some terminals act on
them as code points), and malformed sequences (one `?` each).
Recommendation: pass well-formed UTF-8 (U+00A0 and up, no surrogates, no
overlongs) through in `out_add_locked`, and have the console and fbcon
draw one `?` per code point. Small, kernel and console only; not done
here because it changes what every program's output looks like.

**D. A driver that dies mid-stream (item 14).** Nothing but a driver can
send verbs. Options: the kernel puts the HD Audio function into D3hot on
an unclean exit (the codec link goes down with it; a pop, then silence),
or devmgr restarts it at once (it does, with backoff) and a giving up
leaves it so. Recommendation: leave it; note it in ARCHITECTURE's "muted
unless" sentence.

**E. Decoding in the shell (item 4).** Recommendation: `play` as a
program (`bin/play`) started like any other, with only SR_AUDIO and a
read view of the file's mount, as the music player already is; the shell
waits for it as for `run`. Ctrl+C then kills the decoder, not the
shell's command.

## Outcomes

| # | Outcome |
|---|---|
| 1 | |
| 2 | |
| 3 | Design question A. |
| 4 | Design question E. |
| 5 | Design question B. |
| 6 | |
| 7 | |
| 8 | Not fixed: Low (reported). |
| 9 | Design question C. |
| 10-16 | Not fixed: Low (reported). |
