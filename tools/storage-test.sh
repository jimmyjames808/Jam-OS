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
# stick: bulk transfers by hand (after reset recovery, as a new owner of the
# pair needs it), a pair opened again on a disk left mid-READ (the next CBW is
# refused without recovery, taken after it), a bad CBW's STALL and reset
# recovery, the driver taking over a disk left mid-READ (its reset recovery
# comes before its first command), the ESP's FAT32 boot sector
# through a read-only `block` channel, out-of-range requests, a write to
# the data partition. On the second disk: the same reads and writes at full
# speed through the hub, a usb-storage killed with a READ in flight and a new one
# started on it, a read while a READ waits on the slow disk (it
# must not wait for that one), then the monitor script unplugs it while it
# is being read ("unplug the second disk now" -> device_del). On the slow disk:
# a READ times out (it doesn't hang), and then a later one works or the
# driver gives up after three in a row.
# QEMU_XHCI (e.g. msi=on,msix=off) and QEMU_SMP pass through.
# Usage: tools/storage-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-storage}
# Emulated (TCG) even where KVM is there: when usb-storage resets the
# 4 KiB/s disk (storage_timeout), QEMU drains its throttled requests inside
# the CPU that wrote the doorbell, for seconds. Under KVM the other CPUs
# run on, see that one take no timer interrupt for 5 s and the watchdog
# panics, rightly; under TCG they all wait together. Real hardware never
# stops a CPU for a USB reset.
export QEMU_ACCEL=${QEMU_ACCEL:-tcg}
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
# The whole init run in one boot behind the throttled disks: usbtest's
# slow-disk checks end near 230 s, and posixtest's signal section is slow
# here (its sampler's rounds take about 3.5 ms, not 10 us: 35 s), so the
# run needs more than the kernel's default 300 s (init_timeout).
QEMU_TIMEOUT=${QEMU_TIMEOUT:-500} QEMU_MONITOR="$mon" \
QEMU_USB="-device usb-hub,id=hub1,bus=xhci.0,port=2 \
-drive if=none,id=disk2img,format=raw,file=$disk,throttling.bps-read=262144 \
-device usb-storage,id=disk2,bus=xhci.0,port=2.1,drive=disk2img,serial=jamos-disk2 \
-drive if=none,id=slowimg,format=raw,file=$slow,throttling.bps-read=4096 \
-device usb-storage,id=slow,bus=xhci.0,port=2.2,drive=slowimg,serial=jamos-slow \
-device usb-mouse,bus=xhci.0,port=3" \
    tools/qemu-test.sh "$out" "$name" init init_timeout=450 || true
log="$out/$name.log"
ok=1
grep -q "usbtest: 22 passed, 6 skipped" "$log" || ok=0
grep -q "usbtest: block_in_flight ok" "$log" || { echo "$name: block_in_flight did not pass"; ok=0; }
grep -q "run complete: no problems" "$log" || ok=0
for t in bulk handover stall bind esp range write fence stop disk2 kill apart unplug timeout; do
    grep -q "usbtest: storage_$t ok" "$log" || { echo "$name: storage_$t did not pass"; ok=0; }
done
# usb-storage's own lines: the takeover's reset recovery, the fence
# dropping the WRITEs of the client that left, both disks' RESULTS lines,
# and the unplugged disk's driver seeing the device go
want() {
    grep -qE "$1" "$log" || { echo "$name: no line matching '$1'"; ok=0; }
}
want "\[usb-storage-test\] usb-storage 46f4:0001: taking the device over: reset recovery 1 "
# ... and so the first command was not lost to the mid-READ it was taken over from
if grep -qE "\[usb-storage-test\] usb-storage 46f4:0001: the CBW was not taken" "$log"; then
    echo "$name: usb-storage lost a command to the device it took over"
    ok=0
fi
# usb-bus clears the device's side of a pair it opens (the toggles/sequence numbers a
# real stick keeps); the first open on the boot stick (root port 1, if0, bulk 81 and 02)
want "usb 1: if0: bulk endpoints 81 and 02 opened, the device's toggles put back to 0"
want "usbtest: the first command after the hand-over worked after 1 reset recovery"
want "usbtest: a pair opened again on a device left mid-READ: the first command without recovery"
want "usbtest: a usb-storage killed with a READ in flight: the next one took over in"
want "\[usb-storage-test\] usb-storage 46f4:0001: partition 2: its client left with 4 request\(s\) queued: dropped"
# the boot stick: build/jamos.img's layout (the Makefile's ESP_END_MIB, IMAGE_MIB)
esp_end=$(python3 tools/mkimage.py --data-at build/jamos.img) && esp_end=${esp_end%M}
img_mib=$(( $(wc -c < build/jamos.img) >> 20 ))
want "usb-storage-test: usb-storage 46f4:0001: QEMU QEMU HARDDISK, $img_mib MiB, 2 partition\(s\) \(ef $((esp_end - 1)) MiB, 0c $((img_mib - esp_end)) MiB\)"
want "usb-storage-disk2: usb-storage 46f4:0001: QEMU QEMU HARDDISK, 128 MiB, 2 partition\(s\) \(ef 63 MiB, 0c 64 MiB\)"
want "\[usb-storage-disk2\] usb-storage 46f4:0001: the device is gone"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
grep -E "FAILED|usbtest: .* passed" "$log" | head -20
exit 1
