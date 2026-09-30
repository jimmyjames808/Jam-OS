#!/bin/sh
# The mouse, and the apps that came with it (bin/snake, bin/mines,
# bin/sysmon), from the shell: a plain boot ("shell") with a usb-kbd on
# xhci port 2 and a usb-mouse on port 3. tools/shell-tests/apps.txt runs
# each app's --selftest, then each app, with screenshots of the screen it
# borrowed, and drives the mouse through QEMU's monitor (mouse_move,
# mouse_button): usb-bus -> drv/hid -> console -> libfun -> bin/mines,
# which says every click on the console (`trace`), so the serial log shows
# where the pointer was. It also checks that the shell, which never asked
# for the mouse, is not disturbed by one.
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
