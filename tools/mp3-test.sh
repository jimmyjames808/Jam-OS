#!/bin/sh
# `play` of MP3 files in QEMU (docs/A2-PLAN.md, "MP3"): tones made here
# are encoded with ffmpeg (libmp3lame, and its mp2 encoder), put on the
# stick's data partition (mtools, as the Mac would copy them), and the
# shell plays them (tools/shell-tests/mp3.txt) through the mixer, intel-hda
# and an hda-output codec into QEMU's wav backend (48 kHz 16-bit stereo, as
# tools/play-test.sh). The files:
#   a44.mp3   44100 Hz stereo, 192 kbps CBR (LAME's Info tag), 2.49 s:
#             440 Hz left, 660 Hz right
#   v48.mp3   48000 Hz mono VBR (-q:a 2, a Xing tag), no ID3v2 tag, 1 s, 1000 Hz
#   m22.mp3   22050 Hz mono MPEG-2, 64 kbps, no Xing/Info tag (the length
#             is estimated from the size; the encoder's delay and padding
#             are played), 1.5 s, 500 Hz
#   tag.mp3   44100 Hz stereo 128 kbps with an ID3v2.3 tag holding ~230 KiB
#             of album art, and an ID3v1 tag at the end, 1 s, 750 Hz
#   l2.mp2    MPEG-1 Layer II, 48000 Hz stereo 192 kbps, 1 s, 300 Hz
#   bad.mp3   a44.mp3 with 3000 bytes of noise in its middle (frames lost)
#   cut.mp3   a44.mp3 cut off mid-frame at 60 % of its bytes
#   garbage.mp3 (noise), id3.mp3 (an ID3v2 tag, then noise): refused
#   long.mp3  44100 Hz stereo 128 kbps, 10 s, 600 Hz: Ctrl+C after ~2 s,
#             then `play -n` decodes it whole and says how long that took
# The capture then holds eight sounds, each with silence after it, checked
# as play-test does: each one's frequency (zero crossings over its middle
# 80 %) within 1 % on each channel, its length (within 2 % for the ones with a LAME tag, whose
# delay and padding dr_mp3 drops; within a range for the others), mono
# files equal on both channels, no clicks (bad and cut excepted: frames are
# missing there), Ctrl+C's fade. From `play -n`: all 441000 frames of
# long.mp3 and the decoding cost per second of audio (printed).
# QEMU_SMP passes through. Usage: tools/mp3-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
wav="$out/mp3.wav"
stick="$out/mp3-stick.img"
rm -f "$wav"
ok=1
command -v ffmpeg > /dev/null || { echo "mp3: needs ffmpeg (with libmp3lame) to make the files"; exit 1; }

python3 - "$out" <<'PY'
import math, os, random, struct, sys
out = sys.argv[1]

def tone(rate, secs, hz, amp=0.25):
    n = int(round(rate * secs))
    fade = int(rate * 0.005)
    return [amp * min(1.0, k / fade, (n - 1 - k) / fade) * math.sin(2 * math.pi * hz * k / rate)
            for k in range(n)]

