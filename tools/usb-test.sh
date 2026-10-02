#!/bin/sh
# The QEMU USB scenario: a plain init boot (devmgr binds drv/usb-bus to
# qemu-xhci and drv/hid to each HID interface; init runs utest, then
# usbtest) with
#   xhci port 1   the boot stick (usb-storage, SuperSpeed)
#   xhci port 2   a usb-hub (full speed, port power switching on)
#       port 2.1  a keyboard with serial "jamos-keys": the test keyboard
#       port 2.2  a usb-ccid (no driver: usbtest's set_interface check)
#   xhci port 3   a usb-mouse
#   xhci port 4   a usb-kbd
# usbtest's storage checks run on the boot stick on the way (storage.c).
# usbtest finds the test keyboard by its serial and prints markers; the
# monitor script answers them: `sendkey a`; `sendkey c` (usbtest kills the
# keyboard's hid while c is down) and `sendkey d` once hid is back;
# device_del, device_add, `sendkey b`; and finally device_del of the hub
# (with the keyboard behind it).
# The keys must reach drv/hid through the real chain (usb-bus -> devmgr ->
# hid, no console in this mode): its "key 0x.. down" log lines are checked.
# QEMU_XHCI (e.g. msi=on,msix=off) and QEMU_SMP pass through.
# Usage: tools/usb-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-usb}
mkdir -p "$out"
mon="$out/$name.mon"
kbd="usb-kbd,id=keys,bus=xhci.0,port=2.1,serial=jamos-keys"
cat > "$mon" <<EOF
expect ready for keys
sleep 0.5
send sendkey a
expect usbtest: type c now
sleep 0.3
send sendkey c 1000
expect ready for keys after the restart
sleep 0.5
send sendkey d
expect usbtest: unplug the test keyboard now
send device_del keys
expect usbtest: plug the test keyboard back now
sleep 0.5
send device_add $kbd
expect ready for keys again
sleep 0.5
send sendkey b
expect usbtest: unplug the hub now
send device_del hub1
EOF
QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_MONITOR="$mon" \
QEMU_USB="-device usb-hub,id=hub1,bus=xhci.0,port=2,port-power=on \
-device usb-mouse,bus=xhci.0,port=3 \
-device usb-kbd,bus=xhci.0,port=4 -device usb-ccid,bus=xhci.0,port=2.2 -device $kbd" \
    tools/qemu-test.sh "$out" "$name" init || true
log="$out/$name.log"
ok=1
# 4 skipped: the storage checks on extra disks, which are tools/storage-test.sh's
grep -q "usbtest: 19 passed, 4 skipped (keys + unplug/replug ran)" "$log" || ok=0
grep -q "run complete: no problems" "$log" || ok=0
# hid's own lines, from the test keyboard's hid (behind the hub: "<root>.1:0")
for k in 04 06 07 05; do
    if ! grep -qE "\[hid-[0-9]+\.1:0\] hid 0627:0001 if 0: key 0x$k down" "$log"; then
        echo "$name: no 'key 0x$k down' line from the test keyboard's hid"
        ok=0
    fi
done
# one hid per HID interface at boot (mouse, keyboard, test keyboard), and one on the replug
n=$(grep -c "devmgr: usb 0627:0001 if0 -> drv/hid" "$log" || true)
[ "$n" -ge 4 ] || { echo "$name: $n hid binding line(s), want 4 (3 at boot + the replug)"; ok=0; }
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
grep -E "FAILED|usbtest: .* passed" "$log" | head -20
exit 1
