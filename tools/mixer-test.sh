#!/bin/sh
# The mixer (user/services/mixer, docs/A2-PLAN.md) in QEMU: intel-hda
# (8086:2668) with an hda-output codec whose samples go to a WAV file
# (QEMU's wav audio backend at 48 kHz 16-bit stereo; the codec's
# mixer=off, so the WAV holds exactly what the mixer wrote). The shell runs
# tools/shell-tests/mixer.txt: `vol` with nothing playing, the master set
# and clamped, then `mixtest`, whose tone programs play through the mixer
# (each phase a sound of its own, with silence played between: QEMU
# writes the WAV only while the driver's stream runs). Then:
#   - mixtest's own checks passed (its result line);
#   - the WAV, segment by segment (a segment ends at 50 ms of silence):
#       1  two programs at once: 440 Hz and 1000 Hz both there, the
#          second at -6 dB (half the first's amplitude, within 3 %), the
#          first at the tone's own amplitude (a quarter of full scale)
#       2  the same, the 1000 Hz program killed mid-tone: the 440 Hz tone
#          has no gap (every 10 ms window at its full amplitude, within
#          10 %), and 1000 Hz is gone in its last half second
#       3  440 Hz with the master at -6 dB: half the amplitude
#       4  440 Hz turned to -12 dB through audioctl mid-tone: the end at a
#          quarter of the start
#       5  two programs on <audio.h> (play's and beep's library) at once:
#          1000 Hz from 44.1 kHz samples (resampled) and 440 Hz, both at
#          the tone's own amplitude (within 3 %)
#       then at least 4 more segments of 440 Hz: the mixer killed mid-tone,
#       the hda driver killed mid-tone (QEMU writes the WAV only while the
#       driver's stream runs, so their gaps are not in it) and the stream
#       left empty that wakes the mixer again (two: the mixer plays the
#       empty stream's silence for a second before it closes the output);
#   - the log: two programs' streams open at once; the killed client's
#     stream closed; the mixer restarted by init; the output closed when
#     nothing played and when every stream was empty, opened again on the
#     restarted driver;
#   - the codec's verbs (tools/hda-verbs.awk): only allow-listed ones, the
#     path opened only while the converter has the stream's tag and
#     nothing left open at the end: muted whenever nothing plays.
# QEMU's hda-codec drops its whole buffer (2048 frames at 16-bit stereo)
# when its audio backend falls behind on a busy host: the WAV then skips
# 2048 frames with no silence, a phase jump the guest never made (seen as
# one 10 ms window at about 75 % in segment 2). QEMU runs with the trace
# event hda_audio_overrun, so such a drop is known; segment 2's gap check
# then lets through windows still above half the amplitude, as many as
# twice the drops, and says so. A real gap is a mixer period (42.7 ms) of
# silence or more, so at least three windows near zero: still a failure.
# QEMU_SMP passes through. Usage: tools/mixer-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
wav="$out/mixer.wav"
rm -f "$wav"
devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0,mixer=off,debug=3 -trace hda_audio_overrun"
ok=1

QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_EXTRA="$devs" QEMU_INPUT=tools/shell-tests/mixer.txt \
    tools/qemu-test.sh "$out" mixer shell > "$out/mixer.out" 2>&1 ||
    { echo "mixer: the script failed"; grep "serial-feed: .*no '" "$out/mixer.out"; ok=0; }
log="$out/mixer.log"
grep -aE "mixtest: [0-9]+ passed" "$log" | tail -1
grep -aqE "mixtest: 11 passed($|\r)" "$log" ||
    { echo "mixer: mixtest did not pass 11 of 11"; grep -a "FAILED" "$log" | head -5; ok=0; }
grep -aqE "mixer: stream [0-9]+ \(tone-a\) opened.*" "$log" &&
    grep -aqE "mixer: stream [0-9]+ \(tone-b\) opened" "$log" ||
    { echo "mixer: the tone programs' streams were not opened"; ok=0; }
for want in "(tone-b) closed (its channel closed)" \
            "init: bin/mixer was killed" \
            "mixtest: tone tone-d: the mixer went away" \
            "mixtest: tone tone-d: a new stream after" \
            "mixer: output closed (the driver went away)" \
            "mixer: output closed (every playing stream is empty)" \
            "mixer: output closed (no stream plays)"; do
    grep -aqF -- "$want" "$log" || { echo "mixer: no line with \"$want\""; ok=0; }
done

trace=$(awk -f tools/hda-verbs.awk "$out/mixer.out")
bad=$(echo "$trace" | grep -E "^bad " || true)
if [ -n "$bad" ]; then
    echo "mixer: verbs that are not on the driver's allow-list:"
    echo "$bad" | head -10
    ok=0
fi
state=$(echo "$trace" | tail -1)
[ "$state" = "state untagged 0 released 0 left 0" ] ||
    { echo "mixer: the path was open outside a stream ($state)"; ok=0; }
echo "mixer: codec: $(echo "$trace" | tail -2 | head -1 | sed 's/^total //'); $state"

