#!/bin/sh
# Typing into the shell with a USB keyboard behind a hub (M7 integration):
# a plain boot ("shell": the console, devmgr connected to it, serialin, the
# shell) with a usb-hub on xhci port 2, a usb-kbd behind it and a
# usb-mouse on port 3. tools/shell-tests/usbkeys.txt types through QEMU's
# monitor (`sendkey`): usb-bus -> drv/hid -> console -> shell, kills the
# keyboard's hid and then the console, and types again after each.
# QEMU_SMP / QEMU_XHCI pass through. Usage: tools/usbkeys-test.sh <outdir>
# [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-usbkeys}
mkdir -p "$out"
if QEMU_TIMEOUT=${QEMU_TIMEOUT:-240} QEMU_INPUT=tools/shell-tests/usbkeys.txt \
   QEMU_USB="-device usb-hub,id=hub1,bus=xhci.0,port=2 -device usb-kbd,id=keys,bus=xhci.0,port=2.1 \
-device usb-mouse,bus=xhci.0,port=3" \
   tools/qemu-test.sh "$out" "$name" shell; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $out/$name.log)"
exit 1
