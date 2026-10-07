#!/bin/sh
# The README's pictures (docs/images/desktop-*.png, panic-screen.png),
# taken in QEMU at the PC's 2560x1440 (QEMU's VGA with that mode), so they
# can be taken again after the desktop changes:
#   1. a plain boot (the desktop) with a usb-kbd, a usb-mouse, an
#      hda-output codec into a WAV file (so Jamjar really plays), the
#      real-time clock at a fixed time, and a second stick holding the
#      music library at /usb0/music, running tools/shell-tests/readme.txt:
#      Jamjar playing beside two terminals (tiled), the same windows
#      floating with a notice, the search box, the volume popover;
#   2. a plain boot that panics on purpose (`crash pf yes`) with the
#      restarting screen held 5 s (`panichold=5000`): the calm panic
#      screen with its code.
# The library: a folder laid out Artist/Album/Track.mp3, e.g. the one
# tools/readme-music.py makes from public-domain recordings (the
# README's "Picture credits" names them); keep it outside the
# repository. The pictures go to <outdir> as PNG (lossless, saved again
# by Pillow with optimize: each is well under 1 MB); the search box, the
# popover and the panic screen are cut to the part of the screen around
# them. README_SHOTS_INSTALL=1 copies them into docs/images. Look at them
# before committing: a change to the desktop's sizes can move a window or
# a click (the geometry is in the script's header).
# Needs `make image` first (plain `make` leaves build/jamos.img as it
# was), Pillow and mtools. QEMU_SMP passes through (4 by default).
# Usage: tools/readme-shots.sh <outdir> <music folder>; exit 0 when every
# picture was taken (about 4 minutes on the Mac).
set -eu
out=$1 lib=$2
[ -d "$lib" ] || { echo "readme-shots: no folder $lib"; exit 1; }
mkdir -p "$out"
stick="$out/readme-music.img"
# A FAT32 stick of the library's size plus room: the music at /music.
mib=$(( $(du -sm "$lib" | cut -f1) * 11 / 10 + 48 ))
python3 tools/mkstick.py "$stick" "$mib" 0c
mformat -i "$stick@@1M" -T $(( (mib - 1) * 2048 )) -F -v MUSIC ::
# UTF-8 names as long names; a short name in FatFs's code page (437)
echo "default_codepage=437" > "$out/readme.mtoolsrc"
LC_ALL=en_US.UTF-8 MTOOLSRC="$out/readme.mtoolsrc" mmd -i "$stick@@1M" ::/music
for d in "$lib"/*; do
    LC_ALL=en_US.UTF-8 MTOOLSRC="$out/readme.mtoolsrc" mcopy -s -i "$stick@@1M" "$d" ::/music/
done

vga="-vga none -device VGA,xres=2560,yres=1440,vgamem_mb=64"
snd="-audiodev wav,id=snd0,path=$out/readme.wav,out.frequency=48000,out.channels=2,out.format=s16"
snd="$snd -device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0"
# The clock: Wednesday 7 October 2026, 14:42 (a fresh stick has no time
# zone in its settings, so the desktop shows UTC: the real-time clock's)
rtc="-rtc base=2026-10-07T14:42:00"
usb="-device usb-kbd,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3"
usb="$usb -drive if=none,id=music,format=raw,file=$stick"
usb="$usb -device usb-storage,bus=xhci.0,port=4,drive=music"
ok=1
QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_EXTRA="$vga $snd $rtc" QEMU_USB="$usb" \
    QEMU_INPUT=${README_SHOTS_SCRIPT:-tools/shell-tests/readme.txt} \
    tools/qemu-test.sh "$out" readme shell > "$out/readme.out" 2>&1 ||
    { echo "readme-shots: the desktop script failed (see $out/readme.log, $out/readme.out)"; ok=0; }
rm -f "$out/readme.wav"
if [ -z "${README_SHOTS_SCRIPT:-}" ]; then
    printf '%s\n' "wait 120 Jam OS shell" "wait {prompt}" "sleep 2" "send crash pf yes" \
        "wait 60 KERNEL PANIC" 'wait 10 panic screen: "Jam OS hit a problem and is restarting", JAM-' \
        "sleep 1.5" "shot readme-panic" "wait 60 loader:      Jam OS kexec" \
        "wait 120 the last boot panicked: " "wait {prompt}" "send reboot -f" \
        "wait 30 reboot: resetting" > "$out/readme-panic.txt"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_EXTRA="$vga $rtc" QEMU_INPUT="$out/readme-panic.txt" \
        tools/qemu-test.sh "$out" readme-panic shell panichold=5000 > "$out/readme-panic.out" 2>&1 ||
        { echo "readme-shots: the panic script failed (see $out/readme-panic.log)"; ok=0; }
fi
rm -f "$stick"

# The screenshots under their docs/images names, as palette PNGs.
python3 - "$out" <<'PY' || ok=0
import os, sys
from PIL import Image
out = sys.argv[1]
# The tiled and floating desktops whole; the search box, the popover and
# the panic screen as the part of the screen around them, pixel for pixel
# (in a whole screen they would be a few dozen pixels across on the
# README's page).
names = {"readme-tiled": ("desktop-tiled", None),
         "readme-floating": ("desktop-floating", None),
         "readme-search": ("desktop-search", (0, 0, 1600, 900)),
         "readme-popover": ("desktop-popover", (1000, 0, 2560, 878)),
         "readme-panic": ("panic-screen", (640, 255, 1920, 975))}
for shot, (name, box) in names.items():
    src = os.path.join(out, shot + ".png")
    if not os.path.exists(src):
        continue
    im = Image.open(src).convert("RGB")
    if im.size != (2560, 1440):
        print("readme-shots: %s is %dx%d, not 2560x1440" % (shot, *im.size))
        sys.exit(1)
    if box:
        im = im.crop(box)
    dest = os.path.join(out, name + ".png")
    im.save(dest, optimize=True)
    print("readme-shots: %s.png %d KB" % (name, os.path.getsize(dest) // 1024))
PY
if [ "${README_SHOTS_INSTALL:-0}" = 1 ] && [ $ok = 1 ]; then
    for n in desktop-tiled desktop-floating desktop-search desktop-popover panic-screen; do
        cp "$out/$n.png" docs/images/
    done
    echo "readme-shots: copied into docs/images"
fi
[ $ok = 1 ] && { echo "readme-shots: done"; exit 0; }
exit 1
