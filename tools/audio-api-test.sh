#!/bin/sh
# libos's sound output API (<audio.h>: user/lib/audio.c, audio_flow.c) in
# QEMU: intel-hda (8086:2668) with an hda-output codec whose samples go to
# a WAV file (QEMU's wav audio backend at 48 kHz 16-bit stereo; the
# codec's mixer=off, so the WAV holds exactly what the mixer wrote), as in
# tools/mixer-test.sh. The shell runs tools/shell-tests/audio-api.txt:
# `utest only audio_out`, whose 11 cases use the API as a browser's audio
# back end would (float samples, a writer on its own clock with
# audio_try_write and the room and position queries, audio_start and
# audio_pause, two streams at once, the program's eight-stream cap on its
# one opener and a closed stream's place). Then:
#   - utest passed all 11, none of them skipped (a skip is a case that
#     found no audio output);
#   - the WAV, segment by segment (a segment ends at 50 ms of silence):
#       1  16-bit stereo: the 24000 frames the case wrote, sample for sample
#       2  float stereo made from 16-bit values: the same, sample for sample
#       3  mono float at 44.1 kHz, a 1000 Hz tone a quarter of full scale
#          through the resampler: the tone at that amplitude (within 3 %),
#          both channels equal, 24000 frames at 48 kHz (a few dozen more)
#       4  the paced writer's 440 Hz tone, 96000 frames: no gap (every 10 ms
#          window at its full amplitude, within 10 %), the length within
#          2 %
#       5  two streams at once, 440 Hz and 1000 Hz (a 16-bit stereo and a
#          float mono one), each at the tone's own amplitude (within 3 %)
#       6  pause and resume: the 60000-frame pattern up to where the stream
#          was paused, then the rest of it in order from a later frame (the
#          mixer closes the device when nothing plays, and what it had
#          taken ahead of the speaker, 171 ms at most, is not played); no
#          frame twice and none out of order (the silence of the pause left
#          out: the capture has it as a gap, or not at all)
# QEMU's hda-codec drops its whole buffer (2048 frames) when its audio
# backend falls behind on a busy host, which the WAV shows as 2048 frames
# gone with no silence: a phase jump the guest never made. QEMU runs with
# the trace event hda_audio_overrun, so such a drop is known; an exact
# comparison that fails by no more than 2048 frames a drop is then excused,
# and said. QEMU_SMP passes through.
# Usage: tools/audio-api-test.sh <outdir>; exit 0 on PASS.
set -eu
out=${1:?usage: tools/audio-api-test.sh <outdir>}
mkdir -p "$out"
wav="$out/audio-api.wav"
rm -f "$wav"
devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0,mixer=off,debug=3 -trace hda_audio_overrun"
ok=1

QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_EXTRA="$devs" QEMU_INPUT=tools/shell-tests/audio-api.txt \
    tools/qemu-test.sh "$out" audio-api shell > "$out/audio-api.out" 2>&1 ||
    { echo "audio-api: the script failed"; grep "serial-feed: .*no '" "$out/audio-api.out" || true; ok=0; }
log="$out/audio-api.log"
grep -aE "utest: [0-9]+ passed" "$log" | tail -1
grep -aqE "utest: 11 passed[[:space:]]*$" "$log" ||
    { echo "audio-api: utest did not pass 11 of 11"; grep -a "FAILED" "$log" | head -5; ok=0; }
! grep -aE "utest: audio_out.*skipped" "$log" ||
    { echo "audio-api: a case found no audio output"; ok=0; }
for name in f32 args slots s16 f32_play mono44 room arm paced two pause; do
    grep -aqE "utest: audio_out_$name ok" "$log" ||
        { echo "audio-api: audio_out_$name did not report ok"; ok=0; }
done
grep -aqE "mixer: stream [0-9]+ \(ut-two-a\) opened" "$log" &&
    grep -aqE "mixer: stream [0-9]+ \(ut-two-b\) opened" "$log" ||
    { echo "audio-api: the mixer did not log the two streams' opening"; ok=0; }

