#!/bin/sh
# The boot disk is the one the machine booted from, by its MBR disk id
# (devmgr's disk.c): a plain boot with a second Jam OS stick on qemu-xhci
# port 2, a copy of the image with another disk id and other-stick.txt on
# its data partition (tools/shell-tests/bootdisk.txt). Checked: the
# kernel's "boot disk: MBR disk id" line names the boot stick's id; /data
# is the boot stick's (no other-stick.txt in `ls /data`) and the other
# stick's data partition is /usb1; after a `reboot` (kexec) the next
# kernel has the id from the kernel before and devmgr chooses the same.
# QEMU_SMP passes through. Usage: tools/bootdisk-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1 name=bootdisk
mkdir -p "$out"
other="$out/$name-other.img"
cp build/jamos.img "$other"
printf '\x5a\x5a\x0f\x0f' | dd of="$other" bs=1 seek=440 conv=notrunc 2>/dev/null
echo "the other stick" > "$out/other-stick.txt"
mcopy -o -i "$other@@64M" "$out/other-stick.txt" ::/other-stick.txt
QEMU_TIMEOUT=${QEMU_TIMEOUT:-180} QEMU_INPUT=tools/shell-tests/bootdisk.txt \
QEMU_USB="-drive if=none,id=other,format=raw,file=$other \
-device usb-storage,bus=xhci.0,port=2,drive=other" \
    tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1 ||
    { echo "$name: the script failed"; tail -3 "$out/$name.out"; }
log="$out/$name.log"
ok=1
want=$(od -An -tx4 -j440 -N4 build/jamos.img | tr -d ' ')
n=$(grep -ac "boot disk:   MBR disk id $want" "$log" || true)
[ "$n" -ge 2 ] || { echo "$name: the kernel named disk id $want $n time(s), want 2"; ok=0; }
grep -aq "MBR disk id $want (from the kernel before)" "$log" ||
    { echo "$name: no disk id from the kernel before after the reboot"; ok=0; }
grep -aq "disk id 0f0f5a5a, but the machine booted from disk id $want" "$log" ||
    { echo "$name: the other stick was not held back"; ok=0; }
# `ls /data` must not list the other stick's file: its line comes before
# the `ls /usb1` that lists it.
if awk '/ls \/data/{d=1} /ls \/usb1/{d=0} d && /other-stick.txt/{f=1} END{exit !f}' "$log"; then
    echo "$name: /data is the other stick's"
    ok=0
fi
grep -q "serial-feed: saw 'reboot: resetting'" "$out/$name.out" || ok=0
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
