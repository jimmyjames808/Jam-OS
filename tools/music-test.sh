#!/bin/sh
# The background music player in QEMU (docs/history/A2-PLAN.md, "Music player";
# tools/shell-tests/music.txt is what is typed). A folder tree is made on
# the stick's data partition with mtools, as a Mac would copy a library:
#   /data/music/A/Album/1. One.mp3                         300 Hz, MP3 44.1 kHz stereo
#   /data/music/A/Album/2. Two.wav                         400 Hz, WAV 48 kHz stereo
#   /data/music/B'Side/2001 Disc (Deluxe)/3. Three's (H.O.V.A.).mp3
#                                                          500 Hz, MP3 48 kHz mono VBR
#   /data/music/JAŸ-Z/2025 $ome $ongs 4 U/4. Fünf ~ Ÿ.mp3  600 Hz, MP3 22.05 kHz mono
#   /data/music/Spaced Name/Some Album/5. Five Spaced.WAV  700 Hz, WAV 44.1 kHz mono
#   /data/music/Loose.Mp3                                  800 Hz, MP3 (no artist)
#   /data/music/Bad/x/garbage.mp3                          noise: skipped
#   .DS_Store (top and in A), ._Loose.Mp3, .onthespot-library.json,
#   notes.txt, Empty/Nested/ (an empty folder tree)
# each tone 2 s at a quarter of full scale; and a second stick with
# /music/A/Album/{1,2,3}. Long*.wav (8 s each) for the pulled-stick part.
# The capture (QEMU's hda-output into a WAV file, as play-test) is cut in
# 100 ms windows and each window's loudest tone found (FFT) up to the
# 1000 Hz marker beep typed after `music stop`; then (the log shows the
# UTF-8 names with '?' for each non-ASCII byte: the kernel log is ASCII):
#   - the tracks heard, in order, are the order of the log's "track N:"
#     lines (the shuffle), each of the six once before any repeats, never
#     the same twice in a row, and at least seven of them were heard;
#   - the 1500 Hz beep was mixed over a track (both in the same windows);
#   - between the end of the music and the marker: silence.
# QEMU_SMP passes through. Usage: tools/music-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
wav="$out/music.wav"
stick="$out/music-stick.img"
p="$out/music-p.img"
tmp="$out/music-files"
rm -rf "$wav" "$tmp"
mkdir -p "$tmp"
ok=1
command -v ffmpeg > /dev/null || { echo "music: needs ffmpeg (with libmp3lame) to make the files"; exit 1; }
export LC_ALL=en_US.UTF-8   # mtools: the UTF-8 names as long names

python3 - "$tmp" <<'PY'
import math, random, struct, sys
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

t = tone(44100, 2.0, 300); wav("src1.wav", 44100, [t, t])
t = tone(48000, 2.0, 400); wav("two.wav", 48000, [t, t])
wav("src3.wav", 48000, [tone(48000, 2.0, 500)])
wav("src4.wav", 22050, [tone(22050, 2.0, 600)])
wav("five.wav", 44100, [tone(44100, 2.0, 700)])
t = tone(44100, 2.0, 800); wav("src6.wav", 44100, [t, t])
for k, hz in enumerate((350, 450, 550)):
    t = tone(48000, 8.0, hz); wav("long%d.wav" % (k + 1), 48000, [t, t])
rnd = random.Random(42)
open("%s/garbage.mp3" % out, "wb").write(bytes(rnd.randrange(256) for _ in range(20000)))
open("%s/junk" % out, "wb").write(bytes(rnd.randrange(256) for _ in range(4096)))
open("%s/notes.txt" % out, "w").write("not music\n")
PY
ff="ffmpeg -hide_banner -loglevel error -y"
$ff -i "$tmp/src1.wav" -c:a libmp3lame -b:a 192k "$tmp/one.mp3"
$ff -i "$tmp/src3.wav" -c:a libmp3lame -q:a 2 "$tmp/three.mp3"
$ff -i "$tmp/src4.wav" -c:a libmp3lame -b:a 64k "$tmp/four.mp3"
$ff -i "$tmp/src6.wav" -c:a libmp3lame -b:a 128k "$tmp/six.mp3"

cp build/jamos.img "$stick"
img="$stick@@64M"
for d in "music" "music/A" "music/A/Album" "music/B'Side" "music/B'Side/2001 Disc (Deluxe)" \
         "music/JAŸ-Z" "music/JAŸ-Z/2025 \$ome \$ongs 4 U" "music/Spaced Name" \
         "music/Spaced Name/Some Album" "music/Bad" "music/Bad/x" "music/Empty" "music/Empty/Nested"; do
    mmd -i "$img" "::/$d" || { echo "music: can't make $d on the stick image"; exit 1; }
