#!/bin/sh
# Sound quality in QEMU: known signals made here are put on the stick's
# data partition (mtools), the shell plays them (tools/shell-tests/
# quality.txt: `play -s`, through the mixer) into intel-hda with an
# hda-output codec (mixer=off: its amps leave the samples alone) whose
# output goes to a WAV capture (QEMU's wav backend, 48 kHz 16-bit stereo:
# QEMU's codec takes 16-bit only, so this is the mixer's 16-bit output,
# dithered wherever a volume leaves a fraction). Measured from the capture
# (numpy):
#   bitx    48 kHz stereo at 0 dB: every sample bit-exact (a 997 Hz sine
#           with LSB noise left, full-range noise with both extremes
#           right)
#   t1k     a 1 kHz sine at -1 dBFS played at 0, -20, -40 and -60 dB of
#           stream volume: the level within 0.1 dB, THD+N (20 Hz-20 kHz)
#           and the largest spur; with the dither no spur may stand above
#           the noise (spur under -100 dB re the tone at 0 dB, and no
#           harmonic of 1 kHz above the noise at -40/-60 dB)
#   r44     44.1 kHz tones at 1, 10, 15, 19 and 20 kHz (resampled): each
#           tone's level within 0.1 dB (the passband), and the largest
#           other component (an image or alias: 20 kHz's lands on
#           23.9 kHz) under -90 dB re the tone
#   long    a 20 s 1 kHz tone read from the stick while it plays: no
#           10 ms window off its level by more than 0.5 dB (a dropout),
#           its length within 10 ms, and `play -s` says 0 underruns and
#           0 late periods
# Every sound: no jump between samples bigger than its sine's own (a
# click), from the silence before it to the silence after.
# AQ_NOSTATS=1 runs without `play -s` (for a build that lacks it).
# QEMU_SMP passes through. Usage: tools/audio-quality-test.sh <outdir>;
# exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
wav="$out/quality.wav"
stick="$out/quality-stick.img"
script="$out/quality.txt"
rm -f "$wav"
ok=1
if [ "${AQ_NOSTATS:-0}" = 1 ]; then
    sed 's/play -s /play /' tools/shell-tests/quality.txt > "$script"
else
    cp tools/shell-tests/quality.txt "$script"
fi

python3 - "$out" <<'PY'
import struct, sys
import numpy as np
out = sys.argv[1]

