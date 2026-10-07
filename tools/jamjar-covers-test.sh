#!/bin/sh
# jamjar's now-playing cover at the PC's 2560x1440, over many albums
# (tools/shell-tests/jamjar-covers.txt). The owner's library has 93 albums,
# every MP3 with a ~640x640 PNG cover in its ID3v2 tag, played in shuffle,
# and his now-playing card lost its cover now and then while the big view
# showed it. This makes 32 albums of two songs of 12 s each, every cover a
# teal field with the album's number in a corner (no jar label has teal),
# plays everything in shuffle (so the track heard is often not its
# album's first), skips on 18 times (the first skips while the covers are
# still being read), and screenshots the card twice after each skip. Then
# every shot is checked: the middle of now playing's cover (where jamjar
# says it is, the last it said: on the compositor's boot it opens in a
# tile and the script makes it full screen with Super+F) must be the teal
# of a real cover.
# QEMU_SMP passes through. Usage: tools/jamjar-covers-test.sh <outdir>;
# exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
name=jamjar-covers
wav="$out/$name.wav"
stick="$out/$name-stick.img"
tmp="$out/$name-files"
rm -rf "$tmp" "$wav" "$out"/covers-*.png
mkdir -p "$tmp"
command -v ffmpeg > /dev/null || { echo "$name: needs ffmpeg (with libmp3lame)"; exit 1; }
ALBUMS=32

python3 - "$tmp" "$ALBUMS" <<'PY' || { echo "$name: needs numpy and Python's PIL"; exit 1; }
import struct, sys
import numpy as np
from PIL import Image, ImageDraw
out, albums, rate = sys.argv[1], int(sys.argv[2]), 44100
for a in range(albums):
    for t in range(2):   # 12 s of a chord, different for each song
        f = 110.0 * 2 ** ((a * 2 + t) / 12)
        x = np.arange(int(rate * 12)) / rate
        y = 0.2 * (np.sin(2 * np.pi * f * x) + np.sin(2 * np.pi * f * 1.5 * x))
        d = (np.stack([y, y], axis=1) * 32767).astype("<i2").tobytes()
        fmt = struct.pack("<HHIIHH", 1, 2, rate, rate * 4, 4, 16)
        body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + \
            struct.pack("<I", len(d)) + d
        open("%s/s%02d_%d.wav" % (out, a, t), "wb").write(
            b"RIFF" + struct.pack("<I", len(body)) + body)
    im = Image.new("RGB", (640, 640), (0, 170, 170))
    dr = ImageDraw.Draw(im)
    for k in range(a + 1):   # the album's number as dots along the top
        x, y = 20 + 30 * (k % 20), 20 + 30 * (k // 20)
        dr.ellipse([x, y, x + 20, y + 20], fill=(250, 250, 250))
    im.save("%s/c%02d.png" % (out, a))
PY

cp build/jamos.img "$stick"
img="$stick@@64M"
mmd -i "$img" ::/many || { echo "$name: can't make /many on the stick image"; exit 1; }
a=0
while [ $a -lt $ALBUMS ]; do
    n=$(printf %02d $a)
    mmd -i "$img" "::/many/Artist_$n" "::/many/Artist_$n/2020_Album_$n"
    for t in 0 1; do
        ffmpeg -hide_banner -loglevel error -y -i "$tmp/s${n}_$t.wav" -i "$tmp/c$n.png" \
            -map 0:a -map 1:v -c:a libmp3lame -b:a 96k -c:v copy -id3v2_version 3 \
            -metadata:s:v title="Album cover" -metadata:s:v comment="Cover (front)" \
            "$tmp/song.mp3"
        mcopy -o -i "$img" "$tmp/song.mp3" "::/many/Artist_$n/2020_Album_$n/Song_$t.mp3" ||
            { echo "$name: can't copy a song"; exit 1; }
    done
    a=$((a + 1))
done
rm -rf "$tmp"

devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0"
ok=1
QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_IMAGE="$stick" \
    QEMU_EXTRA="${QEMU_EXTRA:-} -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=64 $devs" \
    QEMU_USB="-device usb-kbd,bus=xhci.0,port=2" \
    QEMU_INPUT=tools/shell-tests/jamjar-covers.txt tools/qemu-test.sh "$out" "$name" shell \
    > "$out/$name.out" 2>&1 ||
    { echo "$name: the script failed"; grep "serial-feed: .*no '" "$out/$name.out" || true; ok=0; }
rm -f "$stick" "$wav"
log="$out/$name.log"
tr -d '\r' < "$log" | grep -aE "jamjar: (frames|library|.*no cover)" | head -20 || true

# Every shot: the middle half of the cover's square must be mostly teal.
where=$(tr -d '\r' < "$log" | grep -aoE "now playing's cover at [0-9]+,[0-9]+, [0-9]+ px" |
        tail -1 | grep -oE "[0-9]+" | tr '\n' ' ')
python3 - "$out" $where <<'PY' || ok=0
import glob, sys
from PIL import Image
out, x, y, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
shots = sorted(glob.glob(out + "/covers-*.png"))
bad = []
for p in shots:
    im = Image.open(p).convert("RGB")
    box = (x + size // 4, y + size // 4, x + 3 * size // 4, y + 3 * size // 4)
    px = list(im.crop(box).getdata())
    teal = sum(1 for r, g, b in px if r < 60 and abs(g - 170) < 45 and abs(b - 170) < 45)
    share = teal / len(px)
    print("%s: %3.0f%% of the cover's middle is the cover's teal" % (p.split("/")[-1], 100 * share))
    if share < 0.8:
        bad.append(p)
if not shots:
    print("no shots")
    sys.exit(1)
if bad:
    print("now playing showed no cover in %d of %d shots" % (len(bad), len(shots)))
    sys.exit(1)
PY
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $out/$name.log, $out/$name.out, $out/covers-*.png)"
exit 1