done
put() {
    mcopy -o -i "$img" "$tmp/$1" "::/music/$2" || { echo "music: can't copy $2 to the stick image"; exit 1; }
}
put one.mp3 "A/Album/1. One.mp3"
put two.wav "A/Album/2. Two.wav"
put three.mp3 "B'Side/2001 Disc (Deluxe)/3. Three's (H.O.V.A.).mp3"
put four.mp3 "JAŸ-Z/2025 \$ome \$ongs 4 U/4. Fünf ~ Ÿ.mp3"
put five.wav "Spaced Name/Some Album/5. Five Spaced.WAV"
put six.mp3 "Loose.Mp3"
put garbage.mp3 "Bad/x/garbage.mp3"
put junk ".DS_Store"
put junk "A/.DS_Store"
put junk "._Loose.Mp3"
put junk ".onthespot-library.json"
put notes.txt "notes.txt"
# mtools turns some apostrophes in folder names into '_' (it did for
# "2001 Disc (Int'l Version)"; a Mac's cp doesn't), so the apostrophes here
# are in names it keeps: check.
mdir -i "$img" -/ ::/music > "$out/music-tree.txt" 2>&1 || true
for name in "B'Side" "Three's (H.O.V.A.).mp3" "JAŸ-Z" "Fünf ~ Ÿ.mp3" "\$ome \$ongs 4 U"; do
    grep -qF "$name" "$out/music-tree.txt" || { echo "music: mtools didn't keep the name $name"; exit 1; }
done

python3 tools/mkstick.py "$p" 64 0c
mformat -i "$p@@1M" -T $((63 * 2048)) -F -v STICK-P ::
for d in music music/A music/A/Album; do mmd -i "$p@@1M" "::/$d"; done
for k in 1 2 3; do mcopy -i "$p@@1M" "$tmp/long$k.wav" "::/music/A/Album/$k. Long $k.wav"; done

devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_IMAGE="$stick" QEMU_EXTRA="$devs" \
    QEMU_USB="-drive if=none,id=pimg,format=raw,file=$p " \
    QEMU_INPUT=tools/shell-tests/music.txt \
    tools/qemu-test.sh "$out" music shell > "$out/music.out" 2>&1 ||
    { echo "music: the script failed"; grep "serial-feed: .*no '" "$out/music.out" || true; ok=0; }
rm -f "$stick" "$p"
log="$out/music.log"
for want in "music: track [0-9]+: A - One \(0:02\)" \
            "music: track [0-9]+: B'Side - Three's \(H.O.V.A.\) \(0:02\)" \
            "music: track [0-9]+: JAŸ-Z - Fünf ~ Ÿ \(0:02\)" \
            "music: track [0-9]+: Spaced Name - Five Spaced \(0:02\)" \
            "music: track [0-9]+: Loose \(0:02\)" \
            "music: the mixer stream failed \(.*\): opening a new one" \
            "music: /usb0/music/A/Album/[123]. Long [123].wav: stopped at"; do
    tr -d '\r' < "$log" | grep -qE "$want" || { echo "music: no log line matching '$want'"; ok=0; }
done
if tr -d '\r' < "$log" | grep -E "music: .*(DS_Store|onthespot|notes.txt|_Loose)"; then
    echo "music: a dotfile or a non-music file was tried"
    ok=0
fi

# The order of the log's track lines, as frequencies, for the capture check.
tr -d '\r' < "$log" | sed -n '/music: playing 7 tracks/,/music: stopped/p' |
    sed -n 's/.*music: track [0-9]*: \(.*\) (0:02)$/\1/p' > "$out/music-order.txt"
echo "music: the log's order: $(tr '\n' '|' < "$out/music-order.txt")"

python3 - "$wav" "$out/music-order.txt" <<'PY' || ok=0
import struct, sys
import numpy as np
data = open(sys.argv[1], "rb").read()
i = data.find(b"data")
if data[:4] != b"RIFF" or i < 0:
    sys.exit("music: %s is not a WAV file" % sys.argv[1])
