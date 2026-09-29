#!/bin/sh
# The QEMU USB scenario (M7 Track A): a plain init boot (devmgr binds
# drv/usb-bus to qemu-xhci; init runs utest, then usbtest) with
#   xhci port 1   the boot stick (usb-storage, SuperSpeed)
#   xhci port 2   a usb-hub (full speed, port power switching on)
#       port 2.1  a keyboard with serial "jamos-keys": the test keyboard
#   xhci port 3   a usb-mouse
#   xhci port 4   a usb-kbd
# usbtest finds the test keyboard by its serial and prints markers; the
# monitor script answers them: `sendkey a`, device_del, device_add, `sendkey b`.
# QEMU_XHCI (e.g. msi=on,msix=off) and QEMU_SMP pass through.
# Usage: tools/usb-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-usb}
mkdir -p "$out"
mon="$out/$name.mon"
kbd="usb-kbd,id=keys,bus=xhci.0,port=2.1,serial=jamos-keys"
cat > "$mon" <<EOF
expect usbtest: ready for keys
sleep 0.5
send sendkey a
expect usbtest: unplug the test keyboard now
send device_del keys
expect usbtest: plug the test keyboard back now
sleep 0.5
send device_add $kbd
expect usbtest: ready for keys again
sleep 0.5
send sendkey b
EOF
QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_MONITOR="$mon" \
QEMU_USB="-device usb-hub,bus=xhci.0,port=2,port-power=on -device usb-mouse,bus=xhci.0,port=3 \
-device usb-kbd,bus=xhci.0,port=4 -device $kbd" \
    tools/qemu-test.sh "$out" "$name" init || true
log="$out/$name.log"
if grep -q "usbtest: 8 passed, 0 skipped (keys + unplug/replug ran)" "$log" &&
   grep -q "run complete: no problems" "$log"; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
grep -E "FAILED|usbtest: .* passed" "$log" | head -20
exit 1
