#!/bin/sh
# tools/flash-usb.sh (`make flash`) on a disk image, never on a real disk:
# a copy of build/jamos.img attached with hdiutil (not mounted: nothing
# appears in /Volumes), flashed with a kernel 4 KiB longer, the boot image
# and a changed boot menu (SUDO= : the image is the user's own), detached,
# and read back with mtools. Checked: the three files are the new ones; the
# stick's kernel and boot image are kept as the previous build
# (boot/prev-jamos.elf, prev-bootfs.img), replacing an older one placed
# there first; no <name>.new is left, including a stale one placed there
# first (as a flash cut short would leave it); the data partition is byte
# for byte what it was. macOS only (hdiutil, diskutil).
# Usage: tools/flash-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1 name=flash
mkdir -p "$out"
img="$out/$name-stick.img"
cp build/jamos.img "$img"
cp build/jamos.elf "$out/$name-new.elf"
head -c 4096 /dev/zero >> "$out/$name-new.elf"
cp boot/limine.conf "$out/$name-new.conf"
echo "# flashed by tools/flash-test.sh" >> "$out/$name-new.conf"
echo stale > "$out/$name-stale"
mcopy -o -i "$img@@1M" "$out/$name-stale" ::/boot/jamos.elf.new
mcopy -o -i "$img@@1M" "$out/$name-stale" ::/boot/prev-jamos.elf   # an older previous build
mcopy -o -i "$img@@1M" "$out/$name-stale" ::/boot/prev-bootfs.img.new
mcopy -o -i "$img@@1M" ::/boot/jamos.elf "$out/$name-old.elf"
mcopy -o -i "$img@@1M" ::/boot/bootfs.img "$out/$name-old.img"
data_before=$(dd if="$img" bs=1m skip=64 2>/dev/null | shasum | cut -d' ' -f1)
dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$img" |
      awk 'NR == 1 { print $1 }')
ok=1
SUDO= tools/flash-usb.sh "$out/$name-new.elf" build/bootfs.img "$out/$name-new.conf" "$dev" \
    > "$out/$name.out" 2>&1 || { echo "$name: flash-usb.sh failed:"; tail -5 "$out/$name.out"; ok=0; }
hdiutil detach "$dev" > /dev/null 2>&1 || hdiutil detach -force "$dev" > /dev/null 2>&1 || true
for f in jamos.elf:"$out/$name-new.elf" bootfs.img:build/bootfs.img \
         limine/limine.conf:"$out/$name-new.conf" prev-jamos.elf:"$out/$name-old.elf" \
         prev-bootfs.img:"$out/$name-old.img"; do
    mcopy -o -i "$img@@1M" "::/boot/${f%%:*}" "$out/$name-got" 2>/dev/null ||
        { echo "$name: no boot/${f%%:*} on the stick"; ok=0; continue; }
    cmp -s "$out/$name-got" "${f#*:}" || { echo "$name: boot/${f%%:*} is not the file it should be"; ok=0; }
done
if mdir -i "$img@@1M" ::/boot ::/boot/limine 2>/dev/null | grep -qi "\.new"; then
    echo "$name: a .new file is left on the ESP"
    ok=0
fi
data_after=$(dd if="$img" bs=1m skip=64 2>/dev/null | shasum | cut -d' ' -f1)
[ "$data_before" = "$data_after" ] || { echo "$name: the data partition changed"; ok=0; }
rm -f "$img" "$out/$name-got" "$out/$name-stale" "$out/$name-old.elf" "$out/$name-old.img"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
