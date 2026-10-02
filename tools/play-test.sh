#!/bin/sh
# `play` in QEMU (docs/A2-PLAN.md, track 1): WAV files made here are put
# on the stick's data partition (mtools, as the Mac would copy them), and
# the shell plays them (tools/shell-tests/play.txt) through intel-hda with
# an hda-output codec whose samples go to a WAV capture (QEMU's wav audio
# backend at 48 kHz 16-bit stereo, as tools/beep-test.sh). The files:
#   s48.wav    48000 Hz stereo 16-bit, 1.5 s: 440 Hz left, 660 Hz right
#   m44.wav    44100 Hz mono 16-bit, 1 s, 1000 Hz (resampled to 48 kHz)
#   u8.wav     22050 Hz mono 8-bit, 1 s, 500 Hz
#   x24.wav    96000 Hz stereo 24-bit WAVE_FORMAT_EXTENSIBLE, 1 s, 750 Hz,
#              played with -v -20 (its mixer stream's volume)
#   garbage.wav, cut.wav (the first 30 bytes of s48.wav), float.wav
#              (32-bit float): refused with their reasons
#   long.wav   48000 Hz mono 16-bit, 10 s, 300 Hz: Ctrl+C after ~2 s
# Then the capture holds five sounds, each with silence after it, and is
# checked: each one's frequency (from zero crossings) within 1 % on each
# channel, its length within 2 % of the file's, mono files equal on both
# channels and the stereo one's channels apart; x24's peak a tenth of the
# others' (-20 dB of stream volume in the mixer, on top of the device's
# gain, which stays at -30 dB);
# the stopped one shorter than 3 s, ending in a fade (its last 1 ms well
# under its peak) and no click (no jump between samples bigger than its
# sine's own). QEMU_SMP passes through.
# QEMU's hda-codec drops its whole buffer (2048 frames) when its audio
# backend falls behind on a busy host (tools/mixer-test.sh has the story):
# the capture then skips 2048 frames, a phase jump the guest never made.
# QEMU runs with the trace event hda_audio_overrun, so such a drop is
# known; each one traced excuses one click and 2048 frames of length.
# Usage: tools/play-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
wav="$out/play.wav"
stick="$out/play-stick.img"
rm -f "$wav"
ok=1

python3 - "$out" <<'PY'
import math, struct, sys
out = sys.argv[1]

def tone(rate, secs, hz, amp=0.25):
    n = int(rate * secs)
    fade = int(rate * 0.005)
    for k in range(n):
        env = min(1.0, k / fade, (n - 1 - k) / fade)
        yield amp * env * math.sin(2 * math.pi * hz * k / rate)

def riff(fmt, data):
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"LIST" + struct.pack("<I", 5) + b"INFOx\0"   # a chunk to skip, odd-sized
    body += b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body

def pcm_fmt(tag, ch, rate, bits):
    return struct.pack("<HHIIHH", tag, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits)

def s16(chans):
    return b"".join(struct.pack("<%dh" % len(f), *[int(round(v * 32767)) for v in f])
                    for f in zip(*chans))

def write(name, data):
    open("%s/%s" % (out, name), "wb").write(data)

l = list(tone(48000, 1.5, 440)); r = list(tone(48000, 1.5, 660))
write("s48.wav", riff(pcm_fmt(1, 2, 48000, 16), s16([l, r])))
write("m44.wav", riff(pcm_fmt(1, 1, 44100, 16), s16([list(tone(44100, 1.0, 1000))])))
write("u8.wav", riff(pcm_fmt(1, 1, 22050, 8),
                     bytes(int(round(128 + v * 127)) for v in tone(22050, 1.0, 500))))
x = list(tone(96000, 1.0, 750))
ext = pcm_fmt(0xfffe, 2, 96000, 24) + struct.pack("<HHI", 22, 24, 3) + \
    struct.pack("<H", 1) + bytes.fromhex("000000001000800000aa00389b71")
d24 = b"".join(struct.pack("<i", int(round(v * 8388607)))[:3] * 2 for v in x)
write("x24.wav", riff(ext, d24))
write("garbage.wav", bytes((k * 37 + 11) & 255 for k in range(4096)))
write("cut.wav", open("%s/s48.wav" % out, "rb").read()[:30])
fl = list(tone(48000, 0.2, 440))
write("float.wav", riff(pcm_fmt(3, 1, 48000, 32), struct.pack("<%df" % len(fl), *fl)))
write("long.wav", riff(pcm_fmt(1, 1, 48000, 16), s16([list(tone(48000, 10.0, 300))])))
PY

cp build/jamos.img "$stick"
for f in s48 m44 u8 x24 garbage cut float long; do
    mcopy -o -i "$stick@@64M" "$out/$f.wav" "::/$f.wav" ||
        { echo "play: can't copy $f.wav to the stick image"; exit 1; }
done

devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0 \
-trace hda_audio_overrun"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_IMAGE="$stick" QEMU_EXTRA="$devs" \
    QEMU_INPUT=tools/shell-tests/play.txt \
    tools/qemu-test.sh "$out" play shell > "$out/play.out" 2>&1 ||
    { echo "play: the script failed"; grep "serial-feed: .*no '" "$out/play.out"; ok=0; }
rm -f "$stick"

drops=$(grep -ac "hda_audio_overrun" "$out/play.out" || true)
[ "$drops" = 0 ] || echo "play: QEMU dropped its codec buffer $drops time(s) (the host was late)"

python3 - "$wav" "$drops" <<'PY' || ok=0
import math, struct, sys
drops = int(sys.argv[2])
data = open(sys.argv[1], "rb").read()
i = data.find(b"data")
if data[:4] != b"RIFF" or i < 0:
    sys.exit("play: %s is not a WAV file" % sys.argv[1])
