#!/bin/sh
# jamjar, the music player's window (docs/history/MUSIC-GUI.md), in QEMU:
# a plain boot ("shell") with a usb-kbd and a usb-mouse, an hda-output
# codec into a WAV file (so the player really plays and its bands are of
# real sound), and a library made by this script on the stick's data
# partition, laid out as the owner's is (OnTheSpot/Artist/Year_Album/
# Song_Name.mp3, UTF-8 names): six made-up songs (a kick, a bass line,
# chords, hats and an arpeggio, 20 s each, as MP3) with made-up covers in
# their ID3 tags (PNG in 2.3 and 2.4 tags, one 611x640, and a JPEG), a
# 1 kHz test tone (WAV, no cover), and two MP3s whose covers must be
# refused (a PNG said to be 5000x5000, a garbled JPEG). tools/shell-tests/jamjar.txt (JAMJAR_HD=1:
# tools/shell-tests/jamjar-hd.txt, at the PC's 2560x1440) runs the
# self-test, jamjar without the player (`run jamjar`), then with it (the
# shell's `jamjar`, `trace` on, so it says each command and what the
# bands show): plays an album by keys, next, back, pause, volume, search,
# the 1 kHz tone (the loudest bar must be bar 34, 0.96-1.05 kHz), help,
# roulette, the full jar, the sleep timer, mouse clicks on a button, the
# volume, the jam and a track row; quits, and the music must still be
# playing (`music status`). Screenshots: <outdir>/jamjar-*.png.
# QEMU_SMP passes through. Usage: tools/jamjar-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
name=jamjar
script=tools/shell-tests/jamjar.txt
if [ "${JAMJAR_HD:-}" = 1 ]; then
    name=jamjar-hd
    script=tools/shell-tests/jamjar-hd.txt
    QEMU_EXTRA_VGA="-vga none -device VGA,xres=2560,yres=1440,vgamem_mb=64"
fi
wav="$out/$name.wav"
stick="$out/$name-stick.img"
tmp="$out/$name-files"
rm -rf "$tmp" "$wav"
mkdir -p "$tmp"
command -v ffmpeg > /dev/null || { echo "jamjar: needs ffmpeg (with libmp3lame)"; exit 1; }
export LC_ALL=en_US.UTF-8   # mtools: the UTF-8 names as long names
# A name that fits 8.3 in a DOS code page ("¥$") gets no long name, so its
# short name's code page must be the fat service's (FatFs: 437), or the
# yen comes back as another character.
echo "default_codepage=437" > "$out/$name.mtoolsrc"
export MTOOLSRC="$out/$name.mtoolsrc"

python3 - "$tmp" <<'PY'
import math, struct, sys
import numpy as np
out, rate = sys.argv[1], 44100

