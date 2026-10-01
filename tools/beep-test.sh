#!/bin/sh
# `beep` in QEMU (docs/A1-PLAN.md, stage 3): intel-hda (8086:2668) with an
# hda-output codec whose samples go to a WAV file (QEMU's wav audio
# backend at 48 kHz 16-bit stereo). Unlike tools/hda-stream-test.sh the
# codec keeps its mixer (mixer=on, the default): its DAC has an output amp
# (0-74 in 1 dB steps, 0 dB at 74, with a mute) that QEMU applies to the
# samples, so the WAV shows the gain and the mutes. The shell runs
# tools/shell-tests/beep.txt: `hda gain`, `beep 440 500` at the default
# -30 dB, then the gain set and clamped. Then:
#   - the tone in the WAV: 440 Hz within 1 % from its zero crossings,
#     500 ms within 30 ms, fades (its first and last 2.5 ms well below its
#     peak, no jump between samples bigger than the sine's own), silence
#     before and after it; its peak is beep's quarter of full scale at
#     QEMU's volume for step 44 (QEMU scales linearly: 44 * 255 / 74 of
#     255), within 5 %;
#   - the codec's verbs (tools/hda-verbs.awk): only allow-listed ones; the
#     path opened only while the converter has the stream's tag and
#     muted again before the tag goes back to 0, nothing open at the end;
#     the DAC's amp opened at step 44 (-30 dB) and no higher; the output
#     stage (the pin's output, EAPD) on before the first stream tag and
#     never off again while the driver runs (the amps are the mute, so the
#     stage's power-up thump is never heard with a stream);
#   - the driver's log: the stream opened, then the path unmuted, muted
#     again, and the stream closed, in that order; the first unmute came
#     with the stage on for at least its settle time (400 ms).
# QEMU_SMP passes through. Usage: tools/beep-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
wav="$out/beep.wav"
rm -f "$wav"
devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0,debug=3"
ok=1

QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_EXTRA="$devs" QEMU_INPUT=tools/shell-tests/beep.txt \
    tools/qemu-test.sh "$out" beep shell > "$out/beep.out" 2>&1 ||
    { echo "beep: the script failed"; grep "serial-feed: .*no '" "$out/beep.out"; ok=0; }
log="$out/beep.log"
for want in "path: plays at -30.0 dB (node 02's output amp, step 44; -74.0 to 0.0 dB)" \
            "output: unmuted at -30.0 dB (node 02 step 44)" "output: muted again" \
            "output: gain set to -20.0 dB (step 54)" "beep: 440 Hz for 500 ms at -30.0 dB"; do
    grep -qF -- "$want" "$log" || { echo "beep: no line with \"$want\""; ok=0; }
done
# The driver's order: stream open < unmuted < muted again < stream closed.
order=$(grep -aoE "stream: open on|output: unmuted|output: muted again|stream: closed" "$log" |
        tr '\n' ',')
[ "$order" = "stream: open on,output: unmuted,output: muted again,stream: closed," ] ||
    { echo "beep: the driver's lines came in the order: $order"; ok=0; }

trace=$(awk -v stage=1 -f tools/hda-verbs.awk "$out/beep.out")
bad=$(echo "$trace" | grep -E "^bad " || true)
if [ -n "$bad" ]; then
    echo "beep: verbs that are not on the driver's allow-list:"
    echo "$bad" | head -10
    ok=0
fi
state=$(echo "$trace" | tail -1)
[ "$state" = "state untagged 0 released 0 left 0" ] ||
    { echo "beep: the path was open outside the stream ($state)"; ok=0; }
# The output stage: on before any stream, never off, still on at the end.
power=$(echo "$trace" | grep "^power on " || true)
echo "$power" | awk '{ g = $3 >= 1 && $5 == 0 && $7 >= 1 && $9 >= 1 } END { exit !g }' ||
    { echo "beep: the output stage was not on before the stream and left on ($power)"; ok=0; }
settled=$(grep -aoE "output: unmuted .* with the output stage on for [0-9]+ ms" "$log" |
          head -1 | sed -E 's/.* on for ([0-9]+) ms/\1/')
[ -n "$settled" ] && [ "$settled" -ge 400 ] ||
    { echo "beep: the first unmute did not find the output stage settled (${settled:-no line})"; ok=0; }