pcm = data[i + 8:]
n = len(pcm) // 4
fr = struct.unpack_from("<%dh" % (2 * n), pcm)
L, R = fr[0::2], fr[1::2]
# The sounds: runs of frames with sound, split at 20 ms of silence.
segs, start, quiet = [], None, 0
for k in range(n):
    if L[k] or R[k]:
        if start is None:
            start = k
        quiet, last = 0, k
    elif start is not None:
        quiet += 1
        if quiet > 960:
            segs.append((start, last)); start = None
if start is not None:
    segs.append((start, last))

def hz(x):
    ups = [k + x[k] / float(x[k] - x[k + 1]) for k in range(len(x) - 1) if x[k] <= 0 < x[k + 1]]
    return (len(ups) - 1) * 48000.0 / (ups[-1] - ups[0]) if len(ups) > 2 else 0

#        name      ms     left  right  mono
want = [("s48", 1500, 440, 660, False), ("m44", 1000, 1000, 1000, True),
        ("u8", 1000, 500, 500, True), ("x24", 1000, 750, 750, True),
        ("long", None, 300, 300, True)]
fails = []
left = [drops]   # QEMU buffer drops traced, not yet spent excusing a check

def excused(what):
    """A failed check one traced drop explains: spend it, say so."""
    if left[0] <= 0:
        return False
    left[0] -= 1
    print("play: %s: excused by a QEMU buffer drop" % what)
    return True

if len(segs) != len(want):
    sys.exit("play: the capture has %d sounds (%s), want %d"
             % (len(segs), ", ".join("%.0f ms" % ((b - a + 1) / 48.0) for a, b in segs), len(want)))
peaks = {}
for (a, b), (name, ms, hl, hr, mono) in zip(segs, want):
    l, r = L[a:b + 1], R[a:b + 1]
    dur = (b - a + 1) / 48.0
    fl, fr_ = hz(l), hz(r)
    peaks[name] = max(max(abs(v) for v in l), max(abs(v) for v in r))
    after = (segs[segs.index((a, b)) + 1][0] if (a, b) != segs[-1] else n) - b - 1
    print("play: %s: %.1f ms, %.2f Hz left, %.2f Hz right, peak %d, %d frames of silence after"
          % (name, dur, fl, fr_, peaks[name], after))
    short = ms and ms - dur > ms * 0.02 and ms - dur <= ms * 0.02 + drops * 2048 / 48.0
    if ms and abs(dur - ms) > ms * 0.02 and not (short and excused("%s lasts %.1f ms" % (name, dur))):
        fails.append("%s lasts %.1f ms, want %d within 2 %%" % (name, dur, ms))
    if abs(fl - hl) > hl * 0.01 or abs(fr_ - hr) > hr * 0.01:
        fails.append("%s is %.2f/%.2f Hz, want %d/%d within 1 %%" % (name, fl, fr_, hl, hr))
    if mono and l != r:
        fails.append("%s: the channels differ" % name)
    # The stopped one is the capture's last sound: what follows it is only
    # how long the output stayed open after its fade drained (0 to about
    # a period, 42.7 ms), so it may be none at all; that it ended in its
    # fade, not cut off, is checked below.
    if name != "long" and after < 2400:
        fails.append("%s: only %d frames of silence after it" % (name, after))
    # No click: no step between samples bigger than the sine's own.
    jump = max(abs(x[k + 1] - x[k]) for x in (l, r) for k in range(len(x) - 1))
    if jump > peaks[name] * 2 * math.pi * max(hl, hr) / 48000 * 1.3 + 300 and \
            not excused("%s: a jump of %d" % (name, jump)):
        fails.append("%s: a jump of %d between samples (a click)" % (name, jump))
    if name == "long":
        if dur > 3000:
            fails.append("long: %.0f ms after Ctrl+C at about 2 s: did not stop" % dur)
        endp = max(abs(v) for v in l[-48:])
        if endp > 0.5 * peaks[name]:
            fails.append("long: no fade: its last 1 ms reaches %d of %d" % (endp, peaks[name]))
        # Its last sample is the fade's end (a 240th of the tone or less),
        # not the tone cut off when the capture stopped.
        endv = max(abs(l[-1]), abs(r[-1]))
        if endv > 0.02 * peaks[name]:
            fails.append("long: cut off at %d of %d, not faded to nothing" % (endv, peaks[name]))
# -v -20 played x24's stream at -20 dB in the mixer (Q15 3277: a tenth),
# the device at its -30 dB like the others.
ratio = peaks["x24"] / float(peaks["s48"])
print("play: x24 at -20 dB vs s48 at 0 dB (both at the device's -30): peaks %d/%d = %.4f "
      "(want 0.1000)" % (peaks["x24"], peaks["s48"], ratio))
if abs(ratio - 0.1) > 0.005:
    fails.append("x24's peak is %.4f of s48's, want 0.1000 within 0.005" % ratio)
if abs(peaks["m44"] / float(peaks["s48"]) - 1) > 0.05 or abs(peaks["long"] / float(peaks["s48"]) - 1) > 0.05:
    fails.append("m44/long peaks %d/%d differ from s48's %d (a volume left over from x24?)"
                 % (peaks["m44"], peaks["long"], peaks["s48"]))
if fails:
    sys.exit("play: " + "; ".join(fails))
PY

if [ $ok = 1 ]; then
    echo "play: PASS"
    exit 0
fi
echo "play: FAIL (see $out/play.log, $out/play.out, $wav)"
exit 1
