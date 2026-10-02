#!/bin/sh
# Update a stick that already boots Jam OS, on macOS: copy the kernel, the
# bootfs and limine.conf onto its ESP. Nothing is erased; /data is not
# touched. (macOS does not mount an MBR partition of type 0xEF by itself,
# so this mounts it by hand, which needs sudo.)
# A pull mid-way must leave a stick that boots: the three files are first
# written under new names (<name>.new) next to the old ones, synced and
# compared, and only then renamed over them, one after the other, which
# rewrites a directory entry each (milliseconds, against the seconds the
# copies take). A pull before that leaves the old files whole (and .new
# files, which the next flash removes first); a pull during the renames
# can leave the new kernel with the old boot image or menu for that boot.
# Before it touches any disk it says the build's network default (the boot
# image's build.txt: the Makefile's JAMOS_VLAN, from local.mk) and, for a
# build that sends untagged frames by default (no local.mk) or one that
# doesn't say, asks before writing it: the owner's PC lives on VLAN 21,
# and his builds come from a tree with `JAMOS_VLAN := 21` in local.mk.
# FLASH_UNTAGGED=yes answers yes (a script that means it).
# Usage: tools/flash-usb.sh <kernel> <bootfs> <limine.conf> [/dev/diskN]
# With no disk given, the one external disk with Jam OS's layout is used.
set -eu

elf=${1:?kernel}
bootfs=${2:?bootfs}
conf=${3:?limine.conf}
dev=${4:-}
SUDO=${SUDO-sudo}   # SUDO= (empty) for a disk image the user owns

net=$(python3 "$(dirname "$0")/update-server.py" --build-net "$bootfs") || net=unknown
case $net in
vlan*)
    echo "network default of this build: VLAN ${net#vlan} (tagged)" ;;
*)
    if [ "$net" = untagged ]; then
        echo "network default of this build: UNTAGGED (no JAMOS_VLAN in local.mk): a boot" \
             "without a vlan= word sends plain untagged frames"
    else
        echo "network default of this build: NOT KNOWN (its build.txt says none: an old build)"
    fi
    echo "  A PC on a VLAN (the owner's: VLAN 21) needs local.mk with JAMOS_VLAN := 21 and"
    echo "  make again (local.mk.example)."
    if [ "${FLASH_UNTAGGED:-}" = yes ]; then
        echo "  FLASH_UNTAGGED=yes: writing it anyway"
    else
        printf '  Write this build to the stick anyway? [y/N] '
        answer=
        { read -r answer < /dev/tty; } 2>/dev/null || answer=
        case $answer in
        y|Y|yes) ;;
        *) echo "not written: no disk was touched"; exit 1 ;;
        esac
    fi ;;
esac

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
# The whole disk, nothing after the number: not a partition (/dev/disk4s1).
if ! printf '%s\n' "$dev" | grep -Eq '^/dev/disk[0-9]+$'; then
    echo "DEV must look like /dev/diskN" >&2
    exit 1
fi

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
trap 'exit 1' INT TERM HUP   # through the EXIT trap: never leave the ESP mounted

$SUDO mount -t msdos "${dev}s1" "$mnt"
mounted=yes
if [ ! -f "$mnt/boot/jamos.elf" ] || [ ! -d "$mnt/boot/limine" ]; then
    echo "refusing: ${dev}s1 holds no boot/jamos.elf: not a Jam OS stick" >&2
    exit 1
fi

# Left over by a flash that was cut short: not booted from, removed.
$SUDO rm -f "$mnt/boot/jamos.elf.new" "$mnt/boot/bootfs.img.new" \
    "$mnt/boot/limine/limine.conf.new"
# -X: no extended attributes, so no ._ files on the FAT volume.
$SUDO cp -X "$elf" "$mnt/boot/jamos.elf.new"
$SUDO cp -X "$bootfs" "$mnt/boot/bootfs.img.new"
$SUDO cp -X "$conf" "$mnt/boot/limine/limine.conf.new"
$SUDO rm -f "$mnt/boot/._"* "$mnt/boot/limine/._"*
sync
cmp "$elf" "$mnt/boot/jamos.elf.new"
cmp "$bootfs" "$mnt/boot/bootfs.img.new"
cmp "$conf" "$mnt/boot/limine/limine.conf.new"
# All three are whole on the stick: now the renames, the menu last.
$SUDO mv -f "$mnt/boot/jamos.elf.new" "$mnt/boot/jamos.elf"
$SUDO mv -f "$mnt/boot/bootfs.img.new" "$mnt/boot/bootfs.img"
$SUDO mv -f "$mnt/boot/limine/limine.conf.new" "$mnt/boot/limine/limine.conf"
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
