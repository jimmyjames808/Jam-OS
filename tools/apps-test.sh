#!/bin/sh
# Programs that draw with libfun in windows, from the shell: a plain boot
# ("shell": the desktop) with a usb-kbd on xhci port 2 and a usb-mouse on
# port 3. tools/shell-tests/apps.txt runs the test program fractal's
# --selftest, then explores with it and its benchmark (every CPU, vector
# maths) taking screenshots of its window, quits it with q and checks the
# prompt comes back; wltest's mouse mode before the mouse stirs and with
# one click, through QEMU's monitor (mouse_button): usb-bus -> drv/hid ->
# the compositor -> libfun -> bin/wltest, which says the click on the
# console; then kills one program (Ctrl+C in the terminal) and crashes one
# while their windows are up, and checks the shell has the keys again.
# (Before M12's Q14 the programs borrowed the screen under `nocomp`.)
# Screenshots: <outdir>/apps-*.png. QEMU_SMP passes through; APPS_HD=1 runs
# at the PC's 2560x1440 (QEMU's VGA with that mode) instead of OVMF's
# 1280x800. Usage: tools/apps-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-apps}
mkdir -p "$out"
if [ "${APPS_HD:-}" = 1 ]; then
    QEMU_EXTRA="${QEMU_EXTRA:-} -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=64"
    export QEMU_EXTRA
fi
if QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_INPUT=tools/shell-tests/apps.txt \
   QEMU_USB="-device usb-kbd,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3" \
   tools/qemu-test.sh "$out" "$name" shell; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $out/$name.log)"
exit 1