trace=$(awk -f tools/hda-verbs.awk "$out/audio-api.out")
bad=$(echo "$trace" | grep -E "^bad " || true)
if [ -n "$bad" ]; then
    echo "audio-api: verbs that are not on the driver's allow-list:"
    echo "$bad" | head -10
    ok=0
fi
state=$(echo "$trace" | tail -1)
[ "$state" = "state untagged 0 released 0 left 0" ] ||
    { echo "audio-api: the path was open outside a stream ($state)"; ok=0; }
drops=$(grep -ac "hda_audio_overrun" "$out/audio-api.out" || true)
[ "$drops" = 0 ] || echo "audio-api: QEMU dropped its codec buffer $drops time(s) (the host was late)"

python3 - "$wav" "$drops" <<'PY' || ok=0
import math, struct, sys
data = open(sys.argv[1], "rb").read()
drops = int(sys.argv[2])
i = data.find(b"data")
if data[:4] != b"RIFF" or i < 0:
    sys.exit("audio-api: %s is not a WAV file" % sys.argv[1])
pcm = data[i + 8:]
n = len(pcm) // 4
fr = struct.unpack_from("<%dh" % (2 * n), pcm)
L, R = fr[0::2], fr[1::2]
RATE, AMP = 48000, 8192
# Segments: runs of sound separated by 50 ms or more of zeros (both channels).
segs, start, zeros, last = [], None, 0, 0
for k in range(n):
    if L[k] or R[k]:
        if start is None:
            start = k
        zeros, last = 0, k
    elif start is not None:
        zeros += 1
        if zeros >= 2400:
            segs.append((start, last + 1))
            start = None
if start is not None:
    segs.append((start, last + 1))
print("audio-api: WAV: %d segments: %s" % (len(segs), ", ".join(
    "%.0f ms" % ((b - a) * 1000.0 / RATE) for a, b in segs)))

# utest/audio_out.c's patterns, frame k of set `which`, channel ch.
MUL = [(7919, 104729), (6007, 7001), (3001, 5003)]
MOD = [(30011, 27449), (29021, 26003), (31013, 25013)]
OFF = [(15005, 13724), (14510, 13001), (15506, 12506)]
def pat(which, ch, k):
    v = (k * MUL[which][ch]) % MOD[which][ch] - OFF[which][ch]
    return v if v else (-1 if ch else 1)

def amp(x, hz):
    """The amplitude of the hz component of x (Hann window, Goertzel)."""
    N = len(x)
    w = [0.5 - 0.5 * math.cos(2 * math.pi * k / (N - 1)) for k in range(N)]
    c = 2 * math.cos(2 * math.pi * hz / RATE)
    s1 = s2 = 0.0
    for k in range(N):
        s0 = x[k] * w[k] + c * s1 - s2
        s2, s1 = s1, s0
    p = s1 * s1 + s2 * s2 - c * s1 * s2
    return 2 * math.sqrt(max(p, 0)) / sum(w)

fails = []
def near(what, got, want, tol):
    if abs(got - want) > want * tol:
        fails.append("%s: %.1f, want %.1f within %d %%" % (what, got, want, tol * 100))

def exact(what, got, which, count):
    """got: a list of (left, right) frames; the pattern's first `count`."""
    want = [(pat(which, 0, k), pat(which, 1, k)) for k in range(count)]
    if got == want:
        return
    bad = sum(1 for a, b in zip(got, want) if a != b)
    if drops and abs(len(got) - count) <= 2048 * drops:
        print("audio-api: %s: differs (%d of %d frames, %d long) after a QEMU buffer drop: "
              "not counted" % (what, bad, count, len(got)))
        return
    first = next((k for k, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), count))
    fails.append("%s: %d frames, want %d; the first difference at frame %d (%s vs %s)"
                 % (what, len(got), count, first, got[first] if first < len(got) else "end",
                    want[first] if first < count else "end"))

if len(segs) < 6:
    sys.exit("audio-api: %d segments, want 6 or more" % len(segs))
