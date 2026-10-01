#!/bin/sh
# The boot splash's video (boot/splash.mpg) from the owner's animation,
# which lives outside the repository:
#     <src>/jamos-boot.mov   the animation with alpha (ProRes 4444), any
#                            length; made at 2560x1440 (the PC's screen) or
#                            at another 16:9 size
#     <src>/jamos-boot.wav   its sound (any rate; made 48 kHz stereo)
# (by default ~/Movies/motion-graphics/jamos-boot; the Makefile's
# SPLASH_SRC). The result is one MPEG-1 program stream, as pl_mpeg
# (third_party/pl_mpeg) plays it:
#   - video: MPEG-1 at 2560x1440, the PC's own resolution, so the PC shows
#     it pixel for pixel (bin/splash scales it down on smaller screens). A
#     2560x1440 source is used as it is; any other size is scaled to fit
#     with Lanczos (an upscale from 1920x1080 is softer than a native
#     render) and centred. Composited over the splash background #1E1A1D
#     (the alpha is straight, not premultiplied). The source's frame rate
#     when MPEG-1 has it (24, 25, 30, 50, 60 ...), else 30. Quality -q:v 2
#     (quantiser 1-2: about 1.4 MB for 6.5 s at 1440p), no B-frames (so
#     frames decode in the order they are shown);
#   - audio: MP2, 48 kHz stereo (the mixer's rate: no resampling), 192 kb/s.
# The file is committed, so a build never needs the owner's files; `make`
# runs this only when they are there and newer than it.
# Usage: tools/mksplash.sh <src dir> <out.mpg>
set -eu
src=$1 out=$2
W=2560 H=1440
command -v ffmpeg > /dev/null || { echo "mksplash: needs ffmpeg (brew install ffmpeg)"; exit 1; }
size=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x \
       "$src/jamos-boot.mov")
rate=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 \
       "$src/jamos-boot.mov")
case $rate in
24000/1001|24/1|25/1|30000/1001|30/1|50/1|60000/1001|60/1) ;;
*) echo "mksplash: frame rate $rate is not one MPEG-1 has: made 30"; rate=30 ;;
esac
if [ "$size" = "${W}x$H" ]; then
    fit="format=rgba"
else
    echo "mksplash: the source is $size: scaled to fit ${W}x$H (Lanczos)"
    fit="scale=$W:$H:force_original_aspect_ratio=decrease:flags=lanczos,format=rgba"
fi
tmp="$out.tmp.mpg"
ffmpeg -nostdin -loglevel error -y \
    -f lavfi -i "color=c=0x1E1A1D:s=${W}x$H:r=$rate" \
    -i "$src/jamos-boot.mov" -i "$src/jamos-boot.wav" \
    -filter_complex "[1:v]$fit[fg];[0:v][fg]overlay=(W-w)/2:(H-h)/2:format=auto:shortest=1,\
format=yuv420p[v]" \
    -map "[v]" -map 2:a \
    -c:v mpeg1video -q:v 2 -qmin 1 -bf 0 -g 30 -r "$rate" \
    -c:a mp2 -b:a 192k -ar 48000 -ac 2 \
    -f mpeg "$tmp"
mv "$tmp" "$out"
echo "mksplash: $out: $size at $rate fps -> ${W}x$H, $(wc -c < "$out" | tr -d ' ') bytes"
