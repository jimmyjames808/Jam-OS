#!/bin/sh
# The boot disk is the one the machine booted from, by its MBR disk id
# (volumes' disk.c): a plain boot with a second Jam OS stick on qemu-xhci
# port 2, a copy of the image with another disk id and other-stick.txt on
# its data partition (tools/shell-tests/bootdisk.txt). Checked: the
# kernel's "boot disk: MBR disk id" line names the boot stick's id; /data
# is the boot stick's (no other-stick.txt in `ls /data`) and the other
# stick's data partition is /usb1; after a `reboot` (kexec) the next
# kernel has the id from the kernel before and volumes chooses the same.
# With QEMU_BOOT_NVME=1, the same from an installed drive, named by its GPT
# disk GUID (M12.7 stage 3): the boot disk is the image as Jam OS's GPT
# layout on an NVMe drive (tools/mkimage.py --gpt, qemu-test.sh's
# QEMU_BOOT_NVME), beside the other stick on port 2 and a second Jam OS
# drive with another GUID and other-drive.txt on its data partition. The
# kernel names the boot drive's GUID (from the loader, then from the kernel
# before), both others are held back for it, /data is the boot drive's
# (neither file in `ls /data`), the other stick's data partition is /usb1
# and the other drive's /nvme1, read-only (an input script made here).
# With the second argument `gone`, the boot names a GPT drive that isn't
# there (R3 stage 3's S1): the stick, its MBR disk id zeroed so the kernel
# takes the boot word bootdisk=gpt:<GUID> as after a kexec from an
# installed drive, is held for that drive and then mounted read-only
# (/usb0, /usb1), never the boot disk: no /esp, no /data.
# QEMU_SMP passes through. Usage: tools/bootdisk-test.sh <outdir> [gone];
# exit 0 on PASS.
set -eu
DATA_AT=$(python3 tools/mkimage.py --data-at build/jamos.img) || exit 1   # /data in the image: mtools' @@offset
out=$1 name=bootdisk
if [ "${2:-}" = gone ]; then
    name=bootdisk-gone
    mkdir -p "$out"
    stick="$out/$name-stick.img" log="$out/$name.log"
    gone=5b7e0c1a-2f4d-4e8b-9a31-6c0d2e4f8a17
    cp build/jamos.img "$stick"
    printf '\000\000\000\000' | dd of="$stick" bs=1 seek=440 conv=notrunc 2>/dev/null
    cat > "$out/$name.txt" <<EOF
# tools/bootdisk-test.sh gone: the boot word names GPT disk GUID $gone,
# which no disk has; the stick waits for it, then is another disk.
wait 120 Jam OS shell
wait {prompt}
seen 60 GPT disk GUID $gone (from the kernel before)
wait 60 is not here
wait 30 partition 2 is /usb1, read-only
sleep 1
send mount
wait /usb1
wait {prompt}
send reboot -f
wait reboot: resetting
EOF
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-180} QEMU_INPUT="$out/$name.txt" QEMU_IMAGE="$stick" \
        tools/qemu-test.sh "$out" "$name" shell "bootdisk=gpt:$gone" > "$out/$name.out" 2>&1 ||
        { echo "$name: the script failed"; tail -3 "$out/$name.out"; }
    ok=1
    grep -aq "init: hello from ring 3 (.* bootdisk=gpt:$gone" "$log" ||
        { echo "$name: init was not given bootdisk=gpt:$gone"; ok=0; }
    grep -aq "but the machine booted from disk GUID $gone: it waits" "$log" ||
        { echo "$name: the stick was not held for the named drive"; ok=0; }
    if grep -aq "may be the boot disk\|is the boot disk" "$log"; then
        echo "$name: the stick became the boot disk on a boot from a GPT drive"
        ok=0
    fi
    grep -aq "partition 1 is /usb0, read-only" "$log" ||
        { echo "$name: the stick's ESP is not /usb0, read-only"; ok=0; }
    grep -q "serial-feed: saw 'reboot: resetting'" "$out/$name.out" || ok=0
    [ $ok = 1 ] && { echo "$name: PASS"; exit 0; }
    echo "$name: FAIL (see $log)"
    exit 1
fi
mkdir -p "$out"
other="$out/$name-other.img"
cp build/jamos.img "$other"
# (octal: dash's printf, Ubuntu's /bin/sh, has no \x)
printf '\132\132\017\017' | dd of="$other" bs=1 seek=440 conv=notrunc 2>/dev/null
echo "the other stick" > "$out/other-stick.txt"
mcopy -o -i "$other@@$DATA_AT" "$out/other-stick.txt" ::/other-stick.txt
log="$out/$name.log"
ok=1

