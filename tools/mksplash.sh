#!/bin/sh
# The boot splash's video (boot/splash.mpg) from the owner's animation,
# which lives outside the repository:
#     <src>/jamos-boot.mov   ProRes 4444 with alpha, 1920x1080, 30 fps, 6.5 s
#     <src>/jamos-boot.wav   48 kHz stereo, its sound
# (by default ~/Movies/motion-graphics/jamos-boot; the Makefile's
# SPLASH_SRC). The result is one MPEG-1 program stream, as pl_mpeg
# (third_party/pl_mpeg) plays it:
#   - video: MPEG-1, 1280x720 (drawn 2x on the PC's 2560x1440), 30 fps,
#     the animation composited over the splash background #1E1A1D (the
#     alpha is straight, not premultiplied), quality -q:v 4, no B-frames
#     (so frames decode in the order they are shown);
#   - audio: MP2, 48 kHz stereo (the mixer's rate: no resampling), 160 kb/s.
# The file is committed, so a build never needs the owner's files; `make`
# runs this only when they are there and newer than it.
# Usage: tools/mksplash.sh <src dir> <out.mpg>
set -eu
src=$1 out=$2
command -v ffmpeg > /dev/null || { echo "mksplash: needs ffmpeg (brew install ffmpeg)"; exit 1; }
tmp="$out.tmp.mpg"
ffmpeg -nostdin -loglevel error -y \
    -f lavfi -i "color=c=0x1E1A1D:s=1280x720:r=30:d=6.5" \
    -i "$src/jamos-boot.mov" -i "$src/jamos-boot.wav" \
    -filter_complex "[1:v]scale=1280:720:flags=lanczos,format=rgba[fg];\
[0:v][fg]overlay=format=auto:shortest=1,format=yuv420p[v]" \
    -map "[v]" -map 2:a \
    -c:v mpeg1video -q:v 4 -bf 0 -g 30 -r 30 \
    -c:a mp2 -b:a 160k -ar 48000 -ac 2 \
    -f mpeg "$tmp"
mv "$tmp" "$out"
echo "mksplash: $out: $(wc -c < "$out" | tr -d ' ') bytes"
