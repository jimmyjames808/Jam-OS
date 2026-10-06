#!/bin/sh
# A tile resized live, in QEMU: the compositor's boot ("shell") with a
# usb-kbd and a usb-mouse, two terminals side by side, the gap between
# them dragged 6 pixels a step, right 120 and back left 240, a screenshot
# every few steps (tools/shell-tests/tile-resize.txt). Each shot is taken
# just after a move, often before the terminals have drawn their new
# sizes: the compositor shows each one's last picture clipped to its new
# tile or padded in its background colour, and libjwl never draws a new
# size over the picture being shown. So in every shot the top and bottom
# padding rows of both tiles (y 52 and 788, away from the gap) are the
# terminal's background, as in the first shot: never the wallpaper, black,
# or a half-drawn frame. Shots: <outdir>/resize-*.png.
# QEMU_SMP passes through. Usage: tools/resize-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1
QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/tile-resize.txt \
    QEMU_USB="-device usb-kbd,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3" \
    tools/qemu-test.sh "$out" resize shell > "$out/resize.out" 2>&1 ||
    { echo "resize: the script failed"; grep "serial-feed: .*no '" "$out/resize.out" || true; ok=0; }
if [ $ok = 1 ]; then
    python3 - "$out" <<'PY' || ok=0
import sys
from PIL import Image
out = sys.argv[1]
def shot(i):
    return Image.open("%s/resize-%02d.png" % (out, i)).convert("RGB")
bg = shot(0).getpixel((20, 788))
bad = 0
for i in range(1, 22):
    gap = 640 + 12 * i if i <= 10 else 760 - 24 * (i - 10)
    if i == 21:
        gap = 520
    im = shot(i)
    wrong = [(x, y, im.getpixel((x, y))) for y in (52, 788)
             for x in list(range(12, gap - 14)) + list(range(gap + 14, 1268))
             if im.getpixel((x, y)) != bg]
    if wrong:
        bad += 1
        print("resize: shot %02d (gap at %d): %d pixels not the terminal's background, "
              "first %s" % (i, gap, len(wrong), wrong[0]))
print("resize: %d of 21 shots with a flash (background %02x%02x%02x)" % ((bad,) + bg))
sys.exit(1 if bad else 0)
PY
fi
if [ $ok = 1 ]; then
    echo "resize: PASS"
    exit 0
fi
echo "resize: FAIL (see $out/resize.log, $out/resize-*.png)"
exit 1
