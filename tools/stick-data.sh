#!/bin/sh
# A Jam OS stick's data partition as an ext volume at the partition's size,
# holding the files of a host directory (docs/M13.6-PLAN.md, "Ext by
# default"). `make usb` runs it (as root: the raw disk) after tools/mbr-grow.py
# has grown the partition, since the image's own ext volume is 64 MiB.
#
#   tools/stick-data.sh <jamext> <disk or image> [<dir>]
#
# jamext's mkfs writes the journal and the first group's tables only (the
# other groups are made on first use, as mke2fs's lazy tables), so a big
# stick takes seconds. Then <dir> (the owner's ~/.config/jamos/data, if
# there is one) is copied in with jamext import and the volume checked.
# A disk image works the same way: the test is mbr-grow on a copy of an
# image, then this. Exit 0 when the volume is made and clean.
set -eu

jamext=${1:?usage: stick-data.sh <jamext> <disk or image> [<dir>]}
disk=${2:?disk}
dir=${3:-}
[ -x "$jamext" ] || { echo "stick-data: no $jamext (make build/host/jamext)" >&2; exit 1; }

# 2T: more than any MBR partition holds; jamext cuts it to the partition's.
"$jamext" -q --part 2 mkfs --label JAMOS-DATA "$disk" 2T
if [ -n "$dir" ] && [ -d "$dir" ]; then
    echo "stick-data: $dir goes into /data"
    "$jamext" -q --part 2 import "$disk" "$dir"
fi
"$jamext" -q --part 2 check "$disk" > /dev/null
echo "stick-data: $disk's data partition is an ext volume"