drops=$(grep -ac "hda_audio_overrun" "$out/mixer.out" || true)
[ "$drops" = 0 ] || echo "mixer: QEMU dropped its codec buffer $drops time(s) (the host was late)"

python3 - "$wav" "$drops" <<'PY' || ok=0
import math, struct, sys
data = open(sys.argv[1], "rb").read()
drops = int(sys.argv[2])
i = data.find(b"data")
if data[:4] != b"RIFF" or i < 0:
    sys.exit("mixer: %s is not a WAV file" % sys.argv[1])
pcm = data[i + 8:]
n = len(pcm) // 4
fr = struct.unpack_from("<%dh" % (2 * n), pcm)
left = fr[0::2]
if fr[1::2] != left:
    sys.exit("mixer: the two channels differ")
RATE, AMP = 48000, 8192
# Segments: runs of sound separated by 50 ms or more of zeros.
segs, start, zeros = [], None, 0
for k, v in enumerate(left):
    if v:
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

print("mixer: WAV: %d segments: %s" % (len(segs), ", ".join(
    "%.0f ms" % ((b - a) * 1000.0 / RATE) for a, b in segs)))
if len(segs) < 9:
    sys.exit("mixer: %d segments, want 9 or more" % len(segs))
s = [left[a:b] for a, b in segs]
# 1: both at once, 1000 Hz at -6 dB.
mid = s[0][len(s[0]) // 2 - 24000:len(s[0]) // 2 + 24000]
a1, b1 = amp(mid, 440), amp(mid, 1000)
near("segment 1: 440 Hz amplitude", a1, AMP, 0.02)
near("segment 1: 1000 Hz / 440 Hz", b1 / a1, 0.50119, 0.03)
# 2: the 1000 Hz program killed: 440 Hz with no gap, 1000 Hz gone at the end.
seg = s[1]
head, tail = seg[12000:28800], seg[-24000:-480]
near("segment 2: 1000 Hz at the start / 440 Hz", amp(head, 1000) / amp(head, 440), 0.50119, 0.05)
if amp(tail, 1000) > 0.01 * AMP:
    fails.append("segment 2: 1000 Hz still there at the end (%.1f)" % amp(tail, 1000))
# The two programs' starts race to a closed output: the one that comes
# second is mixed a lead (4 periods, ~170 ms) behind. So the gap check
# starts where 440 Hz is first at full level, which must be within 250 ms.
first = next((k for k in range(480, 12000, 480) if amp(seg[k:k + 480], 440) >= 0.9 * AMP), None)
if first is None:
    fails.append("segment 2: 440 Hz not at full level within 250 ms of the start")
    first = 480
low = [k for k in range(first, len(seg) - 960, 480) if amp(seg[k:k + 480], 440) < 0.9 * AMP]
# QEMU's own drops (see the top): a window straddling one is still above
# half the amplitude; silence is not.
if low and drops and len(low) <= 2 * drops and \
        all(amp(seg[k:k + 480], 440) >= 0.5 * AMP for k in low):
    print("mixer: segment 2: %d window(s) of 10 ms at a QEMU buffer drop (the first at %d ms), "
          "no silence: not counted" % (len(low), low[0] * 1000 // RATE))
    low = []
if low:
    fails.append("segment 2: 440 Hz below 90 %% in %d window(s) of 10 ms, the first at %d ms"
                 % (len(low), low[0] * 1000 // RATE))
# 3: the master at -6 dB.
near("segment 3: 440 Hz amplitude", amp(s[2][2400:-2400], 440), AMP * 0.50119, 0.03)
# 4: -12 dB mid-tone.
near("segment 4: start amplitude", amp(s[3][2400:12000], 440), AMP, 0.03)
near("segment 4: end / start", amp(s[3][-12000:-2400], 440) / AMP, 0.25119, 0.03)
# 5: the library's two programs at once.
mid = s[4][len(s[4]) // 2 - 12000:len(s[4]) // 2 + 12000]
near("segment 5: 1000 Hz (44.1 kHz, resampled)", amp(mid, 1000), AMP, 0.03)
near("segment 5: 440 Hz", amp(mid, 440), AMP, 0.03)
# The rest: 440 Hz tones.
for k in range(5, len(s)):
    if len(s[k]) >= 4800 and amp(s[k][:4800], 440) < 0.5 * AMP:
        fails.append("segment %d: no 440 Hz tone" % (k + 1))
print("mixer: WAV: 440/1000 Hz %.0f/%.0f (ratio %.4f), killed-client segment %d samples, "
      "master -6 dB %.0f, audioctl -12 dB ratio %.4f"
      % (a1, b1, b1 / a1, len(seg), amp(s[2][2400:-2400], 440),
         amp(s[3][-12000:-2400], 440) / AMP))
if fails:
    sys.exit("mixer: " + "; ".join(fails))
PY

if [ $ok = 1 ]; then
    echo "mixer: PASS"
    exit 0
fi
echo "mixer: FAIL (see $out/mixer.log, $out/mixer.out, $wav)"
exit 1