def riff(ch, rate, frames):
    data = frames.astype("<i2").tobytes()
    fmt = struct.pack("<HHIIHH", 1, ch, rate, rate * ch * 2, ch * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body

def fade(n, rate):
    f = int(rate * 0.005)
    e = np.ones(n)
    r = 0.5 - 0.5 * np.cos(np.pi * np.arange(f) / f)
    e[:f] = r
    e[n - f:] = r[::-1]
    return e

def tone(rate, secs, hz, amp):
    n = int(rate * secs)
    t = np.arange(n) / rate
    return amp * fade(n, rate) * np.sin(2 * np.pi * hz * t)

rng = np.random.default_rng(1234)
n = 48000
t = np.arange(n) / 48000
L = np.round(16000 * np.sin(2 * np.pi * 997 * t)) + rng.integers(-3, 4, n)
R = rng.integers(-32768, 32768, n)
R[:4] = [-32768, 32767, -32768, 32767]
L[0] = 1000
np.save("%s/bitx.npy" % out, np.stack([L, R], 1).astype(np.int16))
open("%s/bitx.wav" % out, "wb").write(riff(2, 48000, np.stack([L, R], 1).reshape(-1)))

x = np.round(tone(48000, 1.2, 1000, 0.891) * 32767)
open("%s/t1k.wav" % out, "wb").write(riff(1, 48000, x))

parts = []
for hz in (1000, 10000, 15000, 19000, 20000):
    parts.append(np.round(tone(44100, 0.4, hz, 0.5) * 32767))
    parts.append(np.zeros(4410))
open("%s/r44.wav" % out, "wb").write(riff(1, 44100, np.concatenate(parts[:-1])))

x = np.round(tone(48000, 20.0, 1000, 0.25) * 32767)
open("%s/long.wav" % out, "wb").write(riff(2, 48000, np.stack([x, x], 1).reshape(-1)))
PY

cp build/jamos.img "$stick"
for f in bitx t1k r44 long; do
    mcopy -o -i "$stick@@64M" "$out/$f.wav" "::/$f.wav" ||
        { echo "quality: can't copy $f.wav to the stick image"; exit 1; }
done

devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0,mixer=off"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-200} QEMU_IMAGE="$stick" QEMU_EXTRA="$devs" QEMU_INPUT="$script" \
    tools/qemu-test.sh "$out" quality shell > "$out/quality.out" 2>&1 ||
    { echo "quality: the script failed"; grep "serial-feed: .*no '" "$out/quality.out"; ok=0; }
rm -f "$stick"
log="$out/quality.log"

grep -E "^play: .*(frames|underrun|lead)" "$log" | tr -d '\r' | sed 's/^/quality: /' || true
if [ "${AQ_NOSTATS:-0}" != 1 ]; then
    n=$(grep -cE "^play: stats: .* 0 underruns, 0 late" "$log" || true)
    [ "$n" -ge 7 ] || { echo "quality: only $n of 7 plays said 0 underruns and 0 late periods"; ok=0; }
fi
if grep -q "frames late" "$log"; then
    echo "quality: the mixer was late:"; grep "frames late" "$log" | head -3; ok=0
fi
grep -E "mixer: stream [0-9]+ \(play\) closed" "$log" | tr -d '\r' | sed 's/^/quality: /' | head -8
if grep -E "mixer: stream [0-9]+ \(play\) closed" "$log" | grep -qvE " 0 underrun"; then
    echo "quality: a stream had underruns"; ok=0
fi

python3 - "$wav" "$out" <<'PY' || ok=0
import sys
import numpy as np
data = open(sys.argv[1], "rb").read()
i = data.find(b"data")
if data[:4] != b"RIFF" or i < 0:
    sys.exit("quality: %s is not a WAV file" % sys.argv[1])
pcm = np.frombuffer(data[i + 8:i + 8 + (len(data) - i - 8) // 4 * 4], "<i2").reshape(-1, 2)
L, R = pcm[:, 0].astype(np.int64), pcm[:, 1].astype(np.int64)
loud = (L != 0) | (R != 0)
# The sounds: runs with sound, split at 20 ms of exact silence.
idx = np.flatnonzero(loud)
segs = []
if len(idx):
    breaks = np.flatnonzero(np.diff(idx) > 960)
    starts = np.concatenate([[idx[0]], idx[breaks + 1]])
    ends = np.concatenate([idx[breaks], [idx[-1]]])
    segs = list(zip(starts, ends))
names = ["bitx", "t1k 0", "t1k -20", "t1k -40", "t1k -60",
         "r44 1k", "r44 10k", "r44 15k", "r44 19k", "r44 20k", "long"]
fails = []
print("quality: %d sounds in the capture (%s)" % (len(segs), ", ".join(
    "%.0f ms" % ((b - a + 1) / 48.0) for a, b in segs)))
if len(segs) != len(names):
    sys.exit("quality: want %d sounds" % len(names))

def bh(n):
    k = np.arange(n) / n * 2 * np.pi
    return 0.35875 - 0.48829 * np.cos(k) + 0.14128 * np.cos(2 * k) - 0.01168 * np.cos(3 * k)

def analyse(x, hz):
    """level (dBFS), THD+N (dB, 20 Hz-20 kHz), largest spur (dB re the tone), harmonics."""
    n = len(x)
    w = bh(n)
    X = np.abs(np.fft.rfft(x * w)) ** 2
    df = 48000.0 / n
    f = np.arange(len(X)) * df
    k0 = int(round(hz / df))
    band = slice(max(k0 - 12, 0), k0 + 13)
    fund = X[band].sum()
    amp = np.sqrt(4 * fund / (n * (w ** 2).sum()))   # the peak, from the band's power
    rest = X.copy()
    rest[band] = 0
    rest[:int(20 / df) + 1] = 0
    inband = rest[:int(20000 / df) + 1].sum()
    spur = rest.max()
    sf = f[rest.argmax()]
    harm = max(X[int(round(m * hz / df)) - 6:int(round(m * hz / df)) + 7].max()
               for m in range(2, 10) if m * hz < 23000) if hz * 2 < 23000 else 0
    noise = np.median(rest[int(20 / df):int(20000 / df)])
    return (20 * np.log10(amp / 32768.0), 10 * np.log10(inband / fund),
            10 * np.log10(spur / X[band].max()), sf, 10 * np.log10(max(harm, 1e-30) / max(noise, 1e-30)))

def clicks(name, a, b, amp, hz):
    seg = np.concatenate([[0], L[a:b + 1], [0]]), np.concatenate([[0], R[a:b + 1], [0]])
    jump = max(np.abs(np.diff(s)).max() for s in seg)
    lim = 2 * amp * np.sin(np.pi * hz / 48000) * 1.1 + 4
    if jump > lim:
        k = max(range(2), key=lambda c: np.abs(np.diff(seg[c])).max())
        at = np.abs(np.diff(seg[k])).argmax()
        fails.append("%s: a jump of %d between samples at %.1f ms (its sine allows %d): a click"
                     % (name, jump, at / 48.0, lim))

# bitx: sample for sample.
want = np.load("%s/bitx.npy" % sys.argv[2]).astype(np.int64)
a, b = segs[0]
got = pcm[a:a + len(want)].astype(np.int64)
bad = np.flatnonzero((got != want).any(1)) if len(got) == len(want) else [0]
print("quality: bitx: %d frames, %d differ; the sound lasts %d frames" % (len(want), len(bad), b - a + 1))
if len(bad) or b - a + 1 != len(want):
    fails.append("bitx is not bit-exact: %d of %d frames differ (first at %s), %d frames long"
                 % (len(bad), len(want), bad[0] if len(bad) else "-", b - a + 1))

# t1k at four volumes.
for k, vol in enumerate((0, -20, -40, -60)):
    a, b = segs[1 + k]
    mid = (a + b) // 2
    x = L[mid - 12000:mid + 12000].astype(float)
    lvl, thdn, spur, sf, harm = analyse(x, 1000)
    want = 20 * np.log10(0.891 * 32767 / 32768) + vol
    print("quality: t1k at %3d dB: level %.2f dBFS (want %.2f), THD+N %.1f dB, largest spur "
          "%.1f dB at %.0f Hz, harmonics %s dB over the noise" % (vol, lvl, want, thdn, spur, sf,
                                                         "%.1f" % harm if harm < 99 else ">99"))
    if abs(lvl - want) > 0.1:
        fails.append("t1k at %d dB: level %.2f dBFS, want %.2f" % (vol, lvl, want))
    if vol == 0 and spur > -100:
        fails.append("t1k at 0 dB: a spur at %.1f dB" % spur)
    if vol <= -40 and harm > 15:
        fails.append("t1k at %d dB: harmonics %.1f dB above the noise (undithered)" % (vol, harm))
    if (L[a:b + 1] != R[a:b + 1]).any():
        fails.append("t1k at %d dB: the channels differ" % vol)
    clicks("t1k %d" % vol, a, b, 0.891 * 32767 * 10 ** (vol / 20.0), 1000)

# r44: the resampler's passband and images.
for k, hz in enumerate((1000, 10000, 15000, 19000, 20000)):
    a, b = segs[5 + k]
    mid = (a + b) // 2
    x = L[mid - 6000:mid + 6000].astype(float)
    lvl, thdn, spur, sf, harm = analyse(x, hz)
    resp = lvl - 20 * np.log10(0.5 * 32767 / 32768)
    print("quality: r44 %5d Hz: response %+.3f dB, largest other component %.1f dB at %.0f Hz, "
          "%.0f ms" % (hz, resp, spur, sf, (b - a + 1) / 48.0))
    if abs(resp) > 0.1:
        fails.append("r44 %d Hz: response %+.2f dB" % (hz, resp))
    if spur > -90:
        fails.append("r44 %d Hz: a component at %.0f Hz at %.1f dB (image/alias)" % (hz, sf, spur))
    clicks("r44 %d" % hz, a, b, 0.5 * 32767 * 1.01, hz)

# long: no dropout.
a, b = segs[10]
x = L[a:b + 1].astype(float)
w = 480
rms = np.sqrt((x[:len(x) // w * w].reshape(-1, w) ** 2).mean(1))
body = rms[10:-10]
db = 20 * np.log10(np.maximum(body, 1e-9) / np.median(body))
print("quality: long: %.3f s, 10 ms windows from %.2f to %+.2f dB of the median" %
      ((b - a + 1) / 48000.0, db.min(), db.max()))
if abs((b - a + 1) - 20 * 48000) > 480:
    fails.append("long lasts %d frames, want 960000 within 480" % (b - a + 1))
if db.min() < -0.5 or db.max() > 0.5:
    fails.append("long: a 10 ms window at %.2f/%+.2f dB (a dropout)" % (db.min(), db.max()))
clicks("long", a, b, 0.25 * 32767, 1000)
if fails:
    sys.exit("quality: " + "; ".join(fails))
PY

if [ $ok = 1 ]; then
    echo "quality: PASS"
    exit 0
fi
echo "quality: FAIL (see $out/quality.log, $out/quality.out, $wav)"
exit 1
