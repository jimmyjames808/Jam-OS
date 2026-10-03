#!/bin/sh
# The QEMU storage scenario: a plain init boot (init runs utest, then
# usbtest, whose storage checks are in user/tests/usbtest/storage.c) with
#   xhci port 1   the boot stick (usb-storage, SuperSpeed): ESP + JAMOS-DATA
#   xhci port 2   a usb-hub (full speed)
#       port 2.1  a second usb-storage disk, serial "jamos-disk2", with the
#                 same two-partition layout (made here, in <outdir>). QEMU
#                 reads it at 256 KiB/s (throttling), so a 64 KiB read
#                 takes a quarter of a second and the unplug lands inside
#                 one (unthrottled, QEMU finishes every transfer at once)
#       port 2.2  a third one, serial "jamos-slow", read at 4 KiB/s: a
#                 READ of 24 KiB outlasts usb-storage's 5 s
#   xhci port 3   a usb-mouse (usbtest's stall_recovered wants a QEMU device)
# usbtest starts a drv/usb-storage of its own on each disk. On the boot
# stick: bulk transfers by hand, a bad CBW's STALL and reset recovery, the
# driver taking over a disk left mid-READ, the ESP's FAT32 boot sector
# through a read-only `block` channel, out-of-range requests, a write to
# the data partition. On the second disk: the same reads and writes at full
# speed through the hub, a read while a READ waits on the slow disk (it
# must not wait for that one), then the monitor script unplugs it while it
# is being read ("unplug the second disk now" -> device_del). On the slow disk:
# a READ times out (it doesn't hang), and then a later one works or the
# driver gives up after three in a row.
# QEMU_XHCI (e.g. msi=on,msix=off) and QEMU_SMP pass through.
# Usage: tools/storage-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-storage}
mkdir -p "$out"
disk="$out/$name-disk2.img"
# The second disk: the image's layout (tools/mkimage.py), both partitions FAT32.
python3 tools/mkimage.py "$disk" 64 128
mformat -i "$disk@@1M" -T $((63 * 2048)) -F -v DISK2-ESP ::
mformat -i "$disk@@64M" -T $((64 * 2048)) -F -v DISK2-DATA ::
slow="$out/$name-slow.img"
python3 tools/mkimage.py "$slow" 2 4
mon="$out/$name.mon"
cat > "$mon" <<EOF
expect usbtest: unplug the second disk now
sleep 0.3
send device_del disk2
EOF
QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_MONITOR="$mon" \
QEMU_USB="-device usb-hub,id=hub1,bus=xhci.0,port=2 \
-drive if=none,id=disk2img,format=raw,file=$disk,throttling.bps-read=262144 \
-device usb-storage,id=disk2,bus=xhci.0,port=2.1,drive=disk2img,serial=jamos-disk2 \
-drive if=none,id=slowimg,format=raw,file=$slow,throttling.bps-read=4096 \
-device usb-storage,id=slow,bus=xhci.0,port=2.2,drive=slowimg,serial=jamos-slow \
-device usb-mouse,bus=xhci.0,port=3" \
    tools/qemu-test.sh "$out" "$name" init || true
log="$out/$name.log"
ok=1
grep -q "usbtest: 18 passed, 6 skipped" "$log" || ok=0
grep -q "run complete: no problems" "$log" || ok=0
for t in bulk stall bind esp range write fence stop disk2 apart unplug timeout; do
    grep -q "usbtest: storage_$t ok" "$log" || { echo "$name: storage_$t did not pass"; ok=0; }
done
# usb-storage's own lines: the takeover's reset recovery, the fence
# dropping the WRITEs of the client that left, both disks' RESULTS lines,
# and the unplugged disk's driver seeing the device go
want() {
    grep -qE "$1" "$log" || { echo "$name: no line matching '$1'"; ok=0; }
}
want "\[usb-storage-test\] usb-storage 46f4:0001: the CBW was not taken: reset recovery 1 "
want "\[usb-storage-test\] usb-storage 46f4:0001: partition 2: its client left with 4 request\(s\) queued: dropped"
want "usb-storage-test: usb-storage 46f4:0001: QEMU QEMU HARDDISK, 128 MiB, 2 partition\(s\) \(ef 63 MiB, 0c 64 MiB\)"
want "usb-storage-disk2: usb-storage 46f4:0001: QEMU QEMU HARDDISK, 128 MiB, 2 partition\(s\) \(ef 63 MiB, 0c 64 MiB\)"
want "\[usb-storage-disk2\] usb-storage 46f4:0001: the device is gone"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
grep -E "FAILED|usbtest: .* passed" "$log" | head -20
exit 1
