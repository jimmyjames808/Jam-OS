#!/bin/sh
# Update a stick that already boots Jam OS, on macOS: copy the kernel, the
# bootfs and limine.conf onto its ESP. Nothing is erased; /data is not
# touched. (macOS does not mount an MBR partition of type 0xEF by itself,
# so this mounts it by hand, which needs sudo.)
# Usage: tools/flash-usb.sh <kernel> <bootfs> <limine.conf> [/dev/diskN]
# With no disk given, the one external disk with Jam OS's layout is used.
set -eu

elf=${1:?kernel}
bootfs=${2:?bootfs}
conf=${3:?limine.conf}
dev=${4:-}
SUDO=${SUDO-sudo}   # SUDO= (empty) for a disk image the user owns

# "yes" if disk $1 has Jam OS's layout: partition 1 of type 0xEF and a partition 2.
jam_layout()
{
    diskutil list "$1" | awk '$1 == "1:" && $2 == "0xEF" { esp = 1 } $1 == "2:" { data = 1 }
                              END { if (esp && data) print "yes" }'
}

if [ -z "$dev" ]; then
    found=
    for d in $(diskutil list external physical | awk '/^\/dev\/disk/ { print $1 }'); do
        if [ "$(jam_layout "$d")" = yes ]; then
            [ -z "$found" ] || { echo "more than one Jam OS stick: say which with DEV=/dev/diskN" >&2; exit 1; }
            found=$d
        fi
    done
    [ -n "$found" ] || { echo "no Jam OS stick found (a new stick needs: make usb DEV=/dev/diskN)" >&2; exit 1; }
    dev=$found
fi
case "$dev" in
    /dev/disk[0-9]*) ;;
    *) echo "DEV must look like /dev/diskN" >&2; exit 1 ;;
esac

info=$(diskutil info "$dev")
if [ -n "$SUDO" ]; then
    if ! echo "$info" | grep -Eq "Device Location: +External|Removable Media: +Removable"; then
        echo "refusing: $dev is not an external/removable disk" >&2
        exit 1
    fi
    if echo "$info" | grep -Eq "Internal: +Yes"; then
        echo "refusing: $dev is internal" >&2
        exit 1
    fi
fi
if [ "$(jam_layout "$dev")" != yes ]; then
    echo "refusing: $dev does not have Jam OS's two partitions (a new stick needs: make usb)" >&2
    exit 1
fi
echo "$info" | grep -E "Device / Media Name|Disk Size"

mnt=$(mktemp -d /tmp/jamos-esp.XXXXXX)
mounted=no
cleanup()
{
    [ "$mounted" = no ] || $SUDO umount "$mnt" 2>/dev/null || true
    rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT

$SUDO mount -t msdos "${dev}s1" "$mnt"
mounted=yes
if [ ! -f "$mnt/boot/jamos.elf" ] || [ ! -d "$mnt/boot/limine" ]; then
    echo "refusing: ${dev}s1 holds no boot/jamos.elf: not a Jam OS stick" >&2
    exit 1
fi

# -X: no extended attributes, so no ._ files on the FAT volume.
$SUDO cp -X "$elf" "$mnt/boot/jamos.elf"
$SUDO cp -X "$bootfs" "$mnt/boot/bootfs.img"
$SUDO cp -X "$conf" "$mnt/boot/limine/limine.conf"
$SUDO rm -f "$mnt/boot/._"* "$mnt/boot/limine/._"*
sync
cmp "$elf" "$mnt/boot/jamos.elf"
cmp "$bootfs" "$mnt/boot/bootfs.img"
cmp "$conf" "$mnt/boot/limine/limine.conf"

$SUDO umount "$mnt"
mounted=no
sync
if [ -n "$SUDO" ]; then
    diskutil eject "$dev"
fi
echo "done: $dev updated and checked; /data was not touched."