# The DAC's output amp (node 2, 0x300 with bit 15): opened at step 44 only.
dac=$(echo "$trace" | grep "^open nid 2 verb 0x300 " | awk '{ print $NF }' | sort -u | tr '\n' ' ')
[ "$dac" = "0xb02c " ] || { echo "beep: the DAC's amp opened with payload(s) $dac, want 0xb02c"; ok=0; }
echo "beep: codec: $(echo "$trace" | tail -2 | head -1 | sed 's/^total //'); $state; $power;" \
    "stage on for ${settled:-?} ms at the first unmute"

python3 - "$wav" <<'PY' || ok=0
import struct, sys
data = open(sys.argv[1], "rb").read()
i = data.find(b"data")
if data[:4] != b"RIFF" or i < 0:
    sys.exit("beep: %s is not a WAV file" % sys.argv[1])
fmt = data.find(b"fmt ")
ch, rate, bits = struct.unpack_from("<H", data, fmt + 10)[0], \
    struct.unpack_from("<I", data, fmt + 12)[0], struct.unpack_from("<H", data, fmt + 22)[0]
if (ch, rate, bits) != (2, 48000, 16):
    sys.exit("beep: WAV is %d ch %d Hz %d-bit, want 2/48000/16" % (ch, rate, bits))
pcm = data[i + 8:]
n = len(pcm) // 4
fr = struct.unpack_from("<%dh" % (2 * n), pcm)
left = fr[0::2]
if fr[1::2] != left:
    sys.exit("beep: the two channels differ")
nz = [k for k in range(n) if left[k]]
if not nz:
    sys.exit("beep: the WAV file (%d frames) is all silence" % n)
first, last = nz[0], nz[-1]
# One tone: no gap of more than 5 ms of zeros inside it.
gaps = [b - a for a, b in zip(nz, nz[1:]) if b - a > 240]
if gaps:
    sys.exit("beep: the tone has gaps of %s frames: more than one sound" % gaps[:3])
RATE, HZ, MS, FADE = 48000, 440, 500, 240
dur = (last - first + 1) * 1000.0 / RATE
fails = []
if abs(dur - MS) > 30:
    fails.append("lasts %.1f ms, want %d within 30" % (dur, MS))
# The frequency: rising zero crossings, the first and last interpolated.
tone = left[first:last + 1]
ups = [k + tone[k] / float(tone[k] - tone[k + 1]) for k in range(len(tone) - 1)
       if tone[k] <= 0 < tone[k + 1] or (tone[k] < 0 <= tone[k + 1])]
ups = sorted(set(ups))
hz = (len(ups) - 1) * RATE / (ups[-1] - ups[0]) if len(ups) > 2 else 0
if abs(hz - HZ) > HZ * 0.01:
    fails.append("%.2f Hz from %d rising zero crossings, want %d within 1 %%" % (hz, len(ups), HZ))
peak = max(abs(v) for v in tone)
want = 8192 * (44 * 255 // 74) / 255.0
if abs(peak - want) > want * 0.05:
    fails.append("peak %d, want %.0f within 5 %% (a quarter of full scale at step 44)" % (peak, want))
# The fades: the first and last 2.5 ms stay well under the peak, and no
# step between samples is bigger than a full-amplitude sine's (a click).
head = max(abs(v) for v in tone[:120])
tail = max(abs(v) for v in tone[-120:])
if head > 0.6 * peak or tail > 0.6 * peak:
    fails.append("no fade: the first 2.5 ms reach %d, the last %d, of %d" % (head, tail, peak))
import math
jump = max(abs(tone[k + 1] - tone[k]) for k in range(len(tone) - 1))
if jump > peak * 2 * math.pi * HZ / RATE * 1.2 + 2:
    fails.append("a jump of %d between samples (a click)" % jump)
after = n - 1 - last
print("beep: WAV: %d frames of silence, %.1f ms of tone at %.2f Hz, peak %d (want %.0f), "
      "first/last 2.5 ms up to %d/%d, then %d frames of silence"
      % (first, dur, hz, peak, want, head, tail, after))
if after < 2400:
    fails.append("only %d frames after the tone (want 50 ms of silence or more)" % after)
if fails:
    sys.exit("beep: " + "; ".join(fails))
PY

if [ $ok = 1 ]; then
    echo "beep: PASS"
    exit 0
fi
echo "beep: FAIL (see $out/beep.log, $out/beep.out, $wav)"
exit 1
