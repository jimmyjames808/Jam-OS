#!/bin/sh
# Write a Jam OS image to a USB stick on macOS.
# Usage: tools/write-usb.sh build/jamos.img /dev/diskN
set -eu

img=${1:?image}
dev=${2:-}
if [ -z "$dev" ]; then
    echo "usage: make usb DEV=/dev/diskN   (find N with: diskutil list external)" >&2
    exit 1
fi
case "$dev" in
    /dev/disk[0-9]*) ;;
    *) echo "DEV must look like /dev/diskN" >&2; exit 1 ;;
esac

info=$(diskutil info "$dev")
if ! echo "$info" | grep -Eq "Device Location: +External|Removable Media: +Removable"; then
    echo "refusing: $dev is not an external/removable disk" >&2
    exit 1
fi
if echo "$info" | grep -Eq "Internal: +Yes"; then
    echo "refusing: $dev is internal" >&2
    exit 1
fi

echo "$info" | grep -E "Device / Media Name|Disk Size|Device Location"
printf "ERASE %s and write %s? Type YES: " "$dev" "$img"
read -r answer
[ "$answer" = "YES" ] || { echo "aborted"; exit 1; }

diskutil unmountDisk "$dev"
raw=$(echo "$dev" | sed 's|/dev/disk|/dev/rdisk|')
sudo dd if="$img" of="$raw" bs=4m
sync
diskutil eject "$dev"
echo "done: boot your PC from this stick (UEFI, Secure Boot off)."