def wav(name, rate, chans):
    data = b"".join(struct.pack("<%dh" % len(f), *[int(round(v * 32767)) for v in f])
                    for f in zip(*chans))
    ch = len(chans)
    fmt = struct.pack("<HHIIHH", 1, ch, rate, rate * ch * 2, ch * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data)) + data
    open("%s/%s" % (out, name), "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)

wav("src-a44.wav", 44100, [tone(44100, 2.49, 440), tone(44100, 2.49, 660)])
wav("src-v48.wav", 48000, [tone(48000, 1.0, 1000)])
wav("src-m22.wav", 22050, [tone(22050, 1.5, 500)])
t = tone(44100, 1.0, 750)
wav("src-tag.wav", 44100, [t, t])
t = tone(48000, 1.0, 300)
wav("src-l2.wav", 48000, [t, t])
t = tone(44100, 10.0, 600)
wav("src-long.wav", 44100, [t, t])
rnd = random.Random(808)
open("%s/garbage.mp3" % out, "wb").write(bytes(rnd.randrange(256) for _ in range(20000)))
# An ID3v2.4 tag of 100 bytes (sizes in 7-bit bytes), then noise.
open("%s/id3.mp3" % out, "wb").write(b"ID3\x04\x00\x00\x00\x00\x00\x64" + bytes(100) +
                                     bytes(rnd.randrange(256) for _ in range(20000)))
PY

ff="ffmpeg -hide_banner -loglevel error -y"
$ff -i "$out/src-a44.wav" -c:a libmp3lame -b:a 192k "$out/a44.mp3"
$ff -i "$out/src-v48.wav" -c:a libmp3lame -q:a 2 -id3v2_version 0 "$out/v48.mp3"
$ff -i "$out/src-m22.wav" -c:a libmp3lame -b:a 64k -write_xing 0 "$out/m22.mp3"
$ff -f lavfi -i "nullsrc=s=400x400,geq=random(1)*255:128:128" -frames:v 1 "$out/art.png"
$ff -i "$out/src-tag.wav" -i "$out/art.png" -map 0 -map 1 -c:a libmp3lame -b:a 128k -c:v copy \
    -id3v2_version 3 -write_id3v1 1 -metadata title="A tone" -metadata artist="Jam OS" \
    -metadata:s:v comment="Cover (front)" "$out/tag.mp3"
$ff -i "$out/src-l2.wav" -c:a mp2 -b:a 192k "$out/l2.mp2"
$ff -i "$out/src-long.wav" -c:a libmp3lame -b:a 128k "$out/long.mp3"
python3 - "$out" <<'PY'
import random, sys
out = sys.argv[1]
a = bytearray(open("%s/a44.mp3" % out, "rb").read())
rnd = random.Random(909)
at = len(a) * 2 // 5
a[at:at + 3000] = bytes(rnd.randrange(256) for _ in range(3000))
open("%s/bad.mp3" % out, "wb").write(a)
a = open("%s/a44.mp3" % out, "rb").read()
open("%s/cut.mp3" % out, "wb").write(a[:len(a) * 3 // 5 + 101])
tag = open("%s/tag.mp3" % out, "rb").read()
if tag[:3] != b"ID3" or tag[-128:-125] != b"TAG" or len(tag) < 200000:
    sys.exit("mp3: tag.mp3 lacks its ID3v2 art or its ID3v1 tag")
if open("%s/v48.mp3" % out, "rb").read()[:3] == b"ID3":
    sys.exit("mp3: v48.mp3 has an ID3v2 tag")
PY

cp build/jamos.img "$stick"
for f in a44.mp3 v48.mp3 m22.mp3 tag.mp3 l2.mp2 bad.mp3 cut.mp3 garbage.mp3 id3.mp3 long.mp3; do
    mcopy -o -i "$stick@@64M" "$out/$f" "::/$f" ||
        { echo "mp3: can't copy $f to the stick image"; exit 1; }
done

devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-180} QEMU_IMAGE="$stick" QEMU_EXTRA="$devs" \
    QEMU_INPUT=tools/shell-tests/mp3.txt \
    tools/qemu-test.sh "$out" mp3 shell > "$out/mp3.out" 2>&1 ||
    { echo "mp3: the script failed"; grep "serial-feed: .*no '" "$out/mp3.out"; ok=0; }
rm -f "$stick"

# play -n: the decoding cost, and every frame of long.mp3.
cost=$(tr -d '\r' < "$out/mp3.log" | grep "play: long.mp3: 0:10 (" | tail -1)
echo "mp3: ${cost:-no play -n line}"
case "$cost" in
*"(441000 frames)"*) ;;
*) echo "mp3: play -n did not decode long.mp3's 441000 frames"; ok=0 ;;
esac