def wav(name, x):
    x = np.clip(x, -1, 1)
    data = (np.stack([x, x], axis=1) * 32767).astype("<i2").tobytes()
    fmt = struct.pack("<HHIIHH", 1, 2, rate, rate * 4, 4, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + \
        struct.pack("<I", len(data)) + data
    open("%s/%s" % (out, name), "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)

def song(seed, secs=20.0):
    rng = np.random.default_rng(seed)
    n = int(rate * secs)
    t = np.arange(n) / rate
    bpm = 96 + 8 * seed
    beat = 60.0 / bpm
    root = 41.2 * 2 ** (rng.integers(0, 7) / 12)        # E1 and up
    prog = [0, 5, 3, 7] if seed % 2 else [0, 8, 3, 10]
    x = np.zeros(n)
    for k in range(int(secs / beat)):                     # the kick
        s = int(k * beat * rate)
        m = min(n - s, int(0.25 * rate))
        tt = np.arange(m) / rate
        f = 50 + 70 * np.exp(-tt * 30)
        x[s:s + m] += 0.9 * np.sin(2 * np.pi * np.cumsum(f) / rate) * np.exp(-tt * 9)
    for k in range(int(secs / beat * 2)):                 # hats and a snare on 2 and 4
        s = int((k + 0.5) * beat / 2 * rate) if k % 2 else int(k * beat / 2 * rate)
        m = min(n - s, int(0.12 * rate))
        if m <= 0:
            continue
        noise = np.diff(rng.standard_normal(m + 1))
        loud = 0.5 if (k % 4 == 2) else 0.12
        x[s:s + m] += loud * noise * np.exp(-np.arange(m) / rate * (18 if loud > 0.2 else 60))
    bar = 4 * beat
    for k in range(int(secs / bar) + 1):                  # bass and chords per bar
        s, e = int(k * bar * rate), min(n, int((k + 1) * bar * rate))
        if s >= n:
            break
        tt = t[s:e] - t[s]
        r = root * 2 ** (prog[k % 4] / 12)
        env = np.minimum(1, tt * 20) * np.exp(-tt * 0.6)
        x[s:e] += 0.35 * env * np.sign(np.sin(2 * np.pi * r * tt)) * 0.6
        for iv in (12, 16 if seed % 2 else 15, 19):
            x[s:e] += 0.08 * np.minimum(1, tt * 3) * np.sin(2 * np.pi * r * 2 ** (iv / 12) * tt)
        for j in range(8):                                # an arpeggio of eighths
            a = int(j * beat / 2 * rate)
            m = min(e - s - a, int(beat / 2 * rate))
            if m <= 0:
                continue
            f = r * 2 ** ((24 + [0, 7, 12, 16, 19, 16, 12, 7][j]) / 12)
            ta = np.arange(m) / rate
            x[s + a:s + a + m] += 0.07 * np.sin(2 * np.pi * f * ta) * np.exp(-ta * 6)
    return 0.6 * x / np.abs(x).max()

for i in range(6):
    wav("song%d.wav" % i, song(i + 1))
t = np.arange(int(rate * 30)) / rate
wav("tone.wav", 0.3 * np.sin(2 * np.pi * 1000 * t))
PY
# The covers: made-up pictures (PIL), as the owner's MP3s carry theirs:
# PNG, about 640x640 (one 611x640), and one JPEG.
python3 - "$tmp" <<'PY' || { echo "jamjar: needs Python's PIL to draw the covers"; exit 1; }
import sys
from PIL import Image, ImageDraw
out = sys.argv[1]
def grad(w, h, a, b):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(3)))
    return im, d
im, d = grad(640, 640, (10, 10, 12), (40, 40, 44))           # 4-44: black, a white frame, 4 bars
d.rectangle([40, 40, 599, 599], outline=(235, 235, 230), width=6)
for k in range(4):
    d.rectangle([150 + 90 * k, 200, 190 + 90 * k, 440], fill=(235, 235, 230))
im.save(out + "/cover-444.png")
im, d = grad(600, 600, (60, 58, 56), (20, 18, 16))           # Vultures: orange rings, a JPEG
for k in range(6):
    r = 40 + 40 * k
    d.ellipse([300 - r, 300 - r, 300 + r, 300 + r], outline=(240, 140 - 15 * k, 30), width=10)
im.save(out + "/cover-vultures.jpg", quality=90)
im, d = grad(611, 640, (200, 20, 40), (90, 0, 20))           # MBDTF: red, a gold diamond
d.polygon([(305, 120), (520, 320), (305, 520), (90, 320)], fill=(220, 170, 50))
d.polygon([(305, 200), (440, 320), (305, 440), (170, 320)], fill=(150, 10, 40))
im.save(out + "/cover-mbdtf.png")
im, d = grad(640, 640, (30, 30, 120), (160, 40, 160))        # Discovery: a grid
for k in range(0, 640, 64):
    d.line([(k, 0), (k, 639)], fill=(250, 220, 250), width=3)
    d.line([(0, k), (639, k)], fill=(250, 220, 250), width=3)