x = np.frombuffer(data[i + 8:i + 8 + (len(data) - i - 8) // 4 * 4], dtype="<i2").reshape(-1, 2)
mono = x.astype(np.float64).mean(axis=1)
# The kernel's log keeps well-formed UTF-8 as it is.
names = {"A - One": 300, "A - Two": 400, "B'Side - Three's (H.O.V.A.)": 500,
         "JAŸ-Z - Fünf ~ Ÿ": 600, "Spaced Name - Five Spaced": 700, "Loose": 800}
order = [names[l.strip()] for l in open(sys.argv[2], encoding="utf-8", errors="replace")
         if l.strip()]
W = 4800
win = np.hanning(W)
freqs = np.fft.rfftfreq(W, 1 / 48000)
def level(spec, hz):
    k = int(round(hz / 10.0))
    return spec[k - 2:k + 3].max()
tones = [300, 400, 500, 600, 700, 800]
seq = []        # per window: the track's tone (0: none), beep, marker, loud
for w in range(len(mono) // W):
    seg = mono[w * W:(w + 1) * W]
    spec = np.abs(np.fft.rfft(seg * win)) / (W / 4)   # a full-scale sine ~ 32767
    lv = {hz: level(spec, hz) for hz in tones + [1000, 1500]}
    best = max(tones, key=lambda h: lv[h])
    seq.append((best if lv[best] > 500 else 0, lv[1500] > 500, lv[1000] > 500,
                np.abs(seg).max()))
# The marker: the first window with 1000 Hz and no track tone.
mk = next((k for k, s in enumerate(seq) if s[2] and s[0] == 0), None)
fails = []
if mk is None:
    sys.exit("music: no 1000 Hz marker beep in the capture")
part = seq[:mk]
heard = []
for k, s in enumerate(part):
    if s[0] and (not heard or heard[-1][0] != s[0]):
        heard.append([s[0], k, k])
    elif s[0]:
        heard[-1][2] = k
heard = [h for h in heard if h[2] - h[1] >= 1]     # at least 200 ms
# Merge what a short drop split (the mixer restarting isn't in this part).
runs = []
for h in heard:
    if runs and runs[-1][0] == h[0]:
        runs[-1][2] = h[2]
    else:
        runs.append(h)
got = [r[0] for r in runs]
print("music: heard %s (%d tracks, %.1f s of music)" % (" ".join(map(str, got)), len(got),
      sum(r[2] - r[1] + 1 for r in runs) / 10.0))
print("music: logged %s" % " ".join(map(str, order)))
if len(got) < 7:
    fails.append("only %d tracks heard before the stop, want 7 or more" % len(got))
if got != order[:len(got)]:
    fails.append("the tracks heard are not in the log's order")
if sorted(got[:6]) != tones:
    fails.append("the first six heard are not the six tracks once each: %s" % got[:6])
if any(a == b for a, b in zip(order, order[1:])):
    fails.append("the log has a track twice in a row: %s" % order)
mixed = [k for k, s in enumerate(part) if s[1] and s[0]]
if not mixed:
    fails.append("no window with the 1500 Hz beep over a track: not mixed")
else:
    print("music: the beep was mixed over a track in %d windows" % len(mixed))
# After the stop: QEMU's capture only runs while the mixer's output is
# open (it closes it when no stream plays), so the stop shows as the
# music fading out (5 ms) and the marker after it with no track under it.
k = mk
while k < len(seq) and seq[k][2]:
    if seq[k][0]:
        fails.append("a track tone under the marker beep, 2 s after the stop: still playing")
        break
    k += 1
# The zero run just before the marker: the music ended where it starts.
amp = np.abs(x).max(axis=1)
zero_runs, z = [], None
for k in range(min(len(amp), (mk + 1) * W)):
    if amp[k] == 0:
        z = k if z is None else z
    else:
        if z is not None and k - z >= 480:
            zero_runs.append((z, k))
        z = None
if not zero_runs:
    fails.append("no silence before the marker beep")
else:
    e, m0 = zero_runs[-1]
    tail = amp[max(0, e - 48):e].max() if e else 0
    print("music: stopped: the music's last 1 ms peaks at %d (a tone is 8192), then %.0f ms "
          "of silence, then the marker" % (tail, (m0 - e) / 48.0))
    if tail > 4096:
        fails.append("no fade at the stop: the music's last 1 ms reaches %d" % tail)
if fails:
    sys.exit("music: " + "; ".join(fails))
PY

rm -rf "$tmp"
if [ $ok = 1 ]; then
    echo "music: PASS"
    exit 0
fi
echo "music: FAIL (see $out/music.log, $out/music.out, $wav)"
exit 1