frames = lambda a, b: list(zip(L[a:b], R[a:b]))
# 1, 2: exact patterns.
exact("segment 1 (16-bit stereo)", frames(*segs[0]), 0, 24000)
exact("segment 2 (float stereo)", frames(*segs[1]), 1, 24000)
# 3: the 44.1 kHz mono float tone, resampled.
a, b = segs[2]
mid = L[(a + b) // 2 - 6000:(a + b) // 2 + 6000]
near("segment 3: 1000 Hz amplitude", amp(mid, 1000), AMP, 0.03)
if L[a:b] != R[a:b]:
    fails.append("segment 3: the two channels differ")
if not 23900 <= b - a <= 24300:
    fails.append("segment 3: %d frames, want 24000 and a few dozen" % (b - a))
# 4: the paced tone: whole, no gap.
a, b = segs[3]
seg = L[a:b]
if abs(len(seg) - 96000) > 0.02 * 96000:
    fails.append("segment 4: %d frames, want 96000 within 2 %%" % len(seg))
first = next((k for k in range(0, 12000, 480) if amp(seg[k:k + 480], 440) >= 0.9 * AMP), None)
if first is None:
    fails.append("segment 4: 440 Hz not at full level within 250 ms of the start")
    first = 0
low = [k for k in range(first, len(seg) - 960, 480) if amp(seg[k:k + 480], 440) < 0.9 * AMP]
if low and drops and len(low) <= 2 * drops and \
        all(amp(seg[k:k + 480], 440) >= 0.5 * AMP for k in low):
    print("audio-api: segment 4: %d window(s) of 10 ms at a QEMU buffer drop, no silence: "
          "not counted" % len(low))
    low = []
if low:
    fails.append("segment 4: 440 Hz below 90 %% in %d window(s) of 10 ms, the first at %d ms"
                 % (len(low), low[0] * 1000 // RATE))
# 5: two streams at once.
a, b = segs[4]
mid = L[(a + b) // 2 - 12000:(a + b) // 2 + 12000]
a440, a1000 = amp(mid, 440), amp(mid, 1000)
near("segment 5: 440 Hz amplitude", a440, AMP, 0.03)
near("segment 5: 1000 Hz amplitude", a1000, AMP, 0.03)
if L[a:b] != R[a:b]:
    fails.append("segment 5: the two channels differ")
# 6: pause and resume: what is left of the capture, silence dropped, is the
# pattern up to where the stream was paused, then the rest of it from a
# later frame: the mixer stops the device when nothing else plays, and what
# it had taken ahead of the speaker (the lead, 171 ms at most) is not
# played, so the resume skips up to the lead, never repeats and never
# reorders.
rest = [(l, r) for s, e in segs[5:] for l, r in zip(L[s:e], R[s:e]) if l or r]
want = [(pat(2, 0, k), pat(2, 1, k)) for k in range(60000)]
at = 0
while at < min(len(rest), 60000) and rest[at] == want[at]:
    at += 1
skip = None
if at == len(rest) == 60000:
    skip = 0   # the whole pattern: the device stayed open and played it out
elif at < len(rest):
    b = want.index(rest[at]) if rest[at] in want else None
    if b is not None and rest[at:] == want[b:b + len(rest) - at] and b + len(rest) - at == 60000:
        skip = b - at
if skip is None or not 0 <= skip <= 16384 or at < 5000:
    if drops:
        print("audio-api: segments 6- (pause and resume): differs after a QEMU buffer drop: "
              "not counted")
    else:
        fails.append("segments 6- (pause and resume): %d frames, the pattern up to frame %d, "
                     "then %s" % (len(rest), at, "no clean resume" if skip is None else
                                  "a skip of %d frames" % skip))
else:
    print("audio-api: pause: %d frames heard before it, %d skipped at the resume (the mixer's "
          "lead was not played)" % (at, skip))
print("audio-api: WAV: 440/1000 Hz together %.0f/%.0f, paced tone %d frames, pause pieces %d"
      % (a440, a1000, len(seg), len(segs) - 5))
if fails:
    sys.exit("audio-api: " + "; ".join(fails))
PY

if [ $ok = 1 ]; then
    echo "audio-api: PASS"
    exit 0
fi
echo "audio-api: FAIL (see $out/audio-api.log, $out/audio-api.out, $wav)"
exit 1