# The GPT disk GUID of an image, as printed (8-4-4-4-12, the first three
# fields little-endian on disk: the primary header's bytes 56 to 71).
guid_of() {
    python3 -c 'import sys, uuid; f = open(sys.argv[1], "rb"); f.seek(568);
print(uuid.UUID(bytes_le=f.read(16)))' "$1"
}

if [ "${QEMU_BOOT_NVME:-0}" = 1 ]; then
    boot="$out/$name-boot.img" drive="$out/$name-drive.img"
    python3 tools/mkimage.py --gpt build/jamos.img "$boot"
    python3 tools/mkimage.py --gpt build/jamos.img "$drive"   # another random GUID
    drive_at=$(python3 tools/mkimage.py --data-at "$drive")
    echo "the other drive" > "$out/other-drive.txt"
    mcopy -o -i "$drive@@$drive_at" "$out/other-drive.txt" ::/other-drive.txt
    want=$(guid_of "$boot") theirs=$(guid_of "$drive")
    [ "$want" != "$theirs" ] || { echo "$name: both drives got GUID $want"; exit 1; }
    cat > "$out/$name.txt" <<EOF
# tools/bootdisk-test.sh with QEMU_BOOT_NVME: the boot drive (GPT disk GUID
# $want) beside another stick and another Jam OS drive.
# (volumes' lines may come before the prompt, as on a fast host: seen)
wait 60 {prompt}
seen 60 is the boot disk
seen 60 disk id 0f0f5a5a, but the machine booted from disk GUID $want
seen 60 disk GUID $theirs, but the machine booted from disk GUID $want
seen 60 partition 2 is /usb1
seen 60 partition 2 is /nvme1, read-only
sleep 1
send ls /data
wait {prompt}
send ls /usb1
wait 30 other-stick.txt
wait {prompt}
send ls /nvme1
wait 30 other-drive.txt
wait {prompt}
send reboot
wait 120 from the kernel before
wait 60 is the boot disk
seen 60 the machine booted from another disk: not the boot disk
wait 60 partition 2 is /usb1
seen 60 partition 2 is /nvme1, read-only
sleep 1
send ls /nvme1
wait 30 other-drive.txt
wait {prompt}
send reboot -f
wait reboot: resetting
EOF
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-180} QEMU_INPUT="$out/$name.txt" QEMU_IMAGE="$boot" \
    QEMU_NVME="$drive" \
    QEMU_USB="-drive if=none,id=other,format=raw,file=$other \
-device usb-storage,bus=xhci.0,port=2,drive=other" \
        tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1 ||
        { echo "$name: the script failed"; tail -3 "$out/$name.out"; }
    n=$(grep -ac "boot disk:   GPT disk GUID $want" "$log" || true)
    [ "$n" -ge 2 ] || { echo "$name: the kernel named GUID $want $n time(s), want 2"; ok=0; }
    grep -aq "GPT disk GUID $want (from the loader)" "$log" ||
        { echo "$name: the loader didn't name the boot drive's GUID"; ok=0; }
    grep -aq "GPT disk GUID $want (from the kernel before)" "$log" ||
        { echo "$name: no GUID from the kernel before after the reboot"; ok=0; }
    grep -aq "init: hello from ring 3 (.* bootdisk=gpt:$want" "$log" ||
        { echo "$name: init was not given bootdisk=gpt:$want"; ok=0; }
    grep -aqE "kexec: stored kernel armed, command line \".* bootdisk=gpt:$want\"" "$log" ||
        { echo "$name: the stored kernel's command line has no bootdisk=gpt:$want"; ok=0; }
    # `ls /data` must list neither of the others' files: its lines come
    # before the `ls /usb1` that lists one of them.
    if awk '/ls \/data/{d=1} /ls \/usb1/{d=0} d && /other-(stick|drive).txt/{f=1} END{exit !f}' "$log"; then
        echo "$name: /data is another disk's"
        ok=0
    fi
else
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-180} QEMU_INPUT=tools/shell-tests/bootdisk.txt \
    QEMU_USB="-drive if=none,id=other,format=raw,file=$other \
-device usb-storage,bus=xhci.0,port=2,drive=other" \
        tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1 ||
        { echo "$name: the script failed"; tail -3 "$out/$name.out"; }
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
fi
grep -q "serial-feed: saw 'reboot: resetting'" "$out/$name.out" || ok=0
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