d.ellipse([220, 220, 420, 420], fill=(250, 210, 60))
im.save(out + "/cover-discovery.png")
PY
mp3() {   # mp3 <song> <cover or -> <id3 version> <out>
    if [ "$2" = - ]; then
        ffmpeg -hide_banner -loglevel error -y -i "$tmp/$1.wav" -c:a libmp3lame -b:a 128k \
            -write_id3v2 0 "$tmp/$4"
    else
        ffmpeg -hide_banner -loglevel error -y -i "$tmp/$1.wav" -i "$tmp/$2" -map 0:a -map 1:v \
            -c:a libmp3lame -b:a 128k -c:v copy -id3v2_version "$3" \
            -metadata:s:v title="Album cover" -metadata:s:v comment="Cover (front)" "$tmp/$4"
    fi
}
mp3 song0 cover-444.png 3 song0.mp3
mp3 song1 cover-444.png 3 song1.mp3
mp3 song2 cover-vultures.jpg 3 song2.mp3
mp3 song3 cover-mbdtf.png 3 song3.mp3
mp3 song4 cover-mbdtf.png 3 song4.mp3
mp3 song5 cover-discovery.png 4 song5.mp3
mp3 song0 - - plain.mp3
# Two that must keep their jar labels: a PNG whose header says 5000x5000
# (refused before decoding), and a JPEG that is garbage after its first
# bytes; each an ID3v2.3 tag of one APIC put before a plain MP3.
python3 - "$tmp" <<'PY'
import struct, sys, random
out = sys.argv[1]
def ss(n):
    return bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])
def tagged(name, img, mime):
    data = b"\x00" + mime + b"\x00" + b"\x03" + b"\x00" + img
    frame = b"APIC" + struct.pack(">I", len(data)) + b"\x00\x00" + data
    tag = b"ID3\x03\x00\x00" + ss(len(frame)) + frame
    open("%s/%s" % (out, name), "wb").write(tag + open(out + "/plain.mp3", "rb").read())
big = bytearray(open(out + "/cover-444.png", "rb").read())
big[16:24] = struct.pack(">II", 5000, 5000)
tagged("oversized.mp3", bytes(big), b"image/png")
rnd = random.Random(5)
tagged("garbled.mp3", b"\xff\xd8\xff\xe0" + bytes(rnd.randrange(256) for _ in range(30000)),
       b"image/jpeg")
PY

cp build/jamos.img "$stick"
img="$stick@@64M"
L="music/OnTheSpot"
for d in music "$L" "$L/JAŸ-Z" "$L/JAŸ-Z/2017_4-44" "$L/¥\$" "$L/¥\$/2024_Vultures_1" \
         "$L/Kanye_West" "$L/Kanye_West/2010_My_Beautiful_Dark_Twisted_Fantasy" \
         "$L/Daft_Punk" "$L/Daft_Punk/2001_Discovery" "$L/Test_Tones" \
         "$L/Test_Tones/2020_Calibration" "$L/Broken" "$L/Broken/2019_Oversized" \
         "$L/Broken/2019_Garbled"; do
    mmd -i "$img" "::/$d" || { echo "jamjar: can't make $d on the stick image"; exit 1; }
done
put() {
    mcopy -o -i "$img" "$tmp/$1" "::/$L/$2" || { echo "jamjar: can't copy $2"; exit 1; }
}
put song0.mp3 "JAŸ-Z/2017_4-44/Kill_Jay_Z.mp3"
put song1.mp3 "JAŸ-Z/2017_4-44/The_Story_of_O.J..mp3"
put song2.mp3 "¥\$/2024_Vultures_1/Carnival.mp3"
put song3.mp3 "Kanye_West/2010_My_Beautiful_Dark_Twisted_Fantasy/Runaway.mp3"
put song4.mp3 "Kanye_West/2010_My_Beautiful_Dark_Twisted_Fantasy/Dark_Fantasy.mp3"
put song5.mp3 "Daft_Punk/2001_Discovery/One_More_Time.mp3"
put tone.wav "Test_Tones/2020_Calibration/1000_Hz_Tone.wav"
put oversized.mp3 "Broken/2019_Oversized/Too_Big.mp3"
put garbled.mp3 "Broken/2019_Garbled/Garbled.mp3"
rm -rf "$tmp"

devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0"
ok=1
QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_IMAGE="$stick" \
    QEMU_EXTRA="${QEMU_EXTRA:-} ${QEMU_EXTRA_VGA:-} $devs" \
    QEMU_USB="-device usb-kbd,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3" \
    QEMU_INPUT=$script tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1 ||
    { echo "jamjar: the script failed"; grep "serial-feed: .*no '" "$out/$name.out" || true; ok=0; }
rm -f "$stick"
log="$out/$name.log"
# What the trace said, for the report.
tr -d '\r' < "$log" | grep -aE "jamjar: (spectrum|frames|library|cover)" | head -20 || true
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $out/$name.log, $out/$name.out)"
exit 1