python3 - "$wav" <<'PY' || ok=0
import math, struct, sys
data = open(sys.argv[1], "rb").read()
i = data.find(b"data")
if data[:4] != b"RIFF" or i < 0:
    sys.exit("mp3: %s is not a WAV file" % sys.argv[1])
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
    # The middle 80 %: MP3's coding noise around the near-silent ends (an
    # encoder's delay, Layer II's padding) adds zero crossings of its own.
    x = x[len(x) // 10:len(x) * 9 // 10]
    ups = [k + x[k] / float(x[k] - x[k + 1]) for k in range(len(x) - 1) if x[k] <= 0 < x[k + 1]]
    return (len(ups) - 1) * 48000.0 / (ups[-1] - ups[0]) if len(ups) > 2 else 0

#        name    ms range      left right mono   clicks checked
want = [("a44", 2490 * .98, 2490 * 1.02, 440, 660, False, True),
        ("v48", 1000 * .98, 1000 * 1.02, 1000, 1000, True, True),
        # no LAME tag: 1.5 s plus up to the encoder's delay and padding
        ("m22", 1500 * .98, 34560 / 22.05 * 1.02, 500, 500, True, True),
        ("tag", 1000 * .98, 1000 * 1.02, 750, 750, False, True),
        ("l2", 1000 * .98, 48384 / 48.0 * 1.02, 300, 300, False, True),
        ("bad", 2490 * .85, 2490 * 1.02, 440, 660, False, False),
        ("cut", 2490 * .50, 2490 * .70, 440, 660, False, False),
        ("long", None, None, 600, 600, False, True)]
fails = []
if len(segs) != len(want):
    sys.exit("mp3: the capture has %d sounds (%s), want %d"
             % (len(segs), ", ".join("%.0f ms" % ((b - a + 1) / 48.0) for a, b in segs), len(want)))
for idx, ((a, b), (name, lo, hi, hl, hr, mono, clicks)) in enumerate(zip(segs, want)):
    l, r = L[a:b + 1], R[a:b + 1]
    dur = (b - a + 1) / 48.0
    fl, fr_ = hz(l), hz(r)
    peak = max(max(abs(v) for v in l), max(abs(v) for v in r))
    after = (segs[idx + 1][0] if idx + 1 < len(segs) else n) - b - 1
    print("mp3: %s: %.1f ms, %.2f Hz left, %.2f Hz right, peak %d, %d frames of silence after"
          % (name, dur, fl, fr_, peak, after))
    if lo is not None and not lo <= dur <= hi:
        fails.append("%s lasts %.1f ms, want %.0f-%.0f" % (name, dur, lo, hi))
    # bad's middle holds what the decoder made of the noise (junk frames
    # before it resynchronises), which adds zero crossings: 5 % there.
    tol = 0.05 if name == "bad" else 0.01
    if abs(fl - hl) > hl * tol or abs(fr_ - hr) > hr * tol:
        fails.append("%s is %.2f/%.2f Hz, want %d/%d within %d %%" % (name, fl, fr_, hl, hr,
                                                                     round(tol * 100)))
    if mono and l != r:
        fails.append("%s: the channels differ" % name)
    # long is the capture's last sound, stopped by Ctrl+C: what follows it is
    # only how long the output stayed open after its fade (0 to a period,
    # so maybe none); that it ended in its fade is checked below.
    if name != "long" and after < 2400:
        fails.append("%s: only %d frames of silence after it" % (name, after))
    if peak < 2000:
        fails.append("%s: peak %d, too quiet for a quarter-scale tone at -30 dB" % (name, peak))
    if clicks:
        # No click: no step between samples much bigger than the sine's own
        # (MP3's coding noise allowed for).
        jump = max(abs(x[k + 1] - x[k]) for x in (l, r) for k in range(len(x) - 1))
        if jump > peak * 2 * math.pi * max(hl, hr) / 48000 * 1.3 + 300:
            fails.append("%s: a jump of %d between samples (a click)" % (name, jump))
    if name == "long":
        if dur > 3000:
            fails.append("long: %.0f ms after Ctrl+C at about 2 s: did not stop" % dur)
        endp = max(abs(v) for v in l[-48:])
        if endp > 0.5 * peak:
            fails.append("long: no fade: its last 1 ms reaches %d of %d" % (endp, peak))
        # Its last sample is the fade's end (a 240th of the tone or less),
        # not the tone cut off when the capture stopped.
        endv = max(abs(l[-1]), abs(r[-1]))
        if endv > 0.02 * peak:
            fails.append("long: cut off at %d of %d, not faded to nothing" % (endv, peak))
if fails:
    sys.exit("mp3: " + "; ".join(fails))
PY

if [ $ok = 1 ]; then
    echo "mp3: PASS"
    exit 0
fi
echo "mp3: FAIL (see $out/mp3.log, $out/mp3.out, $wav)"
exit 1
