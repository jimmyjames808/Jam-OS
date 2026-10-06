#!/bin/sh
# Copy and paste between terminal windows end to end (docs/G1-PLAN.md, "As
# built: copy and paste"): one plain boot ("shell") with a usb-kbd on xhci
# port 2 and a usb-mouse on port 3, running tools/shell-tests/clip.txt:
# a triple click selects a line of terminal 1, Super+C copies it, Super+V
# pastes it bracketed into terminal 2's shell, which runs it only at Enter;
# a drag over two lines, copied and pasted with Ctrl+Shift+V into terminal
# 3, arrives as one line. Then the screenshots: the selected line on the
# blackcurrant tint (45% of #7f77dd over the terminal's #101018: #423e71)
# from edge to edge, the line above it untouched (`clip-selected`); the
# drag's cells tinted from row 0's column 7 to row 1's column 10 and not
# around them (`clip-drag`). The clicks are at OVMF's 1280x800.
# QEMU_SMP passes through. Usage: tools/clip-test.sh <outdir> [name]; exit 0
# on PASS.
set -eu
out=$1 name=${2:-clip}
mkdir -p "$out"
usb="-device usb-kbd,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3"
ok=1
QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/clip.txt QEMU_USB="$usb" \
    tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1 ||
    { echo "$name: the script failed"; tail -3 "$out/$name.out"; ok=0; }
# Pixels: (x, y, want) in each screenshot; the text starts at (18, 58), cells 9x21.
python3 - "$out" <<'EOF' || ok=0
import sys
from PIL import Image
TINT, BG = 0x423e71, 0x101018
checks = {
    "clip-selected": [(30, 95, TINT), (600, 89, TINT), (1250, 85, TINT), (600, 68, BG)],
    "clip-drag": [(300, 68, TINT), (60, 89, TINT), (60, 68, BG), (150, 89, BG)],
}
bad = 0
for shot, points in checks.items():
    im = Image.open(f"{sys.argv[1]}/{shot}.png").convert("RGB")
    for x, y, want in points:
        r, g, b = im.getpixel((x, y))
        got = r << 16 | g << 8 | b
        if got != want:
            print(f"clip: {shot}.png ({x}, {y}) is #{got:06x}, want #{want:06x}")
            bad += 1
sys.exit(1 if bad else 0)
EOF
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $out/$name.log)"
exit 1
